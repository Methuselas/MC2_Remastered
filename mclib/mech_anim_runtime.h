// mech_anim_runtime.h — BT2018-SKEL-ENGINE-1B-RUNTIME
//
// Per-frame CPU re-bake of IMPORTED skinned mechs (Assimp BT2018 GLB). The
// importer (assimp_importer.cpp) bakes ONE static pose into a merged TG type
// shape; this drives that same merged geometry per frame from a looping clip so
// the mech MOVES. Engine-only — the implementation lives in assimp_importer.cpp
// (it needs Assimp + the shared mc2skel FK), so NO Assimp/TG types leak through
// this header. Callers (mech3d.cpp) see only plain scalars.
//
// Scope (1B): single forced clip, CPU mech path (MC2_GPU_MECHS=0). Shared-type
// re-bake → all actors of the chassis animate in lockstep (one demo mech). Gate
// MC2_MECH_IMPORT_ANIMATE (default OFF, opt-in) + MC2_MECH_IMPORT_FORCE_CLIP
// names the clip. Per-actor animation and the GPU mech path are deferred.
#pragma once

namespace mc2mechanim {

// Advance + re-bake every registered imported animated mech, once per frameStamp.
// Idempotent on frameStamp (g_mc2FrameCounter), so it is safe to call from every
// mech's updateGeometry — only the first call for a given frame does work. A
// no-op (single branch) when no imported animated mech is registered.
//   frameLengthSec : per-frame dt (timing.h `frameLength`)
//   frameStamp     : the global frame counter (g_mc2FrameCounter)
void TickImportedMechs(float frameLengthSec, unsigned frameStamp);

// True once at least one imported mech anim is registered (gate on + clip + bones).
bool AnyImportedAnim();

}  // namespace mc2mechanim
