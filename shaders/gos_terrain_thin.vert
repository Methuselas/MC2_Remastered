//#version 430 (version provided by material prefix)

// --- SSBO bindings (must match TerrainQuadThinRecord / TerrainQuadRecipe in gos_terrain_patch_stream.h) ---
// Fix B 2026-05-14: clipPos[4] added — VS reads pre-projected clip-space
// positions from the writer instead of doing terrainMVP*worldPos itself.
// Eliminates the temporal MVP misalignment root cause at its source.
// LOCKSTEP across four declarations per memory/cpp_glsl_ubo_struct_lockstep.md.
struct TerrainQuadThinRecord {
    uvec4 control;       // x=recipeIdx, y=terrainHandle, z=flags(bit0=uvMode,bit1=pzTri1,bit2=pzTri2), w=cementWord
    uvec4 lightRGBs;     // corners 0-3, packed ARGB
    vec4  clipPos[4];    // per-corner clip-space positions (writer-emitted)
};
// AMD L1-coherency fix (mc2-cpu-gpu-offload-expert, 2026-05-14): see paired
// `coherent` qualifier on the compute writer at gpu_driven_terrain_solid.comp:118.
// Without `coherent` on BOTH ends, AMD VS lane prefetches can return stale
// thin-record bytes (specifically tr.control.z = flags) under heavy compute
// throughput, even after glMemoryBarrier and glFinish.  The race is per-lane
// L1-prefetch vs the writer's cache-line invalidation, not a frame-level
// barrier issue.  Without coherent, kCornerTable[uvMode*6+...] can select a
// wrong corner-table half for ONE invocation per frame, producing the giant
// terrain triangle whose worldPos varies frame-to-frame (skybox / mid / water).
layout(std430, binding = 2) coherent readonly buffer ThinRecordBuf {
    TerrainQuadThinRecord thinRecs[];
};

struct TerrainQuadRecipe {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;  // minU, minV, maxU, maxV
};
layout(std430, binding = 1) readonly buffer RecipeBuf {
    TerrainQuadRecipe recipes[];
};

// Output varyings — names MUST match gos_terrain.frag `in` declarations exactly.
// CRITICAL: this set must remain compatible with the legacy non-thin VS chain
// (gos_terrain.vert/.tesc/.tese) which feeds the same gos_terrain.frag — adding
// a varying here that the legacy chain doesn't emit causes silent linker
// failure → terrain renders transparent through to skybox.
out vec4  Color;
out vec2  Texcoord;       // per-tile UV [0,1] — used by frag's detail tiling, anti-tile
                          // derivatives, POM ray-march, matNormal mix. Atlas-absolute
                          // UV for tex1 sampling is reconstructed in the frag from
                          // WorldPos via atlas* uniforms (NOT a varying, see above).
out float TerrainType;
out vec3  WorldNorm;
out vec3  WorldPos;
out float UndisplacedDepth;
flat out uint RecordIdx;  // index into thinRecs[] — frag reads thinRecs[RecordIdx]._pad0
                          // for cement layer-index + validity bit when useCementAtlas != 0.
                          // Legacy chain emits the matching declaration in gos_terrain.tese
                          // (NOT gos_terrain.vert — TCS strips VS outputs not consumed).

// Uniforms used by this shader
uniform int  ssboRecordBase;     // global record index offset for this draw call
// Fix B 2026-05-14: terrainMVP uniform REMOVED from the thin VS — projection
// now comes from tr.clipPos[cornerIdx] written by the producer.
// gosRenderer::terrainBindThinUniformsForPatchStream still uploads it for
// other thin programs (water-fast/mask/etc.); the upload silently no-ops
// here because glGetUniformLocation returns -1 for an absent declaration.
// Fix A scaffolding demoted behind MC2_RING_TRACE=1 env-gate for regression
// probing (VPL retirement step 9, 2026-05-15); default-off. The vertex shader
// has no terrainMVP uniform; clipPos in the thin record is the sole
// projection authority (Fix B). Fix A's terrainOverrideThinMVP path is inert
// regardless (cached loc -1); the per-slot MVP snapshot that fed it is no
// longer populated unless MC2_RING_TRACE is set.
uniform vec4 terrainViewport;    // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;                // projection_: screen pixels -> NDC

