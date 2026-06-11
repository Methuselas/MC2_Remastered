#include "tacticaloverview.h"
#include <cstdlib>
#include <cstdio>
// DEBUGWINS_print is defined in code/ablmc2.cpp (not exported via a header).
// Use OutputDebugStringA as the debug log sink for this TU.
#include <windows.h>
#include "../mclib/camera.h"

TacticalOverview g_tacticalOverview;

// Overview camera envelope. Conservative for v1: altitude near the existing
// gameplay max (keeps Camera::newScaleFactor >= 0; ceiling-raise is a T7 tuning
// step) and a steep — but not singular (88) — perspective angle for a near-top-
// down map read. projectionAngle: 10=shallow .. 35=normal .. 88=max top-down.
static const float kOverviewAltitude = 6000.0f;
static const float kOverviewTiltAngle = 80.0f;

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

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

void TacticalOverview::driveCamera(Camera* eye) {
    if (!enabled() || !eye) return;
    const float t = state_.t();

    // Capture the gameplay altitude + tilt exactly once, on first activation.
    if (t > 0.0f && !returnSnap_.valid) {
        returnSnap_.valid    = true;
        returnSnap_.altitude = eye->getCameraAltitude();
        returnSnap_.tilt     = eye->getProjectionAngle();
        userPannedInOverview_ = false;
    }

    if (t > 0.0f) {
        // Lerp altitude + tilt FROM the captured gameplay values toward overview.
        // Never from zero — that would discard the player's current framing.
        eye->zoomValue(lerpf(returnSnap_.altitude, kOverviewAltitude, t));
        eye->tiltValue(lerpf(returnSnap_.tilt,     kOverviewTiltAngle, t));
    } else if (returnSnap_.valid) {
        // Fully exited (t == 0): restore gameplay altitude + tilt unless the user
        // manually re-tilted in overview. Position/rotation were never touched.
        if (!userPannedInOverview_) {
            eye->zoomValue(returnSnap_.altitude);
            eye->tiltValue(returnSnap_.tilt);
        }
        returnSnap_.valid = false;
        userPannedInOverview_ = false;
    }
}
