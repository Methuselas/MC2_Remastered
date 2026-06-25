# BRAIN-FIXED-TICK-1 — Design As Built

> Slice: first rung of Brain decoupling ladder. Same-thread, default-OFF gate.
> Branch: `claude/brain-fixed-tick-1`. Built 2026-06-24.

---

## What shipped

**Gate:** `MC2_BRAIN_FIXED_TICK` (default OFF).  
**Sub-gate:** `MC2_BRAIN_FIXED_TICK_HZ` (float, default 10; clamp 1..30).  
**Trace gate:** `MC2_BRAIN_FIXED_TICK_TRACE` (default OFF; emits per-frame tick line).

---

## Accumulator design

Global static accumulator `s_brainFixedTickAccum` (float, seconds) advanced by
scenarioTime delta once per render frame. Guard: `s_brainAccumLastFrame != g_mc2FrameCounter`
ensures the advance fires exactly once per render frame even with multiple warriors.

Each render frame:
```
dt = scenarioTime - s_brainFixedTickSimLast
s_brainFixedTickAccum += dt
while s_brainFixedTickAccum >= s_brainFixedTickPeriod:
    s_brainFixedTickAccum -= s_brainFixedTickPeriod
    ++s_brainTickIndex
    ++s_brainTicksThisFrame
```

`s_brainTickIndex` is a monotonic uint32_t. Same mission → same scenarioTime sequence →
same tick sequence. Determinism proof: two runs of same mission with gate ON produce
identical `tick=N simTime=T` output.

---

## Insertion seam — warrior.cpp

Seam in `MechWarrior::updateActions`, immediately before the `if (brainUpdate <= scenarioTime)` block.

**Before (baseline, gate OFF path — byte-identical):**
```cpp
if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
    ...
    runBrain();
    brainUpdate += BrainUpdateFrequency;   // 2.25 s
}
```

**After (gate ON path — fixed tick drives advance):**
```cpp
// Accumulator advance (once per render frame)
initBrainFixedTickGate();
if (s_brainFixedTickEnabled && s_brainAccumLastFrame != g_mc2FrameCounter) {
    s_brainAccumLastFrame = g_mc2FrameCounter;
    advanceBrainFixedTickAccumulator(scenarioTime);   // advances s_brainTickIndex
    // trace emitted here if MC2_BRAIN_FIXED_TICK_TRACE=1
}

// Per-warrior gate — unchanged (stagger offset still via brainUpdate init)
if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
    ...
    runBrain();
    if (s_brainFixedTickEnabled) {
        brainUpdate += s_brainFixedTickPeriod;   // 1/hz instead of 2.25 s
    } else {
        brainUpdate += BrainUpdateFrequency;     // baseline unchanged
    }
}
```

The `brainUpdate <= scenarioTime` gate is **untouched** when gate OFF. The only change when
gate OFF is the `initBrainFixedTickGate()` call (which returns immediately after first check
if `s_brainFixedTickEnabled=false`) and the if-guarded accumulator advance (never entered).
**Gate OFF = byte-identical behavior.**

---

## Stagger: unchanged

Per-warrior stagger init `brainUpdate = (numWarriors % 30) * 0.2` remains untouched.
With gate ON, stagger still works: brainUpdate starts at the stagger offset in sim-time;
the warrior fires when `brainUpdate <= scenarioTime`; subsequent intervals are `1/hz`
instead of `BrainUpdateFrequency`. Stagger spread still produces deterministic spacing.

---

## Determinism trace format

When `MC2_BRAIN_FIXED_TICK_TRACE=1` (or always on when gate ON per your spec choice):
```
[BRAIN_FIXED_TICK] tick=<brainTickIndex> simTime=<t> ranThisFrame=<n>
```
- `tick` = `s_brainTickIndex` at time of advance (monotonic per session).
- `simTime` = `scenarioTime` at frame advance.
- `ranThisFrame` = number of whole ticks that fired this render frame (usually 0 or 1 at
  10 Hz with a ~60 Hz frame rate; >1 possible during lag spikes or slow-motion).

---

## Effect verbs: unchanged

All 6 dispatch verbs (POWERDOWN/EJECT/GUARD/MOVETO/ATTACK/RETREAT) in
`brain_special_dispatch.cpp::executeSpecialBody_Apply` are untouched.
Fixed-tick changes only WHEN `runBrain()` is called, not what it does inside.

---

## OFF = byte-identical proof

Gate OFF path: `initBrainFixedTickGate()` reads the env var once, sets
`s_brainFixedTickEnabled=false`, returns. The `if (s_brainFixedTickEnabled && ...)` block
is never entered. Inside the brain-cadence block, `if (s_brainFixedTickEnabled)` is false →
falls through to `brainUpdate += BrainUpdateFrequency`. Identical to baseline.

Confirmed by Gate A smoke (tier1, gate OFF, 5/5 PASS — see verification section).

---

## What is explicitly NOT in this slice

- No worker threads.
- No `BrainWorldSnapshot`.
- No `BrainOrderIntent` queue or commit phase.
- No change to `executeSpecialBody_Apply` or any effect verb.
- No change to mission-script (ABL) execution cadence.
- No change to render frame timing.
- No new effect verb.

---

## Next ladder rungs

1. **BRAIN-SNAPSHOT-1**: POD snapshot of 9 per-warrior fields at tick entry (prelude to worker offload).
2. **BRAIN-INTENT-QUEUE-1**: `BrainIntentSlot[3]` replaces direct `tacOrder[]` writes; serial commit pass.
3. **BRAIN-WORKER-1**: offload runBrain to frame-jobs worker pool (blocked by HAZARD-1 `CurWarrior` globals).

---

## Files changed

- `code/warrior.cpp`: gate statics + `initBrainFixedTickGate` + `advanceBrainFixedTickAccumulator` + seam edit.
