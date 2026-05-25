// GameAdapters/StaticPropRenderAdapter.cpp
//
// Slice M1: the bridge .cpp. Includes BOTH sides.

#include "StaticPropRenderAdapter.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"
#include "../RenderCore/RenderObjectDesc.h"
#include "../RenderCore/StaticPropInstanceDesc.h"
// m5 fix: late-spawn path uses the new return-recipe accessor.
#include "gos_static_prop_registry.h"
// C1 fix: adapter is one of the two seams for POD-mirror translation;
// it MUST include the batcher to translate GpuStaticPropInstance ->
// StaticPropInstanceDesc.
#include "gos_static_prop_batcher.h"

// Game side. Per spec section 12 + adversarial M2 carve-out, this is
// the ONLY .cpp where game-side headers may be reached.
#include "appear.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

// C1 fix: game-side seam for POD-mirror translation. Engine-side seam
// lives in RenderWorld/legacy/static_prop_backend.cpp. Both perform
// the same translation; both have static_asserts guarding size match.
static_assert(sizeof(RenderCore::StaticPropInstanceDesc) ==
              sizeof(GpuStaticPropInstance),
              "StaticPropInstanceDesc / GpuStaticPropInstance size "
              "mismatch; update RenderCore/StaticPropInstanceDesc.h.");

static_assert(std::is_trivially_copyable<GpuStaticPropInstance>::value,
              "GpuStaticPropInstance must be trivially copyable.");

inline RenderCore::StaticPropInstanceDesc toMirror(
    const GpuStaticPropInstance& src) {
    RenderCore::StaticPropInstanceDesc dst;
    std::memcpy(&dst, &src, sizeof(dst));
    return dst;
}

} // namespace

namespace GameAdapters {
namespace StaticProp {

// Per-mission lifecycle. m1 fix: wired at mission.cpp:1693 / :3279.
// Sibling of GpuStaticPropRegistry::init/destroy (per-mission, NOT
// per-process). Currently a thin pair around RenderWorld::init/destroy;
// promoted to real boundary calls in M2+ (see spec section 4).
void beginMission() {
    RenderWorld::init();
}

void endMission() {
    RenderWorld::destroy();
}

RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut) {

    if (shape == nullptr || batchCount == 0) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    RenderCore::StaticPropDesc desc;
    desc.shape = shape;
    desc.batch.reserve(batchCount);
    for (size_t i = 0; i < batchCount; ++i) {
        desc.batch.push_back(toMirror(batchData[i]));
    }

    RenderCore::RenderObjectHandle h =
        RenderWorld::upsertStaticProp(std::move(desc));

    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut =
            h.isValid() ? static_cast<int32_t>(h.index()) : -1;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_static "
            "shape=%p batch=%zu handle.valid=%d legacy=%d\n",
            (void*)shape, batchCount, (int)h.isValid(),
            legacyRecipeIndexOut ? *legacyRecipeIndexOut : -2);
    }
    return h;
}

RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut) {

    if (!app) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    const int32_t recipe =
        GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(app);

    RenderCore::RenderObjectHandle h =
        RenderWorld::adoptStaticPropRecipe(recipe);

    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut = recipe;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_late "
            "app=%p recipe=%d handle.valid=%d\n",
            (void*)app, recipe, (int)h.isValid());
    }
    return h;
}

void destroyStaticProp(RenderCore::RenderObjectHandle h) {
    RenderWorld::destroy(h);
}

void getRecipeShapeName(int32_t recipeIndex, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (recipeIndex < 0) return;
    const char* name = GpuStaticPropRegistry::getRecipeShapeName(recipeIndex);
    if (!name) return;
    std::strncpy(out, name, outLen - 1);
    out[outLen - 1] = '\0';
}

bool getMaterialGpuData(uint32_t materialHandleBits, RenderCore::MaterialGpu* out) {
    if (!out || materialHandleBits == 0u) return false;
    RenderCore::MaterialHandle mh;
    mh.bits = materialHandleBits;
    return batcher_getMaterialGpuEntry(mh.index(), out);
}

bool isMaterialGpuActive() {
    RenderCore::MaterialGpu dummy;
    // Table is active if entry 0 exists (one sentinel entry is always uploaded
    // when MC2_MATERIAL_GPU=1 and at least one prop registered).
    return batcher_getMaterialGpuEntry(0u, &dummy);
}

} // namespace StaticProp
} // namespace GameAdapters
