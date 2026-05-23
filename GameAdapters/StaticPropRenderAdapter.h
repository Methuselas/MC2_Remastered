// GameAdapters/StaticPropRenderAdapter.h
//
// Slice M1: the ONLY module that may bridge gameData and engine sides.
// Per spec section 12 carve-out, an adapter header may forward-declare
// game-side types; the .cpp may include real game-side headers.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 10 (legacy adapter migration), section 13 (M1 scope).
//
// Sentinel translation rule (load-bearing, spec section 10 amendment
// 2026-05-22): -1 (recipeIndex) <-> RenderCore::RenderObjectHandle::invalid()
// at each game/engine seam. M1 has two seams (GameAdapters seam +
// RenderWorld legacy-backend seam); this header lives at the
// GameAdapters seam. The int32_t MUST NOT leak upward past this header;
// the Handle MUST NOT leak downward into the registry.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../RenderCore/Handle.h"

// Forward-decl game-side base. Spec section 12 carve-out: adapter
// HEADERS may forward-decl; adapter .cpp may include real header.
class Appearance;
class TG_MultiShape;
struct GpuStaticPropInstance;  // adapter .cpp translates via the POD mirror

namespace GameAdapters {
namespace StaticProp {

// M1 fix (adversarial review pass 2): spec D3 entry-point names.
//
// Per-mission lifecycle. Wired at code/mission.cpp:1693 and :3279
// alongside GpuStaticPropRegistry::init/destroy (m1 fix).
void beginMission();
void endMission();

// M1 surface: building / tree first-render and bulk-register sites.
// Returns invalid() on failure (registry disabled, OOM, or batch empty).
//
// Caller MUST persist the returned handle alongside its existing
// staticReg.recipeIndex (M1 keeps the int32_t storage; see Plan
// Decision D4). The legacyRecipeIndexOut OUT parameter exists for the
// transitional period: bdactor.cpp's staticReg.recipeIndex field stays
// int32_t in M1 (slot-side storage; not a public boundary). The adapter
// writes the legacy sentinel value there; the Handle is returned
// for any future upward consumer.
RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// m5 fix: late-spawn entry point (warrior.cpp:7593 path). Uses the
// new GpuStaticPropRegistry::registerStaticPropAndReturnRecipe() to
// obtain the recipe index, then calls
// RenderWorld::adoptStaticPropRecipe() to wrap it in a real Handle
// (so the [RENDER_WORLD v1] counter stays honest).
//
// Returns invalid() on failure. legacyRecipeIndexOut receives the
// raw recipe index (or -1) for any caller that still tracks the
// legacy storage type.
RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// Destroy / invalidate. Mirrors GpuStaticPropRegistry::invalidate but
// goes through RenderWorld so counters and sentinel translation stay
// centralized. Currently unused in M1 (the bdactor sites that
// invalidate do so via separate paths; promotion to this API is a
// future-slice cleanup). Declared here so the surface is shape-complete.
void destroyStaticProp(RenderCore::RenderObjectHandle h);

} // namespace StaticProp
} // namespace GameAdapters
