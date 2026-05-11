//#version 430 (version provided by material prefix)
//
// B4 Slice Stage 1b — mask-SOLID dual-run vertex shader.
//
// Reads:
//   binding=17  SolidMaskBuf   (per-frame bitset, one bit per quad vertexNum)
//   binding=19  RecipeBuf      (dense recipe SSBO, indexed by vertexNum)
//   binding=2   LightingBuf    (GpuTerrainLightingOutput per vertex)
//
// One draw covers ALL quadCount*6 vertices in the dense recipe. Quads outside
// the mask are degenerated (gl_Position pushed offscreen) so the rasterizer
// drops them.

// --- SSBO bindings ---
layout(std430, binding = 17) readonly buffer SolidMaskBuf {
    uint solidMask[];
};

struct TerrainQuadRecipe {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;  // minU, minV, maxU, maxV
};
layout(std430, binding = 19) readonly buffer RecipeBuf {
    TerrainQuadRecipe recipes[];
};

struct GpuTerrainLightingOutput {
    uint lightRGB;
    uint fogRGB;
};
layout(std430, binding = 2) readonly buffer LightingBuf {
    GpuTerrainLightingOutput lighting[];
};

// Output varyings — names MUST match gos_terrain.frag `in` declarations exactly.
// Same set as gos_terrain_thin.vert for linker compatibility.
out vec4  Color;
out vec2  Texcoord;
out float TerrainType;
out vec3  WorldNorm;
out vec3  WorldPos;
out float UndisplacedDepth;
flat out uint RecordIdx;

// Uniforms (shared names with gos_terrain_thin.vert)
uniform mat4  terrainMVP;
uniform vec4  terrainViewport;
uniform mat4  mvp;
uniform int   mapSide;
uniform float atlasNumTexturesAcross;
uniform float atlasMapTopLeftX;
uniform float atlasMapTopLeftY;
uniform float atlasOneOverWorldUnits;

// Unpack ARGB uint to vec4. Stored byte-layout is BGRA in memory (= ARGB in
// uint LE), matching legacy lightRGB packing.
vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

void main() {
    uint vid     = uint(gl_VertexID);
    uint quadIdx = vid / 6u;
    uint vertInQ = vid % 6u;
    uint triIdx  = vertInQ / 3u;
    uint id      = vertInQ % 3u;

    // Assign RecordIdx early so it's always defined (lesson from V21/v2.1:
    // varyings left undefined on an early-out path leak garbage to the frag).
    RecordIdx = quadIdx;

    // ---- Mask cull: emit degenerate triangle if not in the SOLID mask ----
    uint maskWord = solidMask[quadIdx >> 5u];
    uint maskBit  = (maskWord >> (quadIdx & 31u)) & 1u;
    if (maskBit == 0u) {
        gl_Position      = vec4(2.0, 2.0, 2.0, 1.0);
        Color            = vec4(0.0);
        Texcoord         = vec2(0.0);
        TerrainType      = 0.0;
        WorldNorm        = vec3(0.0, 0.0, 1.0);
        WorldPos         = vec3(0.0);
        UndisplacedDepth = 0.0;
        return;
    }

    // ---- uvMode from checkerboard pattern (mirrors mapdata.cpp:117) ------
    // BOTTOMRIGHT (0) when (tileR & 1) == (tileC & 1), else BOTTOMLEFT (1).
    uint ms     = uint(mapSide);
    uint mx     = quadIdx % ms;
    uint my     = quadIdx / ms;
    uint uvMode = ((my & 1u) == (mx & 1u)) ? 0u : 1u;

    // ---- Corner permutation (same table as gos_terrain_thin.vert) --------
    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        } else {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
        }
    } else {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        } else {
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
        }
    }

    // ---- Corner vertex number (vn) ---------------------------------------
    // corner 0: (mx,   my),     corner 1: (mx+1, my)
    // corner 2: (mx+1, my+1),   corner 3: (mx,   my+1)
    uint vn;
    if      (cornerIdx == 0u) vn = quadIdx;
    else if (cornerIdx == 1u) vn = quadIdx + 1u;
    else if (cornerIdx == 2u) vn = quadIdx + ms + 1u;
    else                      vn = quadIdx + ms;

    // ---- Read recipe -----------------------------------------------------
    TerrainQuadRecipe rec = recipes[quadIdx];

    vec4 wp = (cornerIdx == 0u) ? rec.worldPos0
             :(cornerIdx == 1u) ? rec.worldPos1
             :(cornerIdx == 2u) ? rec.worldPos2
             :                    rec.worldPos3;
    vec4 wn = (cornerIdx == 0u) ? rec.worldNorm0
             :(cornerIdx == 1u) ? rec.worldNorm1
             :(cornerIdx == 2u) ? rec.worldNorm2
             :                    rec.worldNorm3;

    vec3 worldPos  = wp.xyz;
    vec3 worldNorm = normalize(wn.xyz);

    // ---- Per-tile UV (cornerIdx-based selection, same as thin VS) --------
    {
        float tileU = (cornerIdx == 1u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
        float tileV = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;
        Texcoord = vec2(tileU, tileV);
    }

    // ---- TerrainType (packed by CPU into recipe._wp0 a.k.a. worldPos0.w) -
    uint terrainTypes = floatBitsToUint(rec.worldPos0.w);
    TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);

    // ---- Lighting from per-vertex GPU compute output SSBO ---------------
    // When MC2_TERRAIN_LIGHTING_GPU is not armed, the SSBO may hold zeros
    // and terrain renders dark. Parity (which quads draw) is unaffected.
    Color = unpackARGB(lighting[vn].lightRGB);

    WorldNorm = worldNorm;
    WorldPos  = worldPos;

    // ---- Double-projection (identical to gos_terrain_thin.vert) ---------
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw + 0.002;  // TERRAIN_DEPTH_FUDGE (quad.cpp:1911)
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position      = vec4(ndc.xyz * absW, absW);
    UndisplacedDepth = screen.z;
}
