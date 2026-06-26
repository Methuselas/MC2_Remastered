# TERRAIN-VISUAL-HEIGHT-SAMPLE-1 — desync consumers (mc2-water-depth-expert)

Read-only recon. Severity assumes the **corner-pinned bake**: divergence is ZERO at
cell corners, bounded by interior amplitude `A_int`, peaks at cell center.

## 1. WATER — NEGLIGIBLE. Do NOT touch.
Water never reads terrain Z. VS projects a FLAT plane: `wz = waveOurCos + waterElevation`
(`gos_terrain_water_fast.vert:206`); `waterElevation` flat (`terrain.cpp:161`, set `:3880`).
The depth-bias lockstep does NOT assume terrain Z == gameplay Z — it assumes water+terrain
share the baked terrain MVP (`terrain_depth_bias.h:63-80`): the fudge is a screen-Z epsilon
off terrain's OWN projected z, so when terrain displaces, water's relative bias rides WITH it.
Only subtle seam = shoreline (visible waterline vs flat plane vs gameplay `getWater()`
`terrain.cpp:4074`), mismatch < `A_int` (corners pinned at shore cell). De-risked via
`sampleWaterClass` (gameplay Z). **TRAP: do NOT "fix water to follow terrain Z" — it
REINTRODUCES the zoom z-fight Fix B killed. Keep the one non-zero fudge.**

## 2. DECALS — BLOCKER-if-ignored, but CHEAP. FIX IN THIS SLICE.
Craters (`crater.cpp:259-262`) + ring (`dynamic_decal_ring.cpp:97`) already route Z via
`TerrainRuntime::decalElevation()`. That fn flips to `sampleVisualHeight` under
`MC2_TERRAIN_RUNTIME_DECALS` — BUT `sampleVisualHeight` (`terrain_runtime.cpp:42-48`) STILL
returns gameplay `getTerrainElevation`. The seam is plumbed to visual but NOT yet FED visual.
**Action: (a) wire `sampleVisualHeight` to the 4x field, (b) enable the decal gate in
lockstep.** Else decals z-fight/float `A_int` on every displaced interior cell. Mine decals
ride the visual quad (`quad.cpp:455/824/903`) → auto-follow, SAFE.

## 3. OBJECT GROUNDING — VISIBLE-BUT-ACCEPTABLE. Defer.
Units/buildings stay gameplay Z (`groundElevation`, ~47 sites). A unit at cell-center
hovers/sinks up to `A_int`; zero at corners. Bounded → acceptable for small `A_int`, NOT a
blocker. Foot-contact is the eye's most sensitive desync — if `A_int` is tuned dramatic,
units read as floating. DEFERRED follow-up: visual-foot-snap = `sampleVisualHeight` for the
RENDER-transform Z ONLY (same pattern as decals), gameplay Z for everything else. Shadow
caster grounding (`gameos_graphics.cpp:2490/2554`) inherits the same gameplay Z → folds into
this follow-up. Ship displacement first, measure, then decide.

## 4. PICKING — VISIBLE-BUT-ACCEPTABLE. Defer.
`screenToTerrainApprox` (`camera.cpp:927` vs `getTerrainElevation` `:979`) + `raycastTerrain`
(full-res gameplay) converge to the gameplay surface; cursor on a displaced peak offsets up to
`A_int` (sub-tile, corner-pinned). RTS-zoom acceptable. Later fix = a visual-height raycast
variant, its own slice.

## 5. SHADOWS/AO — LOW (shader recon owns). 
Dynamic receiver IS the visual mesh (self-consistent). Baked colormap shadow
(`terrtxm2.cpp:609`) painted from gameplay Z at load → cosmetic. Only real desync = the
terrain shadow CASTER geometry staying coarse (see shader recon §3).

**Verdict:** only #2 (decals) is a must-fix in the displacement slice — and it's cheap (seam
already built, just flip+feed). #1 water = insulated, leave alone (and don't "fix" it). #3/#4
bounded by the corner-pin → defer. #5 = shader recon (shadow caster lockstep).
