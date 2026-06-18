# Phase 0–1 Test Asset Selection

## Chosen asset: **Flea** (mech)

### Rationale

Phase 0 finding: GLB probe support (`LoadFromFile`) exists **only for mechs**.
Therefore the test asset must be a mech.

The Flea was selected because:
1. **Appears in smoke mission mc2_03** (tier1) — `st CSVFile = "Flea"` present 6× in
   `data/missions/mc2_03.fit`
2. **Simple structure** — 2 LODs (Flea + FleaL1), 1 shadow mesh (FleaX), small ASE file
   (3540 lines vs Raven 3500+)
3. **All node types present** — 1 smoke node, 1 jump jet node, 5 weapon nodes, 2 foot nodes
   — exercises the full hardpoint extraction path
4. **ASE available** — `A:/Games/mc2-opengl-src/mc2srcdata/tgl/flea.ase` exists

### File paths

| File | Path |
|---|---|
| Primary ASE | `A:/Games/mc2-opengl-src/mc2srcdata/tgl/flea.ase` |
| LOD1 ASE | `A:/Games/mc2-opengl-src/mc2srcdata/tgl/fleal1.ase` |
| Shadow ASE | `A:/Games/mc2-opengl-src/mc2srcdata/tgl/fleax.ase` |
| INI | `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/flea.ini` |
| Baseline JSON (generated) | `docs/baseline-flea.json` |
| Output GLB | `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/Flea.glb` |
| Output sidecar | `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/Flea.mcasset.json` |

### INI summary

- Section `[TGLData]`: 2 LODs (Flea at 0, FleaL1 at 1500), shadow FleaX
- Section `[Nodes]`: 1 smoke, 1 jumpjet, 5 weapon, 2 foot
- No `[AnimationNode]` section → not an animated prop → safe for static roundtrip test
- No existing `[Import] Source=` → fresh baseline (no prior GLB override)

### Smoke mission confirmation

`grep "CSVFile" data/missions/mc2_03.fit` returns `"Flea"` 6 times.
mc2_03 is one of the 5 tier1 smoke missions (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`).

### Note on simpler alternative

For a pure geometry test (no hardpoints), `ammodump` (building, 1457-line ASE, 1 LOD,
1 texture) is the simplest non-mech asset. It was not chosen because buildings have
no GLB probe path today. The Flea is the right choice for validating the full pipeline
including the `[Import]` hook.
