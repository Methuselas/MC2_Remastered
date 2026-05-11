# GPU-Driven Indirect Command Generation — Stage 0 Design (Phase C)
## Completing the GPU transfer for buckets already half-ported

> **Companion to** the dispatch prompt at [`docs/superpowers/plans/progress/2026-05-11-gpu-driven-rendering-prompt.md`](../plans/progress/2026-05-11-gpu-driven-rendering-prompt.md).
>
> **Thesis:** Phase C is a **completion slice, not a greenfield slice.** Track A (object admission), Track B (widened static-prop registry), Track C (GPU compute cull + readback + lifecycle gates), Phase 1 (GPU terrain lighting), PR1 (indirect-terrain SOLID), PR2c (MINE), and the renderWater fast path have all already moved the **heavy GPU-resident pieces** to the GPU. What remains on CPU per-frame for each of these buckets is a **thin-record pack loop** that bridges already-GPU data (Phase 1's lighting SSBO, PR1's recipe SSBO, Track C's visibility) back through a CPU mirror into a CPU-written SSBO that the GPU then consumes. Phase C closes that GPU→CPU→GPU loop by collapsing the bridge into a single compute dispatch that reads directly from the upstream GPU SSBOs.
>
> Stage 0 deliverable: enumerate the **GPU-share-vs-CPU-share for each shipped bucket**, identify the specific CPU residual that Phase C eliminates per bucket, define the compute-shader input/output contracts and sync pattern. The implementation plan (per-bucket stages) is authored against this design.

**Worktree:** `claude/gpu-driven-rendering` (branched from `claude/nifty-mendeleev` @ `5667023`).
**Sibling Phase B:** `claude/pre-bake-terrain` @ `5667023` — **mostly shipped per-bucket** (see "Phase B / Phase C boundary" section below). Per-bucket static recipe SSBOs that Phase C consumes have shipped piecemeal: PR1 for SOLID, water-recipe for water, **PR2c for MINE — all stages incl. 2c shipped 2026-05-08** (commits `619f49f` + `6d4a6f7`), PR2a M2c-emit DELETED 2026-05-08 (commit `7c3a382`). The **only Phase B work still pending** is Overlay recipe (Slice B2) + mask+dispatch infrastructure (Slice B4, Phase B headline). Cross-worktree ping received from Phase B session 2026-05-11: their Stage 0 adversarial review dropped Slices B1 (mine) and B3 (M2c) as already-shipped. See Phase B design at `A:\Games\mc2-opengl-src\.claude\worktrees\pre-bake-terrain\docs\superpowers\specs\2026-05-11-pre-bake-terrain-design.md` for the locked 2-slice arc.

---

## Documentation discipline (load-bearing)

Every symbol, file:line, struct field, env flag, and binding-point number cited below was grep-verified at write-time against the worktree's source at HEAD `5667023`. Each citation appears in the Verification Appendix at the end with status M (matches) / D (divergent) / NF (not found). Per CLAUDE.md, this discipline supersedes any conflicting reference in prior memory files (which decay).

The framing-correction below is the most consequential write-time correction: the prompt names this slice "GPU-driven indirect command generation," but the actual CPU hot work in every shipped indirect path is the **thin-record build**, not the DAIC struct write. The design treats the indirect-cmd struct as a byproduct of the compute pass, not the headline output.

---

## Status

- Stage 0 (this design): **v3** — addresses targeted-review findings from v2 commit (`802aca4`) against the 5 new v2-introduced claims (questions A–E). v3 fixes: (D-CRITICAL) `gos_terrain_lighting::GetOutputSsbo()` accessor does not exist in Phase 1's public API — v3 commits to Phase 1 publishing it as a one-line getter (the only Phase-1-source touch Phase C v1 requires); (E-MAJOR) Phase C SOLID compute dispatch placement corrected — lives inside `ComputePreflight()` at `gos_terrain_indirect.cpp:1605` invoked from `terrain.cpp:1792` (Tracy zone `Terrain::geometry quadSetupTextures`), not `gamecam.cpp`; (C-MAJOR) Phase 1 parity coupling decided as option (a) — smoke-runner sets both `MC2_GPU_DRIVEN_PARITY=1` AND `MC2_TERRAIN_LIGHTING_PARITY=1` (Phase 1's `IsParityCheckEnabled()` caches the env in a `static const bool` so runtime toggle is structurally impossible). Questions A and B passed clean (verdict from v2 reviewer).
- v2 history (preserved for traceability): addressed adversarial-review findings from v1 commit (`13a0c06`) substance + boundary pass — (a) fictional "256-entry / 99% hit rate" `addLightDataStructure` claim, (b) `g_waterRecipeSSBO` symbol misname (actual: `g_recipeBuffer`), (c) MINE Stage 2c boundary-table contradiction (Stage 2c IS shipped on baseline), (d) MINE has no per-frame thin-record pack (uses `glDrawArrays` and a dirty-flag lazy rebuild, not MDI), (e) OVERLAY three-way scope contradiction, (f) binding-point collision in proposed SOLID compute program, (g) water's actual draw shape is 2× `glDrawArrays` not 1, (h) `s_frameSolidArmed` flow has no spec under GPU-driven, (i) Phase 1 lighting frame-pipelined latency breaks the byte-equality parity claim, (j) ring-slot semantics under GPU-driven SOLID not addressed. Cross-session merge: Phase B session edited three additional stale prose spots (intro, decision #1, ordering paragraph) that v2's MINE-removal edits missed; merged into the current state along with a Stage 4→3 OVERLAY renumbering cleanup (commit `802aca4`).
- Scope locked per user direction: Phase C targets the **combined** zones `GameCamera::render textureManagerRenderLists` (1.35 ms) + `GameCamera::render water` (814 µs at the wrapping zone — actual cost lives in `render waterFastPath` when armed) + `GameCamera::render objects` (533 µs) = ~2.7 ms current → ~500 µs target = ~2.2 ms saved at wolfman-mc2_10. **Frame is CPU-bound by ~15 ms** at this baseline, so CPU savings translate directly to frame time.
- Stage 1+: planned, blocked on this v2 design's adversarial-review re-pass clearance.

---

## Target zones (Tracy-anchored)

Phase C's perf gate is empirical, anchored against three specific Tracy zones in `code/gamecam.cpp`'s render activeScene loop. Combined current cost ≈ **2.70 ms/frame mean**; combined target ≤ **500 µs/frame** (one compute dispatch per bucket + per-bucket MDI submit). Win: ~**2.2 ms/frame**.

### Measurement context (load-bearing)

The 1.35 ms / 814 µs / 533 µs numbers below were captured on **mc2_10 at full zoom out (wolfman zoom)** — the worst-case-CPU configuration per `wolfman_is_max_zoom.md` (`GameVisibleVertices=200`, altitude 6000, no fog/haze). This is the right anchor because:

- It is the stress configuration. Per-frame CPU thin-record packing scales with visible-quad count; wolfman zoom maximizes that count. Any other zoom undershoots the cost.
- It is the configuration used for visual-correctness smoke testing across the renderer-modernization arc (see "Wolfman mode status" in the worktree CLAUDE.md and `wolfman_is_max_zoom.md`).
- It exposes the `addLightDataStructure` dedup churn at peak — the hash-dedup walk's cost is proportional to the number of distinct render-state setups per frame, which scales with visible-actor count.

**Measurements at narrower zoom or other missions will show smaller absolute costs.** The Phase C perf gate is therefore expressed as:

1. **mc2_10 wolfman zoom is the anchor** — Stages 2+3+4 must drop Z1 by ≥1.0 ms in this configuration; Stage 1 must drop Z2 by ≥80% in this configuration. The headroom budget (~2.2 ms saved) is wolfman-mc2_10-specific.
2. **Tier1 5/5 PASS triple** must still hold at default zoom on all 5 missions (mc2_01 / mc2_03 / mc2_10 / mc2_17 / mc2_24) — but Tier1's perf delta numbers will be smaller absolute than the wolfman anchor. Tier1 is the regression gate; wolfman-mc2_10 is the empirical anchor.
3. Per-mission Tracy deltas under wolfman zoom are captured in Stage N exit reports (Stage 1 water under wolfman mc2_01 — water-heavy — additionally; Stage 2-4 terrain buckets under wolfman mc2_10).

### The three zones (line numbers grep-verified at write-time)

| Zone | Tracy label | Source | Current cost | Phase C scope inside this zone |
|------|-------------|--------|--------------|--------------------------------|
| **Z1** | `GameCamera::render textureManagerRenderLists` | `code/gamecam.cpp:244-248` wraps `mcTextureManager->renderLists()` at `:245` + `endFrameTexResolve()` at `:246` | **1.35 ms mean** (22.97% of render activeScene). Within Z1, `PatchStream.Flush` (one of several sub-zones consumed by Phase C) measures mean 662 µs / median 626 µs / P99 938 µs per its histogram. Z1's 1.35 ms is the combined wall-clock of ALL per-bucket flush + dispatch CPU work, not any single sub-zone. | Per-bucket CPU work that runs per-frame inside `renderLists()`: thin-record packing for terrain SOLID + OVERLAY (when armed); per-corner `lightRGB`/`fogRGB` copy from CPU `ScreenVertex` mirror into thin-record SSBO (the dominant per-quad work term); `addLightDataStructure()` lookups at `mclib/txmmgr.cpp:1023` (function-zone `"addLightDataStructure scan"` at `:1028` — already hash-deduplicated, per-call cost is small, but it's invoked many times per frame); per-frame texture-handle resolution cache; per-bucket batch list aggregation. **Note:** the v1 design cited a "256-entry / 99% hit rate" property of the dedup map. That was fictional — the map is an unbounded `std::unordered_map<uint64_t, uint32_t>` at `mclib/txmmgr.cpp:901` with initial capacity 128. The Z1 ≥1.0 ms target rests on the combined wall-clock measurement, NOT on attributing the savings to dedup-walk elimination specifically. |
| **Z2** | `GameCamera::render water` | `code/gamecam.cpp:217` wraps `land->renderWater()` at `:218` | **814 µs mean** (13.80% of render activeScene) — note: when `MC2_RENDER_WATER_FASTPATH` is armed (default-on per `renderwater_fastpath_stage2.md`), the legacy zone at `:217` early-returns and a separate `GameCamera::render waterFastPath` zone at `:255-256` (wrapping `land->renderWaterFastPath()`) carries the live cost. Phase C's water-thin-record pack work lives in the fast-path zone, NOT the legacy zone. | `gos_terrain_water_stream.cpp` `UploadAndBindThinRecords` thin-record pack; per-frame water animation state (`Terrain::frameCos` tick) — animation state stays CPU per the design doc's "no new compute scope" rule; water MDI cmd prep |
| **Z3** | `GameCamera::render objects` | `code/gamecam.cpp:212` wraps `ObjectManager->render(true, true, true)` at `:213` | **533 µs mean** (9.03% of render activeScene) | Coordination point with `gpu_static_prop_batcher` (already partially GPU-driven via Track B). Per-mech / per-vehicle dispatch chain (`TG_Shape::Render` virtual chain) is **OUT** — gpu-mech-branch territory. Per-object visibility check + state setup is a Track-D / object-offload question; Phase C v1 does NOT subsume it. |

### Prep work also in scope (called from outside the render activeScene)

| Zone | Tracy label | Source | Phase C scope |
|------|-------------|--------|---------------|
| **Z1-prep** | `GameLogic.Mission.TextureManager` | `code/mission.cpp:527` wraps `mcTextureManager->update()` (guarded by `!isPaused() \|\| MPlayer` at `:526`) | Per-frame texture cache update; precedes Z1's `renderLists()`. **Note:** the pause-guard at `:526` is load-bearing per `pause_unpause_diagnostic_for_static_render_bugs.md` — Phase C does NOT change the pause semantics; it changes only what `update()` builds. |

### Combined budget

```
Current   :  Z1  1.35 ms  +  Z2  0.81 ms  +  Z3  0.53 ms  =  ~2.70 ms / frame
Target    :  Z1+Z2+Z3 combined ≤ 0.50 ms / frame
                                  (one compute dispatch per bucket + per-bucket MDI submit + barrier)
Headroom  :  ~2.20 ms / frame budget for the slice
```

Per-bucket Tracy delta gate per the prompt:
- Z2 (water) — Stage 1 must drop ≥80% (water Stage 3 precedent was 78–85%; pattern is the same).
- Z1 (textureManagerRenderLists) — Stages 2+3+4 (terrain buckets) collectively must drop ≥1.0 ms (most of the 1.35 ms is the per-bucket thin-record packing + addLightDataStructure churn).
- Z3 (objects) — informational only in v1; aggressive Phase C work here is deferred pending gpu-mech-branch's TG_Shape::Render audit. A small drop is acceptable from static-prop coordination polish; a large drop would indicate scope creep.

### Scope boundary against Phase B (per "Phase B / Phase C boundary" below)

> Out-of-scope for Phase C per the target-zone framing:
> - The TERRAIN bucket's **static data** (recipe SSBO contents) — that's Phase B's bake. Phase C reads Phase B's static SSBO if Phase B ships first; otherwise Phase C reads the per-frame CPU-built recipe data (slower but works).
> - **Mech rendering** — gpu-mech-branch territory. A Stage 0 cross-branch audit (called out under "Exit criteria") documents Phase C's compatibility with the gpu-mech-branch's compute pipeline pattern; no Phase C v1 code lands in the mech path.

