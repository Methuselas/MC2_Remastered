# Adversarial review: RenderWorld Slice M1.6 plan (2026-05-23)

- Target: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md` (1709 lines, 8 tasks)
- Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md` (EXECUTABLE-READY; spec review applied 1 CRIT + 2 MAJOR + 6 MINOR all resolved)
- M1.5 plan review gold standard: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-5-plan-adversarial-review.md`
- Reviewer pass: code-grounded; every cited file:line re-grepped at write time against HEAD `842f34f`.
- Mandate: try to BREAK the plan.

## Verdict

EXECUTE WITH FIXES. Plan is well-structured and the Q6 4-site instrumentation map matches grep. The Phase A split (Tasks 1/2/3 each independently build-green) is a real improvement over M1.5's dangling-symbol Phase A hazard. However ONE critical wrong-symbol-name bug will fail compile mid-task, plus the helper-vs-call-site split (T4/T5) carries an `unused private method` warning the plan itself flags but does not gate, and the spec's master env-var name is `MC2_RENDER_WORLD_STATIC_PROP_PICK` with a sanctioned plan-stage rename carve-out (so O1 is genuinely a user sign-off, not a deviation).

Counts: 1 CRITICAL, 3 MAJOR, 5 MINOR.

---

## CRITICAL findings

### C1. Wrong inverse-mapper symbol -- `handleIndexToRecipeIndex` does NOT exist; correct name is `handleToRecipeIndex`

- Plan Task 2 Step 4 (plan line 422) writes:
  ```cpp
  s_lastStaticPropPick.recipeIndex = res.isValid
      ? static_cast<int32_t>(handleIndexToRecipeIndex(res.handle.index()))
      : -1;
  ```
- Grep at write time against HEAD: `RenderWorld/RenderWorld.cpp:62` declares
  ```cpp
  int32_t handleToRecipeIndex(RenderCore::RenderObjectHandle h) { ... }
  ```
  taking a `RenderObjectHandle` (not a `uint32_t handleIndex`). There is NO `handleIndexToRecipeIndex` symbol anywhere in the codebase (verified: zero hits in `RenderWorld/`, `code/`, `mclib/`, `GameOS/gameos/`).
- The plan's grep step at Task 2 Step 4 (plan lines 446-448) DOES include `handleToRecipeIndex` in its grep pattern -- so the executor running that grep WILL find the correct name and is told "substitute it" in plan line 451. But the plan's code snippet uses the wrong name AND the wrong argument type (`res.handle.index()` returns `uint32_t`; the real function takes a `RenderObjectHandle`).
- Two fixes required in plan Task 2 Step 4 before execution:
  - Rename invocation: `handleToRecipeIndex(res.handle)` (pass the handle directly, NOT `handle.index()`).
  - Drop the misleading "if the inverse mapper exists under a different name, substitute it" hedge -- the canonical name is known at plan-write time; bake it in.
- The plan-author's fallback ("`recipeIndex = static_cast<int32_t>(res.handle.index())`") happens to also be correct because `handleToRecipeIndex` returns exactly `static_cast<int32_t>(h.index())` (RenderWorld.cpp:64). But this is incidental to the current M1 mapper implementation; future work that changes the mapping would break the fallback silently. Use the named mapper.

Recommended replacement snippet:
```cpp
s_lastStaticPropPick.recipeIndex = res.isValid
    ? handleToRecipeIndex(res.handle)
    : -1;
```

---

## MAJOR findings

### M1. T4 helper definition will emit `C4505 unreferenced local function` AND plan does not gate the warning-promotion check

- Plan Task 4 Step 6 (plan line 802) acknowledges: "MSVC may emit `C4505: unreferenced local function has been removed` or similar; this is expected and resolved by Task 5. If the warning is promoted to an error in CI, add a `// TODO(M1.6): wired in Task 5` comment above the function and proceed; do NOT add a `[[maybe_unused]]` attribute."
- Two problems:
  - `C4505` only fires for `static` functions at file scope; `tryStaticPropPick` is a non-static class member, so the actual MSVC warning is `C4514` (unreferenced inline) or no warning at all in a non-LTCG release build. The plan's "expected warning" is misidentified.
  - More importantly: a TODO comment does NOT suppress a promoted-to-error warning. If `/WX` is in effect for this TU (it is in some MC2 build configs), the build IS RED at Task 4 commit, and the recovery plan ("add a TODO") does nothing.
