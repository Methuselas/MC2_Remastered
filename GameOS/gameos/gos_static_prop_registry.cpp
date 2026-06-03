#include "gos_static_prop_registry.h"
#include "../../mclib/txmmgr.h"  // 2026-05-05: peekLightSlotNumLights/getLightStructCount for flush trace
#include "../../mclib/appear.h"   // Task 6: Appearance* for registerStaticProp()
#include "../../mclib/apprtype.h"  // AppearanceType::name (inspector shapeName capture)
#include "gpu_cull_substrate.h"  // C1b GPU authority flip: substrate_appendStaticPropRecord
#include "gpu_cull_record.h"     // C1b: GpuActorRecord, Cat_StaticProp, CategoryMask
#include "../../mclib/terrain.h" // C1b temporal-superset: Terrain::worldToBlockIdx()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <unordered_map>
#include <intrin.h>  // __rdtsc — [SPFLUSH_COST_SPLIT v1]

// MC_TextureManager singleton, defined in mclib/txmmgr.cpp.
extern MC_TextureManager* mcTextureManager;

// 2026-05-05: frame counter for cull-aware static replay. When an actor goes
// offscreen, MC2's cull gate skips its update() (`cull_gates_are_load_bearing.md`),
// which means CacheGpuLightData()/ResubmitCachedGpuLightData() also doesn't run,
// so its cachedGpuLightIndex_ points at a UBO slot whose content was filled by
// a different actor THIS frame (or is beyond the upload count). flush() must
// detect this and skip emitting a draw for that recipe.
extern uint32_t g_mc2FrameCounter;

// [LIGHTBAKE-PROOF v1] stability trace observer (defined in mclib/txmmgr.cpp).
// File-scope declaration — never declared inside flush(). Verifies the per-instance
// permanent lightDataIndex is stable across frames + stays in the static prefix [0..S).
extern void mc2LightBakeStabilityObserve(int32_t recipeIndex, uint32_t lightDataIndex);

// ---------------------------------------------------------------------------
// [SPFLUSH_COST_SPLIT v1] — env gate + RDTSC storage + TSC calibration.
// Gate: MC2_STATIC_PROP_FLUSH_COST_SPLIT=1, default OFF.
// All accumulation is no-op when the gate is unset (checked per-frame at
// the top of flush() before any RDTSC reads, AND in the adder functions
// in gos_static_prop_batcher.cpp for the callee-side buckets).
// ---------------------------------------------------------------------------
static const bool s_spflushEnabled = []() {
    const char* v = getenv("MC2_STATIC_PROP_FLUSH_COST_SPLIT");
    return v && v[0] == '1' && v[1] == '\0';
}();

// TSC -> ns calibration. Computed once on first flush() call under the gate.
// Spin std::chrono::steady_clock for ~1ms, measure __rdtsc() delta.
static double s_spflushCyclesPerNs = 1.0;  // safe default: 1 cycle/ns (no divide-by-zero)
static bool   s_spflushCalibrated  = false;

static void spflushCalibrate() {
    if (s_spflushCalibrated) return;
    using Clock = std::chrono::steady_clock;
    const auto wall0 = Clock::now();
    const unsigned long long tsc0 = __rdtsc();
    // Spin ~1ms
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - wall0).count() < 1000) { /* spin */ }
    const unsigned long long tsc1 = __rdtsc();
    const long long wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 Clock::now() - wall0).count();
    if (wallUs > 0 && tsc1 > tsc0) {
        const double wallNs = static_cast<double>(wallUs) * 1000.0;
        s_spflushCyclesPerNs = static_cast<double>(tsc1 - tsc0) / wallNs;
    }
    s_spflushCalibrated = true;
}

