# MC3 Rendering Modernization — End-to-End Roadmap

> **Strategic frame for the rendering modernization arc.** Sits alongside
> `cpu-to-gpu-offload-orchestrator.md` (operational status board) and the
> per-slice specs/plans/brainstorms. The orchestrator says what's shipping
> next; this roadmap says what we're shipping toward.
>
> **Keep this doc under ~300 lines.** Architectural framing only — slice
> mechanics live in specs, status lives in the orchestrator.

---

## Project framing

This is **MechCommander 3, built on the MC2 open-source release.** Keep the
gameplay feel (missions, mech roster, controls, art direction). Replace the
engine internals.

- Full rationale: `memory/mc3_modernization_philosophy.md`
- Stock-install constraint: `memory/stock_install_must_remain_playable.md`
- Cull-cascade constraint (load-bearing): `memory/cull_gates_are_load_bearing.md`

**Default at every architectural fork:** GPU over CPU; modern pattern over
legacy pattern; replace over optimize-in-place. Resource abundance
(multi-GB RAM/VRAM) is assumed.

---

## End state — what "done" looks like

Modern GPU-driven rendering. The pattern (Frostbite/idTech 7/UE5):

```
Mission load (one-time):
  ├─ Persistent static-instance SSBOs (transforms, AABB, partial baked lighting)
  └─ Material/atlas resolution        (cement-style multi-sampler dispatch)

Per frame:
  ├─ CPU upload: dynamic actor visibility records  (mechs/GVs/gates/turrets)
  ├─ Compute cull dispatch        (frustum + distance over static + dynamic
  │                                records, ~50-100µs for 10K instances,
  │                                bucket-keyed scatter-write)
  ├─ glMultiDrawElementsIndirectCount (GPU-written instanceCount, one
  │                                  command per mesh-range bucket)
  ├─ Vertex shader pulls instance (gl_BaseInstance + gl_InstanceID indirection
  │                                via per-bucket visible-ID list)
  └─ Async readback (1-frame lag) feeds CPU's update/lifecycle gates

CPU per-frame work is:
  - AI/sim ticks (independent of visibility — read frame-N-1 GPU result)
  - Mech/GV bone updates (one matrix per actor; Track D's territory)
  - State transitions (damage, destruction, gameplay events)
  - Dynamic actor visibility-record upload (compact per-actor SSBO write)
  - Indirect-draw dispatch (a handful of GL calls)
```

**The half-frame "should this be culled?" cost evaporates.** It moves to
~50-100µs of GPU compute. The cull GATES remain, fed cheaper input — no
cascade.

**GL substrate.** Engine baseline stays at GL 4.3 core (matches the
`#version 430` shader-prefix discipline from the worktree CLAUDE.md). Track
C requires GL 4.6 OR (GL 4.3/4.4 + `ARB_indirect_parameters` +
`ARB_shader_draw_parameters`); a startup probe selects the path or refuses
Track C activation. AMD RX 7900 XTX supports 4.6 core directly; the
extension path is defensive. **Vulkan/D3D12 port DEFERRED**
(not absolutely skipped — see "Explicitly NOT doing" table below for
the post-stability reconsideration). For the current arc: no Vulkan
work because the state-surface multiplier and lack of RHI abstraction
make it net-negative until the GL-side primitives are exhausted. **Mesh
shaders DEFERRED with Vulkan** — `GL_NV_mesh_shader` may or may not
be exposed by AMD's GL driver (runtime extension-string verification
required before assuming); reliable path is Vulkan. Marginal win at
our scale would still apply even with the API available. **HZB occlusion** is back on the table
as a Q21 candidate after the camera model correction
(`memory/camera_model_oblique_cinematic.md`) — earlier "RTS top-down
payoff too small" framing was wrong; the camera is oblique 30° + 360° +
cinematic low-angle, which DOES surface real occlusion (mech behind mech,
mech behind terrain ridge, geometry behind buildings). HZB now needs a
proper brainstorm. Full rationale: session 2026-05-06.

**Dropped from end-state** (post-advisor-review consistency): mission-load
"ever-visible pre-cull" was in earlier drafts but Track B's recon §8
measured ~0% benefit on stock content. Listed here as documented future
optional offline optimization, not part of the shipping pattern.

---

## Substrate audit — what's already built

