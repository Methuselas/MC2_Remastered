#ifndef TACTICALOVERVIEW_STATE_H
#define TACTICALOVERVIEW_STATE_H
// Pure-logic blend-state for the Tactical Overview camera.
// NO engine/GL/Stuff includes — keep this unit-testable in isolation.

class TacticalOverviewState {
public:
    // Cross-fade band over t (see design spec).
    static constexpr float kIconFadeLo  = 0.40f;
    static constexpr float kIconFadeHi  = 0.70f;
    // Wheel sensitivity: t units per wheel "notch" (delta normalized by 120).
    // Small, because the altitude mapping is exponential (driveCamera): each
    // notch multiplies altitude by a constant factor for a uniform zoom feel.
    static constexpr float kWheelGain   = 0.03f;
    // Hotkey animation rate: t units per second toward the setpoint.
    static constexpr float kHotkeyRate  = 4.0f;

    float t() const { return t_; }
    float setpoint() const { return setpoint_; }   // diagnostic

    // Wheel input. delta<0 = zoom out (raise t) only when atCeiling.
    // delta>0 = zoom in (lower t). worldOwnsWheel gates UI exclusion.
    void applyWheel(long delta, bool atCeiling, float dt, bool worldOwnsWheel = true);

    // Hotkey toggles the animated setpoint between 1 and 0.
    void toggleHotkey();

    // Per-frame advance of the hotkey-driven setpoint. dt in seconds.
    void update(float dt);

    // Icon overlay alpha for a given blend value (0 below band, ramps to 1).
    float iconAlpha(float blend) const;
    float iconAlpha() const { return iconAlpha(t_); }

    bool active() const { return t_ > 0.0f; }

private:
    void setT(float v);
    void setSetpoint(float v);
    float t_        = 0.0f;   // current eased blend (drives the camera)
    float setpoint_ = 0.0f;   // persistent desired overview level [0,1]
};

#endif // TACTICALOVERVIEW_STATE_H
