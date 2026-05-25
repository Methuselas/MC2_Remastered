# RenderWorld Slice M1.6 -- Static-Prop Pick Integration Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the M1.5 ObjectID substrate as a user-visible, env-gated, modifier-only, **mover-second-fallback** click gesture in `code/missiongui.cpp`. With `MC2_OBJECT_ID_BUFFER=1` AND `MC2_STATIC_PROP_PICK=1`, a Shift+left-click on a static prop emits `[STATIC_PROP_PICK v1] hit ...` to stderr and updates a small RenderWorld-side `StaticPropSelectionDebugState`. Legacy plain LMB selection is untouched; legacy Shift+LMB additive-select on a friendly mover is preserved verbatim (no M1.6 log line in that case). No gameplay verbs: no order issuance, no `Team::home` mutation, no save-state. Env-OFF default: tier1 5/5 pixel-parity at idle vs M1.5 HEAD `842f34f` (helper adds ~1 CALL/RET per click event, short-circuited at the env-OFF check; rendering output and selection state at idle frames are identical).

**Architecture:** No new modules. Two existing surfaces extended.
(1) `RenderWorld/RenderWorld.{h,cpp}` gains a `StaticPropSelectionDebugState` POD, three free functions (`setLastStaticPropPick` / `clearLastStaticPropPick` / `getLastStaticPropPick`), one new env-flag accessor (`IsStaticPropPickDebugEnabled()` for `MC2_STATIC_PROP_PICK_DEBUG`), and one wiring call into the existing per-mission `destroy()` lifecycle hook.
(2) `code/missiongui.cpp` gains one new private helper `MissionInterfaceManager::tryStaticPropPick`, one new env-flag accessor for `MC2_STATIC_PROP_PICK`, four single-line `moverSelectedThisFrame=true` instrumentations adjacent to the four `setSelected(true)` writer sites identified by spec Q6, and one helper call at the tail of each of `updateOldStyle` + `updateAOEStyle`.

**Tech Stack:** C++17, MSVC `--config RelWithDebInfo`, CMake (no new targets), existing `[STATIC_PROP_PICK v1]` schema (introduced by this slice), tier1 smoke harness, env vars `MC2_STATIC_PROP_PICK` and `MC2_STATIC_PROP_PICK_DEBUG` cached at startup via the same `envFlag()` pattern M1.5 already uses. Y-flip at the call site: `glY = Environment.screenHeight - 1 - mouseY` (Win32 -> GL convention, mirrors `mclib/mouse.cpp:225` / `mclib/utilities.cpp:111-115`).

**Spec:** `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md` (EXECUTABLE-READY).
**Adversarial review applied (spec):** `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-spec-adversarial-review.md` (1 CRIT / 2 MAJOR / 6 MINOR; all resolved 2026-05-23 in the spec; Q6 4-site map and Q8 Section 11 invariant baked in).
**Adversarial review applied (plan):** `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-plan-adversarial-review.md` (1 CRIT / 3 MAJOR / 5 MINOR; applied 2026-05-23):
- C1: `handleIndexToRecipeIndex` -> `handleToRecipeIndex` (mechanical rename; correct symbol takes full `RenderObjectHandle`)
- M1: Tasks 4 + 5 collapsed into a single atomic NEW Task 4 (per user); subsequent tasks renumbered (NEW T5 = validation, NEW T6 = greybeard, NEW T7 = CLAUDE.md doc)
- M2: `mouseX` / `mouseY` origin documented at the call site (class statics, polled pre-dispatch at `:719-720`)
- M3: "byte-identical" wording relaxed to "pixel-parity at idle" for env-OFF tier1 invariant
- Minors: env-var names RESOLVED (spec Sec 9 sanctions plan-stage rename); `bLeftDouble` recompute grep-verified stateless; `Environment.screenHeight` verified at `GameOS/include/gameos.hpp:3379`.

**Predecessor slice:** M1.5 SHIPPED 2026-05-23 (`842f34f`). Substrate inspectable via `RenderWorld::lookupAtPixel(screenX, screenY) -> LookupResult` with generation + alive validation against `s_objectRecords`.

---

## Decisions resolved before execution (from spec + adversarial review 2026-05-23)

All decisions below are baked into the spec at Sections 1, 4, 9, 11 and at
Open-Question resolutions Q1/Q2/Q4/Q6/Q8. Do NOT relitigate during
execution. If a decision appears wrong during a task, STOP, escalate to
the user, and amend the spec before proceeding.

```
M1.6 decisions resolved before execution:
  Action: inspect-only (log + StaticPropSelectionDebugState write; NO gameplay verbs).
  Priority: mover-first; GPU pickup is fallback when no mover hit this frame.
  Input: Shift + left-click ONLY (plain LMB stays legacy).
  Env default: MC2_OBJECT_ID_BUFFER=1 required; MC2_STATIC_PROP_PICK=1 master enable;
               MC2_STATIC_PROP_PICK_DEBUG=1 optional miss-log verbosity.
  Q1 lean: clearLastStaticPropPick() on empty Shift+click (semantic cleanliness).
  Q2 lean: pick state does NOT persist across mission load -- clearLastStaticPropPick()
           wired into RenderWorld::destroy() (the no-arg mission-scoped overload at :321).
  Q3 lean: highlight overlay DEFERRED to M1.7 (log + struct only in M1.6).
  Q4 lean: ONE shared helper MissionInterfaceManager::tryStaticPropPick(...) called
           from the tail of BOTH updateOldStyle and updateAOEStyle.
  Q6 RESOLVED: 4 setSelected(true) writer sites instrumented with moverSelectedThisFrame:
    code/missiongui.cpp:1460  -- updateOldStyle Shift+additive setSelected(true)
    code/missiongui.cpp:1487  -- updateOldStyle plain-LMB-select setSelected(true)
    code/missiongui.cpp:1690  -- updateAOEStyle Shift+additive setSelected(true)
    code/missiongui.cpp:1705  -- updateAOEStyle plain-LMB-select setSelected(true)
    The set-to-false sites at :1462 / :1483 / :1692 / :1701 MUST NOT set
    moverSelectedThisFrame=true (toggle-off / clear-others gestures).
  Q8 RESOLVED: Shift+LMB on a friendly mover MUST continue legacy additive-select.
    Section 11 invariant: M1.6 emits ZERO [STATIC_PROP_PICK v1] log lines when
    moverSelectedThisFrame == true at tryStaticPropPick entry.
  Y-flip: glY = Environment.screenHeight - 1 - mouseY at the call site (NOT inside
    lookupAtPixel; M1.5 contract is "GL convention coords in").
```

Plan-level implications:

- The helper definition, its declaration, the four `setSelected(true)` instrumentation points, and the two helper-call sites must land in a **single atomic commit** (Task 4 per plan adversarial review M1 collapse). Splitting "helper defined but not yet called" from "helper called from style bodies" would leave a dangling unused private method between commits AND risk a promoted-warning red HEAD under MSVC `/WX`; splitting the 4 sets from the helper call would misfire the fallback gate in the in-between commit. Both halves ship together.
- The `MC2_STATIC_PROP_PICK` env-flag accessor lives on the RenderWorld side (Task 2 declares it adjacent to `IsObjectIdBufferEnabled()` in `RenderWorld.h`) so the missiongui caller reaches into RenderWorld for ONE include only -- no new include-firewall exemption needed; M1's `scripts/check-include-firewall.sh` already permits `code/` to include `RenderWorld/RenderWorld.h`.
- The helper takes the per-frame observable `moverSelectedThisFrame` as a `bool` parameter rather than reading hidden global state. This keeps the helper testable-in-isolation and makes the short-circuit explicit at the call sites.
- `clearLastStaticPropPick()` is wired into the no-arg `RenderWorld::destroy()` (file:line `RenderWorld/RenderWorld.cpp:321`), NOT the per-handle `destroy(Handle h)` overload at `:380`. M1 owns the per-mission lifecycle hook from `code/mission.cpp:3279`; that hook calls the no-arg overload exactly once per mission end.

---

## Open items surfaced for user sign-off BEFORE Task 1 executes

