#include "gos_terrain_lod_chunk.h"
#include "utils/gl_utils.h"
#include "utils/shader_builder.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>

// Terrain MVP matrix — exposed by gameos_graphics.cpp for all terrain draw paths.
extern const float* gos_GetTerrainMVPMat4();

// ---------------------------------------------------------------------------
// Static SSBO state — all GL objects live here, never in mclib/.
// ---------------------------------------------------------------------------

static GLuint s_heightSsbo = 0;   // GL handle; 0 = not yet allocated
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
static GLint    s_locQuadCountX     = -1;  // Phase 10.4: block quad extent X (edge detect)
static GLint    s_locQuadCountY     = -1;  // Phase 10.4: block quad extent Y (edge detect)
static GLint    s_locEdgeStitch     = -1;  // Phase 10.4: packed coarser-neighbour stride

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

    // Shader program (Phase 4) — load unconditionally; SubmitDrawCommands gates
    // on the env var so no pixels change unless MC2_TERRAIN_LOD_CHUNK=1.
    {
        static const char* kPrefix = "#version 430\n";
        glsl_program* prog = glsl_program::makeProgram(
            "terrain_lod_chunk",
            "shaders/terrain_lod_chunk.vert",
            "shaders/terrain_lod_chunk.frag",
            kPrefix);
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
            s_locQuadCountX   = glGetUniformLocation(s_terrainProgram, "u_quadCountX");
            s_locQuadCountY   = glGetUniformLocation(s_terrainProgram, "u_quadCountY");
            s_locEdgeStitch   = glGetUniformLocation(s_terrainProgram, "u_edgeStitch");
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

    const float* mvp = gos_GetTerrainMVPMat4();
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
    // Phase 6: save cull state so we can disable it around skirt draws.
    GLboolean prevCullFace = glIsEnabled(GL_CULL_FACE);

    // Phase 10.3: chunk terrain is OPAQUE and must EXPLICITLY own depth state.
    // The driver previously inherited GL_DEPTH_TEST / glDepthMask / GL_BLEND /
    // glDepthFunc from whatever pass ran before. If a prior transparent/overlay
    // pass left depth WRITES off (glDepthMask FALSE) or blend on, the terrain top
    // renders color but writes NO depth -> never occludes -> "transparent,
    // see-through to the skirts / lower terrain", flipping with draw order and
    // mech-selection (which changes the prior pass). Independent of frag output
    // (confirmed: diag7 with all frag outputs neutralized was still transparent).
    // Set the opaque reverse-Z state explicitly; restore everything at the end.
    static const bool s_depthAlways = (getenv("MC2_TERRAIN_LOD_DEPTH_ALWAYS") != nullptr);
    GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevDepthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevBlend     = glIsEnabled(GL_BLEND);
    GLint     prevDepthFunc = GL_GEQUAL; glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDepthFunc(s_depthAlways ? GL_ALWAYS : GL_GEQUAL);   // reverse-Z opaque terrain

    glUseProgram(s_terrainProgram);
    glBindVertexArray(s_patchVao);

    // Phase 10.3: render terrain DOUBLE-SIDED for the whole draw. terrainMVP bakes
    // the kPixelHomogToGLNDC negative-X scale (reverse-Z / GL-NDC X-flip), which
    // INVERTS triangle winding in screen space — so with GL_CULL_FACE enabled and
    // the default CCW front face the terrain TOP is treated as a backface and
    // culled ("transparent terrain, see the skirts through it"; worse on steep
    // slopes near the camera; flipped by whatever global cull state mech-selection
    // happens to leave set, since the main patch previously inherited it). Disable
    // cull once here for main patches AND skirts; restore the inherited state at
    // the end. Terrain is an opaque heightfield, so double-sided is free of
    // visual cost (the underside is never seen).
    glDisable(GL_CULL_FACE);

    // Bind height SSBO (stays bound for all patches this frame).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, s_heightSsbo);

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

    // Cardinality probe: log every 60 submits.
    static int s_submitCount = 0;
    ++s_submitCount;
    if (s_submitCount % 60 == 0) {
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
    if (prevCullFace)
        glEnable(GL_CULL_FACE);   // Phase 10.3: restore inherited cull state
    // Phase 10.3: restore inherited depth/blend state.
    glDepthMask(prevDepthMask);
    if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
    if (prevBlend)      glEnable(GL_BLEND);
    glDepthFunc((GLenum)prevDepthFunc);
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