// Per-frame (window) RDTSC cycle accumulators — reset every 10 frames.
namespace {
// Registry-side buckets (accumulated in flush())
unsigned long long s_w_submit_loop_total_cyc    = 0;
unsigned long long s_w_inst_build_cyc           = 0;
unsigned long long s_w_actor_record_build_cyc   = 0;
unsigned long long s_w_world_to_block_idx_cyc   = 0;
unsigned long long s_w_substrate_append_cyc     = 0;
// Lifetime dirty-rate counters (monotonic, registry-side only)
unsigned long long s_total_invalidates           = 0;
unsigned long long s_total_registrations         = 0;
// recipe_rebuilds / light_index_writes are sourced from txmmgr::bakeStaticLightSlot
// via spflush_ConsumeRecipeRebuildsDelta() + spflush_GetRecipeRebuildTotal() — not tracked here.
// (light_index_writes == recipe_rebuilds — both happen at bakeStaticLightSlot; only one kept.)
// Window dirty-rate deltas (reset after each summary emit)
unsigned long long s_win_invalidates             = 0;
unsigned long long s_win_registrations           = 0;
// Per-frame pass-through counters (use existing s_diag_* but we snapshot them)
int                s_spflushWindowFrames         = 0;
unsigned long long s_win_leaves_appended         = 0;
unsigned long long s_win_ranges_drawn            = 0;
}  // namespace

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

// STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: build cached immutable instance +
// per-recipe actor-record content once and bulk-append per frame instead of
// per-leaf rebuild. Default OFF until Tracy-proven; =1 enables.
static const bool s_flushCachedBlob =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_FLUSH_CACHED_BLOB", false);
// Diagnostic compare (patch 8): when the cached path is active, ALSO build the
// legacy temp instance+record for each leaf and compare hash/count; log any
// mismatch. Default OFF. Requires MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1.
static const bool s_flushCachedBlobCompare =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE", false);

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
    // 2026-05-22 per-prop extent radius (F4 T3 static-prop pop-in fix):
    // world-unit bounding sphere radius from bldgShape/treeShape->GetExtentRadius().
    // Written to GpuActorRecord.boundingRadius in flush(). 0.0f = uncaptured;
    // flush falls back to 200.0f (legacy placeholder) for unpatched callers.
    float              extentRadius;
    // Inspector: AppearanceType::name captured at registerStaticPropAndReturnRecipe
    // time (late-spawn path only; bulk registerRecipe path has no name available
    // in RelWithDebInfo). Empty string = name not captured.
    char               shapeName[128];
    // SHADOW-STATIC-BUILDINGS-2: population tag set at registration via
    // setRecipePopulation() (caller knows Building vs Tree). 0xFF = unset
    // (excluded from the static building shadow). Visibility-independent — the
    // static building shadow pass replays from here, NOT per-frame buckets.
    uint8_t            population;
};

static std::vector<GpuStaticPropInstance> s_recipes;
static std::vector<RecipeRange>           s_recipeRanges;

// v1.1: per-typeID primary material cache. Populated by finalizeGeometry().
// Indexed by typeID (dense); resized as needed by staticPropCacheTypePrimaryMaterial.
static std::vector<GpuStaticPropRegistry::StaticPropTypeMaterialCache> s_typeMatCache;

// v2: tracks whether flush() called substrate_appendStaticPropRecord for each recipe.
// Indexed by recipeIndex (parallel to s_recipeRanges). Reset to 0 at flush() start;
// set to 1 after substrate_appendStaticPropRecord in the flush loop.
// Cleared entirely in destroy() (see the ClearCullSubmissionState helper below).
// Do NOT clear in staticPropRegistryClearMaterialCache() — different lifecycle.
static std::vector<uint8_t> s_recipeHasSubstrateRecord;

// M1.5 C1 fix: typeID -> recipeIndex side-map. Populated by
// registerRecipe(); set to -1 on invalidate(). Lookup returns -1
// if typeID is unknown. Last-write-wins: if two recipes register
// with the same typeID (unusual but legal), the second overrides;
// the first becomes unreachable via this map but remains addressable
// via its returned recipeIndex.
static std::unordered_map<uint32_t, int32_t> s_typeIDToRecipeIndex;

// Per-frame list of regIdx values (one per visible tree).
// markVisible() appends one regIdx per tree; flush() expands to leaves
// and patches lightDataIndex from the live TG_MultiShape.
static std::vector<uint32_t>              s_liveRangeIndices;

// V1A: per-frame visible range count latched at flush() entry (before
// expansion and before frameBegin() clears the vector next frame).
// queryVisibility() reads this for static_props_visible.
static uint64_t                           s_lastFlushLiveCount = 0;

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

// TASK3: real impl replaces this stub (adds s_cachedActorRecord arrays).
// Placed inside the anonymous namespace so it can access the cached arrays
// that Task 3 will add here. With gate OFF (default), this stub fires at
// most once per recipe (idempotent early-return in setRecipePermanentLightIndex).
static inline void invalidateCachedFlushRecord(uint32_t) {}  // TASK3: real impl replaces this stub

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

