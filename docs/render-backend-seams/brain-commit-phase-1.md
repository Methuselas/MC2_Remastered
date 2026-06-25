# BRAIN-COMMIT-PHASE-1 — rung 7 of the Brain decoupling ladder

**Status:** built on branch `claude/brain-commit-phase-1` off nifty `6fa59120`. Build green (confirm after build).

## What this rung does

Separates the per-warrior inline `commitBrainIntents` calls (rung 5 — BRAIN-DECISION-INTENT-QUEUE-1) into a
**distinct main-thread COMMIT PHASE** that runs after ALL warriors have decided, draining their pending
intents in stable ascending WID order.

This is the last structural piece before threading (rung 8 = worker dispatch). The commit phase is still
100% main-thread; the only change is WHEN commit runs (after the loop) and the deterministic WID ordering.

## Gate semantics

| Gate | Default | Behavior |
|---|---|---|
| `MC2_BRAIN_COMMIT_PHASE` | OFF | Inline commit per-warrior (rung-5 behavior, unchanged) |
| `MC2_BRAIN_COMMIT_PHASE=1` | — | Requires `MC2_BRAIN_INTENT_QUEUE=1`; inline commits skipped; deferred phase commits in WID order |

Gate OFF = byte-identical to rung-5 inline behavior. Gate ON with `MC2_BRAIN_INTENT_QUEUE=0` = no-op (phase
gate checks both gates internally).

## Defer mechanism

When `MC2_BRAIN_COMMIT_PHASE=1`, the two inline `commitBrainIntents` calls in `warrior.cpp` are
**skipped** via `if (s_intentQueue && !s_brainCommitPhase)` guard. The intents remain in
`runtime->pendingIntents[]` (per-warrior POD array, already surviving past `runBrain` as per rung-5 design).
`pendingIntentCount > 0` signals the commit phase that this warrior has work.

## Commit-phase function and insertion point

`commitAllBrainIntents()` — implemented as an inline block inside `GameObjectManager::update()` in
`code/objmgr.cpp`, immediately after the mover removal loop, before `if (other)`.

**Insertion point:** `objmgr.cpp` after line ~3220 (after `mission->removeMover` loop, still inside
`if (movers)` closing block).

The function:
1. Iterates `moverList[0..numMovers-1]`, collecting all live movers whose pilot has `pendingIntentCount > 0`.
2. Sorts the collected pairs by ascending `mover->getWatchID()` (stable deterministic order, NOT iteration/pointer order).
3. For each in sorted order, calls `commitBrainIntents(pilot, rt)` — the existing rung-5 commit function,
   the sole caller of `setGeneralTacOrder` when `MC2_BRAIN_INTENT_QUEUE=1`.
4. Emits `[BRAIN_COMMIT_PHASE] committed=<n> warriors in WID order` once per frame (only when n > 0).

## Stable WID ordering — rationale

Worker threads (rung 8) will run warriors in parallel. For deterministic replay and offline harness
validation, the ORDER in which commits are applied across warriors must be stable across runs. WID
(Watch ID) is a stable per-warrior integer assigned at object creation and does not change during a
mission. Sorting ascending by WID gives a canonical ordering independent of memory layout, moverList
insertion order, or iteration sequence.

For single-warrior missions (mc2_01) the ordering is trivially identical to inline. For multi-warrior
missions the order of setGeneralTacOrder application across warriors becomes WID-stable.

## Files changed

| File | Change |
|---|---|
| `code/warrior.cpp` | Added `s_brainCommitPhase` static gate. Inline commit calls now gated: `if (s_intentQueue && !s_brainCommitPhase)` |
| `code/objmgr.cpp` | Added `#include "brain_special_dispatch.h"` + `"warrior.h"`. Added `commitAllBrainIntents` block after mover removal loop |

## What is deferred to rung 8 (JOB-WORKER)

- Worker-thread dispatch of `runBrain` (HAZARD-1: `CurWarrior` process globals block this)
- `BrainWorldSnapshot` pre-copy before worker dispatch
- `g_isBrainWorker` TLS guard (mirror of `g_isFrameJobsWorker`)
- Epoch stamp on commit to prevent alarm re-entry double-commit
- The ~8 legacy ABL-direct mutations (`orderAttackObject`, `setAlarmTacOrder`, etc.)

## Acceptance

Gate OFF: mc2_01 mc2_24 tier1 — byte-identical to rung-5. Gate ON: same order types, same count, same
warriors; only difference is WID-sorted application order (single-warrior missions: identical).
