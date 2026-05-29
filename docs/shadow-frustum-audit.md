# SHADOW-FRUSTUM-AUDIT-1

**Date:** 2026-05-29
**Scope:** Recon/diagnostic only. No shadow visual changes, no default flips, no CSM, no caster migration, no resolution change. One commit (env-gated diagnostic + this doc).
**Trigger:** With shadows working again (terrain objects now cast via the dynamic pass, fix `f04e3997`/`2764cb65`), distant trees/buildings still don't visibly affect the scene. User hypothesis: the dynamic map is fixed in world units and should be camera/viewport-fit.

## TL;DR — the hypothesis is already implemented, and it's degenerate

The dynamic sun-shadow ortho box **is already camera-frustum-fit, pow-2 snapped, and texel-snapped** (`gosPostProcess::buildDynamicLightMatrix`, `GameOS/gameos/gos_postprocess.cpp:1453`). It is **not** a fixed world-unit box. But measured in-game it **degenerates to full-map coverage every frame** because the MC2 RTS camera frustum projects to the entire map. So "fit the dynamic map to the frustum" (Strategy B) is already done and yields **no further benefit** — the frustum *is* the map.

**The real limiter is texel density**, not the fit. Measured: **5.568 world-units/texel**, uniform across a **22808×22808 WU** map at 4096². A tree (~2–5 WU) is ≈1 texel → no visible shadow; buildings cast coarse blocky shadows. Distance does **not** change texel size (coverage is uniform), so "distant" is a red herring — *all* small casters are under-resolved everywhere.

## 1. Dynamic shadow matrix construction (Task 1)

`gosPostProcess::buildDynamicLightMatrix(sunDir, camFitCornersMC2[8])` — `gos_postprocess.cpp:1453-1578`. Called from `mclib/txmmgr.cpp` renderLists() ~1985 with the 8 camera-frustum corners unprojected to MC2 world space.

| Stage | Math | Line |
|---|---|---|
| Frustum XY bounds | min/max of the 8 frustum corners' X,Y | 1468-1474 |
| Map clamp | `r = mapHalfExtent_ * √2 * 1.05`; clamp bounds to `[-r,r]` | 1475-1477 |
| Fit radius | `fitRadius = max(halfX,halfY)`, clamped `[64, r]` | 1478-1484 |
| Pow-2 snap | `xyRadius = 64; while(<fitRadius) *=2; cap at r` (anti-shimmer) | 1485-1487 |
| Texel snap | `worldUnitsPerTexel = 2*xyRadius/4096`; `camX/Y = floor(c/wupt)*wupt` | 1488-1491 |
| Light pos | `camXYZ - sunDir * depthDist`, `depthDist = 5000` (fixed) | 1492-1496 |
| Ortho | half-extent `±xyRadius`; near=1, far=`2*depthDist`=10000; clip-z [0,1] | 1524-1530 |
| View | look-at, Z-up; right=cross(sun,Z), up=cross(right,sun) | 1498-1518 |

**Verdict: view-fit + texel-snapped. Not fixed-world.** Padding/snapping already present.

### Static vs dynamic matrix

| | Static (`buildStaticLightMatrix:1249`) | Dynamic (`buildDynamicLightMatrix:1453`) |
|---|---|---|
| Center | world origin (0,0,0) | texel-snapped frustum XY center |
| XY half-extent | `r` (full map diagonal, FIXED) | `xyRadius` (frustum-fit, clamped to `r`) |
| Far | `2r` | 10000 (fixed) |
| Texel snap | none | yes |
| Rebuilt | once (latched) | every frame |
| Casters | terrain heightfield only | static-prop + mech batchers |

## 2. Measured coverage (Task 2) — `MC2_SHADOW_FRUSTUM_DIAG=1`, mc2_24

```
[SHADOW_FRUSTUM_DIAG] sunDir=(-0.695,0.186,-0.695)
frustumXY=[-11404..11404, -11404..varies]  center=(0, -9204 .. -2768 as cam pans)
fitRadius=11404  xyRadius=11404  mapClampR=11404   ← clamps to full map EVERY frame
texelWU=5.568   orthoWH=22808x22808   depth=[1..10000]   mapSize=4096
```

