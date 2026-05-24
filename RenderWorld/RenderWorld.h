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

// M1.6: static-prop pick master enable. When this is OFF, the missiongui
// Shift+click wiring is dormant even if MC2_OBJECT_ID_BUFFER=1. Three-gate
// opt-in stack per spec Section 9; defense-in-depth so a dev can enable
// the substrate for log-driven inspection without changing click behavior.
//
// Process-lifetime cached; restart required to flip. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick guard
bool IsStaticPropPickEnabled();

// M1.6: static-prop pick verbose-log enable. When this is OFF, the
// `[STATIC_PROP_PICK v1] miss ...` line is suppressed (high-frequency
// empty-Shift+click gesture would otherwise spam stderr). `hit` lines
// fire unconditionally per spec Section 7.
//
// Process-lifetime cached. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch
bool IsStaticPropPickDebugEnabled();

// M1.5 C1 fix: centralize Handle encoding. Returns 0 for invalid
// recipeIndex (< 0). The producer in gos_static_prop_batcher.cpp
// calls this with the result of GpuStaticPropRegistry::getRecipeIndexForType().
uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex);

// M2: kind tag for the unified handle/record table. Every RenderObjectHandle
// issued by this module has an associated kind stored in the record.
// The kind disambiguates static props from mechs (and future kinds) when
// a caller examines a handle returned by lookupAtPixel or any other API.
//
// Values are stable across releases (never renumber; only append).
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // Future: Terrain=2, Vfx=3, Overlay=4
};

// M1.5: per-slot inspection record. Indexed by handle.index().
// Always populated (M1 decision: mission/upsert-time RenderWorld
// metadata; ~127 KB peak at tier1 mc2_24 = 2641 props (48 bytes/record
// with M2 kind+debugCookie fields)). Slot recycle bumps generation;
// alive=false marks a retired slot.
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
    // M2: kind tag. Populated by registerMech (kind=Mech) and upsertStaticProp
    // (kind=StaticProp). lookupAtPixel callers MUST check kind before consuming
    // kind-specific fields.
    RenderObjectKind kind     = RenderObjectKind::StaticProp;  // default for M1 legacy slots
    // M2: opaque debug cookie. Stored for log output; never dereferenced by engine.
    // For mechs: reinterpret_cast<uintptr_t>(&mech3DAppearance). For static props: 0.
    uintptr_t debugCookie     = 0;
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
    // M2.6: kind discriminator copied from RenderObjectRecord.kind.
    // Caller MUST check this before consuming kind-specific fields
    // (recipeIndex for StaticProp; BattleMech reverse-lookup for Mech).
    // Defaults to StaticProp to preserve M1.6 caller behavior on an
    // isValid=false return (callers should gate on isValid first
    // anyway; the default is only relevant for compile-time
    // initializer compatibility).
    RenderObjectKind                kind             = RenderObjectKind::StaticProp;
};

// M1.5: synchronous pixel -> handle lookup. screenX/Y in GL convention
// (origin bottom-left). Returns LookupResult{isValid=false} when env-OFF,
// FBO not initialized, pixel==0, or generation mismatch. Stalls the GPU
// to read the prior frame's attachment-2 -- intended for click-time
// (max ~10/sec) debug; not per-frame.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 7
LookupResult lookupAtPixel(int screenX, int screenY);

// M1.6: most-recent static-prop pick debug state. Updated by
// MissionInterfaceManager::tryStaticPropPick on a successful Shift+click
// pick. Single-slot (latest wins); not a selection list. Not serialized.
// Cleared on per-mission RenderWorld::destroy() so a stale handle does
// not survive mission-load boundaries.
//
// Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6.
struct StaticPropSelectionDebugState {
    bool                            valid              = false;
    RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
    int32_t                         recipeIndex        = -1;
    int32_t                         lastPickMouseX     = 0;  // Win32 origin top-left
    int32_t                         lastPickMouseY     = 0;
    int32_t                         lastPickGlX        = 0;  // GL origin bottom-left
    int32_t                         lastPickGlY        = 0;
    uint64_t                        lastPickFrameIndex = 0;  // mirrors s_frameCounter at pick time
};

// M1.6: populate from a valid LookupResult. Asserts internally that
// res.isValid == true (callers must filter; debug build only).
// mouseX/Y are Win32-convention click coords; glX/Y are the post-y-flip
// GL coords passed to lookupAtPixel. lastPickFrameIndex is sampled from
// the internal frame counter at call time.
void setLastStaticPropPick(const LookupResult& res,
                           int32_t mouseX, int32_t mouseY,
                           int32_t glX,    int32_t glY);

// M1.6: reset to default (valid=false). Idempotent. Called on
// (a) empty Shift+click (Q1 lean: clear on miss), and
// (b) per-mission RenderWorld::destroy() lifecycle hook (Q2 lean).
void clearLastStaticPropPick();

// M1.6: read-only access. Caller MUST check .valid before consuming any
// other field. Returns a copy (the struct is tiny; avoids exposing
// internal mutex state to callers).
StaticPropSelectionDebugState getLastStaticPropPick();

// M2: mech spawn descriptor. Engine types only -- no Mech3DAppearance*,
// no Mech3DAppearanceType*. Firewall: spec section 12 + M2 spec section 10.
//
// mechTypeId: 0 in M2 by design (type identity deferred to M2.5).
//   RenderWorld MUST NOT dereference it.
//
// gameObjectId: opaque uint32_t; the engine-side echo of a game-side
//   identifier. Never dereferenced by RenderWorld. Used for future
//   object-ID correlation in M2.5.
//
// debugCookie: opaque uintptr_t; never dereferenced by engine. Carries
//   the raw Mech3DAppearance* echo for log output in MC2_RENDER_WORLD_TRACE
//   builds. RenderWorld stores it in the record but never casts or follows it.
struct RenderMechDesc {
    uint32_t  mechTypeId;    // 0 in M2; real value deferred to M2.5
    uint32_t  gameObjectId;
    uintptr_t debugCookie;
};

// M2: register a mech with RenderWorld. Returns a new RenderObjectHandle
// on success; invalid() on failure (OOM or internal error).
//
// MUST NOT be called upsert-style (no overwrite of an existing handle).
// This is spawn-only: if the caller's handle is already valid, it means
// a prior destroyMech was missed. The adapter asserts on this in debug.
//
// Route-only in M2: no new GPU path. RenderWorld records the handle in
// the unified s_objectRecords table with kind=Mech; the handle is valid
// from this call until destroyMech.
RenderCore::RenderObjectHandle registerMech(RenderMechDesc desc);

// M2: retire a mech handle. No-op on invalid() input.
//
// AUTHORITATIVE handle retirement path. After this call the handle is
// invalid; any subsequent use of the old handle with lookupAtPixel or
// any future API returns invalid/false.
//
// endMission() force-clears remaining live mech records after logging
// a warning; correctness of per-mech destroy during normal play must
// not depend on endMission() being called.
void destroyMech(RenderCore::RenderObjectHandle h);

// M2: force-clear all live mech records in the unified table. Called by
// GameAdapters::Mech::endMission() after logging a leaked-handle warning.
// Marks every record with kind=Mech and alive=true as alive=false,
// bumps generation, and decrements s_mechs_alive_rw for each.
void clearAllMechRecords();

} // namespace RenderWorld
