# RenderWorld Slice M4 — VFX Adapter Recon

Date: 2026-05-23
Author: recon-only subagent (no spec proposed)
Scope: VFX / particles / gosFX integration into the RenderWorld arc (M1-M2.6 shipped).
Status: recon-only; spec author decides.

---

## 1. Summary (key bullets)

- **VFX in default tier1 is a NEAR-NULL execution surface.** Two independent gates collapse the VFX path to zero work:
  1. `mc2::mlr_gate::is_disabled()` defaults to **true** (`mclib/mlr/mlr_gate.cpp:20` `kDefaultDisabled = true`). All 4 MLR clipper work-leaves early-return (`mclib/mlr/mlrclipper.cpp:410,578,690,721`). gosFX `Effect::Draw` paths therefore produce no GPU work in stock play.
  2. `mc2::particles::Batcher::is_enabled()` reads `MC2_GPU_PARTICLES`, default **OFF** (`mclib/particles/batcher.h:36-37`, `mclib/particles/batcher.cpp`). When OFF, `Flush()` is a no-op even though the per-frame canary emit runs at `code/gamecam.cpp:269-292`.
  3. Smoke evidence (latest tier1 run `tests/smoke/artifacts/2026-05-23T23-32-15/`): `[INSTR v1] enabled: mlr_gate disabled=1`; no `[GPU_PARTICLES v1] event=prog_compiled` lines beyond per-process init noise.
- **There is exactly ONE forward VFX color-write shader pair in the active path:** `shaders/particle_billboard.{vert,frag}`. It is invoked by `gos_particle_bridge_flush()` at `GameOS/gameos/gos_particle_bridge.cpp:119-205`. It is dormant by default (env-gate above).
- The legacy gosFX path (`mclib/gosfx/*.{cpp,hpp}` + `mclib/mlr/`) renders via **immediate-draw through the MLR clipper** (`theClipper->RenderNow()` at `code/gamecam.cpp:309`), using `gos_vertex*` / `gos_tex_vertex*` shaders — i.e. the legacy GOS vertex-shader stable, not a dedicated particle shader. All four MLR clipper draw leaves are gate-dead by default.
- **Particle FS does NOT write `attachment-2` (objectID).** `shaders/particle_billboard.frag:24-27` only writes `outColor`. There is no `layout(location=2) out uint`. Particle pixels therefore leave attachment-2 as whatever the depth-passing draw underneath wrote.
- **The integer attachment-2 trap is REAL for VFX.** GL color-attachment 2 is `R32_UINT` (M1.5 substrate). Blend operations on integer color attachments are not defined; the spec says blending is silently disabled on non-floating-point formats (GL 4.5 §17.3.6). Last-write-wins per fragment — so if particles ever wrote attachment-2 with their objectID, a foreground additive particle would clobber the mech objectID underneath even when the particle is visually transparent.
- **Recommendation surfaced for confirmation: VFX MUST NOT write attachment-2.** The "gap" is a feature. This makes M4 substantially smaller than M2.5 — no SSBO grow, no objectIdRaw fill, no per-emitter cookie plumbing. M4 reduces to (a) handle/lifecycle accounting at the engine boundary, (b) optional debug log of what effects are alive, (c) reaffirming the no-write contract in `setSceneDrawBuffers()`'s policy.
- **Handle range proposal `0x200000` does NOT fit the current 20-bit index field.** `RenderCore/Handle.h:34` masks index to `0xFFFFF` (1,048,575). `0x200000` = 2,097,152 — overflow. If per-emitter handles are wanted, either (a) re-use the unified index space packed by kind (current pattern: `kMechHandleBase = 0x00010000` is just an offset into the same 20-bit index pool), or (b) widen the handle index field — but that ripples through every shader's `objectIdRaw` decode and breaks the M1.5 wire format. Option (a) is the only safe choice.
- **gosFX dev-override re-enable is a separate, documented hazard.** CLAUDE.md "Known issues" entry at `:159-167` warns that `MC2_DISABLE_GOSFX=0` under unified projection renders gosFX wrong (stale MLR `cameraToClip(2,2)/(3,2)` convention). This is independent of M4 — but M4 must NOT depend on the gosFX path being functional, because the dev-override is broken until MLR Slices 1-5 ship.

