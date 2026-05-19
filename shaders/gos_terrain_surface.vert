//#version 430 (version provided by makeProgram prefix "#version 430\n";
//              NEVER a #version line in the shader file -- worktree CLAUDE.md)
//
// [TERRAIN_SURFACE] continuous shared-vertex INDEXED surface vertex shader.
//
// Plan : docs/superpowers/plans/2026-05-18-terrain-continuous-surface-producer-plan.md
//        PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF). Design Sections 1.2/1.3 (Fork V
//        = V-ssbo, RULED), Section 2 (per-fragment WorldPos material PRESERVED,
//        NC1), Section 4 (Fork D clip-space PRE-perspective-divide reverse-Z
//        bias, RULED).
//
// Fork V = V-ssbo (vertex-pulling SSBO, RULED at PR-2 kickoff): this VS reads
// the mission-static surface vertex SSBO by a BAKED INDEX fetched from the
// mission-static index SSBO via gl_VertexID. NO IBO / no VAO element-array
// state (memory/element_array_buffer_is_vao_state_new_draw_paths_own_their_vao.md);
// the draw is a plain glDrawArrays over indexCount vertices, the indirection
// lives entirely in the index SSBO read -- the smallest delta from the
// existing thin-record SSBO-pull pattern (gos_terrain_thin.vert) and the
// smoothest mesh-shader on-ramp.
//
// --- Fork D reverse-Z depth bias: BY CONSTRUCTION, not the deleted mechanism.
//
// The deleted-scope bug (design Section 4.1, gos_terrain_thin.vert:210-225 /
// gos_terrain.tese:126-141) adds the depth fudge to screen.z AFTER the
// perspective divide ( screen.z = clip.z*rhw + FUDGE ) then re-projects. A
// constant added post-divide is distance-NONLINEAR -- the water-recession /
// grazing decal z-fight root mechanism. This VS does NOT port that.
//
// Reconciliation with the MC2 D3D pixel-homogeneous chain (load-bearing,
// memory/terrain_mvp_gl_false.md, code/gamecam.cpp:150-166): terrainMVP =
// axisSwap*worldToClip is NOT a GL clip matrix -- terrainMVP*world yields D3D
// pixel-homogeneous coords; the pixels->NDC step (terrainViewport + the `mvp`
// uniform = projection_, gameos_graphics.cpp:4158, z-row (0,0,1,0)) is a REAL
// coordinate conversion the camera comment explicitly says "can't be matrix".
// So the design's literal "gl_Position = clip; NO second mvp multiply" is not
// realizable here -- but its SUBSTANTIVE Fork D requirement (bias applied in
// clip space PRE-divide so the mechanism is NOT the post-divide-reproject
// nonlinearity) IS:
//
//     clip.z += LAYER_BIAS * clip.w;   // PRE-divide (Fork D §4.2)
//     screen.z = clip.z * rhw;         // NO additive post-divide fudge
//
// Multiplying the epsilon by clip.w before the 1/w divide yields a post-divide
// NDC-z offset ~= LAYER_BIAS (approximately constant with distance -- design
// §4.2), vs the deleted constant-post-divide form which is distance-nonlinear.
// The subsequent screen->NDC `mvp` multiply only remaps x,y (its z-row is
// (0,0,1,0), verified gameos_graphics.cpp:4160) so gl_FragCoord.z == screen.z
// == clip.z*rhw with the bias correctly carried through the divide. This is
// the pixel-chain-faithful realization of Fork D, NOT a screen-z reconstruction
// (the bias never touches a post-divide scalar). CONSTANT to start (design
// §4.2 RULED); slope/dist-capable later as a Fork DE TERM change in LAYER_BIAS.

#include <include/terrain_depth_bias.hglsl>          // single-sourced reverse-Z constants
#include <include/gos_terrain_surface_schema.hglsl>  // std430 lockstep mirror

// ---- Surface SSBOs (V-ssbo). Bindings 20/21 chosen NOT to collide with the
//      live thin path (1=recipe, 2=thin-record) or compute-cull (11/12): a
//      private high range so the additive PR-2 validation draw never disturbs
//      the armed indirect path or any other pipeline.
layout(std430, binding = 20) readonly buffer SurfaceVertexBuf {
    TerrainSurfaceVertex surfaceVerts[];
};
layout(std430, binding = 21) readonly buffer SurfaceIndexBuf {
    uint surfaceIndices[];   // 6 baked vertexNum indices per quad cell
};

