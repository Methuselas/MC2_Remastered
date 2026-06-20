# Tier-1 instrumentation env vars

> **STATUS: Verified current as of 2026-06-19 (nifty HEAD, build pipeline). Includes MC2_TERRAIN_LOD_CHUNK (default ON), MC2_SHADOW_CSM (default ON), asset-mod payload vars (HDRI_BC6H, BUILDING_PBR), and diagnostic JSONL trace. Match against RendererFeatureRegistry.h for new feature gates.**

## Crash-soak harness (MC2_SOAK_AUTOWIN)

- `MC2_SOAK_AUTOWIN=1` — with a campaign booted via `MC2_BOOT_TO_BAY`, auto-launches each mission from logistics (no clicks), auto-wins it, lets the campaign auto-advance, and repeats until `campaign-complete`. Emits `[SOAK]` stdout markers (autowin/advance/launch/campaign-complete). Default OFF = byte-identical.
- `MC2_SOAK_WIN_AFTER_SEC=5.0` — scenario-time seconds before auto-win fires (default 5.0). Only consulted when `MC2_SOAK_AUTOWIN` is set.

## Feature gates (RendererFeatureRegistry.h kFeatureTable)

- `MC2_COLORMAP_KTX2` — BC7 KTX2 atlas. Default **ON**. Kill=`=0`. Bake: `py -3 scripts/bake_colormap_ktx2.py`.
- `MC2_COLORMAP_CPU_RETIRE` — free cpuColorMap after atlas upload. Default **ON**.
- `MC2_VIEW_UNIFORMS` — ViewUniforms UBO. Default **ON**. Kill=`=0`. Requires restart.
- `MC2_SHADOW_ENABLE=1` — dynamic shadow caster pass. Default **OFF**.
- `MC2_IMGUI=1` — ImGui overlay. Default **OFF**.
- `MC2_IMGUI_INSPECTOR=1` — inspector panel. Default **OFF**. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — debug overlay. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_STATIC_PROP_REGISTRY=1` — GpuStaticPropRegistry. Default **ON**. Editor sets `=0`.
- `MC2_HDRI_BC6H` — upload HDRI sky as BC6H_UFLOAT (requires `.ktx2` sidecar + `GL_ARB_texture_compression_bptc`). Default **ON** (absent = ON; set `=0` to force RGBA16F fallback). Sidecar cooked via `tools/cook_bc6h_hdri.py`.

## SPFLUSH cost-split decomposition (SPFLUSH-COST-SPLIT-1)

- `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` — RDTSC cost-split of `StaticPropRegistryFlush`. Default **OFF**. Emits `[SPFLUSH_COST_SPLIT v1] event=summary` every 10 frames with per-bucket ns averages: `submit_loop`, `inst_build`, `map_lookup`, `color_fill`, `actor_record`, `world_to_block`, `substrate_append`, `baseinstance_upload`, plus lifetime + window dirty counters (`invalidates`, `registrations`, `rebuilds`, `light_writes`). TSC calibrated once on first flush (~1ms spin). Zero behavior change.

## TRACKV CPU perf kill-switches (2026-06-03)

- `MC2_STATIC_PROP_LIVE_BUILDER=1` — DrawPacket v8 kill-switch. Default **OFF** = snapshot is sole static-prop draw-packet owner (live builder + per-flush compare retired). `=1` restores the v3-flip dual build + compare path (regression bisect / A-B).
- `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE` — default **OFF**. STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1 gate.
  - unset/0: no-op (no behavior change).
  - 1: builds an independent registry oracle each frame and compares field-by-field with the produced rows (legacy propBuf on dirty frames, the memcpy'd arena span on clean frames). Emits `[SNAPSHOT_BRIDGE_COMPARE v1] path=DIRTY|CLEAN ...` per frame with: regGen, cullVer, rowCount, oracleCount, rowCountMismatch, immutableMismatch, hasCullMismatch.
- `MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY` — default **ON** (flipped after Tracy proof). STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1 gate.
  - unset/1: on clean `registryGeneration` AND `cullRecordVersion` (vs cached), skip Fill + WriteLoop and `memcpy` the cached rows into the snapshot arena (`Extract.SP.CleanCopy` zone); per-prop diagnostic counters restored from cache. Dirty frames run the legacy path and refresh the cache. First ~10 frames after mission load rebuild while cullVer settles — win is steady-state. **Tracy (user, mc2-win64-water):** `ExtractRenderSnapshot` median 1.68ms→36.7µs (−97%); `Extract.SP.Fill` 1.15ms + `WriteLoop` 159µs → gone, replaced by `CleanCopy` ~15µs.
  - 0: legacy full rebuild every frame (`fillStaticPropSlots` + per-prop WriteLoop) — kill-switch.
  - Combine with `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=1` to prove cached rows still match a live registry rebuild (`path=CLEAN immutableMismatch=0`). Note: compare ON forces the oracle's own Fill, so Tracy under compare will NOT show the Fill drop — measure with compare OFF.
- `MC2_THIN_CANARY=1` — re-enable Probe-6 thin-record canary readback (per-frame ~80KB `glGetBufferSubData` ×2 + CPU recipeIdx/flags compare). Default **OFF**. Diagnostic only; gated 2026-06-03 (was ungated per-frame stall).
- `MC2_STATIC_PROP_LEGACY_DISPATCH=1` — DrawPacket v7 kill-switch: revert to legacy `glMultiDrawElementsIndirect`. Default **OFF**.

## Shadow tuning

- `MC2_SHADOW_BOUNDED_NEAR_FIT=1` — cap fit radius for crisp near shadows. Default **OFF**.
- `MC2_SHADOW_BOUNDED_NEAR_RADIUS=2500` — bounded near-fit radius in wu. Default 2500.
- `MC2_SHADOW_FRUSTUM_DIAG=1` — per-frame coverage probe. Read-only.
- `MC2_STATIC_PROP_BUILDING_SHADOW=1` — replay all buildings into world-fixed static shadow map. Default **OFF**. `=2` adds trace.
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` — feed dynamic caster pass from registry. Default **ON**. Kill=`=0`.
- `MC2_SHADOW_ROBUST_BASIS` — light-basis singularity guard + AABB corner-scarcity fallback (SHADOW-ROBUST-BASIS-1). Default **ON**. `=0` restores legacy (byte-identical for normal suns; can go singular).

