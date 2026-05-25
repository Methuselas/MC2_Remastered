//==========================================================================//
// File:    mlr_gate.cpp                                                     //
// Contents: MC2_DISABLE_GOSFX env-gate implementation.                     //
//           Plan v6 §2 (Stage A1) and §3 (Stage A2 default flip).          //
//===========================================================================//

#include "mlr_gate.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace mc2 {
namespace mlr_gate {

namespace {
// A2 default: ON (disabled = true). Particles disappear in stock play.
// Plan v6 §3 — transitional regression state acknowledged in (A) §3 / §5 Q1.
// MC2_DISABLE_GOSFX=0 restores legacy gosFX path.
constexpr bool kDefaultDisabled = true;

bool g_initialized = false;
bool g_disabled = false;
std::mutex g_mutex;

void initialize_locked() {
    if (g_initialized) return;
    const char* v = std::getenv("MC2_DISABLE_GOSFX");
    if (v && v[0] != '\0') {
        // Explicit env override always wins.
        g_disabled = (v[0] == '1');
    } else {
        g_disabled = kDefaultDisabled;
    }
    g_initialized = true;
    std::fprintf(stderr, "[INSTR v1] enabled: mlr_gate disabled=%d\n",
                 g_disabled ? 1 : 0);
}
} // namespace

bool is_disabled() {
    if (g_initialized) return g_disabled;
    std::lock_guard<std::mutex> lock(g_mutex);
    initialize_locked();
    return g_disabled;
}

} // namespace mlr_gate
} // namespace mc2
