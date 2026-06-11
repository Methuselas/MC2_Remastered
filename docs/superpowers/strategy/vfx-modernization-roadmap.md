# MC2 VFX Modernization Roadmap

**Status:** Strategy / execution roadmap (no code yet)
**Date:** 2026-06-11
**Scope:** Finish moving all gosFX effect classes off the legacy MLR CPU path onto GPU substrates; define the transparent sorting/blend/depth policy; sweep dead effect code; flip default-on.
**Grounding:** `mclib/gosfx/` (Card/CardCloud/ShardCloud/PointCloud/Tube/Shape/ShapeCloud/DebrisCloud/PertCloud), `mclib/particles/` (batcher, spawn_*), `GameOS/gameos/gos_particle_bridge.cpp`, `docs/vfx-oracle-coverage.md`, `docs/vfx-3d-mesh-substrate-recon.md`, `docs/oracle-dynamic-pipeline-gate.md`, Stage-0 content recon (`docs/superpowers/plans/2026-05-20-B1-Stage-0-content-recon.md`).
**Sibling docs:** consistent with `runtime-bridge-architecture.md`, `telemetry-oracle-cockpit-architecture.md`, `visual-regression-lab-architecture.md` in this directory.

---

## 1. North star

Every effect that ships in stock `mc2.fx` renders through a GPU-driven path that:

1. **Owns all GL state it depends on** — depth test/func/mask, blend func, cull — set explicitly per pass and restored after (the chunk-terrain depth-mask lesson: a bolt-on pass that inherits `glDepthMask(GL_FALSE)` from a prior transparent pass renders color but writes no depth and becomes see-through; the fix was always "set everything, restore everything").
2. **Is provable without eyes** — every migration step ships with an FX_COUNT-style A/B oracle (legacy count vs modern count, mismatch budget = 0) so headless tier1 smokes gate it, with visual confirmation reserved for the things oracles cannot see (sorting artifacts, blend look).
3. **Leaves the legacy MLR draw path deletable.** End state: `MLRClipper::DrawScalableShape` / `DrawEffect` have zero live callers in the game build, gosFX classes become pure simulation + spec containers, and the renderer is one substrate family (billboard batcher + mesh-instance batcher).

The CPU gosFX `Animate()` simulation stays. This roadmap modernizes the *draw* half only — sim parity work (`vfx-gpu-sim-spec.md`) is a separate arc.

## 2. Current-state inventory (verified in code, 2026-06-11)

Spec counts from the aligned ClassID scan of `mc2srcdata/effects/mc2.fx` (904 specs, version 17), recorded in `docs/vfx-oracle-coverage.md`. Class IDs: `mclib/gosfx/gosfx.hpp:20-35`.

