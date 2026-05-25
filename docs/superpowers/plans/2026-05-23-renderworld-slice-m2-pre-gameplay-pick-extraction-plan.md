# RenderWorld Slice M2-pre -- Gameplay-Pick Extraction Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pay the M1.6 greybeard debt by extracting the gameplay-pick spine (`tryGameplayPick`) and the pure coord transform (`screenToFboPixel`) out of the inline body of `MissionInterfaceManager::tryStaticPropPick` (`code/missiongui.cpp:6168-6273`) into a new shared TU `code/gameplay_pick.{h,cpp}`. M2-pre is a PURE REFACTOR: no new gameplay feature, no new env var (except the validation-gate-only `MC2_GAMEPLAY_PICK_SELFTEST`), no shader edit, no substrate API change, no selection-behavior change. Env-OFF default tier1 5/5 pixel-parity at idle vs M1.6 HEAD `db25d67`. Env-ON Shift+click on a static prop continues to emit `[STATIC_PROP_PICK v1] hit ...` with the SAME 11-field diagnostic format M1.6 ships. The refactored `tryStaticPropPick` body shrinks from ~80 lines to ~30 lines (substitutive proof; inline machinery REPLACED, not duplicated).

**Architecture:** Two new files at `code/gameplay_pick.{h,cpp}` host three POD types (`GameplayPickRequest`, `GameplayPickContext`, `GameplayPickResult`) plus two free functions (`tryGameplayPick`, `screenToFboPixel`) plus the env-flag-gated self-test (`RunGameplayPickSelfTest`). `code/missiongui.cpp` keeps `tryStaticPropPick` but its body becomes: category-env gate + build `GameplayPickRequest` + call `tryGameplayPick(req)` + switch on `result.outcome` (hit emits `[STATIC_PROP_PICK v1] hit`, miss emits `[STATIC_PROP_PICK v1] miss` when debug enabled, gated/skipped silent). `RenderWorld::init` invokes `RunGameplayPickSelfTest` once at startup after the existing substrate self-test, gated by `MC2_GAMEPLAY_PICK_SELFTEST=1` + `MC2_OBJECT_ID_BUFFER=1`. Eight synthetic `GameplayPickRequest` inputs assert each `result.outcome` matches expected; emits `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` or `result=FAIL step=N expected=<...> actual=<...>`.

**Tech Stack:** C++17, MSVC `--config RelWithDebInfo`, CMake explicit source list (root `CMakeLists.txt:212` already lists `code/missiongui.cpp`; new `code/gameplay_pick.cpp` is appended sibling-style). New file uses 4-space indent (matches `tryStaticPropPick` helper body convention at `code/missiongui.cpp:6175+`; `missiongui.cpp` top-level style uses tabs but the M1.6 helper body shipped 4-space). Free-function header is a NEW precedent under `code/` (per spec Section 7 "Precedent note"). Include firewall not edited: `scripts/check-include-firewall.sh:22` SCOPE_DIRS does not list `code/`; new TU reaches into `RenderWorld/RenderWorld.h` via the same pattern `code/missiongui.cpp:33` already uses (verified by spec adversarial review M-2).

**Spec:** `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md` (EXECUTABLE-READY).
**Adversarial review applied (spec):** `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-spec-adversarial-review.md` (0 CRIT / 2 MAJOR / 5 MINOR; all resolved in the spec):
- M-1: Gate 2 mechanism = AUTOMATED `MC2_GAMEPLAY_PICK_SELFTEST=1` substrate self-test (analogous to M1.5 `[RENDER_WORLD_SELFTEST v1]`). User-driven canary retained as supplementary visual verification, NOT load-bearing.
- M-2: Firewall script does NOT scope `code/`; Q11 RESOLVED moot.
- m-1: Line numbers refreshed to HEAD `db25d67`: 4 mover-fallback sites at `:1476/1511/1740/1763` (NOT `:1460/1487/1690/1705` as M1.6 spec cited). 2 helper-call sites at `:1538` and `:1781`. Body at `:6168-6273`.
- m-2: `screenToFboPixel` carries the M1.6 derivation comment verbatim (the load-bearing explanation `1853ad9` fixed turned on).
- m-3: Gate 4 git-show syntax fixed (`git show <sha>:<path>`, not `<path>@<sha>`).
- m-4: `tests/m2_pre_log_diff.sh` named in Section 8 Gate 6 as the SHAPE-check backup; ship as plan deliverable only if Gate 2 self-test fails to land cleanly (otherwise Gate 6 is optional). NOT a Phase A/B/C blocker in this plan.
- m-5: Q1 RESOLVED Candidate A (new file `code/gameplay_pick.{h,cpp}`); precedent established, not mirrored.

**Predecessor slice:** M1.6 SHIPPED 2026-05-23 (`db25d67`); coord-fix `1853ad9` already in HEAD. Substrate inspectable via `RenderWorld::lookupAtPixel(glX, glY) -> LookupResult` with generation + alive validation.

---

## Decisions resolved before execution (from spec + adversarial review 2026-05-23)

All decisions below are baked into the spec at Sections 3, 4, 5, 6, 7, 8, 11 and at Q1/Q7/Q11/M-1 resolutions. Do NOT relitigate during execution. If a decision appears wrong during a task, STOP, escalate to the user, and amend the spec before proceeding.

```
M2-pre decisions resolved before execution:
  Q1 RESOLVED: helper location = NEW FILE code/gameplay_pick.{h,cpp}.
    Establishes the game-side shared-utility convention; M2.6+
    mech-pickup will share the file as the second caller. Header
    is a new precedent under code/ (which is otherwise class-method
    declarations); justification in spec Section 7 "Precedent note."

  Q7 RESOLVED: KEEP the name moverSelectedThisFrame in
    GameplayPickRequest. Variable semantic IS mover-only today;
    renaming preempts a generalization before any non-mover legacy
    path exists. Smaller diff.

  Q11 RESOLVED: No firewall script update needed.
    scripts/check-include-firewall.sh:22 SCOPE_DIRS lists only
    RenderCore RenderWorld Visibility MeshRenderer MaterialSystem
    DebugRenderer RenderDeviceGL. code/ is not scoped, so
    code/gameplay_pick.cpp including RenderWorld/RenderWorld.h is
    unconstrained -- same reach-in pattern code/missiongui.cpp:33
    already uses.

  MAJOR M-1 RESOLVED: Validation gate = AUTOMATED SUBSTRATE
    SELF-TEST gated by MC2_GAMEPLAY_PICK_SELFTEST=1. Exercises
    tryGameplayPick at 8 synthetic GameplayPickRequest inputs;
    asserts each outcome matches expected. Mirrors M1.5
    [RENDER_WORLD_SELFTEST v1] shape. Emits
    [GAMEPLAY_PICK_SELFTEST v1] result=PASS/FAIL.

  Behavior preservation: env-ON [STATIC_PROP_PICK v1] hit/miss log
    lines are byte-identical to M1.6 HEAD (db25d67) format strings.
    Plan stage diffs the format string text against HEAD before
    committing Task 3.

  CMake integration: root CMakeLists.txt:212 lists code/missiongui.cpp
    explicitly. New code/gameplay_pick.cpp is appended to that same
    list. There is NO globbing -- T1 MUST add the explicit entry.

  Coord-translation behavior: must EXACTLY match M1.6 commit 1853ad9.
    T1's screenToFboPixel body is copied VERBATIM from
    code/missiongui.cpp:6217-6233 (the inline block), with the
    gos_GetViewport call HOISTED to the caller (tryGameplayPick).
    The pure function takes vMulX/Y, vAddX/Y, drawableWidth/Height
    as inputs; performs only arithmetic.
```

Plan-level implications:

- **CMakeLists.txt is an explicit list (no glob).** T1 MUST add `"code/gameplay_pick.cpp"` to the source list around `:212` next to `"code/missiongui.cpp"`. If T1 forgets, the new TU is silently excluded from the build, and `code/missiongui.cpp` will refuse to link (undefined references to `tryGameplayPick`, `screenToFboPixel`, `RunGameplayPickSelfTest`). Verify by `grep -n gameplay_pick CMakeLists.txt` after T1's edit; expect ONE hit.
- **Atomicity of T3 (refactor of `tryStaticPropPick` body).** T3 is the load-bearing edit. The spec promises behavior preservation; this plan shows the full "Existing" + "Replace with" blocks so the reviewer can diff line-by-line. T3 lands as a single commit; partial T3 (e.g. "introduce the call but leave inline code beside it") would FAIL the substitutive-not-additive proof.
- **T4 self-test is gated by TWO env vars (`MC2_GAMEPLAY_PICK_SELFTEST=1` AND `MC2_OBJECT_ID_BUFFER=1`).** The substrate-off case makes the self-test vacuous (step 8 cannot reach `lookupAtPixel`); the test refuses to run rather than emitting a misleading PASS. The init banner emits `[GAMEPLAY_PICK_SELFTEST v1] enabled=0 substrate=N` when the env var is off; emits the `step=all` PASS line only when both are on AND all 8 steps pass. The self-test runs at `RenderWorld::init()` BEFORE any scene frame, so attachment-2 is freshly cleared to 0 and step 8's expected outcome is `miss` deterministically (NOT `hit OR miss`); the gate is spine-reachability, not lookup correctness.
- **Greybeard ruling is fresh (no pre-judged verdict).** Spec Section 11 LEANS META-FIX (substitutive: inline machinery REPLACED in `tryStaticPropPick`); T6 dispatches the greybeard skill subagent which may rule either way. Both outcomes are acceptable; the plan records whichever ruling fires.

---

## Open items surfaced for user sign-off BEFORE Task 1 executes

