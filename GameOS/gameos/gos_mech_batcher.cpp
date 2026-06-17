// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.
#include "gos_mech_batcher.h"
#include "gos_materials.h"       // Slice C1: named material profile SSBO (binding 7)
#include "gos_gpu_sync.h"      // GPU-SYNC-CONTRACT typed barrier helper
#include "render_snapshot.h"  // MECH-EXTRACTION-0: ExtractedMechPacket, RenderSnapshot
#include "../../RenderCore/RenderDebugView.h"  // MECH-DEBUG-VIEWS-1
#include "../../RenderCore/PipelineRegistry.h"  // MECH-PIPELINEDESC-1
#include "pipeline_binder.h"                     // MECH-PIPELINEDESC-1 applyPipeline
// M2.5: IsObjectIdBufferEnabled() drives the GLSL prefix that gates the
// mech.frag layout(location=2) write. Mirrors the include shipped by M1.5
// at gos_static_prop_batcher.cpp:3. GameOS/ is outside the firewall
// SCOPE_DIRS (scripts/check-include-firewall.sh:22) so this include is
// not policed by the firewall script; reviewer-discipline gate only.
// RenderWorld/RenderWorld.h is the PUBLIC header (no legacy/* reach).
#include "../../RenderWorld/RenderWorld.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "gameos.hpp"                 // gos_InvalidateRenderStateCache
#include "utils/shader_builder.h"
#include "gos_postprocess.h"           // getGosPostProcess()->getDynamicLightSpaceMatrix()
#include "../../mclib/txmmgr.h"       // mcTextureManager->get_gosTextureHandle (live resolve)
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <array>
#include <cmath>

// M2.5 (external-review C1): file-scope forward declaration of the
// MLR-side per-mission counter getter, defined in mclib/mech3d.cpp.
// Language-linkage declarations must be at file scope -- not inside a
// function body. Avoids a new header.
extern "C" uint64_t consumeAndResetMlrMechDraws();

// MECH_RING_FRAMES must equal STATIC_PROP_RING_FRAMES so the parity SSBO
// ring and the mech fence ring share the same depth.
static_assert(MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "MECH_RING_FRAMES must match STATIC_PROP_RING_FRAMES");

// Default-on flip (2026-05-09): all GPU mech killswitches now default to ON.
// Helper returns true unless the env var is explicitly set to "0". Operators
// can opt out per-flag via MC2_GPU_MECH_<FLAG>=0. Pre-flip pattern was
// `(getenv("X") != nullptr)` (default-off opt-in); post-flip is `default-on,
// "0" opts out`. Each individual slice's killswitch comments in
// gos_mech_killswitch.h document the inverted semantics.
//
// Why default-on shipped 2026-05-09: full slice campaign accumulated
// substantial empirical soak — tier1 5/5 PASS at full bore, multiple 90s
// mc2_10 Tracy captures clean, mouse-pick + sensor-diamond visual canaries
// passed, combined stack delivers Mech3D.UpdateGeometry mean 71→14.08µs/call
// (-80%). User explicitly signed off on B1's "needs more verification"
// constraint based on this evidence.
static bool envFlagDefaultOn(const char* name) {
    const char* v = getenv(name);
    if (v == nullptr) return true;                  // unset → on (new default)
    if (v[0] == '0' && v[1] == '\0') return false;  // exactly "0" → off
    return true;                                     // any other value → on
}

// ImGui visibility kill-switch. Always true at startup; toggled by the
// Graphics Options panel "Draw Mechs" checkbox. When false, both the GPU
// batcher submit and the CPU Render(true) fallback are skipped.
bool g_drawMechs = true;

// Slice A: GPU mech batcher. Foundation for the entire stack.
// Opt-out: MC2_GPU_MECHS=0
bool g_useGpuMechs = envFlagDefaultOn("MC2_GPU_MECHS");

// SimpleCamera UI preview-render context depth (Mech Bay / Mech Purchase).
// See gos_mech_killswitch.h. Zero during tactical/world rendering.
int g_mechPreviewRenderDepth = 0;

// Slice B1: enables calc_light() in mech.vert. Requires g_useGpuMechs=true
// to take effect (the calc_light branch is inside the GPU mech draw path).
// Opt-out: MC2_GPU_MECH_LIGHTING=0
bool g_useGpuMechLighting = envFlagDefaultOn("MC2_GPU_MECH_LIGHTING");

// Slice C1: render-only mech GPU cull. Requires g_useGpuMechs=true.
// Opt-out: MC2_GPU_MECH_CULL=0
bool g_useGpuMechCull = envFlagDefaultOn("MC2_GPU_MECH_CULL");

// Slice C2: weighted multi-bone skinning. Requires g_useGpuMechs=true.
// Opt-out: MC2_GPU_MECH_SKIN=0
bool g_useGpuMechSkin = envFlagDefaultOn("MC2_GPU_MECH_SKIN");

// Slice C3-revised: see gos_mech_killswitch.h. Body-only fast transform.
// Opt-out: MC2_GPU_MECH_FAST_TRANSFORM=0
bool g_useGpuMechFastTransform = envFlagDefaultOn("MC2_GPU_MECH_FAST_TRANSFORM");

// Slice C3-shadow: see gos_mech_killswitch.h. Shadow callsite fast transform.
// Opt-out: MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=0
bool g_useGpuMechShadowFastTransform = envFlagDefaultOn("MC2_GPU_MECH_SHADOW_FAST_TRANSFORM");

// Slice D-shadow-skip: see gos_mech_killswitch.h. Skip mechShadowShape transform.
// Opt-out: MC2_GPU_MECH_SHADOW_SKIP=0
bool g_useGpuMechShadowSkip = envFlagDefaultOn("MC2_GPU_MECH_SHADOW_SKIP");

// Slice D-shadow-state-strip: see gos_mech_killswitch.h.
// Opt-out: MC2_GPU_MECH_SHADOW_STATE_STRIP=0
bool g_useGpuMechShadowStateStrip = envFlagDefaultOn("MC2_GPU_MECH_SHADOW_STATE_STRIP");

// Slice D-leaf-skip-v2: see gos_mech_killswitch.h.
// Opt-out: MC2_GPU_MECH_LEAF_SKIP=0
bool g_useGpuMechLeafSkip = envFlagDefaultOn("MC2_GPU_MECH_LEAF_SKIP");

// Slice D-sensor-skip: see gos_mech_killswitch.h.
// Opt-out: MC2_GPU_MECH_SENSOR_SKIP=0
bool g_useGpuMechSensorSkip = envFlagDefaultOn("MC2_GPU_MECH_SENSOR_SKIP");

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
static bool   s_programLoadTried  = false;
static bool   s_programLoadFailed = false;
static GLuint s_mechProgram       = 0;
static glsl_program* s_mechProgramObj = nullptr;

// MECH-DEBUG-VIEWS-1: ImGui-settable debug mode (mirrors s_staticPropDebugMaterialMode).
// 0 = Final (normal rendering); env MC2_MECH_FRAG_DEBUG wins when set.
static int s_mechDebugMode = 0;

// MECH-VIEWUNIFORMS-BLOCKBINDING-1: mech-specific ViewUniforms opt-in gate,
// DEFAULT OFF (MC2_MECH_VIEWUNIFORMS=1). Independent of the global
// MC2_VIEW_UNIFORMS. When ON, the mech shader is compiled with the
// ViewUniformsBlock (binding=3) consumer and the block binding is enforced
// explicitly after link; the legacy u_worldToClipGL upload is skipped. When
// OFF (default), the mech path is byte-identical to legacy.
static const bool s_mechViewUniforms = []() {
    const char* v = std::getenv("MC2_MECH_VIEWUNIFORMS");
    return !(v != nullptr && v[0] == '0');   // DEFAULT-ON; kill-switch =0
}();
// Shared diag gate (link-time block-binding log + flush-time binding/matrix
// probe). Default OFF (MC2_MECH_VIEWUNIFORMS_DIAG=1).
static const bool s_mechViewUniformsDiag = []() {
    const char* v = std::getenv("MC2_MECH_VIEWUNIFORMS_DIAG");
    return v != nullptr && v[0] == '1';
}();

// MECH-NORMALS-FIX-1: gated mech-LOCAL normal recompute, applied at
// registerTypeLod over each node's triangle-soup verts. Does NOT touch the
// shared TGL ASE loader (mclib/tgl.cpp LoadTGShapeFromASE) — which averages
// ASE per-corner normals by vertex index and destroys hard-edge splits (see
// docs/mech-normals-audit.md) but is used by all props/buildings (high blast
// radius). MC2_MECH_NORMALS_MODE: 0=cooked (legacy; kill-switch), 1=geometric
// face normals to all corners (faceted; confirm/debug), 2=angle-threshold
// smoothed (hard-edge preserving) — DEFAULT. Smoothing angle 60deg.
// These are mutable so ImGui controls (via batcher_setMechNormalsMode /
// batcher_setMechNormalsSmoothDeg + batcher_rebuildMechNormals) can dial them
// at runtime without a restart. Env vars still set the startup default.
// DEFAULT-ON: mode 2 (smoothed, hard-edge preserving) — the intended look that
// fixes the corrupted ASE-averaged normals. Kill-switch MC2_MECH_NORMALS_MODE=0
// restores cooked/legacy normals; =1 = faceted. User-approved 2026-05-28.
static int s_mechNormalsMode = []() {
    const char* v = std::getenv("MC2_MECH_NORMALS_MODE");
    return v ? std::atoi(v) : 2;
}();
// Smoothing angle for mode 2 (degrees): faces meeting at a shared vertex blend
// only when within this angle, so a smaller value preserves MORE hard edges
// (e.g. lower it to sharpen the cockpit). Env-tunable for dial-in; default 60.
static float s_mechNormalsSmoothDeg = []() {
    const char* v = std::getenv("MC2_MECH_NORMALS_SMOOTH_DEG");
    float d = v ? (float)std::atof(v) : 60.0f;
    if (d < 1.0f)   d = 1.0f;
    if (d > 179.0f) d = 179.0f;
    return d;
}();

// MECH-NORMALS-FIX-1: per-node VBO byte ranges recorded during registerTypeLod.
// Each entry is {startByte, endByte} in s_stagingVbo for one node's verts.
// uploadMechGeometryVbo() iterates these to bound recomputeMechNodeNormals
// to a single node (it must not blend normals across node boundaries).
// Cleared at onMapLoad() alongside s_stagingVbo.
static std::vector<std::pair<size_t,size_t>> s_nodeVboRanges;

// MECH-AMBIENT-1: hemisphere ambient FILL for mechs. DEFAULT-ON (user-approved
// 2026-05-28 at strength 0.15) with kill-switch MC2_MECH_AMBIENT_V1=0. When off
// the strength uploaded to the shader is 0.0 -> exact no-op (legacy look). Both
// mutable so ImGui can dial them live (batcher_setMechAmbient*). The shader term
// is a per-fragment hemisphere using the world normal; no PBR/material/team-mask
// data involved. Works on legacy and ViewUniforms mech paths (no camera data).
static bool  s_mechAmbientV1 = []() {
    const char* v = std::getenv("MC2_MECH_AMBIENT_V1");
    return !(v != nullptr && v[0] == '0');   // default-ON; kill-switch =0
}();
static float s_mechAmbientStrength = []() {
    const char* v = std::getenv("MC2_MECH_AMBIENT_V1_STRENGTH");
    float d = v ? (float)std::atof(v) : 0.15f;   // user-tuned default
    if (d < 0.0f) d = 0.0f;
    if (d > 2.0f) d = 2.0f;
    return d;
}();

// MECH-SPECULAR-V1: conservative Blinn specular sheen. DEFAULT-OFF (gate
// MC2_MECH_SPECULAR_V1=1 to enable). Only effective when MC2_MECH_VIEWUNIFORMS=1
// (the shader variant with MC2_USE_VIEW_UNIFORMS defined); on all other paths the
// C++ side uploads strength 0 → exact no-op. Per-flush uniforms — no VBO rebuild.
static bool  s_mechSpecularV1 = []() {
    const char* v = std::getenv("MC2_MECH_SPECULAR_V1");
    return !(v != nullptr && v[0] == '0');  // DEFAULT-ON; kill-switch =0
}();
static float s_mechSpecularStrength = []() {
    const char* v = std::getenv("MC2_MECH_SPECULAR_STRENGTH");
    // Default 0.05: at 1.0 the Blinn highlight read as a near-glow on mech
    // armor. 0.05 is a subtle metal glint. MC2_MECH_SPECULAR_STRENGTH overrides.
    // Also profile-backed via the mechSpecularStrength key (visual_tuning.json).
    float d = v ? (float)std::atof(v) : 0.05f;
    if (d < 0.0f) d = 0.0f;
    if (d > 4.0f) d = 4.0f;
    return d;
}();
static float s_mechMetalRoughness = []() {
    const char* v = std::getenv("MC2_MECH_METAL_ROUGHNESS");
    float d = v ? (float)std::atof(v) : 0.85f;
    if (d < 0.04f) d = 0.04f;
    if (d > 1.0f)  d = 1.0f;
    return d;
}();
static float s_mechGlassRoughness = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_ROUGHNESS");
    float d = v ? (float)std::atof(v) : 0.25f;
    if (d < 0.04f) d = 0.04f;
    if (d > 1.0f)  d = 1.0f;
    return d;
}();
static float s_mechGlassLumaThresh = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_LUMA_THRESH");
    float d = v ? (float)std::atof(v) : 0.12f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}();
static float s_mechGlassMaxChanThresh = []() {
    const char* v = std::getenv("MC2_MECH_GLASS_MAXCHAN_THRESH");
    float d = v ? (float)std::atof(v) : 0.18f;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}();
static bool  s_mechSpecDebugMask = false;  // ImGui only; no env var

// PBR-TUNE-1: StandardLit GGX gate + material influence knobs.
// Mutable so ImGui can dial live (batcher_setStandardLitEnabled / batcher_setPbr*).
// Env-seeded defaults; no VBO rebuild needed (per-flush uniforms).
static int   s_standardLitEnabled = []() {
    const char* v = std::getenv("MC2_STANDARD_LIT_V1");
    return (v && v[0] == '1') ? 1 : 0;
}();
static float s_pbrMetallicInfluence = []() {
    const char* v = std::getenv("MC2_PBR_METALLIC_INFLUENCE");
    return v ? (float)std::atof(v) : 0.15f;
}();
static float s_pbrRoughnessMin = []() {
    const char* v = std::getenv("MC2_PBR_ROUGHNESS_MIN");
    float d = v ? (float)std::atof(v) : 0.45f;
    return d;
}();
static float s_pbrRoughnessMax = []() {
    const char* v = std::getenv("MC2_PBR_ROUGHNESS_MAX");
    float d = v ? (float)std::atof(v) : 0.90f;
    return d;
}();
static float s_pbrAmbientSpecularStrength = []() {
    const char* v = std::getenv("MC2_PBR_AMBIENT_SPECULAR");
    return v ? (float)std::atof(v) : 0.25f;
}();

