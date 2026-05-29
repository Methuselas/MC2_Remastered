# Engine Convergence Roadmap — 2026-05-22

Status: living reference. Update as items ship or are re-scoped.
Scope: architectural direction for the MC2 → open RTS engine arc.
Not a plan: this is a tally and sequencing guide, not an execution spec.

---

## What this document is

The engine has moved from "add modern rendering features" into
"engine architecture convergence." Per-subsystem GPU data tables,
GPU cull → indirect draw for terrain/props, static-prop indirect,
render contracts/timers, and F1 closing the ViewUniforms gap are
already in flight or done.

This document maps the 20-item "engine-as-API" roadmap against the
codebase state as of 2026-05-22, names gaps, and gives a sequencing
recommendation.

---

## Update log (post-2026-05-22 ships)

**2026-05-29 Infrastructure + Track V visual batch.**

*Infrastructure (Track R substrate):*

- **Engine View Registry** (`61d929c5`) — `RenderCore/EngineView.h` ViewId constants
  (`kMainSceneViewId=1`, `kShadowDirectional0ViewId=2`, `kShadowDynamicViewId=3`) +
  ViewKind enum + `view_uniforms_gl.cpp` `registerOrUpdateView()` / `getViewByIndex()`
  API. First multi-view substrate; advances item 7. `[ENGINE_VIEW_REGISTRY v1]`
  log confirmed; gate-off via `MC2_VIEW_UNIFORMS=0`.
- **Render Resource Registry** (`200e9e3d`, RENDER-RESOURCE-REGISTRY-1) —
  `RenderCore/RenderResourceRegistry.{h,cpp}`. RenderResourceId enum
  (MainColor/MainDepth/ShadowStaticMap/TerrainHeightTexture/MaterialGpuBuffer/ShadowDynamicMap),
  28-byte `RenderResourceDesc` POD, `registerOrUpdateRenderResource()` /
  `getRenderResource()`. Owners retain GL lifetime; registry tracks metadata only.
  Auto-registered: TerrainHeightTexture + ShadowStaticMap. Always-on; JSON dump
  `renderResources[]` array. Advances item 18.
- **Debug View Registry** (`8284dde8`, DEBUG-VIEW-REGISTRY-1) —
  `RenderCore/RenderDebugView.{h,cpp}`. Canonical `RenderDebugView` enum (10 values:
  Final/Albedo/Normal/Roughness/Metallic/LightingOnly/IblOnly/SpecularOnly/MaterialIdx/TexArrayLayer).
  Per-lane support masks: StaticPropOpaque=7 modes, Mech=4 modes, Vfx=2 modes,
  Terrain/Shadow=0 placeholder. ImGui combo filtered to lane-supported views.
  `batcher_setDebugMaterialMode()` free function + static-prop + mech wiring.
  Advances item 16 (DebugRenderer M2+).
- **Contract check aggregator** (`241f22df`) — `scripts/check-contracts.sh`;
  orchestrates 8 grep-based checks (env_registry, material_gpu_mirror,
  visibility_log_schema, include_firewall, no_raw_gl_from_game, vfx_no_objectid,
  destroy_invariant, render_contract_gbuf1) with aligned PASS/FAIL table; `--quiet`
  mode for CI. All 8 PASS as of 2026-05-28. Advances item 18 / item 2.
- **Substrate unit tests** (`46a848dc`) — `tests/unit/test_rendercore.cpp`.
  23 test cases / 2015 assertions via doctest; GL-free. Covers
  RenderResourceRegistry, RenderDebugView, RendererFeatureRegistry (COUNT=26),
  IblShRegistry. Target `rendercore_tests`. Always-on. Advances item 14.
- **Shadow EngineView records** (`d939de67`, SHADOW-VIEW-1) — shadow passes
  registered into engine view registry. **Shadow render resource** (`cf7c6bbd`,
  SHADOW-RESOURCE-1) — shadow map registered into render resource registry.
  **Shadow debug views** (`f4581822`, SHADOW-DEBUG-VIEWS-1) — shadow cascade
  visualizers wired through debug view registry.
- **Per-mission visual tuning profiles** (`a086c259`) — save/restore per-mission
  defaults for tuning ImGui sliders; "Set as Mission Defaults" button.

*Track V visual advances:*

- **SHADOW-TUNING-1** (`53c26d47`) — live `shadowBiasFactor` / `shadowBiasUnits`
  ImGui sliders in Graphics Options; byte-identical defaults (2.0/4.0).
- **SHADOW-DYNAMIC-RESTORE-1** (`7cfc637c`) — fixes VAO element-buffer restore
  order in `flushShadow()` (wrong order zeroed IBO, causing dynamic objects to
  vanish). `MC2_SHADOW_ENABLE` default ON.
- **Flush-order fix — terrain objects with shadows** (`f04e3997`) —
  `GpuStaticPropRegistry::flush()` now runs BEFORE `flushShadow()`'s
  `uploadAllBucketsIfNeeded()`. Month-long root cause: registry flush was AFTER the
  shadow upload locked the per-frame SSBO slot → terrain objects (trees/fences/prop-
  buildings) had zero instances in s_typeRanges → invisible. Closes the
  `SHADOW-ENABLE=1 terrain-objects-invisible` bug class.
- **Mech ViewUniforms consumer** (`64b6d40d`) — opt-in `binding=3` UBO consumer
  for `mech.vert`; `MC2_MECH_VIEWUNIFORMS` default OFF. Prerequisite for
  specular V1.
- **Mech smoothed normals default-ON** (`22a96ffd`) — `MC2_MECH_NORMALS_MODE`
  default flipped to 2 (angle-threshold 60°, hard-edge preservation). Fixes
  ASE-loader corrupted normal averaging. Kill-switch `=0`.
- **Mech ambient lighting** (`74fc057c`, `e0ffffbb`) — gated ambient at 0.15
  default-ON. Lifts flat-shaded shadow areas.
- **Mech PBR specular V1 + glass heuristic** (`8fa7da91`) — Blinn specular sheen
  with dark-pixel cockpit heuristic (luma<0.12 AND maxChannel<0.18). Gate
  `MC2_MECH_SPECULAR_V1` default OFF; all params live-tunable. Requires
  `MC2_MECH_VIEWUNIFORMS=1`.
- **VFX real age sample** (`9f90e167`, VFX-AGE-SAMPLE-1) — GPU-routed particles
  sample spec curves at real CPU-advanced `m_age` instead of fixed 0.5 midpoint.
  Restores fade-in/out, grow/shrink. Gate `MC2_VFX_AGE_SAMPLE` default OFF.
- **Water camera-independent sky tint** (`c098a23f`) — gated sky tint for water;
  default OFF. Track V polish preceding IBL.
- **Mech R→V arc recon + debug views** (`00e9b62a`–`ead1b56c`) — docs/mech-rv-arc-recon.md,
  material inventory exposure, kDebugViewMask_Mech wired to `u_debugMode`.

---

**2026-05-26 Extraction arc v2.1–v3 SHIPPED. HEAD `066b5b9d`.**

Item 3 (world snapshot / extraction phase) advances from NOT STARTED to
"in-progress, snapshot-driven dispatch proven as opt-in gate":

- **Extraction v2.1** (HEAD `4245db18`) — `ExtractedStaticPropPacket` 28-byte
  per-slot struct in `render_snapshot.h`; per-frame snapshot of batcher draw-slot
  state via `batcher_getDrawSlotEntry()`; ok hard gate. Tier1 5/5 PASS,
  `sp_packets=134 ok=1`.
- **Extraction v2.2** (HEAD `76f63b3f`) — dispatch-fact compare:
  `sortedSlot / globalPacketIdx / pipelineId / materialIdx / texArrayLayer` all
  cross-checked snapshot vs live. All structural compare counters zero tier1.
- **Extraction v2.3** (HEAD `88379448`) — snap-cull opt-in
  (`MC2_SNAP_CULL=1`): `flush()` skips prev-frame zero-instance slots from
  snapshot; `spSnapCullSlotMismatch` counter in ok gate; all-zero warmup guard.
  Default + opt-in: tier1 5/5 `ok=1 slot_mismatch=0`.
- **Extraction v3** (HEAD `066b5b9d`, `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` opt-in)
  — snapshot-driven v6 builder: `pipelineId_to_group` map from snapshot rows,
  `v6Packets` + `v6Meta` rebuilt from snapshot without touching live batcher
  state. Compare vs live dispatch; `spBuild*` counters in ok gate. Tier1 5/5
  PASS both gates (default path clean, v3 opt-in collision = 0). This is the
  authority-flip prerequisite — v3 proves snapshot can fully own packet
  construction.

**Also shipped (2026-05-26 session):**
- **RenderWorld::getObjectRecordView()** — handle-direct CPU record snapshot
  (`ObjectRecordView` struct + accessor); enables external inspection of any
  handle's live record without raw pointer access.
- **ImGui Renderer Features window** (`feat(inspector)`) — live `kFeatureTable`
  status panel; shows every registered `MC2_*` feature default + env-override.
- **ImGui Render Explain panel** (`feat(inspector)`) — consolidated render-path
  summary (active gates, subsystem status) in one ImGui window.
- **ResourceLifetime taxonomy doc** (`P2-1 partial`) — four-tier classification
  (Persistent / Mission / Frame / PassTransient) with all seven classified
  resources; see `docs/observations/2026-05-26-resource-lifetime-taxonomy.md`.

---

**2026-05-26 DrawPacket arc + infra ships.** Item 11 advances from
"implicit in enqueuers" to a working v0→v4A substitutive dispatch.
Full details in item 11 below. Other ships:

- **DrawPacket v0→v4A** (commits `2503d1ab`→`f070ac49`) — explicit
  `DrawPacket` struct in `draw_packet_emitter.h`; v4A delivers first
  substitutive dispatch for all-opaque static-prop types in
  non-coalesce mode. Gated `MC2_DRAWPACKET_STATIC_PROP_OPAQUE=1`
  (default OFF). Tier1 5/5 PASS; three-way invariant confirmed
  (`invalid=0`, `duplicate=0`, `partial_type=0`). Coalesce mode (v4B)
  and alpha types remain on legacy path.
- **StaticPropTypeDesc** (commits `b94585c7`→`509b1433`) —
  `RenderCore::StaticPropTypeDesc` 16-byte immutable per-type
  descriptor; `s_typeDescTable` populated in `finalizeGeometry`;
  accessor trio in `batcher.h`.
- **HDRI sky** (`hdri-sky-1` arc, commits `e3a29a86`→`b06dff00`) —
  `SkyRenderAdapter` bridge (RenderWorld firewall), tinyexr EXR loader,
  equirect fragment shader, CC0 4K HDR asset. Tier 2 Track V partial
  start; replaces procedural skybox.
- **FX-GPU B3** (commits `c55f7176`→`7918dbf0`) — B3a MissileSmoke
  additive blend fix (was opaque white squares); B3b PpcBolt kind +
  schema bump + INI-driven gpu_trail_kind mapping (lockstep deploy);
  B3c CPU trailEffect suppression + `MC2_GPU_PARTICLES` default flipped
  ON. GPU particles now ship default-enabled.
- **Block-level AABB frustum fallback** (`f253f3b1`) — static-prop
  pop-in fix; no roadmap item but closes a visual regression.

---

**2026-05-24 mega-update.** The RenderWorld arc went from "design
direction" to "engine surface that exists, with three CI scripts
enforcing its boundary." Per-item status changes are reflected in the
tally below. Headline shifts:

- **Item 1 (engine-as-API stable handles)** — REALIZED for the
  StaticProp + Mech kinds. `RenderObjectHandle` is the engine type.
  `GameAdapters/{StaticProp,Mech}RenderAdapter.{h,cpp}` are the
  bridge surfaces. RenderWorld arc reached STEADY STATE; see
  `docs/renderworld_arc_status.md`.
- **Item 2 (render-pass registry)** — PHASE 2 SHIPPED (commit
  `137dc70`). `MC2_RENDER_CONTRACT_ASSERT=1` activates runtime
  asserts that query live GL state against the declared
  `PassStateContract` + new `RequiredAttachments` +
  `ShaderOutputContract`. Phase 3 (enforced; non-debug) remains
  unscheduled.
- **Item 6 (visibility service)** — V0 SHIPPED in a parallel session;
  emits `[VISIBILITY v1] frame=N static_props=S mechs=M
  terrain=deferred vfx=prohibited` per frame.
- **Item 10 (object-ID buffer)** — FULLY REALIZED. M1.5 substrate +
  M1.6/M2.6 consumers + M3 reservation tripwire + M4 prohibition.
  Three CI scripts enforce the contract (`check-include-firewall.sh`,
  `check-no-raw-gl-from-game.sh`, `check-vfx-no-objectid.sh`).
- **Item 14 (feature flags as registry)** — SHIPPED:
  `RenderCore/RendererFeatureRegistry.h` exists (parallel session;
  untracked at time of writing — surface to the user for tracking).
- **Item 16 (debug engine API)** — DebugRenderer M1 SHIPPED (commits
  `c2e877e` + `96b6b3a`); `flushWorldPrims()` wired after weather in
  `gamecam.cpp`.
- **Item 18 (deterministic render audit logs)** — BROADENED. New
  `[RENDER_WORLD v1] frame=N objects=N static_props=S mechs=M
  visible=0 packets=0 views=1 objectid_buffer=on` banner; new
  `[GAMEPLAY_PICK v1]` unified pickup banner; new
  `[MECHBATCHER v1] event=mech_id_summary|mlr_mech_summary`
  per-mission counter lines; new `[RENDER_CONTRACT v2] assert mode
  ACTIVE` startup line.
- **Item 20 (RenderWorld as main engine API)** — REALIZED for the
  StaticProp + Mech axes. `RenderWorld::registerMech`, `destroyMech`,
  `clearAllMechRecords`, `upsertStaticProp`, `lookupAtPixel`,
  `getMechsAliveCount` are now the canonical engine APIs. Game code
  reaches them through GameAdapters bridges. Terrain (M3) and VFX
  (M4) reached STEADY-STATE DECISIONS not full implementations.

**Other infra ships:**

- **C++17 standard flip** (commit `5c03835`) — root `CMakeLists.txt`
  sets `CMAKE_CXX_STANDARD 17` uniformly. Stabilization gauntlet
  GREEN. Rules: `docs/cxx17-coding-rules.md`. Audit:
  `docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`.
- **MaterialGpu** (parallel session; untracked
  `RenderCore/MaterialGpu.h` + `shaders/include/material_gpu.hglsl`)
  — pushes item 5 (bindless-style resource model) from
  per-subsystem-prototype toward partial unification. Surface to the
  user for tracking.
- **VisibilityRequest v0** (parallel session;
  `RenderWorld/VisibilityRequest.h` untracked) — see item 6 above.
- **Migration guide + arc status ledger** —
  `docs/renderworld_migration_guide.md` + `docs/renderworld_arc_status.md`
  encode the contributor contract for the now-stable RenderWorld arc.
- **CLAUDE.md restructured** as tree-of-pointers router (217 → 111
  lines; topic content extracted to `docs/{critical_inline_rules,
  known_issues, tier1_env_vars, disciplines, load_bearing_pointers,
  active_campaigns}.md`).
