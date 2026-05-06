#include "gos_static_prop_batcher.h"
#include "gos_static_prop_killswitch.h"  // gos_GetGLTextureId
#include "gos_profiler.h"
#include "gos_object_parity.h"           // Slice 2 Stage 2.D.1 parity harness
#include "gameos.hpp"
#include "utils/shader_builder.h"
#include "tgl.h"  // TG_Shape::s_worldToClip
#include <GL/glew.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Global runtime toggle for the GPU static-prop renderer. Defined here
// (in the gameos lib) so every consumer — mc2.exe, aseconv, other data
// tools that link mclib — resolves the symbol.
bool g_useGpuStaticProps = false;
// Default-on as of 2026-05-04. Flipped without Stage 2.E visual-diff gate
// passing — Stage 2.D parity check (29 missions, 0 mismatches, +0 destroys)
// establishes numerical correctness; visual confirmation is via prior
// power-user testing with MC2_GPU_OBJECTS=1. Set MC2_GPU_OBJECTS=0 in
// env to opt out.
bool g_useGpuObjects = true;

static const bool s_alphaTrace = (getenv("MC2_ALPHA_TEST_TRACE") != nullptr);
#define ALPHA_TRACE(fmt, ...) \
    do { if (s_alphaTrace) { printf("[ALPHA_TEST] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// MC2_TEX_HANDOFF_TRACE=1 — logs texture handle resolution at register and draw time.
// Prints once per unique (multiShape,slot) pair at registration, and once per draw type
// at flush (first flush only). Use to diagnose the GPU-path upscale-texture miss.
static const bool s_texHandoffTrace = (getenv("MC2_TEX_HANDOFF_TRACE") != nullptr);
#define TEX_HANDOFF(fmt, ...) \
    do { if (s_texHandoffTrace) { printf("[TEX_HANDOFF] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// MC2_TREE_DIAG_TRACE=1 — diagnostic 2026-05-05 for the "black billboard square" bug.
// Prints up to 8 lines from each of submit() (dynamic register/replay path) and
// submitCachedInstance() (static recipe replay path), with all GpuStaticPropInstance
// fields formatted identically so a side-by-side grep diff pinpoints which baked
// field is stale on the static replay. Counters are function-local statics so they
// reset per process lifetime; format matches both call sites verbatim.
static const bool s_treeDiagTrace = (getenv("MC2_TREE_DIAG_TRACE") != nullptr);
#define TREE_DIAG(fmt, ...) \
    do { if (s_treeDiagTrace) { fprintf(stderr, "[TREE_DIAG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } } while (0)

namespace {

// Per-vertex stride in the shared VBO. Layout:
//   vec3  a_position         (0..11)
//   vec3  a_normal           (12..23)
//   vec2  a_uv               (24..31)
//   uint  a_localVertexID    (32..35)
//   float _pad               (36..39)
// Kept in sync with shaders/static_prop.vert (Task 9).
constexpr size_t kVertexStride = 40;

constexpr uint32_t RING_FRAMES = 3;
// Stage 2.D.1.1 (Item 4): STATIC_PROP_RING_FRAMES in the header equals this.
// Enforce in lock-step so a future edit to either side fails loudly.
static_assert(RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "RING_FRAMES and STATIC_PROP_RING_FRAMES must match — "
              "changing one without the other corrupts the parity SSBO readback");
constexpr size_t   INITIAL_INSTANCES_PER_FRAME = 4096;
constexpr size_t   INITIAL_COLORS_PER_FRAME    = 1'000'000;  // uint32 ARGB entries

// Immutable per-map geometry.
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
GLuint s_sharedVao = 0;

// Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO.
// Holds {hotPinkRGB, hotYellowRGB, hotGreenRGB} per registered TG_TypeShape,
// indexed by inst.typeID at draw time. Built once at finalizeGeometry from
// TG_TypeShape::hotPinkRGB/hotYellowRGB/hotGreenRGB (mclib/tgl.h:575-577) and
// uploaded as immutable storage. Consumed by static_prop.vert via std430
// binding 2 to feed get_base_light()'s per-type magic-color parameters.
// 48 bytes per type × ~50 types ≈ 2.4 KB total.
GLuint s_perTypeSsbo = 0;

// Per-frame persistent-mapped rings.
GLuint   s_instanceSsbo = 0;
GLuint   s_colorSsbo    = 0;
void*    s_instanceMap  = nullptr;
void*    s_colorMap     = nullptr;
GLsync   s_fence[RING_FRAMES] = {0};
uint32_t s_frameSlot = 0;
size_t   s_instanceCapacity = 0;
size_t   s_colorCapacity    = 0;

// Slice 2 (object-offload) — Stage 2.D.1: parity readback harness.
//
// The parity SSBO is allocated lazily by gos_object_parity::EnsureParityOutputSSBO()
// the first time flush() runs with MC2_OBJECT_PARITY_CHECK=1 set. It's sized
// kParitySlotBytes per slot * RING_FRAMES (matches s_fence ring depth).
//
// Stock tier1 missions show on the order of low-thousands of GPU-drawn
// instances per frame. Worst per-type vertex count is bounded by the
// largest TG_TypeShape::numTypeVertices in the loaded map (typically
// 100-500 verts for buildings, less for trees). 4 MB per slot covers
// ~1M uint32 entries (1M lit-ARGB writes), which is comfortably above
// the actual draw load (RAlt+0 stress survey ~250-400K verts/frame on
// tier1's busiest shoppingmall+suburb missions). Sized at a power of two
// so the per-type byte alignment trim is cheap.
constexpr size_t kParitySlotBytes = 4 * 1024 * 1024;

// Per-slot byte usage of the parity SSBO. Tracks how much of slot
// kParitySlotBytes was actually written in the most recent visit to that
// slot, so the next-visit readback at the top of flush() can scope its
// glGetBufferSubData to exactly the live range. Indexed by s_frameSlot.
size_t   s_parityBytesUsedThisFrame = 0;
size_t   s_parityBytesUsedPerSlot[RING_FRAMES] = {0};
// Discard buffer for the readback. Sized once on first parity-on visit;
// std::vector keeps it alive for the lifetime of the process so we don't
// reallocate per frame. Bytes are read into this and DISCARDED — Stage
// 2.D.1 does no compare; 2.D.2/2.D.3 will use these bytes against a CPU
// recompute for the byte-wise compare.
std::vector<uint8_t> s_parityReadbackScratch;

// Forward decl -- body appears after state block below, so it can reference
// s_fatalRegistrationFailure which is declared further down in this namespace.
void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries);

// CPU staging for the current frame.
// Instances are staged in per-type buckets (not a flat list) so that
// flush() can write each type's instances into a contiguous SSBO region.
// Binding that region via glBindBufferRange means gl_InstanceID in the
// shader is 0..N-1 within the bucket -- NOT dependent on gl_BaseInstance
// and NOT requiring any extension.
struct PerTypeBucket {
    std::vector<GpuStaticPropInstance> instances;
    std::vector<uint32_t>              colors;  // concatenated per-instance color blocks
};
std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// Stage 3.C: per-submitMultiShape batch accumulator. Cleared at the start
// of each submitMultiShape(); populated by submit() per leaf. After
// submitMultiShape() returns true, getLastBuiltBatch() returns this vector
// for snapshot registration in GpuStaticPropRegistry.
std::vector<GpuStaticPropInstance> s_lastBuiltBatch;

// Populated at flush time: per-type contiguous byte offset into the
// ring-slot SSBO (instance + color), used to bind exactly that range.
struct TypeRangeSsbo {
    size_t instanceByteOffset;
    size_t instanceByteSize;
    size_t colorByteOffset;
    size_t colorByteSize;
    uint32_t instanceCount;
};

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;

// CPU-side staging during registration (cleared after finalizeGeometry).
std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Main static-prop program (Task 9). Lazy-loaded on first flush()/flushShadow().
// We keep a glsl_program* around for any future uniform-introspection needs,
// but most call sites will read the raw GL handle via s_staticPropProgram.
glsl_program* s_staticPropProgramObj = nullptr;
GLuint        s_staticPropProgram    = 0;

// Stage 2.D.1.1 (Item 2): parity uniform locations cached at link time.
// Querying glGetUniformLocation every flush was a per-flush driver round-trip;
// cache once here. The shader is not hot-reloadable (s_programLoadTried latches
// after first attempt, no in-process relink path), so the cache is valid for
// the lifetime of the program handle. -1 signals "not found in shader" and is
// safe to pass to glUniform*i (the spec says glUniform* is a no-op for loc=-1).
GLint s_loc_u_parityWrite        = -1;
GLint s_loc_u_parityVertsPerType = -1;
GLint s_loc_u_parityBaseVertex   = -1;

// Latched once a compile/link attempt has failed. We never retry inside a
// session because shader source can only change between runs. With this
// latched true, submit() returns false (so callers CPU-fallback), and
// flush()/flushShadow() short-circuit immediately. The user can keep the
// killswitch ON or OFF with no behavioral difference until the next build.
bool s_programLoadTried  = false;
bool s_programLoadFailed = false;

void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    std::fprintf(stderr, "[GPUPROPS-DIAG] loadProgramsIfNeeded ENTER\n");
    // Log GL / GLSL version so we know what this driver/context supports.
    const char* glv   = (const char*)glGetString(GL_VERSION);
    const char* glslv = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    std::fprintf(stderr, "[GPUPROPS-DIAG] GL_VERSION=%s\n", glv ? glv : "(null)");
    std::fprintf(stderr, "[GPUPROPS-DIAG] GL_SHADING_LANGUAGE_VERSION=%s\n",
                 glslv ? glslv : "(null)");
    s_programLoadTried = true;

    // makeProgram() is the project's shader loader (see gos_postprocess.cpp
    // for existing usage). Pass the "#version 430\n" prefix explicitly — the
    // shader files must NOT contain a #version directive.
    // GLSL 430 required for std430 SSBO. gos_render.cpp now requests a GL
    // 4.3 core context (bumped from 4.0) to match.
    static const char* kShaderPrefix = "#version 430\n";
    s_staticPropProgramObj = glsl_program::makeProgram(
        "static_prop",
        "shaders/static_prop.vert",
        "shaders/static_prop.frag",
        kShaderPrefix);
    if (!s_staticPropProgramObj || !s_staticPropProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[GPUPROPS] failed to compile/link static_prop shader pair — "
            "GPU path disabled for this session; all static props will "
            "CPU-fallback via submit()==false\n");
        s_staticPropProgramObj = nullptr;
        s_staticPropProgram    = 0;
        s_programLoadFailed    = true;
        return;
    }
    s_staticPropProgram = s_staticPropProgramObj->shp_;
    // Stage 2.D.1.1 (Item 2): cache parity uniform locations once at link time.
    s_loc_u_parityWrite        = glGetUniformLocation(s_staticPropProgram, "u_parityWrite");
    s_loc_u_parityVertsPerType = glGetUniformLocation(s_staticPropProgram, "u_parityVertsPerType");
    s_loc_u_parityBaseVertex   = glGetUniformLocation(s_staticPropProgram, "u_parityBaseVertex");
    std::fprintf(stderr, "[GPUPROPS-DIAG] loadProgramsIfNeeded OK prog=%u "
                 "loc_parityWrite=%d loc_parityVertsPerType=%d loc_parityBaseVertex=%d\n",
                 s_staticPropProgram,
                 s_loc_u_parityWrite, s_loc_u_parityVertsPerType,
                 s_loc_u_parityBaseVertex);
}

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;

void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededColorEntries > s_colorCapacity;
    if (!needGrow) return;

    // Wait for all in-flight frames before resizing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_colorSsbo)    { glDeleteBuffers(1, &s_colorSsbo);    s_colorSsbo    = 0; s_colorMap    = nullptr; }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : INITIAL_INSTANCES_PER_FRAME);
    s_colorCapacity    = std::max(neededColorEntries,
        s_colorCapacity    ? s_colorCapacity    * 2 : INITIAL_COLORS_PER_FRAME);

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    nullptr, storageFlags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    mapFlags);

    glGenBuffers(1, &s_colorSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_colorSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    nullptr, storageFlags);
    s_colorMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    mapFlags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_colorMap) {
        std::fprintf(stderr, "[GPUPROPS] persistent map failed; disabling GPU path\n");
        s_fatalRegistrationFailure = true;
    }
}

// ---------------------------------------------------------------------------
// Counter state added in Task 6 (Stage 1.D — Gate F + late-registration).
// ---------------------------------------------------------------------------

// Population indexed by GpuStaticPropPopulation enum value:
//   0=Building, 1=Tree, 2=Generic, 3=Legacy.
// Legacy is excluded from Gate F's fallback-rate computation
// (slice-1 populations only).
constexpr int kPopCount = 4;

struct ObjBatcherCounters {
    // Per-frame, indexed by population:
    uint32_t eligible_actors_by_pop[kPopCount]     = {0};
    uint32_t submitted_instances_by_pop[kPopCount] = {0};  // success only
    uint32_t cpu_fallback_by_pop[kPopCount]        = {0};
    uint32_t submitted_children                    = 0;
    uint32_t skipped_children                      = 0;
    // Per-flush:
    uint32_t gpu_drawn_instances                   = 0;
    // Monotonic since process start:
    uint64_t mono_eligible_actors_by_pop[kPopCount]     = {0};
    uint64_t mono_submitted_instances_by_pop[kPopCount] = {0};
    uint64_t mono_cpu_fallback_by_pop[kPopCount]        = {0};
    uint64_t mono_submitted_children                = 0;
    uint64_t mono_skipped_children                  = 0;
    uint64_t mono_gpu_drawn_instances               = 0;
    uint64_t frame_count                            = 0;
};
ObjBatcherCounters s_counters;
bool s_objbatcherTrace     = false;
bool s_objbatcherTraceInit = false;
bool s_atexitRegistered    = false;

// Late-registration aggregate per-type accounting + allowlist.
std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
std::unordered_set<std::string> s_lateRegisterAllowlist;
bool s_lateRegisterAllowlistLoaded = false;

// Slice 2, Stage 2.A — late-registration recovery signal.
// Set true inside the late-reg branch of submitMultiShape; cleared to false
// at the top of every submitMultiShape call. Read by the public method
// wasLastFailureLateRegistration() so callers (Stage 2.B) can set
// needsFullBakeNextFrame on the owning actor without adding a return-value
// channel to submitMultiShape.
bool s_lastSubmitWasLateReg = false;

// Monotonic counter of times submitMultiShape returned false on the
// late-registration branch (across all frames since process start).
// Added to the [OBJBATCHER v1] summary line.
uint64_t s_late_register_recovery_skips = 0;

// Slice 2 (Stage 2.D.3) — "currently-submitting multishape" pointer used to
// gate per-leaf snapshot capture in submit() to only the sampled actor.
//
// Set at the top of submitMultiShape (after the registered-types check), used
// inside submit() to compare against the sampler's current pick, and reset to
// nullptr at the bottom of submitMultiShape so a stray submit() call from a
// non-multi path can't leak gating state. In SampledOnly arm mode, only
// submit() calls whose enclosing multishape == s_currentSampledShape will
// record an instance snapshot — the GPU parity SSBO still receives writes
// from every type's draw (those bytes are ignored on compare because the
// snapshot map only carries entries for the sampled actor's types).
const TG_MultiShape* s_currentSubmittingMulti = nullptr;

// Slice 2 (Stage 2.D.3) — late-registration EVENT counters split by
// allowlist disposition. Incremented in the late-reg branch of
// submitMultiShape() per occurrence (NOT once-per-type — the dedup
// printf logic is unchanged, but every event ticks one of these so the
// parity 600-frame summary surfaces actual frequency).
//
//   s_lateReg_allowed_events:    nodeId IS in objbatcher_late_register_allowlist.txt
//   s_lateReg_disallowed_events: nodeId is NOT (or empty)
//
// Read by GpuStaticPropBatcher::getAllowedLateRegEventCount() and
// getDisallowedLateRegEventCount(); consumed by gos_object_parity::ParityFrameTick.
uint64_t s_lateReg_allowed_events    = 0;
uint64_t s_lateReg_disallowed_events = 0;

inline int popIndex(GpuStaticPropPopulation pop) {
    return static_cast<int>(pop);
}

// Forward declaration; defined below.
void accumulateMonotonicAndMaybeEmit(bool forceEmit);

void emitFinalSummaryAtExit() {
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/true);
}

inline void initTraceOnce() {
    if (!s_objbatcherTraceInit) {
        s_objbatcherTrace     = (getenv("MC2_OBJBATCHER_TRACE") != nullptr);
        s_objbatcherTraceInit = true;
    }
    if (!s_atexitRegistered) {
        s_atexitRegistered = true;
        atexit(emitFinalSummaryAtExit);
    }
}

void accumulateMonotonicAndMaybeEmit(bool forceEmit) {
    s_counters.frame_count++;
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.mono_eligible_actors_by_pop[p]     += s_counters.eligible_actors_by_pop[p];
        s_counters.mono_submitted_instances_by_pop[p] += s_counters.submitted_instances_by_pop[p];
        s_counters.mono_cpu_fallback_by_pop[p]        += s_counters.cpu_fallback_by_pop[p];
    }
    s_counters.mono_submitted_children  += s_counters.submitted_children;
    s_counters.mono_skipped_children    += s_counters.skipped_children;
    s_counters.mono_gpu_drawn_instances += s_counters.gpu_drawn_instances;

    const bool periodic = (s_counters.frame_count % 600 == 0
                           && s_counters.frame_count > 0);
    if (s_objbatcherTrace || periodic || forceEmit) {
        // Slice-1 fallback rate uses ONLY Building+Tree+Generic
        // populations (Legacy excluded — it's the prior killswitch path).
        uint64_t slice1_eligible = 0, slice1_fallback = 0;
        for (int p = 0; p <= 2; ++p) {  // Building, Tree, Generic
            slice1_eligible += s_counters.mono_eligible_actors_by_pop[p];
            slice1_fallback += s_counters.mono_cpu_fallback_by_pop[p];
        }
        const double fb_rate = (slice1_eligible > 0)
            ? (double)slice1_fallback / (double)slice1_eligible
            : 0.0;

        uint64_t total_submitted = 0;
        for (int p = 0; p < kPopCount; ++p) {
            total_submitted += s_counters.mono_submitted_instances_by_pop[p];
        }

        std::fprintf(stderr,
               "[OBJBATCHER v1] event=summary frames=%llu "
               "eligible_actors=%llu submitted_instances=%llu "
               "submitted_children=%llu skipped_children=%llu "
               "cpu_fallback=%llu gpu_drawn_instances=%llu "
               "fallback_rate=%.4f "
               "submit_buildings=%llu submit_trees=%llu "
               "submit_generics=%llu submit_legacy=%llu "
               "late_register_recovery_skips=%llu\n",
               (unsigned long long)s_counters.frame_count,
               (unsigned long long)slice1_eligible,
               (unsigned long long)total_submitted,
               (unsigned long long)s_counters.mono_submitted_children,
               (unsigned long long)s_counters.mono_skipped_children,
               (unsigned long long)slice1_fallback,
               (unsigned long long)s_counters.mono_gpu_drawn_instances,
               fb_rate,
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[0],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[1],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[2],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[3],
               (unsigned long long)s_late_register_recovery_skips);
        std::fflush(stderr);
    }

    // Reset per-frame counters for next frame.
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.eligible_actors_by_pop[p]     = 0;
        s_counters.submitted_instances_by_pop[p] = 0;
        s_counters.cpu_fallback_by_pop[p]        = 0;
    }
    s_counters.submitted_children  = 0;
    s_counters.skipped_children    = 0;
    s_counters.gpu_drawn_instances = 0;
}

void loadLateRegisterAllowlistOnce() {
    if (s_lateRegisterAllowlistLoaded) return;
    s_lateRegisterAllowlistLoaded = true;
    FILE* f = fopen("data/objbatcher_late_register_allowlist.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip the # comment delimiter and the trailing newline.
        char* p = line;
        while (*p && *p != '\n' && *p != '\r' && *p != '#') ++p;
        *p = '\0';
        // Trim leading whitespace.
        char* start = line;
        while (*start == ' ' || *start == '\t') ++start;
        // Trim trailing whitespace.
        char* end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (*start == '\0') continue;
        s_lateRegisterAllowlist.insert(start);
    }
    fclose(f);
}

} // namespace

