# Phase D: `quadSetupTextures` Residual Retirement (GameLogic-side, Phase 1 continuation)

> **Model: opus.** Reason: re-opens the Phase 1 design doc's deferred Phase 2 (water vertex projection GPU port), which was blocked on the multi-source `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction cross-cutting concern. The cross-cutting question needs architectural judgment (joint port vs parallel CPU vs scope reduction). Also reopens the per-quad function-call dispatch overhead question. Opus for spec + adversarial review; sonnet for mechanical implementation stages.

> **Required sub-skills:** `superpowers:using-git-worktrees`, `superpowers:writing-plans`, `adversarial-plan-review` (this slice qualifies — touches Phase 1's already-shipped infrastructure, reopens deferred Phase 2 multi-source reduction, may introduce new SSBO schemas, cross-cuts with Track C compute cull lifecycle gates).

> **Sibling slices** (run on independent branches in parallel):
> - Phase B (pre-bake terrain renderer) — render-side; both touch `terrain.cpp` but at different line ranges and different zones. Coordination point: Phase B reads per-mission static data; Phase D writes per-frame results. Phase 1's already-committed `gos_terrain_lighting` module is shared infrastructure both can reference.
> - Phase C (GPU-driven indirect cmds) — render-side; orthogonal to Phase D.
> - Phase A (bindless textures) — DEFERRED, see `2026-05-11-bindless-textures-prompt.md`.
> - gpu-mech branch — separate; GPU mech skinning is complete on that branch per user. Phase D is terrain-only and doesn't touch mechs.
>
> **Cross-branch coordination point:** Phase D's water-vertex-projection sub-slice modifies `leastZ`/`mostZ`/etc. writer set. If gpu-mech branch also reads these globals (unlikely but possible — the projection consumer is `eye->setInverseProject` for view-frustum scaling), the merge contract for Phase D needs a Stage 0 cross-branch grep.

---

## Worktree

Create a fresh worktree off `claude/nifty-mendeleev` HEAD (currently `5667023` at write-time of this prompt — verify at execution).

```
.claude/worktrees/phase-d-quadsetup-residual/  → branch claude/phase-d-quadsetup-residual
```

Use the `using-git-worktrees` skill.

## Roadmap reference

This slice is **Phase D** in the post-Phase-1 retirement landscape:

| Slice | Zone | Status | Target |
|---|---|---|---:|
| Phase 1 (shipped) | `quadSetupTextures` lighting | ✅ default-on (ff35f03) | retired |
| Phase B (in flight) | `Terrain::render drawPass` | session active | ≤0.3 ms |
| Phase C (in flight) | `render textureManager` + `water` + `objects` | session active | ≤0.5 ms |
| **Phase D (this slice)** | **`quadSetupTextures` residual** | **starting now** | **≤0.5 ms** |

Parent CPU→GPU offload arc: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`.

Phase 1's already-committed design doc: `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md`. Read its **Q4 — Multi-source `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction** section verbatim — that's the blocker Phase D inherits.

Phase 1's plan: `docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md`. Its "Phase 2 — water vertex projection" section was outlined but deferred. Phase D incorporates that outline plus the other Phase 1 Slice 0 residual targets.

Phase 1's Slice 0 cost-split commit: `4fa7a9a` — recon infrastructure (8 buckets in `gos_terrain_indirect.{h,cpp}`) is still in the tree, env-gated via `MC2_TERRAIN_COST_SPLIT=1`. Phase D's Stage 0 reuses this for re-measurement.

## Goal

Retire the **~1.87 ms residual** in `Terrain::geometry quadSetupTextures` left after Phase 1's GPU lighting port. The Phase 1 Slice 0 cost-split decomposition named these candidate buckets (cost-split-instrumented values inflated by chrono overhead; production values are smaller but proportions roughly hold):

| Sub-bucket inside `quadSetupTextures` | Slice-0-instrumented cost | Real production share (estimated) | Status |
|---|---:|---:|---|
| `lighting` | 5,177 µs | 0 µs | ✅ Phase 1 retired |
| `water_vert_proj` | 911 µs | ~250-400 µs | NOT retired (deferred Phase 2) |
| `visibility_check` (`isTerrainQuadVisible × ~14K`) | 890 µs | ~250-350 µs | NOT retired |
| `recipe_cache` (Shape C lookup + addTerrainTriangles) | 656 µs | ~180-250 µs | NOT retired |
| `cache_resident` (cache fetch + residency check) | 269 µs | ~75-100 µs | NOT retired |
| `detail_overlay_branch` | 201 µs | ~55-80 µs | NOT retired |
| dispatch + branch overhead (per-quad function-call cost × ~14K) | 3,767 µs | ~600-900 µs | NOT retired |

**Caveat:** the Slice-0 cost-split instrumentation itself adds ~50-100 ns per scope entry/exit × 14K calls × 8 buckets = ~10 ms of overhead when active. Production (no `MC2_TERRAIN_COST_SPLIT`) is ~1.87 ms total. The proportions above are the BEST we have without re-instrumenting at lower overhead. **Phase D Stage 0 must re-measure with lower-overhead instrumentation** before committing to a target ordering.

Expected cut after Phase D: `quadSetupTextures` mean drops from 1.87 ms to ~0.3-0.5 ms. Combined with Phase 1's 5.4 ms cut, total `quadSetupTextures` retirement: ~6-7 ms from a pre-Phase-1 baseline of 7.24 ms mean.

## What to read first (in order)

1. **`docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md`** — Phase 1's design doc. Q4 (multi-source reduction) is the load-bearing inherited blocker. Q5 (lifecycle hooks) shows the pattern Phase D extends. Q6 (flag-bit layout in `GpuTerrainVertexInput`) already reserved bits 4-7 for Phase 2 water — Phase D inherits this.
2. **`docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md`** — Phase 1's plan. The "Phase 2 — water vertex projection" outline section is Phase D's starting point for the water sub-slice.
3. **`memory/vertex_project_loop_d1_asymptotic.md`** — D1 CPU hoist for `vertexProjectLoop` is asymptotic; CPU-only path is near the scalar ceiling. The non-water reduction writers at `terrain.cpp:1549-1552` and `terrain.cpp:1698-1715` (per Phase 1 design doc Q4) are inside `vertexProjectLoop`. Joint-port option (a) of Q4 ports `vertexProjectLoop` too.
4. **`memory/water_ssbo_pattern.md`** — the canonical "static recipe + per-frame thin record" pattern. Water projection is a direct candidate.
5. **`memory/cull_gates_are_load_bearing.md`** — `objBlockInfo[].active` and `objVertexActive[]` writes (set by `vertexProjectLoop` per Phase 1 brainstorm Q3-C1). If joint-port option (a) is chosen, these writes move to GPU and require atomicity discipline.
6. **`memory/track_c_compute_cull.md`** — Track C's GPU visibility decisions. Phase D's `visibility_check` sub-slice may be able to reuse Track C's output instead of computing visibility CPU-side.
7. **`memory/substrate_coalesce_sync_point_lesson.md`** — sync stall pattern to avoid for any GPU readback Phase D might need.
8. **`GameOS/gameos/gos_terrain_lighting.{h,cpp}`** — Phase 1's shipped infrastructure (3-slot ring SSBO, `tl_*` compile helpers, `tryConsume` non-blocking pattern). Phase D extends this OR copies the pattern.
9. **`mclib/quad.cpp:670-1891`** — `TerrainQuad::setupTextures` body. Walk it once; identify the residual sub-task boundaries post-Phase-1.
10. **`mclib/terrain.cpp:1341-1343` + `:1382-1384` + `:1549-1552` + `:1696-1715` + `:1832`** — the multi-source reduction state declared/reset/written/consumed. Re-grep at write-time; line numbers drift.

## Scope

**In:**
- Stage 0 recon: lower-overhead re-measurement of `quadSetupTextures` residual sub-task breakdown (alternative to per-call chrono — use Tracy GPU-style averaging, RDTSC, or per-frame sampling).
- Sub-slice D1: water-vertex-projection GPU port (continuation of Phase 1's Phase 2). Includes the multi-source reduction architectural decision from Phase 1 design Q4 (options a/b/c).
- Sub-slice D2: visibility-check optimization. Options: (a) GPU port via Track C compute cull integration, (b) SIMD/SoA the existing check, (c) batch-cull pre-pass that produces a "needs-update" set.
- Sub-slice D3: recipe-cache + cache-resident retirement or optimization. Likely small-ish standalone win; may be folded into D1 or D2.
- Sub-slice D4: per-quad dispatch/branch overhead reduction. May involve inlining, batching, or refactoring `setupTextures` into smaller per-task functions called from a fast outer loop.

**Out:**
- Anything outside `quadSetupTextures` (Phase B/C territory; Track C territory).
- `addLightDataStructure` call-volume reduction (separate slice; lives in render-side `mcTextureManager`).
- Mech update / Mech3DAppearance (gpu-mech branch).
- Static-prop update (Track B+ static_update_skip already shipped).
- Vulkan/D3D12 migration.

## Plan shape (suggested — spec session owns final)

1. **Stage 0 (recon)**: re-instrument `quadSetupTextures` with lower-overhead measurement. Smoke mc2_10 wolfman + tier1 5/5 to capture production-state residual breakdown. Identify the dominant sub-bucket(s). Compare against Phase 1 Slice 0 ratios — if production data deviates significantly, that informs which sub-slice to prioritize.
2. **Stage 0.5 (architectural decision)**: design doc for multi-source reduction question (Phase 1 design Q4 options a/b/c). Adversarial review gate. Cannot proceed to D1 without resolving this.
3. **Stage 1 — D1 (water-vertex-projection GPU port)**: SSBO + compute shader + parity gate. Reuses Phase 1's `gos_terrain_lighting` infrastructure (3-slot ring, `tl_*` helpers, lifecycle hooks) OR factors a shared compute-helper module if Phase B/C have similar needs.
4. **Stage 2 — D2 (visibility check)**: depending on Stage 0 finding. If high cost: GPU port via Track C integration. If medium: SIMD/SoA. If small: defer.
5. **Stage 3 — D3 (recipe cache + cache_resident)**: smaller per-sub-task work; may be folded into D1/D2 if cost is small.
6. **Stage 4 — D4 (dispatch overhead)**: requires understanding from Stage 0 — if dispatch overhead is the dominant residual (>50% of remaining 1.87 ms), refactor `setupTextures` shape; if minor, defer.
7. **Stage 5 — soak**: 7-day per Track B precedent.
8. **Stage 6 — default-on flip**: per-sub-slice gates can flip independently.

## Parity / Soak gates

- Per-sub-slice parity gate: `MC2_TERRAIN_LIGHTING_PARITY=1`-style schema, comparator covers the output state for each sub-slice (per-vertex water projection state, per-quad visibility result, per-quad recipe).
- Tracy `Terrain::geometry quadSetupTextures` mean ≥ **1.0 ms cut** vs pre-Phase-D baseline (1.87 ms → ≤0.87 ms or better). Sigma compression also tracked.
- Tier1 5/5 PASS both env states + visual identical via screenshot diff.
- No new GL errors.
- 7-day soak under default-on per sub-slice.

## Killswitch + env vars

- `MC2_TERRAIN_WATER_PROJ_GPU=0` — D1 killswitch.
- `MC2_TERRAIN_WATER_PROJ_PARITY=1` — D1 parity check.
- `MC2_TERRAIN_VIS_CHECK_GPU=0` — D2 killswitch (if D2 is GPU-port shape).
- `MC2_TERRAIN_RECIPE_FAST=0` — D3 killswitch.
- (D4 specific env TBD by spec session.)
- All env reads are boot-cached for zero-overhead-when-disabled discipline.

## Load-bearing constraints (per adversarial-plan-review skill step 6)

- **Phase 1 design doc Q4 multi-source reduction**: `leastZ/mostZ/leastW/mostW/leastWY/mostWY` are written by water-projection block (quad.cpp) AND non-water terrain projection (terrain.cpp:1549) AND legacy fallback (terrain.cpp:1696). Consumer at terrain.cpp:1832. D1 cannot port water-projection writers in isolation — Stage 0.5 picks option (a) joint port, (b) parallel CPU reduction, or (c) D1 defers further. **Re-grep all four sites at write-time per Phase 1 design appendix.**
- **`memory/cull_gates_are_load_bearing.md`**: option (a) joint port moves `objBlockInfo[].active` + `objVertexActive[]` writes to GPU. Atomicity discipline required (atomic-relaxed boolean writes).
- **`memory/water_ssbo_pattern.md`**: D1 follows the canonical pattern; reuse Phase 1's infrastructure where possible.
- **`memory/substrate_coalesce_sync_point_lesson.md`**: no `glGetBufferSubData` / `glMapBuffer(GL_MAP_READ_BIT)` on hot path. Use Phase 1's `gpu_cull_readback`-style 3-slot non-blocking ring.
- **`memory/cpp_glsl_ubo_struct_lockstep.md`**: any new SSBO struct must be lockstep-defined.
- **Phase 1's `gos_terrain_lighting` module is ALREADY in the tree** — D1 either extends it (cleanest reuse) or builds a sibling module. If extending: rename module to `gos_terrain_compute` and grow it to cover water + lighting. If sibling: create `gos_terrain_water_proj.{h,cpp}` and copy the ring pattern privately.
- **`memory/quadlist_is_camera_windowed.md`**: per-frame quadList walk vs map-stable `vertexNum` indexing. D1's compute shader walks by `vertexNum`, not quadList slot (Phase 1 already established this pattern).

## Adversarial review gate (mandatory)

Run `adversarial-plan-review` skill against Stage 0/0.5 design doc before any code lands. Triggers:
- Reopens Phase 1's deferred Phase 2 architectural question (multi-source reduction).
- Touches Phase 1's already-shipped infrastructure (`gos_terrain_lighting` module).
- May introduce new SSBO schemas (water-projection output + recipe-cache output).
- Cross-cutting with Track C compute cull (if D2 uses Track C visibility output).
- Cross-cutting with Phase B + Phase C (terrain CPU footprint shared concern; ensure no scope overlap).
- Perf gate ≥1.0 ms.

Dispatch prompt MUST include "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

## Exit criteria

- All Parity/Soak gates pass for each shipped sub-slice.
- `MC2_TERRAIN_WATER_PROJ_GPU=0` (and analogous per-sub-slice killswitches) reproduce pre-Phase-D behavior bit-for-bit.
- `quadSetupTextures` mean drops from ~1.87 ms to ≤0.5 ms post-Phase-D.
- Phase 1's `gos_terrain_lighting` module either cleanly extended (with retitled identity if scope-grown) or sibling module shipped with same API shape.
- Memory file `terrain_compute_phase_d.md` captures: (a) Stage 0 production-state residual breakdown, (b) chosen multi-source reduction option from Q4, (c) per-sub-slice architectural decisions.
- Cross-branch ABI audit notes any merge concerns for gpu-mech branch.

## Stop conditions

- Stage 0 re-measurement shows residual is dominated by **dispatch overhead** (>70% of remaining 1.87 ms is per-call function entry/exit, not the named buckets) → STOP, surface to user. Dispatch overhead is hard to GPU-port; would require refactoring `setupTextures` shape entirely (smaller functions called from a fast outer loop).
- Multi-source reduction option chosen in Stage 0.5 turns out to be option (c) "defer D1 indefinitely" → STOP D1, focus Phase D on D2/D3/D4 only.
- D1 parity won't converge after 3 iteration rounds → STOP D1, debug, do not proceed to default-on.
- Any tier1 mission FAIL under any sub-slice's env=1 → STOP that sub-slice, revert to parity-only mode, bisect.
- Per-sub-slice Tracy cut < 200 µs → STOP that sub-slice (below ROI threshold), surface to user.

## Why opus

This slice:
- Reopens an architectural question deferred from Phase 1 (multi-source reduction is genuinely cross-cutting and requires judgment).
- Touches Phase 1's already-shipped infrastructure — design decisions here affect the gos_terrain_lighting module's eventual shape.
- Sub-slice prioritization depends on Stage 0 re-measurement; opus is needed to interpret the data + pick the highest-ROI lever.
- Cross-cutting with Phase B/C/Track C — coordination decisions are architectural.
- Adversarial review needs deep reasoning about the multi-source reduction options.

Opus for spec + adversarial review + Stage 0.5 architectural decision. Sonnet for per-sub-slice mechanical implementation.

---

## One specific clarification for the Phase D session

The Phase 1 Slice 0 cost-split numbers (`water_vert_proj 911 µs`, `visibility_check 890 µs`, etc.) are **inflated by chrono instrumentation overhead** (~50-100 ns per scope ctor/dtor × 14K calls × 8 buckets). Production-state `quadSetupTextures` is **~1.87 ms total** (user's env-ON Tracy at mc2_10 wolfman, post-Phase-1 measurement). The Slice 0 ratios are still useful for prioritization — they tell us *which sub-task is biggest* — but the absolute numbers are inflated.

Stage 0 of Phase D **must** re-measure with lower-overhead instrumentation before committing to sub-slice priorities. Options:
1. Replace `chrono::steady_clock::now()` with RDTSC (`__rdtsc()` intrinsic, ~5-10 ns per call vs 50-100 ns).
2. Sample-based: instrument only every Nth call (e.g., every 100th setupTextures invocation), extrapolate.
3. Per-frame totals only: replace 8 per-scope timers with one outer timer + per-bucket counters; multiply counter × known-per-call-cost-from-microbench.

Option 1 is cleanest and matches the precedent of Tracy's own internal timing. Option 3 is most truthful to Phase 1's discovery but requires a separate microbench step.

Whichever Stage 0 uses, the goal is a production-state breakdown that fits within the actual 1.87 ms, not the instrumentation-inflated 6.7 ms.