---

## Phase B / Phase C boundary (READ FIRST if you're the sibling session)

The renderer-modernization arc currently runs **two parallel slices**: Phase B (static bakes, sibling worktree `claude/pre-bake-terrain`) and Phase C (per-frame GPU compute, this worktree `claude/gpu-driven-rendering`). They share the post-Phase-1-merge nifty-mendeleev baseline at commit `5667023`. The work in each is non-overlapping; the output of Phase B is the input to Phase C's compute shaders.

### The boundary table

| | Phase B (sibling session) | Phase C (this session) |
|---|---|---|
| **Worktree** | `claude/pre-bake-terrain` | `claude/gpu-driven-rendering` |
| **Time scope** | Mission-load (one-shot baking) | Per-frame (compute dispatch each frame) |
| **Code shape** | Build static SSBOs from `MapData::blocks[]` and equivalent pure-data sources | Compute shaders that consume static SSBOs + dynamic state (terrainMVP, Phase 1 lighting SSBO, Track C visibility) |
| **In scope** | recipe SSBO bakes; PR2c MINE Stage 2c wire-up; OVERLAY recipe; optional static lighting bake | per-bucket cull/pack compute shader (Beta two-dispatch pattern); per-bucket indirect-cmd writer; per-bucket killswitches; parity infra |
| **Out of scope** | per-frame draw work; compute dispatch wiring; thin-record SSBO writes | static bake construction; mission-load recipe building; map-data iteration loops |
| **Bucket coverage** | left column of the half-ported inventory ("Already on GPU (shipped)") | right column ("Still on CPU per frame") |

### Per-bucket coordination (precise — read against your own table row by row)

| Bucket | Phase B status (sibling) | Phase C dependency |
|--------|-------------------------|--------------------|
| **Water main-emit** | ✅ Recipe shipped (renderWater Stage 1+2+3, 2026-04-30) | Phase C Stage 1 reads existing water recipe SSBO. No Phase B work needed for Phase C Stage 1 to ship. |
| **Terrain SOLID main-emit** | ✅ Recipe shipped (PR1, commit `e22fa3a`, default-on 2026-05-01) — `g_recipeSSBO` in `gos_terrain_indirect.cpp` | Phase C Stage 2 reads existing PR1 recipe SSBO. No Phase B work needed for Phase C Stage 2 to ship. |
| **Mine (PR2c)** | ✅ Stages 0c/1c/2c **all shipped** on baseline (commit `6d4a6f7`, default-on). Per `gos_terrain_indirect.cpp:139` `IsMineEnabled()` comment: "Tier1 5/5 PASS with arming verified across PR2c Stages 0c/1c/2c." | **Not a Phase C bucket.** MINE has no per-frame thin-record pack — it's a static-bake VBO + `glDrawArrays` with dirty-flag-gated rebuild. There is no per-frame CPU residual for Phase C to attack. v1 design's "Stage 3 Mine" entry is **removed**. |
| **Overlay (PR2b)** | ⬜ No recipe (sibling Phase B Slice B2 may publish one) | Phase C Stage 3 is **conditional**: if Phase B ships an overlay recipe in the same window, Phase C Stage 3 reads it. If not, **Phase C Stage 3 defers** — building a minimal in-line recipe in Phase C would violate the boundary (static-bake work in the per-frame compute session). |
| **Detail (M2c / PR2a)** | ⬜ Candidate delete (sibling session may retire it) | Not a Phase C bucket. Dead path. |
| **Lighting** | ⬜ Static lighting bake possible (sibling-decided) | Phase C reads Phase 1's per-frame compute lighting SSBO. Whether Phase 1 (the *dynamic* GPU compute lighting; merge commit `93d3cbd` with stages `594add9 / eda2431 / ff8de07 / ff35f03`) is treated as "shipped" or "foundational-but-not-yet-shipped" affects Phase C's "eliminate lighting CPU bounce" headline win — but does not block Phase C from shipping; the binding for Phase 1's output is the seam, and a CPU-fallback path is the obvious degradation. |
| **Vertex projection** | n/a (camera-dependent — not a static bake) | Per `vertex_project_loop_d1_asymptotic.md` D1 hoist closed asymptotic (compiler ceiling reached). SIMD / GPU port deferred. Phase C does NOT subsume vertex projection in v1; the per-bucket compute shader does its own per-quad MVP transform, but it does not consume `vertexProjectLoop`'s output. |
| **Track C compute cull** | n/a | ✅ C0+C1a+C1b+C2+C3 shipped on nifty-mendeleev. Phase C reads Track C's visibility-mirror output for buckets that have one (static-prop, mech/GV — neither is in Phase C v1). Terrain/water buckets do their OWN per-quad projectZ in the compute shader, not Track C cull. |

### The three coordination decisions

1. **Mine — fully shipped on baseline; not a Phase C bucket.** PR2c Stages 0c/1c/2c all shipped 2026-05-08 (commits `619f49f` + `6d4a6f7`). MINE has no per-frame thin-record pack (uses `glDrawArrays` + dirty-flag lazy rebuild); no per-frame CPU residual for Phase C to attack. v1 design's "Stage 3 Mine" entry is **removed in v2** — confirmed by Phase B Stage 0 adversarial review 2026-05-11 dropping Phase B Slice B1 for the same reason.
2. **Overlay — Phase C Stage 3 blocks on Phase B Overlay recipe (Slice B2).** This is the one place Phase C is gated on Phase B. If the sibling session decides not to build an overlay recipe, Phase C Stage 3 falls out of v1 and OVERLAY remains scaffold-only.
3. **Lighting — Phase C reads Phase 1's existing SSBO; static lighting bake is sibling-decided and orthogonal to Phase C.** If the sibling session adds a *static* lighting bake under Phase B, Phase C can be re-pointed at it post-v1 (same binding-swap seam). It does not change v1.

### What Phase B should NOT do that would surprise Phase C

