# RenderWorld Slice M1.6 -- Adversarial Plan Review

- Reviewed: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md` (DRAFT, ~790 lines)
- Predecessor shipped: M1.5 at `842f34f`
- Reviewer: adversarial-plan-review skill, code-grounded against HEAD
- Date: 2026-05-23

## Verdict

**CONDITIONAL PROMOTE.** Spec is structurally sound -- scope is genuinely
small, gates are well-named, the mover-first fallback argument is correct
under the M1.5 substrate shape, and the spec correctly anticipates the
load-bearing Q6 observable as a plan-stage concern rather than papering it
over. However, the spec ships **one CRITICAL fictional symbol**
(`RenderWorld::endMission()`), **one MAJOR shadowing hazard** (existing
Shift+LMB additive-select semantics), and several MINOR items that need
mechanical correction before the plan stage opens. After the listed
mechanical fixes land the spec is EXECUTABLE.

Counts: 1 CRITICAL, 2 MAJOR, 6 MINOR.

---

## CRITICAL

### C1 -- `RenderWorld::endMission()` is fictional

Spec Section 2.1 line 95 ("`RenderWorld::endMission()` already clears
recipe state") and Section 6 Lifecycle line 391 ("`RenderWorld::endMission()`
MUST call `clearLastStaticPropPick()`") and Open Question Q2 line 712
("`RenderWorld::endMission()` already clears the recipe / record tables")
all reference a function that does not exist.

Grep at write time:
- `RenderWorld/RenderWorld.h` declares only `void init();` and
  `void destroy();` (process-/mission-level; see file line 31) and the
  per-handle `void destroy(RenderCore::RenderObjectHandle h);` at line 52.
- `RenderWorld/RenderWorld.cpp` definitions live at line 321 (`destroy()`)
  and 380 (`destroy(handle)`). Zero hits for `endMission` in the entire
  `RenderWorld/` tree.
- The M1 plan (`docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md:199`)
  wires the lifecycle as `RenderWorld::init()` at `code/mission.cpp:1693`
  and `RenderWorld::destroy()` at `code/mission.cpp:3279`. The
  per-mission boundary IS real -- the symbol name is just `destroy()`,
  not `endMission()`.

Impact: a literal reading of Section 6 ("hook into the same lifecycle")
would compile, because the plan author will look at the RenderWorld
header and find `destroy()`. But a less careful plan author could write
a brand-new `endMission()` thinking the spec promises one exists,
introducing an orphan symbol. The Q2 lean ("savegame-canary smoke
verifies `clearLastStaticPropPick()` is called from `endMission()`")
will fail its own gate because the hook point is named differently.

Fix (mechanical): rename `endMission` -> `destroy` (or rephrase to
"the per-mission `RenderWorld::destroy()` hook" since the function name
is overloaded for process+mission lifecycle in the M1 design) in
Sections 2.1, 6, and Q2. Three textual edits.

---

## MAJOR

### M1 -- Shift+LMB on friendly mover already has additive-select semantics; spec underplays the shadowing risk

Spec Section 3 line 154 cites `:1447` and `:1677` and Q8 line 765 calls
the shadow risk "partially resolved." Both are correct that the
fallback gate (mover selected this frame -> skip M1.6) should prevent
shadowing in the normal case. But the *user experience* of Shift+LMB is
already load-bearing for selection management, and spec Section 11
"Forbidden behaviors" never quite says "M1.6 must not change what
Shift+LMB does when it hits a friendly mover."

Grep at write time, with full context read:

- `code/missiongui.cpp:1447-1463` (inside `updateOldStyle`): when
  `shiftDn && target->getCommanderId() == Commander::home->getId()`
  AND target is a friendly mover, the code iterates
  `pTeam->getRosterSize()`, checks `alreadyThere`, and calls
  `target->setSelected(true)` or `setSelected(false)`. This is the
  canonical RTS "Shift+click to add/remove this mover from selection"
  gesture.
- `code/missiongui.cpp:1677-1707` (inside `updateAOEStyle`): identical
  pattern, gated on `selectClicked` instead of `leftMouseReleased()`.

The fallback gate in Section 4 ("mover was selected this frame") will
correctly skip M1.6 *when the gesture lands on a mover*. But the user's
Shift+LMB intent is now AMBIGUOUS by design:

- Shift+LMB on friendly mover: extends/toggles selection (legacy).
- Shift+LMB on enemy/disabled mover: unspecified legacy fall-through.
- Shift+LMB on static prop (M1.6): logs pick.
- Shift+LMB on terrain: nothing (legacy) or `miss` log (M1.6 debug).

This is acceptable as a developer-only inspection gesture (the env-OFF
default makes it dormant for end users), but the spec's Section 11
should explicitly list "Shift+LMB on friendly mover continues to
toggle that mover in the selection list; M1.6 must not log a pick in
that case" -- which is the fallback-gate's job to enforce, but stating
the invariant makes the test gate verifiable.

Fix: extend Section 11 with one bullet: "When Shift+LMB lands on a
friendly mover, the legacy additive-select gesture at `:1447` /
`:1677` runs and M1.6 emits no log line. The plan-stage
`moverSelectedThisFrame` observable MUST be set inside the
`setSelected(true)` site at `:1460` / `:1690` (and any sibling sites
the plan census uncovers)." Promote Q8 from "partially resolved" to
fully resolved with this language.

### M2 -- Q6 observable: spec leans correctly but undercounts the writer sites

Spec Q6 line 743 acknowledges the per-frame "mover selected" observable
is unresolved and lists three candidates. The lean ("introduce
`bool moverSelectedThisFrame`") is the right call. But the spec
implicitly suggests this is a one- or two-site instrumentation:

Grep at write time for the writer sites that mutate selection in the
two style bodies:

- `code/missiongui.cpp:1460` -- `target->setSelected(true)` (Shift+LMB
  additive, OldStyle).
- `code/missiongui.cpp:1462` -- `target->setSelected(false)` (Shift+LMB
  toggle-off, OldStyle).
- `code/missiongui.cpp:1483` -- `pMover->setSelected(false)` (clear
  others, OldStyle non-shift path).
- `code/missiongui.cpp:1487` -- `target->setSelected(true)` (set new
  selection, OldStyle non-shift).
- `code/missiongui.cpp:1690` -- `target->setSelected(true)` (Shift+LMB
  additive, AOEStyle).
- `code/missiongui.cpp:1692` -- `target->setSelected(false)` (Shift+LMB
  toggle-off, AOEStyle).
- `code/missiongui.cpp:1701` -- `pMover->setSelected(false)` (clear
  others, AOEStyle).
- `code/missiongui.cpp:1705` -- `target->setSelected(true)` (set new
  selection, AOEStyle).

That's 8 call sites across two style bodies, not 2. The plan-stage
instrumentation must hit each "set to true" site (4 of them: 1460,
1487, 1690, 1705) -- the set-to-false sites are toggle-off and should
NOT count as "mover selected this frame" because the gesture
deselected. Spec must call this out, or the plan will instrument the
wrong sites and the fallback gate will misfire (e.g., Shift+LMB on
already-selected mover would deselect AND log a pick).

Also, `forcegroupbar.cpp:122` reads `userInput->getKeyDown(KEY_LSHIFT)`
directly for force-group selection -- not in the click-handler path
but adjacent enough that the plan should audit whether force-group
clicks invoke `setSelected` via a separate route. The spec is silent
on force-group interaction.

Fix: rewrite Q6 with the actual 4-writer-sites enumerated:

```
moverSelectedThisFrame should be set true ONLY at:
  - missiongui.cpp:1460  (OldStyle, Shift+LMB add-friendly)
  - missiongui.cpp:1487  (OldStyle, plain-LMB select-friendly)
  - missiongui.cpp:1690  (AOEStyle, Shift+LMB add-friendly)
  - missiongui.cpp:1705  (AOEStyle, plain-LMB select-friendly)
