# Static-Prop Cluster-LOD Scaffolding — Design (substitutive)

Date: 2026-05-19
Status: design, pre-plan (revised after steelman + adversarial + greybeard)
Scope: buildings only; substitutive (single path, legacy ladder deleted)

## Problem

All MC2 native assets are very low-poly. The goal is to make high-detail
assets the norm and to build the scaffolding now so the architecture is
proven before authored high-detail art exists. The earlier additive sidecar
design was rejected by greybeard: an opt-in `.cdag` path that leaves the
legacy discrete-LOD registration alive entrenches a path the real fix must
later unwind (the documented ~0ms additive trap, dual-queue retirement debt).
This design is substitutive: one building geometry path, the dead discrete
ladder deleted in the same slice.

## Core idea (resolves earlier CRITICAL-1)

Every building is represented as a cluster set, selected per-view by GPU
compute, drawn through the existing indirect static-prop path:

- Stock building: its existing full-res `TG_TypeMultiShape` (parsed today via
  `LoadTGMultiShapeFromASE`, mclib/msl.cpp:425) is wrapped IN-MEMORY at load
  as a single degenerate cluster (a 1-cut DAG). No offline cook, no `.cdag`
  file, no new reachability path. Visual result is IDENTITY (same triangles),
  so `stock_install_must_remain_playable` holds by construction, not by a
  screen-space-error tolerance.
- High-detail building: an offline-cooked `<name>.cdag` sidecar supplies the
  multi-level cluster-DAG. `.cdag` exists ONLY as the high-detail extension.

The discrete `currentLOD` / `bldgShape[i]` ladder is deleted for buildings in
the same slice (force-pin sites bdactor.cpp:658/:869/:2787/:2854/:3318;
documented TEMP pin :1226/:1230). Deleting it also retires the LOD-1
invisibility bug class (no discrete ladder ⇒ no LOD-1 to vanish).

## What this proves (and explicitly does not)

Proves: every building (stock as 1-cut, one high-detail asset as N-cut)
renders correctly through a single GPU per-cluster cut-selection +
indirect-draw path, with stock identity-preserved and the legacy ladder
deleted, validated at zoomed-out-big-map.

Does NOT claim any performance win. Capped FPS is an invalid CPU-cost A/B
signal here; per-cluster chrono is observer-effect-dominated (14 terrain
slices falsified that way). Deliverable claim: representation correct (stock
identity, high-detail equivalent at coarse cut, finer cuts available), single
substitutive path, ladder deleted.

## Architecture (units, independently testable)

### 1. In-memory 1-cut wrapper (stock path)

At building registration, wrap the parsed `TG_TypeMultiShape` as a single
cluster. No dependency on meshoptimizer, no file. This is the path every
stock building takes; it is the substitutive replacement for discrete
`bldgShape[i]` registration.

### 2. Offline cooker + `.cdag` format (high-detail extension only)

A standalone build-time tool (NOT the lazy ASE→TGL bake — see Rejected
below) consumes a high-poly source and emits `<name>.cdag`: meshoptimizer
`buildMeshlets` (~128-tri clusters) + 2–3 simplification levels with locked
cluster-border edges; per-cluster bounding sphere + monotonic parent
screen-space error. Bounded max vertex count for any cut
(`tgl_pool_exhaustion_is_silent`: pool exhausts silently at 500K; a
high-poly source is exactly the input that blows it). `.cdag` is a
standalone versioned blob (own magic+version); `CURRENT_SHAPE_VERSION`
(`0xBAFDECAF`, msl.cpp:74) is NOT touched. `.cdag` is a render cache:
regenerable, no savegame depends on it.

`.cdag` freshness is keyed on the high-poly SOURCE hash + `.cdag` format
version, independent of `.ase`/`.tgl` mtimes (the earlier "reuse the
freshness table" idea was broken: that table compares `.tgl`↔`.ase` and the
emit site msl.cpp:943 is unreachable on a stock install).

### 3. Cluster-select compute (NET-NEW, not reuse)

A new per-cluster compute dispatch. Prior art is `gpu_cull_patch.comp`
(NOT `gpu_driven_cmd_patch.comp`, which is the water `DrawArrays` path) —
related but insufficient: it patches only `instanceCount`/`baseInstance`;
`count`/`firstIndex`/`baseVertex` are fixed at mission load by
`compute_buildIndirectBuffer` (GameOS/gameos/gpu_cull_compute.cpp), and the
`glMultiDrawElementsIndirect` drawcount
(gos_static_prop_batcher.cpp:3317/:3339) is a mission-load constant. Per-view
cut selection requires writing per-surviving-cluster
`count`/`firstIndex`/`baseVertex` AND a per-frame-variable drawcount. This is
acknowledged net-new GPU-driven command-generation architecture and is the
core POC work. Cut rule: keep cluster iff
`error(self) > threshold >= error(parent)`. Distance ⇒ coarser cut, never
reject (`distant_buildings_render_at_lower_lod_never_distance_culled`). No
`glGetBufferSubData` after the SSBO copy (the mc2_10 135→62fps stall).
Per-cluster command-slot count is bounded explicitly (stated in plan); the
per-frame drawcount source is the compute-emitted survivor count, replacing
the mission-load constant.

### 4. Draw integration

