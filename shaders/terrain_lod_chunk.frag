// Phase 5: LOD-band debug visualization.
// u_lodStep ∈ {1,2,4,5,10,20} corresponding to LOD levels 0-5.
// Fine (green) -> coarse (dark red). Elevation modulates brightness.
// Phase 6: skirts are darkened (50%) for debug visibility when u_skirtDepth > 0.
// Phase 7.5: u_forceColor=1 enables neon palette — unmistakable proof chunk renderer is active.

// Phase 10 Step 1c: shadows. shadow.hglsl declares shadowMap/lightSpaceMatrix/
// enableShadows/shadowSoftness + the dynamic equivalents and provides calcShadow/
// calcDynamicShadow (Poisson PCF). The chunk DRIVER must bind those uniforms (same
// as the legacy terrain draw) or enableShadows reads 0 -> calcShadow returns 1.0.
#include <include/shadow.hglsl>

in vec3 v_worldPos;
uniform int   u_lodStep;
uniform float u_skirtDepth;  // Phase 6: >0 when drawing a skirt strip
uniform int   u_forceColor;  // Phase 7.5: 1 = neon debug palette; 0 = colormap

// Phase 10 (Step 1a): production base color from the merged colormap atlas
// (tex1 in legacy gos_terrain.frag). Same atlas-UV reconstruction as the
// legacy useAtlasColormap path: atlas-absolute UV from world position.
uniform sampler2D u_colormap;
uniform float u_atlasTopLeftX;            // = Terrain::mapTopLeft3d.x
uniform float u_atlasTopLeftY;            // = Terrain::mapTopLeft3d.y
uniform float u_atlasOneOverWorldUnits;   // = Terrain::oneOverWorldUnitsMapSide
uniform vec4  terrainLightDir;            // Phase 10 Step 1b: sun dir (same uniform as legacy)
uniform int   u_diag;                     // Bisection bitmask (MC2_TERRAIN_LOD_CHUNK_DIAG):
                                          //   1  = do NOT write GBuffer1
                                          //   2  = no depth fudge (raw gl_FragCoord.z)
                                          //   4  = no lighting (colormap only)
                                          //   8  = no shadows (skip calcShadow)
                                          //   16 = flat per-triangle normal (old dFdx)

// Step 1c-fix: SMOOTH per-pixel normal from the heightfield. The frag reads the
// SAME height SSBO the vert uses (binding 23) and takes a bilinear central
// difference, so even coarse LOD triangles shade smoothly instead of as flat
// faceted wedges (the "atrocious cliff" artifact). u_mapSide/u_halfMap are the
// SAME linked-program uniforms the vert already receives from the driver.
uniform int   u_mapSide;
uniform float u_halfMap;
layout(binding = 23, std430) readonly buffer TerrainHeightBufFrag {
    float heightsF[];
};

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 GBuffer1;   // shadow-handled flat-up (terrain MRT composite)

// Phase 10.3: REVERSE-Z depth fudge — the scene runs reverse-Z (glDepthFunc
// GL_GEQUAL) and legacy gos_terrain.frag writes gl_FragDepth = depth +
// TERRAIN_DEPTH_FUDGE so ground-level objects/overlays/selection markers win the
// GEQUAL tie instead of z-fighting terrain. The chunk frag was writing raw
// gl_FragCoord.z -> z-fight (terrain "disappears" depending on selection/overlay
// draw order, worst at distance). Match the legacy convention. Keep LOCKSTEP with
// shaders/include/terrain_depth_bias.hglsl (TERRAIN_DEPTH_FUDGE = -0.002).
const float TERRAIN_DEPTH_FUDGE = -0.002;

float heightAtCell(int cx, int cy) {
    cx = clamp(cx, 0, u_mapSide - 1);
    cy = clamp(cy, 0, u_mapSide - 1);
    return heightsF[cx + cy * u_mapSide];
}

// Bilinear height at a continuous (col,row) — matches the legacy LINEAR-filtered
// terrainHeightTex so the central difference is continuous (no per-cell terrace).
float heightBilinear(float fx, float fy) {
    int   x0 = int(floor(fx)), y0 = int(floor(fy));
    float tx = fx - float(x0), ty = fy - float(y0);
    float h00 = heightAtCell(x0,     y0);
    float h10 = heightAtCell(x0 + 1, y0);
    float h01 = heightAtCell(x0,     y0 + 1);
    float h11 = heightAtCell(x0 + 1, y0 + 1);
    return mix(mix(h00, h10, tx), mix(h01, h11, tx), ty);
}

