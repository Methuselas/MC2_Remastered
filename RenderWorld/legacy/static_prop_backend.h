// RenderWorld/legacy/static_prop_backend.h
//
// Slice M1 (C1 fix): internal bridge between RenderWorld and the
// legacy GpuStaticPropRegistry. The .cpp owns every include of
// gos_static_prop_batcher.h and every call into
// GpuStaticPropRegistry::*.
//
// This header MUST NOT be included from RenderCore/, GameAdapters/,
// or anywhere outside RenderWorld/. The firewall script grandfathers
// the path RenderWorld/legacy/ to keep these reach-throughs scoped.

#pragma once

#include <cstdint>
#include "../../RenderCore/RenderObjectDesc.h"

namespace RenderWorld {
namespace legacy {

// Engine-side translation seam: StaticPropInstanceDesc -> GpuStaticPropInstance.
// Calls GpuStaticPropRegistry::registerRecipe. Returns recipe index
// (>= 0 on success, -1 on disabled / OOM / empty batch).
int32_t registerStaticPropRecipe(RenderCore::StaticPropDesc desc);

// Forwarders. recipe index is int32_t at this layer; sentinel
// translation happens at the seams.
void invalidateStaticProp(int32_t recipeIndex);
void markVisibleStaticProp(int32_t recipeIndex,
                           uint32_t lightDataIndex,
                           float extentRadius);
bool isReadyStaticProp(int32_t recipeIndex);

// m4 fix: registry active-recipe count for [RENDER_WORLD v1] objects=.
uint64_t getStaticPropActiveCount();

// V1A: per-frame visible range count latched at GpuStaticPropRegistry::flush() entry.
// Returns 0 before the first flush (mission not yet loaded).
uint64_t getStaticPropLastFlushLiveCount();

} // namespace legacy
} // namespace RenderWorld
