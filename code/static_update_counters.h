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

// [TOBJSPLIT v1] once-per-frame roll + 600-frame summary for the RDTSC
// cost-split probe. Accumulators defined in code/terrobj.cpp; called from
// code/objmgr.cpp at the end of the GameLogic.Units.TerrainObjects sweep
// (the per-frame boundary for the recalcBounds / appearance->update paths).
//
// Accumulator externs are declared here so mclib/bdactor.cpp (BldgAppearance
// and TreeAppearance::recalcBounds) can reference them without inline-extern
// declarations in function bodies. mclib -> code/ includes are established
// precedent (see unitdesg.h, gameobj.h).
extern unsigned long long g_tobjAngularCyc;
extern unsigned long long g_tobjProjCyc;
void g_tobjSplitRollAndMaybeEmit();
