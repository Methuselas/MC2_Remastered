# Perf budget + oracle harness

The shared vocabulary that turns smoke runs into **comparable** results. Seeded
from Baseline A (`82add3ca`, post-8z, captured off `mc2-win64-0.4c`). Every
structural lane (GlStateGuard, Tube, HZB, IBL/PBR, service-lane) runs its before
/after against this, instead of "looks okay in smoke."

## Pieces

| File | Role |
|---|---|
| `scripts/smoke_lib/oracleparse.py` | Render-oracle parser + `judge_oracles` (hard correctness asserts). Separate from `logparse.py` so adding an oracle can't destabilize the tier1 fault gate. |
| `scripts/oracle_report.py` | CLI: logs dir → per-mission oracle + `[PERF v1]` table, judged vs this budget. `--strict` exits 1 on regression. |
| `docs/perf-budget.json` | Per-mission floors/ceilings. |
| this doc | Vocabulary, thresholds, classification rules. |

## Usage

```bat
py -3 scripts/oracle_report.py --logs <artifact_or_log_dir> ^
    --budget docs/perf-budget.json --strict --md report.md --json report.json
```

Logs are `<mission>.out.log` / `<mission>.err.log` pairs (e.g. the Baseline A
archive at `.claude/baseline-A-logs/`). Run a fresh capture with the headless
pattern from `docs/baseline-A-post-8z.md`, then point the reporter at it.

## Budget vocabulary

| Metric | Source | Meaning | Budgeted? |
|---|---|---|---|
| `avg_fps` | `[PERF v1]` | whole-run mean | yes — floor |
| `p50_ms` | `[PERF v1]` | median frame (steady CPU+GPU) | reference |
| `p99_ms` | `[PERF v1]` | **steady-state tail** — the hitch metric that matters | yes — ceiling |
| `p1low_fps` | `[PERF v1]` | 1%-low fps (hitch severity) | yes — floor (noisy) |
| `peak_ms` | `[PERF v1]` | worst single frame | **NOT budgeted** — captures one-time load/first-frame spike (242–816 ms in Baseline A); not steady-state |

**Why p99_ms, not peak_ms:** `peak_ms` is dominated by the mission-load / first-frame
hitch and is not representative of render-loop cost. `p99_ms` is the steady tail.
A lane that regresses the render loop shows up in `p99_ms` and `p1low_fps`.

Floors/ceilings carry margin (headless idle fly-throughs are noisy):
`avg_fps_floor ≈ 0.85×`, `p99_ms_ceil ≈ 1.5×`, `p1low_fps_floor ≈ 0.7×` of baseline.

## Baseline A reference (the seed)

| Mission | avg_fps | p50_ms | p99_ms | p1low_fps | peak_ms |
|---|---|---|---|---|---|
| mc2_01 | 110.0 | 6.63 | 61.96 | 16.1 | 242.5 |
| mc2_03 | 128.1 | 6.96 | 28.00 | 35.7 | 270.4 |
| mc2_10 | 131.4 | 6.58 | 26.00 | 38.5 | 816.1 |
| mc2_17 | 132.2 | 6.88 | 20.37 | 49.1 | 251.0 |
| mc2_24 | 132.1 | 6.73 | 19.61 | 51.0 | 308.8 |

## Hard oracle asserts (must stay clean post-8z)

`judge_oracles` FAILs on any of: `FASTPATH_DROP≠0`, `terrain_arm_path≠gpu`,
TerrainLOD `parity MISMATCH≠0`, `slimVerts≠0` (legacy slimReduce resurrected),
`fullyArmed=0`, RENDER_SNAPSHOT `fallback`/`count`/`pkt`/`meta`-mismatch≠0,
TEX_RESOLVE `mismatches`/`oob`≠0, MECH_MATERIAL_GPU `mismatches≠0`,
OBJBATCHER `submit_legacy≠0`, GPU_CULL indirect `overflow≠0`.

### OBJBATCHER cpu_fallback classification (the "classify, not panic" rule)

`cpu_fallback` is non-zero in Baseline A on mc2_01 (1456) and mc2_24 (3941) — but
in both `cpu_fallback == late_register_recovery_skips` and `fallback_rate ≤ 0.0007`.
These are **late-register recovery skips** (HUD nodes like `compass` registering
after the batch window), not real geometry on the CPU path.

- `cpu_fallback == late_register_recovery_skips` → **WARN** (classified benign).
- `cpu_fallback  > late_register_recovery_skips` → **FAIL** (`real cpu_fallback` =
  the excess) — real instances fell back to CPU.

This is the structural answer to "OBJBATCHER cpu_fallback needs classification,
not panic": the harness encodes the classification so future runs auto-distinguish.

## Open residual

**Per-pass GPU timings (terrain solid / shadow / 3D / post / present) are NOT in
this budget** — headless smoke has no Tracy client. The only per-pass proxy
parsed is `GPU_CULL indirect_draw elapsed_us` (~11–14 µs CPU submit). True
per-pass GPU times must come from an interactive Tracy/RGP session and be folded
in as a second budget tier before they can gate timing-sensitive lanes. Until
then this budget gates **correctness oracles + whole-frame perf**, which is
enough to make GlStateGuard / Tube / HZB before-after comparable.
