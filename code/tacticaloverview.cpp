#include "tacticaloverview.h"
#include <cstdlib>
#include <cstdio>
// DEBUGWINS_print is defined in code/ablmc2.cpp (not exported via a header).
// Use OutputDebugStringA as the debug log sink for this TU.
#include <windows.h>

TacticalOverview g_tacticalOverview;

bool TacticalOverview::enabled() {
    static int cached = -1;
    if (cached < 0) cached = (getenv("MC2_TACTICAL_OVERVIEW") != nullptr) ? 1 : 0;
    return cached != 0;
}

bool TacticalOverview::tintEnabled() {
    if (!enabled()) return false;
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("MC2_TACTICAL_OVERVIEW_TINT");
        cached = (v && v[0] == '0') ? 0 : 1;   // default ON; only "=0" disables
    }
    return cached != 0;
}

void TacticalOverview::onWheel(long delta, bool atCeiling, float dt, bool worldOwnsWheel) {
    if (!enabled()) return;
    state_.applyWheel(delta, atCeiling, dt, worldOwnsWheel);
}

void TacticalOverview::onHotkey() {
    if (!enabled()) return;
    state_.toggleHotkey();
}

void TacticalOverview::advance(float dt) {
    if (!enabled()) return;
    float before = state_.t();
    state_.update(dt);
    if (before != state_.t() && getenv("MC2_TACTICAL_OVERVIEW_DEBUG")) {
        char buf[96];
        sprintf_s(buf, sizeof(buf), "[TacOverview] t=%.3f iconA=%.3f", state_.t(), state_.iconAlpha());
        OutputDebugStringA(buf);
    }
}