// Cached uniform locations (set at program link time).
static GLint s_loc_u_instanceBase    = -1;
static GLint s_loc_u_materialFlags   = -1;
static GLint s_loc_terrainMVP     = -1;
static GLint s_loc_u_mvp             = -1;
static GLint s_loc_u_tex             = -1;
static GLint s_loc_u_fogValue        = -1;
static GLint s_loc_u_debugMode       = -1;
static GLint s_loc_u_mechAmbientV1Strength = -1;  // MECH-AMBIENT-1
static GLint s_loc_u_lightingMode    = -1;
static GLint s_loc_u_skinningMode    = -1;
// MECH-SPECULAR-V1: cached locations (all -1 on non-viewuniforms variants).
static GLint s_loc_u_mechSpecularV1Strength  = -1;
static GLint s_loc_u_mechMetalRoughness      = -1;
static GLint s_loc_u_mechGlassRoughness      = -1;
static GLint s_loc_u_mechGlassLumaThresh     = -1;
static GLint s_loc_u_mechGlassMaxChanThresh  = -1;
static GLint s_loc_u_mechSpecDebugMask       = -1;
// Slice C1: StandardLit toggle. Returns -1 until mech.frag declares the uniform;
// guarded with >= 0 at upload so no crash when shader lacks it.
static GLint s_loc_u_standardLitEnabled      = -1;
// Slice C2: PBR detail material samplers + tile scale.
static GLint s_loc_u_pbrNormalTex            = -1;
static GLint s_loc_u_pbrOrmTex               = -1;
static GLint s_loc_u_pbrTileScale            = -1;
// PBR-TUNE-1: material influence knobs (metallic scale, roughness clamp, ambient specular).
static GLint s_loc_u_pbrMetallicInfluence      = -1;
static GLint s_loc_u_pbrRoughnessMin           = -1;
static GLint s_loc_u_pbrRoughnessMax           = -1;
static GLint s_loc_u_pbrAmbientSpecularStrength = -1;

// Geometry (immutable after finalizeGeometry).
static GLuint s_sharedVao = 0;
static GLuint s_sharedVbo = 0;
static GLuint s_sharedIbo = 0;
static GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
static bool   s_geometryFinalized = false;
// VPL-#11 (SP-logistics late roster): set by registerTypeLod when a type
// registers AFTER finalizeGeometry (campaign .fit resume spawns the
// player force-group post-Mission::init-finalize). finalizePending()
// consumes it to rebuild the shared VBO/IBO from the (retained) staging.
static bool   s_pendingLateTypes = false;
static uint32_t s_lateStagedCount = 0;  // late (type,lod) regs since last finalizePending

// Staging buffers. RETAINED for the whole mission (NOT freed at
// finalizeGeometry) so finalizePending() can append a late-registered
// type and rebuild the immutable shared VBO/IBO from the full
// original+late staging (VPL-#11 SP-logistics fix; the alternative of
// re-deriving geometry from TG type shapes is fragile). Cleared only at
// onMapLoad(). Memory cost is MB-scale and intentionally not optimized.
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

// MECH-EXTRACTION-0: L2 handoff / persist buffer.
//
// Cross-phase lifetime — this MUST remain a std::vector (or equivalent heap storage).
// It CANNOT be moved to the RenderSnapshot FrameArena.
//
// Write path: flush() (inside DoGameLogic()) fills s_mechExtractPersist and then
//   immediately calls s_pendingSubmits.clear().  At that point the FrameArena for
//   the current frame has not been allocated yet.
//
// Read path: ExtractRenderSnapshot() (after DoGameLogic() returns) calls
//   batcher_getMechPendingCount/Entry, copies into snap.mechPackets which IS
//   FrameArena-backed, then calls batcher_compareMechSnapshot().
//
// The FrameArena is reset at the top of ExtractRenderSnapshot() — after
// DoGameLogic() exits — so any pointer written during flush() into the arena
// would be use-after-reset by the time the read path runs.
// Keep the batcher-side vector as the authoritative handoff buffer; the
// FrameArena copy in snap.mechPackets is the consumer-side view.
static std::vector<ExtractedMechPacket> s_mechExtractPersist;

// Per-frame draw-call snapshot persisted at the END of flush() for use by
// flushShadow() on the NEXT frame.  flushShadow() runs before this frame's
// flush(), so it reads last frame's already-fenced SSBO slot (s_frameSlot)
// together with the drawCalls that were built for that same slot.
struct ShadowDrawEntry {
    uint32_t globalPacketIdx;
    uint32_t instanceBase;
    uint32_t instanceCount;
};
static std::vector<ShadowDrawEntry> s_lastDrawCalls;
static size_t                       s_lastTotalInstances = 0;
static size_t                       s_lastTotalBones     = 0;

// V1A: per-frame submit count latched at flush() entry (before any
// early-return guards clear s_pendingSubmits). Counts GpuMechBatcher
// submits only — does NOT include MLR fallback draws or alive count.
// queryVisibility() reads this for mechs_visible in VisibilityResult.
static uint64_t                     s_lastFlushSubmitCount = 0;

// File-scope counters written by flushShadow() and read by Task 6 probe.
static int s_shadowTypesDrawn = 0;
static int s_shadowInstDrawn  = 0;

// Counters.
static bool     s_mechBatcherTrace     = false;
static bool     s_mechBatcherTraceInit = false;
static bool     s_mechLightTrace       = false;
static bool     s_mechLightTraceInit   = false;
static uint32_t s_lightCacheFullFrames = 0;  // monotonic; emitted on first overflow per frame
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason

// M2.5 (Q4): always-on per-mission counter of per-instance fills whose
// objectIdRaw was non-zero. Incremented in flush()'s per-instance loop;
// emitted on the per-mission [MECHBATCHER v1] event=mech_id_summary
// line from onMapUnload(); reset in onMapLoad() and on emit.
//
// Per adversarial M1: the per-frame intermediate (s_gpuMechIdWritesThisFrame)
// was dropped -- it had NO consumer (only the per-mission counter is read
// by the emit). Direct per-mission accumulation removes three would-be
// reset sites (onMapLoad, flush early-out, flush normal end at line 1365)
// from M2.5's edit surface.
//
// Counter is ALWAYS-ON (NOT env-gated). When env-OFF the handle bits are
// still written into inst.objectIdRaw (Q3 unconditional CPU fill), so
// the counter tracks writer volume regardless of env state.
static uint64_t s_gpuMechIdWritesThisMission = 0;
static uint64_t s_allowedLateRegEvents    = 0;
static uint64_t s_disallowedLateRegEvents = 0;
static bool     s_lastFailWasLateReg      = false;

// [SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.2 baseline. Two-channel evidence:
//   (a) event=type_spotlight_node — emitted from registerTypeLod when a
//       SpotLight_-prefixed TypeNode is skipped during recipe build. Counts
//       distinct (typeLod, nodeIdx) tuples. Per-process monotonic; not gated
//       on per-frame cadence.
//   (b) event=mech_spotlight_draw — emitted from submitActor for actors whose
//       mechType carries >= 1 spotlight TypeNode (seen at recipe build).
//       Per-frame counter, 600-frame summary alongside mono.
// Map keyed by Mech3DAppearanceType* (the desc.mechType field on submitActor).
// Value is the number of SpotLight_-prefixed TypeNodes seen during ANY LOD's
// recipe build (max across LODs, since each instance only renders at one LOD).
static const bool s_spotlightRealTrace =
    (getenv("MC2_SPOTLIGHT_REAL_TRACE") != nullptr);
static std::unordered_map<const Mech3DAppearanceType*, uint32_t>
    s_spotlightRealTypeNodeCount;
static uint64_t s_spotlightReal_typeNodeEvents = 0;  // monotonic (a)
static uint64_t s_spotlightReal_drawWindow     = 0;  // 600-frame window (b)
static uint64_t s_spotlightReal_drawMono       = 0;  // monotonic (b)
static uint64_t s_spotlightReal_drawFrameCount = 0;  // frames flushed since process start
static bool     s_spotlightReal_firstHitTypeNode = false;
static bool     s_spotlightReal_firstHitDraw     = false;

// ---------------------------------------------------------------------------
// MaterialGpu Mech-1 substrate
// MC2_MATERIAL_GPU defaults ON. Set MC2_MATERIAL_GPU=0 to disable.
// ---------------------------------------------------------------------------
#include "../../RenderCore/MaterialGpu.h"
static const bool s_mechMaterialGpuEnabled = []() {
    const char* v = getenv("MC2_MATERIAL_GPU");
    return v == nullptr || (v[0] != '0');
}();
static std::vector<RenderCore::MaterialGpu>        s_mechMaterialTable;
static std::unordered_map<uint32_t, uint32_t>      s_mechHandleToMaterialIdx;
static std::vector<uint32_t>                       s_mechDrawMaterialIdx;
static GLuint                                      s_mechMaterialSsbo          = 0;
static uint32_t                                    s_mechMaterialGpuFrameCount = 0;

// Slice C1: named profile index for binding-7 SSBO (gos_materials).
// Resolved lazily at first flush() (after gos_materials::init() runs).
// 0 = default/passthrough (env unset or none/0). Non-zero = named profile active;
// overrides per-instance materialIdx only when selected so the existing per-actor
// albedo table (binding 2) is undisturbed when MC2_MECH_SURFACE_MATERIAL is unset.
// Protected by s_mechSurfaceMaterialIdxResolved to call getProfileIndex() only once.
static uint32_t s_mechSurfaceMaterialIdx         = 0u;
static bool     s_mechSurfaceMaterialIdxResolved = false;

// ---------------------------------------------------------------------------
// Shader load
// ---------------------------------------------------------------------------
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    // M2.5: GLSL preprocessor does NOT inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    // Build the prefix as a std::string and append the
    // MC2_OBJECT_ID_BUFFER macro definition when the env gate is on,
    // mirroring gos_static_prop_batcher.cpp:510-521.
    std::string mechPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        mechPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    // MECH-VIEWUNIFORMS-1: gated opt-in only (default OFF). Pulls the binding=3
    // ViewUniformsBlock into mech.vert via its #ifdef. The layout(binding=3)
    // qualifier is honored at link (verified below under diag), so no explicit
    // glUniformBlockBinding is needed.
    if (s_mechViewUniforms) {
        mechPrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", mechPrefix.c_str());

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;

    // MECH-PIPELINEDESC-1: register the linked mech program into PipelineRegistry
    // so flush() can drive fixed-function state via applyPipeline(MechOpaque)
    // instead of hand-set GL calls. Mirrors gos_static_prop_batcher.cpp's
    // bindProgram-at-link. Must run before any mech draw (flush() asserts
    // geometry finalized, which calls loadProgramsIfNeeded first).
    RenderCore::bindProgram(RenderCore::PipelineId::MechOpaque, s_mechProgram);

    // Slice B1 (2026-05-09): MAJOR-3 from adversarial review.
    // shaders/include/lighting.hglsl declares LightsData[64] (~113 KB
    // std140). GL spec mandates only 16 KB minimum support; many
    // older drivers / iGPUs cap at 64 KB. If the host GPU can't fit
    // the UBO, force g_useGpuMechLighting off so the shader's
    // u_lightingMode=0 branch (Slice A flat-white) runs and the
    // app doesn't fail compilation or read garbage.
    // [LIGHTSSBO v1] RF4: LightsData is no longer a UBO — it is an
    // unbounded std430 SSBO (binding 20). The old GL_MAX_UNIFORM_BLOCK_SIZE
    // gate (which forced g_useGpuMechLighting=false on GPUs whose max UBO
    // block < ~113KB) is now SPURIOUS: SSBO storage is bounded by
    // GL_MAX_SHADER_STORAGE_BLOCK_SIZE (typically >=128MB, never the
    // constraint) and there is no fixed 64-entry window. Disabling mech
    // lighting on the UBO basis would defeat the ceiling-removal this
    // conversion delivers. Gate removed.
    // Diagnostic gated behind MC2_LIGHTSSBO_TRACE (demote-not-delete;
    // matches gameos_graphics.cpp s_lightSsboTrace). Default-off so it
    // does not pollute frame-time captures.
    if (std::getenv("MC2_LIGHTSSBO_TRACE") != nullptr) {
        GLint maxSsbo = 0;
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsbo);
        std::fprintf(stderr,
            "[LIGHTSSBO v1] event=mech_ssbo_check max_ssbo_block=%d (no 64-slot cap)\n",
            maxSsbo);
    }

    auto loc = [&](const char* name) {
        return glGetUniformLocation(s_mechProgram, name);
    };
    s_loc_u_instanceBase    = loc("u_instanceBase");
    s_loc_u_materialFlags   = loc("u_materialFlags");
    s_loc_terrainMVP     = loc("u_worldToClipGL");
    s_loc_u_mvp             = loc("u_mvp");
    s_loc_u_tex             = loc("u_tex");
    s_loc_u_fogValue        = loc("u_fogValue");
    s_loc_u_debugMode       = loc("u_debugMode");
    s_loc_u_mechAmbientV1Strength = loc("u_mechAmbientV1Strength");  // MECH-AMBIENT-1
    s_loc_u_lightingMode    = loc("u_lightingMode");
    s_loc_u_skinningMode    = loc("u_skinningMode");
    // MECH-SPECULAR-V1: all return -1 on non-viewuniforms variants; guarded at upload.
    s_loc_u_mechSpecularV1Strength  = loc("u_mechSpecularV1Strength");
    s_loc_u_mechMetalRoughness      = loc("u_mechMetalRoughness");
    s_loc_u_mechGlassRoughness      = loc("u_mechGlassRoughness");
    s_loc_u_mechGlassLumaThresh     = loc("u_mechGlassLumaThresh");
    s_loc_u_mechGlassMaxChanThresh  = loc("u_mechGlassMaxChanThresh");
    s_loc_u_mechSpecDebugMask       = loc("u_mechSpecDebugMask");
    // Slice C1: StandardLit toggle (default 0 = passthrough; shader may not declare it yet).
    s_loc_u_standardLitEnabled      = loc("u_standardLitEnabled");
    // Slice C2: PBR detail surface samplers + tile scale.
    s_loc_u_pbrNormalTex            = loc("u_pbrNormalTex");
    s_loc_u_pbrOrmTex               = loc("u_pbrOrmTex");
    s_loc_u_pbrTileScale            = loc("u_pbrTileScale");
    // PBR-TUNE-1: material influence knobs.
    s_loc_u_pbrMetallicInfluence       = loc("u_pbrMetallicInfluence");
    s_loc_u_pbrRoughnessMin            = loc("u_pbrRoughnessMin");
    s_loc_u_pbrRoughnessMax            = loc("u_pbrRoughnessMax");
    s_loc_u_pbrAmbientSpecularStrength = loc("u_pbrAmbientSpecularStrength");

    // MECH-VIEWUNIFORMS-1: on the gated path, verify the ViewUniformsBlock is
    // present and bound to point 3. The GLSL layout(binding=3) qualifier is
    // honored at link (GL 4.2+ core; same as static_prop), so NO explicit
    // glUniformBlockBinding is required — confirmed by the diag below reporting
    // binding=3 with no intervention. Read-only verification, logged under
    // MC2_MECH_VIEWUNIFORMS_DIAG. (The original "block missing / bound to 0"
    // symptom was a deploy gap: the edited shader was never copied to the
    // deploy dir — see docs/mech-viewuniforms-source-dump.md.)
    if (s_mechViewUniforms && s_mechViewUniformsDiag) {
        GLuint vuBlockIdx = glGetUniformBlockIndex(s_mechProgram, "ViewUniformsBlock");
        GLint vuBind = -1;
        if (vuBlockIdx != GL_INVALID_INDEX)
            glGetActiveUniformBlockiv(s_mechProgram, vuBlockIdx,
                                      GL_UNIFORM_BLOCK_BINDING, &vuBind);
        std::fprintf(stderr,
            "[MECH_VU_DIAG v1] event=block_verify prog=%u vu_block_idx=%u "
            "binding=%d (expect 3) loc_u_worldToClipGL=%d\n",
            s_mechProgram, vuBlockIdx, vuBind, s_loc_terrainMVP);
    }

    std::fprintf(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management
// ---------------------------------------------------------------------------
static void ensureRingCapacity(size_t neededInstances, size_t neededBones) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededBones     > s_boneCapacity;
    if (!needGrow) return;

    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);     s_boneSsbo     = 0; s_boneMap     = nullptr; }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : kInitialInstancesPerFrame);
    s_boneCapacity = std::max(neededBones,
        s_boneCapacity ? s_boneCapacity * 2 : kInitialBonesPerFrame);

    // SSBO-BIND-ALIGN: the per-slot bind offset is slot * capacity * sizeof. It
    // MUST be a multiple of GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (256 on
    // NVIDIA) or slots 1+ bind at a misaligned offset -> NVIDIA rejects with
    // GL_INVALID_VALUE -> the instance/bone SSBO never binds -> the VS reads
    // garbage -> mechs invisible/garbled (AMD tolerated it). Rounding capacity up
    // to a multiple of the alignment (in ELEMENTS) makes capacity*sizeof a multiple
    // of the alignment, so every ring-slot offset (and the alloc + CPU write
    // pointer, which also derive from capacity) is aligned. Element-count rounding
    // avoids the lossy byte/sizeof division when sizeof does not divide 256.
    {
        const unsigned long long a = (unsigned long long)gpuSsboOffsetAlignment();
        s_instanceCapacity = (size_t)gpuAlignUp((unsigned long long)s_instanceCapacity, a);
        s_boneCapacity     = (size_t)gpuAlignUp((unsigned long long)s_boneCapacity,     a);
    }

    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)),
        nullptr, flags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)), flags);

    glGenBuffers(1, &s_boneSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_boneSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)),
        nullptr, flags);
    s_boneMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)), flags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_boneMap) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=persistent_map_fail\n");
    }
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=ring_alloc instances=%zu bones=%zu\n",
        s_instanceCapacity, s_boneCapacity);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GpuMechBatcher& GpuMechBatcher::instance() {
    static GpuMechBatcher batcher;
    return batcher;
}

