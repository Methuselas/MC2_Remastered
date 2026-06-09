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
#define PREC highp                // noise.hglsl uses PREC (legacy frag defines it)
#include <include/noise.hglsl>   // fbm() for the colour break-up (matches legacy)

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
                                          //   64 = viz raw matNormalArray rock sample

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

// Live tunables — SAME uniform names + values as the legacy terrain (driver
// uploads them from the gosRenderer members the ImGui terrain panel edits), so
// the sliders (per-material tiling, normal boost, class thresholds, detail
// tiling/strength) drive the chunk path too.
uniform vec4  terrainClassGrass;    // (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi)
uniform vec4  terrainClassDirt;     // (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi)
uniform vec4  matTiling;            // rock, grass, dirt, concrete
uniform vec4  matNormalBoost;       // rock, grass, dirt, concrete
uniform float matTilingSnow;
uniform vec4  detailNormalTiling;   // .x = base tiling multiplier
uniform vec4  detailNormalStrength; // .x = overall detail-normal strength
// Colour mapping (TERRAIN-TINT-UI-1): the colormap is mixed toward per-material
// tints. Same uniforms/values as legacy gos_terrain.frag.
uniform vec3  tintRock;             // default (0.36, 0.37, 0.40)
uniform vec3  tintGrass;            // default (0.35, 0.42, 0.25)
uniform vec3  tintDirt;             // default (0.48, 0.42, 0.33)
uniform float tintStrengthScale;   // 0 = colormap passthrough, 1 = full tint
// Legacy Texcoord is [0,1] per MC2 TILE = MAPCELL_DIM(3) * 128 world units. The
// chunk frag has world coords, so divide by this to get the per-tile UV before
// per-material tiling. (Using /128 = per CELL was ~3x too dense -> sub-pixel
// noise instead of detail.) This is geometry-derived, not a tunable.
const float MAT_WORLD_UNITS_PER_TILE = 384.0;

