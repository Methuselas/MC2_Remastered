# RenderWorld Slice M1.6 -- Static-Prop Pick Integration Spec

- Status: EXECUTABLE-READY (adversarial review applied; greybeard pass
  deferred to first slice execution per skill scope)
- Date: 2026-05-23
- Adversarial review: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-spec-adversarial-review.md`
  (1 CRIT / 2 MAJOR / 6 MINOR; all resolved 2026-05-23)
- Predecessor slice: RenderWorld Slice M1.5 SHIPPED 2026-05-23
  (`842f34f`). Substrate is live: `MC2_OBJECT_ID_BUFFER=1` populates
  `GL_COLOR_ATTACHMENT2` with `Handle.raw()` for static-prop pixels,
  and `RenderWorld::lookupAtPixel(screenX, screenY)` returns a
  validated `LookupResult` (generation + alive check).
- Parent spec: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`
  (Section 8 hybrid picking lifecycle -- M1.5-deferred to this slice).
- Sibling spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`
  (Section 8 retained the lifecycle design for M1.6 reference).
- Required follow-ups before EXECUTABLE:
  - adversarial-plan-review pass (per worktree CLAUDE.md "Review discipline")
  - greybeard skill pass (per worktree CLAUDE.md "Meta-fix discipline")
- DOC-ONLY: no code in this artifact. Pseudocode for the click-handler
  dispatch and the `StaticPropSelectionDebugState` struct only.

This document specifies the **gameplay-side wiring** that exposes the
M1.5 substrate as a user-visible (env-gated) selection gesture. It does
NOT introduce new render-pipeline state. It is a small, surgical edit
to the existing mission-input dispatch path plus a tiny RenderWorld-
side debug-state holder. The substrate already works; M1.6 just calls
into it from one new code path.

---

## 1. Purpose / non-goals

### Purpose

Wire `RenderWorld::lookupAtPixel` into the gameplay selection path as
an **env-gated, modifier-only, mover-second fallback** so that static
props become inspectable in the live game without an editor. This is
the smallest slice that turns the M1.5 substrate from "debuggable via
console" into "selectable via mouse" -- without yet committing to any
gameplay semantics for static props (no orders, no attack targets, no
mission-state mutation).

### Non-goals (explicit)

- **Not mover selection replacement.** Legacy mover selection in
  `selectVisible` / `updateOldStyle` / `updateAOEStyle` is untouched.
  Plain left-click stays exactly as it is today. Drag-rectangle
  selection stays mover-only.
- **Not gameplay verbs.** The picked static prop is NOT a tactical
  target. No order issuance, no attack assignment, no path planning.
  See Section 11.
- **Not mech-pick.** M2 is the slice that writes mech IDs to
  attachment-2. Until then, the ID buffer's mech pixels read as
  static-prop handles from props *behind* the mech -- which is exactly
  why M1.6 must be **fallback only** after legacy mover picking.
- **Not drag-rect static-prop picking.** Single Shift+left-click only.
- **Not double-click semantics.** Single click; re-clicking on the
  same prop just re-emits the log line (idempotent).
- **Not right-click.** Right-click stays order-issuance, untouched.
- **Not multi-select.** Shift+click does NOT additively select a prop
  alongside movers. The static-prop pick lives in a separate
  inspection-state slot.
- **Not a highlight box (in M1.6).** A visible bounds overlay is
  M1.7+. M1.6 emits a log line and updates a debug-state struct only.
  See Section 8 and Open Question Q3.
- **Not async readback.** `lookupAtPixel` already does synchronous
  single-pixel `glReadPixels`. M1.6 calls it once per Shift+click;
  the stall budget is per-click, not per-frame.

### Open questions (carry to next pass)

See Section 13.

---

## 2. Relationship to M1.5 substrate + M2 mech extension

### M1.5 substrate (SHIPPED 2026-05-23)

The substrate M1.6 depends on:

- `MC2_OBJECT_ID_BUFFER=1` enables `R32_UINT` attachment at
  `GL_COLOR_ATTACHMENT2` of the main scene FBO.
- Static-prop fragment shader writes `Handle.raw()` via
  `layout(location=2) out uint v_objectId`.
- `RenderWorld::lookupAtPixel(screenX, screenY)` does a synchronous
  `glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)` on attachment-2,
  validates against `s_objectRecords` (mutex-guarded, generation +
  alive check), returns `LookupResult{isValid, handle, recipeIndex,
  ...}`. Implementation at `RenderWorld/RenderWorld.cpp:448-510`
  (verified by grep at write time).
- `[RENDER_WORLD v1] WARN: lookupAtPixel called with
  MC2_OBJECT_ID_BUFFER=0` warning fires once per session if called
  with the env off.
- The per-mission `RenderWorld::destroy()` hook (M1 wires this at
  `code/mission.cpp:3279`) already clears recipe state; M1.6 hooks
  into the same lifecycle for its debug-state struct (Q2).

### Why M1.6 must be mover-second fallback (load-bearing)

M1.5 only writes attachment-2 from the static-prop fragment path.
Mechs, terrain, VFX, immediate-mode passes do NOT write IDs in M1.5
-- they leave attachment-2 cleared to `Handle::invalid()` (raw=0)
along the pass-clear path, BUT mover/mech pixels that overdraw a
prop pixel do not RE-write attachment-2 to clear. The hardware MRT
state is "writes from the static-prop pass only"; if a mech stands
in front of a building, the depth test in the mover pass discards
the building's color/depth but does NOT touch attachment-2.

Concretely: at a pixel where a mech is visually in front of a
building, attachment-2 may still contain the building's handle from
the static-prop pass, because the static-prop pass ran first and
mover passes do not write attachment-2. `lookupAtPixel` at that
pixel returns the BUILDING handle, not "no hit". The fix is M2 (mech
IDs in attachment-2 with proper depth/Z ordering), not M1.6.

Until M2 closes the coverage gap, **GPU static-prop pickup MUST be a
fallback after legacy mover picking** so that clicking on a mech
selects the mech (legacy path) and never accidentally selects the
building behind it (substrate path).

### M2 extension path

When M2 lands (mech IDs in attachment-2 with proper depth-ordered
writes), the fallback ordering in this spec can be revisited. The
likely shape:

- Mover pick still runs first (it's cheap, it's the gameplay-default).
- If mover pick misses AND substrate is enabled AND modifier is held,
  `lookupAtPixel` becomes authoritative for both mech and prop.
- The "prop behind mech" hazard goes away because attachment-2 will
  reflect mech IDs at mech-front pixels.

M2 is OUT of scope for M1.6 and is mentioned only to clarify why
M1.6's fallback ordering is correct *in the substrate's current
shape* and is not load-bearing forever.

---

## 3. Input handling (Shift + left-click in missiongui)

### Verified context (grep at write time)

Code paths confirmed against
`code/missiongui.cpp` HEAD:

- `userInput->shift()` is the canonical modifier accessor; already
  used in the main per-frame input gather inside
  `MissionInterfaceManager::update*` callers. Grep-confirmed sites
  at write time: `code/missiongui.cpp:599`, `:1739`, `:5490` (also
  `:3682` as a negated check), plus `code/optionsarea.cpp:1255`.
  Semantics: `userInput->shift()` is **level-driven** ("shift
  currently held") -- `mclib/userinput.h:359-362` returns
  `leftShift() || rightShift()`, both of which call
  `getKeyDown(KEY_LSHIFT)` (state, not edge). Combined with
  `leftClicked` (already edge-triggered via `isLeftClick()`) the
  composite gesture fires once per click while shift is depressed.
  Do NOT change either side of this composition.
- `shiftDn` is already plumbed through to both style branches at
  lines 904 (`updateAOEStyle(shiftDn, altDn, ctrlDn, ...)`) and 926
  (`updateOldStyle(shiftDn, altDn, ctrlDn, ...)`).
- Both styles already use `shiftDn` for gameplay verbs (e.g. line
  1447 `if (shiftDn && target->getCommanderId() == ...)` in
  `updateOldStyle`; line 1677 `if (shiftDn)` in `updateAOEStyle`),
  so the spec adds a new conditional alongside existing ones, not a
  new modifier-detection mechanism.
- `leftClicked` is computed identically in both styles:
  `(!userInput->isLeftDrag() && !userInput->isRightDrag() &&
  userInput->isLeftClick())`. M1.6 reuses this boolean.
- `mouseX = userInput->getMouseX()`; `mouseY = userInput->getMouseY()`
  -- gathered at the top of the per-frame update path (lines 719-720).
  Win32 convention: origin top-left.
- The existing `selectVisible()` symbol at line 2124 is a
  hotkey-driven select-all-visible action (`KEY_E` at line 210). It
  is NOT the click-driven mover-selection path. The actual
  click-driven mover selection is inside the `updateOldStyle` /
  `updateAOEStyle` bodies and the `update()` helper -- a mix of
  cursor-state, drag, and `pTeam`-iterating logic that this spec
  intentionally does NOT modify. See Section 4.

### Detection condition

A static-prop pick attempt MUST fire only when ALL of the following
are true at the same frame:

1. `MC2_OBJECT_ID_BUFFER` env is enabled at startup (cached bool, the
   same gate `lookupAtPixel` itself checks).
2. `MC2_RENDER_WORLD_STATIC_PROP_PICK` env is enabled (the M1.6
   opt-in; default OFF -- see Section 9). Strawman name; rename if
   the brainstorm pass picks something shorter.
3. `userInput->shift()` returned true this frame (`shiftDn`).
4. `leftClicked` is true AND `!userInput->isLeftDrag()` (already in
   the `leftClicked` expression) AND `!bLeftDouble` (double-click is
   handled by the legacy path).
5. Cursor is NOT in the control GUI region (`!bGui`).
6. The legacy mover-selection path **did not select a mover this
   frame** -- the fallback gate (Section 4).

### Pseudocode (illustrative; NOT the final patch)

Inserted near the existing post-`updateOldStyle` /
`updateAOEStyle` tail in `MissionInterfaceManager::update*` (call
site picked at plan time; see Section 4 for ordering constraints):

```text
// After legacy mover selection has run for this frame.
if (g_objectIdBufferEnabled
    && g_staticPropPickEnabled
    && shiftDn
    && leftClicked
    && !bLeftDouble
    && !bGui
    && !moverWasSelectedThisFrame)
{
    int glX = mouseX;
    int glY = Environment.screenHeight - 1 - mouseY;   // see Section 5
    RenderWorld::LookupResult res =
        RenderWorld::lookupAtPixel(glX, glY);
    if (res.isValid) {
        RenderWorld::setLastStaticPropPick(res);
        std::fprintf(stderr,
            "[STATIC_PROP_PICK v1] hit handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d)\n",
            res.handle.bits, res.handle.index(),
            res.handle.generation(),
            res.recipeIndex, mouseX, mouseY, glX, glY);
    } else {
        RenderWorld::clearLastStaticPropPick();   // Q1 lean: yes
        if (g_staticPropPickDebug) {
            std::fprintf(stderr,
                "[STATIC_PROP_PICK v1] miss screen=(%d,%d) gl=(%d,%d)\n",
                mouseX, mouseY, glX, glY);
        }
    }
}
```

Notes:

- This is one new conditional block. It does NOT modify any existing
  if/else around mover selection.
- It runs AFTER the legacy mover path. It runs at most once per
  frame because it is gated on `leftClicked`, which the legacy path
  already consumes once-per-event.
- All env gates are cached booleans read at startup, not
  `getenv` per call. The `MC2_OBJECT_ID_BUFFER` cache already exists
  (M1.5).

### Forbidden additions

- Do NOT add a "shift held + mouse hover" preview probe. That would
  call `lookupAtPixel` every frame and silently 1-2ms stall the
  pipeline (synchronous readback).
- Do NOT add new modifier-detection helpers; reuse `userInput->shift()`.
- Do NOT change `userInput->isLeftClick()` semantics.
- Do NOT route the pick through `update()`'s `pTarget` argument --
  the static-prop pick is NOT a `GameObject*` and never will be in
  M1.6.

---

## 4. Selection priority + fallback order

The required per-frame ordering inside the existing missiongui input
dispatch:

1. **Legacy mover / unit selection runs.** Existing
   `updateOldStyle` or `updateAOEStyle` body executes as today. This
   may set / clear `Team::home`'s selection list (the actual
   gameplay selection state). The plan-stage author must identify
   the precise observable: "did a mover get selected THIS frame as
   a result of this click?" -- candidates:
   - a freshly-set selected-mover count delta against the previous
     frame's count
   - a freshly-set `lastSelectedMover` pointer
   - a freshly-fired selection event (if missiongui emits one)
   - The plan must pick one and justify it.
2. **Substrate-fallback gate.** Only if no mover was selected as a
   result of this click AND `MC2_OBJECT_ID_BUFFER=1` AND
   `MC2_RENDER_WORLD_STATIC_PROP_PICK=1` AND `shiftDn`, call
   `lookupAtPixel`.
3. **Inspect / set debug state.** If the lookup returned valid,
   update `StaticPropSelectionDebugState` and emit the
   `[STATIC_PROP_PICK v1] hit` log line.
4. **Empty-click fall-through.** If lookup invalid AND no mover
   selected, behavior matches the existing empty-click path. The
   spec's `clearLastStaticPropPick` (Q1 lean: yes) is the only new
   state mutation.

### Why fallback (not preempt)

Per Section 2: until M2 writes mech IDs into attachment-2, a mech
in front of a building gives a building handle at the mech's
pixels. Preempting the mover path with `lookupAtPixel` would let
the user "pick the building behind the mech" by Shift+clicking the
mech -- a user-visible incoherence. Fallback ordering means
Shift+click on a mech always selects the mech (legacy path wins),
and the substrate only fires when the mover path had nothing to
select.

### Why "this frame" semantics

The legacy mover code maintains running state (drag, double-click,
add-to-selection). The "did a mover get selected as a result of
THIS click" observable must be frame-local. If the plan cannot
isolate a clean per-frame observable, the safest fallback is
"selection-list size strictly increased this frame OR a new
last-selected mover was set this frame".

---

## 5. lookupAtPixel integration (mouse coords -> GL screen coords)

### The Y-flip (load-bearing)

- `userInput->getMouseX/Y()` returns Win32-convention coordinates:
  origin top-left, Y grows down.
- `RenderWorld::lookupAtPixel(screenX, screenY)` forwards its args
  directly to `glReadPixels(screenX, screenY, 1, 1, ...)` at
  `RenderWorld/RenderWorld.cpp:484` (verified by grep at write
  time). `glReadPixels` is GL convention: origin bottom-left, Y
  grows up.
- The call site MUST translate. Established convention exists in
  the codebase: `mclib/mouse.cpp:225` and `mclib/utilities.cpp:111-115`
  use `Environment.screenHeight - 1 - y` for this kind of
  inversion.

### Required translation

```text
int glX = mouseX;
int glY = Environment.screenHeight - 1 - mouseY;
```

### Edge cases

- Off-screen mouse (`mouseX < 0`, `mouseY < 0`, `mouseX >= width`,
  `mouseY >= height`): bail before calling `lookupAtPixel`. The GL
  spec allows `glReadPixels` to clip silently but we want a no-op,
  not a "the framebuffer happened to have a zero at this clipped
  pixel" miss. Plan stage: add explicit bounds check.
- Fractional / DPI scaling: missiongui mouse coords are already in
  the engine's logical canvas. `Environment.screenHeight` is the
  matching logical height (see worktree CLAUDE.md note on
  options.cfg / 800x600 canvas). Do NOT use Win32
  `GetClientRect`-derived dimensions.
- Window-minimized / framebuffer-resized: `lookupAtPixel` already
  bails on `fbo == 0 || tex == 0`. M1.6 inherits that guard.

### What NOT to do

- Do NOT pre-translate inside `lookupAtPixel`. The GL contract for
  that function is "I take GL convention coords." Translating
  inside silently changes the M1.5 contract and breaks the existing
  self-test sampler (`[OBJECT_ID_SELFTEST v1]`) which already passes
  GL coords directly.
- Do NOT route mouse coords through `Stuff::Vector2DOf<long>` for
  this -- that detour is reserved for the inverseProject world
  path.
- Do NOT expand the click point into a 3x3 (or larger)
  neighborhood scan. **M1.6 samples a single pixel only.**
  Thin-prop edge misses are accepted; users can retry with a 1-2px
  cursor adjustment. A neighborhood scan multiplies the readback
  stall cost by 9x for marginal gain and is M1.7+ territory if
  usability data demands it. (The M1.5 self-test's 10-pixel
  pattern is a deterministic substrate validator, not a user
  pick-affordance.)

---

## 6. StaticPropSelectionDebugState (what we track)

A tiny RenderWorld-side struct holding the result of the most recent
successful static-prop pick. Lives in `RenderWorld/RenderWorld.cpp`
anon namespace (mirrors the lifecycle of `s_objectRecords`).

### Shape (pseudocode)

```text
struct StaticPropSelectionDebugState {
    bool                         valid;
    RenderCore::RenderObjectHandle handle;
    int                          recipeIndex;
    int                          lastPickMouseX;     // top-left
    int                          lastPickMouseY;     // top-left
    int                          lastPickGlX;
    int                          lastPickGlY;
    uint64_t                     lastPickFrameIndex; // for staleness
};