| Modern primitive | MC2 equivalent today | Status |
|---|---|---|
| Persistent terrain instance SSBO | `gos_terrain_indirect::g_denseRecipes` | ✅ Default-on 2026-05-02 |
| Per-frame thin record (delta) | M2 thin records, WaterThinRecord | ✅ Shipped |
| Indirect draw plumbing | `gos_terrain_indirect` SOLID PR1 | ✅ Default-on 2026-05-02 |
| Persistent static-prop instance buffer | `GpuStaticPropRegistry` (slice 3.C) | ✅ Default-on 2026-05-05 |
| Material atlasing (one bucket = one draw) | Cement-catalog tex3 multi-sampler | ✅ Shipped 2026-05-01 |
| Predicate isolation | `projectFor*` named wrappers (8 categories) | ✅ Shipped 2026-04-26 |
| Persistent-mapped buffer pattern | thin record SSBOs + recipe SSBOs | ✅ Routine |
| Tracy GPU/CPU profiling | Always-on, 18 zones | ✅ Built-in |
| Parity-validation infra | `MC2_*_PARITY_CHECK` env-gated byte compare | ✅ Established pattern |
| GPU compute cull | _none_ | ❌ |
| Async-readback feedback to CPU gates | _none_ | ❌ |
| GPU vertex skinning (animated objects) | _none_ | ❌ slice 2 queued |

The two missing CPU→GPU primitives are the focus of the roadmap below.
Everything else is incremental refinement on substrate that already works.

---

## The roadmap — tracks in dependency order

### Track A — Predicate replacement (fixes wolfman; no cascade) ⭐ start here

Replace the rect-screen-finite predicate with a frustum + distance test in
the wedge-class wrappers. Same gate, better math. Slices land per-wrapper.

**Slice ordering (revised 2026-05-06 brainstorm):**

1. **A1 — object admission** (1 site, `code/gameobj.cpp:2090`). Smallest
   blast radius; validates the swap pattern.
2. **A2 — effects admission** (7 sites: cloud/crater/weather). Inherits
   the slice-1 pattern.
3. **A3 — terrain admission** (6 sites: `mclib/terrain.cpp:1438` clone +
   `:1597` original + 4 others). **Re-evaluated post-A1+A2** — the project's
   own capture-replay data (`projectz-capture-report.md` §5) verdicts
   terrain admission with High confidence as the *worst* candidate (~36%
   over-cull cross-mission; 74% rectGuard-permissive concentration).
   Only proceed if residual gap is worth the wedge risk after A1+A2 ship.

- **Slice scope (per slice):** dual-output wrapper. New predicate for bool;
  legacy `projectZ` for `screen.x/y/z/w`. The screen output is consumed by
  HUD overlays and gameplay code (verified per-slice via grep) and stays
  byte-identical to legacy. **Hard parity requirement.**
