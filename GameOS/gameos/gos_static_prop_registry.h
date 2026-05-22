#pragma once
#include <cstdint>
#include <vector>
#include "gos_static_prop_batcher.h"

// TG_MultiShape forward-declared via gos_static_prop_batcher.h -> msl.h.
// Appearance forward-declared for registerStaticProp().
class Appearance;

namespace GpuStaticPropRegistry {

// One-time init / teardown (called from code/mission.cpp alongside batcher).
void init();
void destroy();

// Returns true iff MC2_STATIC_PROP_REGISTRY=1 was set at startup.
// Valid before init() because flags are parsed at file scope.
bool isEnabled();

// Returns true iff MC2_STATIC_PROP_MISSION_LOAD_REG=1 was set at startup.
// Gates the mission-load bulk registration walk in GameObjectManager.
bool isMissionLoadRegEnabled();

// Returns true iff MC2_STATIC_PROP_LATE_SPAWN_REG=1 was set at startup.
// Gates the late-spawn per-actor registration calls in spawn sites.
bool isLateSpawnRegEnabled();

// Task 6 (Track B): late-spawn registration API.
// Calls app->registerStatic() under isLateSpawnRegEnabled() guard.
// Returns true iff app->isStaticRegistered() after the call.
// Increments HC-3 counter on failure for gate-signal tracking.
bool registerStaticProp(Appearance* app);

// HC-3 gate signal: count of late-spawn registration attempts where
// isStaticRegistered() returned false (type unknown or ineligible).
// Emitted in destroy() as [STATIC_PROP_REG v1] event=type_unknown_at_late_spawn.
uint64_t getLateSpawnTypeUnknownCount();

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
//
// 2026-05-11: lightDataIndex is the slot value captured at this actor's
// update()/touch() time (when the multi's cache was THIS actor's slot
// before sibling actors of the same multi-type overwrote it). flush()
// uses this per-instance value when MC2_STATIC_PER_INSTANCE_LIGHT=1;
// without the flag, flush() ignores it and reads the multi's cache (the
// historical last-writer-wins behavior). Default arg keeps source-compat
// for callers that haven't been updated.
// extentRadius: per-prop world-unit bounding sphere radius from
// bldgShape->GetExtentRadius() / treeShape->GetExtentRadius().
// Written into GpuActorRecord.boundingRadius at flush time so the GPU
// clipSpaceFrustumAdmitSphere test uses the actual prop footprint rather
// than the old hardcoded 200.0f placeholder. Pass 0.0f when unknown;
// flush falls back to 200.0f (preserves pre-fix behavior for unpatched
// callers).
void markVisible(int32_t regIdx, uint32_t lightDataIndex = 0xFFFFFFFFu,
                 float extentRadius = 0.0f);

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