GpuStaticPropBatcher& GpuStaticPropBatcher::instance() {
    static GpuStaticPropBatcher s;
    return s;
}

void GpuStaticPropBatcher::onMapLoad() {
    // Reset everything; called at every map boundary.
    s_packets.clear();
    s_types.clear();
    s_typeIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_failedTypes.clear();
    s_geometryFinalized = false;
    s_fatalRegistrationFailure = false;
    // Stage 2.D.2: re-arm the dual-emit latch for this mission so the first
    // eligible frame after map load triggers the compare.
    gos_object_parity::OnMissionLoad();
}

void GpuStaticPropBatcher::onMapUnload() {
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo); s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo); s_sharedIbo = 0; }
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    // Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO is
    // also per-map; rebuild on next finalizeGeometry.
    if (s_perTypeSsbo) { glDeleteBuffers(1, &s_perTypeSsbo); s_perTypeSsbo = 0; }
    // Ring buffers are kept across maps (sized to map's worst case -- grow on demand).
}

// ---------------------------------------------------------------------------
// Task 6: Type registration.
//
// Packet enumeration: TG_TypeMultiShape::listOfTypeShapes[] nodes are leaves
// (each is either a TG_TypeShape with geometry or a SHAPE_NODE-less bone).
// Callers iterate listOfTypeShapes in author order and call registerType() on
// each SHAPE_NODE leaf, so per-type packet order within this function only
// needs to preserve the flat listOfTypeTriangles author order.
//
// Vertex layout note: in this fork TG_TypeVertex has no UVs; UVs live on
// TG_TypeTriangle::uvdata as per-corner u0/v0/u1/v1/u2/v2. The same vertex can
// carry different UVs on different triangles, so we cannot emit one shared
// vertex per TG_TypeVertex and share it across triangles with an index buffer.
// We expand each triangle to 3 fresh vertices (triangle-soup) and emit a
// trivial 0..N*3-1 index buffer. baseVertex points at the start of this
// type's vertex run in the shared VBO. Packet indexCount = runTris * 3.
// ---------------------------------------------------------------------------
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape, TG_TypeMultiShape* multiShape) {
    // multiShape parameter introduced by 5327c4b API cleanup for upcoming
    // alpha-test self-awareness work (Path 4 — texture-name-based material
    // classification at register time). Spec:
    // docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md
    // The header signature was committed in 5327c4b without the matching .cpp
    // signature update, leaving the build broken. This commit closes the gap
    // — multiShape is captured here but not yet consumed; the consumption
    // logic lands in the follow-up alpha-test self-awareness slice.
    (void)multiShape;
    if (!typeShape) return;
    if (s_typeIndex.count(typeShape)) return;  // idempotent
    if (s_geometryFinalized) {
        // Layer B: register-after-finalize is a bug in the map-load walk.
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] late registerType for %p -- "
                         "CPU-fallback for this type\n", (void*)typeShape);
            s_failedTypes[typeShape] = true;
        }
        return;
    }

    const uint32_t numTris = typeShape->numTypeTriangles;
    if (numTris == 0 || !typeShape->listOfTypeTriangles ||
        !typeShape->listOfTypeVertices) {
        // Empty / helper node -- register with zero packets so duplicate
        // calls remain idempotent.
        GpuStaticPropType emptyType{};
        emptyType.firstPacket = static_cast<uint32_t>(s_packets.size());
        emptyType.packetCount = 0;
        emptyType.vertexCount = 0;
        emptyType.source      = typeShape;
        s_typeIndex[typeShape] = static_cast<uint32_t>(s_types.size());
        s_types.push_back(emptyType);
        return;
    }

    const uint32_t baseVertex = static_cast<uint32_t>(s_stagingVbo.size() / kVertexStride);
    const uint32_t newTypeID  = static_cast<uint32_t>(s_types.size());

    // Group triangles with the same localTextureHandle into contiguous packets,
    // preserving authored listOfTypeTriangles order. Each packet emits 3
    // vertices per triangle into s_stagingVbo and 3 consecutive indices into
    // s_stagingIbo (triangle-soup -- see vertex layout note above).
    uint32_t runStart = 0;
    uint32_t packetCountForThisType = 0;
    while (runStart < numTris) {
        const DWORD runTextureIdx =
            typeShape->listOfTypeTriangles[runStart].localTextureHandle;
        uint32_t runEnd = runStart;
        while (runEnd < numTris &&
               typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTextureIdx) {
            ++runEnd;
        }

        const uint32_t packetFirstIndex = static_cast<uint32_t>(s_stagingIbo.size());

        for (uint32_t t = runStart; t < runEnd; ++t) {
            const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];

            const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
            const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

            for (int c = 0; c < 3; ++c) {
                const uint32_t localVertIdx = tri.Vertices[c];
                // localVertIdx is an index into listOfTypeVertices for the
                // source TG_TypeVertex; we still pass it through to the shader
                // as a_localVertexID for per-instance color indexing.
                const TG_TypeVertex& src = typeShape->listOfTypeVertices[localVertIdx];

                uint8_t vert[kVertexStride] = {};
                std::memcpy(vert +  0, &src.position.x, 4);
                std::memcpy(vert +  4, &src.position.y, 4);
                std::memcpy(vert +  8, &src.position.z, 4);
                std::memcpy(vert + 12, &src.normal.x,   4);
                std::memcpy(vert + 16, &src.normal.y,   4);
                std::memcpy(vert + 20, &src.normal.z,   4);
                std::memcpy(vert + 24, &cornerU[c],     4);
                std::memcpy(vert + 28, &cornerV[c],     4);
                std::memcpy(vert + 32, &localVertIdx,   4);
                // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight
                // tag at VBO offset 36 (was zero-filled in Stage 2.A's substrate).
                // Source is TG_TypeVertex::aRGBLight at mclib/tgl.h:54 — the per-
                // type vertex hot-color tag (e.g. 0xffff00ff = "lit window at
                // night", 0xffffff00 = "outside building light"). lighting.hglsl
                // get_base_light() decodes the magic via its expected B,G,R,A
                // byte order on little-endian x86, matching memory/mc2_argb_packing.md.
                // Raw DWORD memcpy preserves the exact bit pattern.
                std::memcpy(vert + 36, &src.aRGBLight,  4);
                s_stagingVbo.insert(s_stagingVbo.end(), vert, vert + kVertexStride);

                const uint32_t expandedIdx =
                    static_cast<uint32_t>((s_stagingVbo.size() / kVertexStride) -
                                          1 - baseVertex);
                s_stagingIbo.push_back(expandedIdx);
            }
        }

        GpuStaticPropPacket pkt{};
        pkt.firstIndex    = packetFirstIndex;
        pkt.indexCount    = (runEnd - runStart) * 3;
        pkt.baseVertex    = static_cast<int32_t>(baseVertex);
        // Store the texture slot index, not the handle. MC2 updates the
        // handle each frame via SetTextureHandle; resolving it at draw
        // time picks up the current value instead of a stale snapshot.
        pkt.textureSlot   = runTextureIdx;
        // alphaTestOn captures shape-level alpha test (trees, via SetAlphaTest).
        // textureAlpha per-slot is resolved at draw time (after bdactor.cpp init completes).
        pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
        pkt.owningTypeID  = newTypeID;
        s_packets.push_back(pkt);
        ++packetCountForThisType;

        runStart = runEnd;
    }

    const uint32_t numVerts = typeShape->numTypeVertices;

    GpuStaticPropType type{};
    type.firstPacket = static_cast<uint32_t>(s_packets.size()) - packetCountForThisType;
    type.packetCount = packetCountForThisType;
    type.vertexCount = numVerts;
    type.source      = typeShape;

    s_typeIndex[typeShape] = newTypeID;
    s_types.push_back(type);
}