- **Three CI scripts ship the discipline:**
  `scripts/check-include-firewall.sh` (M1; SCOPE_DIRS layering),
  `scripts/check-no-raw-gl-from-game.sh` (M6; game-side raw-GL
  prohibition), `scripts/check-vfx-no-objectid.sh` (M4; VFX
  attachment-2 prohibition). One pre-existing false-positive in the
  M6 script (trailing-comment-not-stripped) is queued for fix.

**Pivots vs original 2026-05-22 directives:**

- **M3 (Terrain identity)** is now a DECISION, not a slice in flight.
  Recon proved no GPU consumer drives the writer; CPU `worldToTile`
  remains canonical. If M3.1 ever ships per-quad terrain identity
  (editor-driven), use `subKind = Base/Water/Decal/Mine` payload —
  NOT separate `RenderObjectKind` values.
- **M4 (VFX)** is a PROHIBITION, not a substrate. Additive-blend +
  R32_UINT last-write-wins would clobber M2.6 mech-pick under
  particles. CI grep enforces.
- **M5 (Overlay)** is DEFERRED INDEFINITELY. "Overlay" had 7 in-tree
  meanings without an identity-needing consumer. Future use cases
  ship as NEW NAMED slices (HoverKindIndicator /
  RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT as
  "M5 Overlay").
- **M6 (firewall)** SHIPPED as audit-only (1 task; codifies the
  empirical finding that game-side already has zero raw GL calls).

---

## Tally (20 items)

### 1. Engine-as-API — stable handles, not game pointers

**Status: REALIZED for StaticProp + Mech kinds (2026-05-24).**

`RenderCore::RenderObjectHandle` is the engine type
(`RenderCore/Handle.h`). Every live `Mech3DAppearance` carries a
`mechRenderHandle` field; every registered static-prop has a stable
handle in the unified `RenderWorld::s_objectRecords` table indexed
by `handle.index()`. `GpuStaticPropRegistry` (Track B) is one of the
backing stores; the engine surface is `RenderWorld`.

Handle partitioning: StaticProp `0..0xFFFF`, Mech `0x10000..0x3FFFF`,
Terrain `0x40000..0x7FFFF` (reserved; no writer), Vfx
`0x80000..0xBFFFF` (reserved + prohibited writers). See
`docs/renderworld_arc_status.md` § "Handle-range partitioning."

GameAdapters bridge: `GameAdapters/{StaticProp,Mech}RenderAdapter.{h,cpp}`
are the ONLY translation units that include both engine and game-side
headers. `scripts/check-include-firewall.sh` enforces the direction.
`scripts/check-no-raw-gl-from-game.sh` (M6) enforces game-side has
zero raw GL calls.

Gap (residual): Terrain and VFX are RESERVED enum slots with
DECISIONS not implementations. Game code outside Mech / StaticProp
still reaches directly into renderer internals; the arc retired the
specific bug class for the two shipped kinds, not all kinds.

**Next step:** future kinds (HoverKindIndicator,
RenderWorldDebugOverlay, or a real terrain/editor consumer) follow
the same adapter-bridge pattern. The migration guide
(`docs/renderworld_migration_guide.md`) crystallizes the recipe.

---

### 2. Render graph / RenderPassDesc registry

**Status: PHASE 2 SHIPPED 2026-05-24 (env-gated runtime asserts).**

Phase 2 (commit `137dc70`) extends `mclib/render_contract.h` with:
- `RequiredAttachments` — which `COLOR_ATTACHMENTx` a pass needs in
  the active draw-buffer list (critical after M1.5 added
  attachment-2).
- `ShaderOutputContract` — which `layout(location=N)` outputs the
  pass's fragment shader declares; uniqueness check.
- `attachmentCount` in `PassStateContract` — expected
  `glDrawBuffers` arg.
- `assertPassContract()` queries live GL state and compares to
  contract; gated by `MC2_RENDER_CONTRACT_ASSERT=1`. Emits
  `[RENDER_CONTRACT v2] assert mode ACTIVE` on startup.
- All Phase 1 `TODO_RENDER_CONTRACT` rows are now filled in.

Gap (residual): no `void (*execute)(RenderContext&)` member; no
runtime dispatch; no resource dependency tracking. The contract is
ASSERTED, not EXECUTED. Phase 3 (enforced runtime dispatch) remains
unscheduled.

**Next step:** Phase 3 — per-draw contract assertions (currently
per-pass) and a path toward `void execute(RenderContext&)` callable
contracts that replace the imperative `XXX::render()` enqueuers.

---

### 3. World snapshot / extraction phase

**Status: IN PROGRESS — extraction arc v2.1–v3 SHIPPED (2026-05-26). Static-prop axis proven; frame-decoupled dispatch opt-in gate live.**

`GameCamera::render` still calls `objectManager->render()` which
iterates live game objects for ALL subsystems. Render code reads
mutable game state directly for terrain, mechs, VFX, and water.

**What shipped (static-prop axis only):**
- `RenderSnapshot` struct + 2×1 MiB ping-pong arena in
  `GameOS/gameos/render_snapshot.cpp`. `ExtractRenderSnapshot()`
  called once per frame between `DoGameLogic()` and `draw_screen()`.
- `ExtractedStaticPropPacket[]` span: 28-byte per-slot snapshot of
  batcher draw-slot state (pipelineId, materialIdx, texArrayLayer,
  sortedSlot, globalPacketIdx). L2 Frame lifetime.
- v2.2 dispatch-fact compare: cross-checks snapshot vs live batcher
  on 5 fields per slot. All zero tier1.
- v2.3 snap-cull: `flush()` skips zero-instance slots from snapshot.
- v3 (`MC2_SNAPSHOT_STATIC_PROP_BUILD=1`): builds `v6Packets`+`v6Meta`
  entirely from snapshot rows; compare vs live confirms correctness.
  Tier1 5/5 PASS. This is the authority-flip prerequisite for making
  the snapshot the primary render input for static props.

**Gap (residual):**
- v3 is still opt-in gate; authority flip (snapshot-primary) is
  the next slice.
- Mechs, terrain, VFX, water: all 4 subsystems still read live
  game state. No extraction exists for them.
- No `ViewState` / `Span<LightRecord>` extraction yet.

**Next step:** v3 authority flip (snapshot becomes primary for static
props; live path is fallback). Then extend `RenderSnapshot` to mechs
(lightest next target: per-frame SSBO already built in `flush()`).

---

### 4. Data-oriented processors

**Status: PARTIALLY REALIZED in GPU compute paths.**

`gpu_cull.comp` is a processor in spirit: takes `ObjectData[]` +
`CullUBO.viewProj`, emits visibility bits. CPU only books the
dispatch. The F3 object/prop iteration GPU port
(`docs/superpowers/specs/2026-05-18-object-prop-iteration-gpu-port-stage0.md`)
is designing another processor: `Transform[] → inView/canBeSeen[]`.

Gap: no formal `Processor { input fragments, output fragments }`
abstraction. Processors are ad-hoc per subsystem.

**Sequencing note:** don't build a formal ECS. Let each GPU offload
slice continue to be a de-facto processor; the abstraction name is
useful for the design documents, not necessarily the C++ type.

---

### 5. Bindless-style resource model

**Status: ADVANCED — MaterialGpu static-prop arc v4–v7 DEFAULT-ON (2026-05-26).**

Terrain: `nodeId → gosHandle` LUT (per-frame upload,
`gpu_driven_terrain_solid.comp` binding 2).
Static props: `GpuStaticPropRegistry` index-based GPU access.
Lights: `LightsData` SSBO slot-indexed.
**Material (static props — COMPLETE):** `RenderCore/MaterialGpu.h` +
`shaders/include/material_gpu.hglsl` — `MaterialGpu { albedoIndex, normalIndex,
flags, ... }` SSBO schema. Default-ON upload, bind, compare, and shader
sampling as of v7 (HEAD `ae2152cd`). Kill switches: `MC2_MATERIAL_GPU=0`
disables upload/bind/compare; `MC2_MATERIAL_GPU_SAMPLE=0` disables shader
sampling and falls back to `texArrayLayer`. Log tag: `[MATERIAL_GPU v4]`.
Tier1 5/5 PASS all gates. `scripts/check-material-gpu-mirror.sh` enforces
C++/GLSL field-order mirror.

**Material (mechs — PARTIAL):** `GpuMechInstance.materialIdx` field at byte 52
(HEAD `c2dd0a33`). Compare invariant proven (`mismatches=0`), log tag
`[MECH_MATERIAL_GPU v1]`. Shader sampling NOT yet wired.

Gap (residual): Mech shader sampling (Mech-2) is BLOCKED — `albedoTex` on
`GpuMechInstance` holds a `texHandle`/slot (compare-only; NOT shader-actionable
in the current mech pipeline). Requires a texture model decision before
`mech.frag` can consume MaterialGpu indices. Decision doc:
`docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md`.
Terrain material path deferred (separate arc).

**Next step (Mech-2):** texture model decision → wire `mech.frag` albedo through
MaterialGpu table. NOT a direct copy of the static-prop switch — `albedoTex`
semantic differs.

---

### 6. Visibility service as engine API

**Status: V0 SHIPPED (parallel session, 2026-05-24).**

`RenderWorld/VisibilityRequest.h` (currently untracked in this
worktree; parallel session) defines the V0 interface. Runtime emits
per-frame `[VISIBILITY v1] frame=N static_props=S mechs=M
terrain=deferred vfx=prohibited` lines that report the current state
of each kind axis (terrain=deferred per M3 decision;
vfx=prohibited per M4 decision). `scripts/check-visibility-log-schema.sh`
gates the schema (passes: "OK").

Existing cull dispatches (terrain, prop, mech) still run separately;
VisibilityRequest v0 is the OBSERVABILITY + ROUTING surface, not yet
the unified dispatcher. The kind axis is now a first-class concept;
the per-subsystem cull code lives behind it.

Gap (residual): V1 should fold the three per-subsystem cull
dispatches behind a single `VisibilitySet compute({view, layers})`
call so shadow / minimap / selection passes share invocation. The
schema for this is in place; the dispatcher is not.

**Next step:** V1 unification of the three cull dispatches under
the VisibilityRequest API surface. Driven by a multi-view consumer
(item 7) or a shadow-pass refactor.

---

### 7. Multi-view rendering model

**Status: SUBSTRATE SHIPPED (2026-05-29) — Engine View Registry live; dispatch not unified.**

`RenderCore/EngineView.h` defines `ViewId` constants and `ViewKind` enum:
- `kMainSceneViewId = 1` (MainScene)
- `kShadowDirectional0ViewId = 2` (ShadowStatic)
- `kShadowDynamicViewId = 3` (ShadowDynamic)

`view_uniforms_gl.{h,cpp}` `registerOrUpdateView()` / `getViewCount()` /
`getViewByIndex()` API is live. Shadow passes registered via SHADOW-VIEW-1
(`d939de67`). `[ENGINE_VIEW_REGISTRY v1]` log emitted on startup; gate-off
via `MC2_VIEW_UNIFORMS=0`.

Gap (residual): no `EngineView { Frustum, Viewport, RenderMask }` beyond
ViewId+ViewKind+UBO slot. Shadow and main scene each still upload their own
matrix independently — the registry RECORDS views but does not DISPATCH
through them. No minimap, portrait, or reflection view registered yet.

**Next step:** wire `setCurrentView(kShadowDirectional0ViewId)` around
`flushShadow()` so the shadow pass binds through the view registry rather than
raw uniform upload. That is the first DISPATCH unification slice.

---

### 8. Asset cook pipeline

**Status: DESIGNED FOR MECHS/BUILDINGS, NOT IMPLEMENTED.**

`docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`
and `docs/superpowers/plans/2026-05-19-assimp-import-phase0.md`
cover Assimp-based import. Static-prop cluster-LOD PoC
(`2026-05-19-static-prop-cluster-lod-poc-design.md`) has the `.cdag`
sidecar concept for cluster DAGs.

Gap: no `.meshpack` / cooked-buffer format, no cook manifest,
no offline meshOptimizer pipeline. Assimp currently parses at
load time (no offline bake step). meshoptimizer has zero runtime
footprint in the nifty-mendeleev tree today.

**Next step:** the static-prop cluster-LOD PoC's Phase 0 is the
Assimp import path — that merge is the prerequisite to any cook
pipeline. After import: add an offline-cook step that writes a
`.cdag` sidecar with meshOptimizer LODs + meshlets.

---

### 9. Renderable archetypes

**Status: PARTIAL — RenderObjectKind enum + decisions framework (2026-05-24).**

`RenderCore::RenderObjectKind` is the engine-typed kind axis:
- `StaticProp = 0` (SHIPPED, M1 adapter)
- `Mech = 1` (SHIPPED, M2 adapter + M2.5 substrate + M2.6 pickup)
- `Terrain = 2` (RESERVED, M3 decision: no writer; CPU
  `worldToTile` canonical)
- `Vfx = 3` (RESERVED + PROHIBITED writers, M4 decision:
  click-through by design; CI-grep enforces no attachment-2 write)
- `Overlay` (DEFERRED INDEFINITELY, M5 decision)

Each kind has its own adapter pattern + handle-base partition +
per-kind decisions ledger (see `docs/renderworld_arc_status.md`).

Forward-compat: if M3.1 ever ships per-quad terrain identity
(editor-driven), use `subKind = Base/Water/Decal/Mine` payload
attached to the `Terrain` kind — NOT separate `RenderObjectKind`
values for terrain flavors.

Gap (residual): no
`{ MeshRef, MaterialRef, LodPolicy, ShadowPolicy, ImpostorPolicy,
SelectionPolicy }` archetype STRUCT. The kind axis exists; the
policy bundle attached to each archetype does not. `RenderObjectDesc`
(in `RenderCore/`) is the partial PoD mirror; it does not yet carry
policy fields.

**Sequencing note:** policy fields land per-kind as the
corresponding consumer demands them. M2.6 added gameplay-pick policy
(fog respect, sensor visibility) inside `tryGameplayPick`, not
inside an archetype struct — and that's fine for now. Premature
unification before another kind needs the same policy is YAGNI.

---

### 10. Object-ID / visibility-ID buffer

**Status: FULLY REALIZED (2026-05-23 / 2026-05-24).**

This is the central item that landed during the RenderWorld arc.
Substrate, consumers, contracts, and CI scripts are all in place.

**Substrate (M1.5, commit `842f34f`):** `MC2_OBJECT_ID_BUFFER=1`
env-gated `R32_UINT` MRT attachment at `GL_COLOR_ATTACHMENT2` on the
main scene FBO. C1 META-FIX `setSceneDrawBuffers()` helper in
`gos_postprocess.cpp` centralizes draw-buffer policy across 5 sites
(retired the "scattered glDrawBuffers policy drift" bug class).

**Producers:**
- Static-prop (M1.5): `static_prop.frag` writes `Handle.raw()` via
  `layout(location=2) out uint v_objectId`; consumed from
  `PerDrawEntry.objectIdRaw`.
