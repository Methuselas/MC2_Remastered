# GPU Particle Pipeline — Design Spec (B)

- **Status:** DRAFT — ready for plan-phase. Companion to (A) [gosFX retirement](2026-05-20-gosfx-retirement-or-replacement-design.md). Design only; no code changes.
- **Date:** 2026-05-20
- **Authoring branch:** `claude/nifty-mendeleev`
- **Greybeard verdict:** PATCH (justified) — see §3. (A) is the META-FIX; (B) is feature reintroduction onto the clean substrate.
- **Recommended architecture:** **(α) CPU-emit, GPU-batch billboards.** See §3.
- **All file:line citations grep-verified against `nifty-mendeleev` at write-time (Appendix A).**

---

## 1. GPU-side requirements

### 1.1 Effect-type coverage matrix

Derived by reading the gosFX class bodies (`mclib/gosfx/*.cpp`), not class names. Per-type data shape, blend, and sort behavior:

| Type | Per-particle data | Blend | Sort | Hot spec examples |
|---|---|---|---|---|
| `CardCloud` (`cardcloud.cpp:1-885`) | world pos, size, rotZ, color (ARGB DWORD), atlas UV, age. Camera-facing quad. | additive OR alpha (per spec) | depth-sort within bucket | `Fireball`, `Gauss_flare`, `LRM_Smoke`, `Mech_Explosion` |
| `PointCloud` (`pointcloud.cpp:1-487`) | world pos, color, point size. Hardware point sprite or 1px quad. | additive | none required | sparks, embers |
| `ShardCloud` (`shardcloud.cpp:1-671`) | per-particle triangle (3 verts), tumble axis+rate, color. | alpha | depth-sort | shrapnel from `Critical_hit` |
| `Tube` (`tube.cpp:1-1294`) | indexed triangle tube along a path of N "knots"; texture U scrolls along axis | additive | sort by tail-vert | missile contrails, jet trails |
| `ShapeCloud` (`shapecloud.cpp:1-588`) | per-particle `TG_Shape` instance (full mesh) | opaque or alpha | TG_Shape's own depth | rare; complex debris with mesh |
| `DebrisCloud` (`debriscloud.cpp:1-869`) | per-particle `TG_Shape` + 6DOF rigid-body state | opaque | scene depth | building-collapse chunks |
| `PertCloud` (`pertcloud.cpp:1-690`) | per-particle n-gon (3–9 verts per `Max_Number_Of_NGon_Vertices = 9` at `mclib/mlr/mlr.hpp:107`), each vert independently perturbed every tick | alpha or additive | depth-sort | electrical arc effects |
| `EffectCloud` (`effectcloud.cpp:1-339`) | container — spawns child `Effect` instances at particle positions every tick | inherits children | n/a | composite "fireball with smoke and sparks" |

**Volume buckets observed in spawn-site grep** (`code/artlry.cpp`, `code/carnage.cpp`, `code/missiongui.cpp`, `code/terrobj.cpp`, `code/weaponbolt.cpp` + the mech/actor/building chain enumerated in (A) spec §1.3): the **high-volume types are `CardCloud`, `PointCloud`, `ShardCloud`** (weapons / explosions / muzzle / hit / smoke); **low-volume long tail is `Tube`, `ShapeCloud`, `DebrisCloud`, `PertCloud`, `EffectCloud`**.

### 1.2 Per-particle pool size estimate

Worst-case (mc2_24 combat-heavy, six mechs in firefight): ~50 effect instances active simultaneously × ~50 particles/effect peak = ~2500 live particles. Padding for asymmetric peaks → **8K particle SSBO** (8192 × 64 B = 512 KB) is the v1 sizing target. Stress estimate; revise after Stage 0' content recon.

### 1.3 Spec-data semantics (`mc2.fx` parser surface)

The compiled binary library at `mc2srcdata/effects/mc2.fx` (~898 KB) encodes per-spec FCurve splines (`fcurve.cpp:1-1923`), seeded random ranges (`particlecloud.hpp:60-81`), emitter-box dimensions, lifespan/aging, color/size curves, perturbation parameters. The CPU parser is `gosFX::EffectLibrary::Load` (`mclib/gosfx/effectlibrary.cpp:58`); per-spec lookup is `EffectLibrary::Find` (`:92`). After (A) deletes `mclib/gosfx/`, this parser is gone — (B) must resurrect a **content-loader-only** subset (no draw, no execute) that reads `mc2.fx` into a GPU-friendly spec table.

### 1.4 What "ship-ready" means

