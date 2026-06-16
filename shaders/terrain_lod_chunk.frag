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

in vec3  v_worldPos;
in float v_terrainType;       // Step 5b: interpolated per-vertex terrainType (concrete)
uniform int   u_lodStep;
uniform int   u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

// Step 5c: cement catalog atlas (legacy tex3). Concrete tiles sample this instead
// of the colormap. Same UV math as gos_terrain.frag.
uniform sampler2D u_cementAtlas;
uniform int       u_useCement;
uniform int       u_cementGridSide;
uniform float     u_cementWUPT;   // world units per cement tile (= 128)
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
                                          //  128 = viz v_terrainType (grey + red=concrete)

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
// Step 5c: per-tile cement words, read by WORLD TILE in the frag (LOD-independent
// -> continuous runways at any LOD; coarse geometry is fine since cement is flat).
layout(binding = 25, std430) readonly buffer TerrainCementBufFrag {
    uint cementWordsF[];
};

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 GBuffer1;   // shadow-handled flat-up (terrain MRT composite)

// Reverse-Z terrain depth bias (net -0.004, matching the legacy thin path) is now
// applied PRE-DIVIDE in terrain_lod_chunk.vert (clip.z += 2*FUDGE*clip.w), NOT via
// gl_FragDepth here. Writing gl_FragDepth disabled early-Z/Hi-Z on AMD and caused
// decal tearing at the cement boundary under camera motion. See the vert.

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

// Remaining legacy tunables (copied with legacy defaults; driver replicates the
// env gates so default == legacy default). cellBombParams is a DEAD uniform in
// legacy (no shader consumer) -> not wired.
uniform float terrainLightingV1Strength;       // hemisphere ambient; 0 = off (env-gated default off)
uniform float terrainLightingV2ShadowFillFloor;// shadow-aware fill floor; 1 = no influence
uniform float terrainNormalsFromHeightStrength;// macro-slope strength scalar (default 1.0)
uniform vec4  pomParams;                        // .x=scale(0=off), .y=minLayers, .z=maxLayers
uniform int   g_terrainMaterialProfile;         // 0=legacy, 1=sand(mc2_24 dirt-gate widen)
// Legacy Texcoord is [0,1] per MC2 TILE = MAPCELL_DIM(3) * 128 world units. The
// chunk frag has world coords, so divide by this to get the per-tile UV before
// per-material tiling. (Using /128 = per CELL was ~3x too dense -> sub-pixel
// noise instead of detail.) This is geometry-derived, not a tunable.
const float MAT_WORLD_UNITS_PER_TILE = 768.0;  // 2 tiles/repeat (~half the prior density)