void GpuStaticPropBatcher::registerMultiShape(TG_TypeMultiShape* multiShape) {
    if (!multiShape) return;
    const long n       = multiShape->GetNumShapes();
    const long numTxms = multiShape->GetNumTextures();
    for (long i = 0; i < n; ++i) {
        TG_TypeNodePtr node = multiShape->GetTypeNode(i);
        if (node && node->GetNodeType() == SHAPE_NODE) {
            TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(node);
            registerType(typeShape, multiShape);
            // GPU-offloaded actors bypass TransformMultiShape, so the leaf
            // TG_TypeShape::listOfTextures[j].gosTextureHandle is never set by
            // TMS (msl.cpp:1380). Prime it now from the multi-type's
            // mcTextureNodeIndex — the same value TMS would use. This ensures
            // flush()'s draw-time resolve picks up the correct (upscaled) handle
            // instead of the 0xffffffff left by TG_TypeShape::init().
            for (long j = 0; j < numTxms; ++j) {
                const DWORD nodeIdx = multiShape->GetTextureHandle(j);
                typeShape->SetTextureHandle(j, nodeIdx);
                // Session-wide cap on registration-time TEX_HANDOFF prints. Without
                // this cap the trace fires ~10K times during map load (one per
                // multiShape × numTxms<4) which floods stdout and can mask later
                // diagnostics. Matches the flush-time per-type cap of 8 at line
                // ~1573 (`s_traceCount < 8`). Counter is function-local static
                // so it resets per process lifetime.
                static int s_texHandoffRegPrinted = 0;
                if (s_texHandoffTrace && j < 4 && s_texHandoffRegPrinted < 8) {
                    const DWORD gosH = (typeShape->listOfTextures && j < typeShape->numTextures)
                                       ? typeShape->listOfTextures[j].gosTextureHandle
                                       : 0xdeadbeef;
                    TEX_HANDOFF("register multiShape=%p leaf=%p slot=%ld nodeIdx=0x%08x gosHandle=0x%08x",
                                (void*)multiShape, (void*)typeShape, j, nodeIdx, gosH);
                    ++s_texHandoffRegPrinted;
                }
            }
        }
    }
}

void GpuStaticPropBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;

    // Compile shader programs NOW, at map-load time, while we're on the
    // same code path that compiles every other engine shader. Doing it
    // from inside a mid-render submit() triggers a crash somewhere inside
    // shader_builder — possibly related to the shadow_screen compile
    // failure also seen at map load. Mid-render compile is not a pattern
    // this engine is tested for, so we hoist it here.
    loadProgramsIfNeeded();

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingVbo.size()),
                    s_stagingVbo.data(),
                    0);  // flags=0 -> fully immutable, GPU-only (AMD-safe)

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(),
                    0);

    // Vertex attribute layout -- position MUST be location 0 (AMD invariant 1).
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)24);
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)32);
    // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight tag at
    // offset 36, written by registerType from TG_TypeVertex::aRGBLight.
    // Consumed by static_prop.vert -> get_base_light() to decode the
    // hot-color magic and emit per-vertex base lighting before calc_light.
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)36);

    glBindVertexArray(0);

    // Slice 2 (object-offload) — Stage 2.C.2: build per-type hot-color SSBO.
    // 3 vec4 per type, 48 B per type. Decoded from TG_TypeShape::hotPinkRGB
    // (DWORD ARGB) into vec3 + 0 padding (std430 vec4 alignment). Indexed
    // in the shader by inst.typeID — entry 0 corresponds to s_types[0].
    {
        struct PerTypeShaderData {
            float hotPinkRGB[4];
            float hotYellowRGB[4];
            float hotGreenRGB[4];
        };
        static_assert(sizeof(PerTypeShaderData) == 48, "PerTypeShaderData layout must be 48 bytes (3 × vec4 std430)");

        std::vector<PerTypeShaderData> perTypeBlob(s_types.size());
        auto unpack = [](DWORD argb, float out[4]) {
            // Stored as 0xAARRGGBB on the C++ side (matching DWORD convention
            // in tgl.cpp:1799-1801 where the per-vertex hot-color decode
            // pulls (argb>>16) & 0xff = R). Match that.
            out[0] = ((argb >> 16) & 0xFF) / 255.0f;  // R
            out[1] = ((argb >>  8) & 0xFF) / 255.0f;  // G
            out[2] = ((argb >>  0) & 0xFF) / 255.0f;  // B
            out[3] = 0.0f;
        };
        for (size_t i = 0; i < s_types.size(); ++i) {
            const TG_TypeShape* ts = s_types[i].source;
            DWORD pink = 0, yellow = 0, green = 0;
            if (ts) {
                pink   = ts->hotPinkRGB;
                yellow = ts->hotYellowRGB;
                green  = ts->hotGreenRGB;
            }
            unpack(pink,   perTypeBlob[i].hotPinkRGB);
            unpack(yellow, perTypeBlob[i].hotYellowRGB);
            unpack(green,  perTypeBlob[i].hotGreenRGB);
        }

        glGenBuffers(1, &s_perTypeSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perTypeSsbo);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                        static_cast<GLsizeiptr>(perTypeBlob.size() * sizeof(PerTypeShaderData)),
                        perTypeBlob.data(),
                        0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Free CPU staging.
    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    std::fprintf(stderr, "[GPUPROPS] finalize: %zu types, %zu packets\n",
                 s_types.size(), s_packets.size());

    s_geometryFinalized = true;
}