void GpuMechBatcher::onMapLoad() {
    // M2.5 (Q4): per-mission writer counter; emitted on onMapUnload.
    s_gpuMechIdWritesThisMission = 0;
    s_typeLodRecords.clear();
    s_packets.clear();
    s_typeLodIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_nodeVboRanges.clear();   // MECH-NORMALS-FIX-1: per-node ranges are per-mission
    s_geometryFinalized = false;
    // S2.12: DO NOT reset s_programLoadTried / s_programLoadFailed here.
    // The GL program created by loadProgramsIfNeeded() is process-global
    // state cached in shader_builder's s_programs map under the name "mech".
    // Resetting these flags causes loadProgramsIfNeeded() to call
    // glsl_program::makeProgram("mech", ...) again on the second map load,
    // which the shader_builder cache rejects with
    // "Program with this name (mech) already exists" — disabling the GPU
    // mech path on every load after the first. Program creation parameters
    // (prefix derived from RenderWorld::IsObjectIdBufferEnabled() etc.) do
    // not change per-mission, so a one-shot first-load is correct.
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    // Clear persisted shadow state so flushShadow() finds no stale entries
    // that index into the now-cleared s_packets.  Order-independent vs the
    // !s_instanceSsbo guard in flushShadow().
    s_lastDrawCalls.clear();
    s_lastTotalInstances = 0;
    s_lastTotalBones     = 0;
    // Mech-1: reset MaterialGpu SSBO and tables on map load
    if (s_mechMaterialSsbo != 0) {
        glDeleteBuffers(1, &s_mechMaterialSsbo);
        s_mechMaterialSsbo = 0;
    }
    s_mechMaterialTable.clear();
    s_mechHandleToMaterialIdx.clear();
    s_mechDrawMaterialIdx.clear();
    s_mechMaterialGpuFrameCount = 0;
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_load\n");
}

void GpuMechBatcher::onMapUnload() {
    // M2.5 (Q6 amendment 2): consume the MLR-side per-mission counter
    // and emit on its own [MECHBATCHER v1] event=mlr_mech_summary line.
    // The split-line shape is OFFICIAL (external-review M1): the two
    // counters live in different TUs and MUST emit on two adjacent
    // lines (mlr first, then gpu_mech_id). Do not collapse to one line.
    // consumeAndResetMlrMechDraws is forward-declared at file scope
    // (external-review C1) -- declaration is NOT inside this function body.
    const uint64_t mlrDraws = consumeAndResetMlrMechDraws();
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=%llu\n",
        (unsigned long long)mlrDraws);

    // M2.5 (Q4 + Q6 amendment 2): always-on per-mission writer summary.
    // Surfaces gpu_mech_id_writes to the M2.6 readiness decision rule.
    // Always-on (NOT env-gated): M2.6 needs this signal regardless of
    // MC2_OBJECT_ID_BUFFER state to size the MLR-vs-GPU split.
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=%llu\n",
        (unsigned long long)s_gpuMechIdWritesThisMission);
    s_gpuMechIdWritesThisMission = 0;
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
    s_mechExtractPersist.clear();  // MECH-EXTRACTION-0
    // Mech-1: teardown MaterialGpu SSBO and tables
    if (s_mechMaterialSsbo != 0) {
        glDeleteBuffers(1, &s_mechMaterialSsbo);
        s_mechMaterialSsbo = 0;
    }
    s_mechMaterialTable.clear();
    s_mechHandleToMaterialIdx.clear();
    s_mechDrawMaterialIdx.clear();
    // Slice C1: release named material profile SSBO (binding 7).
    // shutdown() resets s_initialized so init() can be called again on next map load.
    // Also reset the lazy-resolve guard so the profile index is re-queried next mission.
    gos_materials::shutdown();
    s_mechSurfaceMaterialIdx         = 0u;
    s_mechSurfaceMaterialIdxResolved = false;
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }
bool GpuMechBatcher::isFinalized() const                    { return s_geometryFinalized; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[(int)r]; }
void GpuMechBatcher::flushShadow() {
    // Depth-only draw of the previous frame's mech instances into the dynamic
    // shadow FBO.  Called from Task-5's gos_BeginDynamicShadowPass region,
    // BEFORE this frame's flush().
    //
    // Ring-slot reasoning (verified against flush() line numbers):
    //   flush():913  s_frameSlot = (s_frameSlot+1) % MECH_RING_FRAMES  -- advance
    //   flush():920  SSBO written to new s_frameSlot                    -- write
    //   flush():1190 s_fence[s_frameSlot] = glFenceSync(...)            -- fence
    // flushShadow() runs before this frame's flush(), so s_frameSlot still
    // holds the PREVIOUS frame's post-advance value.  The previous frame's
    // data lives at s_frameSlot and was already fenced by the previous
    // flush():1097.  Read slot = s_frameSlot.  No advance / write / fence here.
    //
    // Bucket-reuse: Option B (persisted drawCalls).  flush()'s local DrawCall
    // vector is entangled with the ring write inside the same function, so we
    // persist it to s_lastDrawCalls / s_lastTotalInstances / s_lastTotalBones
    // at the end of flush() and consume those statics here.

    // DEFAULT ON. Kill-switch: MC2_SHADOW_ENABLE=0.
    static const bool s_shadowEnabled = !(getenv("MC2_SHADOW_ENABLE") != nullptr &&
                                          getenv("MC2_SHADOW_ENABLE")[0] == '0');
    if (!s_shadowEnabled) return;

    // Geometry-readiness guard, mirroring the color flush() path (:867).
    // The new txmmgr shadow region calls this from frame ~1 and across
    // mid-mission finalizePending() rebuilds; without this, a not-yet-
    // finalized or rebuilt s_packets is indexed by stale persisted
    // s_lastDrawCalls entries and faults in the draw below.
    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return;
    if (s_lastDrawCalls.empty()) return;
    if (!s_instanceSsbo || !s_boneSsbo) return;

    // Resolve shadow_mech program via the global registry (Task 1 registered
    // it under this key via makeProgram("shadow_mech", ...)).
    auto pit = glsl_program::s_programs.find("shadow_mech");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return;
    const GLuint shadowProg = pit->second->shp_;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Save/restore the GL state this pass perturbs, mirroring flush()'s
    // bracket (:1036-1043 / :1209-1218). Leaking program/VAO/element-
    // buffer/SSBO bindings 0+1 here poisons gpu_cull::compute_dispatch()
    // and the indirect flush() that run after -> invisible mechs+props.
    // All early returns above precede any GL mutation.
    GLint prevProgram = 0, prevVao = 0, prevElemBuf = 0, prevSsbo0 = 0, prevSsbo1 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &prevSsbo1);

    glUseProgram(shadowProg);

    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    const GLint smLoc   = glGetUniformLocation(shadowProg, "u_skinningMode");
    const GLint baseLoc = glGetUniformLocation(shadowProg, "u_instanceBase");

    if (smLoc >= 0)
        glUniform1i(smLoc, g_useGpuMechSkin ? 1 : 0);

    glBindVertexArray(s_sharedVao);
    // Same root cause as GpuStaticPropBatcher::flushShadow: do not rely on
    // s_sharedVao carrying the IBO (other GPU paths clobber its VAO-resident
    // GL_ELEMENT_ARRAY_BUFFER binding); bind s_sharedIbo explicitly so the
    // indexed draw never treats firstIndex*4 as a client pointer.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Read the already-fenced previous-frame SSBO slot.
    const uint32_t slot = s_frameSlot;
    gpuBindSsboRange(0, s_instanceSsbo,
        (long long)(slot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (long long)(s_lastTotalInstances * sizeof(GpuMechInstance)),
        "mech.instance");
    gpuBindSsboRange(1, s_boneSsbo,
        (long long)(slot * s_boneCapacity * sizeof(GpuMechBone)),
        (long long)(s_lastTotalBones * sizeof(GpuMechBone)),
        "mech.bone");

    // GPU-SYNC-CONTRACT: same coherent instance/bone SSBOs feed the shadow draws;
    // order the CPU writes before these reads too (see flush() Step 7).
    gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                   "mech_instance_bone_shadow");

    int typesDrawn = 0, instDrawn = 0, skipped = 0;
    for (const ShadowDrawEntry& dc : s_lastDrawCalls) {
        if (baseLoc >= 0)
            glUniform1i(baseLoc, (int)dc.instanceBase);

        // Skip (not break) a truly-stale index so one bad entry post-rebuild
        // does not drop the remaining (valid) draws -> missing parts.
        if (dc.globalPacketIdx >= s_packets.size()) { ++skipped; continue; }
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];
        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++typesDrawn;
        instDrawn += (int)dc.instanceCount;
    }

    s_shadowTypesDrawn = typesDrawn;
    s_shadowInstDrawn  = instDrawn;

    static const bool s_casterDiag = (getenv("MC2_SHADOW_CASTER_DIAG") != nullptr);
    if (s_casterDiag) {
        std::fprintf(stderr,
            "[SHADOW_CASTER] drawList=%zu submitted=%d skipped=%d inst=%d packets=%zu slot=%u\n",
            s_lastDrawCalls.size(), typesDrawn, skipped, instDrawn,
            s_packets.size(), slot);
    }

    // Restore exactly what we changed (mirrors flush()'s restore bracket)
    // so the cull dispatch + indirect flush() that follow are not poisoned.
    // ORDER MATTERS: restore VAO before element-buffer so glBindBuffer(ELEM)
    // writes into prevVao's state, not s_sharedVao's. Wrong order leaves
    // s_sharedVao.elemBuf=0 and flush() faults on the next indexed draw.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glBindVertexArray((GLuint)prevVao);                          // VAO first
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf); // then elem
    glUseProgram((GLuint)prevProgram);
}

// ---------------------------------------------------------------------------
// Registration (Task 4)
// ---------------------------------------------------------------------------

