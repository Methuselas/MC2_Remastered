# Static-Prop Cluster-LOD Scaffolding — Design (substitutive)

Date: 2026-05-19
Status: design, pre-plan (revised: steelman + adversarial + greybeard +
build-system advisor + adjacent-capability research)
Scope: buildings only; substitutive (single path, legacy ladder deleted)

## Problem

All MC2 native assets are very low-poly. The goal is to make high-detail
assets the norm and to build the scaffolding now so the architecture is
proven before authored high-detail art exists. The earlier additive sidecar
design was rejected by greybeard (entrenches a path the real fix must unwind
-- the documented ~0ms additive trap). This design is substitutive: one
building geometry path, the dead discrete ladder deleted in the same slice.

## Core idea

Every building is a cluster set, selected per-view by GPU compute, drawn
through the existing indirect static-prop path:

- Stock building: its existing full-res `TG_TypeMultiShape` (parsed today via
  `LoadTGMultiShapeFromASE`, mclib/msl.cpp:425) is wrapped IN-MEMORY at load
  as a single degenerate cluster (1-cut DAG). No offline cook, no `.cdag`
  file. Visual result is IDENTITY (same triangles), so
  `stock_install_must_remain_playable` holds by construction, not by a
  tolerance.
- High-detail building: an offline-cooked `<name>.cdag` sidecar supplies the
  multi-level cluster-DAG. `.cdag` exists ONLY as the high-detail extension.

The discrete `currentLOD`/`bldgShape[i]` building ladder is deleted in the
same slice (force-pin sites bdactor.cpp:658/:869/:2787/:2854/:3318; TEMP pin
:1226/:1230). Deleting it retires the LOD-1 invisibility bug class.

## Phase 0 (PREREQUISITE): merge the Assimp import path

The high-poly SOURCE importer does NOT exist on this branch. The modern
Assimp glB/FBX path -- `TG_TypeMultiShape::LoadFromFile` probing
`kImportExts[]={".glb",".fbx"}` and calling `ImportGeometryFromFile`
(mclib/msl.cpp:464-493 on the `assimp-testing` branch; `assimp_importer.cpp`
there) -- is grep-confirmed ABSENT on `nifty-mendeleev`. The stock path
(`LoadTGMultiShapeFromASE`, msl.cpp:425) is present; the high-poly source
path is not.

Decision (user): merge the `assimp-testing` import path into
`nifty-mendeleev` as Phase 0 -- Assimp must be merged eventually regardless
(mech import depends on it). Cooker source ingestion then reuses
`ImportGeometryFromFile`, NOT a new importer.

Phase 0 is gated, not assumed safe. "Shouldn't interfere with much" is a
hypothesis to falsify, not a given:

- Blast-radius scope is a Phase-0 task: enumerate every file the
  `assimp-testing` import path touches in shared loaders (`msl.cpp`,
  `tgl.*`, `mech3d.*`), and every behavioral change to the EXISTING stock
  ASE/TGL path (the import probe sits in the same `LoadFromFile`/cache
  region -- regressing stock load is the primary risk).
- Assimp is a HEAVY third-party dependency (unlike meshoptimizer): its
  vendoring/build wiring rides along with the `assimp-testing` merge. Its
  CMake integration on that branch must be inventoried and reconciled with
  this branch's build (see Build integration).
- Phase-0 exit gate: full tier1 smoke at zoomed-out-big-map green with the
  merged import path present but unused by buildings (stock ASE/TGL load
  path behaviorally unchanged -- identity), BEFORE any cooker/cluster work
  builds on it. A partial or regressing merge blocks the slice.

## Build integration (corrected -- there is no vcpkg)

The adversarial premise of a vcpkg manifest was wrong: there is no
`vcpkg.json` anywhere; deps are `find_package` against a prefix-path SDK
bundle plus committed vendored trees under `3rdparty/` (the Tracy
precedent: `3rdparty/tracy/`, single `.cpp` compiled into a target,
headers via `THIRDPARTY_INCLUDE_DIRS`).

