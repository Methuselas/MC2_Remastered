# Renderer Modernization — Phase B: Pre-Bake Terrain Renderer

> **Model: opus.** Reason: introduces per-mission bake + per-frame delta-update split for terrain rendering. Designs the "what's map-stable vs camera-windowed" boundary. Spec touches `Terrain::primeMissionTerrainCache` lifecycle, persistent SSBOs, and the renderLists() flush ABI. After spec ships and adversarial review clears, mechanical implementation stages pass to sonnet.

> **Required sub-skills:** `superpowers:using-git-worktrees`, `superpowers:writing-plans`, `adversarial-plan-review` (this slice qualifies — touches terrain rendering pipeline, retires the per-frame quadList walk in `Terrain::render`, introduces map-stable SSBO that survives camera motion).

> **PREREQUISITE: Phase A (bindless textures) must ship default-on before this slice opens.** Phase B's static SSBO records bindless handles, not legacy slot indices. Without Phase A, this slice has to either (a) bake legacy slot indices and re-bake when bindless ships (wasted work) or (b) wait for Phase A.
>
> **Sibling slice** (independent, can run in parallel post-Phase-A): Phase C at `2026-05-11-gpu-driven-rendering-prompt.md`. Phase B and Phase C operate on mostly-orthogonal data: B = static bake of terrain mesh + textures; C = dynamic per-frame indirect-cmd generation. Coordination point: both write into the terrain indirect-cmd SSBO. Stage 0 must decide whether C's per-frame writes layer on top of B's bake (B writes templates, C fills in per-frame deltas) or whether C supplants B for the dynamic portion.

---

## Worktree

Create a fresh worktree off post-Phase-A main (likely `claude/nifty-mendeleev` after Phase A merges). Verify Phase A's commit chain is in the base before branching.

```
.claude/worktrees/pre-bake-terrain/  → branch claude/pre-bake-terrain
```

## Roadmap reference

Phase B of the renderer modernization tri-slice arc (bindless → pre-bake terrain || GPU-driven rendering).

