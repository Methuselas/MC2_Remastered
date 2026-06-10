# Tactical Overview Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a SupCom/Warno-style "tactical overview": wheel-zoom continuum + hotkey snap into a high-altitude steep-tilt perspective view where units cross-fade into strategic icons, with sensor fog, objectives/nav, and a friendly-coverage tint — all additive, reusing the existing camera, projection, and pick paths.

**Architecture:** A single controller drives one blend factor `t ∈ [0,1]`. Pure state math (t, bands, return snapshot, UI-gating) lives in a GL-free unit-testable module; engine glue drives the existing `Camera` via its public API and draws a 2D overlay in the HUD pass. No ortho swap, no new render pipeline, no new pick path, no v1 model fade.

**Tech Stack:** C++17, MC2 engine (`mclib/camera.*`, `code/missiongui.cpp`, `code/gametacmap.cpp`), GameOS 2D draw (`gos_DrawQuads`/blip textures), doctest (`tests/unit/`), env flags via `getenv`, tier1 smoke harness.

**Spec:** `docs/superpowers/specs/2026-06-10-tactical-overview-camera-design.md`

---

## Hard constraints (must hold every task)

- No orthographic projection — steep-tilt **perspective** only.
- No new render pipeline; overlay draws in the existing HUD/2D pass.
- No new pick path; selection/orders use the existing world-pos pick.
- No threat/enemy aggregation; tint = union of friendly sensor radii only.
- No model-mesh replacement; **no mech3d/material/TG_Shape alpha changes in v1**.
- No minimap gameplay-query duplication — share one enumeration.

## File structure

- **Create** `code/tacticaloverview_state.h` / `.cpp` — GL/engine-free pure logic: the `TacticalOverviewState` struct (blend `t`, bands, hotkey/wheel transitions, return-snapshot bookkeeping, UI-gate decision). Unit-tested.
- **Create** `code/tacticaloverview.h` / `.cpp` — engine glue: owns a `TacticalOverviewState`, reads env flags, drives the `Camera` (`eye`) via public API, draws the 2D icon + tint overlay. Validated by build + smoke + screenshots.
- **Create** `tests/unit/test_tactical_overview.cpp` — doctest for the pure-logic module.
- **Modify** `code/gametacmap.{h,cpp}` — extract the contact/mover/objective walk into a shared read-only `enumerateTacticalBlips()`.
- **Modify** `code/missiongui.cpp` — call the controller from the camera-input site (wheel ~3870/3896, plus a new hotkey) and from the HUD render path; UI-gate the wheel feed.
- **Modify** `tests/unit/CMakeLists.txt` — register the new test + the one pure-logic `.cpp`.

---

## Task 1: Pure-logic blend-state module (T1 — no engine, no camera move)

**Files:**
- Create: `code/tacticaloverview_state.h`
- Create: `code/tacticaloverview_state.cpp`
- Test: `tests/unit/test_tactical_overview.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`tests/unit/test_tactical_overview.cpp`:
```cpp
// tests/unit/test_tactical_overview.cpp
// Tier 2: pure-logic blend-state for the Tactical Overview camera.
// No engine/GL includes — proves the t-state machine, bands, return
// snapshot, and UI-gate in isolation. Engine glue is validated by smoke.
#include "doctest.h"
#include "../../code/tacticaloverview_state.h"

using TO = TacticalOverviewState;

TEST_CASE("t starts at 0 and is clamped to [0,1]") {
    TO s;
    CHECK(s.t() == doctest::Approx(0.0f));
    s.applyWheel(-100, /*atCeiling=*/true, /*dt=*/1.0f); // zoom out past ceiling
    CHECK(s.t() > 0.0f);
    for (int i = 0; i < 100; ++i) s.applyWheel(-100, true, 1.0f);
    CHECK(s.t() <= 1.0f);
    for (int i = 0; i < 100; ++i) s.applyWheel(+100, false, 1.0f);
    CHECK(s.t() >= 0.0f);
}