- Mech (M2.5, commit `eac698e`): `mech.frag` writes `Handle.raw()`
  via `layout(location=2) out uint v_objectId`; per-instance SSBO
  field on `GpuMechInstance` (48B→64B with `objectIdRaw`).
  Per-mission counters: `[MECHBATCHER v1] event=mech_id_summary
  gpu_mech_id_writes=N` + `event=mlr_mech_summary
  mlr_mech_draws=M`.
- Terrain (M3 decision): NO WRITER. `lookupAtPixel` tripwire warns
  if `kind=Terrain` ever returned.
- VFX (M4 decision): PROHIBITED writers. `scripts/check-vfx-no-objectid.sh`
  CI gate forbids `layout(location=2) out` in VFX shader allowlist.

**Consumers:**
- `RenderWorld::lookupAtPixel(x, y) → LookupResult { handle, kind }`
  synchronous readback (generation + alive check) — the API surface.
- M1.6 static-prop pick (`[GAMEPLAY_PICK v1] hit kind=StaticProp`).
- M2.6 mech pick (`[GAMEPLAY_PICK v1] hit kind=Mech`).
- M2-pre `tryGameplayPick(req)` spine — unified dispatcher with
  4-gesture gate ladder + mover-first short-circuit + fog respect.

**Self-tests:** `[OBJECT_ID_SELFTEST v1]`,
`[MECH_OBJECT_ID_SELFTEST v1]`, `[MECH_PICK_SELFTEST v1]`,
`[GAMEPLAY_PICK_SELFTEST v1]` — each gated by its own
`MC2_*_SELFTEST=1` env, all emit `result=PASS|FAIL ...` per mission.

**MLR fallback gap:** measurable via the always-on
`mlr_mech_draws=M` counter; empirically `M=0` across all 5 tier1
missions (Q6 amendment proves the gap is rare-in-practice).

Full ledger: `docs/renderworld_arc_status.md`.

**Gap (residual):** none material. The substrate is mature; future
consumers (HoverKindIndicator, RenderWorldDebugOverlay, editor
inspector) ship as new named slices CONSUMING this substrate
without adding new substrate.

---

### 11. Draw packets / render packets

**Status: v7+v7.1 SHIPPED — DrawPacket canonical dispatch DEFAULT-ON (2026-05-26). HEAD `f780949f`.**

The DrawPacket arc makes the rendering intent explicit in a data structure that
outlives the draw-call submission window, enabling sort, cull, and multi-pass
dispatch, replacing the old "implicit in enqueuers" state:

**Arc summary:**
- **v0** (`2503d1ab`): `draw_packet_emitter.h` — `DrawPacket` struct +
  `DrawPacketCandidate`; two-phase emit after `ExtractRenderSnapshot`.
- **v1** (`ff8bdc7c`): `[DRAW_PACKET v1]` log tag, materialMismatches
  cross-check gate.
- **v2** (`f2546c55`): `pipelineId` + `cachedMaterialFlags` on
  candidate; `comparePacketsToLegacy` with categorized counters.
- **v3** (`6eab374a`): candidate→`DrawPacket` ABI promotion; struct
  promoted as engine type (no dispatch yet).
- **v4A–v4C + v5** (SHIPPED then REMOVED in v7.1): substitutive dispatch
  coverage probes. All gate scaffolding deleted; historical archaeology only.
- **v6** (`57e62dbb`): canonical `DrawPacket[]` + `StaticPropDispatchMeta[]`
  dispatch. `StaticPropDispatchMeta` 32B pod per draw slot. Three-guard
  builder. `ok=1` all tier1 missions.
- **v7** (default-ON flip): `s_v6Enabled` true by default; kill-switch
  `MC2_STATIC_PROP_LEGACY_DISPATCH=1` reverts to legacy multidraw.
  Tier1 5/5 PASS both gates.
- **v7.1** (HEAD `f780949f`): dead-code removal. `StaticPropOpaquePacketView`,
  `batcher_setOpaqueDispatchCandidates`, v4A/v4B/v4C flush blocks,
  deprecated V6 gate plumbing removed. Net -291 lines. Lambda simplified
  to kill-switch only.

**Dispatch hierarchy (v7, current):**

| Path | Trigger | Log tag |
|---|---|---|
| **Primary** — `DrawPacket[]` + `StaticPropDispatchMeta[]` | Default ON | `[DRAW_PACKET_V6]` |
| **Fallback** — legacy `glMultiDrawElementsIndirect` | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | none |
| **Diagnostic** — v5 per-draw-call loop | `MC2_DRAW_PACKET_COALESCE_V5=1` (deprecated) | `[DRAW_PACKET_V5]` |
| **Historical** — v4A/v4B/v4C probes | Removed in v7.1 | n/a |

**Supporting infra:**
- `RenderCore::StaticPropTypeDesc` 16-byte immutable per-type descriptor.
- `MC2_DRAW_PACKET_COMPARE` + verbose env-vars still active (see `docs/tier1_env_vars.md`).
- `scripts/run_smoke.py` propagation tuple includes `MC2_STATIC_PROP_LEGACY_DISPATCH`.

**Gap (residual):** no sort keys, no pipeline-change cost model, no cross-pass
draw-list reuse. `PipelineId` exists on struct; `MeshHandle`/`MaterialHandle`
layout reserved.

**Next steps (v8):** shadow-pass packet dispatch (`flushShadow` currently
legacy), GPU-cull count integration, `sortKey` population, mech/terrain
packets, log-tag normalization `[STATIC_PROP_PACKET_DISPATCH v1]`.

---

### 12. Pipeline-state objects

**Status: ADVANCED — Phase 2 render-contract runtime asserts (2026-05-24).**

`applyRenderStates` state-equality cache (shipped 2026-05-08) avoids
redundant GL state calls. `render_contract.h PassStateContract`
documents blend/depth/MRT requirements per pass. **Phase 2** (commit
`137dc70`): `RequiredAttachments` + `ShaderOutputContract` +
`attachmentCount` extend the contract; `assertPassContract()`
queries live GL state and warns on mismatch when
`MC2_RENDER_CONTRACT_ASSERT=1`.

The contract now COVERS:
- Depth test + write
- Blend mode
- MRT requirement
- Expected FBO label
- Required color attachments (post-M1.5 ObjectID attachment-2)
- Fragment-shader output `layout(location=N)` declarations
  (uniqueness check — must not collide with sibling passes)

Gap (residual): no explicit `PipelineDesc { ShaderProgramHandle,
BlendMode, DepthMode, CullMode, VertexLayout }` struct that can be
CREATED + CACHED + BOUND as a unit. The contract DESCRIBES and
ASSERTS state but doesn't OWN it.

**Next step:** Phase 3 — extend `PassStateContract` from a
documentation-and-assertion type
to a runtime `PipelineDesc` that `applyRenderStates` can take as
input and validate against. The state cache already does the equality
check — just needs the descriptor type.

---

### 13. Frame allocator / transient resource system

**Status: GENERAL `FrameArena` SHIPPED + UNIT-TESTED (2026-05-29). PRODUCTION MIGRATED. PER-SUBSYSTEM RINGS REMAIN. TAXONOMY DOC SHIPPED (2026-05-26).**

Terrain thin records, water thin records, and GPU particle SSBOs
all still use purpose-built ring buffers. The `RenderSnapshot`
ping-pong arena has been generalized into `RenderCore::FrameArena`
(`FRAMEARENA-L2-1`): a GL-free typed `allocArray<T>(n)` bump allocator
over an externally-owned 2×1 MiB ping-pong buffer (Persistent
allocation, per-frame `reset(frameIndex)`, fail-closed overflow,
high-water tracking). Production already consumes it in
`render_snapshot.cpp` (`ExtractedStaticPropPacket[]`,
`ExtractedStaticProp[]`, `ExtractedMechPacket[]` spans) and
`gos_mech_batcher.cpp`.

`FRAME-ARENA-1` (2026-05-29, `eee18be4`) closed the durability gap:
8 GL-free doctest cases (101 assertions — capacity/used/remaining,
reset+high-water persistence, typed span/pointer, alignment incl.
over-aligned, sequential non-overlap, fail-closed overflow,
zero-count inert, frame-flip lifecycle) + non-behavioral
`capacity()`/`remaining()` accessors. No production behavior change.

**ResourceLifetime taxonomy** (P2-1 partial) now documented:
`docs/observations/2026-05-26-resource-lifetime-taxonomy.md`.
Four tiers classified with all 7 "classify" resources placed:
- **L0 Persistent:** RenderSnapshot arena, ObjID FBO, mine tex array
- **L1 Mission:** coalesce SSBOs, tex arrays, materialGpu SSBOs, s_packets
- **L2 Frame:** `ExtractedStaticPropPacket[]` (already arena-backed)
- **L3 PassTransient:** `v6Packets`, `v6Meta` (static-vector reuse in flush)

Gap (remaining): ad-hoc per-subsystem rings; new GPU passes each
still build their own. The `FrameArena` API now exists but adoption
beyond `RenderSnapshot` records is unstarted.

**Done:** ~~expose the ping-pong arena as a general `FrameArena` typed
allocator~~ (`FRAMEARENA-L2-1` substrate + production migration;
`FRAME-ARENA-1` tests/accessors). `FRAME-ARENA-RENDERSNAPSHOT-MIGRATE-1`
is **obsolete** — that migration shipped inside `FRAMEARENA-L2-1`.

**Next step (P2-1 Phase 2 — adoption, not substrate):** the substrate
is no longer the work item; future consumers are. HZB intermediates and
sort-key buffers are the compute-pass consumers to back with `FrameArena`
after Tier 3. Retrofit existing per-subsystem rings (terrain/water thin
records, particle SSBOs) as they are next touched.

---

### 14. Feature flags as productized renderer modes

**Status: REGISTRY EXISTS + UNIT-TESTED (2026-05-29). COUNT=26 entries verified.**

`RenderCore/RendererFeatureRegistry.h` (currently untracked in this
worktree; parallel session) is the registry header that codifies
which `MC2_*` env-vars are part of the productized renderer
contract. Each entry carries default state + dependencies + the
graduation criteria for promotion from "experimental gate" to
"shipped default."

Per-feature documentation in this worktree is also tightened:
`docs/tier1_env_vars.md` extracted from CLAUDE.md (2026-05-24)
groups env-vars by category (always-on / RenderWorld arc / contract
assert / pre-commit / firewall scripts).

Gap (residual): the registry SCHEMA exists; coverage across all
~50+ MC2_* flags is incomplete. Pre-commit / CI gate that requires
new MC2_* env-vars to land with a registry entry is not yet wired.

**Next step:** wire a `scripts/check-render-feature-registry.sh`
that finds new `MC2_*` declarations in source not present in the
registry header, and fails CI. This converts the registry from
"documentation discipline" to "mechanically enforced contract" —
same pattern as M6 firewall script vs. M2.5 reviewer-discipline gap.

---

### 15. Streaming / residency with importance scoring

**Status: NOT STARTED.**

The `distant_buildings_render_at_lower_lod_never_distance_culled.md`
constraint documents that far buildings must stay visible at lower
LOD, never distance-culled. No residency budget, LOD residency
priority, or importance-scored preload exists.

**Sequencing note:** this is post-meshlet-LOD work. Don't start
until cluster-LOD PoC ships and there are actual LOD levels to manage.

---

### 16. Editor / runtime boundary (debug engine API)

**Status: M1 SHIPPED + DEBUG-VIEW-REGISTRY-1 SHIPPED (2026-05-29).**

DebugRenderer M1 (`c2e877e` + `96b6b3a`): `flushWorldPrims()` wired
into `gamecam.cpp` after the weather pass.

**DEBUG-VIEW-REGISTRY-1** (`8284dde8`): Canonical `RenderDebugView` enum
(10 values) in `RenderCore/RenderDebugView.{h,cpp}` with per-lane support
masks (`kDebugViewMask_StaticPropOpaque`, `kDebugViewMask_Mech`,
`kDebugViewMask_Vfx`). ImGui combo now filtered to lane-supported views
replacing raw env-text. `batcher_setDebugMaterialMode()` free function.
Mech debug views wired to `mech.frag` `u_debugMode` with no shader changes.
Substrate unit tests verify enum completeness (23 test cases).

Gap (residual): Terrain and Shadow debug view masks are placeholder-zero.
The API surface is for IN-WORLD primitives + debug views; per-handle
inspection (`drawBounds(handle)`, selection bracket) is M2+ work.

**Next step:** DebugRenderer M2 — per-handle inspection consuming
`[GAMEPLAY_PICK v1]` schema; render selection bracket / mech name overlay
when a pick lands. Wire Terrain debug views through the registry (currently
zero mask).

---

### 17. Sim / render interpolation layer

**Status: NOT STARTED.**

Transforms applied directly from game state each frame. No
`prevTransform` / `currentTransform` lerp.

**Sequencing note:** this is a late-stage concern. The extraction
phase (item 3) is the prerequisite — once rendering consumes an
immutable `RenderSnapshot`, adding `prevSnapshot` / `currentSnapshot`
lerp is a straightforward addition.

---

### 18. Deterministic render audit logs

**Status: BROADENED FURTHER — Render Resource Registry + contract aggregator shipped (2026-05-29).**

New since 2026-05-24:
- `renderResources[]` JSON array in `debug_state_dump.cpp` — per-resource
  RenderResourceDesc (id, kind, format, width, height, glName, valid).
  TerrainHeightTexture + ShadowStaticMap auto-registered.
- `registeredViews[]` JSON array — Engine View Registry entries per-view.
- `scripts/check-contracts.sh` aggregator — 8 grep checks, aligned PASS/FAIL
  table, CI-runnable. All 8 PASS as of 2026-05-28. Subsumes scattered
  per-script invocations.

The `[SUBSYS v1]` banner convention has expanded substantially during
the RenderWorld arc + render contract Phase 2:

- `[RENDER_WORLD v1] frame=N objects=N static_props=S mechs=M
  visible=0 packets=0 views=1 objectid_buffer=on|off` —
  per-frame summary; partitioned by kind (M2 + M3 frameBannerTick
  extensions).
- `[RENDER_WORLD v1] event=init objectid_buffer=on|off`
  + `event=destroy upsert_ok=N upsert_fail=N destroy_calls=N
  mark_visible=N` + `event=mech_end_mission registered=N
  destroyed=N alive=N fail=N` — lifecycle events.
- `[GAMEPLAY_PICK v1] hit kind=X handle=N idx=N gen=N ...` +
  `enabled banner` — unified per-kind pickup schema (M2.6 META-FIX
  retired the per-kind `[STATIC_PROP_PICK v1]` form).
- `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` +
  `event=mlr_mech_summary mlr_mech_draws=M` — per-mission writer +
  fallback counters.
- `[RENDER_CONTRACT v2] assert mode ACTIVE` — Phase 2 startup line
  when `MC2_RENDER_CONTRACT_ASSERT=1`.
- `[VISIBILITY v1] frame=N static_props=S mechs=M terrain=deferred
  vfx=prohibited` — per-frame kind-axis status (V0 ships of
  VisibilityRequest).
- `[OBJECT_ID_SELFTEST v1]`, `[MECH_OBJECT_ID_SELFTEST v1]`,
  `[MECH_PICK_SELFTEST v1]`, `[GAMEPLAY_PICK_SELFTEST v1]` —
  4 substrate / consumer self-tests, each `result=PASS|FAIL ...`.

