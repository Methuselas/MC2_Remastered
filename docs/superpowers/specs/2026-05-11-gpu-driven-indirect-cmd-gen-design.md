# GPU-Driven Indirect Command Generation — Stage 0 Design (Phase C)
## Completing the GPU transfer for buckets already half-ported

> **Companion to** the dispatch prompt at [`docs/superpowers/plans/progress/2026-05-11-gpu-driven-rendering-prompt.md`](../plans/progress/2026-05-11-gpu-driven-rendering-prompt.md).
>
> **Thesis:** Phase C is a **completion slice, not a greenfield slice.** Track A (object admission), Track B (widened static-prop registry), Track C (GPU compute cull + readback + lifecycle gates), Phase 1 (GPU terrain lighting), PR1 (indirect-terrain SOLID), PR2c (MINE), and the renderWater fast path have all already moved the **heavy GPU-resident pieces** to the GPU. What remains on CPU per-frame for each of these buckets is a **thin-record pack loop** that bridges already-GPU data (Phase 1's lighting SSBO, PR1's recipe SSBO, Track C's visibility) back through a CPU mirror into a CPU-written SSBO that the GPU then consumes. Phase C closes that GPU→CPU→GPU loop by collapsing the bridge into a single compute dispatch that reads directly from the upstream GPU SSBOs.
>
> Stage 0 deliverable: enumerate the **GPU-share-vs-CPU-share for each shipped bucket**, identify the specific CPU residual that Phase C eliminates per bucket, define the compute-shader input/output contracts and sync pattern. The implementation plan (per-bucket stages) is authored against this design.

**Worktree:** `claude/gpu-driven-rendering` (branched from `claude/nifty-mendeleev` @ `5667023`).
**Sibling Phase B:** `claude/pre-bake-terrain` @ `5667023` — **partially shipped per-bucket** (see "Phase B / Phase C boundary" section below). The unified Phase B umbrella slice is in flight; the per-bucket static recipe SSBOs that Phase C consumes have shipped piecemeal under prior slices (PR1 for SOLID, water-recipe for water, PR2c for MINE), with Mine wire-up Stage 2c + Overlay recipe + (optional) static lighting bake still pending in the sibling worktree.

---

## Documentation discipline (load-bearing)

Every symbol, file:line, struct field, env flag, and binding-point number cited below was grep-verified at write-time against the worktree's source at HEAD `5667023`. Each citation appears in the Verification Appendix at the end with status M (matches) / D (divergent) / NF (not found). Per CLAUDE.md, this discipline supersedes any conflicting reference in prior memory files (which decay).

The framing-correction below is the most consequential write-time correction: the prompt names this slice "GPU-driven indirect command generation," but the actual CPU hot work in every shipped indirect path is the **thin-record build**, not the DAIC struct write. The design treats the indirect-cmd struct as a byproduct of the compute pass, not the headline output.

---

## Status

- Stage 0 (this design): IN REVIEW. Adversarial review gate per the prompt's "Adversarial review gate (mandatory)" section runs against this doc before any code lands.
- Stage 1+: planned, blocked on Stage 0 sign-off + adversarial-review clearance.

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
| **Z1** | `GameCamera::render textureManagerRenderLists` | `code/gamecam.cpp:244-248` wraps `mcTextureManager->renderLists()` at `:245` + `endFrameTexResolve()` at `:246` | **1.35 ms mean** (22.97% of render activeScene) | Per-bucket CPU thin-record packing for terrain SOLID + MINE + OVERLAY; `addLightDataStructure()` dedup walk at `mclib/txmmgr.cpp:1023` (Tracy zone `"addLightDataStructure scan"` at `:1028`, ~99% hit rate on a 256-entry hash-dedup map); per-frame texture-handle resolution cache; per-bucket batch list aggregation |
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
| **Mine (PR2c)** | 🟡 Stage 1c infrastructure shipped; **Stage 2c wire-up pending in Phase B session** | Phase C Stage 3 has a choice: (a) wait for Phase B Stage 2c wire-up and consume the unified mine recipe; or (b) target the existing PR2c MINE recipe and do a binding swap when Stage 2c lands. **Default: option (b)** — Phase C Stage 3 doesn't block on Phase B. |
| **Overlay (PR2b)** | ⬜ No recipe (sibling session may or may not build one) | Phase C Stage 4 is **conditional**: if Phase B ships an overlay recipe in the same window, Phase C Stage 4 reads it. If not, **Phase C Stage 4 defers** — building a minimal in-line recipe in Phase C would violate the boundary (static-bake work in the per-frame compute session). |
| **Detail (M2c / PR2a)** | ⬜ Candidate delete (sibling session may retire it) | Not a Phase C bucket. Dead path. |
| **Lighting** | ⬜ Static lighting bake possible (sibling-decided) | Phase C reads Phase 1's per-frame compute lighting SSBO. Whether Phase 1 (the *dynamic* GPU compute lighting; merge commit `93d3cbd` with stages `594add9 / eda2431 / ff8de07 / ff35f03`) is treated as "shipped" or "foundational-but-not-yet-shipped" affects Phase C's "eliminate lighting CPU bounce" headline win — but does not block Phase C from shipping; the binding for Phase 1's output is the seam, and a CPU-fallback path is the obvious degradation. |
| **Vertex projection** | n/a (camera-dependent — not a static bake) | Per `vertex_project_loop_d1_asymptotic.md` D1 hoist closed asymptotic (compiler ceiling reached). SIMD / GPU port deferred. Phase C does NOT subsume vertex projection in v1; the per-bucket compute shader does its own per-quad MVP transform, but it does not consume `vertexProjectLoop`'s output. |
| **Track C compute cull** | n/a | ✅ C0+C1a+C1b+C2+C3 shipped on nifty-mendeleev. Phase C reads Track C's visibility-mirror output for buckets that have one (static-prop, mech/GV — neither is in Phase C v1). Terrain/water buckets do their OWN per-quad projectZ in the compute shader, not Track C cull. |

### The three coordination decisions

1. **Mine — Phase C does not block on Phase B Stage 2c.** Stage 3 of Phase C reads PR2c's existing MINE recipe. When Phase B Stage 2c lands, Phase C does a binding swap (no schema change expected). If Phase B Stage 2c changes the recipe layout, Phase C re-syncs after Phase B ships.
2. **Overlay — Phase C blocks on Phase B Overlay recipe.** This is the one place Phase C is gated on Phase B. If the sibling session decides not to build an overlay recipe, Phase C Stage 4 falls out of the v1 scope and OVERLAY remains scaffold-only.
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

Either slice can ship first; they're independent. If Phase B ships first, Phase C v1 simply has more Phase B output to consume (Mine Stage 2c, Overlay recipe, possible static lighting bake). If Phase C ships first, Phase B integrates against an already-running compute path and Phase C swaps bindings post-v1. **The only ordering constraint is:** if Phase B ships an overlay recipe and Phase C wants OVERLAY in v1, Phase B's overlay recipe must land before Phase C Stage 4.

---

## Half-ported inventory — what's already on GPU vs what's still on CPU per bucket

The buckets in scope are not greenfield. Each one already has a substantial GPU share shipped under prior tracks/slices. Phase C's job is to identify the specific CPU residual on the hot path and finish the transfer. This table is the load-bearing inventory for Stage 0's per-bucket inclusion decision.

| Bucket | Already on GPU (shipped) | Still on CPU per frame (Phase C's target) | Origin slice |
|--------|--------------------------|-------------------------------------------|--------------|
| **Water** | Single GPU draw of pre-packed thin records (`renderWaterFastPath`, default-on); recipe SSBO mission-static; VS expands to 6 verts/quad; FS unchanged from legacy | `UploadAndBindThinRecords` per-quad pack loop at `gos_terrain_water_stream.cpp:345-...:478` (walks quadList, projectZ-gates each quad, copies per-corner lightRGB/fogRGB from CPU vertex pool into thin record). Single `glDrawArrays(thinCount*6)` — NOT indirect today. | renderWater Stage 1+2+3 (2026-04-30) |
| **Terrain SOLID** | Recipe SSBO mission-static (`g_recipeSSBO`); MDI draw via `glMultiDrawArraysIndirect` (default-on); colormap atlas; cement multi-sampler at unit 3; PR1's frag-side SSBO fetch via flat RecordIdx varying | `PackThinRecordsForFrame` at `gos_terrain_indirect.cpp:1377` (stages up to 65536 records into stack-local shadow per frame, walks quadList, recipe lookup by `vertexNum`, per-corner mutable-state copy) + trivial 16 B `BuildIndirectCommands` at `:1566` | PR1 (commit `e22fa3a`, default-on 2026-05-01) |
| **Terrain MINE (PR2c)** | Recipe SSBO + MDI path shared with SOLID; default-on commit `6d4a6f7` | Same `PackThinRecordsForFrame`-shape pack loop with mine-specific recipe fields | PR2c |
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
| 2 | **Terrain MINE (PR2c)** | shares `PackThinRecordsForFrame()` machinery | shares PR1 MDI call | default-on (commit `6d4a6f7`) | **Stage 3** (mechanical extension of Stage 2) |
| 3 | **Terrain DETAIL (PR2a)** | dead since `521d83a` per `pr2_detail_overlay_mine_stage0_recon.md` | n/a | dead | **OUT** (delete slice, not Phase C) |
| 4 | **Terrain OVERLAY (PR2b)** | not yet built (CPU path stays legacy) | `IsFrameOverlayArmed()` returns `false` unconditionally per `indirect_terrain_solid_endpoint.md` | scaffold-only | **Stage 4** (ship directly as GPU-driven; skip the CPU intermediate) |
| 5 | **Water** | `gos_terrain_water_stream.cpp:345 UploadAndBindThinRecords()` + `:453 WaterThinRecord` pack | non-indirect `glDrawArrays(thinCount*6)` — see Stage 1 conversion note | default-on (water fast path shipped 2026-04-30) | **Stage 1** (simplest, smallest, has full parity infrastructure already) |
| 6 | **Static-prop substrate** | `gos_static_prop_batcher.cpp` builds DEIC array at **mission load** (not per-frame); `gpu_cull_compute.cpp` compute shader patches `instanceCount` per-frame | `gos_static_prop_batcher.cpp:3228/3250/3441` `glMultiDrawElementsIndirect` / `glDrawElementsIndirect` | default-on (`MC2_GPU_CULL_SUBSTRATE=1`, commit `7b9ad5f`) | **OUT** — already GPU-authoritative for the per-frame field (`instanceCount`). Per-packet layout rebuild is mission-load-amortized, not per-frame. Phase C would have no win here. |
| 7 | **Mech/GV (Track D gpu-mech-batcher)** | merged 2026-05-10 (`0d5ce93`) | various per `mech_vehicle_gpu_pull_in.md` | merged, in soak | **DEFER to Phase C v2** — needs its own soak settle before a port slice. Track D's CacheGpuLightData guard conflict (msl.cpp) is one canary that the substrate is still settling. |
| 8 | **Substrate-coalesce per-packet cmds** | per-packet DEIC cmds rebuilt when packet layout changes (mission load + bucket-touch events) | shared with row 6 | armed default-on 2026-05-11 (commit `7b9ad5f`) | **OUT** — same reasoning as row 6: not per-frame work. |

### Per-stage Phase C scope (v1 of slice)

- **Stage 1 — Water** (precedent proof).
- **Stage 2 — Terrain SOLID** (largest CPU saver).
- **Stage 3 — Terrain MINE** (mechanical extension).
- **Stage 4 — Terrain OVERLAY** (the missing-but-designed bucket; ship as GPU-driven from inception).
- **Stages 5/6/7** per the prompt: soak window, default-on flip per-bucket, demote legacy CPU paths.

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
                                         (terrain: PR1's g_recipeSSBO, etc.;
                                          water: g_waterRecipeSSBO)
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

The codebase already has heavy binding-point pressure (Track C uses 0–16; Phase 1 lighting adds more). Phase C bindings are allocated in a per-bucket non-overlapping range:

| Bucket | Recipe (R0) | Lighting (R1) | QuadList window (R2) | ThinRecord (W0) | IndirectCmd (W1) | VisibleCount (W2) |
|--------|-------------|---------------|----------------------|------------------|--------------------|--------------------|
| Water  | 17          | (Phase 1's existing binding, read-only) | 18 | (existing water thin SSBO binding) | 19 | 20 |
| SOLID  | (existing g_recipeSSBO binding) | (Phase 1's binding) | 21 | (existing thin SSBO binding) | 22 | 23 |
| MINE   | (PR2c recipe binding) | (Phase 1's binding) | 21 (shared windowing) | (PR2c thin SSBO) | 24 | 25 |
| OVERLAY | (new — PR2b's eventual recipe SSBO) | (Phase 1's binding) | 21 (shared windowing) | (new — PR2b's thin SSBO) | 26 | 27 |

The "shared windowing" SSBO for SOLID/MINE/OVERLAY exploits the fact that all three iterate the SAME `quadList` per frame. Building it once at the start of the terrain frame is one of Phase C's CPU-side simplifications (today each bucket's pack loop independently iterates `quadList`).

Verification Appendix item B-1 confirms the exact binding-point numbers currently in use; if any collision is found at adversarial-review time, the table reshuffles. The shape — one block of bindings per bucket — is the load-bearing structure.

---

## Stage plan (per the prompt's roadmap)

### Scope discipline — completion, not redesign

Three rules govern every stage below:

1. **No new SSBO struct layouts** except the tiny new `GpuDrivenBucketHeader` (4 GLuints). The thin-record layouts that exist today (`WaterThinRecord`, `TerrainQuadThinRecord`, and the PR2c MINE variant) are byte-stable across Phase C. The compute shader's pack body must produce byte-identical output to the legacy CPU pack body. This is enforced by the per-bucket parity check and is the load-bearing safety mechanism — if the compute shader's output ever diverges from CPU pack, the parity check fails before the bucket flips default-on.
2. **No new draw consumers** except the OVERLAY exception (Stage 4). Each bucket's MDI consumer is already in place (or, for water, becomes an MDI with one struct's worth of additional setup — see Stage 1 note). Phase C does not invent new render-state pipelines, new shaders for VS/FS, new sampler conventions, new depth-state. It just changes who writes the thin-record SSBO.
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

Tracy anchor: `GameCamera::render textureManagerRenderLists` at `gamecam.cpp:244-248`. Stages 2+3+4 collectively must drop ≥1.0 ms from Z1's current 1.35 ms mean. Stage 2 SOLID is expected to carry the bulk of that drop because SOLID has the highest per-frame thin-record count and the heaviest dedup churn (per `addLightDataStructure` at `txmmgr.cpp:1023` — 99% hit-rate dedup walk that Phase C bypasses by reading Phase 1's lighting SSBO directly).

**Deliverables:**
- `shaders/gpu_driven_terrain_solid.comp` (new) — cull/pack
- Reuse `gpu_driven_cmd_patch.comp` from Stage 1
- `gos_terrain_indirect.cpp` — `PackThinRecordsForFrame` early-returns when `MC2_GPU_DRIVEN_TERRAIN_SOLID=1`; new compute-dispatch path replaces.

**Parity gate:** byte-equality of thin-record SSBO + DAIC struct across tier1 5/5.

### Stage 3 — Terrain MINE (mechanical extension)

PR2c MINE shares enough of SOLID's structure that this stage is largely "extend Stage 2's compute shader with mine-specific recipe fields." Recipe layout is mine-specific but the cull/pack shape is identical.

### Stage 4 — Terrain OVERLAY

PR2b OVERLAY's `IsFrameOverlayArmed()` returns `false` unconditionally today (per `indirect_terrain_solid_endpoint.md`). Phase C ships it directly as GPU-driven, skipping the CPU intermediate that exists in design (`2026-05-08-pr2b-overlay-indirect-design.md`) but never shipped.

This is the **forward-construction** stage — there is no CPU baseline to parity against. The gate becomes:
- Visual canary at fixed seed/camera (no parity SSBO comparison possible)
- Tier1 5/5 visual identity vs current (`IsFrameOverlayArmed()=false` baseline, no overlay draws)
- Then a separate `MC2_GPU_DRIVEN_OVERLAY=1` flag flips it on

### Stage 5 — Soak window

7 days per Track B precedent. All four buckets soak with `_PARITY=1` running silently every Nth frame.

### Stage 6 — Per-bucket default-on flips (rolling)

Each bucket flips independently as it passes parity + soak. Order: water (lowest risk) → SOLID → MINE → OVERLAY.

### Stage 7 — Demote legacy CPU paths

Per-bucket CPU pack loops gated off (`MC2_GPU_DRIVEN_<BUCKET>=0`), NOT deleted. Deletion is a separate post-soak slice.

---

## Killswitches + env vars

Mirroring the prompt's "Killswitch + env vars" section, exact names:

- `MC2_GPU_DRIVEN=0` — global off, falls back to per-bucket CPU pack loops. Default-on after Stage 6 per-bucket flips clear.
- `MC2_GPU_DRIVEN_WATER=0`, `MC2_GPU_DRIVEN_TERRAIN_SOLID=0`, `MC2_GPU_DRIVEN_TERRAIN_MINE=0`, `MC2_GPU_DRIVEN_OVERLAY=0` — per-bucket killswitches. Allow bisection.
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
| D-1 | `code/objmgr.cpp:1939-2050` per-object update loop (prompt's reference) | D | needs re-grep at adversarial-review time — prompt cites range from an older commit, not verified at write-time |
| D-2 | sibling worktree `pre-bake-terrain` HEAD | M | `5667023` matches `nifty-mendeleev` — Phase B not yet shipped |
| D-3 | Phase 1 (terrain lighting) shipped commits `594add9 / eda2431 / ff8de07 / ff35f03` | M | git log shows all four, last is "DEFAULT-ON flip" |
| E-1 | `quadlist_is_camera_windowed.md` constraint | M | memory present, dated 2026-04-30 |
| E-2 | `water_ssbo_pattern.md` skip set (#1-#5) | M | memory present, 10 days old (annotated as stale-warning by harness; concepts unchanged but file:lines need fresh grep at Stage 1 plan-write time) |
| E-3 | `mc2_texture_handle_is_live.md` slot-not-handle rule | M | memory present, 16 days old (concepts unchanged; latest exception is Phase A bindless plan deferred — captured here) |
| E-4 | `cpp_glsl_ubo_struct_lockstep.md` rule | M | memory present, 8 days old; mc2_24 crash 2026-05-02 origin |
| E-5 | `substrate_coalesce_sync_point_lesson.md` rule | M | most recent (today, 2026-05-11) — origin commit mc2_10 62→128 fps |
| F-1 | indirect-cmd schema unchanged from PR1 (load-bearing) | M | `DrawArraysIndirectCommand` is GL-spec-mandated 16 B / 4 GLuints; no Phase C field needed |
| F-2 | binding-point allocations in §"Per-bucket binding-point allocation" | D | TENTATIVE table values 17-27; **MUST be re-grepped against shipped code at Stage 1 plan-write time** before any code lands; collisions resolved at adversarial-review |
| G-1 | `pr2_detail_overlay_mine_stage0_recon.md` PR2a-dead claim | M | memory present, references commit 521d83a |
| G-2 | `indirect_terrain_solid_endpoint.md` PR2b scaffold-only (`IsFrameOverlayArmed()` returns false) | M | memory present, 2026-05-01; needs Stage 4 fresh grep before forward-construction code |
| H-1 | `mech_vehicle_gpu_pull_in.md` Track D merged 2026-05-10 commit `0d5ce93` | M | matches git log "merge: pull in Track D GPU mech+vehicle batcher from claude/gpu-mech-batcher" |
| I-1 | `code/gamecam.cpp:244` Tracy zone `"GameCamera::render textureManagerRenderLists"` | M | exact label at line 244; wraps `mcTextureManager->renderLists()` at :245 and `endFrameTexResolve()` at :246 (range :244–:248) |
| I-2 | `code/gamecam.cpp:217` Tracy zone `"GameCamera::render water"` | M | exact label at line 217; wraps `land->renderWater()` at :218. When `MC2_RENDER_WATER_FASTPATH` armed (default-on), legacy `renderWater` early-returns; live cost is in the next entry. |
| I-3 | `code/gamecam.cpp:255-256` Tracy zone `"GameCamera::render waterFastPath"` | M | wraps `land->renderWaterFastPath()` at :256; this is the active water cost when fast-path is armed |
| I-4 | `code/gamecam.cpp:212` Tracy zone `"GameCamera::render objects"` | M | exact label at line 212; wraps `ObjectManager->render(true, true, true)` at :213 |
| I-5 | `code/mission.cpp:527` Tracy zone `"GameLogic.Mission.TextureManager"` wrapping `mcTextureManager->update()` | D | user-cited `:526` is the guarding `if (!isPaused() \|\| MPlayer)`; the actual Tracy zone + call are at `:527`. Pause-guard at :526 is load-bearing per `pause_unpause_diagnostic_for_static_render_bugs.md`. |
| I-6 | `mclib/txmmgr.cpp:1023` `addLightDataStructure(...)` function declaration | D | user-cited `:1022` is the closing brace of the prior helper; function declaration is at `:1023`. Tracy zone `"addLightDataStructure scan"` is at `:1028`. The 99% hit-rate hash-dedup walk on a 256-entry map is the load-bearing sub-zone within Z1. |

**Status note on D-1:** the prompt cites `code/objmgr.cpp:1939-2050` for "per-object update loop." This citation came from an older commit and was not verified against current HEAD at this design doc's write-time. **Action:** the adversarial-review pass MUST re-grep this range and either confirm or update the citation. Phase C does NOT depend on this range (the per-object update loop is Track-D scope), so this is informational drift, not a design hazard.

**Status note on F-2:** the binding-point table is the most likely site of an adversarial-review CRITICAL finding. Phase C MUST re-grep `binding = N` against the worktree at Stage 1 plan-write time and produce a no-collision proof. The shape of the table — one block per bucket — is the load-bearing architectural decision; the specific numbers are mechanical.

---

## What this design doc does NOT decide

- **The exact compute shader pack-loop body per bucket.** That's Stage 1 plan-write work. The pack body is constrained to byte-match the legacy CPU pack body's output; the parity check enforces this mechanically.
- **The exact texture-binding sequence for OVERLAY (Stage 4).** Forward-construction, no CPU baseline. Stage 4's plan will derive this from PR2b's existing design doc (`2026-05-08-pr2b-overlay-indirect-design.md`) and from Stage 2's MDI bridge.
- **Whether Phase C's compute should also subsume the projectZ pre-cull for the water bucket's static-prop-passing surfaces.** Out of scope for v1; the water compute shader does its own per-quad projectZ.
- **Track D's eventual integration.** Deferred to Phase C v2 per the inventory above.

---

## Open questions for adversarial review

These are the specific spots where the design doc is least confident, surfaced explicitly so adversarial review can hit them first:

1. **Beta-pattern second-dispatch overhead.** The "1-invocation patch dispatch" pays a `glDispatchCompute(1, 1, 1)` cost (~5 µs) per bucket. Is there a single-dispatch alternative that produces the same result via a deterministic last-workgroup pattern? Decision in this doc says no (no global barrier in GL 4.3 compute), but the AMD driver may permit specific patterns that work in practice.
2. **Per-bucket vs shared compute program.** Should there be one compute program per bucket (4 programs in v1), or one program with a per-dispatch uniform selecting the bucket's recipe binding? This doc assumes per-bucket (simpler debugging, no uniform-branch GPU cost). Reviewer should challenge if the shared-program path saves enough binding-point pressure to be worth it.
3. **Indirect-cmd struct write timing.** The Beta-pattern says "second dispatch writes the cmd." An alternative is to have every invocation write `count = visibleCount * 6` to `cmds[0]` (read-modify-write race resolved by atomicMax). Decision in this doc says single-invocation write is cleaner; reviewer should challenge the cleanliness vs simplicity tradeoff.
4. **Phase B coordination contract.** When Phase B ships its static-recipe SSBO, Phase C's compute shader changes binding R0 from "today's per-bucket recipe SSBO" to "Phase B's pre-bake SSBO." Is the layout guaranteed compatible? This doc says yes by construction (both are mission-static per-element data indexed by `vertexNum`), but Phase B's design doc may force a layout that requires a Phase C schema bump.
