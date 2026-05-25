# Execution prompt — Track A1: Object Admission Predicate Replacement

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing **Track A1** of the MC3 rendering modernization arc. First slice of the predicate-replacement work. Single callsite, dual-output wrapper, ~1 week of code + ≥3 day soak.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths below are relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules. Documentation discipline (grep at write-time) is load-bearing for this slice.
2. `docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md` — **this is your plan.** 10 tasks, advisor-sharpened (5 sharpenings already applied).
3. `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` — Q1, Q2, Q3 (entry slice, dual-output scope, parity definition with dual-run + envelope).
4. `memory/cull_gates_are_load_bearing.md` — cascade hazard. Q3's `[DESTROY v1]` count + identity diff is the cascade-safety gate.
5. `memory/camera_model_oblique_cinematic.md` — camera shape context.

## Skill to invoke

**`superpowers:subagent-driven-development`** — fresh subagent per task with two-stage review between tasks. The plan's task structure (1-7 code, 8 soak, 9 flip, 10 memory) maps cleanly.

## Scope summary

Replace bool predicate at single callsite `code/gameobj.cpp:2090` (`projectForObjectAdmission`) with a clip-space frustum test (`clipSpaceFrustumAdmit`). Dual-output: new bool, legacy `projectZ` math preserves screen.x/y/z/w byte-identical. Killswitch via `MC2_OBJECT_ADMISSION_PREDICATE=modern|legacy` (default legacy until soak passes).

Tasks 1-3 build the substrate (predicate header → implementation with lazy probe → trace `homogClipFull` candidate that calls the SAME `clipSpaceFrustumAdmit` — drift impossible). Task 4 wires startup probe + selftest hard-fail. Task 5 wires the wrapper. Task 6 authors acceptance envelope from in-run heatmap. Task 7 verifies `[DESTROY v1]` count + identity parity. Task 8 soaks. Task 9 flips default. Task 10 memory.

## Gates

- **Build gate per task:** `cmake --build build64 --config RelWithDebInfo --target mc2` clean.
- **Visual canary per task touching the wrapper:** `py -3 scripts/run_smoke.py --mission mc2_01 --duration 15 --kill-existing`. 15-second smoke (per `memory/feedback_smoke_policy_30s_mc2_01.md`).
- **Pre-flip gate (Task 9):** five-criterion check from plan Step 8.5. **Hard failure conditions:** `[DESTROY v1]` count delta vs baseline (Task 7), tier1 5/5 visual smoke regression, disagreements outside the reviewed envelope (Task 6).
- **Selftest:** `MC2_OBJECT_ADMISSION_SELFTEST=1` runs 8 boundary cases at startup; hard-fail on any failure (advisor sharpening #2).

## Coordination

- **Independent of in-flight work.** Clip-control already shipped (commits `9e45718` + `4c8f9a4`); A1's predicate body is already in hardware-native form.
- **A1 must flip default-on BEFORE A2 enters soak** (Q15 sequential-with-overlap discipline — see brainstorm-decisions doc).
- **GL hygiene drop-ins** (`docs/superpowers/plans/2026-05-06-gl-hygiene-dropins.md`) may have shipped before this; doesn't affect A1.
- **Track B is GATED on A1 ship** — but you don't need to wait for B to start A2 next.

## When blocked

- **Build broken at HEAD before you start:** check `git log` and `git status`; flag any half-applied commits to the user. Do NOT attempt unrelated fixes.
- **Tier1 fails after Task 5 wrapper edit:** capture the artifact directory, look for `[DESTROY v1]` deltas first (cascade signal), then visual diff. Bisect by setting `MC2_OBJECT_ADMISSION_PREDICATE=legacy` to confirm regression is wrapper-induced.
- **`[DESTROY v1]` count delta in Task 7:** Track A1 fails the parity gate. Do NOT proceed to default-on. Investigate per Task 7 instructions; likely indicates cascade-cousin pathway. Hand back to user.
- **Q3 envelope authoring is stuck:** the predicted disagreement classes in plan Step 6.3 are HYPOTHESES to confirm against capture data. Refine the list based on what actually appears in the heatmap; the writeup is your judgment.

## Soak window

Task 8 is ≥3 days of `MC2_OBJECT_ADMISSION_PREDICATE=modern` runs. NOT a code task — pause execution after Task 7 ships clean and hand back to the user for soak observation. User runs daily smoke; you resume at Task 9 (flip) once soak passes the five-criterion check.

## Deliverable

Single feature branch, 9 commits (Tasks 1-7 implementation, Task 9 default flip, Task 10 memory). Plus the acceptance envelope spec at `docs/superpowers/specs/2026-05-06-track-a1-acceptance-envelope.md`.

After A1 default-on flip ships and soaks ≥3 days clean, **Track A2** is the next slice. Hand back to user before starting A2.