// [SPFLUSH_COST_SPLIT v1] txmmgr-side consume fns. Declared at FILE scope
// (same reason as above — inside the namespace they mangle incorrectly).
extern unsigned long long spflush_ConsumeBaseInstanceUploadCycles();
extern unsigned long long spflush_ConsumeRecipeRebuildsDelta();
extern unsigned long long spflush_GetRecipeRebuildTotal();

namespace GpuStaticPropRegistry {

bool isEnabled()               { return s_enabled; }
bool isMissionLoadRegEnabled() { return s_missionLoadRegEnabled; }
bool isLateSpawnRegEnabled()   { return s_lateSpawnRegEnabled; }

uint64_t getStaticFirstFrameSkipCount()    { return s_firstFrameSkipCount; }
uint64_t getLateSpawnTypeUnknownCount()    { return s_lateSpawnTypeUnknownCount; }

// STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A (Task 1)
// 2A: persist the proven-permanent per-instance light slot into the recipe so
// flush() needs no per-frame light patch. Written once at bake (idempotent).
// patch 4: writing an immutable recipe field MUST dirty the cached flush record
// for that recipe (so the next flush rebuilds it from the new value).
void setRecipePermanentLightIndex(int32_t recipeIndex, uint32_t lightDataIndex) {
    if (recipeIndex < 0) return;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri >= s_recipes.size()) return;
    if (s_recipes[ri].lightDataIndex == lightDataIndex) return;  // no-op: stay clean
    s_recipes[ri].lightDataIndex = lightDataIndex;   // permanent slot == recipeIndex
    invalidateCachedFlushRecord(ri);                 // patch 3/4 — defined in Task 3
}

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

int32_t registerStaticPropAndReturnRecipe(Appearance* app) {
    if (!isLateSpawnRegEnabled()) return -1;
    if (!app) return -1;
    app->registerStatic();
    if (!app->isStaticRegistered()) {
        ++s_lateSpawnTypeUnknownCount;
        return -1;
    }
    const int32_t recipeIdx = app->getStaticRecipeIndex();
    // Capture appearance name for the inspector (AppearanceType::name always
    // present, not debug-gated). Bulk registerRecipe callers have no name.
    if (recipeIdx >= 0 && static_cast<size_t>(recipeIdx) < s_recipeRanges.size()) {
        AppearanceTypePtr at = app->getAppearanceType();
        if (at && at->name) {
            std::strncpy(s_recipeRanges[static_cast<size_t>(recipeIdx)].shapeName,
                         at->name, 127);
            s_recipeRanges[static_cast<size_t>(recipeIdx)].shapeName[127] = '\0';
        }
    }
    return recipeIdx;
}

uint32_t getActiveCount() {
    // Live recipe count = ranges with count > 0 (non-tombstoned).
    uint32_t n = 0;
    for (const auto& rng : s_recipeRanges) {
        if (rng.count > 0) ++n;
    }
    return n;
}

