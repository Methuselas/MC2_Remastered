# TASK-SCHEDULER-CORE-1 — Design-as-Built

## Summary

Deterministic per-warrior brain task queue substrate. Provides `BrainTaskQueue` (MAX 8 slots per warrior) with a four-key sort order ensuring deterministic drain across networked peers. Gate is default OFF; drain is an inert stub this slice (no tacOrder writes). ABL behavior is byte-identical when gate is OFF.

## Non-persistence statement

BrainTaskQueue is mission-runtime ephemeral state. MC2 campaign/checkpoint saves do not resume arbitrary mid-mission brain queues, so the queue is intentionally not serialized. If true mid-mission save/load support is confirmed later, persistence must be designed as a separate compatibility-aware slice.

## Hooks

| Hook | File | Line (approx) | Description |
|------|------|----------------|-------------|
| Init | `code/warrior.cpp` | ~931 | `brainTaskQueue = nullptr;` — after `brain = NULL` in `MechWarrior::init(bool create)` |
| Teardown | `code/warrior.cpp` | ~1539 | `delete brainTaskQueue; brainTaskQueue = nullptr;` — in `MechWarrior::destroy()` |
| Drain | `code/warrior.cpp` | ~5023 | Inside brain cadence gate — `initBrainTaskQGate()` + lazy alloc + `drain()` before `runBrain()` |

## Determinism key

Sort order (ascending = drain highest priority first):

1. `priority_tier` ascending (0 = highest)
2. `insertion_frame_ms` ascending (earlier sim-time = first)
3. `stable_seq_id` ascending (FIFO within tier+frame)
4. `warrior_id` ascending (last-resort stable numeric tie-break)

Where:
- `insertion_frame_ms = (uint32_t)(scenarioTime * 1000.0f + 0.5f)` — sim-time-derived millisecond ticks, NOT render frame (`g_mc2FrameCounter`), NOT wall clock
- `warrior_id = vehicleWID` — stable numeric `int32_t` (`GameObjectWatchID`), NOT pointer, NOT allocation order

No `std::unordered_*`, no hash iteration, no pointer comparison in sort key.

## Gates

| Gate | Default | Description |
|------|---------|-------------|
| `MC2_BRAIN_TASKQ` | OFF (0) | Enable per-warrior BrainTaskQueue alloc + drain |
| `MC2_BRAIN_TASKQ_TRACE` | OFF (0) | Emit `BRAIN_TASKQ:` lines to stderr each cadence tick |

Gate OFF = byte-identical behavior to pre-queue baseline. Gate checked once at first brain cadence tick via `initBrainTaskQGate()`.

## Save/load

NO save/load changes. No version bump. Queue is destroyed in `MechWarrior::destroy()` and not written to any save format.

## Files changed

- `code/brain_task_queue.h` — NEW: `BrainTaskEntry` struct + `BrainTaskQueue` class
- `code/warrior.h` — NEW member `BrainTaskQueue* brainTaskQueue = nullptr;` after `ABLModulePtr brain;`; new `#include "brain_task_queue.h"`
- `code/warrior.cpp` — gate statics + `initBrainTaskQGate()`; init/teardown/drain hooks
- `tests/smoke/brain_task_queue_test.cpp` — NEW: standalone host-compilable unit test (7 cases)

## Acceptance criteria evidence

| Check | Result |
|-------|--------|
| Unit test (9 assertions, 7 named cases) | ALL TESTS PASS |
| Build (RelWithDebInfo) | PASS (warnings only, no errors) |
| Smoke gate OFF (mc2_01 mc2_10 mc2_24) | PASS 3/3 (2026-06-23) |
| Smoke gate ON (MC2_BRAIN_TASKQ=1) | PASS 3/3 (2026-06-23) |
| No tacOrder writes | Confirmed by design (drain() is stub) |
| No save/load touch | Confirmed — zero save-file changes |
| Gate OFF byte-identical | Confirmed by code path — gate check short-circuits all new code |
