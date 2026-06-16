#include "gos_terrain_lod_chunk.h"
#include "utils/gl_utils.h"
#include "utils/shader_builder.h"
#include "gos_postprocess.h"   // Phase 10 Step 1c: shadow textures + light matrices
#include <gameos.hpp>          // Item 1: mc2ShadowCsmEnabled/Count (CSM shader define)
#include <string>
#include "gl_state_guard.h"    // GlStateGuard slice 2: composable depth/blend/cull RAII
#include "../../mclib/render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>
#include <map>

// GlStateGuard slice 2 kill-switch. Default-ON: when set, the terrain chunk
// draw owns its depth/blend/cull/mask/func via RAII guards (mc2gl::GlScoped*).
// MC2_GLSTATEGUARD_TERRAIN=0 reverts to the legacy hand-rolled save/restore
// (kept verbatim, nothing deleted) — the A/B used to prove the guards are
// pixel-neutral. Sampled once at process start.
static bool glStateGuardTerrainEnabled() {
    static const bool on = []() {
        const char* v = std::getenv("MC2_GLSTATEGUARD_TERRAIN");
        return (v == nullptr) || (std::atoi(v) != 0);  // unset/nonzero=ON, 0=OFF
    }();
    return on;
}

// Terrain MVP matrix — exposed by gameos_graphics.cpp for all terrain draw paths.
extern const float* gos_GetTerrainMVPMat4();
// Fix-B frame-of-reference: the GPU water cull and DrawDecalStatic project with
// the PREVIOUS-frame dispatch MVP (baked in Terrain::geometry()) when armed, NOT
// the live current-frame MVP. The legacy terrain draw also uses that baked MVP,
// so terrain depth + water cull + decals all agree at frame N-1. The chunk MUST
// match, or it writes depth at frame N while water/decals project at N-1 -> a
// 1-frame offset -> shore-water dropout + decal tearing under camera motion.
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();
namespace gos_terrain_indirect { bool IsFrameSolidArmed(); }

// ---------------------------------------------------------------------------
// Static SSBO state — all GL objects live here, never in mclib/.
// ---------------------------------------------------------------------------

static GLuint s_heightSsbo = 0;   // GL handle; 0 = not yet allocated
static GLuint s_typeSsbo   = 0;   // Step 5b: per-vertex terrainType SSBO (binding 24)
static GLuint s_cementSsbo = 0;   // Step 5c: per-vertex cement word SSBO (binding 25)
static int    s_mapSide    = 0;   // mapSide stored at last UploadHeightFull
static float  s_halfMap    = 0.0f;// (mapSide * 128.0 * 0.5)

// ---------------------------------------------------------------------------
// Shader program + uniform locations (Phase 4).
// ---------------------------------------------------------------------------

static GLuint   s_terrainProgram    = 0;
static int      s_submitZeroStreak  = 0;  // Phase 7.5: consecutive frames with count==0
static GLint    s_locBlockOriginX   = -1;
static GLint    s_locBlockOriginY   = -1;
static GLint    s_locMapSide        = -1;
static GLint    s_locHalfMap        = -1;
static GLint    s_locMvp            = -1;
static GLint    s_locLodStep        = -1;  // Phase 5: per-block LOD stride uniform
static GLint    s_locSkirtDepth     = -1;  // Phase 6: skirt depth uniform
static GLint    s_locForceColor     = -1;  // Phase 7.5: neon debug palette override
static GLint    s_locColormap       = -1;  // Phase 10: merged colormap atlas sampler
static GLint    s_locAtlasTLX       = -1;  // Phase 10: atlas top-left X (world)
static GLint    s_locAtlasTLY       = -1;  // Phase 10: atlas top-left Y (world)
static GLint    s_locAtlasOOW       = -1;  // Phase 10: atlas oneOverWorldUnitsMapSide
static GLint    s_locLightDir       = -1;  // Phase 10 Step 1b: terrainLightDir (sun)
static GLint    s_locDiag           = -1;  // bisection bitmask (MC2_TERRAIN_LOD_CHUNK_DIAG)
static GLint    s_locPathTint       = -1;  // MC2_SHADER_PATH_TINT debug (u_pathTint)
static GLint    s_locQuadCountX     = -1;  // Phase 10.4: block quad extent X (edge detect)
static GLint    s_locQuadCountY     = -1;  // Phase 10.4: block quad extent Y (edge detect)
static GLint    s_locEdgeStitch     = -1;  // Phase 10.4: packed coarser-neighbour stride
// Phase 10 Step 1c: shadow uniforms (declared by include/shadow.hglsl).
static GLint    s_locShadowMap          = -1;
static GLint    s_locLightSpaceMatrix   = -1;
static GLint    s_locEnableShadows      = -1;
static GLint    s_locShadowSoftness     = -1;
static GLint    s_locDynShadowMap       = -1;
static GLint    s_locDynLightSpaceMat   = -1;
static GLint    s_locEnableDynShadows   = -1;
// Item 1 CSM array-variant locs (only valid when MC2_SHADOW_CSM is ON)
static GLint    s_locDynShadowArray     = -1;
static GLint    s_locDynCascadeMats     = -1;
static GLint    s_locDynCsmCount        = -1;
static GLint    s_locDynCascadeTexel    = -1;  // Stage 3 texel bias
static GLint    s_locCsmDepthSpan       = -1;
// Per-cascade shadow resolution: separate full-map (last) cascade.
static GLint    s_locDynFullMapShadow   = -1;
static GLint    s_locDynFullMapTexel    = -1;
// Mirror gameos_graphics.cpp's file-static terrain shadow texture units (9/10).
static constexpr GLint kChunkTexUnitStaticShadow  = 9;
static constexpr GLint kChunkTexUnitDynamicShadow = 10;
static constexpr GLint kChunkTexUnitDynFullMap    = 13;  // free unit (chunk uses up to 11)
// Phase 10 Step 5a: merged material normal sampler2DArray (own unit, no collision
// with colormap=0 / shadows=9,10). Sourced from gos_GetTerrainNormalArrayTex().
static GLint    s_locMatNormalArray     = -1;
static constexpr GLint kChunkTexUnitMatNormalArray = 5;
extern unsigned int gos_GetTerrainNormalArrayTex();
// Step 5a: live material tunables (same uniforms + source as legacy terrain), so
// the ImGui terrain panel drives the chunk detail too.
static GLint    s_locClassGrass         = -1;
static GLint    s_locClassDirt          = -1;
static GLint    s_locMatTiling          = -1;
static GLint    s_locMatNormalBoost     = -1;
static GLint    s_locMatTilingSnow      = -1;
static GLint    s_locDetailTiling       = -1;
static GLint    s_locDetailStrength     = -1;
static GLint    s_locTintRock           = -1;
static GLint    s_locTintGrass          = -1;
static GLint    s_locTintDirt           = -1;
static GLint    s_locTintStrengthScale  = -1;
extern void  gos_GetTerrainMatTiling(float*, float*, float*, float*, float*);
extern void  gos_GetTerrainTintRock(float*, float*, float*);
extern void  gos_GetTerrainTintGrass(float*, float*, float*);
extern void  gos_GetTerrainTintDirt(float*, float*, float*);
extern float gos_GetTerrainTintStrengthScale();
// Remaining legacy tunables (env gates replicated in the upload so default==legacy).
extern float gos_GetTerrainLightingV1Strength();
extern float gos_GetTerrainLightingV2Floor();
extern float gos_GetTerrainNormalsFromHeightStrength();
extern float gos_GetTerrainPOMScale();
extern int   g_terrainMaterialProfile;   // global; 0 = legacy
static GLint s_locLightingV1     = -1;
static GLint s_locLightingV2     = -1;
static GLint s_locNfhStrength    = -1;
static GLint s_locPomParams      = -1;
static GLint s_locMatProfile     = -1;
// Step 5c: cement catalog atlas (tex3) accessors from gos_terrain_indirect.cpp.
extern unsigned int gos_terrain_indirect_getCementAtlasGLTex();
extern int          gos_terrain_indirect_getCementAtlasGridSide();
extern bool         gos_terrain_indirect_isCementAtlasReady();
static GLint    s_locCementAtlas    = -1;
static GLint    s_locUseCement      = -1;
static GLint    s_locCementGridSide = -1;
static GLint    s_locCementWUPT     = -1;
static constexpr GLint kChunkTexUnitCement = 3;  // matches legacy tex3
// Stage B: transition mask array (GL_TEXTURE_2D_ARRAY, unit 11).
extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
extern bool   gos_terrain_indirect_isTransitionMaskReady();
static GLint s_locTransitionMaskArray = -1;
static GLint s_locUseTransitionMask   = -1;
static constexpr GLint kChunkTexUnitTransitionMask = 11;
extern void  gos_GetTerrainMatNormalBoost(float*, float*, float*, float*);
extern void  gos_GetTerrainClassGrass(float*, float*, float*, float*);
extern void  gos_GetTerrainClassDirt(float*, float*, float*, float*);
extern float gos_GetTerrainDetailTiling();
extern float gos_GetTerrainDetailStrength();