uint64_t getLastFlushLiveCount() {
    // V1A: per-frame visible range count latched at flush() entry.
    // Returns 0 before the first flush (mission not yet loaded).
    return s_lastFlushLiveCount;
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
    s_typeMatCache.clear();          s_typeMatCache.shrink_to_fit();
    staticPropRegistryClearCullSubmissionState();
    // unordered_map has no shrink_to_fit(); swap with empty to release bucket
    // allocation back to the heap, matching the intent of the surrounding
    // vector .clear()+.shrink_to_fit() pattern. Mission 2 may register a
    // completely different set of type IDs, so retaining mission 1's bucket
    // layout would just waste memory.
    s_typeIDToRecipeIndex.clear();
    std::unordered_map<uint32_t, int32_t>().swap(s_typeIDToRecipeIndex);
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
    rng.extentRadius      = 0.0f;
    rng.shapeName[0]      = '\0';         // populated by late-spawn path if Appearance* available
    rng.population        = 0xFFu;        // SHADOW-STATIC-BUILDINGS-2: unset until setRecipePopulation()
    s_recipes.insert(s_recipes.end(), batch.begin(), batch.end());
    const int32_t regIdx = static_cast<int32_t>(s_recipeRanges.size());
    s_recipeRanges.push_back(rng);
    s_recipeHasSubstrateRecord.push_back(0u); // v2: one slot per recipe, parallel to s_recipeRanges
    // M1.5 C1: maintain typeID -> recipeIndex side-map for the
    // batcher's objectIdRaw producer. All instances in `batch` share
    // the same typeID in practice (one recipe = one multi-shape =
    // one type); take the first.
    if (!batch.empty()) {
        s_typeIDToRecipeIndex[batch[0].typeID] = regIdx;
    }
    // [SPFLUSH_COST_SPLIT v1] lifetime registration counter.
    if (s_spflushEnabled) { ++s_total_registrations; ++s_win_registrations; }
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

void markVisible(int32_t regIdx, uint32_t lightDataIndex, float extentRadius) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    if (rng.count == 0) return; // tombstone
    // 2026-05-11: capture per-actor lightDataIndex (the multi's cachedGpuLightIndex_
    // at the moment THIS actor's update/touch wrote it, before sibling actors of
    // the same multi-type overwrote it). flush() consumes this when
    // MC2_STATIC_PER_INSTANCE_LIGHT=1 is set; otherwise flush ignores it.
    rng.lightDataIndex = lightDataIndex;
    // 2026-05-22 F4 T3: store per-prop extent radius for GPU cull record.
    rng.extentRadius = extentRadius;
    s_liveRangeIndices.push_back(static_cast<uint32_t>(regIdx));
}

void invalidate(int32_t regIdx) {
    if (!s_enabled) return;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    // [SPFLUSH_COST_SPLIT v1] lifetime invalidation counter.
    // LOD swaps manifest as paired invalidate+registration churn and are not
    // separately identifiable here — no separate LOD counter.
    if (s_spflushEnabled) { ++s_total_invalidates; ++s_win_invalidates; }
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    releasePinsForRange(rng);
    // M1.5 C1: tombstone the side-map entry BEFORE zeroing s_recipes
    // (typeID would otherwise be zeroed out by the loop below). Only
    // tombstone if our regIdx still owns the mapping; a newer recipe
    // with the same typeID may have taken over.
    if (rng.count > 0 && rng.first < s_recipes.size()) {
        const uint32_t typeID = s_recipes[rng.first].typeID;
        auto it = s_typeIDToRecipeIndex.find(typeID);
        if (it != s_typeIDToRecipeIndex.end() && it->second == regIdx) {
            it->second = -1;
        }
    }
    for (uint32_t i = 0; i < rng.count; ++i)
        s_recipes[rng.first + i] = GpuStaticPropInstance{};
    SP_TRACE("invalidate regIdx=%d (was count=%u)", regIdx, rng.count);
    rng.count = 0;
    // v2: zero-out substrate tracking (don't erase — keeps index stable).
    if (static_cast<size_t>(regIdx) < s_recipeHasSubstrateRecord.size())
        s_recipeHasSubstrateRecord[static_cast<size_t>(regIdx)] = 0u;
    rng.multi  = nullptr;
    rng.lightDataIndex = 0xFFFFFFFFu;  // 2026-05-11 reset capture on invalidate
    rng.extentRadius   = 0.0f;         // 2026-05-22 F4 T3 reset extent radius
    // [LIGHTBAKE v1] drop the baked static-light entry so destruction/LOD
    // multi-swap lazily re-bakes the same position-derived constant.
    ::mc2EraseBakedStaticLight(regIdx);
}

bool isReady(int32_t regIdx) {
    if (!s_enabled) return false;
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return false;
    return s_recipeRanges[static_cast<uint32_t>(regIdx)].count > 0;
}

// M1.5 C1 fix: typeID -> recipeIndex side-map accessor. Returns -1
// for unknown typeID (or after invalidate()).
int32_t getRecipeIndexForType(uint32_t typeID) {
    auto it = s_typeIDToRecipeIndex.find(typeID);
    if (it == s_typeIDToRecipeIndex.end()) return -1;
    return it->second;
}

const char* getRecipeShapeName(int32_t recipeIndex) {
    if (recipeIndex < 0 || static_cast<size_t>(recipeIndex) >= s_recipeRanges.size())
        return nullptr;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    if (rng.count == 0) return nullptr;   // tombstoned
    return rng.shapeName[0] ? rng.shapeName : nullptr;
}

