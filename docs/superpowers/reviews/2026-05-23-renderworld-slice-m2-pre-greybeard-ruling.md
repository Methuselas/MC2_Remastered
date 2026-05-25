# Greybeard ruling -- RenderWorld Slice M2-pre gameplay-pick extraction

- Subject: `tryGameplayPick(GameplayPickRequest)` spine +
  `screenToFboPixel(...)` pure transform + `RunGameplayPickSelfTest()`
  validator, all in `code/gameplay_pick.{h,cpp}`, with
  `MissionInterfaceManager::tryStaticPropPick` refactored to delegate.
- Skill: `.claude/skills/greybeard.md` verbatim.
- Date: 2026-05-23.
- Reviewer stance: fresh ruling, no pre-judged verdict. Evidence
  grep-grounded against HEAD `4442154` (M2-pre T4).
- Predecessor ruling: M1.6 PATCH (justified) at
  `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-6-greybeard-ruling.md`.

---

## 1. Subsystem pin

Gameplay-side substrate-consumer wiring in `code/`. First-hand
evidence from worktree HEAD:

- New TU at `code/gameplay_pick.cpp` (3 PODs + spine + pure transform
  + self-test); header at `code/gameplay_pick.h`. Verified
  `ls code/gameplay_pick.*` returns both files.
- `git grep -n "tryGameplayPick\|screenToFboPixel" -- '*.cpp' '*.h'`
  shows ONE production caller: `code/missiongui.cpp:6213` inside the
  refactored `tryStaticPropPick`. The other 8 hits are inside
  `RunGameplayPickSelfTest` exercising the spine through synthetic
  inputs across the gate ladder.
- Refactored body at `code/missiongui.cpp:6168-...` substitutive
  proof (body-restricted awk + grep):
  - `IsObjectIdBufferEnabled` inline count: **0** (was 1 pre-T3)
  - `gos_GetViewport`        inline count: **0** (was 1 pre-T3)
  - `lookupAtPixel`          inline count: **0** (was 1 pre-T3)
  - `tryGameplayPick`        inline count: **1** (was 0 pre-T3)
- M1.5 substrate API (`lookupAtPixel`, `setLastStaticPropPick`,
  `IsObjectIdBufferEnabled`, etc.) UNCHANGED by M2-pre; the slice is
  purely a refactor of the M1.6 consumer.

Pin: the same gameplay-side substrate-consumer wiring layer M1.6
pinned. M2-pre operates entirely inside that layer; no render-
pipeline state mutated, no shader edits, no GL state policy
touched.

## 2. Symptom vs cause

- **Proximate symptom (what M1.6 left as debt):** the inline pick
  machinery in `tryStaticPropPick` carried four embedded policies
  -- substrate env gate, gesture ladder, mover-fallback observable,
  UI-canvas-to-FBO coord transform -- in a body shaped by
  static-prop assumptions. Any second consumer (M2.6 mech-pickup,
  M3 terrain-pickup, M4 VFX-pickup, M5 overlay-pickup) would
  naturally copy-paste this body and re-embed the policies inline,
  each carrying its own coord-scale block and its own off-screen
  guard. The author's own `1853ad9` commit message flagged the
  extraction; the M1.6 ruling named both extractions verbatim.
- **Upstream condition (the bug class):** "gameplay-side substrate
  consumer wiring drift" -- when each new gameplay-pick category
  reinvents the gate ladder + coord chain inline, the four hazards
  M1.6 ruling Section 2 enumerated (env-gate ordering, mover-vs-
  substrate priority, coord-space translation, per-mission
  lifecycle) re-manifest per slice. Per-instance fix = patch;
  per-class fix = a shared spine that future callers cannot
  bypass.

## 3. The meta-fix

The single upstream change: the inline pick machinery is GONE from
`tryStaticPropPick`. The shared spine `tryGameplayPick(request)`
plus the pure transform `screenToFboPixel(...)` are the ONLY
production sites that touch `gos_GetViewport` and `lookupAtPixel`
on the gameplay-pick path. Future M2.6 / M3 / M4 / M5 callers
cannot reinvent the chain without either (a) calling the spine or
(b) issuing a fresh inline reach-in to substrate APIs that grep
review will catch immediately.

This is "modernize the shim, do not correct the shim" applied to
gameplay-side substrate wiring. The legacy mechanism (per-category
inline gate ladder) is REPLACED at the only existing site, not
patched.

The M1.6 ruling Section 3 anticipated this exact shape:

> "the META-FIX shape would be a `tryGameplayPick(GameplayPickRequest)`
> helper that takes the click gesture + a 'what writes to attachment-
> 2 currently' query and dispatches to type-specific consumers."

M2-pre built precisely this. The request struct is gesture-only
(7 fields mirroring the 7 args the M1.6 helper already took);
category resolution lives in the caller (M2.6 will own
`IsMechPickEnabled()` separately). Spec Section 3 + Section 4
demonstrate the shape is category-agnostic.

## 4. Substitutive test

This is substitutive, not additive. Three independent lines of
evidence:

- **Inline machinery deleted from the only existing site.** The
  body-restricted grep above shows 0/0/0/1 -- the inline
  substrate-gate, viewport-query, and lookup calls are GONE from
  `tryStaticPropPick` and survive only inside `tryGameplayPick`'s
  body. The OLD path is not bypassed; it is removed.
- **Single production producer of `lookupAtPixel` on the gameplay
  path.** `git grep -n lookupAtPixel -- 'code/*.cpp'` (verified via
  the all-callers grep in Section 1) confirms `tryGameplayPick` is
  the lone non-substrate caller. Future drift introduces a new
  inline call that grep will surface immediately.
- **`RunGameplayPickSelfTest` exercises the gate ladder.** The
  validator drives 8 synthetic input vectors through the spine
  (shiftDn=false / leftClicked=false / bGui / bLeftDouble /
  moverSelectedThisFrame / off-screen X / off-screen Y / clean
  center-FBO) and asserts the outcome enum matches the M1.6 gate
  semantics. This is reproducible without a human clicking; gate-
  ladder regressions during M2.6+ extension are catchable in CI.

Substitutive criterion: PASS. The CPU zone (M1.6 inline machinery)
is GONE from the capture, not merely complemented.

Caveat. Production consumer count at M2-pre close is 1, where M1.5
`setSceneDrawBuffers` consolidated 5 simultaneous drift sites. The
honest read: M2-pre is preemptive consolidation. The spine's
category-agnosticism is unproven until M2.6 lands and uses it
unchanged. The Section 10 contract + Gate 5 spine-stability
assertion are the mechanism to catch a premature extraction.

## 5. Verdict

**META-FIX.**

### Upstream change

Shared gameplay-pick spine `tryGameplayPick(GameplayPickRequest)`
in `code/gameplay_pick.cpp:79` is the sole production producer of
substrate readback on the gameplay-pick path. Pure coord transform
`screenToFboPixel(...)` at `code/gameplay_pick.cpp:54` is the sole
production producer of UI-canvas-to-FBO translation on that path.
`RunGameplayPickSelfTest()` is the automated grep gate analogue:
gate-ladder regressions trip the self-test on next launch under
`MC2_GAMEPLAY_PICK_SELFTEST=1`.

### Bug class retired

"Gameplay-side substrate consumer wiring drift across pick
categories." Concretely:

- The four M1.6 hazards (env-gate ordering, mover-vs-substrate
  priority, coord-space translation, off-screen guard) live in
  exactly one place. Per-category callers contribute only their
  category-gate + their log emission + their debug-state mutation.
- The grep gate is the call-site count: `git grep -nE
  'gos_GetViewport|lookupAtPixel' -- code/*.cpp` should show
  matches only inside `code/gameplay_pick.cpp`. New inline reach-
  ins are obvious in review.
- The Section 10 extension contract documents the M2.6 / M3 / M4 /
  M5 obligation: build a `GameplayPickRequest`, call the spine,
  switch on outcome. Bypassing the spine to call `lookupAtPixel`
  directly with a new inline coord block is the regression signal.

### Prevented bug instances

- **M2-pre itself:** the M1.6 inline ladder is gone; no future
  copy-paste source exists.
- **M2.6 (mech-pickup):** cannot reinvent the gate ladder or
  coord chain. Builds `GameplayPickRequest`, calls spine,
  consumes outcome. Section 10 + Gate 5 catch a premature
  extraction.
- **M3 (terrain-pickup):** same contract. The terrain category's
  request differs only in the category-env gate it lives behind.
- **M4 (VFX-pickup) / M5 (overlay-pickup):** same. Each gets a
  new caller in `code/`; the spine is unchanged.
- **Coord-translation drift:** any future "I need UI-canvas mouse
  in FBO pixels" consumer (e.g. a hypothetical world-point inspect
  gesture) calls `screenToFboPixel` and inherits the policy. The
  pure-function shape makes the helper trivially reusable; the
  helper's grep target is unambiguous.

