// gos_terrain_lighting.cpp — Phase 1 Stage 1: terrain lighting GPU compute.
//
// Design doc: docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md (ac7c492)
// Plan:       docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md (2a5ba54)
//
// Stage 1: SSBO scaffold + 3-slot non-blocking ring + dispatch.
//   CopyResultsToVertexPool is a no-op stub (output unused at Stage 1).
//   Legacy CPU lighting still runs unmodified.
//   MC2_TERRAIN_LIGHTING_GPU=1 to enable; default off at Stage 1.
//
// 3-slot ring pattern mirrors gpu_cull_readback.cpp (RING_FRAMES=3, dual-buffer,
// glCopyBufferSubData VRAM→BAR, timeout=0 always on hot path).
//
// tl_ prefix (terrain_lighting) on private compile helpers to avoid ODR conflicts
// with identical statics in gpu_cull_compute.cpp (design doc Q1).

#include "gos_terrain_lighting.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

#include "../../mclib/terrain.h"    // Terrain::realVerticesMapSide, vertexList, numberVertices
#include "../../mclib/vertex.h"     // Vertex, PostcompVertex
#include "../../mclib/camera.h"     // CameraPtr eye, getTerrainLight()
#include "../../mclib/tgl.h"        // TG_Light, TG_LIGHT_* constants

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