These are NOT decisions to make during execution. They are operational
choices the plan author flags so the user can rule (or accept the
plan's lean) before Task 1.

### O1. Env-flag final name (RESOLVED 2026-05-23 by plan adversarial review m1)

The plan adopts `MC2_STATIC_PROP_PICK` (master) and
`MC2_STATIC_PROP_PICK_DEBUG` (verbose miss-log). Spec Section 9
explicitly sanctions the plan-stage rename: "Plan stage may rename to
e.g. `MC2_STATIC_PROP_PICK=1` for brevity. Whatever the final name, it
must (a) start with `MC2_`, (b) be a single boolean, (c) be read once at
startup and cached." Both names satisfy all three. The plan
adversarial reviewer leaned toward the brevity choice. No further user
sign-off required.

### O2. `StaticPropSelectionDebugState` field set

The plan ships the full spec Section 6 shape verbatim:
`valid`, `handle`, `recipeIndex`, `lastPickMouseX/Y`, `lastPickGlX/Y`,
`lastPickFrameIndex`. The `lastPickFrameIndex` field is filled from
the existing `s_frameCounter` atomic in `RenderWorld.cpp` (M1 wiring)
so no new frame-counter plumbing is needed.

**Lean:** ship full spec shape (8 fields). Lets M1.7+ consumers
(HUD readout, editor handoff) reach a fully-populated record.

### O3. Off-screen mouse guard placement

The plan puts the off-screen bounds check (mouseX/Y inside framebuffer
rect) inside `tryStaticPropPick` itself, BEFORE the y-flip and before
`lookupAtPixel`. Rationale: keeps the missiongui style bodies free of
new conditionals -- the style-body call sites pass raw mouseX/mouseY
and trust the helper to short-circuit cleanly.

**Lean:** guard inside the helper (one location to inspect).

---

## Pre-flight reading (engineer MUST read before Task 1)

1. Spec entirely. Especially Sections 3 (input handling + detection condition), 4 (selection priority + fallback order), 5 (lookupAtPixel y-flip), 6 (StaticPropSelectionDebugState shape + lifecycle), 7 (log schema v1), 9 (env gating + opt-in stack), 10 (validation gates 1-4), 11 (forbidden behaviors + 4-site invariant), 13 (resolved Q6/Q8 with 4-site map).
2. Adversarial review entirely (referenced from spec header). The CRITICAL finding and the two MAJORs are now spec decisions, but the WRITE-TIME grep evidence in the review is what every Task 4 code edit must reverify before touching.
3. Worktree CLAUDE.md -- full file. Especially: NO emoji, grep-before-cite, build `--config RelWithDebInfo`, full-relink discipline (`rm build64/RelWithDebInfo/mc2.exe` before `cmake --build` on layout / static-state / inline-template changes; M1.6 is additive helper + 4 single-line sets so a full relink is generally NOT required, but Task 5 verification commands include it as a safety net), canonical smoke gate command, substitutive-not-additive rule, debug-instrumentation rule (`[SUBSYS v1]` schema).
4. M1.5 plan format (`docs/superpowers/plans/2026-05-23-renderworld-slice-m1-5-objectid-buffer-plan.md`). M1.6 mirrors its structure.
5. M1 plan (`docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`) for greybeard / firewall section conventions.
6. Verify every cited file:line in this plan is still at the cited offset BEFORE starting any task that touches that file. The audit was performed 2026-05-23 against HEAD `842f34f`. If shipping HEAD has drifted, adjust line numbers in this plan AND in the commit message, then proceed.

---

## File structure

**Created files:** NONE. M1.6 is substitutive into existing files.

**Modified files (grep-verified 2026-05-23 against HEAD `842f34f`):**

- `RenderWorld/RenderWorld.h` -- declare `StaticPropSelectionDebugState` + `setLastStaticPropPick` + `clearLastStaticPropPick` + `getLastStaticPropPick` + `IsStaticPropPickEnabled` + `IsStaticPropPickDebugEnabled`. (Task 1 + Task 2 + Task 3.)
- `RenderWorld/RenderWorld.cpp` -- implement the accessors + state struct + wire `clearLastStaticPropPick()` into the no-arg `destroy()` lifecycle hook at `:321`. Banner extension (one line) emits `[STATIC_PROP_PICK v1] enabled=<0|1> debug=<0|1>` at `init()`. (Task 1 + Task 2 + Task 3.)
- `code/missiongui.cpp` -- add `tryStaticPropPick` private helper + 4 single-line `moverSelectedThisFrame = true` instrumentations after `setSelected(true)` at `:1460` / `:1487` / `:1690` / `:1705` + `moverSelectedThisFrame` declaration at top of each style body + helper call at tail of each style body. (Task 4; single atomic commit covers all of Phase B.)
- `code/missiongui.h` -- add private method declaration for `tryStaticPropPick`. (Task 4.)
- `.claude/worktrees/nifty-mendeleev/CLAUDE.md` -- add Active campaigns bullet for M1.6 shipped state. (Task 7.)

**Untouched (load-bearing -- confirm via grep, not assumption):**

- `code/missiongui.cpp` order-issuance paths (`giveOrder*`, `addWaypoint*`, `setAttackTarget*`), right-click dispatch, drag-rect selection, double-click semantics. Spec Section 11 explicit non-goal.
- `shaders/*` -- M1.6 does not touch shaders.
- `RenderWorld/legacy/static_prop_backend.{h,cpp}` -- M1's bridge surface; M1.6 does not touch it.
- `GameOS/gameos/gos_postprocess.{h,cpp}` -- M1.5 attachment-2 + helper; M1.6 consumes via `lookupAtPixel` but does not modify.
- `RenderCore/Handle.h` -- already exposes `raw()` (M1 SHIPPED). M1.6 consumes; does not touch.
- Save/load surfaces -- spec Section 11 forbids gameplay-state mutation; pick state explicitly NOT serialized (Q2 lean).

---

## Phase A -- Substrate API additions (build green, env-OFF default, no behavior change)

**Phase A goal:** the `StaticPropSelectionDebugState` type, the env-flag accessors, the set/clear/get free functions, and the `destroy()` lifecycle wiring all exist. Env-OFF: tier1 5/5 pixel-parity at idle vs M1.5 HEAD (the only added cost at idle is the new init-banner line; no per-frame work). Env-ON (master enable WITHOUT yet wiring missiongui): the `[STATIC_PROP_PICK v1] enabled=1 debug=0` banner appears at init; no other behavior change because missiongui does not yet call any of the new functions.

**Phase A gate (must pass before Phase B starts):** tier1 5/5 PASS env-OFF default with `[STATIC_PROP_PICK v1] enabled=0` token in the init banner. Then a single ad-hoc spot-check with `MC2_STATIC_PROP_PICK=1` confirms (a) the banner flips to `enabled=1`, (b) no missiongui behavior change (because Phase A does not wire missiongui), (c) tier1 5/5 still PASS.

### Task 1: Add `MC2_STATIC_PROP_PICK_DEBUG` env-flag accessor in RenderWorld

**Files:**
- Modify: `RenderWorld/RenderWorld.h`
- Modify: `RenderWorld/RenderWorld.cpp`

- [ ] **Step 1: Re-grep the existing M1.5 env-flag accessor shape**

```bash
grep -n "IsObjectIdBufferEnabled\|envFlag" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.h /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp | head -15
```

Expected: declaration at `RenderWorld.h:85`; implementation at
`RenderWorld.cpp:406`; helper `envFlag()` in the anon namespace (used
by every existing `MC2_*` accessor in this TU). Confirm the format we
will be mirroring.

- [ ] **Step 2: Add accessor declarations in `RenderWorld/RenderWorld.h`**

Existing (verbatim, immediately after `IsObjectIdBufferEnabled()` at `:85`):

```cpp
// M1.5: object-ID buffer env-flag accessor. Reads
// MC2_OBJECT_ID_BUFFER once at first call; subsequent calls return the
// cached value. Flipping the env var requires a process restart (the
// linked shader's GLSL macro is fixed at program-load time per
// memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
//
// Consumed by:
//   - gos_postprocess.cpp setSceneDrawBuffers() / createFBOs() / beginScene()
//   - gos_static_prop_batcher.cpp producer (objectIdRaw fill)
//   - RenderWorld.cpp lookupAtPixel() guard
//   - RenderWorld.cpp frameBannerTick() banner token
//   - C++ side of static-prop makeProgram() (gates the GLSL #ifdef prefix)
bool IsObjectIdBufferEnabled();
```

Replace with (identical block plus new declarations below it):

```cpp
// M1.5: object-ID buffer env-flag accessor. ... (unchanged) ...
bool IsObjectIdBufferEnabled();

// M1.6: static-prop pick master enable. When this is OFF, the missiongui
// Shift+click wiring is dormant even if MC2_OBJECT_ID_BUFFER=1. Three-gate
// opt-in stack per spec Section 9; defense-in-depth so a dev can enable
// the substrate for log-driven inspection without changing click behavior.
//
// Process-lifetime cached; restart required to flip. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick guard
bool IsStaticPropPickEnabled();

// M1.6: static-prop pick verbose-log enable. When this is OFF, the
// `[STATIC_PROP_PICK v1] miss ...` line is suppressed (high-frequency
// empty-Shift+click gesture would otherwise spam stderr). `hit` lines
// fire unconditionally per spec Section 7.
//
// Process-lifetime cached. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch
bool IsStaticPropPickDebugEnabled();
```

- [ ] **Step 3: Implement both accessors in `RenderWorld/RenderWorld.cpp`**

Locate the existing `IsObjectIdBufferEnabled` implementation
(`RenderWorld.cpp:406`). Add immediately after it, inside the same
`RenderWorld` namespace block:

Existing (verbatim, around `:406-410`):

```cpp
bool IsObjectIdBufferEnabled() {
    static const bool s_enabled = readObjectIdBufferEnv();
    return s_enabled;
}
```

Add (do NOT modify the existing function; append):

```cpp
// M1.6: master enable for the static-prop pick wiring (missiongui
// Shift+click -> lookupAtPixel -> setLastStaticPropPick). Default OFF.
bool IsStaticPropPickEnabled() {
    static const bool s_enabled = envFlag("MC2_STATIC_PROP_PICK");
    return s_enabled;
}

// M1.6: verbose-log enable. Gates the `[STATIC_PROP_PICK v1] miss`
// line only; the `hit` line is unconditional.
bool IsStaticPropPickDebugEnabled() {
    static const bool s_enabled = envFlag("MC2_STATIC_PROP_PICK_DEBUG");
    return s_enabled;
}
```

- [ ] **Step 4: Extend the init banner with `[STATIC_PROP_PICK v1] enabled=...` line**

Existing (verbatim, around `:265-275` -- the `[OBJECT_ID v1] event=enabled` line that fires when `oid` is true):

```cpp
    const bool oid = IsObjectIdBufferEnabled();
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        std::fprintf(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
```

Replace with (add ONE new line after the OBJECT_ID line):

```cpp
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
```

- [ ] **Step 5: Build (header + cpp; no callers yet)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The two new accessors are unreferenced externally
in this commit; only `init()` consumes them for the banner. MSVC may
inline both to a single `getenv()` cache load.

- [ ] **Step 6: Verify env-OFF tier1 smoke + new banner line**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. `grep -h "STATIC_PROP_PICK v1" tests/smoke/artifacts/<latest>/*.log | head -5` shows `[STATIC_PROP_PICK v1] enabled=0 debug=0` exactly once per mission init. No other `[STATIC_PROP_PICK v1]` lines (nothing else logs yet).

- [ ] **Step 7: Commit**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): add MC2_STATIC_PROP_PICK env flag accessors (M1.6 Task 1)

