# BRAIN-RUNTIME-1A — Design As-Built

## Mode source chosen
Env override `MC2_BRAIN_RUNTIME_FORCE_MODE=legacy|hybrid|enhanced` only.
Real `mission_ai.fit` Brain-record loader deferred to BRAIN-RUNTIME-1B.
Default mode when env var is unset = Legacy.

## Compute+trace-not-apply contract
1A contract: `MechBrainRuntime::computeWouldOwnMask()` computes which tac-order
slots Brain WOULD own in the detected mode, emits `[BRAIN_RT]` trace if
`MC2_BRAIN_RUNTIME_TRACE=1`. No slot is written. `brain->execute()` (ABL) runs
unconditionally in all configurations. No ABL short-circuit.

## Key file:line references
| Item | Location |
|---|---|
| Struct definition | `code/mech_brain_runtime.h` (entire file) |
| Warrior field | `code/warrior.h:868` — `MechBrainRuntime* brainRuntime = nullptr;` |
| Init (nullptr) | `code/warrior.cpp:943` — `brainRuntime = nullptr;` |
| Destroy | `code/warrior.cpp:1555-1556` — `delete brainRuntime; brainRuntime = nullptr;` |
| Gate statics | `code/warrior.cpp:125-146` — `s_brainRuntimeEnabled` etc. + `initBrainRuntimeGate()` |
| Trace hook | `code/warrior.cpp:~5083-5106` — BRAIN_RUNTIME block after BRAIN_TASKQ drain |
| Smoke allowlist | `scripts/run_smoke.py:1346-1355` — `MC2_BRAIN_TASKQ/TRACE/RUNTIME/TRACE/FORCE_MODE` |

## Acceptance evidence

- Deploy fingerprint: `sha=7b2fee532b4d branch=claude/brain-runtime-1a` OK
- Gate-OFF (mc2_01, mc2_10, mc2_24, 30s): PASS 3/3 — `[DEPLOY_FINGERPRINT] OK`
- Gate-ON Legacy (mc2_01, mc2_24, 30s): PASS 2/2 — behavior identical to OFF
- Gate-ON Enhanced+trace (mc2_24, 15s): PASS 1/1

Sample `[BRAIN_RT]` trace line (verified from artifacts/2026-06-23T17-30-10/mc2_24.log):
```
[BRAIN_RT] wid=3 mode=Enhanced would-own GENERAL|PLAYER|ALARM (NOT applied)
[BRAIN_RT] wid=37 mode=Enhanced would-own GENERAL|PLAYER|ALARM (NOT applied)
[BRAIN_RT] wid=1 mode=Enhanced would-own GENERAL|PLAYER|ALARM (NOT applied)
```

## No-save confirmation
`MechBrainRuntime` is not serialized. Same non-persistence policy as `BrainTaskQueue`.
No save/load code added. If mid-mission save support is needed later, that is a
separate `BRAIN-SAVELOAD-1` slice.

## Smoke allowlist note
`MC2_BRAIN_TASKQ`, `MC2_BRAIN_RUNTIME`, `MC2_BRAIN_RUNTIME_TRACE`, and
`MC2_BRAIN_RUNTIME_FORCE_MODE` were not in `run_smoke.py`'s env forward allowlist.
Added in this slice — without them, `subprocess.Popen` drops the vars and gate-ON
smoke runs execute with gate-OFF behavior (runtime stays inert), masking regressions.