void flush() {
    // V1A: latch BEFORE any early return so queryVisibility() always sees
    // a current-frame value (0 when disabled or nothing visible this frame).
    s_lastFlushLiveCount = static_cast<uint64_t>(s_liveRangeIndices.size());
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
    // v2: reset per-recipe cull-submission tracking for this frame.
    // Timing: flush(N) clears → sets bits; extraction(N+1) reads before flush(N+1).
    // So extraction always sees frame N state, NOT the current frame being built.
    std::fill(s_recipeHasSubstrateRecord.begin(), s_recipeHasSubstrateRecord.end(), 0u);
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
        if (s_spflushEnabled) ++s_win_ranges_drawn; // [SPFLUSH_COST_SPLIT v1]
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
        // [LIGHTBAKE-PROOF v1] prove the resolved per-instance index is permanent
        // (stable across frames) + in the static prefix [0..S). No-op unless
        // MC2_LIGHTBAKE_STABILITY is set.
        mc2LightBakeStabilityObserve(static_cast<int32_t>(regIdx), freshLightIdx);
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

        // [SPFLUSH_COST_SPLIT v1] submit_loop_total span wraps the whole leaf loop
        // for this range. One span per range covering all its leaves.
        const unsigned long long _t_loop0 = s_spflushEnabled ? __rdtsc() : 0ULL;
        for (uint32_t i = 0; i < rng.count; ++i) {
            // [SPFLUSH_COST_SPLIT v1] inst_build span: stack copy + lightDataIndex patch.
            const unsigned long long _t_inst0 = s_spflushEnabled ? __rdtsc() : 0ULL;
            GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
            inst.lightDataIndex = freshLightIdx;
            if (s_spflushEnabled) s_w_inst_build_cyc += __rdtsc() - _t_inst0;
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
            // [SPFLUSH_COST_SPLIT v1] actor_record_build span starts here.
            const unsigned long long _t_arb0 = s_spflushEnabled ? __rdtsc() : 0ULL;
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
                // Bounding radius: use per-prop extent radius captured by markVisible()
                // from bldgShape/treeShape->GetExtentRadius() (F4 T3, 2026-05-22).
                // This fixes static-prop pop-in: post-F1 the correct GL-NDC matrix
                // produces a tighter clipSpaceFrustumAdmitSphere envelope than the
                // old D3D-pixel-homog matrix; large props (>200 units) were
                // over-culled when their centroid exited the frustum by >200 units.
                // Fallback to 200.0f when extentRadius == 0.0f (unpatched callers or
                // missing shape pointer) preserves prior behavior.
                gpuRec.boundingRadius = (rng.extentRadius > 0.0f) ? rng.extentRadius : 200.0f;
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
                // [SPFLUSH_COST_SPLIT v1] world_to_block_idx span (nested inside actor_record_build).
                const unsigned long long _t_wtb0 = s_spflushEnabled ? __rdtsc() : 0ULL;
                gpuRec.blockIdx       = static_cast<uint32_t>(
                    Terrain::worldToBlockIdx(gpuRec.worldCenter[0],
                                             gpuRec.worldCenter[1]));
                if (s_spflushEnabled) s_w_world_to_block_idx_cyc += __rdtsc() - _t_wtb0;
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
                // [SPFLUSH_COST_SPLIT v1] substrate_append span.
                const unsigned long long _t_sa0 = s_spflushEnabled ? __rdtsc() : 0ULL;
                gpu_cull::substrate_appendStaticPropRecord(gpuRec);
                if (s_spflushEnabled) s_w_substrate_append_cyc += __rdtsc() - _t_sa0;
                ++s_diag_leaves_appended;
                if (s_spflushEnabled) ++s_win_leaves_appended; // [SPFLUSH_COST_SPLIT v1]
                // v2: record substrate submission for this recipe (extraction reads previous frame's state).
                if (regIdx < static_cast<uint32_t>(s_recipeHasSubstrateRecord.size()))
                    s_recipeHasSubstrateRecord[regIdx] = 1u;
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
            // [SPFLUSH_COST_SPLIT v1] actor_record_build span ends here (includes world_to_block_idx).
            if (s_spflushEnabled) s_w_actor_record_build_cyc += __rdtsc() - _t_arb0;
        }
        // [SPFLUSH_COST_SPLIT v1] submit_loop_total span ends after all leaves of this range.
        if (s_spflushEnabled) s_w_submit_loop_total_cyc += __rdtsc() - _t_loop0;
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
    // [SPFLUSH_COST_SPLIT v1] -- per-10-frame summary emit.
    // s_win_leaves_appended / s_win_ranges_drawn are incremented directly in the
    // leaf loop above (alongside the existing s_diag_* counters). Window accumulators
    // are reset after each emit, so they hold the current window-period totals.
    if (s_spflushEnabled) {
        if (!s_spflushCalibrated) spflushCalibrate();
        ++s_spflushWindowFrames;
        if (s_spflushWindowFrames >= 10) {
            // Consume batcher-side callee accumulators (read+reset).
            const unsigned long long map_lookup_cyc  = spflush_cost_split::ConsumeSubmitMapLookupCycles();
            const unsigned long long color_fill_cyc  = spflush_cost_split::ConsumeColorZeroFillCycles();
            // Consume txmmgr-side accumulators (file-scope externs declared below namespace).
            const unsigned long long bi_upload_cyc       = ::spflush_ConsumeBaseInstanceUploadCycles();
            const unsigned long long win_recipe_rebuilds = ::spflush_ConsumeRecipeRebuildsDelta();
            const unsigned long long tot_recipe_rebuilds = ::spflush_GetRecipeRebuildTotal();
            // Convert cycles -> ns using the calibrated cycles_per_ns.
            const double cpns = s_spflushCyclesPerNs;
            const double wf   = static_cast<double>(s_spflushWindowFrames);
            auto cyc2ns = [&](unsigned long long c) -> long long {
                return static_cast<long long>(static_cast<double>(c) / cpns / wf);
            };
            // Per-frame leaf/range averages (window totals / frames).
            const long long leaves_pf  = static_cast<long long>(s_win_leaves_appended / static_cast<unsigned long long>(s_spflushWindowFrames));
            const long long ranges_pf  = static_cast<long long>(s_win_ranges_drawn    / static_cast<unsigned long long>(s_spflushWindowFrames));
            fprintf(stderr,
                "[SPFLUSH_COST_SPLIT v1] event=summary frames=10 "
                "leaves=%lld ranges=%lld "
                "submit_loop_ns=%lld inst_build_ns=%lld "
                "map_lookup_ns=%lld color_fill_ns=%lld "
                "actor_record_ns=%lld world_to_block_ns=%lld "
                "substrate_append_ns=%lld baseinstance_upload_ns=%lld "
                "| dirty(window): invalidates=%llu registrations=%llu rebuilds=%llu light_writes=%llu"
                " | dirty(total): invalidates=%llu registrations=%llu rebuilds=%llu light_writes=%llu\n",
                leaves_pf, ranges_pf,
                cyc2ns(s_w_submit_loop_total_cyc),
                cyc2ns(s_w_inst_build_cyc),
                cyc2ns(map_lookup_cyc),
                cyc2ns(color_fill_cyc),
                cyc2ns(s_w_actor_record_build_cyc),
                cyc2ns(s_w_world_to_block_idx_cyc),
                cyc2ns(s_w_substrate_append_cyc),
                cyc2ns(bi_upload_cyc),
                s_win_invalidates, s_win_registrations, win_recipe_rebuilds, win_recipe_rebuilds,
                s_total_invalidates, s_total_registrations, tot_recipe_rebuilds, tot_recipe_rebuilds);
            fflush(stderr);
            // Reset window accumulators.
            s_w_submit_loop_total_cyc  = 0;
            s_w_inst_build_cyc         = 0;
            s_w_actor_record_build_cyc = 0;
            s_w_world_to_block_idx_cyc = 0;
            s_w_substrate_append_cyc   = 0;
            s_win_leaves_appended      = 0;
            s_win_ranges_drawn         = 0;
            s_win_invalidates          = 0;
            s_win_registrations        = 0;
            // s_win_recipe_rebuilds reset inside spflush_ConsumeRecipeRebuildsDelta() (txmmgr).
            s_spflushWindowFrames      = 0;
        }
    }
    // compute_dispatch() runs after this (moved to txmmgr.cpp between registry flush
    // and batcher flush) so it sees the appended static prop records.
    // batcher.flush() is called by txmmgr.cpp immediately after compute_dispatch().
}