Process-lifetime cached accessors IsStaticPropPickEnabled() and
IsStaticPropPickDebugEnabled() read MC2_STATIC_PROP_PICK and
MC2_STATIC_PROP_PICK_DEBUG once at first call. Three-gate opt-in
stack per spec Section 9: substrate (M1.5) + wiring (this slice) +
verbosity are independently flippable so a dev can enable the
buffer for log inspection without altering click behavior.

[STATIC_PROP_PICK v1] enabled=N debug=N banner emitted once at
init() so log readers can correlate gate state with subsequent
hit/miss lines. No consumers yet; substrate-only.

Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 9

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 2: Add `StaticPropSelectionDebugState` type + set / clear / get accessors

**Files:**
- Modify: `RenderWorld/RenderWorld.h`
- Modify: `RenderWorld/RenderWorld.cpp`

- [ ] **Step 1: Re-grep the existing `LookupResult` declaration**

```bash
grep -n "LookupResult\|struct LookupResult\|^} // namespace RenderWorld$" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.h
```

Expected: `struct LookupResult` declared around `:119`, `lookupAtPixel`
declared around `:138`, and exactly ONE closing namespace brace.

- [ ] **Step 2: Add the `StaticPropSelectionDebugState` type + free-function declarations**

Existing (verbatim, the file's tail after `lookupAtPixel` declaration around `:138`):

```cpp
LookupResult lookupAtPixel(int screenX, int screenY);

} // namespace RenderWorld
```

Replace with:

```cpp
LookupResult lookupAtPixel(int screenX, int screenY);

// M1.6: most-recent static-prop pick debug state. Updated by
// MissionInterfaceManager::tryStaticPropPick on a successful Shift+click
// pick. Single-slot (latest wins); not a selection list. Not serialized.
// Cleared on per-mission RenderWorld::destroy() so a stale handle does
// not survive mission-load boundaries.
//
// Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6.
struct StaticPropSelectionDebugState {
    bool                            valid              = false;
    RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
    int32_t                         recipeIndex        = -1;
    int32_t                         lastPickMouseX     = 0;  // Win32 origin top-left
    int32_t                         lastPickMouseY     = 0;
    int32_t                         lastPickGlX        = 0;  // GL origin bottom-left
    int32_t                         lastPickGlY        = 0;
    uint64_t                        lastPickFrameIndex = 0;  // mirrors s_frameCounter at pick time
};

// M1.6: populate from a valid LookupResult. Asserts internally that
// res.isValid == true (callers must filter; debug build only).
// mouseX/Y are Win32-convention click coords; glX/Y are the post-y-flip
// GL coords passed to lookupAtPixel. lastPickFrameIndex is sampled from
// the internal frame counter at call time.
void setLastStaticPropPick(const LookupResult& res,
                           int32_t mouseX, int32_t mouseY,
                           int32_t glX,    int32_t glY);

// M1.6: reset to default (valid=false). Idempotent. Called on
// (a) empty Shift+click (Q1 lean: clear on miss), and
// (b) per-mission RenderWorld::destroy() lifecycle hook (Q2 lean).
void clearLastStaticPropPick();

// M1.6: read-only access. Caller MUST check .valid before consuming any
// other field. Returns a copy (the struct is tiny; avoids exposing
// internal mutex state to callers).
StaticPropSelectionDebugState getLastStaticPropPick();

} // namespace RenderWorld
```

- [ ] **Step 3: Add the storage + mutex + frame-counter read in `RenderWorld/RenderWorld.cpp`**

Locate the existing `s_objectRecords` + `s_objectRecordsMutex` declarations (M1.5; in the anonymous namespace near the top of the cpp; grep for `s_objectRecords` to find the exact site). Add immediately after them:

```cpp
// M1.6: most-recent static-prop pick debug state. Single-slot; updated
// by setLastStaticPropPick from the gameplay-side tryStaticPropPick helper.
// Mutex-guarded because get/set may interleave on a future off-thread
// HUD consumer; M1.6 itself is main-thread only.
//
// Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6.
std::mutex                                       s_lastStaticPropPickMutex;
RenderWorld::StaticPropSelectionDebugState       s_lastStaticPropPick;
```

- [ ] **Step 4: Implement the three free functions**

Add inside the public `RenderWorld` namespace block (locate the existing
`lookupAtPixel` implementation around `:448` and add after its closing
brace):

```cpp
void setLastStaticPropPick(const LookupResult& res,
                           int32_t mouseX, int32_t mouseY,
                           int32_t glX,    int32_t glY)
{
    // Callers MUST filter on res.isValid before calling. We do not assert
    // here (release-mode safety) but a misuse populates a "valid pick"
    // with an invalid handle, which the next get() consumer will see
    // and either skip or log-spam. Filter at the call site.
    std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
    s_lastStaticPropPick.valid              = res.isValid;
    s_lastStaticPropPick.handle             = res.handle;
    // recipeIndex: M1.5 LookupResult exposes the recipe handle bits but
    // not the int32 recipe index directly. We project the handle ->
    // recipe index via the existing inverse mapper `handleToRecipeIndex`
    // (declared at `RenderWorld.cpp:62`; takes a full RenderObjectHandle
    // and returns int32_t). If invalid, store -1.
    s_lastStaticPropPick.recipeIndex        = res.isValid
        ? handleToRecipeIndex(res.handle)
        : -1;
    s_lastStaticPropPick.lastPickMouseX     = mouseX;
    s_lastStaticPropPick.lastPickMouseY     = mouseY;
    s_lastStaticPropPick.lastPickGlX        = glX;
    s_lastStaticPropPick.lastPickGlY        = glY;
    s_lastStaticPropPick.lastPickFrameIndex =
        s_frameCounter.load(std::memory_order_relaxed);
}

void clearLastStaticPropPick() {
    std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
    s_lastStaticPropPick = StaticPropSelectionDebugState{};
}

StaticPropSelectionDebugState getLastStaticPropPick() {
    std::lock_guard<std::mutex> lk(s_lastStaticPropPickMutex);
    return s_lastStaticPropPick;  // copy out; struct is tiny
}
```

Note: `handleToRecipeIndex` is the canonical inverse mapper for M1's
`recipeIndexToHandleIndex`. Grep at write time to confirm the symbol is
still where the plan-author left it (`RenderWorld.cpp:62`, taking a
`RenderObjectHandle`, returning `int32_t`):

```bash
grep -n "handleToRecipeIndex\|recipeIndexToHandleIndex" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp
```

Expected: exactly one definition of each at the cited line offsets
(symbols are stable; offsets may drift). The plan adversarial reviewer
verified 2026-05-23 against HEAD `842f34f`. Do NOT substitute the
fallback `static_cast<int32_t>(res.handle.index())` even though it is
incidentally equivalent under M1's identity mapping -- future mapper
changes would break that fallback silently.

- [ ] **Step 5: Build (declarations and implementations both present)**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The three new functions are unreferenced externally; LTO may shrink the implementations.

- [ ] **Step 6: Verify env-OFF tier1 smoke (no behavior change)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. No `[STATIC_PROP_PICK v1] hit` / `miss` lines
because no caller yet. Only the init banner from Task 1 fires.

- [ ] **Step 7: Commit**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): add StaticPropSelectionDebugState + accessors (M1.6 Task 2)

Tiny RenderWorld-side debug-state holder for the most-recent
successful static-prop pick. Single-slot, mutex-guarded, not
serialized. Three free functions:
  setLastStaticPropPick(res, mouseX, mouseY, glX, glY)
  clearLastStaticPropPick()
  getLastStaticPropPick() -> copy

Lives on the RenderWorld side (not missiongui) so the handle table
+ generation check stay co-located with the s_objectRecords table
they reference. Missiongui patch in Task 4 will reach in via one
include (already permitted by the M1 firewall script).

No callers yet; substrate-only.

Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 3: Wire `clearLastStaticPropPick()` into per-mission `RenderWorld::destroy()`

**Files:**
- Modify: `RenderWorld/RenderWorld.cpp`

This is the Q2 wiring: pick state does NOT survive a mission-end. The
M1 wiring at `code/mission.cpp:3279` calls the no-arg
`RenderWorld::destroy()` overload exactly once per mission unload.

- [ ] **Step 1: Re-grep the no-arg `destroy()` overload**

```bash
grep -n "^void destroy() {" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/RenderWorld.cpp
```

Expected: ONE hit at `:321`. The per-handle overload at `:380`
(`destroy(RenderObjectHandle h)`) is NOT the lifecycle hook; do not
touch it.

- [ ] **Step 2: Add the clear call at the top of the no-arg destroy body**

Existing (verbatim, `:321-329`):

```cpp
void destroy() {
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=destroy upsert_ok=%llu upsert_fail=%llu "
        "destroy_calls=%llu mark_visible=%llu\n",
        (unsigned long long)s_upsertOk.load(),
        (unsigned long long)s_upsertFail.load(),
        (unsigned long long)s_destroyCalls.load(),
        (unsigned long long)s_markVisibleCalls.load());
}
```

Replace with:

```cpp
void destroy() {
    // M1.6 Q2: pick state does not survive mission-end. Clearing here
    // prevents a stale handle from pointing into a cleared s_objectRecords
    // table -- the generation check would catch it on read, but the
    // resulting "valid=true handle=N" -> "lookup returns invalid" gap
    // would confuse anyone grepping the log post-load. Cheap; one
    // mutex-guarded scalar reset per mission end.
    clearLastStaticPropPick();
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=destroy upsert_ok=%llu upsert_fail=%llu "
        "destroy_calls=%llu mark_visible=%llu\n",
        (unsigned long long)s_upsertOk.load(),
        (unsigned long long)s_upsertFail.load(),
        (unsigned long long)s_destroyCalls.load(),
        (unsigned long long)s_markVisibleCalls.load());
}
```

- [ ] **Step 3: Build**

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 4: Verify env-OFF tier1 smoke (no behavior change)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. The `clearLastStaticPropPick()` call runs once
per mission end; the struct is already default-initialized in steady state
so the reset is a no-op observable-side. Existing
`[RENDER_WORLD v1] event=destroy` line is unchanged.

- [ ] **Step 5: Commit**

```bash
git add RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): clear pick state on per-mission destroy (M1.6 Task 3)

clearLastStaticPropPick() called at the top of the no-arg
destroy() lifecycle hook so a mid-pick mission-end does not leave
the StaticPropSelectionDebugState pointing into a cleared
s_objectRecords table. Q2 lean per spec Section 13: pick state
does not persist across mission-load boundaries.

No observable change yet (no caller writes the state), but the
hook is in place for Task 4's helper invocation.

Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md sec 6, Q2

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase B -- Missiongui wiring (single atomic commit; helper + 4 instrumentations + 2 call sites)

**Phase B goal:** the gameplay-side gesture. With both env vars on, a
Shift+left-click on a static prop emits `[STATIC_PROP_PICK v1] hit ...`
and updates the debug state. Legacy Shift+LMB additive-select on a
friendly mover is preserved verbatim and emits NO M1.6 log line.

**Phase B gate (must pass before Phase C starts):** Phase A's tier1 5/5
env-OFF pixel-parity-at-idle invariant still holds after Phase B's
edits (helper code path is dormant when `MC2_STATIC_PROP_PICK=0`; the
helper adds ~1 CALL/RET per click event, but click events are not
exercised by the no-user-input tier1 harness so observable cost is
zero).

### Task 4: Add `tryStaticPropPick` helper + wire 4 instrumentation sites + 2 call sites (SINGLE ATOMIC COMMIT)

**Files:**
- Modify: `code/missiongui.h` (private method declaration)
- Modify: `code/missiongui.cpp` (helper definition + 4 instrumentation sites + 2 call sites)

**Atomicity is load-bearing.** This task lands the helper definition,
the four `moverSelectedThisFrame = true` instrumentations, AND both
helper call sites in a single commit. (Plan adversarial review M1
collapsed the previous T4/T5 split: a commit that lands the helper
without callers would leave a dormant private method and risks a
promoted-warning red HEAD on some MSVC build configs; a commit that
lands the helper call without the 4 sets would misfire the fallback
gate -- Shift+LMB on a friendly mover would emit a M1.6 log line AND
toggle selection, violating the Section 11 invariant in the
in-between commit. Both halves ship together.)

**Step ordering:** This task interleaves the helper-add steps (former
T4) with the call-site-wiring steps (former T5). Order (18 steps):
1. Header grep -- `missiongui.h` private method block (former T4 Step 1)
2. Add helper declaration in `missiongui.h` (former T4 Step 2)
3. cpp `#include` block grep (former T4 Step 3)
4. Add `RenderWorld/RenderWorld.h` include if not reachable (former T4 Step 4)
5. Implement `tryStaticPropPick` body (former T4 Step 5; greps for symbol collision)
6. Re-grep 4 setSelected(true)/(false) writer sites (former T5 Step 1)
7. `moverSelectedThisFrame` declaration at top of updateOldStyle (former T5 Step 2)
8. Instrument Q6 site 1 at `:1460` (former T5 Step 3)
9. Instrument Q6 site 2 at `:1487` (former T5 Step 4)
10. Helper call at tail of updateOldStyle (former T5 Step 5)
11. `moverSelectedThisFrame` declaration at top of updateAOEStyle (former T5 Step 6)
12. Instrument Q6 site 3 at `:1690` (former T5 Step 7)
13. Instrument Q6 site 4 at `:1705` (former T5 Step 8)
14. Helper call at tail of updateAOEStyle (former T5 Step 9)
15. Grep gates (former T5 Step 10)
16. Full relink + build (former T5 Step 11)
17. Tier1 env-OFF smoke (former T5 Step 12 -- "pixel-parity at idle")
18. Single atomic commit (former T5 Step 13)

- [ ] **Step 1: Re-grep the existing private method block in `missiongui.h`**

```bash
grep -n "MissionInterfaceManager\|updateOldStyle\|updateAOEStyle\|^private:\|^public:" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.h | head -30
```

Expected: class declaration with `updateOldStyle` / `updateAOEStyle`
declared in some access section. Identify the section (likely private or
protected) where the new helper declaration fits stylistically.

- [ ] **Step 2: Add the helper declaration in `code/missiongui.h`**

Locate the line declaring `void updateAOEStyle(...)`. Add immediately
after it (in the same access section):

```cpp
    // M1.6: env-gated static-prop pick helper. Called from the tail of
    // both updateOldStyle and updateAOEStyle when leftClicked && shiftDn
    // && !bGui. Short-circuits when moverSelectedThisFrame == true so
    // the legacy Shift+LMB additive-select gesture on a friendly mover
    // is preserved verbatim (no M1.6 log line in that case). Emits
    // [STATIC_PROP_PICK v1] hit/miss and updates RenderWorld debug state.
    //
    // Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
    void tryStaticPropPick(bool moverSelectedThisFrame,
                           bool shiftDn,
                           bool leftClicked,
                           bool bGui,
                           bool bLeftDouble,
                           int  mouseX,
                           int  mouseY);
```

- [ ] **Step 3: Re-grep the cpp file's top-of-file `#include` block**

```bash
grep -n "^#include" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -25
```

Expected: a block of `#include` lines at the top. Confirm whether
`RenderWorld/RenderWorld.h` is already included by another header pulled
in by `missiongui.cpp` (M1 wiring may have surfaced it transitively
via `mission.h`). If grep finds `RenderWorld` already reachable, skip
the include addition.

- [ ] **Step 4: Add the include (if not already reachable)**

If grep confirms `RenderWorld/RenderWorld.h` is NOT already pulled in,
add to the top of `code/missiongui.cpp` near the other engine includes:

```cpp
#include "../RenderWorld/RenderWorld.h"  // M1.6: IsStaticPropPickEnabled, lookupAtPixel, setLastStaticPropPick
```

(Path is relative to `code/`; M1's adapter at `code/warrior.cpp` and
the M1 firewall script confirm this is the canonical reach-in path.)

- [ ] **Step 5: Implement `tryStaticPropPick` in `code/missiongui.cpp`**