// Phase 10: colormap atlas accessors (defined in gos_terrain_indirect.cpp,
// global free functions). Same atlas tex1 + UV params the legacy gos_terrain.frag
// useAtlasColormap path consumes.
extern GLuint gos_terrain_indirect_getAtlasGLTex();
extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
// Phase 10 Step 1b: terrain sun direction (gameos.hpp), same value the legacy
// terrain frag's terrainLightDir uniform receives.
extern void   gos_GetTerrainLightDir(float* x, float* y, float* z);

// ---------------------------------------------------------------------------
// Patch geometry cache (Phase 4).
// Each unique (qcX, qcY, lodStep) triple gets one VBO+IBO pair.
// VBO contains int16_t[2] (localX, localY) per vertex.
// IBO contains uint16_t triangle indices.
// ---------------------------------------------------------------------------

struct PatchShape {
    GLuint vbo;          // main patch: int16_t[2] (lx, ly) per vertex
    GLuint ibo;          // main patch: uint16_t triangle indices
    int    vertexCount;
    int    indexCount;
    GLuint skirtVbo;     // Phase 6: int16_t[4] (lx, ly, isSkirt, _pad) per skirt vertex
    GLuint skirtIbo;     // Phase 6: uint16_t triangle indices for skirts
    int    skirtVertexCount;
    int    skirtIndexCount;
    // Phase 10.2b: per-edge index ranges into skirtIbo (build order N,S,W,E) so
    // the driver can draw ONLY the edges whose neighbour LOD differs (per-block
    // edge mask) instead of all four. Offsets/counts are in INDEX units.
    int    skirtEdgeOffset[4];   // 0=N, 1=S, 2=W, 3=E
    int    skirtEdgeCount[4];
};

static std::map<uint32_t, PatchShape> s_patchCache;

static uint32_t patchKey(int qcX, int qcY, int lodStep)
{
    return ((uint32_t)qcX & 0xFF)
         | (((uint32_t)qcY    & 0xFF) << 8)
         | (((uint32_t)lodStep & 0xFF) << 16);
}

// Build sample positions along one axis, always including far edge.
static std::vector<int> makeSamplePositions(int quadCount, int lodStep)
{
    std::vector<int> pos;
    for (int p = 0; p <= quadCount; p += lodStep)
        pos.push_back(p);
    if (pos.back() != quadCount)
        pos.push_back(quadCount);
    return pos;
}