Coverage floor for the (B) ship gate (`stock_install_must_remain_playable.md` reading per (A) §5.1 user resolution):
1. `CardCloud`, `PointCloud`, `ShardCloud` — full coverage. These are the bulk of every combat exchange.
2. `Tube` — full coverage. Contrails are visually the most-missed effect; user explicitly rejected a carve-out per (A) §5 item 3.
3. `ShapeCloud` / `DebrisCloud` / `PertCloud` / `EffectCloud` — defer to (B) v2. File as named debt with `mc2.fx` spec names enumerated.
4. `LightManager`-driven dynamic lights from particles (`mclib/gosfx/pointlight.cpp:1-342`) — covered, because the existing `LightsData` SSBO consumer doesn't care where lights came from. Spawn-side integration only.

---

## 2. Three architectural approaches

### (α) CPU-emit, GPU-batch billboards

**Shape.** Game-event code calls a new `gpu_fx::Spawn(specName, worldPos, ...)` API in place of `gosFX::EffectLibrary::Instance->Find` + `Effect::Start`. CPU-side update loop (per-effect, low cost) advances particle lifespans and emits world-space records into a ring SSBO each frame. One mega-draw per blend-bucket per atlas: vertex shader expands one record into 4 verts (billboard) via `gl_VertexID & 3`. Frag shader samples atlas with `textureLod`.

**Compute usage.** None. Lifecycle stays CPU-side. `mc2.fx` parser stays CPU-side. The "G" in "GPU pipeline" is draw expansion + sampling + projection.

