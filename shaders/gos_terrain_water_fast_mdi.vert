//#version 430 (provided by makeProgram prefix)
// GL_ARB_shader_draw_parameters extension + #define MC2_WATER_MDI 1
// are prepended by the host via the makeProgram preamble (not declared here).
// gl_DrawIDARB is used below (ARB-suffixed name for 4.3 compat).
//
// MDI variant of gos_terrain_water_fast.vert for Task 1.5 (Stage 1 GPU-driven water).
// Replaces per-pass uniforms (uvScale, uvOffset, detailMode, isWater) with a
// WaterPerCmd SSBO indexed by gl_DrawID so that base + detail layers share a
// single glMultiDrawArraysIndirect call.
//
// Changes from gos_terrain_water_fast.vert:
//   - uniforms uvScale, uvOffset, detailMode removed (now from WaterPerCmd SSBO)
//   - uniform isWater removed (now from WaterPerCmd SSBO, passed to FS via flat out)
//   - WaterPerCmd SSBO added at binding 7
//   - flat out int o_isWater; added for gos_terrain_water_mdi.frag
//
// All other logic (projection chain, recipe/thin SSBO, wave math, debug modes)
// is identical to gos_terrain_water_fast.vert.

// --- Recipe SSBO (mission-static) ---
struct WaterRecipe {
    vec4  v01;
    vec4  v23;
    vec4  elev;
    uvec4 ctrl;
};
layout(std430, binding = 5) readonly buffer WaterRecipeBuf {
    WaterRecipe recipes[];
};

// --- Per-frame thin record SSBO (kWaterThinSsboBinding = 6) ---
struct WaterThinRecord {
    uvec4 ctrl;        // (recipeIdx, flags, _pad, _pad)
    uvec4 lightRGB;    // corner0..3 ARGB
    uvec4 fogRGB;      // corner0..3 fogRGB; .w byte is FogValue
};
layout(std430, binding = 6) readonly buffer WaterThinBuf {
    WaterThinRecord thinRecs[];
};
const uint kPzTri1ValidBit = 0x1u;
const uint kPzTri2ValidBit = 0x2u;

// --- Per-draw cmd SSBO (binding 7) — 32 B std430, lockstep with WaterPerCmd C++ ---
struct WaterPerCmd {
    uint  textureSlot;   // reserved; texture unit selection is driven by o_isWater in FS
    int   isWater;       // 1 = base, 2 = detail (matches legacy isWater uniform)
    int   detailMode;    // 0 = base, 1 = detail
    float uvScale;
    float uvOffsetX;
    float uvOffsetY;
    uint  pad0_;
    uint  pad1_;
};
layout(std430, binding = 7) readonly buffer PerCmdBuf { WaterPerCmd perCmd[]; };

// Output varyings — must match gos_terrain_water_mdi.frag `in` exactly.
out vec4  Color;
out vec2  Texcoord;
out float FogValue;
flat out int o_isWater;

// Uniforms — set by Terrain::renderWaterFastPath C++ code.
uniform mat4  terrainMVP;        // axisSwap * worldToClip
uniform mat4  mvp;               // projection_: screen pixels -> NDC
uniform vec4  terrainViewport;   // (vmx, vmy, vax, vay)
uniform float waterElevation;    // Terrain::waterElevation
uniform float alphaDepth;        // MapData::alphaDepth
uniform vec2  mapTopLeft;        // Terrain::mapTopLeft3d.xy (note: y is positive-up)
uniform float frameCos;          // Terrain::frameCos (per-frame oscillator)
uniform float frameCosAlpha;     // Terrain::frameCosAlpha
// Per-pass alpha-band byte values (alphaEdge / alphaMiddle / alphaDeep are
// DWORDs whose alpha-byte is OR'd into argb's alpha). Pass the alpha BYTE
// directly (0..255). Stored as `int` because `uniform uint` crashes the
// project's shader_builder (memory: uniform_uint_crash.md). Cast to uint
// inside the shader before bitwise ops.
uniform int  alphaEdgeByte;
uniform int  alphaMiddleByte;
uniform int  alphaDeepByte;
// Debug: 0 = normal, 1 = solid magenta opaque (verifies geometry),
//        2 = green from worldPos (verifies recipe data), 3 = red from UV.
uniform int   debugMode;
// MaxMinUV wrap floor — replicates legacy quad.cpp:2863-2884 wrap correction.
uniform float maxMinUV;