bool GpuStaticPropBatcher::submit(TG_Shape* shape,
                                  const Stuff::Matrix4D& shapeToWorld,
                                  uint32_t highlightARGB,
                                  uint32_t fogARGB,
                                  uint32_t flags,
                                  uint32_t lightDataIndex) {
    if (!shape || s_fatalRegistrationFailure) return false;
    if (s_programLoadFailed) return false;

    // TG_Shape::myType is a TG_TypeNodePtr; for SHAPE_NODE leaves it's a TG_TypeShape.
    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) {
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] unregistered type %p for shape %p -- "
                         "caller must CPU-fallback\n", (void*)typeShape, (void*)shape);
            s_failedTypes[typeShape] = true;
        }
        return false;  // Layer B: caller calls shape->Render() on false.
    }

    const uint32_t typeID = it->second;
    const GpuStaticPropType& type = s_types[typeID];
    PerTypeBucket& bucket = s_bucketsByType[typeID];

    // firstColorOffset is the index into the bucket's color array:
    // instance K's colors start at K * type.vertexCount (= bucket.colors.size()
    // BEFORE this push). The shader binds the bucket's color range, so this
    // becomes an index relative to the bound range.
    const uint32_t firstColorOffset =
        static_cast<uint32_t>(bucket.colors.size());

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = typeID;
    inst.firstColorOffset = firstColorOffset;
    inst.flags            = flags;
    // Slice 2 (object-offload) — Stage 2.C: per-actor dedup-cache index
    // into the LightsData[32] UBO. Same value for every leaf of one
    // multishape (caller hoisted the gather between the two loops).
    inst.lightDataIndex   = lightDataIndex;
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;
    {
        // MC2_TREE_DIAG_TRACE diagnostic. See macro definition near line ~50.
        static int s_dynPrinted = 0;
        if (s_treeDiagTrace && s_dynPrinted < 8) {
            ++s_dynPrinted;
            TREE_DIAG("path=dyn typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                inst.typeID, inst.flags, inst.lightDataIndex, inst.firstColorOffset,
                // Stuff::Matrix4D translation lives at (3,0)/(3,1)/(3,2) which the
                // operator()(row,col) maps to entries[(col<<2)+row] = [3]/[7]/[11].
                // The W-column is at [12]/[13]/[14] which is always (0,0,0) for
                // affine matrices — wrong field. Verified via mclib/stuff/matrix.cpp:214 BuildTranslation.
                inst.modelMatrix[3], inst.modelMatrix[7], inst.modelMatrix[11],
                inst.fogRGB[0], inst.fogRGB[1], inst.fogRGB[2], inst.fogRGB[3],
                inst.aRGBHighlight[0], inst.aRGBHighlight[1], inst.aRGBHighlight[2], inst.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    bucket.instances.push_back(inst);
    s_lastBuiltBatch.push_back(inst);  // Stage 3.C: batch accumulator

    // Stage 2.D.2 — dual-emit snapshot collection.
    // When the latch is Armed, capture per-vertex lit ARGB in triangle-soup
    // expanded order (matching gl_VertexID in static_prop.vert). The shader
    // writes parityOut_[gl_InstanceID * parityVerts + gl_VertexID] where
    // gl_VertexID is the expanded VBO index (0 .. numTris*3-1). We build the
    // snapshot in the same order: corner (j,c) → index 3*j+c.
    // Note: typeShape->numTypeTriangles is the TOTAL triangle count across all
    // packets (same as the total triangles emitted into the VBO by registerType).
    //
    // Source: listOfVertices[typeTriangles[j].Vertices[c]].argb — NOT
    //   listOfTriangles[j].aRGBLight[c]. The face-loop only writes
    //   aRGBLight[c] for FRONT-FACING triangles; back-facing positions
    //   retain stale pool memory from previous frames (not zeroed on alloc).
    //   listOfVertices[v].argb is always written by the vertex lighting loop
    //   for every vertex (front AND back facing), making it the correct
    //   per-vertex ground truth. Both values are identical for front-facing
    //   vertices when useFaceLighting=false (stock mc2_01 condition).
    // Stage 2.D.3: per-actor narrowing of snapshot capture. The mclib gate
    // sites only fire the full TransformMultiShape (CPU lighting bake) for
    // the sampled actor in SampledOnly arm mode — non-sampled actors have
    // stale listOfVertices[].argb. Capturing their bytes would corrupt the
    // snapshot map. Use the per-actor gate here too: when the enclosing
    // multishape (s_currentSubmittingMulti, set by submitMultiShape) doesn't
    // match the sampler's pick, skip the snapshot. In bootstrap arm (state=
    // Armed + armMode=All) IsDualEmitArmedForActor returns true for every
    // shape, so 2.D.2 baseline coverage is preserved.
    if (gos_object_parity::IsDualEmitArmedForActor(s_currentSubmittingMulti) &&
        shape->listOfVertices &&
        typeShape->listOfTypeTriangles &&
        typeShape->numTypeTriangles > 0) {
        const DWORD numTris = typeShape->numTypeTriangles;
        const uint32_t expandedVerts = numTris * 3u;
        std::vector<uint32_t> perVertexARGB(expandedVerts, 0u);
        // Stage 2.D.2 diagnostic: one-shot dump of original aRGBLight tags vs
        // computed listOfVertices[vi].argb for first instance of typeID=82.
        // Controlled by MC2_OBJECT_PARITY_TRACE env var.
        static bool s_parity82Printed = false;
        const bool doTrace82 = (typeID == 82 && !s_parity82Printed &&
                                gos_object_parity::IsParityTraceEnabled());
        if (doTrace82) s_parity82Printed = true;
        for (DWORD j = 0; j < numTris; ++j) {
            for (int c = 0; c < 3; ++c) {
                const DWORD vi = typeShape->listOfTypeTriangles[j].Vertices[c];
                perVertexARGB[j * 3u + c] = shape->listOfVertices[vi].argb;
                if (doTrace82 && (j * 3u + c) < 20u) {
                    std::fprintf(stderr,
                        "[PARITY_DIAG v1] typeId=82 inst=first vert=%u "
                        "aRGBLight_tag=0x%08X computed_argb=0x%08X\n",
                        (unsigned)(j * 3u + c),
                        (unsigned)typeShape->listOfTypeVertices[vi].aRGBLight,
                        (unsigned)shape->listOfVertices[vi].argb);
                    std::fflush(stderr);
                }
            }
        }
        // Stage 2.D.3: pass explicit instanceIdx = bucket position this leaf
        // just took. bucket.instances.push_back ran above, so size()-1 is
        // the just-pushed entry's index. Required so SampledOnly mode can
        // address the correct GPU slot in CompareAndReport (the sampled
        // leaf might not be at instance 0 — there are typically many
        // unrelated submissions of the same type per frame).
        const uint32_t instanceIdx =
            static_cast<uint32_t>(bucket.instances.size() - 1u);
        gos_object_parity::RecordInstanceSnapshot(typeID,
                                                   instanceIdx,
                                                   perVertexARGB.data(),
                                                   expandedVerts);
    }

    // Append this instance's per-vertex ARGB block.
    // IMPORTANT: listOfColors (TG_Vertex: fog+redSpec+greenSpec+blueSpec) is
    // specular-only and is zero for most buildings — reading it produces
    // black. The real per-vertex lit ARGB is computed by TransformShape
    // into `listOfVertices[j].argb` (mclib/tgl.cpp:2119). gos_VERTEX is
    // 32 bytes with `argb` at offset 16.
    const uint32_t numColors = type.vertexCount;
    if (numColors > 0 && shape->listOfVertices) {
        bucket.colors.reserve(bucket.colors.size() + numColors);
        const gos_VERTEX* src = shape->listOfVertices;
        for (uint32_t v = 0; v < numColors; ++v) {
            bucket.colors.push_back(src[v].argb);
        }
    } else {
        // No source vertices -- pad with zeros so the color block still
        // matches type.vertexCount and indexing math stays valid.
        bucket.colors.insert(bucket.colors.end(), numColors, 0u);
    }

    return true;
}

void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    initTraceOnce();
    s_counters.eligible_actors_by_pop[popIndex(pop)]++;
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    s_counters.cpu_fallback_by_pop[popIndex(pop)]++;
}

bool GpuStaticPropBatcher::submitMultiShape(TG_MultiShape* multi,
                                            GpuStaticPropPopulation pop,
                                            const char* callerName) {
    initTraceOnce();
    s_lastBuiltBatch.clear();  // Stage 3.C: reset per-call accumulator
    // Clear the late-reg signal at the top of every call so a stale "true"
    // from a prior submitMultiShape never masquerades as a signal for this one.
    s_lastSubmitWasLateReg = false;
    // Stage 2.D.3: track the currently-submitting multishape so per-leaf
    // submit() calls can compare against the sampler's pick. Cleared at every
    // exit path of this function to keep the window tight.
    s_currentSubmittingMulti = multi;
    // pop consumed by counters below.
    if (!multi || s_fatalRegistrationFailure) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    // Skip-child-not-fail-multishape policy. The CPU path (TG_Shape::Render
    // in tgl.cpp ~2530) silently returns for any of:
    //   - helper/bone node (numVertices == 0; GetNodeType() != SHAPE_NODE)
    //   - listOfVertices == NULL (TransformShape early-out for
    //       isSpotlight && !isNight, tgl.cpp ~1657)
    //   - listOfColors == NULL (shape not yet transformed this frame)
    //   - lastTurnTransformed != turn (stale)
    // Previously we failed the entire multishape on any of these, which
    // cascaded one helper/spotlight child into a full CPU fallback of every
    // building. Now we skip only the ineligible children and submit the
    // rest. Only fail the whole multishape if a SHAPE_NODE child has an
    // unregistered type — that's a plumbing problem that invalidates
    // self-consistency.

    // First pass: check for unregistered SHAPE_NODE types (fatal for this
    // multishape). All other ineligibility is handled in the submit pass.
    // Slice 2 (object-offload) — Stage 2.C: capture the first valid
    // SHAPE_NODE leaf as the per-actor anchor for the GatherGpuObjectLightDataOnly
    // call hoisted between this loop and the second submit loop. Recon
    // Section 9 Item 5 confirmed all leaves of a multishape see identical
    // lightData_, so any non-null SHAPE_NODE works as the gather receiver.
    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (firstShapeNodeLeaf == nullptr) firstShapeNodeLeaf = rec.node;
        if (s_typeIndex.find(ts) == s_typeIndex.end()) {
            loadLateRegisterAllowlistOnce();
            // Stage 2.C+ instrumentation: build a richer identifier for the
            // unregistered type. The dedup key still uses the pointer so the
            // count-once-per-type logic stays stable across name lookups,
            // but the human-readable log line includes:
            //   - caller actor's appearType->name (e.g. "house3.ase")
            //   - the TG_TypeShape's nodeId (inherited from TG_TypeNode;
            //     getNodeId() is public, returns char*; const_cast is the
            //     minimal-touch workaround for the const TG_TypeShape* we
            //     have here)
            //   - the bare pointer (still useful for cross-referencing
            //     across log files / ts identity over a process lifetime)
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "%p", (const void*)ts);
            const std::string typeKey = addrBuf;  // dedup key — pointer-stable within one run
            auto& count = s_lateRegisterCounts[typeKey];
            // Stage 2.D.3: compute `allowed` for EVERY event so the per-event
            // counters tick correctly. The PRINT (and registered_dump) below
            // remain gated by `count == 0` so the log stays one-line-per-type.
            const char* nodeIdEvt = nullptr;
            if (ts) {
                nodeIdEvt = const_cast<TG_TypeShape*>(ts)->getNodeId();
            }
            const bool allowedEvt =
                nodeIdEvt && nodeIdEvt[0] &&
                (s_lateRegisterAllowlist.find(nodeIdEvt)
                 != s_lateRegisterAllowlist.end());
            if (allowedEvt) ++s_lateReg_allowed_events;
            else            ++s_lateReg_disallowed_events;
            if (count == 0) {
                const char* nodeId = nodeIdEvt;
                // Allowlist matching uses the nodeId, NOT the pointer-key.
                // Pointers are not stable across process runs, so a pointer
                // entry in data/objbatcher_late_register_allowlist.txt is
                // useless. nodeId comes from the .ase author and is stable
                // across runs and across the same nodeId-family of types
                // (LOD variants of the same shape share a nodeId — e.g.,
                // multiple TG_TypeShape* with nodeId="Centipede" at
                // different vertexCounts; allowlisting "Centipede" matches
                // the family). Empty-name shapes cannot be allowlisted —
                // the file would have no useful key for them.
                const bool allowed = allowedEvt;
                std::fprintf(stderr,
                       "[OBJBATCHER v1] event=late_register type=%s nodeId=%s caller=%s allowed=%d\n",
                       typeKey.c_str(),
                       (nodeId && nodeId[0]) ? nodeId : "<unnamed>",
                       (callerName && callerName[0]) ? callerName : "<unknown>",
                       allowed ? 1 : 0);
                std::fflush(stderr);

                // Stage 2.C+ instrumentation: one-shot registered-types
                // dump. Emitted on the FIRST late-register event of the run
                // only (s_emittedRegisteredDump is file-static, set true
                // here). Lists the registered TG_TypeShape pointers AND
                // their nodeIds so an operator can grep this output to find
                // "is the late-reg one in the same family as registered
                // ones, just with a different nodeId variant?" or "is the
                // late-reg type entirely absent from the s_typeIndex, vs.
                // registered as a peer?". Cap at 30 to keep the log
                // tractable; if more types are registered, the count is
                // shown so the truncation is visible.
                static bool s_emittedRegisteredDump = false;
                if (!s_emittedRegisteredDump) {
                    s_emittedRegisteredDump = true;
                    const size_t total = s_types.size();
                    const size_t cap = total < 30 ? total : 30;
                    std::fprintf(stderr,
                        "[GPUPROPS_REG] event=registered_dump total=%zu shown=%zu\n",
                        total, cap);
                    for (size_t k = 0; k < cap; ++k) {
                        const TG_TypeShape* rt = s_types[k].source;
                        const char* rnode = nullptr;
                        if (rt) rnode = const_cast<TG_TypeShape*>(rt)->getNodeId();
                        std::fprintf(stderr,
                            "[GPUPROPS_REG] entry typeID=%zu ptr=%p nodeId=%s vertexCount=%u\n",
                            k, (const void*)rt,
                            (rnode && rnode[0]) ? rnode : "<unnamed>",
                            s_types[k].vertexCount);
                    }
                    std::fflush(stderr);
                }
            }
            ++count;
            // Stage 2.A: signal the caller that this failure was specifically
            // a late-registration miss (not a fatal GPU error). Caller (Stage
            // 2.B) queries wasLastFailureLateRegistration() and sets
            // needsFullBakeNextFrame=true on the owning actor.
            s_lastSubmitWasLateReg = true;
            ++s_late_register_recovery_skips;
            s_currentSubmittingMulti = nullptr;
            return false;
        }
    }

    // Slice 2 (object-offload) — Stage 2.C: per-actor light-data gather.
    // Stage 2.D.2 fix: use cachedGpuLightIndex_ if it was pre-gathered during
    // update() (while worldLights[0]->aRGB was per-actor-correct).
    // Fallback to GatherGpuObjectLightDataOnly() here only when the cache is
    // invalid (0xFFFFFFFF sentinel = not yet cached, e.g. first frame).
    //
    // Root cause of the timing bug this fixes:
    //   BldgAppearance::update() sets worldLights[0]->aRGB = terrainLight(pos).
    //   GatherGpuObjectLightDataOnly() called here (during renderLists()) reads
    //   worldLights[0]->aRGB AFTER all actors have updated — by then later
    //   actors have overwritten it with their own terrain-position-scaled values.
    //   The last-actor's value (often white at high-elevation positions) is what
    //   the GPU UBO receives, while the CPU vertex loop used the per-building
    //   terrain-scaled value during its own update(). This causes the hot-green
    //   (daytime base=0) vertex mismatch: GPU applies white directional light,
    //   CPU applied the terrain-tinted (~0.88) directional light.
    uint32_t lightDataIndex = 0;
    if (multi->cachedGpuLightIndex_ != 0xFFFFFFFFu) {
        // Happy path: lights were gathered at update() time (correct per-actor).
        lightDataIndex = multi->cachedGpuLightIndex_;
    } else if (firstShapeNodeLeaf != nullptr) {
        // Fallback: first frame or non-GPU-object path cached nothing.
        lightDataIndex = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
    }

    // Second pass: submit each eligible child, silently skipping those the
    // CPU path would also skip (helpers, untransformed, daytime spotlights).
    for (int i = 0; i < n; ++i) {
        TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) { s_counters.skipped_children++; continue; }
        TG_Shape* child = rec.node;
        if (!child->myType) { s_counters.skipped_children++; continue; }
        // Helper/bone nodes (non-SHAPE). CPU Render silently returns because
        // numVertices == 0. Do the same — no instance to emit.
        if (child->myType->GetNodeType() != SHAPE_NODE) {
            s_counters.skipped_children++;
            continue;
        }
        // Spotlights during day + other early-outs in TransformShape leave
        // listOfVertices NULL. CPU Render early-outs on the same condition.
        // Also listOfColors NULL: CPU also early-outs. Submit's zero-pad
        // path would render this child black, which is the bug we're
        // avoiding — so skip here.
        if (!child->listOfVertices || !child->listOfColors) {
            s_counters.skipped_children++;
            continue;
        }

        uint32_t flags = 0;
        if (child->lightsOut)   flags |= (1u << 0);
        if (child->isWindow)    flags |= (1u << 1);
        if (child->isSpotlight) flags |= (1u << 2);

        // rec.shapeToWorld is LinearMatrix4D; convert to Matrix4D for submit().
        Stuff::Matrix4D xform(rec.shapeToWorld);
        if (!submit(child, xform,
                    child->aRGBHighlight, child->fogRGB, flags,
                    lightDataIndex)) {
            // submit() rejected after we passed the registration gate —
            // typically a buffer-full condition. Fall back for this frame
            // to keep the visual self-consistent.
            s_currentSubmittingMulti = nullptr;
            return false;
        }
        s_counters.submitted_children++;
    }
    s_counters.submitted_instances_by_pop[popIndex(pop)]++;
    // Stage 2.D.3 — sampler observation. Record the multishape pointer in
    // the parity sidecar's per-frame observation list. Default-off short-
    // circuits internally when MC2_OBJECT_PARITY_CHECK is unset; safe to
    // call unconditionally. Only successful submissions reach here, so
    // late-registered actors (skybox/compass) are structurally excluded
    // from the sampler pool — matching the "registered types only"
    // contract from the dispatch.
    gos_object_parity::ObserveSubmittedShape(multi);
    s_currentSubmittingMulti = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// Task 10: flush() — per-packet instanced draw
