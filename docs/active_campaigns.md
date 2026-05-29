# Active campaigns

Extracted from CLAUDE.md 2026-05-24. SHIPPED slices are kept here for
archaeology; DECISIONS + steady-state pointers stay current. Add new
campaigns at top (most recent first).

For the RenderWorld arc specifically, see also:
- [docs/renderworld_arc_status.md](renderworld_arc_status.md) — steady-state
  ledger reframing M3/M4/M5 as DECISIONS (not ongoing work)
- [docs/renderworld_migration_guide.md](renderworld_migration_guide.md) —
  contributor onboarding for the arc

---

## Shadow lane — dynamic prop shadows working (2026-05-29)

Branch `claude/shadow-lane` (9 commits ahead of nifty, ready to merge).
Full state: `memory/shadow_dynamic_projection_and_caster_feed_fixed.md`.

- **SHADOW-LANE PHANTOM** (SHIPPED `2764cb65`): terrain objects invisible w/
  MC2_SHADOW_ENABLE=1 was a flush-ORDER bug (registry flush after the SSBO slot
  lock). Registry flushed before flushShadow. See
  `memory/shadow_enable_terrain_object_invisibility_resolved.md`.
- **SHADOW-TERRAIN-COMBINE-MIN-1** (SHIPPED `7ea32b83`): terrain shadow combine
  is `min(static,dynamic)` not multiply → no double-darken on overlap.
- **SHADOW-BOUNDED-NEAR-FIT-1** (SHIPPED `d7e95a7f` + center `8428805e`): gate
  MC2_SHADOW_BOUNDED_NEAR_FIT (default OFF) caps the dynamic fit radius for crisp
  near shadows; box centered on screen-center ground-focus ray.
- **SHADOW-STATIC-BUILDINGS-2** (SHIPPED `657d671d`): gate
  MC2_STATIC_PROP_BUILDING_SHADOW (default OFF) replays ALL buildings (registry,
  visibility-independent) into the world-fixed static map; dynamic pass skips
  building types to avoid a fuzzy double-shadow (`8428805e`).
- **SHADOW-DYNAMIC-PROJECTION-FIX-1** (SHIPPED `a365e6ad`): dynamic frustum-fit
  unprojected GL-NDC corners through `inverse(getWorldToClip())` (D3D pixel-homog,
  Y-down) → box MIRRORED, shadows only at certain camera angles. Fixed: invert
  `worldToClipGL()` (GL-NDC + axis-swap baked); drop manual swizzle + w<0 negate.
- **SHADOW-DYNAMIC-PROP-CASTERS-1** (SHIPPED `054ca335`): gate
  MC2_SHADOW_DYNAMIC_PROP_CASTERS (default OFF) feeds the dynamic caster pass from
  the registry (ALL non-building props, visibility-independent) instead of the
  camera-visible s_typeRanges → every tree casts, not just near-camera ones.
  mc2_01 gate-ON 733 props/frame constant, 0 GL err.
- **DEBT:** no light-box cull yet (draws all map props/frame; **HZB planned**);
  camZ=0 focus shim; stale building shadow on destroy; foliage alpha-discard.

## MaterialGpu arc — static-prop complete, Mech-1 substrate done (2026-05-26)

- **MaterialGpu static-prop arc v4–v7** (SHIPPED 2026-05-26): Static props fully
  table-driven for albedo by default. Four commits: v5 (c38c8426) default-ON upload,
  v6 (06962919) docs/invariants locked, v7 (ae2152cd) default-ON shader sampling.
  MC2_MATERIAL_GPU=0 disables upload/bind/compare; MC2_MATERIAL_GPU_SAMPLE=0 disables
  shader sampling and falls back to texArrayLayer. texArrayLayer retained as compare
  authority. Log tag: `[MATERIAL_GPU v4]`. Tier1 5/5 PASS all gates.
  Handoffs: `memory/HANDOFF_2026_05_26_material_gpu_static_prop_complete.md` (v4-v7),
  `memory/HANDOFF_2026_05_26_material_gpu_arc_checkpoint.md` (full arc).

- **MaterialGpu Mech-1** (SHIPPED 2026-05-26): `GpuMechInstance.materialIdx` at byte 52
  (replaces `_pad1`), compare invariant proven (mismatches=0), `[MECH_MATERIAL_GPU v1]`
  log tag. HEAD c2dd0a33. Shader sampling NOT yet wired — pending Mech-2 decision.

