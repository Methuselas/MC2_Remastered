# Tier-1 instrumentation env vars

Extracted from CLAUDE.md 2026-05-24. Each env var here is a STABLE
instrumentation knob — opt-in (mostly default-off) probe that doesn't change
default behavior. Add new env-gated probes to this list when shipping.

Startup banner `[INSTR v1] enabled: ...` enumerates which probes fired at log
start. Grep schema versions with `\[SUBSYS v[0-9]+\]`.

## Always-on background / safety

- `MC2_TGL_POOL_TRACE=1` — per-frame TGL pool NULL trace; monotonic summary every 600 frames always-on
- `MC2_DESTROY_TRACE=1` — per-destruction cull/lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppress first-error prints (PRINT-ON by default; drain loop always runs)
- `MC2_ASSET_SCALE_TRACE=1` — per-key runtime lookup events; first `oob_blit` per `(path, callerTag)` always-on
- `MC2_ASSET_SCALE_SELFTEST=1` — synthetic 2x/4x/8x/1.5x golden tests at startup
- `MC2_HEARTBEAT=1` — stderr `[HEARTBEAT]` per second; detect renderer freezes during mod load
- `MC2_REVERSE_Z_TRACE=1` — `[REVERSE_Z v1]` lifecycle prints (projection-matrix build + inverseProjectZ fence-seam first use)
- `MC2_GL_DEBUG_FATAL=1` — `abort()` on `GL_DEBUG_SEVERITY_HIGH`; opt-in safety net for smoke (Tier 1.2)

## RenderWorld arc (M1.5..M2.6 substrate + pickup)

- `MC2_OBJECT_ID_BUFFER=1` — enable R32_UINT MRT attachment-2 substrate (M1.5; required by all object-ID consumers)
- `MC2_RENDER_WORLD_TRACE=1` — per-frame + per-event RenderWorld trace (always-on banner stays; this enables verbose events)
- `MC2_MECH_OBJECT_ID_SELFTEST=1` — M2.5 mech-substrate validator (emits `[MECH_OBJECT_ID_SELFTEST v1] result=PASS|FAIL`)
- `MC2_MECH_PICK=1` — enable mech-pick consumer (M2.6); requires `MC2_OBJECT_ID_BUFFER=1`
- `MC2_MECH_PICK_DEBUG=1` — verbose mech-pick logging (M2.6)
- `MC2_MECH_PICK_PIERCE_FOG=1` — debug-only fog bypass (M2.6); respect fog by default
- `MC2_MECH_PICK_SELFTEST=1` — M2.6 pickup validator (emits `[MECH_PICK_SELFTEST v1] result=PASS|FAIL`)
- `MC2_STATIC_PROP_PICK=1` — enable static-prop pick consumer (M1.6); requires `MC2_OBJECT_ID_BUFFER=1`
- `MC2_STATIC_PROP_PICK_DEBUG=1` — verbose static-prop pick logging (M1.6)
- `MC2_GAMEPLAY_PICK_SELFTEST=1` — M2-pre spine validator

## EditorBridge

- `MC2_EDITOR_MODE` — default `0`. Set to `1` to activate the `EditorBridge` API surface. All `EditorBridge::*` functions are no-ops when `0`. Must be combined with `MC2_OBJECT_ID_BUFFER=1` for full GPU pick support (terrain raycast still works without it). Process-lifetime cached at `EditorBridge::init()`, which the mission editor must call explicitly — mc2 game startup does NOT auto-call it.

## Render-contract assert (Phase 2)

- `MC2_RENDER_CONTRACT_ASSERT=1` — stderr `[RENDER_CONTRACT v2] assert mode ACTIVE` on startup; runtime queries live GL draw-buffer/depth state and compares to `render_contract::stateContractFor()` declared expectation. Inits once after `glewInit` in `gameosmain.cpp`. Phase 2 shipped 2026-05-24 (commit `137dc70`).

## Pre-commit invariant scripts

Run if you touched the area:

- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas / `code/mechicon.cpp`: `sh scripts/check-asset-scale-callers.sh`

## Firewall / no-raw-GL / VFX-no-objectId

Three CI scripts that lock the RenderWorld arc invariants — see `docs/renderworld_arc_status.md` § "CI / enforcement layer" for the full inventory.

- `sh scripts/check-include-firewall.sh` — SCOPE_DIRS layering
- `sh scripts/check-no-raw-gl-from-game.sh` — game-side raw GL prohibition (M6)
- `sh scripts/check-vfx-no-objectid.sh` — VFX attachment-2 prohibition (M4)