**Pros.**
- Aligns with the codebase's shipped GPU-driven precedent: `GpuStaticPropBatcher` (CPU emits records, GPU expands), water fast path (`code/gamecam.cpp:270`), indirect terrain (CPU emits thin records). No shipped slice has GPU-resident *lifecycle*.
- One new shader pair (`gpu_particle.vert` + `gpu_particle.frag`) + one PertCloud-or-Tube companion = minimum surface area (per render-expert greybeard ruling).
- Vulkan-prep friendly: one SSBO, explicit device-mediated bind, no implicit cross-call GL state.
- Per-frame CPU cost is approximately what gosFX::Execute used to be (low; gosFX isn't a measurable Tracy bucket today) and the saved cost is the MLR clipper — already deleted by (A).

**Cons.**
- CPU-side update loop is "yet another" subsystem alive per frame. Modest cost; no bucket-defeating gain.
- Sort discipline lives on CPU: depth-sort billboard records before SSBO upload for alpha bucket. Standard work.

### (β) GPU-resident lifecycle, CPU-only spawn

**Shape.** Compute shader updates per-particle position/velocity/age each frame from per-spec parameters (also GPU-resident in a spec SSBO). CPU only writes new spawn events into an emission queue. Compute → indirect draw chain.

**Pros.**
- Theoretically lowest per-frame CPU.
- Demonstrates "GPU-driven everywhere" at the lifecycle level.

**Cons.**
- **Spec-language port surface is large.** FCurve splines (`fcurve.cpp:1923 lines`), `SeededCurveOf<ComplexCurve, SplineCurve, ...>` (`particlecloud.hpp:63-81`), per-particle stochastic perturbation (`m_minimumDeviation` / `m_maximumDeviation` at `particlecloud.hpp:60-62`) all need GLSL equivalents. ~80% of specs port simply; the long tail is per-type bespoke work.
- **`EffectCloud` spawn chains** are compute-to-compute emission links (atomic counter into a child pool, sync barrier). Architecturally non-trivial.
- **`LightManager` coupling** — particles spawn dynamic lights into `LightsData` SSBO (`shaders/include/lighting.hglsl:53`, populated CPU-side from `mclib/gosfx/pointlight.cpp`). GPU-resident lifecycle would need a compute → CPU readback to feed `LightsData` (sync-stall — see `memory/substrate_coalesce_sync_point_lesson.md`) OR a parallel GPU-resident light list (cross-SSBO write + sync barrier).
- Solves a **non-problem**: gosFX is not a measurable CPU bucket today. The cost the unified-projection arc wants killed (MLR clip math) is in MLR, not gosFX execute. (A) deletes both. (β) optimizes a zone that is already zero.

### (γ) Discard `mc2.fx`; hardcode effects in shader code

**Shape.** Author ~100 effects as GLSL shader programs. No spec parser. Effect = shader.

**Pros.**
- Smallest loader code.

**Cons.**
- Throws away ~898 KB of stock-authored content.
- **Stock-asset-compat reading.** `stock_install_must_remain_playable.md` requires generation *from unmodified stock data*. `mc2.fx` IS the stock data. (γ) discards it. The cpu-gpu-offload advisor pointed out that the rule allows shader-side math rewrite as long as `mc2.fx` data drives the generation — but (γ) as stated above discards the data, not just the math. Rule-violating.
- Authoring re-investment with no perf gain.

---

## 3. Recommendation

**Ship (α) — CPU-emit, GPU-batch billboards. v1 covers `CardCloud`/`PointCloud`/`ShardCloud`/`Tube`. v2 picks up the long tail. Reject (β) and (γ).**

### 3.1 Greybeard ruling (verbatim, all five answers)

1. **Subsystem pin.** Owner is the post-(A) feature gap: stock-install particle rendering. The (B) work surface is a NEW post-`renderLists()` GPU-direct pass (one billboard shader pair + one n-gon/long-tail companion), the resurrected `mc2.fx` content parser (read-only, no execute, no draw), and the producer-site shim that repoints the 16 `drawInfo.m_clipper` sites + the ~14 `EffectLibrary::Instance->Find` spawn sites onto the new API. The MLR seam is owned by (A) and out of scope here.
2. **Symptom vs cause.** Symptom: between (A) ship and (B) ship, stock install has zero in-game particle visuals (explosions, muzzle, hit, dust, contrails, VTOL effects, beams). Upstream condition: (A) intentionally deletes the only particle backend. (B) is not chasing a bug *class*; it fills a feature void created by (A) by design.
3. **The meta-fix.** There is no upstream change that retires "we need a particle subsystem" — that question was answered by user §5.1 of the (A) spec: particle reintroduction is **required-for-ship debt**. The candidate "accept no-particle as permanent" is incompatible with the ship gate. The candidate "ship a particle subsystem" is (B) itself.
4. **Substitutive test.** (B) is substitutive iff each sub-slice **repoints producer sites in the same commit that adds the GPU consumer**. Concretely: when v1's `CardCloud` coverage lands, the spawn sites in `code/artlry.cpp:793,811,1785,1823`, `code/carnage.cpp:913`, and `code/missiongui.cpp:4357,4405,4426,5838,5863,5884` that called `gosFX::EffectLibrary::Instance->Find(...)` must be repointed to the new `gpu_fx::Spawn(...)` API for the spec types covered in that slice, and the no-op-stub branches left by (A) must be deleted. Shipping the batcher with no caller repoint is the **additive anti-pattern** (`feedback_offload_must_be_substitutive_not_additive.md`) and must be rejected at adversarial review.
5. **Verdict: `PATCH (justified)`.** (B) is not a meta-fix — (A) is. (B) is capability-restoration filed against (A)'s deliberate transitional regression. The named meta-fix (A) is shipped first. Deferral justification: (A) atomically retires the bug class (`mlrclipper.cpp:206` CPU read of `cameraToClip(2,2)`); (B) carries a **greybeard guardrail**: anything (B) introduces that reads `cameraToClip` CPU-side, sorts billboards CPU-side via projected depth, or otherwise re-couples to a CPU projection authority is **a retired-bug-class re-introduction** and is rejected at code review. (B) must consume the unified-projection UBO from the GPU side.

### 3.2 Load-bearing rationale

- **Codebase precedent is uniformly (α)-shaped.** `GpuStaticPropBatcher` (`GameOS/gameos/gos_static_prop_batcher.cpp`), `renderWaterFastPath` (`code/gamecam.cpp:270`), indirect terrain (`shaders/gpu_driven_terrain_solid.comp` + `terrain_overlay.vert`), substrate-coalesce: all CPU-emit + GPU-expand. No shipped slice runs lifecycle on GPU. (β) breaks the pattern with no perf justification — gosFX is not a Tracy bucket today (`docs/render-perf-snapshot.md` carries no `gosFX_execute` zone), and (A) deletes the MLR cost the unified-projection arc actually cares about.
- **Stock-assets constraint.** `stock_install_must_remain_playable.md` requires generation *from unmodified stock data*. (α) preserves `mc2.fx` byte-for-byte and consumes it via a read-only parser; the shader is the engine-internal generator (same shape as terrain: stock tile data + GPU shader = generated surface). (γ) discards `mc2.fx`, violating the rule.
- **Vulkan-prep discipline.** Per `memory/vulkan_prep_explicit_device_discipline.md`: explicit device-mediated binding, std430 lockstep, zero implicit cross-call GL state, enqueue/flush. (α) is built greenfield to this rule. (β) adds a compute → indirect-draw chain with cross-SSBO writes for `LightsData` and `EffectCloud` spawn — strictly larger Vulkan-prep surface.
- **MLR-exception retirement.** (A) deletes `theClipper->RenderNow()` at `code/gamecam.cpp:287` (the immediate-draw exception inside the enqueue phase). The (B) batcher hooks **after `renderLists()`** at `code/gamecam.cpp:259` and after `renderWaterFastPath` at `:270`. This is the documented post-`renderLists()` slot (`memory/render_order_post_renderlists_hook.md`). The integration is "where `theClipper->RenderNow()` used to be" minus the immediate-draw-during-enqueue property.
- **Reversibility / soak-waiver fit.** No byte-parity oracle exists by design (GPU billboards diverge from MLR clip math). The `feedback_soak_waiver_with_probes_and_reviews_validated.md` waiver pattern does NOT apply (it needs parity probes). Validation falls back to: user-driven visual canary across tier1 + per-spec event-count probes (necessary-not-sufficient per `parity_probe_100pct_can_be_correct_redesign_report.md`) + adversarial review at default-on flip. Calendar soak required.
- **GPU-direct bring-up checklist.** All ten traps from `memory/gpu_direct_renderer_bringup_checklist.md` addressed in §4 / Stage 1' design.

### 3.3 Shape of the slice (informs plan-phase, not a plan)

**Modified Stage 0'-5' staging** ("greenfield-with-feature-gate" — Stage 0-6 doesn't apply because there's no CPU baseline after (A)):

1. **Stage 0' — content recon.** Inventory all ~100 specs in `mc2.fx` by primitive type, spawn frequency in tier1 (from (A) Stage 0's `[GOSFX_TRACE v1]` per-mission histograms — runs concurrently with this stage), and visual category (muzzle / explosion / smoke / dust / contrail / beam). Output: coverage table.
2. **Stage 1' — scaffold.** SSBO-backed billboard batcher, post-`renderLists()` hook at `code/gamecam.cpp:270` slot, driven from unified-projection UBO. One hardcoded `Card` test effect. Default-off `MC2_GPU_PARTICLES=1`. No `mc2.fx` consumption. Validates the 10-trap bring-up checklist end-to-end.
3. **Stage 2' — content authority.** Resurrect `gosFX::EffectLibrary::Load` (`mclib/gosfx/effectlibrary.cpp:58`) as a read-only parser feeding the new GPU-friendly spec table. One primitive type at a time: `CardCloud` first (highest spawn count), then `PointCloud`, `ShardCloud`, `Tube`. Each sub-slice repoints its `EffectLibrary::Instance->Find` spawn sites in the same commit (substitutive test per §3.1 ¶4).
4. **Stage 3' — coverage gates.** Per-mission event-count equivalence: tier1 (A)-with-trace records `Effect::Draw` invocations per `Find()` name; (B) must reproduce the same N spawn events per name (within ±10% for non-deterministic seeds).
5. **Stage 4' — visual canary soak.** User-driven visual across all five tier1 missions plus combat-heavy mc2_24. Per-spec subjective accept (`Mech_Explosion`, `Gauss_flare`, `LRM_Smoke`, `VTOL_Effect`, `Recovery_Effect`, `Damaged_fire`, `Critical_hit`). Mandatory adversarial review.
6. **Stage 5' — flip default-on; file v2 debt** (PertCloud, ShapeCloud, DebrisCloud, EffectCloud).

