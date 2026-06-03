# Vehicle GPU-Batch — Design (VEHICLE-GPU-BATCH-1)

**Date:** 2026-06-03
**Slice kind:** draw-path port (default-OFF, parity-gated — mirrors Track D mech-batcher rollout)
**Status:** SPEC — pending review
**Recon:** `docs/superpowers/explorations/2026-06-03-vehicle-gpu-instance-recon.md`
**Precedent (borrow heavily):** Track D mech batcher — `GameOS/gameos/gos_mech_batcher.{h,cpp}` + the CPU submit feed `mclib/mech3d.cpp:2557-2708`.

---

## 1. Purpose

Port ground-vehicle drawing from the legacy per-vehicle CPU MLR vertex transform
(`GVAppearance` → `gvShape->TransformMultiShape` + `gvShape->Render`) to the **existing
GPU mech batcher**. The CPU keeps the cheap per-node `shapeToWorld` matrix transform;
the GPU does the per-vertex transform and draw — exactly what mechs already do.

**Why reuse, not rebuild:** the mech batcher's bone model is already *rigid
one-bone-per-`SHAPE_NODE`, sourced from `shapeToWorld`* (`gos_mech_batcher.cpp:1060-1064/1354-1364`),
which is precisely the vehicle case (body + 1 turret joint). Vehicles are strictly
simpler than mechs. This slice is mostly *mirroring* the mech submit site, not new
infrastructure.

### Non-goals (explicit)
- **`recalcBounds` stays CPU** (screen rect, picking, LOD select, HUD `screenPos`). The
  port removes only the per-vertex transform + MLR submit, not the whole AppearanceUpdate.
- **objectID-buffer pick for vehicles** — new capability (slot exists, unused today). Follow-on.
- **Moving vehicle shadows → batcher `flushShadow()`** — currently half-wired via
  static-prop registration. Follow-on.
- **Sensor blips + fades stay on the CPU MLR path** (tiny shapes; not worth GPU).
- No new SSBO fields, no shader changes beyond what the mech batcher already has (unless
  Task 4 alpha forces a minimal add).

---

## 2. Measurement gate (Task 0 — BLOCKING, do before any port code)

The win is the *vertex transform + MLR submit*, NOT the full ~30–44µs
`Vehicles AppearanceUpdate` Tracy bucket (`recalcBounds` is a separate CPU cost that
stays). Confirm the transform dominates before committing to the port.

- Sub-instrument the vehicle appearance path with non-overlapping Tracy zones (coarse,
  100ns-floor): `Vehicle.RecalcBounds` (`gvactor.cpp:1628`), `Vehicle.UpdateGeometry`
  (the `updateGeometry` body incl. `TransformMultiShape` `:2389-2542`),
  `Vehicle.Render` (`gvShape->Render` `:2173`).