// --- Extraction v1: per-recipe read-only accessors ---

static bool recipeValid(int32_t recipeIndex) {
    if (recipeIndex < 0 || recipeIndex >= static_cast<int32_t>(s_recipeRanges.size()))
        return false;
    return s_recipeRanges[static_cast<size_t>(recipeIndex)].count > 0; // count==0 = tombstone
}

bool staticPropGetModelMatrix(int32_t recipeIndex, float out[16]) {
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    memcpy(out, s_recipes[rng.first].modelMatrix, sizeof(float) * 16);
    return true;
}

bool staticPropGetTypeId(int32_t recipeIndex, uint32_t* out) {
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    *out = s_recipes[rng.first].typeID;
    return true;
}

// SHADOW-STATIC-BUILDINGS-2: tag a recipe's population at registration time.
void setRecipePopulation(int32_t recipeIndex, GpuStaticPropPopulation pop) {
    if (recipeIndex < 0 ||
        recipeIndex >= static_cast<int32_t>(s_recipeRanges.size())) return;
    s_recipeRanges[static_cast<size_t>(recipeIndex)].population =
        static_cast<uint8_t>(pop);
}

// SHADOW-STATIC-BUILDINGS-2: append every non-tombstoned BUILDING recipe's leaf
// instances (baked modelMatrix + typeID) to `out`. Visibility-independent — reads
// the full registry (s_recipes/s_recipeRanges), NOT the per-frame visible buckets.
// Trees and unset-population recipes are excluded. Used once at the static
// shadow-map build to replay all rigid buildings into the world-fixed depth map.
void getBuildingShadowInstances(std::vector<GpuStaticPropInstance>& out) {
    out.clear();
    const uint8_t bldg = static_cast<uint8_t>(GpuStaticPropPopulation::Building);
    for (const RecipeRange& rng : s_recipeRanges) {
        if (rng.count == 0) continue;            // tombstone (invalidated/destroyed)
        if (rng.population != bldg) continue;     // buildings only; trees/unset excluded
        const size_t end = static_cast<size_t>(rng.first) + rng.count;
        if (end > s_recipes.size()) continue;     // defense: stale range
        out.insert(out.end(),
                   s_recipes.begin() + rng.first,
                   s_recipes.begin() + end);
    }
}