- meshoptimizer: vendor source under `3rdparty/meshoptimizer/` (Tracy
  clone -- small, no-deps, MIT). Compile its `*.cpp` directly into a NEW
  separate executable target `cdag_cooker` added in
  `data_tools/CMakeLists.txt` next to `aseconv`/`makefst`/`makersp`
  (`DISABLE_GAMEOS_MAIN`, links `mclib stuff` like `aseconv`). The cooker
  is a build-time HOST tool, never linked into `mc2.exe` -- this avoids the
  mandatory-full-relink hazard entirely (no engine TU touched). Build:
  reconfigure (`cmake -B build64`), `--target cdag_cooker --config
  RelWithDebInfo`; verify `mc2` reports up-to-date (relink-hazard avoided).
- `-DLINUX_BUILD` is global and inherited by `data_tools` tools; the cooker
  uses `PATH_SEPARATOR`/forward slashes, no `_WIN32` branches (silent-crash
  trap `mc2_path_separator_linux_build.md`).
- Assimp build wiring: heavier, NOT a Tracy clone; arrives via the Phase-0
  merge and must be reconciled there (its `assimp-testing` CMake
  integration inventoried against this branch).

## Cooker core (meshoptimizer -- exact API)

Borrow the structure of meshoptimizer's own `demo/nanite.cpp` +
`demo/clusterlod.h` (canonical reference for this exact cooker).

- `meshopt_buildMeshlets(...,max_vertices,max_triangles,cone_weight)` +
  `meshopt_buildMeshletsBound(...)` for alloc -> ~128-tri clusters.
- `meshopt_simplifyWithAttributes(..., vertex_lock, target_index_count,
  target_error, options, &result_error)`. Crack-free internal group seams
  require the PER-VERTEX `vertex_lock` array
  (`meshopt_SimplifyVertex_Lock` on every cluster-group boundary vertex) --
  the coarse `meshopt_SimplifyLockBorder` *option flag* locks only the
  global topological border and is INSUFFICIENT for the DAG case.
- `result_error` computed with `meshopt_SimplifyErrorAbsolute` IS the
  monotonic per-cluster parent error feeding the cut rule. Do not invent a
  separate metric.
- `meshopt_computeMeshletBounds(...)` returns per-cluster bounding sphere
  AND backface cone (apex/axis/cutoff) in one call -- both consumed by the
  cut-select compute.

## `.cdag` format

Standalone versioned blob (own magic+version); `CURRENT_SHAPE_VERSION`
(`0xBAFDECAF`, msl.cpp:74) NOT touched. A render cache: regenerable, no
savegame depends on it. Freshness keyed on high-poly SOURCE hash + `.cdag`
format version (independent of `.ase`/`.tgl` mtimes; the lazy-bake emit at
msl.cpp:943 is unreachable on a stock install).

Bake NOW (adding any later forces a format-version bump + global re-cook of
all high-detail assets):

- per-cluster bounding sphere + backface cone (`computeMeshletBounds`)
- per-cluster absolute `result_error`
- per-cluster vertex-cache-optimized index order
  (`meshopt_optimizeVertexCache`)
- vertex-fetch-optimized vertex layout (`meshopt_optimizeVertexFetch` --
  changes layout, so deferring = a format change)

Defer but RESERVE a header flag (so adding later is NOT a version bump):
vertex/index compression (`meshopt_encodeVertex/IndexBuffer`), overdraw
reorder (`meshopt_optimizeOverdraw`).

## Cluster-select compute (NET-NEW, not reuse)

