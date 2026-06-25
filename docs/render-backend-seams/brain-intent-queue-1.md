# BRAIN-DECISION-INTENT-QUEUE-1 — design as built

**Status:** built on branch `claude/brain-intent-queue-1` off nifty `c30a03f0`. Build green. Implementing agent died mid-flight before commit/gates/doc (same recovery pattern as COREMOVETO-1 / COREATTACK-1); code complete on disk, this doc + commit + gate matrix completed from the main process.

## Scope (ladder rung 5 — the key transition)

The 6 effect verbs (POWERDOWN/EJECT/GUARD/MOVETO/ATTACK/RETREAT) stop calling `setGeneralTacOrder` directly. When `MC2_BRAIN_INTENT_QUEUE=1`, each handler **emits** a `BrainOrderIntent` into `runtime->pendingIntents[]`; an inline same-thread `commitBrainIntents()` drains the buffer and applies the orders. Gate default-OFF = byte-identical direct dispatch. Commit stays inline this rung (a SEPARATED main-thread phase is rung 7).

## Gate semantics (the byte-identical proof)

`brain_special_dispatch.cpp:548`:
```cpp
MechBrainRuntime* runtime = (warrior && s_intentQueueEnabled()) ? warrior->getBrainRuntime() : nullptr;
```
- Gate OFF → `s_intentQueueEnabled()` false → `runtime == nullptr` → every verb handler takes the `else` branch and calls `warrior->setGeneralTacOrder(...)` directly (unchanged from c30a03f0). **Byte-identical.**
- Gate ON → `runtime` non-null → handlers call `emitBrainIntent(...)` and emit `[BRAIN_INTENT_EMIT]`; no `setGeneralTacOrder` in the handler.

The gated `runtime` local is used ONLY for intent emit. The Var.Set/Get handlers use the separate `varStore` parameter (unaffected by the gate).

## Structure

| Piece | Location |
|---|---|
| `BrainOrderIntent` struct (POD, stub-linkable) | `code/brain_order_intent.h` |
| `pendingIntents[kBrainIntentCap]` + `pendingIntentCount` | `code/mech_brain_runtime.h` |
| `s_intentQueueEnabled()` gate | `brain_special_dispatch.cpp:478` |
| `emitBrainIntent()` | `brain_special_dispatch.cpp:489` |
| 6 verb handlers: emit/direct branch | POWERDOWN ~630/637, EJECT ~646/653, GUARD ~662/669, MOVETO ~710/729, ATTACK ~774/790, RETREAT ~? |
| `commitBrainIntents()` — sole `setGeneralTacOrder` caller when gate ON | `brain_special_dispatch.cpp:845` |
| commit call site | `warrior.cpp:2406, 2414` |
| checker update | `scripts/check_brain_relaxed_guard_doc.py` (counts handler direct-calls AND commit calls) |

## Forbidden-call guard moved

When gate ON, `commitBrainIntents()` is the ONLY function that calls `setGeneralTacOrder`. The emit phase calls zero order functions. Both doc-comment blocks + the `check_brain_relaxed_guard_doc.py` checker updated to reflect: gate-OFF = 6 direct call-sites in handlers; gate-ON = 6 call-sites in commit. The triple-guard (ATTACK bad-WID/self/friendly via `getByWatchID`) + MOVETO NaN guard stay at EMIT time — an invalid intent is never emitted.

## Once-guards

The 6 `*EffectApplied` flags gate intent EMISSION (checked/set before emitting), so each effect emits ≤1 intent per mission — identical once-semantics to the direct path. `dispatcherAppliedEffect` / HOLD suppression preserved (true when an intent was emitted+committed).

## Intent tick attribution

Each intent is stamped with `sourceBodyId` + `brainTick` (carries forward the FIXED-TICK `s_brainTickIndex`). This is the seam the SNAPSHOT/COMMIT-PHASE/JOB-WORKER rungs build on.

## Deferred (explicitly NOT this rung)

Worker threads; `BrainWorldSnapshot` (rung 6); a separated main-thread commit phase decoupled from the tick (rung 7); the ~8 legacy ABL-direct mutations; PLAYER/ALARM slots; any new effect verb.

## Acceptance gate matrix

Run from main process — see commit body / verification log. ABORT condition: gate-ON effect differs from gate-OFF for any verb (different order type/count/warrior).