One building geometry path. Geometry registered as ONE `TG_TypeShape` into a
PRIVATE VAO + private IBO. Rationale (corrected): NOT a hard "registry cap" —
the real constraint is per-type instance-cap dilution
(gos_static_prop_batcher.cpp:1519-1523, documented overflow `type=280
count=259 cap=256` in mc2_10). Private VAO is mandatory: shared-VAO element
binding is a LIVE defect — `flushShadow()` is DEFAULT-OFF pending exactly
this dedicated-VAO redesign (gos_static_prop_batcher.cpp:3660/:3681, gate
`MC2_SHADOW_ENABLE` :3688). Submitted via `glMultiDrawElementsIndirect` on
the private VAO using `GL_ARB_shader_draw_parameters` (:472/:497). Hooked
INSIDE `renderLists()` at the existing static-prop flush site
(mclib/txmmgr.cpp:2101, after `compute_dispatch()` :2098) — NOT a
post-renderLists gamecam hook; that rule is for mcTextureManager-bypassing
GPU-direct renderers, which this is not. Depth-state inherited from the
txmmgr.cpp:2101 site. Texture-handle table contract: clusters carry no
independent material handles; all remap to the shape's slot set (the
registerType texture-handle remap path must be traced in the plan, since the
private-VAO path may bypass packet enumeration).

## Data flow

Build time (high-detail only): high-poly source → cooker → `<name>.cdag`.
Load: stock building → in-memory 1-cut wrap; high-detail building → `.cdag`
clusters. Both register into the private VAO + cluster SSBO. Per frame:
cluster-select compute writes per-surviving-cluster indirect commands +
survivor drawcount → `glMultiDrawElementsIndirect` on the private VAO inside
`renderLists()`.

## "Done" gate (cooker-independent oracle)

1. Stock identity: every stock building's 1-cut path renders the same
   triangles as the pre-change full-res registration. Identity check, not a
   tolerance — asserted across all tier1 missions AT zoomed-out-big-map (the
   structurally blind stress path that hid 3 prior regressions).
2. High-detail equivalence: rasterize the high-poly source full-res and the
   `.cdag` coarsest cut from a FIXED set of N camera poses at fixed
   resolution; compute a concrete silhouette metric (XOR coverage ratio,
   stated numeric threshold) with code that does NOT reuse the cooker's
   screen-space-error function (the earlier gate was circular: cooker judged
   cooker). Zero threshold violations.
3. Ladder deleted: the discrete `currentLOD`/`bldgShape[i]` building path no
   longer exists in the source tree (grep proves absence), not merely
   bypassed.
4. Cut-selection measured only by one coarse per-frame zone or an N-frame
   work-count print — never per-cluster chrono, never FPS.

## Greybeard: meta-fix discharged in-slice (not filed as debt)

This design IS the meta-fix: single building geometry path, discrete ladder
deleted, LOD-1 invisibility bug class retired. It is substitutive — there is
no second path kept alive. Remaining narrowly-scoped debt (NOT this slice):
trees carry the analogous dead `currentLOD` pin (bdactor.cpp:3564/:4457/:4511,
TEMP doc :3990/:3994); the tree path is the direct follow-on applying the
same single-path treatment. Concrete trigger for the tree follow-on: this
building slice ships and its "Done" gate passes on all tier1 at
zoomed-out-big-map.

## Hard dependencies (non-negotiable)

- Build-system: meshoptimizer is NOT currently a dependency; no vcpkg
  manifest exists; CMake is documented-fragile. Vendoring strategy + linked
  target + full-relink impact must be resolved with the build-system advisor
  BEFORE plan-writing.
- Private VAO/IBO/cluster SSBO with explicit per-mission lifecycle
  (init/teardown/mid-mission), mirroring `s_sharedVao` teardown at
  gos_static_prop_batcher.cpp:993; state whether `.cdag`-derived geometry is
  process-lifetime or per-mission.
- Net-new per-frame variable-command generation (Unit 3) is core scope, not
  reuse.
- Stock path is identity-preserving and meshoptimizer-free.
- Hook inside `renderLists()` at txmmgr.cpp:2101.
- "Done" gate run at zoomed-out-big-map, every tier1 mission.
- Landing rule: the cooker/format, the stock 1-cut path, the compute, the
  draw, and the gate land in ONE arc. A partial land is now CATASTROPHIC
  (not silently green): with the ladder deleted, a missing draw path means
  every building vanishes. No half-landing.

## Rejected alternatives

- Additive opt-in `.cdag` sidecar leaving the legacy ladder alive: greybeard
  rejected — entrenches the path the real fix must unwind (~0ms additive
  trap).
- Hooking the cooker into the lazy ASE→TGL bake (msl.cpp:943): unreachable on
  stock installs (`SaveBinaryCopy` only runs when `.ase` newer than `.tgl`).
- "Reuse the indirect plumbing verbatim": false; per-cluster command
  generation is net-new (see Unit 3).
- Fixed-cut-only proof: does not scaffold the stated "reduce in real time"
  goal.

## Out of scope (YAGNI)

Mechs / skinned geometry; trees (filed follow-on); full recursive crack-free
DAG; virtual geometry streaming; compute software rasterizer; any default-on
perf flip; any perf claim.
