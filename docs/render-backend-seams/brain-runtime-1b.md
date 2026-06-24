# BRAIN-RUNTIME-1B — first applied Brain-runtime behavior (design-as-built)

First behavior-changing slice of the Brain & AI 2.14 arc. One unit, one task, one slot, one proof. Default OFF. Builds on BRAIN-RUNTIME-1A (scaffold) + TASK-SCHEDULER-CORE-1.

## Corrected control-flow ladder (the seam)
- `MC2_BRAIN_RUNTIME=1, MC2_BRAIN_RUNTIME_APPLY=0`: load+trace mode; ABL runs normally; **no short-circuit, no HOLD drain, no slot write → byte-identical**.
- `MC2_BRAIN_RUNTIME=1, MC2_BRAIN_RUNTIME_APPLY=1`: Enhanced mode short-circuits ABL; one `HOLD_TASK` drains; one `setGeneralTacOrder(STOP)`; unit stops.
Both gates default OFF; APPLY requires RUNTIME (warns + stays inert otherwise).

## Bug fixed during build (housekeeping-leak)
The initial implementation placed the short-circuit at the OUTER caller seam, skipping the **whole `runBrain()`** under APPLY=1+Enhanced — which would drop `runBrain`'s `clearAlarmsHistory()` + `CurGroup/CurObject/CurWarrior/CurContact = NULL` context cleanup (the "global brain context leaks" class). **Relocated the short-circuit INSIDE `runBrain()`** (warrior.cpp ~2216): `enhancedApply` gates only the `brain->execute()` + ABL-order + `brainErr`-read block; the context clear (warrior.cpp ~2278-2283) and `clearAlarmsHistory()` (~2285) are **unconditional, outside the branch**, so they run in BOTH paths. The outer seam now only does 1A mode-detect + the APPLY=0 would-own trace, then calls `runBrain()` normally.

## brainErr disposition
`brainErr` is declared `= 0` before the branch; the legacy path sets it from `brain->getInteger()`; the Enhanced-APPLY path leaves it 0. Documented: APPLY=1+Enhanced does not invoke ABL for the warrior this tick, so no ABL `brainErr` is produced (the `switch(brainErr)` is intentionally not reached). APPLY=0 keeps `brainErr` flowing normally.

## Files
- `code/brain_task_queue.h` — `BrainTaskType` enum + `BRAIN_TASK_HOLD` + `type` field + `drainWithTask()`.
- `code/mech_brain_runtime.h` — `initialHoldPushed` flag.
- `code/warrior.cpp` — gates (`MC2_BRAIN_RUNTIME_APPLY`), the in-`runBrain` short-circuit + HOLD apply (~2216-2287), seam revert.
- `code/warrior.h` — (member already from 1A).
- `code/mission.cpp` (~3037) — minimal per-unit `Brain{}` loader: opens `data/missions/<mission>_ai.fit`, seeks `[Brain]`, reads `unitRef` (string) + `mode` (long: 1=Hybrid, 2=Enhanced), `Warrior%d`→`warriorList[idx]`, `setBrainRuntimeMode`. Single-block fixture only (no seekBlock multi-same-name reliance). `MC2_BRAIN_RUNTIME_FORCE_MODE` stays as post-load global override.
- `scripts/run_smoke.py` — env allowlist for `MC2_BRAIN_RUNTIME*` (else dropped by subprocess.Popen).
- `tests/fixtures/brain_runtime/mc2_01_ai.fit` — test fixture (Warrior4 Enhanced). To exercise APPLY=1, copy it to `<deploy>/data/missions/mc2_01_ai.fit` (the path the loader seeks). Not auto-deployed; `data/*` is gitignored so the fixture is tracked under tests/.

## Save model
`MechBrainRuntime` + `BrainTaskQueue` are mission-runtime ephemeral state, NOT serialized (same policy as the scheduler). No save-format change, no version bump.

## Acceptance evidence (fingerprint exe = b31095c8 built-from-dirty 1B tree, v0.4c)
- Build: GREEN.
- gate OFF mc2_01: PASS, byte-identical (one `heartbeat` flake on first run, PASS on re-run; no [BRAIN_RT] output).
- RUNTIME=1 APPLY=0 mc2_01: PASS, byte-identical, NO `HOLD_TASK applied` trace.
- RUNTIME=1 APPLY=1 + fixture mc2_01: PASS, and exactly:
  ```
  [BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=mc2_01 seeking data/missions/mc2_01_ai.fit
  [BRAIN_RT] FIT load warriorIdx=4 unitRef=Warrior4 mode=Enhanced
  [BRAIN_RT] HOLD_TASK applied wid=4     (exactly ONE — initialHoldPushed guard)
  ```
- Housekeeping outside the branch: code-verified at warrior.cpp ~2278-2285.
- No save change; `gameosmain.cpp` debug probe (added mid-build) reverted.

## Deferred (NOT this slice)
REQUEST_ORDERS, commander chain/recursion, OPORD slot advancement, Hybrid PLAYER-slot apply, BrainSpecial/DO-command VM, variable store, archetype resolver, save/load, multi-warrior `_ai.fit`.