// Lives in anon namespace. Single global; mutex-guarded ONLY if M2
// ever calls this off-thread. M1.6 is main-thread only.
static StaticPropSelectionDebugState s_lastStaticPropPick;

// Public accessors in RenderWorld namespace.
void setLastStaticPropPick(const LookupResult& res, int mouseX,
                           int mouseY, int glX, int glY);
void clearLastStaticPropPick();
const StaticPropSelectionDebugState& getLastStaticPropPick();
```

### Lifecycle

- Initialized to `valid=false` at static-init.
- `setLastStaticPropPick` populates from a valid `LookupResult` and
  sets `lastPickFrameIndex` from the current frame counter.
- `clearLastStaticPropPick` resets `valid=false`. Called on empty
  Shift+click per Q1 lean.
- The per-mission `RenderWorld::destroy()` hook (M1 wires this at
  `code/mission.cpp:3279`) MUST call `clearLastStaticPropPick()` so
  a mid-pick mission-end does not leave a dangling handle (which
  would point into the cleared `s_objectRecords` and fail the
  generation check -- safe, but confusing if logged).
- The struct is NOT serialized into save/load. Picks do not persist
  across the per-mission `RenderWorld::destroy()` hook per Q2 lean.

### Why RenderWorld-side, not missiongui-side

- Keeps the missiongui patch tiny (one log line + one accessor call).
- Centralizes the "what was the last picked thing" question on the
  side that owns the handle table. M1.7+ may add UI surfaces that
  read this without dragging in `missiongui.h`.
- Mirrors how M1.5 left the `LookupResult` shape on the RenderWorld
  side; M1.6 just stores the most recent one.
- **Firewall**: the M1 firewall script
  (`scripts/check-include-firewall.sh`) prohibits `missiongui` from
  including RenderWorld internals. A missiongui-side cache would
  require either type duplication of `LookupResult` /
  `RenderObjectHandle` (data drift) or a firewall exemption
  (boundary erosion). Putting the cache on the RenderWorld side --
  where `s_objectRecords` already lives as a mutex-guarded table
  and `LookupResult` is the native type -- keeps the boundary
  clean.

### Why NOT push it into Team / SelectionManager

- Team / SelectionManager are tactical-selection owners. Putting a
  non-`GameObject` in there would require new conditional code in
  every "what is selected?" consumer. M1.6 must NOT do this --
  see Section 11 "Forbidden behaviors."

---

## 7. [STATIC_PROP_PICK v1] log schema

Per worktree CLAUDE.md "Debug instrumentation rule": any
lifecycle-touching slice ships env-gated `[SUBSYSTEM v1]` lines in
the same commit. M1.6 follows the same convention.

### Two log lines

```
[STATIC_PROP_PICK v1] hit handle=<uint32> idx=<uint> gen=<uint>
  recipe=<int> screen=(<x>,<y>) gl=(<x>,<y>)

