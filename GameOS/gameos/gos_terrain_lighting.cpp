// gos_terrain_lighting.cpp — Phase 1 Stage 3: terrain lighting GPU compute — GPU authoritative.
//
// Design doc: docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md (ac7c492)
// Plan:       docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md (2a5ba54)
//
// Stage 3: GPU path authoritative.
//   CopyResultsToVertexPool: 3-slot T1/T2/T3 non-blocking tryConsume; copies
//     GPU lightRGB/fogRGB into vertices[i] before setupTextures loop.
//   CPU lighting block in quad.cpp:1266-1891 gated off via s_lightingGpuAuth.
//   CostSplitLightingScope bracket retained as retirement telemetry (~0 us post-flip).
//   Parity mode: CopyResultsToVertexPool returns early; CPU body still authoritative.
//   MC2_TERRAIN_LIGHTING_GPU=0 to force legacy CPU path (default-on as of
//   Stage 5 flip 2026-05-11; any other value, including unset, opts in).
//
// 3-slot ring pattern mirrors gpu_cull_readback.cpp (RING_FRAMES=3, dual-buffer,
// glCopyBufferSubData VRAM→BAR, timeout=0 always on hot path).
//
// tl_ prefix (terrain_lighting) on private compile helpers to avoid ODR conflicts
// with identical statics in gpu_cull_compute.cpp (design doc Q1).

#include "gos_terrain_lighting.h"

#include "gos_profiler.h"  // ZoneScopedN, TracyGpuZone

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

#include "../../mclib/terrain.h"    // Terrain::realVerticesMapSide, vertexList, numberVertices
#include "../../mclib/vertex.h"     // Vertex, PostcompVertex
#include "../../mclib/camera.h"     // CameraPtr eye, getTerrainLight(), lightDirection, fogStart/fogFull
#include "../../mclib/tgl.h"        // TG_Light, TG_LIGHT_* constants
#include "../../mclib/quad.h"       // TerrainQuad, TerrainQuad::rainLightLevel, ::lighteningLevel
#include "gameos.hpp"  // gosEnvironment Environment (Environment.Renderer)

// External globals needed for packing
extern DWORD BaseVertexColor;  // defined in code/mechcmd2.cpp / code/logmain.cpp
extern bool  useFog;           // defined in mclib/terrain.cpp

