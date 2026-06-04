# Vehicle GPU-Instancing Port — Surface Recon (2026-06-03)

Read-only recon for `VEHICLE-GPU-BATCH-1`: port ground-vehicle draw from the legacy
CPU MLR transform (`GVAppearance`) to the existing GPU **mech** batcher.

## Verdict
**Reuse `GpuMechBatcher`** (extend its type key to accept `GVAppearanceType`); do NOT
build a sibling. Its bone model is already *rigid one-bone-per-`SHAPE_NODE`, sourced
from `shapeToWorld`* — exactly the vehicle case (body + 1 turret joint). Vehicles are
strictly simpler than mechs. **One slice, not an arc.** Vehicles were a deliberate
scope deferral from the mech offload (`docs/superpowers/explorations/2026-05-03-mech-offload-recon.md:5-7`
explicitly "NOT in scope: vehicles (GVAppearance)"), no technical blocker recorded.

## What the port replaces (ONLY this)
- `gvShape->TransformMultiShape(&xlatPosition,&totalRotation)` — full per-vertex CPU
  transform, `mclib/gvactor.cpp:2542` (in `updateGeometry`).
- `gvShape->Render(true)` — CPU MLR submit, `gvactor.cpp:2173` (in `render`).

Mirror the mech path: `mclib/mech3d.cpp:2557-2708` populates per-node `shapeToWorld`
(cheap), calls `batcher submitActor()`, and **skips `mechShape->Render(true)`** when
GPU submit succeeds (`mech3d.cpp:2701-2708`).

## Stays on CPU (unchanged by the port)
- `recalcBounds()` (`gvactor.cpp:1628`): CPU screen projection + 8-corner OBB screen
  rect + `inView`/visibility gates + LOD select + `screenPos` for HUD. **This is a
  separate cost from the vertex transform** — the port does NOT remove it. (Sizing
  caveat: confirm the transform is the dominant fraction of the ~30–44µs
  `Vehicles AppearanceUpdate` bucket before committing — see spec Task 0.)
- Picking: `isMouseOver` (screen-rect) + `PerPolySelect` (`gvactor.cpp:1608/3170`).
- HUD bars / select brackets / text (`gvactor.cpp:2185-2210`).
- Sensor blips + fade (below).

## Mech batcher reuse surface
- `GameOS/gameos/gos_mech_batcher.h`: `GpuMechSubmitDesc` (`:107-139`) — reads only
  `mechShape->shapeToWorld` per node + tex handle + colors + flags + objectId.
  `GpuMechInstance` 64B SSBO (`:35-70`), `GpuMechBone` rows copied from
  `listOfShapes[i].shapeToWorld` (`gos_mech_batcher.cpp:1354-1364`). Rigid, no skin
  blend (`:1060-1064`, "Slice A"). `registerTypeLod` requires `<=255` nodes (`:950`).
- Type key today is `Mech3DAppearanceType*` / `TG_TypeShape*` but everything READ is
  generic `TG_TypeMultiShape`/`TG_MultiShape`. **Generalize the key** so
  `GVAppearanceType::gvShape[lod]` (`TG_TypeMultiShapePtr`, `gvactor.h:53`) registers
  the same way. No new SSBO fields needed (`materialIdx`/`objectIdRaw`/`fogARGB`/
  `highlightARGB` slots already exist).

## Risks / seams
1. **Sensor blips (MEDIUM):** when `sensorLevel>0` the render path draws a small
   `sensorTriangleShape`/`sensorCircleShape` *in place of* the mesh
   (`gvactor.cpp:2371-2383`). GPU submit must be **suppressed** for `sensorLevel>0`;
   blips stay on the CPU MLR path (tiny vertex count).
2. **Fade (`alphaValue`, MEDIUM):** maps to per-instance alpha; no SSBO slot today —
   small add or pack into an existing field. While fading, keep CPU path or carry alpha.
3. **`gvAnimData` keyframe anim (verify):** repair/refit gestures advance
   `currentFrame`/`SetFrameNum` (`gvactor.cpp:2880-2929`). If these drive NODE matrices
   → free (flow through `shapeToWorld`). If any DEFORM vertices → those frames need CPU
   fallback. **Must verify before relying on GPU draw for animated vehicles.**
4. **Per-actor LOD swap:** `recalcBounds` deletes/recreates `gvShape` on LOD change
   (`gvactor.cpp:1855+`) — ensure re-registration / type-key stability (mechs solved
   this per-LOD).
5. **objectID pick + moving shadows (OUT of slice):** vehicles don't emit objectID
   today (slot exists, new capability) and their dynamic shadows are half-wired
   (static-prop registration, `gvactor.cpp:1102-1106`; `renderShadows` early-returns
   under tessellation `:2110`). Proper home is the batcher `flushShadow()`. Follow-ons.

## CPU/GPU-cull already wired for vehicles
`actorHandle_` (`gvactor.h:211`), `readback_isActorVisibleLagged` gates in
render/update/renderShadows (`gvactor.cpp:2116/2138/2940`) — vehicles already ride the
same GPU-cull readback infra mechs use. Good.

## Essential files
- `mclib/gvactor.{h,cpp}` — GVAppearance(Type); recalcBounds 1628, render 2132,
  updateGeometry/TransformMultiShape 2389/2542, render-shadow 2107, static-prop shadow
  reg 1102, picking 1608/3170.
- `code/gvehicl.cpp` — appearance alloc 964, updateAnimations 3004, render-call +
  sensor-quality switch 3710/3915-3967.
- `GameOS/gameos/gos_mech_batcher.{h,cpp}` — reuse target.
- `mclib/mech3d.cpp:2557-2708` — reference CPU submit + fallback skip.
- `docs/superpowers/explorations/2026-05-03-mech-offload-recon.md` — the arc this extends.