Append the function body at the end of the file (or adjacent to
`updateAOEStyle`'s body, whichever matches local style). Verify by grep
that no name collision exists:

```bash
grep -n "tryStaticPropPick" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Expected: zero hits (the symbol does not yet exist in the file).

Add:

```cpp
// M1.6: env-gated static-prop pick helper. See spec
// docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
// Sections 3 (detection condition), 4 (fallback order), 5 (y-flip), 11
// (forbidden behaviors), Q6 (4-site mover observable), Q8 (legacy
// preservation invariant).
//
// Mover-first short-circuit: if the legacy click-driven mover-select
// path set moverSelectedThisFrame=true at one of the 4 setSelected(true)
// writer sites this frame, M1.6 must NOT emit a log line (Section 11
// invariant). This preserves Shift+LMB additive-select on a friendly
// mover verbatim.
//
// Y-flip: missiongui mouseY is Win32-convention (origin top-left).
// RenderWorld::lookupAtPixel takes GL-convention coords (origin
// bottom-left). The translation lives at this call site; lookupAtPixel
// must not silently translate (M1.5 contract).
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

    // Off-screen bounds guard (O3 lean: guard inside helper). Win32
    // mouseX/Y are already clamped by the OS during in-game capture,
    // but a defensive check costs one branch and avoids a silent
    // glReadPixels at a clipped pixel returning a wrong-looking 0.
    const int sw = Environment.screenWidth;
    const int sh = Environment.screenHeight;
    if (mouseX < 0 || mouseY < 0 || mouseX >= sw || mouseY >= sh)
        return;

    // Spec Section 5: Win32 origin top-left -> GL origin bottom-left.
    // Established pattern at mclib/mouse.cpp:225 and mclib/utilities.cpp:111-115.
    const int glX = mouseX;
    const int glY = sh - 1 - mouseY;

    // Synchronous single-pixel readback; stalls ~1-5ms (acceptable for
    // click-time pick; not per-frame). M1.5 lookupAtPixel internally
    // validates against s_objectRecords (generation + alive).
    RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(glX, glY);

    if (res.isValid) {
        // Update RenderWorld debug state. Single-slot; latest wins.
        RenderWorld::setLastStaticPropPick(res, mouseX, mouseY, glX, glY);
        // Unconditional hit log (spec Section 7: if M1.6 ran the lookup,
        // the user expressed intent and we owe them visibility).
        std::fprintf(stderr,
            "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d)\n",
            res.handle.bits,
            (unsigned)res.handle.index(),
            (unsigned)res.handle.generation(),
            (int)res.recipeIndex,
            mouseX, mouseY, glX, glY);
    } else {
        // Q1 lean: clear the debug-state struct on empty Shift+click so
        // a stale prior pick does not survive an empty-click gesture.
        RenderWorld::clearLastStaticPropPick();
        // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1.
        // Empty Shift+click is a high-frequency gesture; default-OFF
        // verbosity prevents log spam.
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            std::fprintf(stderr,
                "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d)\n",
                mouseX, mouseY, glX, glY);
        }
    }
}
```

- [ ] **Step 6: Re-grep the 4 setSelected(true) writer sites + the 4 setSelected(false) sibling sites**

```bash
grep -n "setSelected( true )\|setSelected( false )" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp | head -20
```

Expected (verify line numbers; symbols are stable but offsets may drift):
- `:1460` -- updateOldStyle Shift+additive `target->setSelected( true );` (Q6 site 1)
- `:1462` -- updateOldStyle Shift+additive `target->setSelected( false );` (toggle-off; do NOT instrument)
- `:1483` -- updateOldStyle plain-LMB-loop `pMover->setSelected( false );` (clear-others; do NOT instrument)
- `:1487` -- updateOldStyle plain-LMB-select `target->setSelected( true );` (Q6 site 2)
- `:1690` -- updateAOEStyle Shift+additive `target->setSelected( true );` (Q6 site 3)
- `:1692` -- updateAOEStyle Shift+additive `target->setSelected( false );` (toggle-off; do NOT instrument)
- `:1701` -- updateAOEStyle plain-LMB-loop `pMover->setSelected( false );` (clear-others; do NOT instrument)
- `:1705` -- updateAOEStyle plain-LMB-select `target->setSelected( true );` (Q6 site 4)

If grep finds a 5th `setSelected( true )` in either style body that
the spec Q6 census missed, STOP and escalate. The spec is grep-verified
and amending it requires a new spec pass; do NOT silently instrument an
unaccounted site.

- [ ] **Step 7: Declare `moverSelectedThisFrame` at top of `updateOldStyle`**

Existing (verbatim, `:1314-1336`, the top of `updateOldStyle`):

```cpp
void MissionInterfaceManager::updateOldStyle( bool shiftDn, bool altDn, bool ctrlDn, 
											 bool bGui, bool lineOfSight, bool passable, 
											 long moverCount, long nonMoverCount )
{

	printDebugInfo();

	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();
	
 	// Update the waypoint markers so that they are visible!
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick() && !lastUpdateDoubleClick);
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isRightClick());
```

Replace with (add ONE declaration immediately after `leftClicked`):

```cpp
void MissionInterfaceManager::updateOldStyle( bool shiftDn, bool altDn, bool ctrlDn, 
											 bool bGui, bool lineOfSight, bool passable, 
											 long moverCount, long nonMoverCount )
{

	printDebugInfo();

	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();
	
 	// Update the waypoint markers so that they are visible!
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick() && !lastUpdateDoubleClick);
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isRightClick());

	// M1.6 Q6 4-site observable: set to true immediately after each of
	// the 4 setSelected(true) writer sites this style body owns
	// (lines :1460, :1487 in updateOldStyle). Read at tail by
	// tryStaticPropPick to short-circuit when the legacy path already
	// claimed this click. Local scope; no leakage.
	bool moverSelectedThisFrame = false;
```

- [ ] **Step 8: Instrument the updateOldStyle Shift+additive site (Q6 site 1, around `:1460`)**

Existing (verbatim, `:1456-1465`):

```cpp
					if (!alreadyThere && !target->isDisabled())
						target->setSelected( true );
					else
						target->setSelected( false );
				}
```

Replace with:

```cpp
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 1: legacy Shift+additive-select on
						// friendly mover. Setting the gate so the tail
						// tryStaticPropPick short-circuits and the Section 11
						// invariant holds (no M1.6 log line here).
						moverSelectedThisFrame = true;
					} else {
						target->setSelected( false );
						// Q6 set-to-false sibling: toggle-off / deselect
						// gesture. MUST NOT set moverSelectedThisFrame=true.
						// Doing so would let the user deselect AND then have
						// M1.6 still suppress its log on the same click; the
						// intended behavior is "user-visible deselect, no
						// M1.6 line emitted because the click landed on a
						// mover" -- which is exactly what `=true` here would
						// give us. EXPLANATION: the spec wants
						// moverSelectedThisFrame to mean "the click landed
						// on a mover and the legacy path consumed it",
						// regardless of toggle direction. Both
						// setSelected(true) AND setSelected(false) on the
						// SAME target inside this Shift-additive branch
						// satisfy that. Per spec Q6 RESOLVED literal: only
						// the setSelected(true) sites are instrumented;
						// the false sibling is left alone. Rationale: the
						// Q6 census audited the 4 setSelected(true) writers
						// as the canonical "mover was consumed" predicate;
						// the false-branch is a deselect that does NOT
						// "claim" the click in the sense that matters for
						// the fallback gate (a user who Shift-clicks an
						// already-selected mover to deselect it has not
						// expressed pick-the-prop-behind intent either,
						// but ALSO has not expressed pick-the-prop intent
						// -- the gesture is ambiguous and the spec resolves
						// to "legacy wins, no M1.6"). To make that resolve
						// correctly, the false-branch ALSO sets the gate.
						moverSelectedThisFrame = true;
					}
				}
```

Wait -- re-reading spec Q6 RESOLVED literal:

> The set-to-false sites at `:1462`, `:1483`, `:1692`, `:1701` are
> toggle-off / clear-others gestures and MUST NOT set
> `moverSelectedThisFrame = true`. Doing so would misfire the
> fallback gate (e.g. Shift+LMB on already-selected mover would
> deselect AND log a pick).

The spec is explicit: false-branches MUST NOT set the gate. The
verbose justification above is wrong; correct the replacement.

**Corrected replacement** for the updateOldStyle Shift+additive site (Q6 site 1 at `:1460`):

```cpp
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 1: legacy Shift+additive-select on
						// friendly mover. Sets fallback gate; tail
						// tryStaticPropPick short-circuits so Section 11
						// invariant holds (no M1.6 log line in this case).
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: setSelected(false) toggle-off
						// site at :1462 MUST NOT set the gate. Per spec
						// rationale: Shift+LMB on already-selected mover
						// deselects AND would otherwise emit a M1.6 log
						// line, both of which are wrong. The fallback gate
						// stays false; tryStaticPropPick will then either
						// find a static prop at the cursor (hit log) or
						// log a debug-mode miss. The deselect itself is
						// the legacy gesture and is preserved.
						target->setSelected( false );
					}
				}
```

- [ ] **Step 9: Instrument the updateOldStyle plain-LMB-select site (Q6 site 2, around `:1487`)**

Existing (verbatim, `:1480-1494`):

```cpp
				else if ( target->isMover() && !target->isDisabled() && target->getCommanderId() == Commander::home->getId() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							pMover->setSelected( false );
						}
					}
					
					target->setSelected( true );
				}
```

Replace with:

```cpp
				else if ( target->isMover() && !target->isDisabled() && target->getCommanderId() == Commander::home->getId() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							// M1.6 Q6 RESOLVED: clear-others setSelected(false)
							// loop at :1483 MUST NOT set the gate -- the
							// gate must reflect "did this click select A
							// mover", not "did this click touch the
							// selection list at all". Per spec, only the
							// final setSelected(true) below sets the gate.
							pMover->setSelected( false );
						}
					}
					
					target->setSelected( true );
					// M1.6 Q6 site 2: legacy plain-LMB-select on friendly
					// mover. Sets fallback gate.
					moverSelectedThisFrame = true;
				}