These are NOT decisions to make during execution. They are operational choices the plan author flags so the user can rule (or accept the plan's lean) before Task 1.

### O1. Header convention under `code/` (RESOLVED 2026-05-23 by spec Q1)

The plan ships `code/gameplay_pick.h` as a free-function utility header (3 POD structs + 3 function declarations). The reviewer m-5 finding confirmed this ESTABLISHES a precedent in `code/` (where existing headers are class-method declarations). Future M3 (terrain pick) and M4 (VFX pick) extensions follow the same shape; M2.6 (mech pickup) is the first non-static-prop caller. No further user sign-off required.

### O2. Indent style of the new file

The plan ships `code/gameplay_pick.cpp` with 4-space indent (matches the helper body convention `tryStaticPropPick` shipped at `code/missiongui.cpp:6175+`). The surrounding `missiongui.cpp` top-level style uses tabs; the new file is NOT a continuation of `missiongui.cpp` and the 4-space choice keeps the new TU readable as a standalone unit. If reviewer prefers tabs, T1 can swap before commit.

**Lean:** 4-space indent in the new file.

### O3. Self-test step 8 pixel choice (T4)

Step 8 asserts the spine reached `lookupAtPixel` (outcome=miss DETERMINISTICALLY, or hit as a defensive accept). `RunGameplayPickSelfTest()` runs at `RenderWorld::init()` which executes BEFORE any scene frame; attachment-2 is freshly cleared to 0, so `lookupAtPixel` returns `LookupResult{isValid=false}` and `tryGameplayPick` reports outcome=miss. The plan uses `(640, 360)` as the test mouse position (viewport center on a 1280x720 logical canvas) -- this lands inside any tier1 mission's FBO regardless of resolution. Step 8 is therefore a SPINE REACHABILITY check (the gate chain reaches `lookupAtPixel`), NOT a lookup-correctness check; whether `lookupAtPixel` returns hit on a live scene is verified by the M1.5 `[OBJECT_ID_SELFTEST v1]` passive canary, NOT here. Failure modes are outcome=skipped or outcome=gated (indicating the spine short-circuited before reaching `lookupAtPixel`).

**Lean:** `(640, 360)` viewport-center synthetic mouse position.

---

## Pre-flight reading (engineer MUST read before Task 1)

1. Spec entirely. Especially Sections 3 (shared types), 4 (tryGameplayPick contract + algorithm), 5 (screenToFboPixel contract + derivation), 6 (caller pattern, refactored tryStaticPropPick body), 7 (helper location decision), 8 (validation gates 1-6), 9 (forbidden behaviors), 10 (M2.6 extension contract), 11 (greybeard expectation).
2. Adversarial review entirely. The 2 MAJORs are now spec decisions, but the WRITE-TIME grep evidence in the review is what every Task code edit must reverify before touching.
3. M1.6 plan format (`docs/superpowers/plans/2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md`). M2-pre mirrors its structure (Phase A substrate / Phase B refactor / Phase C closure).
4. M1.6 implementation (refactor target):
   - `code/missiongui.cpp:6168-6273` -- `tryStaticPropPick` body (the thing being refactored).
   - `code/missiongui.cpp:1476/1511/1740/1763` -- 4 `moverSelectedThisFrame = true` writer sites (UNTOUCHED by M2-pre; verify present before T3).
   - `code/missiongui.cpp:1538` + `:1781` -- 2 helper call sites at tails of `updateOldStyle` + `updateAOEStyle` (UNTOUCHED by M2-pre; signature unchanged).
5. M1.5 substrate-self-test pattern (`RenderWorld/RenderWorld.cpp:210-305`, the `runSubstrateSelfTest` function). T4's `RunGameplayPickSelfTest` mirrors this shape: env-flag gate + step assertions + PASS/FAIL stderr line + early-return on first FAIL.
6. Worktree CLAUDE.md -- full file. Especially: NO emoji, grep-before-cite, build `--config RelWithDebInfo`, full-relink discipline, canonical smoke gate command, substitutive-not-additive rule, debug-instrumentation rule (`[SUBSYS v1]` schema).
7. Verify every cited file:line in this plan is still at the cited offset BEFORE starting any task that touches that file. The audit was performed 2026-05-23 against HEAD `db25d67`. If shipping HEAD has drifted, adjust line numbers in this plan AND in the commit message, then proceed.

---

## File structure

**Created files (M2-pre introduces TWO new files):**

- `code/gameplay_pick.h` -- header for the new TU. Declares 3 POD types (`GameplayPickRequest`, `GameplayPickContext`, `GameplayPickResult`) + 3 free functions (`tryGameplayPick`, `screenToFboPixel`, `RunGameplayPickSelfTest`). New precedent under `code/` (free-function utility header).
- `code/gameplay_pick.cpp` -- implementation. Defines `tryGameplayPick`, `screenToFboPixel`, `RunGameplayPickSelfTest`. Reaches into `../RenderWorld/RenderWorld.h` for `IsObjectIdBufferEnabled`, `lookupAtPixel`, `LookupResult` (same reach-in pattern `code/missiongui.cpp:33` already uses).

**Modified files (grep-verified 2026-05-23 against HEAD `db25d67`):**

- `CMakeLists.txt` -- add ONE line `"code/gameplay_pick.cpp"` to the explicit source list around `:212` next to `"code/missiongui.cpp"`. (Task 1.)
- `code/missiongui.cpp` -- add `#include "gameplay_pick.h"`; refactor `tryStaticPropPick` body (`:6168-6273`) to delegate to `tryGameplayPick`. Body shrinks ~80 -> ~30 lines. (Task 3.)
- `RenderWorld/RenderWorld.cpp` -- add ONE call to `RunGameplayPickSelfTest()` at the end of `init()` (line `:332`, immediately after the existing `runSubstrateSelfTest()` call). (Task 4.)
- `.claude/worktrees/nifty-mendeleev/CLAUDE.md` -- add Active campaigns bullet for M2-pre shipped state. (Task 7.)

**Untouched (load-bearing -- confirm via grep, not assumption):**

- `code/missiongui.cpp:1476/1511/1740/1763` -- the 4 `moverSelectedThisFrame = true` writer sites. Spec Q7 RESOLVED keep-name.
- `code/missiongui.cpp:1538` + `:1781` -- 2 helper-call-site tail dispatch. Argument list unchanged (`tryStaticPropPick` signature preserved).
- `code/missiongui.h:272-278` -- `tryStaticPropPick` private method declaration. Signature preserved.
- `RenderWorld/RenderWorld.h` -- no changes (M2-pre does NOT extend the substrate API).
- `shaders/*` -- M2-pre does NOT touch shaders.
- Save/load surfaces -- spec Section 9 forbids gameplay-state mutation; pick state explicitly NOT serialized.

---

## Phase A -- Substrate API additions (build green, no callers yet, no behavior change)

**Phase A goal:** the new `code/gameplay_pick.{h,cpp}` files exist and build. `code/missiongui.cpp` does NOT yet include the new header; `tryStaticPropPick` is unmodified; `RenderWorld::init` does NOT yet call `RunGameplayPickSelfTest`. Build green. Env-OFF tier1 5/5 PASS bit-identically vs M1.6 HEAD `db25d67` (no callers; new functions are dead code that LTO may elide).

**Phase A gate (must pass before Phase B starts):** build green AND tier1 5/5 PASS env-OFF default with no observable change. Linker accepts the new TU (verifies CMakeLists edit landed correctly).

### Task 1: Create `code/gameplay_pick.{h,cpp}` with POD types + screenToFboPixel + tryGameplayPick + RunGameplayPickSelfTest declarations only

**Files:**
- Create: `code/gameplay_pick.h`
- Create: `code/gameplay_pick.cpp` (declarations + empty body stubs)
- Modify: `CMakeLists.txt` (add `"code/gameplay_pick.cpp"`)

This task INTRODUCES the new TU with the header declarations + a minimal `.cpp` body containing `screenToFboPixel` ONLY (the pure transform; lowest risk). `tryGameplayPick` and `RunGameplayPickSelfTest` are declared in the header but their definitions are deferred to T2 + T4. The CMake edit lands here; build must go green with the partial implementation present.

- [ ] **Step 1: Re-grep the cited M1.6 anchor points (refresh against HEAD)**

```bash
grep -n "moverSelectedThisFrame\s*=\s*true\|tryStaticPropPick\|MissionInterfaceManager::tryStaticPropPick" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -20
```

Expected (verify exact line offsets; symbols stable, numbers may drift):
- 4 writer sites: `:1476`, `:1511`, `:1740`, `:1763`
- 2 helper call sites: `:1538`, `:1781`
- Body: `:6168` (function header)
- Body extends through `:6273` (closing brace)

If grep finds different offsets, update the plan body in T3's "Existing"/"Replace with" blocks BEFORE editing.

- [ ] **Step 2: Re-grep the firewall script SCOPE_DIRS**

```bash
grep -n "SCOPE_DIRS=" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: ONE hit at `:22` listing `RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL`. `code/` is NOT listed; no firewall edit needed. If grep shows `code/` in SCOPE_DIRS, STOP and escalate (spec assumption invalid).

- [ ] **Step 3: Re-grep the CMakeLists source list for `missiongui.cpp` placement**

```bash
grep -n "missiongui.cpp\|code/mainmenu.cpp" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CMakeLists.txt
```

Expected: `"code/missiongui.cpp"` at `:212`. The list is explicit (no glob); new entries are appended sibling-style. Identify the exact line to insert after.

- [ ] **Step 4: Create `code/gameplay_pick.h`**

Write a new file at `code/gameplay_pick.h`. Content:

```cpp
// code/gameplay_pick.h
//
// M2-pre (RenderWorld Slice): shared gameplay-pick spine extracted from
// MissionInterfaceManager::tryStaticPropPick (code/missiongui.cpp).
// Pays the M1.6 greybeard debt: when M2.6 mech-pickup wires up, both
// callers (tryStaticPropPick + the future tryMechPick) call
// tryGameplayPick() instead of duplicating the inline machinery.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
// Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md
//
// Free-function utility header -- new convention in code/ (existing
// headers here are class-method declarations). Justified by the named
// multi-consumer trajectory in spec Section 7.

#ifndef GAMEPLAY_PICK_H
#define GAMEPLAY_PICK_H

#include "../RenderWorld/RenderWorld.h"  // RenderWorld::LookupResult

// Input gesture + missiongui state. Mirrors the 7 args
// tryStaticPropPick already takes, plus nothing.
struct GameplayPickRequest {
    // Viewport-space mouse coords (Win32 convention: origin top-left,
    // Y grows down). Match userInput->getMouseX/Y().
    int  mouseX;
    int  mouseY;

    // Edge-triggered gesture flags from missiongui input gather.
    bool shiftDn;            // userInput->shift() this frame
    bool leftClicked;        // (!isLeftDrag && !isRightDrag && isLeftClick && !lastUpdateDoubleClick)
    bool bGui;               // cursor in HUD/control region
    bool bLeftDouble;        // double-click in progress

    // Mover-first fallback observable. Caller sets to true when the
    // legacy click-driven selection path consumed THIS click (see M1.6
    // Q6 4-site instrumentation map at code/missiongui.cpp:1476/1511/
    // 1740/1763). Spec Q7 RESOLVED: keep the mover-only name.
    bool moverSelectedThisFrame;
};

// Diagnostic context propagated to caller for logging. All fields echo
// the inputs + the intermediate coord-translation results so caller
// logs can show the full transform on one line. Field set mirrors the
// M1.6 [STATIC_PROP_PICK v1] hit/miss log printf args exactly.
struct GameplayPickContext {
    int   mouseX, mouseY;        // echo of request (viewport-space, top-left origin)
    int   fboX,   fboY;          // post-scale FBO pixel (top-left origin)
    int   glX,    glY;           // post-y-flip GL pixel (bottom-left origin)
    float vMulX,  vMulY;         // gos_GetViewport scale (FBO-pixel units)
    float vAddX,  vAddY;         // gos_GetViewport offset
    int   drawableWidth;
    int   drawableHeight;
};

// Outcome enum + result. Caller switches on outcome.
struct GameplayPickResult {
    enum class Outcome {
        skipped, // env / gesture / off-screen gates failed; no lookup ran
        gated,   // moverSelectedThisFrame fired; legacy path won
        miss,    // lookupAtPixel ran; pixel was background (raw=0)
        hit,     // lookupAtPixel ran; valid handle returned
    };
    Outcome                    outcome;
    RenderWorld::LookupResult  lookup;  // valid iff outcome == hit
    GameplayPickContext        ctx;
};

// The shared gameplay-pick dispatcher. Single substrate-consumer spine.
// See spec Section 4 for full algorithm + ordering. M2.6 mech-pickup
// will be the second caller (spec Section 10 extension contract).
//
// Side effects: NONE on outcome in {skipped, gated}. ONE synchronous
// glReadPixels via RenderWorld::lookupAtPixel when all gates pass and
// moverSelectedThisFrame is false (i.e. outcome IN {miss, hit}).
// Logging: NONE. Debug-state mutation: NONE. Caller owns both.
// Thread-safety: main thread only (same as M1.6).
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req);

