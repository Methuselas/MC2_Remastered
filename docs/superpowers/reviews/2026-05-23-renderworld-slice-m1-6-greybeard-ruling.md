# Greybeard ruling -- RenderWorld Slice M1.6 static-prop pick integration

- Subject: `MissionInterfaceManager::tryStaticPropPick` helper at
  `code/missiongui.cpp:6168-6273` + `moverSelectedThisFrame` 4-site
  fallback observable at `code/missiongui.cpp:1460,1487,1690,1705` +
  viewport-to-FBO coord scaling via `gos_GetViewport` (introduced
  at `1853ad9`).
- Skill: `.claude/skills/greybeard.md` verbatim.
- Date: 2026-05-23.
- Reviewer stance: fresh ruling, no pre-judged verdict. Evidence
  grep-grounded against HEAD `1853ad9`.

---

## 1. Subsystem pin

Gameplay-side input dispatch in `code/missiongui.cpp` consuming the
M1.5 RenderWorld substrate. First-hand evidence:

- 4 mover-gate writer sites confirmed by `git show 32ee49b`: line
  1467 (Shift+additive, oldStyle), 1502 (plain-LMB, oldStyle), 1731
  (Shift+additive, AOE), 1755 (plain-LMB, AOE). The 4 sibling
  `setSelected(false)` sites at neighboring lines are EXPLICITLY
  not instrumented per spec Q6 rationale.
- Helper called once at the tail of each style body (oldStyle tail,
  AOE tail). Both callers pass identical 7 args; only one helper
  body exists.
- Coord-translation lives in helper only: `gos_GetViewport` query
  + scale + Y-flip happen at `code/missiongui.cpp:6213-6233`. No
  other call site in the worktree translates UI-canvas mouse to
  FBO pixels for the M1.5 substrate.
- M1.5 substrate (`RenderWorld::lookupAtPixel`,
  `setLastStaticPropPick`, `IsObjectIdBufferEnabled`,
  `IsStaticPropPickEnabled`) is unchanged by M1.6; M1.6 is purely
  a consumer.

Pin: gameplay-side substrate-consumer wiring in missiongui input
dispatch. NOT a render-pipeline change.

## 2. Symptom vs cause

- **Proximate symptom (what M1.6 fixes for static props):** the
  M1.5 substrate is invisible to the user. `lookupAtPixel` is
  reachable only via the passive selftest. There is no gesture that
  turns a click into a handle.
- **Upstream condition (the bug class M1.6 must address):**
  gameplay-side wiring of a render substrate has a recurring shape
  that, if reinvented per slice, generates a class of integration
  bugs. The four hazards visible in M1.6 alone:
  1. Env-gate ordering (the cheap cached-bool check must precede
     any FBO bind / `glReadPixels`).
  2. Mover-vs-substrate priority (without a per-frame observable,
     Shift+LMB on a mech logs a building behind it -- the M2
     coverage gap leak).
  3. Coord-space translation (UI mouse is in 800x600 viewport
     space; FBO is at drawable resolution; identity assumption
     produces the `22 misses / 0 hits` symptom from `1853ad9`).
  4. Per-mission lifecycle (mid-pick mission-end leaves a stale
     handle that fails generation check forever, confuses log
     readers).
  Each future slice (M2 mech pick, M3 terrain pick, M4 VFX pick,
  M5 overlay pick) would hit all four hazards on its own if M1.6
  established a per-prop pattern instead of a reusable spine.

## 3. The meta-fix

**Candidate META-FIX:** the `tryStaticPropPick` helper signature +
the `moverSelectedThisFrame` fallback-observable pattern + the
viewport-to-FBO coord translation, considered as ONE pattern, is
the canonical extension surface for gameplay-side substrate
consumers.

Examined honestly, the helper is NOT yet that pattern. Three
things separate it from a META-FIX:

- **The helper is type-specific.** `tryStaticPropPick` returns a
  `LookupResult` whose `recipeIndex` lookup goes through
  `handleToRecipeIndex` -- which today is `handle.index()` (M1.5
  identity mapping) but is conceptually "static-prop recipe table
  index." M2 mechs will not have a recipe index; they will have a
  GameObject ID through a different mapper. The helper body
  contains no static-prop-only invariants, but its name and its
  log schema (`[STATIC_PROP_PICK v1] hit handle=... recipe=...`)
  bake in the type assumption.
- **The mover-gate observable is selection-mechanic-specific.**
  `moverSelectedThisFrame` is set after `setSelected(true)` on a
  `Mover*`. If M2 (mech pickup) changes mover-selection mechanics
  -- e.g. priority-based pick that competes with substrate
  results -- the observable's semantics change. The 4 sites today
  are correct because mover selection is a pre-substrate gesture;
  the moment substrate becomes authoritative (Section 2 of the
  M1.6 spec: "After M2... revisit"), the gate semantics flip.
- **The coord-scaling fix at `1853ad9` is `gos_GetViewport`-
  derived but lives inline.** Plan author's own commit message:
  "Consider extracting a static helper screenToFboPixel(...)
  when M2 lands." It is a local fix today, not a centralized
  translation. M2 will COPY this block, not call it.

Substitutive analysis: the META-FIX shape would be a
`tryGameplayPick(GameplayPickRequest)` helper that takes the click
gesture + a "what writes to attachment-2 currently" query and
dispatches to type-specific consumers. M1.6 did NOT build that. It
built one type-specific helper with an inline coord translation
that the author flagged for extraction in M2.

## 4. Substitutive test

This is additive, not substitutive:

- M1.6 adds `tryStaticPropPick` alongside the existing legacy
  selection paths. It does not delete or repoint any prior input-
  dispatch path. Section 11 of the spec explicitly forbids any
  replacement of legacy verbs.