### Dynamic CSM (reworked 2026-06-16 `8d36b37d`/`b8f764b6`/`13e96cc9`/`ead760df`)

- `MC2_SHADOW_CSM` — master cascaded-shadow-map toggle. Default **ON** (2026-06-18, `8ff13a36`); opt out with `=0`.
- `MC2_SHADOW_MAP_SIZE` — per-cascade shadow texture size. Default **8192**. 3×8192² ≈ 805 MB VRAM; `=4096` ≈ 201 MB (still sharp at R0=512).
- `MC2_SHADOW_CSM_R0` — near-cascade fit radius (WU). Default **512** (→ 0.25 WU/texel at 4096).
- `MC2_SHADOW_CSM_R1` — mid-cascade fit radius (WU). Default **4096**.
- `MC2_SHADOW_CSM_COUNT` — cascade count. Default **3** (R0 near / R1 mid / full-map far).
- `MC2_SHADOW_CSM_SOFTNESS` — PCF softness. Default **0.9**.
- `MC2_SHADOW_OBJ_NORMAL_BIAS` — object self-shadow normal-offset bias (kills residual acne). Default **2.0**.
- `MC2_SHADOW_MECH_SOFT` — mech self-shadow softness (wider PCF penumbra + terminator smoothstep so flat low-poly facets fade instead of flip + raised floor). Default **1.0**, clamp [0,4]. Object self-shadow is now per-type via GBuffer1.a mask: terrain skip / static-prop NO self-shadow / mech soft (`3253d582`).
- `MC2_SHADOW_PROP_ALPHA` — tree-foliage shadow alpha-test (`shadow_static_prop.frag`, legacy texture path only). Default **ON**.
- `MC2_CLOUD_SHADOW` — cloud-shadow pass. Default **ON**.
- **DEBUG-only:** `MC2_SHADER_PATH_TINT` (shader-path tint); `MC2_TERRAIN_DEBUG_MODE` / `MC2_TERRAIN_LOD_CHUNK_DIAG` = **30/31** = dynamic-shadow viz.
- **RETIRED:** `MC2_SHADOW_CSM_FULLMAP_LAST`, `MC2_SHADOW_CSM_NEAR_MAX` (folded into the fixed-cascade rework).