// MECH-NORMALS-FIX-1: recompute normals for one node's triangle-soup vertices
// in-place, mech-locally, over the byte range [startByte, endByte). The verts
// are a triangle list (corners 3k,3k+1,3k+2 form triangle k), all in mech model
// space — same space as the cooked normal the shader already consumes, so no
// axis swap here. mode 1 = geometric face normal to all 3 corners (faceted).
// mode 2 = exact-position-grouped, angle-threshold accumulation that preserves
// hard edges (faces meeting at > smoothDeg do not blend). Grouping is exact on
// source position bits (shared verts have bit-identical TG positions) and stays
// within this node (separate rigid mech pieces are never blended). Face normals
// are oriented to the cooked-normal hemisphere to stay winding-agnostic.
// endByte: exclusive end of this node's data in vbo; use vbo.size() for
// "rest of buffer". Added for per-node bounding (MECH-NORMALS-FIX-1 rebuild).
static void recomputeMechNodeNormals(std::vector<uint8_t>& vbo,
                                     size_t startByte, size_t endByte,
                                     int mode, float smoothDeg) {
    if (mode != 1 && mode != 2) return;
    const size_t stride = sizeof(GpuMechVertex);
    if (startByte >= endByte || startByte >= vbo.size()) return;
    const size_t clampedEnd = (endByte < vbo.size()) ? endByte : vbo.size();
    const size_t n = (clampedEnd - startByte) / stride;   // corners
    if (n < 3) return;
    GpuMechVertex* V = reinterpret_cast<GpuMechVertex*>(vbo.data() + startByte);
    const size_t triCount = n / 3;

    auto dot3 = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };

    // Per-triangle area-weighted geometric normal (un-normalized cross), plus a
    // unit copy for the angle test. Oriented to agree with the cooked-normal
    // hemisphere (sum of the 3 corners' cooked normals) to handle winding.
    std::vector<std::array<float,3>> faceRaw(triCount), faceUnit(triCount);
    for (size_t k = 0; k < triCount; ++k) {
        const float* p0 = V[3*k+0].position;
        const float* p1 = V[3*k+1].position;
        const float* p2 = V[3*k+2].position;
        float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        float fr[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                        e1[2]*e2[0]-e1[0]*e2[2],
                        e1[0]*e2[1]-e1[1]*e2[0] };
        float ref[3] = {
            V[3*k+0].normal[0] + V[3*k+1].normal[0] + V[3*k+2].normal[0],
            V[3*k+0].normal[1] + V[3*k+1].normal[1] + V[3*k+2].normal[1],
            V[3*k+0].normal[2] + V[3*k+1].normal[2] + V[3*k+2].normal[2] };
        if (dot3(fr, ref) < 0.0f) { fr[0]=-fr[0]; fr[1]=-fr[1]; fr[2]=-fr[2]; }
        faceRaw[k] = { fr[0], fr[1], fr[2] };
        float m = std::sqrt(dot3(fr, fr));
        float inv = (m > 1e-12f) ? 1.0f/m : 0.0f;
        faceUnit[k] = { fr[0]*inv, fr[1]*inv, fr[2]*inv };
    }

    if (mode == 1) {
        for (size_t k = 0; k < triCount; ++k)
            for (int c = 0; c < 3; ++c) {
                V[3*k+c].normal[0] = faceUnit[k][0];
                V[3*k+c].normal[1] = faceUnit[k][1];
                V[3*k+c].normal[2] = faceUnit[k][2];
            }
        return;
    }

    // mode 2: group corners by exact source position (3 float-bit key), then for
    // each corner accumulate area-weighted face normals of same-position corners
    // whose face is within smoothDeg of this corner's face (hard-edge preserving).
    const float cosT = std::cos(smoothDeg * 3.14159265358979f / 180.0f);
    std::map<std::array<uint32_t,3>, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        std::array<uint32_t,3> kkey{};
        std::memcpy(kkey.data(), V[i].position, 12);
        groups[kkey].push_back(i);
    }
    std::vector<std::array<float,3>> out(n);
    for (auto& kv : groups) {
        const std::vector<uint32_t>& g = kv.second;
        for (uint32_t i : g) {
            const size_t ti = i / 3;
            float acc[3] = {0,0,0};
            for (uint32_t j : g) {
                const size_t tj = j / 3;
                if (dot3(faceUnit[ti].data(), faceUnit[tj].data()) >= cosT) {
                    acc[0] += faceRaw[tj][0];
                    acc[1] += faceRaw[tj][1];
                    acc[2] += faceRaw[tj][2];
                }
            }
            float m = std::sqrt(dot3(acc, acc));
            if (m > 1e-12f) { out[i] = { acc[0]/m, acc[1]/m, acc[2]/m }; }
            else            { out[i] = faceUnit[ti]; }  // degenerate fallback
        }
    }
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        V[i].normal[0] = out[i][0];
        V[i].normal[1] = out[i][1];
        V[i].normal[2] = out[i][2];
    }
}

void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType* mechType, int lod) {
    if (!mechType) return;
    const TypeLodKey key{mechType, lod};
    if (s_typeLodIndex.count(key)) return;  // idempotent
    static const bool s_mechRestoreTrace = (getenv("MC2_MECH_RESTORE_TRACE") != nullptr);
    if (s_geometryFinalized) {
        // Late registration (post-finalizeGeometry). DO NOT drop: stage
        // the type into the retained s_stagingVbo/Ibo (fall through to
        // the body below) and mark pending so finalizePending() rebuilds
        // the shared buffers. Caller MUST invoke finalizePending() once
        // its late-spawn batch completes (VPL-#11: logistics.cpp SP
        // force-group loop). Until then submitActor still fast-rejects
        // this type (s_typeLodIndex has the entry but the GL buffers
        // predate it) -- harmless because no frame renders between the
        // late registers and the finalizePending() call on that path.
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=late_register type=%p lod=%d (staged_pending)\n",
            (void*)mechType, lod);
        if (s_mechRestoreTrace)
            std::fprintf(stderr,
                "[MECHRESTORE v1] event=register type=%p lod=%d result=staged_pending finalized=1\n",
                (void*)mechType, lod);
        s_pendingLateTypes = true;
        ++s_lateStagedCount;
        // fall through -- stage like a normal pre-finalize registration
    }

    TG_TypeMultiShape* typeMulti = mechType->mechShape[lod];
    if (!typeMulti) return;

    const int numNodes = typeMulti->GetNumShapes();
    if (numNodes == 0 || numNodes > 255) {
        if (numNodes > 255) {
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=u8_bone_overflow type=%p lod=%d numNodes=%d\n",
                (void*)mechType, lod, numNodes);
        }
        return;
    }

    static const bool s_nodeTrace = (getenv("MC2_MECH_NODE_TRACE") != nullptr);
    if (s_nodeTrace) {
        std::fprintf(stderr, "[MECHREG v1] event=register type=%p lod=%d numBones=%d\n",
                     (void*)mechType, lod, numNodes);
    }

    const uint32_t typeLodIdx = (uint32_t)s_typeLodRecords.size();
    s_typeLodIndex[key] = typeLodIdx;
    if (s_mechRestoreTrace)
        std::fprintf(stderr,
            "[MECHRESTORE v1] event=register type=%p lod=%d result=registered finalized=0 numBones=%d\n",
            (void*)mechType, lod, numNodes);

    GpuMechTypeLodRecord rec{};
    rec.firstBoneIndex = 0;
    rec.numBones       = (uint32_t)numNodes;
    rec.firstPacket    = (uint32_t)s_packets.size();
    rec.packetCount    = 0;
    rec.vertexCount    = 0;
    rec.sourceNode0    = nullptr;

    for (int nodeIdx = 0; nodeIdx < numNodes; ++nodeIdx) {
        TG_TypeNodePtr tnode = typeMulti->GetTypeNode(nodeIdx);
        if (!tnode || tnode->GetNodeType() != SHAPE_NODE) continue;
        TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(tnode);
        if (nodeIdx == 0) rec.sourceNode0 = typeShape;

        // Skip spotlight leaves: TG_Shape::isSpotlight is set on the
        // per-instance shape from "SpotLight_*" node name prefix at
        // tgl.cpp:259/475. Post-T3.1 ((E) SpotLight_ -> real illumination),
        // mech spotlight child shapes are emitted as real TG_Light
        // registrations via Mech3DAppearance::updateGeometry (T1.6/T1.7,
        // commit fceb304). Skip the geometry emission here — the cone is
        // no longer drawn, only the light contribution lives downstream.
        // See docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md.
        const char* nodeName = tnode->getNodeId();
        if (nodeName && S_strnicmp(nodeName, "SpotLight_", 10) == 0) {
            if (s_nodeTrace) {
                std::fprintf(stderr,
                    "[MECHREG v1] event=skip_spotlight type=%p lod=%d nodeIdx=%d name=%s\n",
                    (void*)mechType, lod, nodeIdx, nodeName);
            }
            // [SPOTLIGHT_REAL_TRACE v1] T0.2 (a) — record SpotLight_ TypeNode
            // observed at recipe build. Tracks max across LODs since each
            // instance only renders at one LOD; the running submitActor counter
            // (b) below cares about whether the actor has ANY spotlight nodes,
            // and lazy-registration in T1.6 walks the live mechShape regardless
            // of LOD layout.
            ++s_spotlightReal_typeNodeEvents;
            const uint32_t newCount = s_spotlightRealTypeNodeCount[mechType] + 1;
            s_spotlightRealTypeNodeCount[mechType] = newCount;
            if (!s_spotlightReal_firstHitTypeNode) {
                s_spotlightReal_firstHitTypeNode = true;
                std::fprintf(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_recipe_build "
                    "type=%p lod=%d nodeIdx=%d name=%s\n",
                    (const void*)mechType, lod, nodeIdx, nodeName);
                std::fflush(stderr);
            }
            if (s_spotlightRealTrace) {
                std::fprintf(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=type_spotlight_node "
                    "type=%p lod=%d nodeIdx=%d name=%s type_node_count=%u\n",
                    (const void*)mechType, lod, nodeIdx, nodeName, newCount);
            }
            continue;
        }

        if (!typeShape->numTypeTriangles || !typeShape->listOfTypeTriangles ||
            !typeShape->listOfTypeVertices) continue;

        const int32_t baseVertex = (int32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex));

        // Group triangles by localTextureHandle (same as static prop batcher).
        const uint32_t numTris = typeShape->numTypeTriangles;
        uint32_t runStart = 0;
        while (runStart < numTris) {
            const DWORD runTexSlot =
                typeShape->listOfTypeTriangles[runStart].localTextureHandle;
            uint32_t runEnd = runStart;
            while (runEnd < numTris &&
                   typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTexSlot)
                ++runEnd;

            const uint32_t packetFirstIndex = (uint32_t)s_stagingIbo.size();

            for (uint32_t t = runStart; t < runEnd; ++t) {
                const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
                const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
                const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

                for (int c = 0; c < 3; ++c) {
                    const TG_TypeVertex& src =
                        typeShape->listOfTypeVertices[tri.Vertices[c]];

                    GpuMechVertex vert{};
                    std::memcpy(vert.position, &src.position.x, 12);
                    std::memcpy(vert.normal,   &src.normal.x,   12);
                    vert.uv[0] = cornerU[c];
                    vert.uv[1] = cornerV[c];
                    // boneIndices: .x = nodeIdx (rigid, Slice A), .yzw = 0
                    vert.boneIndices[0] = (uint8_t)(nodeIdx & 0xFF);
                    vert.boneIndices[1] = 0;
                    vert.boneIndices[2] = 0;
                    vert.boneIndices[3] = 0;
                    // boneWeights: .x = 255 (= 1.0 normalized), .yzw = 0
                    vert.boneWeights[0] = 255;
                    vert.boneWeights[1] = 0;
                    vert.boneWeights[2] = 0;
                    vert.boneWeights[3] = 0;
                    // tangentOct: zero-fill for stock (no .tglgpu sidecar)
                    vert.tangentOct[0] = 0;
                    vert.tangentOct[1] = 0;
                    vert.aRGBLight = src.aRGBLight;

                    s_stagingVbo.insert(s_stagingVbo.end(),
                        reinterpret_cast<uint8_t*>(&vert),
                        reinterpret_cast<uint8_t*>(&vert) + sizeof(GpuMechVertex));

                    // Write index LOCAL to this packet's baseVertex.
                    // glDrawElementsInstancedBaseVertex adds pkt.baseVertex at draw time,
                    // so IBO must contain (globalVertex - baseVertex), not globalVertex.
                    const uint32_t localIdx =
                        (uint32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex) - 1u)
                        - (uint32_t)baseVertex;
                    s_stagingIbo.push_back(localIdx);
                    ++rec.vertexCount;
                }
            }

            // Derive ALPHA_TEST_BIT from the texture slot's textureAlpha flag.
            uint32_t matFlags = 0;
            if (runTexSlot < (DWORD)typeShape->numTextures &&
                typeShape->listOfTextures[runTexSlot].textureAlpha) {
                matFlags = 1u;  // ALPHA_TEST_BIT (matches mech.frag ALPHA_TEST_BIT constant)
            }

            GpuMechPacket pkt{};
            pkt.firstIndex          = packetFirstIndex;
            pkt.indexCount          = (runEnd - runStart) * 3;
            pkt.baseVertex          = baseVertex;
            pkt.textureSlot         = (uint32_t)runTexSlot;
            pkt.materialFlags       = matFlags;
            pkt.owningTypeLodRecord = typeLodIdx;
            pkt.nodeLocalIndex      = (uint32_t)nodeIdx;
            pkt.owningTypeShape     = typeShape;
            s_packets.push_back(pkt);
            ++rec.packetCount;

            runStart = runEnd;
        }

        // MECH-NORMALS-FIX-1: record this node's byte range in s_stagingVbo
        // so uploadMechGeometryVbo() can apply recomputeMechNodeNormals per-node
        // on a transient copy (preserving s_stagingVbo as pristine cooked data).
        // Registration no longer mutates normals; the recompute runs at upload time.
        {
            const size_t nodeStart = (size_t)baseVertex * sizeof(GpuMechVertex);
            const size_t nodeEnd   = s_stagingVbo.size();
            s_nodeVboRanges.push_back({nodeStart, nodeEnd});
        }
    }

    s_typeLodRecords.push_back(rec);
}

