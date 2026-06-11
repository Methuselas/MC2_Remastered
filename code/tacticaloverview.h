#ifndef TACTICALOVERVIEW_H
#define TACTICALOVERVIEW_H
// Engine glue for the Tactical Overview camera. Owns the pure blend-state,
// reads env flags, drives the Camera via its public API, draws the 2D overlay.
#include "tacticaloverview_state.h"

class Camera;

class TacticalOverview {
public:
    // True iff MC2_TACTICAL_OVERVIEW is set (master gate). Cached on first call.
    static bool enabled();
    // True iff the friendly-coverage tint is enabled (default ON under master,
    // killable via MC2_TACTICAL_OVERVIEW_TINT=0).
    static bool tintEnabled();

    // Called from the camera-input site every frame.
    void onWheel(long delta, bool atCeiling, float dtSeconds, bool worldOwnsWheel);
    void onHotkey();                 // hotkey pressed (snap-toggle)
    void advance(float dtSeconds);   // per-frame state advance + debug log

    float blend() const { return state_.t(); }
    bool  active() const { return state_.active(); }

private:
    TacticalOverviewState state_;
};

extern TacticalOverview g_tacticalOverview;

#endif // TACTICALOVERVIEW_H
