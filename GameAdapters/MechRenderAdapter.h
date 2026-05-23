// GameAdapters/MechRenderAdapter.h
//
// Slice M2 (route-only): mech spawn/destroy lifecycle adapter.
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md
//
// Include discipline (load-bearing):
//   - Header MAY include RenderCore/Handle.h only (RenderCore is pure types;
//     no GL, no game-side headers).
//   - Header MAY forward-declare class Mech3DAppearance.
//   - Header MUST NOT include mech3d.h, RenderWorld.h, or any game-side
//     or engine implementation header.
//   - The .cpp includes mech3d.h and RenderWorld.h (the only TU that may).
//   - Adapter is TEMPORARY per spec Section 10 deletion criteria.

#pragma once

#include <cstdint>
#include "../RenderCore/Handle.h"

// Forward-decl game-side mech appearance. Spec section 12 carve-out:
// adapter headers may forward-declare game-side types; the .cpp includes
// the real header.
class Mech3DAppearance;

namespace GameAdapters {
namespace Mech {

// Per-mission lifecycle. Wire alongside GpuMechBatcher::instance().onMapLoad()
// and GameAdapters::StaticProp::beginMission() at code/mission.cpp:1695
// (currently that line calls StaticProp::beginMission; Mech::beginMission
// is added adjacent, per the mission.cpp wiring in Task 4).
void beginMission();
void endMission();  // safety sweep / force-clear after warning; see spec Section 8.3

// Spawn hook. Call AFTER appearance->initFX() succeeds (code/mech.cpp:1311).
// Takes a mutable reference so the adapter can call
// mech.setRenderWorldHandleForAdapter() on the appearance instance.
// gameObjectId is an opaque engine-side cookie (0 in M2; M2.5 refines).
//
// Returns invalid() on RenderWorld failure. The handle is also stored
// in mech via setRenderWorldHandleForAdapter(); caller should assert both
// are consistent in debug builds.
RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId);

// Destroy hook. Call BEFORE delete appearance (code/mech.cpp:3724-3728).
// Retires the handle in RenderWorld and calls mech.clearRenderWorldHandleForAdapter().
// No-op if mech.getRenderWorldHandle() is already invalid().
//
// THIS is the AUTHORITATIVE handle retirement path. endMission() is a
// safety sweep only and must not be relied upon for per-mech cleanup.
void destroyMech(Mech3DAppearance& mech);

} // namespace Mech
} // namespace GameAdapters
