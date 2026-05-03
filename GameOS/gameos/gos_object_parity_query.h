// GameOS/gameos/gos_object_parity_query.h
//
// Thin GL-free header for callers in mclib/ that only need to query the
// parity env-gate and dual-emit state. Does NOT include <GL/glew.h> so it
// can be included safely from any TU, including mclib/bdactor.cpp and
// mclib/genactor.cpp.
//
// All function bodies are in gos_object_parity.cpp. The full API (including
// GL-dependent functions) is in gos_object_parity.h (which includes this
// header).
//
// Stage 2.D.2 use case: the dual-emit hook in
//   mclib/bdactor.cpp  (BldgAppearance::update, TreeAppearance::update)
//   mclib/genactor.cpp (GenericAppearance::update)
// needs to know whether to run full TransformMultiShape in addition to
// TransformMultiShape_PositionsOnly. IsDualEmitArmedForActor() provides
// this query without pulling in any GL types.
//
// Stage 2.D.3: per-actor gating. The zero-arg IsDualEmitArmed() applied
// uniformly to all eligible actors, which prevented narrowing dual-emit
// to a single sampled actor per frame. The per-actor variant takes the
// actor's TG_MultiShape pointer; identity is "the actor whose multishape
// pointer matches the sampler's pick this frame". Bootstrap arm phase
// (first frame after OnMissionLoad) returns true for ALL actors so the
// 2.D.2 baseline behavior is preserved on the first compare; sampled
// frames after that return true for ONLY the picked actor.

#pragma once

#include <cstdint>

// Forward declaration so callers in mclib/ don't need to pull tgl.h /
// msl.h transitively just to call IsDualEmitArmedForActor.
class TG_MultiShape;

namespace gos_object_parity {

// Env-gate reader. Returns true iff MC2_OBJECT_PARITY_CHECK=1.
// Boot-time cached; subsequent calls are a single bool load.
bool IsParityCheckEnabled();

// Stage 2.D.3 — per-actor dual-emit gate.
//
// Returns true iff the parity sidecar wants this specific actor to emit
// the full TransformMultiShape (CPU lighting bake) on top of the normal
// TransformMultiShape_PositionsOnly, for this frame, so the snapshot
// machinery can compare against GPU readback.
//
//   - Bootstrap phase (state == Armed && armMode == All): returns true
//     for every shape pointer (matches the prior 2.D.2 zero-arg gate
//     behavior on the first frame post-mission-load).
//   - Sample phase (state == Armed && armMode == SampledOnly): returns
//     true iff `shape` matches the per-frame sampler pick.
//   - All other states (WaitingForReadback / Done) and parity OFF:
//     returns false unconditionally.
//
// `shape` may be nullptr; the function returns false in that case.
bool IsDualEmitArmedForActor(const TG_MultiShape* shape);

}  // namespace gos_object_parity
