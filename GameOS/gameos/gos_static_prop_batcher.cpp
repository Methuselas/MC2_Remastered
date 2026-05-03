#include "gos_static_prop_batcher.h"
#include "gos_static_prop_killswitch.h"  // gos_GetGLTextureId
#include "gos_profiler.h"
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
bool g_useGpuObjects = false;

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
    std::fprintf(stderr, "[GPUPROPS-DIAG] loadProgramsIfNeeded OK prog=%u\n",
                 s_staticPropProgram);
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
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape) {
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
    const long n = multiShape->GetNumShapes();
    for (long i = 0; i < n; ++i) {
        TG_TypeNodePtr node = multiShape->GetTypeNode(i);
        if (node && node->GetNodeType() == SHAPE_NODE) {
            registerType(static_cast<TG_TypeShape*>(node));
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
    bucket.instances.push_back(inst);

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
    // Clear the late-reg signal at the top of every call so a stale "true"
    // from a prior submitMultiShape never masquerades as a signal for this one.
    s_lastSubmitWasLateReg = false;
    // pop consumed by counters below.
    if (!multi || s_fatalRegistrationFailure) return false;
    if (s_programLoadFailed || s_staticPropProgram == 0) return false;

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) return false;

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
            const std::string typeKey = addrBuf;  // dedup key — pointer-stable
            auto& count = s_lateRegisterCounts[typeKey];
            if (count == 0) {
                const bool allowed =
                    (s_lateRegisterAllowlist.find(typeKey)
                     != s_lateRegisterAllowlist.end());
                const char* nodeId = nullptr;
                if (ts) {
                    // TG_TypeNode::getNodeId is non-const; we have a
                    // const TG_TypeShape* by way of the static_cast above.
                    // const_cast is the minimal workaround.
                    nodeId = const_cast<TG_TypeShape*>(ts)->getNodeId();
                }
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
            return false;
        }
    }

    // Slice 2 (object-offload) — Stage 2.C: per-actor light-data gather.
    // Hoisted BETWEEN the two for-loops per the spec's locked Sign-Off #2:
    // - inside loop 1 (registration check) it would gather for actors that
    //   get rejected on a later child — wasted work;
    // - inside loop 2 (submit) it would be per-leaf, incurring N-fold
    //   redundant GatherLightsParameters calls per multishape per frame.
    // Here, after the registration-check loop has cleared. firstShapeNodeLeaf
    // is non-null because the registration loop accepted at least one
    // SHAPE_NODE (otherwise we would have early-returned above).
    uint32_t lightDataIndex = 0;
    if (firstShapeNodeLeaf != nullptr) {
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
            return false;
        }
        s_counters.submitted_children++;
    }
    s_counters.submitted_instances_by_pop[popIndex(pop)]++;
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
        return;
    }
    // Program compile/link latch. submitMultiShape already gates submissions
    // on this, so reaching here with an empty program is a logic bug — but
    // guard anyway so we never pump uniform calls against a null program.
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_bucketsByType.clear();
        s_lastUploadedSlot = 0xFFFFFFFFu;
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        return;
    }

    // Save ALL GL state we'll mutate so we can restore it at the end.
    // This is the defensive-save approach — the engine's MLR/HUD paths
    // under 4.3 core are fragile about inherited bindings, so we behave
    // as if every caller expects state unchanged.
    GLint prevProgram=0, prevVao=0, prevArrayBuf=0, prevElemBuf=0;
    GLint prevActiveTex=0, prevTexUnit0=0;
    // Slice 2 (object-offload) — Stage 2.C.2: also save/restore SSBO slot 2
    // (per-type hot-color SSBO; bound once for the whole flush, not per-type).
    GLint prevSsbo0=0, prevSsbo1=0, prevSsbo2=0;
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

    // Direct uniforms (AMD invariant: direct, GL_FALSE transpose).
    // worldToClip: TG_Shape::s_worldToClip is set by camera each frame in
    // mclib/tgl.cpp:1558. Matrix4D is row-major (Stuff layout); upload with
    // GL_FALSE just like the terrain MVP path.
    const GLint locWTC = glGetUniformLocation(s_staticPropProgram, "u_worldToClip");
    const float* wtc = (const float*)&TG_Shape::s_worldToClip.entries[0];
    // Stuff::Matrix4D is stored column-major with row-vec convention (per
    // entries[col*4+row]). GL_FALSE treats data as column-major, so GLSL
    // would see the same matrix — and `M * v` (col-vec math) would
    // miss the translation in row 3. GL_TRUE transposes on upload: GLSL
    // then sees the matrix swapped, and `M * v` becomes row-vec math =
    // correct. This matches what terrain does by explicitly writing its
    // matrix to memory row-major before upload (gamecam.cpp:169).
    if (locWTC >= 0) glUniformMatrix4fv(locWTC, 1, GL_TRUE, wtc);
    // Terrain projection chain — matches shaders/terrain_overlay.vert usage.
    // TG_Shape outputs are in Stuff/camera world coords; u_worldToClip gives
    // MC2 D3D-style screen-pixel homogeneous, and we then do divide +
    // viewport + pixel->NDC with abs(w).
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
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_materialFlags"),
                        (int)pkt.materialFlags);
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
    }

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // Restore GL state to EXACTLY what it was at flush start.
    // SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, (GLuint)prevSsbo2);
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