vec4 chunkColorWeights(vec3 color) {
    vec4 w = vec4(0.0);
    float gMinusR = color.g - color.r;
    w.y = smoothstep(terrainClassGrass.x, terrainClassGrass.y, gMinusR)
        * smoothstep(terrainClassGrass.z, terrainClassGrass.w, color.g);
    float rMinusG = color.r - color.g;
    w.z = smoothstep(terrainClassDirt.x, terrainClassDirt.y, rMinusG)
        * smoothstep(terrainClassDirt.z, terrainClassDirt.w, color.r);
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

// Material weights + snow from the colormap colour (computed once, shared by the
// detail normal AND the colour tint). w = rock/grass/dirt/concrete (sums to 1).
void chunkWeights(vec3 colAvg, out vec4 w, out float snowWeight) {
    w = chunkColorWeights(colAvg);
    vec3 hsv = rgb2hsvChunk(colAvg);
    float snowRaw = smoothstep(0.15, 0.03, hsv.y) * smoothstep(0.42, 0.62, hsv.z);
    snowWeight = smoothstep(0.25, 0.55, snowRaw);
    w *= (1.0 - snowWeight);
    float tot = w.x + w.y + w.z + w.w;
    if (tot > 0.01) w /= tot; else w = vec4(1.0, 0.0, 0.0, 0.0);
}

// Accumulated tangent-space (Z-up) detail normal from precomputed weights.
vec3 chunkDetailNormal(vec4 w, float snowWeight, vec2 worldXY) {
    vec2 uv = worldXY * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE);  // per-tile, GL_REPEAT
    // Screen-space derivative AA (legacy fwRock/fwGrass/...): fade a layer to 0
    // as its tiling goes sub-pixel. WITHOUT this, far/zoomed-out detail collapses
    // to a dark-biased mean normal -> uniform darkening with no visible relief.
    vec2 uvRock  = uv * matTiling.x;
    vec2 uvGrass = uv * matTiling.y;
    vec2 uvDirt  = uv * matTiling.z;
    vec2 uvSnow  = uv * matTilingSnow;
    float fwRock  = clamp(1.0 - (length(fwidth(uvRock))  - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass = clamp(1.0 - (length(fwidth(uvGrass)) - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt  = clamp(1.0 - (length(fwidth(uvDirt))  - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow  = clamp(1.0 - (length(fwidth(uvSnow))  - 0.5) * 2.0, 0.0, 1.0);
    vec3 dN = vec3(0.0);
    if (w.x > 0.01) dN += w.x * matNormalBoost.x * fwRock *
        (texture(matNormalArray, vec3(uvRock,  float(MAT_LAYER_ROCK))).rgb  * 2.0 - 1.0);
    if (w.y > 0.01) dN += w.y * matNormalBoost.y * fwGrass *
        (texture(matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS))).rgb * 2.0 - 1.0);
    if (w.z > 0.01) dN += w.z * matNormalBoost.z * fwDirt *
        (texture(matNormalArray, vec3(uvDirt,  float(MAT_LAYER_DIRT))).rgb  * 2.0 - 1.0);
    if (snowWeight > 0.01) dN += snowWeight * 0.9 * fwSnow *
        (texture(matNormalArray, vec3(uvSnow,  float(MAT_LAYER_SNOW))).rgb * 2.0 - 1.0);
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

    // DIAG bit 64: visualize the raw matNormalArray ROCK-layer sample as color
    // (no lighting). Bluish normal-map texture with visible detail => sampling
    // works (then the fault is the combine/strength). Flat/black/gray uniform =>
    // the array sample itself is broken (layer count, format, or coords).
    if ((u_diag & 64) != 0) {
        vec2 uvDbg = v_worldPos.xy * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE) * matTiling.x;
        fragColor = vec4(texture(matNormalArray, vec3(uvDbg, float(MAT_LAYER_ROCK))).rgb, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Step 1b: geometric normal from world-pos screen derivatives (faceted per
    // triangle; gives relief lighting without sampling the height SSBO in the
    // frag). Terrain world up = +Z (elevation); flip to keep N up-facing.
    // Phase 10.2: SKIRT pixels (u_skirtDepth>0) are vertical seam-fillers — their
    // geometric normal is horizontal, which would shade them as dark walls at LOD
    // edges. Use a flat-up normal so the skirt is lit like the adjacent surface it
    // fills and blends invisibly instead of drawing a dark line.
    // Material weights + snow (shared by detail normal AND colour tint).
    vec4  matWeights; float snowWeight;
    chunkWeights(base, matWeights, snowWeight);

    // --- Surface normal (smooth macro slope + tangent-space detail) ---
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
            vec3 dN   = chunkDetailNormal(matWeights, snowWeight, v_worldPos.xy);
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) + dN.xy * detailNormalStrength.x, 1.0);
            N = normalize(pert);
        }
    }

    // --- Colour mapping: mix the colormap toward per-material tints (legacy
    // TERRAIN-TINT-UI-1). Concrete tile colour blend needs per-vertex TerrainType
    // (deferred) so concreteColorBlend=0 here. ---
    const vec3  kLumaWeights = vec3(0.299, 0.587, 0.114);
    const vec3  tintConcrete = vec3(0.55, 0.53, 0.50);
    const vec3  tintSnow     = vec3(0.75, 0.78, 0.84);
    vec3  materialTint = tintRock * matWeights.x + tintGrass * matWeights.y
                       + tintDirt * matWeights.z + tintConcrete * matWeights.w
                       + tintSnow * snowWeight;
    float colLum      = dot(base, kLumaWeights);
    float tintBase    = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
    float tintStrength= mix(tintBase, 0.85, snowWeight) * tintStrengthScale;
    vec3  baseColor   = mix(base, materialTint, tintStrength);

    // Cliff mapping: desaturate + darken toward rock on steep slopes (uses N.z).
    {
        float cliffBlend = smoothstep(0.85, 0.55, abs(N.z));
        if (cliffBlend > 0.01) {
            float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            vec3  cliffColor = mix(vec3(luma), tintRock, 0.6) * 0.8;
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7);
        }
    }
    // World-space two-octave break-up noise (non-snow).
    {
        float lowFreq  = fbm(v_worldPos.xy * 0.0035, 3) * 0.5 + 0.5;
        float highFreq = fbm(v_worldPos.xy * 0.018,  2) * 0.5 + 0.5;
        float breakup  = mix(0.78, 1.18, mix(lowFreq, highFreq, 0.55));
        baseColor *= mix(1.0, breakup, 1.0 - snowWeight);
    }

    // --- Lighting: NdotL relief band + sun shadow (baked; GBuffer1 stays
    // shadowHandled_flatUp so the compositor does not re-shadow terrain). ---
    float NdotL       = dot(N, terrainLightDir.xyz);
    float diffuse     = clamp(NdotL, 0.02, 1.0);
    float normalLight = ((u_diag & 4) != 0) ? 1.0 : mix(0.35, 1.20, diffuse);

    float shadow = 1.0;
    if ((u_diag & 8) == 0) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynS    = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        shadow = min(staticS, dynS);
    }

    fragColor = vec4(baseColor * normalLight * shadow, 1.0);        // alpha forced 1.0
    if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);     // rc_gbuffer1_shadowHandled_flatUp
}