- **Parity gate (per slice):** dual-run legacy + modern predicates with
  reviewed acceptance envelope. Disagreement is the *signal of migration*,
  not the failure mode — a predicate replacement that produces zero
  disagreements is a no-op. Hard failure conditions: (a) `[DESTROY v1]`
  count delta vs baseline, (b) tier1 5/5 visual smoke regression,
  (c) disagreements outside the reviewed envelope (zoom band / camera
  angle / object class / distance band the envelope didn't account for).
  Screen-output byte-identity is the only "= 0" parity required.
- **Prereqs:** none. (RAlt+P overlay GL state bug — earlier-listed prereq
  — already fixed in commit `dec89aa`.)
- **Retires:** legacy rect-finite predicate per wrapper as each slice
  ships; wrappers without a slice keep the legacy predicate.
- **Size:** A1 ~1 week. A2 ~1-2 weeks (7 sites). A3 deferred and conditional.

### Track B — Widen static-prop registry to "every static prop"

Promote `GpuStaticPropRegistry` from "fast-path replay of cull-approved
instances" to "single source of truth for all world-static-prop geometry in
the mission." Per-instance transforms pre-baked once; per-frame churn =
visibility bit + dynamic-light cache refresh.

- **Prereq:** Track A slice 1 ships clean. (Slice 2/3 of Track A do not
  block Track B — only the swap pattern needs validation.)
- **Components:**
  - Mission-load bulk registration of known static props at
    `objmgr.cpp:1132 addObject` between map-load and `finalizeGeometry()`.
  - Register-on-spawn API for late types (artillery, vTOL, mech-bay-mid-mission).
    Gameplay code calls `registerStaticProp()` at spawn time. **No
    first-render lazy fallback** — explicit layer-separation discipline
    (per Q4 decision in `specs/2026-05-06-track-abc-brainstorm-decisions.md`).
  - **Scope: world-static-prop population only.** GenericAppearance is
    descoped (only stock instances are HUD: `Cylinder01`, `compassplane`,
    handled via existing allowlist at commit `06ac847`). Two registration
    paths preserved: world-prop registry vs. HUD allowlist. Boundary
    documented.
  - Persistent instance SSBO sized for total registered population (not
    visible).
  - TGL pool sizing audit (currently 500K post-`4888084`; verify per tier1
    mission peak).
  - **Mission-load bake of *static* fields only** (`modelMatrix`,
    `firstColorOffset`, `flags`, `aRGBHighlight`, `fogRGB`). Per-frame
    `cachedFrame_`-stamped lighting cache **preserved** for dynamic lights
    (weapon flashes, fires, mech spotlights consumed via global
    `s_listOfLights` at `txmmgr.cpp:959`). Earlier "static lighting input
    is also static" framing was wrong — partial bake only.
  - **First-frame race fix is structural by default.** Mission-load
    registration runs before `update()` populates `cachedFrame_`; without
    intervention the flush would drop every static draw on frame 0
    (guaranteed-visible blank-world flicker, not a maybe-imperceptible
    artifact). Fix: at registration time, set `cachedFrame_` to
    `currentFrame - 1` so the flush invariant treats the entry as
    valid-this-frame (~5 lines). `[STATIC_FIRST_FRAME v1]` env-gated
    counter ships alongside as proof the structural fix works
    (count must be zero); non-zero indicates the pre-populated stamp
    doesn't satisfy the flush invariant — escalate to baseline-lighting
    fallback or revisit invariant directly.
- **Descoped from Track B:**
  - "Ever-visible" pre-cull at mission load — recon §8 measured ~0% benefit
    on stock content.
  - GenericAppearance — see scope note above.
- **Exit:** registered population matches enumerated mission spawns + late
  spawns; `submit_legacy` = 0 across tier1; pool peaks bounded by registered
  count; +0 `[DESTROY v1]` count delta vs baseline; visual parity at all
  zoom levels including wolfman; `[STATIC_FIRST_FRAME v1]` data captured.
- **Retires:** per-frame `TransformMultiShape` for static prop categories;
  per-frame ARGB **static-field** rebuild (dynamic light bake stays).
- **Size:** ~3 weeks. Week 1 conditional on factoring "build batch without
  submitting" out of `submitMultiShape`/`getLastBuiltBatch` for the
  mission-load walk.

### Track C — GPU compute cull + async-readback feedback (the killshot) ⭐

The architectural endpoint. Compute shader reads the GPU visibility-record
substrate (Track B's static instances + C0's dynamic actor records) plus
camera frustum, writes per-bucket visible-ID lists + atomic counts.
Indirect draw consumes via `gl_BaseInstance`. Async fence reads the
visibility result 1 frame later and feeds it into existing CPU lifecycle
gates — the cull GATES stay, their INPUT moves to GPU.

**Splits into four sequential slices** (revised 2026-05-06 post-advisor-review).
Each slice has its own gate and rollback; the big-bang risk of monolithic
Track C is removed.

- **Prereq (track-level):**
  - Track B (static-instance persistent buffer).
  - **GL version probe at engine init.** Track C requires GL 4.6 OR
    (GL 4.3/4.4 + `ARB_indirect_parameters` + `ARB_shader_draw_parameters`).
    `glMultiDrawElementsIndirectCount` and `gl_DrawID`/`gl_BaseInstance`
    vertex-shader access are the load-bearing primitives. AMD RX 7900 XTX
    supports 4.6 core directly; the extension path is defensive for
    future hardware portability. Startup logs `[GPU_CULL v1] gl_version=...
    support=4.6|extensions|none`; refuse Track C activation if neither
    path is available. **Baseline engine GL context stays 4.3.**

#### C0 — Dynamic actor visibility record substrate

CPU per-frame upload of compact visibility records for dynamic actors
(mechs, GVs, gates, turrets, weapon emitters). Common cull-input schema
with Track B's static records — compute shader doesn't distinguish.

- One record per dynamic actor: `id`, `worldAABB` (or sphere), `category`
  (Mech / GV / Gate / Turret / Other), `prevVisibilityBit`, gate-consumer
  flags (`hasAIConsumer`, `hasWeaponSpawnConsumer`).
- No vertex skinning dependency; bone matrices stay where they are
  (Track D's territory).
- **Exit:** dynamic actor record count matches `objmgr` dynamic-actor
  enumeration; record contents validated against legacy `recalcBounds()`
  output for AABB equivalence; AMD canary builds clean.
- **Size:** ~3-5 days.

#### C1 — Compute cull for render draw only

Compute shader fills per-bucket visible-ID lists and atomic counts.
Indirect draw consumes them. **CPU lifecycle gates still use legacy
`inView` at this stage** — render path moves; gates don't. Validates the
load-bearing GPU contracts in isolation.

- **Compute shader:** frustum-plane + distance test, single combined
  dispatch over Track B + C0 record buffers, bucket-keyed scatter-write
  via atomic per-bucket counters. **Per frame: one dispatch + required
  GPU memory barriers + indirect draw.** No CPU readback in C1 except
  optional debug-only telemetry. The readback ring and fence/fallback
  behavior begin in C2 — this slice validates GPU→GPU rendering only.
- **Bucket key (sharpened post-advisor-review):** mesh-range +
  shader-program + texture-binding-set + VAO + index-type. One
  `DrawElementsIndirectCommand` per bucket with mesh-static fields
  (`firstIndex`, `baseVertex`, `count`) populated at mission load; GPU
  writes per-bucket `instanceCount` from atomic counter at compute time.
  **Material-only bucketing is rejected** — indirect commands carry
  per-mesh-geometry fields; material-only bucketing breaks unless every
  mesh in the bucket shares pre-packed geometry tables. Options B (per
  mesh+material with binding minimization) and C (single material pass
  with shader-side mesh indirection) are documented escalations,
  not-adopted in C1.
- **Compute output:** visible-ID list + atomic count per bucket. Vertex
  shader reads instance via `gl_BaseInstance + gl_InstanceID` indirection
  through visible-ID list. Compute-writes-`DrawElementsIndirectCommand`-direct
  is documented as future escalation if profiling motivates.
- **Synchronization contracts (must be in C1 plan):**
  - **Counter representation.** Per-bucket counters are `uint` fields
    inside the cull-output SSBO, updated with shader `atomicAdd`. Not
    OpenGL atomic-counter buffer objects (ACBOs). This determines the
    barrier set below — choose-and-stick.
  - **Counter reset.** Per-bucket counters cleared at the start of every
    cull dispatch via `glClearNamedBufferSubData` on the SSBO region (or
    a small clear kernel). Reset must complete before cull dispatch reads.
  - **Capacity + overflow.** Per-bucket visible-ID list sized for
    worst-case (= total bucket population). Shader-side bounds check on
    the atomic counter; emit `[GPU_CULL v1] overflow=` log on hit; never
    silently drop.
  - **`instanceCount` patch.** GPU writes into the pre-built command
    buffer's `instanceCount` field (offset 4 bytes per std430).
  - **Memory barriers (SSBO-atomics model):**
    - Cull dispatch → indirect draw consumption:
      `GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT`. The SSBO
      bit covers both the visible-ID list AND the in-SSBO atomic counters
      (since they're SSBO-resident `uint` fields).
    - Cull dispatch → CPU async readback (introduced in C2):
      `GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT` + `glFenceSync`.
  - **Do NOT add `GL_ATOMIC_COUNTER_BARRIER_BIT`** unless the implementation
    deliberately switches to OpenGL atomic-counter buffer objects. The
    barrier bit is for ACBOs specifically; using it on SSBO-resident
    `atomicAdd` counters is a cargo-cult error.
- **Exit:** indirect-draw output visually matches legacy CPU-cull at all
  zoom levels including wolfman; tier1 5/5 PASS; +0 `[DESTROY v1]` delta
  (lifecycle gates untouched at this stage); AMD canary verifies barrier
  + atomic-counter behavior; `[GPU_CULL v1] overflow` count = 0 across
  tier1.
- **Size:** ~1 week.

#### C2 — Async readback into non-lifecycle consumers

Validates the readback ring + fence + fallback paths without risking the
`setExists(false)` cascade.

- CPU reads N-1 visibility for non-destructive consumers first: Tracy
  visibility plot, `[GPU_CULL v1]` summary line, debug overlay. Pick
  smallest-blast consumer at C2 plan time.
- **Async readback fallback contract:** CPU consumes only completed ring
  entries (frame N-1 with fence signaled). If frame N-1's readback isn't
  ready, fallback in priority order: (1) frame N-2's last-good readback,
  (2) conservative-visible (assume all actors visible — never under-cull),
  (3) never block on `glClientWaitSync` in the render-loop hot path.
- **Exit:** readback ring proven non-stalling under tier1; fallback paths
  exercised via injected fence-not-ready cases; non-lifecycle consumers
  produce sensible output for ≥30s of gameplay.
- **Size:** ~3-5 days.

#### C3 — Gate handoff + `Camera::UpdateRenderers` stub

The actual perf payoff. After C1+C2 soak clean (≥1 week tier1 default-on):
route lifecycle gates to GPU-derived visibility.

- `objmgr::update`, `Mech3DAppearance::update`, `GVAppearance::update`
  consume frame N-1's GPU visibility instead of frame N's CPU `inView`.
- AI gate (`code/mech.cpp:6497`) and weapon-spawn-node queries
  (`mclib/mech3d.cpp:721,759,795,833`, `mclib/gvactor.cpp:445,500,533`)
  read the same GPU-derived (1-frame-lagged) visibility.
- `Camera::UpdateRenderers` becomes a stub (Tracy verifies).
- **Hard exit criteria:** half-frame CPU "should this be culled?" cost
  replaced by ~50-100µs compute dispatch; tier1 5/5 PASS; +0 `[DESTROY v1]`
  delta; pool peaks unchanged; FPS gate ≥30% improvement at wolfman zoom;
  **`Camera::UpdateRenderers` is a stub.** The stub criterion is
  load-bearing — preserving CPU `recalcBounds` for AI/weapon consumers
  (the rejected option B in Q7) is disallowed under the engine/content
  layer framing.
- **AI/weapon 1-frame artifact framing (revised post-advisor-review):**
  accepted as a **gameplay-tolerance tradeoff**, not because it's
  categorically imperceptible. At 60Hz one frame is ~16.7ms; at 30Hz
  ~33ms; during hitch-recovery longer. Validated via explicit visual
  canaries on zoom-transition, camera-jump, first-contact, and
  weapon-spawn scenarios before the C3 default-on flip. Reversibility
  hedge: if shipped behavior turns out observably annoying, a separate
  engine API for synchronous visibility can be added at specific
  call sites — paying the cost only there as a content-layer concern.
- **Retires:** `recalcBounds()`-based per-actor screen-space cull;
  `Camera::UpdateRenderers` becomes a stub.
- **Size:** ~1 week.

**Total Track C size:** ~3-4 weeks across C0/C1/C2/C3 (up from monolithic
~2 weeks, but with per-slice gates).

### Track D — GPU vertex skinning (mechs / GVs / animated objects)

Already on the orchestrator's queue as "object offload slice 2." Move
`TG_Shape::TransformShape` per-vertex skinning + lighting bake from CPU
to GPU vertex shader. Bone matrices uploaded per actor per frame; vertex
shader pulls bone weights from instance SSBO.

- **Prereq:** Recon Zero per existing prompt
  (`specs/2026-05-02-object-offload-slice2-recon-zero-prompt.md`):
  enumerate every consumer of `listOfShadowTVertices` (used by
  `RenderShadows`), pick path 2-a/2-b/2-c.
- **Components:**
  - Per-mech bone matrix SSBO (~30 matrices × 64B × N mechs = trivial).
  - Vertex shader skinning + per-vertex lighting (gouraud, baked-static
    light contribution + dynamic light contribution).
  - Shadow caster path: either move shadow vertex transform to GPU
    concurrently (2-a), or keep reduced CPU path for shadow-only data
    (2-b), or accept smaller win (2-c).
- **Exit:** ~2ms `appearanceUpdate` cost recovered; tier1 5/5 PASS; visual
  parity (mech damage decals, salvage states, paint schemes preserved).
- **Retires:** `TransformMultiShape` per-frame CPU work for animated actors.
- **Size:** ~3-4 weeks (depends on shadow path decision).

### Track E — Legacy retirement + cleanup (post-soak)

Once Tracks A-D are default-on and have soaked clean for ≥2 weeks of stock
play, physically delete:

- `TerrainPatchStream::flush()` (M0/M1/M2 shipped paths superseded by
  indirect terrain).
- M2 thin-record-direct emit, M2b/c/c-ext/d branches.
- The `MC2_*` opt-out env flags (kept during soak, removed at retirement).
- `recalcBounds()` rect-screen-finite math (replaced in Track A; deleted
  here once Track C ships).
- The `g_useGpuStaticProps` killswitch infrastructure (RAlt+0) — its bypass
  pattern is no longer needed once GPU-driven is the default.

Mechanical work, no new design. Adversarial review applies (legacy
retirement is a triggering condition).

---

## Adjacent tracks (shipped under separate roadmaps, listed for orientation)

These tracks share substrate with the rendering arc but have their own
specs / brainstorms / leads:

- **Modding sidecar layers:** `memory/modders_paradise_roadmap.md`,
  `memory/methuselas_techscript_proposal.md`. Decal/overlay/footprint
  consolidation will leverage the same cement-multi-sampler primitive.
- **UI modernization:** `memory/imgui_fit_ui_design.md`. ImGui replaces
  the UI renderer; FIT stays the modder contract. Methuselas-led; editor
  parallel.
- **Mod content workstreams:** Carver5O, Magic, MCO Omnitech, MC2X,
  Wolfman are all OUT OF SCOPE for the rendering roadmap (validation gate
  is stock content only — `memory/feedback_offload_scope_stock_only.md`).
- **Asset pipeline:** texture upscaling (4x ESRGAN) + AssetScale subsystem
  + loose-file overrides. Already shipping; orthogonal to render arc.
- **AI/sim decoupling:** the 200ms `GameLogic` spike is a future concern
  (pathfinding activation when camera reveals new area). Not blocked by or
  blocking this arc; revisit after Track C makes everything else cheap
  enough that AI cost dominates.

---

## Explicitly NOT doing (with reasoning)

| Decision | Why |
|---|---|
| Vulkan / D3D12 port — **DEFERRED** (not absolute skip) | State-surface multiplier ~3-4× over GL; no RHI abstraction; AZDO + GL 4.3-4.6 extensions get ~95% of the win for the current arc. Stays deferred until Tracks A/B/C/D ship and the rendering arc stabilizes. **At that point Vulkan port becomes a real option** — perf ceiling on GL once modern primitives are exhausted, plus unlocking of Vulkan-only features (mesh shaders being the big one), makes the port economics different post-stability. Bookmark for a future "Track H: Vulkan port" decision after Track E retirement. |
| Build an RHI abstraction now | Same reason. Single-target codebases run faster without one. (Reconsider if Track H Vulkan port becomes real — at that point an RHI is the natural way to keep the GL fallback alive during migration.) |
| Mesh shaders / Nanite-style — **DEFERRED** (tied to Vulkan port) | Native OpenGL mesh-shader support is `GL_NV_mesh_shader` (NVIDIA-original); AMD's GL driver may expose it via runtime extension query — verify per build before assuming. Reliable path is Vulkan, where AMD ships first-class mesh-shader support. Mesh shaders ride along with the Vulkan-port deferral above. Marginal win at ~10K-instance scale would still apply even with the API available — per-instance cull (Track C) catches most of what per-meshlet cull would. Reconsider only if a future content roadmap pushes per-mesh poly counts or instance counts an order of magnitude higher. |
| ~~HZB occlusion culling~~ ⚠️ NOW UNDER REVIEW (Q21 candidate) | Earlier "RTS top-down payoff too small" framing was incorrect. Camera is oblique 30° + 360° + cinematic — see `memory/camera_model_oblique_cinematic.md`. Real occlusion (mech-behind-terrain-ridge, mech-behind-building, low-angle foreground occlusion) means HZB has genuine payoff. Q21 brainstorm pending; reclassify out of "Skip" and into "Future arc" after Q21 closes. |
| Fix the rect-screen-finite predicate | Replace it (Track A). Repairing it is fighting the wrong battle. |
| Bypass the cull gates | Cascade hazard documented in `cull_gates_are_load_bearing.md`. Replace, don't bypass. |
| CPU SIMD on `vertexProjectLoop` | Compiler-ceiling outcome already measured (`memory/vertexproject_loop_asymptotic.md`). The slice belongs on GPU (Track D), not as CPU SIMD. |
| Migrate to non-Anthropic ML stacks | Out of band — engine modernization, not tooling. |

---

## Approximate sequencing

```
NOW (queued/in-flight)
├─ Slice 3.C/3.D static-prop registry soak (default-on 2026-05-05)
├─ Cement multi-sampler bundle soak
└─ Object offload slice 2 Recon Zero (gating)

NEAR (next ~5 weeks)
├─ Track A1 — Object admission predicate                   [~1 wk]
├─ Track A2 — Effects admission predicate                  [~1-2 wk]
├─ Track B — Widen registry to all static props            [~3 wk]
└─ Adjacent: mc2res→FIT Phase 2 (Methuselas)               [parallel]

MID (~6-9 weeks out)
├─ Track C0 — Dynamic actor visibility record substrate    [~3-5 d]
├─ Track C1 — Compute cull for render draw only            [~1 wk]
├─ Track C2 — Async readback into non-lifecycle consumers  [~3-5 d]
├─ Track C3 — Gate handoff + UpdateRenderers stub          [~1 wk]
├─ Track D — GPU vertex skinning (slice 2)                 [~3-4 wk, parallel-able with C]
└─ Adjacent: ImGui+FIT UI track milestones                 [parallel, Methuselas-owned]

LATE (~12-14 weeks out)
├─ Track A3 — Terrain admission predicate (conditional)
├─ Track E — Legacy retirement
├─ Tracy hygiene: post-arc zone cleanup
└─ Adjacent: decal/overlay/footprint sidecar layers
```

Total rendering arc: **~12-14 weeks of focused work** to the architectural
endpoint (revised up from ~10-12 weeks after Track C's per-slice split and
Track A reordering). Adjacencies (UI, modding, asset pipeline) ship in
parallel and do not gate the arc.

---

## Approved execution order (post-advisor pass on plans)

The advisor pass on A2/B/C produced a specific approved order. Each step
is a hard gate — do not start the next step until the prior step's exit
criteria are met.

1. **A1 Tasks 1-7** (predicate + env + trace + wrapper + envelope +
   DESTROY parity verification). Ends pre-soak.
2. **Human review of A1 envelope + DESTROY identity capture.**
3. **A1 soak** (≥3 days under tier1 default-on-modern).
4. **A1 default-on flip** (Task 9) only after soak passes the
   five-criterion gate.
5. **A2 Tasks 1-6 under A1-already-modern** (sequential-with-overlap
   per Q15). A2 enters the production-relevant joint configuration
   directly.
6. **A2 soak under joint A1+A2-modern** (≥3 days), then A2 flip
   (Task 8).
7. **Alpha-test prep slice ships first** (parallel-tracked spec at
   `specs/2026-05-06-static-prop-alpha-test-self-awareness.md`),
   independent of A1/A2. Track B execution starts AFTER the prep
   slice ships.
8. **Track B Task 1 spike** — resolves Q16 (`firstColorOffset`
   ownership) before any B implementation work. Spike outcome
   (Candidate A/B/C) determines Tasks 2-9 shape.
9. **Track B Tasks 2-10** under prep-slice baseline + Q16 commitment.
   Three hard constraints (no `void*` cast-compat / Q16 written
   decision / fallback retirement gated on invariant) enforced
   throughout.
10. **C0 may execute independently** — schema substrate slice, no
    compute, no readback, no gate handoff. Can land any time once C0
    schema header lands.
11. **C1 GATED on Track B substrate ready + Q16 closed + Q17 chosen
    (block-active rollup path A or B).** No C1 execution without all
    three.
12. **C2 after C1 ships + soaks.**
13. **C3 GATED on C2 ship + soak + Q18 lights preflight audit.**
    Q18 audit is a pre-rewiring grep step at C3-0 that determines
    whether lights join the C3 routing list.

Slices that can run in parallel without violating gating: alpha-test
prep + A1 (different code paths), C0 + B (different SSBOs), Track D
(GPU vertex skinning) once its own Recon Zero closes.

Slices with hard sequential gates: A1 → A2, B → C1, C0 → C1 →
C2 → C3.

---

## Risk register

| Risk | Mitigation |
|---|---|
| Cascade hazard if Track B sizes pool wrong | Pool sizing audit per mission before flip; instrumented `[TGL_POOL v1]` already monitors. |
| Track C async readback latency artifacts | Accepted as gameplay-tolerance tradeoff (Q7 framing); validated via explicit zoom-transition / camera-jump / first-contact / weapon-spawn canaries before C3 default-on. |
| Track D shadow-path entanglement | Recon Zero is the gate; three branching paths (2-a/2-b/2-c) costed before plan-write. |
| Late-registerType pointer instability (artillery/bomber spawns) | Already documented; widened registry covers known-late-types via mission-load enumeration of spawn definitions, not first-render registration. |
| Driver quirks on AMD RX 7900 XTX (compute + indirect) | `docs/amd-driver-rules.md` is the running log; canary build before flip. |
| Validation conflated with mod content | Tier1 stock-only gate enforced; `feedback_offload_scope_stock_only.md`. |

---

## Update log

> Append a one-liner when the architecture shifts. Most-recent at top.

- **2026-05-06 (post-advisor-on-plans)** — Advisor pass on the four
  written plans (A1 sharpened earlier, A2/B/C0-C3 written this session).
  A1 confirmed execution-ready. A2 soak ordering locked as
  sequential-with-overlap (Q15) — A1 must flip default-on before A2
  enters soak; A2 soaks under joint A1+A2-modern, the production
  config. Track B given three hard constraints before execution: HC-1
  no `void*` cast-compat for Bldg/Tree registration (use typed setters
  or shared interface); HC-2 Task 1 spike must commit to a written
  `firstColorOffset` ownership decision (Q16; bake-at-register OR
  patch-per-frame OR recipe-field redesign) before Task 2 begins;
  HC-3 first-render fallback retirement gated on invariant proof, not
  time. Track C C1 gated on Q15+Q16+Q17 resolution AND Track B
  substrate readiness; Q17 (block-active rollup) added as new C1
  task with two paths (GPU compute aggregation OR CPU-side conservative
  walk); C3 gated on Q18 lights preflight audit (any
  `lightAppearance->inView` consumer outside light.cpp/Appearance
  itself triggers lights joining C3 routing). Approved execution order
  enumerated as 13 hard-gated steps; sequential-required gates
  documented. New Q15/Q16/Q17/Q18 sections added to brainstorm-decisions
  doc; cross-cutting revisions extended to items 14-17.
- **2026-05-06 (canonical, post-advisor-pass-2)** — Two cleanup edits
  before freezing as canonical: (1) C1 slice boundary made explicit —
  GPU→GPU only, no CPU readback in C1; readback ring + fence + fallback
  paths begin in C2. (2) Counter representation choose-and-stick: SSBO
  `uint` fields + shader `atomicAdd`, NOT OpenGL atomic-counter buffer
  objects. Barrier set narrowed to `GL_SHADER_STORAGE_BARRIER_BIT |
  GL_COMMAND_BARRIER_BIT` for cull→indirect draw (the SSBO bit covers
  the in-SSBO atomic counters); explicit "do NOT add
  `GL_ATOMIC_COUNTER_BARRIER_BIT`" warning to prevent cargo-culting the
  wrong barrier model. Greenlit for Track A1 plan-writing.
- **2026-05-06 (post-advisor-review)** — Outside-input review absorbed
  into roadmap and decisions doc. Eight sharpenings applied:
  (1) Track A parity gate rewritten as dual-run with reviewed acceptance
  envelope (predicate replacement that produces zero disagreements is a
  no-op — disagreement is the migration signal); screen byte-identity
  preserved as the only "= 0" parity, `[DESTROY v1]` count delta + visual
  smoke + out-of-envelope as hard failures. (2) Track C0 added as
  dynamic-actor-visibility-record pre-slice (closes the static-only gap
  Track B leaves; "Camera::UpdateRenderers becomes a stub" is unreachable
  without it). (3) Bucket key sharpened to mesh-range + shader +
  texture-set + VAO + index-type — material-only bucketing is rejected
  because indirect commands carry per-mesh-geometry fields. (4) Track C
  synchronization contracts (counter reset, overflow, instanceCount
  patch, glMemoryBarrier bits, async readback fallback) documented as
  C1-plan requirements not implementation detail. (5) "Ever-visible
  pre-cull" removed from end-state diagram for consistency with Track B
  descope. (6) GL version harmonized: baseline 4.3, Track C requires 4.6
  OR ARB_indirect_parameters + ARB_shader_draw_parameters with startup
  probe. (7) Q7 AI 1-frame artifact reframed as gameplay-tolerance
  tradeoff (not "below human-perception floor" — overclaim at 60Hz/30Hz
  framerates and during hitch recovery). (8) Track B first-frame race
  promoted to structural fix by default (pre-populate `cachedFrame_` at
  registration); counter is proof-of-fix, not deferral gate. Track C
  splits into C0/C1/C2/C3 sequential slices (~3-4 wk total, up from
  monolithic ~2 wk). Total arc revised to ~12-14 wk.
- **2026-05-06** — Brainstorm decisions absorbed across all three tracks
  (`specs/2026-05-06-track-abc-brainstorm-decisions.md`). Track A reorders
  to object → effects → (re-evaluate) terrain — capture-replay data argued
  against terrain-first. Slice scope formalized as dual-output wrapper
  (new bool, legacy screen). RAlt+P prereq struck (already fixed `dec89aa`).
  Terrain site count corrected to 6. Track B Generic descoped; ever-visible
  descoped; lighting bake narrowed to genuinely-static fields only;
  register-on-spawn API replaces lazy first-render fallback. Track C adds
  4.6 context bump prereq; combined-dispatch + visible-ID-list shape
  formalized; "Camera.UpdateRenderers becomes a stub" promoted to hard exit
  criterion; AI 1-frame visual artifact explicitly accepted on
  layer-separation grounds.
- **2026-05-06** — Roadmap drafted. Reflects the strategic reframe captured
  in `memory/mc3_modernization_philosophy.md` and the GPU-compute-cull +
  async-readback architectural endpoint discussed in session 2026-05-06.
  Replaces the implicit "we'll figure it out as the orchestrator clears"
  shape that prior work was operating under.