- Do not extend `WaterThinRecord`, `TerrainQuadThinRecord`, or PR2c MINE's thin-record struct. Phase C is byte-stable against these layouts; extending them violates `cpp_glsl_ubo_struct_lockstep.md` unless Phase C is updated in the same commit.
- Do not change `DrawArraysIndirectCommand` struct layout (it's GL-spec-mandated 16 B / 4 GLuints anyway).
- Do not rename `g_recipeSSBO` (PR1) or the water recipe SSBO without a Phase C commit in lockstep — these are the Phase C binding R0 sources.
- Do not introduce a GPU→CPU readback path that Phase C compute shaders would have to participate in. Phase C's hot path is GPU→GPU only per `substrate_coalesce_sync_point_lesson.md`.

### What Phase C will NOT do that would surprise Phase B

- Will not write to recipe SSBOs at runtime. Recipes are read-only from compute's perspective.
- Will not touch `MapData::blocks[]` or any other pure-data mission-load source.
- Will not change the on-disk asset format or any sidecar manifest.
- Will not delete the legacy CPU pack loops in v1 (gate them off, leave in tree).

### Stage ordering (cross-slice)

Either slice can ship first; they're independent. If Phase B ships first, Phase C v1 has Overlay recipe available (Mine and lighting are independent of either slice — mine is fully shipped on baseline; Phase 1 lighting compute is the GPU-resident SSBO Phase C reads directly). If Phase C ships first, Phase B integrates against an already-running compute path and Phase C swaps bindings post-v1. **The only ordering constraint is:** if Phase B ships an overlay recipe and Phase C wants OVERLAY in v1, Phase B's overlay recipe must land before Phase C Stage 3.

---

## Half-ported inventory — what's already on GPU vs what's still on CPU per bucket

The buckets in scope are not greenfield. Each one already has a substantial GPU share shipped under prior tracks/slices. Phase C's job is to identify the specific CPU residual on the hot path and finish the transfer. This table is the load-bearing inventory for Stage 0's per-bucket inclusion decision.

| Bucket | Already on GPU (shipped) | Still on CPU per frame (Phase C's target) | Origin slice |
|--------|--------------------------|-------------------------------------------|--------------|
| **Water** | Single GPU draw of pre-packed thin records (`renderWaterFastPath`, default-on); recipe SSBO mission-static; VS expands to 6 verts/quad; FS unchanged from legacy | `UploadAndBindThinRecords` per-quad pack loop at `gos_terrain_water_stream.cpp:345-...:478` (walks quadList, projectZ-gates each quad, copies per-corner lightRGB/fogRGB from CPU vertex pool into thin record). Single `glDrawArrays(thinCount*6)` — NOT indirect today. | renderWater Stage 1+2+3 (2026-04-30) |
| **Terrain SOLID** | Recipe SSBO mission-static (`g_recipeSSBO`); MDI draw via `glMultiDrawArraysIndirect` (default-on); colormap atlas; cement multi-sampler at unit 3; PR1's frag-side SSBO fetch via flat RecordIdx varying | `PackThinRecordsForFrame` at `gos_terrain_indirect.cpp:1377` (stages up to 65536 records into stack-local shadow per frame, walks quadList, recipe lookup by `vertexNum`, per-corner mutable-state copy) + trivial 16 B `BuildIndirectCommands` at `:1566` | PR1 (commit `e22fa3a`, default-on 2026-05-01) |
| **Terrain MINE (PR2c)** | Static-bake VBO (mission-load + dirty-flag lazy rebuild via `RebuildMineStaticVBOIfDirty` at `gos_terrain_indirect.cpp:1923`); single `glDrawArrays` at `gameos_graphics.cpp:2609`; default-on commit `6d4a6f7`. Lives in its own Tracy zone `Render.TerrainMines` at `mclib/txmmgr.cpp:1812`, NOT inside `textureManagerRenderLists`/Z1. | **No per-frame thin-record pack.** CPU residual is dirty-flag-gated rebuild (cold path on terrain-state mutation, not every frame) + the one `glDrawArrays` call. There is no per-frame CPU work for Phase C to GPU-ify here. | PR2c |
| **Terrain DETAIL (PR2a)** | n/a — path is dead | n/a | dead since commit `521d83a` |
| **Terrain OVERLAY (PR2b)** | Designed (`2026-05-08-pr2b-overlay-indirect-design.md`), scaffold-only at `gos_terrain_indirect.cpp` (`IsFrameOverlayArmed()` returns `false`) | The full draw path. This is the only exception to the "completion" framing — for OVERLAY there is no CPU baseline to complete *from*; we ship it as GPU-driven from inception. | not yet shipped |
| **Phase 1 terrain lighting** | Compute shader writes lightRGB/fogRGB to SSBO; 3-slot ring; default-on (commit `ff35f03`) | CPU readback of the SSBO back into `ScreenVertex::lightRGB`/`fogRGB` mirror (1-frame pipelined latency), THEN re-pack into thin records on the next CPU pack pass. The mirror's only purpose is to feed the four buckets above. | Phase 1 (parallel-amdahl, 2026-05-10) |
| **Static-prop substrate** | Substrate upload (Track C C0); compute cull (C1a); GPU-authoritative `instanceCount` (C1b); fenced readback ring (C2); lifecycle gates (C3); coalesce-armed per-packet MDI (substrate-coalesce 2026-05-11) | Per-frame substrate record emission (`emitGpuCullRecord` in `code/objmgr.cpp`, 6 iteration sites). Cull/visibility/draw are all GPU. **Phase C v1 leaves this alone** — the residual is a Track-D / object-offload scope question, not a thin-record question. | Track A/B/C + substrate-coalesce |
| **Mech/GV (Track D)** | GPU mech batcher (merged 2026-05-10, commit `0d5ce93`); per-mech transforms cached for submitActor pipeline | Per-frame data path still settling post-merge (msl.cpp CacheGpuLightData conflict resolved at merge time but soak not complete) | gpu-mech-batcher (Slices A–D, all 10 killswitches default-on) |

### The two specific GPU→CPU→GPU bounces Phase C eliminates

1. **Phase 1 lighting → CPU mirror → thin-record bounce.** Phase 1 publishes lightRGB/fogRGB to a GPU SSBO; the CPU readback ring copies those bytes back into `ScreenVertex::lightRGB`/`fogRGB`; the per-bucket pack loops read the CPU mirror to populate per-corner fields in the thin record SSBO; the VS reads the thin record back on GPU. After Phase C: the per-bucket compute shader reads Phase 1's SSBO directly. The CPU readback ring stays alive (cull readback still uses it for visibility-lagged decisions), but lighting bytes never round-trip through CPU.
2. **CPU pre-cull (`projectZ` per quad) → thin-record flags → VS gl_Position emit.** The per-tri pzValid bits the CPU pack loop today emits into thin-record `flags` are recomputable on GPU from the same terrainMVP. The per-bucket compute shader does this directly (per `water_ssbo_pattern.md` rule that CPU pre-cull was load-bearing because `worldToClip` produces finite values for behind-camera vertices — the compute shader uses the SAME formula so the gate stays load-bearing; it just runs on GPU now).

These two bounces ARE the "heavy code portions still on CPU." The per-bucket thin-record pack loop is the structural form of those bounces; eliminating the pack loop is the surface signal that the bounces are gone.

---

## Framing correction: the indirect-cmd struct is not the cost

### What the prompt names

> "Replace per-frame CPU construction of indirect commands with GPU compute shader generation. The compute shader reads visibility + per-element state... and writes indirect-command SSBO that `glMultiDrawArraysIndirect` consumes."

### What current shipped code actually shows

The DAIC build for the SOLID indirect-terrain bucket is:

```cpp
// GameOS/gameos/gos_terrain_indirect.cpp:1566-1580 (grep-verified at write-time)
static int BuildIndirectCommands(int thinCount) {
    if (thinCount <= 0) return 0;

    DrawArraysIndirectCommand cmd{};
    cmd.count         = static_cast<GLuint>(thinCount * 6);
    cmd.instanceCount = 1u;
    cmd.first         = 0u;
    cmd.baseInstance  = 0u;

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    (GLsizeiptr)sizeof(cmd), &cmd);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    return 1;
}
```

One DAIC write per frame (16 bytes via `glBufferSubData`). Cost: microseconds. Not the thing to GPU-ify.

The real CPU per-frame work is the thin-record build above it:

```cpp
// GameOS/gameos/gos_terrain_indirect.cpp:1377-1403 (grep-verified)
static int PackThinRecordsForFrame() {
    ZoneScopedN("Terrain::ThinRecordPack");
    ...
    static TerrainQuadThinRecord s_shadow[kMaxThinRecords];   // kMaxThinRecords = 65536
    ...
    for (long qi = 0; qi < total && (size_t)packed < kMaxThinRecords; ++qi) {
        ...
    }
}
```

Walks live `quadList` (camera-windowed, rebuilt by `makeLists` every frame per `quadlist_is_camera_windowed.md`), does per-vertex `pz` checks, recipe lookups, per-corner fog/light copies. This is the per-quad CPU loop the prompt's "1.5–3 ms cut" budget targets.

### Design implication

Phase C's compute shader's **primary output** is the thin-record SSBO (`TerrainQuadThinRecord[]`, `WaterThinRecord[]`). The DAIC/DEIC struct is a **byproduct** written by a single thread (`gl_GlobalInvocationID == 0`) that snapshots the atomic counter into `cmd.count = atomicVisibleCount * 6`. The MDI consumer is unchanged from PR1.

This reframing is load-bearing for every downstream decision: bucket inclusion (which buckets currently have a per-frame thin-record build), schema (the thin-record struct layout is the lockstep target, not the cmd struct), sync (compute→MDI barrier still uses `GL_COMMAND_BARRIER_BIT`, but the cross-dispatch dataflow is dominated by thin-record bytes, not 16-byte cmd structs).

---

## Bucket inventory (recon)

This is the "what to finish" table, paired with the half-ported inventory above. For each bucket: the per-frame CPU residual, the draw consumer (already in place), and the Phase C completion verdict.

| # | Bucket | CPU per-frame site | Current MDI consumer | Shipped state | Phase C decision |
|---|--------|--------------------|----------------------|---------------|------------------|
| 1 | **Terrain SOLID (PR1)** | `gos_terrain_indirect.cpp:1377 PackThinRecordsForFrame()` + `:1566 BuildIndirectCommands()` | `gameos_graphics.cpp:2468 glMultiDrawArraysIndirect` | default-on (`MC2_TERRAIN_INDIRECT=1`, commit `e22fa3a`) | **Stage 2** (largest single CPU saver) |
| 2 | **Terrain MINE (PR2c)** | n/a — no per-frame thin-record pack. `RebuildMineStaticVBOIfDirty` at `gos_terrain_indirect.cpp:1923` is dirty-flag gated and cold-path | `glDrawArrays` at `gameos_graphics.cpp:2609` (single bucket, not indirect) — in its own Tracy zone `Render.TerrainMines` at `mclib/txmmgr.cpp:1812` | default-on (commit `6d4a6f7`) | **OUT** — no per-frame CPU residual to GPU-ify. v2 removes the prior "Stage 3 Mine" entry; the v1 design assumed MINE shared SOLID's machinery, which is wrong. |
| 3 | **Terrain DETAIL (PR2a)** | dead since `521d83a` per `pr2_detail_overlay_mine_stage0_recon.md` | n/a | dead | **OUT** (delete slice, not Phase C) |
| 4 | **Terrain OVERLAY (PR2b)** | not yet built (CPU path stays legacy) | `IsFrameOverlayArmed()` returns `false` unconditionally per `gos_terrain_indirect.cpp:169` | scaffold-only | **Stage 3 — CONDITIONAL.** Phase C does NOT construct OVERLAY's recipe (that would be a static-bake = Phase B scope). Phase C Stage 3 ships only if/when Phase B publishes an overlay recipe SSBO; the compute shader then consumes it. If Phase B doesn't ship an overlay recipe in this window, Stage 3 falls out of v1 and OVERLAY remains scaffold-only. |
| 5 | **Water** | `gos_terrain_water_stream.cpp:345 UploadAndBindThinRecords()` + `:453 WaterThinRecord` pack | non-indirect `glDrawArrays(thinCount*6)` — see Stage 1 conversion note | default-on (water fast path shipped 2026-04-30) | **Stage 1** (simplest, smallest, has full parity infrastructure already) |
| 6 | **Static-prop substrate** | `gos_static_prop_batcher.cpp` builds DEIC array at **mission load** (not per-frame); `gpu_cull_compute.cpp` compute shader patches `instanceCount` per-frame | `gos_static_prop_batcher.cpp:3228/3250/3441` `glMultiDrawElementsIndirect` / `glDrawElementsIndirect` | default-on (`MC2_GPU_CULL_SUBSTRATE=1`, commit `7b9ad5f`) | **OUT** — already GPU-authoritative for the per-frame field (`instanceCount`). Per-packet layout rebuild is mission-load-amortized, not per-frame. Phase C would have no win here. |
| 7 | **Mech/GV (Track D gpu-mech-batcher)** | merged 2026-05-10 (`0d5ce93`) | various per `mech_vehicle_gpu_pull_in.md` | merged, in soak | **DEFER to Phase C v2** — needs its own soak settle before a port slice. Track D's CacheGpuLightData guard conflict (msl.cpp) is one canary that the substrate is still settling. |
| 8 | **Substrate-coalesce per-packet cmds** | per-packet DEIC cmds rebuilt when packet layout changes (mission load + bucket-touch events) | shared with row 6 | armed default-on 2026-05-11 (commit `7b9ad5f`) | **OUT** — same reasoning as row 6: not per-frame work. |

### Per-stage Phase C scope (v1 of slice, v2 design)

- **Stage 1 — Water** (precedent proof; Z2 anchor).
- **Stage 2 — Terrain SOLID** (largest CPU saver; primary contributor to Z1 reduction).
- **Stage 3 — Terrain OVERLAY** (CONDITIONAL — consumes Phase B's overlay recipe if/when the sibling slice ships one; otherwise OVERLAY stays scaffold-only and Stage 3 falls out of v1).
- **Stages 4/5/6** per the prompt: soak window, default-on flip per-bucket, demote legacy CPU paths.

**MINE has been removed** from the Phase C v1 scope per the inventory above. The v1 design treated MINE as a mechanical extension of SOLID; that was wrong — MINE has no per-frame thin-record pack today and lives in a different Tracy zone (`Render.TerrainMines`, not Z1's `textureManagerRenderLists`). No CPU residual to attack.

Phase C v1 (this slice) intentionally does NOT include Track D mechs/vehicles. The prompt's "Stage 4: extend to static-prop bucket" is reinterpreted in light of the inventory above: static-prop's per-frame indirect-cmd work is already on the GPU (Track C C1b). The remaining "static-prop work" is one of two things, both deferred:
1. Move the per-frame substrate record emission (`emitGpuCullRecord` in `code/objmgr.cpp`, 6 iteration sites) onto compute — this is a Track-D / cull-arc question, not a thin-record question.
2. Move the per-packet layout rebuild — mission-load-amortized, not on hot path.

If a stakeholder reads this and disagrees, the disagreement should be resolved at adversarial review time, not silently in code.

---

## Architecture

### One compute shader per bucket

Each Phase C bucket adds **one new compute shader** (`shaders/gpu_driven_<bucket>.comp`) plus **one host-side wiring change** (extends the bucket's existing `<bucket>_ComputePreflight()` to dispatch the compute pass instead of running the CPU thin-record build).

Per-bucket compute shader contract:

```
INPUT  (SSBOs, all read-only from compute's perspective):
  binding R0: <bucket>RecipeSSBO       — mission-static per-element state
                                         (terrain SOLID: PR1's `g_recipeSSBO`;
                                          water: `g_recipeBuffer` at
                                            `gos_terrain_water_stream.cpp:45`;
                                          OVERLAY: Phase B's overlay recipe SSBO
                                            when shipped)
  binding R1: gpu_terrain_lighting     — Phase 1's per-vertex lightRGB/fogRGB output SSBO
                                         (compute can read DIRECTLY; no CPU bounce)
  binding R2: <bucket>QuadListSSBO     — per-frame quadList index window
                                         (CPU still owns; thin substitute for camera-windowed
                                          quadList until Phase B static + delta lands)
  UBO     U0: TerrainMVP / camera      — already exists per-frame
  UBO     U1: bucketParams             — per-frame uniforms (alpha, animation phase, etc.)

OUTPUT (SSBOs, written by compute, ALIASED for both compute + draw):
  binding W0: <bucket>ThinRecordSSBO   — written by compute; consumed by VS via gl_VertexID
                                         (layout identical to the CPU-written struct today)
  binding W1: <bucket>IndirectCmdSSBO  — written by compute by invocation 0
                                         (also bound as GL_DRAW_INDIRECT_BUFFER for MDI)
  binding W2: <bucket>VisibleCount     — atomic counter (4-byte SSBO header slot,
                                         atomicAdd from each visible-passing invocation)

INVOCATION SHAPE:
  layout(local_size_x = 64) in;
  dispatched as glDispatchCompute( (N_elements + 63) / 64, 1, 1 )
  one invocation per recipe element (per-quad for terrain, per-quad for water)

ALGORITHM (per invocation):
  uint elementId = gl_GlobalInvocationID.x;
  if (elementId >= u_elementCount) return;

  Recipe r = recipeSsbo[elementId];

  // Visibility (replaces CPU projectZ pre-cull):
  vec4 clipCorners[4] = transform recipe corners by terrainMVP
  bool visible = perCornerProjectZGate(clipCorners) && bucketAdmissionGate(r);
  if (!visible) return;

  // Per-frame mutable state (Phase 1's GPU-resident lighting):
  uvec4 lightRGB = lightingSsbo[r.vnTopLeft.. r.vnBotRight];
  uvec4 fogRGB   = lightingSsbo[..].fogRGB;

  // Atomic slot allocation:
  uint outSlot = atomicAdd(visibleCount, 1u);
  if (outSlot >= u_maxThinRecords) return;   // overflow guard, no-op on overflow

  // Pack thin record (byte-identical to legacy CPU pack):
  thinRecordSsbo[outSlot] = ThinRecord{
    recipeIdx  = elementId,
    flags      = packPzValidBits(clipCorners),
    perCorner  = packCornerState(r, lightRGB, fogRGB, ...)
  };

  // Indirect cmd write — single invocation (the last one to atomicAdd):
  if (atomicAdd in this group reaches u_elementCount, OR via final-cmd-write barrier)
    indirectCmdSsbo[0] = DrawArraysIndirectCommand{
      count         = visibleCount * 6,
      instanceCount = 1,
      first         = 0,
      baseInstance  = 0
    };
```

The "final invocation writes the cmd" pattern requires a barrier between the cull/pack pass and the cmd-write pass. Two designs are viable, both proven in shipped code:

#### Design Alpha — single compute, in-shader barrier

```glsl
groupMemoryBarrier(); barrier();
if (gl_LocalInvocationID.x == 0 && gl_WorkGroupID.x == lastGroup)
    indirectCmdSsbo[0].count = atomicVisibleCount * 6;
```

But cross-workgroup `lastGroup` synchronization is not safe in GL compute (no global barrier). Reject.

#### Design Beta — two-dispatch pattern (PRECEDENT: `gpu_cull_compute.cpp` cull→patch)

```cpp
// Dispatch 1: cull + pack + atomicAdd into visibleCount
glDispatchCompute( (N+63)/64, 1, 1 );
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

// Dispatch 2: 1 invocation that reads atomic and writes cmd struct
glDispatchCompute( 1, 1, 1 );           // tiny "patch" shader
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

// MDI consumes
glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, 1, 0);
```

**Decision: Beta.** Matches the proven Track C C1b pattern at `gpu_cull_compute.cpp:938-1033`. The "patch shader" is trivial (one invocation, one buffer write). The barrier sequence is identical to the one in production today, including the `GL_COMMAND_BARRIER_BIT` ordering that's already validated against AMD RX 7900 XTX drivers.

### Sync pattern (load-bearing)

Per `substrate_coalesce_sync_point_lesson.md`: **no `glGetBufferSubData` on hot path against any buffer written by GPU this frame**. The sync stall pattern that cost mc2_10 6 ms/frame (62→128 fps) MUST be avoided. Phase C compute shaders write all their outputs to SSBOs that are consumed only by GL pipeline (compute→barrier→MDI). CPU side reads zero bytes back from these buffers in steady state.

Visible-count for diagnostics (e.g., `[GPU_DRIVEN v1] event=summary visible=N` every 600 frames) uses the **3-slot fenced readback ring** pattern from `gpu_cull_readback.cpp:440-455`. Lagged by N frames. NOT on hot path. NOT a draw dependency.

The barrier sequence per dispatch is:

```cpp
glDispatchCompute(cmd_gen_groups, 1, 1);
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);     // cull/pack→patch
glDispatchCompute(1, 1, 1);                          // patch invocation 1 writes cmd
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);    // patch→MDI
// ... bind bucket texture (one glBindTexture per bucket, per Phase A deferral) ...
glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, /*drawcount=*/1, /*stride=*/0);
```

`GL_COMMAND_BARRIER_BIT` is the load-bearing flag. Per `gpu_cull_compute.cpp:1030-1033` and the Q12 contract: the patch→MDI barrier MUST include it, or MDI reads stale data on AMD drivers.

### Per-frame CPU work after Phase C ships (per bucket)

```
1. Reset per-bucket counter SSBO (glClearNamedBufferSubData, 4 bytes)        ~5 µs
2. Bind compute SSBOs (4 glBindBufferBase calls)                              ~5 µs
3. glDispatchCompute(cull/pack)                                               ~5 µs
4. glMemoryBarrier(SHADER_STORAGE)                                            ~1 µs
5. glDispatchCompute(patch, 1 group)                                          ~5 µs
6. glMemoryBarrier(SHADER_STORAGE | COMMAND)                                  ~1 µs
7. Save/restore + bind bucket texture (Phase A deferred; one glBindTexture)   ~5 µs
8. glMultiDrawArraysIndirect                                                  ~5 µs
                                                                              ──────
                                                                              ~30 µs
```

Replaces a 1–3 ms CPU thin-record build. Win: 1–3 ms minus 30 µs per shipped bucket.

### Stage 1 water draw shape (resolves substance C-2)

Current water fast path issues **two** sequential `glDrawArrays` calls over the same vertex range with different uniforms + texture:

- Base layer at `gameos_graphics.cpp:2215`: `isWater=1, detailMode=0, uvScale=oneOverTF`, `baseTex`.
- Detail/spray layer at `:2242`: `isWater=2, detailMode=1, uvScale=oneOverWaterTF`, `detailTex`.

A single `glMultiDrawArraysIndirect` with `drawcount=1` covers only the base layer. v2 resolves this with **2-cmd MDI + per-cmd uniforms via SSBO indexed by `gl_DrawID`**.

The `WaterPerCmdSSBO` (slot 5 in the water compute program above) holds two elements:

```cpp
struct WaterPerCmd {
    uint32_t textureSlot;   // 0 = baseTex's slot, 1 = detailTex's slot (per mc2_texture_handle_is_live.md)
    uint32_t isWater;       // 1 or 2 (matches existing CPU uniform)
    uint32_t detailMode;    // 0 or 1
    float    uvScale;       // oneOverTF or oneOverWaterTF
    vec2     uvOffset;      // cloudOffset (base) or sprayOffset (detail)
    uint32_t _pad0, _pad1;  // std430 16-byte alignment
};
static_assert(sizeof(WaterPerCmd) == 32);
```

VS + FS read `WaterPerCmd[gl_DrawID]` from slot 5 via a `flat varying uint cmdId = gl_DrawID;` pattern, identical to the existing pattern that PR2 uses for `runTextureIdx` (per `cement-multi-sampler-plan-v2.md`).

The CPU MDI bridge:

```cpp
// One bind per draw call's primary texture (per Phase A deferral rule):
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, gos_GetGLTextureId(perCmd[0].textureSlot resolved via list));
// (above pattern still requires per-cmd glBindTexture today; Phase A bindless
// would collapse to a single bindless-handle-array read by gl_DrawID.)
// For 2-cmd water this is 2 binds, no extra cost vs today.

glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, /*drawcount=*/2, /*stride=*/0);
```

Tradeoff: the FS branch on `isWater`/`detailMode` becomes a flat-uniform-lookup instead of a per-draw uniform read. Equivalent GPU cost; the parity check enforces visual identity. The 2 separate texture binds preserved is the only "ugly" residual — Phase A bindless would collapse it.

Alternative considered: merge base + detail into a single shader pass (single draw, branchless). Rejected because (a) parity-risky, (b) requires re-deriving the existing `gos_water_fast.frag` for the merged path, and (c) the 2-cmd approach has zero new shader logic — same FS, just consumes per-cmd state from `gl_DrawID`-indexed SSBO instead of uniforms.

### Stage 2 SOLID arming flow under GPU-driven (resolves substance C-3)

Current arming chain in `gos_terrain_indirect.cpp:1605-1637`:

1. `PackThinRecordsForFrame()` returns CPU-computed `thinCount`.
2. If `thinCount == 0`, set `s_frameSolidArmed = false`, return `false` (preflight_skip).
3. Otherwise `BuildIndirectCommands(thinCount)` returns `cmdCount`.
4. Store both counts in `s_frameSolidPackedThinCount` / `s_frameSolidCmdCount`.
5. `DrawIndirect()` passes `s_frameSolidCmdCount` to `gos_terrain_bridge_drawIndirect`.

Under GPU-driven SOLID, neither count is known on CPU at preflight time. Per `substrate_coalesce_sync_point_lesson.md`, we MUST NOT `glGetBufferSubData` to read GPU-computed `visibleCount` back on the hot path.

**Decision:** always-arm-when-mission-running + tolerate count=0 MDI.

```cpp
bool ComputePreflight() {  // v2 GPU-driven SOLID path
    s_frameSolidArmed = false;
    if (s_processArmingDisabled) return false;
    if (!IsEnabled())            return false;
    if (!IsDenseRecipeReady())   return false;
    if (!ResourcesReady())       return false;
    if (InMissionTransition())   return false;

    FlushDirtyRecipeSlotsToGPU();          // unchanged
    UploadQuadListWindowSSBO();            // new — per-frame index list of in-window quads

    s_frameSolidArmed   = true;
    s_frameSolidCmdCount = 1;              // always 1 for SOLID (single bucket)
    return true;
}
```

`DrawIndirect()` always invokes the compute dispatch + barrier + MDI sequence when `s_frameSolidArmed`. If the compute shader determines zero quads pass cull this frame, the indirect cmd it writes has `count=0` and the MDI executes as a no-op (well-defined per GL spec: "If count is zero, no triangles are drawn").

**Mech-bay / menu protection:** `s_processArmingDisabled` and `IsDenseRecipeReady()` already gate mech-bay/menu frames per `gos_terrain_indirect.cpp:1590-1614` — both still apply. The removed `thinCount == 0` early-return was a redundant performance optimization (skip the bind + dispatch when nothing would draw); the cost of issuing a count=0 MDI is microseconds (one bind + one dispatch invocation that no-ops on cull-zero); the mech-bay/menu protection comes from the recipe-not-ready / process-disabled gates, not from `thinCount`.

The CPU loses visibility into per-frame quad counts. For debug diagnostics (`MC2_GPU_DRIVEN_TRACE=1`), the visible-count is fence-ring-lagged via the existing `gpu_cull_readback.cpp` 3-slot pattern (read N-2 frames late). Not on hot path.

### Phase 1 lighting same-frame barrier ordering (resolves substance M-5)

Phase 1's existing per-frame sequence (per `gos_terrain_lighting.cpp:613-633`):

1. `glDispatchCompute(numGroups, 1, 1)` — writes `s_computeOutputSsbo`.
2. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` at `:621`.
3. `glCopyBufferSubData(s_computeOutputSsbo → s_stagingRing[currentSlot])` at `:628`.
4. `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)` at `:633`.

Phase C compute reads `s_computeOutputSsbo` directly. The barrier at step 2 already makes the buffer visible to subsequent compute reads. Phase C dispatch must occur AFTER step 2 (no additional barrier needed before Phase C dispatch — the post-Phase-1 barrier covers it) and BEFORE Phase 1's NEXT frame's dispatch (trivially true because Phase C runs in the same frame as the Phase 1 dispatch that produced the data).

The per-frame ordering under v3 (call-sites grep-verified):

```
Phase 1 dispatch (mission.cpp:527 — Tracy zone Mission.TextureManager
                   wraps mcTextureManager->update() which invokes Phase 1
                   under the hood)
  ↓ glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)        ← Phase 1 emits this at gos_terrain_lighting.cpp:621
  ↓ glCopyBufferSubData (stays — Track C cull readback uses the staging ring)
  ↓ glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT)  ← Phase 1 emits this at :633

... CPU work, other dispatches ...

Phase C SOLID compute dispatch — placed inside ComputePreflight() at
                                  gos_terrain_indirect.cpp:1605, invoked from
                                  terrain.cpp:1792 inside Tracy zone
                                  Terrain::geometry quadSetupTextures
  reads slot 1 = s_computeOutputSsbo (via gos_terrain_lighting::GetOutputSsbo())
  writes slot 3 = SolidThinRecordSSBO, slot 4 = SolidIndirectCmdSSBO, slot 5 = SolidBucketHeader
  ↓ glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)        ← Phase C emits between cull/pack and patch
Phase C patch dispatch (1 invocation, same site — still inside ComputePreflight)
  reads slot 5 (BucketHeader)
  writes slot 4 (IndirectCmdSSBO[0].count)
  ↓ glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)