---

## 4. Negative space

Load-bearing risks found by grep + advisor review. Each addressed in design; flagged for plan-phase.

### 4.1 Hook point inheritance — the GPU-direct bring-up traps

The batcher's `flush()` lives at `code/gamecam.cpp` between the existing water fast-path call at `:270` and the soon-to-be-deleted `theClipper->RenderNow()` at `:287`. The ten traps from `memory/gpu_direct_renderer_bringup_checklist.md`, ranked by exposure:

| # | Trap | Exposure | Mandatory remediation |
|---|------|---|---|
| 2 | Texture handle indirection (`mc2_texture_handle_is_live.md`) | **HIGH** | Resolve atlas texture handles per-flush, not at parse time |
| 4 | VAO=0 silent drop on AMD | **HIGH** | Bind own VAO every flush; never inherit |
| 5 | Sampler-state inheritance (`sampler_state_inheritance_in_fast_paths.md`) | **HIGH** | `glGenSamplers` per-class (atlas: LINEAR+mipmaps+CLAMP_TO_EDGE); `glGetIntegeri_v` save/restore at unit boundary |
| 9 | Depth-state inheritance (`gpu_direct_depth_state_inheritance.md`) | **HIGH** | Explicit `glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE)` — never inherit |
| 10 | AMD auto-LOD on incomplete mips (`amd_auto_lod_strict_fail.md`) | **HIGH** | `textureLod(uTex, uv, 0.0)` in particle FS — non-negotiable on 7900 XTX |
| Blend | `blend_state_inheritance_in_post_process.md` | **HIGH** | Two draws min: additive bucket (`GL_ONE,GL_ONE`), alpha bucket (`GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA`). Reset at flush start |
| 7 | CPU pre-cull / `clip_w_sign_trap.md` | MEDIUM | Emitter pre-cull at world-space AABB; use `projectZ()` for in-front tests, not `sign(clip.w)` |
| 3 | terrainMVP `GL_FALSE` convention | LOW | Particles consume scene MVP; standard convention |
| 1 | `uniform uint` crash | LOW | Use `int` + cast |
| 8 | Map-stable indexing | LOW | Particles are transient, not indexed against map state |

