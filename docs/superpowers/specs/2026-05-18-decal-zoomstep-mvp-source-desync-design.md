# Decal/Water Zoom-Step MVP-Source Desync (Root A) - the zoom-step jump

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated; user integrates separately)
**Owner:** assistant (designated water owner)
**Status:** DESIGN - pending user spec review, then 2 mandatory adversarials.
**Grounding:** 3 code-grounded advisor passes (render-expert recon ->
shader-expert formula -> decal/zoom-step re-ground after new user evidence),
Rule 0. file:line grep-verified 2026-05-18; symbols stable, lines drift -
Section 6 re-confirms at plan.
**Relationship:** This is **Root A**, the DOMINANT user-visible defect (the
zoom-step jump). **Root B** = steady-state distance-nonlinearity, spec
`2026-05-18-water-terrain-zfight-distance-clipz-bias-design.md` (commit
`64f265b`), which correctly deferred this as "Sym3". New user evidence
(jump is zoom-STEP-triggered AND decals share it) promotes Root A to FIRST;
Root B proceeds after, re-prioritized.

---

## 1. Symptom (user, first-hand)

A discrete zoom STEP (one step in OR out) causes a ~0.1 world-unit JUMP +
transient z-fight at the shoreline, settling the next frame. **Decals
exhibit the SAME issue.** Decals are not water - they share only the depth-
projection machinery. A defect common to terrain-vs-water AND terrain-vs-
decal, triggered by a discrete zoom step, = a SHARED-machinery root, not
water-specific.

Out of scope (distinct roots, not failures of this slice): Sym1 (constant
sit-low at all zooms = `waterElevation` baseline); Root B / Sym2 (steady-
state recede that *scales* with zoom-out = the distance-nonlinearity, its
own spec).

## 2. Root cause (grep-verified)

Terrain (armed) projects from **pre-baked `tr.clipPos[cornerIdx]`**
(`shaders/gos_terrain_thin.vert` ~:210), baked by the compute dispatch with
the **dispatch MVP** `g_dispatchMvp16` (`gos_terrain_indirect.cpp` ~:2893).
The 926/0 fix (`gos_terrain_water_stream.cpp` ~:1409-1416) retrofitted the
**water** VS to consume `gos_terrain_indirect_getDispatchMvp16()` when
`IsFrameSolidArmed()`, making armed water bit-consistent with terrain's
baked clip across ALL MVP changes (incl. zoom steps - it is matrix-source
consistency, not pan-specific).

**Decals were never retrofitted.** The decal/overlay VS
(`shaders/terrain_overlay.vert` ~:31/36) projects live
`terrainMVP * worldPos`; its `terrainMVP` uniform is uploaded by
`uploadOverlayUniforms_` (`GameOS/gameos/gameos_graphics.cpp` ~:7014) from
the **live, late-sampled `getTerrainMVP()` / `terrain_mvp_`** - NO
`IsFrameSolidArmed()` gate, NO `getDispatchMvp16()` path. On a discrete
zoom step the projection changes between frame N and N+1; for the
transition frame the terrain quad uses frame-N's baked dispatch MVP while
the decal at the same world point uses frame-N+1's live `terrain_mvp_` =
a 1-frame depth desync = the observed ~0.1u jump + transient z-fight.
Decals show it ALWAYS (no armed path at all); residual water jump only in
the un-armed branch (armed water is already 926/0-consistent). This is the
`ring_slot_state_must_travel_with_slot` recurrence: terrain=Fix B,
water=926/0 retrofit, **decals=neither**.

## 3. The fix (no shader change; mirror the proven 926/0 pattern)

Retrofit the decal MVP source to the SAME dispatch-MVP-when-armed source
water uses. **No shader edit** - `terrain_overlay.vert` already does
`terrainMVP * worldPos`; only the uploaded matrix VALUE changes.

In `GameOS/gameos/gameos_graphics.cpp` `uploadOverlayUniforms_` (~:7014),
for the **armed static decal bake path only**: source the overlay
`terrainMVP` from `gos_terrain_indirect_getDispatchMvp16()` when
`gos_terrain_indirect::IsFrameSolidArmed()`, else fall back to
`getTerrainMVP()` - byte-identical in shape to the water retrofit
`gos_terrain_water_stream.cpp` ~:1409-1416.

**Scope discipline (load-bearing trap):** `uploadOverlayUniforms_` is
SHARED between the armed static decal bake (`drawDecalStaticBatch`, armed-
gated via `IsFrameOverlayArmed()` `mclib/txmmgr.cpp` ~:2079) AND the live
per-frame `gos_DrawTerrainOverlays` path (correctly CPU-pz-culled per
frame - it is NOT the armed static bake and must keep its live MVP). Do
NOT blindly repoint the shared helper. Either (a) pass the MVP (or an
"use-dispatch-MVP" bool) as a parameter so only the static-bake caller
opts in, or (b) split the upload. The plan picks the minimal-touch form;
the per-frame overlay path's behaviour must be byte-unchanged.

## 4. Load-bearing constraints / regression risks (adversarial focus)

- **THE shared-helper repoint (mandated adversarial focus):** prove the
  change affects ONLY `drawDecalStaticBatch`'s armed bake and leaves the
  live `gos_DrawTerrainOverlays` overlay path's uploaded MVP byte-
  identical. A naive repoint of `uploadOverlayUniforms_` regresses live
  overlays. Grep every caller of `uploadOverlayUniforms_` and classify.
- **Frame-anchor correctness:** the dispatch MVP must be read via
  `gos_terrain_indirect_getDispatchMvp16()` gated on `IsFrameSolidArmed()`
  EXACTLY as the water retrofit does (the armed anchor, never a separately-
  sampled `terrain_mvp_` - that is the bug). Inherit the 926/0 pattern;
  do not re-derive.