### Sub-judgment: `screenToFboPixel`

**META-FIX** (sub-judgment converges with the overall verdict).
The coord helper is the lower-risk extraction independently. It
is a pure function with a single call site today (`tryGameplayPick`
inside the same TU) and a documented bug class (the
`22 misses / 0 hits` symptom from `1853ad9`, traced to identity-
assumption coord-translation). Centralization of the
viewport-to-FBO policy on a single grep target makes the bug
class structurally unrepresentable for any future consumer that
uses the helper. The author flagged this extraction in their own
commit message; M2-pre honored the cadence by extracting on the
named-but-not-yet-built second consumer.

The sub-judgment would survive even if the spine itself were
ruled PATCH: a pure function with a documented prior bug class is
the clearest META-FIX shape.

### Blast radius

- Files: `code/gameplay_pick.{h,cpp}` (new), `code/missiongui.cpp`
  (body of one method), `CMakeLists.txt` (one source list edit;
  verified at plan stage), `RenderWorld/RenderWorld.cpp` (wire of
  self-test in `init()`).
- Symbols: 3 new PODs + 2 free functions + 1 self-test +
  1 refactored caller body. Six new symbol-level surfaces total.
- Grep gates:
  - `git grep -nE 'gos_GetViewport\|lookupAtPixel' -- code/*.cpp`
    expected to match only `code/gameplay_pick.cpp`.
  - `awk '/^void MissionInterfaceManager::tryStaticPropPick/,/^}/'
    code/missiongui.cpp | grep -cE
    'IsObjectIdBufferEnabled|gos_GetViewport|lookupAtPixel'`
    expected to return 0.
  - `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all` on next
    launch under `MC2_GAMEPLAY_PICK_SELFTEST=1
    MC2_OBJECT_ID_BUFFER=1`.

### Verdict line

META-FIX. The `tryGameplayPick` + `screenToFboPixel` +
`RunGameplayPickSelfTest` package retires the "gameplay-side
substrate consumer wiring drift" bug class for M2.6 + M3 + M4 + M5
extensions. The M1.6 named debt is paid; the trigger condition is
honored preemptively per the user's chosen sequencing.

### Why upgrade-from-PATCH is legitimate here

The M1.6 ruling explicitly named both extractions and named the
trigger condition. M1.6 deferred because:
- (a) sample size of one with no named second consumer queued
  imminently, and
- (b) selection-priority semantics expected to flip at M2.

M2-pre operates in a different state:
- (a) M2 / M2.5 / M2.6 are the user's announced sequencing; the
  named second consumer is queued.
- (b) M2-pre extracts BEFORE M2.6 has a chance to fork the inline
  body; semantics-flip risk is moved to M2.6's plan stage, where
  Section 10 + Gate 5 catch it explicitly.

The PATCH-vs-META-FIX hinge per skill discipline is the
substitutive test (skill section 4): "the CPU zone must be gone
from the capture, not merely bypassed." The M2-pre substitutive
proof PASSES. Sample-size-one is a real counter-argument but
addresses validation strength (will the shape survive M2.6?), not
the bug-class-retirement criterion (is the inline path gone?). The
sample-size risk is mitigated by the Section 10 contract + Gate 5
assertion + the self-test; the bug-class criterion is met
unambiguously.

---

## Closing recommendation

Adopt as **META-FIX**. Sub-judgment on `screenToFboPixel`: also
**META-FIX**. Add to slice closing notes:

- Grep gate 1: `git grep -nE 'gos_GetViewport\|lookupAtPixel'
  -- code/*.cpp` expected to match only `code/gameplay_pick.cpp`.
- Grep gate 2: body-restricted check that
  `tryStaticPropPick` carries no inline substrate/viewport calls.
- Self-test gate: `[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all`.
- M2.6 plan stage MUST grep-cite the unchanged `tryGameplayPick`
  signature against M2-pre HEAD (`4442154`). Any change to the
  signature or the `GameplayPickRequest` field set is the signal
  the M2-pre extraction was premature; the slice would need
  reshaping rather than extension.
- If a future slice introduces a new gameplay-pick category and
  the author finds the spine cannot serve it, that is the explicit
  fallback path the user's sequencing accepts -- file a new M-pre
  slice to reshape the spine, not a per-category inline regression.

End of ruling.