... CPU work between Terrain::geometry quadSetupTextures zone and
    Render.TerrainSolid zone — no GL state changes that invalidate
    the dispatched compute output ...

glMultiDrawArraysIndirect (Stage 2 SOLID) — DrawIndirect() at
                                            gos_terrain_indirect.cpp:1639,
                                            invoked from txmmgr.cpp:1664
                                            inside Tracy zone Render.TerrainSolid
                                            (zone defined at txmmgr.cpp:1626)
```

No new barrier is required between Phase 1's dispatch and Phase C's dispatch — Phase 1's existing post-dispatch barrier already publishes the SSBO.

**Why the compute dispatch placement is inside `ComputePreflight()` (the existing arming hook), NOT inside `Render.TerrainSolid`:** the dispatch must complete before MDI; the patch dispatch + final `GL_COMMAND_BARRIER_BIT` make this so regardless of which Tracy zone the dispatch is wrapped in. Placing the dispatch inside `ComputePreflight()` reuses the existing arming-gate site at `:1605-1637` (which already gates on `IsDenseRecipeReady`, `ResourcesReady`, `InMissionTransition`, and the killswitch latch) and adds zero new call-site plumbing. The dispatched work executes asynchronously on the GPU between the two Tracy zones; the `GL_COMMAND_BARRIER_BIT` at the end of the dispatch sequence is the load-bearing synchronization, not the Tracy zone boundary.

This corrects v2's prose, which conflated the file (it said `gamecam.cpp`; the SOLID indirect-draw path lives in `mclib/txmmgr.cpp` for the MDI side and `mclib/terrain.cpp` for the preflight side).

### Parity-gate reframe for Phase 1 frame-pipelined latency (resolves substance M-2)

Phase 1 uses a 3-slot staging-ring with non-blocking `tryConsume` (`gos_terrain_lighting.cpp:644-`). Lighting bytes are visible to CPU readback at frame N+1 (T1, normal case), N+2 (T2 fallback), or N+3 (T3 stale-frame fallback).

Under legacy CPU pack:
- `vertices[i]->lightRGB` is the CPU mirror, populated from Phase 1's frame-N output during the readback pass, consumed by `PackThinRecordsForFrame` at frame N+1.

Under Phase C compute:
- Phase C compute reads `s_computeOutputSsbo` directly — frame N's output is consumed at frame N (same frame!).

**Byte-equal parity is therefore impossible** during any frame where lighting state mutates (sun rotation, dynamic lights, mech engine glow ticks), because the two paths read lighting from different frame indices.

**Decision:** parity mode forces Phase 1 into **non-pipelined synchronous mode** for the parity window. Concretely: the smoke-runner sets BOTH `MC2_GPU_DRIVEN_PARITY=1` AND `MC2_TERRAIN_LIGHTING_PARITY=1` as a **documentation contract** — Phase C does NOT toggle Phase 1's env at runtime (which it cannot anyway; see "Why option (a)" below).

Why option (a) — smoke-runner sets both env vars externally:

- Phase 1's `IsParityCheckEnabled()` at `gos_terrain_lighting.cpp:85-87` caches the env value in `static const bool s_parity = (getenv("MC2_TERRAIN_LIGHTING_PARITY") != nullptr);` on first call. The cache is **process-lifetime**; runtime mutation is not possible after the first call. Phase C cannot toggle Phase 1's parity mode from preflight time.
- The other alternatives are: (b) Phase 1 publishes a `SetParityMode(bool)` accessor — but this requires invalidating the static cache and is a larger Phase-1-API extension than the one Phase C v1 already needs (the `GetOutputSsbo()` getter); (c) Phase C `_putenv` before Phase 1's first `IsParityCheckEnabled()` call — fragile, depends on init-order, and is a hack.
- Option (a) is also how every existing tier1 instrumentation env var works per CLAUDE.md "Tier-1 Instrumentation Env Vars": smoke-runner sets, code reads at startup.

**Contract documented in CLAUDE.md / smoke-runner:** `MC2_GPU_DRIVEN_PARITY=1` requires `MC2_TERRAIN_LIGHTING_PARITY=1` to also be set at process startup. The smoke-runner's parity-test invocation sets both; ad-hoc developer runs of `MC2_GPU_DRIVEN_PARITY=1` without `MC2_TERRAIN_LIGHTING_PARITY=1` will produce false-positive parity mismatches (lighting frame-skew) and should be diagnosed by checking the `[INSTR v1] enabled:` banner at startup.

In Phase 1's parity mode (`MC2_TERRAIN_LIGHTING_PARITY=1`, set at process startup):
- Phase 1 dispatches and waits (sync stall accepted ONLY in parity windows, not in soak / steady-state).
- CPU readback ring is consumed at frame N, NOT frame N+1.
- `vertices[i]->lightRGB` and `s_computeOutputSsbo` reflect the SAME frame N values.
- Phase C compute reads `s_computeOutputSsbo` at frame N; legacy CPU pack reads `vertices[i]->lightRGB` at frame N (now same-frame, not lagged).
- Byte-equality is achievable.

Steady-state (no parity envs set) keeps Phase 1's frame-pipelined behavior. Phase C compute is then 1 frame "ahead" of the legacy CPU pack would be — i.e., it sees fresher lighting. That's a visible improvement, not a regression.

**Documentation contract:** the parity-gate is REQUIRED to pass under `MC2_GPU_DRIVEN_PARITY=1` BEFORE Stage 2's default-on flip. It is NOT required to pass in steady-state (without parity env set), because steady-state is not byte-equal by design — it's `frame N` lighting in Phase C vs `frame N-1` lighting in the would-be legacy CPU pack, and the byte difference is a visible improvement.

### Ring-slot persistence under GPU-driven SOLID (resolves substance M-3)

PR1's existing thin-record SSBO is multi-slot ring-buffered via `glBindBufferRange` per-frame offset arithmetic (host-side; the compute shader doesn't see the ring). Under GPU-driven SOLID, the compute shader must write into the same per-frame ring slot the MDI consumer subsequently reads from.

**Decision:** keep the ring. The compute shader's slot 3 (SolidThinRecordSSBO) is bound via `glBindBufferRange` at the per-frame ring-slot offset BEFORE the compute dispatch. The MDI consumer's existing slot-2 bind at `gameos_graphics.cpp:2458-2464` (per the C-1-flagged binding) is unchanged — same offset arithmetic, same buffer.

```cpp
const uint32_t slot = gos_terrain_indirect_getRingSlot();   // existing
const GLintptr offset = slot * kMaxRecs * kRecordSz;
const GLsizeiptr size  = kMaxRecs * kRecordSz;