**AMD attribute-0 rule** (`docs/amd-driver-rules.md:6`): the billboard expansion VS must declare and reference `layout(location=0)` (e.g., a dummy attribute or `gl_VertexID`-derived). AMD silently drops draws otherwise.

### 4.2 SSBO/UBO slot map collision risk

Current bindings (grep-verified):
- UBO `binding=1`: `SceneData` (`shaders/include/scene.hglsl:16` — `SCENE_DATA_ATTACHMENT_SLOT=1`)
- SSBO `binding=0/1/2/3`: static_prop pipeline (`shaders/static_prop.vert:55-67`)
- SSBO `binding=20`: `LIGHT_DATA_SSBO_BINDING` (`shaders/include/lighting.hglsl:15`)

**UBO and SSBO binding indices are separate namespaces** in OpenGL — SSBO binding 2 (static_prop) does not collide with a hypothetical UBO binding 2 (unified-projection). Reserve for (B):
- **UBO `binding=2`** for the unified-projection block when that arc lands (cross-coordinated with HANDOFF DRAFT spec).
- **SSBO `binding=10`** for `Particles` (well clear of static_prop's 0-3 and below the lights' 20).
- **SSBO `binding=11`** for `ParticleSpecTable` (spec parameters, populated by Stage 2' parser).
- **SSBO `binding=12`** (optional, β only) for `ParticleDrawIndirect`.

Pre-grep `binding = 2` and `binding = 10` across `shaders/` at plan-phase to confirm no drift.

### 4.3 std430 lockstep — Particle struct

Per `memory/cpp_glsl_ubo_struct_lockstep.md`: any C++/GLSL struct pair must be coupled by a shared header AND a build-time `static_assert(sizeof==N)`. Proposed schema (64 B, vec4-aligned):

```glsl
struct Particle {
    vec4 posSize;     // .xyz = world pos, .w = size (curve-eval)
    vec4 velAge;      // .xyz = velocity, .w = normalized age [0,1]
    vec4 spinTilt;    // .x = rotZ rad, .y = aspect, .z = lifespan, .w = seed
    uint colorARGB;   // packed 0xAARRGGBB (DWORD-stable per mc2_argb_packing.md)
    uint typeFlags;   // .lo16 = spec index, .hi16 = render mode bits
    uint atlasUV;     // packed half2 UV origin OR atlas tile index
    uint _pad;
};
```

Color is the legacy ARGB DWORD with bit-decode in shader — matches `mc2_argb_packing.md`'s BGRA-in-memory convention without the SSBO swizzle trap.

Pair the C++ struct and GLSL block in one header pair (e.g. `include/gpu_particle_schema.h` + `shaders/include/gpu_particle_schema.hglsl`) with `static_assert(sizeof(GpuParticle) == 64)`. The mc2_24 silent-corruption bug class (the canonical cited case in the lockstep memory) is exactly this surface.

### 4.4 EffectCloud nesting

`EffectCloud` particles (`effectcloud.cpp`) spawn child `Effect` instances at runtime. In (α): the CPU-side update loop iterates the parent's particles, calls the existing spawn API recursively. No new infrastructure needed — recursion stays in CPU code. (β) would need atomic-counter emission to a child pool with sync barrier, which is the principal reason (β) is rejected.

### 4.5 LightManager coupling

`gosFX::LightManager` (`mclib/gosfx/pointlight.cpp:1-342`) is a per-process singleton (created at `code/mechcmd2.cpp:1676`, destroyed at `:2053`) that effects use to spawn dynamic lights consumed by `LightsData` SSBO (`shaders/include/lighting.hglsl:53`). (A) deletes `mclib/gosfx/` entirely, taking `LightManager` with it. **(B) must resurrect a minimal `LightManager`-equivalent** (CPU-side, just feeds `LightsData` exactly as today) — naming TBD, but the API is `MakePointLight`/`ChangeLight`/`DeleteLight`. This is producer-side only; the consumer (`LightsData` shader code) is unchanged. Grep `gosFX::LightManager::Instance` outside `mclib/gosfx/` before plan-phase to enumerate caller sites (`mechcmd2.cpp:1676` and `:2053` are the singleton plumbing; `pointlight.cpp:233,265,266,318,319,332` are internal uses — confirm no external producers exist).

### 4.6 `mc2.fx` parser placement

(B) keeps the parser CPU-side (subset of `gosFX::EffectLibrary::Load` at `effectlibrary.cpp:58`). Parser feeds a GPU spec table at startup. Per-spawn lookup is a hashmap-by-name (mirroring `EffectLibrary::Find` at `:92`). No per-frame parsing.

After (A) Stage 4 deletes `mclib/gosfx/`, the parser must be **moved** (not copied) into the new (B) namespace before deletion. Coordination: (A)'s Stage 4 PR depends on (B)'s parser-resurrection PR landing first, OR (A) Stage 4 carries a temporary `mclib/gpu_fx/effect_loader.cpp` extracted from the deletion.

### 4.7 ShapeCloud / DebrisCloud — TG_Shape inheritance

These two effect types use full `TG_Shape` / `MLRShape` meshes per particle, not billboards. Mapping options:
- **Reclassify** — treat as one-off mesh draws via the existing mech / static-prop GPU-direct path. Spawn site emits a transient `TG_Shape` instance into the modern queue.
- **Defer to v2** — small visual loss; rare effect.

Recommend defer to v2. Document in the v2 debt note.

### 4.8 PertCloud n-gons

`Max_Number_Of_NGon_Vertices = 9` (`mclib/mlr/mlr.hpp:107`). Per-vertex perturbation per tick. Three implementation paths:
- Geometry shader expanding 1 point → up to 9 verts (slow on AMD; PertCloud low-volume).
- Compute-emit vertex buffer + `glMultiDrawArraysIndirect` with `GL_TRIANGLE_FAN` per particle.
- **Per-type pipeline** alongside the billboard one (cleanest; matches single-billboard-shader greybeard ruling of "two pipelines, not seven, not one").

Recommend the per-type pipeline OR defer to v2. PertCloud is low-volume — defer is safe.

### 4.9 Texture atlas vs per-effect textures

`mc2.fx` references textures by name; effects today resolve handles via `mcTextureManager` (handles mutate per frame per `mc2_texture_handle_is_live.md`). Options:
- **Atlas** — bake all effect textures into one (or a few) atlas. CLAMP_TO_EDGE on sampler. Resolves the per-flush sampler problem.
- **Per-effect texture binding** — slower binding cost but easier path; one draw per (atlas-or-effect-texture, blend-mode) bucket.

Recommend atlas for v1 (single bind per blend bucket); fall back to per-effect if atlas build is non-trivial. Defer atlas-build decision to Stage 1' implementation.

### 4.10 Per-mission lifecycle

`mission_load_inits_mirror_init_per_subsystem.md`: every paired `*_init` must be mirrored in `Mission::load` after `ObjectManager->Load`. (B)'s state is process-scoped (parser at startup; SSBO at Stage 1' init), NOT per-mission. The per-frame particle pool is transient. **No mission-init mirror needed** — but confirm at plan-phase by grep on `Mission::destroy` for any new (B) subsystem teardown call.