// ---------------------------------------------------------------------------
namespace {

// Per-frame upload state shared between flushShadow() (Task 13) and flush().
// Whichever runs first this frame owns the upload; the other skips it when
// s_lastUploadedSlot == s_frameSlot.
std::unordered_map<uint32_t, TypeRangeSsbo> s_typeRanges;
uint32_t s_lastUploadedSlot = 0xFFFFFFFFu;

bool uploadAllBucketsIfNeeded() {
    if (s_lastUploadedSlot == s_frameSlot) return true;

    if (s_bucketsByType.empty()) return false;

    loadProgramsIfNeeded();
    if (s_fatalRegistrationFailure) return false;

    // SSBO offset alignment must be queried to size the ring correctly.
    // glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ..., offset, size) requires
    // offset % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0 (minimum 256).
    // Each per-type range starts at an aligned offset, which wastes up to
    // (alignment - 1) bytes per bucket. The CAPACITY request must include
    // that slack or we overrun the mapped buffer on zoom-out (more buckets
    // active -> more padding overhead).
    static GLint s_ssboAlignment = 0;
    if (s_ssboAlignment == 0) {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &s_ssboAlignment);
        if (s_ssboAlignment < 16) s_ssboAlignment = 256;  // sane fallback
    }
    auto alignUp = [](size_t v, size_t a) {
        return (v + (a - 1)) & ~(a - 1);
    };

    // Compute EXACT total byte usage with per-bucket alignment padding.
    size_t instBytesNeeded = 0;
    size_t colBytesNeeded  = 0;
    for (auto& kv : s_bucketsByType) {
        instBytesNeeded = alignUp(instBytesNeeded, (size_t)s_ssboAlignment);
        colBytesNeeded  = alignUp(colBytesNeeded,  (size_t)s_ssboAlignment);
        instBytesNeeded += kv.second.instances.size() * sizeof(GpuStaticPropInstance);
        colBytesNeeded  += kv.second.colors.size() * sizeof(uint32_t);
    }
    if (instBytesNeeded == 0) return false;

    // Convert back to element counts (ceil) for ensureRingCapacity, which is
    // element-based. Round up so subsequent ring-indexing in bytes fits.
    const size_t instCountNeeded =
        (instBytesNeeded + sizeof(GpuStaticPropInstance) - 1) / sizeof(GpuStaticPropInstance);
    const size_t colCountNeeded =
        (colBytesNeeded + sizeof(uint32_t) - 1) / sizeof(uint32_t);

    ensureRingCapacity(instCountNeeded, colCountNeeded);
    if (s_fatalRegistrationFailure) return false;

    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    const size_t slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof(GpuStaticPropInstance);
    const size_t slotColByteBase  = s_frameSlot * s_colorCapacity    * sizeof(uint32_t);
    auto* instMapBase = static_cast<uint8_t*>(s_instanceMap) + slotInstByteBase;
    auto* colMapBase  = static_cast<uint8_t*>(s_colorMap)    + slotColByteBase;

    // Deterministic ascending typeID iteration — makes Tracy / RenderDoc
    // diffs stable and shader-debug repro repeatable across runs.
    std::vector<uint32_t> sortedTypeIDs;
    sortedTypeIDs.reserve(s_bucketsByType.size());
    for (auto& kv : s_bucketsByType) sortedTypeIDs.push_back(kv.first);
    std::sort(sortedTypeIDs.begin(), sortedTypeIDs.end());

    s_typeRanges.clear();
    size_t instCursor = 0;
    size_t colCursor  = 0;
    for (uint32_t typeID : sortedTypeIDs) {
        PerTypeBucket& b = s_bucketsByType[typeID];

        // Align the start of each per-type region to the SSBO alignment
        // requirement before writing.
        instCursor = alignUp(instCursor, static_cast<size_t>(s_ssboAlignment));
        colCursor  = alignUp(colCursor,  static_cast<size_t>(s_ssboAlignment));

        TypeRangeSsbo r{};
        r.instanceByteOffset = slotInstByteBase + instCursor;
        r.instanceByteSize   = b.instances.size() * sizeof(GpuStaticPropInstance);
        r.colorByteOffset    = slotColByteBase  + colCursor;
        r.colorByteSize      = b.colors.size() * sizeof(uint32_t);
        r.instanceCount      = static_cast<uint32_t>(b.instances.size());

        if (r.instanceByteSize)
            std::memcpy(instMapBase + instCursor, b.instances.data(), r.instanceByteSize);
        if (r.colorByteSize)
            std::memcpy(colMapBase  + colCursor,  b.colors.data(),    r.colorByteSize);

        instCursor += r.instanceByteSize;
        colCursor  += r.colorByteSize;
        s_typeRanges[typeID] = r;
    }

    s_lastUploadedSlot = s_frameSlot;
    return true;
}

} // namespace