---

## 2. VFX rendering write paths

### 2.1 Forward (active in default config) — currently disabled by env

| Stage | File:line | Shader | Notes |
|---|---|---|---|
| CPU producer | `mclib/particles/spawn_card.cpp:127`, `spawn_cardcloud.cpp:113`, `spawn_point.cpp:120`, `spawn_shard.cpp:144`, `spawn_tube.cpp:176` | n/a | `Batcher::Instance().Emit(p)` push into staging SSBO mirror |
| Producer call sites | `mclib/gosfx/card.cpp:369`, `cardcloud.cpp:909`, `pointcloud.cpp:522`, `shardcloud.cpp:694`, `tube.cpp:756` | n/a | Inside legacy `gosFX::*::Draw`; the `Batcher::is_enabled()` branch enqueues the per-particle record |
| Per-frame flush | `code/gamecam.cpp:269-292` | n/a | `Batcher::Instance().Flush()` runs unconditionally — but the impl no-ops when `is_enabled()==false` |
| GL bridge | `GameOS/gameos/gos_particle_bridge.cpp:119-205` | `shaders/particle_billboard.{vert,frag}` | Owns SSBO binding=14, sampler, atlas, depth state (`GL_GEQUAL`, `glDepthMask(FALSE)`), blend state (`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`) |
| Vertex expansion | `shaders/particle_billboard.vert:48-85` | self | gl_VertexID-driven 6-vert quad expansion; reads `Particle` from SSBO binding 14; applies axis-swap + `u_worldToClipGL` |
| Fragment | `shaders/particle_billboard.frag:17-27` | self | `outColor = textureLod(uAtlas, v_uv, 0.0) * v_color;` — ONLY writes color attachment-0. No `v_objectId` declaration. |

### 2.2 Legacy (active when `MC2_DISABLE_GOSFX=0`; dormant default) — currently broken under unified projection

| Stage | File:line | Shader | Notes |
|---|---|---|---|
| Per-frame driver | `code/gamecam.cpp:150` | n/a | `theClipper->StartDraw(cameraOrigin, cameraToClip, ...)` |
| Per-frame submit | `code/gamecam.cpp:309` | n/a | `theClipper->RenderNow()` — immediate-draw exception (documented in `memory/render_functions_are_enqueuers_not_submitters.md`) |
| Producer call sites (16 total) | `mclib/bdactor.cpp:1409,1453`, `mclib/gvactor.cpp:2220,2253,2288,2329`, `code/artlry.cpp:1339,1423`, `code/carnage.cpp:830`, `code/missiongui.cpp:2997,3020`, ... | n/a | `drawInfo.m_clipper = theClipper; gosFX::Effect::Draw(&drawInfo)` |
| MLR work-leaves | `mclib/mlr/mlrclipper.cpp:410,578,690,721` | n/a | All 4 start with `MC2_GOSFX_GATE_EARLY_RETURN()` — early-return when default disabled |
| Eventual GL submission | via GOS vertex stable | `shaders/gos_vertex.{vert,frag}`, `shaders/gos_tex_vertex.{vert,frag}`, `shaders/gos_tex_vertex_lighted.{vert,frag}` | None of these declare `layout(location=2) out uint v_objectId`. They write `outColor` only. |

### 2.3 Adjacent (NOT VFX, but related transient overlays)

- **Weather** (`code/gamecam.cpp:316` `weather->render()`): rain drops; declared in `code/weather.h:53`. Not in M4 scope per user framing (rain is environmental ambient, never spawned by a "fireball" event), but uses similar additive-overlay semantics.
- **Debug renderer** (M1 design in `docs/superpowers/specs/2026-05-23-debug-renderer-m1-design.md`, post-weather flush): debug world primitives, depth-tested before post-process.
- **Skybox / shoreline / godrays** — environmental, not spawn-event VFX.

---

## 3. Lifetime + cardinality

### 3.1 Particles (atomic visual unit)