New per-cluster compute dispatch. Prior art is `gpu_cull_patch.comp` (NOT
`gpu_driven_cmd_patch.comp`, the water `DrawArrays` path) -- related but
insufficient: it patches only `instanceCount`/`baseInstance`;
`count`/`firstIndex`/`baseVertex` are fixed at mission load by
`compute_buildIndirectBuffer` (gpu_cull_compute.cpp), and the
`glMultiDrawElementsIndirect` drawcount (gos_static_prop_batcher.cpp:3317/
:3339) is a mission-load constant. Per-view cut selection writes
per-surviving-cluster `count`/`firstIndex`/`baseVertex` AND a
per-frame-variable drawcount -- acknowledged net-new GPU-driven
command-generation, the core POC work. Cut rule:
`keep iff error(self) > threshold >= error(parent)`; backface-cone reject;
distance => coarser cut, never reject
(`distant_buildings_render_at_lower_lod_never_distance_culled`). No
`glGetBufferSubData` after the SSBO copy (the mc2_10 135->62fps stall).

REUSE (scaffolding, not the kernel): the per-mission SSBO
free-prev/realloc lifecycle, substrate-staging-SSBO sizing, and the
`GL_COMMAND_BARRIER_BIT` sequencing already in `gpu_cull_compute.cpp`
(`compute_buildIndirectBuffer`, `compute_dispatch`, the command-barrier in
`GpuStaticPropBatcher::flush`). A new kernel, not new GPU plumbing or
barrier discipline.

## Draw integration

One building geometry path. Geometry registered as ONE `TG_TypeShape` into
a PRIVATE VAO + private IBO. Rationale (corrected): NOT a hard "registry
cap" -- the real constraint is per-type instance-cap dilution
(gos_static_prop_batcher.cpp:1519-1523, documented overflow `type=280
count=259 cap=256` mc2_10). Private VAO is mandatory: shared-VAO element
binding is a LIVE defect -- `flushShadow()` is DEFAULT-OFF pending exactly
this dedicated-VAO redesign (gos_static_prop_batcher.cpp:3660/:3681, gate
`MC2_SHADOW_ENABLE` :3688). Submitted via `glMultiDrawElementsIndirect` on
the private VAO using `GL_ARB_shader_draw_parameters` (:472/:497). Hooked
INSIDE `renderLists()` at the existing static-prop flush site
(mclib/txmmgr.cpp:2101, after `compute_dispatch()` :2098) -- depth-state
inherited there; NOT a post-renderLists gamecam hook (that rule is for
mcTextureManager-bypassing GPU-direct renderers; this is not one).
Texture-handle table contract: clusters carry no independent material
handles; all remap to the shape's slot set (registerType remap path traced
in the plan, since the private-VAO path may bypass packet enumeration).
High-detail TEXTURE side is already solved by the asset-scale/upscaler
infra (loose-file `_4x_gpu/` resolution + scale-aware blit, spec
2026-04-23) -- the `.cdag` carries no material handles, the texture path
upscales transparently.

## Data flow

Phase 0: merged Assimp path present, stock load identity-verified. Build
time (high-detail only): glB/FBX source -> `ImportGeometryFromFile` ->
`TG_TypeMultiShape` -> `cdag_cooker` (meshoptimizer) -> `<name>.cdag`.
Load: stock building -> in-memory 1-cut wrap; high-detail -> `.cdag`
clusters. Both register into the private VAO + cluster SSBO. Per frame:
cluster-select compute writes per-surviving-cluster indirect commands +
survivor drawcount -> `glMultiDrawElementsIndirect` on the private VAO
inside `renderLists()`.

## "Done" gate (cooker-independent oracle)

1. Phase-0 gate passed (stock ASE/TGL load identity; tier1 zoomed-out
   green with Assimp merged-but-unused).
2. Stock identity: every stock building's 1-cut path renders the same
   triangles as the pre-change full-res registration. Identity check (not
   a tolerance), all tier1 missions AT zoomed-out-big-map.
3. High-detail equivalence: rasterize the high-poly source full-res and the
   `.cdag` coarsest cut from a FIXED set of N camera poses at fixed
   resolution; concrete silhouette metric (XOR coverage ratio, stated
   numeric threshold) computed by code that does NOT reuse the cooker's
   error function (the earlier gate was circular). Zero violations.
