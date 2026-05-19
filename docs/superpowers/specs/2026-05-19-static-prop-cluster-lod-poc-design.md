# Static-Prop Cluster-LOD Scaffolding (POC) — Design

Date: 2026-05-19
Status: design, pre-plan
Scope: proof-of-concept, static props / buildings only

## Problem

All MC2 native assets are very low-poly (hundreds–2000 tris). The goal is to
make high-detail assets the norm. This POC builds the scaffolding now — an
offline cook to a continuous-LOD cluster representation consumed by the
already-shipped GPU-driven indirect static-prop path — so the future
substitutive slice has a proven, correct foundation rather than discovering
the architecture cannot carry high-detail assets after the fact.

## What this POC proves (and explicitly does not)

Proves: one high-poly building round-trips offline-cook -> shallow
cluster-DAG sidecar -> GPU per-cluster cut selection -> existing indirect
static-prop draw, rendering correctly at a runtime-chosen cut, validated at
zoomed-out-big-map.

Does NOT claim: any performance win. Capped FPS is an invalid CPU-cost A/B
signal here; per-cluster chrono is observer-effect-dominated (this codebase
falsified 14 terrain slices that way). The cooked path is structurally
additive for the POC because stock assets must keep the legacy full-res path
(`stock_install_must_remain_playable`). Deliverable claim is exactly:
representation equivalent at coarse cut, finer cuts available, dispatch path
proven.

## Architecture (5 independently-testable units)

### 1. Cooker (in-engine, sidecar)

Extend the existing lazy `.ase` -> `.tgl` bake in
`TG_TypeMultiShape::LoadTGMultiShapeFromASE` (mclib/msl.cpp:425). When a
high-poly source and the opt-in env flag are present, after the existing
`SaveBinaryCopy` (msl.cpp:943) also emit a `<name>.cdag` sidecar keyed by the
same `tglPath/name` stem, reusing the existing freshness-table logic for
staleness. `CURRENT_SHAPE_VERSION` (`0xBAFDECAF`, msl.cpp:74) is NOT touched.

- Input: high-poly source mesh. Output: `.cdag` blob. Depends on: meshoptimizer.

### 2. DAG builder (inside cooker)

meshoptimizer `buildMeshlets` (~128-tri clusters) + 2–3 simplification levels
with locked cluster-border edges; per-cluster bounding sphere + monotonic
parent screen-space error. Bounded maximum vertex count for any cut (TGL pool
exhausts silently at 500K — `tgl_pool_exhaustion_is_silent`; a high-poly
source is exactly the input that blows it).

### 3. `.cdag` format

Standalone versioned sidecar (own magic + version int), independent of
`CURRENT_SHAPE_VERSION`. Absent ⇒ loader untouched ⇒ stock behavior. The
stock-playable rule is satisfied by construction: stock assets ship no
`.cdag`; `BldgAppearanceType::init` still produces a valid `bldgShape` from
`.ase`/`.tgl` (bdactor.cpp:202 / single-LOD fallback :220).

### 4. Cluster-select compute

New per-cluster dispatch (distinct work-item domain from the per-instance
`gpu_cull.comp`), reusing the `gpu_cull` -> `gpu_driven_cmd_patch` plumbing
verbatim: SSBO staging-copy pattern, baseInstance prefix-sum compaction,
20-byte indirect command layout. Cut rule: keep cluster iff
`error(self) > threshold >= error(parent)`. Distance ⇒ coarser cut, never
reject (long-sightline rule, `distant_buildings_render_at_lower_lod_never_distance_culled`).
No `glGetBufferSubData` readback after the copy (the documented mc2_10
135->62fps sync stall).

### 5. Draw integration

Cooked types register as ONE `TG_TypeShape` (not per-cluster shapes — the
registry cap) into a PRIVATE VAO + private IBO. Shared-VAO element binding is
a known LIVE defect: `GpuStaticPropBatcher::flushShadow()` is DEFAULT-OFF
pending exactly this dedicated-VAO redesign
(gos_static_prop_batcher.cpp:3660 / :3681). The cut's surviving-cluster list
becomes the indirect command's index ranges; submitted via
`glMultiDrawElementsIndirect` (gos_static_prop_batcher.cpp:3317/:3339) on the
private VAO, using `GL_ARB_shader_draw_parameters` (:472/:497). Hooked after
`renderLists()` (enqueuer-vs-submitter). Texture-handle table contract
preserved (clusters remap to the shape's slot set).

## Data flow

`.ase` (+ high-poly source) -> cooker -> `.cdag` beside `.tgl` -> load
registers cooked type into private VAO + cluster SSBO -> per frame:
cluster-select compute writes surviving-cluster indirect args ->
`glMultiDrawElementsIndirect` on private VAO.

## "Done" / equivalence gate

Env-gated probe asserts the DAG coarsest cut reproduces the source mesh's own
full-res silhouette within a fixed screen-space-error threshold; zero
violations across tier1 missions run AT zoomed-out-big-map (the structurally
blind stress path that hid 3 regressions; tier1 default camera is
insufficient). Cut-selection measured only by one coarse per-frame zone or an
N-frame work-count print — never per-cluster chrono, never FPS.

Note on baseline: building `currentLOD` is dead-pinned to 0 (the LOD-1
invisibility bug; force-pin sites bdactor.cpp:658/:869/:2787/:2854/:3318,
documented TEMP pin :1226/:1230). So the gate is NOT a diff against a disabled
discrete ladder — it is a cooker round-trip check against the source mesh's
own full-res. This makes the gate well-defined and independent of the LOD-1
bug.

## Filed debt + retirement trigger (greybeard, mandatory)

Greybeard verdict: justified PATCH. The cooked path is structurally additive
because stock/uncooked assets must keep the legacy full-res registration path.

- Named meta-fix: once cooked high-detail assets are the norm, delete the
  fixed full-res geometry registration for cooked types and make the DAG cut
  the sole detail authority — repointing the three consumers (geometry
  registration, indirect-command builder at the cull-compute command build,
  cull compute).
- Retirement trigger: cooked-asset coverage reaches the "norm" threshold.
- A patch with no named meta-fix is disallowed by greybeard; this section is
  the filed meta-fix and is load-bearing, not prose.

## Input dependency

All current assets are low-poly, so the POC needs one deliberately high-poly
test building. Default: synthesize it (subdivide + displace a stock building
mesh) so the cooker has real density to reduce — no external sourcing.

## Hard dependencies (non-negotiable)

- Private VAO for the cluster IBO (shared VAO is a live `flushShadow` defect).
- Hook after `renderLists()`.
- No readback after the SSBO copy.
- Runtime cut emits a bounded vertex count.
- Register as one `TG_TypeShape`.
- Preserve the texture-handle table contract.
- Equivalence gate run at zoomed-out-big-map, every tier1 mission.

## Out of scope (YAGNI)

Mechs / skinned geometry; full recursive crack-free DAG; virtual geometry
streaming; compute software rasterizer; any default-on flip; any perf claim;
retiring the legacy path.