- Lifetime: typically 0.5-5 seconds (the `lifetime` field at `mclib/particles/spec.h:47`, `age` advances on CPU per frame in Stage 1').
- Per-frame budget: 1024-4096 records by default (`GameOS/gameos/gos_particle_bridge.cpp:106-107` ensures capacity, grows in 1024-record blocks).
- Spawn rate during combat: hundreds per second — `mclib/particles/spawn_card.cpp` emits 1 per call; `spawn_cardcloud.cpp` emits N per call; producers fire from artillery, weapons, explosions, smoke trails.

### 3.2 Emitters (logical "effect" instance)

- `gosFX::Effect` instances (`mclib/gosfx/effect.hpp:167`): one per "explosion", "muzzle flash", "smoke trail", "shock wave", etc.
- Typical concurrent: dozens (per smoke evidence, no exact counter — needs instrumentation if relevant).
- Lifetime: seconds to ~30s for trailing effects.

### 3.3 Spawn-source game objects

- Already first-class in M2 (mechs at `kMechHandleBase=0x10000`). A weapon-fire effect is sourced FROM a `BattleMech` and visually located AT a position derived from the mech's pose. The source has an existing RenderObjectHandle.

---

## 4. Identity candidates evaluation

| Candidate | Cardinality | Lifecycle pressure | Pickup semantic | Verdict |
|---|---|---|---|---|
| **Per-particle** | thousands/sec | extreme (handle alloc per particle per frame) | meaningless — user can't "click a single particle" | REJECT |
| **Per-emitter** (gosFX Effect instance) | dozens live | high (seconds-scale create/destroy) | "what effect did I click on?" — possibly meaningful in editor/debug only | DEFENSIBLE for debug-only; OVERKILL for stock gameplay |
| **Per-spawn-source-game-object** | bounded by source count (mechs/vehicles) | matches M2 cardinality | redundant — clicking a particle would resolve to mech-behind anyway under "no attachment-2 write" recommendation | REDUNDANT |
| **None — VFX is unpickable** | n/a | zero | particle pixels fall through to whatever the underlying opaque draw wrote to attachment-2 | RECOMMENDED |

**Why "None" is the right answer:**

1. VFX visual semantics: particles are additive/alpha-blended overlays on top of a scene. The user's mental model of "clicking on an effect" is fuzzy — the click target is whatever the effect is decorating, not the effect itself.
2. Integer-attachment overwrite trap (see §5) — writing per-particle/per-emitter objectIDs to attachment-2 actively breaks M2.6 mech-pickup for any mech that has a tracer / muzzle-flash / impact effect in front of it.
3. The "VFX is environmental" framing matches the existing weather / debug-prim pattern — they don't have pickup either.
4. M2.6 already ships the inspect-only meta-fixed `GameplaySelectionDebugState` — if a debug visualizer ever wants "what effects are alive", a separate `RenderWorld::vfxAliveCount()` accessor (analogous to `getMechsAliveCount()` at `RenderWorld.h:308`) is cheaper than wiring per-emitter handles.

---

## 5. Additive-blending interaction (the integer-attachment trap)

GL 4.5 spec §17.3.6 ("Blending"): blend equations are **only defined for fixed-point and floating-point color buffers**. For an integer color buffer (the M1.5 `R32_UINT` attachment-2), blending is silently treated as `GL_FUNC_ADD` with `GL_ONE, GL_ZERO` — i.e. **last write wins per fragment**, regardless of `glBlendFunc` state set for attachment-0.

This means a particle fragment that writes attachment-2 OVERWRITES whatever the depth-passing opaque draw underneath wrote, even if the particle's color contribution to attachment-0 is near-zero (because alpha is low or additive accumulation hasn't reached salience).

**Concrete failure mode under "particles write their own objectID":**

1. Mech M renders to (color attachment-0, depth, objectID attachment-2) — fragment writes `(color_mech, z_mech, handle_mech.raw())`.
2. Particle P (muzzle flash, alpha=0.05) renders in front of M with `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`. attachment-0 blend: `0.95*color_mech + 0.05*color_P` — visually still mech. attachment-2 (no blend): `handle_P.raw()` — overwritten.
3. User Shift+clicks on the mech. `lookupAtPixel(x, y)` returns `handle_P` (or invalid if particle has no kind). M2.6 mech-pick FAILS.

**Particle billboards use `GL_DEPTH_TEST=on, GL_GEQUAL, glDepthMask=FALSE`** (`gos_particle_bridge.cpp:184-186`). This means particles can pass the depth test in front of mechs but do NOT write depth. attachment-2 writes still occur per passing fragment.