// Pure coord transform: viewport-space mouse coords -> FBO pixel ->
// GL pixel. No GL state mutation, no global reads, no I/O. Safe
// anywhere on any thread.
//
// Coord derivation (restored verbatim from M1.6 commit 1853ad9 at
// code/missiongui.cpp:6197-6216 -- the inline comment explains why
// mouseX/Y is viewport-relative and not FBO pixels; the scale formula
// "drawableWidth / vMulX" backs the viewport-scaling out to drawable-
// pixel coords). See M2-pre spec Section 5 for the precondition list
// (vMulX, vMulY > 0; caller has already off-screen-guarded).
void screenToFboPixel(int   mouseX,         int   mouseY,
                      float vMulX,          float vMulY,
                      float vAddX,          float vAddY,
                      int   drawableWidth,  int   drawableHeight,
                      int*  outFboX,        int*  outFboY,
                      int*  outGlX,         int*  outGlY);

// Validation-gate self-test. Gated by MC2_GAMEPLAY_PICK_SELFTEST=1 +
// MC2_OBJECT_ID_BUFFER=1. Exercises tryGameplayPick with 8 synthetic
// GameplayPickRequest inputs; asserts each result.outcome matches
// expected. Emits to stderr:
//   [GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
//   [GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
// Mirrors M1.5 RenderWorld::runSubstrateSelfTest() shape. FAIL is a
// STOP indicating the extracted spine has diverged from M1.6 gate
// semantics. Spec Section 8 Gate 2.
void RunGameplayPickSelfTest();

#endif // GAMEPLAY_PICK_H
```

- [ ] **Step 5: Create `code/gameplay_pick.cpp` with screenToFboPixel implementation; tryGameplayPick + RunGameplayPickSelfTest stubs only**

Write a new file at `code/gameplay_pick.cpp`. T1 lands `screenToFboPixel` only (lowest-risk extraction; pure function). T2 lands `tryGameplayPick`; T4 lands `RunGameplayPickSelfTest`.

```cpp
// code/gameplay_pick.cpp
//
// M2-pre: shared gameplay-pick spine. See gameplay_pick.h for the
// full contract. Implementation steps:
//   T1: screenToFboPixel (this file's pure transform; the lowest-risk
//       extraction with a verbatim copy of the M1.6 inline coord block).
//   T2: tryGameplayPick (the spine; gate ladder + coord scaling +
//       lookupAtPixel).
//   T4: RunGameplayPickSelfTest (gated by MC2_GAMEPLAY_PICK_SELFTEST=1).

#include "gameplay_pick.h"

#include <cstdio>   // std::fprintf for self-test result lines
#include <cstdlib>  // std::getenv for the self-test env-flag gate
#include <cstring>  // std::strcmp for env-flag value parse

// Forward-declare the GOS viewport accessor; matches the signature at
// code/missiongui.cpp:6218. Pulled forward here to avoid dragging the
// full GameOS header chain into this small TU.
extern "C" void gos_GetViewport(float* vMulX, float* vMulY,
                                float* vAddX, float* vAddY);

// Environment struct accessor: M2-pre needs Environment.drawableWidth /
// drawableHeight (same fields M1.6's inline body reads at
// code/missiongui.cpp:6219-6220). Reach into the GameOS header that
// declares Environment. The exact include matches what missiongui.cpp
// already pulls.
#include "../GameOS/include/gameos.hpp"

//----------------------------------------------------------------------
// screenToFboPixel -- pure coord transform.
//
// Derivation (verbatim from M1.6 commit 1853ad9 inline comment at
// code/missiongui.cpp:6197-6216):
//
//   missiongui mouseX/Y from userInput->getMouseX/Y() is
//   mouseXPosition * viewMulX, where mouseXPosition is normalized to
//   drawable (0.0-1.0) and viewMulX is the UI viewport width (800 in
//   the 800x600 logical canvas). So mouseX/Y is in viewport-relative
//   coords (0-vMulX, 0-vMulY), NOT FBO pixels. lookupAtPixel needs
//   FBO pixels.
//
//   Translation: mouseX/Y (viewport space) -> FBO pixel:
//     normalized = mouseX / vMulX (fraction across viewport)
//     fboPixel   = vAddX + normalized * vMulX_fbo
//   where vMulX/Y from gos_GetViewport are already in FBO-pixel units
//   (viewportRight - viewportLeft) * drawableWidth. So vMulX IS the
//   FBO-pixel width of the viewport region. But mouseXPosition is
//   normalized to drawable (not viewport), and getMouseX() multiplies
//   by vMulX -- yielding (mouseX_pixel_in_drawable / drawableWidth) *
//   viewport_pixel_width. For full-screen viewport (vMulX=drawableWidth),
//   this is identity. For sub-viewport, it's a scaled-down value.
//
//   To convert back: multiply by (drawableWidth / vMulX) to get raw
//   drawable-pixel-X. (Equivalent to "undo the viewport scaling".)
//   Then add vAddX for viewport-offset case. Y mirrors.
//
// Precondition: caller has off-screen-guarded (mouseX/Y in [0, vMulX/Y)
// AND vMulX/Y > 0). This helper does NOT re-check.
//----------------------------------------------------------------------
void screenToFboPixel(int   mouseX,         int   mouseY,
                      float vMulX,          float vMulY,
                      float vAddX,          float vAddY,
                      int   drawableWidth,  int   drawableHeight,
                      int*  outFboX,        int*  outFboY,
                      int*  outGlX,         int*  outGlY)
{
    // Undo the viewport scaling: viewport-relative -> drawable-pixel.
    const float scaleX = (vMulX > 0.0f) ? ((float)drawableWidth  / vMulX) : 1.0f;
    const float scaleY = (vMulY > 0.0f) ? ((float)drawableHeight / vMulY) : 1.0f;

    *outFboX = (int)(vAddX + (float)mouseX * scaleX);
    *outFboY = (int)(vAddY + (float)mouseY * scaleY);

    // GL convention: origin bottom-left. Mirrors mclib/mouse.cpp:225
    // and mclib/utilities.cpp:111-115 y-flip pattern.
    *outGlX  = *outFboX;
    *outGlY  = drawableHeight - 1 - *outFboY;
}

//----------------------------------------------------------------------
// tryGameplayPick -- the shared gameplay-pick spine.
// Implementation lands in T2; T1 ships a STUB so the header symbol
// resolves cleanly and the linker accepts the new TU.
//----------------------------------------------------------------------
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req)
{
    // T1 stub: returns skipped. T2 replaces the body with the full
    // algorithm per spec Section 4.
    (void)req;
    GameplayPickResult r{};
    r.outcome = GameplayPickResult::Outcome::skipped;
    return r;
}

//----------------------------------------------------------------------
// RunGameplayPickSelfTest -- validation-gate self-test.
// Implementation lands in T4; T1 ships a STUB so the header symbol
// resolves cleanly. T4's body wires the 8-step assertion sequence.
//----------------------------------------------------------------------
void RunGameplayPickSelfTest()
{
    // T1 stub: no-op. T4 replaces with the env-flag gate + 8-step
    // assertion sequence per spec Section 8 Gate 2.
}
```

- [ ] **Step 6: Add the new .cpp to CMakeLists.txt**

Existing (verbatim, around `:211-213`):

```cmake
    "code/mechicon.cpp"
    "code/missiongui.cpp"
    "code/mpconnectiontype.cpp"
```

Replace with:

```cmake
    "code/mechicon.cpp"
    "code/missiongui.cpp"
    "code/gameplay_pick.cpp"
    "code/mpconnectiontype.cpp"
```

- [ ] **Step 7: Build (header + stub body present; no callers yet)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The new TU contributes 3 symbols (`tryGameplayPick`, `screenToFboPixel`, `RunGameplayPickSelfTest`) that nothing yet references; MSVC may not elide them at compile time but the linker will keep them as the TU is part of the explicit source list.

If build fails with "cannot find file gameplay_pick.cpp" or "unresolved external symbol gameplay_pick", the CMakeLists edit at Step 6 did not land. Re-verify with `grep -n gameplay_pick CMakeLists.txt` (expect ONE hit).

- [ ] **Step 8: Verify env-OFF tier1 smoke (no behavior change)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. No `[GAMEPLAY_PICK_SELFTEST v1]` lines (T1's stub does nothing; T4 adds the init-banner emission). `[STATIC_PROP_PICK v1] enabled=0 debug=0` continues to appear once per mission init (M1.6 banner, unaffected by M2-pre).

- [ ] **Step 9: Commit**

```bash
git add code/gameplay_pick.h code/gameplay_pick.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(gameplay_pick): introduce shared gameplay-pick TU + screenToFboPixel (M2-pre T1)

New free-function utility TU code/gameplay_pick.{h,cpp} establishing
the game-side shared-pick convention. Header declares 3 POD types
(GameplayPickRequest, GameplayPickContext, GameplayPickResult) + 3
free functions (tryGameplayPick, screenToFboPixel,
RunGameplayPickSelfTest).

T1 lands:
  - The header declarations in full
  - screenToFboPixel implementation -- the pure coord transform,
    verbatim from M1.6 commit 1853ad9 inline body at
    code/missiongui.cpp:6217-6233 with gos_GetViewport hoisted to
    the caller (T2 tryGameplayPick) and Environment.drawableWidth/
    Height passed as args
  - tryGameplayPick + RunGameplayPickSelfTest STUBS (bodies in T2 / T4)
  - CMakeLists.txt entry next to code/missiongui.cpp

Header is a NEW precedent under code/ (existing headers here are
class-method declarations); justified by the named multi-consumer
trajectory (M2.6 mech-pickup is the second caller) per spec Section
7. Include firewall not edited: code/ is outside
scripts/check-include-firewall.sh:22 SCOPE_DIRS; the reach-in to
RenderWorld/RenderWorld.h uses the same pattern code/missiongui.cpp:33
already ships.

No callers yet; substrate-only. Env-OFF tier1 5/5 PASS bit-identical
vs M1.6 HEAD db25d67.

Spec: 2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 2: Implement `tryGameplayPick` body in `code/gameplay_pick.cpp`

**Files:**
- Modify: `code/gameplay_pick.cpp` (replace `tryGameplayPick` stub with full algorithm)

This task replaces the T1 stub with the full algorithm per spec Section 4: env-substrate gate (`IsObjectIdBufferEnabled`) + gesture gates + mover-first short-circuit + viewport query + bounds check + coord scaling via `screenToFboPixel` + `lookupAtPixel`. Returns `GameplayPickResult` with outcome enum + lookup + ctx populated.

Still no callers (T3 wires `tryStaticPropPick` to delegate); build must remain green.

- [ ] **Step 1: Re-grep the M1.6 inline body to confirm gate order**

```bash
grep -n "IsStaticPropPickEnabled\|IsObjectIdBufferEnabled\|shiftDn\|leftClicked\|bGui\|bLeftDouble\|moverSelectedThisFrame\|gos_GetViewport\|lookupAtPixel" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -30
```

