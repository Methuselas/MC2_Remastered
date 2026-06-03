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
- One user-driven Tracy capture, vehicle-heavy mission.
- **GATE:** the GPU-portable cost = `Vehicle.UpdateGeometry`(transform part) +
  `Vehicle.Render`. If that is a clear majority of the per-vehicle cost (expected —
  `TransformMultiShape` is per-vertex over the whole mesh ×2), **PROCEED**. If
  `recalcBounds` dominates and the transform is small, **STOP** and write a short note;
  the port would be a big change for a small win.
- This task ships its own commit (observational zones, no behavior change) and stays in
  the tree (useful for the before/after perf proof).

---

## 3. Architecture

### 3.1 Generalize the batcher type key
Today `gos_mech_batcher` keys registration on `Mech3DAppearanceType*` / `TG_TypeShape*`,
but everything it READS is generic `TG_TypeMultiShape`/`TG_MultiShape`. Generalize so a
`GVAppearanceType::gvShape[lod]` (`TG_TypeMultiShapePtr`, `gvactor.h:53`) can register the
same way mechs do (`registerTypeLod` already accepts a `TG_TypeMultiShape*`). Minimal
surface: an opaque type-key (handle/int) instead of the concrete `Mech3DAppearanceType*`,
or a small base accepted by `registerTypeLod`/`submitActor`. Keep mech registration
byte-identical.

### 3.2 Vehicle registration (type-load, mirror mech3d.cpp:381)
At `GVAppearanceType` load, `registerTypeLod(vehicleTypeKey, lod)` for each populated
`gvShape[lod]` — same call mechs use. `finalizeGeometry()` at map-load already covers all
registered types (no vehicle-specific change).

### 3.3 Vehicle submit site (mirror mech3d.cpp:2557-2708)
In the vehicle per-frame path (where `updateGeometry` runs the node transforms), after
populating per-node `shapeToWorld`:
```
IF s_vehicleGpuBatch AND NOT suppressGpu(this):
    fill GpuMechSubmitDesc from gvShape (shapeToWorld nodes, slot0TexHandle=localTextureHandle,
        lightDataIndex, renderFlags, highlightARGB, fogARGB, objectIdRaw=0 for now)
    if batcher.submitActor(desc) succeeded:
        skip gvShape->TransformMultiShape (per-vertex)   // the win
        skip gvShape->Render(true)                       // the win
    else:
        fall through to legacy CPU path (TransformMultiShape + Render)
ELSE:
    legacy CPU path (unchanged)
```
`suppressGpu(this)` = `sensorLevel > 0` (blip path) OR fading (`alphaValue != 0`) OR
(Task 5) any vertex-deforming anim frame. The cheap per-node `shapeToWorld` transform is
still computed on the CPU (needed for the bone SSBO and for `recalcBounds`/picking).

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
| Fade (`alphaValue`) — no per-instance alpha slot | MED | While fading, keep CPU path (`suppressGpu`); OR carry alpha in an existing SSBO field if cheap |
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

## 7. Open questions for review
1. **Type-key generalization shape:** opaque int/handle key vs a thin `AppearanceType`
   base — which keeps mech path most byte-identical with least churn?
2. **Fade handling:** CPU-fallback while fading (simplest) vs add per-instance alpha
   (one SSBO field + shader) — worth it, or are fades rare/brief enough to keep CPU?
3. **Task 0 STOP threshold:** what transform-fraction is the go/no-go line (e.g. ≥60% of
   the per-vehicle bucket)?