// MECH-NORMALS-FIX-1: Create (or recreate) the immutable VBO from s_stagingVbo,
// applying the current s_mechNormalsMode/s_mechNormalsSmoothDeg on a transient
// copy so s_stagingVbo stays pristine (cooked normals). Also sets up all 7 vertex
// attribute pointers on the already-bound VAO (s_sharedVao must be bound by caller).
//
// VBO recreation:
//   - If s_sharedVbo already exists (rebuild path): delete it first (glBufferStorage
//     buffers are immutable — they CANNOT be resized or re-uploaded in place).
//   - Allocate a new s_sharedVbo with glGenBuffers + glBufferStorage.
//   - Re-run glVertexAttribPointer for all 7 attribs, which bind to the currently
//     bound GL_ARRAY_BUFFER. The element-array binding (s_sharedIbo) is VAO state
//     and is NOT disturbed by rebinding GL_ARRAY_BUFFER or by glVertexAttribPointer
//     calls — the VAO already has s_sharedIbo bound from finalizeGeometry, and the
//     standard guarantees that GL_ELEMENT_ARRAY_BUFFER VAO state is only changed by
//     explicit glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...) while a VAO is bound.
//     Therefore the element binding is preserved across VBO replacement and is safe
//     to skip re-binding here.
//
// Called from: finalizeGeometry() (initial) and batcher_rebuildMechNormals() (live).
static void uploadMechGeometryVbo() {
    // Build transient work copy of the staging VBO (s_stagingVbo = pristine cooked).
    // mode 0 means no recompute — work is byte-identical to cooked.
    std::vector<uint8_t> work = s_stagingVbo;

    if (s_mechNormalsMode != 0) {
        // Apply recompute per-node using the recorded per-node byte ranges.
        // Each range is [start, end) in work; recomputeMechNodeNormals modifies
        // work in place over that range only (bounded by the endByte param added
        // in MECH-NORMALS-FIX-1 so normals from adjacent nodes are never blended).
        for (const auto& rng : s_nodeVboRanges) {
            recomputeMechNodeNormals(work, rng.first, rng.second,
                                     s_mechNormalsMode, s_mechNormalsSmoothDeg);
        }
    }

    // Delete the old VBO if this is a rebuild (immutable buffer must be re-created).
    if (s_sharedVbo != 0) {
        glDeleteBuffers(1, &s_sharedVbo);
        s_sharedVbo = 0;
    }

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    (GLsizeiptr)work.size(),
                    work.data(), 0);  // immutable, GPU-only

    // Vertex attribute setup — 48-byte GpuMechVertex, 7 attributes.
    // glVertexAttribPointer binds each attrib to the currently bound GL_ARRAY_BUFFER
    // (s_sharedVbo just bound above). The VAO's GL_ELEMENT_ARRAY_BUFFER binding
    // (s_sharedIbo) is a separate piece of VAO state and is not disturbed here.
    auto enableF = [](GLuint loc, GLint sz, GLenum type, GLboolean norm,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, sz, type, norm, stride, (void*)offset);
    };
    auto enableI = [](GLuint loc, GLint sz, GLenum type,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, sz, type, stride, (void*)offset);
    };

    const GLsizei S = (GLsizei)sizeof(GpuMechVertex);
    enableF(0, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, position));
    enableF(1, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, normal));
    enableF(2, 2, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, uv));
    enableI(3, 4, GL_UNSIGNED_BYTE,            S, offsetof(GpuMechVertex, boneIndices));
    enableF(4, 4, GL_UNSIGNED_BYTE,  GL_TRUE,  S, offsetof(GpuMechVertex, boneWeights));
    enableF(5, 2, GL_SHORT,          GL_TRUE,  S, offsetof(GpuMechVertex, tangentOct));
    enableI(6, 1, GL_UNSIGNED_INT,             S, offsetof(GpuMechVertex, aRGBLight));
}

void GpuMechBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;
    loadProgramsIfNeeded();
    // Slice C1: init named material profile SSBO (binding 7) once per process.
    // Idempotent; requires GL context (already active at finalizeGeometry call site).
    gos_materials::init();

    // Bail cleanly if shader failed: geometry upload skipped.
    // submit() fast-rejects on s_geometryFinalized==false.
    if (s_programLoadFailed) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_skip reason=shader_fail\n");
        return;
    }

    if (s_stagingVbo.empty()) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_empty — no types registered\n");
        s_geometryFinalized = true;
        return;
    }

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    // VBO creation + vertex attrib setup delegated to uploadMechGeometryVbo()
    // so batcher_rebuildMechNormals() can re-run just the VBO part without
    // recreating the VAO, IBO, or sampler.
    uploadMechGeometryVbo();

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(), 0);

    glBindVertexArray(0);

    // Session-lifetime sampler: GL_REPEAT / GL_LINEAR.
    // GL_LINEAR (not GL_LINEAR_MIPMAP_LINEAR) because mech textures may not
    // have mipmaps generated — sampling a mipmap chain that doesn't exist
    // is undefined behavior on AMD, often black. Slice A+ can revisit if
    // mech textures get mipmaps from the upscaler pipeline.
    glGenSamplers(1, &s_sampler);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Staging RETAINED (not cleared/shrunk) so finalizePending() can
    // append a late type and rebuild from the full original+late
    // staging. See s_stagingVbo declaration. Cleared only at onMapLoad().

    s_geometryFinalized = true;
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=finalize_ok types=%zu packets=%zu\n",
        s_typeLodRecords.size(), s_packets.size());
}

// VPL-#11 SP-logistics fix. Rebuild the immutable shared VBO/IBO/VAO so
// types registered AFTER a prior finalizeGeometry() (campaign .fit
// resume spawns the player force-group post-Mission::init-finalize, via
// logistics.cpp addMover) become drawable. No-op unless a late type was
// staged. SAFE because: (a) no frame renders between the late registers
// and this call on the SP path (adversarial-review grep-closed:
// GpuMechBatcher::flush only via renderLists from the frame loop, after
// logistics.cpp mission->update()); (b) glBufferStorage buffers are
// immutable so they MUST be deleted before recreate; (c) s_typeLodIndex
// /s_typeLodRecords/s_packets are append-only and preserved here, so the
// ~pre-finalize types keep stable indices and the retained staging holds
// their original-offset vertices + the appended late ones.
void GpuMechBatcher::finalizePending() {
    if (!s_pendingLateTypes) return;

    // VPL-#11 shadow-regression hardening (2026-05-16): finalizePending
    // runs mid-mission-setup; save EVERY GL binding it perturbs and
    // restore on exit so it is provably state-neutral to the rest of the
    // frame (vulkan_prep_explicit_device_discipline). The mech advisor
    // ruled GL-state OUT as the half-map-shadow cause (left-bound buffer
    // is overwritten before first render), but this removes it as a
    // variable definitively and is correct discipline regardless.
    GLint prevVao = 0, prevArr = 0, prevElem = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,        &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,        &prevArr);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&prevElem);

    // Drain ring fences before deleting GL objects (defensive; mirrors
    // onMapUnload). No draw can be in flight here per (a), so this is
    // belt-and-suspenders, not load-bearing.
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    // Delete the immutable shared geometry objects finalizeGeometry()
    // glGen'd; it will recreate all four from the retained staging.
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo);      s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo);      s_sharedIbo = 0; }
    if (s_sampler)   { glDeleteSamplers(1, &s_sampler);       s_sampler   = 0; }

    const uint32_t lateAdded = s_lateStagedCount;
    s_lateStagedCount   = 0;
    s_pendingLateTypes  = false;
    s_geometryFinalized = false;   // re-open the guard so finalizeGeometry runs
    finalizeGeometry();            // rebuilds VBO/IBO/VAO from full retained staging

    // The rebuild above repopulates s_packets with new indices/order, so
    // the persisted shadow draw list (pre-rebuild globalPacketIdx values)
    // is now stale. Clear it (mirrors onMapLoad); the next flush() will
    // repopulate it consistently with the rebuilt s_packets. flushShadow()
    // early-returns on empty s_lastDrawCalls until then.
    s_lastDrawCalls.clear();
    s_lastTotalInstances = 0;
    s_lastTotalBones     = 0;

    // Restore the bindings finalizeGeometry left dirty (it ends on
    // glBindVertexArray(0) with GL_ARRAY_BUFFER still = the new s_sharedVbo).
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER,         (GLuint)prevArr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElem);

    std::fprintf(stderr,
        "[MECHBATCHER v1] event=finalize_pending types=%zu lateAdded=%u rebuilt=%d\n",
        s_typeLodRecords.size(), lateAdded, s_geometryFinalized ? 1 : 0);
    if (getenv("MC2_MECH_RESTORE_TRACE") != nullptr)
        std::fprintf(stderr,
            "[MECHRESTORE v1] event=finalize_pending types=%zu lateAdded=%u rebuilt=%d\n",
            s_typeLodRecords.size(), lateAdded, s_geometryFinalized ? 1 : 0);
}