// Compute dispatch bind:
glBindBufferRange(GL_SHADER_STORAGE_BUFFER, /*compute slot 3*/ 3,
                  g_thinRecordSSBO, offset, size);
glDispatchCompute(...);

// MDI consumer bind (existing path, unchanged):
glBindBufferRange(GL_SHADER_STORAGE_BUFFER, /*VS slot 2*/ 2,
                  g_thinRecordSSBO, offset, size);
glMultiDrawArraysIndirect(...);
```

No collapse to single-slot. Rationale: the ring's per-frame isolation protects against (a) recipe dirty-flush interactions at `gos_terrain_indirect.cpp:1617` that flush mid-frame, (b) future async-compute experiments that might overlap frame N+1's compute with frame N's MDI. Keeping the existing ring topology means Phase C doesn't perturb any current invariant.

### Phase B recipe-layout-frozen contract (resolves substance M-7 Q4)

The Phase C compute shaders read recipe SSBOs by struct member. Field reordering, type changes, or size changes in any Phase B recipe SSBO are SHADER-source-breaking changes for Phase C, not host-side binding swaps.

**Explicit contract** (added to "What Phase B should NOT do that would surprise Phase C" in the boundary section above):

- Phase B MUST NOT change field layout, order, type, or size in any recipe SSBO that Phase C v1 consumes (`g_recipeSSBO` for SOLID; `g_recipeBuffer` for water; the future overlay recipe SSBO if Stage 3 ships) without a lockstep Phase C compute-shader commit AND a content-diverse tier1 smoke (per `cpp_glsl_ubo_struct_lockstep.md`, mc2_24's mc2_17/mc2_24 distinct-light-setup canary catches mid-array drift).
- Phase B MAY add new recipe SSBOs (fresh symbol names, fresh bindings, no Phase C consumer yet). Phase C will adopt them in a follow-up slice.
- Phase B MAY change the SOURCE that populates a recipe (the build function) without Phase C lockstep — Phase C reads the buffer, not the build site.

This is more restrictive than the v1 "compatible by construction" framing. v1 was wrong: there is no construction-level invariant; both sides could index by `vertexNum` and still have incompatible byte layouts.

### Texture binding (Phase A deferred — see prompt)

Per `mc2_texture_handle_is_live.md` and the prompt's "Phase A is DEFERRED" section:

- Compute shader writes `uint32 textureSlot` into the thin-record per quad (NOT bindless handles).
- CPU MDI bridge resolves the bucket's primary texture slot once before each MDI, via `gos_GetGLTextureId(listOfTextures[slot].gosTextureHandle)` + `glBindTexture`.
- Within a bucket, the bucket atlas (or single texture) is bound once before its MDI. Per-quad `textureSlot` is consumed by the FS via `flat varying uint runTextureIdx` (existing pattern for SOLID PR2).

When Phase A bindless ships (separate slice, no dependency from Phase C), the texture-slot field in thin-record evolves to a `uvec2` bindless handle and the `glBindTexture` is removed. The schema bump is one struct field; the rest of Phase C is unchanged.

---

## SSBO schemas

### Lockstep contract (per `cpp_glsl_ubo_struct_lockstep.md`)

Every C++ struct that mirrors a GLSL SSBO/UBO declaration MUST byte-match. Same commit, both sides. Detection: tier1 5/5 with content diversity (mc2_24 desert biome with many distinct light setups is the canary that exposed the prior bug).

### Indirect-cmd struct (UNCHANGED from PR1)

```cpp
// GameOS/gameos/gos_terrain_indirect.cpp:1262-1269 — exact reuse, no schema change
struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};
static_assert(sizeof(DrawArraysIndirectCommand) == 16, "GL spec");
```

GLSL mirror (extracted into a new `shaders/include/indirect_cmd.hglsl` for shader-side reuse across buckets):

```glsl
struct DrawArraysIndirectCommand {
    uint count;
    uint instanceCount;
    uint first;
    uint baseInstance;
};
layout(std430, binding = <W1>) coherent buffer IndirectCmd {
    DrawArraysIndirectCommand cmds[];
};
```

### Thin-record structs (UNCHANGED layouts; field-population moves to GPU)

Per-bucket layouts as they exist today:

- `TerrainQuadThinRecord` — `GameOS/gameos/gos_terrain_indirect.cpp` (see Verification Appendix for exact line)
- `WaterThinRecord` — `gos_terrain_water_stream.cpp:453` declaration site

Phase C does NOT change either struct in this slice. The compute shader writes the same byte layout the CPU pack loops write today. **Parity check trivially synthesizes the expected bytes by re-running the legacy CPU pack against the same recipe input** — same approach as the renderWater parity infrastructure described in `water_ssbo_pattern.md`.

When Phase B ships and the input source changes from "CPU per-frame quadList window" to "static recipe + GPU delta," the thin-record struct STILL doesn't change — only the compute shader's input bindings do.

### Per-bucket visible-count SSBO (NEW, tiny)

```cpp
// One 16-byte block per bucket: [visibleCount, _pad0, _pad1, _pad2].
// _pad0..2 reserved for future per-bucket telemetry (cull-rejected count, overflow flag).
struct GpuDrivenBucketHeader {
    uint32_t visibleCount;     // atomic
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(GpuDrivenBucketHeader) == 16);
```

std430 4-byte alignment is sufficient; `atomicAdd` on `visibleCount` is the only access pattern.

---

## Per-bucket binding-point allocation

### Per-program namespace clarification (load-bearing)

GL binding points are PER-PROGRAM-NAMESPACE — `layout(std430, binding = 2)` in Phase 1's `gos_terrain_lighting.comp` and `layout(std430, binding = 2)` in `shaders/gos_terrain_thin.vert` do NOT collide at runtime because they are different programs with independent binding tables. The reviewer-flagged "binding=2 collision" (substance C-1) is real only **within a single new Phase C compute program** where Phase 1's lighting output and the bucket's thin-record SSBO need distinct slots at the same time.

This v2 table therefore allocates explicit numbers per Phase C compute program. Each program independently picks any 0–N for its slots; consumers (the bridge MDI loop) re-bind as needed.

### Phase C compute program binding table (concrete numbers)

The shipped binding pressure across DIFFERENT programs is concentrated at slots 0–16 (Track C / Phase 1 / static-prop / patch shaders all use slots in this range). Phase C compute programs use 0–7 internally and remap on the host side.

**`shaders/gpu_driven_water.comp` (Stage 1):**

| Slot | Direction | SSBO | GL handle source |
|------|-----------|------|------------------|
| 0    | R  | WaterRecipeSSBO       | `g_recipeBuffer` (`gos_terrain_water_stream.cpp:45`) |
| 1    | R  | PhaseOneLightingSSBO  | `s_computeOutputSsbo` (`gos_terrain_lighting.cpp:572`, Phase 1's `TL_OUTPUT_BINDING`) |
| 2    | R  | WaterQuadListWindow   | new per-frame SSBO (CPU-uploaded list of recipe indices in this frame's window) |
| 3    | W  | WaterThinRecordSSBO   | the existing water thin SSBO at `gos_terrain_water_stream.cpp` (currently CPU-written, now compute-written; same buffer) |
| 4    | W  | WaterIndirectCmdSSBO  | new — aliased as both `GL_SHADER_STORAGE_BUFFER` (compute writes) and `GL_DRAW_INDIRECT_BUFFER` (MDI reads) |
| 5    | W  | WaterPerCmdSSBO       | new — per-cmd uniforms indexed by `gl_DrawID`, see "Stage 1 water draw shape" below |
| 6    | RW | WaterBucketHeader     | new — `GpuDrivenBucketHeader{visibleCount, …}`, `atomicAdd` counter slot |
| 7    |    | (reserved)            | |

UBO 0 = TerrainMVP / camera (existing). UBO 1 = per-frame bucketParams (alpha-band uniforms etc.; new).

**`shaders/gpu_driven_terrain_solid.comp` (Stage 2):**

| Slot | Direction | SSBO | GL handle source |
|------|-----------|------|------------------|
| 0    | R  | SolidRecipeSSBO       | PR1's `g_recipeSSBO` (existing static recipe, mission-load built at `gos_terrain_indirect.cpp`) |
| 1    | R  | PhaseOneLightingSSBO  | same as water — Phase 1's `s_computeOutputSsbo` |
| 2    | R  | SolidQuadListWindow   | new per-frame SSBO (same shape as water's; potentially shared if OVERLAY ships and iterates same `quadList`) |
| 3    | W  | SolidThinRecordSSBO   | existing PR1 thin-record SSBO at `gos_terrain_indirect.cpp:1300` (currently CPU-written via `PackThinRecordsForFrame`, now compute-written; same buffer) |
| 4    | W  | SolidIndirectCmdSSBO  | existing PR1 `g_indirectCmdBuffer` at `gos_terrain_indirect.cpp:1272` — aliased as both bind targets |
| 5    | RW | SolidBucketHeader     | new — same shape as water's `GpuDrivenBucketHeader` |
| 6–7  |    | (reserved)            | |

**`shaders/gpu_driven_terrain_overlay.comp` (Stage 3, conditional):**

Defined if Stage 3 ships under outcome (1) of the OVERLAY contingency. Binding layout mirrors SOLID's with `OverlayRecipeSSBO` from Phase B and a new `OverlayIndirectCmdSSBO`.

**`shaders/gpu_driven_cmd_patch.comp` (shared across buckets):**

| Slot | Direction | SSBO |
|------|-----------|------|
| 0    | R  | `BucketHeader` (the bucket whose cmd is being patched) |
| 1    | W  | `IndirectCmdSSBO` (one cmd to patch, count = visibleCount × VERTS_PER_ELEMENT) |

Each invocation of the patch program is a 1-thread dispatch; the bucket's BucketHeader + IndirectCmdSSBO are bound at slots 0/1 before each `glDispatchCompute(1, 1, 1)`.

### Host-side binding for Phase C compute dispatch (water Stage 1 example)

```cpp
// Bind for water compute:
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeBuffer);                       // SLOT 0
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gos_terrain_lighting::GetOutputSsbo()); // SLOT 1 — see "Required Phase 1 API extension" below
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_waterQuadListWindowSSBO);            // SLOT 2
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g_waterThinRecordSSBO);                // SLOT 3
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, g_waterIndirectCmdBuffer);             // SLOT 4
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, g_waterPerCmdSSBO);                    // SLOT 5
glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_waterBucketHeaderSSBO);              // SLOT 6
glUseProgram(g_gpuDrivenWaterProgram);
glDispatchCompute(/*…*/);
// Slot rebinding for MDI is separate — the host-side bridge restores
// the prior MDI bindings (slot 2 = water thin SSBO at the consumer side,
// per existing water fast-path bind order at gameos_graphics.cpp:2197+).
```

The compute program's slot 1 = Phase 1's lighting SSBO is bound RANGE-BOUND READ-ONLY. The pre-existing Phase 1 binding at `TL_OUTPUT_BINDING=2` in Phase 1's own program is **not affected** — that's a different program's binding table.

### Required Phase 1 API extension (resolves substance D — accessor does not exist)

`s_computeOutputSsbo` is file-static in `gos_terrain_lighting.cpp` (declarations at `:137`, `:369`, bind site at `:572`). Phase 1's public API in `gos_terrain_lighting.h` has no SSBO accessor. Phase C v1 needs one.

**Contract:** Phase 1 ships a one-line getter as part of Phase C v1's landing commit (or as a small standalone commit Phase 1 publishes ahead of Phase C v1):

```cpp
// gos_terrain_lighting.h — added under existing namespace gos_terrain_lighting
namespace gos_terrain_lighting {
    // ...existing public API...

