#include "gos_static_prop_registry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

// Rejects "0", "false", "off", "no"; accepts anything else (including "1").
// Matches the ParseEnvBool pattern in code/terrobj.cpp:79-85.
static bool parseEnvBool(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return false;
    if (v[0] == '0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}

// Parsed at file-scope (program start) so isEnabled() is valid before init()
// is called and before the [INSTR v1] banner fires.
static const bool s_enabled = parseEnvBool("MC2_STATIC_PROP_REGISTRY");
static const bool s_trace   = parseEnvBool("MC2_STATIC_PROP_TRACE");

#define SP_TRACE(fmt, ...) \
    do { if (s_trace) { printf("[STATIC_PROP] " fmt "\n", ##__VA_ARGS__); \
         fflush(stdout); } } while (0)

namespace {

struct RecipeRange {
    uint32_t       first;   // index into s_recipes
    uint32_t       count;   // 0 = invalidated (tombstone)
    TG_MultiShape* multi;   // for per-frame lightDataIndex patch via
                            // getCachedGpuLightIndex(); NULL when count==0
};

static std::vector<GpuStaticPropInstance> s_recipes;
static std::vector<RecipeRange>           s_recipeRanges;

// Per-frame list of regIdx values (one per visible tree).
// markVisible() appends one regIdx per tree; flush() expands to leaves
// and patches lightDataIndex from the live TG_MultiShape.
static std::vector<uint32_t>              s_liveRangeIndices;

} // namespace

namespace GpuStaticPropRegistry {

bool isEnabled() { return s_enabled; }

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
    s_recipes.insert(s_recipes.end(), batch.begin(), batch.end());
    const int32_t regIdx = static_cast<int32_t>(s_recipeRanges.size());
    s_recipeRanges.push_back(rng);
    SP_TRACE("register regIdx=%d first=%u count=%u", regIdx, rng.first, rng.count);
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
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    for (uint32_t regIdx : s_liveRangeIndices) {
        const RecipeRange& rng = s_recipeRanges[regIdx];
        if (rng.count == 0 || !rng.multi) continue; // tombstone guard

        // Patch lightDataIndex from CacheGpuLightData() result gathered in
        // TreeAppearance::render() immediately before markVisible(). The UBO
        // is reset every frame by resetLightData(), so the baked recipe value
        // is stale; we read the freshly-gathered slot here.
        // UINT32_MAX must not reach flush(): render() guards against emitting
        // a static instance when getCachedGpuLightIndex() == UINT32_MAX by
        // calling invalidateStaticRegistration() and falling through to the
        // dynamic submit path instead.
        const uint32_t freshLightIdx = rng.multi->getCachedGpuLightIndex();
        SP_TRACE("flush regIdx=%u lightIdx=%u count=%u", regIdx, freshLightIdx, rng.count);

        for (uint32_t i = 0; i < rng.count; ++i) {
            GpuStaticPropInstance inst = s_recipes[rng.first + i]; // stack copy
            inst.lightDataIndex = freshLightIdx;
            batcher.submitCachedInstance(inst);
        }
    }
    // batcher.flush() is called by txmmgr.cpp immediately after this returns.
}

} // namespace GpuStaticPropRegistry
