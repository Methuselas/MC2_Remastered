# Water v2 - Slice S6: Armed-Water Draw-Side Decouple (reframe B)

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated worktree; keep-as-is, user integrates separately)
**Supersedes:** the S6 row of `2026-05-17-water-v2-scope-and-decomposition.md`
Section 4 ("arm-gate the per-water-quad projection block"). That documented
design is EMPIRICALLY NON-SUBSTITUTIVE and dead - see Section 2.
**Status:** DESIGN - grounded (3 code-grounded advisor passes this session,
Rule 0). Plan-ready PENDING two explicit gates (Section 7). Not yet 2x
adversarial.
**Grounding sources:** S6 recon + reframe-A/B + (B)-substitutivity
verification advisors, 2026-05-17. All file:line below were grep-verified
this session; symbols are stable, line numbers drift - the plan-stage Rule-0
block (Section 6) re-confirms before implementation.

---

## 1. Goal

Retire, on armed frames, the per-frame CPU draw-side work the legacy water
path still performs in `TerrainQuad::setupTextures` (`mclib/quad.cpp`), which
the armed GPU water path (water-v1/S1 living-surface MDI) already renders.
SUBSTITUTIVE: the legacy CPU enqueue is DELETED on armed frames (not a GPU
path added beside a still-live CPU path); "done" = the armed
`quadSetupTextures` cost drops in a clean total-frame Tracy with no displaced
cost AND the `[WATER_INVPROJ v1]` parity probe stays `result=identical`.

**Non-goals:** touching the projection/6-tuple reduction (stays
unconditional - Section 3); the un-armed legacy path (untouched - it must
keep working, that is the whole point of the byte-identical predicate);
S2/S5; the `setInverseProject`/minimap consumer (preserved exactly).

## 2. Why the documented S6 is DEAD (load-bearing)

The documented design ("arm-gate the whole per-water-quad projection block")
was falsified by the wired `[WATER_INVPROJ v1]` parity probe (commit
`b273c89`, env `MC2_WATER_INVPROJ_PARITY=1`): on mc2_01 the water-projection
block contributes UNIQUE extrema to the file-scope 6-tuple
`leastZ/mostZ/leastW/mostW/leastWY/mostWY` (`mclib/terrain.cpp` ~:1454-1456)
that feeds `eye->setInverseProject(...)` (`terrain.cpp` ~:1837) -
`result=divergent`, repeatedly, large (e.g. `leastZ` -35 -> -129.9). Cause:
water-elevation Z (`quad.cpp` ~:1040 `vertex3D.z = ourCos +
Terrain::waterElevation`) produces extrema beyond terrain-Z. Arm-gating the
whole block changes what `setInverseProject` receives = NON-substitutive.
The probe did its job: it reshaped S6, it did not merely block it.

## 3. Design (reframe B): split the block; keep (i), arm-gate (ii)

`TerrainQuad::setupTextures` water work decomposes into two interleaved
concerns inside each of the (up to 4) per-water-vertex sub-blocks
(`quad.cpp` ~:1006-1321):

- **(i) KEEP UNCONDITIONAL - projection + 6-tuple reduction.** The up-to-4
  `eye->projectForTerrainAdmission(vertex3D,screenPos)` at water-elevation Z
  (~:1045/1112/1179/1246) and the inline min/max of the local `screenPos`
  into `leastZ/mostZ/leastW/mostW/leastWY/mostWY` (~:1065-1088 /1132-1155
  /1199-1222 /1266-1289). This is cheap (<=4 self-skipping projects per quad,
  guarded by `clipped*` and the already-projected `calcThisFrame&2` skip) and
  reads the LOCAL `screenPos` directly - it does NOT read `->wx..ww`, so it
  is independent of (ii) (grounding Q4). Its 6-tuple is consumed by EXACTLY
  ONE live consumer: the cosmetic tactical-map minimap camera-footprint
  trapezoid (`gametacmap.cpp` ~:225-246 -> `inverseProjectZ` `screen.w==0`
  branch -> `setInverseProject` scalars `camera.h` ~:1115). The
  cursor->ground / move-order path `Camera::inverseProject` (`camera.cpp`
  ~:732) was repointed by a prior VPL-retirement and reads NONE of the
  stored scalars; object-pick never touches the 6-tuple (water/terrain are
  never pick targets - `memory/mc2_selection_picking_model_water_terrain_
  never_picked.md`). Keeping (i) unconditional => the parity probe stays
  `result=identical` => zero behaviour change to the minimap, picking, cull,
  camera, fog.

