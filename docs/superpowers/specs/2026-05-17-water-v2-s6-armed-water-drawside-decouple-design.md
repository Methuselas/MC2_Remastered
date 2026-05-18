# Water v2 - Slice S6: Armed-Water Draw-Side Decouple (reframe B)

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated worktree; keep-as-is, user integrates separately)
**Supersedes:** the S6 row of `2026-05-17-water-v2-scope-and-decomposition.md`
Section 4 ("arm-gate the per-water-quad projection block"). That documented
design is EMPIRICALLY NON-SUBSTITUTIVE and dead - see Section 2.
**Status:** PLAN-READY (2026-05-17). Grounded (4 code-grounded advisor
passes, Rule 0); dual-adversarial APPROVE-WITH-REQUIRED-EDITS folded (8b);
both escalated forks resolved by user - M1a (shared predicate) + M2b-i
(accept 1-frame transition pop), M2a locked (8c). Execution gated ONLY on
the two Section 7 user-driven execute-gates (worst-case Tracy cost baseline
+ post-change parity-identical).
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
  `result=identical` in STEADY STATE => zero steady-state behaviour change
  to the minimap, picking, cull, camera, fog. **(M2b-i, user 2026-05-17:)**
  one bounded, cosmetically-negligible 1-frame minimap-trapezoid pop at the
  intro-pan->mission `IsFrameSolidArmed()` edge is ACCEPTED as out of scope
  (intrinsic to the existing slimReduce/clipped-gate machinery, not
  introduced by S6, never during gameplay, minimap rectangle only - see
  8c-UPDATE). No arm-transition probe gate (M2b-ii declined).

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

### 3.1 The gate predicate (load-bearing - RESOLVED M1a: single shared source)

**FORK M1 RESOLVED -> M1a (user, 2026-05-17): extract ONE shared
predicate.** Add a single callable - `Terrain::WaterFastPathOwnsArmedDraw()`
(or a `gos_terrain_indirect` sibling; plan picks the exact home) - that
encapsulates the FULL conjunction including the `s_fastPath` definition
(`getenv("MC2_RENDER_WATER_FASTPATH") != nullptr ||
gpu_driven::IsWaterEnabled()`) + `gos_terrain_indirect::IsFrameSolidArmed()`
+ `WaterStream::IsReady()` + `WaterStream::GetRecipeCount() > 0` +
`Terrain::terrainTextures2 != nullptr`. `Terrain::renderWater`'s
early-return (`terrain.cpp` ~:1209-1217) is refactored to CALL it, and the
new `quad.cpp` (ii) gate calls the SAME helper. This RETIRES the
already-documented-fragile `terrain.cpp` ~:1184 "MUST stay byte-identical"
hand-copy contract (single source of truth -> zero drift-regression risk;
the `s_fastPath` function-static is no longer hand-reconstructed anywhere).
Minimal-touch-justified: S6 already modifies the predicate's consumer, so
bringing the predicate to a single source is the modern-when-touched move,
not unrelated cleanup. **Do NOT gate on `IsFrameMaskWaterArmed()`**
(grep-verified dead, zero consumers).

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

## 8b. DUAL ADVERSARIAL OUTCOME (opus + sonnet, 2026-05-17) - APPROVE-WITH-REQUIRED-EDITS

Both independent code-grounded reviews: APPROVE-WITH-REQUIRED-EDITS. NO
CRITICAL, NO BLOCK. Negative claims survived adversarial grep
(`IsFrameMaskWaterArmed()` zero-consumer = confirmed dead; `drawWater` sole
`wx..ww` reader = confirmed; slimReduce `clipInfo` write `terrain.cpp:~1668`
runs before the setupTextures loop = cull-cascade immune confirmed;
`addTriangleBulk` legacy `masterVertexNodes` reservation/fill paired =
confirmed). Findings folded below; two are escalated design forks (see 8c).

FOLDED (unambiguous):
- **(sonnet MAJOR-1) Consumer chain corrected.** `gametacmap.cpp:~225-246`
  calls `eye->inverseProjectForPicking` (thin inline, `camera.h:~634`) which
  delegates to `inverseProjectZ`; the `setInverseProject` scalars
  (`startZInverse/zPerPixel/startWInverse/wPerPixel`) are read ONLY in the
  `screen.w==0` branch UNDER `usePerspective` (`camera.cpp:~1957-1966`); the
  `!usePerspective` path falls through to `Camera::inverseProject` which
  reads none of them. The "sole live consumer" claim holds in the shipped
  oblique-30deg PERSPECTIVE config but is camera-mode-conditional. Section 3
  reworded; plan-stage V-check added (V6) asserting `usePerspective` true in
  the targeted config.
