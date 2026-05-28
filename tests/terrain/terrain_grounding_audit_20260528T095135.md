# Terrain Grounding Audit — empirical run

- Script: `scripts/terrain_grounding_audit.py` (TERRAIN-GROUNDING-AUDIT-1)
- Run at: 2026-05-28T09:51:35
- Missions dir: `A:\Games\Carver5-feasibility\data\missions`
- Missions: mc2_01, mc2_03, mc2_10, mc2_17, mc2_24
- Sub-cell probe positions: [(0.25, 0.25), (0.5, 0.25), (0.75, 0.25), (0.25, 0.5), (0.5, 0.5), (0.75, 0.5), (0.25, 0.75), (0.5, 0.75), (0.75, 0.75)]

Measures the height delta between the bilinear-resampled render heightfield (what TERRAIN-NORMALS-FROM-HEIGHT-1 / TERRAIN-RESAMPLE-1 sample) and the triangle-linear interpolation the existing terrain mesh actually renders. Drift = how much the ground would visually move under a future TERRAIN-DISPLACE-VISUAL-1 with strength=1. Gameplay height (Terrain::getTerrainElevation) is unaffected — units/buildings would continue to read CPU values; this is the float/sink budget if the visual surface starts moving.

Read-only audit; reads only mission `.pak` files, makes no engine, gameplay, or render mutation.

Diagonals are the two possible mesh tessellations of each quad (top-left↔bottom-right vs top-right↔bottom-left). The renderer's actual choice depends on tile vertex order; the 'worst' column reports max(both) per probe — a conservative upper bound.

### mc2_01

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_01.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 100×100; 9801 cells × 9 sub-positions = 88209 drift samples

**Drift |bilinear − triangle-linear| in world units:**

| diagonal | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| tl→br | 88209 | 0.0000 | 0.0000 | 2.0882 | 11.8922 | 80.1667 | 0.7591 |
| tr→bl | 88209 | 0.0000 | 0.0000 | 2.0882 | 11.8922 | 80.1667 | 0.7591 |
| worst | 88209 | 0.0000 | 0.0000 | 2.4363 | 13.9216 | 80.1667 | 0.9109 |

⚠ worst-case drift 80.17 wu exceeds 5.0 wu threshold — near-unit displacement fade strongly recommended for this mission.

### mc2_03

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_03.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 100×100; 9801 cells × 9 sub-positions = 88209 drift samples

**Drift |bilinear − triangle-linear| in world units:**

| diagonal | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| tl→br | 88209 | 0.0000 | 0.8088 | 3.2353 | 8.8971 | 41.6059 | 1.3435 |
| tr→bl | 88209 | 0.0000 | 0.8088 | 3.2353 | 8.8971 | 41.6059 | 1.3435 |
| worst | 88209 | 0.0000 | 1.0784 | 4.0441 | 9.8284 | 41.6059 | 1.6122 |

⚠ worst-case drift 41.61 wu exceeds 5.0 wu threshold — near-unit displacement fade strongly recommended for this mission.

### mc2_10

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_10.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120; 14161 cells × 9 sub-positions = 127449 drift samples

**Drift |bilinear − triangle-linear| in world units:**

| diagonal | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| tl→br | 127449 | 0.0000 | 0.7591 | 3.7955 | 11.9560 | 59.9696 | 1.5194 |
| tr→bl | 127449 | 0.0000 | 0.7591 | 3.7955 | 11.9560 | 59.9696 | 1.5194 |
| worst | 127449 | 0.0000 | 1.1387 | 4.5547 | 13.6640 | 59.9696 | 1.8233 |

⚠ worst-case drift 59.97 wu exceeds 5.0 wu threshold — near-unit displacement fade strongly recommended for this mission.

### mc2_17

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_17.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120; 14161 cells × 9 sub-positions = 127449 drift samples

**Drift |bilinear − triangle-linear| in world units:**

| diagonal | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| tl→br | 127449 | 0.0000 | 0.3324 | 1.9947 | 5.9840 | 41.3073 | 0.7580 |
| tr→bl | 127449 | 0.0000 | 0.3324 | 1.9947 | 5.9840 | 41.3073 | 0.7580 |
| worst | 127449 | 0.0000 | 0.4987 | 2.4934 | 6.4827 | 41.3073 | 0.9096 |

⚠ worst-case drift 41.31 wu exceeds 5.0 wu threshold — near-unit displacement fade strongly recommended for this mission.

### mc2_24

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_24.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120; 14161 cells × 9 sub-positions = 127449 drift samples

**Drift |bilinear − triangle-linear| in world units:**

| diagonal | n | min | p50 | p90 | p99 | max | mean |
|---|---|---|---|---|---|---|---|
| tl→br | 127449 | 0.0000 | 0.0000 | 2.9412 | 13.2353 | 82.1478 | 1.1245 |
| tr→bl | 127449 | 0.0000 | 0.0000 | 2.9412 | 13.2353 | 82.1478 | 1.1245 |
| worst | 127449 | 0.0000 | 0.0000 | 3.3088 | 15.4412 | 82.1478 | 1.3494 |

⚠ worst-case drift 82.15 wu exceeds 5.0 wu threshold — near-unit displacement fade strongly recommended for this mission.