- The split's stated rationale ("reviewer reads the helper in isolation before seeing the 4-site diff") is real but the cost is a known-red-or-warning HEAD between Task 4 and Task 5 commits. M1.5 plan review C2/C3 explicitly flagged this pattern as a hazard.
- Recommended fix: collapse Task 4 into Task 5 as a single atomic Phase B commit. The reviewer can still review the helper-definition chunk separately within the same patch; git's per-hunk review handles this without needing a separate commit. The plan's own "Task 5 is the load-bearing atomic commit" framing already implies the boundary is artificial.
- If the split is retained for review-quality reasons, add Task 4 Step 6.5: "Verify `cmake --build` exit code is 0 -- promoted-warning failures here BLOCK the commit; do NOT proceed with a `[[maybe_unused]]` band-aid; collapse Tasks 4+5 if MSVC's warning policy blocks the split."

### M2. `mouseX` / `mouseY` at style-tail call sites are CLASS STATICS, not parameters -- plan's "static class member" claim at line 1143 is right, but the spec/plan rationale at O3 (off-screen guard) assumes raw click coords; the statics are last-mouse-poll coords updated at line 719-720 of `update()` BEFORE the style body runs

- Grep at write time:
  - `code/missiongui.cpp:167-168` -- `int MissionInterfaceManager::mouseX = 0; int mouseY = 0;` (static class members)
  - `code/missiongui.cpp:719-720` -- `mouseX = userInput->getMouseX(); mouseY = userInput->getMouseY();` inside `update()` body, BEFORE the dispatch to `updateOldStyle` / `updateAOEStyle` at lines 899/922
- So at the tail of `updateOldStyle` / `updateAOEStyle`, `mouseX/Y` contain the current frame's poll, NOT necessarily the LMB-down coordinates. For a single-click gesture this is fine (the click frame's mouse position IS the down position). But if `userInput->isLeftClick()` is set on the release-edge (typical Win32 click semantics), `mouseX/Y` may be the release-frame coords -- usually identical to down-coords for a clean click but NOT for a drag-then-click. The plan's `leftClicked` definition at line 1335 already filters drags via `!isLeftDrag()`, so this is bounded but worth a write-time grep.
- Recommended fix: add one line of explicit reasoning in plan Task 5 Step 5/Step 9, citing that `leftClicked = !isLeftDrag && !isRightDrag && isLeftClick && !lastUpdateDoubleClick` (line 1335) already excludes drag gestures, so release-edge mouseX/Y is the correct click point. No code change; just documentation so a future maintainer does not assume LMB-down capture.

### M3. T5 Step 12 "byte-identical" tier1 invariant is overstated; helper-call-site adds one CALL/RET per click

- Plan Phase B gate (line 606) and Task 5 Step 12 (line 1300) claim "byte-identical to M1.5 HEAD `842f34f`" with `MC2_STATIC_PROP_PICK` unset.
- The helper's first instruction is `if (!RenderWorld::IsStaticPropPickEnabled()) return;` (plan line 725), reading a cached `static bool`. The cost is one CALL + one cached-bool load + one CMP + one RET per Shift+LMB click event. This is NOT byte-identical to a HEAD where the helper does not exist; the binary has one extra call site and instructions per click.
- The plan also writes `moverSelectedThisFrame = true` UNCONDITIONALLY (no env-gate) at 4 sites. That is 4 unconditional stack-bool writes per click that selects a friendly mover -- again, not byte-identical to M1.5 HEAD.
- Tier1 5/5 is unlikely to detect this (the harness runs without user clicks and the 4 instrumented sites only fire on user mover-selection). But the plan's framing of "byte-identical" sets a false expectation; the correct invariant is **behavior-identical at click rate**, **byte-identical at non-click steady-state frames**.
- Recommended fix: relax the gate description in plan lines 606, 1300, and the "Pre-execution gates" item 2 (plan line 1574). Replace "byte-identical to M1.5 HEAD" with "tier1 5/5 PASS env-OFF; pixel parity vs M1.5 HEAD `842f34f` at idle frames; helper call site adds ~5 instructions per Shift+LMB click which tier1's no-user-input harness does not exercise." The pixel-parity invariant is what actually matters.

---

## MINOR findings

### m1. O1 user sign-off on env-var name is genuine -- spec EXPLICITLY sanctions the plan-stage rename

Spec section 9 line 544-547 reads: "Strawman name. Plan stage may rename to e.g. `MC2_STATIC_PROP_PICK=1` for brevity. Whatever the final name, it must (a) start with `MC2_`, (b) be a single boolean, (c) be read once at startup and cached."