## GPU cull readback diagnostics

- `MC2_GPU_CULL_READBACK_TRACE=1` — gate all `[GPU_CULL v1]` diagnostic prints (fallback_n2, fallback_conservative, readback_ok, lifecycle_snapshot, motion_tolerance). Default **OFF**. Without this, zero stdout/fflush on hot path. `readback_stale_reset` (10-frame miss → slot abandon) remains unconditional as it signals a real error.

## Always-on / safety

- `MC2_TGL_POOL_TRACE=1` — TGL pool NULL trace
- `MC2_DESTROY_TRACE=1` — per-destruction lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppress first-error prints
- `MC2_HEARTBEAT=1` — stderr heartbeat per second
- `MC2_REVERSE_Z_TRACE=1` — reverse-Z lifecycle prints
- `MC2_GL_DEBUG_FATAL=1` — abort on GL_DEBUG_SEVERITY_HIGH

## RenderWorld arc

- `MC2_OBJECT_ID_BUFFER=1` — R32_UINT MRT attachment-2 (M1.5)
- `MC2_RENDER_WORLD_TRACE=1` — verbose RenderWorld events
- `MC2_MECH_OBJECT_ID_SELFTEST=1` — M2.5 validator
- `MC2_MECH_PICK=1` — mech-pick consumer (M2.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_STATIC_PROP_PICK=1` — static-prop pick (M1.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_GAMEPLAY_PICK_SELFTEST=1` — M2-pre spine validator
- `MC2_GPU_PICK_HOVER=1` — GPU hover pick for mechs/dynamic actors (GPU_PICK_HOVER_DYNAMIC-1). Requires `MC2_OBJECT_ID_BUFFER=1`. Only mech kind uses GPU result; static props fall through to CPU.
- `MC2_GPU_PICK_HOVER_TRACE=1` — verbose hover-pick log (hit/miss/fallback per frame + session totals on exit).

## EditorBridge

- `MC2_EDITOR_MODE` — activates `EditorBridge` API. Default `0`. Combine with `MC2_OBJECT_ID_BUFFER=1` for GPU pick.

## Render-contract assert

- `MC2_RENDER_CONTRACT_ASSERT=1` — runtime GL draw-buffer/depth state vs declared expectation. Phase 2 (`137dc70`).

## Pre-commit invariant scripts

- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas: `sh scripts/check-asset-scale-callers.sh`
- Shader ABI: `cmake --build build64 --target shader_schema`
- RenderWorld firewall: `sh scripts/check-include-firewall.sh`
- No raw GL from game: `sh scripts/check-no-raw-gl-from-game.sh`
- VFX no objectId: `sh scripts/check-vfx-no-objectid.sh`

## Material / static prop gates
- `MC2_SKIP_STATIC_TREES` — pure static-natural update skip (R2b, ~4977→~145 terrain-object updates on dense maps). **Default ON** (opt-out: only `=0` disables; gameplay-critical gates/turrets/special-buildings are excluded from the skip set and still tick every frame).

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader for static-prop tex array. Default **OFF**.
- `MC2_MATERIAL_GPU` — MaterialGpu table upload + SSBO bind. Default **ON**.
- `MC2_MATERIAL_GPU_SAMPLE` — route albedo through MaterialGpu in frag. Default **ON**.
- `MC2_STATIC_PROP_AMBIENT_V1=1` — hemisphere ambient fill in `static_prop.vert`. Default **OFF**.
- `MC2_STATIC_PROP_IBL_SH` — SH-L2 IBL ambient. Default **ON**. Kill=`=0`.
- `MC2_STATIC_PROP_DEBUG_MATERIAL=N` — frag debug view (0=Final,1=Albedo,2=MatIdx,3=Normal,4=TexLayer,5=Rough,6=Metal).
- `MC2_STATIC_PROP_PBR_V1=1` — per-frag Schlick-Fresnel specular. Default **OFF**.
- `MC2_STATIC_LIGHT_UPLOAD_SPLIT` — upload immutable static light prefix `[0..S)` once/dirty-only + dynamic suffix per frame (vs whole LightsData SSBO every frame). Default **ON**. Kill=`=0` (legacy whole-buffer, bit-identical). Requires `MC2_LIGHTBAKE`.
- `MC2_LIGHTBRIDGE_STABLE_SKIP=1` — skip per-frame `EmitBakedGpuLightData` + `getCachedGpuLightIndex` + `staticReg` pointer-chain for stable baked static props (LIGHTBRIDGE-STABLE-SKIP-WIRE-1). Default **OFF**. Requires `MC2_LIGHTBAKE=1` (implicit when bake map populated). Gate: `stableLightSkipArmed` + `stableLightSkipEligible` + valid `lightDataIndex` + generation match. Expected: `serial_commit_us` ~593µs → ~50–80µs on stable frames. Diagnostics: `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1` (prints `LIGHTBRIDGE_STABLE_SKIP:` every 300 frames; independent of skip gate).
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1` — STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into the existing rings. Keeps per-frame range walk + staleness skip. Default **OFF** pending Tracy proof.
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE=1` — diagnostic compare: with the cached path active, also build the legacy temp instance+record per leaf and compare hash/count; logs mismatches. Default **OFF**; requires `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1`.
- `MC2_LIGHTBAKE_STABILITY=1` — trace: per-instance `lightDataIndex` permanence/stability proof (`[LIGHTBAKE-PROOF v1] event=first/UNSTABLE`). Diagnostic, capped 32. Default **OFF**.
- `MC2_LIGHTBAKE_PARITY=1` — trace: baked permanent record == gathered transient record byte/hash (`[LIGHTBAKE-PROOF v1] event=parity match=1`). Diagnostic, capped 32. Default **OFF**.
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1` — cached-blob registry flush (bulk submit + per-recipe cached cull records). Default **OFF** (pending soak/default-flip). `_COMPARE=1` adds the FNV cached-vs-legacy oracle.
- `MC2_STATIC_PROP_COLORS_FILL=1` — restore the per-static-instance Colors SSBO zero-fill. Default **OFF = skip** (no production shader reads colors; ~80ns/leaf saved). Kill-switch only.
- `MC2_STATIC_PROP_PERSISTENT_BUCKETS` — (2b Stage 2) persistent static instance store; skip per-frame static re-push when registry generation clean. Default **ON** (Tracy: flush 312µs→68µs). Kill=`=0`. `_COMPARE=1` adds the FNV store-consistency oracle. `MC2_BUCKET_ORDER_TRACE=1` = Task-0 static/dynamic bucket probe.

## Mech PBR lighting (StandardLit GGX path)

- `MC2_STANDARD_LIT_V1` — Cook-Torrance GGX PBR on mechs. Default **ON**. Kill=`=0`.
- `MC2_MECH_SURFACE_MATERIAL` — surface material profile for mechs. Default **unset** = passthrough (no surface material). Explicit values:
  - `=metal061b` — Metal061B normal+ORM as substrate + PaintedMetal003 paint layer (debug/exposed-metal look; strong PBR)
  - `=painted_subtle` — ORM-only (flat normal, no perturbation) using Metal061B roughness/metalness. Soft defaults: metallic=0.03, roughness 0.65–0.90, wear=0.0, tile_scale=2.0. For "mech catches light better" not "mech looks like raw metal".
- `MC2_MECH_IBL_SH` — SH-L2 HDRI ambient on mechs (shared coefficients with static props). Default **ON**. Kill=`=0`.
- `MC2_MECH_BACK_FILL` — cool-sky fill light for shadow hemisphere (`max(-NdotL,0) * strength`). Default **2.0**. Set `=0` to disable.
- `MC2_PBR_METALLIC_INFLUENCE` — metallic weight multiplier (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_ROUGHNESS_MIN` — minimum roughness clamp (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_ROUGHNESS_MAX` — maximum roughness clamp (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_WEAR_STRENGTH` — wear factor modulation (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_TILE_SCALE` — UV tile scale for PBR detail textures (profile-aware default: see MC2_MECH_SURFACE_MATERIAL). ENV VAR overrides always win.
- `MC2_PBR_TRIPLANAR` — world-space triplanar sampling (no UV seams). Default **OFF**. Set `=1` to enable.
- `MC2_MECH_AMBIENT_V1` — hemisphere ambient fill (legacy Blinn path only; no effect when StandardLit ON). Default **ON**. Kill=`=0`.
- `MC2_MECH_SPECULAR_V1` — Blinn specular sheen (legacy Blinn path only). Default **ON**. Kill=`=0`.
- `MC2_MECH_VIEWUNIFORMS` — mech ViewUniforms block binding (required for StandardLit + specular). Default **ON**. Kill=`=0`.

## Building animation gate (BLDG-TYPE-ANIM-GATE-FIX-1)

- `MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE` — Default **ON**. When enabled, buildings with animation TYPE data but `bdAnimationState==-1` are eligible for the static fast path (`touch()` instead of `TransformMultiShape`). Set `=0` to restore legacy behaviour (`bldgTypeHasAnimations` disqualifies the whole type). Gate logic and env-var read in `mclib/bdactor.cpp`; `[ANIM_GATE v1]` summary lines emitted by `g_staticUpdateEmitSummary()` in `code/terrobj.cpp` every 600 frames.

## Debug state dump

- `MC2_DEBUG_STATE_DUMP=1` — JSON render-state snapshot every 300 frames to `debug_state/latest_render_state.json`. Default **OFF**.
- `MC2_DEBUG_STATE_DUMP_DIR=<path>` — override output dir.
- `MC2_DEBUG_STATE_DUMP_HISTORY=1` — rolling 8-slot history ring. Requires `MC2_DEBUG_STATE_DUMP=1`.

## Terrain fast-path drop log

- `MC2_FASTPATH_DROP_LOG=1` — log terrain fast-path -> legacy setupTextures transitions (transition-only, `[FASTPATH_DROP]`). Default **OFF**.

## Terrain gates

### MC2_TERRAIN_NORMAL_ARRAY

**Default:** off

Switches terrain normal-map sampling from 5 individual `sampler2D` uniforms
(units 5–8, 12) to a single `sampler2DArray` on unit 5. Packs all 5
per-material normal maps (rock/grass/dirt/concrete/snow) into one `GL_RGBA8`
array texture, rebuilt once on texture slot change (lazy, at first draw).
Copies source mip levels from each individual texture rather than regenerating
them, preserving authored displacement-alpha behavior.

**Ceiling removed:** per-material texture-unit ceiling at the sampler-binding
layer. The classifier/blending ceiling (vec4 matWeights, 5 named layers,
separate snow logic) is a separate future task.

**When to enable:** when adding more than 8 terrain materials, or as a
long-term cleanup of the individual-sampler path. After a soak period, delete
the `#else` branches (see post-migration section in the plan).

**Safety:** `buildTerrainNormalArray()` saves/restores all GL pixel-store, PBO,
active texture unit, and texture binding state via `GlPixelStoreGuard`. Safe to
call from inside a draw-bind path.

## Terrain debug / visual

- `MC2_TERRAIN_DEBUG_MODE=N` — terrain frag debug-mode (0=off, 1=DepthCmp, 2=RawColormap, 3=BlurredColormap, 4=MatWeights R=rock/G=grass/B=dirt, 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow, 8=CementDiag, 9=ThinRecordDiag, 10=HeightNormal, 11=HemiAdditive, -1=TessAliveProbe).
- `MC2_TERRAIN_NORMALS_FROM_HEIGHT=1` — derive normal from R32F height tex. Default **OFF**.
- `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR=N` — CPU bilinear resample (1/2/4). Default 1.
- `MC2_TERRAIN_LIGHTING_V1=1` — hemisphere ambient fill on terrain. Default **OFF**.
- `MC2_TERRAIN_LIGHTING_V2=1` — shadow-aware V1 modulation. Default **OFF**.

## Water gates

- `MC2_WATER_SKYTINT=1` — camera-independent sky tint on MDI water. Default **OFF**.
- `MC2_WATER_REFLECTION=1` — SH-L2 sky reflection (camera-dependent). Default **OFF**.
- `MC2_WATER_REFLECTION_RT=1` — mirrored terrain fill into reflection RT. Default **OFF**.
- `MC2_WATER_DEBUG_MODE=N` — MDI water frag debug (0=Final,1=Tint,2=Alpha,3=Normal,4=Depth,5=Shore,6=Lighting,7=ReflSH,8=ReflRT,9=ReflBlend).

## VFX gates

- `MC2_VFX_DEBUG_MODE=N` — GPU particle frag debug (0=Final,1=Albedo,2=Alpha,3=Kind,4=Overdraw).
- `MC2_VFX_AGE_SAMPLE=1` — sample curves at real m_age. Default **OFF**.
- `MC2_VFX_ORACLE_RENDER=1` — Phase 1 originals-restoration (CardCloud+ShardCloud oracle). Default **OFF**.
- `MC2_VFX_GPU_SIM_CARDCLOUD=1` — CardCloud GPU sim SSBO substrate. Default **OFF**.
- `MC2_VFX_SOFT_PARTICLES=1` — depth-fade soft particles. Default **OFF**.
- `MC2_VFX_LIT_PARTICLES=1` — ambient+sun*0.5 fill on particles. Default **OFF**.

## Terrain LOD chunk

- `MC2_TERRAIN_LOD_CHUNK` — chunked heightfield LOD renderer. **DEFAULT ON** (cutover
  2026-06-09 `a7b090be`); set `=0` to opt out (legacy tessellated path, e.g. editor).
  Single-source gate `mc2TerrainLodChunkEnabled()` (terrain.h). Replaces
  makeLists/geometry()/TerrainQuad::draw() + suppresses legacy DrawIndirect.
- `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` — bridge ObjBlockInfo.active from
  blockMeta.inFrustum each frame. Default **OFF**. Phase 2 only; removed Phase 4. Runtime getenv().
- `MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k` — force every chunk block to LOD k (diag).
- `MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS` / `_NO_APRON` / `_NO_STITCH` — disable skirt seal /
  1-ring draw apron / vertex edge-stitch (A/B).
- `MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX=px` — cap skirt depth (default 256).
- `MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD=k` — relax the cement-block LOD0 clamp (default 0).
- `MC2_TERRAIN_LOD_CHUNK_DIAG=<bitmask>` — frag viz (zero-cost off): 1 no-GBuffer1,
  4 no-lighting, 8 no-shadow, 16 flat-normal, 32 no-detail, 64 rock-sample,
  128 terrainType, 256 raw-colormap, 512 bypass-tint. (bit 2 dead — depth now in vert.)
- `MC2_TERRAIN_NORMAL_ARRAY` — material normal sampler2DArray path. Now **default ON**
  (opt-out `=0`); the chunk path needs it built (mip-completeness fixed 2026-06-09).
- Hemisphere ambient on the chunk path is env-gated like legacy: `MC2_TERRAIN_LIGHTING_V1`
  / `MC2_TERRAIN_LIGHTING_V2` (default OFF).

## Deterministic capture clock

- `MC2_SMOKE_FIXED_TIMESTEP=1` — deterministic capture clock (S9D sim + S9E render-shader).
  Opt-in, default **OFF** (byte-identical retail/normal smoke). Pins BOTH the sim clock
  (S9D: scenarioTime/frameRate advance a fixed 1/30s per frame) AND every render-shader
  `time` uniform (S9E: water/terrain/overlay/decal/screen-shadow/godray/shoreline, all in
  seconds, driven off the shared S9D fixed sim-frame counter via `SmokeMode::fixedClockSeconds()`),
  so frame N renders identically every run. Makes sim speed fps-proportional → it is a
  CAPTURE/DETERMINISM knob, **NOT** for full-duration regression smokes.

## Diagnostic JSONL trace (diagnostic_trace.cpp — 2026-06-17)

Structured per-event JSONL output replacing env-gated stderr prints.
Survives normal exits and most crashes better than stderr.
Crash handlers still write to stderr as the last-resort path.

- `MC2_DIAGNOSTIC_TRACE_FILE=<path>` — JSONL output file. Default: `debug_state/diagnostic_trace.jsonl`. Set to `off` to disable.
- `MC2_DIAG_TAGS=<list>` — comma-separated tag whitelist. Controls which tags write events.
  - Unset: default high-value whitelist (`GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,SPFLUSH_COST_SPLIT,CONFIG,BUILD,DEVICE,SHADER_COMPILE`).
  - `*` — all registered tags.
  - `none` — disable JSONL output entirely.
  - Unknown tag names in list → startup warning to stderr, tag ignored.

### Per-event format

```json
{"tag":"GPU_CULL","v":1,"session_id":"12345-1718616000000","pid":12345,"tid":6789,"frame":1234,"ts_ms":16234,"written_epoch":1718616016.234,"data":{...}}
```

### Registered tags

| Tag | Source | Replaces |
|---|---|---|
| `GPU_CULL` | GPU cull readback | `MC2_GPU_CULL_READBACK_TRACE=1` stderr |
| `LIGHTBAKE_PROOF` | Light bake parity | `MC2_LIGHTBAKE_PARITY` stderr |
| `ANIM_GATE` | Animation eligibility summary | `MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE` stderr |
| `SPFLUSH_COST_SPLIT` | Static prop flush perf | `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` stderr |
| `TerrainLOD_prod` | Terrain chunk production telemetry | `[TerrainLOD prod]` stderr |
| `TERRAIN_ACTIVE_AB` | A/B false-negative counts | `MC2_TERRAIN_ACTIVE_AB` stderr |
| `TERRAIN_SOLID_AB` | Solid window A/B counts | `MC2_TERRAIN_SOLID_AB` stderr |
| `CONFIG` | Feature gate snapshot at startup | (new) |
| `BUILD` | Build identity at startup | (new) |
| `DEVICE` | GPU/driver info at startup | (new) |
| `SHADER_COMPILE` | Shader compile results | (new) |

### MCP query

Sessions query via `get_diagnostic_events(tag, last_n)` instead of reading log files:
```
get_diagnostic_events("GPU_CULL", 20)     → last 20 GPU_CULL events
get_diagnostic_events("*", 50)            → last 50 events of any tag
```
Unknown tag → error with known tag list. Known tag with no events → empty list.

### Rotation

At engine startup, if `diagnostic_trace.jsonl` exceeds 10 MB, it is renamed to `diagnostic_trace.prev.jsonl` and a fresh file is started. Previous run's trace is preserved in `.prev.jsonl`.

### Flush policy

Each event is flushed immediately (`fflush`) for crash-survival on most platforms. `mc2_diag::flush()` may be called from crash handlers (no allocation). Full crash survival (hardware fault, SIGKILL, kernel panic) is not guaranteed without `fsync`/`FlushFileBuffers`, which is too expensive per-event.

### Diagnostic profiles

**Standard smoke** (canonical `MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE`):
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"
```

**Shader triage** — add `SHADER_COMPILE` to diagnose boot shader failures or missing vegetation card compiles:
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,SHADER_COMPILE"
```
Then: `get_diagnostic_events("SHADER_COMPILE", 20)` — each event includes `stage`, `path`, `result`, and `info_log` on failure.

**GPU cull triage** — startup health of substrate/compute/readback init:
```powershell
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,GPU_CULL"
```
`get_diagnostic_events("GPU_CULL", 20)` — substrate_init, gl_probe, c1b_cull_ok/fail, compute_selftest, readback_init, readback_selftest per session.

**Static prop flush perf triage** — 10-frame perf window with per-bucket ns breakdown:
```powershell
$env:MC2_STATIC_PROP_FLUSH_COST_SPLIT="1"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,SPFLUSH_COST_SPLIT"
```
`get_diagnostic_events("SPFLUSH_COST_SPLIT", 10)` — summary events with `frame`, `window_frames`, and all ns buckets.

**Light bake stability triage** — bake index permanence/parity proof:
```powershell
$env:MC2_LIGHTBAKE_STABILITY="1"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,LIGHTBAKE_PROOF"
```
`get_diagnostic_events("LIGHTBAKE_PROOF", 20)` — enabled/first/coverage/UNSTABLE events.

**Fail-open:** Shader compile failures emit `SHADER_COMPILE compile_fail` with `info_log` and do not abort optional shader paths where the existing loader already continues gracefully (e.g. vegetation, optional overlays). Core shaders that call `STOP()` on failure remain fatal — fail-open applies only to paths the loader was already skipping.
