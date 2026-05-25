# M2-pre Plan Adversarial Review

**Target:** `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md` (1662 lines, 7 tasks)
**Spec:** `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md` (EXECUTABLE-READY)
**HEAD:** `db25d67` (M1.6 SHIPPED 2026-05-23)
**Reviewer mandate:** break the plan; grep every cited file:line; assume at least one CRITICAL hides.
**Date:** 2026-05-23

## Verdict

**APPROVE WITH MINOR FIXES.** Plan is grep-clean against HEAD on every cited anchor (4 writer sites, 2 caller sites, body extent, CMakeLists explicit list, firewall SCOPE_DIRS, RenderWorld::init shape, missiongui.h decl). The substitutive proof is well-instrumented (T3 Steps 5/6/7 are gate-grade). Findings below are operational / observational and do not block execution.

Counts: 0 CRITICAL / 0 MAJOR / 5 MINOR.

## Grep evidence (anchors confirmed)

All anchors re-grepped 2026-05-23 against worktree HEAD `db25d67`:

| Plan claim | Grep result | Status |
|---|---|---|
| 4 `moverSelectedThisFrame = true` writer sites at `:1476/1511/1740/1763` | EXACT match (4 hits) | OK |
| 2 helper call sites at `:1538` + `:1781` | EXACT match | OK |
| Function header `tryStaticPropPick` at `:6168` | EXACT match | OK |
| Body extends through `:6273` (closing brace) | `wc -l missiongui.cpp` = 6273; closing `}` is final line | OK |
| `[STATIC_PROP_PICK v1] hit` format at `:6250` | EXACT match | OK |
| `[STATIC_PROP_PICK v1] miss` format at `:6267` (plan said `:6266`) | Actual `:6267`; 1-line drift inside acceptable | OK (minor) |
| CMakeLists `code/missiongui.cpp` at `:212` | EXACT (`:211` mechicon, `:212` missiongui, `:213` mpconnectiontype) | OK |
| Firewall SCOPE_DIRS at `scripts/check-include-firewall.sh:22` (no `code/`) | EXACT match: `RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL` | OK |
| `missiongui.cpp:33` includes `../RenderWorld/RenderWorld.h` | EXACT match | OK |
| `RenderWorld::init` at `:311` + `runSubstrateSelfTest` at `:332` | EXACT match | OK |
| `missiongui.h:272-278` tryStaticPropPick declaration | EXACT match (7-arg signature) | OK |
| Body indentation 4-space (per plan O2) | Confirmed via `cat -A` -- spaces, not tabs | OK |
| `gos_GetViewport(&vMulX, ...)` call site at `:6218` | EXACT match; one additional COMMENT mention at `:6206` (text-only) | OK |
| `Environment.drawableWidth/Height` reads in body | 2 hits both inside `tryStaticPropPick` body | OK |

## CRITICAL

None.

## MAJOR

None.

## MINOR

### m1. T3 Step 5 grep-gate may yield a false positive on the doc-comment text reference

The "machinery is GONE" grep gate at T3 Step 5:

```bash
grep -n "gos_GetViewport\|IsObjectIdBufferEnabled\|lookupAtPixel" code/missiongui.cpp
```

is well-conceived but applied to the whole file; the plan then says "Expected: ZERO hits inside the `tryStaticPropPick` body (line range ~6168 to closing brace)." The current pre-refactor file has TWO `gos_GetViewport` mentions inside the body (a comment at `:6206` and the call at `:6218`); the M2-pre Replace block deletes the entire pre-refactor body including the comment, so post-refactor expectation is correct. However, the plan author's strict gate command grep'd against the whole file will still find OTHER hits elsewhere in `missiongui.cpp` (e.g. menu-rendering call sites for `gos_GetViewport`). The plan acknowledges this in parens ("Other hits elsewhere in `missiongui.cpp` are fine -- the gate is body-local, not file-global") but does NOT supply a body-restricted command.

**Recommendation:** add an `awk` body-extractor variant alongside the file-wide grep so the gate is mechanically checkable rather than relying on the executor's manual triage of the grep output:

```bash
awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/' code/missiongui.cpp \
  | grep -E "gos_GetViewport|IsObjectIdBufferEnabled|lookupAtPixel" \
  || echo "OK: ZERO inline-machinery symbols in refactored body"
```

This costs one line in the plan and converts the gate from "grep + read carefully" to "pipe-exit-status."

### m2. Self-test step 8 pixel choice (640, 360) lands BEFORE the first scene render

The plan's R1 captures the concern partially. `RenderWorld::init()` runs early in mission load -- BEFORE any scene render has touched the attachment-2 buffer for that mission. attachment-2 is freshly cleared to 0 at FBO setup. Therefore at the moment `RunGameplayPickSelfTest()` runs, `lookupAtPixel(640, 360)` will deterministically return invalid (raw=0 -> `LookupResult{isValid=false}`), driving step 8 to outcome `miss`.

