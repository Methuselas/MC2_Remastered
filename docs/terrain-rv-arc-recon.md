# Terrain R→V Arc Recon (TERRAIN-ARC-RECON-0)

Pass-level analog to `static-prop-rv-arc-audit.md`. Maps the terrain
pipeline end-to-end so the Track R substrate gaps are visible before
any Track V (visual) work. **Docs-only artifact**, no code changes.

Worktree: `claude/nifty-mendeleev`. Recon HEAD: `f889b1b8`.
Pairs with [docs/engine-closure-audit.md](engine-closure-audit.md),
[docs/render-binding-registry.md](render-binding-registry.md),
[docs/renderpass-contract-spec.md](renderpass-contract-spec.md).

## TL;DR

Terrain is the **opposite** of StaticPropOpaque in substrate maturity:
- StaticPropOpaque: snapshot+DrawPacket+PipelineDesc+ViewUniforms all wired, PBR visual layer experimental.
- Terrain: legacy MVP+per-program uniforms+CPU dispatch, `TerrainPassFacts` exists but flag-only, **ViewUniforms NOT consumed** by terrain shaders, **NOT registered in PipelineDesc**, no DrawPacket. Visual layer mostly cosmetic (POM, normal blend) on top of a low-res 128 wu / vertex heightfield.

Terrain must adopt the lane template **at pass granularity** (not per-object). Object identity is explicitly deferred — there is no real consumer.

## 1. Terrain authority chain

| Stage | Authority | Files |
|---|---|---|
| Mission file (disk) | `PacketFile` binary; per-vertex `PostcompVertex` (elevation float, normal vec3, textureData dword, localRGBLight dword) | [mclib/vertex.h:32-47](../mclib/vertex.h), [mclib/mapdata.cpp:205-218](../mclib/mapdata.cpp) |
| Heightfield grid | `MapData` flat `blocks[]`, `realVerticesMapSide` = `sqrt(bytes / sizeof PostcompVertex)` — typical 120×120 ≈ 14.4k samples | [mclib/terrain.cpp:322](../mclib/terrain.cpp), [mclib/mapdata.cpp:209](../mclib/mapdata.cpp) |
| World scale | `worldUnitsPerVertex = 128.0f`, `MAPCELL_DIM = 3` → `worldUnitsPerCell ≈ 42.67`. Map extent ≈ 15,360 wu / side | [mclib/terrain.cpp:101-107](../mclib/terrain.cpp), [mclib/terrain.h:54](../mclib/terrain.h) |
| Tile mesh | `verticesBlockSide = 20` (fixed); 20×20 vertex tiles fed into legacy GL draws | [mclib/terrain.h:76-84](../mclib/terrain.h) |
| Render dispatch | `Terrain::render()` (~mclib/terrain.cpp:860) + GPU-driven indirect path (`gpu_driven_terrain_solid.comp`) via `gos_terrain_indirect.cpp` | [mclib/terrain.cpp](../mclib/terrain.cpp), [GameOS/gameos/gos_terrain_indirect.cpp](../GameOS/gameos/gos_terrain_indirect.cpp) |
| Bridge / program selection | `gos_terrain_bridge_*` (getMaterial, bindUniforms, drawPatchStreamBucket, drawSingleBucket, water variants) | [GameOS/gameos/gameos_graphics.cpp:1957-2094](../GameOS/gameos/gameos_graphics.cpp) |
| GL programs | `thin_terrain_prog_`, `terrain_surface_prog_`, `shadow_terrain_material_`, `water_fast_prog_` | [GameOS/gameos/gameos_graphics.cpp:1680](../GameOS/gameos/gameos_graphics.cpp), uniform-loc lookup 1792-1793 |
| Shaders | `gos_terrain.{vert,tesc,tese,frag}`, `shadow_terrain.{vert,frag}`, `gos_terrain_water_fast.vert` | [shaders/gos_terrain.frag](../shaders/gos_terrain.frag) |
| Gameplay height (authoritative) | `Terrain::getTerrainElevation(Vector3D)` — ~25 callers in code/ (gvehicl, ablmc2, bldng, gate) | [mclib/terrain.h:205](../mclib/terrain.h) |
| World→tile mapping | `worldToTile`, `worldToCell`, `worldToTileCell` | [mclib/terrain.h:335-375](../mclib/terrain.h) |

**Authority rule (preserve):** `getTerrainElevation()` / `worldToTile()`
remain the CPU-side ground truth for gameplay. Any render-side
resampling is visual-only and must not feed back into these.

## 2. Current pass facts substrate

