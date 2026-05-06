# Execution prompt — Track A2: Effects Admission Predicate Replacement

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing **Track A2** of the MC3 rendering modernization arc. Inherits substrate from A1 (predicate function, env probe, trace `homogClipFull`); rewires 7 effect-admission callsites to the same predicate.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths below are relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules.
2. `docs/superpowers/plans/2026-05-06-track-a2-effects-admission-predicate.md` — **this is your plan.** 8 tasks (smaller than A1's 10 because substrate already exists).
3. `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` — Q1 (A1→A2 ordering), Q2 (dual-output), Q3 (parity), **Q15 (sequential-with-overlap soak — A1 must already be flipped default-on before A2 enters soak)**.
4. `docs/superpowers/explorations/2026-05-06-track-a-predicate-replacement-recon.md` — recon backing the slice.
5. `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md` — reference for the substrate shape A2 inherits.

## Skill to invoke

**`superpowers:subagent-driven-development`** — fresh subagent per task. A2 is structurally A1's cousin; the discipline is the same.

## Scope summary

7 callsites to rewire (cloud/crater/weather), each consuming `Camera::projectForEffectAdmission` from `mclib/camera.h:557-570`. New env flag `MC2_EFFECT_ADMISSION_PREDICATE=modern|legacy` (default legacy until soak). The predicate function (`clipSpaceFrustumAdmit`) and trace candidate are already shipped from A1 — A2 only changes the wrapper body.

Per the recon: **zero callsite code changes needed.** All 7 sites consume `screen.{x,y,z,w}` but the dual-output wrapper preserves screen output byte-identical via the existing `projectZ` call. Only the wrapper body changes.

## Critical prerequisite — DO NOT START UNTIL A1 FLIPS

Per Q15 sequential-with-overlap discipline: A1 must have shipped its default-on flip (Task 9 of the A1 plan) AND soaked clean ≥3 days BEFORE A2 enters its own soak. This produces the production-relevant joint configuration (A1+A2 both modern) as the actual soak target — clean attribution if anything regresses.

If A1 has NOT flipped default-on: **stop, hand back to user.**

## Gates

- **Build per task:** clean.
- **Visual canary per task:** 15s mc2_01 mission smoke (`py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing`).
- **Effect-specific visual canaries** (the real signals — DESTROY parity is defense-in-depth here because effects aren't lifecycle-cascade producers):
  - mc2_03: rain (`code/weather.cpp:497,500` — screen.z consumer for ambient).
  - mc2_24: craters/explosions during heavy combat.
  - Default biomes: cloud shadows.
- **Soak under joint A1+A2-modern config** (Q15) — A1 already default-on, set `MC2_EFFECT_ADMISSION_PREDICATE=modern` and run.
- **Out-of-envelope conditions** (hard fail): per-site disagreement count >5x A1's authored envelope baseline; new disagreement classes not in the candidate list.

## Coordination

- A1 must be default-on. Verify via `[INSTR v1] object_admission_mode=modern` in pre-soak log.
- Track B execution is INDEPENDENT of A2 — runs on a parallel branch gated by alpha-test prep + A1 flip. Don't block on B.
- Track C0 may also be running in parallel — also independent.

## When blocked

- **A1 hasn't flipped:** stop. Wait for A1. Don't start A2 in parallel — Q15 sequential discipline protects regression attribution.
- **Tier1 fails under A2-modern but A1-modern alone is clean:** likely an A1-A2 interaction. Bisect: set `MC2_OBJECT_ADMISSION_PREDICATE=legacy` while keeping A2 modern — if regression disappears, the predicates interact. Hand back to user.
- **One of the 7 sites turns out to need callsite-level changes** (recon expected zero, but verify per-site at plan-time): document the surprise, scope the additional work, hand back.
- **Disagreement count outside envelope:** review per-site heatmap; refine envelope OR investigate cause. If predicate is over/under-culling rain/cloud/crater visibly, that's a bug; if it's just an unauthored class, extend envelope.

## Soak window

Task 7 is ≥3 days of joint A1+A2-modern soak. Pause after Task 6 ships clean; hand back to user for soak observation. Resume at Task 8 (default flip + memory) once soak passes.

## Deliverable

Single feature branch, 7 commits + memory. Acceptance envelope at `docs/superpowers/specs/2026-05-06-track-a2-acceptance-envelope.md`.

After A2 default-on flips and soaks ≥3 days, **Track A3 (terrain admission)** becomes a data-driven decision per `docs/superpowers/specs/2026-05-06-track-a3-terrain-admission-sketch.md`. May or may not happen; review post-soak.

Track B and Track C0 are independent slices that may already be in flight.