- **Also clarify the transform SEAM (load-bearing — patch 6):** read what
  `TransformMultiShape` actually does (node `shapeToWorld` matrix production vs per-vertex
  transform) and what `mech3d.cpp:2557-2708` actually skips. The recon is internally
  inconsistent here (it says both "skip TransformMultiShape" and "mechs keep
  TransformMultiShape, skip Render"). Resolve it: identify the exact call(s) the mech port
  skips, and confirm the **node-matrix production survives** (the batcher's bone SSBO needs
  `listOfShapes[i].shapeToWorld`). Record the precise seam: which call produces node
  matrices (KEEP) vs which does the per-vertex transform + MLR submit (SKIP).
- One user-driven Tracy capture, vehicle-heavy mission.
- **GATE (patch 1):** portable cost = the per-vertex transform + `Vehicle.Render` (per the
  seam above; node-matrix production is NOT portable, it stays CPU).
  - **PROCEED** if `(per-vertex transform + Render) >= 60% of vehicle appearance cost`
    **OR** absolute savings `>= 0.15ms` in a vehicle-heavy mission.
  - **STOP** if portable cost `< 0.10ms` AND `recalcBounds` dominates — write a short note;
    the port would be a big change for a small win.
- This task ships its own commit (observational zones + the seam note, no behavior change)
  and stays in the tree (useful for the before/after perf proof).

---

## 3. Architecture

### 3.1 Generalize the batcher type key — OPAQUE handle, mech path byte-identical
Today `gos_mech_batcher` keys registration on `Mech3DAppearanceType*` / `TG_TypeShape*`,
but everything it READS is generic `TG_TypeMultiShape`/`TG_MultiShape`.
- **(patch 3)** Use an **opaque type key (handle / integer ID)** — NOT a new shared
  `AppearanceType` base class. Mechs and vehicles each map their concrete type to a unique
  opaque key the batcher stores. No cross-hierarchy coupling.
- **Mech registration/submit must stay byte-identical** — the mech call sites pass the same
  data; only the internal key type widens. Verify mech tier1 (mc2_17/24) unchanged.
- **(patch 4 — naming)** Do NOT rename `gos_mech_batcher.{h,cpp}` / `GpuMechBatcher` /
  `GpuMech*` structs broadly in this slice. Add vehicle support **internally** under the
  existing names. A rename to a neutral name (e.g. `GpuActorBatcher`) is a later cosmetic
  slice if desired. Vehicle-facing wrappers may use vehicle-named thin shims.

### 3.2 Vehicle registration (type-load, mirror mech3d.cpp:381)
At `GVAppearanceType` load, `registerTypeLod(vehicleTypeKey, lod)` for each populated
`gvShape[lod]` — same call mechs use. `finalizeGeometry()` at map-load already covers all
registered types (no vehicle-specific change).

### 3.3 Vehicle submit site (mirror mech3d.cpp:2557-2708)
In the vehicle per-frame path (where `updateGeometry` runs the node transforms):
```
ALWAYS: produce per-node shapeToWorld matrices on the CPU   // patch 6 — NEVER skip this
        (the cheap per-node transform; the bone SSBO + recalcBounds + picking all need it)
IF s_vehicleGpuBatch AND NOT suppressGpu(this):
    fill GpuMechSubmitDesc from gvShape (per-node shapeToWorld, slot0TexHandle=localTextureHandle,
        lightDataIndex, renderFlags, highlightARGB, fogARGB, objectIdRaw=0 for now)
    if batcher.submitActor(desc) succeeded:
        SKIP only the per-vertex CPU transform + the MLR submit   // the win
        (mirror EXACTLY what mech3d.cpp:2557-2708 skips — per Task 0's seam finding;
         mechs keep node-matrix production and skip gvShape->Render(true))
    else:
        fall through to legacy CPU path (full per-vertex transform + Render)
ELSE:
    legacy CPU path (unchanged)
```
**(patch 6) Transform seam — load-bearing:** do NOT skip "all of `TransformMultiShape`"
blindly. Node `shapeToWorld` matrices MUST still be produced (the batcher's `GpuMechBone`
rows are copied from `listOfShapes[i].shapeToWorld`; `recalcBounds`/picking need them too).
Skip only the per-vertex CPU transform + MLR render — the exact set Task 0 identifies that
`mech3d` skips. If `TransformMultiShape` produces node matrices AND transforms vertices in
one indivisible call, the seam may require the same mechanism mech3d uses (e.g. keep the
call, skip `Render`) rather than skipping `TransformMultiShape` itself.

`suppressGpu(this)` = `sensorLevel > 0` (blip path) OR fading (`alphaValue != 0`, patch 2 —
CPU fallback) OR (patch 5) any vertex-deforming anim frame.

### 3.4 Draw + dispatch
No new draw code — the batcher's existing `flush()` (after `renderLists`) draws all
submitted instances (mechs + now vehicles) in one path. Vehicle types simply appear as
additional registered types/instances.

### 3.5 Kill-switch + parity (mirror Track D rollout)
- `MC2_VEHICLE_GPU_BATCH` — **default OFF** initially. `=1` enables GPU submit.
- Parity gate during soak: with GPU on, the legacy CPU path is the comparison authority;
  ship default-OFF, A/B visually + counter, flip default-ON after soak (Track D precedent:
  operator visual + `[MECHBATCHER v1] fallback_total=0` style gate).
- A `[VEHICLE_BATCH v1]` summary counter (submitted / fallback / suppressed-sensor) per
  mission for observability.

---

## 4. Risks & handling (recon-mapped)

| Risk | Severity | Handling |
|---|---|---|
| Sensor blips render small shapes *in place of* mesh when `sensorLevel>0` (`gvactor.cpp:2371`) | MED | `suppressGpu` excludes `sensorLevel>0`; blips stay on CPU MLR path |
| Fade (`alphaValue`) — no per-instance alpha slot | MED | **(patch 2)** While fading, keep CPU path (`suppressGpu`). NO SSBO/shader alpha in this slice. |
| `gvAnimData` keyframe anim (repair/refit) may deform vertices, not just node matrices | MED→VERIFY | **Task 5 pre-check:** confirm `SetFrameNum`/`currentFrame` drive node `shapeToWorld` only. Node-only → free on GPU. Vertex-deform → `suppressGpu` for those frames (CPU fallback) |
| Per-actor LOD swap deletes/recreates `gvShape` (`gvactor.cpp:1855`) | MED | Re-register or key on type×LOD so the swap finds the right registered recipe (mechs solved this per-LOD) |
| Batcher type-key generalization regresses mechs | MED | Keep mech registration/submit byte-identical; gate vehicle path entirely behind `MC2_VEHICLE_GPU_BATCH`; tier1 mech missions (mc2_17/24) unaffected with flag off |
| objectID pick / shadows not ported | LOW (scoped out) | Vehicles keep current pick (CPU screen-rect) + current half-wired shadow; explicit follow-ons |

---

## 5. Validation
- **Task 0 gate:** transform is the dominant per-vehicle cost (else STOP).
- **Visual parity (default-OFF vs `MC2_VEHICLE_GPU_BATCH=1`):** same-camera A/B on a
  vehicle-heavy mission — vehicles render identically (body + turret rotation), sensor
  blips unchanged, fades unchanged, picking + HUD bars unchanged.
- **Perf proof:** `Vehicle.UpdateGeometry`+`Vehicle.Render` (from Task 0 zones) drop with
  the flag on; total frame drops (substitutive, not moved).
- **Mech non-regression:** flag OFF = byte-identical; flag ON = mechs unaffected (separate
  type keys). tier1 5/5 +0 destroys GL-clean both states.
- **Gates:** `scripts/check-contracts.sh` 8/8 (register `MC2_VEHICLE_GPU_BATCH`).

---

## 6. Scope boundary
**This slice:** Task 0 measurement gate → batcher type-key generalization → vehicle
register + submit + CPU-fallback skip → sensor/fade `suppressGpu` → kill-switch + parity
counter. **Follow-ons (separate slices):** vehicle objectID-buffer pick; move vehicle
dynamic shadows to batcher `flushShadow()`; flip default-ON after soak.

---

## 7. Resolved review decisions (2026-06-03)
1. **Type key:** opaque handle/integer key — NO new shared `AppearanceType` base; mech path
   byte-identical (§3.1, patch 3).
2. **Fade:** CPU fallback while fading; no SSBO/shader alpha this slice (§4, patch 2).
3. **Task 0 gate:** PROCEED if portable cost ≥60% of vehicle appearance cost OR absolute
   savings ≥0.15ms (vehicle-heavy mission); STOP if portable <0.10ms AND recalcBounds
   dominates (§2, patch 1).
4. **Naming:** no broad `GpuMechBatcher` rename this slice — add vehicle support internally;
   rename later if desired (§3.1, patch 4).
5. **Animation:** verify `gvAnimData` is node-transform-only; vertex-deform frames take CPU
   fallback via `suppressGpu` (§4 risk + Task 5, patch 5).
6. **Transform seam:** keep node-matrix production; skip only per-vertex transform + MLR
   render — exact set per Task 0's seam finding, mirroring `mech3d` (§3.3, patch 6).