## MaterialKtx sidecar (Phase 0)

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader: try `.ktx2` alongside `.tga` when building static-prop texture array. Phase 0: RGBA8 (VK_FORMAT_R8G8B8A8_UNORM/SRGB) only; compressed/supercompressed formats return false silently. Default OFF. Requires `MC2_COALESCE=1`. Implementation: `RenderCore/KtxLoader.{h,cpp}`. Call site: `RenderCore::ktxLoadRgba8(path, out)` in `gos_static_prop_batcher.cpp`.

## MaterialGpu sidecar (MaterialGpu-2)

- `MC2_MATERIAL_GPU=1` — enable static-prop MaterialGpu sidecar: builds table from `texArrayLayer`, uploads mission-lifetime SSBO at binding 5, binds in `flush()`, compares `albedoTex` vs legacy layer. Default OFF. Log prefix: `[MATERIAL_GPU v1]`. Emits: `event=table_upload materials=N bytes=B emitted=M`, `event=compare emitted=M mismatches=0`, `event=unload materials=N bytes=B`. No visual change. No shader consumer until MaterialGpu-3.
- `MC2_MATERIAL_GPU_SAMPLE=1` — enable MaterialGpu shader sampling (MaterialGpu-3).
  Requires `MC2_MATERIAL_GPU=1` to have any effect (both required for `u_materialGpuSample=1`).
  Default OFF. When both active: `static_prop.frag` reads `materials[materialIdx].albedoTex`
  instead of `PerDrawEntry.texArrayLayer`. Expected result: zero pixel delta (same layer index).
  Log: `event=sample_mode enabled=1 loc=N` (once per flush). Diagnostic reason codes:
  `upload_env_off | sample_env_off | no_ssbo | sidecar_invalid | uniform_missing`.

## DrawPacket v2 compare

- `MC2_DRAW_PACKET_COMPARE=1` — per-frame candidate vs batcher field check (summary log). Emits one `[DRAW_PACKET_COMPARE v1]` line per frame to stderr: `frame=%u packets=%u pipeline_invalid=%u pipeline_oob=%u geom_mismatch=%u type_mismatch=%u alpha_mismatch=%u`. Default OFF. Log tag `v1`; increment if fields are added/removed in future slices. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_COMPARE_VERBOSE=1` — also emit per-mismatch detail lines (`[DRAW_PACKET_COMPARE detail]`) for firstIndex, indexCount, owningType, and alpha disagreements. Requires `MC2_DRAW_PACKET_COMPARE=1` to have any effect (compare must be enabled for the per-candidate loop to run). Valid values: `1` to enable; omit or set to any other value to disable. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_V3=1` — enable DrawPacket v3 conversion + build log — diagnostic only; no GL state mutation, no pixel change. Emits one `[DRAW_PACKET v3]` line per frame to stderr with 9 build counters (input, emitted, invalid_pipeline, pipeline_oor, invalid_index, invalid_instance, overflow, object_sentinel, light_sentinel) plus sorted_packet_cap.
- `MC2_DRAWPACKET_STATIC_PROP_OPAQUE` — enables v4A substitutive opaque dispatch for all-opaque static-prop types. Active only when `IsCoalesceEnabled()==false`; coalesce mode logs `event=coalesce_noop`. Requires: no v3 gate needed (uses v0 emit candidates directly). Gate latched at first `batcher_setOpaqueDispatchCandidates()` call; process restart to change. Default: OFF. Logs: `[DRAW_PACKET_DISPATCH v1]` + `[DRAW_PACKET_SUPPRESS v1]` at 600-frame cadence. `cachedMaterialFlags`: TRANSITIONAL field dependency — update v4A if semantics change.

## SSBO binding registry

Note: bindings are **per-pass** (each GL program declares its own layout bindings and each pass re-binds before drawing). The static_prop pass and terrain pass are separate programs.

### static_prop pass

| Binding | Owner | Status |
|---|---|---|
| 0 | Instances | active |
| 1 | Colors (legacy) | active |
| 2 | PerType | active |
| 3 | Parity (debug) | active (`MC2_OBJECT_PARITY_CHECK=1`) |
| 4 | PerDraw | active (coalesce path) |
| 5 | MaterialGpu | ACTIVE (v3+) — always declared in static_prop.frag coalesce variant; SSBO bound when MC2_MATERIAL_GPU=1, unbound otherwise. Shader accesses only when u_materialGpuSample=1. |

### terrain pass (separate GL program — independent binding namespace)

| Binding | Owner | Status |
|---|---|---|
| 5 | WaterRecipeBuf | active (gos_terrain_mask_water.vert, gos_terrain_water_fast.vert, gos_terrain_water_fast_mdi.vert) |