### 4.11 Save-game

Per (A) §1.5, gosFX is not persisted; (B) inherits this. The transient particle pool is never serialized. Add savegame canary to (B) Stage 4' smoke matrix per the 2026-05-20 EVENING handoff lesson.

### 4.12 Editor convergence

(A) §5 item 2 deferred editor transition. (B) ships engine-only. The editor lives on the older MLR rails until its own convergence slice. Don't introduce (B)-specific shims in editor; let it stay on the `mclib/mlr/` linkage the build keeps for editor / Viewer / aseconv targets per (A) §5.1.

### 4.13 Tracy zones — measurement semantics

After (B) ships default-on, expect a NEW Tracy zone (`gpu_fx_update` or similar) ≪ 1ms. This is NEW cost, not displaced cost — (A) already retired the gosFX/MLR cost to zero. F3 telemetry post-(B) should record the new zone and confirm it stays under the budget. Treat as additive cost analytically; the substitutive test (per §3.1 ¶4) is about *which code is alive*, not about Tracy delta.

### 4.14 Producer-site census drift

(A)'s 16 `drawInfo.m_clipper = theClipper` sites are the *consumer* side. (B)'s producer-side site list is the `EffectLibrary::Instance->Find` callers — grep-verified at write time: `code/artlry.cpp:793,811,1785,1823`, `code/carnage.cpp:913`, `code/missiongui.cpp:4357,4405,4426,5838,5863,5884`, plus calls in `code/terrobj.cpp`, `code/weaponbolt.cpp`, the mech/actor/building chain via `weaponEffects` manager. **Count is at least 11 direct + indirect via `weaponEffects` manager — drive enumeration off a compile-time mechanism (delete the deleted-by-(A) API; let the compiler enumerate broken call sites) at plan-phase, NOT a grep list.**

