// gpu_cull_compute.cpp — Track C slice C1a: GPU visibility mirror (diagnostic mode).
//
// Runs the GPU cull compute shader every frame alongside the CPU path.
// CPU still renders normally — zero change to rendering output.
// Parity summary: [GPU_CULL v1] event=parity_summary every 600 frames.
//
// Killswitch: MC2_GPU_CULL=0 (default) — nothing happens.
// Enable:     MC2_GPU_CULL=1 + MC2_GPU_CULL_SUBSTRATE=1

#include "gpu_cull_compute.h"
#include "gpu_cull_substrate.h"
#include "gpu_cull_record.h"
#include "gos_static_prop_killswitch.h"   // gos_GetTerrainMVPMat4()

#include <GL/glew.h>
#include <gameos.hpp>                     // STOP()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Trace macro (env-gated, default off)
// ---------------------------------------------------------------------------
static const bool s_computeTrace = (getenv("MC2_GPU_CULL_COMPUTE_TRACE") != nullptr);
#define COMPUTE_TRACE(fmt, ...) \
    do { if (s_computeTrace) { printf("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// Debug SSBO layout constants (binding 9)
// Layout: [uint gpuVisibleCount][uint gpuCulledCount][uint visibilityResults[maxActors]]
// ---------------------------------------------------------------------------
static constexpr uint32_t DEBUG_SSBO_BINDING    = 9u;
static constexpr uint32_t CULL_UBO_BINDING      = 2u;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static GLuint    s_computeProgram    = 0;
static GLuint    s_debugSsbo         = 0;
static GLuint    s_frustumUbo        = 0;
static GLuint    s_stagingSsbo       = 0;  // single-slot copy of current ring slot
static uint32_t  s_maxActors         = 0;
static size_t    s_stagingBytes      = 0;  // size of one substrate ring slot
static bool      s_initialized       = false;

// GL timer query for dispatch_us measurement.
static GLuint    s_timerQuery        = 0;
static bool      s_timerPending      = false;
static uint64_t  s_lastDispatchNs    = 0;

// 600-frame summary counter.
static uint32_t  s_summaryCount      = 0;
static bool      s_firstSummaryDone  = false;

// Accumulated false_neg / false_pos between summary emissions.
static uint64_t  s_accFalseNeg       = 0;
static uint64_t  s_accFalsePos       = 0;
static uint32_t  s_dispatchFrames    = 0;

// ---------------------------------------------------------------------------
// Lazy env probe
// ---------------------------------------------------------------------------
bool compute_isEnabled() {
    static bool s_initialized_flag = false;
    static bool s_enabled = false;
    if (!s_initialized_flag) {
        s_initialized_flag = true;
        s_enabled = (getenv("MC2_GPU_CULL") != nullptr);
    }
    return s_enabled;
}

// ---------------------------------------------------------------------------
// Helpers: load + compile a compute shader from file
// ---------------------------------------------------------------------------

// Read a text file into a heap-allocated buffer. Caller must delete[].
static char* load_text_file(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    if ((long)fread(buf, 1, sz, f) != sz) {
        fclose(f); delete[] buf; return nullptr;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

// Compile a GL_COMPUTE_SHADER from source strings.
// Returns 0 on failure (error printed to stdout).
static GLuint compile_compute_shader(const char** strings, int count) {
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
            printf("[GPU_CULL v1] compute shader compile error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Link a program from a compiled compute shader.
// Returns 0 on failure.
static GLuint link_compute_program(GLuint shader) {
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
            printf("[GPU_CULL v1] compute program link error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------
// compute_init
// ---------------------------------------------------------------------------
bool compute_init() {
    if (!compute_isEnabled()) return true;
    if (!substrate_isEnabled()) {
        printf("[GPU_CULL v1] event=compute_init_skip reason=substrate_disabled\n");
        fflush(stdout);
        return true;
    }

    if (s_initialized) {
        compute_shutdown();
    }

    // --- GL version probe ---
    const char* glVer = (const char*)glGetString(GL_VERSION);
    int glMaj = 0, glMin = 0;
    if (glVer) sscanf(glVer, "%d.%d", &glMaj, &glMin);
    const bool computeOk = (glMaj > 4) || (glMaj == 4 && glMin >= 3);
    printf("[GPU_CULL v1] event=gl_probe version=%d.%d compute=%s\n",
           glMaj, glMin, computeOk ? "ok" : "fail");
    fflush(stdout);
    if (!computeOk) {
        STOP(("[GPU_CULL] GL 4.3+ required for compute shaders (got %d.%d)", glMaj, glMin));
        return false;
    }

    // --- Load and compile the compute shader ---
    // The predicate GLSL file is inlined by the include preprocessor in shader_builder,
    // but since we're doing raw GL here, we load both files and concatenate manually.
    const char* kPrefix      = "#version 430\n";
    char* predicateSrc = load_text_file("shaders/gpu_cull_predicate.glsl");
    char* computeSrc   = load_text_file("shaders/gpu_cull.comp");
    if (!predicateSrc || !computeSrc) {
        printf("[GPU_CULL v1] compute shader source not found (predicate=%s, comp=%s)\n",
               predicateSrc ? "ok" : "MISSING",
               computeSrc   ? "ok" : "MISSING");
        fflush(stdout);
        delete[] predicateSrc;
        delete[] computeSrc;
        STOP(("[GPU_CULL] compute shader source files not found"));
        return false;
    }

    // Replace the '#include <gpu_cull_predicate.glsl>' directive in the compute
    // shader with the actual predicate source. This mirrors what the glsl_program
    // include preprocessor does for VS/FS programs.
    // We do a simple text substitution: find the #include line and splice in.
    static const char* kIncludeLine = "#include <gpu_cull_predicate.glsl>";
    char* incPos = strstr(computeSrc, kIncludeLine);
    std::string fullSrc;
    if (incPos) {
        fullSrc.append(computeSrc, incPos - computeSrc);
        fullSrc += "\n// --- begin gpu_cull_predicate.glsl ---\n";
        fullSrc += predicateSrc;
        fullSrc += "\n// --- end gpu_cull_predicate.glsl ---\n";
        const char* afterInc = incPos + strlen(kIncludeLine);
        fullSrc += afterInc;
    } else {
        // No include directive found — use as-is (predicate already inlined).
        fullSrc = computeSrc;
    }
    delete[] predicateSrc;
    delete[] computeSrc;

    const char* shaderStrings[] = { kPrefix, fullSrc.c_str() };
    GLuint sh = compile_compute_shader(shaderStrings, 2);
    if (!sh) {
        STOP(("[GPU_CULL] compute compile failed; see log above"));
        return false;
    }
    GLuint prog = link_compute_program(sh);
    glDeleteShader(sh);
    if (!prog) {
        STOP(("[GPU_CULL] compute link failed; see log above"));
        return false;
    }
    s_computeProgram = prog;

    // --- Debug SSBO (binding 9) ---
    // maxActors from substrate: read from substrate SSBO header would require a
    // roundtrip, so we use a safe upper bound. Use 4096 actors (generous for MC2).
    // Per-spec: size = sizeof(uint)*2 + sizeof(uint)*maxActors.
    s_maxActors = 4096u;
    const GLsizeiptr debugSsboBytes =
        static_cast<GLsizeiptr>(2 * sizeof(uint32_t) + s_maxActors * sizeof(uint32_t));
    glGenBuffers(1, &s_debugSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, debugSsboBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Staging SSBO (binding 8, replaces direct substrate bind) ---
    // The substrate is triple-buffered; each ring slot starts at a non-power-of-two
    // byte offset that doesn't satisfy GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT.
    // glBindBufferRange with a misaligned offset fails silently (GPU reads garbage).
    // Solution: per-frame, copy the active ring slot to a single-slot staging SSBO
    // at offset 0 via glCopyBufferSubData. The compute shader always reads binding 8
    // at offset 0, so no alignment problem.
    // Staging size = one substrate slot = header + capacity * record.
    // We don't know capacity yet (substrate not init'd?), but the substrate reports
    // slotBytes after init. Use a generous maximum: 4096 actors * 64B + 16B header.
    s_stagingBytes = sizeof(GpuActorRecordHeader) + s_maxActors * sizeof(GpuActorRecord);
    glGenBuffers(1, &s_stagingSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_stagingSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Frustum UBO (binding 2) ---
    // Layout: [mat4 viewProj (64B)][uint slotRecordStart (4B)][uint slotRecordCount (4B)][pad pad (8B)]
    // Total: 80 bytes.
    glGenBuffers(1, &s_frustumUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
    glBufferStorage(GL_UNIFORM_BUFFER, 80, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Bind UBO to the binding point declared in the shader.
    glBindBufferBase(GL_UNIFORM_BUFFER, CULL_UBO_BINDING, s_frustumUbo);

    // --- GL timer query ---
    glGenQueries(1, &s_timerQuery);
    s_timerPending = false;

    s_initialized      = true;
    s_summaryCount     = 0;
    s_firstSummaryDone = false;
    s_accFalseNeg      = 0;
    s_accFalsePos      = 0;
    s_dispatchFrames   = 0;

    printf("[GPU_CULL v1] event=compute_selftest result=ok maxActors=%u debugSsboBytes=%lld\n",
           s_maxActors, (long long)debugSsboBytes);
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// compute_shutdown
// ---------------------------------------------------------------------------
void compute_shutdown() {
    if (!compute_isEnabled()) return;

    if (s_timerQuery) {
        if (s_timerPending) {
            // Drain pending query to avoid GL errors.
            GLuint64 dummy = 0;
            glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &dummy);
        }
        glDeleteQueries(1, &s_timerQuery);
        s_timerQuery = 0;
        s_timerPending = false;
    }

    if (s_frustumUbo)  { glDeleteBuffers(1, &s_frustumUbo);  s_frustumUbo  = 0; }
    if (s_debugSsbo)   { glDeleteBuffers(1, &s_debugSsbo);   s_debugSsbo   = 0; }
    if (s_stagingSsbo) { glDeleteBuffers(1, &s_stagingSsbo); s_stagingSsbo = 0; }
    if (s_computeProgram) { glDeleteProgram(s_computeProgram); s_computeProgram = 0; }
    s_stagingBytes = 0;

    s_initialized = false;
    s_maxActors   = 0;

    printf("[GPU_CULL v1] event=compute_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// compute_dispatch
// ---------------------------------------------------------------------------
void compute_dispatch() {
    if (!compute_isEnabled() || !s_initialized) return;

    // The terrain MVP is only valid after the first terrain render.
    const float* mvp = gos_GetTerrainMVPMat4();
    if (!mvp) {
        COMPUTE_TRACE("event=dispatch_skip reason=mvp_null");
        return;
    }

    // Upload view-projection matrix + slot info to the frustum UBO.
    // UBO layout (std140): [mat4 viewProj (64B)][uint slotRecordStart (4B)][uint slotRecordCount (4B)][_pad0][_pad1]
    // Since we use a staging SSBO (always at offset 0), slotRecordStart = 0.
    // slotRecordCount is filled in after we know recordCount (below), but we
    // upload the matrix now and the counts just before dispatch.
    glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, 16 * sizeof(float), mvp);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Copy the active ring slot to the staging SSBO (binding 8).
    // The substrate is triple-buffered; each ring slot in the main SSBO starts at
    // a non-power-of-two byte offset that may not satisfy
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT. glBindBufferRange with a
    // misaligned offset silently fails — the shader reads the wrong data.
    // Fix: copy the active slot to a dedicated single-slot staging SSBO at offset 0.
    GLuint substrateSsbo = substrate_getInstanceSsboName();
    if (!substrateSsbo || !s_stagingSsbo) {
        COMPUTE_TRACE("event=dispatch_skip reason=ssbo_not_ready");
        return;
    }
    const GLintptr   slotOffset = substrate_getCurrentSlotOffset();
    const GLsizeiptr slotBytes  = substrate_getSlotBytes();
    if (slotBytes == 0) {
        COMPUTE_TRACE("event=dispatch_skip reason=slot_bytes_zero");
        return;
    }
    const GLsizeiptr copyBytes =
        (slotBytes <= (GLsizeiptr)s_stagingBytes) ? slotBytes : (GLsizeiptr)s_stagingBytes;

    // Copy substrate slot → staging at offset 0.
    glBindBuffer(GL_COPY_READ_BUFFER,  substrateSsbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingSsbo);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        slotOffset, 0, copyBytes);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    // Read record count from the staging SSBO header (now at offset 0).
    GpuActorRecordHeader hdrCopy{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_stagingSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GpuActorRecordHeader), &hdrCopy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const uint32_t recordCount = hdrCopy.recordCount;
    if (recordCount == 0) {
        COMPUTE_TRACE("event=dispatch_skip reason=record_count_zero");
        return;
    }

    // Bind the staging SSBO at binding 8 (the shader always reads from offset 0).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                     substrate_getInstanceSsboBindingPoint(), s_stagingSsbo);

    // Clamp debug SSBO to our allocated maxActors.
    const uint32_t effectiveCount = (recordCount <= s_maxActors) ? recordCount : s_maxActors;

    // Zero the debug SSBO (counts + per-actor array for effectiveCount actors).
    const GLsizeiptr clearBytes =
        static_cast<GLsizeiptr>(2 * sizeof(uint32_t) + effectiveCount * sizeof(uint32_t));
    glClearNamedBufferSubData(s_debugSsbo, GL_R32UI, 0, clearBytes, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);

    // Bind debug SSBO (binding 9).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, DEBUG_SSBO_BINDING, s_debugSsbo);

    // Upload slot record count to UBO.
    // UBO slot-info is at byte offset 64 (after the 16×4 = 64-byte mat4).
    // std140 layout: each uint is 4 bytes, aligned to 4.
    {
        uint32_t slotInfo[4] = { 0u, recordCount, 0u, 0u };
        glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 64, sizeof(slotInfo), slotInfo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    // Bind frustum UBO to binding 2.
    glBindBufferBase(GL_UNIFORM_BUFFER, CULL_UBO_BINDING, s_frustumUbo);

    // Begin GL timer query.
    if (s_timerPending) {
        // Collect previous frame's result before starting a new query.
        glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &s_lastDispatchNs);
        s_timerPending = false;
    }
    glBeginQuery(GL_TIME_ELAPSED, s_timerQuery);

    // Dispatch the compute shader.
    glUseProgram(s_computeProgram);
    const uint32_t groups = (recordCount + 63u) / 64u;
    glDispatchCompute(groups, 1, 1);
    glUseProgram(0);

    glEndQuery(GL_TIME_ELAPSED);
    s_timerPending = true;

    // Memory barrier: ensure shader writes are visible before CPU reads back counts.
    // GL_SHADER_STORAGE_BARRIER_BIT covers SSBO writes.
    // NOT GL_ATOMIC_COUNTER_BARRIER_BIT (no ACBOs).
    // NOT GL_COMMAND_BARRIER_BIT (no indirect draws in C1a).
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    ++s_dispatchFrames;
    COMPUTE_TRACE("event=dispatch recordCount=%u groups=%u", recordCount, groups);
}

// ---------------------------------------------------------------------------
// compute_emitParitySummary
// ---------------------------------------------------------------------------
void compute_emitParitySummary() {
    if (!compute_isEnabled() || !s_initialized) return;
    if (s_dispatchFrames == 0) return;

    // Collect timer result (may be from previous frame if query is still pending).
    uint64_t dispatchNs = s_lastDispatchNs;
    if (s_timerPending) {
        // Try a non-blocking check first.
        GLint available = 0;
        glGetQueryObjectiv(s_timerQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &dispatchNs);
            s_lastDispatchNs = dispatchNs;
            s_timerPending = false;
        }
        // If not available yet, use last known value.
    }
    const uint32_t dispatchUs = (uint32_t)(dispatchNs / 1000u);

    // Read back gpu visible/culled counts from debug SSBO.
    uint32_t gpuVis = 0, gpuCull = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t), &gpuVis);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(uint32_t), sizeof(uint32_t), &gpuCull);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    const uint32_t cpuVis = substrate_getCpuVisibleCount();

    // --- Per-actor false_neg / false_pos via full readback ---
    // This is acceptable in shadow/diagnostic mode (C1a); not done in hot paths.
    const uint32_t totalActors = gpuVis + gpuCull;
    uint32_t falseNeg = 0, falsePos = 0;
    if (totalActors > 0 && totalActors <= s_maxActors) {
        // Read per-actor visibility bits from the debug SSBO.
        const uint32_t visResultOffset = 2 * sizeof(uint32_t);
        std::vector<uint32_t> gpuBits(totalActors);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           static_cast<GLintptr>(visResultOffset),
                           static_cast<GLsizeiptr>(totalActors * sizeof(uint32_t)),
                           gpuBits.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Read per-actor prevVisibilityBit from the staging SSBO.
        // The staging SSBO always holds the active ring slot at offset 0.
        if (s_stagingSsbo) {
            std::vector<uint32_t> cpuBits(totalActors);
            // GpuActorRecord: prevVisibilityBit at offset 52, stride 64, header 16B.
            static const size_t kRecordStride     = 64u;
            static const size_t kPrevVisBitOffset = 52u;
            static const size_t kHeaderBytes      = sizeof(GpuActorRecordHeader);

            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_stagingSsbo);
            for (uint32_t i = 0; i < totalActors; ++i) {
                GLintptr off = static_cast<GLintptr>(
                    kHeaderBytes + i * kRecordStride + kPrevVisBitOffset);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   off, sizeof(uint32_t), &cpuBits[i]);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            for (uint32_t i = 0; i < totalActors; ++i) {
                const bool cpuVisible = (cpuBits[i] != 0u);
                const bool gpuVisible = (gpuBits[i] != 0u);
                if (cpuVisible && !gpuVisible) ++falseNeg;
                if (!cpuVisible && gpuVisible) ++falsePos;
            }
        }
    }

    s_accFalseNeg += falseNeg;
    s_accFalsePos += falsePos;
    ++s_summaryCount;

    // Emit every 600 frames and on first summary.
    const bool doEmit = !s_firstSummaryDone || ((s_summaryCount % 600) == 0);
    if (doEmit) {
        s_firstSummaryDone = true;
        printf("[GPU_CULL v1] event=parity_summary"
               " gpu_visible=%u cpu_visible=%u"
               " total_actors=%u"
               " false_neg=%u false_pos=%u"
               " dispatch_us=%u flush=%u\n",
               gpuVis, cpuVis,
               totalActors,
               falseNeg, falsePos,
               dispatchUs, s_dispatchFrames);
        fflush(stdout);
        // Reset accumulators.
        s_accFalseNeg    = 0;
        s_accFalsePos    = 0;
        s_dispatchFrames = 0;
    }
}

} // namespace gpu_cull