// Smooth world-space normal from the heightfield (central difference). World
// convention matches the vert: worldX = col*128 - halfMap, worldY = halfMap -
// row*128 (row increases as Y decreases -> dh/dy = -dh/drow). Returns +Z up.
vec3 smoothTerrainNormal(vec2 worldXY) {
    float colF = (worldXY.x + u_halfMap) / 128.0;
    float rowF = (u_halfMap - worldXY.y) / 128.0;
    float span = 2.0 * 128.0;
    float dhdx   = (heightBilinear(colF + 1.0, rowF) - heightBilinear(colF - 1.0, rowF)) / span;
    float dhdrow = (heightBilinear(colF, rowF + 1.0) - heightBilinear(colF, rowF - 1.0)) / span;
    float dhdy   = -dhdrow;
    return normalize(vec3(-dhdx, -dhdy, 1.0));
}

void main() {
    // Reverse-Z terrain depth fudge (bit 2 disables it: raw gl_FragCoord.z).
    float fudge = ((u_diag & 2) != 0) ? 0.0 : TERRAIN_DEPTH_FUDGE;
    gl_FragDepth = clamp(gl_FragCoord.z + fudge, 0.0, 1.0);

    // Phase 7.5 debug: neon LOD-band palette when u_forceColor=1 (launch_lod_*color.bat).
    if (u_forceColor != 0) {
        vec3 fc;
        if      (u_lodStep == 1)  fc = vec3(0.0,  1.0,  0.0);   // LOD0 neon green
        else if (u_lodStep == 2)  fc = vec3(1.0,  1.0,  0.0);   // LOD1 yellow
        else if (u_lodStep == 4)  fc = vec3(1.0,  0.0,  1.0);   // LOD2 magenta
        else if (u_lodStep == 5)  fc = vec3(0.0,  1.0,  1.0);   // LOD3 cyan
        else if (u_lodStep == 10) fc = vec3(1.0,  0.0,  0.0);   // LOD4 red
        else                      fc = vec3(1.0,  1.0,  1.0);   // LOD5 white
        if (u_skirtDepth > 0.0)   fc = vec3(0.0,  0.0,  0.5);   // skirts dark blue
        fragColor = vec4(fc, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Production: sample the colormap atlas. Atlas UV in [0,1] across the map.
    // Skirt verts share the edge surface vertex's worldPos.xy, so they sample
    // the same color as the adjacent surface -> seamless production skirts (no
    // debug darken). (Step 1b will add normals-from-height lighting + GBuffer1.)
    vec2 uv;
    uv.x = (v_worldPos.x - u_atlasTopLeftX) * u_atlasOneOverWorldUnits;
    uv.y = (u_atlasTopLeftY - v_worldPos.y) * u_atlasOneOverWorldUnits;
    vec3 base = texture(u_colormap, uv).rgb;

    // Step 1b: geometric normal from world-pos screen derivatives (faceted per
    // triangle; gives relief lighting without sampling the height SSBO in the
    // frag). Terrain world up = +Z (elevation); flip to keep N up-facing.
    // Phase 10.2: SKIRT pixels (u_skirtDepth>0) are vertical seam-fillers — their
    // geometric normal is horizontal, which would shade them as dark walls at LOD
    // edges. Use a flat-up normal so the skirt is lit like the adjacent surface it
    // fills and blends invisibly instead of drawing a dark line.
    vec3 N;
    if (u_skirtDepth > 0.0) {
        N = vec3(0.0, 0.0, 1.0);
    } else if ((u_diag & 16) != 0) {
        // A/B fallback: old flat per-triangle geometric normal.
        N = normalize(cross(dFdx(v_worldPos), dFdy(v_worldPos)));
        if (N.z < 0.0) N = -N;
    } else {
        // Smooth heightfield normal -> no faceted cliffs at coarse LOD.
        N = smoothTerrainNormal(v_worldPos.xy);
    }
    float NdotL       = dot(N, terrainLightDir.xyz);
    float diffuse     = clamp(NdotL, 0.02, 1.0);
    float normalLight = ((u_diag & 4) != 0) ? 1.0 : mix(0.35, 1.20, diffuse); // legacy band

    // Step 1c: bake the sun shadow into the colour (GBuffer1 stays
    // shadowHandled_flatUp -> the compositor does NOT re-shadow terrain). Use a
    // flat up-normal for the shadow test (matches legacy gos_terrain.frag: detail-
    // normal bias flips cause sprinkle/inverted shadows). min(static,dynamic) so
    // overlapping casters don't double-darken. Skirts (u_skirtDepth>0) are flat
    // seam-fillers — shadow them like the surface they fill. u_diag&8 disables.
    float shadow = 1.0;
    if ((u_diag & 8) == 0) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynS    = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        shadow = min(staticS, dynS);
    }

    fragColor = vec4(base * normalLight * shadow, 1.0);              // alpha forced 1.0
    if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);      // rc_gbuffer1_shadowHandled_flatUp
}