Expected: confirms the gate order in `tryStaticPropPick` body is:
1. `IsStaticPropPickEnabled` (CATEGORY gate; STAYS in `tryStaticPropPick` caller, NOT in `tryGameplayPick`)
2. `IsObjectIdBufferEnabled` (SUBSTRATE gate; MOVES into `tryGameplayPick`)
3. `shiftDn` / `leftClicked` / `bGui` / `bLeftDouble` (GESTURE gates; MOVE into `tryGameplayPick`)
4. `moverSelectedThisFrame` (FALLBACK gate; MOVES into `tryGameplayPick`)
5. `gos_GetViewport` + bounds + coord scale + `lookupAtPixel` (COORD + LOOKUP; MOVE into `tryGameplayPick`)

The CATEGORY gate (`IsStaticPropPickEnabled`) is the only one that stays in the caller, because it is category-specific and `tryGameplayPick` is category-agnostic. M2.6's `tryMechPick` will gate on `MC2_MECH_PICK` (strawman) before calling the same `tryGameplayPick`.

- [ ] **Step 2: Replace the `tryGameplayPick` stub in `code/gameplay_pick.cpp`**

Existing (verbatim, from T1):

```cpp
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req)
{
    // T1 stub: returns skipped. T2 replaces the body with the full
    // algorithm per spec Section 4.
    (void)req;
    GameplayPickResult r{};
    r.outcome = GameplayPickResult::Outcome::skipped;
    return r;
}
```

Replace with:

```cpp
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req)
{
    GameplayPickResult r{};
    r.ctx.mouseX  = req.mouseX;
    r.ctx.mouseY  = req.mouseY;
    r.outcome     = GameplayPickResult::Outcome::skipped;

    // Engine substrate gate. M1.5 cached bool; cheap.
    if (!RenderWorld::IsObjectIdBufferEnabled())
        return r;

    // Gesture gates (M1.6 Section 3 detection condition).
    if (!req.shiftDn)       return r;
    if (!req.leftClicked)   return r;
    if (req.bGui)           return r;
    if (req.bLeftDouble)    return r;

    // Mover-first fallback gate (M1.6 Section 4 + Q6/Q8 invariant).
    // The legacy click-driven selection path already consumed this
    // click to set a friendly mover; emitting a gameplay-pick log line
    // here would shadow the legacy gesture. Distinct from `skipped`
    // (the gesture WAS intended; the legacy path just won).
    if (req.moverSelectedThisFrame) {
        r.outcome = GameplayPickResult::Outcome::gated;
        return r;
    }

    // Viewport state for coord translation. gos_GetViewport returns
    // FBO-pixel-unit viewport extents + offset; see spec Section 5
    // derivation comment.
    gos_GetViewport(&r.ctx.vMulX, &r.ctx.vMulY,
                    &r.ctx.vAddX, &r.ctx.vAddY);
    r.ctx.drawableWidth  = Environment.drawableWidth;
    r.ctx.drawableHeight = Environment.drawableHeight;

    // Off-screen guard (viewport-space bounds). Defensive; in-game
    // Win32 mouse capture already clamps but a defensive check costs
    // one branch and avoids a silent glReadPixels at a clipped pixel.
    if (req.mouseX < 0 || req.mouseY < 0
        || req.mouseX >= (int)r.ctx.vMulX
        || req.mouseY >= (int)r.ctx.vMulY)
        return r;

    // Coord translation: viewport-space -> FBO pixel -> GL pixel.
    // Pure helper; no GL state mutation.
    screenToFboPixel(req.mouseX, req.mouseY,
                     r.ctx.vMulX, r.ctx.vMulY,
                     r.ctx.vAddX, r.ctx.vAddY,
                     r.ctx.drawableWidth, r.ctx.drawableHeight,
                     &r.ctx.fboX, &r.ctx.fboY,
                     &r.ctx.glX,  &r.ctx.glY);

    // Synchronous single-pixel readback (M1.5 substrate). Internally
    // validates against s_objectRecords (generation + alive).
    r.lookup = RenderWorld::lookupAtPixel(r.ctx.glX, r.ctx.glY);

    r.outcome = r.lookup.isValid
        ? GameplayPickResult::Outcome::hit
        : GameplayPickResult::Outcome::miss;
    return r;
}
```

- [ ] **Step 3: Build (full body present; still no callers)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The body now references `RenderWorld::IsObjectIdBufferEnabled` and `RenderWorld::lookupAtPixel` which `gameplay_pick.h` already pulled via `#include "../RenderWorld/RenderWorld.h"`. Also references `gos_GetViewport` (extern C forward-declared in T1's `.cpp`) and `Environment.drawableWidth/Height` (from `../GameOS/include/gameos.hpp` included in T1's `.cpp`).

If build fails with undefined reference to `gos_GetViewport`, the extern C declaration in T1's `.cpp` is mis-spelled; re-grep `code/missiongui.cpp:6218` for the canonical signature.

- [ ] **Step 4: Verify env-OFF tier1 smoke (no behavior change; still no callers)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. No `[GAMEPLAY_PICK_SELFTEST v1]` lines (T4 adds the init banner). `[STATIC_PROP_PICK v1] enabled=0 debug=0` continues to appear.

- [ ] **Step 5: Commit**

```bash
git add code/gameplay_pick.cpp
git commit -m "$(cat <<'EOF'
feat(gameplay_pick): implement tryGameplayPick spine (M2-pre T2)

Full algorithm per spec Section 4:
  - substrate env gate (IsObjectIdBufferEnabled)
  - 4 gesture gates (shiftDn, leftClicked, !bGui, !bLeftDouble)
  - mover-first short-circuit (moverSelectedThisFrame -> gated)
  - viewport query (gos_GetViewport) + bounds check
  - coord scaling via screenToFboPixel (pure helper from T1)
  - synchronous single-pixel readback via lookupAtPixel
  - outcome enum + lookup + ctx populated

Side effects: NONE on outcome in {skipped, gated}. ONE glReadPixels
on {miss, hit}. Logging: NONE -- caller emits category-specific log
lines. Debug-state mutation: NONE -- caller owns setLastStaticPropPick.

Still no callers; T3 refactors MissionInterfaceManager::tryStaticPropPick
to delegate. Build green; env-OFF tier1 5/5 PASS bit-identical vs
M1.6 HEAD.

Spec: 2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md sec 4

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase B -- Missiongui refactor + self-test (load-bearing; substitutive proof)

**Phase B goal:** `MissionInterfaceManager::tryStaticPropPick` body shrinks from ~80 lines to ~30 lines by delegating to `tryGameplayPick`. Self-test is wired into `RenderWorld::init` and emits `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` on a `MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1` run. Env-OFF tier1 5/5 PASS bit-identical (no per-frame cost added; helper short-circuits at category gate). Env-ON `[STATIC_PROP_PICK v1] hit/miss` log lines are byte-identical to M1.6 HEAD.

**Phase B gate (must pass before Phase C starts):** Gate 1 (env-OFF tier1 pixel-parity) PASS AND Gate 2 (self-test `result=PASS step=all`) PASS AND visual canary on mc2_03 (Gate 3) confirms hit-line format matches M1.6 HEAD diagnostic fields.

### Task 3: Refactor `MissionInterfaceManager::tryStaticPropPick` to delegate to `tryGameplayPick`

**Files:**
- Modify: `code/missiongui.cpp` (replace `tryStaticPropPick` body; add `#include "gameplay_pick.h"`)

This is the LOAD-BEARING refactor commit. The spec promises the body shrinks substitutively (inline machinery REPLACED, not duplicated beside the new helper call). Log format strings preserved verbatim. Signature preserved (the 7-arg method declared at `code/missiongui.h:272-278` is unchanged).

- [ ] **Step 1: Re-grep the M1.6 body to confirm offsets + log format strings**

```bash
grep -n "tryStaticPropPick\|STATIC_PROP_PICK v1\] hit\|STATIC_PROP_PICK v1\] miss" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -20
```

Expected:
- `:6168` -- `void MissionInterfaceManager::tryStaticPropPick(...)`
- `:6250` -- `"[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "`
- `:6266` -- `"[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d) "`

Capture the FULL printf format strings from grep output for the Step 3 byte-identical preservation check.

- [ ] **Step 2: Re-grep the existing `#include` block in missiongui.cpp**

```bash
grep -n "^#include\s*\".*gameplay_pick\|^#include\s*\".*RenderWorld" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -5
```

Expected: ONE hit at `:33` for `"../RenderWorld/RenderWorld.h"`. ZERO hits for `gameplay_pick.h` (not yet included). Identify where to insert the new include (immediately after `:33` is the canonical sibling-style spot).

- [ ] **Step 3: Add the `#include "gameplay_pick.h"` near the top of `code/missiongui.cpp`**

Existing (verbatim, `:33`):

```cpp
#include "../RenderWorld/RenderWorld.h"
```

Replace with (add the new include immediately after):

```cpp
#include "../RenderWorld/RenderWorld.h"
#include "gameplay_pick.h"  // M2-pre: tryGameplayPick spine + GameplayPickRequest
```

- [ ] **Step 4: Replace the `tryStaticPropPick` body**

Existing (verbatim, `code/missiongui.cpp:6160-6273`):