// ---------------------------------------------------------------------------
// submitActor (Task 6) — bone staging + texture capture
// ---------------------------------------------------------------------------
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return false;
    if (!desc.mechShape || !desc.mechType) return false;

    const TypeLodKey key{desc.mechType, desc.currentLOD};
    auto it = s_typeLodIndex.find(key);
    if (it == s_typeLodIndex.end()) {
        s_lastFailWasLateReg = true;
        ++s_disallowedLateRegEvents;
        return false;
    }
    const uint32_t typeLodIdx = it->second;
    const GpuMechTypeLodRecord& rec = s_typeLodRecords[typeLodIdx];

    PendingSubmit ps;
    ps.desc       = desc;
    ps.typeLodIdx = typeLodIdx;
    ps.bones.reserve(rec.numBones);

    // Stage bone matrices from live shapeToWorld (set by TransformMultiShape).
    // listOfShapes[i].shapeToWorld is a Stuff::LinearMatrix4D with entries[12]
    // stored column-major: entries[(col<<2)+row], 3 explicit cols + implicit col3=[0,0,0,1].
    // Row k extraction: [entries[k], entries[4+k], entries[8+k], w] where w=1 for row3 only.
    const int numShapes = desc.mechShape->GetNumShapes();
    for (int i = 0; i < numShapes && i < (int)rec.numBones; ++i) {
        const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
        const float* e = (const float*)sr.shapeToWorld.entries;
        GpuMechBone bone;
        bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
        bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
        bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
        bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
        ps.bones.push_back(bone);
    }
    while ((int)ps.bones.size() < (int)rec.numBones) {
        GpuMechBone id{};
        id.row0[0]=1.f; id.row1[1]=1.f; id.row2[2]=1.f; id.row3[3]=1.f;
        ps.bones.push_back(id);
    }

    // Capture live per-actor texture handle for each packet.
    //
    // SLOT 0 is per-actor (paint scheme / team color). TG_TypeShape::listOfTextures
    // is a shared type-level cache mutated by TransformMultiShape — by render time
    // it reflects the LAST actor through, not the current one. Use desc.slot0TexHandle
    // (the raw gos handle passed by the caller) directly for slot 0.
    //
    // SLOTS 1+ are type-stable; reading from owningTypeShape is correct.
    ps.packetTexHandles.resize(rec.packetCount, 0);
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.owningTypeShape && pkt.owningTypeShape->listOfTextures &&
                   pkt.textureSlot < (uint32_t)pkt.owningTypeShape->numTextures) {
            ps.packetTexHandles[p] =
                pkt.owningTypeShape->listOfTextures[pkt.textureSlot].gosTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — per-instance counter. Fires once
    // per submitActor when the actor's mechType has at least one SpotLight_
    // TypeNode (recorded at registerTypeLod time). Per-frame increment is
    // unconditional; 600-frame summary is env-gated in flush() below.
    if (s_spotlightRealTypeNodeCount.find(desc.mechType)
            != s_spotlightRealTypeNodeCount.end()) {
        ++s_spotlightReal_drawWindow;
        ++s_spotlightReal_drawMono;
        if (!s_spotlightReal_firstHitDraw) {
            s_spotlightReal_firstHitDraw = true;
            std::fprintf(stderr,
                "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_submit "
                "type=%p lod=%d\n",
                (const void*)desc.mechType, desc.currentLOD);
            std::fflush(stderr);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush (Task 7) — bucket-sorted compaction + draw loop
// ---------------------------------------------------------------------------
void GpuMechBatcher::flush() {
    finalizePending();

    // V1A: latch BEFORE any early-return guard clears s_pendingSubmits.
    // Counts GpuMechBatcher submits only (not MLR fallback, not alive count).
    s_lastFlushSubmitCount = static_cast<uint64_t>(s_pendingSubmits.size());

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — periodic summary at 600-frame
    // cadence. Frame counter advances every flush() regardless of env or
    // pending submits, so the cadence stays stable across missions. Window
    // counter resets after emit; mono is monotonic.
    ++s_spotlightReal_drawFrameCount;
    const bool spotlightRealPeriodic =
        (s_spotlightReal_drawFrameCount % 600 == 0 && s_spotlightReal_drawFrameCount > 0);
    if (s_spotlightRealTrace && spotlightRealPeriodic) {
        std::fprintf(stderr,
            "[SPOTLIGHT_REAL_TRACE v1] event=summary site=mech_submit "
            "frames=%llu window_mech_spotlight_draws=%llu mono_mech_spotlight_draws=%llu "
            "type_node_events_total=%llu distinct_types_with_spotlights=%zu\n",
            (unsigned long long)s_spotlightReal_drawFrameCount,
            (unsigned long long)s_spotlightReal_drawWindow,
            (unsigned long long)s_spotlightReal_drawMono,
            (unsigned long long)s_spotlightReal_typeNodeEvents,
            s_spotlightRealTypeNodeCount.size());
        std::fflush(stderr);
    }
    if (spotlightRealPeriodic) {
        s_spotlightReal_drawWindow = 0;
    }

    if (!s_mechBatcherTraceInit) {
        s_mechBatcherTrace     = (getenv("MC2_MECH_BATCHER_STATS") != nullptr);
        s_mechBatcherTraceInit = true;
    }
    if (!s_mechLightTraceInit) {
        s_mechLightTrace     = (getenv("MC2_MECH_LIGHT_TRACE") != nullptr);
        s_mechLightTraceInit = true;
    }
    if (s_mechLightTrace) {
        // [LIGHTSSBO v1] RF4: LightsData is an unbounded SSBO now — there
        // is NO 64-slot cap, so the old event=cache_full overflow alarm
        // would false-fire on exactly the dense missions this conversion
        // enables. Repurposed to a pure maxIdx observation (no cap, no
        // disable). s_lightCacheFullFrames retired.
        uint32_t maxIdx = 0;
        for (const auto& ps : s_pendingSubmits) {
            if (ps.desc.lightDataIndex > maxIdx) maxIdx = ps.desc.lightDataIndex;
        }
        std::fprintf(stderr,
            "[LIGHTSSBO v1] event=mech_lightidx_max submitted=%zu maxIdx=%u (unbounded SSBO)\n",
            s_pendingSubmits.size(), maxIdx);
    }

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed ||
        s_pendingSubmits.empty()) {
        s_pendingSubmits.clear();
        s_eligibleActorsThisFrame = 0;
        std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
        // MECH-EXTRACTION-0: persist buffer is retained for frames with no pending submits;
        // it will be cleared in onMapUnload() or repopulated in the normal flush() path.
        return;
    }

    // Slice C1: resolve s_mechSurfaceMaterialIdx once after gos_materials::init() has run.
    // init() is called from finalizeGeometry() which always precedes the first flush().
    if (!s_mechSurfaceMaterialIdxResolved) {
        s_mechSurfaceMaterialIdxResolved = true;
        const char* v = std::getenv("MC2_MECH_SURFACE_MATERIAL");
        if (v && v[0] != '\0' && strcmp(v, "none") != 0 && strcmp(v, "0") != 0)
            s_mechSurfaceMaterialIdx = gos_materials::getProfileIndex(v);
        // else stays 0 (default passthrough)
    }

    // Step 1: Count total bones (one block per actor).
    size_t totalBones = 0;
    for (const auto& ps : s_pendingSubmits) totalBones += ps.bones.size();

    // Step 2: Build draw buckets.
    // Key: (typeLodIdx, globalPacketIdx, texHandle, materialFlags).
    // Each actor × packet produces one entry in the matching bucket.
    // Different per-actor paint schemes for the same packet -> different buckets.
    struct BucketKey {
        uint32_t typeLodIdx;
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        bool operator<(const BucketKey& o) const {
            if (typeLodIdx      != o.typeLodIdx)      return typeLodIdx      < o.typeLodIdx;
            if (globalPacketIdx != o.globalPacketIdx) return globalPacketIdx < o.globalPacketIdx;
            if (texHandle       != o.texHandle)       return texHandle       < o.texHandle;
            return materialFlags < o.materialFlags;
        }
    };

    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]

    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        const PendingSubmit& ps = s_pendingSubmits[si];
        const GpuMechTypeLodRecord& rec = s_typeLodRecords[ps.typeLodIdx];
        for (uint32_t p = 0; p < rec.packetCount; ++p) {
            const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
            BucketKey key;
            key.typeLodIdx       = ps.typeLodIdx;
            key.globalPacketIdx  = rec.firstPacket + p;
            key.texHandle        = ps.packetTexHandles[p];
            key.materialFlags    = pkt.materialFlags;
            buckets[key].push_back(si);
        }
    }

    size_t totalInstances = 0;
    for (const auto& kv : buckets) totalInstances += kv.second.size();

    ensureRingCapacity(totalInstances, totalBones);
    if (!s_instanceMap || !s_boneMap) {
        s_pendingSubmits.clear();
        return;
    }

    // Step 3: Advance ring slot and wait for oldest fence.
    s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    GpuMechInstance* instDst = (GpuMechInstance*)s_instanceMap + s_frameSlot * s_instanceCapacity;
    GpuMechBone*     boneDst = (GpuMechBone*)    s_boneMap     + s_frameSlot * s_boneCapacity;

    // Step 4: Write bone SSBO once per actor; record each actor's boneBase offset.
    std::vector<uint32_t> actorBoneBase(s_pendingSubmits.size());
    uint32_t boneHead = 0;
    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        actorBoneBase[si] = boneHead;
        for (const auto& b : s_pendingSubmits[si].bones)
            boneDst[boneHead++] = b;
    }

    // Step 2.5: Build mech MaterialGpu table (same bucket iteration order as Step 5).
    // s_mechDrawMaterialIdx is per-draw (bucket-iteration order) — rebuilt every flush.
    // s_mechMaterialTable / s_mechHandleToMaterialIdx PERSIST within a mission
    // (texHandle->idx is stable; reset by onMapUnload). Upload only when the table grows.
    s_mechDrawMaterialIdx.clear();
    if (s_mechMaterialGpuEnabled) {
        if (s_mechMaterialTable.empty())
            s_mechMaterialTable.push_back(RenderCore::MaterialGpu{}); // index 0 = sentinel/not-assigned
        bool tableDirty = false;
        for (const auto& kv : buckets) {
            const uint32_t texHandle = kv.first.texHandle;
            uint32_t mIdx;
            auto it = s_mechHandleToMaterialIdx.find(texHandle);
            if (it != s_mechHandleToMaterialIdx.end()) {
                mIdx = it->second;
            } else {
                mIdx = static_cast<uint32_t>(s_mechMaterialTable.size());
                RenderCore::MaterialGpu entry{};
                entry.albedoTex = texHandle;
                s_mechMaterialTable.push_back(entry);
                s_mechHandleToMaterialIdx[texHandle] = mIdx;
                tableDirty = true;
            }
            s_mechDrawMaterialIdx.push_back(mIdx);
        }
        // Upload only when the table grew (or buffer not yet created).
        if (s_mechMaterialSsbo == 0 || tableDirty) {
            const size_t byteSize = s_mechMaterialTable.size() * sizeof(RenderCore::MaterialGpu);
            if (s_mechMaterialSsbo == 0) glGenBuffers(1, &s_mechMaterialSsbo);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_mechMaterialSsbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(byteSize),
                         s_mechMaterialTable.data(), GL_DYNAMIC_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }
    // Throttled table_link log (first flush + every 600th)
    ++s_mechMaterialGpuFrameCount;
    if (s_mechMaterialGpuFrameCount % 600 == 1) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "[MECH_MATERIAL_GPU v1] event=table_link materials=%u\n",
            static_cast<uint32_t>(s_mechMaterialTable.size()));
        std::fputs(buf, stderr);
    }

    // Step 5: Write instance SSBO in bucket order; collect draw calls.
    struct DrawCall {
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        uint32_t instanceBase;
        uint32_t instanceCount;
    };
    std::vector<DrawCall> drawCalls;
    drawCalls.reserve(buckets.size());

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t instHead = 0;
    uint32_t dcIdx    = 0;
    for (const auto& kv : buckets) {
        const BucketKey& key              = kv.first;
        const std::vector<uint32_t>& subs = kv.second;

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = instHead;
        dc.instanceCount   = (uint32_t)subs.size();

        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            // M2.5 (Q3 unconditional): carry the RenderWorld handle through
            // to the SSBO. Env-OFF: GLSL macro gates out the FS write, so
            // the value is never read by the GPU.
            inst.objectIdRaw        = d.objectIdRaw;
            // M2.5 (Q4): count non-zero writes for per-mission observability.
            // Direct per-mission accumulation (no per-frame intermediate;
            // per adversarial M1 the per-frame counter had no consumer).
            if (d.objectIdRaw != 0u) {
                ++s_gpuMechIdWritesThisMission;
            }
            // Mech-1: per-instance materialIdx from parallel s_mechDrawMaterialIdx table
            inst.materialIdx = (s_mechMaterialGpuEnabled && dcIdx < s_mechDrawMaterialIdx.size())
                ? s_mechDrawMaterialIdx[dcIdx]
                : 0u;
            // Slice C1: when a named surface material profile is selected via
            // MC2_MECH_SURFACE_MATERIAL, override materialIdx with the binding-7
            // profile index. 0 = env unset/none → preserve existing albedo-table value.
            if (s_mechSurfaceMaterialIdx != 0u)
                inst.materialIdx = s_mechSurfaceMaterialIdx;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: per-mech visual state into the
            // SSBO record. No shader reads these in Slice 1.
            inst.visualDamage01     = d.damage01;
            inst.visualFlags        = d.visualFlags;
            instDst[instHead++]     = inst;
        }
        drawCalls.push_back(dc);
        ++dcIdx;
    }

    // Task 5: Compare validation — verify albedoTex == texHandle per draw call
    if (s_mechMaterialGpuEnabled) {
        int mismatches = 0;
        for (uint32_t i = 0; i < (uint32_t)drawCalls.size(); ++i) {
            const uint32_t mIdx = (i < (uint32_t)s_mechDrawMaterialIdx.size()) ? s_mechDrawMaterialIdx[i] : 0u;
            if (mIdx == 0u || mIdx >= (uint32_t)s_mechMaterialTable.size()) continue;
            const uint32_t albedo   = s_mechMaterialTable[mIdx].albedoTex;
            const uint32_t expected = drawCalls[i].texHandle;
            if (albedo != expected) {
                if (mismatches < 10) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[MECH_MATERIAL_GPU v1] MISMATCH dc=%u materialIdx=%u albedo=%u expected=%u\n",
                        i, mIdx, albedo, expected);
                    std::fputs(buf, stderr);
                }
                ++mismatches;
            }
        }
        if (s_mechMaterialGpuFrameCount % 600 == 1) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "[MECH_MATERIAL_GPU v1] event=compare frame=%u mechs=%u mismatches=%d\n",
                s_mechMaterialGpuFrameCount,
                static_cast<uint32_t>(drawCalls.size()),
                mismatches);
            std::fputs(buf, stderr);
        }
    }

    // Mech-1: save binding 2 before Step 6 mutates it
    GLint prevSsbo2 = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 2, &prevSsbo2);

    // Step 6: Bind SSBOs (whole per-frame slices; shader indexes via u_instanceBase).
    gpuBindSsboRange(0, s_instanceSsbo,
        (long long)(s_frameSlot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (long long)(totalInstances * sizeof(GpuMechInstance)),
        "mech.instance.shadow");
    gpuBindSsboRange(1, s_boneSsbo,
        (long long)(s_frameSlot * s_boneCapacity * sizeof(GpuMechBone)),
        (long long)(totalBones * sizeof(GpuMechBone)),
        "mech.bone.shadow");
    // Mech-1: bind MaterialGpu table SSBO at binding 2
    if (s_mechMaterialGpuEnabled && s_mechMaterialSsbo != 0) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_mechMaterialSsbo);
    }
    // Slice C1: bind named material profile SSBO at binding 7.
    // No-op when MC2_MATERIAL_GPU=0, init() not called, or no textured profiles.
    gos_materials::bindMaterialTable();

    // Save prior GL state. The mech flush bypasses applyRenderStates'
    // tracked slot set, so the engine's render-state cache becomes
    // out-of-sync after we mutate state directly. We save EVERYTHING the
    // static_prop batcher saves (mirror its pattern), restore at end, and
    // call gos_InvalidateRenderStateCache() to force the next
    // applyRenderStates to re-issue. Skipping invalidate has been observed
    // to make subsequent draws (water, mechs themselves on later frames)
    // disappear after a few frames as the cache thinks GL is in state X
    // while reality is state Y.
    GLint     prevDepthFunc;   glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    GLboolean prevDepthTest;   glGetBooleanv(GL_DEPTH_TEST,      &prevDepthTest);
    GLboolean prevDepthMask;   glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevBlend;       glGetBooleanv(GL_BLEND,            &prevBlend);
    GLboolean prevCull;        glGetBooleanv(GL_CULL_FACE,        &prevCull);
    GLint     prevCullMode;    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    GLint     prevProgram;     glGetIntegerv(GL_CURRENT_PROGRAM,  &prevProgram);
    GLint     prevVao;         glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint     prevArrayBuf;    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    GLint     prevElemBuf;     glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    GLint     prevActiveTex;   glGetIntegerv(GL_ACTIVE_TEXTURE,   &prevActiveTex);
    GLint     prevSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &prevSampler);
    GLint     prevSsbo0     = 0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);
    GLint     prevSsbo1     = 0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &prevSsbo1);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTexUnit0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);
    // Slice C2: save PBR detail sampler units 1 and 2.
    glActiveTexture(GL_TEXTURE1);
    GLint prevTexUnit1 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit1);
    GLint prevSampler1 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    GLint prevTexUnit2 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit2);
    GLint prevSampler2 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &prevSampler2);
    glActiveTexture(GL_TEXTURE0);

    // MECH-PIPELINEDESC-1: program + fixed-function state (depth test+write+func
    // GEQUAL, blend Opaque, cull Back) from the registered MechOpaque PipelineDesc,
    // replacing the prior hand-set glUseProgram/glEnable/glDepthFunc/... block.
    // applyPipeline also does glUseProgram(desc.glProgramName) (= s_mechProgram via
    // bindProgram at link). Sampler + VAO/IBO stay manual (not PipelineDesc fields).
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque));

    glBindVertexArray(s_sharedVao);
    // Explicit IBO rebind: mirrors flushShadow() pattern. GL_ELEMENT_ARRAY_BUFFER
    // is VAO state; s_sharedVao's slot can be left at 0 by a prior flushShadow()
    // restore-order bug. Belt-and-suspenders self-heal.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Bind our REPEAT/LINEAR sampler. Per
    // memory/sampler_state_inheritance_in_fast_paths.md: when a prior pass
    // (e.g. patch_stream) leaves a CLAMP_TO_EDGE sampler bound on unit 0,
    // mech UVs that fall outside [0,1] (legitimate for tiled mech body
    // textures) all clamp to the texture edge — which for many mech
    // textures is a black border, producing the all-black mech symptom.
    // Sampler-object state OVERRIDES the texture object's glTexParameter
    // values, so setting them on the texture object alone is insufficient.
    // Identified via debug=7 (hardcoded UV (0.5, 0.5) showed yellow paint
    // while debug=2 with v_uv showed black).
    glBindSampler(0, s_sampler);

    // Slice C2: bind PBR surface-detail textures to units 1 and 2.
    // s_mechSurfaceMaterialIdx 0 → both getters return 0 → unbind (safe; shader
    // only samples these when u_standardLitEnabled != 0, which requires a non-zero profile).
    {
        static const float s_pbrTileScale = []() {
            const char* v = std::getenv("MC2_PBR_TILE_SCALE");
            return v ? (float)std::atof(v) : 4.0f;
        }();
        const GLuint pbrNormal = gos_materials::getProfileNormalTex(s_mechSurfaceMaterialIdx);
        const GLuint pbrOrm    = gos_materials::getProfileOrmTex(s_mechSurfaceMaterialIdx);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, pbrNormal);
        glBindSampler(1, s_sampler);   // REPEAT/LINEAR same as unit 0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, pbrOrm);
        glBindSampler(2, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        if (s_loc_u_pbrNormalTex >= 0) glUniform1i(s_loc_u_pbrNormalTex, 1);
        if (s_loc_u_pbrOrmTex    >= 0) glUniform1i(s_loc_u_pbrOrmTex,    2);
        if (s_loc_u_pbrTileScale >= 0) glUniform1f(s_loc_u_pbrTileScale, s_pbrTileScale);
    }

    // Static uniforms.
    glUniform1i(s_loc_u_tex,      0);
    glUniform1f(s_loc_u_fogValue, 1.0f);
    // Slice B1: lighting mode 0 = Slice A flat-white passthrough,
    // 1 = calc_light() per-vertex. Set per-flush from killswitch.
    if (s_loc_u_lightingMode >= 0)
        glUniform1i(s_loc_u_lightingMode, g_useGpuMechLighting ? 1 : 0);
    // Slice C2: skinning mode 0 = rigid per-bone (Slice A), 1 = weighted
    // multi-bone blend. Stock data is byte-identical across modes.
    if (s_loc_u_skinningMode >= 0)
        glUniform1i(s_loc_u_skinningMode, g_useGpuMechSkin ? 1 : 0);
    if (s_mechBatcherTrace) {
        static int s_uniDiagPrinted = 0;
        if (s_uniDiagPrinted < 2) {
            ++s_uniDiagPrinted;
            GLint utexVal = -99;
            if (s_loc_u_tex >= 0)
                glGetUniformiv(s_mechProgram, s_loc_u_tex, &utexVal);
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=uni_probe loc_u_tex=%d u_tex_val=%d s_loc_terrainMVP=%d s_loc_u_mvp=%d\n",
                s_loc_u_tex, utexVal, s_loc_terrainMVP, s_loc_u_mvp);
        }
    }
    {
        // MC2_MECH_FRAG_DEBUG=N: legacy env path; wins over ImGui s_mechDebugMode.
        // 0=normal, 1=magenta, 2=texOnly, 3=lightOnly, 4=normal-as-color.
        // When env is unset and s_mechDebugMode=0: dbgMode=0 → byte-identical.
        const char* dbg = std::getenv("MC2_MECH_FRAG_DEBUG");
        const int dbgMode = dbg ? std::atoi(dbg) : s_mechDebugMode;
        if (s_loc_u_debugMode >= 0)
            glUniform1i(s_loc_u_debugMode, dbgMode);
    }
    // MECH-AMBIENT-1: hemisphere ambient fill strength. Uploads 0.0 when the
    // gate is OFF -> exact no-op (byte-identical default path).
    if (s_loc_u_mechAmbientV1Strength >= 0)
        glUniform1f(s_loc_u_mechAmbientV1Strength,
                    s_mechAmbientV1 ? s_mechAmbientStrength : 0.0f);
    // MECH-SPECULAR-V1: upload all 6 specular uniforms. Strength is 0.0 when
    // the gate is off -> exact no-op. The roughness/threshold uniforms are
    // uploaded unconditionally; they are inert when strength is 0. All 6 locs
    // are -1 on non-viewuniforms variants (glUniform* with loc -1 is a no-op
    // per GL spec), so the guards are redundant but kept for clarity.
    if (s_loc_u_mechSpecularV1Strength >= 0)
        glUniform1f(s_loc_u_mechSpecularV1Strength,
                    (s_mechSpecularV1 && s_mechViewUniforms)
                        ? s_mechSpecularStrength : 0.0f);
    if (s_loc_u_mechMetalRoughness >= 0)
        glUniform1f(s_loc_u_mechMetalRoughness, s_mechMetalRoughness);
    if (s_loc_u_mechGlassRoughness >= 0)
        glUniform1f(s_loc_u_mechGlassRoughness, s_mechGlassRoughness);
    if (s_loc_u_mechGlassLumaThresh >= 0)
        glUniform1f(s_loc_u_mechGlassLumaThresh, s_mechGlassLumaThresh);
    if (s_loc_u_mechGlassMaxChanThresh >= 0)
        glUniform1f(s_loc_u_mechGlassMaxChanThresh, s_mechGlassMaxChanThresh);
    if (s_loc_u_mechSpecDebugMask >= 0)
        glUniform1i(s_loc_u_mechSpecDebugMask, s_mechSpecDebugMask ? 1 : 0);
    // Slice C1: StandardLit GGX gate (mutable via ImGui / batcher_setStandardLitEnabled).
    if (s_loc_u_standardLitEnabled >= 0)
        glUniform1i(s_loc_u_standardLitEnabled, s_standardLitEnabled);

    // PBR-TUNE-1: metallic influence, roughness clamp, ambient specular fill.
    // Mutable via ImGui / batcher_setPbr* accessors; env-seeded defaults.
    if (s_loc_u_pbrMetallicInfluence      >= 0) glUniform1f(s_loc_u_pbrMetallicInfluence,       s_pbrMetallicInfluence);
    if (s_loc_u_pbrRoughnessMin           >= 0) glUniform1f(s_loc_u_pbrRoughnessMin,            s_pbrRoughnessMin);
    if (s_loc_u_pbrRoughnessMax           >= 0) glUniform1f(s_loc_u_pbrRoughnessMax,            s_pbrRoughnessMax);
    if (s_loc_u_pbrAmbientSpecularStrength >= 0) glUniform1f(s_loc_u_pbrAmbientSpecularStrength, s_pbrAmbientSpecularStrength);

    // Projection uniforms — match static_prop batcher and the
    // terrain_overlay.vert convention: terrainMVP = CPU-composed
    // axisSwap * worldToClip, row-major, uploaded with GL_FALSE.
    // Plan template said "TG_Shape::s_worldToClip with GL_TRUE" — that's
    // wrong; it skips the axis swap, mech ends up off-screen. Caught
    // 2026-05-08 by operator visual smoke (mechs invisible after PREC fix
    // re-enabled the GPU path).
    // terrainMVP fetched unconditionally so the DIAG probe below can compare
    // it against the binding-3 UBO even on the gated ViewUniforms path.
    const float* terrainMVP = gos_GetTerrainMVPMat4();
    // MECH-VIEWUNIFORMS-BLOCKBINDING-1: on the gated path the ViewUniformsBlock
    // UBO (binding=3) supplies u_worldToClipGL, so s_loc_terrainMVP is -1 (the
    // legacy uniform is #ifndef'd out) and this upload would be a no-op anyway;
    // skip it explicitly. Default path (gate OFF) uploads the legacy uniform
    // exactly as before.
    if (!s_mechViewUniforms && s_loc_terrainMVP >= 0 && terrainMVP)
        glUniformMatrix4fv(s_loc_terrainMVP, 1, GL_FALSE, terrainMVP);
    const float* mm = gos_GetProj2ScreenMat4();
    if (s_loc_u_mvp >= 0 && mm)
        glUniformMatrix4fv(s_loc_u_mvp, 1, GL_TRUE, mm);

    // MECH-VIEWUNIFORMS-BINDING-DIAG-1: gated read-only probe
    // (MC2_MECH_VIEWUNIFORMS_DIAG=1, default OFF). Diagnoses the reverted
    // MECH-VIEWUNIFORMS-1-PRE failure (mechs vanished on the UBO path). Recon
    // proved binding=3 is uploaded once per frame at gamecam top and is never
    // clobbered, so this confirms empirically AT MECH FLUSH TIME: (a) is the
    // ViewUniforms UBO still bound at binding point 3, and (b) does its
    // worldToClipGL match what the mech path actually uploads
    // (gos_GetTerrainMVPMat4). If max_diff != 0 the once-per-frame UBO capture
    // is stale/wrong for the mech pass; if binding3==0 the bind is dead here.
    // Pure read-back: no render effect. Rate-limited. Saves+restores the
    // generic GL_UNIFORM_BUFFER target binding.
    {
        if (s_mechViewUniformsDiag) {
            static int s_vuDiagFrame = 0;
            ++s_vuDiagFrame;
            if (s_vuDiagFrame <= 5 || (s_vuDiagFrame % 300) == 0) {
                GLint b3 = 0;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 3, &b3);
                float ubo[16] = {0.0f};
                float maxDiff = -1.0f;
                if (b3 != 0) {
                    GLint prevBound = 0;
                    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prevBound);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)b3);
                    glGetBufferSubData(GL_UNIFORM_BUFFER, 0,
                                       (GLsizeiptr)sizeof(ubo), ubo);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prevBound);
                    if (terrainMVP) {
                        maxDiff = 0.0f;
                        for (int i = 0; i < 16; ++i) {
                            float d = ubo[i] - terrainMVP[i];
                            if (d < 0.0f) d = -d;
                            if (d > maxDiff) maxDiff = d;
                        }
                    }
                }
                std::fprintf(stderr,
                    "[MECH_VU_DIAG v1] frame=%d submits=%llu binding3_ubo=%d "
                    "max_diff_vs_terrainMVP=%.6f ubo_row0=[%.3f %.3f %.3f %.3f] "
                    "mvp_row0=[%.3f %.3f %.3f %.3f]\n",
                    s_vuDiagFrame,
                    (unsigned long long)s_lastFlushSubmitCount, b3, maxDiff,
                    ubo[0], ubo[1], ubo[2], ubo[3],
                    terrainMVP ? terrainMVP[0] : 0.0f,
                    terrainMVP ? terrainMVP[1] : 0.0f,
                    terrainMVP ? terrainMVP[2] : 0.0f,
                    terrainMVP ? terrainMVP[3] : 0.0f);
            }
        }
    }

    // GPU-SYNC-CONTRACT: the instance + bone SSBOs were just written through a
    // persistent GL_MAP_COHERENT_BIT mapping (Steps 4-5). Order those CPU writes
    // BEFORE the instanced draws below read them as SSBO. Without this, NVIDIA may
    // draw from stale/zero instance+bone data -> mechs garbled/invisible; AMD
    // tolerated it. (Default-on GPU mech path -- a sibling of the static-prop tree
    // bug; routed through the typed helper.)
    gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                   "mech_instance_bone");

    // Step 7: Issue one draw call per bucket.
    uint32_t drawnCalls = 0;
    static int s_texDiagPrinted = 0;
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        if (s_loc_u_instanceBase >= 0)
            glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
        if (s_loc_u_materialFlags >= 0)
            glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);

        // dc.texHandle is the mcTextureManager slot index, NOT a gos
        // handle (TG_TinyTexture::gosTextureHandle is a misnamed slot
        // index per memory/mc2_texture_handle_is_live.md). Resolve to the
        // live gos handle THIS FRAME, then to GL texture id.
        const DWORD liveGosHandle = (dc.texHandle != 0xFFFFFFFFu && mcTextureManager)
            ? mcTextureManager->get_gosTextureHandle(dc.texHandle)
            : 0u;
        const uint32_t glTexId = gos_GetGLTextureId((uint32_t)liveGosHandle);
        if (s_mechBatcherTrace && s_texDiagPrinted < 16) {
            ++s_texDiagPrinted;
            // Probe what the bound texture object actually contains.
            GLint tw = 0, th = 0, tfmt = 0, tMin = 0, tMag = 0;
            GLint sR = 0, sG = 0, sB = 0, sA = 0;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &th);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tfmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tMin);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &tMag);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sR);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sG);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sB);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sA);
            // Probe the actually-effective sampler state (sampler-object
            // overrides texture-object params silently per brainstorm #1).
            GLint boundSampler = -1;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &boundSampler);
            GLint sampMin = -1, sampMag = -1, sampWrapS = -1;
            if (boundSampler != 0) {
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MIN_FILTER, &sampMin);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MAG_FILTER, &sampMag);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_WRAP_S,     &sampWrapS);
            }
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=tex_probe glTex=%u w=%d h=%d fmt=0x%x texMin=0x%x texMag=0x%x boundSampler=%d sampMin=0x%x sampMag=0x%x sampWrapS=0x%x\n",
                glTexId, tw, th, tfmt, tMin, tMag, boundSampler, sampMin, sampMag, sampWrapS);
            // Pixel-readback diagnostic — gated on its own env var
            // (NOT MC2_MECH_BATCHER_STATS) because glGetTexImage is a
            // synchronous GPU stall that pollutes any perf measurement.
            // Only enable when actively diagnosing texture-content bugs.
            static const bool s_pixelReadback = (getenv("MC2_MECH_TEX_READBACK") != nullptr);
            if (s_pixelReadback && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                std::vector<uint8_t> pixels((size_t)tw * th * 3, 0);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                int nonZero = 0;
                for (uint8_t b : pixels) if (b != 0) { nonZero = 1; break; }
                std::fprintf(stderr,
                    "[MECHBATCHER v1] event=tex_pixels glTex=%u bytes=%zu nonZero=%d first15="
                    "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
                    glTexId, pixels.size(), nonZero,
                    pixels[ 0], pixels[ 1], pixels[ 2],
                    pixels[ 3], pixels[ 4], pixels[ 5],
                    pixels[ 6], pixels[ 7], pixels[ 8],
                    pixels[ 9], pixels[10], pixels[11],
                    pixels[12], pixels[13], pixels[14]);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glTexId);
        // The actual texture-black fix in Slice A is mech.frag's
        // textureLod(u_tex, v_uv, 0.0) — see memory/amd_auto_lod_strict_fail.md.
        // Sampler-state inheritance is defended by the glBindSampler(0,
        // s_sampler) above (REPEAT/LINEAR), which OVERRIDES texture-object
        // params anyway. So no per-texture glTexParameteri here. Keeping the
        // texture object's persistent state untouched avoids leaking mech-
        // specific filter/wrap onto a gosTexture handle that another renderer
        // may want to sample with auto-LOD or CLAMP later.

        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++drawnCalls;
    }

    // Restore prior state in reverse order of save.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, (GLuint)prevSsbo2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    // Slice C2: restore PBR detail sampler units.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit1);
    glBindSampler(1, (GLuint)prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit2);
    glBindSampler(2, (GLuint)prevSampler2);
    glActiveTexture((GLenum)prevActiveTex);
    glBindSampler(0, (GLuint)prevSampler);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevCull)  { glEnable(GL_CULL_FACE); glCullFace((GLenum)prevCullMode); }
    else             glDisable(GL_CULL_FACE);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // RENDER_STATES v1: even with the explicit save/restore above, the
    // engine's applyRenderStates cache mirrors a separate (slot,value)
    // table — it does NOT re-read GL state. Without invalidate, a
    // subsequent applyRenderStates call early-outs on matching cached
    // values while the actual texture binding / sampler / depth bits we
    // touched go unrechecked. Mirrors gos_static_prop_batcher.cpp:1791.
    gos_InvalidateRenderStateCache();

    // Per-reason stats output (MC2_MECH_BATCHER_STATS=1).
    if (s_mechBatcherTrace) {
        static const char* const kFallbackNames[] = {
            "UnregisteredType", "U8BoneOverflow", "RingOverflow",
            "TglGpuUnsupported", "ShaderInitFailure"
        };
        uint32_t fallbackTotal = 0;
        for (int i = 0; i < 5; ++i) fallbackTotal += s_fallbacksThisFrame[i];
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=summary eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u fallback_total=%u\n",
            s_eligibleActorsThisFrame, s_pendingSubmits.size(),
            buckets.size(), drawnCalls, fallbackTotal);
        for (int i = 0; i < 5; ++i) {
            if (s_fallbacksThisFrame[i] > 0) {
                std::fprintf(stderr,
                    "[MECHBATCHER v1] event=fallback reason=%s count=%u\n",
                    kFallbackNames[i], s_fallbacksThisFrame[i]);
            }
        }
    }

    // Persist this frame's draw-call list and SSBO sizes for flushShadow()
    // to consume on the NEXT frame (Option B: persisted drawCalls).
    s_lastDrawCalls.clear();
    s_lastDrawCalls.reserve(drawCalls.size());
    for (const DrawCall& dc : drawCalls) {
        ShadowDrawEntry e;
        e.globalPacketIdx = dc.globalPacketIdx;
        e.instanceBase    = dc.instanceBase;
        e.instanceCount   = dc.instanceCount;
        s_lastDrawCalls.push_back(e);
    }
    s_lastTotalInstances = totalInstances;
    s_lastTotalBones     = totalBones;

    // MECH-EXTRACTION-0: persist per-actor facts for batcher_getMechPendingCount/Entry.
    // Must run before s_pendingSubmits.clear() so ExtractRenderSnapshot can read this frame's data.
    static const bool s_mechExtractEnabled = [] {
        const char* v = std::getenv("MC2_SNAPSHOT_MECH_EXTRACT");
        return v && v[0] == '1';
    }();
    if (s_mechExtractEnabled) {
        s_mechExtractPersist.clear();
        s_mechExtractPersist.reserve(s_pendingSubmits.size());
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_pendingSubmits.size()); ++i) {
            const PendingSubmit& ps = s_pendingSubmits[i];
            ExtractedMechPacket pkt{};
            pkt.objectIdRaw = ps.desc.objectIdRaw;
            pkt.instanceIdx = i;
            // MECH-EXTRACTION-1: wire real materialIdx from the handle map populated
            // in Step 2.5 earlier this flush(). Sentinel when disabled or handle absent.
            if (s_mechMaterialGpuEnabled) {
                auto it = s_mechHandleToMaterialIdx.find(ps.desc.slot0TexHandle);
                pkt.materialIdx = (it != s_mechHandleToMaterialIdx.end())
                    ? it->second : 0xFFFFFFFFu;
            } else {
                pkt.materialIdx = 0xFFFFFFFFu;
            }
            pkt.texHandle   = ps.desc.slot0TexHandle;
            pkt.typeLodIdx  = ps.typeLodIdx;
            pkt.renderFlags = ps.desc.renderFlags;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: visual state for the debug dump.
            pkt.heat01      = ps.desc.heat01;
            pkt.damage01    = ps.desc.damage01;
            pkt.visualFlags = ps.desc.visualFlags;
            s_mechExtractPersist.push_back(pkt);
        }
    }

    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
}

