# FIXB-MVP-CURRENCY-1 — fix + structural meta-fix

Root (git-pinned): `7f43ee37` (Jun 27, default-ON, gates `MC2_PROP_FIXB_MVP` +
`MC2_MVP_PUBLISH_EARLY`). It re-pointed **static props (cacti) and mechs** from the
live camera MVP (`gos_GetTerrainMVPMat4`) to the terrain compute snapshot
(`gos_terrain_indirect_getDispatchMvp16` → `g_dispatchMvp16`). Consumers read the
snapshot **without checking it was published this frame**. On any frame where the
snapshot is stale relative to the live camera (rotation, `turn<=1`, sub-loops,
armed-but-not-redispatched), objects transform with current positions but project
through last frame's camera matrix:

- large objects shimmer during rotation  → **BUG3**
- small props (cacti) cross a clip edge / fail w-clip and drop entirely → **BUG5**
- continuous wobble while zooming → **BUG1**

Pause/resume forces a consistent publish → all reappear. Same failure family the
terrain-indirect code already documents for water ("recede/flicker/vanish"); FixB
extended that risk from water to the whole object/mech family.

BUG1 + BUG3 + BUG5 are ONE root.

---

## The bug class (general statement)

> A draw path consumes a **snapshot** (a value captured in an earlier phase) without
> proving the snapshot belongs to **this frame / this camera**. When it doesn't,
> geometry is transformed by current data but projected by stale data → mismatch.

The snapshot already carries a stamp (`g_dispatchMvpFrameIdx`, set at publish,
gos_terrain_indirect.cpp:3332) but **no consumer reads it**. The fix makes currency
mandatory; the meta-fix makes skipping the currency check structurally impossible.

---

## FIX (correctness, gated default-ON, currency-safe)

### 1. Stamp the snapshot in a clock the draw phase can read
`ringFrameIdx` (terrain-ring clock) is not visible to object draw. Add an
**engine-frame** stamp alongside it at the publish site.

`gos_terrain_indirect.cpp` near :3332:
```cpp
extern unsigned long g_mc2FrameCounter;          // engine frame clock (mclib)
g_dispatchMvpFrameIdx       = ringFrameIdx;       // (existing, terrain-ring clock)
g_dispatchMvpEngineFrame    = g_mc2FrameCounter;  // NEW: engine-frame stamp
```
Add `static uint64_t g_dispatchMvpEngineFrame = 0;` + getter
`uint64_t gos_terrain_indirect_getDispatchMvpEngineFrame()`.

### 2. Single currency-gated accessor for OBJECT/MECH draw
New shared helper (header `gos_object_draw_mvp.h`, tiny):
```cpp
// Returns the snapshot ONLY if it was published THIS engine frame; otherwise the
// live camera MVP. Never returns a stale matrix. fixB killswitch preserved.
inline const float* gos_GetObjectDrawMVP(bool fixBEnabled) {
    if (fixBEnabled && gos_terrain_indirect::IsFrameSolidArmed()) {
        extern unsigned long g_mc2FrameCounter;
        if (gos_terrain_indirect_getDispatchMvpEngineFrame() == g_mc2FrameCounter) {
            if (const float* m = gos_terrain_indirect_getDispatchMvp16()) return m; // current
        }
#ifndef NDEBUG
        gos_object_mvp_note_stale();   // loud in debug/test, no-op in release
#endif
    }
    return gos_GetTerrainMVPMat4();    // live fallback — never stale
}
```

### 3. Re-point the two object consumers through it
- `gos_static_prop_batcher.cpp:86` `gos_GetObjectFixBMVPMat4()` → body becomes
  `return gos_GetObjectDrawMVP(s_propFixBMvpEnabled());`
- `gos_mech_batcher.cpp:75` `gos_GetMechFixBMVPMat4()` → likewise.

**Do NOT touch** the water / overlay / decal getters in gameos_graphics.cpp
(:3234,:3441,:9158,:9987,:10108,:10182). Those are same-phase as terrain by design
(the original, correct water fix). Currency for them is structurally guaranteed
because terrain draws in the same phase the snapshot was published.

### Behavior
- Snapshot current (common case, static camera): objects use it → z-fight-on-zoom
  fix from 7f43ee37 PRESERVED.
- Snapshot stale (rotation / skipped redispatch): objects use live MVP → no
  vanish / no flicker. Worst case = the original mild z-fight returns for that one
  stale frame, which is invisible vs the vanish it replaces.
- `MC2_PROP_FIXB_MVP=0` still hard-reverts to live MVP everywhere (unchanged).

---

## META-FIX (make the bug class structurally impossible)

Three layers, cheapest first:

### M1. One sanctioned accessor — no raw snapshot reads in object/mech draw
`gos_GetObjectDrawMVP()` is the ONLY way object/mech geometry may obtain the
dispatch MVP. Raw `gos_terrain_indirect_getDispatchMvp16()` is reserved for
terrain/water (same-phase). This removes the choice that let 7f43ee37 read it blind.

### M2. Loud on staleness (turn silent-flicker into a test failure)
`gos_object_mvp_note_stale()` increments a counter surfaced in the debug-state dump
and the smoke `diag-state` block. A stale object-MVP read during a normal play frame
becomes a **counted, asserted event** — caught by the gate, not by a user on NVIDIA
three days later. Optional `MC2_OBJ_MVP_STALE_FATAL=1` to abort in CI.

### M3. Enforced invariant script (project already uses `scripts/check-*.py`)
`scripts/check-object-mvp-currency.py`:
- FAIL if any TU **other than** terrain/water draw paths references
  `getDispatchMvp16(` directly (object/mech/prop draw must use the accessor).
- FAIL if `gos_GetObjectDrawMVP` is bypassed in the prop/mech batchers.
- Allowlist = the sanctioned terrain/water sites, checked by path.
Wire into the existing check suite so a future "just read the snapshot" regression
fails the build, not the player's screen.

### Why this is structural, not another patch
The original bug was possible because (a) a snapshot getter existed with no currency
contract, and (b) any consumer could call it. M1 removes (b) for the object family;
M2 makes a violation observable the instant it happens; M3 makes (a)-style blind
reads a build failure. The frame-stamp that already existed is finally *required*.

---

## Verification
1. A/B (no rebuild) on current 0.4c build: `MC2_PROP_FIXB_MVP=0` → flicker+wobble+
   cacti gone = root confirmed. (Decisive before building.)
2. After fix (default-ON, currency-safe): rotate at normal height → no flicker;
   cacti stay; zoom → no wobble AND building base z-fight still fixed.
3. `MC2_PROP_FIXB_MVP=0` still reverts cleanly.
4. tier1 5/5, +0 destroy delta, byte-identical stock.
5. M2 counter == 0 across a rotation+zoom soak; deliberately stale a frame in a
   harness → counter increments (proves the guard fires).

## Files
- `GameOS/gameos/gos_terrain_indirect.cpp` (:3332 publish + new getter)
- `GameOS/gameos/gos_object_draw_mvp.h` (new accessor + stale counter)
- `GameOS/gameos/gos_static_prop_batcher.cpp` (:86)
- `GameOS/gameos/gos_mech_batcher.cpp` (:75)
- `scripts/check-object-mvp-currency.py` (new invariant)

## Non-goals
- No change to water/overlay/decal MVP (correct as-is).
- No change to `MC2_MVP_PUBLISH_EARLY` semantics (it can stay on for terrain/water).
- No change to the snapshot publish timing — currency is enforced at consume, where
  the staleness is observable.
