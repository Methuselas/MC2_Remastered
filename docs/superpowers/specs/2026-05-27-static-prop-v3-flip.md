# Static-Prop V3 Flip — Default-ON Plan

**Date:** 2026-05-27
**Slice kind:** default-flip (behavior-changing under default config)
**Gate:** `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` becomes the kill-switch (was the opt-in)
**Prerequisite HEAD:** `153dcc2b` (soak-state doc committed)
**Prerequisite gates:** see §Prerequisites below

---

## Scope

- Flip `s_snapshotBuildEnabled` to default-ON (unset → snapshot dispatch active)
- Kill-switch: `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` disables and restores live-primary dispatch
- Live builder continues to run every flush (compare authority; fallback path)
- Compare continues to run every flush (mismatch counters in ok gate)
- No removal of live builder
- No snap-cull coupling change (snap-cull stays opt-in; collision guard unchanged)
- No dispatch loop refactor

---

## Architecture Post-Flip

```
flush():
  1. Build live arrays (v6Packets, v6Meta)           ← unchanged, always runs
  2. [IF !s_snapshotBuildEnabled] dispatch live, done
  3. Build snapshot arrays (s_snapV6Packets, s_snapV6Meta)
  4. Compare field-by-field → spBuildPacketMismatch / spBuildMetaMismatch
  5. Dispatch:
       clean compare  → snapshot arrays  (s_spBuildFallback stays 0)
       mismatch       → live arrays      (++s_spBuildFallback; ok=0 next frame)
```

Live and snapshot always built in lock-step. Comparison is never skipped.
Fallback is always available — flip is fully reversible via `=0`.

---

## Prerequisites

All must be confirmed before executing the flip.

### P1 — mc2_10 ok=1 re-validation (BLOCKING)

`7bdbd1fd` fixed the zombie-slot that was setting `sp_fail=1` from frame ~1706
in mc2_10. Must confirm ok=1 throughout before flip.

```powershell
$env:MC2_RENDER_SNAPSHOT_LOG="1"
py -3 scripts/run_smoke.py --missions mc2_10 --duration 30 --keep-logs
```

Pass criteria: no `sp_fail=1` in log; `ok=1` for all frames in mission (frame 1706–3326
range previously showed persistent failure).

### P2 — Visual identity (BLOCKING)

With `MC2_SNAPSHOT_STATIC_PROP_BUILD=1`, terrain + static props must be visually
identical to the live path. Run a side-by-side or sequential visual smoke:

```powershell
# Gate-off baseline
py -3 scripts/run_smoke.py --missions mc2_01,mc2_24 --duration 30 --keep-logs

# Gate-on (current opt-in behavior = post-flip default)
$env:MC2_SNAPSHOT_STATIC_PROP_BUILD="1"
py -3 scripts/run_smoke.py --missions mc2_01,mc2_24 --duration 30 --keep-logs
```

Pass criteria: no visible prop placement/pop/flash difference. Screenshots from
both runs agree.

---

## Code Changes

### T1 — Flip `s_snapshotBuildEnabled` to default-ON

**File:** `GameOS/gameos/gos_static_prop_batcher.cpp`

Replace the current strict opt-in lambda:

```cpp
// BEFORE
static const bool s_snapshotBuildEnabled = []() -> bool {
    const char* v = std::getenv("MC2_SNAPSHOT_STATIC_PROP_BUILD");
    return v && v[0] == '1';
}();
```

With default-ON semantics (matches `envFlagDefaultOn` in `gos_mech_batcher.cpp`):

```cpp
// AFTER
// Default ON. Kill-switch: MC2_SNAPSHOT_STATIC_PROP_BUILD=0.
// Any unset or non-"0" value leaves snapshot dispatch active.
static const bool s_snapshotBuildEnabled = []() -> bool {
    const char* v = std::getenv("MC2_SNAPSHOT_STATIC_PROP_BUILD");
    if (v == nullptr)               return true;   // unset → on
    if (v[0] == '0' && v[1] == '\0') return false;  // exactly "0" → off
    return true;                                    // any other value → on
}();
```

No other changes to batcher logic. Collision guard, compare loop, dispatch
ref-swap — all unchanged.

### T2 — Update `docs/tier1_env_vars.md`

Update `MC2_SNAPSHOT_STATIC_PROP_BUILD` entry:

- Was: "opt-in gate, default OFF; set =1 to enable v3 snapshot dispatch"
- Now: "default ON; set =0 to disable (kill-switch to live-primary dispatch)"
- Add kill-switch column to tier1 canary table
- `MC2_SNAP_CULL` entry unchanged (stays opt-in)

### T3 — Update `docs/static-prop-v3-soak-state.md`

Mark "Authority-Flip Acceptance Gates" table rows complete as each passes.
Add flip commit SHA to arc summary table.

---

## Smoke Gate for This Flip

Four runs required; all must be 5/5 PASS:

| Run | Env | Expected |
|-----|-----|----------|
| Default (no env vars) | — | Snapshot dispatch active; ok=1; spBuildFallback=0; mismatch counters=0 |
| Kill-switch | `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | Live dispatch; spBuild*=0; ok=1; identical to pre-flip default |
| Collision | `MC2_SNAP_CULL=1` | Collision guard fires; spBuildFallback>0; ok=1 (fallback not in gate) |
| Explicit opt-in | `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` | Identical to default run (sanity) |

Full tier1 (all 5 missions × 4 runs). Default run is the release gate.

---

## Commit Sequence

```
T1: feat(batcher): static-prop v3 default-ON flip (MC2_SNAPSHOT_STATIC_PROP_BUILD=0 kill-switch)
T2: docs(tier1_env_vars): update MC2_SNAPSHOT_STATIC_PROP_BUILD to default-ON
T3: test: tier1 default/kill-switch/collision/opt-in smoke PASS 5/5 (v3 flip)
T4: docs(soak-state): mark flip complete, update acceptance gates
```

T3 is a test-evidence commit (updates soak-state doc + any smoke artifact summary).
Single PR.

---

## What Does NOT Change

- Live builder: always runs, always compare authority
- `spBuildFallback` semantics: informational, not in ok gate
- `spBuildAttempted`: with default-ON this will be 1 every frame; keep in log
  (useful for confirming gate is active), consider removing from per-mission
  summary display if it becomes noise
- Shadow pass: untouched
- `s_snapCullEnabled`: stays strict opt-in (`=1` to enable)
- Snap-cull collision guard: unchanged
- Live-only dispatch path (kill-switch `=0`): must remain permanently available
  as the debug/regression path — do not remove in v3.1 or later without a
  separate review

---

## Rollback

Set `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` in the launch environment or
`run-with-log.bat`. Equivalent to pre-flip behavior. No code change needed.

---

## Not In This Slice

- Removal of live builder
- `spBuildFallback` promotion to ok gate
- v3.1 dispatch loop refactor
- snap-cull + snapshot joint behavior change
- Any shadow-pass or other render path changes