| Class | Specs | Status | Code path today | Spawn evidence |
|---|---|---|---|---|
| **CardCloud** | 388 | **DONE** — GPU oracle, default-ON | `mclib/gosfx/cardcloud.cpp` harvest → `mclib/particles/spawn_cardcloud.cpp` → `gos_particle_bridge.cpp` billboard draw; MLR suppressed at oracle exit via `Effect::Draw` | tier1 mc2_10/mc2_24 (~30/frame), interactive confirmed |
| **ShardCloud** | 24 | **DONE** — GPU oracle, default-ON | `shardcloud.cpp` → `spawn_shard.cpp` | mc2_24 combat |
| **Card** | 136 | **DONE** — GPU oracle, default-ON | `card.cpp` → `spawn_card.cpp` | mc2_10/mc2_24 (Flare) |
| **PointCloud** | 2 | **DONE** — oracle wired | `pointcloud.cpp` → `spawn_point.cpp` | 0 births in passive smoke (needs interactive MG fire) |
| **Tube** | 31 | **DONE on branch, UNMERGED** | In nifty: CPU MLR swept mesh (billboard-per-profile oracle disabled `if(false&&...)` in `tube.cpp` — caused "ladder/fence" artifact). Modern ribbon lives on `claude/gosfx-tube-ribbon-1` (+ `claude/tube-additive-discard-fix`); eyes-on evidence in repo-root `.claude/glsg-tube-eyeson/`. **Merge gated on Baseline A capture** (golden frames off `mc2-win64-0.4c` per project memory) — do not merge before. | mc2_24 passive (PPC trail "core") |
| **EffectCloud** | 39 | N/A — container | Delegates to children; no direct draw | children validated |
| **Shape** | 10 | **LEGACY** | `shape.cpp::Draw` → `MLRClipper::DrawScalableShape` (gate re-enabled 2026-05-31, `mlr_gate.cpp` `kDefaultDisabled=false`) | never fires in tier1 (Stage-0 recon: zero `DrawScalableShape` enqueues across all 5 missions); explosion singleton meshes — interactive only |
| **ShapeCloud** | 9 | **LEGACY** | `shapecloud.cpp::Draw` → `DrawScalableShape`; N copies of one `MLRShape`, per-particle scale/rot/color | same — explosion mesh swarms |
| **DebrisCloud** | 34 | **LEGACY** | `debriscloud.cpp::Draw` → `DrawScalableShape`; N *different* meshes, rigid-body debris | same — mech/building destruction |
| **PertCloud** | **0** | **DEAD (verified)** | Class registered (`gosfx.cpp:49,108`), zero specs in `mc2.fx`, zero non-self code references outside registration + `tools/mc2fx` round-trip support. No spawn site can exist for stock content. | none possible |
| **PointLight** | **0** | **DEAD (verified)** | Same: 0 specs in `mc2.fx` | none possible |

**Spawn sites:** all gosFX effects are driven from the 15 `->Draw(` sites in `mclib/mech3d.cpp` (jump jets, smoke, water wakes, dust, crit/destruction effects) plus weapon-bolt sites in `code/weaponbolt.cpp` (hit/miss/waterMiss/muzzle, already oracle-suppression-aware per `vfx-oracle-coverage.md`). The `MC2_FX_COUNT_LOG` counter (`mech3d.cpp:69`, env-gated) instruments these sites; the agent-checkable gate contract is `docs/oracle-dynamic-pipeline-gate.md`. **Known caveat: tier1 smokes are idle fly-throughs — Shape/ShapeCloud/DebrisCloud FX counts are ZERO in smoke; their oracles only produce signal in interactive sessions or a scripted-destruction fixture (see §5).**

## 3. Shared mesh substrate design (Shape / ShapeCloud / DebrisCloud)

All three remaining classes draw `MLRShape` mesh geometry, not billboards. They get **one** substrate, not three — per `docs/vfx-3d-mesh-substrate-recon.md`, extended here.

### 3.1 GpuMeshCache (upload once)

- At spec load (`EffectLibrary` load time, not first-draw), walk each Shape/ShapeCloud/DebrisCloud spec's embedded `MLRShape*`, flatten to position/normal/UV vertex+index buffers, upload to one shared VBO/IBO pool. Key = spec pointer + sub-shape index. ~53 specs total → tiny VRAM.
- DebrisCloud's N-different-meshes case is just N cache entries from one spec.

### 3.2 Instance stream (per frame, CPU sim → GPU draw)

One persistent-mapped SSBO/VBO of instance records shared by all three classes:

```
struct FxMeshInstance {     // 96 bytes
    float localToWorld[12]; // 3x4 row-major (gosFX LinearMatrix4D)
    float colorRGBA[4];     // lifetime curve output
    float scale;            // lifetime curve output
    uint  meshId;           // GpuMeshCache entry
    uint  blendClass;       // see §4 (opaque-ish / alpha / additive)
    uint  sortKeyHi;        // packed camera-distance (see §4.1)
};
```