// ---------------------------------------------------------------------------
// MECH-EXTRACTION-0: snapshot API (gate: MC2_SNAPSHOT_MECH_EXTRACT=1)
// ---------------------------------------------------------------------------

uint32_t batcher_getMechPendingCount() {
    return static_cast<uint32_t>(s_mechExtractPersist.size());
}

bool batcher_getMechPendingEntry(uint32_t idx, ExtractedMechPacket* out) {
    if (!out || idx >= static_cast<uint32_t>(s_mechExtractPersist.size()))
        return false;
    *out = s_mechExtractPersist[idx];
    out->instanceIdx = idx;
    return true;
}

// V1A: per-frame mech submit count, latched at flush() entry.
// GpuMechBatcher submits only. Does NOT include MLR fallback draws or alive count.
uint64_t batcher_getLastFlushSubmitCount() {
    return s_lastFlushSubmitCount;
}

void batcher_compareMechSnapshot(RenderSnapshot* snap) {
    if (!snap) return;
    const uint32_t liveCount  = static_cast<uint32_t>(s_mechExtractPersist.size());
    const uint32_t snapCount  = static_cast<uint32_t>(snap->mechPackets.size());
    if (snapCount != liveCount)
        snap->mechCountMismatch = 1u;
    const uint32_t compareCount = snapCount < liveCount ? snapCount : liveCount;
    for (uint32_t i = 0u; i < compareCount; ++i) {
        const ExtractedMechPacket& row = snap->mechPackets[i];
        const ExtractedMechPacket& ref = s_mechExtractPersist[i];
        if (row.typeLodIdx  != ref.typeLodIdx)  ++snap->mechHandleMismatch;
        if (row.objectIdRaw != ref.objectIdRaw) ++snap->mechObjectIdMismatch;
        if (row.texHandle   != ref.texHandle)   ++snap->mechTexHandleMismatch;
        // MECH-EXTRACTION-1: independent live materialIdx from s_mechHandleToMaterialIdx
        // (still valid post-flush; cleared at START of next flush). Stronger than
        // snapshot-vs-persist self-consistency — map is the same authority the SSBO
        // upload used, so a divergence here means the persist capture was wrong.
        {
            auto it = s_mechHandleToMaterialIdx.find(row.texHandle);
            const uint32_t liveMat = (s_mechMaterialGpuEnabled &&
                                      it != s_mechHandleToMaterialIdx.end())
                ? it->second : 0xFFFFFFFFu;
            if (row.materialIdx != liveMat)
                ++snap->mechMaterialIdxMismatch;
        }
    }
}

