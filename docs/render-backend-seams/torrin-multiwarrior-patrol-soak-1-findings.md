# TORRIN-MULTIWARRIOR-PATROL-SOAK-1 — findings (NOT a clean pass)

**Date:** 2026-06-25. Advisor next-session item #3. Goal: close the rung-7 multi-warrior
ordered-commit confidence gap (multiple warriors patrol → stable WID-ordered commits,
no nondeterminism / deadlock / arrival cross-talk).

**Setup:** mc2_24 (stock multi-mech mission, chosen over torrin because torrin is a
DarkRain mod mission whose specials-fixture path needs separate wiring — recon flagged
it uses ABL brains not Enhanced TechScript). All-Enhanced via `MC2_BRAIN_RUNTIME_FORCE_MODE=enhanced`,
patrol fixture (2-waypoint loop), `MC2_BRAIN_COMMIT_PHASE=1 MC2_BRAIN_INTENT_QUEUE=1
MC2_BRAIN_PATROL=1`, 120s.

**Outcome: mission PASS (no crash), but the multi-warrior ordered-commit invariant could
NOT be proven — the soak surfaced TWO real interaction gaps.**

## Gap A — multi-warrior specials application is mission-load-time + ai.fit-gated
- The specials body (`MC2_BRAIN_SPECIAL_FIT`) parsed "1 verbs" and only **Warrior4 (wid=4)**
  patrolled, despite `FORCE_MODE=enhanced`.
- Root cause: the per-warrior specials-application loop in `mission.cpp` runs at MISSION LOAD
  and only attaches the specials body to warriors that already have `brainRuntime` at that
  point — i.e. those with an `ai.fit` Brain record (Warrior4). The other warriors get
  `brainRuntime` allocated LAZILY in `runBrain` (confirmed: tactic-select fires for many
  wids in a separate run), AFTER the specials loop has run, so they never receive the body.
- **Fix direction (next session):** either (a) author an `ai.fit` with Brain records for ALL
  target warriors, or (b) re-apply / defer the specials-body attachment to the lazy-allocation
  path so any Enhanced warrior gets it. (b) is the general fix and the real enabler for
  multi-warrior brain content.

## Gap B — patrol (per-tick re-emit) does NOT compose with the deferred commit phase
- With `MC2_BRAIN_COMMIT_PHASE=1`, inline commit is skipped (warrior.cpp:2482/2491) and
  patrol's `tickPatrolAdvance` (warrior.cpp:~2512) emits MOVETO into `pendingIntents`. The
  deferred commit phase (objmgr.cpp ~3222) should drain them — but `[BRAIN_COMMIT_PHASE]
  committed=N` never fired (committed=0), while 4 `PATROL_ADVANCE` did occur.
- Patrol was only ever validated with commit-phase OFF (inline commit — the earlier arrival
  smoke showed 16 inline `[BRAIN_INTENT_COMMIT]`). Patrol × deferred-commit-phase is an
  UNTESTED combination, now shown broken: patrol's intents strand (emitted, never committed
  by the deferred phase).
- **Fix direction (next session):** investigate the timing — patrol's tickPatrolAdvance emit
  vs when the deferred phase reads pendingIntents (does patrol emit after the deferred phase
  ran that frame? does something clear pendingIntents between?). Likely patrol needs to emit
  on the same path/timing the deferred commit phase drains, or the deferred phase needs to
  cover the patrol-advance emit point.

## What WAS confirmed
- mc2_24 ran 120s with all gates on, NO crash, NO deadlock (single-warrior patrol active).
- Single-warrior patrol cycling works in-engine (proven earlier on mc2_01: 15 advances, looping).

## Net
The rung-7 multi-warrior ordered-commit gap is STILL OPEN. Two concrete prerequisite fixes
identified (Gap A: multi-warrior specials attachment; Gap B: patrol × commit-phase). Both are
real interaction bugs the single-warrior tests could not surface. Recommend both as next-session
slices BEFORE re-attempting the multi-warrior soak. torrin (real AI-vs-AI mod content) remains
the eventual richest target but needs Enhanced-brain wiring for its movers on top of Gap A/B.

---

## RE-SOAK 2026-06-25 — Gap A + Gap B FIXED, rung-7 gap CLOSED (one new narrower finding)

After GAP A (mission-level specials cache + lazy-apply) and GAP B (patrol inline-commit gated
on !commit-phase) shipped (merged nifty, commits `decd5d34` etc), re-ran the SAME mc2_24 setup
(FORCE_MODE=enhanced, `mc2_24_patrol` 2-wp loop, COMMIT_PHASE+INTENT_QUEUE+PATROL, seed=7, 30s,
exe `6948f4bb`):

- **Gap A CLOSED:** 70 distinct wids PATROL_ADVANCE (was 1=Warrior4), 69 `GAP-A: applied cached`.
- **Gap B CLOSED:** deferred `[BRAIN_COMMIT_PHASE] committed=1/2/3` (was 0) — multiple warriors
  committed per deferred WID-ordered drain. 172 advances all committed via the deferred phase.
- **STABLE across runs:** two identical-env runs both 70 wids / 69 applies / 172-173 advances
  (the ±1 is real-time arrival variance in the 30s window). No crash, no deadlock.
- **rung-7 multi-warrior ordered-commit gap = CLOSED** for the Enhanced+patrol+commit-phase combo.

**NEW FINDING — GAP-A × TACTIC_WEIGHTS pre-emption (narrower interaction):**
Adding `MC2_TACTIC_WEIGHTS=1` collapses multi-warrior patrol 70→1 (gapA_applies 69→0). Mechanism
CONFIRMED: the [Tactics] loader at `code/mission.cpp:3151` eager-allocates `brainRuntime`
(`setBrainRuntimeMode(Legacy)`) for EVERY warrior at mission load so tactic weights can attach.
That pre-empts GAP-A's lazy-alloc-and-apply path (`warrior.cpp:~5454 if(!brainRuntime){…apply
cache}`) — brainRuntime already exists, so the `if(!brainRuntime)` block is skipped and
`applyCachedSpecialBodyToRuntime` never runs. Only Warrior4 (real `_ai.fit` specials) dispatches.
- **Root cause:** GAP-A's cache-apply is coupled to the "allocated-because-null" path; any
  pre-existing brainRuntime (tactic-weights now; future eager allocators later) silently bypasses it.
- **Fix (small, GAP-A-ROBUSTNESS-1):** decouple — call `applyCachedSpecialBodyToRuntime(brainRuntime)`
  whenever `brainRuntime && !brainRuntime->specialBody.loaded` (already a no-op otherwise), not only
  inside the `if(!brainRuntime)` alloc block. ~1-2 lines in warrior.cpp runBrain pre-dispatch region.
- Soak primary goal is unaffected (gaps A/B closed without tactic-weights); this is a separate
  composition bug for the tactic-weights + multi-warrior-specials combo.