- **MaterialGpu Mech-2** (BLOCKED — texture model decision required): texHandle is not
  shader-actionable in the current mech pipeline. Decision doc:
  `docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md`.

- **Static-prop pipeline binder** (STARTED 2026-05-26, commit 9f958536):
  `applyPipeline` wired in `GpuStaticPropBatcher::flush()`. PipelineDesc v1
  (depthFunc/reverse-Z encoding, DrawPacket pipelineId wiring) is the next slice.

---

## DrawPacket arc — v7+v7.1 SHIPPED, default-ON (2026-05-26)

- **DrawPacket v7** (SHIPPED 2026-05-26): Default-ON flip. `s_v6Enabled` true by
  default. Kill-switch: `MC2_STATIC_PROP_LEGACY_DISPATCH=1` reverts to legacy
  `glMultiDrawElementsIndirect`. `run_smoke.py` propagation tuple updated. Tier1
  5/5 PASS both gates. Log tag `[DRAW_PACKET_V6]` fires by default — NOT opt-in.

- **DrawPacket v7.1** (SHIPPED 2026-05-26, HEAD `f780949f`): Dead-code removal.
  Deleted: `StaticPropOpaquePacketView` struct, `batcher_setOpaqueDispatchCandidates()`,
  `s_opaqueDispatchCandidates`/`s_opaqueDispatchCandidateCount`/`s_v4TypeDrawCount` statics,
  v4A flush block (~90 lines), v4A legacy-path suppression guard + `[DRAW_PACKET_SUPPRESS v1]` log,
  v4B coverage-compare block (~80 lines), v4C slot-coverage block (~70 lines),
  deprecated `MC2_DRAW_PACKET_STATIC_PROP_V6` gate plumbing. Net -291 lines.
  `s_v6Enabled` lambda simplified to kill-switch only. Dispatch hierarchy table
  added to `docs/tier1_env_vars.md`. Tier1 5/5 PASS post-cleanup.
  Handoff: `memory/HANDOFF_2026_05_26_drawpacket_v7_shipped.md`.

- **DrawPacket v7.2** (PENDING — 1+ week soak): Remove `MC2_DRAW_PACKET_COALESCE_V5`
  gate plumbing after confirming no disarms. Not yet started.

- **DrawPacket v8** (DEFERRED): Shadow-pass packet dispatch, GPU-cull count
  integration, sortKey population, mech/terrain packets, log-tag normalization
  `[STATIC_PROP_PACKET_DISPATCH v1]`. No spec yet.

---

## In flight / ready to execute

- **Unified-projection F1** (design + plan complete; ready for execution):
  Spec v2.8 greybeard-signed at
  `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`.
  Plan v1.1 codex-signed at
  `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`.
  Handoff at
  `~/.claude/projects/A--Games-mc2-opengl-src/memory/HANDOFF_2026_05_22_unified_projection_F1_ready_for_execution.md`.
  Correctness-only (CPU budget already met per F3 ~55us total post-A2).
  Collapses inline `axisSwap*worldToClip` at `gamecam.cpp:165-187` into
  `Camera::worldToClipGL()`; renames `terrainMVP` → `u_worldToClipGL`
  across 14 vert + 3 compute/frag + 10 CPU bind sites; deletes SSAO
  runtime entirely; Stage A-pre parity probe + Stage A atomic
  single-commit flip. 21 tasks across 4 phases. Execute via
  `superpowers:subagent-driven-development` skill.

---

## Infrastructure / platform

- **C++17 Standard Flip** (SHIPPED 2026-05-24): root CMakeLists.txt now
  sets `CMAKE_CXX_STANDARD 17` (was implicit C++14 from MSVC default;
  three modern modules already C++17 via `target_compile_features`).
  12-line CMake edit, zero compile fixes, tier1 5/5 PASS, no runtime
  behavior change. Allowed-feature rules: `docs/cxx17-coding-rules.md`.
  Audit recon:
  `docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`.
  Stabilization gauntlet:
  `docs/superpowers/explorations/2026-05-24-cxx17-stabilization-gauntlet.md`
  (GREEN). First useful cleanups: inline constexpr handle-base
  constants, std::optional in offline tools, structured bindings in
  tools/tests. NOT runtime renderer churn.

