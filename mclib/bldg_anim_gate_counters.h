#pragma once
#include <stdint.h>

// BLDG-TYPE-ANIM-GATE-FIX-1 diagnostic counter accessors.
// Counters defined in mclib/bdactor.cpp; consumed by code/terrobj.cpp.
// All values are cumulative (never reset).
uint32_t g_bldgAnimGate_typeIdleNowStatic();
uint32_t g_bldgAnimGate_animStartInvalidated();
uint32_t g_bldgAnimGate_animStateToState();
