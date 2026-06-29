#pragma once
// RENDER-VIEW-CURRENCY-1 — the ONE sanctioned way object/mech/prop draw obtains
// the terrain dispatch MVP snapshot.
//
// Invariant: camera-dependent snapshot data may only be used if its VIEW EPOCH
// matches the current authoritative view. The view epoch is g_mvpDiagFrame, bumped
// on every gos_SetWorldToClipGL (every authoritative camera-matrix publish). A
// snapshot whose epoch != the current epoch was taken under a different camera
// (stale frame OR same-frame-different-camera) and MUST NOT be used to project
// current-position object geometry — that is the BUG1/BUG3/BUG5 root (objects
// projected through last frame's / a mid-frame-superseded camera -> wobble on
// zoom, flicker on rotate, small props vanish until pause/resume).
//
// Raw gos_terrain_indirect_getDispatchMvp16() is PHASE-PRIVATE to terrain-coupled
// passes (terrain / water / shoreline / overlay / decal — same draw phase as the
// snapshot publish, so currency is structural for them). Object/mech/prop/vehicle
// draw MUST go through gos_GetObjectDrawMVP(). Enforced by
// scripts/check-object-mvp-currency.py.

#include <cstdlib>

extern const float*     gos_GetTerrainMVPMat4();                       // live MVP
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();       // snapshot
extern "C" long         gos_terrain_indirect_getDispatchMvpViewEpoch();// its view-content epoch
namespace gos_terrain_indirect { bool IsFrameSolidArmed(); }
// VIEW-EPOCH-DEDUPE-1: compare against the semantic VIEW-CONTENT epoch (bumps only on
// real camera change), NOT the raw publish counter g_mvpDiagFrame (~2x/frame), which
// false-staled the snapshot every frame and disabled the FixB z-fight fix.
extern long g_viewContentEpoch;

// Telemetry (defined once in gameos_graphics.cpp; surfaced in the debug-state dump):
//   stale = armed but snapshot was from another view -> fell back to live MVP
//   used  = projected through the depth-matched snapshot -> FixB z-fight fix active
// MC2_OBJ_MVP_STALE_FATAL=1 aborts on a stale read (CI use).
unsigned long gos_object_mvp_stale_count();
void          gos_object_mvp_note_stale();
unsigned long gos_object_mvp_used_count();
void          gos_object_mvp_note_used();

// fixBEnabled = the per-consumer MC2_PROP_FIXB_MVP killswitch (preserved). Returns
// the dispatch snapshot ONLY when it belongs to the current view; otherwise the
// live MVP, which is always a correct projection of current-position geometry.
static inline const float* gos_GetObjectDrawMVP(bool fixBEnabled) {
    if (fixBEnabled && gos_terrain_indirect::IsFrameSolidArmed()) {
        if (gos_terrain_indirect_getDispatchMvpViewEpoch() == g_viewContentEpoch) {
            if (const float* m = gos_terrain_indirect_getDispatchMvp16()) {
                gos_object_mvp_note_used();   // depth-matched snapshot -> z-fight fix active
                return m;                     // snapshot belongs to THIS view -> safe
            }
        }
        gos_object_mvp_note_stale();      // armed but snapshot is from another view
    }
    return gos_GetTerrainMVPMat4();       // live MVP -> never stale / wrong-camera
}
