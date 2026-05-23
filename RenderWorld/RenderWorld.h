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

// M1.5: per-slot inspection record. Indexed by handle.index().
// Always populated (M1 decision: mission/upsert-time RenderWorld
// metadata; ~85 KB peak at tier1 mc2_24 = 2641 props). Slot recycle
// bumps generation; alive=false marks a retired slot.
//
// Most fields are documentary in M1.5 (PipelineId / DrawPacket /
// pathReasonCode have no real consumers yet); their sentinels are
// returned through LookupResult so M2+ slices can fill them without
// API churn.
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
};

static constexpr uint16_t kRenderObjectFlagAlive = 1u << 0;

// M1.5: result of lookupAtPixel. isValid=false on background pixel or
// generation mismatch (stale-pixel-after-destroy). Caller MUST check
// isValid before consuming any other field.
struct LookupResult {
    bool                            isValid          = false;
    RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
    uint32_t                        meshHandleBits   = 0;
    uint32_t                        materialHandleBits = 0;
    uint8_t                         lodLevel         = 0xFFu;
    uint16_t                        pipelineId       = 0;
    uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
    uint32_t                        pathReasonCode   = 0;
    uint32_t                        gameObjectId     = 0;
};

// M1.5: synchronous pixel -> handle lookup. screenX/Y in GL convention
// (origin bottom-left). Returns LookupResult{isValid=false} when env-OFF,
// FBO not initialized, pixel==0, or generation mismatch. Stalls the GPU
// to read the prior frame's attachment-2 -- intended for click-time
// (max ~10/sec) debug; not per-frame.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 7
LookupResult lookupAtPixel(int screenX, int screenY);

} // namespace RenderWorld