`miss` is a valid step-8 PASS per the plan (R1 mitigation: outcome IN `{hit, miss}`); so the test does NOT spuriously fail. BUT it means step 8's "live readback reached" claim is satisfied by a *blank framebuffer read*, not by sampling actual scene content. The signal value of step 8 is reduced from "the spine reached lookupAtPixel against rendered content" to "the spine reached lookupAtPixel against an uninitialized texel."

The plan accepts this implicitly. Two improvements possible:

(a) Document explicitly in the step 8 comment: *"This step asserts the spine REACHED `lookupAtPixel` without short-circuit; the pixel will be background (raw=0 -> miss) because the self-test runs at init time before any scene draws have touched attachment-2. A bound-but-cleared texel sample is sufficient signal for the spine-reachability claim; live-content sampling would require deferring the self-test to first-stable-frame, which the plan declines to do for simplicity."*

(b) Alternative: defer the self-test invocation to first-stable-frame (mirroring M1.5's optional `OBJECT_ID_SELFTEST` passive canary at mc2_03 frame=N). Not required for M2-pre to ship; step 8 carries enough signal as-is.

**Recommendation:** accept the plan's choice; add the (a) comment to T4 Step 2's step-8 block. No spec change.

### m3. Forward-declaration in RenderWorld.cpp crosses a firewall layer (game-side symbol into engine-side TU)

The plan (T4 Step 3 note) forward-declares `void RunGameplayPickSelfTest();` at the top of `RenderWorld/RenderWorld.cpp` rather than including `../code/gameplay_pick.h`. The justification ("avoid dragging game-side types into a substrate-side TU") is reasonable AT THE TYPE LEVEL, but the *symbol* still crosses the firewall: the engine-side TU calls a game-side function.

Reality check: this is exactly the same shape M1.6 already ships (engine-side substrate has its own self-test invoked from init; game-side adapter is *NOT* invoked from init). M2-pre adds a NEW direction: engine-side TU invoking game-side TU. Firewall script SCOPE_DIRS does not scope `code/`, so the script does not flag this -- but the script's silence is not architectural endorsement.

Two reads:

(a) **The plan's read (acceptable):** validation gates are exceptional; the self-test wire is a one-line developer hook, not a runtime hot path. The forward-declaration limits the surface to one symbol.

(b) **Stricter read:** the self-test should be invoked from the game-side mission init path (e.g. `Mission::init` after the M1.5 substrate banner), NOT from `RenderWorld::init()`. This keeps engine-side TUs free of game-side symbols.

The plan choice (a) is consistent with the spec resolution and with the surrounding M1.5 `runSubstrateSelfTest()` pattern -- both run as part of `RenderWorld::init()`. Sticking with (a) is fine. But the plan should note in T4 Step 3 *"this introduces a new direction of cross-firewall linkage (engine-side TU calling game-side TU); future engine-side TUs should not generalize this pattern without explicit greybeard review."*

**Recommendation:** add the above note to T4 Step 3.

### m4. T3 Step 7 LOC-gate threshold is loose

T3 Step 7 asserts the refactored body is "substantially smaller than the pre-refactor count of ~115 lines" and expects "~55 lines." The Replace-with block in T3 Step 4 is ~79 lines (including the multi-line printf calls and switch boilerplate). After the function-header + closing brace, `awk` will count ~80 lines. The expected "~55" undersells the actual replacement.

The intent of the gate is sound (catch the "I duplicated code beside the call" failure mode) but the threshold is sloppy. Pre-refactor block runs 6168-6273 = 106 lines; post-refactor will be roughly 6168 + 80 lines = ~80 lines.

**Recommendation:** change T3 Step 7 expected to "between 60 and 90 lines (down from 106)" with the explicit pre-refactor count for diff context. If the awk count is >100, the substitutive proof failed.

### m5. T7 CLAUDE.md bullet runs ~30 lines (too long for a refactor slice)

The proposed M2-pre bullet (plan lines 1521 - one giant inline markdown block) is roughly the same SIZE as the M1.6 bullet for a *new gameplay feature wire*. M2-pre is a refactor; the bullet should be ~10-12 lines (slice goal, substitutive proof, gate-2 self-test PASS, greybeard verdict slot, spec/plan links). Current draft repeats the M1.6 context that's already in the M1.6 bullet immediately above (mover-fallback sites, firewall non-edit rationale, etc.).

The current worktree CLAUDE.md is 186 lines; adding a 30-line bullet pushes it to ~216, past the 200-line cap (the plan flags this risk in operational notes but doesn't prescribe the trim).

**Recommendation:** trim the bullet to roughly:

```
- **RenderWorld Slice M2-pre** (SHIPPED 2026-05-23): META-FIX refactor
  paying the M1.6 greybeard debt. Extracts `tryGameplayPick(req) -> result`
  + `screenToFboPixel` from `MissionInterfaceManager::tryStaticPropPick`
  into new `code/gameplay_pick.{h,cpp}` (POD request/context/result +
  free functions; first non-class-method header under `code/`). M1.6
  body shrinks from ~106 lines of inline machinery to ~30 lines of
  caller pattern (category gate -> request build -> tryGameplayPick ->
  outcome switch). `[STATIC_PROP_PICK v1] hit/miss` log byte-identical
  to M1.6 HEAD `db25d67` (git-show diff gate); 4 `moverSelectedThisFrame`
  writer sites + 2 helper call sites + signature untouched. New
  validation self-test `RunGameplayPickSelfTest()` from `RenderWorld::init()`
  under `MC2_GAMEPLAY_PICK_SELFTEST=1 + MC2_OBJECT_ID_BUFFER=1` emits
  `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` across 8 synthetic
  inputs. Tier1 5/5 PASS env-OFF + env-ON. Greybeard ruling 2026-05-23:
  <verdict>. Spec: `.../m2-pre-gameplay-pick-extraction-spec.md`.
  Plan: `.../m2-pre-gameplay-pick-extraction-plan.md`. Ruling:
  `.../m2-pre-greybeard-ruling.md`.
```

That holds under 200 lines.

## Strengths confirmed

- **Every cited file:line grep-verified against HEAD `db25d67`.** Plan author's "verified 2026-05-23" annotations match my fresh grep precisely (the one drift -- `:6266` for miss-log vs actual `:6267` -- is within "symbols stable, numbers may drift" tolerance).
- **CMakeLists integration is correct.** Explicit list at `:211-213` confirmed (no glob); T1 Step 6's Replace block matches the actual surrounding lines verbatim.
- **Substitutive proof is well-instrumented.** T3 Step 5 (grep gate for inline-machinery removal), Step 6 (grep gate for helper call presence), Step 7 (LOC gate), Step 9 (git-show byte-diff for log format strings) collectively form a stronger substitutive verification than any prior M1.x slice shipped.
- **Self-test 8-step assertion sequence covers all five outcome paths** (skipped via 4 gate flavors + off-screen, gated via mover-fallback, hit-or-miss via clean request). One step per failure mode; FAIL line includes both expected and actual for fast triage.
- **Coord-derivation comment preserved verbatim.** T1 Step 4's header comment + T1 Step 5's `screenToFboPixel` block carry the full M1.6 commit `1853ad9` derivation; no paraphrasing risk.
- **Q1/Q7/Q11/M-1 spec resolutions baked into the plan up front** ("Decisions resolved before execution" block) prevents executor-time re-litigation.
- **R1-R4 risk register is honest** about the step-8 pixel-choice tradeoff (caught and accepted), header-include reach-in (justified by precedent), coord-translation parity (covered by self-test step-order), and CMakeLists glob (covered by fail-loud linker behavior).
- **Pre-execution gates (6 items) are mechanically checkable.** Each one has a grep or `git log` command that exit-status-tells whether the gate holds.
- **ASCII discipline maintained.** Spot-checked the plan; no pictographic emoji. Unicode em-dashes used freely (permitted per worktree CLAUDE.md).
- **Gate 4 git-show syntax correctness.** T3 Step 9's `git show db25d67:code/missiongui.cpp` form is canonically correct (the `<sha>:<path>` form, not `<path>@<sha>`).
- **Off-screen guard semantics preserved.** T2 Step 2's body uses `mouseX >= (int)r.ctx.vMulX || mouseY >= (int)r.ctx.vMulY` -- exact match to M1.6 HEAD `:6223-6225` (viewport-space bounds, not drawable-space).

## Decisions needing user/advisor sign-off before revision pass

None of the MINOR findings require user sign-off; they are operational tweaks the executor or plan author can apply. The plan is APPROVED for execution as written.

For completeness, two decisions remain implicit choices the plan made that *could* be reconsidered but do not need to be:

- **D1. Self-test invocation point.** Currently `RenderWorld::init()`, which runs before scene render -> step 8 samples an uninitialized texel (m2 above). Alternative would be deferring to first-stable-frame. Plan accepts the early-init choice; reasonable. No action.

- **D2. Forward-decl direction of firewall crossing.** Currently engine-side TU forward-declares game-side function (m3 above). Alternative would be invoking the self-test from `Mission::init` instead. Plan accepts the current direction as continuation of the M1.5 substrate-self-test pattern; reasonable. No action.

If the user wants to revisit either, the spec needs a small amendment; otherwise proceed to execution.

## Recommendation

Apply MINOR fixes (m1, m2 comment, m3 note, m4 threshold, m5 trim) during T1/T3/T4/T7 execution -- they are <30 LOC total of plan/code tweaks and do not change task scope. Plan is grep-clean and structurally sound. Approve for executor session.