- **Render contract Phase 2** (SHIPPED 2026-05-24): `render_contract.h`
  gains `RequiredAttachments` (which `COLOR_ATTACHMENTx` a pass needs in
  the active draw-buffer list) + `ShaderOutputContract` (per-pass FS
  `layout(location=N)` uniqueness check) + `attachmentCount` in
  `PassStateContract` (expected `glDrawBuffers` arg). New runtime
  asserts gated by `MC2_RENDER_CONTRACT_ASSERT=1` — emit `[RENDER_CONTRACT
  v2] assert mode ACTIVE` on startup; compare live GL state to
  `render_contract::stateContractFor()` declarations and warn on
  mismatch. Tier1 5/5 PASS both modes; zero violations observed at HEAD.

---

## RenderWorld arc — STEADY STATE reached 2026-05-24

See [docs/renderworld_arc_status.md](renderworld_arc_status.md) for the
full ledger with decisions, handle-range partitioning, CI enforcement
layer, and what's NOT an upcoming slice. Quick summary:

- **M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6, M6** SHIPPED
- **M3** SHIPPED (DECISION: terrain deferred indefinitely; CPU
  `worldToTile` canonical)
- **M4** SHIPPED (DECISION: VFX click-through by design; CI-grep
  prohibits attachment-2 writes)
- **M5** DEFERRED INDEFINITELY (overlay had 7 in-tree meanings without
  identity-needing consumer)

Future arc work is opt-in only: M3.1 if editor consumer emerges, M2.7
mech-select-on-click if desired, or a new named slice consuming existing
substrate.

### Per-slice archaeology (verbose entries, kept for grep)

- **RenderWorld Slice M1** (SHIPPED 2026-05-23): static-prop adapter
  routes 5 audited call sites (`mclib/bdactor.cpp:1471,2802,4273,4859`,
  `code/warrior.cpp:7593`) through
  `GameAdapters::StaticPropRenderAdapter` ->
  `RenderWorld::upsertStaticProp` -> `GpuStaticPropRegistry::registerRecipe`.
  Three new modules at repo root: `RenderCore/`, `RenderWorld/`,
  `GameAdapters/`. New virtual `Appearance::getStaticRecipeIndex()`
  enables m5 late-spawn handle adoption. `[RENDER_WORLD v1]` banner
  emits per mission. Firewall: `scripts/check-include-firewall.sh`.
  Greybeard PATCH (justified) — adapter is TEMPORARY per spec section 10.
  Tier1 5/5 PASS; objects counts: mc2_01=997, mc2_03=2552, mc2_10=2611,
  mc2_17=1521, mc2_24=2641. Spec:
  `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`.

- **RenderWorld Slice M1.5** (SHIPPED 2026-05-23): substrate-only
  ObjectID buffer. `MC2_OBJECT_ID_BUFFER=1` env-gated `R32_UINT` MRT
  attachment at `GL_COLOR_ATTACHMENT2` on the main scene FBO; static-prop
  fragment writes `Handle.raw()` via `layout(location=2) out uint
  v_objectId`. RenderWorld API extension: `s_objectRecords`
  always-populated table indexed by `handle.index()` +
  `lookupAtPixel(x,y) -> LookupResult` synchronous readback (generation
  + alive check). C1 META-FIX: `setSceneDrawBuffers(SceneDrawBufferMode,
  bool objectIdAttachmentReady)` helper in `gos_postprocess.cpp`
  centralizes scene-FBO draw-buffer policy across 5 sites. Greybeard:
  META-FIX. Tier1 5/5 PASS env-OFF AND env-ON. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`.

- **RenderWorld Slice M1.6** (SHIPPED 2026-05-23): static-prop pick
  wiring. `MC2_STATIC_PROP_PICK=1` + `MC2_OBJECT_ID_BUFFER=1` enables
  Shift+left-click inspect-only static-prop selection. User-driven
  canary on mc2_03: 26 hits + 11 diagnostic misses. **Note:** Log schema
  `[STATIC_PROP_PICK v1]` and `StaticPropSelectionDebugState` were
  renamed to `[GAMEPLAY_PICK v1] kind=StaticProp` and
  `GameplaySelectionDebugState` in M2.6 META-FIX (commit `ca08d0c`).
  Archaeology: grep `[GAMEPLAY_PICK v1]`. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`.