// [RENDER_CONTRACT:Pass=StaticProp id=GpuStaticPropBatcher_flush]
//   Routes through static_prop.frag which writes
//   rc_gbuffer1_screenShadowEligible (production) or
//   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug).
void GpuStaticPropBatcher::flush() {
    ZoneScopedN("GpuStaticProps.Flush");
    initTraceOnce();

    if (!s_geometryFinalized || s_fatalRegistrationFailure) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        return;
    }
    if (!uploadAllBucketsIfNeeded()) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }
    // Program compile/link latch. submitMultiShape already gates submissions
    // on this, so reaching here with an empty program is a logic bug — but
    // guard anyway so we never pump uniform calls against a null program.
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_bucketsByType.clear();
        s_lastUploadedSlot = 0xFFFFFFFFu;
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }

    // Slice 2 (object-offload) — Stage 2.D.1: parity readback handshake.
    //
    // uploadAllBucketsIfNeeded() above just glClientWaitSync'd on
    // s_fence[s_frameSlot] (the fence from RING_FRAMES frames ago). At
    // this point the parity SSBO writes from that prior cycle are
    // GPU-complete and safe to read on the CPU.
    //
    // 2.D.1 acceptance is "the readback runs without crash or stall" — we
    // glGetBufferSubData the live byte range from this slot into a discard
    // scratch buffer and DROP the bytes. 2.D.2/2.D.3 will swap the discard
    // for a CPU recompute compare.
    //
    // Skips entirely when MC2_OBJECT_PARITY_CHECK is unset
    // (IsParityCheckEnabled() returns false → EnsureParityOutputSSBO()
    // returns 0 → the bind/uniform/readback all short-circuit). This is
    // the load-bearing default-off contract per the advisor.
    GLuint parityBuffer = 0;
    if (gos_object_parity::IsParityCheckEnabled()) {
        parityBuffer = gos_object_parity::EnsureParityOutputSSBO(kParitySlotBytes);
        if (parityBuffer != 0) {
            const size_t bytesToRead = s_parityBytesUsedPerSlot[s_frameSlot];
            if (bytesToRead > 0) {
                if (s_parityReadbackScratch.size() < bytesToRead) {
                    s_parityReadbackScratch.resize(bytesToRead);
                }
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   slotBase,
                                   static_cast<GLsizeiptr>(bytesToRead),
                                   s_parityReadbackScratch.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                gos_object_parity::Counters_AddReadbackBytesThisFrame(
                    static_cast<uint64_t>(bytesToRead));
                // Stage 2.D.2: if the latch is WaitingForReadback, compare
                // the GPU bytes against the CPU snapshot captured on frame N.
                // After compare, advance latch to Done and free the snapshot.
                // The fence wait above (glClientWaitSync in
                // uploadAllBucketsIfNeeded) guarantees GPU writes are complete.
                // Stage 2.D.2: compare only when the ring has returned to the
                // exact slot the Armed frame wrote to (IsDualEmitReadyForSlot
                // checks both state==WaitingForReadback AND slot match).
                if (gos_object_parity::IsDualEmitReadyForSlot(s_frameSlot)) {
                    gos_object_parity::CompareAndReport(
                        s_parityReadbackScratch.data(),
                        bytesToRead,
                        static_cast<unsigned>(s_counters.frame_count));
                    gos_object_parity::AdvanceDualEmitToDone();
                }
            }
        }
        // Reset this slot's usage; it will be re-populated by the per-type
        // bind loop below.
        s_parityBytesUsedPerSlot[s_frameSlot] = 0;
    }
    s_parityBytesUsedThisFrame = 0;

    // Save ALL GL state we'll mutate so we can restore it at the end.
    // This is the defensive-save approach — the engine's MLR/HUD paths
    // under 4.3 core are fragile about inherited bindings, so we behave
    // as if every caller expects state unchanged.
    GLint prevProgram=0, prevVao=0, prevArrayBuf=0, prevElemBuf=0;
    GLint prevActiveTex=0, prevTexUnit0=0;
    // Slice 2 (object-offload) — Stage 2.C.2: also save/restore SSBO slot 2
    // (per-type hot-color SSBO; bound once for the whole flush, not per-type).
    // Stage 2.D.1: also save/restore SSBO slot 3 (parity readback harness;
    // bound only when MC2_OBJECT_PARITY_CHECK=1).
    GLint prevSsbo0=0, prevSsbo1=0, prevSsbo2=0, prevSsbo3=0;
    GLboolean prevDepthTest=GL_FALSE, prevDepthMask=GL_FALSE;
    GLboolean prevCullFace=GL_FALSE, prevBlend=GL_FALSE;
    GLint prevDepthFunc=GL_LESS, prevCullMode=GL_BACK;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &prevSsbo1);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 2, &prevSsbo2);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 3, &prevSsbo3);
    prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    prevCullFace = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    prevBlend = glIsEnabled(GL_BLEND);

    glUseProgram(s_staticPropProgram);
    glBindVertexArray(s_sharedVao);

    // Explicit state for our pass.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Direct uniforms. Static props use the same CPU-composed terrainMVP as
    // terrain/terrain_overlay.vert: axisSwap * worldToClip, row-major
    // rewritten in gamecam.cpp and uploaded GL_FALSE.
    const GLint locTerrainMVP = glGetUniformLocation(s_staticPropProgram, "terrainMVP");
    const float* terrainMVP = gos_GetTerrainMVPMat4();
    if (locTerrainMVP >= 0 && terrainMVP)
        glUniformMatrix4fv(locTerrainMVP, 1, GL_FALSE, terrainMVP);
    // Terrain projection chain matches shaders/terrain_overlay.vert usage:
    // terrainMVP gives D3D-style screen-pixel homogeneous coords, then the
    // shader does divide + viewport + pixel->NDC with abs(w).
    const GLint locVP  = glGetUniformLocation(s_staticPropProgram, "u_terrainViewport");
    const float* vp = gos_GetTerrainViewportVec4();
    if (locVP >= 0 && vp) glUniform4fv(locVP, 1, vp);
    const GLint locMVP = glGetUniformLocation(s_staticPropProgram, "u_mvp");
    const float* mm = gos_GetProj2ScreenMat4();
    if (locMVP >= 0 && mm) glUniformMatrix4fv(locMVP, 1, GL_TRUE, mm);
    glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_tex"),           0);
    glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode"), debugAddrMode_);
    // FIXME(task-10): no clean per-scene global fog scalar source available
    // for static props; per-instance fog color is already on v_fog. 1.0 ==
    // "clear" per shader convention (matches gos_tex_vertex.frag non-overlay
    // convention). Revisit if distance fog needs to attenuate props.
    glUniform1f(glGetUniformLocation(s_staticPropProgram, "u_fogValue"),      1.0f);

    // Slice 2 (object-offload) — Stage 2.C.2: bind per-type hot-color SSBO
    // once for the whole flush. The data is per-map immutable so a single
    // bind covers every typeID in the per-type loop below — static_prop.vert
    // indexes by inst.typeID. Restored at the bottom alongside slots 0+1.
    if (s_perTypeSsbo) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    }

    // Slice 2 (object-offload) — Stage 2.D.1: parity write defaults OFF.
    // Per-type loop below flips it to 1 only for draws whose bound
    // parity-buffer range fits in this slot's remaining byte budget.
    // When MC2_OBJECT_PARITY_CHECK is unset, parityBuffer == 0 and the
    // uniform stays 0 for every draw (no shader writes happen).
    // Stage 2.D.1.1 (Item 2): use link-time cached locations instead of
    // per-flush glGetUniformLocation (one driver round-trip per flush saved).
    const GLint locParityWrite      = s_loc_u_parityWrite;
    const GLint locParityVerts      = s_loc_u_parityVertsPerType;
    const GLint locParityBaseVertex = s_loc_u_parityBaseVertex;
    if (locParityWrite >= 0) glUniform1i(locParityWrite, 0);

    // Stage 2.D.2: zero the Armed frame's parity SSBO slot before drawing so
    // that back-facing vertices (GPU-culled by GL_CULL_FACE GL_BACK, not
    // written by the shader) read as zero on the compare frame — matching the
    // zero-initialized CPU snapshot (vector initialized to 0u in submit()).
    // Without this clear, stale uint32 values from RING_FRAMES ago remain in
    // the slot for unwritten (back-facing) positions and cause false mismatches
    // against the CPU's 0x00000000 for those same positions.
    if (parityBuffer != 0 && gos_object_parity::IsDualEmitArmedAnyActor()) {
        const GLintptr slotBase =
            static_cast<GLintptr>(s_frameSlot) *
            static_cast<GLintptr>(kParitySlotBytes);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
        const GLuint zero = 0u;
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                             slotBase,
                             static_cast<GLsizeiptr>(kParitySlotBytes),
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Per-type drawing: bind per-type instance & color SSBO ranges, then
    // issue one instanced draw per packet. gl_InstanceID in the shader
    // addresses 0..N-1 within the bound range (no gl_BaseInstance needed).
    for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];
        if (r.instanceCount == 0 || type.packetCount == 0) continue;

        // Gate F: count actors that produced ≥1 packet draw this frame.
        // Per-type increment, not per-packet — we want "actors drawn,"
        // not "draw-call count."
        s_counters.gpu_drawn_instances += r.instanceCount;

        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
                          static_cast<GLintptr>(r.instanceByteOffset),
                          static_cast<GLsizeiptr>(r.instanceByteSize));
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_colorSsbo,
                          static_cast<GLintptr>(r.colorByteOffset),
                          static_cast<GLsizeiptr>(r.colorByteSize));

        // SEMANTIC: max VALID local vertex index, not count. Lets the
        // gradient debug mode hit t=1.0 at the last vertex.
        // NOTE: shader declares u_* ints (uniform uint crashes this engine's
        // shader compile); values are always positive and < 2^31, so
        // signed int is lossless here. Upload via glUniform1i.
        const int maxID = (type.vertexCount > 0u) ? (int)(type.vertexCount - 1u) : 0;
        glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID"), maxID);

        // Slice 2 (object-offload) — Stage 2.D.1: per-type parity bind.
        //
        // The shader writes parityOut[gl_InstanceID * vertsPerType + gl_VertexID].
        // We bind a tight glBindBufferRange so that index always lands inside
        // the bound region. Each per-type draw consumes its bytes from a
        // monotonic cursor within slot s_frameSlot of the parity SSBO; if
        // the slot's budget would be exceeded by this draw, we fall back to
        // u_parityWrite=0 and the shader skips the write (the readback at
        // top of next visit then doesn't see those vertices — fine for
        // 2.D.1 acceptance, will be tightened in 2.D.2/2.D.3 if real-world
        // peak exceeds kParitySlotBytes).
        // Stage 2.D.2: the parity SSBO uses the expanded (triangle-soup) vertex
        // count so gl_VertexID in static_prop.vert stays in-bounds. gl_VertexID
        // runs 0..numTris*3-1 (per the VBO layout in registerType), so the
        // correct u_parityVertsPerType and SSBO range size is numTris*3, not
        // type.vertexCount (which is numTypeVertices, the shared-vertex count).
        const uint32_t parityVerts = (type.source && type.source->numTypeTriangles > 0u)
            ? type.source->numTypeTriangles * 3u
            : type.vertexCount;

        bool wroteParityThisDraw = false;
        if (parityBuffer != 0 && parityVerts > 0u) {
            const size_t needBytes = static_cast<size_t>(r.instanceCount) *
                                     static_cast<size_t>(parityVerts) *
                                     sizeof(uint32_t);
            // SSBO offset alignment requirement. Mirrors the per-type
            // instance/color bind alignment used by uploadAllBucketsIfNeeded
            // above (its s_ssboAlignment is function-scope, so query once
            // here too — single static, set on first hit).
            static GLint s_parityAlignment = 0;
            if (s_parityAlignment == 0) {
                glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT,
                              &s_parityAlignment);
                if (s_parityAlignment < 16) s_parityAlignment = 256;
            }
            const size_t alignMask = static_cast<size_t>(s_parityAlignment) - 1u;
            const size_t cursor = (s_parityBytesUsedThisFrame + alignMask) & ~alignMask;
            if (needBytes > 0 && cursor + needBytes <= kParitySlotBytes) {
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, parityBuffer,
                                  slotBase + static_cast<GLintptr>(cursor),
                                  static_cast<GLsizeiptr>(needBytes));
                // u_parityVertsPerType = expanded (triangle-soup) vertex count.
                if (locParityVerts >= 0)
                    glUniform1i(locParityVerts, (int)parityVerts);
                // u_parityBaseVertex = type's VBO baseVertex; shader subtracts
                // this from gl_VertexID to get the type-local index.
                if (locParityBaseVertex >= 0) {
                    const int typeBase = (type.packetCount > 0)
                        ? s_packets[type.firstPacket].baseVertex
                        : 0;
                    glUniform1i(locParityBaseVertex, typeBase);
                }
                if (locParityWrite >= 0)
                    glUniform1i(locParityWrite, 1);
                s_parityBytesUsedThisFrame = cursor + needBytes;
                gos_object_parity::Counters_AddVerticesWrittenThisFrame(
                    static_cast<uint64_t>(r.instanceCount) *
                    static_cast<uint64_t>(parityVerts));
                wroteParityThisDraw = true;
                // Stage 2.D.2: record slot-relative cursor + expanded vertex
                // count so CompareAndReport can decode this type's bytes on
                // the readback frame. parityVerts (not type.vertexCount) here.
                if (gos_object_parity::IsDualEmitArmedAnyActor()) {
                    gos_object_parity::RecordParityTypeRange(
                        typeID, cursor, r.instanceCount, parityVerts);
                }
            } else if (needBytes > 0) {
                // Stage 2.D.1.1 (Item 1): slot budget exhausted — record it.
                // u_parityWrite stays 0 so the shader skips the write;
                // the draw still happens but no parity bytes are produced.
                gos_object_parity::Counters_AddSlotOverflowThisFrame(
                    typeID, needBytes, kParitySlotBytes);
            }
        }

        // Per-type draw-time trace: fires once per type per flush call.
        static thread_local uint32_t s_traceCount = 0;
        const bool doTypeTrace = s_texHandoffTrace && (s_traceCount < 8);
        if (doTypeTrace) ++s_traceCount;

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
            // Resolve the texture handle at draw time. MC2 mutates
            // TG_TypeShape::listOfTextures[slot].gosTextureHandle each frame
            // in TransformMultiShape (msl.cpp:1321 via SetTextureHandle),
            // so capturing the handle at registration time gives stale
            // (usually zero) reads.
            uint32_t gosHandle = 0;
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
                gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
            }
            const uint32_t glTexId = gos_GetGLTextureId(gosHandle);
            if (doTypeTrace && p < 2) {
                TEX_HANDOFF("flush  typeID=%u pkt=%u slot=%u src=%p gosHandle=0x%08x glTexId=%u numTex=%d",
                            typeID, p, pkt.textureSlot, (void*)src, gosHandle, glTexId,
                            src ? src->numTextures : -1);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            // Re-resolve materialFlags at draw time so the textureAlpha flag set during
            // bdactor.cpp init (after registerType) is captured. Same pattern as gosTextureHandle.
            // textureAlpha==true means the CPU path routes via MC2_DRAWALPHA; the GPU path
            // approximates this with shader discard (valid for binary-alpha fence/gate textures).
            uint32_t effectiveMaterialFlags = pkt.materialFlags;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures &&
                src->listOfTextures[pkt.textureSlot].textureAlpha) {
                effectiveMaterialFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
            }
            ALPHA_TRACE("draw type=%u pkt=%u slot=%u pktFlags=0x%x effective=0x%x texAlpha=%d",
                        typeID, p, pkt.textureSlot, pkt.materialFlags, effectiveMaterialFlags,
                        (src && src->listOfTextures && pkt.textureSlot < src->numTextures)
                            ? (int)src->listOfTextures[pkt.textureSlot].textureAlpha : -1);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_materialFlags"),
                        (int)effectiveMaterialFlags);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_packetID"),
                        (int)(type.firstPacket + p));
            // Drain any stale GL error first so our check is clean.
            while (glGetError() != GL_NO_ERROR) {}
            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES,
                pkt.indexCount,
                GL_UNSIGNED_INT,
                reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                r.instanceCount,
                pkt.baseVertex);
        }
        // Stage 2.D.1: clear u_parityWrite back to 0 so a subsequent type
        // that gets rejected by the slot-budget check (or has zero
        // vertices) can't inherit a stale write authorization against this
        // type's binding range.
        if (wroteParityThisDraw && locParityWrite >= 0) {
            glUniform1i(locParityWrite, 0);
        }
    }

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Stage 2.D.1: record this slot's parity-byte usage so the next visit
    // (RING_FRAMES frames from now) knows exactly how much to glGetBufferSubData.
    s_parityBytesUsedPerSlot[s_frameSlot] = s_parityBytesUsedThisFrame;
    // Stage 2.D.2: after the fence is inserted (GPU draw committed), advance
    // the dual-emit latch from Armed → WaitingForReadback. Pass s_frameSlot
    // so the compare only fires when the ring revisits this exact slot (after
    // glClientWaitSync ensures GPU writes are complete — RING_FRAMES later).
    if (gos_object_parity::IsDualEmitArmedAnyActor()) {
        gos_object_parity::AdvanceDualEmitToWaiting(s_frameSlot);
    }

    // Restore GL state to EXACTLY what it was at flush start.
    // SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, (GLuint)prevSsbo2);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, (GLuint)prevSsbo3);
    // Texture binding on unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    glActiveTexture((GLenum)prevActiveTex);
    // Program / VAO / buffer bindings
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    // Pipeline state
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glCullFace((GLenum)prevCullMode);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);

    // Slice 2 (object-offload) — Stage 2.D.1: tick parity counters and emit
    // the 600-frame summary line if the cadence is hit. Internally a no-op
    // when MC2_OBJECT_PARITY_CHECK is unset.
    gos_object_parity::ParityFrameTick();

    // Stage 2.D.3 — round-robin sampler pick for next frame.
    //
    // Called AFTER ParityFrameTick so the summary's parity_compared_actors
    // sees the just-finalized count, and AFTER AdvanceDualEmitToWaiting so
    // the state machine has progressed for this frame. PickNextSampleIfReady:
    //   - rotates this frame's submitMultiShape observation list into the
    //     prior-frame pool (always),
    //   - if state == Done AND the pool is non-empty, picks one shape via
    //     round-robin and re-arms (state=Armed, armMode=SampledOnly,
    //     s_currentSampledShape=picked).
    // Default-off inside the sidecar; safe to call unconditionally.
    gos_object_parity::PickNextSampleIfReady();

    s_bucketsByType.clear();
    s_lastUploadedSlot = 0xFFFFFFFFu;  // reset for next frame
}