---

## 5. Open questions

To answer before plan-phase begins:

1. **(B) v1 ordering relative to (A) Stage 4.** (A) §5 says (B) "does not block (A) shipping" but is required-for-ship. Three feasible orderings:
   - (A) Stages 1-4 ship first; (B) lands after on the clean substrate. Visual-gap window is non-zero.
   - (A) Stages 1-3 ship; (B) v1 lands; (A) Stage 4 (atomic deletion of `mclib/gosfx/` + `mclib/mlr/` runtime-side) lands after (B) covers `CardCloud`/`PointCloud`/`ShardCloud`/`Tube`. Visual-gap window is zero, but (B) and gosFX coexist briefly in the build (with (B) off by default per Stage 1' env-gate). Manageable.
   - (A) and (B) merge into a single atomic slice. Substantially larger PR. Reject; too risky.
   - **Recommend ordering #2.** Confirm.

2. **`mc2.fx` parser ownership.** (B) resurrects a subset of `gosFX::EffectLibrary::Load`. Naming and namespace TBD. Suggested: `gpu_fx::EffectLibrary` in `mclib/gpu_fx/`. Confirm.

3. **Atlas vs per-effect texture binding** (§4.9). Recommend atlas for v1; defer to Stage 1' implementation. Confirm scope.

4. **PertCloud / ShapeCloud / DebrisCloud / EffectCloud for v1 vs v2.** Recommend defer to v2 for ShapeCloud / DebrisCloud / PertCloud / EffectCloud; v1 covers Card/Point/Shard/Tube. Confirm scope or expand v1.

5. **LightManager naming** (§4.5). Resurrect under `gpu_fx::LightManager` in `mclib/gpu_fx/pointlight.cpp`? Or fold into the main batcher? Confirm.

6. **Soak duration.** No parity probe substrate (per §3.2 / cpu-gpu-offload advisor). Calendar soak duration TBD — recommend match the standard-slice cadence used for other substitutive-but-non-parity-validated work; user judgment.

7. **(γ) hard-rejected, or held as exploratory backup?** Recommended hard-rejected per §3.2 stock-assets constraint. Confirm.

---

## Appendix A — grep-verification log (write-time, 2026-05-20)

All file:line citations re-grepped at write time against the `nifty-mendeleev` worktree.

- `code/gamecam.cpp:259` — `mcTextureManager->renderLists();` (confirmed)
- `code/gamecam.cpp:270` — `land->renderWaterFastPath();` (existing post-`renderLists()` precedent)
- `code/gamecam.cpp:287` — `theClipper->RenderNow();` (the (A)-target deletion site)
- `mclib/gosfx/effectlibrary.cpp:58` — `gosFX::EffectLibrary::Load(Stuff::MemoryStream* stream)`
- `mclib/gosfx/effectlibrary.cpp:92` — `gosFX::EffectLibrary::Find(const char* name)`
- `mclib/gosfx/pointlight.cpp:8-9` — `gosFX::LightManager *gosFX::LightManager::Instance = NULL;`
- `mclib/gosfx/pointlight.cpp:12,18,26` — `MakePointLight` / `ChangeLight` / `DeleteLight`
- `code/mechcmd2.cpp:1676` — `gosFX::LightManager::Instance = new gosFX::LightManager();`
- `code/mechcmd2.cpp:2053` — `delete gosFX::LightManager::Instance;`
- `code/artlry.cpp:793,811,1785,1823` — `EffectLibrary::Instance->Find(...)` spawn sites (4)
- `code/carnage.cpp:913` — `EffectLibrary::Instance->Find(...)` spawn site (1)
- `code/missiongui.cpp:4357,4405,4426,5838,5863,5884` — `EffectLibrary::Instance->Find(...)` spawn sites (6)
- `shaders/include/lighting.hglsl:15` — `LIGHT_DATA_SSBO_BINDING = 20`
- `shaders/include/lighting.hglsl:53` — `layout(binding = LIGHT_DATA_SSBO_BINDING, std430) buffer LightsData`
- `shaders/include/scene.hglsl:16` — `layout(binding = SCENE_DATA_ATTACHMENT_SLOT, std140) uniform SceneData`
- `shaders/static_prop.vert:55-57,67` — SSBO bindings 0/1/2/3 in static_prop pipeline
- `mclib/mlr/mlr.hpp:107` — `Max_Number_Of_NGon_Vertices = 9`
- `mclib/gosfx/particlecloud.hpp:60-81` — `SeededCurveOf<...>` FCurve menagerie (β port surface)
- `mclib/gosfx/pertcloud.hpp:82-84` — PertCloud per-particle vertex array
- `mclib/gosfx/fcurve.cpp` — 1923-line spline evaluator (β port surface)
- `mclib/gosfx/` line counts per primitive — see §1.1 table

No fictional symbols; no inherited citations from upstream session messages used without re-grep. The (A) spec's §1.3 site census (16 `drawInfo.m_clipper = theClipper` sites) is consumer-side and not re-grepped here (it is producer-side citations that drive (B)).

## Appendix B — adversarial-plan-review self-pass

Reviewing this spec for what could be wrong:

- **Step 2 (grep cited symbols).** Done in Appendix A. All file:line claims verified at write-time.
- **Step 3 (relationship claims).** (A) Stage 4 deletes `mclib/gosfx/` — verified in (A) spec §3.3 step 4. (B) parser resurrection depends on this — surfaced as Open Question #1.
- **Step 5 (perf claims).** None made; (B) is a feature reintroduction, not a perf claim. Mentioned in §4.13 that (B) introduces new Tracy cost.
- **Step 6 (load-bearing constraints).** Walked the 10-trap bring-up checklist in §4.1, lockstep in §4.3, mission-lifecycle in §4.10, savegame in §4.11. Stock-assets in §3.2. AMD rules in §4.1.
- **Step 7 (per-mission lifecycle).** Addressed in §4.10 — confirmed no per-mission init needed; flagged for plan-phase grep.
- **Step 8 (partial-landing hazard).** §3.1 ¶4 explicit: per-sub-slice substitutive test (producer repoint + stub deletion in same commit). §3.3 Stage staging mirrors.
- **Step 9 (exhaustive census).** Producer-site census in §4.14 enumerated from grep; flagged "drive enumeration off compile-time mechanism, not grep list" because the `weaponEffects` manager indirection means grep misses indirect callers. This is the same trap pattern that bit the (A) spec's first 8-site count (corrected to 16).

**Findings against this spec:**
- **MAJOR:** the producer-site census in §4.14 lists 11 direct `EffectLibrary::Instance->Find` sites; (A) §1.3 enumerates 16 *consumer*-side `drawInfo.m_clipper = theClipper` sites in 8 source files (including mech3d, gvactor, bdactor, terrobj, weaponbolt that the producer-grep above missed because their effect lookups go through the `weaponEffects` manager indirection). The actual producer site count is "at least 11 + N indirect via `weaponEffects`." Plan-phase MUST drive site enumeration off a compile-time mechanism (delete the `gpu_fx::Find` API and let the compiler enumerate), not the §4.14 grep list. Annotated in §4.14.
- **MAJOR:** SSBO binding 10 is asserted free but not exhaustively grep-verified (I grepped only the four cited shader files). Plan-phase must `grep -rn 'binding *= *10\b' shaders/` before claim. Annotated in §4.2.
- **MINOR:** the (B) parser must move (not copy) from `mclib/gosfx/effectlibrary.cpp` to its new home before (A) Stage 4 deletes the source. Open Question #1's recommended ordering #2 makes this a non-issue if Stage 4 lands after (B) v1.
- **MINOR:** `EffectCloud` recursion in (α) (§4.4) is hand-waved — "CPU-side recursion stays in CPU code." Plan-phase should verify the spawn API supports recursion and that the CPU-side update doesn't blow stacks on pathological specs.

No CRITICAL findings against this spec. Spec is plan-phase-ready subject to the open questions in §5.