- **`xyRadius == mapClampR == 11404` on every frame** → the frustum-fit always saturates the map-diagonal clamp. The camera sees the whole map; the "fit" never produces a smaller box.
- **5.568 WU/texel**, uniform. `mapHalfExtent_ ≈ 7680` ⇒ `r = 7680·√2·1.05 ≈ 11404`.
- Depth corridor ±5000 WU about the frustum centre along the sun ray — ample for MC2 terrain elevations; not the limiter.
- Caster set: `flushShadow()` draws **all** uploaded instances (`s_typeRanges`), no per-caster shadow-frustum cull. Submission is gated upstream by the game render loop (caster feed); off-screen / active-radius-culled actors never enter `s_bucketsByType`.

## 3. Diagnostic added (Task 3)

`MC2_SHADOW_FRUSTUM_DIAG=1` — env-gated, read-only, logged in `buildDynamicLightMatrix` (first 3 frames + every 300). Dumps sun dir, frustum XY bounds, centre, fitRadius, xyRadius, map-clamp r, **texel world size**, ortho WxH, depth range, map size. No behavior change; default silent. Per-class caster counts are available via existing `gos_get{StaticProp,Mech}ShadowInstDrawn()` accessors (not yet in the JSON dump — wiring them in is a trivial follow-up if needed).

## 4. Strategy comparison (Task 4)

| | Strategy | Effect given the data | Verdict |
|---|---|---|---|
| **A** | Keep current fixed/degenerate bounds | 5.57 WU/texel full-map; trees ≈1 texel → invisible shadows | status quo, inadequate |
| **B** | Fit dynamic bounds to camera frustum | **Already implemented** — degenerates to full-map (camera spans map). No gain. | NO-OP here |
| **B′** | Fit to a **bounded near radius** around camera look-at (cap fitRadius ≪ r, e.g. ~2500 WU) | ~1.2 WU/texel near the camera → crisp near shadows; **loses far-map casters** | **strong near-term** |
| **C** | Add rigid **buildings** to the world-fixed static shadow map | distant buildings cast stable shadows independent of the dynamic near box; complements B′ | good, separate slice |
| **D** | Add **trees** to the static shadow map | trees are foliage + numerous + animate; static map is terrain/rigid; poor fit | not recommended |
| **E** | Full **CSM** (near crisp cascade + far full-map cascade) | the principled fix: crisp near AND full coverage; B′ is its cascade-0 groundwork | **long-term target, defer** |

## 5. Recommendation (Task 5)

**Immediate next slice — bounded near-radius dynamic fit (B′), NOT plain view-fit.**
The user's "fit to viewport" instinct is right in spirit but already coded; it fails because the RTS camera frustum spans the whole map, saturating the map-diagonal clamp. The lever is to **stop covering the whole map**: cap `fitRadius` at a fixed near radius around the camera look-at (≈2000–3000 WU) instead of clamping up to `r`. At 2500 WU radius → 5000 WU box → **1.22 WU/texel** (≈4.6× finer), giving crisp shadows for trees and near buildings. Keep the existing pow-2 + texel snap (already present) to avoid shimmer.

- **Should the dynamic map be view-fitted?** It already is — and that's the trap. Refine it to a **bounded near region**, not the full clamped frustum.
- **Static buildings → static shadow map?** Yes, as a **separate follow-up (C)**: rigid buildings are world-fixed, so a once-built static-map pass gives them stable crisp shadows and covers the far field that B′'s near box drops. Pairs naturally with B′ (near=dynamic, far-rigid=static).
- **Trees → static or dynamic?** Stay **dynamic** (foliage, animated, numerous; not world-fixed-map material). B′ makes their near-field shadows crisp.
- **CSM?** Needed eventually (E) — it's the only way to get crisp near *and* full far coverage simultaneously. **Defer**; B′ is deliberately cascade-0 groundwork so CSM is an extension, not a rewrite.
- **Caster feed** (off-screen casters whose `render()` isn't called) is a **separate, secondary** gap from texel density; address after B′/C if low-sun off-screen casters still matter.
- **Resolution bump** (4096→8192 → 2.78 WU/texel) is the cheapest pure-quality lever if VRAM/fillrate allow, but it only halves texel size vs B′'s ~4.6×; mention as a fallback, not primary.

**Sequencing:** B′ (bounded near fit) → C (rigid buildings in static map for far field) → E (CSM) when near+far must both be crisp. All behind a default-off gate per slice; this audit slice changes nothing visual.