```cpp

//--------------------------------------------------------------------------------------

// M1.6: env-gated static-prop pick helper. See spec
// docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
// Sections 3 (detection condition), 4 (fallback order), 5 (y-flip), 11
// (forbidden behaviors), Q6 (4-site mover observable), Q8 (legacy
// preservation invariant).
void MissionInterfaceManager::tryStaticPropPick(bool moverSelectedThisFrame,
                                                bool shiftDn,
                                                bool leftClicked,
                                                bool bGui,
                                                bool bLeftDouble,
                                                int  mouseX,
                                                int  mouseY)
{
    // Fast path: env-OFF default. Two cached bools; no per-frame getenv.
    if (!RenderWorld::IsStaticPropPickEnabled())
        return;
    // Defense-in-depth: substrate must also be on. Skipping here avoids
    // the FBO-bind + glReadPixels stall on every dormant Shift+click.
    if (!RenderWorld::IsObjectIdBufferEnabled())
        return;

    // Gesture gate: Shift + single LMB click, NOT inside the GUI region,
    // NOT a double-click (legacy double-click path owns that gesture).
    if (!shiftDn)        return;
    if (!leftClicked)    return;
    if (bGui)            return;
    if (bLeftDouble)     return;

    // Section 11 + Q6/Q8 invariant: the legacy path already consumed
    // this click to select a mover. Emitting a log line here would
    // shadow the legacy gesture and create a user-visible incoherence.
    if (moverSelectedThisFrame)
        return;

    // M1.6 coord-space: missiongui mouseX/Y from userInput->getMouseX/Y()
    // is `mouseXPosition * viewMulX`, where mouseXPosition is normalized to
    // drawable (0.0-1.0) and viewMulX is the UI viewport width (800 in the
    // 800x600 logical canvas). So mouseX/Y is in viewport-relative coords
    // (0-vMulX, 0-vMulY), NOT FBO pixels. lookupAtPixel needs FBO pixels.
    //
    // Translation: mouseX/Y (viewport space) -> FBO pixel:
    //   normalized = mouseX / vMulX (fraction across viewport)
    //   fboPixel   = vAddX + normalized * vMulX_fbo
    // where vMulX/Y from gos_GetViewport are already in FBO-pixel units
    // (viewportRight - viewportLeft) * drawableWidth. So vMulX IS the
    // FBO-pixel width of the viewport region. But mouseXPosition is
    // normalized to drawable (not viewport), and getMouseX() multiplies
    // by vMulX -- yielding (mouseX_pixel_in_drawable / drawableWidth) *
    // viewport_pixel_width. For full-screen viewport (vMulX=drawableWidth),
    // this is identity. For sub-viewport, it's a scaled-down value.
    //
    // To convert back: multiply by (drawableWidth / vMulX) to get raw
    // drawable-pixel-X. (Equivalent to "undo the viewport scaling".)
    // Then add vAddX for viewport-offset case. Y mirrors.
    float vMulX = 0.0f, vMulY = 0.0f, vAddX = 0.0f, vAddY = 0.0f;
    gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
    const int dw = Environment.drawableWidth;
    const int dh = Environment.drawableHeight;

    // Off-screen bounds guard against UI canvas (viewport) dimensions.
    if (mouseX < 0 || mouseY < 0
        || mouseX >= (int)vMulX || mouseY >= (int)vMulY)
        return;

    // Undo the viewport scaling: viewport-relative -> drawable-pixel.
    const float scaleX = (vMulX > 0.0f) ? ((float)dw / vMulX) : 1.0f;
    const float scaleY = (vMulY > 0.0f) ? ((float)dh / vMulY) : 1.0f;
    const int fboX = (int)(vAddX + (float)mouseX * scaleX);
    const int fboY = (int)(vAddY + (float)mouseY * scaleY);
    const int glX  = fboX;
    const int glY  = dh - 1 - fboY;

    // Synchronous single-pixel readback. M1.5 lookupAtPixel internally
    // validates against s_objectRecords (generation + alive).
    RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(glX, glY);

    if (res.isValid) {
        // Update RenderWorld debug state. Single-slot; latest wins.
        RenderWorld::setLastStaticPropPick(res, mouseX, mouseY, glX, glY);
        // Sample back the debug-state struct so the log can include the
        // recipeIndex (LookupResult itself does not carry it; the
        // recipe lookup is done inside setLastStaticPropPick).
        const RenderWorld::StaticPropSelectionDebugState picked =
            RenderWorld::getLastStaticPropPick();
        // Unconditional hit log (spec Section 7); coord-diag fields included
        // to confirm viewport translation on this build.
        std::fprintf(stderr,
            "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
            "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
            res.handle.bits,
            (unsigned)res.handle.index(),
            (unsigned)res.handle.generation(),
            (int)picked.recipeIndex,
            mouseX, mouseY, glX, glY, fboX, fboY,
            vMulX, vMulY, vAddX, vAddY, dw, dh);
    } else {
        // Q1 lean: clear the debug-state struct on empty Shift+click so
        // a stale prior pick does not survive an empty-click gesture.
        RenderWorld::clearLastStaticPropPick();
        // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1; coord-diag
        // included to confirm viewport translation on this build.
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            std::fprintf(stderr,
                "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d) "
                "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
                mouseX, mouseY, glX, glY, fboX, fboY,
                vMulX, vMulY, vAddX, vAddY, dw, dh);
        }
    }
}
```

Replace with:

```cpp

//--------------------------------------------------------------------------------------

// M1.6 + M2-pre: env-gated static-prop pick. Thin caller wrapper around
// the M2-pre tryGameplayPick spine. Owns ONLY the category gate
// (IsStaticPropPickEnabled), the request build, and the outcome switch
// (which emits [STATIC_PROP_PICK v1] hit/miss with the M1.6 format
// preserved verbatim). All inline gate machinery + coord scaling +
// lookupAtPixel was migrated to code/gameplay_pick.cpp at M2-pre.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
// Sections 6 (caller pattern) + 9 (forbidden behaviors). M1.6 spec also
// remains the source of truth for Q6/Q8 invariants (4-site mover
// observable + legacy preservation).
void MissionInterfaceManager::tryStaticPropPick(bool moverSelectedThisFrame,
                                                bool shiftDn,
                                                bool leftClicked,
                                                bool bGui,
                                                bool bLeftDouble,
                                                int  mouseX,
                                                int  mouseY)
{
    // Category gate (static-prop wiring opt-in). The substrate gate
    // (IsObjectIdBufferEnabled) lives INSIDE tryGameplayPick; checking
    // it twice would be defense-in-depth but adds no protection (both
    // are cached bools). Caller checks ONLY the category-specific gate.
    if (!RenderWorld::IsStaticPropPickEnabled())
        return;

    GameplayPickRequest req{};
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // Update RenderWorld debug state. Single-slot; latest wins.
        RenderWorld::setLastStaticPropPick(r.lookup,
                                           r.ctx.mouseX, r.ctx.mouseY,
                                           r.ctx.glX,    r.ctx.glY);
        // Sample back the debug-state struct so the log can include the
        // recipeIndex (LookupResult itself does not carry it).
        const RenderWorld::StaticPropSelectionDebugState picked =
            RenderWorld::getLastStaticPropPick();
        // Unconditional hit log (spec Section 7); format byte-identical
        // to M1.6 HEAD db25d67. coord-diag fields included to confirm
        // viewport translation on this build.
        std::fprintf(stderr,
            "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
            "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
            r.lookup.handle.bits,
            (unsigned)r.lookup.handle.index(),
            (unsigned)r.lookup.handle.generation(),
            (int)picked.recipeIndex,
            r.ctx.mouseX, r.ctx.mouseY, r.ctx.glX, r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY, r.ctx.vAddX, r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight);
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Q1 lean: clear the debug-state struct on empty Shift+click so
        // a stale prior pick does not survive an empty-click gesture.
        RenderWorld::clearLastStaticPropPick();
        // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1.
        // Format byte-identical to M1.6 HEAD db25d67.
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            std::fprintf(stderr,
                "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d) "
                "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                "draw=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY, r.ctx.glX, r.ctx.glY,
                r.ctx.fboX,   r.ctx.fboY,
                r.ctx.vMulX,  r.ctx.vMulY, r.ctx.vAddX, r.ctx.vAddY,
                r.ctx.drawableWidth, r.ctx.drawableHeight);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // No-op. Legacy path consumed the click (gated) or the gesture
        // filter rejected it (skipped). Section 11 invariant: M1.6
        // emits ZERO log lines in either case.
        break;
    }
}
```

- [ ] **Step 5: Grep gate -- confirm inline machinery is GONE from missiongui.cpp**

Whole-file counts (informational; comments elsewhere in the file that mention these symbols WILL match and are not a failure):

```bash
grep -c "IsObjectIdBufferEnabled" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
grep -c "gos_GetViewport"         /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
grep -c "lookupAtPixel"           /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Body-restricted counts (LOAD-BEARING; restricts grep to inside the `MissionInterfaceManager::tryStaticPropPick` body only -- between the function open line and its closing brace at column 0):

```bash
awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | grep -c "IsObjectIdBufferEnabled"
awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | grep -c "gos_GetViewport"
awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | grep -c "lookupAtPixel"
```

Expected: each body-restricted count MUST be `0` post-refactor. Whole-file counts may be nonzero (other functions, comments elsewhere in the file may still mention the symbols; that is fine -- the gate is body-local, not file-global). If any body-restricted count is nonzero, the substitutive proof has failed -- the inline machinery survived alongside the helper call. STOP, revisit the Step 4 replace block.

- [ ] **Step 6: Grep gate -- confirm `tryGameplayPick` IS called from the refactored body**

```bash
grep -n "tryGameplayPick" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Expected: ONE hit at the refactored body inside `tryStaticPropPick`. If ZERO hits, the Step 4 edit did not land.

- [ ] **Step 7: LOC count gate -- confirm body shrank from ~80 lines to ~30 lines**

```bash
awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/' /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | wc -l
```

Expected: approximately 80 lines (function header + body + closing brace; the 30-line semantic body plus the multi-line printf calls expand the count). The pre-refactor body is also approximately 80 lines (the refactor REDUCES the SEMANTIC body from ~80 to ~30 lines by delegating to the helper; the surrounding printf calls and function frame keep the awk-measured total in the same ballpark). If the count is substantially larger than 80 (say, >100), inline machinery likely survived alongside the helper call; revisit. The load-bearing substitutive proof is the body-restricted grep gate in Step 5, NOT this LOC count.

- [ ] **Step 8: Full relink (additive helper call + body rewrite touches inline templates)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/code/missiongui.cpp.obj build64/RelWithDebInfo/code/gameplay_pick.cpp.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 9: Log-string byte-diff against M1.6 HEAD (spec Section 8 Gate 4)**

```bash
diff <(git show db25d67:code/missiongui.cpp \
        | grep 'STATIC_PROP_PICK v1') \
     <(git show HEAD:code/missiongui.cpp \
        | grep 'STATIC_PROP_PICK v1')
```

Expected: format-string-line differences only in printf-argument expressions (`res.handle.bits` -> `r.lookup.handle.bits`, `mouseX` -> `r.ctx.mouseX`, etc.). The literal string-content lines (`"[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u ..."`) MUST diff to NOTHING. If the literal format strings differ, byte parity has broken; revisit Step 4.

- [ ] **Step 10: Tier1 env-OFF smoke (pixel-parity at idle)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. `[STATIC_PROP_PICK v1] enabled=0 debug=0` banner emits once per mission init (unchanged). No `[STATIC_PROP_PICK v1] hit/miss` lines (env-OFF; no Shift+clicks driven by the smoke harness).

- [ ] **Step 11: Commit**