`TerrainPassFacts` exists but is a flag-only record, not a contract.

- Definition: [GameOS/gameos/render_snapshot.h:128-141](../GameOS/gameos/render_snapshot.h) (16-byte static_assert).
- Fields: `viewId` (u32), `legacyProgramId` (GL program name), `drawCallCount`, `flags` bitfield (`kFlagTessellationOn`, `kFlagViewUniformsBound`, `kFlagOverflow`).
- Producer: snapshot extraction at [GameOS/gameos/gameosmain.cpp:1418-1443](../GameOS/gameos/gameosmain.cpp). Source comment notes: *"terrain shaders don't consume ViewUniforms yet"* — `kFlagViewUniformsBound` is currently informational of upload, not of consumption.
- Consumer: inspector readout at [GuiRuntime/EditorInspector.cpp:953-956](../GuiRuntime/EditorInspector.cpp) — surfaces "binding=3: consumed" or "NOT consumed (legacy uniforms)".
- No DrawPacket equivalent; no PipelineDesc registration; no per-bucket facts (water-fast vs solid vs shadow are not separately recorded).

## 3. Shader / uniform inventory

### gos_terrain.frag tunables (CPU-uploaded)

| Uniform | Type | Upload site (file:line) | Notes |
|---|---|---|---|
| `matNormal0..3` | sampler2D | bound per-frame via material binder | Rock / grass / dirt / concrete |
| `matNormalBoost` | vec4 | [gameos_graphics.cpp:1792, 5141-5142, 5254-5255, 5376-5377](../GameOS/gameos/gameos_graphics.cpp) | Per-material normal strength; 3 upload sites (one per program) |
| `tintStrengthScale` | float | as above | Global tint blend |
| `detailNormalTiling` / `detailNormalStrength` | vec4 / vec4 | bridge | Detail blend |
| `terrainLightDir`, `terrainViewDir`, `cameraPos`, `pomParams`, `fog_color` | misc | bridge | Lighting / parallax / fog |

### Shader files

| Stage | File | Role |
|---|---|---|
| Vertex | `shaders/gos_terrain.vert` | Legacy MVP; outputs color, fog (material idx packed in `.x` as 0–255), texcoord, world pos |
| TCS | `shaders/gos_terrain.tesc` (1–61) | Distance-LOD inner/outer; crack-free edge-midpoint heuristic |
| TES | `shaders/gos_terrain.tese` (1–115) | Barycentric interp + Phong smoothing + **texture-based displacement along interpolated normal**; dual `terrainMVP` + `mvp` projection |
| Fragment | `shaders/gos_terrain.frag` (1–120) | Per-material normal sampling, parallax (isometric-tuned), detail tiling (`TC_MAT_TILING`), NdotL + shadow |
| Shadow VS | `shaders/shadow_terrain.vert` (1–11) | `lightSpaceMatrix` only |
| Shadow FS | `shaders/shadow_terrain.frag` (1–6) | Empty body (auto-depth) |
| Water fast | `shaders/gos_terrain_water_fast.vert` | std430 32-byte record at SSBO binding 7 |

### ViewUniforms status

`MC2_USE_VIEW_UNIFORMS` shader-prefix path is NOT enabled for terrain.
Terrain shaders still consume legacy program-scope uniforms. View
upload itself is global ([code/gamecam.cpp:219-226](../code/gamecam.cpp))
and binding-3 UBO is populated when `MC2_VIEW_UNIFORMS != 0` (default ON
post-F1-3D), but terrain shaders ignore it.

### PipelineDesc status

Not registered. [RenderCore/PipelineDesc.h:23](../RenderCore/PipelineDesc.h)
comment explicitly notes static-prop-first scope. Depth/blend/cull state
for terrain still applied via legacy `gosRenderer` state-set, not via
`PipelineRegistry::applyPipeline()`.

## 4. Debug / inspector state

| Surface | Where | What it shows |
|---|---|---|
| ImGui Terrain Pass panel | [EditorInspector.cpp:953-956](../GuiRuntime/EditorInspector.cpp) | viewId, legacyProgramId, drawCallCount, ViewUniforms-consumed flag |
| `MC2_DEBUG_RENDERER=1` terrain pick highlight | EditorInspector.cpp:760-765 | Crosshair on picked tile (IMG-INSPECT-2/3 ship, see memory handoff 2026-05-24) |
| Terrain debug-mode uniform | **none** | No shader-side mode selector exists today |
| Capture metadata | `scripts/capture_baseline.py` `CAPTURE_ENV_KEYS` | Records render envs but no terrain-specific keys |

