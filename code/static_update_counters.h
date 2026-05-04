#pragma once
#include <stdint.h>

// Slice 3 static-update counter accessors. Definitions live in code/terrobj.cpp.
// objmgr.cpp uses these to emit the once-per-frame [STATIC_UPDATE v1] summary
// and TracyPlot calls without taking a hard dependency on terrobj.cpp internals.
//
// All summary/emitter declarations live HERE — never re-declare these as
// `extern` inside a function body in a consuming TU. The header IS the
// contract.
uint32_t g_staticUpdateRunCount();
uint32_t g_staticUpdateSkipCount();
uint32_t g_staticUpdateSeenCount();
uint32_t g_staticUpdateFallingCount();

// Summary emission (called from the per-frame hook in code/objmgr.cpp at the
// end of the TerrainObjects update sweep). State for the "last summary frame"
// guard lives in code/terrobj.cpp alongside the counters.
uint32_t g_staticUpdateLastSummaryFrame_get();
void     g_staticUpdateEmitSummary(uint32_t frame);