- **RenderWorld Slice M2-pre** (SHIPPED 2026-05-23): preemptive META-FIX
  refactor of M1.6 gameplay-pick machinery. New TU
  `code/gameplay_pick.{h,cpp}` hosts shared types + `tryGameplayPick(req)`
  dispatcher (env-substrate gate + 4 gesture gates + mover-first
  short-circuit + viewport query + bounds + coord scale +
  lookupAtPixel) + pure `screenToFboPixel(...)` coord transform. New
  automated validator `RunGameplayPickSelfTest()` gated by
  `MC2_GAMEPLAY_PICK_SELFTEST=1`. Greybeard: META-FIX. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`.

- **RenderWorld Slice M2** (SHIPPED 2026-05-23): route-only
  `MechRenderAdapter`. Every live `Mech3DAppearance` instance now has a
  `RenderObjectHandle` stored on it. `GameAdapters/MechRenderAdapter.{h,cpp}`
  bridges `BattleMech::init/destroy` to
  `RenderWorld::registerMech/destroyMech`. Mechs allocate from the
  unified `s_objectRecords` table at `kMechHandleBase=0x00010000`.
  Tier1 5/5 PASS.

- **RenderWorld Slice M2.5** (SHIPPED 2026-05-23): mech object-ID
  substrate. `GpuMechInstance` (std430 SSBO) grows 48B->64B with
  `objectIdRaw` field. `mech.vert` + `mech.frag` write per-instance
  `objectIdRaw` to attachment-2. Always-on per-mission counters split:
  `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` +
  `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M`.
  Per-mission `gpu_mech_id_writes`: mc2_01=63836, mc2_03=19230,
  mc2_10=53872, mc2_17=407061, mc2_24=1232. MLR gap empirically rare:
  all 5 tier1 missions show `mlr_mech_draws=0`.

- **RenderWorld Slice M2.6** (SHIPPED 2026-05-23): mech pickup
  integration (inspect-only v1). Closes the RenderWorld arc's first
  user-visible loop. Shift+LMB on a hostile mech visible to sensors
  emits `[GAMEPLAY_PICK v1] hit kind=Mech ...`. Three new env vars
  (`MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG`).
  Handle→BattleMech reverse-lookup via linear scan over `ObjectManager`
  movers (Option B; partId-cookie rejected). META-FIX retired per-kind
  log+state pattern: `[STATIC_PROP_PICK v1]` → `[GAMEPLAY_PICK v1]
  kind=X`. Latent post-M2.5 mislabel bug fixed simultaneously. Tier1
  5/5 PASS env-OFF AND env-ON. Gate 6 substitutive-proof grep: zero
  hits for retired symbols.

- **RenderWorld Slice M6** (SHIPPED 2026-05-24): firewall audit script
  — no raw GL from game side. Codifies the empirical finding that
  `code/` has ZERO raw GL calls and `mclib/` has 3 diagnostic-only
  gated hits in `render_contract.cpp`. New
  `scripts/check-no-raw-gl-from-game.sh` (function-level grep, NOT
  include-level) with allowlist of exactly one TU. Turns the arc from
  "discipline by memory" to "discipline enforced by script."

- **RenderWorld Slice M3** (SHIPPED 2026-05-24; **DECISION: deferred
  indefinitely**): terrain reservation/deferral. `RenderObjectKind::Terrain
  = 2` + `kTerrainHandleBase = 0x40000` + defensive `lookupAtPixel`
  tripwire. No shaders edited, no adapter, no env var, no consumer.
  Forward-compat: if M3.1 ever ships per-quad terrain identity
  (editor-driven), use `subKind = Base/Water/Decal/Mine` payload (NOT
  separate enum values).

- **RenderWorld Slice M4** (SHIPPED 2026-05-24; **DECISION: never write
  attachment-2**): VFX prohibition + scaffold. `RenderObjectKind::Vfx
  = 3` + `kVfxHandleBase = 0x00080000u` + NEW
  `scripts/check-vfx-no-objectid.sh` firewall grep gate. Additive
  blending + R32_UINT last-write-wins would clobber M2.6 mech-pick
  under particles. Source-game-object lookup stays in game logic.

- **RenderWorld Slice M5** (DEFERRED INDEFINITELY 2026-05-24): "overlay"
  had 7 in-tree meanings without identity-needing consumers. Enum slot
  un-reserved (comment-only deferral note). If a future use case
  emerges, ship as a NEW NAMED slice (HoverKindIndicator /
  RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT as
  "M5 Overlay").

---

## Pending durable artifacts (write-when-ready)

- **Render contract document** at `docs/render-contract.md` (or
  `.planning/codebase/RENDER-CONTRACT.md`) — enumerate at
  function/symbol level: who enqueues into which master array, who
  flushes when, what state is inherited at each hook point, what each
  Track A/B/C/coalesce slice consumes/emits. Currently we burn context
  re-deriving this every render slice. Captured 2026-05-14 from
  codebase-architecture mapping; promote to real artifact when next
  render slice plan lands.