static const PatchShape& getOrBuildPatch(int qcX, int qcY, int lodStep)
{
    uint32_t key = patchKey(qcX, qcY, lodStep);
    auto it = s_patchCache.find(key);
    if (it != s_patchCache.end()) return it->second;

    auto xs = makeSamplePositions(qcX, lodStep);
    auto ys = makeSamplePositions(qcY, lodStep);

    struct LocalVertex { int16_t lx, ly; };
    std::vector<LocalVertex> verts;
    verts.reserve(xs.size() * ys.size());
    for (int yy : ys)
        for (int xx : xs)
            verts.push_back({(int16_t)xx, (int16_t)yy});

    // Two CCW triangles per quad cell: TL, BL, BR  /  TL, BR, TR
    std::vector<uint16_t> indices;
    int W = (int)xs.size();
    for (int j = 0; j < (int)ys.size() - 1; ++j) {
        for (int i = 0; i < (int)xs.size() - 1; ++i) {
            uint16_t tl = (uint16_t)(j*W+i);
            uint16_t tr = (uint16_t)(j*W+i+1);
            uint16_t bl = (uint16_t)((j+1)*W+i);
            uint16_t br = (uint16_t)((j+1)*W+i+1);
            indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
        }
    }

    PatchShape ps;
    ps.vertexCount = (int)verts.size();
    ps.indexCount  = (int)indices.size();

    glGenBuffers(1, &ps.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ps.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(LocalVertex)),
                 verts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ps.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ps.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(indices.size() * sizeof(uint16_t)),
                 indices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // -----------------------------------------------------------------------
    // Phase 6: Build skirt geometry — four edge strips, each a quad-strip.
    // SkirtVertex: lx, ly, isSkirt (0=surface, 1=below), _pad.
    // The vertex shader reads isSkirt and applies: h -= isSkirt * u_skirtDepth.
    // Winding is CCW viewed from outside; backface culling is disabled during
    // skirt draws so winding does not need to be perfect in this first pass.
    // -----------------------------------------------------------------------
    struct SkirtVertex { int16_t lx, ly, isSkirt, _pad; };
    std::vector<SkirtVertex> skirtVerts;
    std::vector<uint16_t>    skirtIdx;

    // Each edge strip has 2 * edgeLen vertices.
    // Indices: for each quad in the strip, 2 triangles connecting top[i]/bot[i] to top[i+1]/bot[i+1].
    // Layout within strip (base offset B, 2 verts per column: top=B+2*i, bot=B+2*i+1):
    //   tri1: top[i], bot[i], bot[i+1]  → B+2*i, B+2*i+1, B+2*(i+1)+1
    //   tri2: top[i], bot[i+1], top[i+1] → B+2*i, B+2*(i+1)+1, B+2*(i+1)
    // This is CCW when the strip faces the camera from the outside (correct for all four edges
    // when backface culling is disabled, so no per-edge winding correction needed).

    auto buildEdge = [&](const std::vector<int>& uPos, int fixedCoord, bool fixedIsY)
    {
        int base = (int)skirtVerts.size();
        int n    = (int)uPos.size();
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? (int16_t)uPos[i] : (int16_t)fixedCoord;
            int16_t ly = fixedIsY ? (int16_t)fixedCoord : (int16_t)uPos[i];
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = (uint16_t)(base + 2*i);
            uint16_t b0 = (uint16_t)(base + 2*i + 1);
            uint16_t t1 = (uint16_t)(base + 2*(i+1));
            uint16_t b1 = (uint16_t)(base + 2*(i+1) + 1);
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    // Phase 10.2b: record each edge's index range (build order N,S,W,E).
    ps.skirtEdgeOffset[0] = (int)skirtIdx.size(); buildEdge(xs, (int)ys.front(), false); ps.skirtEdgeCount[0] = (int)skirtIdx.size() - ps.skirtEdgeOffset[0]; // North (y=ys[0])
    ps.skirtEdgeOffset[1] = (int)skirtIdx.size(); buildEdge(xs, (int)ys.back(),  false); ps.skirtEdgeCount[1] = (int)skirtIdx.size() - ps.skirtEdgeOffset[1]; // South (y=ys.back)
    ps.skirtEdgeOffset[2] = (int)skirtIdx.size(); buildEdge(ys, (int)xs.front(), true ); ps.skirtEdgeCount[2] = (int)skirtIdx.size() - ps.skirtEdgeOffset[2]; // West  (x=xs[0])
    ps.skirtEdgeOffset[3] = (int)skirtIdx.size(); buildEdge(ys, (int)xs.back(),  true ); ps.skirtEdgeCount[3] = (int)skirtIdx.size() - ps.skirtEdgeOffset[3]; // East  (x=xs.back)

    ps.skirtVertexCount = (int)skirtVerts.size();
    ps.skirtIndexCount  = (int)skirtIdx.size();

    glGenBuffers(1, &ps.skirtVbo);
    glBindBuffer(GL_ARRAY_BUFFER, ps.skirtVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(skirtVerts.size() * sizeof(SkirtVertex)),
                 skirtVerts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ps.skirtIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ps.skirtIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(skirtIdx.size() * sizeof(uint16_t)),
                 skirtIdx.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    s_patchCache[key] = ps;
    return s_patchCache.at(key);
}

// ---------------------------------------------------------------------------
// VAO for patch draws — reused every frame, attributes re-pointed per batch.
// ---------------------------------------------------------------------------

static GLuint s_patchVao = 0;

// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    if (s_heightSsbo != 0)
        return; // idempotent

    glGenBuffers(1, &s_heightSsbo);
    if (s_heightSsbo == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    glGenBuffers(1, &s_typeSsbo);   // Step 5b: terrainType SSBO (concrete)
    glGenBuffers(1, &s_cementSsbo); // Step 5c: cement word SSBO

    // Shader program (Phase 4) — load unconditionally; SubmitDrawCommands gates
    // on the env var so no pixels change unless MC2_TERRAIN_LOD_CHUNK=1.
    {
        // Item 1: inject MC2_SHADOW_CSM define so terrain_lod_chunk.frag (which
        // #includes shadow.hglsl) compiles the array-sampler variant when ON.
        std::string prefix = "#version 430\n";
        if (mc2ShadowCsmEnabled()) {
            char csmDef[64];
            snprintf(csmDef, sizeof(csmDef),
                     "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                     mc2ShadowCsmCount());
            prefix += csmDef;
        }
        glsl_program* prog = glsl_program::makeProgram(
            "terrain_lod_chunk",
            "shaders/terrain_lod_chunk.vert",
            "shaders/terrain_lod_chunk.frag",
            prefix.c_str());
        if (!prog || !prog->shp_)
        {
            fprintf(stderr, "[TerrainLodChunk] WARNING: shader compile failed"
                            " -- LOD chunk draw disabled\n");
            fflush(stderr);
        }
        else
        {
            s_terrainProgram  = prog->shp_;
            s_locBlockOriginX = glGetUniformLocation(s_terrainProgram, "u_blockOriginX");
            s_locBlockOriginY = glGetUniformLocation(s_terrainProgram, "u_blockOriginY");
            s_locMapSide      = glGetUniformLocation(s_terrainProgram, "u_mapSide");
            s_locHalfMap      = glGetUniformLocation(s_terrainProgram, "u_halfMap");
            s_locMvp          = glGetUniformLocation(s_terrainProgram, "u_worldToClipGL");
            s_locLodStep      = glGetUniformLocation(s_terrainProgram, "u_lodStep");    // Phase 5
            s_locSkirtDepth   = glGetUniformLocation(s_terrainProgram, "u_skirtDepth"); // Phase 6
            s_locForceColor   = glGetUniformLocation(s_terrainProgram, "u_forceColor"); // Phase 7.5
            s_locColormap     = glGetUniformLocation(s_terrainProgram, "u_colormap");   // Phase 10
            s_locAtlasTLX     = glGetUniformLocation(s_terrainProgram, "u_atlasTopLeftX");
            s_locAtlasTLY     = glGetUniformLocation(s_terrainProgram, "u_atlasTopLeftY");
            s_locAtlasOOW     = glGetUniformLocation(s_terrainProgram, "u_atlasOneOverWorldUnits");
            s_locLightDir     = glGetUniformLocation(s_terrainProgram, "terrainLightDir");
            s_locDiag         = glGetUniformLocation(s_terrainProgram, "u_diag");
            s_locPathTint     = glGetUniformLocation(s_terrainProgram, "u_pathTint");
            s_locQuadCountX   = glGetUniformLocation(s_terrainProgram, "u_quadCountX");
            s_locQuadCountY   = glGetUniformLocation(s_terrainProgram, "u_quadCountY");
            s_locEdgeStitch   = glGetUniformLocation(s_terrainProgram, "u_edgeStitch");
            s_locShadowMap        = glGetUniformLocation(s_terrainProgram, "shadowMap");
            s_locLightSpaceMatrix = glGetUniformLocation(s_terrainProgram, "lightSpaceMatrix");
            s_locEnableShadows    = glGetUniformLocation(s_terrainProgram, "enableShadows");
            s_locShadowSoftness   = glGetUniformLocation(s_terrainProgram, "shadowSoftness");
            s_locDynShadowMap     = glGetUniformLocation(s_terrainProgram, "dynamicShadowMap");
            s_locDynLightSpaceMat = glGetUniformLocation(s_terrainProgram, "dynamicLightSpaceMatrix");
            s_locEnableDynShadows = glGetUniformLocation(s_terrainProgram, "enableDynamicShadows");
            s_locDynShadowArray   = glGetUniformLocation(s_terrainProgram, "dynamicShadowArray");
            s_locDynCascadeMats   = glGetUniformLocation(s_terrainProgram, "dynamicCascadeMatrices");
            s_locDynCsmCount      = glGetUniformLocation(s_terrainProgram, "dynamicCsmCount");
            s_locDynCascadeTexel  = glGetUniformLocation(s_terrainProgram, "dynamicCascadeTexelWorld");
            s_locCsmDepthSpan     = glGetUniformLocation(s_terrainProgram, "csmDepthSpan");
            s_locDynFullMapShadow = glGetUniformLocation(s_terrainProgram, "dynamicFullMapShadow");
            s_locDynFullMapTexel  = glGetUniformLocation(s_terrainProgram, "dynamicFullMapTexelWorld");
            s_locMatNormalArray   = glGetUniformLocation(s_terrainProgram, "matNormalArray");
            s_locClassGrass     = glGetUniformLocation(s_terrainProgram, "terrainClassGrass");
            s_locClassDirt      = glGetUniformLocation(s_terrainProgram, "terrainClassDirt");
            s_locMatTiling      = glGetUniformLocation(s_terrainProgram, "matTiling");
            s_locMatNormalBoost = glGetUniformLocation(s_terrainProgram, "matNormalBoost");
            s_locMatTilingSnow  = glGetUniformLocation(s_terrainProgram, "matTilingSnow");
            s_locDetailTiling   = glGetUniformLocation(s_terrainProgram, "detailNormalTiling");
            s_locDetailStrength = glGetUniformLocation(s_terrainProgram, "detailNormalStrength");
            s_locTintRock          = glGetUniformLocation(s_terrainProgram, "tintRock");
            s_locTintGrass         = glGetUniformLocation(s_terrainProgram, "tintGrass");
            s_locTintDirt          = glGetUniformLocation(s_terrainProgram, "tintDirt");
            s_locTintStrengthScale = glGetUniformLocation(s_terrainProgram, "tintStrengthScale");
            s_locLightingV1  = glGetUniformLocation(s_terrainProgram, "terrainLightingV1Strength");
            s_locLightingV2  = glGetUniformLocation(s_terrainProgram, "terrainLightingV2ShadowFillFloor");
            s_locNfhStrength = glGetUniformLocation(s_terrainProgram, "terrainNormalsFromHeightStrength");
            s_locPomParams   = glGetUniformLocation(s_terrainProgram, "pomParams");
            s_locMatProfile  = glGetUniformLocation(s_terrainProgram, "g_terrainMaterialProfile");
            s_locCementAtlas    = glGetUniformLocation(s_terrainProgram, "u_cementAtlas");
            s_locUseCement      = glGetUniformLocation(s_terrainProgram, "u_useCement");
            s_locCementGridSide = glGetUniformLocation(s_terrainProgram, "u_cementGridSide");
            s_locCementWUPT     = glGetUniformLocation(s_terrainProgram, "u_cementWUPT");
            s_locTransitionMaskArray = glGetUniformLocation(s_terrainProgram, "u_transitionMaskArray");
            s_locUseTransitionMask   = glGetUniformLocation(s_terrainProgram, "u_useTransitionMask");
            printf("[TerrainLodChunk] shader loaded prog=%u "
                   "locs: originX=%d originY=%d mapSide=%d halfMap=%d mvp=%d lodStep=%d skirtDepth=%d forceColor=%d\n",
                   (unsigned)s_terrainProgram,
                   s_locBlockOriginX, s_locBlockOriginY,
                   s_locMapSide, s_locHalfMap, s_locMvp, s_locLodStep, s_locSkirtDepth, s_locForceColor);
            fflush(stdout);
            // Phase 7.5: separate startup confirmation line for easy grep.
            printf("[TerrainLOD v1] shader program compiled OK (program=%u)\n", s_terrainProgram);
            fflush(stdout);
        }
    }

    // VAO — one global; attributes are re-pointed each draw in SubmitDrawCommands.
    glGenVertexArrays(1, &s_patchVao);
}

void gos_TerrainLodChunk_Destroy()
{
    // Free patch cache VBOs/IBOs (main + Phase 6 skirt).
    for (auto& kv : s_patchCache) {
        glDeleteBuffers(1, &kv.second.vbo);
        glDeleteBuffers(1, &kv.second.ibo);
        glDeleteBuffers(1, &kv.second.skirtVbo);
        glDeleteBuffers(1, &kv.second.skirtIbo);
    }
    s_patchCache.clear();

    if (s_patchVao != 0) {
        glDeleteVertexArrays(1, &s_patchVao);
        s_patchVao = 0;
    }

    // Shader program is owned by glsl_program cache; delete by name.
    if (s_terrainProgram != 0) {
        glsl_program::deleteProgram("terrain_lod_chunk");
        s_terrainProgram    = 0;
        s_locBlockOriginX   = -1;
        s_locBlockOriginY   = -1;
        s_locMapSide        = -1;
        s_locHalfMap        = -1;
        s_locMvp            = -1;
        s_locLodStep        = -1;
        s_locSkirtDepth     = -1;
        s_locForceColor     = -1;
    }

    if (s_heightSsbo != 0)
    {
        glDeleteBuffers(1, &s_heightSsbo);
        s_heightSsbo = 0;
        s_mapSide    = 0;
        s_halfMap    = 0.0f;
    }
    if (s_typeSsbo != 0)
    {
        glDeleteBuffers(1, &s_typeSsbo);
        s_typeSsbo = 0;
    }
    if (s_cementSsbo != 0)
    {
        glDeleteBuffers(1, &s_cementSsbo);
        s_cementSsbo = 0;
    }
}

// ---------------------------------------------------------------------------
// Submit draw commands — Phase 4 real implementation.
// Called only from Terrain::flushDrawCommands() in mclib/terrain.cpp.
// count==0 is a strict no-op. Restores GL state on exit.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds,
    const float*              skirtDepths,
    const unsigned char*      skirtEdgeMasks,
    const unsigned int*       edgeStitch,
    int                       count)
{
    if (count == 0) return;
    if (s_terrainProgram == 0 || s_heightSsbo == 0) return;
    if (s_patchVao == 0) return;

    // [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
    // Chunk path is the default-on production terrain draw (8z cutover).
    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainBase,
                                    "gos_TerrainLodChunk_SubmitDrawCommands");

    // Match the water-cull / decal frame-of-reference: use the baked dispatch MVP
    // when the solid pass is armed (== what the legacy terrain draw used), else
    // the live MVP. Eliminates the 1-frame offset that caused shore-water dropout
    // + decal tearing under camera motion (greybeard META-FIX).
    const float* mvp = gos_terrain_indirect::IsFrameSolidArmed()
                       ? gos_terrain_indirect_getDispatchMvp16()
                       : gos_GetTerrainMVPMat4();
    if (!mvp) mvp = gos_GetTerrainMVPMat4();
    {
        // Diag 7.5: log every time mvp is null (causes silent bail-out).
        static int s_mvpNullCount = 0;
        if (!mvp) {
            ++s_mvpNullCount;
            if (s_mvpNullCount <= 5 || s_mvpNullCount % 300 == 0)
                printf("[TerrainLOD submit] mvp=NULL bail count=%d (count=%d)\n",
                    s_mvpNullCount, count);
            return;
        }
    }

    // Save state.
    GLint prevProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint prevVAO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // Phase 10.3: chunk terrain is OPAQUE and must EXPLICITLY own depth/blend/
    // cull state. The driver previously inherited GL_DEPTH_TEST / glDepthMask /
    // GL_BLEND / glDepthFunc / GL_CULL_FACE from whatever pass ran before. If a
    // prior transparent/overlay pass left depth WRITES off (glDepthMask FALSE)
    // or blend on, the terrain top renders color but writes NO depth -> never
    // occludes -> "transparent, see-through to the skirts / lower terrain",
    // flipping with draw order and mech-selection (which changes the prior
    // pass). Confirmed independent of frag output (diag7). The opaque reverse-Z
    // state is set explicitly here and restored at the end.
    //
    // GlStateGuard slice 2: when MC2_GLSTATEGUARD_TERRAIN is on (default), RAII
    // guards (mc2gl::GlScoped*) own that save/set/restore — the function-scope
    // optionals capture prev in ctor and restore in dtor at the closing brace
    // (no gos_InvalidateRenderStateCache() here, so function scope is correct).
    // =0 reverts to the legacy hand-rolled path below, byte-for-byte. cull is
    // disabled for the WHOLE draw (terrainMVP bakes the GL-NDC X-flip -> winding
    // is inverted -> the terrain TOP would be culled as a backface; terrain is
    // an opaque heightfield so double-sided is free).
    static const bool s_depthAlways = (getenv("MC2_TERRAIN_LOD_DEPTH_ALWAYS") != nullptr);
    const GLenum s_wantDepthFunc = s_depthAlways ? GL_ALWAYS : GL_GEQUAL;
    const bool useGuards = glStateGuardTerrainEnabled();

    // Guard-path objects (constructed only when useGuards; restore at scope end).
    std::optional<mc2gl::GlScopedCapability> gDepthTest, gBlend, gCull;
    std::optional<mc2gl::GlScopedDepthState> gDepthState;
    // Legacy-path saved values (used only when !useGuards).
    GLboolean prevCullFace  = glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepthTest = GL_FALSE;
    GLboolean prevDepthMask = GL_TRUE;
    GLboolean prevBlend     = GL_FALSE;
    GLint     prevDepthFunc = GL_GEQUAL;

    if (useGuards) {
        gDepthTest.emplace(GL_DEPTH_TEST, /*enable=*/true);
        gDepthState.emplace(GL_TRUE, s_wantDepthFunc);
        gBlend.emplace(GL_BLEND, /*enable=*/false);
        gCull.emplace(GL_CULL_FACE, /*enable=*/false);
    } else {
        prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        prevBlend     = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDepthFunc(s_wantDepthFunc);   // reverse-Z opaque terrain
        glDisable(GL_CULL_FACE);        // double-sided (see comment above)
    }

    glUseProgram(s_terrainProgram);
    glBindVertexArray(s_patchVao);

    // (Depth/blend/cull state is owned above — RAII guards when
    // MC2_GLSTATEGUARD_TERRAIN is on, else the legacy explicit calls. Cull is
    // disabled for the whole draw: terrainMVP bakes the GL-NDC X-flip so winding
    // is inverted and the opaque heightfield is rendered double-sided.)

    // Bind height SSBO (stays bound for all patches this frame).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, s_heightSsbo);
    // Step 5b: terrainType SSBO (concrete). 0 if never uploaded -> vert reads 0.
    if (s_typeSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_TYPE_SSBO_BINDING, s_typeSsbo);
    // Step 5c: cement word SSBO.
    if (s_cementSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, s_cementSsbo);

    // Upload per-frame uniforms (same for every patch).
    if (s_locMapSide >= 0)
        glUniform1i(s_locMapSide, s_mapSide);
    if (s_locHalfMap >= 0)
        glUniform1f(s_locHalfMap, s_halfMap);
    if (s_locMvp >= 0)
        glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);

    // Phase 7.5: neon force-color mode — set once per frame before the patch loop.
    {
        int forceColorMode = getenv("MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR") ? 1 : 0;
        if (s_locForceColor >= 0)
            glUniform1i(s_locForceColor, forceColorMode);
    }

    // Bisection bitmask uniform (MC2_TERRAIN_LOD_CHUNK_DIAG): 1=no GBuffer1,
    // 2=no depth fudge, 4=no lighting. Shader-side A/B without rebuilding.
    if (s_locDiag >= 0) {
        const char* dv = getenv("MC2_TERRAIN_LOD_CHUNK_DIAG");
        glUniform1i(s_locDiag, dv ? atoi(dv) : 0);
    }

    // MC2_SHADER_PATH_TINT: solid GREEN for the chunk terrain path (default 0 = OFF).
    if (s_locPathTint >= 0)
        glUniform1i(s_locPathTint, mc2ShaderPathTint());

    // Phase 10 (Step 1a): bind the merged colormap atlas (tex1) on unit 0 and
    // feed the atlas-UV reconstruction params (same source as the legacy
    // gos_terrain.frag useAtlasColormap path). When the atlas is not yet built
    // (g_atlasGLTex==0) the sampler reads the default texture -> dark; the
    // colormap pipeline normally has it ready by first in-mission frame.
    {
        const GLuint atlasTex = gos_terrain_indirect_getAtlasGLTex();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        if (s_locColormap >= 0) glUniform1i(s_locColormap, 0);
        if (s_locAtlasTLX >= 0) glUniform1f(s_locAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
        if (s_locAtlasTLY >= 0) glUniform1f(s_locAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
        if (s_locAtlasOOW >= 0) glUniform1f(s_locAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        // Step 1b: sun direction for NdotL relief lighting (same as legacy terrain).
        if (s_locLightDir >= 0) {
            float lx = 0.f, ly = 0.f, lz = 1.f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            glUniform4f(s_locLightDir, lx, ly, lz, 0.0f);
        }
    }

    // Phase 10 Step 1c: bind the shadow maps + light matrices that
    // include/shadow.hglsl reads. The chunk draw is a bolt-on — like the GL depth
    // state, it MUST set these or enableShadows defaults to 0 and calcShadow
    // returns 1.0 (no shadows). Mirrors gosRenderer::terrainBindShadowUniforms.
    {
        gosPostProcess* pp = getGosPostProcess();
        if (pp && pp->shadowsEnabled_) {
            if (s_locLightSpaceMatrix >= 0)
                glUniformMatrix4fv(s_locLightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
            if (s_locEnableShadows >= 0)  glUniform1i(s_locEnableShadows, 1);
            if (s_locShadowSoftness >= 0) glUniform1f(s_locShadowSoftness, 2.5f);  // shadow.hglsl default
            if (s_locShadowMap >= 0) {
                glUniform1i(s_locShadowMap, kChunkTexUnitStaticShadow);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitStaticShadow);
                glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
            if (pp->getDynamicShadowFBO()) {
                if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 1);
                if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
                    // Item 1 CSM: array-sampler variant.
                    if (s_locDynCascadeMats >= 0)
                        glUniformMatrix4fv(s_locDynCascadeMats, pp->getDynamicShadowCascadeCount(),
                                           GL_FALSE, pp->getDynamicCascadeMatrices());
                    if (s_locDynCsmCount >= 0) glUniform1i(s_locDynCsmCount, pp->getDynamicShadowCascadeCount());
                    // Stage 3: per-cascade texel-scaled depth bias inputs.
                    if (s_locDynCascadeTexel >= 0)
                        glUniform1fv(s_locDynCascadeTexel, pp->getDynamicShadowCascadeCount(),
                                     pp->getDynamicCascadeTexelWorld());
                    if (s_locCsmDepthSpan >= 0)
                        glUniform1f(s_locCsmDepthSpan, pp->getCsmDepthSpan());
                    if (s_locDynShadowArray >= 0) {
                        glUniform1i(s_locDynShadowArray, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                    // Per-cascade shadow resolution: separate full-map (last) cascade.
                    if (s_locDynFullMapTexel >= 0)
                        glUniform1f(s_locDynFullMapTexel, pp->getDynamicFullMapTexelWorld());
                    if (s_locDynFullMapShadow >= 0) {
                        glUniform1i(s_locDynFullMapShadow, kChunkTexUnitDynFullMap);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynFullMap);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                } else {
                    if (s_locDynLightSpaceMat >= 0)
                        glUniformMatrix4fv(s_locDynLightSpaceMat, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
                    if (s_locDynShadowMap >= 0) {
                        glUniform1i(s_locDynShadowMap, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                }
            } else if (s_locEnableDynShadows >= 0) {
                glUniform1i(s_locEnableDynShadows, 0);
            }
        } else {
            if (s_locEnableShadows >= 0)    glUniform1i(s_locEnableShadows, 0);
            if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 0);
        }
    }

    // Phase 10 Step 5a: bind the merged material normal sampler2DArray (same
    // texture the legacy terrain uses) on its own unit. 0 until all 5 material
    // slots are populated -> the frag samples the default texture (flat-ish
    // normal -> falls back to the smooth base normal, no crash).
    if (s_locMatNormalArray >= 0) {
        GLuint matArrTex = (GLuint)gos_GetTerrainNormalArrayTex();
        glUniform1i(s_locMatNormalArray, kChunkTexUnitMatNormalArray);
        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitMatNormalArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArrTex);
        glActiveTexture(GL_TEXTURE0);
    }

    // Step 5c: cement catalog atlas (tex3 / unit 3) + params — concrete tiles
    // sample this instead of the colormap (legacy gos_terrain.frag:414-426).
    {
        bool cementReady = gos_terrain_indirect_isCementAtlasReady();
        if (s_locUseCement >= 0)
            glUniform1i(s_locUseCement, (cementReady && s_cementSsbo != 0) ? 1 : 0);
        if (cementReady) {
            if (s_locCementGridSide >= 0)
                glUniform1i(s_locCementGridSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (s_locCementWUPT >= 0)
                glUniform1f(s_locCementWUPT, 128.0f);  // Terrain::worldUnitsPerVertex
            if (s_locCementAtlas >= 0) {
                glUniform1i(s_locCementAtlas, kChunkTexUnitCement);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitCement);
                glBindTexture(GL_TEXTURE_2D, (GLuint)gos_terrain_indirect_getCementAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
            }
        }
    }

    // Stage B: transition mask array at unit 11.
    {
        const bool tmReady = gos_terrain_indirect_isTransitionMaskReady();
        if (s_locUseTransitionMask >= 0)
            glUniform1i(s_locUseTransitionMask, tmReady ? 1 : 0);
        if (tmReady && s_locTransitionMaskArray >= 0) {
            glUniform1i(s_locTransitionMaskArray, kChunkTexUnitTransitionMask);
            glActiveTexture(GL_TEXTURE0 + kChunkTexUnitTransitionMask);
            glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
            glActiveTexture(GL_TEXTURE0);
        }
    }

    // Step 5a: upload the live material tunables (driven by the ImGui terrain
    // panel via the same gosRenderer members the legacy terrain reads).
    {
        float mt[5] = {3,2,1,6,1};  gos_GetTerrainMatTiling(&mt[0], &mt[1], &mt[2], &mt[3], &mt[4]);
        float nb[4] = {0.9f,1.1f,1.1f,2.5f}; gos_GetTerrainMatNormalBoost(&nb[0], &nb[1], &nb[2], &nb[3]);
        float cg[4] = {-0.02f,0.06f,0.22f,0.40f}; gos_GetTerrainClassGrass(&cg[0], &cg[1], &cg[2], &cg[3]);
        float cd[4] = {-0.02f,0.06f,0.22f,0.45f}; gos_GetTerrainClassDirt(&cd[0], &cd[1], &cd[2], &cd[3]);
        float dt = gos_GetTerrainDetailTiling();
        float ds = gos_GetTerrainDetailStrength();
        if (s_locMatTiling      >= 0) glUniform4f(s_locMatTiling,      mt[0], mt[1], mt[2], mt[3]);
        if (s_locMatTilingSnow  >= 0) glUniform1f(s_locMatTilingSnow,  mt[4]);
        if (s_locMatNormalBoost >= 0) glUniform4f(s_locMatNormalBoost, nb[0], nb[1], nb[2], nb[3]);
        if (s_locClassGrass     >= 0) glUniform4f(s_locClassGrass,     cg[0], cg[1], cg[2], cg[3]);
        if (s_locClassDirt      >= 0) glUniform4f(s_locClassDirt,      cd[0], cd[1], cd[2], cd[3]);
        if (s_locDetailTiling   >= 0) glUniform4f(s_locDetailTiling,   dt, 0.0f, 0.0f, 0.0f);
        if (s_locDetailStrength >= 0) glUniform4f(s_locDetailStrength, ds, 0.0f, 0.0f, 0.0f);

        float tr[3]={0.36f,0.37f,0.40f}; gos_GetTerrainTintRock(&tr[0],&tr[1],&tr[2]);
        float tg[3]={0.35f,0.42f,0.25f}; gos_GetTerrainTintGrass(&tg[0],&tg[1],&tg[2]);
        float td[3]={0.48f,0.42f,0.33f}; gos_GetTerrainTintDirt(&td[0],&td[1],&td[2]);
        float tss = gos_GetTerrainTintStrengthScale();
        if (s_locTintRock          >= 0) glUniform3f(s_locTintRock,  tr[0], tr[1], tr[2]);
        if (s_locTintGrass         >= 0) glUniform3f(s_locTintGrass, tg[0], tg[1], tg[2]);
        if (s_locTintDirt          >= 0) glUniform3f(s_locTintDirt,  td[0], td[1], td[2]);
        if (s_locTintStrengthScale >= 0) glUniform1f(s_locTintStrengthScale, tss);

        // Remaining tunables. Hemisphere V1/V2 are env-gated OFF by default (match
        // legacy: force-zeroed unless MC2_TERRAIN_LIGHTING_V1/V2 set). NFH strength
        // scales the chunk's always-on smooth normal (default 1.0 = no change). POM
        // = legacy scale (default 0.02). Material profile = global int (0=legacy).
        static const bool s_v1Env = (getenv("MC2_TERRAIN_LIGHTING_V1") != nullptr);
        static const bool s_v2Env = (getenv("MC2_TERRAIN_LIGHTING_V2") != nullptr);
        if (s_locLightingV1  >= 0) glUniform1f(s_locLightingV1,  s_v1Env ? gos_GetTerrainLightingV1Strength() : 0.0f);
        if (s_locLightingV2  >= 0) glUniform1f(s_locLightingV2,  s_v2Env ? gos_GetTerrainLightingV2Floor()    : 1.0f);
        if (s_locNfhStrength >= 0) glUniform1f(s_locNfhStrength, gos_GetTerrainNormalsFromHeightStrength());
        if (s_locPomParams   >= 0) glUniform4f(s_locPomParams,   gos_GetTerrainPOMScale(), 8.0f, 32.0f, 0.0f);
        if (s_locMatProfile  >= 0) glUniform1i(s_locMatProfile,  g_terrainMaterialProfile);
    }

    // Phase 7.5: log first successful submit so the user can confirm the path is live.
    // Also reset the zero-submit streak counter on any non-zero submit.
    {
        static bool s_firstSubmit = true;
        s_submitZeroStreak = 0;  // reset on any non-zero submit
        if (s_firstSubmit && count > 0) {
            printf("[TerrainLOD v1] FIRST SUBMIT: %d draw commands queued\n", count);
            fflush(stdout);
            s_firstSubmit = false;
        }
    }

    // Cardinality probe: log every 600 submits.
    static int s_submitCount = 0;
    ++s_submitCount;
    if (s_submitCount % 600 == 0) {
        printf("[TerrainLOD submit] count=%d cmds=%d\n", s_submitCount, count);
        fflush(stdout);
    }

    for (int i = 0; i < count; ++i)
    {
        const TerrainDrawCommand& cmd = cmds[i];
        int qcX = cmd.quadCountsPacked & 0xFF;
        int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;

        if (qcX <= 0 || qcY <= 0) continue;

        const PatchShape& patch = getOrBuildPatch(qcX, qcY, cmd.lodStep);

        // Per-block uniforms (shared by main patch and skirt).
        if (s_locBlockOriginX >= 0)
            glUniform1i(s_locBlockOriginX, cmd.blockOriginX);
        if (s_locBlockOriginY >= 0)
            glUniform1i(s_locBlockOriginY, cmd.blockOriginY);
        if (s_locLodStep >= 0)
            glUniform1i(s_locLodStep, cmd.lodStep);  // Phase 5: LOD band for debug vis

        // Phase 10.4: edge stitching. Block quad extent (for edge detection) +
        // packed coarser-neighbour stride per edge. Skirt verts (isSkirtFlag!=0)
        // skip the snap in the vert, so this is safe to set once per block.
        // u_quadCount* must be the MAX localOffset the patch actually emits, which
        // makeSamplePositions caps at the last multiple of lodStep <= quad count.
        // (For partial map-edge blocks qcX may not be a multiple of lodStep.)
        const int maxOffX = (cmd.lodStep > 0) ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
        const int maxOffY = (cmd.lodStep > 0) ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
        if (s_locQuadCountX >= 0) glUniform1i(s_locQuadCountX, maxOffX);
        if (s_locQuadCountY >= 0) glUniform1i(s_locQuadCountY, maxOffY);
        if (s_locEdgeStitch >= 0)
            glUniform1i(s_locEdgeStitch, edgeStitch ? (GLint)edgeStitch[i] : 0);

        // --- Draw main patch (skirtDepth=0 so isSkirtFlag pulls height by 0) ---
        if (s_locSkirtDepth >= 0)
            glUniform1f(s_locSkirtDepth, 0.0f);

        // Attrib 0: ivec2 localOffset. Attrib 1 (isSkirt) left disabled -> reads as 0.
        glDisableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, patch.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(2 * sizeof(int16_t)), (const void*)0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.ibo);
        glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_SHORT, 0);

        // --- Phase 6: Draw skirt strips ---
        if (patch.skirtIndexCount > 0 && skirtDepths != nullptr)
        {
            float skirtDepth = skirtDepths[i];
            if (skirtDepth > 0.0f)
            {
                if (s_locSkirtDepth >= 0)
                    glUniform1f(s_locSkirtDepth, skirtDepth);

                // (GL_CULL_FACE already disabled for the whole draw — see top.)
                // Attrib 0: lx, ly (first 2 int16_t of SkirtVertex, stride=8).
                // Attrib 1: isSkirt (third int16_t of SkirtVertex, offset=4).
                glBindBuffer(GL_ARRAY_BUFFER, patch.skirtVbo);
                glEnableVertexAttribArray(0);
                glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribIPointer(1, 1, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)(2 * sizeof(int16_t)));

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.skirtIbo);
                // Phase 10.2b: draw ONLY the edges flagged in the per-block mask
                // (bit 0=N,1=S,2=W,3=E). No mask array -> all four (back-compat).
                const unsigned int mask = skirtEdgeMasks ? skirtEdgeMasks[i] : 0xFu;
                for (int e = 0; e < 4; ++e) {
                    if (((mask >> e) & 1u) == 0u) continue;
                    if (patch.skirtEdgeCount[e] <= 0) continue;
                    glDrawElements(GL_TRIANGLES, patch.skirtEdgeCount[e], GL_UNSIGNED_SHORT,
                                   (const void*)(size_t)(patch.skirtEdgeOffset[e] * sizeof(uint16_t)));
                }

                glDisableVertexAttribArray(1);
            }
        }
    }

    // Restore GL state.
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, 0);
    if (!useGuards) {
        // Phase 10.3: restore inherited cull/depth/blend state (legacy path).
        if (prevCullFace)   glEnable(GL_CULL_FACE);
        glDepthMask(prevDepthMask);
        if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
        if (prevBlend)      glEnable(GL_BLEND);
        glDepthFunc((GLenum)prevDepthFunc);
    }
    // useGuards path: gCull/gBlend/gDepthState/gDepthTest restore depth/blend/
    // cull/mask/func when their optionals destruct at the closing brace below.
    glBindVertexArray((GLuint)prevVAO);
    glUseProgram((GLuint)prevProg);
}

// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    if (s_heightSsbo == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    if (!elevations || mapSide <= 0)
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, elevations, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, s_heightSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_mapSide = mapSide;
    s_halfMap = (float)mapSide * 128.0f * 0.5f;

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float), &firstSample);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (firstSample != elevations[0])
    {
        fprintf(stderr,
            "[TerrainLodChunk] readback mismatch: wrote %.6f, got %.6f\n",
            elevations[0], firstSample);
        fflush(stderr);
    }
#endif
}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (s_typeSsbo == 0 || !types || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_typeSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, types, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_TYPE_SSBO_BINDING, s_typeSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (s_cementSsbo == 0 || !words || count <= 0 || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)count * sizeof(unsigned int);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cementSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, words, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, s_cementSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// Dirty-patch upload — called after setVertexHeight() modifies a block.
// rowData: compact float[(quadCountY+1)*(quadCountX+1)] row-major.
// The full SSBO is row-major with stride mapSide, so this MUST be row-by-row.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide)
{
    if (s_heightSsbo == 0 || !rowData || mapSide <= 0)
        return;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo);

    for (int row = 0; row <= quadCountY; ++row)
    {
        int        dstIdx    = (originY + row) * mapSide + originX;
        GLintptr   dstOffset = (GLintptr)dstIdx * sizeof(float);
        GLsizeiptr bytes     = (GLsizeiptr)(quadCountX + 1) * sizeof(float);
        const float* src     = rowData + row * (quadCountX + 1);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, dstOffset, bytes, src);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