// MECH-SPINE-1: read-only free-function accessors for the Object Inspector's
// Mech Snapshot panel. Mirrors the TERRAIN-SPINE-0 pattern (program-id
// accessors in gameos_graphics.cpp). Each returns 0 / nullptr when the
// underlying state is not yet initialized. Read-only — no GL state touched,
// no draw-path mutation.
extern "C" uint32_t gos_getMechProgramId() {
    return (uint32_t)s_mechProgram;
}
extern "C" uint32_t gos_getMechShadowProgramId() {
    auto pit = glsl_program::s_programs.find("shadow_mech");
    if (pit == glsl_program::s_programs.end() || !pit->second)
        return 0u;
    return (uint32_t)pit->second->shp_;
}
// SHADOW-SPINE-0: read-only accessors for mech shadow caster counts written
// at the end of flushShadow(). Returns last-frame values; not reset.
extern "C" uint32_t gos_getMechShadowTypesDrawn() {
    return (uint32_t)s_shadowTypesDrawn;
}
extern "C" uint32_t gos_getMechShadowInstDrawn() {
    return (uint32_t)s_shadowInstDrawn;
}
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx) {
    if (nodeIdx == 0xFFFFFFFFu || !mcTextureManager) return nullptr;
    return mcTextureManager->getTextureName((DWORD)nodeIdx);
}

// MECH-DEBUG-VIEWS-1: RenderDebugView <-> mech.frag u_debugMode mapping.
// Shader branch numbers: 0=Final, 1=magenta, 2=texOnly(Albedo),
// 3=lightOnly(LightingOnly), 4=normal-as-color(Normal), 5=UV, 6=alpha, 7..9=diag.
// Mirror of StaticPropViewToShaderMode/StaticPropShaderModeToView in
// gos_static_prop_batcher.cpp; co-located here to stay with the flush site
// and the s_mechDebugMode state it drives.
int MechViewToFragDebugMode(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:        return 0;
        case RenderDebugView::Albedo:       return 2;
        case RenderDebugView::Normal:       return 4;
        case RenderDebugView::LightingOnly: return 3;
        default:                            return 0;  // unsupported -> Final
    }
}

RenderDebugView MechFragDebugModeToView(int m) {
    switch (m) {
        case 0: return RenderDebugView::Final;
        case 2: return RenderDebugView::Albedo;
        case 4: return RenderDebugView::Normal;
        case 3: return RenderDebugView::LightingOnly;
        default: return RenderDebugView::Final;
    }
}

extern "C" void batcher_setMechDebugMode(int shaderMode) {
    if (shaderMode < 0) shaderMode = 0;
    s_mechDebugMode = shaderMode;
}

extern "C" int batcher_getMechDebugMode() {
    return s_mechDebugMode;
}

// MECH-NORMALS-FIX-1: runtime getter/setter API for the normals mode and
// smoothing angle. These allow ImGui controls to dial settings without a restart.
// batcher_rebuildMechNormals() applies the current settings by re-uploading the
// VBO from the pristine s_stagingVbo (mode 0 = byte-identical to cooked).

extern "C" void batcher_setMechNormalsMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_mechNormalsMode = mode;
}

extern "C" int batcher_getMechNormalsMode() {
    return s_mechNormalsMode;
}

extern "C" void batcher_setMechNormalsSmoothDeg(float deg) {
    if (deg < 1.0f)   deg = 1.0f;
    if (deg > 179.0f) deg = 179.0f;
    s_mechNormalsSmoothDeg = deg;
}

extern "C" float batcher_getMechNormalsSmoothDeg() {
    return s_mechNormalsSmoothDeg;
}

// MECH-AMBIENT-1: runtime API for the gated hemisphere ambient fill. No VBO
// rebuild needed — strength is a per-flush uniform, so changes take effect the
// next frame. enabled=false uploads 0.0 (no-op).
extern "C" void batcher_setMechAmbientEnabled(int on) { s_mechAmbientV1 = (on != 0); }
extern "C" int  batcher_getMechAmbientEnabled()       { return s_mechAmbientV1 ? 1 : 0; }
extern "C" void batcher_setMechAmbientStrength(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 2.0f) s = 2.0f;
    s_mechAmbientStrength = s;
}
extern "C" float batcher_getMechAmbientStrength() { return s_mechAmbientStrength; }

// MECH-SPECULAR-V1: runtime API for the gated Blinn specular sheen. All per-flush
// uniforms — no VBO rebuild. Changes take effect the next frame. When the
// viewuniforms variant is not active (s_mechViewUniforms==false) the C++ flush
// uploads strength 0 regardless of the gate, so the setter is safe to call but
// the visual has no effect until MC2_MECH_VIEWUNIFORMS=1 is also set.
extern "C" void  batcher_setMechSpecularEnabled(int on)      { s_mechSpecularV1 = (on != 0); }
extern "C" int   batcher_getMechSpecularEnabled()            { return s_mechSpecularV1 ? 1 : 0; }
extern "C" void  batcher_setMechSpecularStrength(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 4.0f) s = 4.0f;
    s_mechSpecularStrength = s;
}
extern "C" float batcher_getMechSpecularStrength()           { return s_mechSpecularStrength; }
extern "C" void  batcher_setMechMetalRoughness(float r) {
    if (r < 0.04f) r = 0.04f;
    if (r > 1.0f)  r = 1.0f;
    s_mechMetalRoughness = r;
}
extern "C" float batcher_getMechMetalRoughness()             { return s_mechMetalRoughness; }
extern "C" void  batcher_setMechGlassRoughness(float r) {
    if (r < 0.04f) r = 0.04f;
    if (r > 1.0f)  r = 1.0f;
    s_mechGlassRoughness = r;
}
extern "C" float batcher_getMechGlassRoughness()             { return s_mechGlassRoughness; }
extern "C" void  batcher_setMechGlassLumaThresh(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_mechGlassLumaThresh = t;
}
extern "C" float batcher_getMechGlassLumaThresh()            { return s_mechGlassLumaThresh; }
extern "C" void  batcher_setMechGlassMaxChanThresh(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_mechGlassMaxChanThresh = t;
}
extern "C" float batcher_getMechGlassMaxChanThresh()         { return s_mechGlassMaxChanThresh; }
extern "C" void  batcher_setMechSpecDebugMask(int on)        { s_mechSpecDebugMask = (on != 0); }
extern "C" int   batcher_getMechSpecDebugMask()              { return s_mechSpecDebugMask ? 1 : 0; }

// PBR-TUNE-1: StandardLit GGX toggle + material influence knobs.
// All per-flush uniforms — no VBO rebuild needed.
extern "C" void  batcher_setStandardLitEnabled(int on)  { s_standardLitEnabled = (on != 0) ? 1 : 0; }
extern "C" int   batcher_getStandardLitEnabled()        { return s_standardLitEnabled; }
extern "C" void  batcher_setPbrMetallicInfluence(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrMetallicInfluence = v;
}
extern "C" float batcher_getPbrMetallicInfluence()      { return s_pbrMetallicInfluence; }
extern "C" void  batcher_setPbrRoughnessMin(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrRoughnessMin = v;
}
extern "C" float batcher_getPbrRoughnessMin()           { return s_pbrRoughnessMin; }
extern "C" void  batcher_setPbrRoughnessMax(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    s_pbrRoughnessMax = v;
}
extern "C" float batcher_getPbrRoughnessMax()           { return s_pbrRoughnessMax; }
extern "C" void  batcher_setPbrAmbientSpecularStrength(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 2.0f) v = 2.0f;
    s_pbrAmbientSpecularStrength = v;
}
extern "C" float batcher_getPbrAmbientSpecularStrength() { return s_pbrAmbientSpecularStrength; }

// batcher_rebuildMechNormals: re-upload the mech geometry VBO with the current
// s_mechNormalsMode and s_mechNormalsSmoothDeg. No-op if geometry is not yet
// finalized. Saves and restores the current VAO and ARRAY_BUFFER bindings so
// it is safe to call from an ImGui callback between frames.
extern "C" void batcher_rebuildMechNormals() {
    if (!s_geometryFinalized || s_sharedVao == 0) return;

    // Save current bindings that uploadMechGeometryVbo() will perturb.
    GLint prevVao = 0, prevArr = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArr);

    // uploadMechGeometryVbo() expects the VAO to be bound (attrib pointers are
    // VAO state). It also preserves the VAO's existing element-array binding
    // (s_sharedIbo) — see the comment in uploadMechGeometryVbo().
    glBindVertexArray(s_sharedVao);
    uploadMechGeometryVbo();
    glBindVertexArray(0);

    // Restore bindings so nothing upstream is surprised.
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArr);

    std::fprintf(stderr,
        "[MECHBATCHER v1] event=normals_rebuild mode=%d smoothDeg=%.1f nodes=%zu vbo_bytes=%zu\n",
        s_mechNormalsMode, s_mechNormalsSmoothDeg, s_nodeVboRanges.size(), s_stagingVbo.size());
}
