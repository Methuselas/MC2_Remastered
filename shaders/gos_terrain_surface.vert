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
// META-FIX 2026-05-19: per-tile material table (binding 22). The surface VS
// forwards the REAL Texcoord / TerrainType varyings the frag's detail/POM
// (gos_terrain.frag:509-520) and concrete/cement (:465-470) paths consume,
// instead of the PR-2 placeholders (Texcoord=vec2(0), TerrainType=0) that
// collapsed detail to one texel and never engaged the concrete blend close-up.
// Greybeard META-FIX (memory/terrain_continuous_surface_forks_ruled_*):
// complete the 3-producer->1-frag varying contract from the PR-1 tile table
// (NOT a frag-side surface-mode branch -- that is the rejected additive trap).
// Binding 22 is free in the live LOD-OFF draw (the parked band-compute is the
// only other 22 user and is unreachable, kLodOffWave1 == true). Derivation
// mirrors gos_terrain_thin.vert:183-193 EXACTLY (uvData<->uvExt, same
// per-corner selectors; TerrainType from the wp0 4x8-bit pack).
layout(std430, binding = 22) readonly buffer SurfaceTileBuf {
    TerrainSurfaceTile surfaceTiles[];   // row-major-keyed: mx + my*cells
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
uniform mat4 u_worldToClipGL;   // world -> GL clip (kAxisSwapMC2toGL * worldToClip)
uniform int  u_mapSide;         // META-FIX: realVerticesMapSide; cells = mapSide-1.
                                // Recovers the ROW-MAJOR tile key from the cell's
                                // top-left vertexNum (ADJUST-1-emission-safe).

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
    Color       = vec4(1.0);    // neutral; per-fragment lighting in the frag
    RecordIdx   = 0u;           // frag only dereferences thinRecsFrag[] when
                                // useCementAtlas!=0; this draw binds
                                // useCementAtlas=0 so this is never read.

    // ---- META-FIX: REAL per-tile Texcoord / TerrainType varyings ----------
    // PR-1 emits 6 contiguous indices per quad cell (two tris); ADJUST-1
    // reordered the CELL VISITING order to block-clustered but kept each
    // cell's 6 indices contiguous, so cell ordinal = gl_VertexID / 6 and the
    // per-corner slot = gl_VertexID % 6 still hold. The PR-1 emit pushes the
    // top-left corner (c0 = mx + my*mapSide) as the FIRST index of every
    // cell's 6 for BOTH winding modes (gos_terrain_surface.cpp:240-244), so
    // surfaceIndices[(vid/6)*6] is always c0. The per-tile table
    // (surfaceTiles[]) is ROW-MAJOR-keyed (mx + my*cells) -- DECOUPLED from
    // the block-clustered emission order -- so recover the row-major key from
    // c0's grid coords (independent of ADJUST-1; this is why we do NOT index
    // surfaceTiles by vid/6 directly).
    {
        uint cellBase = (vid / 6u) * 6u;
        uint c0vn     = surfaceIndices[cellBase];
        uint ms       = uint(u_mapSide);
        uint mx       = c0vn % ms;
        uint my       = c0vn / ms;
        uint cells    = ms - 1u;
        uint cell     = mx + my * cells;            // ROW-MAJOR key (matches CPU)

        // Per-corner index within the cell. PR-1 emit order
        // (gos_terrain_surface.cpp:239-244), winding from
        // worldQuadUVMode == cellIsBottomLeft = ((mx&1) != (my&1)):
        //   BOTTOMLEFT  : indices {c0,c1,c3, c1,c2,c3} -> corners {0,1,3, 1,2,3}
        //   BOTTOMRIGHT : indices {c0,c1,c2, c0,c2,c3} -> corners {0,1,2, 0,2,3}
        // Recipe corner layout v0..v3 == this c0..c3 (top-left=0, then
        // (mx+1,my)=1, (mx+1,my+1)=2, (mx,my+1)=3), identical to the thin
        // path's cornerIdx semantics (gos_terrain_thin.vert:177-184).
        uint slot = vid % 6u;
        bool bottomLeft = ((mx & 1u) != (my & 1u));
        uint cornerIdx;
        if (bottomLeft) {
            uint blCorners[6] = uint[6](0u,1u,3u, 1u,2u,3u);
            cornerIdx = blCorners[slot];
        } else {
            uint brCorners[6] = uint[6](0u,1u,2u, 0u,2u,3u);
            cornerIdx = brCorners[slot];
        }

        TerrainSurfaceTile tile = surfaceTiles[cell];

        // Per-tile UV -- mirrors gos_terrain_thin.vert:183-185 EXACTLY.
        // uvExt = (minU, minV, maxU, maxV) == thin recipe uvData.xyzw layout:
        //   corner 0=(minU,minV) 1=(maxU,minV) 2=(maxU,maxV) 3=(minU,maxV)
        float tileU = (cornerIdx == 1u || cornerIdx == 2u) ? tile.uvExt.z
                                                           : tile.uvExt.x;
        float tileV = (cornerIdx == 0u || cornerIdx == 1u) ? tile.uvExt.y
                                                           : tile.uvExt.w;
        Texcoord = vec2(tileU, tileV);

        // TerrainType -- mirrors gos_terrain_thin.vert:191-193 EXACTLY.
        // wp.x == recipe._wp0: 4 corner material types, 4 x 8 bits.
        uint terrainTypes = tile.wp.x;
        TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);
    }

    // ---- F1 Stage A: direct GL clip emit with pre-divide depth bias ----------
    // Apply TERRAIN_DEPTH_FUDGE in clip space before the hardware divide so the
    // post-divide NDC offset stays ~constant. Same effect as the legacy Fork D
    // pre-divide form (clip.z += FUDGE * clip.w), now against the true GL clip.
    vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
    clip.z   += TERRAIN_DEPTH_FUDGE * clip.w;  // PRE-divide; *clip.w keeps post-divide offset ~const
    gl_Position      = clip;
    UndisplacedDepth = clip.z / clip.w;  // no displacement on base surface; overlay tie uses this

    // ---- Clip-safe front/far reject (raster-sheet guard) --------------------
    // Degenerate vertices (clip.w <= 0) would produce NaN or sign-flipped NDC,
    // causing a frustum-spanning raster sheet
    // (memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md).
    // Under ZERO_TO_ONE, in-frustum verts have clip.w > 0 and clip.z in [0, clip.w].
    // Collapse out-of-frustum verts to a degenerate off-screen point.
    if (!(clip.w > 0.0 && clip.z >= 0.0 && clip.z <= clip.w))
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
