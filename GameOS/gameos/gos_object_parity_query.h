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
// TransformMultiShape_PositionsOnly. IsDualEmitArmed() provides this query
// without pulling in any GL types.

#pragma once

#include <cstdint>

namespace gos_object_parity {

// Env-gate reader. Returns true iff MC2_OBJECT_PARITY_CHECK=1.
// Boot-time cached; subsequent calls are a single bool load.
bool IsParityCheckEnabled();

// State query. Returns true when the dual-emit latch is in the Armed state
// (first eligible frame post-mission-start). Once the dual-emit frame's
// GPU draw is fenced, the latch advances to WaitingForReadback and
// IsDualEmitArmed() returns false for the rest of the mission.
// Returns false unconditionally when IsParityCheckEnabled()==false.
bool IsDualEmitArmed();

}  // namespace gos_object_parity