// ---------------------------------------------------------------------------
// Env-gated trace (boot-cached per Debug Instrumentation Rule)
// ---------------------------------------------------------------------------
static const bool s_trace = (getenv("MC2_TERRAIN_LIGHTING_GPU_TRACE") != nullptr);
#define TL_TRACE(fmt, ...) \
    do { if (s_trace) { printf("[TERRAIN_LIGHTING_GPU v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

// Phase 1 Stage 5 — DEFAULT-ON flip (2026-05-11).
//
// Bisection guide for future lighting issues:
//   - Set MC2_TERRAIN_LIGHTING_GPU=0 in env to force the legacy CPU lighting
//     path (quad.cpp:1267-1890 runs; GPU pipeline dormant).
//   - Set MC2_TERRAIN_LIGHTING_PARITY=1 to force dual-run mode: CPU body
//     authoritative + GPU compute compared per-vertex; mismatches print to
//     stderr as [TERRAIN_LIGHTING_PARITY v1] event=mismatch ...
//   - Killswitch-off (env="0") was bit-identical to pre-Phase-1 at flip time;
//     any visual regression that disappears with env="0" is THIS slice.
//   - Killswitch-off that still shows the regression is unrelated.
//
// Stage 4 soak was skipped at user's direction (Tracy delta empirically
// validated: 7.24 ms → 1.87 ms mean on quadSetupTextures at mc2_10 wolfman,
// 4.2× σ compression, 0 parity mismatches across 64M vertex comparisons,
// tier1 5/5 PASS both env states — strong enough perf+parity evidence to
// flip without the 7-day burn-in window).
//
// Phase 1 commit chain for bisection: 4fa7a9a (recon) → 594add9 (Stage 1
// scaffold) → eda2431 (Stage 2 parity-clean math) → ff8de07 (Stage 3
// consumer flip) → THIS COMMIT (Stage 5 default-on).
bool IsEnabled() {
    static const bool s_enabled = [] {
        const char* env = getenv("MC2_TERRAIN_LIGHTING_GPU");
        return (env == nullptr) || (env[0] != '0');
    }();
    return s_enabled;
}

bool IsParityCheckEnabled() {
    static const bool s_parity = (getenv("MC2_TERRAIN_LIGHTING_PARITY") != nullptr);
    return s_parity;
}

} // namespace gos_terrain_lighting

namespace {

// ---------------------------------------------------------------------------
// Private file-scope constants
// ---------------------------------------------------------------------------
static constexpr uint32_t RING_FRAMES = 3u;  // matches gpu_cull_readback.cpp:40

// SSBO binding slots (must match gos_terrain_lighting.comp)
static constexpr uint32_t TL_VERTEX_INPUT_BINDING = 0u;
static constexpr uint32_t TL_LIGHT_INPUT_BINDING  = 1u;
static constexpr uint32_t TL_OUTPUT_BINDING        = 2u;

// Uniform locations (must match layout(location=N) in shader)
static constexpr GLint TL_UNI_NUM_VERTICES    = 0;
static constexpr GLint TL_UNI_NUM_LIGHTS      = 1;
static constexpr GLint TL_UNI_SUN_LIGHT_DIR   = 2;
static constexpr GLint TL_UNI_LIGHT_RED       = 3;
static constexpr GLint TL_UNI_LIGHT_GREEN     = 4;
static constexpr GLint TL_UNI_LIGHT_BLUE      = 5;
static constexpr GLint TL_UNI_AMBIENT_RED     = 6;
static constexpr GLint TL_UNI_AMBIENT_GREEN   = 7;
static constexpr GLint TL_UNI_AMBIENT_BLUE    = 8;
static constexpr GLint TL_UNI_BASE_COLOR_R    = 9;
static constexpr GLint TL_UNI_BASE_COLOR_G    = 10;
static constexpr GLint TL_UNI_BASE_COLOR_B    = 11;
static constexpr GLint TL_UNI_RAIN_LEVEL      = 12;
static constexpr GLint TL_UNI_LIGHTNING_LEVEL = 13;
static constexpr GLint TL_UNI_FOG_START       = 14;
static constexpr GLint TL_UNI_FOG_FULL        = 15;
static constexpr GLint TL_UNI_USE_FOG         = 16;
static constexpr GLint TL_UNI_RENDERER_SW     = 17;

// ---------------------------------------------------------------------------
// Private GL state
// ---------------------------------------------------------------------------
static bool     s_initialized  = false;
static bool     s_shaderBad    = false;   // sticky disable on compile failure
static GLuint   s_program      = 0;

static uint32_t s_numVertices  = 0;
static uint32_t s_maxLights    = 0;

// GPU-side SSBOs (compute reads/writes; no persistent map needed)
static GLuint s_vertexInputSsbo  = 0;   // GpuTerrainVertexInput per vertex
static GLuint s_lightInputSsbo   = 0;   // GpuTerrainLight per light slot
static GLuint s_computeOutputSsbo = 0;  // GpuTerrainLightingOutput per vertex

// 3-slot staging ring (CPU-readable persistent map) matching gpu_cull_readback pattern
static GLuint   s_stagingRing[RING_FRAMES]   = {};
static void*    s_stagingMapped[RING_FRAMES] = {};
static GLsync   s_stagingFence[RING_FRAMES]  = {};
static uint32_t s_currentSlot               = 0;

// Output slot bytes (numVertices * sizeof(GpuTerrainLightingOutput))
static size_t   s_outputSlotBytes = 0;

static uint64_t s_frameIndex = 0;

// Stage 3: tryConsume fallback telemetry counters (mirrors readback_fallback_n2 /
// readback_fallback_conservative pattern from gpu_cull_readback.cpp)
static uint64_t s_lightingFallbackN1          = 0;  // consumed N-1 (production path)
static uint64_t s_lightingFallbackN2          = 0;  // used N-2 fallback
static uint64_t s_lightingFallbackConservative = 0; // skipped update (both not ready)
static uint64_t s_fallbackSummaryFrames        = 0; // frames since last summary

// Parity state
static int       s_parityMismatchesThisFrame = 0;
static long long s_paritySummaryFrames       = 0;
static long long s_paritySummaryVerts        = 0;
static long long s_paritySummaryMismatches   = 0;

// Dense vertex pack buffer (persistent across frames to avoid per-frame alloc)
static std::vector<gos_terrain_lighting::GpuTerrainVertexInput> s_packBuf;
// Light pack buffer
static std::vector<gos_terrain_lighting::GpuTerrainLight> s_lightBuf;

// DRAM-side shadow copy of the last-consumed staging ring slot.
// BAR memory (s_stagingMapped) is WC (write-combining) — reads from it bypass
// CPU caches and go over PCIe, which is ~100-1000× slower than DRAM reads for
// random-access patterns. Reading 14K entries indexed by vertexNum from BAR
// causes severe perf regression. Fix: memcpy the entire staging slot from BAR
// to DRAM once per frame (sequential, WC-read-friendly), then index from DRAM.
static std::vector<gos_terrain_lighting::GpuTerrainLightingOutput> s_dramShadow;

// ---------------------------------------------------------------------------
// Private shader-compile helpers (tl_ prefix — mirrors gpu_cull_compute.cpp:145-231
// privately; design doc Q1: copy, don't factor until a third module appears)
// ---------------------------------------------------------------------------
static char* tl_load_text_file(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static GLuint tl_compile_compute_shader(const char** strings, int count) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    if (!sh) return 0;

    glShaderSource(sh, count, strings, nullptr);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetShaderInfoLog(sh, logLen, nullptr, log);
            printf("[TERRAIN_LIGHTING_GPU v1] compute shader compile error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint tl_link_compute_program(GLuint shader) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;

    glAttachShader(prog, shader);
    glLinkProgram(prog);

    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetProgramInfoLog(prog, logLen, nullptr, log);
            printf("[TERRAIN_LIGHTING_GPU v1] compute program link error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Build compute program from file with shared-header inlining.
// "#version 430\n" is always the first string (per CLAUDE.md Critical Rules).
static GLuint tl_build_terrain_lighting_program(const char* compFname,
                                                 const char* sharedFname,
                                                 const char* debugName)
{
    char* fileSrc    = tl_load_text_file(compFname);
    char* sharedSrc  = tl_load_text_file(sharedFname);

    if (!fileSrc) {
        printf("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail source_missing=%s\n", compFname);
        fflush(stdout);
        delete[] sharedSrc;
        return 0;
    }

    // Inline terrain_lighting_shared.hglsl by replacing the marker block in the .comp file.
    static const char* kBeginMarker = "// --- begin terrain_lighting_shared.hglsl ---";
    static const char* kEndMarker   = "// --- end terrain_lighting_shared.hglsl ---";
    std::string fullSrc;

    if (sharedSrc) {
        char* beginPos = strstr(fileSrc, kBeginMarker);
        char* endPos   = beginPos ? strstr(beginPos, kEndMarker) : nullptr;
        if (beginPos && endPos) {
            fullSrc.append(fileSrc, beginPos - fileSrc);
            fullSrc += "\n// --- begin terrain_lighting_shared.hglsl (inlined) ---\n";
            fullSrc += sharedSrc;
            fullSrc += "\n// --- end terrain_lighting_shared.hglsl (inlined) ---\n";
            const char* afterEnd = endPos + strlen(kEndMarker);
            fullSrc += afterEnd;
        } else {
            fullSrc = fileSrc;
        }
    } else {
        printf("[TERRAIN_LIGHTING_GPU v1] event=shader_load_warn shared_missing=%s\n", sharedFname);
        fflush(stdout);
        fullSrc = fileSrc;
    }

    delete[] fileSrc;
    delete[] sharedSrc;

    const char* kVersion = "#version 430\n";
    std::vector<const char*> srcStrs;
    srcStrs.push_back(kVersion);
    srcStrs.push_back(fullSrc.c_str());

    GLuint sh = tl_compile_compute_shader(srcStrs.data(), (int)srcStrs.size());
    if (!sh) {
        printf("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail compile_failed=%s\n", debugName);
        fflush(stdout);
        return 0;
    }
    GLuint prog = tl_link_compute_program(sh);
    glDeleteShader(sh);
    if (!prog) {
        printf("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail link_failed=%s\n", debugName);
        fflush(stdout);
    }
    return prog;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace gos_terrain_lighting {

GLuint GetOutputSsbo() {
    return s_computeOutputSsbo;
}

// ---------------------------------------------------------------------------
// mission_init — per-mission, called alongside gpu_cull::compute_init()
// ---------------------------------------------------------------------------
void mission_init(uint32_t numVertices, uint32_t maxLights) {
    if (!IsEnabled()) return;

    // Compile shader on first call (lazy, single process lifetime)
    if (!s_program && !s_shaderBad) {
        s_program = tl_build_terrain_lighting_program(
            "shaders/gos_terrain_lighting.comp",
            "shaders/include/terrain_lighting_shared.hglsl",
            "terrain_lighting");
        if (!s_program) {
            s_shaderBad = true;
            printf("[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail sticky_disable\n");
            fflush(stdout);
            return;
        }
    }
    if (s_shaderBad) return;

    // Teardown previous mission's SSBOs if they exist
    mission_shutdown();

    s_numVertices = numVertices;
    s_maxLights   = (maxLights > 0) ? maxLights : 1u;

    const GLsizeiptr vertInputBytes  = static_cast<GLsizeiptr>(numVertices * sizeof(GpuTerrainVertexInput));
    const GLsizeiptr lightInputBytes = static_cast<GLsizeiptr>(s_maxLights * sizeof(GpuTerrainLight));
    const GLsizeiptr outputBytes     = static_cast<GLsizeiptr>(numVertices * sizeof(GpuTerrainLightingOutput));
    s_outputSlotBytes = static_cast<size_t>(outputBytes);

    // Pre-size pack buffers
    s_packBuf.assign(numVertices, GpuTerrainVertexInput{});
    s_lightBuf.resize(s_maxLights);
    // DRAM shadow of staging ring: initialized to all-white (lightRGB/fogRGB=0xFFFFFFFF)
    // so that T3 (conservative, no update) leaves existing vertex colors unchanged on
    // first-frame startup (vertex pool already initialized to 0xFFFFFFFF at vertex.h:122).
    s_dramShadow.assign(numVertices, GpuTerrainLightingOutput{0xFFFFFFFFu, 0xFFFFFFFFu});

    // --- Vertex input SSBO (GPU-write from CPU pack; no persistent map) ---
    glGenBuffers(1, &s_vertexInputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vertexInputSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, vertInputBytes, nullptr,
                    GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Light input SSBO ---
    glGenBuffers(1, &s_lightInputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightInputSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, lightInputBytes, nullptr,
                    GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Compute output SSBO (GPU writes here; no persistent map — pure VRAM) ---
    glGenBuffers(1, &s_computeOutputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_computeOutputSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, outputBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- 3-slot staging ring (CPU-readable persistent map, BAR memory) ---
    // Pattern from gpu_cull_readback.cpp:230-235.
    const GLbitfield stageStorageFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;
    const GLbitfield stageMapFlags     = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;

    glGenBuffers(RING_FRAMES, s_stagingRing);
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[i]);
        glBufferStorage(GL_COPY_WRITE_BUFFER, outputBytes, nullptr, stageStorageFlags);
        s_stagingMapped[i] = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, outputBytes, stageMapFlags);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

        if (!s_stagingMapped[i]) {
            fprintf(stderr, "[TERRAIN_LIGHTING_GPU v1] FATAL: staging persistent map failed slot=%u\n", i);
            fflush(stderr);
            s_shaderBad = true;
            mission_shutdown();
            return;
        }
        memset(s_stagingMapped[i], 0, static_cast<size_t>(outputBytes));
        s_stagingFence[i] = nullptr;
    }

    s_currentSlot  = 0;
    s_frameIndex   = 0;
    s_initialized  = true;

    printf("[TERRAIN_LIGHTING_GPU v1] event=mission_init verts=%u lights=%u outputSlotKB=%zu\n",
           numVertices, s_maxLights, s_outputSlotBytes / 1024u);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// mission_shutdown — per-mission teardown
// ---------------------------------------------------------------------------
void mission_shutdown() {
    if (!s_initialized) return;

    // Wait for all in-flight fences before releasing (GL_TIMEOUT_IGNORED OK in teardown).
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_stagingFence[i]) {
            glClientWaitSync(s_stagingFence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_stagingFence[i]);
            s_stagingFence[i] = nullptr;
        }
    }

    // Unmap and release staging ring
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_stagingRing[i] && s_stagingMapped[i]) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[i]);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            s_stagingMapped[i] = nullptr;
        }
    }
    glDeleteBuffers(RING_FRAMES, s_stagingRing);
    memset(s_stagingRing, 0, sizeof(s_stagingRing));

    if (s_computeOutputSsbo) { glDeleteBuffers(1, &s_computeOutputSsbo); s_computeOutputSsbo = 0; }
    if (s_lightInputSsbo)    { glDeleteBuffers(1, &s_lightInputSsbo);    s_lightInputSsbo    = 0; }
    if (s_vertexInputSsbo)   { glDeleteBuffers(1, &s_vertexInputSsbo);   s_vertexInputSsbo   = 0; }

    s_numVertices     = 0;
    s_maxLights       = 0;
    s_outputSlotBytes = 0;
    s_currentSlot     = 0;
    s_frameIndex      = 0;
    s_initialized     = false;
    s_dramShadow.clear();

    printf("[TERRAIN_LIGHTING_GPU v1] event=mission_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// BeginFrame — advance ring slot
// ---------------------------------------------------------------------------
void BeginFrame() {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;
    s_currentSlot = (s_currentSlot + 1u) % RING_FRAMES;
    ++s_frameIndex;
}

// ---------------------------------------------------------------------------
// PackAndDispatch — pack vertex + light SSBOs, dispatch compute, barrier, copy, fence
// ---------------------------------------------------------------------------
void PackAndDispatch() {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;

    ZoneScopedN("Terrain::TerrainLightingDispatch");

    // --- Stage 2: Dense vertex pack by vertexNum ---
    // Walk the camera-windowed vertex list (land->vertexList, numberVertices entries).
    // For each vertex, slot it at inputs[vertexNum] (dense map-stable index).
    // Zero-initialize the pack buffer each frame so unreached slots are clean.
    if (land && land->getVertexList() && land->getNumVertices() > 0) {
        const int nv = land->getNumVertices();
        const VertexPtr vlist = land->getVertexList();

        // Reset entries for live vertices (don't memset entire array each frame —
        // only clear the flag so shader knows the vertex is "untouched" if not reached,
        // but since we're writing ALL reached slots, this is fine to do selectively).
        // Actually: clear entire pack buffer for correctness (stale data from prior frame
        // at vertexNums that are no longer in camera window). The buffer is numVertices
        // entries; at 14400 × 32B = ~450KB, a memset is fast (~200µs worst case).
        memset(s_packBuf.data(), 0, s_numVertices * sizeof(GpuTerrainVertexInput));

        for (int i = 0; i < nv; ++i) {
            const Vertex& v = vlist[i];
            if (v.vertexNum < 0 || static_cast<uint32_t>(v.vertexNum) >= s_numVertices) continue;

            GpuTerrainVertexInput& vi = s_packBuf[v.vertexNum];
            vi.xy[0]      = v.vx;
            vi.xy[1]      = v.vy;
            vi.elevation  = v.pVertex ? v.pVertex->elevation : 0.0f;
            vi.hazeFactor = v.hazeFactor;  // Stage 2: carry distance fog factor

            if (v.pVertex) {
                vi.normal[0] = v.pVertex->vertexNormal.x;
                vi.normal[1] = v.pVertex->vertexNormal.y;
                vi.normal[2] = v.pVertex->vertexNormal.z;
            } else {
                vi.normal[0] = vi.normal[1] = 0.0f;
                vi.normal[2] = 1.0f;
            }

            // Pack flags (Phase 1 bits 0-3 + Phase 2 bits 4-7)
            uint32_t flags = 0;
            if (v.pVertex) {
                if (v.pVertex->shadow)       flags |= GPU_VERT_SHADOW;
                if (v.calcThisFrame & 1)      flags |= GPU_VERT_CALCFRAME_LIGHT;
                if (v.pVertex->water & 1)     flags |= GPU_VERT_WATER;
                if (v.pVertex->water & 128)   flags |= GPU_VERT_WATER_ANIM_NEG;
                if (v.pVertex->water & 64)    flags |= GPU_VERT_WATER_ANIM_POS;
                if (v.calcThisFrame & 2)      flags |= GPU_VERT_CALCFRAME_WATER;
            }
            vi.flags = flags;
        }

        // Upload full dense pack to GPU
        const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>(s_numVertices * sizeof(GpuTerrainVertexInput));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vertexInputSsbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, uploadBytes, s_packBuf.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // --- Pack light input SSBO (lights starting at index 2 — first two factored into sun) ---
    // quad.cpp:1325: "First two lights are already factored into the above equations!"
    // The shader receives only lights [2..N-1]; u_numLights = count of those.
    uint32_t numLights = 0;
    if (eye) {
        const long nLights = eye->getNumTerrainLights();
        for (long i = 2; i < nLights && numLights < s_maxLights; ++i) {
            TG_LightPtr tl = eye->getTerrainLight(i);
            if (!tl) continue;
            GpuTerrainLight& gl = s_lightBuf[numLights];
            gl.position[0]   = tl->position.x;
            gl.position[1]   = tl->position.y;
            gl.position[2]   = tl->position.z;
            // Map MC2 light types to GPU constants
            switch (tl->lightType) {
                case TG_LIGHT_POINT:   gl.lightType = 1u; break;  // TG_LIGHT_POINT_GPU
                case TG_LIGHT_SPOT:    gl.lightType = 2u; break;  // TG_LIGHT_SPOT_GPU
                case TG_LIGHT_TERRAIN: gl.lightType = 3u; break;  // TG_LIGHT_TERRAIN_GPU
                default:               gl.lightType = 0u; break;
            }
            // Extract RGB from packed aRGB (0xAARRGGBB) → normalize to 0..1
            const DWORD argb = tl->GetaRGB();
            gl.color[0]        = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;  // R
            gl.color[1]        = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;  // G
            gl.color[2]        = static_cast<float>( argb        & 0xFFu) / 255.0f;  // B
            gl.closeDistance   = tl->closeDistance;
            gl.farDistance     = tl->farDistance;
            gl.oneOverDistance = tl->oneOverDistance;
            gl._pad1           = 0.0f;
            gl._pad2           = 0.0f;
            ++numLights;
        }

        if (numLights > 0) {
            const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>(numLights * sizeof(GpuTerrainLight));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightInputSsbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, uploadBytes, s_lightBuf.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }

    // --- Dispatch compute ---
    glUseProgram(s_program);

    // Bind SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_VERTEX_INPUT_BINDING, s_vertexInputSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_LIGHT_INPUT_BINDING,  s_lightInputSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_OUTPUT_BINDING,        s_computeOutputSsbo);

    // Set per-frame uniforms
    glUniform1ui(TL_UNI_NUM_VERTICES, s_numVertices);
    glUniform1ui(TL_UNI_NUM_LIGHTS,   numLights);

    if (eye) {
        // Sun direction (eye->lightDirection — already computed by camera each frame)
        glUniform3f(TL_UNI_SUN_LIGHT_DIR,
                    eye->lightDirection.x,
                    eye->lightDirection.y,
                    eye->lightDirection.z);
        // getLightRed/Green/Blue channels (unsigned char, 0-255)
        glUniform1ui(TL_UNI_LIGHT_RED,    static_cast<GLuint>(eye->lightRed));
        glUniform1ui(TL_UNI_LIGHT_GREEN,  static_cast<GLuint>(eye->lightGreen));
        glUniform1ui(TL_UNI_LIGHT_BLUE,   static_cast<GLuint>(eye->lightBlue));
        glUniform1ui(TL_UNI_AMBIENT_RED,  static_cast<GLuint>(eye->ambientRed));
        glUniform1ui(TL_UNI_AMBIENT_GREEN,static_cast<GLuint>(eye->ambientGreen));
        glUniform1ui(TL_UNI_AMBIENT_BLUE, static_cast<GLuint>(eye->ambientBlue));
        // Fog parameters
        glUniform1f(TL_UNI_FOG_START, eye->fogStart);
        glUniform1f(TL_UNI_FOG_FULL,  eye->fogFull);
    }

    // BaseVertexColor (DWORD 0xAARRGGBB)
    glUniform1ui(TL_UNI_BASE_COLOR_R, static_cast<GLuint>((BaseVertexColor >> 16) & 0xFFu));
    glUniform1ui(TL_UNI_BASE_COLOR_G, static_cast<GLuint>((BaseVertexColor >>  8) & 0xFFu));
    glUniform1ui(TL_UNI_BASE_COLOR_B, static_cast<GLuint>( BaseVertexColor        & 0xFFu));

    // rainLightLevel + lighteningLevel (TerrainQuad statics)
    glUniform1f(TL_UNI_RAIN_LEVEL,      TerrainQuad::rainLightLevel);
    glUniform1ui(TL_UNI_LIGHTNING_LEVEL, static_cast<GLuint>(TerrainQuad::lighteningLevel));

    // useFog (terrain.cpp global)
    glUniform1ui(TL_UNI_USE_FOG, useFog ? 1u : 0u);

    // Environment.Renderer == 3 => software renderer (skip lighting)
    glUniform1ui(TL_UNI_RENDERER_SW, (Environment.Renderer == 3) ? 1u : 0u);

    // Dispatch: ceil(numVertices / 64) workgroups
    const uint32_t numGroups = (s_numVertices + 63u) / 64u;
    glDispatchCompute(numGroups, 1, 1);

    glUseProgram(0);

    TL_TRACE("event=dispatch frame=%llu verts=%u lights=%u groups=%u",
             (unsigned long long)s_frameIndex, s_numVertices, numLights, numGroups);

    // --- Memory barrier: ensure compute shader writes complete ---
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // --- Copy compute output → staging ring slot (VRAM→BAR) ---
    const GLintptr  slotOff = 0;
    const GLsizeiptr slotSz = static_cast<GLsizeiptr>(s_outputSlotBytes);
    glBindBuffer(GL_COPY_READ_BUFFER,  s_computeOutputSsbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingRing[s_currentSlot]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, slotOff, slotOff, slotSz);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    // --- Make persistent-map copy visible to CPU ---
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);

    // --- Delete any prior fence for this slot, then place new fence ---
    if (s_stagingFence[s_currentSlot]) {
        glDeleteSync(s_stagingFence[s_currentSlot]);
        s_stagingFence[s_currentSlot] = nullptr;
    }
    s_stagingFence[s_currentSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

// ---------------------------------------------------------------------------
// CopyResultsToVertexPool — Stage 3: 3-slot T1/T2/T3 non-blocking tryConsume.
// Copies GPU staging ring → vertices[i]->lightRGB / ->fogRGB.
//
// Under parity mode (IsParityCheckEnabled()==true): returns early.
//   The comparator (Parity_CompareFrame) reads GPU values from the mapped ring
//   slot directly via GetMappedOutputForParity() with GL_TIMEOUT_IGNORED.
//   CPU body runs authoritative for vertices[i] in parity mode.
//   (plan v2 MIN-2 fold — avoid CPU-overwrites-GPU dataflow ambiguity.)
//
// Under authoritative mode (IsEnabled() && !IsParityCheckEnabled()):
//   Non-blocking tryConsume with three-tier fallback (design doc Q2):
//     T1: N-1 fence signaled → copy (1-frame latency, normal case).
//     T2: N-2 fence signaled → copy (2-frame latency, emit N2 counter).
//     T3: neither ready → skip (retain prior-frame values, emit conservative counter).
//   glClientWaitSync timeout is ALWAYS 0 on hot path. Never GL_TIMEOUT_IGNORED.
// ---------------------------------------------------------------------------
void CopyResultsToVertexPool(TerrainQuad* quadList, int numberQuads) {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;

    // Under parity mode: do NOT write GPU output into vertex pool.
    // Parity_CompareFrame reads the mapped ring directly.
    if (IsParityCheckEnabled()) return;

    // ---------------------------------------------------------------------------
    // Non-blocking tryConsume (T1/T2/T3) — timeout=0 always.
    // Mirrors readback_tryConsume() pattern in gpu_cull_readback.cpp:317-424.
    // ---------------------------------------------------------------------------
    const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
    const uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;

    // Helper: non-blocking fence check (timeout=0).
    auto isFenceReady = [](uint32_t slot) -> bool {
        if (!s_stagingFence[slot]) return false;
        GLenum result = glClientWaitSync(s_stagingFence[slot], 0, 0);
        return (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
    };

    uint32_t readSlot = RING_FRAMES;  // sentinel: no slot selected
    bool     isN2     = false;

    if (isFenceReady(n1Slot)) {
        glDeleteSync(s_stagingFence[n1Slot]);
        s_stagingFence[n1Slot] = nullptr;
        readSlot = n1Slot;
        isN2     = false;
    } else if (isFenceReady(n2Slot)) {
        glDeleteSync(s_stagingFence[n2Slot]);
        s_stagingFence[n2Slot] = nullptr;
        readSlot = n2Slot;
        isN2     = true;
    }

    ++s_fallbackSummaryFrames;

    if (readSlot == RING_FRAMES) {
        // T3: neither slot ready — retain prior DRAM shadow, vertex pool not updated.
        ++s_lightingFallbackConservative;
        TL_TRACE("event=copy_fallback_conservative frame=%llu", (unsigned long long)s_frameIndex);
    } else {
        // T1 or T2: memcpy BAR staging slot → DRAM shadow (sequential WC read, fast),
        // then scatter DRAM shadow values into vertex pool (DRAM→DRAM, cached, fast).
        //
        // WHY: BAR persistent-mapped memory is WC (write-combining) on x86/AMD.
        // Random-indexed reads from WC memory bypass CPU caches and cause PCIe
        // round-trips. At 14K quad × 4 vertices × indexed reads = ~56K random WC reads
        // → massive perf regression. Sequential memcpy from WC is stream-friendly (WC
        // read coalesces naturally at 64B cache-line granularity) and is 100-1000× faster
        // than random indexed WC reads. After memcpy, all indexing is in DRAM.
        const GpuTerrainLightingOutput* barSrc =
            static_cast<const GpuTerrainLightingOutput*>(s_stagingMapped[readSlot]);

        if (barSrc && !s_dramShadow.empty()) {
            // Step 1: sequential BAR → DRAM copy (stream-friendly WC read).
            memcpy(s_dramShadow.data(), barSrc, s_numVertices * sizeof(GpuTerrainLightingOutput));

            // Step 2: scatter DRAM shadow into vertex pool (DRAM→DRAM, cached).
            const GpuTerrainLightingOutput* src = s_dramShadow.data();
            for (int q = 0; q < numberQuads; ++q) {
                const TerrainQuad& quad = quadList[q];
                for (int vi = 0; vi < 4; ++vi) {
                    Vertex* v = quad.vertices[vi];
                    if (!v) continue;
                    if (v->vertexNum < 0 || static_cast<uint32_t>(v->vertexNum) >= s_numVertices) continue;
                    const GpuTerrainLightingOutput& out = src[v->vertexNum];
                    v->lightRGB = out.lightRGB;
                    v->fogRGB   = out.fogRGB;
                }
            }
        }

        if (isN2) {
            ++s_lightingFallbackN2;
            TL_TRACE("event=copy_fallback_n2 frame=%llu slot=%u", (unsigned long long)s_frameIndex, readSlot);
        } else {
            ++s_lightingFallbackN1;
            TL_TRACE("event=copy_n1 frame=%llu slot=%u", (unsigned long long)s_frameIndex, readSlot);
        }
    }

    // 600-frame fallback summary (mirrors gos_terrain_indirect.cpp:291 cadence).
    if (s_fallbackSummaryFrames % 600 == 0) {
        printf("[TERRAIN_LIGHTING_GPU v1] event=fallback_summary frames=%llu "
               "n1=%llu n2=%llu conservative=%llu\n",
               (unsigned long long)s_fallbackSummaryFrames,
               (unsigned long long)s_lightingFallbackN1,
               (unsigned long long)s_lightingFallbackN2,
               (unsigned long long)s_lightingFallbackConservative);
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// Parity_CompareFrame — Stage 2 implementation
// Design doc Q3: walk quadList, filter calcThisFrame&1, index outputs[vertexNum].
// ---------------------------------------------------------------------------

static void Parity_PrintMismatch(uint64_t frame, int vn, const char* field,
                                 uint32_t legacy, uint32_t gpu)
{
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    ++s_paritySummaryMismatches;
    fprintf(stderr,
            "[TERRAIN_LIGHTING_PARITY v1] event=mismatch frame=%llu vertex=%d "
            "field=%s legacy=0x%08X gpu=0x%08X\n",
            (unsigned long long)frame, vn, field ? field : "?", legacy, gpu);
    fflush(stderr);
}

void Parity_CompareFrame(TerrainQuad* quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput)
{
    if (!IsParityCheckEnabled() || !mappedOutput) return;

    // Reset per-frame mismatch counter for this new frame
    s_parityMismatchesThisFrame = 0;
    ++s_paritySummaryFrames;

    long long vertsChecked = 0;

    for (int q = 0; q < numberQuads; ++q) {
        const TerrainQuad& quad = quadList[q];
        for (int i = 0; i < 4; ++i) {
            const Vertex* v = quad.vertices[i];
            if (!v) continue;
            if (v->vertexNum < 0) continue;       // off-map vertex
            if (!(v->calcThisFrame & 1)) continue; // CPU didn't write this vertex this frame

            const int vn = v->vertexNum;
            ++vertsChecked;

            // Compare lightRGB
            uint32_t legacyLight = v->lightRGB;
            uint32_t gpuLight    = mappedOutput[vn].lightRGB;
            if (legacyLight != gpuLight) {
                Parity_PrintMismatch(s_frameIndex, vn, "lightRGB", legacyLight, gpuLight);
            }

            // Compare fogRGB
            uint32_t legacyFog = v->fogRGB;
            uint32_t gpuFog    = mappedOutput[vn].fogRGB;
            if (legacyFog != gpuFog) {
                Parity_PrintMismatch(s_frameIndex, vn, "fogRGB", legacyFog, gpuFog);
            }
        }
    }

    s_paritySummaryVerts += vertsChecked;

    // 600-frame summary (mirrors gos_terrain_indirect.cpp:291 pattern)
    if (s_paritySummaryFrames % 600 == 0) {
        fprintf(stderr,
                "[TERRAIN_LIGHTING_PARITY v1] event=summary frames=%lld "
                "verts_checked=%lld total_mismatches=%lld\n",
                s_paritySummaryFrames, s_paritySummaryVerts, s_paritySummaryMismatches);
        fflush(stderr);
    }
}

// Helper: synchronously wait and return mapped output for parity comparison.
// ONLY called from terrain.cpp parity path — uses GL_TIMEOUT_IGNORED.
// Production hot path NEVER calls this.
const GpuTerrainLightingOutput* GetMappedOutputForParity()
{
    if (!IsParityCheckEnabled() || !s_initialized) return nullptr;

    // Synchronously wait on current frame's fence (parity mode — stall OK)
    if (s_stagingFence[s_currentSlot]) {
        glClientWaitSync(s_stagingFence[s_currentSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
    }
    return static_cast<const GpuTerrainLightingOutput*>(s_stagingMapped[s_currentSlot]);
}

} // namespace gos_terrain_lighting

// Slice B4 Stage 1b — C-linkage accessor for the per-vertex lighting output SSBO.
// Used by gos_terrain_mask_dispatch::DrawMaskSolid() to bind at binding=2.
// Defined in the same TU as s_computeOutputSsbo (which has internal linkage
// via the anonymous namespace); the implicit using-directive makes the name
// visible by unqualified lookup at file scope.
extern "C" GLuint gos_terrain_lighting_getOutputSSBO() {
    return s_computeOutputSsbo;
}