**Gap:** no fragment-shader debug mode (albedo / normal / slope /
height / layer-id). Slice 2 (TERRAIN-DEBUG-VIEWS-1) lands this.

## 5. Heightfield / render-grid facts

| Fact | Value | Source |
|---|---|---|
| Per-vertex stride | 28 bytes (`PostcompVertex`) | [mclib/vertex.h:32-47](../mclib/vertex.h) |
| Typical grid | 120 × 120 vertices ≈ 14,400 samples | observed in MC2 campaign data |
| World units / vertex | 128.0 | [mclib/terrain.cpp:101-107](../mclib/terrain.cpp) |
| World units / cell (3×3 subdivide) | 42.67 | [mclib/terrain.h:54](../mclib/terrain.h) |
| Tile fixed mesh | 20 × 20 vertices | [mclib/terrain.h:76-84](../mclib/terrain.h) |
| Map extent (typical) | ≈ 15,360 wu / side | derived |
| Raw heightfield size (typical) | ≈ 403 KB | 120² × 28 |
| Render tessellation | Per-patch dynamic (`tesc` distance-LOD inner/outer); patches per mission via indirect cull recipe | [gos_terrain.tesc](../shaders/gos_terrain.tesc), [gos_terrain_indirect.cpp](../GameOS/gameos/gos_terrain_indirect.cpp) |
| Render-side normal | Barycentric-interpolated from `PostcompVertex.vertexNormal` (CPU-baked) + per-material normal-map blend | [gos_terrain.tese:42-45](../shaders/gos_terrain.tese), [gos_terrain.frag](../shaders/gos_terrain.frag) |
| GPU height query | none — gameplay-only via `getTerrainElevation()` | [mclib/terrain.h:205](../mclib/terrain.h) |
| Existing extraction tool | `.claude/dump_terrain.py` is RenderDoc-only, NOT a `.PacketFile` parser | repo root |

## 6. Env vars / gates touching terrain

| Gate | Default | File:line | Effect on terrain |
|---|---|---|---|
| `MC2_VIEW_UNIFORMS` | ON (post F1-3D) | [code/gamecam.cpp:226](../code/gamecam.cpp) | Uploads UBO binding=3. Terrain shaders do NOT consume yet. |
| `MC2_DEBUG_RENDERER` | OFF | EditorInspector.cpp:760 | Tile-pick highlight overlay |
| `MC2_IMGUI_INSPECTOR` | OFF | EditorInspector.cpp:153 | Inspector enable (terrain panel lives inside) |
| `MC2_RENDER_CONTRACT_ASSERT` | OFF | mclib/render_contract.* | Asserts; no terrain-specific subset |
| `TERRAIN_DEPTH_FUDGE` | constant | legacy 0.001f Z-bias | Z-bias only; not a runtime gate |

**No `MC2_TERRAIN_*` family exists.** All terrain debug visibility is
piggybacking on shared inspector flags.

## 7. Validation / capture support

- `scripts/capture_baseline.py` works for terrain-heavy missions but has
  no terrain-specific env keys or debug-mode field (gap).
- `tools/shader_reflect/reflect.py` covers terrain shader variants in
  the golden set (71 JSON goldens; status PASS for terrain at recon HEAD).
- `tests/smoke/run_smoke.py --tier tier1` exercises terrain rendering
  but does no terrain-specific assertions beyond "rendered N frames".
- `check-env-registry.sh` — no terrain-named envs to check today.
- No terrain-pass dispatch counter parallel to `spBuild*` counters.

## 8. Risks / what's blunt today

1. **Visual ceiling = 128 wu / vertex.** 120×120 baked grid + barycentric
   normals gives a tessellated-but-flat look; per-material normal maps +
   POM fake depth but cannot recover lost geometric frequency.
2. **No pass-pipeline contract.** Depth/blend/cull state for solid /
   shadow / water-fast / decal is implicit in legacy state-set code; a
   shader-program swap can silently change effective state.
3. **`TerrainPassFacts` is a single bucket.** Water-fast vs solid vs
   shadow vs detail are not separately accounted; cannot localize a
   regression to a bucket from the snapshot.
4. **ViewUniforms not consumed by terrain shaders.** Two view pipelines
   coexist (UBO for static-prop, legacy uniforms for terrain). Risk:
   skew if camera basis ever desyncs between paths.
5. **`matNormalBoost` / `tintStrengthScale` uploaded at three program
   sites.** Drift between solid and shadow paths is possible — no
   single-source-of-truth uploader.
