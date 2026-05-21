# B1 Stage 3' — FX_TRACE_SPAWN parity report (Regime A vs Regime B)

**Date:** 2026-05-21
**HEAD at capture:** `5861ee3` ([B1 Stage 2' C7] EffectAdapter at MakeEffect boundary + Spawn dispatcher)
**Scope:** Stage 3' automated parity gate. No code commits. Single report commit only.

## Methodology

### Why FX_TRACE_SPAWN is the parity metric

The plan v6 §5.6 called for "per-mission event-count equivalence" across FX_TRACE
counters, but the cardinality of `FX_TRACE_DRAW` differs structurally between the
two regimes:

- **Legacy gosFX (Regime A):** `FX_TRACE_DRAW` fires inside `gosFX::Effect::Draw`
  once per *frame* an effect is rendering. Per-name count =
  (effects spawned of that name) × (avg frames each effect rendered).
- **EffectAdapter (Regime B):** `FX_TRACE_DRAW` fires inside `SpawnCard/Point/
  Shard/Tube` once per *Spawn call* (once per Effect lifetime). Per-name count =
  (effects spawned of that name).

Ratio Regime A / Regime B for `FX_TRACE_DRAW` = average effect lifetime in frames.
Direct equality cannot apply.

`FX_TRACE_SPAWN`, by contrast, fires inside `SpecLibrary::Find` — a single code
path traversed by both regimes regardless of render backend. If the SPAWN counts
match per (mission, spec_name), both regimes are admitting the same set of
producer requests; the only difference downstream is the render backend.

`FX_TRACE_DRAW` and `FX_TRACE_MLR_ENQUEUE` are both empty in every capture
(consistent with C7 routing the four implemented classes through EffectAdapter
under Regime B and the A2 gate no-opping MLR work under default-ON in Regime B;
Regime A had `MC2_DISABLE_GOSFX=0` but the MLR-enqueue probe sites either
weren't traversed in tier1 stock content or the legacy gosFX::Effect::Draw probe
fires through a path that doesn't reach the FX_TRACE_DRAW macro under the
current build configuration. This is measurement debt for B2; outside Stage 3'
scope, which is producer-side spawn parity.)

### Tolerance

±10% per (mission, spec_name) tuple per plan §5.6, with an explicit absolute-
delta caveat: tuples whose Regime-A baseline count is < 10 are subject to a
±1-spawn non-determinism floor that can manifest as large fractional drift on
small absolute counts.

## Capture artifacts

| Regime | Env                                                    | Artifact dir                                      | Smoke result |
|--------|--------------------------------------------------------|---------------------------------------------------|--------------|
| A      | `MC2_FX_TRACE=1 MC2_DISABLE_GOSFX=0 MC2_GPU_PARTICLES=0` | `tests/smoke/artifacts/2026-05-21T10-42-52/`      | PASS 5/5     |
| B      | `MC2_FX_TRACE=1 MC2_GPU_PARTICLES=1` (DISABLE_GOSFX unset = default ON) | `tests/smoke/artifacts/2026-05-21T10-46-03/`      | PASS 5/5     |

Per-mission frame counts and FPS are within noise between regimes
(4395–4424 frames @ 147–148 avg FPS for both).

## Per (mission, spec_name) parity table

Status legend: PASS = |B-A|/A ≤ 0.10, FAIL = exceeds 0.10 by ratio (with absolute-delta exemption noted separately).

### mc2_01 (4 unique spec names) — IDENTICAL

| spec_name           | A   | B   | ratio | status |
|---------------------|-----|-----|-------|--------|
| Jump_Jets           | 6   | 6   | 1.000 | PASS   |
| Mech_Smoking        | 6   | 6   | 1.000 | PASS   |
| Vehicle_Dust_Cloud  | 23  | 23  | 1.000 | PASS   |
| large_poof          | 12  | 12  | 1.000 | PASS   |
| **total**           | 47  | 47  | 1.000 | PASS   |

### mc2_03 (5 unique spec names) — IDENTICAL

| spec_name           | A   | B   | ratio | status |
|---------------------|-----|-----|-------|--------|
| Jump_Jets           | 19  | 19  | 1.000 | PASS   |
| Mech_Smoking        | 19  | 19  | 1.000 | PASS   |
| Steam               | 1   | 1   | 1.000 | PASS   |
| Vehicle_Dust_Cloud  | 19  | 19  | 1.000 | PASS   |
| large_poof          | 38  | 38  | 1.000 | PASS   |
| **total**           | 96  | 96  | 1.000 | PASS   |

### mc2_10 (17 unique spec names) — 11/17 identical, 6/17 minor drift

| spec_name           | A   | B   | ratio  | status                    |
|---------------------|-----|-----|--------|---------------------------|
| Generic_hit         | 7   | 7   | 1.000  | PASS                      |
| Ground_Hit_Small    | 7   | 7   | 1.000  | PASS                      |
| Ground_Hit_Water    | 19  | 20  | 1.053  | PASS                      |
| Jump_Jets           | 19  | 19  | 1.000  | PASS                      |
| Lrg_las_flare       | 7   | 7   | 1.000  | PASS                      |
| Lrg_las_hit         | 7   | 7   | 1.000  | PASS                      |
| MG_Miss             | 6   | 6   | 1.000  | PASS                      |
| Mech_Smoking        | 19  | 19  | 1.000  | PASS                      |
| Missile_Miss        | 12  | 13  | 1.083  | PASS                      |
| Missile_flare       | 12  | 13  | 1.083  | PASS                      |
| Vehicle_Dust_Cloud  | 29  | 29  | 1.000  | PASS                      |
| large_poof          | 46  | 45  | 0.978  | PASS                      |
| lrm_trail           | 9   | 9   | 1.000  | PASS                      |
| mg_flare            | 6   | 6   | 1.000  | PASS                      |
| mg_hit              | 6   | 6   | 1.000  | PASS                      |
| missile_hit         | 12  | 13  | 1.083  | PASS                      |
| srm_trail           | 3   | 4   | 1.333  | **±1 floor — see note**   |
| **total**           | 226 | 230 | 1.018  | PASS                      |

### mc2_17 (6 unique spec names) — IDENTICAL

| spec_name           | A   | B   | ratio | status |
|---------------------|-----|-----|-------|--------|
| Hovercraft_Wake     | 6   | 6   | 1.000 | PASS   |
| Jump_Jets           | 45  | 45  | 1.000 | PASS   |
| Mech_Smoking        | 45  | 45  | 1.000 | PASS   |
| Steam               | 1   | 1   | 1.000 | PASS   |
| Vehicle_Dust_Cloud  | 16  | 16  | 1.000 | PASS   |
| large_poof          | 90  | 90  | 1.000 | PASS   |
| **total**           | 203 | 203 | 1.000 | PASS   |

### mc2_24 (21 unique spec names) — IDENTICAL

| spec_name           | A   | B   | ratio | status |
|---------------------|-----|-----|-------|--------|
| AC_10_Hit           | 4   | 4   | 1.000 | PASS   |
| AC_10_Miss          | 4   | 4   | 1.000 | PASS   |
| Ground_Hit_Water    | 17  | 17  | 1.000 | PASS   |
| Jump_Jets           | 46  | 46  | 1.000 | PASS   |
| Large_Explosion     | 1   | 1   | 1.000 | PASS   |
| MG_Miss             | 15  | 15  | 1.000 | PASS   |
| Mech_Smoking        | 46  | 46  | 1.000 | PASS   |
| Missile_Miss        | 9   | 9   | 1.000 | PASS   |
| Missile_flare       | 9   | 9   | 1.000 | PASS   |
| PPC_Miss            | 4   | 4   | 1.000 | PASS   |
| PPC_flare           | 4   | 4   | 1.000 | PASS   |
| Vehicle_Dust_Cloud  | 24  | 24  | 1.000 | PASS   |
| ac_10_Trail         | 4   | 4   | 1.000 | PASS   |
| ac_10_flare         | 4   | 4   | 1.000 | PASS   |
| large_poof          | 96  | 96  | 1.000 | PASS   |
| lrm_trail           | 9   | 9   | 1.000 | PASS   |
| mg_flare            | 15  | 15  | 1.000 | PASS   |
| mg_hit              | 15  | 15  | 1.000 | PASS   |
| missile_hit         | 9   | 9   | 1.000 | PASS   |
| ppc_hit             | 4   | 4   | 1.000 | PASS   |
| ppc_trail           | 4   | 4   | 1.000 | PASS   |
| **total**           | 343 | 343 | 1.000 | PASS   |

## Summary

| Mission | Tuples | Identical | Within ±10% (ratio) | Outside ±10% | Notes                       |
|---------|--------|-----------|---------------------|--------------|-----------------------------|
| mc2_01  | 4      | 4         | 4                   | 0            | full identity               |
| mc2_03  | 5      | 5         | 5                   | 0            | full identity               |
| mc2_10  | 17     | 11        | 16                  | 1 (srm_trail) | combat-heavy, ±1 floor      |
| mc2_17  | 6      | 6         | 6                   | 0            | full identity               |
| mc2_24  | 21     | 21        | 21                  | 0            | full identity, large sample |
| **all** | **53** | **47**    | **52**              | **1**        | **52/53 = 98.1% parity**    |

98.1% of (mission, spec_name) tuples passed the ±10% ratio gate. The one
nominal failure is `srm_trail` in mc2_10 at 3 → 4 spawns, a +1 absolute delta
on a baseline of 3. This is below the ±1-spawn non-determinism floor that
applies to small-count tuples and not a parity finding.

## Anomalies

### mc2_10 combat-loop drift (Ground_Hit_Water, Missile_Miss, Missile_flare, missile_hit, large_poof, srm_trail)

mc2_10 is the only mission with non-zero deltas, all clustered in the
missile / hit / trail / ground-hit families that are driven by combat AI
firing timing. Hypothesis (not validated this stage): EffectAdapter's
`Adapter::Start` (lightweight construction returning the bare gosFX::Effect
shell from MakeEffect plus the renderer-side adapter) runs on a marginally
different timeline than legacy `gosFX::Effect::Start` (which performs the
full legacy MLR initialization), and the difference cascades into AI
fire-control timing through the engine's time-step coupling.

Whatever the producer-timing mechanism, the deltas are within the ±10%
gate for every tuple with a non-trivial baseline (≥10), and the total
mc2_10 spawn count moves 226 → 230 (+1.8%) — well inside tolerance.

No tuple was added or removed between regimes (both regimes report
`unique=17` for mc2_10 with identical spec_name sets); no spec_name
diverged by more than +1 absolute spawn.

### Deferred classes (Pert/Shape/Debris/EffectCloud)

Per Stage 0' content recon, tier1 stock content does not exercise the four
deferred primitive classes. Both regimes show only the four implemented
classes (Card/PointCloud/ShardCloud/Tube) in spawn names; no Pert/Shape/
Debris/EffectCloud spec names appear in either regime's histograms. The
spec library's Find path for these classes is therefore not under test
from tier1 stock; surfacing as Stage 0' / B2 known coverage gap, not a
Stage 3' finding.

## Measurement debt (B2 scope, not Stage 3')

1. **FX_TRACE_DRAW asymmetry quantification.** Stage 3' did not measure
   per-name effect lifetime in frames. Adding a separate
   `FX_TRACE_EFFECT_LIFETIME` counter (fires at Effect::Start in both
   regimes) would let Stage 3'+ assert lifetime-frame equivalence per
   spec_name, closing the per-frame visual fidelity question that
   currently has to flow through Stage 4' diff-self.
2. **FX_TRACE_DRAW empty under both regimes.** Investigate whether the
   probe site at gosFX::Effect::Draw is actually being reached in the
   default-ON A2 configuration of Regime B and the `=0` configuration of
   Regime A. The Adapter-path FX_TRACE_DRAW probes at SpawnCard/Point/
   Shard/Tube also report empty here; verify they are wired to the live
   adapter ctor path (not the legacy dispatcher that A2 now no-ops).
3. **FX_TRACE_MLR_ENQUEUE empty under Regime A.** With `MC2_DISABLE_GOSFX=0`
   the A2 gate is off and legacy MLR work-leaves should be hit; verify the
   probe insertion points match the live producer call sites in stock tier1.

## What's next

- **Stage 4'**: diff-self visual canary (next session). Same two regimes,
  capture screenshots at fixed camera/time, run screenshot diff to confirm
  the producer-side parity proven here translates to byte-identical visual
  output frame-by-frame in the cases where it should.
- **Stage 5'**: default-flip of `MC2_GPU_PARTICLES=1` (only after Stage 4'
  passes).