vec2 cornerXY(WaterRecipe r, uint cornerIdx) {
    if (cornerIdx == 0u) return r.v01.xy;
    if (cornerIdx == 1u) return r.v01.zw;
    if (cornerIdx == 2u) return r.v23.xy;
    return r.v23.zw;
}

float cornerElev(WaterRecipe r, uint cornerIdx) {
    if (cornerIdx == 0u) return r.elev.x;
    if (cornerIdx == 1u) return r.elev.y;
    if (cornerIdx == 2u) return r.elev.z;
    return r.elev.w;
}

uint unpackByte(uint packed, uint cornerIdx) {
    return (packed >> (cornerIdx * 8u)) & 0xFFu;
}

uint cornerLightRGB(WaterThinRecord t, uint cornerIdx) {
    if (cornerIdx == 0u) return t.lightRGB.x;
    if (cornerIdx == 1u) return t.lightRGB.y;
    if (cornerIdx == 2u) return t.lightRGB.z;
    return t.lightRGB.w;
}

uint cornerFogRGB(WaterThinRecord t, uint cornerIdx) {
    if (cornerIdx == 0u) return t.fogRGB.x;
    if (cornerIdx == 1u) return t.fogRGB.y;
    if (cornerIdx == 2u) return t.fogRGB.z;
    return t.fogRGB.w;
}

vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

float waveOurCos(uint waterBits) {
    if ((waterBits & 0x80u) != 0u) return -frameCos;
    return frameCos;
}

uint elevAlphaBandByte(float elev) {
    int a = alphaMiddleByte;
    if (elev >= (waterElevation - alphaDepth))
        a = alphaEdgeByte;
    if (elev <= (waterElevation - (alphaDepth * 3.0)))
        a = alphaDeepByte;
    return uint(a);
}

