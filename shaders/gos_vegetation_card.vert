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
layout(location=5) in uint  i_atlasFrame; // bits 0-3 = atlas index (0..7); bits 4-5 = card role (0=vertical,1=tilted,2=top)
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

// MC2_VEG_DEBUG_FORCE_VISIBLE=1: bypass blockVis/LOD cull — forces all instances
// to LOD0 regardless of SSBO. If this makes cards appear, blockVis/shader cull is
// the failing stage. If cards still absent, placement/draw/texture/depth is guilty.
uniform int u_forceVisible;

out vec2  v_atlasUV;
out float v_camDist;
out float v_camTrueDist;
out float v_cardBottom;   // 1=roots, 0=tips
out float v_seed;
out vec3  v_worldPos;
out float v_lodFade;      // 1.0=LOD0 full, 0.4=LOD1 dithered, 0.0=culled (unused in frag)
out float v_tilt;         // 0.0=vertical, 0.5=tilted, 1.0=top/cap — role for frag alpha/dim

void main()
{
    // Atlas UV: lower 4 bits = atlas index; bits 4-5 = card role.
    uint atlasIdx = i_atlasFrame & 0x0Fu;
    uint frameCol = atlasIdx % 4u;
    uint frameRow = atlasIdx / 4u;
    v_atlasUV = vec2(a_uv.x * 0.25 + float(frameCol) * 0.25,
                     float(frameRow) * 0.5 + (1.0 - a_uv.y) * 0.5);

    // Compute terrain block index from world position at render time.
    uint blockIdx = 0u;
    float lodFade = 1.0;
    if (u_chunkSide > 0 && u_blockSideWU > 0.0) {
        int bCol = clamp(int((i_worldPos.x + u_mapHalfWU) / u_blockSideWU), 0, u_chunkSide - 1);
        int bRow = clamp(int((-i_worldPos.y + u_mapHalfWU) / u_blockSideWU), 0, u_chunkSide - 1);
        blockIdx  = uint(bRow * u_chunkSide + bCol);
        uint lodVis = (u_forceVisible != 0) ? 2u : b_blockVis[blockIdx];
        if (lodVis == 0u) {
            gl_Position  = vec4(2.0, 2.0, 2.0, 1.0);
            v_atlasUV    = vec2(0.0); v_camDist = 0.0; v_camTrueDist = 0.0;
            v_cardBottom = 0.0;       v_seed    = 0.0; v_worldPos    = vec3(0.0);
            v_lodFade    = 0.0;       v_tilt    = 0.0;
            return;
        }
        lodFade = (lodVis >= 2u) ? 1.0 : 0.4;
    }

    // Card role from bits 4-5 of i_atlasFrame:
    //   0 = vertical (~55% of instances, 0° tilt)
    //   1 = tilted   (~30% of instances, 45° from vertical)
    //   2 = top/cap  (~15% of instances, 80° from vertical — nearly horizontal)
    uint  cardRole  = (i_atlasFrame >> 4u) & 0x3u;
    float tiltAngle = (cardRole == 2u) ? 1.3963 : (cardRole == 1u) ? 0.7854 : 0.0;
    float vertFactor = cos(tiltAngle);  // 1.0 / 0.707 / 0.174
    float flatFactor = sin(tiltAngle);  // 0.0 / 0.707 / 0.985

    // Top/cap cards are smaller so they don't dominate the silhouette.
    float effectiveScale = i_scale * (cardRole == 2u ? 0.6 : 1.0);

    float totalYaw = i_yaw + a_card.z;
    float cy = cos(totalYaw), sy = sin(totalYaw);

    // Horizontal spread: a_card.x rotated by totalYaw.
    float eastFromX  = a_card.x * cy;
    float northFromX = a_card.x * sy;
    // Vertical component: a_card.y rises if vertFactor>0, spreads outward if flatFactor>0.
    // Outward direction = perpendicular to card face = (−sy, cy).
    float eastFromY  = -a_card.y * sy * flatFactor;
    float northFromY =  a_card.y * cy * flatFactor;
    float upFromY    =  a_card.y      * vertFactor;

    // Wind: two overlapping harmonics for organic, non-repeating motion.
    // Top cards are lightly damped (horizontal face catches less lateral wind).
    float trueDist   = length(i_worldPos - u_cameraPos);
    float windFade   = 1.0 - smoothstep(300.0, 450.0, trueDist);
    float windPhase1 = u_time * 1.8 + i_worldPos.x * 0.07 + i_worldPos.y * 0.11 + i_seed * 6.28;
    float windPhase2 = u_time * 2.7 + i_worldPos.x * 0.13 + i_worldPos.y * 0.17 + i_seed * 3.14;
    float windNoise  = sin(windPhase1) * 0.75 + sin(windPhase2) * 0.25;
    float windStr    = (cardRole == 2u) ? 0.03 : 0.06;
    float sway = windNoise * a_card.y * windStr * effectiveScale * windFade * vertFactor;

    vec3 worldPos;
    worldPos.x = i_worldPos.x + (eastFromX + eastFromY) * effectiveScale + sway * cy;
    worldPos.y = i_worldPos.y + (northFromX + northFromY) * effectiveScale + sway * sy;
    worldPos.z = i_worldPos.z + upFromY * effectiveScale;

    v_camDist     = 0.0;
    v_camTrueDist = trueDist;
    v_cardBottom  = 1.0 - a_card.y;
    v_seed        = i_seed;
    v_worldPos    = worldPos;
    v_lodFade     = lodFade;
    v_tilt        = float(cardRole) * 0.5;  // 0.0=vertical, 0.5=tilted, 1.0=top

    // VEGETATION-DEPTH-BIAS: shift by 1× TERRAIN_DEPTH_FUDGE (−0.002), NOT 2×.
    // Terrain chunk applies 2× (−0.004). With GL_GREATER:
    //   D−0.002 > D−0.004 = TRUE  → veg wins over coplanar terrain surface ✓
    //   D−0.002 > D       = FALSE → veg stays behind static props ✓
    vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
    clip.z += TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