```

- [ ] **Step 10: Add helper call at the tail of `updateOldStyle`**

Locate the closing brace of `updateOldStyle` body. The function ends
around `:1507` (right before the `void MissionInterfaceManager::updateAOEStyle` declaration at `:1509`). Find the last `}` before that
declaration.

Existing (verbatim, the last few lines of updateOldStyle body before its
closing brace -- grep at write time, the tail may have minor whitespace
drift):

```cpp
			else
			{
				if ( controlGui.getGuard() ) // trying to guard invalid thing, cancel guard
					controlGui.toggleGuard();
				else if ( userInput->getMouseCursor() == mState_INFO && 
						target->isMover()  )
							controlGui.setInfoWndMover( (Mover*)target );
				else if ( !target->isDisabled() && !bForcedShot && !bAimedShot)
					doAttack();
			
			}
		}
	}

}	
void MissionInterfaceManager::updateAOEStyle(bool shiftDn, bool altDn, bool ctrlDn, 
```

Replace with (inject helper call inside the final `}` of `updateOldStyle`, BEFORE the closing brace):

```cpp
			else
			{
				if ( controlGui.getGuard() ) // trying to guard invalid thing, cancel guard
					controlGui.toggleGuard();
				else if ( userInput->getMouseCursor() == mState_INFO && 
						target->isMover()  )
							controlGui.setInfoWndMover( (Mover*)target );
				else if ( !target->isDisabled() && !bForcedShot && !bAimedShot)
					doAttack();
			
			}
		}
	}

	// M1.6: env-gated static-prop pick attempt. Runs AFTER the legacy
	// mover-selection path above. Short-circuits internally when
	// moverSelectedThisFrame == true (Section 11 + Q6/Q8 invariant) or
	// when MC2_STATIC_PROP_PICK / MC2_OBJECT_ID_BUFFER are off.
	// bLeftDouble is a local in the parent update() body; recompute here.
	const bool bLeftDouble = userInput->isLeftDoubleClick();
	tryStaticPropPick(moverSelectedThisFrame,
	                  shiftDn,
	                  leftClicked,
	                  bGui,
	                  bLeftDouble,
	                  mouseX,
	                  mouseY);

}	
void MissionInterfaceManager::updateAOEStyle(bool shiftDn, bool altDn, bool ctrlDn, 
```

Note (plan adversarial review M2): `mouseX` / `mouseY` are
`MissionInterfaceManager` CLASS STATICS (declared at `:167-168` --
`int MissionInterfaceManager::mouseX = 0; int mouseY = 0;`), polled
at `code/missiongui.cpp:719-720` inside `update()` BEFORE the style
dispatch at lines 899/922:

```cpp
mouseX = userInput->getMouseX();
mouseY = userInput->getMouseY();
```

So at the style-body tail, `mouseX/Y` contain the CURRENT FRAME poll,
NOT necessarily LMB-down coordinates. This is safe because
`leftClicked` filters drags: per `:1335` definition `leftClicked =
!isLeftDrag() && !isRightDrag() && isLeftClick() && !lastUpdateDoubleClick`.
A drag-then-click is excluded; only a clean single-frame LMB click
satisfies the gate, and on that frame mouseX/Y reflect the click
position. The helper short-circuits unless `leftClicked == true`, so
the mouseX/Y values reaching `lookupAtPixel` are always the click
position. No code change; the helper is only invoked on click frames.

`bGui` is passed in as a parameter to `updateOldStyle`. `shiftDn` is a
parameter. `leftClicked` is local. `bLeftDouble` is a local in
`update()` but NOT visible inside the style bodies; we recompute via
`userInput->isLeftDoubleClick()` (the same expression `update()` uses
at `:794`). Plan adversarial review m2 verified `isLeftDoubleClick`
is a pure read (no state mutation; idempotent query) so the recompute
at the style tail returns the same value as the parent `update()` call
on the same frame. Verify by grep at write time:

```bash
grep -n "isLeftDoubleClick\|bLeftDouble" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/userinput.cpp | head -10
```

If `isLeftDoubleClick` is found to be stateful (e.g. consumes the flag
on read), change the helper signature to accept `bLeftDouble` as a
parameter passed from `update()` instead of recomputed here.

- [ ] **Step 11: Declare `moverSelectedThisFrame` at top of `updateAOEStyle`**

Mirror Step 2 for the AOE style body (around `:1509-1535`). Add
`bool moverSelectedThisFrame = false;` immediately after the local
`leftClicked` / `rightClicked` declarations.

- [ ] **Step 12: Instrument the updateAOEStyle Shift+additive site (Q6 site 3, around `:1690`)**

Existing (verbatim, `:1685-1695`):

```cpp
					if (!alreadyThere && !target->isDisabled())
						target->setSelected( true );
					else
						target->setSelected( false );
				}
```

Replace with:

```cpp
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 3: AOE-style Shift+additive-select.
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: AOE-style toggle-off site at
						// :1692 MUST NOT set the gate. Same rationale as
						// updateOldStyle's :1462 sibling.
						target->setSelected( false );
					}
				}
```

- [ ] **Step 13: Instrument the updateAOEStyle plain-LMB-select site (Q6 site 4, around `:1705`)**

Existing (verbatim, `:1697-1710`):

```cpp
				else if ( target->isMover() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							pMover->setSelected( false );
						}
					}
					
					target->setSelected( true );
				}
```

Replace with:

```cpp
				else if ( target->isMover() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							// M1.6 Q6 RESOLVED: AOE clear-others
							// setSelected(false) loop at :1701 MUST NOT
							// set the gate.
							pMover->setSelected( false );
						}
					}
					
					target->setSelected( true );
					// M1.6 Q6 site 4: AOE-style plain-LMB-select.
					moverSelectedThisFrame = true;
				}
```

- [ ] **Step 14: Add helper call at the tail of `updateAOEStyle`**

Find the closing brace of `updateAOEStyle` body. Insert the helper call
immediately before it (mirroring Step 5's pattern for `updateOldStyle`).
Use the same parameter set; the AOE body's `leftClicked` is its local,
`shiftDn` / `bGui` are parameters, `mouseX` / `mouseY` are class
members, `bLeftDouble` is computed via `userInput->isLeftDoubleClick()`.

Add (immediately before the closing `}` of `updateAOEStyle`):

```cpp
	// M1.6: env-gated static-prop pick attempt for the AOE input style.
	// Same short-circuit semantics as updateOldStyle's call (Section 11
	// + Q6/Q8 invariant).
	const bool bLeftDouble = userInput->isLeftDoubleClick();
	tryStaticPropPick(moverSelectedThisFrame,
	                  shiftDn,
	                  leftClicked,
	                  bGui,
	                  bLeftDouble,
	                  mouseX,
	                  mouseY);
```

- [ ] **Step 15: Grep gate -- verify exactly 4 `moverSelectedThisFrame = true` writes**

```bash
grep -n "moverSelectedThisFrame = true" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Expected: EXACTLY 4 hits (one per Q6 site). If grep returns a different
count, STOP -- either an instrumentation was duplicated or one of the
4 sites was missed.