TEST_CASE("wheel only raises t when at the zoom ceiling") {
    TO s;
    s.applyWheel(-100, /*atCeiling=*/false, 1.0f); // normal zoom-out, not ceiling
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
    CHECK(s.t() == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: build `mc2_tests` (see CMake step below) — expected: FAIL to compile (`tacticaloverview_state.h` not found).

- [ ] **Step 3: Write the header**

`code/tacticaloverview_state.h`:
```cpp
#ifndef TACTICALOVERVIEW_STATE_H
#define TACTICALOVERVIEW_STATE_H
// Pure-logic blend-state for the Tactical Overview camera.
// NO engine/GL/Stuff includes — keep this unit-testable in isolation.

class TacticalOverviewState {
public:
    // Cross-fade band over t (see design spec §"Altitude bands").
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
```

- [ ] **Step 4: Write the implementation**

`code/tacticaloverview_state.cpp`:
```cpp
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
```

- [ ] **Step 5: Register the test + pure-logic cpp in CMake**

Modify `tests/unit/CMakeLists.txt` — add to the `add_executable(mc2_tests ...)` source list (after `test_host_services.cpp`, line ~40):
```cmake
    test_tactical_overview.cpp   # TACTICAL-OVERVIEW-1: blend-state pure logic
```
and add the subject `.cpp` to the engine-source section (after the `DefaultHostServices.cpp` line ~46):
```cmake
    ${MC2_WORKTREE_ROOT}/code/tacticaloverview_state.cpp  # TACTICAL-OVERVIEW-1
```

- [ ] **Step 6: Run the test to verify it passes**

Run (from worktree root):
```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --target mc2_tests --config RelWithDebInfo
build64/tests/unit/RelWithDebInfo/mc2_tests.exe -tc="*tactical*","*t starts*","*wheel only*","*icon alpha*","*hotkey toggle*","*UI-gate*"
```
Expected: all TACTICAL-OVERVIEW cases PASS.

- [ ] **Step 7: Commit**

```bash
git add code/tacticaloverview_state.h code/tacticaloverview_state.cpp tests/unit/test_tactical_overview.cpp tests/unit/CMakeLists.txt
git commit -m "feat(overview): tactical overview blend-state pure logic (T1)"
```

---

## Task 2: Controller shell + env flag + debug log (T1 glue — still no camera move)

**Files:**
- Create: `code/tacticaloverview.h`
- Create: `code/tacticaloverview.cpp`
- Modify: `code/missiongui.cpp` (wheel site ~3870/3896, hotkey table ~227, render path)

- [ ] **Step 1: Write the controller header**

`code/tacticaloverview.h`:
```cpp
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
    // delta = mouse wheel delta; atCeiling = camera already at max zoom-out;
    // worldOwnsWheel = mission world owns the wheel (UI-exclusion).
    void onWheel(long delta, bool atCeiling, float dtSeconds, bool worldOwnsWheel);
    void onHotkey();                 // hotkey pressed (snap-toggle)
    void advance(float dtSeconds);   // per-frame state advance + debug log

    float blend() const { return state_.t(); }
    bool  active() const { return state_.active(); }

private:
    TacticalOverviewState state_;
};

// Process-wide instance (mission-scoped lifetime is fine; state resets at t=0).
extern TacticalOverview g_tacticalOverview;

#endif // TACTICALOVERVIEW_H
```

- [ ] **Step 2: Write the controller implementation (T1: log only)**

`code/tacticaloverview.cpp`:
```cpp
#include "tacticaloverview.h"
#include <cstdlib>
#include <cstdio>

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
        sprintf(buf, "[TacOverview] t=%.3f iconA=%.3f", state_.t(), state_.iconAlpha());
        DEBUGWINS_print(buf, 0);   // same debug sink used across code/
    }
}
```
Note: `DEBUGWINS_print` is already used throughout `code/` (e.g. `code/ablmc2.cpp`). Include whatever header `missiongui.cpp` uses for it; if unavailable in this TU, fall back to `OutputDebugStringA`.

- [ ] **Step 3: Wire the wheel + hotkey + advance into missiongui.cpp**

In `code/missiongui.cpp`, add `#include "tacticaloverview.h"` near the other includes.

**WHEEL-SIGN CONTRACT — verify before wiring (do not change existing zoom direction):**
```
Before wiring TacticalOverview::onWheel, verify the existing missiongui wheel sign:
- which sign currently calls zoomChoiceOut()  (current code: delta > 0)
- which sign currently calls zoomChoiceIn()   (current code: delta < 0)

Do not change existing wheel zoom direction — keep the zoomChoiceOut/In calls
exactly as they are.

TacticalOverviewState semantic (canonical, keep the doctest as written):
  negative delta = zoom out / ENTER overview (raise t)
  positive delta = zoom in  / EXIT  overview (lower t)

If GameOS getMouseWheelDelta() reports the OPPOSITE sign at this site
(i.e. delta > 0 is the zoom-OUT notch, since current code calls zoomChoiceOut on
delta > 0), translate the sign ONCE at the missiongui boundary before calling
onWheel — pass `-mouseWheelDelta`. Do NOT flip the state-module convention.
```
At BOTH wheel sites (the `cameraClicked` branch ~3870 and the plain branch ~3896), BEFORE the existing `zoomChoiceOut()/zoomChoiceIn()` calls, forward to the controller. **In T1, pass `atCeiling=false`** — the real ceiling test does not exist until Task 3, and faking `atCeiling=true` would let wheel-past-ceiling mutate `t` with the flag on before the gate is real. With `atCeiling=false`, T1 wheel is inert; only the hotkey/debug path exercises `t`. Example at ~3896 (note the boundary sign-translate — adjust per the verified sign above):
```cpp
    long mouseWheelDelta = userInput->getMouseWheelDelta();
    if (mouseWheelDelta)
    {
        // Tactical overview: feed the blend-state. worldOwnsWheel is true here
        // because this branch runs only when mission world input owns the wheel
        // (UI panels consume the wheel before reaching this handler).
        // Sign: current code calls zoomChoiceOut() on delta > 0, so the zoom-OUT
        // notch is positive here; negate to match the state convention
        // (negative = zoom out = enter overview). T1: atCeiling=false (inert).
        g_tacticalOverview.onWheel(-mouseWheelDelta, /*atCeiling=*/false,
                                   frameLength, /*worldOwnsWheel=*/true);
        if (mouseWheelDelta > 0) zoomChoiceOut(); else zoomChoiceIn();
        ...
```

Add a hotkey. In the hotkey table (near `KEY_HOME ... cameraNormal` line ~227) add a row bound to a free key (e.g. `KEY_TAB` or `KEY_O`) calling a new handler `MissionInterfaceManager::toggleTacticalOverview`. Add that handler near `cameraNormal()` (~2851):
```cpp
int MissionInterfaceManager::toggleTacticalOverview()
{
    g_tacticalOverview.onHotkey();
    return 1;
}
```
Declare it in `code/missiongui.h` alongside `cameraNormal`.

Call `advance` once per frame. In `MissionInterfaceManager::update` (or the per-frame method that already runs the camera input), add near the top:
```cpp
    g_tacticalOverview.advance(frameLength);
```

- [ ] **Step 4: Add the cpp to the game build**

Add `code/tacticaloverview_state.cpp` and `code/tacticaloverview.cpp` to the game target's source list (the same CMake list that already names `code/missiongui.cpp` / `code/gametacmap.cpp` — grep `CMakeLists.txt` for `missiongui.cpp` to find it).

- [ ] **Step 5: Build the game**

Run:
```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --target mc2 --config RelWithDebInfo
```
Expected: builds clean.

- [ ] **Step 6: Smoke — must be inert (flag OFF)**

Run:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS (controller is a no-op without `MC2_TACTICAL_OVERVIEW`).

- [ ] **Step 7: Commit**

```bash
git add code/tacticaloverview.h code/tacticaloverview.cpp code/missiongui.cpp code/missiongui.h CMakeLists.txt
git commit -m "feat(overview): controller shell + env flag + hotkey/wheel t-state (T1)"
```

---

## Task 3: Camera blend via wheel/hotkey + return contract (T2 — camera moves, no visuals)

**Files:**
- Modify: `code/tacticaloverview.h` / `.cpp` (add camera drive + return snapshot)
- Modify: `code/missiongui.cpp` (pass real `atCeiling`; give controller the camera)

- [ ] **Step 1: Add return-snapshot fields + camera drive to the controller header**

In `code/tacticaloverview.h`, add (inside the class):
```cpp
    // Drive the camera each frame from the current blend t. Pass the live
    // mission camera (the global `eye`). No-op when t == 0 and no snapshot.
    void driveCamera(Camera* eye);

private:
    struct CamSnapshot {
        bool  valid = false;
        float altitude = 0.0f;
        float rotation = 0.0f;
        float tilt     = 0.0f;   // captured gameplay tilt — REQUIRED; tilt lerps
                                 // from this toward overview tilt, never from 0.
        // position captured via Camera::getPosition(); stored as 3 floats to
        // keep this header engine-free.
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    };
    CamSnapshot returnSnap_;
    bool userPannedInOverview_ = false;
```
Add a public setter the input site calls when the user pans/rotates while active:
```cpp
    void notifyUserPan() { if (state_.active()) userPannedInOverview_ = true; }
```

- [ ] **Step 2: Implement `driveCamera` with the return contract**

In `code/tacticaloverview.cpp` (engine-facing — include `mclib/camera.h` and `Stuff` vector here, NOT in the pure-logic module):
```cpp
#include "../mclib/camera.h"

// Overview camera envelope (tune in Task: T7).
static constexpr float kOverviewAltitude = 6000.0f;  // high pull-back
static constexpr float kOverviewTiltDeg  = 78.0f;    // steep, NOT 90 (perspective)

void TacticalOverview::driveCamera(Camera* eye) {
    if (!enabled() || !eye) return;
    const float t = state_.t();

    // Capture the return snapshot exactly once, on first activation.
    if (t > 0.0f && !returnSnap_.valid) {
        returnSnap_.valid = true;
        returnSnap_.altitude = eye->getCameraAltitude();
        returnSnap_.rotation = eye->getCameraRotation();
        returnSnap_.tilt     = eye->getCameraTilt();   // capture gameplay tilt too
        Stuff::Vector3D p = eye->getPosition();
        returnSnap_.posX = p.x; returnSnap_.posY = p.y; returnSnap_.posZ = p.z;
        userPannedInOverview_ = false;
    }

    if (t > 0.0f) {
        // Blend altitude/tilt toward the overview envelope. Drive via public API.
        float baseAlt = returnSnap_.altitude;
        float alt = baseAlt + (kOverviewAltitude - baseAlt) * t;
        eye->setCameraAltitude(alt);          // existing setter; clamps internally
        // Tilt: lerp from the CAPTURED gameplay tilt toward overview tilt.
        // Do NOT use overviewTilt * t — that starts from 0 and loses the
        // player's current tilt at t==0+.
        float baseTilt = returnSnap_.tilt;
        float tilt = baseTilt + (kOverviewTiltDeg - baseTilt) * t;
        eye->setCameraTilt(tilt);             // confirm exact setter name
    } else if (returnSnap_.valid) {
        // Fully exited: restore the snapshot unless the user panned in overview.
        if (!userPannedInOverview_) {
            eye->setCameraAltitude(returnSnap_.altitude);
            eye->setCameraTilt(returnSnap_.tilt);
            eye->setCameraRotation(returnSnap_.rotation, returnSnap_.rotation);
            Stuff::Vector3D p; p.x = returnSnap_.posX; p.y = returnSnap_.posY; p.z = returnSnap_.posZ;
            eye->setPosition(p, /*swoopy=*/false);
        }
        returnSnap_.valid = false;
        userPannedInOverview_ = false;
    }
}
```
Note: confirm exact camera setter names by grepping `mclib/camera.h` (`setCameraAltitude`, `setCameraTilt`, `getCameraAltitude`, `getCameraRotation`). If a direct altitude setter is absent, drive altitude through `ZoomIn/ZoomOut` deltas toward the target instead — keep the public-API-only rule.

- [ ] **Step 3: Compute real `atCeiling` and call `driveCamera`**

In `code/missiongui.cpp`, replace the T1 inert `atCeiling=false` with a real test (camera at/above its max zoom-out). Grep `mclib/camera.h` for the max-zoom/altitude query; e.g. `eye->getCameraAltitude() >= eye->getMaxCameraAltitude() - epsilon`. Keep the boundary sign-translate from Task 2 (`-mouseWheelDelta` or as verified). Then after the per-frame `advance`, call:
```cpp
    g_tacticalOverview.advance(frameLength);
    g_tacticalOverview.driveCamera(eye);
```
In the right-drag rotate/tilt branch (`cameraClicked`, ~3836), add `g_tacticalOverview.notifyUserPan();` so manual movement in overview suppresses the return.

- [ ] **Step 4: Build**

Run: `cmake --build build64 --target mc2 --config RelWithDebInfo` (full path as above).
Expected: clean build.

- [ ] **Step 5: Smoke — flag OFF (inert) and flag ON (no crash)**

Flag OFF:
```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS.

Flag ON (camera drive exercised; idle flythrough = t stays 0, but path is compiled/linked live):
```powershell
$env:MC2_TACTICAL_OVERVIEW=1; py -3 ...\scripts\run_smoke.py --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs; Remove-Item Env:MC2_TACTICAL_OVERVIEW
```
Expected: 2/2 PASS, no crash.

- [ ] **Step 6: Manual visual gate (user)**

In a live mission with `MC2_TACTICAL_OVERVIEW=1`: wheel-out past max zoom raises altitude smoothly toward overview; hotkey snaps to overview and back; returning lands at the captured spot (unless you panned). No projection/pick weirdness.

- [ ] **Step 7: Commit**

```bash
git add code/tacticaloverview.h code/tacticaloverview.cpp code/missiongui.cpp
git commit -m "feat(overview): camera altitude/tilt blend + return contract (T2)"
```

---

## Task 4: Shared read-only blip enumeration (T3 refactor)

**Files:**
- Modify: `code/gametacmap.h` / `code/gametacmap.cpp` (extract enumeration)

- [ ] **Step 1: Define the blip record + enumerator**

In `code/gametacmap.h`, add a small POD and a static enumerator (read-only):
```cpp
    struct TacBlip {
        Stuff::Vector3D world;   // unit world position
        unsigned long   color;   // team/selection color (existing palette)
        int             shape;   // existing blip shape id
        float           sensorRadius; // >0 for friendly sensors (tint), else 0
        bool            isFriendlySensor;
    };
    // Read-only walk of current mission visibility/contact state.
    // No sensor updates, no visibility recompute, no gameplay side effects.
    // Fills `out` up to `maxOut`; returns count. Caller owns storage (no alloc).
    static int enumerateTacticalBlips(TacBlip* out, int maxOut);
```

- [ ] **Step 2: Move the existing loop body into the enumerator**

In `code/gametacmap.cpp`, lift the mover/sensor/objective walk (currently inlined in `GameTacMap::render`, ~lines 235–360) into `enumerateTacticalBlips`, writing `TacBlip` records instead of drawing. Keep the exact color/contact-status logic (friendly green `0xff00cc00` / neutral blue / enemy red / selected brighter / shutdown ring=0). Then make `GameTacMap::render` call `enumerateTacticalBlips` into a fixed-size stack buffer (`TacBlip buf[MAX_MOVERS];`) and draw from it via the existing `drawBlip`/`drawSensor`. This proves the extraction is behavior-preserving for the minimap.

- [ ] **Step 3: Build**

Run: `cmake --build build64 --target mc2 --config RelWithDebInfo`.
Expected: clean build.

- [ ] **Step 4: Smoke — minimap must be unchanged**

```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS (minimap renders identically; extraction is behavior-preserving).

- [ ] **Step 5: Manual visual check (user)** — minimap blips/sensor rings/objectives look identical to before the refactor.

- [ ] **Step 6: Commit**

```bash
git add code/gametacmap.h code/gametacmap.cpp
git commit -m "refactor(overview): extract read-only enumerateTacticalBlips, share with minimap (T3)"
```

---

## Task 5: Icon overlay (T4 — first visuals)

**Files:**
- Modify: `code/tacticaloverview.h` / `.cpp` (add `drawOverlay`)
- Modify: `code/missiongui.cpp` (call `drawOverlay` in the HUD pass)

- [ ] **Step 1: Add `drawOverlay` to the controller**

In `code/tacticaloverview.h`:
```cpp
    // Draw the 2D strategic-icon overlay (and, in Task 6, the tint). Called
    // from the HUD 2D pass, after the world is rendered. No-op when t == 0.
    void drawOverlay(Camera* eye);
```

- [ ] **Step 2: Implement icon projection + draw**

In `code/tacticaloverview.cpp`:
```cpp
#include "gametacmap.h"   // GameTacMap::TacBlip + enumerateTacticalBlips

void TacticalOverview::drawOverlay(Camera* eye) {
    if (!enabled() || !active() || !eye) return;
    const float a = state_.iconAlpha();
    if (a <= 0.0f) return;

    GameTacMap::TacBlip blips[MAX_MOVERS];
    int n = GameTacMap::enumerateTacticalBlips(blips, MAX_MOVERS);
    for (int i = 0; i < n; ++i) {
        Stuff::Vector4D screen;
        Stuff::Vector3D w = blips[i].world;
        if (!eye->projectZ(w, screen)) continue;     // existing projection; off-screen skip
        // Modulate alpha into the icon color (high byte), draw a screen-space
        // sprite at (screen.x, screen.y) using the existing blip texture.
        unsigned long col = applyAlpha(blips[i].color, a);
        drawScreenIcon(screen.x, screen.y, col, blips[i].shape); // gos_DrawQuads helper
    }
}
```
Implement `drawScreenIcon` with the existing GameOS 2D quad/blip draw used by `GameTacMap::drawBlip` (reuse `blipHandle` texture). `applyAlpha` scales the existing color's alpha byte by `a`. No per-frame texture loads (load the blip texture once, cached).

- [ ] **Step 3: Call from the HUD pass**

In `code/missiongui.cpp`, in the same render path that draws the tac map / HUD (after world render), add:
```cpp
    g_tacticalOverview.drawOverlay(eye);
```

- [ ] **Step 4: Build**

Run: `cmake --build build64 --target mc2 --config RelWithDebInfo`.

- [ ] **Step 5: Screenshot acceptance (user/headless)**

```powershell
$env:MC2_TACTICAL_OVERVIEW=1; $env:MC2_SCREENSHOT_AT_FRAME=600
py -3 ...\scripts\run_smoke.py --mission mc2_01 --duration 30 --keep-logs
Remove-Item Env:MC2_TACTICAL_OVERVIEW; Remove-Item Env:MC2_SCREENSHOT_AT_FRAME
```
Capture three states (drive `t` via hotkey before the frame, or capture at distinct frames): `t=0` normal, `t≈0.5` crossfade (icons faint), `t=1` icons full. Expected: icons appear at unit positions, team-colored, fading in across the band.

- [ ] **Step 6: Smoke — flag OFF still inert**

```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS.

- [ ] **Step 7: Commit**

```bash
git add code/tacticaloverview.h code/tacticaloverview.cpp code/missiongui.cpp
git commit -m "feat(overview): 2D strategic-icon overlay with altitude cross-fade (T4)"
```

---

## Task 6: Friendly-coverage tint (T5)

**Files:**
- Modify: `code/tacticaloverview.cpp` (extend `drawOverlay` with tint layer)

- [ ] **Step 1: Draw friendly sensor coverage as additive soft circles**

In `drawOverlay`, before the icon loop, when `TacticalOverview::tintEnabled()`:
```cpp
    if (tintEnabled()) {
        const float ta = state_.iconAlpha() * 0.35f;   // tint strength under blend
        for (int i = 0; i < n; ++i) {
            if (!blips[i].isFriendlySensor || blips[i].sensorRadius <= 0.0f) continue;
            // Project the sensor center + a rim point to get a screen-space radius,
            // draw an additive friendly-colored soft disc (reuse a radial texture or
            // a fan of the blip sprite). No new aggregation; one disc per sensor.
            drawFriendlyCoverage(eye, blips[i].world, blips[i].sensorRadius, ta);
        }
    }
```
`drawFriendlyCoverage` projects center + `center + (radius along camera-right)` to derive a pixel radius, then draws one additive friendly-tinted disc. Low alpha; additive blend so overlaps brighten — the intended "where I can see" feel.

- [ ] **Step 2: Build**

Run: `cmake --build build64 --target mc2 --config RelWithDebInfo`.

- [ ] **Step 3: Screenshot acceptance + kill-switch (user/headless)**

```powershell
$env:MC2_TACTICAL_OVERVIEW=1; $env:MC2_SCREENSHOT_AT_FRAME=600
py -3 ...\scripts\run_smoke.py --mission mc2_01 --duration 30 --keep-logs
$env:MC2_TACTICAL_OVERVIEW_TINT=0   # kill switch must remove tint, keep icons
py -3 ...\scripts\run_smoke.py --mission mc2_01 --duration 30 --keep-logs
Remove-Item Env:MC2_TACTICAL_OVERVIEW; Remove-Item Env:MC2_TACTICAL_OVERVIEW_TINT; Remove-Item Env:MC2_SCREENSHOT_AT_FRAME
```
Expected: friendly areas tinted at overview altitude; `=0` removes tint, icons remain.

- [ ] **Step 4: Smoke — flag OFF inert**

```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS.

- [ ] **Step 5: Commit**

```bash
git add code/tacticaloverview.cpp
git commit -m "feat(overview): friendly-coverage tint with independent kill switch (T5)"
```

---

## Task 7: Pick/selection polish + perf check (T6)

**Files:**
- Modify: `code/missiongui.cpp` (verify pick at altitude; no new pick path)
- Modify: `code/tacticaloverview.cpp` (perf guard if needed)

- [ ] **Step 1: Verify selection works in overview (manual, user)**

With `MC2_TACTICAL_OVERVIEW=1`, in overview: left-click near an icon selects the unit; right-drag rotates/tilts; orders issue correctly. This uses the existing world-pos pick — confirm no new path was introduced. If clicks feel imprecise at altitude, widen the icon's effective click radius by snapping the click to the nearest enumerated blip within N pixels (still resolves to the unit's world pos — no new pick path).

- [ ] **Step 2: Perf check against budget**

Confirm the overlay meets `<0.3 ms CPU / <0.3 ms GPU` on tier1-scale maps. If Tracy is convenient, wrap `drawOverlay` in a coarse `ZoneScopedN("TacOverview.Overlay")` (single per-pass zone — respect the 100ns floor / no per-blip zones). Verify: no per-frame texture loads, no per-blip heap allocation (stack buffer only).

- [ ] **Step 3: Build + smoke**

Run: `cmake --build build64 --target mc2 --config RelWithDebInfo` then
```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: clean build, 5/5 PASS.

- [ ] **Step 4: Commit**

```bash
git add code/missiongui.cpp code/tacticaloverview.cpp
git commit -m "feat(overview): overview pick polish + perf-budget zone (T6)"
```

---

## Task 8: Default-on decision (T7 — after visual gate)

**Files:**
- Modify: `code/tacticaloverview.cpp` (flip the master gate default, if approved)

- [ ] **Step 1: User visual sign-off**

User confirms the three acceptance screenshots (`t=0`, `t≈0.5`, `t=1`) plus interactive feel are good. **Do not flip the default before this gate.**

- [ ] **Step 2: Flip the default (only if approved)**

Change `TacticalOverview::enabled()` so the feature is ON unless `MC2_TACTICAL_OVERVIEW=0` (opt-out), mirroring the terrain-LOD cutover pattern:
```cpp
bool TacticalOverview::enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("MC2_TACTICAL_OVERVIEW");
        cached = (v && v[0] == '0') ? 0 : 1;   // default ON; only "=0" disables
    }
    return cached != 0;
}
```

- [ ] **Step 3: Full smoke on the default path**

```powershell
py -3 ...\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```
Expected: 5/5 PASS with the feature default-on.

- [ ] **Step 4: Commit**

```bash
git add code/tacticaloverview.cpp
git commit -m "feat(overview): default-on cutover after visual gate (T7)"
```

---

## Self-review notes

- **Spec coverage:** hybrid cross-fade (Tasks 1,3,5), wheel+hotkey activation (Tasks 1–3), unit icons (Task 5), sensor fog+contacts (Task 4 reuses contact-status walk), objectives+nav (Task 4 reuses existing markers), friendly-coverage tint (Task 6), steep-perspective no-ortho (Task 3 envelope), camera-return contract (Task 3), UI-exclusion (Tasks 1–2), read-only enumeration contract (Task 4), tint kill switch (Task 6), perf budget (Task 7), screenshot oracle (Tasks 5–6), env flags + default-OFF→ON (Tasks 2,8), no model fade v1 (kept out of all tasks). All covered.
- **Open confirmations (grep before coding):** exact camera accessor names (`setCameraAltitude`/`getCameraAltitude`/`setCameraTilt`/`getCameraTilt`/`getCameraRotation`/max-zoom query) in `mclib/camera.h` — if a direct altitude/tilt setter is absent, drive toward target via the existing `ZoomIn/ZoomOut` + `tiltUp/tiltDown` deltas (public-API-only rule holds); the **mouse-wheel sign** at the real missiongui site (Task 2 contract); the HUD render call site in `code/missiongui.cpp`; the game CMake source list that names `code/missiongui.cpp`; the `DEBUGWINS_print` header. These are integration lookups, not design gaps.
- **Type consistency:** `TacticalOverviewState` API (`t/applyWheel/toggleHotkey/update/iconAlpha/active`) is identical across Tasks 1–3; `TacBlip`/`enumerateTacticalBlips` identical across Tasks 4–6.