    // Returns the GL buffer name of the per-vertex lighting output SSBO
    // (lightRGB/fogRGB), as written by the per-frame compute dispatch.
    // Returns 0 if Phase 1 is disabled or not yet initialized. Phase C
    // compute shaders bind this at their input slot 1 to read lighting
    // bytes directly, eliminating the CPU-mirror bounce that the legacy
    // pack loops require.
    GLuint GetOutputSsbo();
}
```

Implementation is a one-liner: `return s_computeOutputSsbo;` (plus `0` guard if pre-init). This is the only Phase-1-source touch Phase C v1 requires.

**Naming convention:** namespaced `gos_terrain_lighting::GetOutputSsbo()` matches the existing public API surface in `gos_terrain_lighting.h:97-99` (parity API uses the same namespaced style). Free-function `gos_terrain_lighting_getOutputSsbo()` was the v2 placeholder and is corrected in v3.

---

## Stage plan (per the prompt's roadmap)

### Scope discipline — completion, not redesign

Three rules govern every stage below:

1. **No new SSBO struct layouts** except the tiny new `GpuDrivenBucketHeader` (4 GLuints). The thin-record layouts that exist today (`WaterThinRecord`, `TerrainQuadThinRecord`, and the PR2c MINE variant) are byte-stable across Phase C. The compute shader's pack body must produce byte-identical output to the legacy CPU pack body. This is enforced by the per-bucket parity check and is the load-bearing safety mechanism — if the compute shader's output ever diverges from CPU pack, the parity check fails before the bucket flips default-on.
2. **No new draw consumers** except the OVERLAY exception (Stage 3, conditional). Each bucket's MDI consumer is already in place (or, for water, becomes an MDI with one struct's worth of additional setup — see Stage 1 note). Phase C does not invent new render-state pipelines, new shaders for VS/FS, new sampler conventions, new depth-state. It just changes who writes the thin-record SSBO.
3. **No CPU pack loop is deleted in v1.** Each one is gated off via `MC2_GPU_DRIVEN_<BUCKET>=0` and left in the tree per CLAUDE.md's "demote-don't-delete" rule. A separate post-soak slice physically removes them after the per-bucket flips have stuck for the soak window.

### Stage 0 — Spec + design doc (THIS DOCUMENT)

Adversarial-review gate runs before Stage 1 ships.

### Stage 1 — Water (precedent proof, targets Z2)

**Why first:** smallest scope (single thin-record bucket, single texture slot, complete CPU parity infrastructure already exists per `water_ssbo_pattern.md`). Single point of failure if the compute pattern is wrong. Tracy anchor: `GameCamera::render waterFastPath` at `gamecam.cpp:255-256` (the live cost is in the fast-path zone, not the legacy `render water` zone at `:217` which early-returns when armed).

**Deliverables:**
- `shaders/gpu_driven_water.comp` (new) — cull/pack
- `shaders/gpu_driven_cmd_patch.comp` (new, shared across buckets) — single-invocation cmd writer
- `gos_terrain_water_stream.cpp` — `UploadAndBindThinRecords` early-returns when `MC2_GPU_DRIVEN_WATER=1`; new `ComputeDispatchAndBindThinRecords` dispatches Beta-pattern compute.
- Bridge wires existing `renderWaterFastPath()` hook to call MDI when armed.

**Parity gate:** `MC2_GPU_DRIVEN_PARITY=1` runs BOTH paths, compares thin-record SSBO byte-equality at `(thin_record_count * sizeof(WaterThinRecord))` precision. Same parity-check shape as the renderWater Stage 3 parity infrastructure (silent-on-pass, 600-frame summary).

**Soak window:** 7 days per Track B precedent.

### Stage 2 — Terrain SOLID (largest CPU saver, targets Z1)

Tracy anchor: `GameCamera::render textureManagerRenderLists` at `gamecam.cpp:244-248`. Stage 2 SOLID (plus Stage 3 OVERLAY if it ships) must drop Z1 by ≥1.0 ms from current 1.35 ms mean. Stage 2 SOLID carries the bulk of the drop because SOLID has the highest per-frame thin-record count, and the dominant per-quad CPU term is the per-corner `lightRGB`/`fogRGB` copy from `ScreenVertex` mirror into the thin record. Phase C's compute shader reads Phase 1's lighting SSBO directly (eliminating that copy entirely) and writes the thin record on GPU — collapsing the per-quad CPU work in `PackThinRecordsForFrame` to a single `glDispatchCompute` per bucket.

**Deliverables:**
- `shaders/gpu_driven_terrain_solid.comp` (new) — cull/pack
- Reuse `gpu_driven_cmd_patch.comp` from Stage 1
- `gos_terrain_indirect.cpp` — `PackThinRecordsForFrame` early-returns when `MC2_GPU_DRIVEN_TERRAIN_SOLID=1`; new compute-dispatch path replaces.

**Parity gate:** byte-equality of thin-record SSBO + DAIC struct across tier1 5/5.

### Stage 3 — Terrain OVERLAY (CONDITIONAL — gated on Phase B)

PR2b OVERLAY's `IsFrameOverlayArmed()` returns `false` unconditionally today at `gos_terrain_indirect.cpp:169`. Phase C does NOT construct OVERLAY's static recipe — recipe-building is Phase B's scope per the Phase B/C boundary section.

**Two outcomes possible for v1:**

1. **Phase B ships an overlay recipe in this window** → Phase C Stage 3 wires a compute shader against it (same Beta two-dispatch shape as Stages 1+2; new `shaders/gpu_driven_terrain_overlay.comp`), and flips `IsFrameOverlayArmed()` to true under `MC2_GPU_DRIVEN_OVERLAY=1`.
2. **Phase B does NOT ship an overlay recipe in this window** → Phase C Stage 3 falls out of v1. OVERLAY remains scaffold-only. A follow-up slice picks it up when Phase B's recipe lands.

Outcome (2) is the default planning assumption — Phase C does not block on Phase B and ships Stage 1+2 as v1; Stage 3 is opportunistic.

When Stage 3 does ship (under outcome 1), the gate becomes:
- No legacy CPU baseline to parity against (the path was scaffold-only) → parity check is degenerate; visual canary at fixed seed/camera is the substitute.
- Tier1 5/5 visual identity check: WITH `MC2_GPU_DRIVEN_OVERLAY=0` (matches current `IsFrameOverlayArmed()=false` baseline, no overlay drawn) AND with `MC2_GPU_DRIVEN_OVERLAY=1` (overlay drawn — visual canary inspection only).

### Stage 4 — Soak window

7 days per Track B precedent. All shipped buckets (water + SOLID; OVERLAY if Stage 3 shipped) soak with `_PARITY=1` running silently every Nth frame.

### Stage 5 — Per-bucket default-on flips (rolling)

Each bucket flips independently as it passes parity + soak. Order: water (lowest risk) → SOLID → OVERLAY (if shipped).

### Stage 6 — Demote legacy CPU paths

Per-bucket CPU pack loops gated off (`MC2_GPU_DRIVEN_<BUCKET>=0`), NOT deleted. Deletion is a separate post-soak slice.

---

## Killswitches + env vars

Mirroring the prompt's "Killswitch + env vars" section, exact names:

- `MC2_GPU_DRIVEN=0` — global off, falls back to per-bucket CPU pack loops. Default-on after Stage 5 per-bucket flips clear.
- `MC2_GPU_DRIVEN_WATER=0`, `MC2_GPU_DRIVEN_TERRAIN_SOLID=0`, `MC2_GPU_DRIVEN_OVERLAY=0` (only if Stage 3 ships) — per-bucket killswitches. Allow bisection. (No `MC2_GPU_DRIVEN_TERRAIN_MINE` — MINE is not a Phase C bucket per v2 design.)
- `MC2_GPU_DRIVEN_PARITY=1` — runs both paths, comparator on. Default off.
- `MC2_GPU_DRIVEN_TRACE=1` — per-bucket dispatch counters + draw-count diagnostic. Default off.

All env-gated logs land in `[GPU_DRIVEN v<N>]` namespace per the `Debug Instrumentation Rule` in CLAUDE.md.

---

## Parity / Soak gates (per the prompt + tier-1 instrumentation rules)

- Per-frame thin-record SSBO byte-equal under `MC2_GPU_DRIVEN_PARITY=1` per bucket, across tier1 5/5 (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`).
- Indirect-cmd struct byte-equal (4 GLuints) per bucket per frame.
- Tracy zone targets per the "Target zones" section above — **all expressed against the wolfman-mc2_10 anchor** (see Measurement Context):
  - **Z2 water** (`render water` / `render waterFastPath`) — Stage 1 must drop ≥80% at wolfman-mc2_01 (water-heavy) anchor (water Stage 3 precedent was 78–85%).
  - **Z1 textureManagerRenderLists** — Stages 2+3+4 (terrain buckets) collectively must drop ≥1.0 ms from current 1.35 ms mean at wolfman-mc2_10.
  - **Z3 objects** — informational only; small drop acceptable from static-prop coordination polish, large drop indicates scope creep.
- Combined Z1+Z2+Z3 ≤ 500 µs/frame at wolfman-mc2_10 after all stages flip default-on (compute dispatch + barrier overhead budget).
- Sum of per-bucket `gpu_driven_dispatch_us_per_frame` ≤ 500 µs.
- Tier1 5/5 PASS triple: unset / `<BUCKET>=1` / `<BUCKET>=1 PARITY=1`.
- No new `GL_INVALID_*` lines.
- No sync stalls (per `substrate_coalesce_sync_point_lesson.md` — verified by absence of `glGetBufferSubData` against Phase-C-written buffers on the hot path; grep enforced).
- `[DESTROY v1]` delta = 0 per bucket per mission.

---

## Load-bearing constraints (with grep evidence)

Per the prompt's "Load-bearing constraints" section + the worktree CLAUDE.md's documentation discipline:

| # | Constraint | Source | Phase C compliance |
|---|------------|--------|--------------------|
| 1 | GL 4.3 single-context — all GL on render thread | CLAUDE.md "Shader #version" rule | Compute dispatches occur in `ComputePreflight()` → bridge, both on render thread |
| 2 | No `glGetBufferSubData` on hot path | `substrate_coalesce_sync_point_lesson.md` + `gpu_cull_compute.cpp:834-846` | All Phase C SSBOs flow GPU→GPU (compute→MDI); diagnostic readback uses fenced ring lagged N frames |
| 3 | C++/GLSL struct lockstep | `cpp_glsl_ubo_struct_lockstep.md` | Indirect-cmd struct unchanged from PR1; thin-record structs unchanged (compute writes same layout) |
| 4 | Texture slot, not bindless handle | `mc2_texture_handle_is_live.md` | Compute writes `uint32 textureSlot`; CPU MDI bridge does one `glBindTexture(slot)` per bucket |
| 5 | `atomicAdd` on per-bucket counter, std430 aligned | GL spec | `GpuDrivenBucketHeader.visibleCount` at offset 0, std430-aligned |
| 6 | `IsFrameArmed()`-style preflight gate | `track_c_compute_cull.md` lifecycle gates + existing `IsFrameSolidArmed()` at `gos_terrain_indirect.cpp:1590` | Each bucket extends its existing armed flag; dispatch only fires when input data is ready |
| 7 | `GL_COMMAND_BARRIER_BIT` after compute writes indirect-cmd SSBO | `gpu_cull_compute.cpp:1030-1033` | Beta-pattern dispatch sequence ends with `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT \| GL_COMMAND_BARRIER_BIT)` before MDI |
| 8 | `quadList` is camera-windowed and rebuilt per frame | `quadlist_is_camera_windowed.md` | The QuadList-window input SSBO (R2) is CPU-uploaded per frame as a thin list of recipe indices that are in this frame's window. Static recipe (R0) indexes by `vertexNum = mapY * realVerticesMapSide + mapX` per the water_ssbo_pattern.md rule. |
| 9 | CPU pre-cull is the load-bearing frustum gate (legacy `worldToClip` produces finite values for behind-camera vertices) | `water_ssbo_pattern.md` constraint #3 | Phase C compute REPLACES the CPU pre-cull. The compute shader does the per-corner `projectZ` check using the same MVP that the CPU path uses today; per-tri `pzValid` bits are emitted into `flags` exactly as legacy. Parity check synthesizes from the same MVP — drift impossible by construction. |
| 10 | Depth/sampler/blend state inheritance in fast paths | `gpu_direct_renderer_bringup_checklist.md` | Bridge wraps each bucket's MDI with explicit `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`, `TERRAIN_DEPTH_FUDGE=0.001` (per `power_generator_decal_below_terrain.md`), sampler binding, blend reset |