```bash
grep -n "moverSelectedThisFrame = false" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Expected: EXACTLY 2 hits (the local declarations at the top of each
style body; `= false` is part of the declaration).

```bash
grep -n "tryStaticPropPick(" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missiongui.cpp
```

Expected: EXACTLY 3 hits -- the function definition + the 2 call sites
(one per style tail).

- [ ] **Step 16: Full relink (defensive; helper introduces new private method to class layout)**

Per worktree CLAUDE.md "Full relink before deploy" rule: class-layout
changes (adding a private method does NOT change object layout in
MSVC's normal class memory model, but full relink is a safety net for
inline-template / static-state pitfalls):

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 17: Verify env-OFF tier1 smoke (pixel-parity at idle vs M1.5 HEAD)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. With `MC2_STATIC_PROP_PICK` unset (default),
the helper short-circuits on the first env-flag check before any work.
The 4 `moverSelectedThisFrame=true` writes run regardless but the local
bool is not consumed by anything except the dormant helper -- net cost
is one stack-local bool write per setSelected(true) call, AND one
CALL/RET (env-flag read + return) per Shift+LMB click event. Tier1's
no-user-input harness exercises neither path under steady state.
Pixel-parity at idle vs M1.5 HEAD `842f34f`; click-rate behavior is
identical (helper returns early at the env-OFF check).
`grep '\[STATIC_PROP_PICK v1\] (hit|miss)' tests/smoke/artifacts/<latest>/*.log` returns zero matches.

- [ ] **Step 18: Commit (single atomic commit for all Phase B wiring)**

```bash
git add code/missiongui.h code/missiongui.cpp
git commit -m "$(cat <<'EOF'
feat(missiongui): wire static-prop pick helper + 4-site mover gate (M1.6 Task 4)

Single atomic commit lands the helper definition, its declaration,
the 4 mover-gate instrumentations, and both helper call sites.
Plan adversarial review M1 collapsed the previous T4/T5 split;
splitting helper-add from call-site-wire would leave either a
dormant private method (warning-promotion risk under MSVC /WX) or
a misfiring fallback gate (Shift+LMB on friendly mover would emit
a M1.6 log line AND toggle selection, violating Section 11) in the
in-between commit.

Contents:
  - tryStaticPropPick(...) private method declared in missiongui.h
    and defined in missiongui.cpp (env gates, mover-first
    short-circuit, Win32 -> GL y-flip, off-screen bounds guard,
    hit / miss log emission, RenderWorld debug-state write)
  - moverSelectedThisFrame local bool declared at top of both
    updateOldStyle and updateAOEStyle
  - 4 single-line `= true` writes immediately after each of the 4
    setSelected(true) writer sites identified by spec Q6:
      :1460 updateOldStyle Shift+additive
      :1487 updateOldStyle plain-LMB-select
      :1690 updateAOEStyle Shift+additive
      :1705 updateAOEStyle plain-LMB-select
  - tryStaticPropPick(...) call at the tail of each style body
  - 4 setSelected(false) sibling sites EXPLICITLY left uninstrumented
    per spec Q6 RESOLVED rationale (toggle-off / clear-others gestures
    must NOT misfire the fallback gate)

Section 11 + Q8 invariant: Shift+LMB on a friendly mover continues
to toggle the legacy additive-select gesture and emits ZERO M1.6
log lines (the mover-selected gate short-circuits the helper before
the lookup runs).

Env-OFF tier1 5/5 pixel-parity at idle vs M1.5 HEAD 842f34f.
Helper adds ~1 CALL/RET per Shift+LMB click event, short-circuited
at the env-OFF check; no per-frame cost.

Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
  sections 3, 4, 5, 11; Q1, Q6, Q8

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase C -- Validation gates

**Phase C goal:** confirm spec Section 10's four validation gates pass.
Gate 1 (env-OFF parity) is covered by Task 4 Step 17 already; this phase
runs the env-ON canary and Section 11 invariant canary.

### Task 5: Validation gates (env-OFF parity + env-ON canary + Section 11 invariant)

**Files:** none modified. Run-only.

- [ ] **Step 1: Gate 1 -- env-OFF tier1 5/5 pixel-parity at idle (re-confirm Task 4 Step 17)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. `grep '\[STATIC_PROP_PICK v1\] (hit|miss)' tests/smoke/artifacts/<latest>/*.log` returns zero matches. `grep '\[STATIC_PROP_PICK v1\] enabled=0 debug=0' tests/smoke/artifacts/<latest>/*.log` returns 5 matches (one per mission init). Pixel parity vs M1.5 HEAD `842f34f`.

- [ ] **Step 2: Gate 2 -- env-ON Shift+click canary on mc2_03**

Per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN": the canary
requires user-driven Shift+click input. The user runs:

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_STATIC_PROP_PICK=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 60 --kill-existing --keep-logs --missions mc2_03
```

(60s instead of 30s so the user has time to sweep the cursor across
multiple visible static props and Shift+click each.)

User observations to confirm:
1. The init banner shows `[STATIC_PROP_PICK v1] enabled=1 debug=0`.
2. Shift+LMB on a visible building emits `[STATIC_PROP_PICK v1] hit handle=... idx=... gen=... recipe=... screen=(...) gl=(...)`.
3. Plain LMB on the same building does nothing (no hit log, no selection -- legacy behavior).
4. Plain LMB on a friendly mech selects the mech (legacy unchanged).
5. Plain LMB on empty terrain deselects (legacy unchanged).
6. **Section 11 invariant** -- Shift+LMB on a friendly mech: the mech toggles in/out of the selection list (legacy additive-select) AND ZERO `[STATIC_PROP_PICK v1]` log lines are emitted on those clicks. The Q6/Q8 short-circuit holds.
7. Shift+LMB on empty terrain: ZERO log lines (Gate 3 default).

Verification commands after the canary run:

```bash
grep '\[STATIC_PROP_PICK v1\] hit' tests/smoke/artifacts/<latest>/mc2_03.log | head -10
grep '\[STATIC_PROP_PICK v1\] miss' tests/smoke/artifacts/<latest>/mc2_03.log | head -5
```

Expected: hit grep returns >=1 line (assuming the user Shift+clicked a
visible prop); miss grep returns ZERO lines (debug verbosity is OFF
this canary).

- [ ] **Step 3: Gate 3 -- debug-verbosity ON, empty-click miss log**

User runs (note: the third env var):

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_STATIC_PROP_PICK=1 MC2_STATIC_PROP_PICK_DEBUG=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 60 --kill-existing --keep-logs --missions mc2_03
```

Init banner now shows `[STATIC_PROP_PICK v1] enabled=1 debug=1`.

User observations:
1. Shift+LMB on a building still emits `hit`.
2. Shift+LMB on empty terrain emits `[STATIC_PROP_PICK v1] miss screen=(...) gl=(...)`. Each empty click produces exactly ONE line.
3. Shift+LMB on a friendly mech still emits ZERO M1.6 log lines (Section 11 invariant; verbosity flag does not override the mover gate).

Verification:

```bash
grep '\[STATIC_PROP_PICK v1\] miss' tests/smoke/artifacts/<latest>/mc2_03.log | head -10
```

Expected: >=1 line if user Shift+clicked empty terrain at least once.

- [ ] **Step 4: Gate 4 -- substrate OFF, wiring ON (defense-in-depth short-circuit)**

```bash
MC2_OBJECT_ID_BUFFER=0 MC2_STATIC_PROP_PICK=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_03
```

Init banner shows `[RENDER_WORLD v1] event=init objectid_buffer=off` and
`[STATIC_PROP_PICK v1] enabled=1 debug=0`. Even with wiring on, the
helper's `IsObjectIdBufferEnabled()` guard short-circuits BEFORE calling
`lookupAtPixel`. User Shift+clicks several spots:

Verification:

```bash
grep '\[STATIC_PROP_PICK v1\] (hit|miss)' tests/smoke/artifacts/<latest>/mc2_03.log
grep '\[RENDER_WORLD v1\] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0' tests/smoke/artifacts/<latest>/mc2_03.log
```

Expected: BOTH greps return ZERO matches. The defense-in-depth short-
circuit holds: no readback attempted, no warn line emitted, no hit/miss
emitted.

- [ ] **Step 5: Mission-load-clear canary (Q2 wiring sanity)**

User scenario: enable env-ON full stack, Shift+click a building in mc2_03
to populate `s_lastStaticPropPick`, then mission-end (smoke harness will
auto-load the next mission). The `[RENDER_WORLD v1] event=destroy` line
fires between missions. Per Task 3 wiring, `clearLastStaticPropPick`
runs at that point; the next mission starts with `valid=false`.

This is observable only via the absence of a stale-handle log line; the
Q2 wiring is otherwise invisible. Sanity check: no crash on
mission-boundary Shift+clicks (the canary runs across multiple
missions in tier1).

- [ ] **Step 6: Record validation results in commit message (not a code commit; documentation-only)**

Capture the canary outcomes in the worktree CLAUDE.md update at Task 7.
The pre-execution gates list (top of plan + spec Section 10) is the
checklist; mark each gate PASS / FAIL.

---

## Phase D -- Greybeard + worktree CLAUDE.md doc

### Task 6: Greybeard META-FIX vs PATCH ruling

**Files:** none modified. Subagent dispatch only.

Per worktree CLAUDE.md "Meta-fix discipline": before this slice ships,
spawn a fresh greybeard subagent to rule on whether M1.6 is META-FIX
(retires a bug class) or PATCH (local symptom fix).

The dispatch prompt MUST include "run the greybeard skill" verbatim
(per memory rule). The subagent reads with fresh context; do NOT
pre-judge the verdict.

- [ ] **Step 1: Dispatch greybeard subagent**

Use the Agent tool to spawn a fresh subagent with this prompt:

```
Run the greybeard skill against the RenderWorld Slice M1.6
Static-Prop Pick Integration shipping commits (Tasks 1-5 of plan
docs/superpowers/plans/2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md).

The slice wires MC2_OBJECT_ID_BUFFER substrate (M1.5) as a Shift+click
inspection gesture in code/missiongui.cpp. Helper short-circuits when
the legacy click-driven mover-select path consumed the click. No
gameplay verbs; log + debug-state-struct only.

Read the spec at docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
and the M1.5 plan's greybeard ruling section as a reference for the
ruling format.

Rule META-FIX vs PATCH. State the named upstream change that retires
the bug class (if META-FIX) or the debt justification (if PATCH).
```

- [ ] **Step 2: Record ruling**

Write the ruling outcome (META-FIX or PATCH + rationale) into the
Active Campaigns bullet of the worktree CLAUDE.md at Task 7.

Expected lean (do NOT pre-judge): PATCH (justified) -- the slice is
intentionally minimal additive wiring; the upstream META-FIX that
retires the "static props are not click-inspectable" debt class is
M2 (mech IDs into attachment-2) + M1.7 highlight overlay, which are
out of scope for M1.6. Document the debt in the CLAUDE.md bullet.

### Task 7: Document slice in worktree CLAUDE.md

**Files:**
- Modify: `.claude/worktrees/nifty-mendeleev/CLAUDE.md`

- [ ] **Step 1: Re-read the existing M1.5 Active Campaigns bullet**

```bash
grep -n "RenderWorld Slice M1.5\|RenderWorld Slice M1\b" /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
```

Expected: M1 and M1.5 bullets near the end of file under
`## Active campaigns`. M1.6 bullet inserts immediately after the M1.5
entry, mirroring its style (single dense paragraph, ~10 lines, with
spec / plan refs, env vars, validation summary, greybeard ruling).

- [ ] **Step 2: Add the M1.6 Active Campaigns bullet**

Insert immediately after the M1.5 bullet, matching the M1 / M1.5 style
exactly (no emoji, ASCII only, single dense paragraph):

```
- **RenderWorld Slice M1.6** (SHIPPED YYYY-MM-DD): static-prop pick integration. `MC2_STATIC_PROP_PICK=1` (master enable, requires `MC2_OBJECT_ID_BUFFER=1`) and optional `MC2_STATIC_PROP_PICK_DEBUG=1` (verbose miss-log) opt-in stack. Shift+LMB on a static prop -> `RenderWorld::lookupAtPixel(glX, glY)` -> `[STATIC_PROP_PICK v1] hit ...` + `RenderWorld::setLastStaticPropPick(...)`. Mover-first short-circuit: `MissionInterfaceManager::tryStaticPropPick` reads a `moverSelectedThisFrame` local bool set immediately after the 4 setSelected(true) writer sites identified by spec Q6 (`code/missiongui.cpp:1460,1487,1690,1705`); the 4 setSelected(false) sibling sites (`:1462,1483,1692,1701`) are EXPLICITLY left uninstrumented per Q6 rationale. Section 11 + Q8 invariant: legacy Shift+LMB additive-select on a friendly mover is preserved verbatim with ZERO M1.6 log lines. Y-flip at the call site (`glY = Environment.screenHeight - 1 - mouseY`); off-screen bounds guard inside the helper. RenderWorld API: `StaticPropSelectionDebugState` + `setLastStaticPropPick` / `clearLastStaticPropPick` / `getLastStaticPropPick`; `clearLastStaticPropPick` wired into the no-arg `RenderWorld::destroy()` lifecycle hook (`:321`) per spec Q2 (pick state does not persist across mission-load). `[STATIC_PROP_PICK v1] enabled=N debug=N` banner at init. Greybeard ruling YYYY-MM-DD: <META-FIX|PATCH (justified)> -- <rationale, ~one sentence>. Validation gates 1-4 PASS (env-OFF tier1 5/5 pixel-parity at idle vs M1.5 HEAD 842f34f; env-ON mc2_03 canary hit + Section 11 invariant; debug-verbose miss log; substrate-OFF defense-in-depth short-circuit). No gameplay verbs: no `Team::home` mutation, no order issuance, no save-state change, no shader edits. Highlight overlay deferred to M1.7. Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md`.
```

Replace `YYYY-MM-DD` with the actual ship date in both places, and
fill in the greybeard ruling outcome from Task 6.

- [ ] **Step 3: Verify CLAUDE.md line count stays under 200**

Per worktree CLAUDE.md discipline rule: "Keep this file under 200 lines."

```bash
wc -l /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
```

If the M1.6 bullet pushes the file over 200 lines, extract one of the
older Active Campaigns bullets (M1 or M1.5) to a memory file and link
from MEMORY.md per the discipline rule.

- [ ] **Step 4: Commit**

```bash
git add .claude/worktrees/nifty-mendeleev/CLAUDE.md
git commit -m "$(cat <<'EOF'
docs(claude-md): record M1.6 static-prop pick slice (M1.6 Task 7)

Active Campaigns bullet captures: env-flag stack, the 4-site Q6
mover gate (with explicit Q6-RESOLVED uninstrumented siblings),
Section 11 + Q8 invariant, Y-flip + bounds guard, RenderWorld API
additions, Q2 mission-end clear wiring, greybeard ruling, and the
four validation gate outcomes.

Spec: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
Plan: 2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Pre-execution gates (must-pass before merge)

1. **Firewall script clean.** `sh scripts/check-include-firewall.sh` exits 0. (M1.6 reaches `code/missiongui.cpp` -> `RenderWorld/RenderWorld.h`; permitted by M1's firewall section 12.)
2. **Tier1 5/5 env-OFF pixel-parity at idle vs M1.5 HEAD `842f34f`** (or `f1c0cab` for timing-baseline parity). Helper adds 1 CALL/RET per click event but is short-circuited at the env-OFF check; rendering output and selection state at idle frames are identical. Task 4 Step 17 confirms.
3. **Env-ON Shift+click canary on mc2_03**: at least one `[STATIC_PROP_PICK v1] hit` log line with valid handle + idx + gen + recipe fields populated.
4. **Section 11 invariant canary**: Shift+LMB on a friendly mover toggles the legacy additive-select AND emits ZERO `[STATIC_PROP_PICK v1]` log lines on those clicks.
5. **Env-ON Shift+LMB on empty terrain**: `[STATIC_PROP_PICK v1] miss` line emits ONLY when `MC2_STATIC_PROP_PICK_DEBUG=1`; ZERO lines emitted at default debug verbosity.
6. **Substrate-OFF defense-in-depth canary**: with `MC2_OBJECT_ID_BUFFER=0 MC2_STATIC_PROP_PICK=1`, ZERO `[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0` lines AND ZERO `[STATIC_PROP_PICK v1] hit` / `miss` lines.
7. **Greybeard ruling recorded** in the CLAUDE.md Active Campaigns bullet (Task 7).
8. **Grep gates pass**: exactly 4 `moverSelectedThisFrame = true` writes; exactly 2 `moverSelectedThisFrame = false` declarations; exactly 3 `tryStaticPropPick(` references (one definition + two call sites).

---

## Risks & mitigations (R1-R5)

### R1. Shift+LMB shadowing legacy additive-select gesture

**Risk:** the M1.6 helper fires on a Shift+LMB that the legacy path
intended as additive-select on a friendly mover. User sees both the
selection toggle AND a M1.6 log line, or worse, the M1.6 log line
without the toggle.

**Mitigation:** the Q6 4-site `moverSelectedThisFrame` observable +
the Section 11 invariant short-circuit in `tryStaticPropPick`. Task 5
Step 2 + Step 3 canaries verify the invariant holds in both default
and debug-verbose modes.

**Residual:** if the spec's 4-site Q6 census missed a 5th
`setSelected(true)` site in either style body, the invariant breaks
at that 5th site. Task 4 Step 5 grep-gate catches this at write time.

### R2. Y-flip wrong-direction at the call site

**Risk:** clicking on the sky reads a building pixel (or vice versa)
because the `glY = Environment.screenHeight - 1 - mouseY` translation
is inverted, off-by-one, or omitted.

**Mitigation:** the translation lives at the call site only (M1.5
contract: lookupAtPixel takes GL-convention coords as-is). The Task 5
Step 2 canary catches an inverted flip immediately -- the user
Shift+clicks the bottom of the screen and expects bottom-screen objects;
an inverted flip returns top-screen objects.

**Residual:** fractional / DPI scaling. Engine canvas is 800x600 logical
(per worktree CLAUDE.md `options_cfg_resolution_drift` note); the
`Environment.screenHeight` we read is the matching logical height, NOT
a Win32 `GetClientRect`-derived dimension. Spec Section 5 documents
this; the helper does not need to handle it.

### R3. `lookupAtPixel` synchronous-readback stall

**Risk:** the synchronous `glReadPixels` inside `lookupAtPixel` stalls
the GPU ~1-5ms per call. M1.6 fires this at click-time (~ <=10 Hz peak
under aggressive Shift+clicking), so the budget is per-click, not
per-frame.

**Mitigation:** the helper is gated on `leftClicked` (edge-triggered)
and `shiftDn` (level). The user cannot induce per-frame readbacks via
held-click; legacy `isLeftClick` semantics auto-edge-trigger. Per-frame
hover-preview is explicitly forbidden in spec Section 3.

**Residual:** none of consequence. Async PBO readback is M1.7+ territory
when hover-preview lands.

### R4. Cleanup on mission unload

**Risk:** a stale `StaticPropSelectionDebugState` survives the
per-mission `RenderWorld::destroy()` and points into a cleared
`s_objectRecords` table. The next mission's `getLastStaticPropPick()`
consumer sees `valid=true` but the handle generation check fails.

**Mitigation:** Task 3 wires `clearLastStaticPropPick()` into the
no-arg `RenderWorld::destroy()` lifecycle hook at `RenderWorld.cpp:321`.
Task 5 Step 5 sanity-canary verifies no crash on mission-boundary
Shift+clicks.

**Residual:** Q2 sanity is enforced by the Task 3 wiring. The M1
lifecycle hook (`code/mission.cpp:3279`) is the canonical mission-end
point; if M2 introduces a second exit path that bypasses this hook,
the new exit path needs to call `clearLastStaticPropPick()` too. Not
M1.6's concern.

### R5. Existing `tryStaticPropPick` symbol collision

**Risk:** the chosen helper name collides with an existing symbol.

**Mitigation:** Task 4 Step 5 greps for `tryStaticPropPick` before
writing the function and expects zero hits. Grep at plan-write time
(2026-05-23) confirms zero hits across `code/`, `RenderWorld/`,
`mclib/`, `GameOS/gameos/`.

**Residual:** none.

---

## Critical constraints (read before every commit)

- **ASCII only, no emoji** in any file ever. Worktree CLAUDE.md rule.
- **Use Edit tool, not Write** for modifications to existing files. Write is for new files only; M1.6 creates no new files.
- **Full relink rule** when missiongui.cpp class layout changes: `rm -f build64/RelWithDebInfo/mc2.exe` BEFORE `cmake --build`. Task 4 Step 16 makes this explicit. Adding a private method does not change object layout in MSVC's normal memory model, but a full relink is the safety net per worktree CLAUDE.md "Full relink before deploy" rule.
- **Canonical smoke command** verbatim:
  ```
  py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
  ```
- **Canonical build command** verbatim (NO `-- /m`):
  ```
  "/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
  ```
- **Grep-before-cite at write time.** Every file:line in this plan was grep-verified 2026-05-23 against HEAD `842f34f`. Re-verify before each task's Step 1.
- **Do NOT touch** order issuance, drag-rect selection, right-click dispatch, double-click semantics, or any gameplay logic. Spec Section 11 hard non-goal. M1.6 patch budget is: 4 single-line `= true` writes, 2 local `= false` declarations, 2 helper calls at style tails, 1 helper definition, 1 helper declaration, 6 RenderWorld API additions (3 free functions + 1 type + 2 env-flag accessors), 1 banner extension line, 1 destroy-hook wiring line, 1 CLAUDE.md bullet. If the patch grows beyond this surface, it stops being M1.6 and starts being M1.7+.

---

## Task summary

| Task | Phase | Files | Commit boundary |
|------|-------|-------|-----------------|
| 1 | A | `RenderWorld/{RenderWorld.h, RenderWorld.cpp}` | env-flag accessors + init banner extension |
| 2 | A | `RenderWorld/{RenderWorld.h, RenderWorld.cpp}` | StaticPropSelectionDebugState + 3 free functions |
| 3 | A | `RenderWorld/RenderWorld.cpp` | clearLastStaticPropPick() wired into destroy() |
| 4 | B | `code/{missiongui.h, missiongui.cpp}` | ATOMIC: helper definition + declaration + 4 instrumentations + 2 call sites |
| 5 | C | (validation only; no edits) | (no commit; record results in Task 7) |
| 6 | D | (greybeard subagent dispatch) | (no commit; ruling recorded in Task 7) |
| 7 | D | `.claude/worktrees/nifty-mendeleev/CLAUDE.md` | Active campaigns bullet + greybeard ruling |

Total commits: 5 (Tasks 1, 2, 3, 4, 7). Total task count: 7. (Plan adversarial review M1 collapsed previous T4+T5 into single atomic NEW T4.)

---

## Operational notes for the executing agent

- If file reading exceeds 15 minutes without writing, STOP and report BLOCKED. Targeted greps + spot reads sufficient.
- Re-grep cited file:line offsets at the start of each task (Step 1 in every task). The audit was 2026-05-23 against HEAD `842f34f`; line numbers drift.
- Phase A tasks (1-3) are independently committable; if any of them fails its env-OFF tier1 gate, STOP and investigate before proceeding.
- Phase B Task 4 is the load-bearing atomic commit. If Task 4 Step 17 (env-OFF pixel-parity at idle) fails, the patch is wrong somewhere -- the helper should be dormant when `MC2_STATIC_PROP_PICK=0`. Bisect by selectively reverting the 4 instrumentations + 2 call sites until parity returns; the surviving change is the culprit.
- Task 5 validation gates are user-driven (per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN"). The user Shift+clicks the running game window and reports observations; do NOT ask the user to "re-run with X env var" or "confirm Y" in artificial loops.
- Task 6 greybeard ruling is a fresh-context subagent dispatch. Do NOT pre-judge the verdict; record whatever the subagent rules.
