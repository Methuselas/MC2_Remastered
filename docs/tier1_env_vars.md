# Tier-1 instrumentation env vars

> **NOTE: This file is stale as of 2026-06-01. Treat as orientation only; verify against RendererFeatureRegistry.h and source before relying on any entry.**

## Feature gates (RendererFeatureRegistry.h kFeatureTable)

- `MC2_COLORMAP_KTX2` — BC7 KTX2 atlas. Default **ON**. Kill=`=0`. Bake: `py -3 scripts/bake_colormap_ktx2.py`.
- `MC2_COLORMAP_CPU_RETIRE` — free cpuColorMap after atlas upload. Default **ON**.
- `MC2_VIEW_UNIFORMS` — ViewUniforms UBO. Default **ON**. Kill=`=0`. Requires restart.
- `MC2_SHADOW_ENABLE=1` — dynamic shadow caster pass. Default **OFF**.
- `MC2_IMGUI=1` — ImGui overlay. Default **OFF**.
- `MC2_IMGUI_INSPECTOR=1` — inspector panel. Default **OFF**. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — debug overlay. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_STATIC_PROP_REGISTRY=1` — GpuStaticPropRegistry. Default **ON**. Editor sets `=0`.

## SPFLUSH cost-split decomposition (SPFLUSH-COST-SPLIT-1)

- `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` — RDTSC cost-split of `StaticPropRegistryFlush`. Default **OFF**. Emits `[SPFLUSH_COST_SPLIT v1] event=summary` every 10 frames with per-bucket ns averages: `submit_loop`, `inst_build`, `map_lookup`, `color_fill`, `actor_record`, `world_to_block`, `substrate_append`, `baseinstance_upload`, plus lifetime + window dirty counters (`invalidates`, `registrations`, `rebuilds`, `light_writes`). TSC calibrated once on first flush (~1ms spin). Zero behavior change.

## TRACKV CPU perf kill-switches (2026-06-03)

- `MC2_STATIC_PROP_LIVE_BUILDER=1` — DrawPacket v8 kill-switch. Default **OFF** = snapshot is sole static-prop draw-packet owner (live builder + per-flush compare retired). `=1` restores the v3-flip dual build + compare path (regression bisect / A-B).
- `MC2_THIN_CANARY=1` — re-enable Probe-6 thin-record canary readback (per-frame ~80KB `glGetBufferSubData` ×2 + CPU recipeIdx/flags compare). Default **OFF**. Diagnostic only; gated 2026-06-03 (was ungated per-frame stall).
- `MC2_STATIC_PROP_LEGACY_DISPATCH=1` — DrawPacket v7 kill-switch: revert to legacy `glMultiDrawElementsIndirect`. Default **OFF**.

## Shadow tuning

- `MC2_SHADOW_BOUNDED_NEAR_FIT=1` — cap fit radius for crisp near shadows. Default **OFF**.
- `MC2_SHADOW_BOUNDED_NEAR_RADIUS=2500` — bounded near-fit radius in wu. Default 2500.
- `MC2_SHADOW_FRUSTUM_DIAG=1` — per-frame coverage probe. Read-only.
- `MC2_STATIC_PROP_BUILDING_SHADOW=1` — replay all buildings into world-fixed static shadow map. Default **OFF**. `=2` adds trace.
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` — feed dynamic caster pass from registry. Default **ON**. Kill=`=0`.

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

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader for static-prop tex array. Default **OFF**.
- `MC2_MATERIAL_GPU` — MaterialGpu table upload + SSBO bind. Default **ON**.
- `MC2_MATERIAL_GPU_SAMPLE` — route albedo through MaterialGpu in frag. Default **ON**.
- `MC2_STATIC_PROP_AMBIENT_V1=1` — hemisphere ambient fill in `static_prop.vert`. Default **OFF**.
- `MC2_STATIC_PROP_IBL_SH` — SH-L2 IBL ambient. Default **ON**. Kill=`=0`.
- `MC2_STATIC_PROP_DEBUG_MATERIAL=N` — frag debug view (0=Final,1=Albedo,2=MatIdx,3=Normal,4=TexLayer,5=Rough,6=Metal).
- `MC2_STATIC_PROP_PBR_V1=1` — per-frag Schlick-Fresnel specular. Default **OFF**.
- `MC2_STATIC_LIGHT_UPLOAD_SPLIT` — upload immutable static light prefix `[0..S)` once/dirty-only + dynamic suffix per frame (vs whole LightsData SSBO every frame). Default **ON**. Kill=`=0` (legacy whole-buffer, bit-identical). Requires `MC2_LIGHTBAKE`.
- `MC2_LIGHTBAKE_STABILITY=1` — trace: per-instance `lightDataIndex` permanence/stability proof (`[LIGHTBAKE-PROOF v1] event=first/UNSTABLE`). Diagnostic, capped 32. Default **OFF**.
- `MC2_LIGHTBAKE_PARITY=1` — trace: baked permanent record == gathered transient record byte/hash (`[LIGHTBAKE-PROOF v1] event=parity match=1`). Diagnostic, capped 32. Default **OFF**.

## Debug state dump

- `MC2_DEBUG_STATE_DUMP=1` — JSON render-state snapshot every 300 frames to `debug_state/latest_render_state.json`. Default **OFF**.
- `MC2_DEBUG_STATE_DUMP_DIR=<path>` — override output dir.
- `MC2_DEBUG_STATE_DUMP_HISTORY=1` — rolling 8-slot history ring. Requires `MC2_DEBUG_STATE_DUMP=1`.

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