Each constraint above has a grep-citation in the Verification Appendix.

---

## Risk register (precedents from past slices)

| Risk | Precedent | Phase C mitigation |
|------|-----------|--------------------|
| Compute math drift vs CPU produces sub-pixel artifacts | renderWater Stage 2 mc2_17 right-edge blank-vertex skip bug | `MC2_GPU_DRIVEN_PARITY=1` per-bucket byte-equality check with field-level mismatch printer (throttled). Required to pass tier1 5/5 before flip. |
| Per-frame `glGetBufferSubData` introduced by future maintainer | substrate-coalesce sync stall (62→128 fps after fix) | grep gate in pre-commit / adversarial-review checklist: any new `glGetBufferSubData` against Phase-C SSBOs requires explicit fence-ring justification. |
| C++/GLSL struct drift exposed only by content-diverse mission | `TG_HWLightsData` mc2_24 crash 2026-05-02 | Tier1 5/5 already mixes content; mc2_24 is the canary mission. Schema changes (none planned in v1) require lockstep commit + content-diverse smoke. |
| AMD driver compute-dispatch-before-MDI ordering | C2 / Q12 first GL_COMMAND_BARRIER_BIT in engine | Reuse exact barrier sequence from `gpu_cull_compute.cpp:1018-1033`. Validated on AMD RX 7900 XTX 26.3.1. |
| Lifecycle gate fail-open | C3-6 M-1 interlock bug | Each bucket's `IsFrame<bucket>Armed()` honors the same arming-disabled latch as PR1's `s_processArmingDisabled`. |
| Plan v1-style fictional symbol references | `indirect-terrain-draw plan v1` fictional `TerrainQuadRecipe` fields | This design doc grep-verifies every cited symbol; adversarial-review gate runs against this doc explicitly per CLAUDE.md "Review Discipline" rule. Verification Appendix below. |
| LOD-swap collateral when an "unaffected" path actually shares state | BldgAppearance LOD swap unsafe for animated buildings | Phase C does not touch building / mech paths. Bucket boundaries explicit; cross-bucket state untouched. |

---

## Adversarial-review trigger checklist (per CLAUDE.md "Review Discipline")

This slice qualifies for FULL adversarial-plan-review:

- ☒ Architectural endpoint (closes the CPU→GPU draw pipeline arc).
- ☒ Multiple new compute shaders + SSBO schemas.
- ☒ Retires CPU per-frame indirect-cmd / thin-record build across multiple draw paths.
- ☒ Cross-cutting with Phase B (sibling slice, may ship in either order).
- ☒ Sync-pattern hazards (the substrate-coalesce lesson is the precedent that almost-caught us before).
- ☒ Perf gate ≥1.5 ms cut.

**Dispatch prompt MUST include verbatim:** "use the adversarial-plan-review skill in `.claude/skills/`".

