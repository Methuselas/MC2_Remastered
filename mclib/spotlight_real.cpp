//---------------------------------------------------------------------------
// mclib/spotlight_real.cpp
//
// (E) SpotLight_ -> real illumination — env gate implementation.
// See spotlight_real.h for the contract.
//---------------------------------------------------------------------------
#include "spotlight_real.h"
#include <cstdlib>

namespace mc2_spotlight_real {

static bool s_enabled    = false;
static bool s_initialized = false;

void initFromEnv() {
    if (s_initialized) return;
    s_initialized = true;
    const char* v = std::getenv("MC2_SPOTLIGHT_REAL");
    // Stage 1 default-off semantics: unset -> false; explicitly "0" -> false;
    // anything else -> true. T2.1 will invert this.
    if (v == nullptr) { s_enabled = false; return; }
    if (v[0] == '0' && v[1] == '\0') { s_enabled = false; return; }
    s_enabled = true;
}

bool isEnabled() {
    return s_enabled;
}

}  // namespace mc2_spotlight_real