```bash
git add code/missiongui.cpp
git commit -m "$(cat <<'EOF'
refactor(missiongui): tryStaticPropPick delegates to tryGameplayPick (M2-pre T3)

Substitutive refactor: the inline gate machinery + viewport query +
coord scaling + lookupAtPixel call is GONE from missiongui.cpp. The
function body now contains ONLY:
  - category gate (IsStaticPropPickEnabled)
  - GameplayPickRequest build from the 7 args
  - tryGameplayPick(req) call
  - outcome switch (hit emits unconditional log + setLastStaticPropPick;
    miss emits verbose log + clearLastStaticPropPick; gated/skipped
    no-op per Section 11 invariant)

Body shrank from ~80 inline-machinery lines to ~30 caller-pattern
lines. Substitutive proof verified by grep: gos_GetViewport,
IsObjectIdBufferEnabled, lookupAtPixel ZERO hits inside the body
(they all moved to code/gameplay_pick.cpp at T1/T2).

Log format strings byte-identical to M1.6 HEAD db25d67 (verified by
git-show diff). [STATIC_PROP_PICK v1] hit/miss field order + printf
format specifiers + 11 diagnostic fields all preserved.

Signature unchanged. The 7-arg method declared at code/missiongui.h:272
is untouched. The 2 call sites at code/missiongui.cpp:1538 and :1781
take no diff. The 4 moverSelectedThisFrame=true writer sites at
:1476/1511/1740/1763 are untouched (spec Q7 RESOLVED keep-name).

Env-OFF tier1 5/5 PASS pixel-parity at idle vs M1.6 HEAD.

Spec: 2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md sec 6

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 4: Implement `RunGameplayPickSelfTest` + wire into `RenderWorld::init`

**Files:**
- Modify: `code/gameplay_pick.cpp` (replace `RunGameplayPickSelfTest` stub with 8-step assertion sequence)
- Modify: `RenderWorld/RenderWorld.cpp` (add ONE call to `RunGameplayPickSelfTest()` at the end of `init()`)

This task lands the load-bearing validation gate (spec Section 8 Gate 2). Gated by `MC2_GAMEPLAY_PICK_SELFTEST=1` AND `MC2_OBJECT_ID_BUFFER=1`. Emits `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` on success or `result=FAIL step=N expected=<...> actual=<...>` on first divergence.

- [ ] **Step 1: Re-grep the M1.5 substrate self-test for the canonical PASS/FAIL line format**

```bash
grep -n "RENDER_WORLD_SELFTEST\|runSubstrateSelfTest" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp | head -10
```

Expected: implementation at `:221-305`; invocation from `init()` at `:332`. Confirms the shape T4 mirrors.

- [ ] **Step 2: Replace the `RunGameplayPickSelfTest` stub in `code/gameplay_pick.cpp`**

Existing (verbatim, from T1):

```cpp
//----------------------------------------------------------------------
// RunGameplayPickSelfTest -- validation-gate self-test.
// Implementation lands in T4; T1 ships a STUB so the header symbol
// resolves cleanly. T4's body wires the 8-step assertion sequence.
//----------------------------------------------------------------------
void RunGameplayPickSelfTest()
{
    // T1 stub: no-op. T4 replaces with the env-flag gate + 8-step
    // assertion sequence per spec Section 8 Gate 2.
}
```

Replace with:

```cpp
//----------------------------------------------------------------------
// RunGameplayPickSelfTest -- validation-gate self-test.
//
// Gated by MC2_GAMEPLAY_PICK_SELFTEST=1 AND MC2_OBJECT_ID_BUFFER=1.
// Substrate-off makes the test vacuous (step 8 cannot reach
// lookupAtPixel); we refuse to run rather than emit a misleading PASS.
//
// 8-step assertion sequence per spec Section 8 Gate 2:
//   step=1 shiftDn=false                                  -> skipped
//   step=2 leftClicked=false                              -> skipped
//   step=3 bGui=true                                      -> skipped
//   step=4 bLeftDouble=true                               -> skipped
//   step=5 moverSelectedThisFrame=true (else clean)       -> gated
//   step=6 all gates clean + mouseX=-1                    -> skipped (off-screen)
//   step=7 all gates clean + mouseY=INT_MAX               -> skipped (off-screen)
//   step=8 all gates clean + center pixel (640,360)       -> miss
//
// Signal-reality note: RunGameplayPickSelfTest() runs at the tail of
// RenderWorld::init(), which executes BEFORE any scene frame has been
// rendered into the FBO. Attachment-2 (the R32_UINT object-id buffer)
// is freshly cleared to 0 at init; lookupAtPixel() will read raw=0
// and return LookupResult{isValid=false}. Step 8 therefore expects
// outcome=miss DETERMINISTICALLY (NOT hit-or-miss). This reduces step
// 8 from a "lookup correctness check" to a SPINE REACHABILITY check:
// it proves the gate chain (env -> gesture -> mover-fallback ->
// viewport -> bounds -> coord-scale) reaches lookupAtPixel without
// short-circuiting. Whether lookupAtPixel returns hit on a live scene
// is a substrate-level concern verified by the M1.5 OBJECT_ID_SELFTEST
// passive canary, NOT by this gate.
//
// Result lines mirror [RENDER_WORLD_SELFTEST v1]:
//   [GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
//   [GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
//
// FAIL is a STOP: indicates the extracted helper diverged from M1.6
// gate semantics.
//----------------------------------------------------------------------

// Local helper: env-flag check matching the RenderWorld envFlag() shape.
// (Not extracted to a shared utility -- the substrate-side envFlag() is
// in RenderWorld.cpp's anonymous namespace; reaching in would be ugly.
// The duplication is 4 lines; acceptable.)
static bool selftestEnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

// Local helper: build a "clean" request (gates pass; would reach
// lookupAtPixel) and let callers mutate one field per step.
static GameplayPickRequest selftestCleanRequest() {
    GameplayPickRequest req{};
    req.mouseX                  = 640;   // viewport-center on 1280x720 logical canvas
    req.mouseY                  = 360;
    req.shiftDn                 = true;
    req.leftClicked             = true;
    req.bGui                    = false;
    req.bLeftDouble             = false;
    req.moverSelectedThisFrame  = false;
    return req;
}

// Local helper: outcome enum -> stable string for FAIL line.
static const char* selftestOutcomeName(GameplayPickResult::Outcome o) {
    switch (o) {
    case GameplayPickResult::Outcome::skipped: return "skipped";
    case GameplayPickResult::Outcome::gated:   return "gated";
    case GameplayPickResult::Outcome::miss:    return "miss";
    case GameplayPickResult::Outcome::hit:     return "hit";
    }
    return "unknown";
}

void RunGameplayPickSelfTest()
{
    if (!selftestEnvFlag("MC2_GAMEPLAY_PICK_SELFTEST")) return;

    // Defense: substrate must be on for step 8 to be meaningful. If
    // substrate is off, every step 1-7 still asserts correctly, BUT
    // step 8 would assert skipped (which IS the expected outcome under
    // substrate-off) instead of hit/miss. To avoid the confusion of
    // "PASS step=all" meaning two different things depending on
    // substrate state, refuse to run with substrate off.
    if (!RenderWorld::IsObjectIdBufferEnabled()) {
        std::fprintf(stderr,
            "[GAMEPLAY_PICK_SELFTEST v1] result=SKIP reason=substrate_off\n");
        return;
    }

    using Outcome = GameplayPickResult::Outcome;

    // Step 1: shiftDn=false -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.shiftDn = false;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=1 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 2: leftClicked=false -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.leftClicked = false;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=2 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 3: bGui=true -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.bGui = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=3 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 4: bLeftDouble=true -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.bLeftDouble = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=4 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 5: moverSelectedThisFrame=true (else clean) -> gated
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.moverSelectedThisFrame = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::gated) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=5 expected=gated actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 6: clean gates + mouseX=-1 -> skipped (off-screen)
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.mouseX = -1;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=6 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 7: clean gates + mouseY huge -> skipped (off-screen)
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.mouseY = 100000;  // larger than any plausible vMulY
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=7 expected=skipped actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    // Step 8: clean gates + center pixel -> miss (DETERMINISTIC at init time)
    //
    // init() runs pre-frame; attachment-2 is freshly cleared to 0, so
    // lookupAtPixel returns isValid=false and tryGameplayPick reports
    // outcome=miss. The assertion is spine-reachability: NOT skipped
    // AND NOT gated. We accept outcome=hit as a PASS too (defensive --
    // if a future init-order change renders something into the FBO
    // before this self-test runs, hit is the correct outcome and we do
    // not want to flake), but the EXPECTED outcome is miss.
    {
        GameplayPickRequest req = selftestCleanRequest();
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::miss && r.outcome != Outcome::hit) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=8 expected=miss actual=%s\n",
                selftestOutcomeName(r.outcome));
            return;
        }
    }

    std::fprintf(stderr,
        "[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all\n");
}
```

- [ ] **Step 3: Add the `RunGameplayPickSelfTest()` call into `RenderWorld::init`**

Existing (verbatim, `RenderWorld/RenderWorld.cpp:311-333`):

```cpp
void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    const bool oid = IsObjectIdBufferEnabled();
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        std::fprintf(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
    // M1.6: pick-wiring banner. Always emitted (both 0/0 and 1/1 states
    // useful to log readers diagnosing "why did Shift+click do nothing").
    std::fprintf(stderr, "[STATIC_PROP_PICK v1] enabled=%d debug=%d\n",
                 IsStaticPropPickEnabled() ? 1 : 0,
                 IsStaticPropPickDebugEnabled() ? 1 : 0);
    // M1.5 T10: substrate self-test (gated by MC2_RENDER_WORLD_SELFTEST=1).
    runSubstrateSelfTest();
}
```

Replace with:

```cpp
void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    const bool oid = IsObjectIdBufferEnabled();
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        std::fprintf(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
    // M1.6: pick-wiring banner. Always emitted (both 0/0 and 1/1 states
    // useful to log readers diagnosing "why did Shift+click do nothing").
    std::fprintf(stderr, "[STATIC_PROP_PICK v1] enabled=%d debug=%d\n",
                 IsStaticPropPickEnabled() ? 1 : 0,
                 IsStaticPropPickDebugEnabled() ? 1 : 0);
    // M1.5 T10: substrate self-test (gated by MC2_RENDER_WORLD_SELFTEST=1).
    runSubstrateSelfTest();
    // M2-pre: gameplay-pick self-test (gated by MC2_GAMEPLAY_PICK_SELFTEST=1
    // + MC2_OBJECT_ID_BUFFER=1). Validates the extracted spine has not
    // diverged from M1.6 gate semantics. Mirrors substrate self-test shape.
    RunGameplayPickSelfTest();
}
```

Note: `RunGameplayPickSelfTest` is declared in `code/gameplay_pick.h`, NOT in any RenderWorld header. The cleanest way to make the symbol visible at the call site in `RenderWorld/RenderWorld.cpp` is to forward-declare it at the top of the file (next to the existing extern declarations) OR include `../code/gameplay_pick.h`. The forward-declaration is preferred (smaller include footprint):

Add near the top of `RenderWorld/RenderWorld.cpp` (after the existing `#include` block; verify exact insertion point at write time):

```cpp
// M2-pre: forward-declare the gameplay-pick self-test entry point.
// Lives in code/gameplay_pick.h; full include would drag the game-side
// types into a substrate-side TU unnecessarily. Symbol is resolved by
// the linker at full-relink.
void RunGameplayPickSelfTest();
```

Firewall direction note: `RenderWorld::init()` (engine-side, `RenderWorld/RenderWorld.cpp`) will FORWARD-DECLARE `RunGameplayPickSelfTest()` from `code/gameplay_pick.cpp` (game-side, `code/`). This is the first engine-side -> game-side reach in the codebase (previously only game-side -> engine-side via the `code/missiongui.cpp:33` reach-in pattern). It works because: (1) forward-decl avoids including `code/gameplay_pick.h` from `RenderWorld.cpp`, so no header reach crosses the boundary; (2) the linker resolves the symbol at link time; (3) `scripts/check-include-firewall.sh:22` SCOPE_DIRS excludes `code/` AND the script does NOT enforce reach direction (only forbidden-headers + forbidden-symbols within scoped engine-side TUs). Document as a known new pattern. Future M3+ slices may invert again (game-side -> engine-side, or engine-side reaching into another game-side helper); if so, this is the precedent. If the firewall script is ever extended to enforce direction, the M2-pre self-test wire-up will need an explicit allowlist entry.

- [ ] **Step 4: Full relink (init() body change + new TU symbol cross-reference)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/RenderWorld/RenderWorld.cpp.obj build64/RelWithDebInfo/code/gameplay_pick.cpp.obj
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. Linker resolves `RunGameplayPickSelfTest` at the call site in `RenderWorld.cpp` via the new TU.

- [ ] **Step 5: Verify env-OFF tier1 smoke (self-test does NOT run; gate off)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. `grep '\[GAMEPLAY_PICK_SELFTEST v1\]' tests/smoke/artifacts/<latest>/*.log` returns ZERO matches (env var not set; self-test early-returns silently).

- [ ] **Step 6: Verify self-test env-ON (Phase B gate; spec Section 8 Gate 2)**

