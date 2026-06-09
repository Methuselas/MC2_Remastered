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
                                          //   32 = no material detail normals (Step 5a)

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

// Phase 10 Step 5a: detail material normals. The legacy terrain blends 5 per-
// material tangent-space normal maps (rock/grass/dirt/concrete/snow) chosen by
// colormap-derived weights -> high-frequency surface relief the flat colormap
// lacks. Port the COLOR-weight + normal-blend core (POM / anti-tiling / per-cell
// TerrainType concrete deferred). Tunables hardcoded to the legacy CPU defaults;
// wire to the ImGui uniforms in a later pass. u_diag&32 disables (A/B).
#include <include/terrain_mat_layers.hglsl>
uniform sampler2DArray matNormalArray;

const vec4  MAT_CLASS_GRASS    = vec4(-0.02, 0.06, 0.22, 0.40);
const vec4  MAT_CLASS_DIRT     = vec4(-0.02, 0.06, 0.22, 0.45);
const vec4  MAT_TILING         = vec4(3.0, 2.0, 1.0, 6.0);   // rock, grass, dirt, concrete
const vec4  MAT_NORMAL_BOOST   = vec4(0.9, 1.1, 1.1, 2.5);
const float MAT_TILING_SNOW    = 1.0;
const float MAT_BASE_TILING    = 1.0;   // detailNormalTiling.x default
const float MAT_DETAIL_STRENGTH= 4.0;   // detailNormalStrength.x default

vec4 chunkColorWeights(vec3 color) {
    vec4 w = vec4(0.0);
    float gMinusR = color.g - color.r;
    w.y = smoothstep(MAT_CLASS_GRASS.x, MAT_CLASS_GRASS.y, gMinusR)
        * smoothstep(MAT_CLASS_GRASS.z, MAT_CLASS_GRASS.w, color.g);
    float rMinusG = color.r - color.g;
    w.z = smoothstep(MAT_CLASS_DIRT.x, MAT_CLASS_DIRT.y, rMinusG)
        * smoothstep(MAT_CLASS_DIRT.z, MAT_CLASS_DIRT.w, color.r);
    w.x = 1.0 - max(w.y, w.z);   // everything else -> rock
    float isWater = smoothstep(0.0, 0.08, min(color.g, color.b) - color.r);
    w.x += isWater; w.y *= (1.0 - isWater); w.z *= (1.0 - isWater);
    float total = w.x + w.y + w.z + w.w;
    return (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
}

vec3 rgb2hsvChunk(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1.0e-10)), d / (q.x + 1.0e-10), q.x);
}

// Accumulated tangent-space (Z-up) detail normal from the colormap color.
vec3 chunkDetailNormal(vec3 colAvg, vec2 worldXY) {
    vec4 w = chunkColorWeights(colAvg);
    vec3 hsv = rgb2hsvChunk(colAvg);
    float snowRaw    = smoothstep(0.15, 0.03, hsv.y) * smoothstep(0.42, 0.62, hsv.z);
    float snowWeight = smoothstep(0.25, 0.55, snowRaw);
    w *= (1.0 - snowWeight);
    float tot = w.x + w.y + w.z + w.w;
    if (tot > 0.01) w /= tot; else w = vec4(1.0, 0.0, 0.0, 0.0);

    vec2 uv = worldXY * (MAT_BASE_TILING / 128.0);   // per-cell tiling, GL_REPEAT
    vec3 dN = vec3(0.0);
    if (w.x > 0.01) dN += w.x * MAT_NORMAL_BOOST.x *
        (texture(matNormalArray, vec3(uv * MAT_TILING.x, float(MAT_LAYER_ROCK))).rgb  * 2.0 - 1.0);
    if (w.y > 0.01) dN += w.y * MAT_NORMAL_BOOST.y *
        (texture(matNormalArray, vec3(uv * MAT_TILING.y, float(MAT_LAYER_GRASS))).rgb * 2.0 - 1.0);
    if (w.z > 0.01) dN += w.z * MAT_NORMAL_BOOST.z *
        (texture(matNormalArray, vec3(uv * MAT_TILING.z, float(MAT_LAYER_DIRT))).rgb  * 2.0 - 1.0);
    if (snowWeight > 0.01) dN += snowWeight * 0.9 *
        (texture(matNormalArray, vec3(uv * MAT_TILING_SNOW, float(MAT_LAYER_SNOW))).rgb * 2.0 - 1.0);
    return dN;
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
    } else {
        vec3 baseN;
        if ((u_diag & 16) != 0) {
            baseN = normalize(cross(dFdx(v_worldPos), dFdy(v_worldPos)));  // flat A/B
            if (baseN.z < 0.0) baseN = -baseN;
        } else {
            baseN = smoothTerrainNormal(v_worldPos.xy);  // smooth macro slope
        }
        if ((u_diag & 32) != 0) {
            N = baseN;  // detail disabled (A/B)
        } else {
            // Perturb the macro slope with tangent-space detail in a Z-up param
            // (matches legacy: N.xy = detailN.xy*strength + slope.xy/slope.z).
            vec3 dN   = chunkDetailNormal(base, v_worldPos.xy);
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) + dN.xy * MAT_DETAIL_STRENGTH, 1.0);
            N = normalize(pert);
        }
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
