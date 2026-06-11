# Baseline A — post-8z frozen render/perf reference

**Stamp:** post-8z, pre-GlStateGuard, pre-visual-lanes
**Branch:** `claude/nifty-mendeleev`
**nifty HEAD:** `034c3176` (`docs(terrain): record 8z production closeout milestone`)
**Build captured:** `A:/Games/mc2-opengl/mc2-win64-0.4c` — verified terrain-closeout build (game `mc2.exe` mtime 2026-06-10 21:06, `Mission Editor.exe` 21:14). State dump reports `build.config=Release`, `build.commit=unknown` (commit string not stamped in this build).
**Captured:** 2026-06-10, headless, `mc2.exe --profile stock --mission <stem> --duration 30` with `MC2_SMOKE_MODE=1 MC2_FASTPATH_DROP_LOG=1 MC2_DEBUG_STATE_DUMP=1 MC2_DEBUG_STATE_DUMP_HISTORY=1`. Logs + per-mission state JSON archived at `.claude/baseline-A-logs/`.

This is the frozen reference that gates opening the modernization backlog. The game terrain renderer is **chunk/GPU-only** (8z closeout): `setupTextures` is editor-gated, `slimReduce` + `MC2_TERRAIN_ACTIVE_AB` are deleted. Re-run any future engine change against these numbers before merging.

---

## 1. Summary table (tier1, 30s headless each)

| Mission | Result | Frames | Avg FPS | FATAL | FASTPATH_DROP | Terrain path | Parity | slimVerts |
|---------|--------|--------|---------|-------|---------------|--------------|--------|-----------|
| mc2_01  | pass   | 3284   | 109.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_03  | pass   | 3825   | 127.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_10  | pass   | 3924   | 130.7   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_17  | pass   | 3948   | 131.5   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |
| mc2_24  | pass   | 3944   | 131.4   | 0     | 0             | gpu (armed)  | 4 MATCH / 0 MISMATCH | 0 |

FPS = `frames / 30.02s`. These are **idle fly-through** smoke runs (passive mode, no AI/weapon-fire load) — they are a renderer-floor reference, not an interactive-gameplay benchmark.

---

## 2. Golden frames (fixed-frame state captures)

State dumps every 300 frames; the frame-3000/3600 (`latest_render_state.json`) snapshot is the golden frame per mission, archived as `.claude/baseline-A-logs/<mission>.state.json`. Invariant fields, identical across all 5 missions:

- `schema=MC2_DEBUG_STATE_V1`, `renderSnapshot.ok=true`, `arenaOverflow=false`.
- **engineView:** `MainScene` Visual, viewport `[0,0,1920,1080]`, viewUniformsBinding 3.
- **registeredViews:** MainScene 1920×1080 · ShadowDirectional0-Static 4096×4096 · ShadowDynamic 4096×4096 (all valid).
- **renderPasses:** shadow=true, screenShadow=true, bloom=false, fxaa=false, tonemap=false.
- **feature gates:** `MC2_VIEW_UNIFORMS`, `MC2_SNAPSHOT_STATIC_PROP_BUILD`, `MC2_MATERIAL_GPU(+SAMPLE)`, `MC2_STATIC_PROP_IBL_SH`, `MC2_QUADSETUP_ARMED_SKIP`, `MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE` = ON; `MC2_STATIC_PROP_PBR_V1` = OFF.
- **staticPropOpaque:** `snapshotDispatchDefault=true`, `legacyDispatch=false`, shaderVariant `snapshot+materialGpu+sample+iblSh`, materialGpu table=34 / inventory=34, iblSh strength 0.5, pbr OFF (roughness override 0.95).
- **renderResources:** MainDepth Depth24 1920×1080; ShadowStaticMap + ShadowDynamicMap Depth24 4096×4096 (64 MB each); TerrainHeightTexture R32F 100×100; WaterReflection 480×270.

mc2_01 golden frame is reproduced in full at `.claude/baseline-A-logs/mc2_01.state.json` (frame=3000).

---

## 3. FASTPATH_DROP = 0 (terrain stays armed, no legacy fallback)

`[FASTPATH_DROP]` count = **0** in every mission's stdout+stderr. Confirmed armed-state signals per mission:

- `[TERRAIN_INDIRECT v1] event=first_arm path=gpu` — terrain came up on the GPU indirect path (nodeIds 19–109, atlasTex 99–258 per mission).
- `[QUADSETUP_SKIP v1] fullyArmed=1 skip=1 quadsSkipped=0` — legacy quad setup skipped, fully armed.
- `[TerrainLOD prod] … slimVerts=0` every dump — legacy `slimReduce` is gone (chunk producer is sole path), confirming the 8z deletion at runtime.
- `[TerrainLOD parity] sample[0..3] … MATCH` (4/4, 0 mismatch) — chunk world-pos output matches the legacy reference at the four probe cells.