// Atlas UV decomposition — set by the indirect bridge for glMultiDrawArraysIndirect.
// The full merged colormap is bound as a single GL_TEXTURE_2D; per-tile UV (stored
// in recipe.uvData as fractions within one tile) must be converted to atlas-absolute
// UV by determining which tile the quad belongs to from its world position.
// Formula mirrors terrtxm2.cpp:resolveTextureHandle exactly:
//   posX  = (worldX - atlasMapTopLeftX) * atlasOneOverWorldUnits
//   posY  = (atlasMapTopLeftY - worldY) * atlasOneOverWorldUnits
//   tileX = floor((posX + 0.0005) * atlasNumTexturesAcross)
//   tileY = floor((posY + 0.0005) * atlasNumTexturesAcross)
//   atlasUV = (vec2(tileX, tileY) + perTileUV) / atlasNumTexturesAcross
uniform float atlasNumTexturesAcross;
uniform float atlasMapTopLeftX;
uniform float atlasMapTopLeftY;
uniform float atlasOneOverWorldUnits;

// Unpack ARGB uint to vec4 each component 0..255 -> 0..1.
vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

// Get uvec4 component by index 0-3.
// AMD RDNA3 GLSL compilers have been observed to mis-lower the chained-if
// pattern when inlined into per-vertex hot paths (amd-shader-reviewer
// finding, 2026-05-14).  Use an explicit array-init form: the compiler is
// required to produce table-lookup or branchless select, never speculative
// scalarization that could pick the wrong component.
uint uvec4Idx(uvec4 v, uint idx) {
    uint comps[4] = uint[4](v.x, v.y, v.z, v.w);
    return comps[idx];
}

