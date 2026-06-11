// tests/unit/test_tactical_overview.cpp
// Tier 2: pure-logic blend-state for the Tactical Overview camera.
// No engine/GL includes — proves the t-state machine, bands, return
// snapshot, and UI-gate in isolation. Engine glue is validated by smoke.
#include "doctest.h"
#include "../../code/tacticaloverview_state.h"

using TO = TacticalOverviewState;

TEST_CASE("wheel raises the setpoint; t eases toward it and stays put") {
    TO s;
    CHECK(s.t() == doctest::Approx(0.0f));
    CHECK(s.setpoint() == doctest::Approx(0.0f));
    s.applyWheel(-100, /*atCeiling=*/true, /*dt=*/1.0f); // zoom out past ceiling
    CHECK(s.setpoint() > 0.0f);                          // setpoint moved
    CHECK(s.t() == doctest::Approx(0.0f));               // t not yet (eases in update)
    s.update(10.0f);                                     // ease t toward setpoint
    CHECK(s.t() == doctest::Approx(s.setpoint()));
    float held = s.t();
    s.update(1.0f);                                      // no input: t must NOT decay
    CHECK(s.t() == doctest::Approx(held));
}

TEST_CASE("setpoint and t are clamped to [0,1]") {
    TO s;
    for (int i = 0; i < 100; ++i) s.applyWheel(-100, true, 1.0f);
    s.update(100.0f);
    CHECK(s.t() <= 1.0f);
    CHECK(s.setpoint() <= 1.0f);
    for (int i = 0; i < 100; ++i) s.applyWheel(+100, false, 1.0f);
    s.update(100.0f);
    CHECK(s.t() >= 0.0f);
    CHECK(s.setpoint() >= 0.0f);
}

TEST_CASE("wheel only raises the setpoint when at the zoom ceiling") {
    TO s;
    s.applyWheel(-100, /*atCeiling=*/false, 1.0f); // normal zoom-out, not ceiling
    s.update(10.0f);
    CHECK(s.setpoint() == doctest::Approx(0.0f));
    CHECK(s.t() == doctest::Approx(0.0f));
}

TEST_CASE("icon alpha follows the cross-fade band 0.4..0.7") {
    TO s;
    CHECK(s.iconAlpha(0.30f) == doctest::Approx(0.0f));
    CHECK(s.iconAlpha(0.40f) == doctest::Approx(0.0f));
    CHECK(s.iconAlpha(0.55f) == doctest::Approx(0.5f));
    CHECK(s.iconAlpha(0.70f) == doctest::Approx(1.0f));
    CHECK(s.iconAlpha(0.90f) == doctest::Approx(1.0f));
}

TEST_CASE("hotkey toggle drives t toward 1 then back toward 0") {
    TO s;
    s.toggleHotkey();              // arm: target = 1
    s.update(/*dt=*/10.0f);        // big dt -> reach target
    CHECK(s.t() == doctest::Approx(1.0f));
    s.toggleHotkey();              // disarm: target = 0
    s.update(10.0f);
    CHECK(s.t() == doctest::Approx(0.0f));
}

TEST_CASE("UI-gate blocks wheel feed") {
    TO s;
    s.applyWheel(-100, /*atCeiling=*/true, 1.0f, /*worldOwnsWheel=*/false);
    s.update(10.0f);
    CHECK(s.setpoint() == doctest::Approx(0.0f));
    CHECK(s.t() == doctest::Approx(0.0f));
}