void GpuStaticPropBatcher::flushShadow() {
    // Filled in Task 13.
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }

// ---------------------------------------------------------------------------
// Slice 2, Stage 2.A — new method and free-function definitions
// ---------------------------------------------------------------------------

// GpuStaticPropBatcher::wasLastFailureLateRegistration (lines added below)
// Returns whether the most recent submitMultiShape() call returned false
// because of a late-registration miss. The flag s_lastSubmitWasLateReg is
// cleared at the TOP of every submitMultiShape() call (before any checks)
// so the window of confusion is exactly one submit.
bool GpuStaticPropBatcher::wasLastFailureLateRegistration() const {
    return s_lastSubmitWasLateReg;
}

// Slice 2 (Stage 2.D.3) — late-registration event-counter queries.
// Static methods so the parity sidecar can call them without a singleton
// hop. The counters live in the file-static block above; they are
// monotonic since process start.
uint64_t GpuStaticPropBatcher::getAllowedLateRegEventCount() {
    return s_lateReg_allowed_events;
}
uint64_t GpuStaticPropBatcher::getDisallowedLateRegEventCount() {
    return s_lateReg_disallowed_events;
}

// GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects
// Cheap, side-effect-free eligibility check used by the addRenderShape gate
// (tgl.cpp:2522 Stage 2.B) and by *Appearance::update (Stage 2.B) before
// calling submitMultiShape. Returns true iff g_useGpuObjects is enabled AND
// every SHAPE_NODE leaf under `multi` is registered in s_typeIndex.
// MUST NOT mutate any batcher state, log, or increment counters.
bool GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(
        const TG_MultiShape* multi) const {
    if (!g_useGpuObjects) return false;
    if (!multi) return false;

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) return false;

    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers/bones
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (s_typeIndex.find(ts) == s_typeIndex.end()) return false;
    }
    return true;
}

// eligibleForGpuObjects — free function (declared in gos_static_prop_batcher.h)
// Per-leaf analog of isMultiShapeEligibleForGpuObjects. Called from the
// addRenderShape gate in tgl.cpp:2522 inside the bShadersDrawPathEnabled block.
// Returns true iff g_useGpuObjects is enabled AND `shape->myType` is a
// registered TG_TypeShape in s_typeIndex.
// Under default g_useGpuObjects=false this returns false unconditionally —
// the addRenderShape gate stays byte-for-byte equivalent in stock builds.
// MUST NOT mutate any batcher state.
bool eligibleForGpuObjects(TG_Shape* shape) {
    if (!g_useGpuObjects) return false;
    if (!shape) return false;
    if (!shape->myType) return false;
    if (shape->myType->GetNodeType() != SHAPE_NODE) return false;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(shape->myType);
    return s_typeIndex.find(ts) != s_typeIndex.end();
}

void gos_GpuPropsCycleDebugMode() {
    auto& b = GpuStaticPropBatcher::instance();
    int next = (b.getDebugAddrMode() + 1) % 8;
    b.setDebugAddrMode(next);
}
int gos_GpuPropsGetDebugMode() {
    return GpuStaticPropBatcher::instance().getDebugAddrMode();
}

// Stage 3.C ----------------------------------------------------------------

const std::vector<GpuStaticPropInstance>& GpuStaticPropBatcher::getLastBuiltBatch() const {
    return s_lastBuiltBatch;
}

void GpuStaticPropBatcher::submitCachedInstance(const GpuStaticPropInstance& inst) {
    if (inst.typeID >= s_types.size()) return;
    const GpuStaticPropType& type = s_types[inst.typeID];
    PerTypeBucket& bucket = s_bucketsByType[inst.typeID];

    // firstColorOffset must be updated to the current bucket color position
    // so the GPU shader indexes colors correctly for this frame's layout.
    // Diagnostic counters (submitted_instances_by_pop etc.) are NOT incremented
    // for registry-injected instances — they measure the dynamic compute path.
    GpuStaticPropInstance updated = inst;
    updated.firstColorOffset = static_cast<uint32_t>(bucket.colors.size());
    {
        // MC2_TREE_DIAG_TRACE diagnostic. Same format as submit() so a grep
        // diff (`grep "[TREE_DIAG]" log | sort`) lines up dyn-vs-static fields.
        static int s_staticPrinted = 0;
        if (s_treeDiagTrace && s_staticPrinted < 8) {
            ++s_staticPrinted;
            TREE_DIAG("path=stat typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                updated.typeID, updated.flags, updated.lightDataIndex, updated.firstColorOffset,
                // Same offset fix as submit() — translation at [3]/[7]/[11], not [12..14].
                updated.modelMatrix[3], updated.modelMatrix[7], updated.modelMatrix[11],
                updated.fogRGB[0], updated.fogRGB[1], updated.fogRGB[2], updated.fogRGB[3],
                updated.aRGBHighlight[0], updated.aRGBHighlight[1], updated.aRGBHighlight[2], updated.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    bucket.instances.push_back(updated);
    // Zero-fill colors. Normal render ignores the Colors SSBO (binding 1);
    // debug addr-mode 4 shows black for static-registry instances.
    bucket.colors.insert(bucket.colors.end(), type.vertexCount, 0u);
}

// Track B -----------------------------------------------------------------

bool GpuStaticPropBatcher::buildRecipeFromShape(
        TG_Shape* shape,
        const Stuff::Matrix4D& shapeToWorld,
        uint32_t highlightARGB,
        uint32_t fogARGB,
        uint32_t flags,
        GpuStaticPropInstance* outRecipe) const {
    if (!shape || !outRecipe) return false;

    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) return false;

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = it->second;
    inst.firstColorOffset = 0u;          // placeholder; submitCachedInstance patches per-frame
    inst.flags            = flags;
    inst.lightDataIndex   = 0xFFFFFFFFu; // sentinel; flush() patches per-frame
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;

    *outRecipe = inst;
    return true;
}