NOT at setSelected(false) sites; those are deselect-gestures.
```

This makes the plan a mechanical patch instead of a fresh audit.

---

## MINOR

### m1 -- "missiongui.cpp:599" cited for the wrong shift-read context

Spec Section 3 line 146-147 says `userInput->shift()` "is used at line
599 (`bool shiftDn = userInput->shift();`) in the main per-frame input
gather inside `MissionInterfaceManager::update*` callers."

Grep confirms `:599` is a hit (`MissionInterfaceManager::update`,
inside the top-of-frame input gather). But the spec also has a second
hit at `:1739` (inside the smaller `update()` helper that the larger
`update*()` calls into), AND a third at `:5490` (a separate context
spec did not cite), AND a fourth at `optionsarea.cpp:1255`. All four
exist. None contradict the spec, but the spec's "599 and 1739" framing
in Section 3 misses `:5490`. Inconsequential for the patch site -- the
plan correctly uses the `:599` `shiftDn` plumbing -- but cite either
all or none to keep grep-discipline tight.

Fix: drop the explicit line-number list from Section 3 ("`shift()` is
the canonical accessor; already used in the main per-frame gather")
or extend it to all four sites.

### m2 -- `shift()` is level-driven; spec is right to want "held," but does not state it

Grep `mclib/userinput.h:359-362`:

```cpp
bool shift (void)
{
    return (leftShift() || rightShift());
}
```

`leftShift()` calls `getKeyDown(KEY_LSHIFT)` which is "key currently
held" (level-driven), not "edge-triggered key-down event." The spec
implicitly assumes this -- gating `leftClicked AND shiftDn` correctly
fires once per click while shift is held, which is exactly the
intended UX. But state the semantic explicitly in Section 3 so a
future maintainer rewriting this doesn't accidentally swap to an
edge-triggered helper:

> `userInput->shift()` returns level-driven "shift currently held";
> combined with `leftClicked` (already edge-triggered via
> `isLeftClick()`) the composite gesture fires once per click while
> shift is depressed. Do NOT change either side of this composition.

### m3 -- Y-flip cited line is correct; `Environment.screenHeight` is correct symbol

Grep at write time:

- `RenderWorld/RenderWorld.cpp:484` -- confirmed
  `glReadPixels(screenX, screenY, 1, 1, GL_RED_INTEGER, ...)`. Spec is
  accurate.
- `Environment.screenHeight` -- confirmed canonical: written to at
  `GameOS/gameos/gameos_graphics.cpp:1719` (`Environment.screenHeight
  = height_`), read at `gameosmain.cpp:309`, used as the logical
  canvas height across `gui/*.cpp`. Defaults `View.cpp:762` (600) and
  `aseconv.cpp:584` (480) further confirm logical-canvas semantics.
- The translation `Environment.screenHeight - 1 - mouseY` is the
  codebase's idiom (cited `mclib/mouse.cpp:225` and `utilities.cpp:111`
  in the spec; not re-greped here but well-established pattern).

Spec is correct. No fix needed -- this entry confirms the Y-flip
section is grep-clean.

### m4 -- Substrate-OFF WARN spam concern is overstated

Spec Gate 4 (Section 10 line 614) treats the
"`[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0`"
spam as load-bearing for the gate.

Grep `RenderWorld/RenderWorld.cpp:450-456`:

```cpp
if (!IsObjectIdBufferEnabled()) {
    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0\n");
        warned = true;
    }
    return out;
}
```

The WARN is once-per-process (`static bool warned`). Even without the
spec's cached-bool short-circuit, the user would see ONE warning, not
spam. Gate 4's `g_objectIdBufferEnabled` short-circuit is still good
defense-in-depth (avoids the FBO bind + glReadPixels call entirely
when the buffer is off), but the "would be log spam" framing is
inaccurate.

Fix: restate Gate 4's rationale as "avoid the FBO-bind + readback
syscall on every dormant Shift+click" rather than "prevent log spam."

### m5 -- `StaticPropSelectionDebugState` location is correct, but justification is thin

Spec Section 6 ("Why RenderWorld-side, not missiongui-side") gives
three reasons. They are all reasonable but the strongest one is not
stated: `s_objectRecords` already lives there as a mutex-guarded
table, AND `LookupResult` is a RenderWorld type. Putting the
"last pick" cache in `missiongui.cpp` would require either copying
the result fields into missiongui-side state (data duplication) or
exposing `RenderWorld::RenderObjectRecord` to `missiongui.h`
(boundary erosion -- M1 firewall script
`scripts/check-include-firewall.sh` exists specifically to prevent
this kind of leak).

Fix (one sentence in Section 6): "The M1 firewall
(`scripts/check-include-firewall.sh`) prohibits missiongui from
including RenderWorld internals; a missiongui-side cache would
require either type duplication or a firewall exemption. Putting
the cache on the RenderWorld side keeps the boundary clean."

### m6 -- Multi-pixel sampling decision is implicit; spec should state it

Spec lean per user note: single-pixel only for M1.6 minimal scope.
Spec Section 5 ("lookupAtPixel integration") says nothing about
multi-pixel sampling. The M1.5 self-test used a deterministic 10-pixel
pattern explicitly because single-pixel can miss thin geometry. The
M1.6 spec inherits single-pixel semantics from `lookupAtPixel` itself
but never names the tradeoff.

Fix: add to Section 5 "What NOT to do":

> Do NOT expand the click point into a 3x3 neighborhood scan. Single
> pixel only for M1.6. Thin-prop edge misses are accepted; users can
> retry with a 1-2px cursor adjustment. A neighborhood scan multiplies
> the readback stall cost by 9x for marginal gain and is M1.7+
> territory if usability data demands it.

---

## Strengths confirmed under grep

These claims were grep-verified and stand without findings:

- Section 3 line 158: `leftClicked = (!isLeftDrag() && !isRightDrag()
  && isLeftClick())` -- confirmed at `:792`, `:1335`, `:1529`. Spec's
  reuse of `leftClicked` correctly inherits drag-rect exclusion.
- Section 3 line 162: `mouseX/mouseY` are gathered at top-of-frame.
  Confirmed `:720-721` -- top of `MissionInterfaceManager::update`.
  Spec's "Win32 origin top-left" claim matches the convention used
  by every other input consumer in the file.
- Section 3 line 166-169: `selectVisible` at `:2124` is the `KEY_E`
  hotkey handler (NOT click-driven). Confirmed: `:210`
  `KEY_E, ..., &MissionInterfaceManager::selectVisible`. The spec
  correctly identifies that click-driven mover selection lives in
  the style bodies and there is NO single named function to wrap.
- Section 3 line 199-225 pseudocode flow: structure matches the
  existing dispatch tail at `:1368` (`leftMouseReleased() &&
  !wasLeftDrag() && !bGui && !lastUpdateDoubleClick`) for the
  OldStyle path. The spec's `!bLeftDouble && !bGui` gating is
  consistent with existing guards.
- Section 9 env-var naming: `MC2_*_DEBUG` is an established
  codebase convention (`MC2_WATER_DEBUG`, `MC2_THIN_DEBUG`,
  `MC2_TERRAIN_DEBUG_MODE`, `MC2_RENDER_WATER_FASTPATH_DEBUG`,
  `MC2_MECH_FRAG_DEBUG`, `MC2_GL_DEBUG`, `MC2_GL_DEBUG_FATAL`).
  `MC2_STATIC_PROP_PICK_DEBUG` fits the pattern. `MC2_OBJECT_ID_BUFFER`
  is the M1.5 master gate (confirmed already cached).
- Section 9 boot banner format: matches the M1.5 banner discipline
  (`[STATIC_PROP_PICK v1] enabled=<0|1> debug=<0|1>` mirrors
  `[RENDER_WORLD v1] frame=...` shape).
- Section 11 forbidden-behaviors list is exhaustive and matches the
  "do not touch what you don't have to" CLAUDE.md discipline.
- Q1 lean (clear on miss): correct -- keeps the debug-state struct
  semantically clean. The alternative (stale state + frame index)
  is strictly worse with zero current consumers.
- Q3 lean (defer highlight): correct -- highlight requires either
  AABB plumbing or a shader edit, both of which are bigger blast
  radius than M1.6 warrants.
- Q4 lean (`tryStaticPropPick` helper called from both style tails):
  correct -- avoids duplicating the conditional.

---

## Decisions needing user/advisor sign-off before revision pass

None. All findings are mechanical text fixes resolvable in a single
revision pass by the spec author. No architectural ambiguities remain
after C1/M1/M2 are applied; the remaining MINORs are documentation
polish.

The closest thing to an open architectural question is M2's note that
the plan-stage observable touches 4 (not 2) writer sites in two
sibling functions. This is plan-stage work (the spec correctly defers
the HOW), but the plan author should pre-commit to "instrument all 4
setSelected(true) sites" as a mechanical patch.

---

## Q6 resolution summary (load-bearing per user dispatch)

**Q6 is mechanically resolvable as a 4-site instrumentation: drop a
local `bool moverSelectedThisFrame = false;` at the top of
`updateOldStyle` and `updateAOEStyle`, set it to `true` immediately
after each of the four `setSelected(true)` calls at
`missiongui.cpp:1460,1487,1690,1705`, and thread it back through the
`tryStaticPropPick` helper call at each style tail.** The observable
is local-only (no new public state, no new member), edge-triggered by
the click that just landed, and trivially testable: with the spec's
env-ON config, Shift+LMB on a friendly mover sets the bool true (no
M1.6 log fires); Shift+LMB on empty space or on a static prop leaves
it false (M1.6 fallback gate opens). The plan-stage worst-case fallback
from spec Section 4 ("selection-list size strictly increased this
frame OR a new last-selected mover was set this frame") is NOT needed
once the four-site grep map above is in hand. The spec's Q6 framing
implied a 1-2 site change; the actual census is 4 sites, but they
share the trivial pattern and live in two adjacent functions, so the
edit budget stays within Section 11's "one new bounds check, one new
Y-flip" envelope (call it: four new bool assignments + one threaded
parameter).

---

End of review. ASCII only. Findings: 1 CRITICAL, 2 MAJOR, 6 MINOR.