// Whether the feature is enabled (default off at Stage 1).
bool IsEnabled() {
    static const bool s_enabled = (getenv("MC2_TERRAIN_LIGHTING_GPU") != nullptr);
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

// ---------------------------------------------------------------------------
// Private GL state
// ---------------------------------------------------------------------------
static bool     s_initialized  = false;
static bool     s_shaderBad    = false;   // sticky disable on compile failure
static GLuint   s_program      = 0;

static uint32_t s_numVertices  = 0;
static uint32_t s_maxLights    = 0;

// GPU-side SSBOs (compute reads/writes; no persistent map needed)
static GLuint s_vertexInputSsbo = 0;   // GpuTerrainVertexInput per vertex
static GLuint s_lightInputSsbo  = 0;   // GpuTerrainLight per light slot
static GLuint s_computeOutputSsbo = 0; // GpuTerrainLightingOutput per vertex

// 3-slot staging ring (CPU-readable persistent map) matching gpu_cull_readback pattern
static GLuint   s_stagingRing[RING_FRAMES]   = {};
static void*    s_stagingMapped[RING_FRAMES] = {};
static GLsync   s_stagingFence[RING_FRAMES]  = {};
static uint32_t s_currentSlot               = 0;

// Output slot bytes (numVertices * sizeof(GpuTerrainLightingOutput))
static size_t   s_outputSlotBytes = 0;

static uint64_t s_frameIndex = 0;

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

// Build compute program from file with preamble injection.
// Inlines terrain_lighting_shared.hglsl by replacing the marker comment
// (no GL_GOOGLE_include_directive needed).
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
            // Everything before the begin marker
            fullSrc.append(fileSrc, beginPos - fileSrc);
            // The shared header content
            fullSrc += "\n// --- begin terrain_lighting_shared.hglsl (inlined) ---\n";
            fullSrc += sharedSrc;
            fullSrc += "\n// --- end terrain_lighting_shared.hglsl (inlined) ---\n";
            // Everything after the end marker line
            const char* afterEnd = endPos + strlen(kEndMarker);
            fullSrc += afterEnd;
        } else {
            fullSrc = fileSrc;
        }
    } else {
        // shared header missing: proceed without it (struct defs in .comp are inline-commented)
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
    // MAP_READ | MAP_WRITE | MAP_PERSISTENT | MAP_COHERENT.
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
            // Don't abort — just disable gracefully
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

    printf("[TERRAIN_LIGHTING_GPU v1] event=mission_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// BeginFrame — advance ring slot
// ---------------------------------------------------------------------------
void BeginFrame() {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;
    // Advance current slot at start of frame so PackAndDispatch writes to fresh slot.
    s_currentSlot = (s_currentSlot + 1u) % RING_FRAMES;
    ++s_frameIndex;
}

// ---------------------------------------------------------------------------
// PackAndDispatch — pack vertex + light SSBOs, dispatch compute, barrier, copy, fence
// ---------------------------------------------------------------------------
void PackAndDispatch() {
    if (!IsEnabled() || !s_initialized || s_shaderBad) return;

    // --- Pack vertex input SSBO ---
    // Walk Terrain::vertexList (camera-windowed) and pack each entry by vertexNum.
    // We upload the entire dense array [0, numVertices) using glBufferSubData.
    // Only entries present in this frame's vertexList will have valid data;
    // the rest retain zeros from memset at mission_init.
    //
    // For Stage 1 (output unused), we need a valid pack to drive dispatch —
    // we don't need perfect data, just non-crashing data.
    // We pack only the live camera-windowed vertices to avoid O(realVerticesMapSide²) CPU work.
    // Stage 2 will expand this to full dense pack if needed for correct lighting.
    {
        if (land && land->getVertexList() && land->getNumVertices() > 0) {
            const int nv = land->getNumVertices();
            // Allocate temp pack buffer on stack (max ~16KB for 500 verts × 32 B — fine)
            // For large maps use heap. Use static vector to avoid per-frame alloc.
            static std::vector<GpuTerrainVertexInput> s_packBuf;
            if (static_cast<int>(s_packBuf.size()) < nv) s_packBuf.resize(nv);

            const VertexPtr vlist = land->getVertexList();
            for (int i = 0; i < nv; ++i) {
                const Vertex& v = vlist[i];
                GpuTerrainVertexInput& vi = s_packBuf[i];
                vi.xy[0]      = v.vx;
                vi.xy[1]      = v.vy;
                vi.elevation  = v.pVertex ? v.pVertex->elevation : 0.0f;
                vi._pad0      = 0.0f;
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

            // Upload to GPU input SSBO (by vertex index, not vertexNum, for Stage 1)
            const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>(nv * sizeof(GpuTerrainVertexInput));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vertexInputSsbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, uploadBytes, s_packBuf.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }

    // --- Pack light input SSBO ---
    {
        uint32_t numLights = 0;
        static std::vector<GpuTerrainLight> s_lightBuf;
        if (s_lightBuf.size() < s_maxLights) s_lightBuf.resize(s_maxLights);

        if (eye) {
            const long nLights = eye->getNumTerrainLights();
            for (long i = 0; i < nLights && numLights < s_maxLights; ++i) {
                TG_LightPtr tl = eye->getTerrainLight(i);
                if (!tl) continue;
                GpuTerrainLight& gl = s_lightBuf[numLights];
                gl.position[0] = tl->position.x;
                gl.position[1] = tl->position.y;
                gl.position[2] = tl->position.z;
                // Map MC2 light types to GPU constants
                switch (tl->lightType) {
                    case TG_LIGHT_POINT:   gl.lightType = 1u; break;  // TG_LIGHT_POINT_GPU
                    case TG_LIGHT_SPOT:    gl.lightType = 2u; break;  // TG_LIGHT_SPOT_GPU
                    case TG_LIGHT_TERRAIN: gl.lightType = 3u; break;  // TG_LIGHT_TERRAIN_GPU
                    default:               gl.lightType = 0u; break;
                }
                // Extract RGB from packed aRGB (0xAARRGGBB)
                const DWORD argb = tl->GetaRGB();
                gl.color[0]      = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;  // R
                gl.color[1]      = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;  // G
                gl.color[2]      = static_cast<float>( argb        & 0xFFu) / 255.0f;  // B
                gl.falloffParam  = tl->closeDistance;
                ++numLights;
            }
        }

        if (numLights > 0) {
            const GLsizeiptr uploadBytes = static_cast<GLsizeiptr>(numLights * sizeof(GpuTerrainLight));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightInputSsbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, uploadBytes, s_lightBuf.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }

        // --- Dispatch compute ---
        glUseProgram(s_program);

        // Bind SSBOs
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_VERTEX_INPUT_BINDING, s_vertexInputSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_LIGHT_INPUT_BINDING,  s_lightInputSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TL_OUTPUT_BINDING,        s_computeOutputSsbo);

        // Set uniforms
        glUniform1ui(TL_UNI_NUM_VERTICES, s_numVertices);
        glUniform1ui(TL_UNI_NUM_LIGHTS,   numLights);

        // Dispatch: ceil(numVertices / 64) workgroups
        const uint32_t numGroups = (s_numVertices + 63u) / 64u;
        glDispatchCompute(numGroups, 1, 1);

        glUseProgram(0);

        TL_TRACE("event=dispatch frame=%llu verts=%u lights=%u groups=%u",
                 (unsigned long long)s_frameIndex, s_numVertices, numLights, numGroups);

        // --- Memory barrier: ensure compute shader writes complete ---
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // --- Copy compute output → staging ring slot (VRAM→BAR) ---
        const GLintptr  slotOff = 0;  // Each staging slot is exactly outputBytes
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
}

// ---------------------------------------------------------------------------
// CopyResultsToVertexPool — Stage 1: no-op stub
// (output unused at Stage 1; CPU lighting still authoritative)
// ---------------------------------------------------------------------------
void CopyResultsToVertexPool(TerrainQuad* /*quadList*/, int /*numberQuads*/) {
    // Stage 1 no-op. Stage 3 implements 3-slot T1/T2/T3 non-blocking tryConsume
    // and copies staging ring data into vertices[i]->lightRGB / ->fogRGB.
    (void)0;
}

// ---------------------------------------------------------------------------
// Parity_CompareFrame — Stage 2 implementation (placeholder at Stage 1)
// ---------------------------------------------------------------------------
void Parity_CompareFrame(TerrainQuad* /*quadList*/, int /*numberQuads*/,
                         const GpuTerrainLightingOutput* /*mappedOutput*/) {
    // Stage 2 implementation.
    (void)0;
}

} // namespace gos_terrain_lighting