Schema-grep convention `\[SUBSYS v[0-9]+\]` continues to work; new
schemas register here.

Gap (residual): terrain, water, and particle subsystems don't yet
have their own `[SUBSYS v1]` banners. The RenderWorld arc shipped
the umbrella `[RENDER_WORLD v1]` banner; per-subsystem audit on
non-RenderWorld kinds is future work.

**Next step:** when terrain / water / particle work next opens,
land a `[TERRAIN v1]` / `[WATER v1]` / `[PARTICLE v1]` summary
banner per the established convention.

---

### 19. Renderer owns fallback paths

**Status: PARTIALLY DONE per-subsystem.**

`IsFrameSolidArmed()`, `IsFrameOverlayArmed()`, `MC2_GPU_*`
gates — engine-owned predicates that choose GPU vs legacy path.
The minePass `IsFrameMineArmed()` is the reference precedent.

Gap: no `MeshCapability { HasMeshlets, HasImpostor, HasClusterLOD,
HasLegacyMeshOnly }` flag set on mesh assets. Capability-based
path selection doesn't exist yet — each subsystem hard-codes its
own gate.

**Next step:** introduce `MeshCapability` flags in the asset import
pipeline (Assimp phase 0 prerequisite), so the renderer chooses
the path from capability, not from a global env-var.

---

### 20. RenderWorld as main engine API object

**Status: REALIZED for the StaticProp + Mech axes (2026-05-24).**

The arc reached STEADY STATE. `RenderWorld` IS the main engine API
object for the two shipped kinds. Canonical APIs:

- **Handle lifecycle:**
  - `RenderWorld::upsertStaticProp(StaticPropInstanceDesc) → RenderObjectHandle` (M1)
  - `RenderWorld::registerMech(RenderMechDesc) → RenderObjectHandle` (M2)
  - `RenderWorld::destroyMech(RenderObjectHandle)` (M2; idempotent per `05f1f2d`)
  - `RenderWorld::clearAllMechRecords()` (M2)
- **Object-ID readback:**
  - `RenderWorld::lookupAtPixel(x, y) → LookupResult { handle, kind }` (M1.5; M2.6 added `kind`)
- **Substrate accessors:**
  - `RenderWorld::IsObjectIdBufferEnabled() → bool`
  - `RenderWorld::getMechsAliveCount() → uint64_t` (M2.6)