**Three options ranked:**

1. **Particles do NOT write attachment-2** (RECOMMENDED). Achieved by particle FS having no `layout(location=2) out uint`. Currently the case — `particle_billboard.frag` only writes `outColor`. M4 maintains this invariant explicitly: a CI/firewall check + a doc note in `setSceneDrawBuffers()` that attachment-2 is opaque-only.
2. **Particles write 0 / sentinel to attachment-2 explicitly.** Would require an alpha-test discard threshold or per-particle policy. Adds code for zero benefit over option (1) — the no-write behavior already leaves attachment-2 untouched.
3. **Particles write their own objectID.** WORST option; breaks M2.6 picking through any visible effect.

---

## 6. gosFX dev-override status (default tier1)

Verified empirically and by code inspection:

- `mclib/mlr/mlr_gate.cpp:20` — `kDefaultDisabled = true`.
- `mclib/mlr/mlr_gate.cpp:26-38` — `initialize_locked()` reads `MC2_DISABLE_GOSFX`; if unset OR `=1`, gate is disabled (gosFX no-op).
- `mclib/mlr/mlr_gate.h:23-24` — `MC2_GOSFX_GATE_EARLY_RETURN()` macro inserted at the 4 MLR clipper leaves.
- Smoke artifact `tests/smoke/artifacts/2026-05-23T23-32-15/*.log` shows `[INSTR v1] enabled: mlr_gate disabled=1` per process — consistent with default-disabled.
- CLAUDE.md "Known issues" `:159-167` documents that re-enabling (`MC2_DISABLE_GOSFX=0`) is BROKEN under the shipped unified-projection F1 because MLR clipper reads `cameraToClip(2,2)/(3,2)` in stale MC2-pixel-homog convention.
- `scripts/run_smoke.py:171-176` ASSERTS `MC2_DISABLE_GOSFX != "0"` at smoke entry — tier1 cannot run with the dev-override enabled.

**M4 implication: under default config, gosFX produces ZERO GPU work.** The CPU-side `gosFX::Effect::Update` simulation may still run (need to verify if `MC2_GOSFX_GATE_EARLY_RETURN` covers Update or only Draw — quick grep below shows the 4 gates are at clipper leaves, NOT Update sites). So CPU cost continues; GPU is silent.

The GPU particle batcher (`MC2_GPU_PARTICLES=1`) is the only forward-path VFX renderer, and it is ALSO default OFF. In default tier1, there is NO live VFX color-attachment-0 writing.

**This makes M4 a "future-proofing" slice, not a "fix what's rendering today" slice.** Reasonable response: scope M4 to (a) reaffirm the no-write contract via firewall, (b) add `[VFX v1]` counter banner so future enablement is observable, (c) leave actual handle plumbing for when one of the two gates flips default-ON.

---

## 7. Particle-batcher path

There IS a modern particle batcher: `mclib/particles/Batcher` (`mclib/particles/batcher.h:28-62`).

- It is analog of `gos_static_prop_batcher` / `gos_mech_batcher` but for particles (not per-object props/mechs).
- The bridge `gos_particle_bridge_flush()` is the GL-side flush; it mirrors `gos_static_prop_batcher` patterns: SSBO upload, empty VAO + `gl_VertexID`-driven expansion, save/restore of program/VAO/blend/depth/sampler state.
- Comment at `mclib/particles/batcher.cpp:18` declares the bridge call extern-C.
- It is feature-complete for Stage 1' Card test effect (one billboard per particle, full-atlas UV); Stage 2' would add per-primitive Spawn callsites in producers, view-aligned billboarding, GPU-side age advance.

**Status:** functional, gated OFF, default unused. Coverage table for stage 0' Spawn-counter data is in `docs/superpowers/plans/2026-05-20-B1-Stage-0-content-recon.md`.

---

## 8. Handle range proposal

**Existing layout (from `RenderCore/Handle.h:32-43` + observed bases):**