- **Harvest, don't re-simulate:** each class's `Draw()` keeps its existing `Animate()` math and, at the point where it currently fills `MLRShape` draw info for `DrawScalableShape`, instead appends `FxMeshInstance` records (same pattern as the Card/CardCloud harvest exit: write records, then call `Effect::Draw` children-only to suppress MLR).
- **Per-effect draw vs batched:** batched. One `glMultiDrawElementsIndirect` (or a loop of `glDrawElementsInstancedBaseInstance` per mesh group — fine at these counts) per blend class, instances grouped by `meshId` then sorted per §4. Per-effect draws are an anti-goal: 34 DebrisCloud pieces × several simultaneous explosions is exactly the draw-call storm we just removed elsewhere.
- **Shader:** new `fx_mesh.vert/frag` — instance transform, vertex color × instance color, single texture (most of these specs are vertex-lit untextured or single-texture), lit by the existing simple NdotL terrain light dir (`gos_GetTerrainLightDir`), no shadows cast or received in v1.
- **Bridge:** a sibling of `gos_particle_bridge.cpp` (`gos_fxmesh_bridge.cpp`) — mclib/particles stages, GameOS draws. Keep the layering rule already established in `batcher.h`: mclib/particles may include MLR headers, GameOS cannot.

### 3.3 Why not reuse the billboard batcher

The billboard batcher (`mclib/particles/batcher.cpp` → `gos_particle_bridge.cpp`) expands point records to camera-facing quads in the draw. Mesh instances need real index buffers, per-instance 3x4 transforms, and backface state. Sharing the *frame-lifecycle and GL-state ownership pattern* (BeginGroup/Flush, save-restore) is the reuse; sharing the vertex pipeline is not.

## 4. Transparent sorting / blend / depth policy (currently OPEN — this section closes it)

### 4.1 Blend modes inventory (grepped from code)

gosFX blend state is `MLRState::AlphaMode` (consumed at `card.cpp:520`, `cardcloud.cpp:578`, `pointcloud.cpp:497`, `shardcloud.cpp:346`, `tube.cpp:1211`, `effect.cpp:212`):

| MLRState mode | GL mapping | Class |
|---|---|---|
| `OneZeroMode` | blend off (opaque) | **opaque FX class** |
| `AlphaMode` | `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` | **alpha class** |
| `OneOneMode`, `AlphaOneMode` | `GL_SRC_ALPHA, GL_ONE` (already collapsed to "additive" in `gos_particle_bridge.cpp:602-607`) | **additive class** |

The bridge today already does the right baseline (`gos_particle_bridge.cpp:504-644`): depth test ON, `glDepthMask(GL_FALSE)`, per-group blend func, full save/restore of prior state. **What's missing is sorting and an explicit policy** — groups are texture-grouped, draw order is harvest order.

### 4.2 Policy (binding)

**Pass order within the frame:** opaque world (incl. chunk terrain, depth prepass) → opaque FX → water/decals → **additive FX** → **alpha FX** → post. Rationale: additive is order-independent among itself, so it goes first within the transparent FX block; alpha-blended smoke goes last, sorted.

**Depth-write rules per effect class:**

| Effect class | Depth test | Depth write | Sort |
|---|---|---|---|
| Opaque FX (`OneZeroMode` — Shape/DebrisCloud solid debris meshes) | ON, `GL_GEQUAL` (reverse-Z) | **ON** | front-to-back optional (early-Z), correctness-free |
| Additive FX (flares, sparks, tracers, Tube cores) | ON | **OFF** | none required (commutative); keep texture-group order for batching |
| Alpha FX (smoke, dust, debris-smoke ShapeCloud) | ON | **OFF** | back-to-front, per §4.3 |

**Reverse-Z:** the engine runs `glDepthFunc(GL_GEQUAL)` with reverse-Z (established by the terrain chunk work, incl. the net `-0.004` depth-fudge rule). FX passes must set `GL_GEQUAL` explicitly — *never* assume it — and any FX depth bias must be applied as a pre-divide vertex bias, not `gl_FragDepth` (same load-bearing rule as terrain, `HANDOFF_2026_06_09_terrain_lod_chunk_phase10_fidelity_cutover_prep`).

