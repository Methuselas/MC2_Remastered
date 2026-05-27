// RenderWorld/legacy/static_prop_backend.cpp
//
// Slice M1 (C1 fix): the ONLY engine-side TU that touches
// gos_static_prop_batcher.h and GpuStaticPropRegistry::*.

#include "static_prop_backend.h"

#include "gos_static_prop_registry.h"
#include "gos_static_prop_batcher.h"  // GpuStaticPropInstance

#include <cstring>
#include <type_traits>
#include <vector>

namespace {

// Size compatibility check between the POD mirror and the legacy struct.
// If the legacy struct grows or shrinks, this check fires at compile
// time and the executor must update StaticPropInstanceDesc to match.
static_assert(sizeof(RenderCore::StaticPropInstanceDesc) ==
              sizeof(GpuStaticPropInstance),
              "StaticPropInstanceDesc and GpuStaticPropInstance "
              "size mismatch; update RenderCore/StaticPropInstanceDesc.h.");

static_assert(std::is_trivially_copyable<GpuStaticPropInstance>::value,
              "GpuStaticPropInstance must be trivially copyable for "
              "memcpy bridge.");
static_assert(std::is_trivially_copyable<
                  RenderCore::StaticPropInstanceDesc>::value,
              "StaticPropInstanceDesc must be trivially copyable.");

inline GpuStaticPropInstance toEngine(
    const RenderCore::StaticPropInstanceDesc& src) {
    GpuStaticPropInstance dst;
    std::memcpy(&dst, &src, sizeof(dst));
    return dst;
}

} // namespace

namespace RenderWorld {
namespace legacy {

int32_t registerStaticPropRecipe(RenderCore::StaticPropDesc desc) {
    if (desc.shape == nullptr || desc.batch.empty()) {
        return -1;
    }
    std::vector<GpuStaticPropInstance> engineBatch;
    engineBatch.reserve(desc.batch.size());
    for (const auto& src : desc.batch) {
        engineBatch.push_back(toEngine(src));
    }
    return GpuStaticPropRegistry::registerRecipe(desc.shape, engineBatch);
}

void invalidateStaticProp(int32_t recipeIndex) {
    GpuStaticPropRegistry::invalidate(recipeIndex);
}

void markVisibleStaticProp(int32_t recipeIndex,
                           uint32_t lightDataIndex,
                           float extentRadius) {
    GpuStaticPropRegistry::markVisible(recipeIndex,
                                       lightDataIndex,
                                       extentRadius);
}

bool isReadyStaticProp(int32_t recipeIndex) {
    return GpuStaticPropRegistry::isReady(recipeIndex);
}

uint64_t getStaticPropActiveCount() {
    return static_cast<uint64_t>(GpuStaticPropRegistry::getActiveCount());
}

uint64_t getStaticPropLastFlushLiveCount() {
    return GpuStaticPropRegistry::getLastFlushLiveCount();
}

} // namespace legacy
} // namespace RenderWorld