Parent arc: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`.

Foundational reference: Phase 1 (terrain lighting GPU compute) on `claude/parallel-amdahl` shipped the pattern for per-mission lifecycle + per-frame consumer + parity gate. Reuse aggressively.

Track B (widen static prop registry, `memory/track_b_widen_static_prop_registry.md`) shipped the analogous "static data baked at mission load" pattern for props. Phase B applies the same shape to terrain.

## Goal

Retire the per-frame `Terrain::render` quadList walk (~2.6 ms on mc2_10 wolfman) by pre-baking the map-stable portions of terrain rendering at mission load. Per-frame work reduces to: update visibility mask + dispatch.

Expected cut: 2.0-2.5 ms/frame in the `GameCamera::render terrain` Tracy zone. Per-frame terrain CPU prep drops from ~2.6 ms to ~100-300 µs (just the visibility update + dispatch trigger).

The win does NOT depend on camera stationarity (RTS camera motion preserved). Per-frame work scales only with VISIBLE quads, not iterated quads — the difference is the camera-windowed subset.

## What to read first (in order)

1. **`memory/patchstream_shape_c.md`** — Shape C cache (mission-load terrain recipe bake). Phase B extends this pattern to the renderLists tri-list level.
2. **`memory/m2_thin_record_cpu_reduction_results.md`** — M2 fast path optimizations that brought `Terrain::render drawPass` from 25ms → 1.46ms (mc2_01) / 2.62ms (mc2_10). Phase B builds on top.
3. **`memory/water_ssbo_pattern.md`** — "static recipe + per-frame thin record + single draw post-renderLists" pattern from the water arc. Phase B applies same shape to terrain.
4. **`memory/indirect_terrain_solid_endpoint.md`** — PR1+PR2 indirect-terrain SOLID architecture. Already partially does what Phase B aims for; Phase B extends to ALL terrain draw paths.
5. **`memory/quadlist_is_camera_windowed.md`** — load-bearing constraint: quadList is camera-windowed each frame; map-stable indexing must use `vertexNum` not quadList slot.
6. **`memory/track_b_widen_static_prop_registry.md`** — analogous mission-load static-bake pattern for props.
7. **`GameOS/gameos/gos_terrain_indirect.{h,cpp}`** — current SOLID-PR1 implementation. Phase B extends to detail/overlay/mine paths too.
8. **`code/mission.cpp` `Mission::init`** + **`mclib/terrain.cpp:595 Terrain::primeMissionTerrainCache`** — mission-load chokepoints where the bake fires.
9. **Phase A's bindless-handle ABI** — committed memory file `bindless_handle_abi.md`. Phase B's static SSBO records bindless handles.
10. **Sibling Phase C prompt + design doc** — coordination point on terrain indirect-cmd SSBO writes.

## Scope

**In:**
- Per-mission bake of map-stable terrain data: per-quad recipe (texture handles, UV layout, vertex indices) baked once at `primeMissionTerrainCache`, lives in a persistent SSBO indexed by `vertexNum` (map-stable per `mapdata.cpp:1119`).
- Per-frame work reduces to: compute visibility mask (one uint per `realVerticesMapSide²`), dispatch indirect draws using mask as a stencil.
- All current terrain draw paths (SOLID, detail, overlay, water-terrain-tile, mines) baked into the static SSBO.
- Killswitch `MC2_TERRAIN_PREBAKE=0` opts out (falls back to per-frame quadList walk); default-on after soak.
- Parity gate `MC2_TERRAIN_PREBAKE_PARITY=1` compares per-frame indirect-cmd output between baked and walk paths.
- Cache invalidation hooks: `primeMissionTerrainCache` triggers full re-bake on mission load; `invalidateTerrainFaceCache(vertexNum)` already exists (per memory `patchstream_shape_c.md`) — extend to invalidate this layer too.

**Out:**
- Per-frame visibility compute on GPU — that's Phase C's territory (Phase B just consumes a visibility mask that Phase C produces, OR a CPU-computed mask if Phase C isn't shipped yet).
- Dynamic objects (mechs, vehicles, particles) — out of scope, gpu-mech branch handles mechs.
- Water animation (`frameCos`-driven vertex displacement) — orthogonal; existing WaterStream pattern handles per-frame water deltas. Phase B bakes only the STATIC portion of water (texture handles, UV layout); the per-frame projection stays on WaterStream's existing infrastructure (or Phase 2 GPU compute when it ships).

## Plan shape (suggested — spec session owns final)

1. Stage 0: spec + design doc. Define the static-SSBO struct layout (uses Phase A bindless handles). Define cache-invalidation contract. Adversarial review gate.
2. Stage 1: Build the per-mission baker (no consumer wired). `MC2_TERRAIN_PREBAKE_TRACE=1` prints bake stats at mission load.
3. Stage 2: Wire a parity-mode consumer — per-frame baked path runs ALONGSIDE legacy walk, comparator checks indirect-cmd output equality. Iterate until zero mismatches.
4. Stage 3: Consumer flip — baked path becomes authoritative when `MC2_TERRAIN_PREBAKE=1`. Legacy walk runs only under killswitch or parity mode.
5. Stage 4: Soak window (7-day per Track B precedent).
6. Stage 5: Default-on flip.
7. Stage 6: Demote legacy walk infrastructure (gated off, not deleted).

## Parity / Soak gates

- Per-frame indirect-cmd SSBO byte-equal under `MC2_TERRAIN_PREBAKE_PARITY=1` across tier1 5/5 + mc2_10 wolfman + mc2_01 water-heavy.
- Tracy `GameCamera::render terrain` mean drops ≥1.5 ms vs pre-Phase-B baseline.
- `lighting_ns_per_frame`-style retirement telemetry: existing per-frame quadList walk bucket drops to ~0 µs under default-on.
- Tier1 5/5 PASS both env states + visual identical via screenshot diff.
- No new GL_INVALID_* lines.

## Killswitch + env vars

- `MC2_TERRAIN_PREBAKE=0` — force legacy per-frame quadList walk. Default-on after Stage 5.
- `MC2_TERRAIN_PREBAKE_PARITY=1` — dual-run + comparator. Default off.
- `MC2_TERRAIN_PREBAKE_TRACE=1` — bake stats + per-frame stats. Default off.

## Load-bearing constraints (per adversarial-plan-review skill step 6)

- **`memory/quadlist_is_camera_windowed.md`**: quadList is rebuilt each frame by `makeLists`. Map-stable static SSBO MUST be indexed by `vertexNum`, not quadList slot.
- **`memory/cull_gates_are_load_bearing.md`**: `objBlockInfo[].active` and `objVertexActive[]` writes are made by `vertexProjectLoop`. Phase B can't bake "the visible set" — that's per-frame. Phase B bakes only the per-vertex render-state mapping; visibility comes from elsewhere (CPU or Phase C GPU).
- **`memory/cpp_glsl_ubo_struct_lockstep.md`**: static SSBO struct definition must be in a shared header.
- **`memory/stock_install_must_remain_playable.md`**: bake must succeed from stock terrain data. No bake step that requires modern sidecar data.
- **Phase A bindless-handle ABI**: static SSBO records bindless `uvec2` handles, not legacy slot indices. Re-grep `bindless_handle_abi.md` at write-time.
- **`memory/patchstream_shape_c.md`**: existing Shape C cache invalidates whole array on `setTerrain()`. Phase B's cache layer must match this invalidation semantics or document divergence.

## Adversarial review gate (mandatory)

Run `adversarial-plan-review` skill against Stage 0 design doc before code lands. Triggers:
- New SSBO schema (per-mission static terrain SSBO).
- Retires the per-frame quadList walk (load-bearing for legacy fallback).
- Cross-cutting cache invalidation contract (interaction with `patchstream_shape_c.md`).
- Perf gate ≥1.5 ms.

Dispatch prompt MUST include "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

## Exit criteria

- All Parity/Soak gates pass.
- `MC2_TERRAIN_PREBAKE=0` reproduces pre-slice behavior bit-for-bit.
- Memory file `terrain_prebake.md` captures static SSBO struct + invalidation contract + per-mission lifecycle.
- Phase C's GPU-driven indirect-cmd generation can layer on top cleanly (or supplant the per-frame visibility step if shipped after Phase C).

## Stop conditions

- Per-frame baked vs walk parity diff non-zero after 3 iteration rounds → STOP, surface findings.
- Per-frame Tracy delta < 1.5 ms → STOP, surface to user. The slice's value proposition is the cut.
- Cache invalidation correctness bug surfaces during soak → STOP, revert flip to parity-only mode, bisect.
- Map-load time regresses > 100 ms from the bake step → STOP, defer optimization to later slice or rethink bake scope.

## Why opus

This slice:
- Defines the per-mission vs per-frame data boundary for terrain — architectural decision.
- Cache invalidation contract is cross-cutting with `patchstream_shape_c.md` and Track C compute cull.
- Phase A and Phase C coordination points need careful design.
- Static SSBO struct layout is locked in by this slice for future slices to consume.

Opus for spec + adversarial review. Sonnet for Stage 1-3 mechanical implementation.