**GL state ownership (the terrain lesson, made law):** every FX submit function sets, explicitly, on entry: depth test enable, depth func, depth mask, blend enable, blend func, cull face state — and restores the saved values on exit. The bridge already complies; the mesh bridge and the Tube ribbon pass must comply before merge. Add a debug assert (`MC2_RENDER_CONTRACT_ASSERT=1` hook in `mclib/render_contract.*`) that snapshots depth-mask before/after each FX pass.

**Depth prepass interaction:** the foliage depth prepass (`MC2_STATIC_PROP_DEPTH_PREPASS`) writes depth before color. FX transparent classes test against that depth normally — no interaction beyond "FX pass runs after all depth writers." Opaque FX meshes do NOT join the prepass in v1 (they're transient; prepass is for static props).

### 4.3 Sort key proposal

64-bit key, radix/std::sort on CPU at Flush time (instance counts are hundreds, not millions — CPU sort is free):

```
bits 63-62: pass class   (0=opaque, 1=additive, 2=alpha)   — primary split
bits 61-40: view depth   (22-bit quantized eye-space -Z, REVERSED for alpha
                          class so ascending sort = back-to-front; forward
                          for opaque = front-to-back early-Z)
bits 39-24: texture/material id                              — batch coherence
bits 23-0 : submission sequence                              — stable tiebreak,
                          preserves legacy intra-effect layering (gosFX relies
                          on emission order within one effect)
```

Depth quantization uses eye-space distance, not post-projection depth — immune to reverse-Z confusion by construction. Sorting granularity is per-instance for mesh FX and per-*particle-group* (one CardCloud draw bucket) for billboards in v1; per-particle billboard sorting is deferred until a visible artifact demands it (smoke self-intersection is mostly hidden by soft alpha at MC2 camera distances).

## 5. Oracle + parity gating

Pattern: the proven FX_COUNT A/B family (`docs/oracle-dynamic-pipeline-gate.md`, `MC2_FX_COUNT_LOG` at `mech3d.cpp:69`).

- **Per-step count oracle:** legacy path increments `legacy_draw[class]`, modern path increments `modern_draw[class]` at the harvest exit; shadow mode (`MC2_VFX_MESH_AB=1`) runs both producers, renders legacy, asserts per-frame per-spec counts equal. Budget = 0 mismatches.
- **Geometry oracle:** in AB mode, hash the first N instance transforms+colors per spec per frame on both sides (legacy `DrawScalableShape` dinfo vs new `FxMeshInstance`); mismatch dumps spec name + frame. This is the analog of the MECH_MATERIAL_GPU mismatch counters — agent-checkable, mission-independent.
- **The smoke blind spot:** tier1 is idle — Shape/ShapeCloud/DebrisCloud never spawn (Stage-0 recon proved zero `DrawScalableShape` enqueues). Gates therefore need a **destruction fixture**: a scripted session (or `MC2_FX_FORCE_SPAWN=<specname>` debug spawner injected at a mech3d.cpp site) that births each of the 53 mesh specs deterministically. Build the spawner in Slice 1 — without it every later slice is unverifiable headlessly.
- **State-ownership oracle:** render-contract assert that depth-mask/blend state entering the FX pass equals state leaving it, and that the FX pass never runs with inherited depth-mask OFF on its opaque sub-pass.
- **Visual gates:** golden-frame diffs via the visual-regression lab (`visual-regression-lab-architecture.md`) on the destruction fixture, gated AFTER Baseline A exists (same sequencing as the Tube merge).

**Flag naming** (consistent with existing `MC2_VFX_*` / `MC2_GPU_*` family, all default-OFF until §8 flip):

| Flag | Meaning |
|---|---|
| `MC2_VFX_MESH=1` | enable GPU mesh substrate render (suppresses `DrawScalableShape` at harvest exit) |
| `MC2_VFX_MESH_AB=1` | shadow A/B: both producers, legacy renders, count+hash oracles active |
| `MC2_VFX_MESH_LOG=1` | per-class stderr counters (mirrors `MC2_GPU_PARTICLES_LOG`) |
| `MC2_VFX_SORT=0` | kill-switch for the new sort (falls back to harvest order) |
| `MC2_FX_FORCE_SPAWN=<spec>` | debug deterministic spawner for fixtures |

## 6. Dead effects sweep list

Verified-dead, delete in Phase 4 (after default-on, never before):

- **PertCloud** — `mclib/gosfx/pertcloud.{cpp,hpp}` (~75-field spec save), registration at `gosfx.cpp:49,108`, `PertCloudClassID` enum entry, `MLRNGonCloud` backend if it becomes caller-less. 0 specs in `mc2.fx`. **Caveat:** `tools/mc2fx` (`mc2fx_core.cpp:66,288,445,532`) supports PertCloud round-trip and `mc2x.fx` mods could theoretically author one — decision: keep the *spec read/skip* path (don't crash on modded .fx), delete the *render* path. Loader logs `[VFX] PertCloud spec ignored (retired)`.
- **PointLight** — 0 specs in `mc2.fx`; same keep-parse/drop-render treatment (`pointlight.{cpp,hpp}`). Note `mclib/particles/light_manager.cpp` exists for the future light-illumination arc — do not delete that.
- **Tube billboard-per-profile oracle stub** — the `if(false&&...)` dead branch in `tube.cpp` (~line 1190s) once the ribbon branch merges.
- **MLR work leaves** — after all classes migrate: `DrawScalableShape`, then `DrawEffect` (Tube-ribbon merge removes its last caller), then the `mlr_gate.cpp` gate itself. `DrawShape`/`DrawScreenQuads` already have zero live callers (Stage-0 recon) — sweep candidates immediately, behind their own commit.
- **Orphan audit:** one slice greps every `gosFX::*ClassID` against the spec scan to catch any other zero-spec classes before deleting.

## 7. Anti-goals

- **No GPU simulation in this arc.** CPU `Animate()` stays authoritative; `vfx-gpu-sim-spec.md` is a separate future arc.
- **No per-particle billboard sorting in v1** (group-level only) — wait for a demonstrated artifact.
- **No FX shadows** (cast or received) in v1.
- **No per-effect draw calls** in the mesh substrate.
- **No editing of effect *behavior*** — parity means visually indistinguishable from the MLR path on the destruction fixture, not "improved."
- **No Tube merge, no default-on flip, no MLR deletion before Baseline A exists** (project gate per memory).
- **No new render passes that inherit GL state.** Set everything, restore everything.

## 8. Phased roadmap

```
Phase 0  Foundations: destruction fixture + MC2_FX_FORCE_SPAWN + sort/state
         policy implemented in the EXISTING billboard bridge (low-risk venue)
Phase 1  Shape       (simplest: 1 mesh, 1 instance, Singleton lifecycle)
Phase 2  ShapeCloud  (adds per-particle instance fan-out, same mesh)
Phase 3  DebrisCloud (adds multi-mesh per spec, rigid-body transforms)
         — each phase: AB-shadow → oracle 0-mismatch on fixture →
           visual golden vs MLR → MC2_VFX_MESH covers class
Phase 3.5 Tube ribbon merge (when Baseline A lands; independent of 1-3)
Phase 4  Dead-code sweep (PertCloud/PointLight render paths, Tube stub,
         DrawShape/DrawScreenQuads, orphan audit)
Phase 5  Default-on flip: MC2_VFX_MESH default-ON (opt-out =0, mirroring
         the a7b090be terrain cutover pattern: single-source gate fn),
         then DrawScalableShape/DrawEffect retirement once a full release
         soak passes
```

**Risks (per phase):**

- *Phase 0:* sort reorders existing billboard output → subtle look changes in shipped FX. Mitigate: `MC2_VFX_SORT=0` kill-switch + golden frames on mc2_10/mc2_24 before/after.
- *Phases 1-3:* `MLRShape` flattening fidelity (multi-pass materials, vertex color formats) — the recon doc warns spec meshes embed MLR-era state; budget a decode slice. DebrisCloud rigid-body transform extraction is the most state-entangled (per-piece velocity/rotation live in the effect, not the spec).
- *Phase 3.5:* Tube additive blending vs new sort order — the `tube-additive-discard-fix` branch exists because this already bit once; re-run its eyes-on protocol (`.claude/glsg-tube-eyeson/`).
- *Phase 4:* deleting parse paths breaks modded `mc2x.fx` loads — keep read/skip, only delete render.
- *Phase 5:* the classic "stale deploy" trap — verify deployed exe mtime ≥ flip commit (v0.4 AND 0.4c, they are different exes per project memory).
- *Cross-cutting:* an FX pass leaving depth-mask OFF breaks the next pass — the render-contract assert is the tripwire; land it in Phase 0.

## 9. First 5 implementation slices

1. **`MC2_FX_FORCE_SPAWN` + destruction fixture** — debug spawner at a mech3d.cpp Draw site that births a named spec at the camera focal point; smoke-runnable list mode that cycles all 53 mesh specs. Gate: each spec logs a spawn+draw count under `MC2_FX_COUNT_LOG`.
2. **Sort + explicit-state pass in the billboard bridge** — implement §4.2/§4.3 in `gos_particle_bridge.cpp` (pass-class split, 64-bit key, explicit `GL_GEQUAL`+state save/restore audit, render-contract depth-mask assert). Gate: tier1 5/5, golden diff on mc2_10/mc2_24 within threshold, `MC2_VFX_SORT=0` parity.
3. **GpuMeshCache + Shape AB-shadow** — spec-load-time `MLRShape` flatten/upload; `Shape::Draw` harvest exit writes `FxMeshInstance` in shadow mode (`MC2_VFX_MESH_AB`), legacy still renders. Gate: count+transform-hash oracle 0 mismatches on the Shape specs of the fixture.
4. **`fx_mesh` draw path live for Shape** — `gos_fxmesh_bridge.cpp` + `fx_mesh.vert/frag`, batched instanced draw, policy-compliant state. Gate: `MC2_VFX_MESH=1` renders Shape, fixture golden vs MLR, tier1 5/5 both flag states.
5. **ShapeCloud onto the same substrate** — per-particle instance fan-out from `shapecloud.cpp` harvest. Gate: same oracle trio; then DebrisCloud is a repeat with multi-mesh.

## 10. Follow-up prompts (for Opus/Codex sessions)

1. *"In worktree `.claude/worktrees/nifty-mendeleev`, implement Slice 1 of `docs/superpowers/strategy/vfx-modernization-roadmap.md`: an env-gated `MC2_FX_FORCE_SPAWN=<specname|@all_mesh>` debug spawner hooked at a `mclib/mech3d.cpp` effect Draw site, spawning at camera focal point, integrated with the existing `MC2_FX_COUNT_LOG` counter (mech3d.cpp:69). Must be zero-cost when unset. Verify with the canonical tier1 smoke and a manual run listing per-spec spawn/draw counts for all Shape/ShapeCloud/DebrisCloud specs."*
2. *"Implement Slice 2 (transparent FX sort + explicit GL state policy) per §4 of `docs/superpowers/strategy/vfx-modernization-roadmap.md` in `GameOS/gameos/gos_particle_bridge.cpp`: pass-class split (opaque/additive/alpha), 64-bit eye-space-depth sort key, explicit GL_GEQUAL reverse-Z depth state, save/restore audit, render-contract depth-mask assert, `MC2_VFX_SORT=0` kill-switch. Golden-frame diff mc2_10/mc2_24 before/after; tier1 5/5 both flag states."*
3. *"Design-review then implement Slices 3-4 (GpuMeshCache + Shape GPU path) per §3 of `docs/superpowers/strategy/vfx-modernization-roadmap.md`, grounding in `docs/vfx-3d-mesh-substrate-recon.md` for the MLRShape flattening details. Run `adversarial-plan-review` on the MLRShape decode plan before coding — multi-pass material handling is the named risk. AB oracle (`MC2_VFX_MESH_AB`) must show 0 count/hash mismatches on the destruction fixture before the live path lands."*
