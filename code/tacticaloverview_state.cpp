#include "tacticaloverview_state.h"

static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void TacticalOverviewState::setT(float v) { t_ = clamp01(v); }

void TacticalOverviewState::applyWheel(long delta, bool atCeiling, float /*dt*/,
                                       bool worldOwnsWheel) {
    if (!worldOwnsWheel) return;
    float notches = (float)delta / 120.0f;        // GameOS wheel granularity
    if (delta < 0) {                              // zoom out
        if (atCeiling) setT(t_ + (-notches) * kWheelGain);
    } else if (delta > 0) {                       // zoom in
        setT(t_ - notches * kWheelGain);
    }
}

void TacticalOverviewState::toggleHotkey() {
    hotkeyOn_ = !hotkeyOn_;
    setpoint_ = hotkeyOn_ ? 1.0f : 0.0f;
}

void TacticalOverviewState::update(float dt) {
    float step = kHotkeyRate * dt;
    if (t_ < setpoint_)      setT(t_ + step > setpoint_ ? setpoint_ : t_ + step);
    else if (t_ > setpoint_) setT(t_ - step < setpoint_ ? setpoint_ : t_ - step);
}

float TacticalOverviewState::iconAlpha(float blend) const {
    if (blend <= kIconFadeLo) return 0.0f;
    if (blend >= kIconFadeHi) return 1.0f;
    return (blend - kIconFadeLo) / (kIconFadeHi - kIconFadeLo);
}
