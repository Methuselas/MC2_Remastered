# Track B — Parity Verification & Acceptance Envelope

**Date:** 2026-05-06  
**Tasks covered:** 1–8  
**Env:** `MC2_STATIC_PROP_MISSION_LOAD_REG=1 MC2_STATIC_PROP_LATE_SPAWN_REG=1`  
**Run:** 30s per mission, tier1 (5/5 PASS)  

## Cascade-safety gates (Task 8)

| Gate | Requirement | mc2_01 | mc2_03 | mc2_10 | mc2_17 | mc2_24 | Status |
|------|------------|--------|--------|--------|--------|--------|--------|
| DESTROY parity | Δ destroys = +0 | +0 | +0 | +0 | +0 | +0 | ✅ PASS |
| submit_legacy=0 | submit_legacy=0 | 0 | 0 | 0 | 0 | 0 | ✅ PASS |
| First-frame fix | STATIC_FIRST_FRAME skip_count=0 | 0 | 0 | 0 | 0 | 0 | ✅ PASS |
| HC-3 signal | type_unknown_at_late_spawn=0 | 0 | 0 | 0 | 0 | 0 | ✅ PASS |

## Pool sizing gate (Task 7)

All pools below 80% threshold. Worst: mc2_17 face pool 44% (89606/200000).  
See `docs/superpowers/specs/2026-05-06-track-b-baseline-measurements.md`.

## Key counters at run end

| Mission | registered | skipped | skip_count | late_spawn_unknown |
|---------|-----------|---------|-----------|-------------------|
| mc2_01  | 997 / 1000 | 3 | 0 | 0 |
| mc2_03  | (from 60s run) | — | 0 | 0 |
| mc2_10  | — | — | 0 | 0 |
| mc2_17  | — | — | 0 | 0 |
| mc2_24  | — | — | 0 | 0 |

## Acceptance conditions for Task 9 (flip to default-on)

Track B can flip `MC2_STATIC_PROP_MISSION_LOAD_REG` and `MC2_STATIC_PROP_LATE_SPAWN_REG` to default-on when:

1. ✅ **DESTROY parity**: `Δ destroys = +0` across all tier1 missions  
2. ✅ **submit_legacy=0**: No legacy fallback path activated  
3. ✅ **skip_count=0**: `[STATIC_FIRST_FRAME v1]` counter zero (first-frame fix working)  
4. ✅ **HC-3 gate**: `type_unknown_at_late_spawn count=0` across tier1  
5. **Soak ≥7 days**: Under joint A1+A2+B-modern + alpha-test-prep-modern config (Q15)

Conditions 1–4 are MET as of 2026-05-06. Condition 5 (soak) begins when Task 9 fires.

## Observations (not blockers)

- `submit_trees=0` on most missions is **correct** — trees go through the registry fast-path (`markVisible`), which correctly does not count in the dynamic-path `submit_trees` counter
- `cpu_fallback` growing on mc2_24 is pre-existing: mc2_24 has actors whose types are not in `s_typeIndex` (registered as `late_register_recovery_skips`). This is unaffected by Track B
- The 3 `skipped` at mission-load on mc2_01 are actors that fail `isStaticEligible()` — expected behavior, first-render fallback covers them