- Handle: 20-bit index `[19:0]` (range 0..1,048,575) + 12-bit generation `[31:20]` (range 0..4095).
- Static-props: index allocated from low end. Observed peak `mc2_24=2641` (CLAUDE.md L190).
- Mechs: `kMechHandleBase = 0x00010000` (= 65,536). Peak per-mission writes seen up to 1,232 (mc2_24), so range stays small relative to base.
- Future kinds noted at `RenderWorld.h:134`: `Terrain=2, Vfx=3, Overlay=4`.

**Proposal `kVfxHandleBase = 0x200000` is OUT OF RANGE.** `0x200000` = 2,097,152 > 0xFFFFF = 1,048,575 (20-bit max). Would silently truncate to `index=0` and collide with static-prop slot 0.

**Workable proposals:**

- `kVfxHandleBase = 0x40000` (= 262,144) — leaves 786K slots for VFX between mech ceiling (~65,536 + 1M practically unused) and the 20-bit ceiling. Allows up to ~786K live VFX handles before collision, more than enough for "dozens of effects".
- `kVfxHandleBase = 0x80000` (= 524,288) — half of address space; clean visual split when reading raw values in logs.
- If terrain ever needs a base too, choose: static-prop `0`, mech `0x10000`, terrain `0x20000`, vfx `0x40000`, overlay `0x80000`.

**STRONGLY consider: do not allocate VFX handles at all.** Per §4 recommendation, VFX is unpickable; no `objectIdRaw` writes; no need for per-emitter handles in the unified record table. M4 reduces to a counter + lifecycle banner, no handle issuance.

---

## 9. Picking semantics options

Per §1 framing:

| Option | Semantic | Cost | Recommendation |
|---|---|---|---|
| A. Inspect-only debug | Click → log "what effect am I clicking on" | per-emitter handle plumbing + non-additive policy decision for attachment-2 | DEFER — premature for debug-only utility |
| B. Source-game-object lookup | Click on explosion → identifies which mech fired | redundant with M2.6; mech is already pickable when visible | REDUNDANT |
| C. None — pass through to underlay | VFX is environmental; the click resolves to terrain/mech beneath | zero cost; preserves M2.6 invariants | **RECOMMENDED** |

Option C is consistent with the broader RenderWorld philosophy of "kind-discriminated handle issuance only when there is a real consumer" — see M2-pre handling for the `RenderObjectKind` enum at `RenderWorld.h:131-135` already having `Vfx=3` reserved without an issuer.

---

## 10. Open questions for spec author