Adversarial review is expected to find:
- Any divergence between cited file:line and actual code (verification appendix catches at write-time, but it's the safety net not the only gate).
- Any binding-point collision against the existing 0–16 range used by Track C / Phase 1.
- Any thin-record byte-layout drift between the compute shader's pack order and the legacy CPU pack order.
- Any sync stall introduced by a future-design diagnostic readback.
- Any cross-bucket dependency the per-bucket killswitch story doesn't cover.

---

## Exit criteria (Phase C v1)

- All Parity / Soak gates pass for each bucket that ships in v1 (water, SOLID, MINE, OVERLAY).
- `MC2_GPU_DRIVEN=0` reproduces pre-slice tier1 behavior bit-for-bit per-bucket.
- Memory file `gpu_driven_indirect_cmds.md` captures the compute-shader pattern + per-bucket invalidation contract + sync-stall avoidance pattern.
- Phase B (if it ships after Phase C) can layer its static SSBO under the compute shader's input read by changing the binding for R0 only.
- gpu-mech-batcher branch's compute pipeline for mechs has documented compatibility with Phase C's pattern (a brief section appended to this design doc, OR a sibling memory file).

---

## Stop conditions

- Per-bucket parity diff non-zero after 3 iteration rounds → STOP that bucket, surface findings. Likely causes: compute-shader math vs CPU math precision drift, or barrier ordering.
- Per-bucket Tracy delta < 200 µs → STOP that bucket, surface to user. Other buckets may still ship.
- Sync stall surfaces in profiling (Tracy GPU timeline shows CPU wait for GPU completion) → STOP, switch to non-blocking ring + skip-frame fallback per `gpu_cull_readback.cpp` precedent.
- Any tier1 mission FAIL under `MC2_GPU_DRIVEN_<BUCKET>=1` → STOP, revert that bucket to parity-only mode, bisect.
- AMD driver compute-dispatch-before-MDI ordering bug surfaces → STOP, surface to user; may need explicit fence between dispatch and draw.

---

## Verification Appendix (grep evidence per cited symbol)

Run all greps at write-time against `claude/gpu-driven-rendering` worktree HEAD `5667023`. Status: **M** matches as cited, **D** divergent line but symbol present, **NF** not found / fictional.

| # | Citation | Status | Notes |
|---|----------|--------|-------|
| A-1 | `gos_terrain_indirect.cpp:1262-1269` `DrawArraysIndirectCommand` struct | M | exact match incl. `static_assert(sizeof == 16)` |
| A-2 | `gos_terrain_indirect.cpp:1566-1580` `BuildIndirectCommands` body | M | one DAIC per frame via `glBufferSubData` |
| A-3 | `gos_terrain_indirect.cpp:1377` `PackThinRecordsForFrame` declaration | M | `static int PackThinRecordsForFrame()` |
| A-4 | `gos_terrain_indirect.cpp:1400` `s_shadow[kMaxThinRecords]` stack-local stage | M | `static TerrainQuadThinRecord s_shadow[kMaxThinRecords];` |
| A-5 | `gos_terrain_indirect.cpp:1253` `kMaxThinRecords = 65536u` | M | exact line |
| A-6 | `gos_terrain_indirect.cpp:1590` `IsFrameSolidArmed()` | M | exact line |
| A-7 | `gos_terrain_indirect.cpp:1605` `ComputePreflight` body | M | preflight that returns false if not armed |
| A-8 | `gameos_graphics.cpp:2468` `glMultiDrawArraysIndirect` PR1 call | M | exact line per `2026-05-08-substrate-coalesce-design-v2.md:453` cross-check |
| B-1 | `gos_static_prop_batcher.cpp:3228` `glMultiDrawElementsIndirect` armed-mode | M | grep output line 3228 |
| B-2 | `gos_static_prop_batcher.cpp:3250` `glMultiDrawElementsIndirect` second | M | grep output line 3250 |
| B-3 | `gos_static_prop_batcher.cpp:3441` `glDrawElementsIndirect` legacy | M | grep output line 3441 |
| B-4 | `gpu_cull_compute.cpp:1018` `glDispatchCompute(patchGroups, 1, 1)` | M | exact line; pattern source for Beta dispatch shape |
| B-5 | `gpu_cull_compute.cpp:1030-1033` `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT \| GL_COMMAND_BARRIER_BIT)` | M | exact comment cites "Q12: first GL_COMMAND_BARRIER_BIT in engine" |
| B-6 | `gpu_cull_compute.cpp:834-846` substrate-counter swap (replaces `glGetBufferSubData`) | M | comment explicitly references the 6 ms/frame sync stall mc2_10 62→128 fps fix |
| B-7 | `shaders/gpu_cull_patch.comp` `DrawElementsIndirectCommand` mirror | M | std430 binding 2 |
| C-1 | `gos_terrain_water_stream.cpp:345` `UploadAndBindThinRecords` | M | exact line per grep |
| C-2 | `gos_terrain_water_stream.cpp:453` `WaterThinRecord tr{}` (pack body) | M | exact line per grep |
| D-1 | `code/objmgr.cpp:1939-2050` per-object update loop (prompt's reference) | **M (resolved v2)** | adversarial-review pass grep-confirmed `:1939` opens `ZoneScopedN("GameLogic.Units.TerrainObjects")`; range :1939–:2050 covers the terrain-objects update sub-block (specialBuildings, gates, terrain-block iteration, TracyPlot summary). Citation matches; v1 caveat was over-cautious. |
| D-2 | sibling worktree `pre-bake-terrain` HEAD | M | `5667023` matches `nifty-mendeleev` — Phase B not yet shipped |
| D-3 | Phase 1 (terrain lighting) shipped commits `594add9 / eda2431 / ff8de07 / ff35f03` | M | git log shows all four, last is "DEFAULT-ON flip" |
| E-1 | `quadlist_is_camera_windowed.md` constraint | M | memory present, dated 2026-04-30 |
| E-2 | `water_ssbo_pattern.md` skip set (#1-#5) | M | memory present, 10 days old (annotated as stale-warning by harness; concepts unchanged but file:lines need fresh grep at Stage 1 plan-write time) |
| E-3 | `mc2_texture_handle_is_live.md` slot-not-handle rule | M | memory present, 16 days old (concepts unchanged; latest exception is Phase A bindless plan deferred — captured here) |
| E-4 | `cpp_glsl_ubo_struct_lockstep.md` rule | M | memory present, 8 days old; mc2_24 crash 2026-05-02 origin |
| E-5 | `substrate_coalesce_sync_point_lesson.md` rule | M | most recent (today, 2026-05-11) — origin commit mc2_10 62→128 fps |
| F-1 | indirect-cmd schema unchanged from PR1 (load-bearing) | M | `DrawArraysIndirectCommand` is GL-spec-mandated 16 B / 4 GLuints; no Phase C field needed |
| F-2 | binding-point allocations in §"Per-bucket binding-point allocation" | **M (resolved v2)** | v1 table was symbolic ("(existing X binding)") and concealed within-program collisions in newly-proposed compute programs. v2 replaces with concrete per-program tables using slots 0–7 internally per compute program. Each GL program has its own binding namespace, so cross-program reuse of `binding=2` (Phase 1 lighting output AND existing thin-record SSBO) does NOT collide at runtime — they live in different programs. Adversarial-review confirmed: no shipped code uses slots 17–27 (entirely free), but the v1 table's design issue was structural (within-program collision in proposed compute programs), not numerical. v2 fixes the structural issue. |
| G-1 | `pr2_detail_overlay_mine_stage0_recon.md` PR2a-dead claim | M | memory present, references commit 521d83a |
| G-2 | `indirect_terrain_solid_endpoint.md` PR2b scaffold-only (`IsFrameOverlayArmed()` returns false) | M | memory present, 2026-05-01; needs Stage 3 fresh grep before forward-construction code |
| H-1 | `mech_vehicle_gpu_pull_in.md` Track D merged 2026-05-10 commit `0d5ce93` | M | matches git log "merge: pull in Track D GPU mech+vehicle batcher from claude/gpu-mech-batcher" |
| I-1 | `code/gamecam.cpp:244` Tracy zone `"GameCamera::render textureManagerRenderLists"` | M | exact label at line 244; wraps `mcTextureManager->renderLists()` at :245 and `endFrameTexResolve()` at :246 (range :244–:248) |
| I-2 | `code/gamecam.cpp:217` Tracy zone `"GameCamera::render water"` | M | exact label at line 217; wraps `land->renderWater()` at :218. When `MC2_RENDER_WATER_FASTPATH` armed (default-on), legacy `renderWater` early-returns; live cost is in the next entry. |
| I-3 | `code/gamecam.cpp:255-256` Tracy zone `"GameCamera::render waterFastPath"` | M | wraps `land->renderWaterFastPath()` at :256; this is the active water cost when fast-path is armed |
| I-4 | `code/gamecam.cpp:212` Tracy zone `"GameCamera::render objects"` | M | exact label at line 212; wraps `ObjectManager->render(true, true, true)` at :213 |
| I-5 | `code/mission.cpp:527` Tracy zone `"GameLogic.Mission.TextureManager"` wrapping `mcTextureManager->update()` | D | user-cited `:526` is the guarding `if (!isPaused() \|\| MPlayer)`; the actual Tracy zone + call are at `:527`. Pause-guard at :526 is load-bearing per `pause_unpause_diagnostic_for_static_render_bugs.md`. |
| I-6 | `mclib/txmmgr.cpp:1023` `addLightDataStructure(...)` function declaration | **M (corrected v2)** | grep-verified function declaration at `:1023`; Tracy zone `"addLightDataStructure scan"` at `:1028`. **v2 strikes the v1 "256-entry / 99% hit rate" claim** — the dedup map is `std::unordered_map<uint64_t, uint32_t>` at `:901`, unbounded, initial capacity 128. The fictional sub-zone characterization is removed; the Z1 ≥1.0 ms target rests on combined-zone wall-clock measurement, not on attributing savings to dedup-walk elimination. |
| J-1 (v2) | `gameos_graphics.cpp:2215` water base `glDrawArrays` + `:2242` detail/spray `glDrawArrays` | M | grep-verified — base layer uses `isWater=1, detailMode=0, baseTex`; detail layer uses `isWater=2, detailMode=1, detailTex`; same `drawVerts`. v2's Stage 1 water draw-shape section commits to 2-cmd MDI with `gl_DrawID`-indexed per-cmd SSBO. |
| J-2 (v2) | `gameos_graphics.cpp:2609` MINE single `glDrawArrays` (NOT MDI) | M | grep-verified inside the MINE bridge function; called from `gos_terrain_indirect::DrawMineStatic()` at `gos_terrain_indirect.cpp:1963`, which is invoked once per frame in `Render.TerrainMines` Tracy zone at `mclib/txmmgr.cpp:1812-1817`. v2 removes MINE from Phase C scope (no per-frame thin-record pack to GPU-ify). |
| J-3 (v2) | `gos_terrain_water_stream.cpp:45` `GLuint g_recipeBuffer = 0` | M | grep-verified — this is the water recipe SSBO symbol. v1's fictional `g_waterRecipeSSBO` replaced throughout v2. |
| J-4 (v2) | `gos_terrain_lighting.cpp:102` `TL_OUTPUT_BINDING = 2u`; `:572` `glBindBufferBase(..., TL_OUTPUT_BINDING, s_computeOutputSsbo)` | M | grep-verified Phase 1's output SSBO is bound at slot 2 in Phase 1's OWN program. Phase C compute programs bind it at their own internal slot 1 (per the v2 binding tables) — different programs, different binding namespaces, no collision. |
| J-5 (v2) | `shaders/gos_terrain_thin.vert:9` `layout(std430, binding = 2) readonly buffer ThinRecordBuf` | M | grep-verified existing thin-record SSBO consumer at slot 2 in the thin VS. Confirms cross-program binding-namespace independence — the same numeric binding `=2` is used in three different programs (Phase 1 compute, thin VS, future Phase C compute) without runtime conflict because each program has its own binding table. |
| J-6 (v2) | `gos_terrain_indirect.cpp:139` `IsMineEnabled()` comment "Tier1 5/5 PASS with arming verified across PR2c Stages 0c/1c/2c" | M | grep-verified Stage 2c is shipped on baseline — resolves boundary-reviewer C-1 contradiction. v2 corrects boundary table + half-ported inventory. |
| J-7 (v2) | `gos_terrain_indirect.cpp:1923` `RebuildMineStaticVBOIfDirty()` (dirty-flag lazy mission-load) | M | grep-verified MINE's per-frame work is dirty-flag-gated lazy rebuild, NOT a per-frame thin-record pack. Confirms v2's removal of MINE from Phase C scope. |
| J-8 (v2) | `mclib/txmmgr.cpp:1812-1817` `Render.TerrainMines` Tracy zone wrapping `gos_terrain_indirect::DrawMineStatic()` | M | grep-verified MINE has its OWN Tracy zone, separate from Z1's `textureManagerRenderLists`. MINE is not inside the Phase C target zones. |
| J-9 (v2) | `gos_terrain_indirect.cpp:169` `IsFrameOverlayArmed()` returns `false` unconditionally | M | grep-verified OVERLAY is scaffold-only. Resolves the v2 OVERLAY contingency framing — Stage 3 ships only under outcome (1) (Phase B publishes overlay recipe). |
| J-10 (v2) | `gos_terrain_lighting.cpp:613-633` Phase 1 dispatch + barrier + copy + barrier sequence | M | grep-verified Phase 1 emits `GL_SHADER_STORAGE_BARRIER_BIT` post-dispatch at `:621`. Phase C compute can read `s_computeOutputSsbo` directly after that barrier — no additional barrier required between Phase 1 dispatch and Phase C dispatch. |
| J-11 (v2) | `gos_terrain_lighting.cpp:644-` 3-slot non-blocking `tryConsume` ring | M | grep-verified Phase 1 is frame-pipelined (lighting bytes lag by ≥1 frame in steady-state). v2's parity-gate reframe section commits to forcing Phase 1 into non-pipelined synchronous mode during `MC2_GPU_DRIVEN_PARITY=1` windows only. |
| K-1 (v3) | `gos_terrain_lighting.cpp:86` `static const bool s_parity = (getenv("MC2_TERRAIN_LIGHTING_PARITY") != nullptr)` | M | grep-verified env name AND the static-cache structure. The cache locks Phase 1's parity mode to whatever was set at process startup; runtime toggle structurally impossible. Forced v3's C-decision toward option (a) (smoke-runner sets both env vars externally). |
| K-2 (v3) | `gos_terrain_lighting.h` public API — no SSBO accessor today | M (negative-grep) | grep-verified `s_computeOutputSsbo` is file-static at `gos_terrain_lighting.cpp:137`; no `GetOutputSsbo` / `getOutputSsbo` / equivalent accessor exists in the public header. v3's "Required Phase 1 API extension" subsection commits to publishing `gos_terrain_lighting::GetOutputSsbo()` (one-line getter) as part of Phase C v1's landing commit or a small standalone Phase-1 commit. |
| K-3 (v3) | `mclib/txmmgr.cpp:1626` `ZoneScopedN("Render.TerrainSolid")` | M | grep-verified the SOLID Tracy zone lives in `mclib/txmmgr.cpp`, not `gamecam.cpp`. Zone ends at `txmmgr.cpp:1743`. |
| K-4 (v3) | `mclib/txmmgr.cpp:1664` `gos_terrain_indirect::DrawIndirect()` call site | M | grep-verified `DrawIndirect()` invoked inside `Render.TerrainSolid` zone. This is the MDI call site, AFTER Phase C's compute dispatch must have completed. |
| K-5 (v3) | `mclib/terrain.cpp:1792` `gos_terrain_indirect::ComputePreflight()` call site | M | grep-verified `ComputePreflight()` invoked from `Terrain::geometry quadSetupTextures` Tracy zone at `terrain.cpp:1787`. This is where v3 places Phase C SOLID compute dispatch — same hook the existing arming flow uses. |
| K-6 (v3) | `gos_terrain_lighting.h:97-99` namespaced parity API style | M | grep-verified existing public API uses namespaced `gos_terrain_lighting::FnName()` style. v3's `GetOutputSsbo()` follows this convention. |

**Status note on D-1:** the prompt cites `code/objmgr.cpp:1939-2050` for "per-object update loop." This citation came from an older commit and was not verified against current HEAD at this design doc's write-time. **Action:** the adversarial-review pass MUST re-grep this range and either confirm or update the citation. Phase C does NOT depend on this range (the per-object update loop is Track-D scope), so this is informational drift, not a design hazard.

**Status note on F-2:** the binding-point table is the most likely site of an adversarial-review CRITICAL finding. Phase C MUST re-grep `binding = N` against the worktree at Stage 1 plan-write time and produce a no-collision proof. The shape of the table — one block per bucket — is the load-bearing architectural decision; the specific numbers are mechanical.

---

## What this design doc does NOT decide

- **The exact compute shader pack-loop body per bucket.** That's Stage 1 plan-write work. The pack body is constrained to byte-match the legacy CPU pack body's output; the parity check enforces this mechanically.
- **The exact texture-binding sequence for OVERLAY (Stage 3, conditional).** Forward-construction, no CPU baseline. Stage 3's plan will derive this from PR2b's existing design doc (`2026-05-08-pr2b-overlay-indirect-design.md`) and from Stage 2's MDI bridge.
- **Whether Phase C's compute should also subsume the projectZ pre-cull for the water bucket's static-prop-passing surfaces.** Out of scope for v1; the water compute shader does its own per-quad projectZ.
- **Track D's eventual integration.** Deferred to Phase C v2 per the inventory above.

---

## Open questions — v2 status

The v1 adversarial-review pass (`13a0c06` substance + boundary reviewers) resolved most of these. Status:

1. **Beta-pattern second-dispatch overhead** — **RESOLVED.** Reviewer M-7 Q1 confirmed: GL 4.3 has no global compute barrier across workgroups; single-dispatch "last-workgroup writes cmd" relies on driver scheduling order not guaranteed by spec (same trap class as the substrate sync stall). Beta two-dispatch is the right call. Cost ~5 µs/bucket; well within budget.
2. **Per-bucket vs shared compute program** — **RESOLVED.** Reviewer M-7 Q2 confirmed per-bucket is correct: bucket recipe layouts differ; uniform-branch in a shared program would introduce GPU-side divergence across buckets; per-bucket helps driver shader-cache utilization. Binding-pressure concern was unfounded (slots 0–7 used internally per program).
3. **Indirect-cmd struct write timing** — **RESOLVED.** Reviewer M-7 Q3 confirmed single-invocation patch is correct: atomicMax-from-every-invocation creates hot-spot SSBO write traffic on `cmds[0]`; single-invocation patch is easier to reason about for parity synthesis; the Beta two-dispatch already pays the second-dispatch cost, no win from collapsing.
4. **Phase B coordination contract** — **RESOLVED (v2 makes explicit).** Reviewer M-7 Q4 correctly flagged "compatible by construction" as wrong. v2's "Phase B recipe-layout-frozen contract" subsection states the explicit restriction: Phase B MUST NOT change field layout/order/type/size in any recipe SSBO Phase C v1 consumes without lockstep Phase C commit.

### v2 questions — all resolved in v3

The v2 adversarial-review pass (targeted reviewer against commit `802aca4`) verdicts:

A. **WaterPerCmd SSBO layout captures all per-layer uniforms** — **PASS.** Reviewer grep-confirmed every uniform the FS reads per-layer at `gameos_graphics.cpp:2206-2243` is captured by the v2 `WaterPerCmd` struct. No edit required in v3.

B. **`IsFrameSolidArmed()` consumers tolerate count=0 MDI** — **PASS** with one MINOR ring-fence churn note: every armed frame issues a `glBindBufferRange` at the per-frame ring slot whether or not the MDI draws any quads. Cost is microseconds; not blocking. No edit required in v3; flagged for Stage 2 plan-write to confirm ring-fence cost stays under 5 µs in the count=0 case.

C. **Parity-gate Phase 1 coupling** — **DECIDED, v3 commits to option (a).** Smoke-runner sets both `MC2_GPU_DRIVEN_PARITY=1` AND `MC2_TERRAIN_LIGHTING_PARITY=1` at process startup. Reason: Phase 1's `IsParityCheckEnabled()` at `gos_terrain_lighting.cpp:85-87` caches the env value in a `static const bool`; runtime toggling is structurally impossible. v3's parity-gate reframe section commits to this; documentation contract is now the only valid mechanism.

D. **`GetOutputSsbo()` accessor** — **FAIL → FIXED in v3.** Accessor did not exist in `gos_terrain_lighting.h`; `s_computeOutputSsbo` was file-static. v3 adds "Required Phase 1 API extension" subsection committing to a one-line namespaced getter `gos_terrain_lighting::GetOutputSsbo()` as part of Phase C v1's landing commit (or a small standalone Phase-1 commit ahead of it).

E. **Phase C dispatch site placement** — **FAIL → FIXED in v3.** v2 wrongly placed compute dispatch "inside `gamecam.cpp Render.TerrainSolid`." Reviewer grep-confirmed `Render.TerrainSolid` zone lives in `mclib/txmmgr.cpp:1626`, and the SOLID `ComputePreflight()` is invoked from `mclib/terrain.cpp:1792` inside `Terrain::geometry quadSetupTextures`. v3's barrier-ordering diagram correctly places the compute dispatch inside `ComputePreflight()` at `gos_terrain_indirect.cpp:1605` and the MDI inside `Render.TerrainSolid` at `txmmgr.cpp:1664` — different Tracy zones, with `GL_COMMAND_BARRIER_BIT` as the load-bearing sync (not zone boundary).

### v3 → v4 questions (if a further review pass runs)

The v3 revisions are mechanical fixes against v2's flagged issues; no major new claims were introduced. If a further review pass runs, the natural targets are:

F. **Ring-fence count=0 cost confirmation.** Stage 2 plan-write should measure: how much does the per-frame `glBindBufferRange` + compute dispatch + barrier + count=0 MDI cost in the worst case (mission start, no quads visible, but armed)? Budget: ≤5 µs. If higher, add an early-out path that bypasses the dispatch when zero quads can possibly be visible (e.g., `IsDenseRecipeReady() && quadList.size() > 0`).

G. **`Mission.TextureManager` vs `Terrain::geometry quadSetupTextures` zone interaction.** Phase 1 dispatch lives in `Mission.TextureManager` (mission.cpp:527); Phase C SOLID dispatch lives in `Terrain::geometry quadSetupTextures` (terrain.cpp:1792). Confirm at Stage 2 plan-write that the two zones execute in order each frame (Phase 1 BEFORE Phase C SOLID), and that no Tracy zone between them resets compute SSBO bindings or invalidates the barrier sequence.
