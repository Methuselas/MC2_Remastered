#pragma once
#include <cstdint>
#include <vector>
#include "gos_static_prop_batcher.h"

// TG_MultiShape forward-declared via gos_static_prop_batcher.h -> msl.h.

namespace GpuStaticPropRegistry {

// One-time init / teardown (called from code/mission.cpp alongside batcher).
void init();
void destroy();

// Returns true iff MC2_STATIC_PROP_REGISTRY=1 was set at startup.
// Valid before init() because flags are parsed at file scope.
bool isEnabled();

// Called once per frame from gamecam.cpp BEFORE land->render().
// Clears the per-frame live-range list.
void frameBegin();

// Called from TreeAppearance::render() after a successful first-time
// submitMultiShape(). Stores multi (for per-frame lightDataIndex patch)
// and snapshots the batch returned by GpuStaticPropBatcher::getLastBuiltBatch().
// Returns recipeIndex (>= 0) on success, -1 if disabled or OOM.
int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch);

// Called from TreeAppearance::render() when IsStaticNow() is true (and
// getCachedGpuLightIndex() is valid). Appends regIdx to the per-frame live
// list; flush() does leaf expansion + lightDataIndex patch.
void markVisible(int32_t regIdx);

// Called when static registration must be cleared (fall, late-reg recovery,
// shape-pointer change, UINT32_MAX light index). Sets recipe range to
// count=0 (tombstone) and NULLs multi; caller must also clear staticReg.
void invalidate(int32_t regIdx);

// Returns true iff regIdx is valid and not invalidated (count > 0).
bool isReady(int32_t regIdx);

// Called from txmmgr.cpp BEFORE GpuStaticPropBatcher::instance().flush().
// For each live regIdx: reads multi->getCachedGpuLightIndex() (freshened by
// CacheGpuLightData() in render()), patches lightDataIndex in a stack copy
// of each leaf recipe, injects via submitCachedInstance(). Batcher flush()
// then draws everything in one combined GPU pass.
void flush();

// [STATIC_FIRST_FRAME v1] proof-of-fix accessor (Track B Task 4).
// Returns the count of registrations whose VERY FIRST flush() attempt was
// rejected by the staleness gate. Must read zero after Task 3's cachedFrame_
// pre-population; non-zero means the pre-population didn't protect the entry.
uint64_t getStaticFirstFrameSkipCount();

} // namespace GpuStaticPropRegistry
