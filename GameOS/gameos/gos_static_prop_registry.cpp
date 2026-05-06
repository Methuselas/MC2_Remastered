#include "gos_static_prop_registry.h"
#include "../../mclib/txmmgr.h"  // 2026-05-05: peekLightSlotNumLights/getLightStructCount for flush trace
#include "../../mclib/appear.h"  // Task 6: Appearance* for registerStaticProp()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

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
// Task 5 (Track B): opt-in bulk registration at mission load.
// Default OFF until verified across all mission + biome variants.
// Set MC2_STATIC_PROP_MISSION_LOAD_REG=1 to enable.
static const bool s_missionLoadRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_MISSION_LOAD_REG", false);
// Task 6 (Track B): opt-in late-spawn registration for actors spawned after
// mission load (artillery, reinforcements, warrior waypoint markers).
// Default OFF until verified; Task 9 flips alongside mission-load reg.
// Set MC2_STATIC_PROP_LATE_SPAWN_REG=1 to enable.
static const bool s_lateSpawnRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_LATE_SPAWN_REG", false);

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

void markVisible(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    if (s_recipeRanges[static_cast<uint32_t>(regIdx)].count == 0) return; // tombstone
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
    for (uint32_t regIdx : s_liveRangeIndices) {
        RecipeRange& rng = s_recipeRanges[regIdx];
        if (rng.count == 0 || !rng.multi) continue; // tombstone guard

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
            continue;
        }
        rng.firstFlushSeen = true;

        // Patch lightDataIndex from CacheGpuLightData() result gathered in
        // TreeAppearance::render() immediately before markVisible(). The UBO
        // is reset every frame by resetLightData(), so the baked recipe value
        // is stale; we read the freshly-gathered slot here.
        // UINT32_MAX must not reach flush(): render() guards against emitting
        // a static instance when getCachedGpuLightIndex() == UINT32_MAX by
        // calling invalidateStaticRegistration() and falling through to the
        // dynamic submit path instead.
        const uint32_t freshLightIdx = rng.multi->getCachedGpuLightIndex();
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

        for (uint32_t i = 0; i < rng.count; ++i) {
            GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
            inst.lightDataIndex = freshLightIdx;
            batcher.submitCachedInstance(inst);
        }
    }
    // batcher.flush() is called by txmmgr.cpp immediately after this returns.
}

} // namespace GpuStaticPropRegistry
