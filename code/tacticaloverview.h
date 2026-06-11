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

    // Drive the camera each frame from the current blend t. Pass the live
    // mission camera (the global `eye`). Captures a return snapshot once on
    // entry; restores it on full exit unless the user panned in overview.
    void driveCamera(Camera* eye);

    // Call from the camera-input site when the user manually pans/rotates while
    // overview is active — suppresses the auto-return so we don't yank them back.
    void notifyUserPan() { if (state_.active()) userPannedInOverview_ = true; }

    float blend() const { return state_.t(); }
    float iconAlpha() const { return state_.iconAlpha(); }  // overlay fade
    float setpoint() const { return state_.setpoint(); }   // diagnostic
    int   hotkeyFires() const { return hotkeyFires_; }      // diagnostic
    bool  active() const { return state_.active(); }

private:
    // Engine-free snapshot of the gameplay camera, captured once on entry.
    // Only altitude + tilt are captured/restored: those are the ONLY fields
    // driveCamera mutates while active. Position and rotation are never touched
    // by the overview, so they are naturally preserved (and stay wherever the
    // player panned to) — no restore needed, no yank-back on exit.
    struct CamSnapshot {
        bool  valid    = false;
        float altitude = 0.0f;   // cameraAltitude (set via zoomValue)
        float tilt     = 0.0f;   // projectionAngle (set via tiltValue) — lerp FROM this
        float maxAltLo = 0.0f;   // Camera::AltitudeMaximumLo — the zoom-out ceiling
        float maxAltHi = 0.0f;   // Camera::AltitudeMaximumHi — raised so we can pull back
    };

    TacticalOverviewState state_;
    CamSnapshot returnSnap_;
    bool        userPannedInOverview_ = false;
    int         hotkeyFires_ = 0;   // diagnostic: count of onHotkey() calls
};

extern TacticalOverview g_tacticalOverview;

#endif // TACTICALOVERVIEW_H
