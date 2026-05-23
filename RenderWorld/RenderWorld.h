// RenderWorld/RenderWorld.h
//
// Slice M1: engine-facing scene API. The first surface in the
// RenderWorld boundary; takes engine types only.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 4 (lifecycle), section 13 (first-slice scope).
//
// Firewall (spec section 12, load-bearing):
//   - This header MUST NOT include any game-side header.
//   - This header MUST NOT forward-declare any game-side type
//     (no `class Appearance;`, no `class BldgAppearance;`, ...).
//   - Adapters in GameAdapters/ may bridge both sides; this header
//     may not.

#pragma once

#include "../RenderCore/Handle.h"
#include "../RenderCore/RenderObjectDesc.h"

namespace RenderWorld {

// Lifecycle.
//
// Phase 1 / M1: a process-singleton implementation lives in
// RenderWorld.cpp. M1 does not yet model mission scope at the
// RenderWorld layer; static-prop slots are mission-lifetime by virtue
// of the underlying GpuStaticPropRegistry behavior. M2+ promotes
// begin/endMission to a real boundary call.
void init();
void destroy();

// Engine-facing upsert. Adapter calls this after building a StaticPropDesc
// from a game-side Appearance. The desc is MOVED in; ownership of the
// vector inside the desc transfers to the forwarder.
//
// Returns RenderObjectHandle::invalid() on failure. Caller MUST translate
// at the adapter boundary if a legacy sentinel (-1) is expected upward.
RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc);

// m5 fix (adversarial review pass 2 2026-05-22): late-spawn path. The
// adapter's syncStaticPropLateSpawn calls the legacy
// GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(Appearance*)
// which actually creates the recipe; THIS function wraps the already-
// created recipe index in a Handle so the counter and handle table stay
// honest. Does NOT create a new registry entry. Returns invalid() if
// recipeIndex < 0.
RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex);

// Engine-facing destroy. Adapter calls this when a registration must be
// torn down (per-actor invalidate path). No-op on invalid() input.
void destroy(RenderCore::RenderObjectHandle h);

// Engine-facing visibility mark. M1: thin forwarder onto
// GpuStaticPropRegistry::markVisible. lightDataIndex and extentRadius
// match the existing registry signature; default args preserved.
void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex = 0xFFFFFFFFu,
                 float    extentRadius  = 0.0f);

// Validity probe (engine-side; useful for debug asserts above the adapter).
bool isReady(RenderCore::RenderObjectHandle h);

// Frame banner emission. Called once per frame from gamecam.cpp's
// frame-end path (Task 14). Emits `[RENDER_WORLD v1]` with the current
// active prop count read from the underlying registry counters.
//
// Env-gated:
//   MC2_RENDER_WORLD_TRACE=1 -> per-frame banner
//   default                  -> monotonic 600-frame summary
void frameBannerTick();

// M1.5: object-ID buffer env-flag accessor. Reads
// MC2_OBJECT_ID_BUFFER once at first call; subsequent calls return the
// cached value. Flipping the env var requires a process restart (the
// linked shader's GLSL macro is fixed at program-load time per
// memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
//
// Consumed by:
//   - gos_postprocess.cpp setSceneDrawBuffers() / createFBOs() / beginScene()
//   - gos_static_prop_batcher.cpp producer (objectIdRaw fill)
//   - RenderWorld.cpp lookupAtPixel() guard
//   - RenderWorld.cpp frameBannerTick() banner token
//   - C++ side of static-prop makeProgram() (gates the GLSL #ifdef prefix)
bool IsObjectIdBufferEnabled();

} // namespace RenderWorld