1. **Confirm or reject: VFX shall NOT write attachment-2 (objectID).** Recon recommends NOT — the integer-attachment overwrite trap actively breaks M2.6 mech-pick when any particle is in front of a mech. Confirmation needed because a "yes write" position would require a non-trivial alpha-test policy in the FS plus an emitter-handle scheme.
2. **If "yes write" (against recon advice): per-emitter or per-source-object?** Per-emitter implies a new gosFX `Effect` → handle table at `gosFX::Effect` construction; per-source means looking up the source mech's existing M2 handle and reusing it — but then a particle hit gives you the mech, which M2.6 already gives you when the mech itself is in the pixel.
3. **Is there a debug visualizer / editor use case that DRIVES per-emitter identity?** If no — confirm None. If yes — describe it; that determines whether handles are needed (visualizer) vs a separate `[VFX_DEBUG v1]` log channel (sufficient for one-shot inspection).
4. **Does the gosFX-disabled + GPU-particles-disabled default config make M4 effectively a no-op for tier1?** Recon evidence: YES under stock defaults; the only forward color-0 writer is the dormant particle billboard FS. If M4 still wants to ship "future-proofing" scope, frame it explicitly as such (firewall + counter banner) — do not pretend it's fixing live render behavior.
5. **Handle base if any chosen — pick from §8 candidates.** `0x200000` is OUT OF RANGE for the current 20-bit index encoding; either pick from `0x40000` / `0x80000` or widen the handle (breaks M1.5 wire format).
6. **Does the gosFX "Update" CPU path also early-return under `mlr_gate disabled=1`, or only Draw?** Spec author should grep `gosFX::Effect::Update` for the gate macro before claiming "VFX is fully no-op default" vs "only draw is no-op; CPU sim still costs". (Recon didn't verify; out-of-scope for the question being asked.)

---

## 11. File:line citations (grep-verified at write time)

| Citation | Purpose |
|---|---|
| `RenderWorld/RenderWorld.h:131-135` | `RenderObjectKind` enum with reserved `Vfx=3` |
| `RenderWorld/RenderWorld.h:283-300` | M2 register/destroyMech pattern that M4 would mirror |
| `RenderWorld/RenderWorld.h:308` | `getMechsAliveCount()` analog for proposed `getVfxAliveCount()` |
| `RenderCore/Handle.h:32-43` | 20-bit index, 12-bit generation; `0x200000` overflow |
| `mclib/mlr/mlr_gate.cpp:20` | `kDefaultDisabled = true` — gosFX default-OFF |
| `mclib/mlr/mlr_gate.cpp:26-38` | env-read + cache logic |
| `mclib/mlr/mlr_gate.h:23-24` | `MC2_GOSFX_GATE_EARLY_RETURN()` macro |
| `mclib/mlr/mlrclipper.cpp:410,578,690,721` | 4 MLR clipper work-leaves, all gated |
| `mclib/particles/batcher.h:28-62` | producer-facing Batcher API |
| `mclib/particles/batcher.cpp:18,116,121` | bridge declaration; Flush() forwards to bridge; singleton |
| `mclib/particles/spec.h:41-66` | `GpuParticle` std430 struct (64B) |
| `mclib/particles/spawn_card.cpp:127`, `spawn_cardcloud.cpp:113`, `spawn_point.cpp:120`, `spawn_shard.cpp:144`, `spawn_tube.cpp:176` | per-primitive Spawn → `Batcher::Emit` |
| `mclib/gosfx/card.cpp:369`, `cardcloud.cpp:909`, `pointcloud.cpp:522`, `shardcloud.cpp:694`, `tube.cpp:756` | legacy gosFX Draw sites that branch into Batcher when enabled |
| `mclib/bdactor.cpp:1409,1453`, `mclib/gvactor.cpp:2220,2253,2288,2329`, `code/artlry.cpp:1339,1423`, `code/carnage.cpp:830`, `code/missiongui.cpp:2997,3020` | legacy `drawInfo.m_clipper = theClipper` producer sites |
| `code/gamecam.cpp:150` | `theClipper->StartDraw(...)` per-frame |
| `code/gamecam.cpp:269-292` | GPU particle Flush + canary emit |
| `code/gamecam.cpp:309` | `theClipper->RenderNow()` — legacy immediate-draw submission |
| `code/gamecam.cpp:316` | `weather->render()` — adjacent transient (not in M4 scope) |
| `GameOS/gameos/gos_particle_bridge.h:16` | bridge entry-point declaration |
| `GameOS/gameos/gos_particle_bridge.cpp:119-205` | full bridge implementation; SSBO binding=14; depth `GL_GEQUAL`; blend `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` |
| `GameOS/gameos/gos_postprocess.cpp:29-46` | `SceneDrawBufferMode`, `setSceneDrawBuffers` central policy (M1.5 META-FIX) |
| `GameOS/gameos/gos_postprocess.cpp:332` | attachment-2 framebuffer texture binding |
| `GameOS/gameos/gameosmain.cpp:844-850` | `MC2_DISABLE_GOSFX=0` warn under unified projection (F1 known issue) |
| `shaders/particle_billboard.vert:24-85` | particle billboard expansion VS; no objectID out |
| `shaders/particle_billboard.frag:17-27` | particle FS; writes ONLY `outColor` (attachment-0). Confirms VFX does not currently write attachment-2. |
| `shaders/static_prop.frag:71,179` | static-prop `layout(location=2) out uint v_objectId` for contrast |
| `shaders/mech.frag:46-47,93` | mech `layout(location=2) out uint v_objectId` for contrast |
| `scripts/run_smoke.py:171-176` | F1 assert blocking tier1 under `MC2_DISABLE_GOSFX=0` |
| `tests/smoke/artifacts/2026-05-23T23-32-15/*.log` | empirical: `mlr_gate disabled=1` per process in default tier1 |
| `CLAUDE.md:159-167` | "Known issues" gosFX dev-override broken |
| `CLAUDE.md:190-196` | M1..M2.6 shipped slice descriptions |

---

RECON STATUS: COMPLETE