vec4 chunkColorWeights(vec3 color) {
    vec4 w = vec4(0.0);
    float gMinusR = color.g - color.r;
    w.y = smoothstep(terrainClassGrass.x, terrainClassGrass.y, gMinusR)
        * smoothstep(terrainClassGrass.z, terrainClassGrass.w, color.g);
    float rMinusG = color.r - color.g;
    // Material profile 1 (sand, mc2_24) widens the dirt gate (legacy lowers the
    // dirt saturation thresholds). Approximate by lowering the dirt brightness
    // floor so more of the sandy colormap classifies as dirt. Profile 0 = legacy.
    float dirtLo = (g_terrainMaterialProfile == 1) ? terrainClassDirt.z * 0.6 : terrainClassDirt.z;
    float dirtHi = (g_terrainMaterialProfile == 1) ? terrainClassDirt.w * 0.8 : terrainClassDirt.w;
    w.z = smoothstep(terrainClassDirt.x, terrainClassDirt.y, rMinusG)
        * smoothstep(dirtLo, dirtHi, color.r);
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

// --- Anti-tiling (legacy sampleAntiTileArr) + POM (parallaxMapping) helpers ---
// Both are near-camera detail effects; gated on the per-layer fwidth (sub-pixel
// at distance -> skip, bounding cost on a default-on renderer). matTiling drives
// the anti-tile cell scale; pomParams.x (0=off) drives POM.
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec4 sampleAntiTileArr(int layer, vec2 uv, float scale) {
    vec2 off1 = hash22(floor(uv / scale)) * scale;
    vec2 off2 = hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s0 = texture(matNormalArray, vec3(uv,        float(layer)));
    vec4 s1 = texture(matNormalArray, vec3(uv + off1, float(layer)));
    vec4 s2 = texture(matNormalArray, vec3(uv + off2, float(layer)));
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    return (s0 + s1 * w1 + s2 * w2) / (1.0 + w1 + w2);
}
float chunkSampleDisplacement(vec2 uv, vec4 w) {
    float d = 0.0;
    if (w.x > 0.01) d += w.x * texture(matNormalArray, vec3(uv, float(MAT_LAYER_ROCK))).a;
    if (w.y > 0.01) d += w.y * texture(matNormalArray, vec3(uv, float(MAT_LAYER_GRASS))).a;
    if (w.w > 0.01) d += w.w * texture(matNormalArray, vec3(uv, float(MAT_LAYER_CONCRETE))).a;
    return 1.0 - d;
}
// Parallax occlusion (fixed faux view dir, like legacy). Bounded 16 layers.
vec2 chunkParallax(vec2 uv, float scale, vec4 w) {
    const vec3 viewDirTS = vec3(0.15, 0.85, 0.15);
    float numLayers = mix(pomParams.y, pomParams.z, max(viewDirTS.y, 0.0));
    numLayers = clamp(numLayers, 4.0, 16.0);
    float layerDepth = 1.0 / numLayers;
    float curLayer = 0.0;
    vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    vec2 dUV = P / numLayers;
    vec2 curUV = uv;
    float curD = chunkSampleDisplacement(curUV, w);
    for (int i = 0; i < 16; ++i) {
        if (curLayer >= curD) break;
        curUV -= dUV;
        curD = chunkSampleDisplacement(curUV, w);
        curLayer += layerDepth;
    }
    vec2 prevUV = curUV + dUV;
    float after  = curD - curLayer;
    float before = chunkSampleDisplacement(prevUV, w) - curLayer + layerDepth;
    return mix(curUV, prevUV, after / (after - before));
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
    // Rock tiling /3 (and normal strength /3 below) — rock detail was too dense/strong.
    vec2 uvRock     = uv * (matTiling.x / 3.0);
    vec2 uvGrass    = uv * matTiling.y;
    vec2 uvDirt     = uv * matTiling.z;
    vec2 uvConcrete = uv * matTiling.w;
    vec2 uvSnow     = uv * matTilingSnow;
    float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow     = clamp(1.0 - (length(fwidth(uvSnow))     - 0.5) * 2.0, 0.0, 1.0);

    // POM: parallax-offset the material UVs (legacy pomParams.x>0). fwidth-gated
    // (near only) -> bounded cost on a default-on renderer. Off when scale<=0.
    if (pomParams.x > 0.0 && fwRock > 0.4) {
        const vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);
        float pomScale = pomParams.x * dot(pomScaleMat, w) * fwRock;
        vec2  pomUV    = uv * dot(matTiling, w);
        vec2  pomOff   = (chunkParallax(pomUV, pomScale, w) - pomUV) * fwRock;
        uvRock += pomOff; uvGrass += pomOff; uvDirt += pomOff; uvConcrete += pomOff;
    }

    // Anti-tile scale per material (legacy: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((matTiling.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((matTiling.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((matTiling.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((matTiling.w - 1.0) / 3.0, 0.0, 1.0));
    bool  antiTile = true;  // gated per-layer by fw* (near) + ats below

    vec3 dN = vec3(0.0);
    if (w.x > 0.01) {
        vec4 s = (antiTile && atsRock > 0.01 && fwRock > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock)
               : texture(matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)));
        dN += w.x * (matNormalBoost.x / 3.0) * fwRock * (s.rgb * 2.0 - 1.0);
    }
    if (w.y > 0.01) {
        vec4 s = (antiTile && atsGrass > 0.01 && fwGrass > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass)
               : texture(matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)));
        dN += w.y * matNormalBoost.y * fwGrass * (s.rgb * 2.0 - 1.0);
    }
    if (w.z > 0.01) {
        vec4 s = (antiTile && atsDirt > 0.01 && fwDirt > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt)
               : texture(matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)));
        dN += w.z * matNormalBoost.z * fwDirt * (s.rgb * 2.0 - 1.0);
    }
    if (w.w > 0.01) {
        vec4 s = (antiTile && atsConc > 0.01 && fwConcrete > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc)
               : texture(matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)));
        dN += w.w * matNormalBoost.w * fwConcrete * (s.rgb * 2.0 - 1.0);
    }
    if (snowWeight > 0.01) dN += snowWeight * 0.9 * fwSnow *
        (texture(matNormalArray, vec3(uvSnow,  float(MAT_LAYER_SNOW))).rgb * 2.0 - 1.0);
    return dN;
}