```bash
MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1 \
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py \
    --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Each mission's log shows `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` exactly ONCE (emitted at init() before the mission render loop starts). If any mission emits `result=FAIL step=N`, STOP -- the extracted spine has diverged from M1.6 gate semantics; revisit T2 algorithm.

```bash
grep -c "GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all" tests/smoke/artifacts/<latest>/*.log
```

Expected: 5 (one per mission). If 0 or fewer than 5, the gate is off or the self-test silently early-returned; check `MC2_OBJECT_ID_BUFFER=1` did propagate (env-var inheritance from the smoke-runner env is the most likely failure mode).

- [ ] **Step 7: Commit**

```bash
git add code/gameplay_pick.cpp RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(gameplay_pick): RunGameplayPickSelfTest + RenderWorld::init wire (M2-pre T4)

Validation-gate self-test mirroring M1.5 [RENDER_WORLD_SELFTEST v1].
Gated by MC2_GAMEPLAY_PICK_SELFTEST=1 + MC2_OBJECT_ID_BUFFER=1.
Substrate-off prints result=SKIP reason=substrate_off (rather than
a misleading PASS).

8-step assertion sequence per spec Section 8 Gate 2:
  1. shiftDn=false              -> skipped
  2. leftClicked=false          -> skipped
  3. bGui=true                  -> skipped
  4. bLeftDouble=true           -> skipped
  5. moverSelectedThisFrame=true -> gated
  6. mouseX=-1                  -> skipped (off-screen)
  7. mouseY=100000              -> skipped (off-screen)
  8. clean gates + (640,360)    -> hit OR miss (NOT skipped/gated)

Emits to stderr:
  [GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
  [GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
  [GAMEPLAY_PICK_SELFTEST v1] result=SKIP reason=substrate_off

FAIL is a STOP: indicates the extracted spine diverged from M1.6 gate
semantics. The user-driven Shift+click canary on mc2_03 remains as a
SUPPLEMENTARY visual check, but the self-test is the load-bearing
substitutive proof (reproducible without a human in the loop).

Wired into RenderWorld::init() AFTER runSubstrateSelfTest()
(RenderWorld/RenderWorld.cpp:332). Forward-declared at top of
RenderWorld.cpp to avoid dragging game-side types into the substrate
TU.

Env-OFF tier1 5/5 PASS (gate off; ZERO selftest log lines). Env-ON
tier1 5/5 PASS step=all on first launch.

Spec: 2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md sec 8 Gate 2

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase C -- Validation, greybeard, doc

**Phase C goal:** validate the substitutive refactor against the three load-bearing gates (env-OFF parity, env-ON self-test, visual canary), dispatch the greybeard skill for the META-FIX-vs-PATCH ruling (fresh, no pre-judged verdict), and document the slice in worktree CLAUDE.md Active Campaigns.

### Task 5: Validation gates (no commit; smoke runs only)

**Files:** none modified; smoke artifacts captured into `tests/smoke/artifacts/<timestamp>/`.

This task runs the three load-bearing gates from spec Section 8. NO commits land here; the gates are verification of the T1-T4 commits. If any gate fails, STOP and escalate (revisit the prior task's edits).

- [ ] **Gate 1: env-OFF tier1 5/5 pixel-parity at idle**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected:
- tier1 5/5 PASS.
- `grep -c "STATIC_PROP_PICK v1\] hit\|STATIC_PROP_PICK v1\] miss" tests/smoke/artifacts/<latest>/*.log` returns 0 (env-OFF default; no Shift+clicks driven by smoke harness; no pick log lines).
- `grep -c "STATIC_PROP_PICK v1\] enabled=0 debug=0" tests/smoke/artifacts/<latest>/*.log` returns 5 (one banner per mission).
- `grep -c "GAMEPLAY_PICK_SELFTEST v1" tests/smoke/artifacts/<latest>/*.log` returns 0 (gate off).
- FPS p1% delta vs M1.6 HEAD `db25d67` baseline: <1 fps. (Helper-call cost short-circuits at the category gate; no per-frame work added.)

Failure modes:
- Tier1 mission crash: T3 refactor introduced a regression. Diff the body against M1.6 HEAD format strings.
- FPS delta >1 fps: unexpected; helper-call overhead should be negligible. Investigate before proceeding.

- [ ] **Gate 2: MC2_GAMEPLAY_PICK_SELFTEST=1 + MC2_OBJECT_ID_BUFFER=1 -> result=PASS step=all**

```bash
MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1 \
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py \
    --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected:
- tier1 5/5 PASS.
- `grep -c "GAMEPLAY_PICK_SELFTEST v1\] result=PASS step=all" tests/smoke/artifacts/<latest>/*.log` returns exactly 5 (one PASS per mission).
- ZERO `result=FAIL` lines.
- ZERO `result=SKIP` lines (substrate is on; self-test should not skip).

Failure modes:
- `result=FAIL step=N`: extracted spine diverged from M1.6 gate semantics. T2 algorithm bug. Inspect `expected=<...> actual=<...>` in the log to identify which gate misfired.
- `result=SKIP reason=substrate_off`: `MC2_OBJECT_ID_BUFFER=1` did not propagate to the smoke harness's child process. Verify env-var inheritance.
- 0 matches: `MC2_GAMEPLAY_PICK_SELFTEST=1` did not propagate. Same diagnostic as above.

- [ ] **Gate 3: env-ON visual canary on mc2_03 (supplementary; NOT load-bearing)**

User-driven; not script-automated. Launch with both env vars on:

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_STATIC_PROP_PICK=1 \
A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
```

(Deploy first if needed; verify the build64 exe has been copied to the deploy dir.)

Once in mission mc2_03:
1. Shift+left-click on a static prop (building, tree).
2. Observe stderr (or log file) for `[STATIC_PROP_PICK v1] hit handle=N idx=N gen=N recipe=N screen=(x,y) gl=(x,y) fbo=(x,y) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(x,y)`.
3. Compare field-for-field against M1.6 HEAD `db25d67`'s log shape. Same 11 fields, same printf format specifiers, same field order.

Expected: log-line shape byte-identical to M1.6. Field values will differ between runs (camera state drifts), but the SHAPE check (field count, field order, type sanity) holds.

If the log line differs in shape (e.g. missing a field, extra field, different specifier order), T3's printf rewrite has byte-drifted from M1.6 HEAD; revisit Step 4 of T3.

Spot-check is sufficient; this is NOT a full reproduction of M1.6's 26-hit + 11-miss user canary.

- [ ] **No commit for this task.** Validation gates are verification; the commits are T1-T4 above.

### Task 6: Greybeard META-FIX vs PATCH ruling (fresh subagent; no pre-judged verdict)

**Files:** `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md` (NEW; written by the greybeard subagent).

This task dispatches the greybeard skill against the M2-pre slice in its CURRENT shipped state (T1+T2+T3+T4 committed). The spec Section 11 LEANS META-FIX (substitutive: inline machinery REPLACED in `tryStaticPropPick`; named M2.6 trigger condition satisfied), but the greybeard subagent rules FRESH against the sample-size-of-one counter-argument. Both outcomes are acceptable; the plan records whichever ruling fires.

- [ ] **Step 1: Dispatch the greybeard skill subagent**

Spawn a fresh subagent with NO pre-loaded verdict. Include the verbatim phrase "run the greybeard skill" in the dispatch prompt. The subagent's task:

1. Read the spec, the M1.6 greybeard ruling that triggered M2-pre (`docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-greybeard-ruling.md`), the M2-pre HEAD diff (T1+T2+T3+T4 commits), and the relevant memory files (`memory/feedback_offload_must_be_substitutive_not_additive.md`).
2. Verify substitutive criterion by grep: confirm `gos_GetViewport`, `IsObjectIdBufferEnabled`, `lookupAtPixel` have ZERO hits inside the refactored `tryStaticPropPick` body (per T3 Step 5 gate). If grep finds inline machinery surviving alongside the helper call, the substitutive proof has failed -> PATCH ruling.
3. Rule META-FIX vs PATCH (justified):
   - **META-FIX criteria:** inline machinery REPLACED (not duplicated); central spine in place ahead of named M2.6 trigger; category-agnostic shape that M2.6 will consume unchanged.
   - **PATCH (justified) criteria:** sample-size-of-one extraction; one current caller; defer META-FIX claim until M2.6 actually wires the second caller; M2-pre justified as preparatory refactor with NAMED debt-payment intent.
4. Write the ruling to `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md` matching the format of `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-greybeard-ruling.md`.

- [ ] **Step 2: Confirm the ruling file landed**

```bash
ls -la /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md
```

Expected: file exists; content matches the M1.6 ruling shape (verdict + criteria + grep evidence + signature).

- [ ] **Step 3: Capture ruling verdict for T7 doc**

Read the ruling file's verdict line (META-FIX or PATCH-justified). T7's CLAUDE.md update cites this verdict.

- [ ] **No commit for this task as a standalone unit.** The ruling-file commit is folded into T7 below (single commit lands the ruling file + the CLAUDE.md update, mirroring M1.6's pattern).

### Task 7: Document in worktree CLAUDE.md Active Campaigns + commit greybeard ruling

**Files:**
- Modify: `.claude/worktrees/nifty-mendeleev/CLAUDE.md` (add Active Campaigns bullet)
- Add: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md` (from T6)

- [ ] **Step 1: Re-grep the CLAUDE.md Active campaigns section**

```bash
grep -n "Active campaigns\|M1.6\|M2-pre" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md | head -10
```

Expected: `Active campaigns` heading. Find the last bullet (M1.6 SHIPPED 2026-05-23) and insert the M2-pre bullet immediately after.

- [ ] **Step 2: Add the M2-pre bullet to CLAUDE.md Active Campaigns**

Existing (verbatim, the M1.6 bullet line; abbreviated -- preserve full content):

```markdown
- **RenderWorld Slice M1.6** (SHIPPED 2026-05-23): static-prop pick wiring on top of the M1.5 substrate. ...
```

Add after the M1.6 bullet (preserve the M1.6 bullet verbatim; append the new bullet):

```markdown
- **RenderWorld Slice M2-pre** (SHIPPED 2026-05-23): preemptive META-FIX
  extraction of `tryGameplayPick(req) -> result` + `screenToFboPixel` from
  `MissionInterfaceManager::tryStaticPropPick` into new
  `code/gameplay_pick.{h,cpp}`. `tryStaticPropPick` now delegates: category
  gate -> request build -> `tryGameplayPick` call -> outcome switch.
  `[STATIC_PROP_PICK v1]` log format byte-identical to M1.6 HEAD `db25d67`.
  Validation gate: `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` (8-step
  assertion sequence at `RenderWorld::init()` under `MC2_GAMEPLAY_PICK_SELFTEST=1`
  + `MC2_OBJECT_ID_BUFFER=1`). Tier1 5/5 PASS env-OFF (pixel-parity vs M1.6
  HEAD) + visual canary on mc2_03 PASS. Greybeard ruling 2026-05-23:
  <META-FIX | PATCH (justified)> per T6 ruling file. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`.
  Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md`.
```

(Replace `<META-FIX | PATCH (justified)>` with the actual T6 verdict before commit.)

- [ ] **Step 3: Verify the bullet was added and CLAUDE.md is under the 200-line cap**

```bash
wc -l /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
grep -c "RenderWorld Slice M" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
```

Expected:
- Line count: <= 200 (worktree CLAUDE.md discipline). If exceeded, extract older content to memory and link.
- 4 `RenderWorld Slice M` matches (M1 + M1.5 + M1.6 + M2-pre).

- [ ] **Step 4: Run the root CLAUDE.md pointer check (defense; root file is not touched by this slice)**

```bash
sh /a/Games/mc2-opengl-src/scripts/check-claude-md-pointer.sh
```

Expected: exit 0. The root pointer is unchanged.

- [ ] **Step 5: Commit (CLAUDE.md update + greybeard ruling file, single commit)**

```bash
git add .claude/worktrees/nifty-mendeleev/CLAUDE.md docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md
git commit -m "$(cat <<'EOF'
docs(M2-pre): worktree CLAUDE.md + greybeard ruling (M2-pre T7)

Active campaigns bullet for M2-pre shipped state. The slice extracts
tryGameplayPick + screenToFboPixel from MissionInterfaceManager::
tryStaticPropPick into new code/gameplay_pick.{h,cpp}, refactoring
the M1.6 body from ~80 lines of inline machinery to ~30 lines of
caller pattern while preserving [STATIC_PROP_PICK v1] hit/miss log
byte-identically. Validation-gate self-test
[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all confirms gate
semantics match M1.6 across 8 synthetic input scenarios.

Greybeard ruling 2026-05-23: <verdict>. See ruling file for full
substitutive-criterion grep evidence and the META-FIX-vs-PATCH
discussion. Per spec Section 11, the ruling considers the
sample-size-of-one counter-argument (one current caller) against
the named M2.6 trigger condition (second caller planned within the
v0.4 milestone).

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md
Ruling: docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Post-execution checklist

After T1-T7 land, confirm the following before declaring M2-pre SHIPPED:

- [ ] **5 commits land in order:** T1 (introduce TU + screenToFboPixel + CMake), T2 (implement tryGameplayPick), T3 (refactor tryStaticPropPick), T4 (RunGameplayPickSelfTest + RenderWorld::init wire), T7 (CLAUDE.md + greybeard ruling). T5 is validation runs (no commit); T6 is subagent dispatch (ruling-file commit folded into T7).
- [ ] **Tier1 5/5 PASS env-OFF** with no `[STATIC_PROP_PICK v1] hit/miss` or `[GAMEPLAY_PICK_SELFTEST v1]` lines.
- [ ] **Tier1 5/5 PASS env-ON** (`MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1`) with 5 `result=PASS step=all` lines.
- [ ] **Visual canary on mc2_03:** Shift+click building emits `[STATIC_PROP_PICK v1] hit` line with same 11-field diagnostic format as M1.6 HEAD.
- [ ] **`tryStaticPropPick` body LOC reduced by ~50%** (from ~80 to ~30 semantic lines; substitutive proof).
- [ ] **Inline machinery (env check, gesture gate, coord scaling, lookupAtPixel call) NOT present** in `code/missiongui.cpp` post-refactor (substitutive grep: `gos_GetViewport`, `IsObjectIdBufferEnabled`, `lookupAtPixel` zero hits inside `tryStaticPropPick` body).
- [ ] **Greybeard ruling recorded** at `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-pre-greybeard-ruling.md`.
- [ ] **CLAUDE.md Active campaigns updated** with the M2-pre bullet; file under 200 lines.
- [ ] **Root CLAUDE.md pointer unchanged** (`scripts/check-claude-md-pointer.sh` exit 0).
- [ ] **Firewall script unaffected** (no edit to `scripts/check-include-firewall.sh`; `code/` is outside SCOPE_DIRS).

---

## Risks (R1-R4)

- **R1. Self-test step 8 hardcoded pixel may not have a static prop rendered at it on every tier1 mission.** (Mitigated by signal-reality: the test runs at init() pre-frame; attachment-2 is cleared to 0, so outcome=miss is deterministic regardless of what the live scene will render.)
  Mitigation: Step 8 asserts outcome=miss (DETERMINISTIC at init-time; attachment-2 is freshly cleared to 0 so `lookupAtPixel` returns isValid=false) with outcome=hit accepted defensively. The check is spine-reachability, not lookup correctness. `skipped` or `gated` at step 8 IS a FAIL (indicates the spine short-circuited before reaching `lookupAtPixel`, which is the failure mode the test catches). Lookup correctness on live scenes is verified by the M1.5 `[OBJECT_ID_SELFTEST v1]` passive canary, not by this gate.

- **R2. Header file dependency (`code/gameplay_pick.h` includes `RenderWorld/RenderWorld.h`).**
  Mitigation: `code/missiongui.cpp:33` already does `#include "../RenderWorld/RenderWorld.h"` with no firewall complaint; the new TU follows the same reach-in pattern. Spec adversarial review M-2 verified `code/` is outside SCOPE_DIRS at `scripts/check-include-firewall.sh:22`. No firewall edit needed.

- **R3. Coord-translation behavior must EXACTLY match M1.6 commit `1853ad9`.**
  Mitigation: T1's `screenToFboPixel` body is copied VERBATIM from `code/missiongui.cpp:6217-6233` (the inline block). The plan shows the full M1.6 inline body in T3's "Existing" block so the reviewer can diff line-by-line. T2's `tryGameplayPick` algorithm preserves the M1.6 gate order (env -> gesture -> mover-fallback -> viewport -> bounds -> scale -> lookup). Gate 2 (self-test 8-step) catches any gate-order divergence reproducibly.

- **R4. CMakeLists glob/list integration.**
  Mitigation: CMakeLists is an EXPLICIT list (no glob) per write-time grep -- T1 Step 3 verifies this. T1 Step 6 adds the explicit entry. If T1 forgets, the linker complains at T2 build time (undefined references to `tryGameplayPick`); the failure is loud and easy to diagnose.

---

## Critical considerations

- **CMakeLists.txt update is non-optional.** Confirmed via grep that the source list at root `CMakeLists.txt:212` is explicit (no globbing). T1 MUST add `"code/gameplay_pick.cpp"`; T1 Step 7 build will fail-loud if forgotten.
- **Header include from missiongui.cpp.** T3 adds `#include "gameplay_pick.h"` to `missiongui.cpp`. Firewall script unaffected (verified by T1 Step 2 grep).
- **Behavior preservation.** T3 is the load-bearing step. Spec promises `tryStaticPropPick` remains semantically identical post-refactor. Plan shows full "Existing" + "Replace with" blocks for the body so the reviewer can diff line-by-line. Log format strings byte-identical (T3 Step 9 diff gate).
- **Self-test pixel choice (T4 step 8).** The self-test runs at `RenderWorld::init()` pre-frame; attachment-2 is freshly cleared to 0, so outcome=miss is DETERMINISTIC regardless of what the live scene would render at `(640, 360)`. The assertion is spine-reachability (NOT `skipped` AND NOT `gated`); outcome=hit is accepted defensively for future-proofing against init-order changes that may put rendered content into the FBO before this test runs.
- **Tier1 timing baseline.** Env-OFF post-refactor tier1 FPS should match pre-refactor (5/5 PASS expected; <1 fps p1% delta acceptable). T5 Gate 1.
- **Greybeard ruling is FRESH.** T6 dispatches a subagent with NO pre-loaded verdict. Spec Section 11 LEANS META-FIX but does not predetermine the outcome. Both verdicts are acceptable; T7 records whichever fires.

---

## Pre-execution gates (must hold before T1 starts)

1. **Firewall script clean** (no SCOPE_DIRS edit needed; verify by running `sh scripts/check-include-firewall.sh` -- expect exit 0).
2. **Tier1 5/5 env-OFF pixel-parity at idle vs M1.6 HEAD `db25d67`** (capture baseline tier1 artifact for post-T3 diff if any FPS regression suspected).
3. **HEAD line offsets refreshed:** T1 Step 1 grep confirms `:1476/1511/1740/1763` (4 writers), `:1538/1781` (2 call sites), `:6168-6273` (body). If offsets have drifted, plan body must be amended in-place before T1 Step 4 executes.
4. **Spec EXECUTABLE-READY status confirmed:** spec header reads `Status: EXECUTABLE-READY`. If status has reverted to DRAFT, escalate and abort.
5. **M1.6 SHIPPED at HEAD:** `git log --oneline | grep db25d67` returns one match.
6. **CLAUDE.md M1.6 bullet present:** `grep -c "RenderWorld Slice M1.6" .claude/worktrees/nifty-mendeleev/CLAUDE.md` returns >=1.

If any pre-execution gate fails, STOP and escalate. Do NOT proceed with task execution.

---

## Critical constraints

- **ASCII only, no emoji.** Per worktree CLAUDE.md no-emoji-in-files rule.
- **TAB indentation in `code/missiongui.cpp`** matching existing style (top-level functions use TABs). The `tryStaticPropPick` body M1.6 shipped is an exception (4-space); T3's replacement preserves the 4-space convention for the body.
- **4-space indent in new file `code/gameplay_pick.{h,cpp}`** (matches the helper-body convention; new file is a clean unit).
- **Use Edit tool for modifications; Write tool only for new file creation.** T1 Steps 4-5 use Write; all subsequent edits use Edit.
- **Do NOT modify:** mech rendering, terrain rendering, attachment-2 shader output, env vars (beyond the new `MC2_GAMEPLAY_PICK_SELFTEST`).
- **Do NOT switch branches.** All work on `terrain-pbr-mod` worktree branch.
- **Do NOT add gameplay verbs.** No order issuance, no `Team::home` mutation, no save-state. Same constraint M1.6 inherited.
- **Do NOT edit shader code.** Spec Section 9 "No shader work" non-goal.
- **Do NOT extend the substrate API.** `RenderWorld::lookupAtPixel`, `setLastStaticPropPick`, `clearLastStaticPropPick`, `getLastStaticPropPick`, `IsObjectIdBufferEnabled`, `IsStaticPropPickEnabled`, `IsStaticPropPickDebugEnabled` are all untouched.
- **Do NOT touch the 4 `moverSelectedThisFrame` writer sites.** Spec Q7 RESOLVED keep-name; M2-pre is a refactor of the helper body, not the gate-set sites.

---

## Operational notes for the executing agent

- After T1 Step 4 (Write new files), check that the file landed at `code/gameplay_pick.h` and `code/gameplay_pick.cpp` (NOT under `RenderCore/` or `RenderWorld/` -- spec Q1 RESOLVED Candidate A places them under `code/`).
- T3 Step 4 ("Replace with") is the longest single Edit in the plan. The 80-line Existing block and the 75-line Replace block must be applied in ONE Edit call (not split), so the file does not pass through a half-refactored intermediate state where some inline code survives.
- If T4 Step 6 (self-test env-ON) returns 0 PASS lines despite both env vars set, the most likely cause is `MC2_OBJECT_ID_BUFFER=1` not propagating to the smoke-runner's child process. Verify by adding a trace `[GAMEPLAY_PICK_SELFTEST v1] gate_check obj=%d gate=%d` line at the top of `RunGameplayPickSelfTest` before the env-flag check, then re-run.
- If T7 CLAUDE.md grows past 200 lines after adding the M2-pre bullet, follow the worktree CLAUDE.md "Memory & CLAUDE.md Discipline" rule: extract older content to memory and link.
- Greybeard ruling (T6) is the only step that requires a subagent dispatch. Use the canonical phrase "run the greybeard skill" verbatim per worktree CLAUDE.md "Meta-fix discipline" rule.

---

End of M2-pre executable plan.
