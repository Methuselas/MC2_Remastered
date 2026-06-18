//#version 430 (version provided by material prefix)

// Vertical crossed-quad vegetation card shader.
// Coordinate space: terrain-chunk, matching terrain_lod_chunk.vert:
//   x = east_centered   (increases eastward)
//   y = north_centered  (increases northward)
//   z = elevation       (increases upward)
// Matrix u_worldToClipGL = gos_GetTerrainMVPMat4() — same as terrain_lod_chunk.vert.

// Per-vertex static attributes (8 vertices, 2 crossed vertical quads)
// a_card.x = width_local  [-0.5..0.5]  horizontal spread along card face
// a_card.y = height_local [0..1]       0=ground, 1=tip (goes upward in Z)
// a_card.z = yawOffset    (0 or PI/2)  crosses two quads 90 degrees apart
layout(location=0) in vec3 a_card;
layout(location=1) in vec2 a_uv;

// Per-instance attributes (terrain-chunk space)
layout(location=2) in vec3  i_worldPos;   // (east_centered, north_centered, elevation)
layout(location=3) in float i_yaw;        // rotation about elevation axis (radians)
layout(location=4) in float i_scale;      // card height in WU (also controls width)
layout(location=5) in uint  i_atlasFrame; // 0..7
layout(location=6) in float i_seed;       // per-instance random [0,1]
// location 7 removed — blockIdx computed in shader from world position

// terrain-chunk MVP (same convention as terrain_lod_chunk.vert u_worldToClipGL)
#include <include/terrain_depth_bias.hglsl>
uniform mat4  u_worldToClipGL;
uniform float u_time;
uniform vec3  u_cameraPos;  // camera position in terrain-chunk space (wind fade only)

// Terrain block-index computation.
// blockIdx = bRow * u_chunkSide + bCol, computed per-vertex from i_worldPos.
// Avoids stale baked value from mission-load-time when chunk system wasn't ready.
uniform float u_mapHalfWU;   // worldUnitsMapSide * 0.5
uniform float u_blockSideWU; // worldUnitsBlockSide (2560.0)
uniform int   u_chunkSide;   // s_terrainChunkSide

// Per-block LOD visibility SSBO (binding 12), updated each frame.
//   0 = cull (lodLevel >= 2)
//   1 = LOD1  (visible but dithered in frag — looks sparser at distance)
//   2 = LOD0  (full bright, full density)
layout(binding=12, std430) readonly buffer BlockVis { uint b_blockVis[]; };

out vec2  v_atlasUV;
out float v_camDist;
out float v_camTrueDist;
out float v_cardBottom;   // 1=roots, 0=tips
out float v_seed;
out vec3  v_worldPos;
out float v_lodFade;      // 1.0=LOD0 full, 0.4=LOD1 dithered, 0.0=culled (unused in frag)

void main()
{
    // 2-row × 4-col atlas (2048×1024).
    uint frameCol = i_atlasFrame % 4u;
    uint frameRow = i_atlasFrame / 4u;
    v_atlasUV = vec2(a_uv.x * 0.25 + float(frameCol) * 0.25,
                     float(frameRow) * 0.5 + (1.0 - a_uv.y) * 0.5);

    // Compute terrain block index from world position at render time.
    // i_worldPos is in terrain-chunk space (east/north centered).
    // bCol = (gameWorldX - originX) / blockSide = (i_worldPos.x + mapHalf) / blockSide
    // bRow = (originY   - gameWorldY) / blockSide = (-i_worldPos.y + mapHalf) / blockSide
    uint blockIdx = 0u;
    float lodFade = 1.0;
    if (u_chunkSide > 0 && u_blockSideWU > 0.0) {
        int bCol = clamp(int((i_worldPos.x + u_mapHalfWU) / u_blockSideWU), 0, u_chunkSide - 1);
        int bRow = clamp(int((-i_worldPos.y + u_mapHalfWU) / u_blockSideWU), 0, u_chunkSide - 1);
        blockIdx  = uint(bRow * u_chunkSide + bCol);
        uint lodVis = b_blockVis[blockIdx];
        if (lodVis == 0u) {
            // Cull: clip to outside NDC — all 8 verts of the crossed-quad
            gl_Position  = vec4(2.0, 2.0, 2.0, 1.0);
            v_atlasUV    = vec2(0.0); v_camDist = 0.0; v_camTrueDist = 0.0;
            v_cardBottom = 0.0;       v_seed    = 0.0; v_worldPos    = vec3(0.0);
            v_lodFade    = 0.0;
            return;
        }
        // 2u = LOD0 (close, full density).  1u = LOD1 (far, Bayer-dithered in frag).
        // LOD1 flat-card was replaced by vertical cards everywhere; flat mode removed.
        lodFade = (lodVis >= 2u) ? 1.0 : 0.4;
    }

    // LOD0 = vertical crossed-quad billboard (full 3D card).
    // LOD1 = flat horizontal ground patch: card lies on terrain surface.
    //   a_card.x → east/north spread (unchanged),
    //   a_card.y → second horizontal axis instead of height (rotated 90° in tangent plane),
    //   Z stays at i_worldPos.z (ground level).
    // This makes LOD1 look like a ground-cover patch rather than a floating billboard.
    float vertFactor = (lodFade >= 1.0) ? 1.0 : 0.0;  // 1=vertical (LOD0), 0=flat (LOD1)
    float flatFactor = 1.0 - vertFactor;

    float totalYaw = i_yaw + a_card.z;
    float cy = cos(totalYaw), sy = sin(totalYaw);

    // Horizontal spread (both modes): a_card.x rotated by totalYaw
    float eastFromX  = a_card.x * cy;
    float northFromX = a_card.x * sy;
    // Vertical-mode: a_card.y goes straight up.
    // Flat-mode:     a_card.y goes into the perpendicular horizontal axis (−sy, cy).
    float eastFromY  = -a_card.y * sy * flatFactor;
    float northFromY =  a_card.y * cy * flatFactor;
    float upFromY    =  a_card.y      * vertFactor;

    // Wind sway — vertical cards only (flat cards are ground, no sway)
    float trueDist  = length(i_worldPos - u_cameraPos);
    float windFade  = 1.0 - smoothstep(300.0, 450.0, trueDist);
    float windPhase = u_time * 1.8 + i_worldPos.x * 0.07 + i_worldPos.y * 0.11 + i_seed * 6.28;
    float sway = sin(windPhase) * a_card.y * 0.06 * i_scale * windFade * vertFactor;

    // Flat cards use 2× scale in horizontal so they cover more ground area
    float scaleMult = 1.0 + flatFactor * 1.5;  // LOD0=1.0, LOD1=2.5

    vec3 worldPos;
    worldPos.x = i_worldPos.x + (eastFromX  + eastFromY)  * i_scale * scaleMult + sway * cy;
    worldPos.y = i_worldPos.y + (northFromX  + northFromY) * i_scale * scaleMult + sway * sy;
    worldPos.z = i_worldPos.z + upFromY * i_scale;

    v_camDist     = 0.0;
    v_camTrueDist = trueDist;
    v_cardBottom  = 1.0 - a_card.y;
    v_seed        = i_seed;
    v_worldPos    = worldPos;
    v_lodFade     = lodFade;

    vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