// SHADOW-DYNAMIC-PROP-CASTERS-1: append every non-tombstoned NON-BUILDING recipe's
// leaf instances (trees/fences/generic props) to `out`. Visibility-independent —
// reads the full registry (s_recipes/s_recipeRanges), NOT the per-frame visible
// buckets. Buildings are EXCLUDED (population==Building) because they cast via the
// world-fixed static map; everything else (Tree/Generic/Legacy and unset recipes,
// which are non-buildings in practice — buildings are tagged Building at register)
// is included so the dynamic shadow pass admits ALL props regardless of camera
// visibility. Mirror of getBuildingShadowInstances with the filter inverted.
void getDynamicPropShadowInstances(std::vector<GpuStaticPropInstance>& out,
                                   bool includeBuildings) {
    out.clear();
    const uint8_t bldg = static_cast<uint8_t>(GpuStaticPropPopulation::Building);
    for (const RecipeRange& rng : s_recipeRanges) {
        if (rng.count == 0) continue;            // tombstone (invalidated/destroyed)
        if (!includeBuildings && rng.population == bldg) continue;  // buildings cast via the static map
        const size_t end = static_cast<size_t>(rng.first) + rng.count;
        if (end > s_recipes.size()) continue;     // defense: stale range
        out.insert(out.end(),
                   s_recipes.begin() + rng.first,
                   s_recipes.begin() + end);
    }
}

bool staticPropGetExtentRadius(int32_t recipeIndex, float* out) {
    if (!recipeValid(recipeIndex)) return false;
    *out = s_recipeRanges[static_cast<size_t>(recipeIndex)].extentRadius;
    return true;
}

bool staticPropGetLightDataIndex(int32_t recipeIndex, uint32_t* out) {
    if (!recipeValid(recipeIndex)) return false;
    *out = s_recipeRanges[static_cast<size_t>(recipeIndex)].lightDataIndex;
    return true;
}

// --- Extraction v1.1: per-typeID primary material cache ---