void main() {
    uint vid          = uint(gl_VertexID);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint recordIdx    = uint(ssboRecordBase) + vid / 6u;
    RecordIdx = recordIdx;  // assign BEFORE pz-cull early-out (V21/v2.1 lesson:
                            // varyings left undefined on the early-out path leak
                            // garbage to the frag).

    TerrainQuadThinRecord tr = thinRecs[recordIdx];
    uint flags   = tr.control.z;
    uint uvMode  = flags & 1u;
    uint pzTri1  = (flags >> 1u) & 1u;
    uint pzTri2  = (flags >> 2u) & 1u;
    uint pzValid = (triIdx == 0u) ? pzTri1 : pzTri2;

    // pz-culled triangles: degenerate position (behind near clip, never rasterized).
    if (pzValid == 0u) {
        gl_Position    = vec4(0.0, 0.0, -2.0, 1.0);
        Color          = vec4(0.0);
        Texcoord       = vec2(0.0);
        TerrainType    = 0.0;
        WorldNorm      = vec3(0.0, 0.0, 1.0);
        WorldPos       = vec3(0.0);
        UndisplacedDepth = 0.0;
        return;
    }

    uint recipeIdx = tr.control.x;
    TerrainQuadRecipe rec = recipes[recipeIdx];

    // Corner index table — same convention as gos_terrain.tesc thin path.
    // TOPRIGHT  (uvMode=0): tri0=corners[0,1,2], tri1=corners[0,2,3]
    // BOTTOMLEFT(uvMode=1): tri0=corners[0,1,3], tri1=corners[1,2,3]
    //
    // AMD RDNA3 mis-lower hazard (mc2-terrain-indirect-expert, 2026-05-14):
    // the previous fix (commit 53cd157) replaced the wpsArr[cornerIdx]
    // CONSUMER with array indexing, but left the cornerIdx PRODUCER as a
    // nested ternary chain — which is the very pattern the comment below
    // warns against.  If RDNA3 mis-picks cornerIdx the array lookup
    // faithfully fetches the wrong corner — identical symptom to before.
    // Fix: full constant-table lookup keyed by (uvMode, triIdx, id).
    // Flat 12-entry form is friendliest to AMD's SPIR-V lowering — fewer
    // levels of array indirection than uint[2][2][3].
    const uint kCornerTable[12] = uint[12](
        0u, 1u, 2u,  // uvMode=0, triIdx=0  (TOPRIGHT tri0)
        0u, 2u, 3u,  // uvMode=0, triIdx=1  (TOPRIGHT tri1)
        0u, 1u, 3u,  // uvMode=1, triIdx=0  (BOTTOMLEFT tri0)
        1u, 2u, 3u   // uvMode=1, triIdx=1  (BOTTOMLEFT tri1)
    );
    uint cornerIdx = kCornerTable[uvMode * 6u + triIdx * 3u + id];

    // World position and normal from recipe corners.
    // AMD RDNA3 mis-lower hazard (amd-shader-reviewer 2026-05-14): the
    // chained-ternary form against a runtime cornerIdx is the documented
    // pattern most likely to cause one VS invocation to select a worldPos
    // belonging to a *different* corner than its siblings, producing a
    // triangle with one corner positioned wildly away from the others.
    // Symptom match: giant grey-banded terrain triangle under fast camera
    // rotation (worldPos varies across atlas tiles in a single tri).
    // Fix: pack into local arrays and index — compiler emits table lookup
    // or branchless select, never speculative scalarization.
    vec4 wpsArr[4] = vec4[4](rec.worldPos0, rec.worldPos1, rec.worldPos2, rec.worldPos3);
    vec4 wnsArr[4] = vec4[4](rec.worldNorm0, rec.worldNorm1, rec.worldNorm2, rec.worldNorm3);
    vec4 wp = wpsArr[cornerIdx];
    vec4 wn = wnsArr[cornerIdx];
    vec3 worldPos  = wp.xyz;
    vec3 worldNorm = normalize(wn.xyz);

    // Per-tile UV (LEGACY semantic): cornerIdx-based selection of recipe.uvData.
    //   corner 0=(minU,minV), 1=(maxU,minV), 2=(maxU,maxV), 3=(minU,maxV)
    // Atlas-absolute UV is reconstructed in the frag from WorldPos via atlas*
    // uniforms (see gos_terrain.frag) — keeping it out of the varying interface
    // preserves linker compatibility with the legacy non-thin VS chain.
    {
        float tileU = (cornerIdx == 1u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
        float tileV = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;
        Texcoord = vec2(tileU, tileV);
    }

    // Lighting per corner.
    uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);

    // TerrainType: packed by CPU into recipe._wp0 (worldPos0.w), 4 corners × 8 bits.
    uint terrainTypes = floatBitsToUint(rec.worldPos0.w);
    TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);

    Color       = unpackARGB(lrgb);
    // Texcoord already set by the atlas UV block above.
    WorldNorm   = worldNorm;
    WorldPos    = worldPos;

    // Fix B 2026-05-14: clip-space position is now READ from the thin record
    // (writer pre-projected via the same u_terrainMVP it used for pzOk gates,
    // so by construction the projection and the gate agree).  The VS no
    // longer touches terrainMVP at all — eliminating the temporal MVP
    // misalignment that Fix A patched at the binder, by fixing it at the
    // source.  See docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md
    // and memory/ring_slot_state_must_travel_with_slot.md for context.
    // Same array-form indexing pattern as wpsArr[cornerIdx] above (AMD RDNA3
    // mis-lower mitigation — kCornerTable produces cornerIdx; array indexing
    // produces table-lookup or branchless select, never speculative scalarization).
    vec4 clip = tr.clipPos[cornerIdx];
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Match legacy CPU emit's TERRAIN_DEPTH_FUDGE=0.002 (mclib/quad.cpp:1707)
    // so decals/GpuStaticProps/water-on-terrain at coincident depth win the
    // GL_LEQUAL tie. Precedent: gos_terrain_water_fast.vert:350.
    // Doubled 0.001→0.002 post glClipControl(ZERO_TO_ONE); see gos_terrain.tese:133.
    screen.z = clip.z * rhw + 0.002;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position      = vec4(ndc.xyz * absW, absW);
    // glClipControl(ZERO_TO_ONE) makes screen.z (D3D-style [0, 1]) native;
    // matches gl_FragCoord.z range without remap.
    UndisplacedDepth = screen.z;
}