- **glPolygonOffset double-stack:** the decal path also runs
  `glPolygonOffset(-1,-1)` (`gameos_graphics.cpp` ~:7170-7171). This slice
  does NOT change the decal depth-offset mechanism (only the MVP source),
  so glPolygonOffset stays as-is. NOTE for Root B: if decals later move to
  a clip-z bias, that glPolygonOffset MUST be removed in the same change
  (double-stack) - out of scope here, recorded so it is not lost.
- **Un-armed transition:** when `!IsFrameSolidArmed()` the decal falls back
  to `getTerrainMVP()` (today's behaviour) - armed<->un-armed boundary must
  not introduce a new pop (mirror exactly how the water retrofit handles
  the same boundary; un-armed is the brief intro/deploy pan).
- **No regression** of: the shipped S1/S6/transparency water (untouched -
  this is decal-MVP-source only), the static decal bake correctness
  (`MC2_TERRAIN_INDIRECT_OVERLAY`), terrain Fix-B, the water 926/0
  invariant (`[WATER_DEPTHPROBE v2]` stays equal=1 - it is blind to decals,
  NOT this slice's canary).
- Sym1 / Root-B Sym2 remain present after this - NOT failures of Root A.

## 5. Canary (new; existing probes are blind to the decal MVP source)

`[WATER_DEPTHPROBE v2]` (`gos_terrain_water_stream.cpp` ~:1429) hashes the
water-vs-terrain MVP only; it never sees the decal-uploaded MVP and stays
`equal=1` through a decal-jump regression. Add `MC2_DECAL_DEPTHPROBE`
(env-gated, silent default, demote-not-delete), mirroring the
`[WATER_DEPTHPROBE v2]` equal-hash invariant: once per N frames on armed
frames, hash the MVP `uploadOverlayUniforms_` actually uploaded for the
static decal bake vs `gos_terrain_indirect_getDispatchMvp16()`; emit
`[DECAL_DEPTHPROBE v1] f=<frame> armed=<0|1> equal=<0|1>`. Post-fix it
must read `equal=1` on EVERY armed frame including zoom-step transition
frames (the mirror of the 926/0 water invariant). Pre-fix it reads
`equal=0` (or diverges) on the zoom-step transition frame - that is the
numeric proof of the root and of the fix.

## 6. Plan-stage Rule-0 verifications (close in the plan)

- **V1:** re-grep `uploadOverlayUniforms_` def + EVERY caller; classify
  each armed-static-bake vs live-per-frame-overlay; confirm the exact
  minimal opt-in form that leaves the live path byte-unchanged.
- **V2:** re-grep the water retrofit `gos_terrain_water_stream.cpp`
  ~:1409-1416 (the byte-identical pattern to mirror) +
  `gos_terrain_indirect_getDispatchMvp16()` signature + `IsFrameSolidArmed`
  callability from the overlay-upload TU.
- **V3:** re-grep `drawDecalStaticBatch` + its `IsFrameOverlayArmed()`
  gate (`txmmgr.cpp` ~:2079) - confirm it is the armed static bake caller
  of `uploadOverlayUniforms_`.
- **V4:** confirm `terrain_overlay.vert` needs NO edit (already
  `terrainMVP*worldPos`); confirm the `glPolygonOffset(-1,-1)` site is
  untouched by this slice.
- **V5:** confirm `[WATER_DEPTHPROBE v2]` is blind to decals (stays equal=1
  regardless) + the per-frame probe block location for
  `[DECAL_DEPTHPROBE v1]`.

## 7. Gates

Build (RelWithDebInfo, full relink - `gameos_graphics.cpp` load-bearing) +
deploy exe to isolated `mc2-win64-water` ONLY. Kill-aware `mc2_01` smoke +
`MC2_DECAL_DEPTHPROBE=1`: `[DECAL_DEPTHPROBE v1] equal=1` on every armed
frame INCLUDING during a zoom sweep (the harness/user steps zoom) +
`[WATER_DEPTHPROBE v2]` still equal=1 + no `0(N): error`. Then the REAL
gate = USER visual: the zoom-step jump + transient z-fight GONE on decals
(and on water if any un-armed residual), across in/out zoom steps; no live-
overlay regression; no S1/S6/transparency/static-decal-bake regression.
Sym1 + Root-B steady-state recede expected to persist (NOT failures of
this slice). RenderDoc pixel-history on a shoreline+decal pixel across one
zoom step (frame N vs N+1) is the recommended root-confirm + fix-confirm.

## 8. Files (anticipated; grep-confirm at plan)

```
MODIFIED  GameOS/gameos/gameos_graphics.cpp  -- uploadOverlayUniforms_ /
                                                drawDecalStaticBatch armed
                                                path: dispatch-MVP-when-armed
                                                source (mirror water 926/0);
                                                [DECAL_DEPTHPROBE v1] canary
(UNCHANGED) shaders/terrain_overlay.vert (already terrainMVP*worldPos -
            value-only change), glPolygonOffset site, all water/terrain VS
```

## 9. Discipline

This spec -> user spec review -> 2 adversarials (opus|sonnet,
adversarial-plan-review skill, code-grounded, CRITICAL/MAJOR/MINOR; the
shared-`uploadOverlayUniforms_`-repoint-must-not-regress-live-overlays is
the mandated focus) -> fold -> plan -> subagent-driven execute (per-task
spec+quality review) -> isolated build/deploy -> kill-aware mc2_01 smoke
(`[DECAL_DEPTHPROBE v1] equal=1` across zoom + `[WATER_DEPTHPROBE v2]`
equal=1) -> USER visual gate (Root A only) -> then proceed to Root B
(`64f265b` spec, re-prioritized). Branch isolated; user integrates
separately.