- **(sonnet MAJOR-2) Gate boundary vs the clipped `else` sentinel.** The
  (ii) gate wraps ONLY the `if (clipped1 || clipped2) { ... }` body (handle
  resolution `~:1297-1305` + the two `addTriangleBulk` `~:1307-1308`); the
  `else { waterHandle = 0xffffffff; waterDetailHandle = 0xffffffff; }`
  sentinel (`~:1310-1314`) stays UNCONDITIONAL so armed frames always write
  the sentinel (no stale prior-frame handle). The stale anticipatory comment
  at `terrain.cpp:~1821` ("Armed: setupTextures() gated, waterHandle never
  set" - written for the DEAD old S6 design) MUST be corrected by the
  executor in the same commit. Section 3 (ii) enumeration updated.
- **(sonnet MAJOR-3 / opus) Canary scope qualified.** `[WATER_INVPROJ v1]
  result=identical` on mc2_01 is the STEADY-STATE substitutive proof and is
  valid only with `usePerspective==true` (per MAJOR-1) and only for the
  armed in-mission steady state - it structurally cannot see an arm/un-arm
  TRANSITION-frame pop (opus M2). Section 7 gate 2 qualified; an
  arm-transition assertion is part of the open fork 8c-(2).
- **(sonnet MINOR-1) clipInfo if/else both branches assign identically**
  (`quad.cpp:~1053-1056` etc. - collapsed DX8/GL dead-code pattern): the
  gate/move needs 8 wrap/move points (2 per sub-block x4), not 4. Documented
  for the executor; added to V1.
- **(line nits)** `drawWater` is `quad.cpp:~3264` (`wx..ww` reads
  `~:3313-3614`); `drawLine` `~:3891` reads `clipInfo` but NOT `wx..ww` and
  is debug-grid, NOT same-predicate - V4 must distinguish it. handle block
  `~:1297-1305` (not 1295). All -> V-checks (Section 6), not blocking.

## 8c. OPEN DESIGN FORKS escalated by both reviewers (require sign-off before plan)

- **(opus M1) The "byte-identical mirror" predicate is not mechanically
  reproducible.** `s_fastPath` is a function-static local in
  `Terrain::renderWater` (`terrain.cpp:~1194-1196` =
  `getenv("MC2_RENDER_WATER_FASTPATH")!=nullptr || gpu_driven::
  IsWaterEnabled()`), NOT visible from `quad.cpp`'s TU. An executor cannot
  "mirror" it; reconstructing it and dropping the `gpu_driven::
  IsWaterEnabled()` term (the load-bearing half per the `:1184` contract)
  silently desyncs the gate from `renderWater` -> reintroduces the
  stale-`wx..ww` regression `water_fastpath_interim_fixes_and_residuals.md`
  fix #2 prevents. FORK: (M1a) extract a single shared predicate
  `WaterFastPathOwnsArmedDraw()` that BOTH `renderWater:~1209` and the new
  `quad.cpp` gate call (retires the fragile `:1184` hand-copy contract;
  touches `renderWater`; minimal-touch rule favors this since the spec
  already touches the predicate's consumer) vs (M1b) mandate verbatim
  reconstruction incl. the full `s_fastPath` definition + a char-for-char
  plan V-check across both sites (smaller blast radius; keeps the
  documented-fragile contract alive a third time).
- **(opus M2) clipInfo cross-frame feedback into (i).** (i)'s reduction is
  gated by `clipped1/clipped2` derived from `vertices[N]->clipInfo`; (ii)
  writes `clipInfo`. With the `calcThisFrame&2` fast-skip, a quad skipped
  next frame re-reads STORED `clipInfo`. If (ii)'s clipInfo write is gated
  off on armed frames, the water-corner `clipInfo` becomes whatever
  slimReduce wrote (terrain-Z onscreen test, NOT the water-Z admission (ii)
  would write) -> on the first armed frame after a legacy frame, (i)'s
  `clipped1/2` gate input changes, possibly flipping whether the 6-tuple
  reduction runs for a boundary quad -> a 1-frame minimap-trapezoid pop at
  the intro-pan->mission edge that the steady-state canary cannot see, and
  §1's "zero behaviour change to the minimap" promise is at risk. So (ii)'s
  `clipInfo` write is redundant for the CULL CASCADE but NOT proven
  redundant for (i)'s own re-entry gate. FORK: (M2a) MOVE the water-corner
  `clipInfo` write OUT of (ii) and keep it UNCONDITIONAL as part of (i) (one
  cheap assignment per corner; makes (i) self-consistent across the arm
  transition; CHANGES the (i)/(ii) boundary the spec is built on -> needs
  the substitutivity-verification advisor to re-bless that (i)+clipInfo
  stays parity-identical) vs (M2b) keep clipInfo in (ii) + add an explicit
  arm-transition probe (assert the 6-tuple matches a forced-legacy
  reference on the first N frames after an armed<->un-armed edge) and accept
  a possibly-visible 1-frame cosmetic trapezoid pop as out-of-scope (needs
  USER ruling vs the §1 zero-minimap-change promise).

### 8c-UPDATE: M2 re-blessed (substitutivity-verification advisor, 2026-05-17)

**M2a is ADOPTED as a settled boundary rule, NOT a fork.** Grounded:
`clipData` is produced wholly by the (i) projection
(`clipData = eye->projectForTerrainAdmission(...)`, then `if(!isVisible)
clipData=false`) with ZERO (ii) dependency; the `clipInfo = clipData` write
is ALREADY physically above the proposed (ii) boundary today, so "keep it in
(i)" is parity-identical-to-today by construction and breaks no consumer
(every `clipInfo` reader uses it as a reset-then-set boolean, never as a
written-ness "water-drew" sentinel). **S6 (ii)-gate boundary RULE: the
`clipInfo = clipData` assignment(s) stay above the gate (in (i)),
unconditional.** Free, correct, mandatory.

**But M2a does NOT make the canary sufficient — the adversarial's deeper
premise was FALSE.** The cross-arm-transition variance in next-frame
`clipped1/clipped2` does NOT originate in (ii)'s clipInfo write. It
originates in: (a) slimReduce (`terrain.cpp:~1668`) unconditionally stamping
EVERY vertex's `clipInfo` with a TERRAIN-Z admission every frame, before
setupTextures; and (b) (i) being entered at all only when the
slimReduce-derived `clipped1||clipped2` is true and `calcThisFrame&2` is
clear. M2a touches neither. So a bounded 1-frame 6-tuple/minimap-trapezoid
pop across the `IsFrameSolidArmed()` edge (intro-pan -> mission) can still
occur, and the `[WATER_INVPROJ v1]` STEADY-STATE canary structurally cannot
see it (it latches A-vs-B within one frame, never frame-N vs frame-N+1
across an arm edge). This residual is intrinsic to the existing
slimReduce/clipped-gate machinery, NOT introduced by S6's (ii) gate - S6
merely makes one frame's water-corner 6-tuple contribution blink at the
transition.

### 8c-FORKS still requiring user sign-off before plan

- **(M1) predicate single-sourcing** - unchanged from above: (M1a) extract
  shared `WaterFastPathOwnsArmedDraw()` both sites call (retires the fragile
  `:1184` hand-copy; touches `renderWater`; minimal-touch-favored) vs (M1b)
  verbatim reconstruct incl. full `s_fastPath` def + char-for-char V-check
  (smaller blast radius; keeps fragile contract a 3rd time).
- **(M2b) the intrinsic 1-frame arm-transition minimap pop** (M2a adopted
  regardless): (M2b-i) ACCEPT it as cosmetically negligible - the 6-tuple
  feeds ONLY `setInverseProject` -> the minimap camera-footprint trapezoid
  (`terrain.cpp:~1928`); one frame of slightly-off screen->world extrema at
  the intro-pan->mission edge, never during gameplay; this RELAXES the
  Section 1 "zero behaviour change to the minimap" promise to "zero
  steady-state change; one cosmetically-negligible transition frame" - a
  USER ruling. vs (M2b-ii) add a MANDATORY arm-transition parity probe
  (snapshot the 6-tuple on the last armed + first unarmed frame across an
  `IsFrameSolidArmed()` edge, assert match) as a third execute-gate -
  definitively safe, more instrumentation.

**BOTH FORKS RESOLVED (user, 2026-05-17): M1 -> M1a (shared predicate
`WaterFastPathOwnsArmedDraw()`, retires the `:1184` hand-copy); M2b ->
M2b-i (accept the 1-frame transition pop, promise relaxed to steady-state,
NO arm-transition probe).** M2a locked (boundary rule). Spec is now
PLAN-READY. Execution remains gated ONLY on the two Section 7 execute-gates
(user-driven worst-case Tracy cost baseline + post-change `[WATER_INVPROJ
v1] result=identical`).

## 8d. Tracked follow-up (Task 1 code-quality finding, non-blocking)

`Terrain::renderWaterFastPath` (`terrain.cpp` ~:1299-1305) still hand-rolls
its OWN `s_fastPath` getenv-once + `IsReady()`/`GetRecipeCount()`/
`terrainTextures2` guards - a THIRD independent copy of 4/5 of the same
conjunction `WaterFastPathOwnsArmedDraw()` now single-sources (it differs
only on the `IsFrameSolidArmed()` term, since the fast-path's own
early-returns are arm-shaped differently). M1a correctly drift-proofs the
TWO consumers it scopes (`renderWater`'s legacy-skip + Task 2's `quad.cpp`
(ii) gate), but the PRODUCER (`renderWaterFastPath`) is not yet routed
through the shared predicate. Pre-existing (not introduced by S6); flagged
so the "gate can never drift" claim is honest. Consolidating it needs an
arm-agnostic predicate variant (the fast-path entry differs on the
`IsFrameSolidArmed()` term) - a separate small follow-up slice, NOT in S6
scope. Do not silently expand S6 to cover it; track it as the natural next
single-sourcing step after S6 ships.

## 9. Discipline

This spec -> 2 adversarials (opus|sonnet, adversarial-plan-review skill,
code-grounded, CRITICAL/MAJOR/MINOR) -> fold -> plan -> subagent execute ->
isolated build/deploy -> kill-aware mc2_01 smoke (`[WATER_INVPROJ v1]` MUST
stay `identical` + `[WATER_S6 v1]` shows armed-skip) -> user-driven
total-frame Tracy cost gate (the real substitutive proof) -> done only when
the armed `quadSetupTextures` zone drops with no displaced cost AND parity
stays identical. Branch isolated; user integrates separately.