4. Ladder deleted: discrete `currentLOD`/`bldgShape[i]` building path
   absent from the source tree (grep proves absence), not bypassed.
5. Cut-selection measured by ONE coarse per-frame zone or an N-frame
   work-count print -- never per-cluster chrono, never FPS.

## Greybeard: meta-fix discharged in-slice (not filed as debt)

This design IS the meta-fix: single building geometry path, discrete ladder
deleted, LOD-1 invisibility bug class retired. Substitutive -- no second
path kept alive. Remaining narrowly-scoped debt (NOT this slice): trees
carry the analogous dead `currentLOD` pin (bdactor.cpp:3564/:4457/:4511,
TEMP doc :3990/:3994); the tree path is the direct follow-on. Concrete
trigger for the tree follow-on: this building slice ships and its "Done"
gate passes on all tier1 at zoomed-out-big-map.

## Hard dependencies (non-negotiable)

- Phase 0 (Assimp merge) lands and its exit gate passes BEFORE cooker/
  cluster work builds on it. Blast radius scoped, not assumed.
- meshoptimizer vendored Tracy-style into a separate `cdag_cooker` host
  target (no mc2.exe relink); Assimp build wiring reconciled in Phase 0.
- Net-new per-frame variable-command generation (cluster-select compute) is
  core scope, not reuse.
- Stock path is identity-preserving and meshoptimizer-free.
- Private VAO/IBO/cluster SSBO with explicit per-mission lifecycle
  (init/teardown/mid-mission), mirroring `s_sharedVao` teardown at
  gos_static_prop_batcher.cpp:993; state process-lifetime vs per-mission
  for `.cdag`-derived geometry.
- Hook inside `renderLists()` at txmmgr.cpp:2101.
- "Done" gate run at zoomed-out-big-map, every tier1 mission.
- Landing rule: cooker/format, stock 1-cut path, compute, draw, and gate
  land in ONE arc (Phase 0 may land separately as the gated prerequisite).
  With the ladder deleted, a missing draw path means every building
  vanishes -- a partial land is CATASTROPHIC, not silently green.

## Rejected alternatives

- Additive opt-in `.cdag` sidecar leaving the legacy ladder alive:
  greybeard rejected (entrenches the path the real fix must unwind).
- Hooking the cooker into the lazy ASE->TGL bake (msl.cpp:943): unreachable
  on stock installs.
- "Reuse the indirect plumbing verbatim": false; per-cluster command
  generation is net-new.
- Fixed-cut-only proof: does not scaffold the stated "reduce in real time"
  goal.
- vcpkg/`find_package(meshoptimizer)`: no vcpkg in this build; would add an
  unused dependency-resolution mechanism. Use the Tracy vendored pattern.
- New bespoke glB/FBX loader in the cooker: rejected -- Assimp must merge
  eventually for mechs anyway; reuse `ImportGeometryFromFile`.

## Out of scope (YAGNI)

Mechs / skinned geometry; trees (filed follow-on); full recursive
crack-free DAG beyond 2-3 levels; virtual geometry streaming; compute
software rasterizer; vertex/index compression + overdraw (deferred,
header-flag reserved); any default-on perf flip; any perf claim.

## External prior art

- meshoptimizer `demo/nanite.cpp` + `demo/clusterlod.h` -- borrow the
  cooker structure wholesale (canonical correct use of the API above).
- `Scthe/nanite-webgpu` -- compute-driven per-cluster cut-select + indirect
  draw with NO mesh shaders / no int64 (closest constraint match to GL 4.3
  + AMD); borrow the Unit-cluster-select flow.
- `nvpro-samples/nv_cluster_lod_builder` -- crack-free monotonic-error
  group invariants only (cook-side); its Vulkan runtime is out of scope.