- **(ii) ARM-GATE - the draw-side that does NOT feed the 6-tuple.** Per
  sub-block: the `clipInfo` writes (~:1053/1056, 1121/1123, 1188/1190,
  1255/1257), the per-vertex `wx/wy/wz/ww` writes (~:1058-1061, 1125-1128,
  1192-1195, 1259-1262), the water texture-handle resolution block
  (`getTextureHandle`/`getWaterTextureHandle`/`getWaterDetailHandle`/
  `setDetail`, ~:1295-1305), and the two
  `addTriangleBulk(waterHandle,...)` / `addTriangleBulk(waterDetailHandle,
  ...)` calls (~:1307-1308). This is the dominant per-frame term (2 bulk
  enqueues x every water-touching quad every frame + per-frame animated
  handle resolution). The gate is INTERLEAVED inside each sub-block (project
  + reduce stay; the stores + handles + bulk go) - NOT one contiguous range.

### 3.1 The gate predicate (load-bearing - get this exactly right)

Gate (ii) on **`gos_terrain_indirect::IsFrameSolidArmed()`**, encoded
**byte-identical** to `Terrain::renderWater`'s loop early-return conjunction
`s_fastPath && gos_terrain_indirect::IsFrameSolidArmed() &&
WaterStream::IsReady() && WaterStream::GetRecipeCount() > 0 &&
terrainTextures2` (`terrain.cpp` ~:1209-1217), mirrored EXACTLY per the
existing "MUST stay byte-identical" contract at `terrain.cpp` ~:1184.

**Do NOT gate on `IsFrameMaskWaterArmed()`** (`gos_terrain_mask_dispatch.cpp`
~:170): grep-verified ZERO consumers anywhere - it is a DEAD predicate (an
earlier recon wrongly recommended it; corrected). Gating on it would skip
(ii) on un-armed intro/deployment-pan frames where `drawWater` STILL runs
(it gates on `IsFrameSolidArmed()`), so `drawWater` would read stale
prior-frame `wx/wy/wz/ww` -> the exact non-substitutive regression
`memory/water_fastpath_interim_fixes_and_residuals.md` fix #2 prevents. The
predicate MUST be the one that already pairs the (ii)-output consumers.

## 4. Substitutivity proof (grounding Q1-Q4, grep-verified; adversarial focus)

- **`clipInfo` cull cascade is immune.** The cull-driving `clipInfo` write
  is the slimReduce `rv->clipInfo = clipR` (`terrain.cpp` ~:1668),
  unconditional for every terrain vertex, running BEFORE the `setupTextures`
  loop (~:1808). (ii)'s water-corner `clipInfo` rewrite is redundant for the
  cascade. The other `clipInfo` reader (`quad.cpp` ~:2160, M2D pz/overlay)
  is `!IsFrameSolidArmed()`-gated (terrain branch) or decal-overlay - not a
  solid-armed water consumer.
- **`wx/wy/wz/ww` sole reader is `drawWater`** (`quad.cpp` ~:3313-3763),
  called only from `Terrain::renderWater` (`terrain.cpp` ~:1250), whose loop
  early-returns under the SAME conjunction when armed (~:1209-1217). So when
  (ii) is skipped, its only `wx..ww` consumer is skipped by the identical
  predicate - paired, no stale read.
