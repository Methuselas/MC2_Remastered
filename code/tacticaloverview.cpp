#include "tacticaloverview.h"
#include <cstdlib>
#include <cstdio>
// DEBUGWINS_print is defined in code/ablmc2.cpp (not exported via a header).
// Use OutputDebugStringA as the debug log sink for this TU.
#include <windows.h>
#include "../mclib/camera.h"

TacticalOverview g_tacticalOverview;

// Overview camera envelope. The engine clamps cameraAltitude to testMax (derived
// from AltitudeMaximumLo/Hi) every frame in GameCamera::update(), so to pull back
// past the normal zoom-out stop we must RAISE that ceiling while active and drive
// the altitude well above the gameplay cap (~6400). projectionAngle: 10=shallow ..
// 35=normal .. 88=max top-down; we hold a steep near-top-down read.
static const float kOverviewAltitude = 28000.0f;  // target pull-back altitude
static const float kOverviewCeiling  = 32000.0f;  // raised AltitudeMaximum* while active (> altitude)
static const float kOverviewTiltAngle = 85.0f;    // steep, just under singular 88

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
    ++hotkeyFires_;
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

    // Capture the gameplay altitude + tilt + zoom-out ceiling exactly once, on
    // first activation.
    if (t > 0.0f && !returnSnap_.valid) {
        returnSnap_.valid    = true;
        returnSnap_.altitude = eye->getCameraAltitude();
        returnSnap_.tilt     = eye->getProjectionAngle();
        returnSnap_.maxAltLo = Camera::AltitudeMaximumLo;
        returnSnap_.maxAltHi = Camera::AltitudeMaximumHi;
        userPannedInOverview_ = false;
    }

    if (t > 0.0f) {
        // Raise the altitude ceiling so GameCamera::update() won't clamp our
        // pulled-back altitude back down to the gameplay testMax. Setting both
        // Lo and Hi to the overview ceiling makes testMax == ceiling at any tilt.
        Camera::AltitudeMaximumLo = kOverviewCeiling;
        Camera::AltitudeMaximumHi = kOverviewCeiling;
        // Lerp altitude + tilt FROM the captured gameplay values toward overview.
        // Never from zero — that would discard the player's current framing.
        eye->zoomValue(lerpf(returnSnap_.altitude, kOverviewAltitude, t));
        eye->tiltValue(lerpf(returnSnap_.tilt,     kOverviewTiltAngle, t));
    } else if (returnSnap_.valid) {
        // Fully exited (t == 0): restore the gameplay ceiling always, and restore
        // altitude + tilt unless the user manually re-framed in overview.
        // Position/rotation were never touched.
        Camera::AltitudeMaximumLo = returnSnap_.maxAltLo;
        Camera::AltitudeMaximumHi = returnSnap_.maxAltHi;
        if (!userPannedInOverview_) {
            eye->zoomValue(returnSnap_.altitude);
            eye->tiltValue(returnSnap_.tilt);
        }
        returnSnap_.valid = false;
        userPannedInOverview_ = false;
    }
}
