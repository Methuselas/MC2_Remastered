//---------------------------------------------------------------------------
// mclib/spotlight_real.cpp
//
// (E) SpotLight_ -> real illumination — env gate implementation.
// See spotlight_real.h for the contract.
//---------------------------------------------------------------------------
#include "spotlight_real.h"
#include <cstdlib>

namespace mc2_spotlight_real {

static bool s_enabled    = true;
static bool s_initialized = false;

void initFromEnv() {
    if (s_initialized) return;
    s_initialized = true;
    const char* v = std::getenv("MC2_SPOTLIGHT_REAL");
    // Stage 2 (T2.1) default-on semantics: unset -> true; explicitly "0" -> false;
    // anything else -> true. MC2_SPOTLIGHT_REAL=0 is the opt-out regression escape
    // hatch; default behavior matches the gate-on path. T3.1 deletes the gate
    // entirely after soak.
    if (v == nullptr) { s_enabled = true; return; }
    if (v[0] == '0' && v[1] == '\0') { s_enabled = false; return; }
    s_enabled = true;
}

bool isEnabled() {
    return s_enabled;
}

}  // namespace mc2_spotlight_real