- **GPU path covers the enqueue.** `renderWaterFastPath` narrow-candidate set
  is `(pVertex->water & 1)` on any corner when armed (`terrain.cpp` ~:1820),
  the IDENTICAL primary predicate as (ii)'s `(vertices[k]->pVertex->water &
  1)` (`quad.cpp` ~:1008-1011); GPU shader applies the secondary
  `pzOk`/clip. So skipping (ii)'s `addTriangleBulk(water/waterDetail)`
  removes a now-redundant CPU enqueue, not geometry the GPU does not draw.
  `waterHandle`/`waterDetailHandle` (incl. `0xffffffff` skip sentinel) read
  downstream ONLY by `drawWater` (~:3282) + `renderWater` counters - all
  inside the same arm-gated legacy loop. Sentinel-safe.
- **Clean skip, no half-pair.** `addTriangleBulk` (`txmmgr.h` ~:850) is the
  legacy `masterVertexNodes` immediate path; reservation (setupTextures) and
  fill (`drawWater`) are paired and BOTH skipped together under the same
  predicate -> no desynced counter.
- **(i)/(ii) independent.** The 6-tuple reduction reads the local `screenPos`
  from `projectForTerrainAdmission`, not `->wx..ww` - keeping (i) while
  skipping (ii)'s vertex writes is a clean split.

## 5. Telemetry / probes

- `[WATER_INVPROJ v1]` (already wired, commit `b273c89`) is the in-place
  SUBSTITUTIVE CANARY: it MUST stay `result=identical` across the S6 change
  (because (i) is untouched). Any `result=divergent` after S6 = the gate
  leaked into (i) - a hard failure, not a tuning step.
- Add `[WATER_S6 v1]` env-gated (`MC2_WATER_S6_TRACE`), latched edge-print
  (same idiom as `[WATER_REFL v1]`/`[WATER_INVPROJ v1]`): reports the gate
  decision (armed-skip vs legacy-run) + the predicate sub-terms so a smoke
  proves (ii) is actually being skipped on armed frames (not silently always
  running).
- Cost proof is the user-driven clean TOTAL-FRAME Tracy (Section 7) - the
  per-quad `std::chrono` COST_SPLIT scopes here are observer-effect-poisoned
  (terminal terrain-CPU campaign) and MUST stay off / are disqualified.

## 6. Plan-stage Rule-0 verifications (close in the plan)

- **V1:** re-grep the exact per-sub-block line ranges for (i) project+reduce
  vs (ii) clipInfo/`wx..ww`/handle/`addTriangleBulk` in `quad.cpp` (all 4
  sub-blocks); the gate is interleaved - enumerate the exact stays-vs-gated
  statements per sub-block.
- **V2:** re-grep `Terrain::renderWater`'s early-return conjunction
  (`terrain.cpp` ~:1209-1217) + the `~:1184` byte-identical contract; the S6
  gate expression must be a verbatim mirror (same sub-terms, same order).
- **V3:** re-confirm `IsFrameMaskWaterArmed()` still has zero consumers (do
  not regress to it); confirm `IsFrameSolidArmed()` callable from
  `quad.cpp`'s TU.
- **V4:** re-grep every reader of `->wx/->wy/->wz/->ww` and water-quad
  `clipInfo` to re-confirm no armed-frame consumer outside the same-predicate
  `drawWater`/`renderWater` (negative-claim discipline; the single residual
  non-substitutivity risk).
- **V5:** confirm the `addTriangleBulk` queue (legacy `masterVertexNodes`)
  reservation/fill pairing is fully inside the same predicate.

## 7. Gates before execute (BOTH required - not plan-ready until closed)

1. **User-driven clean total-frame Tracy** (armed, user-positioned worst-case
   wolfman zoom-out over a water-heavy mission - autonomous default-camera
   runs under-attribute per `memory/feedback_cost_split_worst_case_camera.md`):
   read the coarse `ZoneScopedN("Terrain::geometry quadSetupTextures")`
   (`terrain.cpp` ~:1755) zone. Must show (ii) is a real per-frame cost on
   the armed path (else S6 is not worth it - blocked-on-telemetry, not a
   wording fix). Post-change: same capture must show the zone drop with NO
   displaced cost elsewhere (substitutive proof).
2. **`[WATER_INVPROJ v1] result=identical`** on a kill-aware mc2_01 smoke
   after the change (the (i)-untouched canary). `divergent` => abort.

## 8. Files (anticipated; grep-confirm at plan)

```
MODIFIED  mclib/quad.cpp        -- TerrainQuad::setupTextures: interleaved
                                   IsFrameSolidArmed()-conjunction gate around
                                   the (ii) clipInfo/wx..ww/handle/addTriangleBulk
                                   statements in all 4 water sub-blocks; (i)
                                   project+6-tuple reduction stays unconditional
MODIFIED  mclib/<probe site>    -- [WATER_S6 v1] env-gated latched gate-decision probe
(UNCHANGED) terrain.cpp [WATER_INVPROJ v1] parity probe = the substitutive canary
```

Load-bearing C++ change -> full relink + isolated deploy to
`A:/Games/mc2-opengl/mc2-win64-water` only.

## 9. Discipline

This spec -> 2 adversarials (opus|sonnet, adversarial-plan-review skill,
code-grounded, CRITICAL/MAJOR/MINOR) -> fold -> plan -> subagent execute ->
isolated build/deploy -> kill-aware mc2_01 smoke (`[WATER_INVPROJ v1]` MUST
stay `identical` + `[WATER_S6 v1]` shows armed-skip) -> user-driven
total-frame Tracy cost gate (the real substitutive proof) -> done only when
the armed `quadSetupTextures` zone drops with no displaced cost AND parity
stays identical. Branch isolated; user integrates separately.