No legacy terrain fallback occurred in any run.

---

## 4. Terrain / path snapshot + frame counts

| Mission | first_arm nodeIds | atlasTex | objBlocks (last) | objVerts | solidWindow | TerrainLOD flush cmds |
|---------|-------------------|----------|------------------|----------|-------------|----------------------|
| mc2_01  | 65  | 129 | 20 | 8000  | 8000  | 12–20 |
| mc2_03  | 35  | 132 | 24 | 9600  | 9600  | ~18   |
| mc2_10  | 19  | 99  | 31 | 12400 | 12400 | ~18   |
| mc2_17  | 38  | 107 | 25 | 10000 | 10000 | ~18   |
| mc2_24  | 109 | 258 | 24 | 9600  | 9600  | ~18   |

`objVerts == solidWindow` every dump (chunk producer writes both from the same active set; no stale window). Frame counts in §1.

---

## 5. Oracle counters (all clean)

Per-mission, end-of-run / last-dump values:

| Mission | RENDER_SNAPSHOT fallback | TEX_RESOLVE mismatches / oob | MECH_MATERIAL_GPU mismatches | OBJBATCHER cpu_fallback (rate) | OBJBATCHER submit_legacy |
|---------|--------------------------|------------------------------|------------------------------|--------------------------------|--------------------------|
| mc2_01  | 0 (using_snapshot=1) | 0 / 0 (213433 resolves) | 0 | 1456 (0.0005) | 0 |
| mc2_03  | 0 | 0 / 0 (133911) | 0 | 0 (0.0000) | 0 |
| mc2_10  | 0 | 0 / 0 (83538)  | 0 | 0 (0.0000) | 0 |
| mc2_17  | 0 | 0 / 0 (150029) | 0 | 0 (0.0000) | 0 |
| mc2_24  | 0 | 0 / 0 (429910) | 0 | 3941 (0.0007) | 0 |

- **RENDER_SNAPSHOT v3:** `fallback=0`, `count/pkt/meta_mismatch=0`, `using_snapshot=1` every 600-frame checkpoint. Static-prop snapshot path authoritative, no fallback.
- **TEX_RESOLVE v1:** `mismatches=0 oob=0` at shutdown, all missions.
- **MECH_MATERIAL_GPU v1:** `mismatches=0` every compare (mechs 12–84 per mission).
- **OBJBATCHER v1:** `gpu_drawn_instances=0`, `submit_legacy=0`, `submit_trees=0` everywhere. **`cpu_fallback` is non-zero on mc2_01 (1456) and mc2_24 (3941)** — but in both, `cpu_fallback == late_register_recovery_skips` and `fallback_rate ≤ 0.0007`. These are **late-register recovery skips** (e.g. the `compass` HUD node registering after the batch window), **not** GPU-cull CPU fallback of real geometry. Recorded honestly as a non-zero baseline value to watch, not a regression.
- **GPU_CULL v1:** substrate ready every frame (records 2.6k–2.9k / capacity 12295), `indirect_draw overflow=0`, `submit flush=600`. Clean shutdown (substrate/compute/readback).

---

## 6. Per-pass timings — LIMITATION (honest note)

**Tracy per-pass GPU zones (terrain solid / shadow / 3D objects / post / present) are NOT captured in this baseline.** Tracy is compiled in (`TRACY_ENABLE`) but requires a live Tracy profiler client attached to a windowed session; the headless smoke harness has no profiler connection and the state-dump JSON exposes no per-pass timer. Per-pass GPU timings must be captured in a separate **interactive** session (windowed `mc2.exe` + Radeon Developer Panel / Tracy) and appended here before they can gate timing-sensitive lanes.

The only per-pass timing signal exposed by the headless dump/log is the GPU-cull indirect submit cost:

| Mission | `GPU_CULL indirect_draw elapsed_us` (typical) |
|---------|-----------------------------------------------|
| mc2_01  | 11–13 |
| mc2_03  | 11–13 |
| mc2_10  | 11–14 |
| mc2_17  | 12–13 |
| mc2_24  | 11    |

`buckets=374, overflow=0, flush=600` — terrain/static indirect draw submit is ~11–14 µs CPU. This is a CPU-submit proxy, **not** a GPU pass time. Frame-count avg FPS (§1) is the only whole-frame timing reference in this baseline.

---

## Gate

Baseline A captured. Tier1 5/5 pass, FATAL=0, FASTPATH_DROP=0, all correctness oracles clean (terrain chunk-only + armed, parity MATCH), OBJBATCHER late-register skips recorded as known non-zero. **Per-pass GPU timings remain OPEN** (headless limitation) and must be filled from an interactive Tracy/RGP session. With that caveat noted, this freezes the post-8z reference; the modernization backlog (GlStateGuard, visual lanes, Tube merge) may open against it.
