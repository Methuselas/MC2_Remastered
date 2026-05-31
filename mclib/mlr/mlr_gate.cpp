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
// VFX-WEAPON-FX-RESTORE-OPUS-1: restored to default-ENABLED so classes without
// a working GPU oracle (Tube/missile smoke, Shape, ShapeCloud, DebrisCloud) render
// via original CPU MLR path. Oracle classes (CardCloud, ShardCloud, Card, PointCloud)
// skip MLR in their own Draw() by calling Effect::Draw instead of their base class.
// MC2_DISABLE_GOSFX=1 can re-disable for regression testing.
constexpr bool kDefaultDisabled = false;

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