The plan's `MC2_STATIC_PROP_PICK` choice satisfies all three. This is NOT scope creep or a deviation. The user sign-off is for naming preference, NOT for whether the rename is permitted. The plan-author's framing in plan lines 68-77 is correct; reviewer flags 8 in the dispatch prompt is over-cautious -- spec explicitly authorizes the rename.

Action: confirm user picks one name (the spec's `MC2_RENDER_WORLD_STATIC_PROP_PICK` or the plan's `MC2_STATIC_PROP_PICK`); both are spec-compliant.

### m2. `bLeftDouble` recompute at style-tail is safe but worth a write-time grep on `isLeftDoubleClick` side effects

Plan lines 1130 and 1246 recompute `userInput->isLeftDoubleClick()` at the style-tail. Grep at write time:
- `code/missiongui.cpp:794` -- `bool bLeftDouble = userInput->isLeftDoubleClick();` (called once in `update()`)
- `code/missiongui.cpp:3373` -- another caller in a different update path

The accessor name suggests pure read (no state mutation), but the plan does not grep `isLeftDoubleClick` IMPLEMENTATION to confirm. If the function is stateful (e.g. consumes the double-click flag on read), the second call at the style tail will see `false` after `update()`'s call at :794 already consumed it -- the M1.6 helper would never see `bLeftDouble=true` and the bLeftDouble guard at plan line 737 becomes dead code (harmless, but a misleading "we filter double-click" claim).

Recommended fix: add one grep step in Task 4 Step 1 to verify `isLeftDoubleClick` is a pure read (e.g. `grep -A 5 "isLeftDoubleClick" mclib/userinput.cpp`). If it is stateful, change the helper signature to accept `bLeftDouble` as a parameter passed from `update()` instead of recomputed at the tail.

### m3. Y-flip placement is correct; `Environment.screenHeight` symbol verified

Plan line 750 uses `Environment.screenHeight`. Grep confirms:
- `GameOS/include/gameos.hpp:3379` -- `extern gosEnvironment Environment;`
- `code/controlgui.cpp:193, 454, 525, 543, ...` -- `Environment.screenWidth` / `Environment.screenHeight` extensively used in code/ TUs.

Spec section 5 ("Y-flip at the call site") matches the plan's helper-body placement at lines 749-757. No fix needed.

### m4. `tryStaticPropPick` namespace -- plan says `MissionInterfaceManager::tryStaticPropPick` (member); this is correct given the helper needs to be callable from `updateOldStyle` and `updateAOEStyle` (both class members) without surfacing to global scope

Plan Task 4 Step 2 (plan line 643) declares the method in `missiongui.h` as a private class member. This is the right choice (matches the style of `updateOldStyle` / `updateAOEStyle`) and avoids polluting global namespace. No fix needed; flagged only because the dispatch prompt asked for verification.

### m5. T8 CLAUDE.md bullet is ~12-15 lines as written (plan line 1530) -- acceptable but on the long side

Spec target was "~10 lines matching M1/M1.5 style." Plan's bullet at line 1530 is a single dense paragraph that, when wrapped, lands around 12-15 displayed lines. M1's bullet is ~17 lines, M1.5's is ~20 lines, so the plan is shorter than its precedents -- the spec target of "~10 lines" was optimistic. The plan's choice is fine.

Recommended fix: none. If the user prefers tighter, the executor can trim the validation-gate enumeration at the end of the bullet to "Validation gates 1-4 PASS." instead of listing each gate inline.

---

## Strengths confirmed

- **Phase A genuinely build-green at each task boundary.** Unlike M1.5 plan review C2 (dangling symbol across Task 5-7), M1.6 Phase A Tasks 1, 2, 3 each define + declare their additions in the same commit. The init banner update in Task 1 is the only externally-observable change at that boundary, and it is safe to ship in isolation.
- **Q6 4-site grep verified.** Independent grep at write time against HEAD `842f34f`:
  - `:1460, :1487, :1690, :1705` are the `setSelected(true)` writer sites (4 hits)
  - `:1462, :1483, :1692, :1701` are the `setSelected(false)` sibling sites (4 hits)
  - Plan correctly instruments only the `true` set; correctly leaves `false` set uninstrumented (Q6 RESOLVED rationale).
  - Additional `setSelected(true)` sites at `:2154` (in a different code path -- `pilot::setSelected`-style) and `:3692` (drag-rect selection inside `selectObjsInRect`) are correctly NOT included in the 4-site census because spec Section 11 limits scope to the two style-body click handlers.