void staticPropCacheTypePrimaryMaterial(uint32_t typeID,
                                        int32_t  texArrayLayer,
                                        uint32_t materialIdx,
                                        bool     hasMaterialIdx,
                                        bool     wasAlphaOn,
                                        bool     multiPacket,
                                        uint8_t  alphaClass,
                                        uint32_t packetCount,
                                        uint32_t firstPacket) {
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) {
        s_typeMatCache.resize(typeID + 1u); // default-init: hasPrimary=false
    }
    StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    // Type metadata: always idempotent (same type → same values).
    // Written unconditionally BEFORE the prefer-alpha-off early-return checks
    // so alphaClass/packetCount/firstPacket are always set regardless of primary outcome.
    c.alphaClass   = alphaClass;
    c.packetCount  = packetCount;
    c.firstPacket  = firstPacket;
    // Prefer alpha-off primary over alpha-on fallback.
    // Rule: alpha-off overwrites alpha-on; nothing overwrites alpha-off.
    if (c.hasPrimary) {
        if (!c.primaryWasAlphaOn) return; // already have alpha-off primary; done
        if (wasAlphaOn)           return; // both alpha-on; keep first
        // Upgrading from alpha-on fallback to alpha-off primary -- fall through.
    }
    c.hasPrimary        = true;
    c.primaryWasAlphaOn = wasAlphaOn;
    c.multiPacket       = multiPacket;
    c.texArrayLayer     = texArrayLayer;
    c.materialIdx       = materialIdx;
    c.hasMaterialIdx    = hasMaterialIdx;
}

void staticPropRegistryClearMaterialCache() {
    s_typeMatCache.clear();
    s_typeMatCache.shrink_to_fit();
}

void staticPropRegistryClearCullSubmissionState() {
    s_recipeHasSubstrateRecord.clear();
    s_recipeHasSubstrateRecord.shrink_to_fit();
}

bool staticPropGetHasCullRecord(int32_t recipeIndex, bool* out) {
    if (!out) return false;
    *out = false;
    if (!recipeValid(recipeIndex)) return false;
    if (static_cast<size_t>(recipeIndex) < s_recipeHasSubstrateRecord.size())
        *out = (s_recipeHasSubstrateRecord[static_cast<size_t>(recipeIndex)] != 0u);
    return true;
}

bool staticPropGetTexArrayLayer(int32_t recipeIndex, int32_t* out) {
    if (!out) return false;
    *out = -1;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary) return false;
    *out = c.texArrayLayer;
    return true;
}

bool staticPropGetMaterialIdx(int32_t recipeIndex, uint32_t* out) {
    if (!out) return false;
    *out = 0xFFFFFFFFu;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary || !c.hasMaterialIdx) return false;
    *out = c.materialIdx;
    return true;
}

bool staticPropGetMaterialCacheInfo(int32_t recipeIndex,
                                    StaticPropTypeMaterialCache* out) {
    if (!out) return false;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary) return false;
    *out = c;
    return true;
}

void staticPropGetMaterialCacheStats(MaterialCacheStats* out) {
    if (!out) return;
    *out = {};
    out->cacheVectorSize = static_cast<uint32_t>(s_typeMatCache.size());
    for (const auto& c : s_typeMatCache) {
        if (c.hasPrimary) {
            ++out->texWired;
            if (c.hasMaterialIdx)    ++out->matWired;
            if (c.multiPacket)       ++out->multiPacket;
            if (c.primaryWasAlphaOn) ++out->alphaOnFallback;
        } else {
            ++out->noPrimary; // informational only; may include resize-padding slots
        }
    }
}

bool staticPropGetInstanceFlags(int32_t recipeIndex, uint32_t* out) {
    if (!out) return false;
    *out = 0u;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    *out = s_recipes[rng.first].flags;
    return true;
}

} // namespace GpuStaticPropRegistry

// ---------------------------------------------------------------------------
// STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A (Task 1): cross-TU free function
// Defined at file scope after the GpuStaticPropRegistry namespace so the
// linker sees an unmangled symbol (same pattern as mc2EraseBakedStaticLight).
// Declared extern at file scope in txmmgr.cpp (NOT inside any function).
// ---------------------------------------------------------------------------
void mc2RegistrySetRecipePermanentLightIndex(int32_t recipeIndex, uint32_t lightDataIndex) {
    GpuStaticPropRegistry::setRecipePermanentLightIndex(recipeIndex, lightDataIndex);
}
