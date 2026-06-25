# Terrain Beautify — Arc B (stock-mission beautification)

Non-destructive analysis + (future) beautification of **authored** stock missions.
Sibling to `tools/terrain_gen/` (Arc A: new-map procedural generation). Both share the
same feature-mask language (slope / curvature / cliff / flat / shoreline).

## Slices

| Slice | Status | What |
|---|---|---|
| **B1 MISSION-TERRAIN-ANALYZER-1** | ✅ DONE | read-only `.pak`+`.fit` → debug masks + report. No writes. |
| B4 TERRAIN-SLOPE-BIAS-VISUAL-1 | next | `gos_terrain.frag` slopeZ rock/cliff bias + veg suppress, default-OFF gate |
| B2 SIDECAR-FORMAT-1 | gated on B1 trust | baseHash + reversible delta package |
| B3 SMOOTH-PASS-1 | gated on B2 | protection-weighted height edit, applied via `pak_exporter.patch_pak` |

## B1 — mission_terrain_analyzer.py

```
python tools/terrain_beautify/mission_terrain_analyzer.py mc2_01 mc2_24 \
    [--missions-dir DIR] [--out tests/terrain/beautify]
```

Read-only. Emits per-mission `report.json` + `masks/*.png`:
height, slope, curvature, blockiness, water, shoreline, islands, pyramid_island_candidates,
cliff_candidates, flat_playable, roads_overlays, building_footprints, protected_hard, protected_soft.

### Verified format facts (the load-bearing recon — reuse for B2/B3)

- **PacketFile** (`mclib/packet.h`): magic `0xFEEDFACE`; entry = `type<<29 | offset`.
  Stock tier1 paks are all `RAW` (no LZD/ZLIB), but decoder handles both.
- **Heightfield** = packet 0, `side²·32` bytes, side ∈ {60,80,100,120}, 128 wu/vertex.
- **PostcompVertex** (`mclib/vertex.h`, 32 B): normal[0..11], **elevation@12 (float)**,
  **textureData@16** (hi16 = overlay handle, lo16 = base TXM), localRGBLight@20,
  terrainType@24, selected@28, water@29, shadow@30, highlighted@31.
- ⚠️ **`PostcompVertex.water` byte is NOT a clean bool** — it's a packed alpha
  ({0,1,64,65,128,129,192,193} on mc2_01). Treating nonzero as water gave a false
  92% / 78%. **Authoritative water = `elevation ≤ [Water].Elevation` from mission `.fit`**
  (`mclib/terrain.cpp:3740`). mc2_01 Elevation=350, mc2_24 Elevation=0.
- ⚠️ Per-vertex `terrainType` is dominated by a single code (2 on mc2_01) — not a usable
  water/material signal at the vertex level. Use elevation + overlay + slope instead.
- **Terrain objects packet** (`code/objmgr.cpp:1090` countTerrainObjects): `1 int count`
  then `count × 40 B` records = `objTypeNum(i), x,y,z(f), rotation(f), damage(i),
  teamId(i), parentId(i), 2 pad(i)`. mc2_01 → 1000 objs, mc2_24 → 2677.
- **World→grid** (`mclib/terrain.h:446`): `mapTopLeft = (-side·128/2, +side·128/2)`;
  `col=(x-tlx)/128`, `row=(tly-y)/128` (note Y flip).
- **Overlay enum** (`mclib/mapdata.h`): DIRT_ROAD=0 … RUNWAY=7 … OBRIDGE=14 …
  DAMAGED_BRIDGE=16. Detected via textureData hi16 ≠ {0, 0xFFFF}.

### B1 acceptance (all met)

- mc2_01 (100²) + mc2_24 (120²) load; objs 1000 / 2677 match registry counts.
- Waterline correct via `.fit` (not the broken vertex byte).
- Roads/buildings/bridges land on real authored features (road grid + city cluster visible
  in masks); pyramid detector flags 2 small isolated islands on mc2_01, 0 on land-map mc2_24.
- Read-only: only outputs under `--out` written. Deterministic (no RNG/timestamp in masks).

## Tunables (top of analyzer)
`CLIFF_SLOPE_DEG=35`, `FLAT_SLOPE_DEG=10`, `PYRAMID_MAX_AREA=600`, `BUILDING_FOOTPRINT_RADIUS=1`.