- **Debug state:**
  - `RenderWorld::GameplaySelectionDebugState` + `setLastGameplayPick()` (M2.6; META-FIX rename from M1.6's per-kind state)
- **Self-tests:** `runSubstrateSelfTest()`,
  `RunGameplayPickSelfTest()`, `RunMechObjectIdSelfTest()`,
  `RunMechPickSelfTest()` — wired in `RenderWorld::init()` chain;
  each env-gated.

GameAdapters layer is the de-facto NS3 boundary:
- `GameAdapters/StaticPropRenderAdapter.{h,cpp}` (M1)
- `GameAdapters/MechRenderAdapter.{h,cpp}` (M2; the only TU that
  may include both `mclib/mech3d.h` and `RenderWorld/RenderWorld.h`)

`scripts/check-include-firewall.sh` enforces the direction;
`scripts/check-no-raw-gl-from-game.sh` enforces no raw-GL bleed from
game side.

How other 20-items map to today's RenderWorld:
- `createObject` (item 1) → REALIZED for StaticProp + Mech
- `render` dispatching pass registry (items 2, 11) → render contract
  Phase 2 ASSERTS the contract; Phase 3 would EXECUTE it
- `computeVisibility` (item 6) → V0 SHIPPED; per-subsystem dispatch
  still separate
- `createView` (item 7) → NOT STARTED; F1 prerequisite
- `extractSnapshot` (item 3) → NOT STARTED

Full ledger: `docs/renderworld_arc_status.md`. Contributor onboarding:
`docs/renderworld_migration_guide.md` (646 lines, 16 sections).

Gap (residual): `RenderWorld::render()` doesn't exist as a single
entrypoint that owns the full frame yet — game code still calls
`objectManager->render()` + `land->render()` + etc. The arc shipped
the HANDLE + IDENTITY axis; the FRAME-OWNS axis (extraction +
multi-view + per-pass dispatch) is still future work and depends on
items 3, 7, and 11.

**Sequencing note:** the boundary document and the slice arc both
landed; the NS3 deliverable is alive in the worktree. Future work
extends `RenderWorld` SCOPE, not its existence.

---

## Sequencing recommendation

Ordered by: (1) unblocks other items, (2) low blast radius, (3) NS alignment.

```
Already in execution / recently shipped:
  DrawPacket v4A                                — item 11 partial; opaque non-coalesce dispatch proven
  F1 (unified-projection ViewUniforms UBO)      — Phase 0+1 done; Phase 2+3 (atomic flip) pending
  quadSetupTextures retirement                  — clean Tracy gate for NS2/terrain
  HDRI sky (SkyRenderAdapter)                   — Track V Tier 2 partial start
  FX-GPU B3 (GPU particles default ON)          — Track V Tier 2 GPU VFX partial start

Sequence A — engine API boundary (items 1, 2, 12, 20):
  A1. Write RenderWorld boundary spec (doc only, no code)    ← DONE (renderworld_arc_status.md)
  A2. Extend render_contract.h Phase 2 (debug assertions)   ← SHIPPED (commit 137dc70)
  A3. Extend PassStateContract → PipelineDesc runtime type  ← NOT STARTED
  A4. RendererFeatureRegistry header (existing flags)        ← SHIPPED (RendererFeatureRegistry.h)

Sequence B — mesh geometry tier (items 8, 5, 19, 9):
  B1. Assimp import phase 0                ← VENDOR DONE (889cebff); cooker off
  B2. Offline meshOptimizer cook step      ← VENDOR DONE (774f074a); cook not wired
  B3. MaterialGpu SSBO + shader-side       ← SCHEMA SHIPPED (MaterialGpu.h); per-pass adoption pending
  B4. MeshCapability flags at import time  ← NOT STARTED
  B5. Renderable archetype design          ← NOT STARTED

Sequence C — visibility + debug tier (items 6, 10, 16, 18):
  C1. VisibilityRequest/Result wrapper     ← V0 SHIPPED (VisibilityRequest.h)
  C2. R32_UINT object-ID buffer            ← FULLY REALIZED (M1.5+M2.5+M2.6)
  C3. DebugRenderer namespace              ← M1 SHIPPED (flushWorldPrims)
  C4. [RENDER_WORLD v1] frame summary      ← SHIPPED

Sequence D — draw packet path (item 11):
  D1. DrawPacket explicit type             ← SHIPPED (v0→v3)
  D2. Opaque non-coalesce dispatch         ← SHIPPED (v4A, gated off by default)
  D3. Coalesce-mode integration            ← NEXT (v4B)
  D4. Default flip + legacy loop retire    ← v5 (after v4B tier1 PASS)
  D5. Sort keys + cross-type ordering      ← after v5

In progress (items 3, 13):
  Extraction phase     — v2.1–v3 SHIPPED (static props); authority flip next
                         Mechs/terrain/VFX extraction not started
  Frame allocator      — FrameArena SHIPPED + unit-tested (FRAMEARENA-L2-1,
                         FRAME-ARENA-1); production migrated. Adoption by
                         HZB/sort-key/per-subsystem rings next

Deferred (items 7, 15, 17):
  Multi-view model     — after F1 atomic flip + RenderWorld boundary
  Residency/streaming  — after meshlet LOD ships
  Sim/render interp    — after extraction phase (item 3) is broader
```

---

## Outside advisor input (2026-05-22) — endorsed + additions

Advisor confirmed the three-workstream A/B/C framing and the do-not-start
list (no full ECS, no full render graph scheduler, no full Vulkan
abstraction, no full Nanite clone, no rewriting gameData). Three
substantive additions beyond the audit above:

---

### Addition 1: LegacyRenderAdapter pattern (explicit migration firewall)

The riskiest transition is not building `RenderWorld`. It is migrating
`BldgAppearance`, `Mech3D`, `GameCamera`, terrain, particles, overlays,
etc. without making `RenderWorld` a mirror of the old engine.

Define temporary adapters:

```cpp
class BuildingRenderAdapter {
public:
    RenderObject sync(RenderWorld&, const BldgAppearance&);
};

class MechRenderAdapter {
public:
    RenderObject sync(RenderWorld&, const Mech3D&);
};
```

**Rule (load-bearing):**
`RenderWorld` must not `#include` `BldgAppearance.h`, `Mech3D.h`,
`ObjectManager.h`, or any mission/gameplay header.
Adapters may include both sides.
Adapters are explicitly TEMPORARY — they exist to bridge old gameData
into the new API and are deleted once the caller is migrated.

---

### Addition 2: Forbidden dependencies — write these in the spec

The RenderWorld boundary spec MUST include explicit forbidden-dependency
lists. These prevent future sessions from re-coupling the layers.

**RenderWorld must not depend on:**
- `ObjectManager`
- `BldgAppearance`
- `Mech3D`
- `Mission` or any gameplay header
- gameData concrete types
- OpenGL global state outside `RenderDevice`/`RenderContext`

**gameData must not depend on:**
- GL buffer IDs
- shader program IDs
- SSBO/UBO binding points
- material table indices
- indirect draw command layout
- GPU sync primitives

These rules are to be enforced by the include-graph checker (or at
minimum documented in the spec and audited at adversarial review).

---

### Addition 3: First migration target — static props/buildings

First end-to-end `RenderWorld` slice should be static props / buildings,
not mechs or terrain. Reasons:

1. `GpuStaticPropRegistry` already proves the handle model — routing
   through `RenderWorld` is an API-shape exercise, not a new GPU path.
2. Buildings are less animation-heavy than mechs.
3. They are already the cluster-LOD PoC target (Sequence B).
4. They exercise: handles, materials, draw packets, visibility, fallback.

The first slice:
```
BldgAppearance / static prop legacy adapter
→ RenderWorld::createObject
→ MeshHandle + MaterialHandle
→ DrawPacket
→ existing GpuStaticPropRegistry / indirect path
```

No renderer behavior change at first. Just route through the boundary.
Correctness gate: zero visual delta vs legacy path (same indirect draw
commands emitted, same pixels).

---

### RenderWorld boundary spec — required sections (advisor-supplied)

When writing the A1 boundary spec doc, include all 13 sections:

```
 1. Purpose / non-goals
 2. Ownership boundary
 3. Handle model
 4. Render object lifecycle
 5. View model
 6. Draw packet model
 7. Visibility service
 8. Material / mesh / texture handles
 9. Fallback and capability rules
10. Legacy adapter migration plan
11. Debug/audit requirements
12. Forbidden dependencies (see Addition 2 above)
13. First migration target (static props/buildings)
```

---

### Architectural north star (advisor phrasing — capture verbatim)

```
RenderWorld    = engine-facing scene API
GpuStaticPropRegistry / TerrainQuadRecipe / LightsData /
  future MechMeshlets = internal storage backends
DrawPacket     = renderer-facing unit of work
VisibilityRequest = view-facing culling API
MeshCapability = fallback path decider
```

Bridge:
```
old MC2 gameData
   ↓ legacy adapters (temporary)
RenderWorld
   ↓ extraction / visibility / packets
OpenGL renderer today
   ↓ same API shape
Vulkan renderer tomorrow
```

---

---

## Phase 2: meta-fixes (outside advisor, 2026-05-22)

Phase 1 convergence (A/B/C above) delivers the engine API shape.
Phase 2 delivers **invisible contracts made explicit, testable, and hard to regress.**
Not bigger features. Sharper boundaries.

Meta-fix principle (verbatim):
> Every time two systems know the same fact, create one owner and
> make the other system ask for it.

---

### P2-1. Resource lifetime / ownership taxonomy

**Status: PHASE 1 DOC SHIPPED (2026-05-26).**
`docs/observations/2026-05-26-resource-lifetime-taxonomy.md`

Four lifetime tiers classified with all 7 "classify" resources placed:

```
L0 Persistent    — RenderSnapshot arena, ObjID FBO, terrain mine tex array
L1 Mission       — coalesce SSBOs, tex arrays (α-OFF/ON), materialGpu SSBOs
                   (static props + mechs), s_packets global packet table
L2 Frame         — ExtractedStaticPropPacket[] (arena-backed ping-pong)
L3 PassTransient — v6Packets, v6Meta (static-vector reuse in flush())
```

Correction note (from review): mech `MaterialGpu` SSBO has a split:
physical GL buffer = L1 Mission (freed at `onMapUnload`), contents
populated = L2 Frame-ish (rebuilt each `flush()` with `GL_DYNAMIC_DRAW`).
The allocator must manage buffer lifetime (L1), not upload cadence (L2).

**Phase 2 (not started):** promote `ResourceLifetime` from documentation
to a code type. Attach to each GPU resource allocation site. The
`render_contract.h` Phase 1→2 pattern is the template. Phase 2 is
gated on the frame allocator (item 13) existing to enforce L2/L3.

Meta-fix goal: no more "who frees this buffer?" or "can this pass
still read this?" — the same question that caused the ring-slot state
transport bug (`ring_slot_state_must_travel_with_slot.md`).

---

### P2-2. Shader interface schema

Prevent C++/GLSL struct layout drift. Add a YAML-or-similar schema
that defines each UBO/SSBO binding point:

```yaml
ViewUniforms:
  binding: 0
  layout: std140
  fields:
    - mat4 u_worldToClipGL
    - mat4 u_worldToView
    - vec4 u_cameraWorldPos

ObjectData:
  binding: 2
  layout: std430
  fields:
    - mat4 world
    - uint materialIndex
    - uint objectId
```

The schema generates or validates: C++ struct layout, GLSL block
declaration, binding point, debug name, size/alignment.

Meta-fix goal: eliminate hand-maintained C++/GLSL drift. The existing
`// lockstep with gos_terrain_patch_stream.h:87-97` comments in
`gpu_driven_terrain_solid.comp` are the manual version of this —
they work but don't prevent silent breakage.

---

### P2-3. Capability-driven renderer paths

Generalize `MeshCapability` (roadmap item 19) into a full
`RenderableCapability` struct that drives path selection:

```cpp
struct RenderableCapability {
    bool hasLegacyMesh;
    bool hasLodChain;
    bool hasMeshlets;
    bool hasClusterDag;
    bool hasImpostor;
    bool supportsObjectId;
    bool supportsShadows;
};
```

Path selection becomes:
```
asset capability + feature registry + device capability + view requirement
    → chosen render path
```

Meta-fix goal: eliminate the env-var maze as architecture. `MC2_*`
flags become overrides and diagnostic toggles, not the structural
decision point.

---

### P2-4. Device capability table

Introduce `RenderDeviceCaps` so feature startup can declare
requirements explicitly instead of failing silently:

```cpp
struct RenderDeviceCaps {
    bool supportsCompute;
    bool supportsSSBO;
    bool supportsMultiDrawIndirect;
    bool supportsPersistentMapping;
    bool supportsTextureArrays;
    bool supportsBindlessTextures;
    int  maxSSBOBindings;
    int  maxTextureArrayLayers;
};
```

Feature startup example:
```
StaticPropIndirect: required compute + MDI
MeshletLOD:        required compute + SSBO + indirect
ObjectIDBuffer:    required integer render target
HZB:               required compute or mip-chain fallback
```

Meta-fix goal: no feature silently assumes device support. Also
helps stock-install compatibility checks at startup.

---

### P2-5. One-owner-per-decision matrix

Document which system owns each rendering decision — prevents the
`terrainMVP` / `projectZ` / CPU `pz` / shader-variant split-brain
recurrence.

```
Projection matrix     → View system (F1 UBO)
Visibility            → Visibility service
LOD choice            → LOD service / capability resolver
Material binding      → Material system
Fallback path         → Capability resolver
Draw ordering         → DrawPacket sorter
GPU buffer lifetime   → Resource manager (P2-1)
Debug display         → DebugRenderer
Selection ID          → Object-ID pass
```

This is a living document section, not a code artifact. Start it
inside the RenderWorld boundary spec (A1) as section "ownership
boundary" and maintain it as systems are added.

---

### P2-6. Contract tests for engine APIs

After `RenderWorld` exists, add lightweight smoke tests that validate
API contracts without loading a full mission:

```
RenderWorld creates object  → stable handle valid
destroyObject               → handle invalid, GPU slot deferred-released
setTransform                → ObjectData dirty bit set
createView                  → ViewUniforms buffer updated
computeVisibility           → only requested layers dispatched
feature disabled            → fallback path selected, no GL error
```

Meta-fix goal: engine API changes break fast, not three missions
later. Analogous to the existing parity probes (`MC2_TERRAIN_INDIRECT_CPU_FALLBACK`
crosscheck) but for the API layer.

---

### P2-7. Golden micro-scenes

Complement smoke missions with tiny deterministic synthetic scenes:

```
micro_scene_projection          — one quad, known MVP, verify pixel pos
micro_scene_static_prop         — one building, handle lifecycle, zero visual delta
micro_scene_object_id           — N objects, verify object-ID pixel correctness
micro_scene_lod_hysteresis      — camera crosses band, no single-frame snap
micro_scene_impostor_switch     — far/near transition, no pop
micro_scene_visibility_occlusion — known occluder, verify culled count
micro_scene_material_table      — material index out-of-bounds → no crash
```

Meta-fix goal: big missions catch integration bugs; micro-scenes
catch contract bugs. They run faster and localize failures better.

---

### P2-8. Feature graduation registry

Formalize the `RendererFeature` concept (roadmap item 14) with an
explicit graduation contract per feature:

```cpp
struct RendererFeature {
    const char*        name;
    DefaultState       defaultState;
    Span<const char*>  dependencies;
    Span<const char*>  validationGates;
    const char*        graduationCondition; // "legacy zone ~0 + visual canary"
    const char*        rollbackPlan;        // "force HasLegacyMesh"
};
```

Example entry:
```
Feature: MechMeshletLOD
Default: off
Dependencies: RenderWorld, MeshCapability, meshopt cook, ObjectData
Validation: tier1, object-ID debug, LOD debug, visual canary
Graduation: legacy mech LOD zone ~0, substitutive Tracy proof
Rollback: force HasLegacyMesh path via MeshCapability
```

Meta-fix goal: no immortal experiment flags. Every `MC2_*` flag
either graduates (CPU zone ~0, substitutive proof), stays explicitly
experimental (named, documented), or is deleted.

---

### P2-9. Cooked asset manifest

Once Assimp + meshOptimizer cook path begins (Seq B), add a per-asset
manifest early:

```json
{
  "asset": "buildings/hq",
  "source": "hq.fbx",
  "cookVersion": 3,
  "meshCapabilities": ["LegacyMesh", "LodChain", "Meshlets"],
  "materials": ["hq_wall", "hq_trim"],
  "bounds": { "center": [...], "extents": [...] },
  "dependencies": ["textures/hq_albedo", "textures/hq_normal"]
}
```

Meta-fix goal: runtime never guesses what an asset supports. The
manifest is the link between Sequence B (cook pipeline) and
RenderWorld (handle creation from manifest-described capabilities).

---

### P2-10. Object-ID as engine truth (not just a debug feature)

The roadmap lists the object-ID buffer as item 10 (debug/picking
infrastructure). Treat it as more:

```
hover
selection
"what rendered this pixel?" debugger
bad cull diagnosis
LOD diagnosis (which LOD is this mech?)
meshlet disappearance diagnosis
editor inspect workflow
```

Meta-fix goal: visual output becomes inspectable. This makes future
engine bugs diagnosable in minutes instead of hours.

---

### Negative-space (do not build yet)

- **Full ECS** — data-oriented processors are useful as a design doc concept, not a mandatory C++ type rewrite.
- **Full render graph scheduler** — start with pass contracts and debug assertions; scheduling, aliasing, and barrier management can wait.
- **Full Nanite** — meshOptimizer LODs + meshlets + indirect draw + HZB is the right "Nanite-lite." No visibility-buffer / material-resolve / streaming mega-system until the simpler version proves value.
- **Streaming before streaming-worthy assets exist** — correctly deferred to after meshlet LOD ships (roadmap item 15).
- **RenderWorld including game type headers** — boundary failure. Use legacy adapters.

---

## Load-bearing constraints (do not violate)

### Language standard

```
C++17 is the current stable baseline.
C++20 is deferred until the next API-shape cluster:
  DrawPacket, PipelineDesc runtime, FrameAllocator, AssetCook, or job graph.
No C++20 flip while ImGui / MaterialGpu / EditorBridge are in flight.
```

Rationale: the CXX17 flip itself was a 12-line CMake edit with zero
compile fixes (audit recon
`docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`,
commit `5c03835`, stabilization gauntlet GREEN), but the migration
risk for any language flip lives in the IN-FLIGHT consumers — ImGui
integration, MaterialGpu adoption, EditorBridge work — whose APIs
may shift under a new standard. Hold C++20 for the natural break
between this cluster (RenderWorld arc + adjacent infra) and the next
API-shape cluster (DrawPacket items 11+12, PipelineDesc item 12
Phase 3, FrameAllocator item 13, AssetCook item 8, or the job-graph
direction). Feature use policy: see `docs/cxx17-coding-rules.md`.

---

## Vulkan strategy: saturate OpenGL, then swap the backend (2026-05-22)

```
Saturate OpenGL with modern-engine architecture
while making every system Vulkan-shaped,
then replace the backend once the engine API is stable.
```

This is NOT "write a Vulkan renderer." It is building the renderer/engine model
first, then swapping the API underneath it. The distinction matters: "port to
Vulkan" gives you the same old engine with new API pain; "make engine
Vulkan-shaped" lets OpenGL prove the architecture before you pay the Vulkan
migration cost.

### The Vulkan-prep line

Every new OpenGL feature should pass this test:

```
Could this be expressed as:
  - a resource handle
  - a pipeline descriptor
  - a pass descriptor
  - a descriptor/binding table
  - a command / draw packet
  - or a RenderWorld API call?

If yes  → Vulkan-prep.
If no   → GL-state creep.
```

```cpp
// Good — Vulkan-prep
ctx.bindPipeline(pipeline);
ctx.bindView(view);
ctx.bindMaterialTable(table);
ctx.dispatch(pass);

// Bad — GL-state creep
// random subsystem calls glUseProgram/glUniform/glBindTexture directly
```

Rule: **No renderer feature depends on implicit GL state as its architecture.**

### OpenGL saturation checklist (exploit before Vulkan)

```
UBO/SSBO for all global/object/material data
Persistent-mapped ring buffers where safe
Compute shaders for cull/bin/build-lists
glMultiDrawElementsIndirect for all major paths
Texture arrays / atlases / optional bindless
Debug labels + timer queries everywhere
Explicit PipelineDesc over raw GL state
DSA-style resource creation
Object-ID and debug render targets
HZB mip-chain generation via compute
```

---

## Feature tier ladder (2026-05-22)

Execute tiers in order. Do not start Tier 2 before Tier 1 is stable.

### Tier 1 — Engine contracts (makes everything else safe)

```
RenderWorld boundary spec                        ← DONE (renderworld_arc_status.md)
PipelineDesc runtime type                        ← not started (Phase 3)
RenderPassDesc assertions (render_contract Phase 2) ← SHIPPED (MC2_RENDER_CONTRACT_ASSERT=1)
RendererFeatureRegistry                          ← SHIPPED + unit-tested (COUNT=26)
Shader interface schema (UBO/SSBO layout source of truth) ← not started
ResourceLifetime taxonomy                        ← PHASE 1 DOC SHIPPED (2026-05-26)
DebugRenderer namespace                          ← M1 + DEBUG-VIEW-REGISTRY-1 SHIPPED
Object-ID buffer                                 ← FULLY REALIZED
Contract check aggregator                        ← SHIPPED (check-contracts.sh, 8 checks)
Substrate unit tests                             ← SHIPPED (23 test cases / 2015 assertions)
Engine View Registry                             ← SUBSTRATE SHIPPED (item 7 prerequisite)
Render Resource Registry                         ← SHIPPED (metadata only; 6 resource IDs)
```

Tier 1 does not make screenshots prettier immediately. It prevents pretty
features from becoming spaghetti.

### Tier 2 — UE4/MW5 visual baseline

```
PBR MaterialGpu contract (static props first)    ← static props DEFAULT-ON; mech specular V1 gated OFF
                                                     mech smoothed normals DEFAULT-ON (mode 2)
                                                     mech ambient 0.15 DEFAULT-ON
HDR scene color buffer                            ← not started
Tonemap + bloom post stack                        ← not started
Stable CSM shadows                               ← TUNING-1 + DYNAMIC-RESTORE-1 SHIPPED (default ON)
                                                     terrain-object flush-order fix SHIPPED
                                                     shadow debug views + resource registry wired
HDRI sky                                          ← SHIPPED (SkyRenderAdapter + tinyexr EXR)
Clean SSAO/GTAO-lite (new path, not deleted one) ← not started
Decal system v1                                   ← not started
GPU particle/VFX substrate                        ← PARTIAL: FX-GPU B3c default ON + VFX-AGE-SAMPLE-1
                                                     (MC2_VFX_AGE_SAMPLE default OFF — real age curves)
Terrain material modernization                    ← not started
Water visual modernization                        ← MDI water + sky tint gated (default OFF); recon done
```

Full Track V spec: `docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md`.

### Tier 3 — Track G: Geometry scale (Nanite-lite)

```
Assimp import + offline cook
meshOptimizer LODs + meshlets
.cdag / .meshpack sidecars
MeshCapability flags
HZB culling
Indirect cluster draws
Impostor generation
```

Mechs/buildings first. Terrain uses pieces of it; terrain meshlet is the
ruled deferred LOD slice.

### Tier 4 — World density and richness

```
Detail-cell instancing (rocks/debris/grass)
Impact decals + persistent battlefield scars
Far-building impostors with hysteresis bands
LOD hysteresis (switch_to_lower at D1, switch_to_higher at D2 < D1)
Streaming/residency budget
Importance-based preload (combat relevance + selected + distance)
```

For an RTS, density and readability matter more than close-up fidelity.

### Tier 5 — Vulkan backend

Only after Tier 1-2 contracts exist:

```
RenderDevice
CommandList
DescriptorSet-ish binding model
PipelineDesc → VkPipeline
RenderPassDesc → Vk render/dependency model
Buffer/Texture handles → Vulkan resources
FeatureRegistry → device capability resolver
```

At that point, Vulkan is not a rewrite. It is a backend swap.

---

## Engine module boundary

Hard module boundaries prevent the `RenderWorld.cpp` that secretly includes
`Mech3D.h` six months from now.

```
RenderCore        — types, handles, contracts, base math
RenderDeviceGL    — GL backend (all raw GL calls live here)
RenderWorld       — scene API (createObject/destroyObject/setTransform/render)
AssetCook         — Assimp/meshOptimizer offline pipeline
MaterialSystem    — PBR material tables, texture arrays
Visibility        — VisibilityRequest/Result, cull dispatch wrappers
TerrainRenderer   — terrain surface, chunks, LOD
MeshRenderer      — static props, buildings, mechs (indirect draw)
VfxRenderer       — particles, beams, decals
DebugRenderer     — bounds draws, frustum overlays, object-ID inspection
GameAdapters      — LegacyRenderAdapter classes (temporary bridge)
```

**Firewall rule (load-bearing):**
```
GameAdapters may include gameData headers (BldgAppearance.h, Mech3D.h, etc.).
RenderWorld and everything below may NOT include any gameData header.
Violation = boundary failure.
```

---

## Job graph direction

Not a generic job system first. Named jobs with explicit dependencies:

```
ExtractRenderSnapshot
BuildRenderWorldUpdates
ComputeVisibilityRequests
BuildDrawPackets
UpdateStreamingResidency
CookPendingAssets
```

Dependency graph:
```
ExtractRenderSnapshot
    ↓
ComputeVisibilityRequests → BuildDrawPackets → Render
    ↓
BuildRenderWorldUpdates

CookPendingAssets → AssetRegistry → RenderWorld
```

This gives idTech-like throughput discipline without overengineering a generic
job system first. Start with `std::async` or simple thread pool; the
dependency structure matters more than the scheduler implementation.

---

## Borrowed feature board (source → invariant → MC2 implementation)

| Source | Invariant to borrow | MC2 implementation |
|---|---|---|
| idTech | Frame architecture — orchestration/extraction/GPU owns draw | ExtractRenderSnapshot + draw packets + compute cull |
| idTech | Dense lights/decals via compute binning | LightGpu[]+DecalGpu[] → screen-tile bin → compact lists |
| idTech | Every feature has counters + debug view + cost + fallback | Expand [SUBSYS v1] + RendererFeatureRegistry + DebugRenderer |
| Nanite/Unreal | Geometry is data-driven, hierarchical, GPU-oriented, inspectable | meshOptimizer cook → meshlets/.cdag → HZB cull → indirect |
| Unreal | Material instances — materials are data, not shader variants | MaterialGpu SSBO + PBR contract |
| O3DE/Godot | Pass/resource declarations, feature modules | RenderPassDesc assertions + PipelineDesc + module boundaries |
| Bevy | Extract → render world → prepare/queue/render separation | GameAdapters → RenderWorld → DrawPackets → backend |
| OpenRA/Spring | RTS usability, moddability, determinism | collaborator lane (scripting/editor/AI); our piece = object-ID + audit logs |
| Vulkan engines | Explicit binding, pipeline objects, descriptor-like resources | PipelineDesc + binding tables + RendererFeature requirements |
| Filament | PBR material math, IBL/CSM/SSAO/HDR/tonemap reference implementation | Embedding Filament; steal discipline + math, not the library |
| RenderDoc | Capture-oriented GPU debugging discipline | Replacing object-ID / DebugRenderer (they complement, not compete) |
| bgfx | API-shape inspiration for backend abstraction (GL/Vulkan/D3D/Metal) | Swapping backend before engine contracts are stable |
| KTX-Software/BasisU | Texture containers, mip chains, Basis Universal GPU compression | Custom texture-pack format before KTX2 pipeline is proven |
| SPIRV-Reflect/Cross | Shader binding/layout validation from compiled SPIR-V in CI | Hand-written YAML schema as source of truth |
| gltfpack | One-command asset optimization baseline (vertex cache, quantize, LOD) | Runtime glTF dependence |
| glTF-Transform | Reproducible scriptable cook pipeline (meshopt, KTX2, ETC1S/UASTC) | Web-style asset assumptions |
| Taskflow | Header-only C++ dependency graph job system | General job system before named job graph exists |

---

## Project identity and competitive positioning (2026-05-22)

### The lane

```
Not "better Godot"    — Godot is general-purpose, editor-first, broad ecosystem
Not "smaller O3DE"    — O3DE is AAA-industrial, cathedral-scale, tooling-heavy
Not "Bevy clone"      — Bevy is full-ECS Rust, general engine ergonomics
Not "OpenRA clone"    — OpenRA is 2D/isometric classic RTS, modding platform
Not "Spring/Recoil"   — Spring/BAR is large-scale RTS simulation/multiplayer

But:
  A modern, open-source, RTS-specialized 3D engine
  with GPU-driven visibility, explicit render contracts,
  cooked mesh LODs, engine-as-API boundaries,
  and Vulkan-prep resource discipline.
```

### Where this engine outcompetes after roadmap lands

| Axis | Projected position |
|---|---|
| General engine completeness | Behind Godot / O3DE / Bevy |
| Editor / tooling | Far behind Godot / O3DE (collaborator lane) |
| RTS specialization | Potentially top-tier open-source |
| 3D RTS renderer architecture | Potentially very strong |
| GPU-driven culling / indirect | Strong, especially for RTS scale |
| Meshlet / Nanite-lite path | Promising — proven when cook + runtime ships |
| Asset pipeline | Behind until Assimp/cook/manifest lands |
| Modding / scripting | Collaborator lane (TechScript/Lua/AI/editor) |
| Debuggability / auditability | Could be unusually strong |
| Vulkan-readiness | Strong if explicit binding discipline holds |
| Maintainability | Could become a defining advantage |

### Division of labor (load-bearing — do not scope into collaborator lane)

```
User (this roadmap):
  Renderer, engine architecture, asset pipeline,
  performance, visual fidelity, Vulkan-prep

Collaborator:
  GUI, mission editor, animations,
  TechScript / BattleTech Lua+C++ scripting,
  AI brains (pathfinding, unit behavior, etc.)
```

### Four-track model

```
Track R — Renderer architecture (this doc)
  RenderWorld, ViewUniforms, PipelineDesc, RenderPassDesc, VisibilityRequest,
  FeatureRegistry, ResourceLifetime, DebugRenderer, object-ID

Track G — Geometry scale (Tier 3 of this doc)
  Assimp → offline cook → meshOptimizer LODs → meshlets → .cdag/.meshpack
  → MeshCapability → HZB cull → indirect cluster draws → impostor gen
  Full seed: docs/superpowers/specs/2026-05-22-engine-convergence-roadmap.md Tier 3

Track V — Visual fidelity
  PBR → IBL → CSM → HDR/bloom → SSAO → decals → particles → terrain → impostors
  Full doc: docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md

Track C — Command readability
  Zoom presentation ladder → unit icon system → group/lance aggregation
  → TacticalVisibilityService (S11) → sensor/LOS overlays → threat/range arcs
  → strategic map mode → contact certainty states → order preview rendering
  Full seed: docs/superpowers/specs/2026-05-22-command-readability-zoom-presentation-seed.md
```

Track R must precede all other tracks. Track G overlaps R and V but is its own pillar.
Track V features must enter through Track R contracts — no raw GL state ownership,
no one-off material layouts, no per-program camera uploads.
Track C sits on top of Track R (uses RenderWorld, DebugRenderer, object-ID) and
alongside Track V (same viewport, complementary rendering layers). The command-tool
logic (orders, AI hooks, scripting) is the collaborator's lane; the rendering of
overlays, icons, and zoom-driven presentation rules is this track.

**Division of labor for Track C:**
```
User's lane (rendering):
  zoom presentation ladder rules, unit icon rendering, overlay geometry generation,
  LOS/sensor overlay draw, threat arc rendering, tactical view mode rendering,
  icon LOD/aggregation, contact state visualization, order-preview path rendering

Collaborator's lane (simulation):
  actual order system, AI behavior hooks, scripting integration, mission editor
  integration, pilot/unit state management
```

### The SoTA RTS engine pillars this roadmap covers

```
Covered by Track R + Track V:
  2. Modern GPU-driven renderer
  3. Deterministic / debuggable visibility and selection
  4. Scalable terrain + units + buildings
  6. Engine-as-API boundary
  7. Open, inspectable, maintainable architecture

Not yet attacked (collaborator lane or future):
  1. Large-world RTS simulation support
  5. Mod / content pipeline
```

### North-star statements

**Engine architecture statement (from advisor 2026-05-22):**
```
A fully open-source, modern 3D RTS engine built around explicit
engine APIs, GPU-driven visibility, scalable terrain/unit rendering,
deterministic debugging, and data-driven content —
OpenGL today, Vulkan-shaped tomorrow.
```

**Product experience statement (from advisor 2026-05-22):**
```
A fully modern 3D BattleTech RTS:
MW5-style mech spectacle at close range,
WARNO-style battlefield command at long range,
with deep LOS, sensors, heat, and tactical overlays.
```

Alternatively phrased as the internal design rule:
```
Make the battlefield look like MechWarrior,
but command like WARNO.
```

The secret stolen from WARNO:
```
The map remains visually grounded, but information density
scales up as you zoom out. The player never "leaves" the battle —
they shift from seeing vehicles to reading the battlefield.
```

BattleTech gives MORE than WARNO at the icon/overlay layer because the system
depth (heat, shutdown, sensor lock, ECM/ECCM, pilot quality, damaged limbs/crits,
visual vs sensor detection, active probe) maps directly onto readable per-unit state.
A WARNO-style icon can only show position and type. A BattleTech icon can show:
silhouette (chassis class), border (team/lance), fill (armor/structure health),
heat strip (thermal load), badges (jump jets, ECM, sensor lock, shutdown),
and ring (weapon reach / sensor field / selection state). This is a feature,
not a constraint.

---

## S-series: future candidate pillars (advisor input, 2026-05-22)

Beyond the R/G/V three-track model. These are named, sequenced behind the current
roadmap, and explicit enough to plan against when the time comes.

```
S1  Sensor View System           — perception-first rendering (thermal/IR/night/tactical)
S2  Semantic Visibility          — importance-based LOD + cull (not distance-only)
S3  HZB as shared engine service — shared mip-chain consumer across cull/SSAO/decals/AO
S4  Tiled Light/Decal/VFX Binning — compute-binned per-screen-tile lists (idTech invariant)
S5  Temporal Stack               — TAA-lite → TSR-lite → dynamic resolution
S6  Terrain Data Layers          — terrain as world-data platform (nav/cover/sensor/height)
S7  Weather/Atmosphere           — gameplay-readable sky/fog/wetness/dust
S8  Visualizer Framework         — visualizers as first-class engine features, not afterthoughts
S9  Paper Borrow Queue           — structured SIGGRAPH/I3D technique intake process
S10 Perception-First Rendering   — the engine signature: game readability > photorealism
S11 Tactical Sensor Field        — TacticalVisibilityService: unit LOS + terrain height +
                                   sensor channels + contact confidence (sibling to RenderVisibility)
```

Critical framing: S1 and S11 are NOT the same system.
```
S1  (RenderVisibility side)  — how the renderer shows sensor data: ViewMode, material channels,
                               thermal shader, IR colorization, tactical overlay draw
S11 (Simulation side)        — how the engine computes tactical knowledge: heightfield LOS,
                               sensor channels, contact confidence, pilot skill, fog state
```
S11 feeds S1: TacticalVisibilityService produces a visibility mask → S1 ViewMode
consumes it to colorize the frame.

Do not start any S-item before it is unblocked by the tier it depends on. S1 needs
Tier 1 contracts (ViewMode is a RenderView field). S3/S4/S5 need Tier 3 geometry.
S6/S7 need Tier 3 terrain. S2 needs Tier 2 visual baseline + S3 HZB.

---

### S1. Sensor View System (perception-first rendering anchor)

MechWarrior's heat vision, infrared, and tactical overlay are not "features added
to the renderer." They are a design claim: **the renderer serves game perception
first, and photorealism second.** RTS and mech sim modes alike need this.

**ViewMode enum (minimum viable design):**

```cpp
enum class ViewMode : uint8_t {
    Visual,             // standard RGB rendering
    LowLight,           // boosted exposure / desaturated
    Thermal,            // object heat → color ramp
    Infrared,           // emissive + reflected IR via material channel
    TacticalOverlay,    // silhouette + IFF colors + threat overlay
    ObjectIdDebug,      // reuse object-ID buffer for engine debugging
};
```

`ViewMode` is a field of `RenderView` (the multi-view model from roadmap item 7).
When a view is created, its `ViewMode` controls which render passes run and which
material channels are consumed.

**Minimum viable version (S1-MVP):**

Reuse the object-ID buffer to colorize by faction/type/threat level.
No new render pass needed. Gate: `MC2_SENSOR_VIEW` (default OFF). One line of
view-setup code changes which color-output shader the final blit uses.

**Full version (S1-full):**

```
MaterialSensorGpu  — extends MaterialGpu with heatSignature, radarCrossSection,
                     IRReflectance per-material index
SensorUBO          — ambient sensor parameters per view (atmosphericAbsorption,
                     noiseLevel, rangeMax)
Thermal pass       — object-ID → material lookup → heatSignature → thermal LUT
Tactical overlay   — silhouette extraction + IFF color + threat-level overlay
```

`MaterialSensorGpu` is additive to the `MaterialGpu` contract (V1). New SSBO
binding; does NOT replace PBR channels.

**Sequencing:**
```
S1-MVP: after Tier 1 (needs ViewMode slot in RenderView)
S1-full: after Tier 2 V1 (needs MaterialGpu SSBO as base)
```

---

### S2. Semantic Visibility (importance-based LOD and cull)

The existing constraint (`distant_buildings_render_at_lower_lod_never_distance_culled`)
is architecturally correct but distance-only. Real RTS visibility priority is
importance-driven:

```
Priority weight  =  f(distance, is_selected, is_combat_target,
                       is_mission_objective, is_player_unit, on_screen_fraction)
```

**Design targets:**
- Selected mechs hold full LOD regardless of distance
- Mission-objective buildings never drop below LOD-1 (never impostor)
- Combat-active units get +1 LOD tier over their distance-based choice
- Off-screen background units can use LOD-last + impostor
- Hysteresis bands (existing roadmap V8 requirement) apply to LOD transitions

The `VisibilityRequest` API (roadmap item 6 / Seq C1) is the right place to carry
importance hints from game code into the visibility service.

**Integration with S1:**
Under tactical overlay (S1), importance maps directly to rendering:
high-importance targets get full silhouette + highlight; background fills
use simplified geometry.

---

### S3. HZB as shared engine service (Tier 1.5 — R+G bridge)

HZB (Hierarchical Z Buffer) is currently implied in the Tier 3 geometry work
(indirect cluster draws, GPU cull). Treat it as a shared service, not a
per-feature private mip-chain.

**Consumers:**
```
Occlusion cull (Track G)       — indirect cluster draw pipeline
SSAO / GTAO-lite (V5)          — per-pixel ambient occlusion
Decal depth fade (V6)          — soft decal projection
Probe occlusion (S7)           — weather/fog occlusion
Debug AO view (V5 debug mode)  — show raw HZB in debug visualizer
```

**Implementation contract:**
```
HZB is generated by compute (one dispatch) immediately after the depth pre-pass.
It lives in a `PassTransient` SSBO/texture (P2-1 ResourceLifetime).
No consumer allocates or destroys it — the frame allocator owns it.
Any pass that needs depth occlusion declares `requires HZB` in its PassDesc.
```

**Tier placement:** Tier 1.5. Do not add per-feature private HZB mip-chains.
When the first consumer ships (occlusion cull in Tier 3), the service is
established and all Tier 2/S features consume it.

---

### S4. Tiled light/decal/VFX binning (idTech invariant)

Dense per-tile lighting is what gives RTS combat legibility at scale:
dozens of muzzle flashes, explosion glows, and secondary fires on a single
viewport without per-light draw overhead.

**Data model:**
```
LightGpu[]      — persistent/mission SSBO (existing LightsData + VFX dynamic emitters)
DecalGpu[]      — per-frame decal records (V6 depth-projected)
VfxEmitterGpu[] — per-frame GPU particle emitter positions + radii
```

**Pipeline:**
```
Compute dispatch: for each tile → find LightGpu[]/DecalGpu[]/Emitter[] that overlap
                  → emit compact TileLightList / TileDecalList / TileVfxList SSBOs
Fragment shader:  indexed by tile coord → iterate TileLightList → accumulate radiance
```

This reuses the ViewUniforms (F1) tile-to-world projection. The tile list SSBOs
are `PassTransient` (frame allocator, P2-1). Tile size: 16x16 or 32x32.

**When to build:** after Tier 1 contracts (PipelineDesc, FeatureRegistry) and
Tier 2 V7 GPU particle work exist. The particle emitters are the first consumers.

---

### S5. Temporal stack

Visual coherence at RTS camera speeds. Three-stage ladder:

```
Stage 0 (already exists): per-frame stable CSM texel snapping (V3)
Stage 1 (TAA-lite):        2x jitter + history reprojection + blend
                            Reduces aliasing, improves SSAO stability
                            Adds one history RGBA16F buffer
Stage 2 (TSR-lite):        spatial upscale from 75% → 100% + temporal integrate
                            Higher resolution than native at lower GPU cost
Stage 3 (dynamic resolution): scale 50%–100% based on frame GPU timer
```

**Sequencing:** TAA-lite (Stage 1) needs ViewUniforms prev-frame matrix slot.
Add `u_prevWorldToClipGL` to the ViewUniforms UBO when F1 is implemented. Free
at F1 time — trivial UBO field addition.

---

### S6. Terrain as world-data platform

Terrain in MC2 is a GPU rendering surface today. For the full engine arc it
should also be a CPU-accessible data store:

```
Navigation mesh    — passability per hex/tile for unit pathfinding (collaborator lane)
Cover data         — height map → cover score per position
Sensor occlusion   — LOS occlusion for thermal/IR ViewMode (S1)
Height query       — synchronous per-frame height sample for gameplay (already partial)
Scorch/damage      — persistent modification layer (V6 decal integration)
```

**Rendering note:** this does NOT change the GPU terrain surface pipeline.
It adds read-access APIs on the CPU-side recipe data.

**Collaborator interface:** navigation mesh and unit behavior are the
collaborator's lane; the height API and LOS query are engine-side.

---

### S7. Weather/atmosphere as gameplay-readable rendering

MW5-era atmosphere is achievable without full volumetric simulation.

```
Exponential height fog (already partial in terrain shader)
Sun-fog scatter tint (Rayleigh-ish single-scatter approximation)
Wetness pass         — post-rain material roughness modifier (per-tile rain intensity)
Dust/ash overlay     — particle haze layer affecting sensor range (S1 integration)
Day/night cycle      — directional light direction + color animation
```

**Gameplay readable:** weather affects sensor range in S1 ViewMode, not just
visuals. Rain reduces thermal clarity; dust reduces IR range.

**Sequencing:** after V4 HDR/bloom (needs HDR buffer), after S1-MVP (weather
affects sensor view parameters).

---

### S8. Visualizer Framework

The debug mode table (Track V) establishes a discipline: every feature ships
a visualizer. Promote this from a discipline to an engine system.

**Principle:** a visualizer is a named render mode, not a debug printout.

```cpp
struct VisualizeMode {
    const char*   name;           // "material_channels", "cascade_colors", "lod_bands"
    FeatureFlag   requiredFeature; // MC2_PBR_MATERIALS, MC2_CSM, MC2_LOD_STACK
    RenderPassDesc passDesc;
};
```

**Registry-driven:** visualizers registered via `RendererFeatureRegistry` (P2-8)
so a debug menu can enumerate all available modes without hard-coding the list.

**UI:** a single `MC2_VISUALIZE=<mode_name>` env var selects a visualizer.
The game loop does not branch — the renderer injects the mode at the pass level.

---

### S9. SIGGRAPH / I3D paper borrow queue

Not a technology. A process artifact: an explicit list of papers and the
invariants worth borrowing vs. the implementations worth skipping.

```
Borrow invariants, not implementations. Read papers to find the load-bearing
geometric truth; then implement MC2's version from first principles.
```

**Current borrow candidates:**

| Paper / source | Invariant to borrow | MC2 scope |
|---|---|---|
| Virtual Shadow Maps (UE5) | Stable texel density across cascades via projection linearization | V3 CSM snapping |
| Lumen DDGI-lite (UE5) | Probe spacing for RTS scale; irradiance smoothing | S-probes post-V2 |
| FXAA / SMAA (Jimenez) | Edge-detect anti-alias without temporal history | S5 TAA-lite fallback |
| Hierarchical Z-buffer (Greene) | mip-based occlusion rejection depth bound | S3 HZB |
| Clustered Deferred (Olsson) | Screen-tile light list construction via compute | S4 binning |
| Hable filmic curve (Uncharted) | Shoulder/toe shape of ACES-ish tonemap for outdoor RTS | V4 HDR |
| Interleaved sampling HBAO (NVIDIA) | Alternating sample patterns for SSAO bilateral blur | V5 SSAO |
| Impostor rendering (Forest/Two Tribes) | N-view atlas pre-bake + parallax blend at LOD boundary | V8 impostor |

**Process:** before implementing any visual technique, ask "is there a paper for
this?" and extract one-line invariant before looking at implementation code.

---

### S10. Perception-first rendering (engine signature)

The thesis that distinguishes this engine from generic renderers:

```
Combat clarity > photorealism.
A selected mech must read clearly at 60 zoom-out.
A burning building must read clearly through fog.
A heat-signature target must be identifiable in thermal overlay.
Rendering serves the human operating the RTS, not a physics simulation.
```

**Concrete consequences:**

```
LOD policy    — important objects hold fidelity longer (S2 semantic LOD)
Shadow policy — per-object shadow priority, not uniform distance fade
Sensor views  — gameplay-meaningful ViewMode as first-class render concern (S1)
Color design  — faction colors + threat colors + unit status readable at distance
Post stack    — bloom + color grade designed around tactical legibility, not mood
Decals        — scorch, blast, damage readable at tactical zoom, not only up close
```

**This should be the engine's public identity sentence:**
```
An open 3D RTS engine built around perception-first rendering:
every feature serves tactical legibility, from GPU-driven visibility
to multi-spectral sensor views. OpenGL today, Vulkan-shaped tomorrow.
```

---

### S11. Tactical Sensor Field (TacticalVisibilityService)

**Load-bearing framing (capture verbatim):**

```
Render visibility  != gameplay visibility  != sensor visibility

RenderVisibility:   what should the GPU draw for this camera?
TacticalVisibility: what does this unit/player/sensor know?
```

These are sibling services, not one system. The GPU cull pipeline (Track C, `VisibilityRequest`)
answers the render question. This answers the tactical question. They live at the
same layer inside `RenderWorld` but serve different consumers.

---

#### Architecture

```
RenderWorld
  |-- RenderVisibilityService
  |     camera frustum, HZB, GPU cull, draw lists
  |
  |-- TacticalVisibilityService
  |     unit LOS, sensors, fog, contact confidence
  |
  +-- DebugRenderer
        draw LOS contours, sensor fields, contact states
```

**Meta-fix rule (verbatim):**
Do not let this become `Mech3D::drawGreenLOS()` or `ObjectManager::isVisibleButAlsoRadar()`.
One owner: `TacticalVisibilityService` owns tactical knowledge. `DebugRenderer` owns drawing
it. `RenderWorld` provides object/terrain/view data. `gameData` consumes results, not
implementation details.

---

#### Data model

```cpp
struct TacticalSensorSource {
    UnitId    unit;
    float     eyeHeightMeters;
    float     visualRange;
    float     thermalRange;
    float     radarRange;
    float     sensorQuality;
    float     pilotSkill;
    float     ecmResistance;
    uint32_t  teamMask;
};

struct TacticalVisibilityRequest {
    UnitId                sourceUnit;
    TacticalChannelMask   channels;          // Visual, Thermal, Radar, IR, WeaponLOS
    Rect                  worldRegion;
    float                 resolutionMeters;
};

struct TacticalVisibilityResult {
    TextureHandle visibilityMask;    // grid or texture
    BufferHandle  contourVertices;   // optional marching-squares outline
    uint32_t      version;
};
```

---

#### Sensor channels

Do not collapse everything into one `visible` bool. Use channels:

```
VisualLOS     — terrain height + weather + night + pilot optics
Thermal       — heat signatures + thermal occlusion + weather + shutdown state
Radar         — range + ECM + terrain masking + emission visibility
Passive       — heat/noise/emissions, lower certainty
WeaponLOS     — stricter ray/path test, weapon mount height, arc constraints
```

Contact states (progressive confidence model):

```
Unknown → Suspected → Detected → Classified → Identified → Targetable → ConfirmedVisual
```

---

#### Height model

Height is the heart of physical feel:

```
sourceEyeZ = terrainHeight(sourceXY) + mechCockpitHeight + sensorMastBonus + stanceBonus

targetVisibleZ = terrainHeight(targetXY) + targetHeightBias
```

For mechs/buildings, sample multiple heights: feet/base, center mass, cockpit/top,
sensor mast. Visibility is then graded:

```
0.0 = hidden
0.3 = lower body blocked, top visible
0.7 = mostly visible
1.0 = clear line of sight
```

This creates ridge-flank nuance: a mech cresting a hill is partially visible
before it is fully visible.

---

#### Algorithm: heightfield horizon sweep (first version)

```
originHeight = terrainHeight(origin) + mechEyeHeight

For each radial direction:
  maxSlope = -infinity
  Walk outward tile by tile:
    sampleHeight  = terrainHeight(tile) + targetHeightBias
    slope         = (sampleHeight - originHeight) / distance
    visible       = slope > maxSlope - tolerance
    maxSlope      = max(maxSlope, slope)
```

Properties: deterministic, cheap at RTS grid resolutions, naturally handles
hill ridges blocking valleys, works directly against existing terrain height data,
produces a clean mask for drawing.

Run CPU first. Move to GPU compute once stable.

**GPU compute version (later):**

```
Input:  TerrainHeightTexture + UnitSensorSource[] + TacticalVisibilityParams
Output: R8/R16 visibility mask per selected unit (or packed bitset per unit/team)

Dispatch: one thread per output tile — raymarch toward source, compare terrain
          heights, write visibility confidence.
```

---

#### Update rate discipline (do not compute every frame)

```
Selected mech LOS:          every frame or every 100ms
Hovered unit:               on demand
All friendly units:         amortized over frames
Team fog visibility:        2-5 Hz
Sensor contact confidence:  2-10 Hz depending on sensor type
Last-known decay:           time-based scalar
```

The `TacticalVisibilityResult.version` field enables consumers to skip
re-rendering when the result has not changed.

---

#### Drawing pipeline

```
TacticalVisibilityMask
   |
marching squares / contour extraction
   |
green world-space line mesh (dynamic line buffer)
   |
optional translucent fill

Per-channel color:
  green  = visual LOS
  blue   = sensor/radar coverage
  orange = threat/weapon envelope
  dashed = degraded/uncertain
  faded  = last-known / stale
```

Simple first version: CPU marching squares over visibility grid.
Later: compute shader extracts edge tiles → edge list SSBO → indirect line draw
(this fits the future draw-packet/indirect model naturally).

---

#### Pilot skill integration

Pilot skill should NOT just increase range. Richer model:

```
Rookie:   sees blob / uncertain contact, slow classification
Veteran:  identifies chassis class / facing / heat state sooner,
          faster contact refresh, less sensor noise,
          better degraded-LOS detection,
          more accurate last-known position
```

Skill modifies classification speed and confidence, not magic-range numbers.

---

#### Commander/satellite fantasy — free camera, bounded knowledge

```
Commander camera:   can look anywhere (camera frustum is unrestricted)
Commander knowledge: limited by sensors, LOS, radar, intel, last-known contacts

Visible enemy:      full render
Detected not visual: sensor ghost / outline / uncertainty marker
Last known:         faded silhouette + timestamp marker
Unknown:            hidden
```

The camera is free; the information is not. Players can inspect the battlefield
without losing the tactical value of scouts, elevation, night vision, and sensors.

Integration with S1 `ViewMode`: each view mode selects which channel (VisualLOS /
Thermal / Radar) governs what appears "visible."

Integration with V10 object-ID: click a contact → show which unit sees it, which
sensor channel, confidence, and why.

---

#### Staged plan

```
Stage 0: Debug prototype
  One selected mech, CPU heightfield LOS,
  green mask + outline, no gameplay consequences,
  debug hotkey only (MC2_TACTICAL_VIS_DEBUG=1)

Stage 1: TacticalVisibilityService API
  Formal TacticalVisibilityRequest/Result API
  Selected-unit LOS, visual channel only
  DebugRenderer overlay wired up

Stage 2: Sensor channels
  Thermal/radar/IR masks
  Contact confidence and contact states
  Night/weather modifiers, pilot skill modifiers

Stage 3: Gameplay integration
  Targetability gating
  Fog/contact state (Unknown→ConfirmedVisual)
  Shared team vision composition
  Last-known markers
  AI/scripting hooks (collaborator lane)

Stage 4: GPU acceleration
  Compute-generated visibility masks
  Team visibility atlas (one texture per team per frame)
  Contour extraction on GPU
  Amortized update scheduler
```

**Sequencing gates:**
Stage 0: any time (CPU-only, standalone)
Stage 1: after Tier 1 DebugRenderer + VisibilityRequest API exists
Stage 2: after S1 ViewMode slot in RenderView exists
Stage 3: after RenderWorld + GameAdapters firewall is in place (collaborator integration)
Stage 4: after Tier 1 frame allocator + HZB service (S3) exists

---

**Feature name:** Tactical Sensor Field (not just "line of sight"). The real system is
LOS + terrain + height + pilot skill + thermal + radar + night + sensor confidence +
overlays + targetability. This could be a signature feature of the engine.

---

### Process: real-world measurement discipline

Before shipping any visual feature, answer three questions with numbers:

```
1. Legibility: at what zoom level (tiles from camera) is X readable?
   Target: mechs/buildings readable at maximum tactical zoom.

2. Cost: what is the GPU timer delta (ms) for this feature, default scene?
   Capture before and after, same mission, same camera, Tracy GPU zone.

3. Regression: does this feature change any existing parity probe results?
   Run tier1 smoke, diff probe logs.
```

This discipline prevents "it looks better to me" from becoming a shipped feature
that costs 4ms at RTS scale and breaks the parity probe on AMD.

---

## Advisor simplifications (2026-05-22 — second review pass)

### Source-of-truth ownership table (collapse 20+ items into 7 owners)

Every renderer should ask one of these owners, not invent its own answer:

| Owner | What it answers |
|---|---|
| `RenderWorld` | What exists? |
| `ViewUniforms / EngineView` | From where are we rendering? |
| `VisibilityRequest` | What is visible for this view/layer? |
| `MaterialGpu + AssetManifest` | What does this thing look like and what does it support? |
| `DrawPacket + PipelineDesc` | How is it submitted? |
| `ObjectID + DebugRenderer` | What rendered this pixel and why? |
| `RendererFeatureRegistry + DeviceCaps` | Is this feature allowed today? |

This is exactly the Tier 1 content, restated as a lookup table instead of a feature list.

---

### Simplification 1: glTF/GLB as the asset staging format

Do not design the cook pipeline from scratch. Use Assimp only as an importer
for weird/legacy source formats, convert to a constrained internal GLB staging
artifact, then cook to your MC2 runtime format.

```
FBX / OBJ / DAE / legacy MC2 asset
    -> Assimp import (triangulate, gen normals/tangents, degenerate removal)
    -> canonical GLB staging file (offline, not committed to git)
    -> gltfpack / glTF-Transform (optimize, quantize, meshopt compress, KTX2 textures)
    -> MC2 cook:
         MeshGpu buffers
         MaterialGpu table + KTX2 texture array
         MeshCapability flags
         .meshpack / .cdag sidecar
         manifest.json
```

`gltfpack` optimizes glTF/GLB for GPU consumption: vertex cache, quantize, merge
meshes, meshopt compression, simplify LODs on request.
`glTF-Transform` is a scriptable pipeline for meshopt/Draco compression, texture
resizing, WebP, KTX2, UASTC, and ETC1S — gives you a reproducible asset pipeline
without writing the tools from scratch.

**Negative space:** do NOT make the runtime a glTF engine. glTF/GLB is the
import/staging ABI between source art and the MC2 cook step. The runtime still
consumes your cooked buffers.

---

### Simplification 2: meshoptimizer/clusterlod.h deletes most of the Nanite-lite design

The roadmap already points at meshOptimizer LODs + meshlets + `.cdag`. The enabler
is stronger than implied. `meshoptimizer::clusterlod.h` implements continuous LOD
by generating a hierarchy of clusters that are progressively grouped and simplified,
"similarly to Nanite," explicitly positioned as usable as-is or as a starting point.

**Reframe Track G:**
```
Do not design Nanite-lite first.
First, wrap meshoptimizer outputs into the asset manifest.
Only build custom cluster logic where meshoptimizer fails RTS constraints.
```

First deliverable is not "cluster DAG runtime." It is a manifest entry:
```json
{
  "mesh": "buildings/hq",
  "hasLodChain": true,
  "hasMeshlets": true,
  "hasClusterLod": true,
  "lods": [...],
  "meshlets": [...],
  "bounds": [...],
  "materialSlots": [...]
}
```

Runtime just asks `MeshCapability`. The manifest is the contract.

---

### Simplification 3: Object-ID as Tier 1.5 mandatory inspection substrate

Promote from "debug/picking infrastructure" to the inspection substrate that
makes every subsequent feature debuggable without a full editor:

```
Click pixel ->
  RenderObjectHandle
  MeshHandle + MaterialHandle
  LOD level
  PipelineId
  DrawPacket index
  Feature path (which render path selected this?)
  Visibility source (which cull dispatch admitted it?)
```

PBR wrong? Click pixel. LOD wrong? Click pixel. Shadow wrong? Click pixel.
Decal wrong? Click pixel. This turns every later feature into an inspectable
feature before a real editor exists.

**Move object-ID from "Tier 2 prerequisite" to Tier 1.5** — ship it immediately
after `PipelineDesc` and `RenderPassDesc` assertions exist.

---

### Simplification 4: SPIR-V reflection as the shader schema validator

The roadmap proposes a YAML-or-similar schema for UBO/SSBO binding layout.
The schema is correct as a concept. Hand-written YAML as source of truth is not.

```
GLSL source
    -> glslangValidator -> SPIR-V (in CI / tooling)
    -> SPIRV-Reflect or SPIRV-Cross
    -> compare reflected bindings/layouts against C++ struct definitions
    -> CI fails when C++ and GLSL disagree
```

`SPIRV-Reflect` extracts descriptor bindings, push-constant sizes, and full layout
data for UBO/SSBO blocks from SPIR-V bytecode. `SPIRV-Cross` provides SPIR-V
reflection and high-level language disassembly.

The meta-fix becomes:
```
Do not generate shaders yet.
First, fail CI when C++ and GLSL disagree.
```

This prevents the exact layout drift the YAML schema targets, with less fragile
tooling and no hand-written schema file to drift from the truth.

---

### Simplification 5: MaterialGpu + KTX2 texture arrays in one milestone

Texture compression is part of the V1 PBR material milestone, not a later
asset-pipeline concern. The `MaterialGpu` struct should index into a
KTX2-backed texture array from day one.

KTX is a lightweight texture container for OpenGL, Vulkan, and GPU APIs;
KTX files contain mipmaps, cubemap arrays, block-compressed formats, Basis
Universal (UASTC/ETC1S) formats, or uncompressed textures. Basis Universal
can transcode quickly to any GPU-supported format at load time.

```cpp
struct MaterialGpu {
    uint albedoKtxIndex;       // index into KTX2 texture array
    uint normalKtxIndex;
    uint mrKtxIndex;           // metallic-roughness packed
    uint aoKtxIndex;
    uint emissiveKtxIndex;
    uint wetnessMaskIndex;     // V11 surface state
    uint sensorChannelIndex;   // S1 thermal/IR mask (optional)
    uint flags;
};
```

**Negative space:** do NOT chase bindless textures first. Texture arrays + KTX2
+ material SSBO indices are enough for a long time and are more Vulkan-shaped
than ad-hoc per-draw GL binds.

---

### Roadmap edits (advisor-specified)

**Edit 1: Rename "Render graph" to "Pass Contract Registry"**

Do not call it a render graph internally until it schedules or owns resources.
Phase 1 is documentation structs; Phase 2 is debug assertions; Phase 3 is
enforced. The name "render graph" invites building Granite/O3DE-scale machinery.

```
Use:  RenderPassContractRegistry
Not:  RenderGraph
```

**Edit 2: First RenderWorld slice = write-only / route-only**

The first migration target must not own extraction, lifetime, sorting, visibility,
and fallback all at once:

```
BldgAppearance adapter
    -> RenderWorld::upsertStaticProp(desc)
    -> existing GpuStaticPropRegistry path
    -> same indirect commands
    -> same pixels
```

Rules for the first slice:
```
No new renderer behavior.
No new LOD decision.
No new material model.
No new visibility model.
Only a new boundary.
```

**Edit 3: PresentationBand feeds the capability resolver**

Track C's `PresentationBand` should feed the same resolver as LOD:

```
asset capability
+ feature registry
+ device caps
+ view requirement
+ presentation band       <- from Track C / ZoomBand
+ importance              <- from S2 semantic visibility
= chosen render path
```

This deletes per-system "if zoom > X" branches from every renderer.

**Edit 4: Add `RenderPathDecision` as a logged object**

```cpp
struct RenderPathDecision {
    RenderObjectHandle   object;
    MeshCapability       meshCaps;
    RenderableCapability renderCaps;
    PresentationBand     band;
    RendererFeatureMask  features;
    DeviceCaps           device;
    ChosenPath           path;
    const char*          reason;
};
```

Then `[RENDER_WORLD v1]` can emit aggregated summaries:
```
[RENDER_PATH v1] StaticPropIndirect=1842 LegacyMesh=37 MissingMaterial=5 IconOnly=112
```

This makes fallback behavior auditable instead of magical. Audit logs are the
existing `[SUBSYS v1]` precedent — `RenderPathDecision` is that pattern promoted
to a first-class logged type.

---

### Expanded enablers / repos to steal from

| Enabler | Invariant to steal | What NOT to steal |
|---|---|---|
| `meshoptimizer` / `clusterlod.h` | LOD gen, meshlets, cluster hierarchy seed ("similarly to Nanite") | Full custom Nanite clone |
| `gltfpack` | One-command asset optimization baseline | Runtime glTF dependence |
| `glTF-Transform` | Reproducible scriptable cook pipeline | Web-style asset assumptions |
| Assimp | Broad source-format import (40+ formats, normals/tangents/triangulate) | Loading source assets at runtime |
| KTX-Software / BasisU | Texture containers, mip chains, Basis Universal GPU compression transcoding | Custom texture-pack format too early |
| SPIRV-Reflect / SPIRV-Cross | Shader binding/layout validation from compiled SPIR-V | Building a full shader language |
| Bevy renderer | Extract → prepare → queue separation (render world copy, no ECS required) | ECS rewrite |
| RenderDoc | Capture-oriented debugging discipline, GPU frame inspection | Replacing your own object-ID / debug views |
| Filament | PBR/material/post-process reference (IBL, CSM, SSAO, HDR bloom, tone mapping) | Embedding Filament as your renderer |
| bgfx | API-shape inspiration for backend abstraction across GL/Vulkan/D3D/Metal | Swapping backend before engine contracts |
| Taskflow | Header-only C++ dependency graph job system (low-friction if named jobs grow) | Generic job system before named jobs exist |

Bevy's Extract stage copies only the render data needed from the main world into
the render world; Prepare sets up GPU data; Queue sets up rendering jobs. That
is almost exactly the `RenderSnapshot / RenderWorld / DrawPacket` direction.

Filament is the best visual-fidelity steal: compact real-time PBR engine with
OpenGL/Vulkan backends covering IBL, CSM, SSAO, HDR bloom, tone mapping, color
grading — the full Track V feature set. Steal its material discipline and PBR
math. Do not embed it.

---

### Negative space (explicit do-not-build list — updated)

1. **Full ECS** — processors are a design concept, not a C++ rewrite
2. **Full render graph scheduler** — pass contracts + debug assertions first; no aliasing/barrier scheduler yet
3. **Full Vulkan backend** — OpenGL saturation is the work
4. **Full Nanite** — meshoptimizer + HZB + indirect + object-ID inspection first
5. **Runtime glTF** — cook offline, always
6. **Streaming/residency** — defer until LOD/impostors exist
7. **Virtual texturing / terrain clipmaps** — deferred behind terrain LOD in V9
8. **Deferred renderer rewrite** — forward/forward+ plus explicit material tables is enough
9. **Full editor** — object-ID + DebugRenderer + forced LOD/material/debug overlays = 60% of editor value
10. **GPU-driven everything** — CPU is fine for orchestration, manifests, logs, low-count decisions
11. **General job system first** — named job graph (ExtractRenderSnapshot, CookPendingAssets, etc.) is enough; Taskflow if it grows
12. **Hero weather/heat FX before PBR + particles** — V11 ladder is correct: wetness-only → steam after particle substrate

---

### Revised near-term sequence (data first, behavior second, pretty third)

```
 0. Camera zoom clamp lower + near-plane sanity
 1. RenderWorld boundary spec (doc only)
 2. StaticProp / Building LegacyRenderAdapter — route-only slice
 3. Object-ID buffer + click-to-inspect debug path
 4. RendererFeatureRegistry — top 10 active flags only
 5. PipelineDesc + PassContractRegistry debug assertions
 6. MaterialGpu + KTX2 texture array — static props only
 7. Assimp -> canonical GLB staging
 8. gltfpack / glTF-Transform / KTX2 cook experiment
 9. meshoptimizer LOD + meshlet bake for one building
10. MeshCapability + RenderPathDecision logs
```

This gives visible payoff, better debugging, and asset-cook runway without a
giant architectural cliff.

---

### Capping meta-fix principle (verbatim)

> Every feature should enter the engine as **data first**, **behavior second**,
> and **pretty output third**.

```
Manifest before loader.
Capability before fallback.
Object-ID before editor.
MaterialGpu before PBR polish.
Pass contract before render graph.
DrawPacket before backend abstraction.
PresentationBand before zoom-specific render hacks.
```

---

## C++ standard strategy (advisor input, 2026-05-22)

### Current state (audited)

```
Main build (MSVC):       no CMAKE_CXX_STANDARD set — defaults to MSVC 2022 C++14
Linux/GCC path:          -std=c++0x (old GCC alias for C++11, root CMakeLists.txt:31)
Unit tests only:         CMAKE_CXX_STANDARD 17 (tests/unit/CMakeLists.txt:25-26)
```

This is the split: tests can use C++17 features; the main build has no guarantee.
The immediate fix is to set the standard once at the root.

---

### Migration plan

**Phase 1 — Set C++17 floor (do now, single CMake change):**

```cmake
# root CMakeLists.txt — add after project(mc2)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

Also remove the `-std=c++0x` Linux override (line 31) — it is now redundant and
overrides the policy. Fix any build breaks that surface; do not do style rewrites.

**Phase 2 — ModernCore header utilities (new engine code only):**

Add small types used only in new engine-facing code:

```cpp
Handle<T>          // typed stable handle (index + generation)
GenerationId       // generation counter type
EnumFlags<T>       // type-safe bitflag wrapper
NonOwningPtr<T>    // documents intent; same layout as raw pointer
// std::span already available at C++20; use gsl::span shim at C++17 if needed
```

Do not force legacy `gameData` / `mclib` / `code/` files to use these immediately.

**Phase 3 — C++20 for new engine modules (after RenderWorld boundary spec):**

Once the following modules exist as new code, compile them at C++20:

```
RenderCore      RenderWorld     RenderDeviceGL
AssetCook       MaterialSystem  Visibility
DebugRenderer
```

Do not force legacy files to C++20. Use per-target `target_compile_features`:

```cmake
target_compile_features(RenderWorld PRIVATE cxx_std_20)
```

**Phase 4 — Selective C++20 feature adoption:**

```
Use:
  std::span              — engine API boundaries (DrawPackets, VisibleObjects, etc.)
  std::bit_cast          — GPU/debug code, packed IDs, binary asset formats
  std::jthread           — job graph workers (ExtractRenderSnapshot, CookPendingAssets)
  concepts               — Handle/resource template utilities; use sparingly
  constexpr improvements — RendererFeatureRegistry, binding-point tables

Avoid (for now):
  modules                — too disruptive with mixed legacy headers
  coroutines             — no clear use case yet
  ranges in hot loops    — profile first; SIMD/direct array beats ranges at render scale
  std::pmr rewrite       — not load-bearing
  global smart-ptr migration — not the bottleneck
```

---

### The value is not speed — it is contract clarity

For this project, the upgrade payoff is:

```
std::span<const DrawPacket>         instead of DrawPacket* + size_t count
std::span<const MaterialGpu>        instead of MaterialGpu* + uint32_t n
[[nodiscard]] RenderObject          instead of hoping callers check the return
std::optional<RenderObject>         instead of returning a null handle + separate bool
std::bit_cast<uint32_t>(float)      instead of memcpy aliasing trick for GPU packing
std::jthread + stop_token           instead of hand-rolled "running" flag + join
```

That aligns exactly with the roadmap: `RenderWorld`, explicit handles, draw packets,
visibility requests, `PipelineDesc`, feature registries, shader schemas, resource
lifetime contracts. C++20's `std::span` in particular is purpose-built for
non-owning views over GPU-adjacent contiguous arrays.

---

### Example: RenderWorld at the C++20 API point

```cpp
struct RenderFrameDesc {
    RenderView                         mainView;
    std::span<const RenderObjectUpdate> objectUpdates;
    std::span<const LightUpdate>       lightUpdates;
    float                              deltaSeconds;
};

class RenderWorld {
public:
    [[nodiscard]] RenderObject  createObject(const RenderObjectDesc& desc);
    void                        updateObjects(std::span<const RenderObjectUpdate> updates);
    [[nodiscard]] VisibilityResult computeVisibility(const VisibilityRequest& request);
    void                        render(const RenderFrameDesc& frame);
};
```

This is much cleaner than pointer/count pairs and much safer than `std::vector`
everywhere. The span carries no ownership, which is exactly right for APIs that
hand arrays to the GPU.

---

### What to avoid: modernization theater

```
Do NOT: "upgrade to C++20 and rewrite everything modern."
That is a month of work producing zero visible progress.

Do NOT: jump to C++23 as a project-wide baseline.
Toolchain support is less boring; MSVC conformance still has nuances.

DO: tie every standard upgrade to a specific new module or API surface.
The upgrade earns its keep only at the RenderWorld / engine-API boundary.
```

---

## Load-bearing constraints (do not violate)

- **Re-charter rule**: terrain/surface work re-charters as NS2/structural (NOT CPU-cost claim), gated behind clean non-COST_SPLIT total-frame Tracy. LOD in-scope from day one. See `memory/terrain_continuous_surface_forks_ruled_option1_killlegacy.md`.
- **Substitutive not additive**: a GPU slice is not done until the CPU original zone reads ~0 in a fresh Tracy capture. Adding GPU paths while CPU paths still run = failed slice. See `memory/feedback_offload_must_be_substitutive_not_additive.md`.
- **Adversarial review required** for any "all callers must X" contract (new cross-cutting rule). See `.claude/skills/adversarial-plan-review.md`.
- **Distant buildings never distance-culled**: lower LOD at distance, never culled. See `memory/distant_buildings_render_at_lower_lod_never_distance_culled.md`.
- **Stock install must remain playable**: modern paths degrade to stock-compatible data generation. See `memory/stock_install_must_remain_playable.md`.
- **Vulkan-prep discipline**: new GPU-resource code uses device-mediated binding, zero implicit cross-call GL state. See `memory/vulkan_prep_explicit_device_discipline.md`.
- **Track V / Track R dependency gate**: Track V features may not introduce new raw GL state ownership, new one-off material layouts, or new per-program camera/projection uploads. Every Track V feature must enter through Track R contracts: ViewUniforms, PipelineDesc, RenderPassDesc, FeatureRegistry, and DebugRenderer.
- **Three-track model**: Track R = renderer architecture; Track G = geometry scale / asset cook; Track V = visual fidelity. Track G (Assimp → meshOptimizer → .cdag → MeshCapability → LOD/impostors/HZB) is its own pillar, not a sub-item of R or V.