[STATIC_PROP_PICK v1] miss screen=(<x>,<y>) gl=(<x>,<y>)
```

Rules:

- **`hit`** fires every successful pick, unconditionally (no env
  gate beyond the M1.6 enable bool -- if M1.6 ran the lookup at all,
  the user expressed intent; we owe them visibility).
- **`miss`** fires ONLY when `MC2_STATIC_PROP_PICK_DEBUG=1` (Section
  9). Plain "Shift+click empty space" is a high-frequency gesture
  and must not spam.
- One line per gesture; no per-frame repetition. The trigger is
  edge-driven (`leftClicked && shiftDn`), not level-driven.
- Field order is fixed. The schema version `v1` lets us extend
  later without breaking grep-based log analysis.

### Schema-version discipline

Per the worktree's `[SUBSYS v[0-9]+\]` convention: a future spec
that adds e.g. "bounds" to the hit line bumps the schema to `v2`
and adds the new fields at the END. Existing fields keep their
position and meaning.

### Grep test for plans

The verification gate in Section 10 runs:

```
grep -E '^\[STATIC_PROP_PICK v1\] (hit|miss) ' <smoke-log>
```

on env-ON smokes to confirm format.

---

## 8. Optional debug highlight (deferred or minimal)

### Lean: defer to M1.7

M1.6 emits a log line and updates `StaticPropSelectionDebugState`
only. No on-screen highlight, no bounds box, no color tint on the
picked prop.

### Why defer

- The recipe handle is sufficient for log-driven inspection.
- Drawing a screen-rect overlay requires either (a) projecting the
  prop's `TG_MultiShape` AABB through the current view matrix
  (cross-frame state hazard if the click and the next draw straddle
  a `worldToClip` update) or (b) a fragment-shader tint that
  branches on `v_objectId == selectedHandle.raw()` (which is a
  shader edit -- minor, but bigger blast radius than M1.6
  warrants).
- The substrate is the load-bearing deliverable. A highlight is
  ergonomics polish that benefits from a separate
  brainstorm/greybeard pass.

### If implemented anyway (alternative scope)

If reviewer disagrees and wants a minimal highlight in M1.6:

- Minimum viable: a 1-pixel screen-space rectangle drawn over the
  prop's bounding-box projection, rendered as a final overlay
  pass in `gos_postprocess.cpp` after all scene draws. Source
  bounds from the recipe via a new
  `RenderWorld::getRecipeWorldBounds(recipeIndex) -> Bounds` helper.
- This requires (a) `TG_MultiShape` bounds plumbed onto the recipe
  (currently lives in the legacy registry, not the RenderWorld
  POD mirror -- M1.7 work), (b) a new shader, (c) a new uniform
  upload path. **Not in M1.6 by default.**

See Open Question Q3.

---

## 9. Env gating (default OFF; opt-in stack)

### The two new env vars

1. `MC2_RENDER_WORLD_STATIC_PROP_PICK` -- master enable for the
   M1.6 wiring. Default OFF. Even if `MC2_OBJECT_ID_BUFFER=1`, the
   missiongui Shift+click path is dormant unless this is also set.
   Rationale: substrate-vs-feature separation; the substrate is
   already env-gated, and the wiring needs its OWN gate so a user
   can enable the buffer (e.g. for a developer probe) without
   altering input behavior.

   Strawman name. Plan stage may rename to e.g.
   `MC2_STATIC_PROP_PICK=1` for brevity. Whatever the final name,
   it must (a) start with `MC2_`, (b) be a single boolean, (c) be
   read once at startup and cached.

2. `MC2_STATIC_PROP_PICK_DEBUG` -- enables the verbose "miss" log
   line. Independent of `MC2_OBJECT_ID_BUFFER`; when the buffer is
   off, the debug flag has nothing to log because no pick attempt
   ever fires. Default OFF.

### The opt-in stack

| `MC2_OBJECT_ID_BUFFER` | `MC2_RENDER_WORLD_STATIC_PROP_PICK` | `MC2_STATIC_PROP_PICK_DEBUG` | Behavior |
|---|---|---|---|
| 0 (default) | * | * | Exact legacy behavior. Zero new code paths fire. |
| 1 | 0 (default) | * | M1.5 substrate active (selftest, lookupAtPixel debug API). No Shift+click wiring. |
| 1 | 1 | 0 (default) | Shift+click static-prop pick active. `hit` logs only. |
| 1 | 1 | 1 | Shift+click static-prop pick active. `hit` AND `miss` logs. |

### Why three gates, not one

- `MC2_OBJECT_ID_BUFFER` is a *render-pipeline* gate (FBO MRT
  shape, shader writes). Owning team: renderer.
- `MC2_RENDER_WORLD_STATIC_PROP_PICK` is an *input-path* gate
  (missiongui dispatches). Owning team: gameplay.
- `MC2_STATIC_PROP_PICK_DEBUG` is a *log-verbosity* gate.

Conflating these (e.g. "if buffer is on, also wire input") would
make it impossible to ship the buffer to dev/QA for substrate
debugging without surprising someone with new click behavior.

### Boot-time print

On engine init, after the M1.5 `[RENDER_WORLD v1]` banner:

```
[STATIC_PROP_PICK v1] enabled=<0|1> debug=<0|1>
```

Single line; only emitted if M1.5's buffer banner emits. Lets
smoke artifacts confirm the gate state without re-reading env.

---

## 10. Validation gates

### Gate 1: env-OFF parity

With both new env vars at their default OFF:

- Run `tier1` smoke (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`,
  `mc2_24`).