// Output varyings -- this set MUST match gos_terrain.frag `in` declarations
// EXACTLY (Color, Texcoord, TerrainType, WorldNorm, WorldPos, UndisplacedDepth,
// flat RecordIdx) and stay compatible with the legacy non-thin chain that
// feeds the same frag. A mismatched varying set is a SILENT linker failure ->
// terrain renders transparent to skybox (gos_terrain_thin.vert:36-53 warning).
out vec4  Color;
out vec2  Texcoord;
out float TerrainType;
out vec3  WorldNorm;
out vec3  WorldPos;
out float UndisplacedDepth;
flat out uint RecordIdx;

// Projection chain uniforms (set by terrainBindThinUniformsForPatchStream;
// SAME names/semantics as gos_terrain_thin.vert / gos_terrain.tese).
uniform mat4 terrainMVP;        // axisSwap*worldToClip (D3D pixel-homogeneous)
uniform vec4 terrainViewport;   // (vmx, vmy, vax, vay) post-divide viewport map
uniform mat4 mvp;               // projection_: screen pixels -> NDC (z-row identity)

void main() {
    // V-ssbo indirection: gl_VertexID -> baked vertexNum -> surface vertex.
    // glDrawArrays(GL_TRIANGLES, 0, indexCount): one VS invocation per emitted
    // index; the index SSBO is the mission-static topology (two tris/cell, the
    // worldQuadUVMode diagonal parity already baked CPU-side in PR-1).
    uint vid       = uint(gl_VertexID);
    uint vertexNum = surfaceIndices[vid];
    TerrainSurfaceVertex sv = surfaceVerts[vertexNum];

    vec3 worldPos  = sv.pos.xyz;
    vec3 worldNorm = normalize(sv.normal.xyz);

    // Per-fragment material model PRESERVED (design Section 2, NC1): the VS
    // forwards ONLY WorldPos -- the frag does the floor(WorldPos->tile) bucket
    // + atlas-absolute UV reconstruction itself (gos_terrain.frag:338-346,
    // useAtlasColormap=1). NO tile/cement/atlas decision is moved to a vertex
    // or interpolated as a varying. Texcoord is a per-tile [0,1] coordinate
    // for the frag's detail tiling / anti-tile / POM math; with the atlas
    // colormap path the surface uses WorldPos-derived UV in the frag, so a
    // benign per-vertex placeholder here is sufficient and never selects the
    // colormap tile (that is WorldPos-driven in the frag).
    WorldPos    = worldPos;
    WorldNorm   = worldNorm;
    Texcoord    = vec2(0.0);
    TerrainType = 0.0;          // surface material is per-fragment (frag classifies)
    Color       = vec4(1.0);    // neutral; per-fragment lighting in the frag
    RecordIdx   = 0u;           // frag only dereferences thinRecsFrag[] when
                                // useCementAtlas!=0; PR-2 validation draw binds
                                // useCementAtlas=0 so this is never read.

    // ---- Fork D: clip-space PRE-perspective-divide reverse-Z bias ----------
    // terrainMVP*world -> D3D pixel-homogeneous clip (see header block). Apply
    // the single-sourced, already-flipped (do NOT re-flip) reverse-Z terrain
    // layer bias in clip space BEFORE the 1/w divide. consume TERRAIN_DEPTH_FUDGE
    // unchanged (terrain_depth_bias.hglsl:43, = -0.002, reverse-Z convention:
    // OVERLAY>0>WATER; static_assert pinned C++ side terrain_depth_bias.h:78-81).
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    clip.z   += TERRAIN_DEPTH_FUDGE * clip.w;   // PRE-divide; *clip.w keeps the
                                                // post-divide NDC offset ~const
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;                    // NO additive post-divide fudge
                                                // (the deleted §4.1 mechanism)
    vec4 ndc  = mvp * vec4(screen, 1.0);        // pixels->NDC; z-row (0,0,1,0)
                                                // so ndc.z == screen.z exactly
    float absW = abs(clip.w);                   // memory/clip_w_sign_trap.md:
                                                // never sign(clip.w); abs is
                                                // load-bearing for oblique cam
    gl_Position      = vec4(ndc.xyz * absW, absW);
    UndisplacedDepth = screen.z;                // no displacement on the base
                                                // surface; overlay tie uses this
}