- The coord-scaling block at `:6213-6233` is fresh code; nothing
  pre-existing was replaced.
- The 4 `moverSelectedThisFrame = true` instrumentations are
  additive observables; the underlying `setSelected(true)` writers
  are untouched.

Additive is appropriate at this slice (Section 11: "every touch
has blast radius"). But it means the helper is NOT yet a
substitutive central spine; it is a first instance. M2 either (a)
copies the pattern and we end up with four near-duplicate helpers
(static / mech / terrain / VFX), each carrying its own
coord-scale block; or (b) the M2 plan extracts the spine, which is
the actual META-FIX. (a) is the bug class re-emerging; (b) is
deferring the META-FIX to M2.

The CPU zone (no old gameplay-side wiring exists) has nothing to
delete, so the substitutive criterion is N/A in the strict sense.
What matters is whether the NEW path becomes the reusable spine.
Today it does not.

## 5. Verdict

**PATCH (justified).**

### The meta-fix (named, not implemented in M1.6)

Extract `tryGameplayPick(request)` as the single entry point for
all gameplay-side substrate consumers. Its body:

1. Cheap cached env-gate (substrate + wiring + verbosity).
2. Gesture gate (modifier + edge-triggered click + GUI-region
   filter + double-click filter).
3. Caller-supplied `legacyPathConsumedThisFrame` observable (the
   shape generalized from `moverSelectedThisFrame`).
4. Off-screen guard against viewport-space bounds.
5. Centralized `screenToFboPixel(mouseX, mouseY) -> (fboX, fboY)`
   using `gos_GetViewport`. SHARED with any other slice that
   needs UI-mouse-to-FBO translation (e.g. world-point lookups).
6. `RenderWorld::lookupAtPixel(glX, glY) -> LookupResult`.
7. Caller-supplied "what to do on hit" + "what to do on miss"
   callback or strategy. Log schema versioned per consumer
   (`[STATIC_PROP_PICK v1]`, `[MECH_PICK v1]`, etc).

The 4-site `moverSelectedThisFrame` instrumentation pattern stays
as-is in `updateOldStyle` / `updateAOEStyle`; the gate is correct
where the legacy code WRITES selection state. The helper just
receives the bool.

### Deferral reason (the debt)

Three reasons M1.6 ships as PATCH:

1. **Pattern not yet validated.** With only one consumer (static
   props), the right generalization is a guess. Premature
   abstraction would bake in static-prop assumptions and force M2
   to either fight the API or fork it. The brainstorm/refactor
   pass for the spine should happen WHEN M2 lands and the
   delta-between-consumers is concrete.
2. **Selection-priority semantics will change at M2.** Per spec
   Section 2: "until M2 writes mech IDs into attachment-2, GPU
   pickup MUST be a fallback after legacy mover picking." M2
   inverts this. The mover-gate observable's semantics flip at
   that point; designing the spine NOW would lock in the M1.6
   fallback shape and require rework anyway.
3. **Coord-scaling helper extraction is explicitly deferred by
   the author.** `1853ad9` commit message: "Consider extracting
   a static helper screenToFboPixel when M2 lands." This is the
   correct cadence: extract on the second consumer, not the
   first.

### Trigger condition for taking on the META-FIX

When M2 (mech pickup) begins planning, the M2 spec MUST:

- Reuse the `screenToFboPixel` translation via a shared static
  helper (extracted as the FIRST task of M2).
- Reuse the `tryGameplayPick` spine; the M1.6 helper becomes the
  static-prop strategy plug-in.
- Replace `moverSelectedThisFrame` with the generalized observable
  name (the M1.6 sites stay, just the type of the bool's
  consumer changes).

If M2 plan instead copies the helper as `tryMechPick` with its own
inline `gos_GetViewport` block, the bug class has re-emerged and
the META-FIX must be done as a separate slice between M2 and M3.

### Filed debt artifact

This ruling itself is the debt record. Plan-stage author should
add a note to the M2 spec when it lands:

> "M1.6 greybeard ruling 2026-05-23 named the
> `tryGameplayPick` spine + `screenToFboPixel` helper as the
> deferred META-FIX. M2 plan must extract both before adding the
> mech-pick strategy, OR explicitly justify deferring to M3."

### Why this is NOT META-FIX

The M1.5 `setSceneDrawBuffers` ruling is the bar. That helper:
- has 5 simultaneous call sites consolidated in one commit;
- has a grep gate (`grep -n glDrawBuffers gos_postprocess.cpp`)
  that mechanically detects regressions;
- replaces a documented per-site policy-drift bug class for which
  multiple instances existed;
- the OLD path is GONE.

M1.6 has 1 consumer, no grep gate (because `lookupAtPixel` is
only called from one site -- there is no policy to enforce yet),
no consolidation (additive), and the author flags the extraction
for the next slice. The pattern may BECOME a META-FIX at M2; it
is not one yet.

---

## Closing recommendation

Adopt as **PATCH (justified)**. Record the named META-FIX
(`tryGameplayPick` + `screenToFboPixel`) and the M2 trigger
condition in slice closing notes. When M2 spec lands, the
greybeard pass for M2 MUST explicitly state whether the META-FIX
is taken in M2 or deferred again -- repeated deferral with no
trigger ratchet is exactly the additive-slice anti-pattern
documented in `memory/feedback_offload_must_be_substitutive_not_additive.md`.

Sub-judgment on the coord-scaling fix at `1853ad9`: also PATCH
(justified). It is the first known consumer of UI-mouse-to-FBO
translation; extracting `screenToFboPixel` on a sample size of one
would risk wrong-API guesses. Same M2 trigger applies.

End of ruling.
