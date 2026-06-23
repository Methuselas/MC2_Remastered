# TASK-SCHEDULER-CORE-RECON-1 — Design for the Brain Priority Task Queue

**Arc:** Brain & AI 2.14 / TechScript (GitHub discussion #19)  
**Date:** 2026-06-23  
**Author:** Claude Code  
**Status:** RECON/DESIGN — no code written; design only  
**Prereqs confirmed DONE:** BRAIN-FIT-SCHEMA-1 (data layer, checker, schema)

---

## Purpose

This document settles the design for `TASK-SCHEDULER-CORE-1` — the 7-tier priority task queue that
is the critical-path prerequisite for `BRAIN-RUNTIME-1`. It answers all 7 design questions mandated
by the recon prompt, grounded in current engine source (canonical worktree
`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`). No code is written here.

---

## §1 — Engine ground truth

### The existing runBrain gate

`code/warrior.cpp:5023` (condensed):

```cpp
if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
    runBrain();
    brainUpdate += BrainUpdateFrequency;  // 2.25 s game-time
}
```

`brainUpdate` is initialized at warrior creation (`warrior.cpp:937`):

```cpp
brainUpdate = (float)(numWarriors % 30) * 0.2;
```

`numWarriors` is `static int32_t MechWarrior::numWarriors` (`warrior.h:933`) — a running creation
counter incremented each time a warrior is constructed. The stagger is therefore **creation-order
based**: the N-th warrior created gets phase offset `(N % 30) × 200ms`, spreading the brain-fire
events across a 0–5.8 s window. `BrainUpdateFrequency` = 2.25 s (`warrior.cpp:225`).

**Inside `runBrain()`**: executes the ABL module (`brain->execute()`). The ABL script posts its
decision back to the GENERAL order slot via `setGeneralTacOrder()`, writing into
`tacOrder[ORDERSTATE_GENERAL]`. It does NOT push to the per-warrior FIFO queue — that queue is
used for player-issued multi-step move sequences.

### The tac-order data structures

`code/tacordr.h`:

| Structure | Key fields |
|---|---|
| `TacticalOrder` | `id` (warrior-local), `code` (25 enum values), `moveParams`, `attackParams`, `targetWID`, `time`, `stage`, `statusCode`; network: `pointLocalMoverId` (char), `groupFlags` (ulong), `data[2]` (8-byte opaque payload) |
| Order slots | `tacOrder[NUM_ORDERSTATES]` — 3 slots: `ORDERSTATE_GENERAL=0`, `ORDERSTATE_PLAYER=1`, `ORDERSTATE_ALARM=2` |
| Per-warrior FIFO | `tacOrderQueue[MAX_QUEUED_TACORDERS_PER_WARRIOR]` = 16 slots; `numTacOrdersQueued`; `tacOrderQueueLooping` |
| Global pool | `MAX_QUEUED_TACORDERS` = 2000 (allocated in 16-slot chunks) |

**There is NO existing priority task queue.** The tac-order FIFO is a flat 16-slot circular buffer
per warrior for queued move sequences. The 3-slot order-state array is a parallel priority override
(GENERAL < PLAYER < ALARM by usage convention, not a priority field).

### Save/load current state

Brain state is serialized in `copyToData(MechWarriorData&)` / `copyFromData(MechWarriorData&)`
(`warrior.cpp:8409–8705`) as raw binary packets via `PacketFile`. Fields currently saved:

- `brainUpdate` (float)
- `tacOrder[3]` (3 × `TacticalOrder` POD struct)
- `newTacOrderReceived[3]`, `orderState`, `timeOfLastOrders`
- `tacOrderQueue[16]` + `numTacOrdersQueued` + `tacOrderQueueLooping`
- `memory[NUM_MEMORY_CELLS]` (ABL brain memory cells)
- `brain` (ABL module pointer — pointer saved, re-initialized on load)

Version check: `versionStamp` string in `saveload.cpp:673`. There is **no integer version field** in
`MechWarriorData` itself — compatibility is the stamp string. A new struct member that changes the
binary layout requires incrementing that stamp and providing a migration path.

### Network determinism surface

`TacticalOrder` network fields: `pointLocalMoverId` (char), `groupFlags` (ulong), `data[2]`
(8 bytes opaque). Orders are communicated between peers via these fields. The ordering guarantee
today is per-slot overwrite (GENERAL/PLAYER/ALARM are not a FIFO; they are named slots, so the
determinism comes from "both peers write the same TacticalOrder to the same slot"). There is no
sequence number or monotonic counter in `TacticalOrder`.

---

## §2 — Design answer: 7-tier model → engine mapping

### The 7 GDD tiers

The GDD spec names (highest to lowest priority):

```
Tier 0: Logistics       (time-critical service jobs, capture, sabotage)
Tier 1: Alarm           (immediate threat response)
Tier 2: Combat          (active engagement decisions)
Tier 3: Tactical        (OPORD slot execution)
Tier 4: Movement        (waypoint traversal, repositioning)
Tier 5: Sensor/Comms    (scan cycles, radio, RequestOrders)
Tier 6: Background      (tactic-weight recalc, idle behavior)
```

### Recommended mapping: WRAP-beside, not replace

**The 3 existing tac-order slots (`GENERAL/PLAYER/ALARM`) are NOT replaced.** They remain the
runtime contract for the rest of the engine (mech movement, GUI, network). The new task queue sits
BESIDE them as a brain-side decision queue that FEEDS into those slots.

```
  runBrain() gate (warrior.cpp:5023)
       │
       ▼
  [NEW] BrainTaskQueue::drain(warrior, scenarioTime)   ← inserted here, default-OFF
       │                                                  (MC2_BRAIN_TASKQ gate)
       │   pops highest-priority pending task for this warrior
       │   task executor writes result into existing tacOrder[GENERAL/PLAYER/ALARM]
       │                                                  ↑
       ▼                                                  │ unchanged runtime contract
  [EXISTING] ABL brain->execute()  (Legacy/Hybrid mode)  │
       │                                                  │
       └──────────────────────────────────────────────────┘
```

The task queue **drains one task per `runBrain()` invocation** (same 2.25 s cadence), then falls
through to the ABL path. In Legacy mode (no Brain record) the queue is empty and costs nothing but
one branch.

### Tier → slot mapping

| GDD Tier | Priority | Feeds engine slot | Notes |
|---|---|---|---|
| 0 Logistics | Highest | `ORDERSTATE_PLAYER` | Time-critical; overrides general brain decision |
| 1 Alarm | | `ORDERSTATE_ALARM` | Maps directly to the existing ALARM slot |
| 2 Combat | | `ORDERSTATE_GENERAL` | Active engagement; main ABL-replacement path |
| 3 Tactical | | `ORDERSTATE_GENERAL` | OPORD slot execution; overrides lower tiers |
| 4 Movement | | `ORDERSTATE_GENERAL` | Waypoint traversal |
| 5 Sensor/Comms | | (side effect) | RequestOrders upchain, radio; no slot write |
| 6 Background | Lowest | (side effect) | Tactic-weight recalc; no slot write per-drain |

Tiers 0–4 produce a `TacticalOrder` and write it into the appropriate existing slot.
Tiers 5–6 produce side effects (fire radio call, update weight table) with no slot write; they
execute only when no higher-priority task is pending.

**Shape: WRAP-beside.** The queue wraps the brain-cadence gate; slot writes feed the existing
tac-order contract. No new slot types. No new engine primitive.

---

## §3 — Insertion point

**Exactly:** insert `BrainTaskQueue::drain()` at `warrior.cpp:5023`, inside the `brainUpdate <=
scenarioTime` branch, BEFORE `runBrain()` is called.

```
// warrior.cpp:5023 region — new shape with queue gate:
if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
    if (MC2_BRAIN_TASKQ_enabled && brainTaskQueue != nullptr) {
        brainTaskQueue->drain(this, scenarioTime);   // NEW: drain one task
    }
    runBrain();                                       // EXISTING: ABL legacy path
    brainUpdate += BrainUpdateFrequency;
}
```

### Cadence coexistence

The stagger (`brainUpdate` phase offset from creation order) is **preserved unchanged**. The
`BrainTaskQueue::drain()` fires on the SAME cadence as the ABL brain — every 2.25 s per warrior,
staggered by creation order. There is no new timer. The queue does not change when warriors fire.

**Do not replace the stagger.** The creation-order stagger is the cheapest possible load
distribution and has no ordering side effects. The new queue rides it for free.

### Hybrid mode arbitration

When `MC2_BRAIN_TASKQ=1` AND the warrior has a Brain record (Enhanced/Hybrid mode):

1. `drain()` pops the highest-priority task and writes its tac-order into the appropriate slot.
2. `runBrain()` then runs the ABL script.
3. ALARM slot: ABL has always been allowed to overwrite GENERAL with `setGeneralTacOrder()`. The
   Brain queue writes GENERAL first; if ABL also writes GENERAL, ABL wins (existing slot-overwrite
   semantics preserved — do NOT add special-case bypass of ABL writes).

In **Legacy mode** (no Brain record, `MC2_BRAIN_TASKQ=0` or no `Brain*` pointer): `drain()` is
never called. Stock behavior is byte-identical.

---

## §4 — Determinism contract (the crux)

### The ordering rule

Every task in the queue has a sort key:

```
(tier: u8, insert_seq: u32)
```

- `tier`: 0–6, lower = higher priority (drain highest first).
- `insert_seq`: a **per-warrior monotonic counter** incremented at task insertion. It is NOT a
  global clock, NOT a pointer, NOT an address. It is a simple `uint32_t` stored in the `Brain`
  struct alongside the warrior's task queue.

**Drain rule:** pop the task with the lowest `(tier, insert_seq)` pair. Ties on `tier` break
by `insert_seq` (FIFO within tier). This is a **stable, deterministic, address-independent** order.

### Why this is sufficient

The existing tac-order determinism works because both peers write the **same TacticalOrder** to the
**same named slot**. The Brain task queue's drain produces the same result on both peers as long as:

1. Both peers have the same `Brain` record (loaded from the same `mission_ai.fit`).
2. Both peers insert tasks in the same order (same world-state events trigger task insertion; task
   insertion is driven by OPORD-slot completion and event intake, NOT by frame timing).
3. The sort key uses only `(tier, insert_seq)` — no pointer, no wall-clock, no random element.

**Tac-order slot write is the sync boundary.** The queue drains to a `TacticalOrder` value that is
byte-identical on both peers. That `TacticalOrder` is then sent via the existing network pack path
(`pointLocalMoverId`/`groupFlags`/`data[2]`). No new network message type is needed.

### Save/load determinism

The `insert_seq` counter and the pending task queue must be saved and restored. On save, the queue
is serialized as an ordered array of `(tier, insert_seq, task_payload)` tuples. On load, it is
restored in the same order. The sort invariant is trivially preserved (seq numbers are already
monotone). After load, `drain()` produces the same task order as before save.

---

## §5 — Save/load design

### What must persist

New fields added to `MechWarriorData` (or a companion `BrainData` sub-struct):

```cpp
struct BrainTaskEntry {
    uint8_t  tier;           // 0–6
    uint32_t insertSeq;      // per-warrior insert counter at time of push
    uint8_t  taskType;       // enum: OPORD_START, REQUEST_ORDERS, RADIO_CALL, etc.
    uint32_t payload[4];     // 16 bytes opaque; enough for a TacticalOrder reference
};

struct BrainQueueState {
    uint32_t       insertSeqCounter;    // current counter value
    uint8_t        numPending;          // 0–MAX_BRAIN_TASKS (propose: 8 per warrior)
    BrainTaskEntry pending[8];          // serialized in (tier, insertSeq) sorted order
};
```

`BrainQueueState` is saved alongside `MechWarriorData` in the existing `PacketFile` per-warrior
packet. It is a **new packet type** (e.g. packet tag `WARRIOR_BRAIN_QUEUE`) appended after the
existing per-warrior packet — not inserted into `MechWarriorData` struct layout (avoids breaking
the existing POD binary layout).

### Version bump + migration

`saveload.cpp` uses a `versionStamp` string. The bump:

```
old: "MC2SaveV12"
new: "MC2SaveV13BrainQ"
```

Migration: if loading `MC2SaveV12`, the brain queue packet is absent. Load path: if packet tag
`WARRIOR_BRAIN_QUEUE` is not found for a warrior, construct an empty `BrainQueueState`
(`numPending=0`, `insertSeqCounter=0`). This is valid — an empty queue means no brain tasks are
pending, which is the correct state for any save made before this feature existed.

**Old saves load cleanly.** New saves do not load in old exe (versionStamp mismatch → existing
"incompatible save" dialog — already the behavior for version changes).

### Relink implication

`BrainQueueState` is a new struct. Adding it to `warrior.h` or a new `brain_queue.h` header is a
class-layout change if added as a member of `MechWarrior`. Per build rules: **full relink required**.
Mitigation: keep `BrainQueueState` in a separately heap-allocated `Brain*` struct (the same
`Brain*` pointer that will be added for BRAIN-RUNTIME-1). Then `MechWarrior` gains one `Brain*`
pointer member — one pointer = full relink, but only once for the arc, amortized across all
Brain slices.

---

## §6 — Coexistence / migration modes

| Mode | Brain record in `mission_ai.fit`? | `MC2_BRAIN_TASKQ` | Behavior |
|---|---|---|---|
| **Legacy** | No | OFF (default) | `drain()` never called; `brain` ptr = null; `runBrain()` executes ABL only. Stock missions byte-identical. |
| **Legacy + queue inert** | No | ON | `drain()` called; queue is empty; returns immediately; `runBrain()` executes ABL. Behavior identical to Legacy, minor branch cost. |
| **Hybrid** | Yes | ON | `drain()` fires; task result written to appropriate slot; then `runBrain()` executes ABL. ABL may overwrite GENERAL slot (allowed). |
| **Enhanced** (future) | Yes + `ABL_MODE=Enhanced` | ON | `drain()` owns all slots; ABL is bypassed (separate gate in BRAIN-RUNTIME-1, not this slice). |

**Gate:** `MC2_BRAIN_TASKQ` — default OFF. The gate is checked once per `runBrain()` invocation.
When OFF, zero overhead.

**Stock tier1 invariant:** with `MC2_BRAIN_TASKQ` unset, `run_smoke.py --tier tier1 --duration 30`
must produce results byte-identical to the pre-scheduler baseline. This is enforceable because no
`drain()` call is made when the gate is off.

---

## §7 — Minimal first increment: TASK-SCHEDULER-CORE-1 build slice

### Scope (data structure + inert drain, default-OFF, NO new task types)

TASK-SCHEDULER-CORE-1 ships exactly:

1. **New header `code/brain_task_queue.h`** — defines `BrainTaskEntry` and `BrainTaskQueue` class
   with: `push(tier, insertSeq, taskType, payload)`, `drain(MechWarrior*)` (pops one task; in this
   slice, no task type is implemented so drain pops and discards), `isEmpty()`, `numPending()`,
   `reset()`. No tac-order slot write in this slice (drain is a stub).

2. **`MechWarrior` gets a `BrainTaskQueue* brainTaskQueue = nullptr` member** — 8-byte pointer,
   allocated only when `MC2_BRAIN_TASKQ=1` AND a Brain record exists (null check at drain site).
   Full relink required (new member in warrior.h). This is the ONE relink for the arc.

3. **`warrior.cpp:5023` insertion** — the `drain()` call inside the `brainUpdate <=` branch, behind
   the null pointer check. ABL `runBrain()` continues unchanged.

4. **Save/load stub** — `WARRIOR_BRAIN_QUEUE` packet: if `MC2_BRAIN_TASKQ=0` or pointer is null,
   write zero-length packet. On load, if packet is absent or zero-length, initialize empty queue.
   `versionStamp` bump to `MC2SaveV13BrainQ`.

5. **Determinism self-check** — optional `MC2_BRAIN_TASKQ_DETCHECK=1` gate: on drain, assert
   `(tier < 7)` and `(insertSeq >= lastDrainedSeq)`. Fires in debug builds only.

6. **Gate** — `MC2_BRAIN_TASKQ` (default OFF). Registered in `docs/tier1_env_vars.md`.

### What is explicitly NOT in this slice

- No `Brain` struct (BRAIN-RUNTIME-1).
- No task type implementations (no OPORD_START, no REQUEST_ORDERS — those are BRAIN-RUNTIME-1).
- No actual tac-order slot writes from the queue.
- No new tac-order codes.
- No BrainSpecial dispatch.
- No FIT loading changes.
- No editor panel.

The queue drains but discards tasks; it is provably inert.

### Acceptance criteria

| # | Criterion |
|---|---|
| A1 | `run_smoke.py --tier tier1 --duration 30` with `MC2_BRAIN_TASKQ` unset exits 0; results byte-identical to pre-queue baseline |
| A2 | Same smoke with `MC2_BRAIN_TASKQ=1` exits 0 (no crash, no behavioral regression — queue is empty for all stock warriors) |
| A3 | `MC2_BRAIN_TASKQ=1` + `MC2_BRAIN_TASKQ_DETCHECK=1`: no assertion fires across tier1 |
| A4 | Save a game with `MC2_BRAIN_TASKQ=1`; reload with `MC2_BRAIN_TASKQ=1` — mission resumes correctly (queue state restored as empty) |
| A5 | Old save (`MC2SaveV12`) loaded with new exe: "incompatible save" dialog appears (expected — versionStamp change) |
| A6 | Determinism self-check: two independent `runBrain()` calls on same warrior with same task insertion sequence produce same drain order (`insert_seq` monotone; no address-dependent tiebreak ever reached) |

---

## §8 — Risks and open items

### Top risks

1. **Full relink on pointer add** (severity: medium). Adding `BrainTaskQueue*` to `MechWarrior` is
   a class-layout change. Per build rules, this requires deleting the `.obj` and `mc2.exe` and doing
   a full relink (~4–5 min). This is unavoidable and intentional — amortize it here so BRAIN-RUNTIME-1
   doesn't need another one. Do not add any other `MechWarrior` members in this slice.

2. **Version stamp churn** (severity: low). Bumping `versionStamp` invalidates all existing saves.
   The save-load migration for empty brain queue is trivial, but users lose in-progress saves on the
   new exe. This is acceptable for the first scheduler slice (feature is gate-OFF and invisible). If
   the project policy is to avoid save breaks during development, hold the version bump until
   BRAIN-RUNTIME-1 ships actual queue content.

3. **Creation-order stagger vs. task insertion order** (severity: low). The `insert_seq` counter is
   per-warrior and independent of when the warrior was created. Tasks pushed to warrior A and warrior
   B both start their `insertSeq` at 0. This is correct — the determinism guarantee is per-warrior,
   not cross-warrior. Cross-warrior ordering is provided by the existing stagger (warriors fire at
   different `scenarioTime` points and cannot observe each other's queue state).

4. **`drain()` + `runBrain()` both write GENERAL slot** (severity: medium, deferred to BRAIN-RUNTIME-1).
   In Hybrid mode, both can write `tacOrder[ORDERSTATE_GENERAL]`. The last write wins (slot overwrite
   semantics). In this slice, `drain()` is a stub (no write), so no conflict. The arbitration rule
   (Brain queue owns GENERAL; ABL may override in Hybrid; Enhanced mode bypasses ABL entirely) must
   be specified in BRAIN-RUNTIME-1's design before that slice starts. Do NOT implement the Enhanced
   bypass in this slice.

5. **Per-warrior per-`runBrain()` queue cost** (severity: low). `drain()` on an empty queue is:
   one null-pointer check + one `numPending == 0` check + return. ~3 instructions. At 2.25 s cadence
   with 30 staggered warriors, the call rate is ~13/s. Cost is negligible.

6. **`MAX_BRAIN_TASKS` per warrior = 8** (proposed). The task queue holds max 8 pending tasks per
   warrior. At 3 OPORD slots + 1 RequestOrders + a few sensor/background tasks, 8 is generous. If
   exceeded, push returns false (no crash); the pushed task is dropped and an MC2_BRAIN_TASKQ_TRACE
   log line is emitted. This is a soft failure, matching the project's existing soft-fail philosophy.

### Open items before BRAIN-RUNTIME-1

- **ABL arbitration rule in Hybrid mode**: specify which slot(s) the Brain queue "owns" vs. which
  ABL is still allowed to overwrite. Leaving it at "last write wins" (slot overwrite) is safe but
  may produce unexpected behavior if both try to move the same unit in conflicting directions. Needs
  a decision before BRAIN-RUNTIME-1 writes to slots.
- **`insert_seq` rollover**: `uint32_t` at 2.25 s cadence with 8 tasks/drain → ~4 billion / (8/2.25)
  = ~1 billion seconds of game-time before rollover. Not a practical concern; document and ignore.
- **Static data save packet ordering**: `StaticMechWarriorData` is written BEFORE per-warrior packets
  (`warrior.cpp:8677`). The `WARRIOR_BRAIN_QUEUE` per-warrior packet must be written AFTER the
  existing per-warrior packet, not before the static block. Verify packet ordering in
  `copyToData`/save() before implementing.

---

## §9 — VERDICT

**GO.**

The scheduler design is clean, safe, and fully grounded in the engine:

- The insertion point is unambiguous (`warrior.cpp:5023` inside the `brainUpdate <=` branch).
- The existing stagger is preserved intact; the queue rides the same 2.25 s cadence for free.
- The determinism contract is simple and address-independent: `(tier, insert_seq)` sort, per-warrior
  monotone counter, no global ordering across warriors needed.
- The save/load shape is minimal: a new per-warrior packet appended after existing data; absent
  packet = empty queue (correct migration for old saves).
- The coexistence story is solid: gate-OFF = zero overhead, zero behavioral change, stock missions
  byte-identical.
- The minimal first slice is provably inert (drain discards, no slot writes, no task types).
- The one non-negotiable cost (full relink for the `BrainTaskQueue*` pointer member) is acceptable
  and amortized for the whole arc.

**Remaining design risk before BRAIN-RUNTIME-1:** the Hybrid ABL arbitration rule (who owns
`ORDERSTATE_GENERAL` when both queue and ABL want to write it). This is not blocking for
TASK-SCHEDULER-CORE-1 (the slice is inert), but must be resolved before BRAIN-RUNTIME-1 starts
writing to slots.

The flagged arc determinism risk (§5 of the arc doc) is **resolved**: the `(tier, insert_seq)`
ordering rule is byte-deterministic across peers and across save/load. No floating-point, no
pointer, no address-dependent tiebreak.