void main() {
    // Pull per-draw data from the WaterPerCmd SSBO using gl_DrawIDARB
    // (ARB-suffixed name; gl_DrawID is core only in GLSL 4.6).
    WaterPerCmd cmd       = perCmd[gl_DrawIDARB];
    float       uvScale   = cmd.uvScale;
    vec2        uvOffset  = vec2(cmd.uvOffsetX, cmd.uvOffsetY);
    int         detailMode = cmd.detailMode;

    // Emit isWater flat to the fragment shader.
    o_isWater = cmd.isWater;

    uint vid          = uint(gl_VertexID);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint thinIdx      = vid / 6u;

    WaterThinRecord trec = thinRecs[thinIdx];
    uint recipeIdx       = trec.ctrl.x;
    uint thinFlags       = trec.ctrl.y;
    WaterRecipe     rec  = recipes[recipeIdx];

    uint pzValid = (triIdx == 0u)
                   ? (thinFlags & kPzTri1ValidBit)
                   : (thinFlags & kPzTri2ValidBit);
    if (pzValid == 0u) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        Color       = vec4(0.0);
        Texcoord    = vec2(0.0);
        FogValue    = 0.0;
        return;
    }

    uint flags  = rec.ctrl.y;
    uint uvMode = flags & 1u;

    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        else
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
    } else {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        else
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
    }

    vec2 vxy   = cornerXY(rec, cornerIdx);
    float velev = cornerElev(rec, cornerIdx);
    uint waterBits = unpackByte(rec.ctrl.w, cornerIdx);

    float wz = waveOurCos(waterBits) + waterElevation;
    vec3 worldPos = vec3(vxy, wz);

    float u = (vxy.x - mapTopLeft.x) * uvScale + uvOffset.x;
    float v = (mapTopLeft.y - vxy.y) * uvScale + uvOffset.y;

    // Per-triangle MaxMinUV wrap correction (legacy quad.cpp:2863-2884).
    {
        uint c0, c1, c2;
        if (uvMode == 0u) {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=2u; }
            else              { c0=0u; c1=2u; c2=3u; }
        } else {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=3u; }
            else              { c0=1u; c1=2u; c2=3u; }
        }
        vec2 p0 = cornerXY(rec, c0);
        vec2 p1 = cornerXY(rec, c1);
        vec2 p2 = cornerXY(rec, c2);
        float u0 = (p0.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u1 = (p1.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u2 = (p2.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float v0 = (mapTopLeft.y - p0.y) * uvScale + uvOffset.y;
        float v1 = (mapTopLeft.y - p1.y) * uvScale + uvOffset.y;
        float v2 = (mapTopLeft.y - p2.y) * uvScale + uvOffset.y;
        float maxU = max(u0, max(u1, u2));
        float maxV = max(v0, max(v1, v2));
        if (maxU > maxMinUV || maxV > maxMinUV) {
            float shiftU = floor(maxU - (maxMinUV - 1.0));
            float shiftV = floor(maxV - (maxMinUV - 1.0));
            u -= shiftU;
            v -= shiftV;
        }
    }

    uint elevAlphaByte = elevAlphaBandByte(velev);
    uint argb;
    if (detailMode == 0) {
        uint lrgb = cornerLightRGB(trec, cornerIdx);
        argb = (lrgb & 0x00FFFFFFu) | (elevAlphaByte << 24);
    } else {
        argb = (elevAlphaByte << 24) | 0x00FFFFFFu;
    }

    uint frgb = cornerFogRGB(trec, cornerIdx);
    FogValue = float((frgb >> 24u) & 0xFFu) / 255.0;

    Color    = unpackARGB(argb).bgra;
    Texcoord = vec2(u, v);

    if (debugMode == 1) {
        Color = vec4(1.0, 0.0, 1.0, 1.0).bgra;
    } else if (debugMode == 2) {
        Color = vec4(0.0, 1.0, 0.0, 1.0).bgra;
    } else if (debugMode == 3) {
        Color = vec4(1.0, 1.0, 0.0, 1.0).bgra;
    } else if (debugMode == 4) {
        float a = float(elevAlphaByte) / 255.0;
        vec3 col = vec3(a, 1.0 - abs(a - 0.5) * 2.0, 1.0 - a);
        Color = vec4(col, 1.0).bgra;
        Texcoord = vec2(0.5);
    } else if (debugMode == 5) {
        vec3 col;
        if      (cornerIdx == 0u) col = vec3(1.0, 0.0, 0.0);
        else if (cornerIdx == 1u) col = vec3(0.0, 1.0, 0.0);
        else if (cornerIdx == 2u) col = vec3(0.0, 0.0, 1.0);
        else                       col = vec3(1.0, 1.0, 0.0);
        Color = vec4(col, 1.0).bgra;
        Texcoord = vec2(0.5);
    } else if (debugMode == 6) {
        float normElev = clamp(velev / 400.0, 0.0, 1.0);
        Color = vec4(normElev, 0.0, 1.0 - normElev, 1.0).bgra;
        Texcoord = vec2(0.5);
    }

    // Double projection chain — identical to gos_terrain_thin.vert.
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Three-tier z-ordering: terrain=+0.002, water=+0.003 (delta 0.001 so water
    // loses GL_LEQUAL to already-drawn terrain at shorelines; wins on open water).
    // Full change history and drift rationale: gos_terrain_water_fast.vert lines 327-363.
    screen.z = clip.z * rhw + 0.003;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

}
