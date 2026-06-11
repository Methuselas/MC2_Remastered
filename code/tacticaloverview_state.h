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
    static constexpr float kWheelGain   = 0.15f;
    // Hotkey animation rate: t units per second toward the setpoint.
    static constexpr float kHotkeyRate  = 4.0f;

    float t() const { return t_; }

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
    float t_        = 0.0f;
    float setpoint_ = 0.0f;   // hotkey target; <0 means "no hotkey override"
    bool  hotkeyOn_ = false;
};

#endif // TACTICALOVERVIEW_STATE_H
