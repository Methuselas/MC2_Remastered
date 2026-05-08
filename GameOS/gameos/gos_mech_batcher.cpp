// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.
#include "gos_mech_batcher.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "utils/shader_builder.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>

// MECH_RING_FRAMES must equal STATIC_PROP_RING_FRAMES so the parity SSBO
// ring and the mech fence ring share the same depth.
static_assert(MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "MECH_RING_FRAMES must match STATIC_PROP_RING_FRAMES");

// Enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M in gameosmain.cpp hotkey handler.
bool g_useGpuMechs = (getenv("MC2_GPU_MECHS") != nullptr);

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
static bool   s_programLoadTried  = false;
static bool   s_programLoadFailed = false;
static GLuint s_mechProgram       = 0;
static glsl_program* s_mechProgramObj = nullptr;

// Cached uniform locations (set at program link time).
static GLint s_loc_u_instanceBase    = -1;
static GLint s_loc_u_materialFlags   = -1;
static GLint s_loc_u_worldToClip     = -1;
static GLint s_loc_u_terrainViewport = -1;
static GLint s_loc_u_mvp             = -1;
static GLint s_loc_u_tex             = -1;
static GLint s_loc_u_fogValue        = -1;

// Geometry (immutable after finalizeGeometry).
static GLuint s_sharedVao = 0;
static GLuint s_sharedVbo = 0;
static GLuint s_sharedIbo = 0;
static GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
static bool   s_geometryFinalized = false;

// Staging buffers (cleared after finalizeGeometry).
static std::vector<uint8_t>   s_stagingVbo;
static std::vector<uint32_t>  s_stagingIbo;

// Type registration.
static std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
static std::vector<GpuMechPacket>        s_packets;

// Key: (Mech3DAppearanceType*, lod) -> s_typeLodRecords index
struct TypeLodKey {
    const Mech3DAppearanceType* type;
    int lod;
    bool operator==(const TypeLodKey& o) const { return type == o.type && lod == o.lod; }
};
struct TypeLodKeyHash {
    size_t operator()(const TypeLodKey& k) const {
        return std::hash<const void*>()(k.type) ^ (std::hash<int>()(k.lod) * 2654435761u);
    }
};
static std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;

// Per-frame ring SSBOs.
static GLuint   s_instanceSsbo    = 0;
static GLuint   s_boneSsbo        = 0;
static void*    s_instanceMap     = nullptr;
static void*    s_boneMap         = nullptr;
static size_t   s_instanceCapacity = 0;  // per ring slot (in GpuMechInstance units)
static size_t   s_boneCapacity     = 0;  // per ring slot (in GpuMechBone units)
static uint32_t s_frameSlot        = 0;
static GLsync   s_fence[MECH_RING_FRAMES] = {};

static constexpr size_t kInitialInstancesPerFrame = 512;
static constexpr size_t kInitialBonesPerFrame     = 8192;

// Per-frame pending submit list.
struct PendingSubmit {
    GpuMechSubmitDesc        desc;
    std::vector<GpuMechBone> bones;          // staged from listOfShapes[i].shapeToWorld
    std::vector<uint32_t>    packetTexHandles; // per-packet live gosHandle captured at submit
    uint32_t                 typeLodIdx;
};
static std::vector<PendingSubmit> s_pendingSubmits;

// Counters.
static bool     s_mechBatcherTrace     = false;
static bool     s_mechBatcherTraceInit = false;
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason
static uint64_t s_allowedLateRegEvents    = 0;
static uint64_t s_disallowedLateRegEvents = 0;
static bool     s_lastFailWasLateReg      = false;

// ---------------------------------------------------------------------------
// Shader load
// ---------------------------------------------------------------------------
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", "#version 430\n");

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;

    auto loc = [&](const char* name) {
        return glGetUniformLocation(s_mechProgram, name);
    };
    s_loc_u_instanceBase    = loc("u_instanceBase");
    s_loc_u_materialFlags   = loc("u_materialFlags");
    s_loc_u_worldToClip     = loc("u_worldToClip");
    s_loc_u_terrainViewport = loc("u_terrainViewport");
    s_loc_u_mvp             = loc("u_mvp");
    s_loc_u_tex             = loc("u_tex");
    s_loc_u_fogValue        = loc("u_fogValue");

    std::fprintf(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management (Task 5 replaces this stub)
// ---------------------------------------------------------------------------
static void ensureRingCapacity(size_t, size_t) {}  // STUB — Task 5

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GpuMechBatcher& GpuMechBatcher::instance() {
    static GpuMechBatcher batcher;
    return batcher;
}

void GpuMechBatcher::onMapLoad() {
    s_typeLodRecords.clear();
    s_packets.clear();
    s_typeLodIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_geometryFinalized = false;
    s_programLoadTried  = false;
    s_programLoadFailed = false;
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_load\n");
}

void GpuMechBatcher::onMapUnload() {
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_sharedVao)    { glDeleteVertexArrays(1, &s_sharedVao);    s_sharedVao    = 0; }
    if (s_sharedVbo)    { glDeleteBuffers(1, &s_sharedVbo);         s_sharedVbo    = 0; }
    if (s_sharedIbo)    { glDeleteBuffers(1, &s_sharedIbo);         s_sharedIbo    = 0; }
    if (s_sampler)      { glDeleteSamplers(1, &s_sampler);          s_sampler      = 0; }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo);      s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);          s_boneSsbo     = 0; s_boneMap     = nullptr; }
    s_instanceCapacity  = 0;
    s_boneCapacity      = 0;
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[(int)r]; }
void GpuMechBatcher::flushShadow() {}  // no-op in Slice A/B1/B2

// ---------------------------------------------------------------------------
// Stubs — replaced in Tasks 4, 6, 7. Present so the project links cleanly.
// ---------------------------------------------------------------------------
void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType*, int) {}
void GpuMechBatcher::finalizeGeometry() { s_geometryFinalized = true; }
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc&) { return false; }
void GpuMechBatcher::flush() {}