- Verification:
  - `grep '\[STATIC_PROP_PICK v1\]' <log>` returns ZERO matches in
    every run.
  - `grep '\[RENDER_WORLD v1\] WARN: lookupAtPixel called with
    MC2_OBJECT_ID_BUFFER=0' <log>` returns ZERO matches.
  - Tier1 5/5 PASS.
  - No fps delta vs M1.5 baseline (no new per-frame code on the
    default path).

Pass criterion: bit-for-bit identical click behavior vs M1.5 ship.

### Gate 2: env-ON Shift+click canary

With `MC2_OBJECT_ID_BUFFER=1` and `MC2_RENDER_WORLD_STATIC_PROP_PICK=1`:

- Run a manual smoke (user-driven; per worktree CLAUDE.md "Smoke
  sessions are USER-DRIVEN"). User Shift+clicks on a visible static
  prop in `mc2_03` (highest static-prop count: 2552).
- Verification:
  - `grep '\[STATIC_PROP_PICK v1\] hit' <log>` returns at least 1
    match.
  - Each hit's `handle=` value, when looked up in the M1.5 selftest
    log lines, matches an alive recipe.
  - Tier1 5/5 PASS with both env vars on.
- User-driven observation:
  - Plain left-click on a mover still selects the mover.
  - Plain left-click on a static prop does nothing (legacy
    behavior: no mover at that location).
  - Shift+left-click on a static prop emits the log line.
  - Shift+left-click on a mover selects the mover (legacy wins; no
    log line because the fallback gate sees mover was selected).
  - Right-click on a static prop issues an order to the selected
    mover toward that world point -- untouched legacy behavior.

### Gate 3: empty-click fall-through

Shift+left-click on empty terrain (no mover, no prop):

- With `MC2_STATIC_PROP_PICK_DEBUG=0`: no log line. Behavior
  matches legacy empty-click.
- With `MC2_STATIC_PROP_PICK_DEBUG=1`: one `[STATIC_PROP_PICK v1]
  miss` line per click.

### Gate 4: substrate-OFF + wiring-ON sanity

With `MC2_OBJECT_ID_BUFFER=0` and `MC2_RENDER_WORLD_STATIC_PROP_PICK=1`:

- The wiring code MUST short-circuit on the cached
  `g_objectIdBufferEnabled` bool BEFORE calling `lookupAtPixel`.
  The rationale is **defense-in-depth: avoid the FBO-bind +
  `glReadPixels` syscall on every dormant Shift+click**, not "log
  spam." `RenderWorld.cpp:451` already gates the WARN behind
  `static bool warned = false` (once-per-process), so the WARN
  itself would not spam -- the short-circuit avoids the readback
  cost.
- Verification:
  - `grep '\[RENDER_WORLD v1\] WARN' <log>` returns zero matches.
  - `grep '\[STATIC_PROP_PICK v1\] hit' <log>` returns zero
    matches.

---

## 11. Forbidden behaviors (no gameplay verbs in M1.6)

The picked static prop:

- MUST NOT become the argument to any order-issuance code path
  (`giveOrder*`, `addWaypoint*`, `setAttackTarget*`).
- MUST NOT be added to `Team::home`'s selection list.
- MUST NOT block, queue, or otherwise interact with subsequent
  right-click order issuance.
- MUST NOT change mission state, save-game state, or replay state.
- MUST NOT participate in tactical AI (visibility, threat
  assessment, line-of-sight, attack range).
- MUST NOT trigger any sound, voice line, HUD readout, or
  scoreboard event.
- MUST NOT be reachable via the `pTarget` / `GameObject*` argument
  paths in `MissionInterfaceManager::update` -- those are typed for
  game objects, and a static prop is not one.
- **Shift+LMB on friendly mover MUST continue to toggle that mover
  in the selection list (legacy additive-select gesture at
  `code/missiongui.cpp:1447` / `:1677`).** M1.6 emits no
  `[STATIC_PROP_PICK v1]` log line in that case. The plan-stage
  `moverSelectedThisFrame` observable MUST be set inside the
  `setSelected(true)` write at `:1460` / `:1487` / `:1690` / `:1705`
  (the four sites identified in Q6). Tested by a Shift+LMB canary
  on a friendly mover -> expect existing toggle behavior + zero
  M1.6 log lines.

The only effects of a successful pick are:

- One `[STATIC_PROP_PICK v1] hit` stderr line.
- One write to `StaticPropSelectionDebugState`.

That is the entire surface of M1.6.

### Why this matters

Per worktree CLAUDE.md "Change discipline": "don't touch what you
don't have to (every touch has blast radius)." `missiongui.cpp` is
3000+ lines of load-bearing input dispatch. M1.6's edit budget is:

- Exactly one new conditional block in the input dispatch tail.
- Exactly one new stderr log call.
- Exactly one new accessor call into RenderWorld.
- Exactly one new bounds check (off-screen mouse).
- Exactly one new Y-flip expression.

If the patch grows beyond this, it stops being M1.6 and starts
being M1.7+.

---

## 12. M1.7+ extension path

Plausible follow-up slices once M1.6 ships:

- **M1.7a -- Highlight box.** Source AABB from the recipe (requires
  `getRecipeWorldBounds` accessor); project to screen; render
  overlay rect. Either CPU-side `glLineLoop` overlay or
  fragment-shader `v_objectId == selected` tint.
- **M1.7b -- HUD readout.** Read `getLastStaticPropPick()` from a
  HUD frame consumer; print mesh / material / LOD info matching the
  M1.5 inspection promise (per parent boundary spec Section 11
  "pixel -> handle -> mesh / material / LOD / pipeline / packet /
  path chain").
- **M1.7c -- Editor handoff.** If/when an editor inspection surface
  exists, it reads `getLastStaticPropPick()` and pivots focus.
- **M2 -- Mech IDs in attachment-2.** Closes the fallback ordering
  necessity. After M2, the fallback gate in Section 4 can be
  revisited: `lookupAtPixel` may become authoritative for both
  mechs and props once the substrate covers both.
- **M3 -- Terrain IDs.** Closes terrain pixels (currently raw=0
  reads as "background" but is actually "terrain hit"). After M3,
  the empty-click case will distinguish "off-map" from "hit
  terrain at world-point W."
- **Async readback.** Replace the synchronous `glReadPixels` in
  `lookupAtPixel` with a PBO double-buffer path. M1.6 does not need
  this because the call is per-click, not per-frame -- but a
  hover-preview feature would.

---

## 13. Open questions

- **Q1.** Does the spec need a "clear selection" verb when
  Shift+left-clicking empty space?
  - **Lean: yes.** `clearLastStaticPropPick()` on miss keeps the
    debug-state struct semantically clean ("the last gesture was a
    pick attempt that hit nothing"). Alternative: leave the prior
    pick stale and let consumers check `lastPickFrameIndex` for
    staleness. Lean against the alternative because consumers
    haven't been written yet -- M1.6 picks the simpler invariant
    now and lets M1.7 relax it if needed.

- **Q2.** Should `StaticPropSelectionDebugState` persist across
  mission load?
  - **Lean: no.** The per-mission `RenderWorld::destroy()` hook (M1
    wires this at `code/mission.cpp:3279`) already clears the
    recipe / record tables (M1 + M1.5 wiring). The pick state must
    follow -- a stale handle pointing into a cleared record table
    would fail generation check on every read and confuse anyone
    grepping the log post-load. Plan stage: add the
    `clearLastStaticPropPick()` call to the per-mission
    `RenderWorld::destroy()` hook and verify via a savegame-canary
    smoke.

- **Q3.** Highlight box -- defer to M1.7 entirely, or implement a
  minimal "screen-space rect from recipe bounds" overlay?
  - **Lean: defer.** M1.6 is already substrate-wiring + one stderr
    line. Adding an overlay drags in shader work, recipe-bounds
    plumbing, and a new uniform path. Reviewer may overrule; if so,
    Section 8 has the alternative scope sketch.

- **Q4.** What is the actual missiongui click-handler function the
  M1.6 conditional inserts into -- `updateOldStyle`,
  `updateAOEStyle`, or both?
  - **Lean: both, via a shared helper.** The two style branches
    have the same Shift+leftClick semantics for M1.6's purposes
    (the gesture is identical; the cursor / drag logic that
    differs is upstream). Plan stage MUST define
    `MissionInterfaceManager::tryStaticPropPick(shiftDn, leftClicked,
    bGui, mouseX, mouseY)` -- one helper, called once at the tail
    of each style. Avoids two copies of the same conditional.

- **Q5.** Does missiongui already have a "shift held" helper, or
  do we add `userInput->getKeyHeld(KEY_LSHIFT)` style direct check?
  - **Resolved at write time: `userInput->shift()` exists** and is
    used at `code/missiongui.cpp:599` and again at `:1739`. The
    spec uses it as-is. No new helper needed.

- **Q6 RESOLVED 2026-05-23** (adversarial-review-derived 4-site
  census). What is the per-frame observable for "did the legacy
  path select a mover as a result of this click?" (Section 4
  fallback gate)
  - The `moverSelectedThisFrame` observable is set immediately
    after each `setSelected(true)` writer in the click-driven
    mover-select paths. Grep at write time confirms exactly four
    writer sites:

    ```
    code/missiongui.cpp:1460  -- updateOldStyle Shift+additive
    code/missiongui.cpp:1487  -- updateOldStyle plain-LMB-select
    code/missiongui.cpp:1690  -- updateAOEStyle Shift+additive
    code/missiongui.cpp:1705  -- updateAOEStyle plain-LMB-select
    ```

    Plan: declare `bool moverSelectedThisFrame = false;` at the
    top of each style body, set to true immediately after each of
    these four `setSelected(true)` calls (single-line
    instrumentation per site), then the tail of each style body
    calls `tryStaticPropPick(moverSelectedThisFrame, shiftDn,
    mouseX, mouseY)` which short-circuits when
    `moverSelectedThisFrame == true`. Total instrumentation: 4
    single-line sets + 2 helper calls + 1 helper definition.
    Mechanical.

    The set-to-false sites at `:1462`, `:1483`, `:1692`, `:1701`
    are toggle-off / clear-others gestures and MUST NOT set
    `moverSelectedThisFrame = true`. Doing so would misfire the
    fallback gate (e.g. Shift+LMB on already-selected mover would
    deselect AND log a pick). Local-only scope; no new public
    state.

- **Q7.** Should the M1.6 wiring run during paused-without-menu
  state?
  - **Lean: yes.** Inspecting a prop while the game is paused is
    the natural "look at this" gesture. The existing
    `isPaused() && !isPausedWithoutMenu()` checks in the input
    dispatch only suppress gameplay verbs, which M1.6 doesn't have.
    Plan stage: verify by checking whether `leftClicked` is
    suppressed during pause; if so, M1.6 inherits the suppression
    (acceptable -- user can still pick by unpausing).

- **Q8 RESOLVED 2026-05-23** (Section 11 invariant bullet + Q6
  4-site map). Are there in-game contexts where Shift+leftClick
  already has a meaning that M1.6 would shadow?
  - At `code/missiongui.cpp:1447` (`if (shiftDn &&
    target->getCommanderId() == ...)`) and `:1677` (`if (shiftDn)`),
    Shift+click currently has friendly-mover additive-select
    semantics (`addVisibleToSelection` at `KEY_E | SHIFT` line 304
    is the hotkey, not click). The Section 11 forbidden-behaviors
    bullet "Shift+LMB on friendly mover MUST continue to toggle
    that mover" makes preservation explicit; the Q6 4-site map
    instruments `moverSelectedThisFrame` at exactly the four
    setSelected(true) writers; the Section 4 fallback gate
    short-circuits M1.6 when that bool is set. Together: the
    legacy gesture is preserved verbatim, and M1.6 emits no log
    line when the click landed on a friendly mover.

---

## 14. Spec author notes (not part of the contract)

- This spec was written with the M1.5 substrate already shipped at
  `842f34f` and verified live (tier1 5/5 PASS, selftest passing).
  Every cited M1.5 symbol was grep-verified at write time.
- The biggest open ambiguity (Q6) is intentional: it is a
  plan-stage concern, not a spec-stage concern. The spec specifies
  WHAT (mover-first fallback) and the plan picks HOW (which
  observable signals "mover selected this frame").
- The `MissionInterfaceManager::tryStaticPropPick` helper proposed
  in Q4 is a strawman name. The plan may rename.
- No code in this artifact. Pseudocode in Sections 3, 6 is
  illustrative.
- ASCII only; no emoji.