void main() {
    // Depth: NO gl_FragDepth write. The -0.004 net terrain bias is applied
    // PRE-DIVIDE in the vert (clip.z += 2*FUDGE*clip.w). Writing gl_FragDepth here
    // disabled early-Z/Hi-Z on AMD -> decal tearing at the cement boundary under
    // camera motion (greybeard META-FIX; vulkan_aligned_depth_bias_ruling.md).

    // MC2_SHADER_PATH_TINT: solid GREEN so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        fragColor = vec4(0.0, 1.0, 0.0, 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);   // shadowHandled_flatUp
        return;
    }

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
    // 9-tap disc blur for a soft splatted colormap (legacy colAvg style). CMAP_BLUR
    // is an atlas-UV radius — one-line tunable.
    const float CMAP_BLUR = 0.0020;
    const float CMAP_R2   = CMAP_BLUR * 0.707;
    vec3 base = texture(u_colormap, uv).rgb
              + texture(u_colormap, uv + vec2( CMAP_BLUR, 0.0)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_BLUR, 0.0)).rgb
              + texture(u_colormap, uv + vec2(0.0,  CMAP_BLUR)).rgb
              + texture(u_colormap, uv + vec2(0.0, -CMAP_BLUR)).rgb
              + texture(u_colormap, uv + vec2( CMAP_R2,  CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_R2,  CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2( CMAP_R2, -CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_R2, -CMAP_R2)).rgb;
    base /= 9.0;

    // Step 5c: cement catalog override. Pure-cement tiles (validity bit set)
    // sample the cement atlas instead of the colormap (legacy gos_terrain.frag).
    bool cementHit = false;
    int  ctX = clamp(int(floor((v_worldPos.x + u_halfMap) / 128.0)), 0, u_mapSide - 1);
    int  ctY = clamp(int(floor((u_halfMap - v_worldPos.y) / 128.0)), 0, u_mapSide - 1);
    uint cw  = cementWordsF[ctX + ctY * u_mapSide];
    if (u_useCement != 0 && (cw & 0x80000000u) != 0u) {
        // CEMENT-DIFFUSE-COLOR: sample the real cement DIFFUSE atlas (u_cementAtlas).
        // Was sampling matNormalArray[MAT_LAYER_PAINTED_CONC] (a NORMAL map) as
        // colour -> flat dark bluish slab. Port the atlas-UV reconstruction from
        // gos_terrain.frag: atlas index from the cement word (cw & 0xFFFFu),
        // grid cell + fract world tile UV via u_cementWUPT (world units per tile).
        uint cLayerIdx = cw & 0xFFFFu;
        int  cGridSide = u_cementGridSide;
        if (cGridSide < 1) cGridSide = 1;
        int  cCol = int(cLayerIdx) % cGridSide;
        int  cRow = int(cLayerIdx) / cGridSide;
        vec2 cTileUV  = fract(vec2(v_worldPos.x, -v_worldPos.y) / u_cementWUPT);
        vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(cGridSide);
        base = texture(u_cementAtlas, cAtlasUV).rgb;
        cementHit = true;
    }

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
    // DIAG bit 128: visualize v_terrainType (material index 0-3) as grayscale +
    // RED where it would be treated as concrete (smoothstep(2,3)). Mostly-black
    // with red only on cement = correct. Uniform grey / all-red = SSBO garbage.
    if ((u_diag & 128) != 0) {
        float g = clamp(v_terrainType / 3.0, 0.0, 1.0);
        float pc = smoothstep(2.0, 3.0, v_terrainType);
        fragColor = vec4(g + pc, g * (1.0 - pc), g * (1.0 - pc), 1.0);
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
    // DIAG bit 256: raw colormap sample only (no tint/detail/lighting). Varied
    // colours => colormap fine, muting is in the composition. Uniform/flat =>
    // the colormap sample itself is broken (wrong atlas/UV/over-blur).
    if ((u_diag & 256) != 0) {
        fragColor = vec4(base, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Material weights + snow (shared by detail normal AND colour tint).
    vec4  matWeights; float snowWeight;
    chunkWeights(base, matWeights, snowWeight);

    // Step 5b: concrete/cement. TerrainType ~3 at cement vertices (interpolated,
    // so boundary patches blend). pureConcrete pushes weights fully to concrete
    // (.w), suppresses snow, and (below) restores the authored colormap tone +
    // flattens lighting. Matches legacy gos_terrain.frag:539-548,738,772.
    float pureConcrete       = smoothstep(2.0, 3.0, v_terrainType);
    if (cementHit) pureConcrete = 1.0;  // cement atlas hit -> full concrete (base already set)
    float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));
    matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);
    snowWeight *= (1.0 - pureConcrete);

    // --- Surface normal (smooth macro slope + tangent-space detail) ---
    vec3  N;
    float macroNz = 1.0;   // MACRO (un-perturbed) slope Z — used for the cliff test
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
        macroNz = baseN.z;
        if ((u_diag & 32) != 0) {
            N = baseN;  // detail disabled (A/B)
        } else {
            vec3 dN   = chunkDetailNormal(matWeights, snowWeight, v_worldPos.xy)
                      * (1.0 - pureConcrete);  // suppress regular detail on cement
            // Cement normal: apply painted-concrete normal (MAT_LAYER_PAINTED_CONC) in the
            // cement-masked region so runways get real surface relief instead of flat shading.
            // Uses the same concrete UV tiling as chunkDetailNormal() (legacy parity).
            if (pureConcrete > 0.001) {
                vec2 uvC   = v_worldPos.xy * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE) * matTiling.w;
                float fwC  = clamp(1.0 - (length(fwidth(uvC)) - 0.5) * 2.0, 0.0, 1.0);
                vec4 cN    = texture(matNormalArray, vec3(uvC, float(MAT_LAYER_PAINTED_CONC)));
                dN        += pureConcrete * matNormalBoost.w * fwC * (cN.rgb * 2.0 - 1.0);
            }
            // terrainNormalsFromHeightStrength scales the macro-slope tilt (default 1.0).
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) * terrainNormalsFromHeightStrength
                             + dN.xy * detailNormalStrength.x, 1.0);
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
    if ((u_diag & 512) != 0) baseColor = base;   // DIAG 512: bypass material tint (A/B)
    // Cement: restore the authored colormap tone (runway/apron) instead of the
    // generic concrete tint.
    baseColor = mix(baseColor, base, concreteColorBlend);

    // Cliff mapping: desaturate + darken toward rock on steep slopes. MUST use the
    // MACRO slope (un-perturbed), NOT the detail-perturbed N — detail bumps drop
    // N.z to ~0.65 even on flat ground, which fired the cliff blend everywhere and
    // desaturated the whole map to grey (matches legacy using WorldNorm.z).
    {
        float cliffBlend = smoothstep(0.85, 0.55, abs(macroNz));
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
    normalLight = mix(normalLight, 1.0, pureConcrete * 0.85);  // cement: flatter lit

    float shadow = 1.0;
    if ((u_diag & 8) == 0) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynS    = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        shadow = min(staticS, dynS);
    }

    // DEBUG-VIZ (exact-value escape on u_diag; bitmask modes never reach 30/31):
    //   30 = dynamic-cast shadow only (isolates building dynamic shadow), 31 = min(static,dyn).
    if (u_diag == 30) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float dynVizS = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        fragColor = vec4(vec3(dynVizS), 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }
    if (u_diag == 31) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticVizS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynVizS2   = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        fragColor = vec4(vec3(min(staticVizS, dynVizS2)), 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    vec3 lit = baseColor * normalLight * shadow;
    // Hemisphere ambient fill (TERRAIN-LIGHTING-1/2) — added AFTER shadow so sky/
    // ground bounce still lights shadowed terrain. Env-gated OFF by default
    // (terrainLightingV1Strength=0 -> skipped, byte-safe). Matches gos_terrain.frag.
    if (terrainLightingV1Strength > 0.0) {
        const vec3 hemiSkyTint    = vec3(0.55, 0.62, 0.75);
        const vec3 hemiGroundTint = vec3(0.32, 0.28, 0.22);
        float skyFactor    = N.z * 0.5 + 0.5;
        vec3  hemiFill     = mix(hemiGroundTint, hemiSkyTint, skyFactor);
        float hemiAmount   = terrainLightingV1Strength * (1.0 - 0.5 * snowWeight);
        float hemiShadowMix = mix(terrainLightingV2ShadowFillFloor, 1.0, shadow);
        lit += hemiFill * hemiAmount * 0.25 * hemiShadowMix;
    }

    fragColor = vec4(lit, 1.0);                                     // alpha forced 1.0
    if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);     // rc_gbuffer1_shadowHandled_flatUp
}