6. **No CPU↔GPU height parity probe.** A future render-height resample
   (Slice 5 plan) cannot be validated against gameplay sample without
   one; today the only authority is `getTerrainElevation()`.
7. **`.PacketFile` has no out-of-engine parser.** A height-resolution
   audit (Slice 3) needs a minimal parser or an engine-side dump hook.
8. **Capture metadata gap.** `CAPTURE_ENV_KEYS` does not record terrain
   tunables, so a captured frame is not reproducible if a tunable changes.
9. **GPU-driven terrain indirect path** ([gos_terrain_indirect.cpp](../GameOS/gameos/gos_terrain_indirect.cpp))
   adds a second dispatch authority that the inspector does not separately
   surface today.

## 9. Recommended next slices

The approved queue (TERRAIN-DEBUG-VIEWS-1, TERRAIN-HEIGHT-AUDIT-0,
TERRAIN-BASELINE-0, TERRAIN-RESAMPLE-PLAN-0) is correct and ordered.
Recon-derived refinements:

1. **TERRAIN-DEBUG-VIEWS-1** — minimum viable mode set, given current
   shader inputs:
   - 0 final (default)
   - 1 base/layer color (already separable; per-material albedo blend
     is available in `gos_terrain.frag`)
   - 2 normal (interpolated `vertexNormal` only — pre-detail-blend)
   - 3 normal (post detail+material blend)
   - 4 slope (derived from interpolated normal)
   - 5 material/layer id (re-color from packed `fog.x` index)
   - height-grayscale (mode 6) deferred — height texture not currently
     in the fragment stage; lands after Slice 5 plan.

   Implementation route: single `int u_terrainDebugMode` uniform fed
   from `MC2_TERRAIN_DEBUG_MODE` env + ImGui dropdown in the existing
   Terrain Pass inspector panel. Gate default = 0 (final). Adds one
   new env var → must register in `check-env-registry.sh`.

2. **TERRAIN-HEIGHT-AUDIT-0** — fastest path is a minimal Python
   `.PacketFile` parser that locates the `MapData` packet (file format
   is fixed-record; `realVerticesMapSide = sqrt(packet_bytes / 28)`).
   Run on tier1 + report min/max/mean elevation, slope distribution,
   adjacent-delta histogram, blockiness metric (variance of
   second-difference). If parser is non-trivial, fall back to a
   one-shot engine-side dump behind an opt-in env var (`MC2_TERRAIN_HEIGHT_DUMP=1`).

3. **TERRAIN-BASELINE-0** — extend `capture_baseline.py` `CAPTURE_ENV_KEYS`
   to include `MC2_TERRAIN_DEBUG_MODE` (after Slice 2), plus a terrain-
   heavy preset list (recommend mc2_01 grass, mc2_03 salvage, mc2_17
   combined-arms). One capture per debug mode per preset.

4. **TERRAIN-RESAMPLE-PLAN-0** — plan must reconcile two render paths
   (legacy `Terrain::render()` and GPU-driven indirect) and decide
   where a resampled height texture is consumed: TES displacement is
   the natural insertion point (`tc_sampleDisplacement` already runs
   in `gos_terrain.tese:87-88`), but the texture binding and update
   policy need a slot. Gameplay sample preservation rule: resampled
   height MUST equal `getTerrainElevation()` at original vertex
   positions (zero-error constraint at sample sites).

## 10. Open questions for the user

- Are TerrainPassFacts buckets (solid / water-fast / shadow / detail)
  acceptable scope for a Slice 1.5 contract-broadening before Slice 2?
- Is `MC2_TERRAIN_DEBUG_MODE` the right env name, or do we want
  `MC2_TERRAIN_DEBUG_VIEW` to match the static-prop family naming?
- For the height parser (Slice 3), is a tier1-only run sufficient, or
  do you want tier2 (full campaign)?

---

**Status:** docs-only artifact. No code touched. No build required.
Slice 1 of approved batch (TERRAIN-ARC-RECON-0). Next slice
(TERRAIN-DEBUG-VIEWS-1) is **out of scope for this orchestrator**
this session: other session owns debug-functionality edits in the
same worktree (modifies `gos_static_prop_batcher.h`, adds
`debug_state_dump.{cpp,h}`); a terrain debug-mode uniform + ImGui +
shader-reflect golden refresh would race their work. Recommend Slice 3
(TERRAIN-HEIGHT-AUDIT-0) docs-only recon + Slice 5
(TERRAIN-RESAMPLE-PLAN-0) plan as the next two safe artifacts in this
session.