- **Inverse-mapper does exist** (modulo C1's wrong name): `handleToRecipeIndex` at `RenderWorld.cpp:62`. The plan-author's hedge "if the mapper exists" was right to grep; the executor following Task 2 Step 4 will find the correct name.
- **`Environment.screenHeight` is the right Y-flip operand.** Verified extern at `GameOS/include/gameos.hpp:3379`.
- **`bLeftDouble` recompute is plausibly safe** but should be verified (see m2).
- **Pre-existing 4-site instrumentation atomicity argument is sound.** Splitting the 4 sets from the 2 call sites would break the Section 11 invariant in the in-between commit (Shift+LMB on a friendly mover would emit a log AND toggle the mover) -- the plan correctly bundles all of Task 5.
- **No `tryStaticPropPick` symbol collision** (verified: zero hits across `code/`, `RenderWorld/`, `mclib/`, `GameOS/gameos/`).
- **Greybeard task (T7) is NOT pre-judged.** Plan line 1502 explicitly says "do NOT pre-judge" and presents PATCH (justified) as a *lean*, not a verdict. Contrast with M1.5 reviewer flag #9 from the dispatch prompt; M1.6 T7 prompt at plan line 1480-1495 stays neutral. PASS.
- **T6 canary mission is correct.** Plan line 1369, 1387, 1400 use `mc2_03` (per M1.5 standard for env-ON canary). Stress mission `mc2_24` is correctly NOT used for the visual canary.
- **`lookupAtPixel` WARN-spam guard.** Helper's defense-in-depth check `IsObjectIdBufferEnabled()` at plan line 729-730 fires BEFORE the `lookupAtPixel` call, so M1.5's once-per-process WARN line is NOT triggered by M1.6 Shift+clicks in substrate-OFF mode. Gate 4 in Task 6 (plan line 1418-1438) verifies this.

---

## Decisions needing user sign-off

### D1. Env-flag final name (was plan-author O1)

Both `MC2_STATIC_PROP_PICK` (plan's lean) and `MC2_RENDER_WORLD_STATIC_PROP_PICK` (spec's strawman) are spec-compliant. The user should pick one before Task 1 because Task 6's grep gate (plan line 1421, 1369, 1400) hardcodes the name and Task 8's CLAUDE.md bullet references it. Reverting later means a rebuild + CLAUDE.md edit.

Reviewer lean: ship as `MC2_STATIC_PROP_PICK` (brevity matters for env-var typing during canary; the `MC2_` prefix already implies render-world scope in this project's convention).

### D2. T4/T5 split vs collapse (M1 above)

If MSVC `/WX` is in effect for `code/missiongui.cpp` TU, T4 alone is RED. The plan-author's mitigation (a TODO comment) does not suppress promoted warnings. User should decide:
- Collapse T4+T5 into a single Phase B commit (loses isolated-helper-review benefit, gains build-green-at-every-boundary discipline matching M1.5 review C2).
- Keep the split and accept the risk that Task 4 commit may be RED on some build configs (gains review-clarity, costs build-discipline).

Reviewer lean: COLLAPSE. The "review helper in isolation" benefit is overstated; reviewers read patches by hunk, not by commit, and the per-commit boundary buys nothing if it costs a red HEAD.

### D3. C1 fix application

Plan Task 2 Step 4 needs the `handleIndexToRecipeIndex(res.handle.index())` -> `handleToRecipeIndex(res.handle)` substitution before execution. Trivially mechanical; user should authorize the in-plan edit (or accept that the executor will catch it via the Task 2 Step 4 grep at plan line 446-448 and apply the correction at write time).

Reviewer lean: edit the plan now (5-second fix); do not rely on the executor's runtime grep to catch a known-wrong symbol name in shipped plan text.

---

## Summary

The plan is closer to EXECUTE-READY than any prior M-series plan reviewed this session. The Phase A boundary discipline is genuinely improved over M1.5. The Q6 4-site map is grep-verified and the explicit-NOT-instrumented siblings rationale is sound. The single CRITICAL is a one-line wrong-symbol bug in Task 2 Step 4 that the plan-author's own hedge would catch at execution time, but should be fixed in the plan text to remove ambiguity. Three MAJORs are all "framing/discipline" rather than logic bugs. Apply C1, decide D1+D2, and EXECUTE.

Counts: 1 CRITICAL / 3 MAJOR / 5 MINOR. Verdict: EXECUTE WITH FIXES.
