#include "gos_static_prop_registry.h"
#include "../../mclib/txmmgr.h"  // 2026-05-05: peekLightSlotNumLights/getLightStructCount for flush trace
#include "../../mclib/appear.h"  // Task 6: Appearance* for registerStaticProp()
#include "gpu_cull_substrate.h"  // C1b GPU authority flip: substrate_appendStaticPropRecord
#include "gpu_cull_record.h"     // C1b: GpuActorRecord, Cat_StaticProp, CategoryMask
#include "../../mclib/terrain.h" // C1b temporal-superset: Terrain::worldToBlockIdx()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <array>
#include <chrono>
#include <set>

// MC_TextureManager singleton, defined in mclib/txmmgr.cpp.
extern MC_TextureManager* mcTextureManager;

// 2026-05-05: frame counter for cull-aware static replay. When an actor goes
// offscreen, MC2's cull gate skips its update() (`cull_gates_are_load_bearing.md`),
// which means CacheGpuLightData()/ResubmitCachedGpuLightData() also doesn't run,
// so its cachedGpuLightIndex_ points at a UBO slot whose content was filled by
// a different actor THIS frame (or is beyond the upload count). flush() must
// detect this and skip emitting a draw for that recipe.
extern uint32_t g_mc2FrameCounter;

// Rejects "0", "false", "off", "no"; accepts anything else (including "1") or
// the unset/empty case (returns `defaultValue`). Matches the ParseEnvBool
// pattern in code/terrobj.cpp:79-85, extended with a default for "0=disable
// only, default-on" use.
static bool parseEnvBoolWithDefault(const char* name, bool defaultValue) {
    const char* v = getenv(name);
    if (!v || !*v) return defaultValue;
    if (v[0] == '0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}

// Parsed at file-scope (program start) so isEnabled() is valid before init()
// is called and before the [INSTR v1] banner fires.
//
// 2026-05-05: slice 3.C ship gate. MC2_STATIC_PROP_REGISTRY now defaults ON.
// Set MC2_STATIC_PROP_REGISTRY=0 to disable (e.g., for A/B comparison or
// when chasing a regression). The MC2_FORCE_DYNAMIC_TREES env in
// TreeAppearance::render() still works as the per-tree operator escape.
static const bool s_enabled             = parseEnvBoolWithDefault("MC2_STATIC_PROP_REGISTRY",          true);
static const bool s_trace               = parseEnvBoolWithDefault("MC2_STATIC_PROP_TRACE",              false);
// Task 9 (Track B): mission-load + late-spawn registration ON by default.
// Soak started 2026-05-06; all Task 8 parity gates passed.
// Set MC2_STATIC_PROP_MISSION_LOAD_REG=0 to opt out.
static const bool s_missionLoadRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_MISSION_LOAD_REG", true);
// Set MC2_STATIC_PROP_LATE_SPAWN_REG=1 to opt in (default OFF as of 2026-05-20).
//
// Previously default-on (Task 9 Track B, 2026-05-06). The sole caller is
// MechWarrior::getWayPointMarker (code/warrior.cpp:7593), which registers
// tactical-order waypoint markers ("WalkWayPoint"/"RunWayPoint"/"JumpWayPoint")
// into the static-prop batcher so they GPU-batch alongside building props.
//
// 2026-05-20: User-driven savegame-restore canary on mc2_10 (post-commit
// 4008185) surfaced that these late-registered markers render as a
// persistent black octagon at the LZ. Two contributing factors:
//   1. MechWarrior::copyFromData (warrior.cpp:8345) iterates the restored
//      tacOrderQueue and re-creates a marker for every saved slot — including
//      slots whose orders had completed pre-save but whose .point was never
//      cleared, planting a marker at stale coordinates.
//   2. Late-registered instances never get their per-instance lightDataIndex
//      patched by flush(); calc_light() returns (0,0,0); v_argb is black.
//
// Project does not use waypoint markers as a gameplay feature, so disabling
// late-spawn registration unconditionally fixes the visible artifact with
// no behavior loss. Operator can opt back in (MC2_STATIC_PROP_LATE_SPAWN_REG=1)
// if revisiting Task 6/9 Track B's lighting-patch integration.
static const bool s_lateSpawnRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_LATE_SPAWN_REG", false);

// 2026-05-11 per-instance light-idx capture. flush() consumes the value
// stored in RecipeRange.lightDataIndex by markVisible() when this is on;
// otherwise flush() reads the multi's shared cachedGpuLightIndex_ (the
// historical last-writer-wins behavior — buggy under MC2_STATIC_UPDATE_SKIP=1
// because multiple actors of the same multi-type write to the same per-multi
// scratch slot).
//
// 2026-05-11 default-on after user soak: interactive mc2_01 run with
// MC2_STATIC_UPDATE_SKIP=1 confirmed widespread wrong-RGB symptom retired,
// 1 residual instance correlated with substrate-cap edge case (separate
// follow-up). Set MC2_STATIC_PER_INSTANCE_LIGHT=0 to opt back into the
// historical multi-cache path.
static const bool s_perInstanceLight =
    parseEnvBoolWithDefault("MC2_STATIC_PER_INSTANCE_LIGHT", true);

#define SP_TRACE(fmt, ...) \
    do { if (s_trace) { printf("[STATIC_PROP] " fmt "\n", ##__VA_ARGS__); \
         fflush(stdout); } } while (0)

// MC2_TEX_LIFECYCLE_TRACE=1 — same env flag used by mclib/txmmgr.cpp,
// mclib/msl.cpp, GameOS/gameos/gos_static_prop_batcher.cpp so all
// [TEX_LIFECYCLE v1] events stream together under one invocation.
// See docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
static const bool s_texPinTrace =
    (getenv("MC2_TEX_LIFECYCLE_TRACE") != nullptr);
#define TEX_LC_PIN(fmt, ...)                                            \
    do { if (s_texPinTrace) {                                           \
        printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__);          \
        fflush(stdout);                                                 \
    } } while (0)

namespace {

struct RecipeRange {
    uint32_t       first;   // index into s_recipes
    uint32_t       count;   // 0 = invalidated (tombstone)
    TG_MultiShape* multi;   // for per-frame lightDataIndex patch via
                            // getCachedGpuLightIndex(); NULL when count==0
    // Texture pin sibling (texture-pin-fix spec):
    std::vector<DWORD> pinnedTextureNodes;  // mcTextureNodeIndex values pinned for this range
    bool               pinsReleased;        // double-release guard for invalidate→destroy ordering
    // [STATIC_FIRST_FRAME v1] proof-of-fix fields (Track B Task 4):
    uint32_t           registeredOnFrame;   // g_mc2FrameCounter at registerRecipe()
    bool               firstFlushSeen;      // cleared at registerRecipe; set on first successful flush
    // 2026-05-11 per-instance light-idx capture (MC2_STATIC_PER_INSTANCE_LIGHT):
    // populated by markVisible() with the slot the actor's update/touch wrote
    // into multi->cachedGpuLightIndex_ before sibling actors overwrote it.
    // UINT32_MAX = uncaptured (flush falls back to multi->getCachedGpuLightIndex()).
    uint32_t           lightDataIndex;
};

static std::vector<GpuStaticPropInstance> s_recipes;
static std::vector<RecipeRange>           s_recipeRanges;

// Per-frame list of regIdx values (one per visible tree).
// markVisible() appends one regIdx per tree; flush() expands to leaves
// and patches lightDataIndex from the live TG_MultiShape.
static std::vector<uint32_t>              s_liveRangeIndices;

// Pin-call accounting for the [TEX_LIFECYCLE v1] event=pin_summary line
// emitted in destroy(). leakedPins = totalPinCalls - totalUnpinCalls;
// non-zero is a refcount imbalance bug. Reset to 0 in destroy() after
// summary emit so per-mission accounting is clean across load/unload.
static uint64_t s_totalPinCalls      = 0;
static uint64_t s_totalUnpinCalls    = 0;
// [STATIC_FIRST_FRAME v1]: counts entries whose very first flush() attempt was
// rejected by the staleness gate. Must read zero after Task 3's cachedFrame_
// pre-population; non-zero means the pre-population didn't reach flush in time.
static uint64_t s_firstFrameSkipCount = 0;
// [STATIC_PROP_REG v1] HC-3 gate signal: counts late-spawn registration
// attempts where isStaticRegistered() returned false (type unknown or
// ineligible). Emitted in destroy() for per-mission accounting.
static uint64_t s_lateSpawnTypeUnknownCount = 0;

// Release every pin held by a single RecipeRange. Idempotent via
// rng.pinsReleased — invalidate() may run before destroy() does its
// safety-net sweep, and we don't want to unpinNode the same node twice.
// No shrink_to_fit() — parent s_recipeRanges is also cleared on destroy(),
// and the per-range vector destructor handles deallocation.
static void releasePinsForRange(RecipeRange& rng) {
    if (rng.pinsReleased) return;
    if (mcTextureManager) {
        for (DWORD nodeIdx : rng.pinnedTextureNodes) {
            mcTextureManager->unpinNode(nodeIdx);
            ++s_totalUnpinCalls;
            TEX_LC_PIN("event=unpin nodeIdx=%lu refcount=%lu",
                       (unsigned long)nodeIdx,
                       (unsigned long)mcTextureManager->getPinCount(nodeIdx));
        }
    }
    rng.pinnedTextureNodes.clear();
    rng.pinsReleased = true;
}

} // namespace

// [LIGHTBAKE v1] free fns defined in mclib/txmmgr.cpp. Declared at FILE
// scope (NOT inside namespace GpuStaticPropRegistry, else the linker
// looks for GpuStaticPropRegistry::mc2... -> LNK2019).
extern void mc2EraseBakedStaticLight(int32_t);
extern void mc2ClearAllBakedStaticLight();

namespace GpuStaticPropRegistry {

bool isEnabled()               { return s_enabled; }
bool isMissionLoadRegEnabled() { return s_missionLoadRegEnabled; }
bool isLateSpawnRegEnabled()   { return s_lateSpawnRegEnabled; }

uint64_t getStaticFirstFrameSkipCount()    { return s_firstFrameSkipCount; }
uint64_t getLateSpawnTypeUnknownCount()    { return s_lateSpawnTypeUnknownCount; }

bool registerStaticProp(Appearance* app) {
    if (!isLateSpawnRegEnabled()) return false;
    if (!app) return false;
    app->registerStatic();
    const bool ok = app->isStaticRegistered();
    if (!ok) {
        ++s_lateSpawnTypeUnknownCount;
    }
    return ok;
}

void init() {
    // Env flags already parsed at file scope. init() reserves memory.
    if (s_enabled) {
        s_recipes.reserve(20000);
        s_recipeRanges.reserve(15000);
        s_liveRangeIndices.reserve(15000);
        printf("[STATIC_PROP] registry init: memory reserved\n");
        fflush(stdout);
    }
}

void destroy() {
    // [STATIC_FIRST_FRAME v1] summary — emit BEFORE pin-release loop.
    // Non-zero skip_count means Task 3's cachedFrame_ pre-population didn't
    // reach flush in time for at least one registration; escalate if nonzero.
    fprintf(stderr,
        "[STATIC_FIRST_FRAME v1] event=summary skip_count=%llu\n",
        (unsigned long long)s_firstFrameSkipCount);
    fflush(stderr);
    s_firstFrameSkipCount = 0;

    // [STATIC_PROP_REG v1] HC-3 gate signal: late-spawn registration failures.
    // count=0 means every late-spawn actor was eligible and registered.
    // count>0 identifies types that fell through to the first-render path.
    fprintf(stderr,
        "[STATIC_PROP_REG v1] event=type_unknown_at_late_spawn count=%llu\n",
        (unsigned long long)s_lateSpawnTypeUnknownCount);
    fflush(stderr);
    s_lateSpawnTypeUnknownCount = 0;

    // Texture-pin spec: release any unreleased pins (mission-teardown
    // safety net — covers ranges that were never explicitly invalidated).
    for (auto& rng : s_recipeRanges) {
        releasePinsForRange(rng);
    }

    // Texture-pin spec: pin-call accounting summary.  leakedPins != 0 is a
    // bug signal (refcount imbalance between registerRecipe and invalidate).
    if (s_texPinTrace) {
        printf("[TEX_LIFECYCLE v1] event=pin_summary mission_end "
               "totalPinCalls=%llu totalUnpinCalls=%llu leakedPins=%lld\n",
               (unsigned long long)s_totalPinCalls,
               (unsigned long long)s_totalUnpinCalls,
               (long long)((int64_t)s_totalPinCalls - (int64_t)s_totalUnpinCalls));
        fflush(stdout);
    }
    s_totalPinCalls   = 0;
    s_totalUnpinCalls = 0;

    s_recipes.clear();          s_recipes.shrink_to_fit();
    s_recipeRanges.clear();     s_recipeRanges.shrink_to_fit();
    s_liveRangeIndices.clear(); s_liveRangeIndices.shrink_to_fit();
    // [LIGHTBAKE v1] recipeIndex restarts next mission -> stale baked
    // entries would alias a different actor. Drop the mission-scoped map.
    ::mc2ClearAllBakedStaticLight();
}

void frameBegin() {
    if (!s_enabled) return;
    s_liveRangeIndices.clear();
}

int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch) {
    if (!s_enabled || batch.empty() || !multi) return -1;
    RecipeRange rng;
    rng.first = static_cast<uint32_t>(s_recipes.size());
    rng.count = static_cast<uint32_t>(batch.size());
    rng.multi = multi;
    rng.pinsReleased      = false;
    rng.registeredOnFrame = g_mc2FrameCounter;
    rng.firstFlushSeen    = false;
    rng.lightDataIndex    = 0xFFFFFFFFu;  // 2026-05-11 per-instance capture sentinel
    s_recipes.insert(s_recipes.end(), batch.begin(), batch.end());
    const int32_t regIdx = static_cast<int32_t>(s_recipeRanges.size());
    s_recipeRanges.push_back(rng);
    SP_TRACE("register regIdx=%d first=%u count=%u", regIdx, rng.first, rng.count);

    // Pin every mcTextureNodeIndex referenced by this recipe's multi-shape.
    // mcTextureManager evicts textures during gameplay (txmmgr.cpp:update);
    // for actors using touch() instead of update() under MC2_STATIC_UPDATE_SKIP=1
    // there's no re-cache pathway, so the leaf TG_TypeShape::listOfTextures
    // gosTextureHandle goes stale and the batcher renders black quads.
    // Pinning the master node prevents eviction while this range is registered.
    // Use the public TG_MultiShape::GetNumTextures()/GetTextureHandle(j) API
    // (msl.h:454-470) — GetTextureHandle(j) returns mcTextureNodeIndex directly.
    // Do not access TG_TypeMultiShape::numTextures/listOfTextures — protected.
    if (mcTextureManager) {
        RecipeRange& storedRng = s_recipeRanges.back();
        const long numTex = multi->GetNumTextures();
        for (long j = 0; j < numTex; ++j) {
            const DWORD nodeIdx = multi->GetTextureHandle(j);
            if (nodeIdx != 0xffffffff) {
                mcTextureManager->pinNode(nodeIdx);
                storedRng.pinnedTextureNodes.push_back(nodeIdx);
                ++s_totalPinCalls;
                TEX_LC_PIN("event=pin nodeIdx=%lu refcount=%lu regIdx=%d multi=%p",
                           (unsigned long)nodeIdx,
                           (unsigned long)mcTextureManager->getPinCount(nodeIdx),
                           regIdx, (void*)multi);
            }
        }
    }
    // Track B: structural first-frame fix. Pre-populate cachedFrame_ so the
    // first flush() after registration passes the staleness gate at flush()
    // without requiring a prior CacheGpuLightData() call.
    multi->setCachedFrame(g_mc2FrameCounter);
    return regIdx;
}

void markVisible(int32_t regIdx, uint32_t lightDataIndex) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    if (rng.count == 0) return; // tombstone
    // 2026-05-11: capture per-actor lightDataIndex (the multi's cachedGpuLightIndex_
    // at the moment THIS actor's update/touch wrote it, before sibling actors of
    // the same multi-type overwrote it). flush() consumes this when
    // MC2_STATIC_PER_INSTANCE_LIGHT=1 is set; otherwise flush ignores it.
    rng.lightDataIndex = lightDataIndex;
    s_liveRangeIndices.push_back(static_cast<uint32_t>(regIdx));
}

void invalidate(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    releasePinsForRange(rng);
    for (uint32_t i = 0; i < rng.count; ++i)
        s_recipes[rng.first + i] = GpuStaticPropInstance{};
    SP_TRACE("invalidate regIdx=%d (was count=%u)", regIdx, rng.count);
    rng.count = 0;
    rng.multi  = nullptr;
    rng.lightDataIndex = 0xFFFFFFFFu;  // 2026-05-11 reset capture on invalidate
    // [LIGHTBAKE v1] drop the baked static-light entry so destruction/LOD
    // multi-swap lazily re-bakes the same position-derived constant.
    ::mc2EraseBakedStaticLight(regIdx);
}

bool isReady(int32_t regIdx) {
    if (!s_enabled) return false;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return false;
    return s_recipeRanges[static_cast<uint32_t>(regIdx)].count > 0;
}

void flush() {
    if (!s_enabled || s_liveRangeIndices.empty()) return;
    const uint32_t currentFrame = g_mc2FrameCounter;
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    // 2026-05-10 diag: per-frame outcome counters across all ranges.
    static uint64_t s_diag_flush_calls = 0;
    static uint64_t s_diag_ranges_total = 0;
    static uint64_t s_diag_ranges_tombstone = 0;
    static uint64_t s_diag_ranges_stale_frame = 0;
    static uint64_t s_diag_ranges_drawn = 0;
    static uint64_t s_diag_leaves_appended = 0;
    static uint64_t s_diag_total_ns = 0;
    const auto _flush_t0 = std::chrono::steady_clock::now();
    ++s_diag_flush_calls;
    for (uint32_t regIdx : s_liveRangeIndices) {
        RecipeRange& rng = s_recipeRanges[regIdx];
        ++s_diag_ranges_total;
        if (rng.count == 0 || !rng.multi) { ++s_diag_ranges_tombstone; continue; } // tombstone guard

        // 2026-05-05: cull-aware static replay. Skip recipes whose multi-shape
        // cache wasn't refreshed this frame (offscreen actor whose update()
        // was cull-gate-skipped). The cached lightDataIndex would point at a
        // slot whose content this frame was filled by a different actor —
        // emitting a draw with that index produces wrong/black lighting.
        // Suppressing the draw means the offscreen actor doesn't render this
        // frame; the next frame after it returns to view will refresh the
        // cache and the static path resumes correctly.
        const bool isFirstFlush = !rng.firstFlushSeen;
        if (rng.multi->getCachedFrame() != currentFrame) {
            if (isFirstFlush) {
                ++s_firstFrameSkipCount;
                static int s_warnPrinted = 0;
                if (s_warnPrinted < 16) {
                    ++s_warnPrinted;
                    fprintf(stderr,
                        "[STATIC_FIRST_FRAME v1] event=skip_first_flush regIdx=%u "
                        "registeredOnFrame=%u currentFrame=%u cachedFrame=%u\n",
                        regIdx, rng.registeredOnFrame, currentFrame,
                        rng.multi->getCachedFrame());
                    fflush(stderr);
                }
            }
            ++s_diag_ranges_stale_frame;
            continue;
        }
        rng.firstFlushSeen = true;
        ++s_diag_ranges_drawn;
        // 2026-05-10 diag: env-gated dump of multi-leaf ranges (MC2_REGFLUSH_MULTI=1).
        {
            static const bool s_traceMulti = (getenv("MC2_REGFLUSH_MULTI") != nullptr);
            static std::set<uint32_t> s_seenMulti;
            if (s_traceMulti && rng.count > 1 && s_seenMulti.size() < 100 &&
                s_seenMulti.find(regIdx) == s_seenMulti.end()) {
                s_seenMulti.insert(regIdx);
                uint32_t firstTid = s_recipes[rng.first].typeID;
                fprintf(stderr, "[REGFLUSH_MULTI v1] regIdx=%u count=%u firstTypeID=%u multi=%p\n",
                    regIdx, rng.count, firstTid, (void*)rng.multi);
                fflush(stderr);
            }
        }

        // Patch lightDataIndex from CacheGpuLightData() result gathered in
        // TreeAppearance::render() immediately before markVisible(). The UBO
        // is reset every frame by resetLightData(), so the baked recipe value
        // is stale; we read the freshly-gathered slot here.
        // UINT32_MAX must not reach flush(): render() guards against emitting
        // a static instance when getCachedGpuLightIndex() == UINT32_MAX by
        // calling invalidateStaticRegistration() and falling through to the
        // dynamic submit path instead.
        //
        // 2026-05-11 per-instance light source-of-truth: when
        // MC2_STATIC_PER_INSTANCE_LIGHT=1 is set AND markVisible() captured a
        // non-sentinel value into rng.lightDataIndex, prefer it over the
        // multi's per-type cache. This retires the last-writer-wins aliasing
        // that produced the MC2_STATIC_UPDATE_SKIP=1 wrong-RGB residual:
        // multiple actor instances sharing one multi-shape were all reading
        // the same multi->cachedGpuLightIndex_ at flush time, getting whichever
        // sibling's update/touch ran last. Per-actor capture decouples them.
        // Without the env flag, behavior is byte-identical to the historical
        // path.
        const uint32_t freshLightIdx =
            (s_perInstanceLight && rng.lightDataIndex != 0xFFFFFFFFu)
                ? rng.lightDataIndex
                : rng.multi->getCachedGpuLightIndex();
        // 2026-05-05 black-billboard diagnostic: report numLights at the cached
        // slot. If numLights==0 here, calc_light returns base_light only — for
        // tree leaves with aRGBLight=0xFF000000 + BaseVertexColor=0, that's black.
        // Capped to first 16 SP_TRACE lines per session to avoid 300K-line spam
        // (~100 trees × 3000 frames in a 30s smoke).
        {
            static int s_flushTracePrinted = 0;
            if (s_trace && s_flushTracePrinted < 16) {
                ++s_flushTracePrinted;
                MC_TextureManager::LightSlotPeek peek = {-2, -2, 0, 0, 0};
                if (mcTextureManager) peek = mcTextureManager->peekLightSlot(freshLightIdx);
                const uint32_t structCount = mcTextureManager
                    ? mcTextureManager->getLightStructCount() : 0;
                // H2 trace: if peek.numLights>0 but firstColor is (0,0,0) and/or
                // firstType is AMBIENT, that explains tree-leaf vertices going
                // black on the static replay (base_light=0 + ambient*0 = 0).
                SP_TRACE("flush regIdx=%u lightIdx=%u count=%u nL=%d type0=%d c0=(%.3f,%.3f,%.3f) sc=%u",
                         regIdx, freshLightIdx, rng.count,
                         peek.numLights, peek.firstType,
                         peek.firstColorR, peek.firstColorG, peek.firstColorB,
                         structCount);
            }
        }

        // 2026-05-10 actor-center fix: every leaf of one multishape must share
        // the SAME substrate worldCenter (= the parent actor's position), so
        // the GPU frustum cull accepts/rejects all leaves of an actor as a
        // group. Without this, high-elevation leaves (LitWin_LookoutTower
        // ~60u up, S_admin roof tiles, watchtower platform tops) get
        // independently rejected by the sphere-vs-frustum test while the
        // base leaf passes — visually: base renders, all detail vanishes.
        // The first recipe in the range is the first SHAPE_NODE leaf
        // captured by registerStatic (typically the root/base mesh whose
        // local-to-actor transform is identity), so its modelMatrix
        // translation IS the actor's xlatPosition.
        const float* rootMtx = s_recipes[rng.first].modelMatrix;
        const float actorWorldCenter[3] = {
            -rootMtx[3],   // raw.x = -stuff.x
             rootMtx[11],  // raw.y =  stuff.z
             rootMtx[7],   // raw.z =  stuff.y (elev)
        };

        for (uint32_t i = 0; i < rng.count; ++i) {
            GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
            inst.lightDataIndex = freshLightIdx;
            batcher.submitCachedInstance(inst);

            // C1b GPU authority flip: emit one GpuActorRecord per submitted static prop
            // instance so the compute cull shader can scatter it into the correct bucket
            // (typeID). This appends to the already-flushed substrate slot; compute_dispatch()
            // runs AFTER this loop (moved from mission.cpp to txmmgr.cpp) and picks up the
            // updated hdr->recordCount.
            //
            // category encoding: low 4 bits = Cat_StaticProp (5), upper 28 bits = typeID.
            // Shader: uint cat = rec.category & CATEGORY_MASK; if cat == CAT_STATIC_PROP,
            //         uint bucket = rec.category >> 4;  →  correct bucket scatter.
            //
            // Flag_AlwaysVisible NOT set: the GPU frustum test runs (using worldCenter +
            // boundingRadius). This can conservatively cull props near the frustum edge;
            // acceptable because the CPU-side markVisible() already gates which recipes
            // reach this loop (CPU visibility IS the admission gate for registry path).
            // Any GPU-culled prop that the CPU admitted will just fall back to 0-count
            // draw — invisible for that frame, restored next frame as it re-enters frustum.
            if (gpu_cull::substrate_isEnabled()) {
                gpu_cull::GpuActorRecord gpuRec{};
                // World position: inst.modelMatrix is Stuff row-vector
                // convention stored in column-major array order
                // (mclib/stuff/matrix.hpp:133 — `entries[(column<<2)+row]`).
                // Translation lives at row 3, columns 0..2. With column-
                // major storage, M(row,col) = entries[(col<<2)+row], so:
                //   M(3,0) = entries[(0<<2)+3] = entries[3]   (tx)
                //   M(3,1) = entries[(1<<2)+3] = entries[7]   (ty)
                //   M(3,2) = entries[(2<<2)+3] = entries[11]  (tz)
                // entries[12]/[13]/[14] are the BOTTOM ROW of columns 0..2
                // (always 0 for affine transforms). Reading those produces
                // worldCenter≈(0,0,0) for every static prop — the cull then
                // projects every prop to the world origin, so all admit/
                // reject together as the camera rotates (1° flip = all on/
                // all off). Verified against Matrix4D::BuildTranslation at
                // mclib/stuff/matrix.cpp:214 and the existing diagnostic at
                // gos_static_prop_batcher.cpp:1879-1882.
                //
                // Coord-space: the translation is in Stuff/MLR camera frame
                // (.x=-rawX, .y=elev, .z=rawY) per BldgAppearance and
                // TreeAppearance::registerStatic in mclib/bdactor.cpp.
                // gos_GetTerrainMVPMat4 (axisSwap * worldToClip) expects raw
                // MC2 world coords (x=east, y=north, z=elev) and bakes the
                // swap, so unswap here:
                //   raw.x = -stuff.x  =  -entries[3]
                //   raw.y =  stuff.z  =   entries[11]
                //   raw.z =  stuff.y  =   entries[7]   (elev)
                // Mirrors the per-vertex swap in static_prop.vert at
                // `world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y)`.
                // 2026-05-10 actor-center fix: use the parent multishape's
                // root translation (computed once per range above) for EVERY
                // leaf's substrate record. Cull treats the actor as a single
                // visibility unit; all (typeID, leaf) records of one actor
                // accept-or-reject as a group. Per-leaf inst.modelMatrix
                // remains correct in the per-frame instance SSBO for shader
                // placement — only the cull-side worldCenter is unified.
                gpuRec.worldCenter[0] = actorWorldCenter[0];
                gpuRec.worldCenter[1] = actorWorldCenter[1];
                gpuRec.worldCenter[2] = actorWorldCenter[2];
                // Bounding radius: 200.0f covers stock MC2 buildings (largest are
                // ~150 units across, e.g. warehouse footprint). Trees/fences are
                // smaller but over-admission at the frustum edge is harmless —
                // CPU-side markVisible already gates which actors reach this code,
                // and the cull is sphere-aware via clipSpaceFrustumAdmitSphere
                // (gpu_cull_predicate.glsl). 50.0f (pre-2026-05-10) was too small:
                // building centroids offset from the visible silhouette failed the
                // strict point-in-frustum test even when most of the building was
                // on screen — empty-render symptom under substrate=ON.
                gpuRec.boundingRadius = 200.0f;
                gpuRec.worldAabbMin[0] = gpuRec.worldCenter[0] - gpuRec.boundingRadius;
                gpuRec.worldAabbMin[1] = gpuRec.worldCenter[1] - gpuRec.boundingRadius;
                gpuRec.worldAabbMin[2] = gpuRec.worldCenter[2] - gpuRec.boundingRadius;
                gpuRec.worldAabbMax[0] = gpuRec.worldCenter[0] + gpuRec.boundingRadius;
                gpuRec.worldAabbMax[1] = gpuRec.worldCenter[1] + gpuRec.boundingRadius;
                gpuRec.worldAabbMax[2] = gpuRec.worldCenter[2] + gpuRec.boundingRadius;
                // Category: typeID in upper 28 bits + Cat_StaticProp (5) in lower 4 bits.
                gpuRec.category = (static_cast<uint32_t>(inst.typeID) << 4)
                                | static_cast<uint32_t>(gpu_cull::Cat_StaticProp);
                // 2026-05-10 diag: temp force always-visible to A/B-test whether
                // the cull is rejecting buildings.
                static const bool s_diag_forceAdmit =
                    (getenv("MC2_STATIC_FORCE_ADMIT") != nullptr);
                gpuRec.flags          = s_diag_forceAdmit
                                          ? static_cast<uint32_t>(gpu_cull::Flag_AlwaysVisible)
                                          : gpu_cull::Flag_None;
                gpuRec.actorId        = 0u;   // static props have no actor handle
                gpuRec.prevVisibilityBit = 1u; // CPU admitted this prop this frame
                gpuRec.consumerFlags  = 0u;
                // C1b temporal-superset Slice 1: real terrain block index so
                // the block rollup can stamp the right block. worldCenter[0]
                // is raw-MC2 east, [1] raw-MC2 north (the -stuff.x unswap was
                // applied above producing the east-frame). Feed [0],[1] ONLY;
                // NEVER [2] (elevation). worldCenter fully populated above.
                gpuRec.blockIdx       = static_cast<uint32_t>(
                    Terrain::worldToBlockIdx(gpuRec.worldCenter[0],
                                             gpuRec.worldCenter[1]));
                // [BLKIDX v1] env-gated GEOMETRIC probe (demote-not-delete).
                // Non-degeneracy is provably blind to a frame mirror — assert
                // the helper == hand-rolled CPU block math for THIS prop's
                // true raw position, plus zero_verify of the [2]-exclusion.
                {
                    static const bool s_blkidxTrace =
                        (getenv("MC2_BLKIDX_TRACE") != nullptr);
                    if (s_blkidxTrace) {
                        const float pwx = gpuRec.worldCenter[0];
                        const float pwy = gpuRec.worldCenter[1];
                        long mx = ((long)pwx >> 7) + Terrain::halfVerticesMapSide;
                        long bx = (long)(mx * Terrain::oneOverVerticesBlockSide);
                        long my = Terrain::halfVerticesMapSide -
                                  (((long)pwy >> 7) + 1);
                        long by = (long)(my * Terrain::oneOverVerticesBlockSide);
                        long cpuBlk = bx + (by * Terrain::blocksMapSide);
                        long helperBlk = Terrain::worldToBlockIdx(pwx, pwy);
                        fprintf(stderr,
                            "[BLKIDX v1] event=geom_check src=registry"
                            " wx=%.1f wy=%.1f wz=%.1f helper=%ld cpu=%ld"
                            " match=%d\n",
                            pwx, pwy, gpuRec.worldCenter[2],
                            helperBlk, cpuBlk, (helperBlk == cpuBlk) ? 1 : 0);
                        // zero_verify: elevation [2] must NEVER influence the
                        // index — recompute ignoring [2] (trivially true here
                        // since helper takes only [0],[1]) and assert equality.
                        fprintf(stderr,
                            "[BLKIDX v1] event=zero_verify src=registry"
                            " blockIdx=%u z_excluded=1\n",
                            gpuRec.blockIdx);
                        fflush(stderr);
                    }
                }
                gpu_cull::substrate_appendStaticPropRecord(gpuRec);
                ++s_diag_leaves_appended;
                // 2026-05-10 diag: typeID histogram. MC2_REGFLUSH_TYPEHIST=1 to enable.
                {
                    static const bool s_th = (getenv("MC2_REGFLUSH_TYPEHIST") != nullptr);
                    static std::array<uint64_t, 1024> s_typeHist{};
                    if (s_th) {
                        if (inst.typeID < s_typeHist.size()) ++s_typeHist[inst.typeID];
                        if (s_diag_flush_calls == 600 && i == 0) {
                            fprintf(stderr, "[REGFLUSH_TYPEHIST v1] non-zero buckets:\n");
                            for (size_t t = 0; t < s_typeHist.size(); ++t) {
                                if (s_typeHist[t] > 0) {
                                    fprintf(stderr, "[REGFLUSH_TYPEHIST v1] typeID=%zu count=%llu\n",
                                        t, (unsigned long long)s_typeHist[t]);
                                }
                            }
                            fflush(stderr);
                        }
                    }
                }
            }
        }
    }
    s_diag_total_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _flush_t0).count());
    static const bool s_regflushTrace = (getenv("MC2_REGFLUSH_DIAG_TRACE") != nullptr);
    if (s_regflushTrace && (s_diag_flush_calls % 600) == 0) {
        const double mean_us = (s_diag_flush_calls > 0)
            ? (static_cast<double>(s_diag_total_ns) /
               static_cast<double>(s_diag_flush_calls)) / 1000.0
            : 0.0;
        fprintf(stderr,
            "[REGFLUSH_DIAG v1] event=summary calls=%llu ranges_seen=%llu "
            "tombstone=%llu stale_frame=%llu drawn=%llu leaves_appended=%llu liveSize=%zu "
            "mean_us=%.2f\n",
            (unsigned long long)s_diag_flush_calls,
            (unsigned long long)s_diag_ranges_total,
            (unsigned long long)s_diag_ranges_tombstone,
            (unsigned long long)s_diag_ranges_stale_frame,
            (unsigned long long)s_diag_ranges_drawn,
            (unsigned long long)s_diag_leaves_appended,
            s_liveRangeIndices.size(), mean_us);
        fflush(stderr);
    }
    // compute_dispatch() runs after this (moved to txmmgr.cpp between registry flush
    // and batcher flush) so it sees the appended static prop records.
    // batcher.flush() is called by txmmgr.cpp immediately after compute_dispatch().
}

} // namespace GpuStaticPropRegistry
