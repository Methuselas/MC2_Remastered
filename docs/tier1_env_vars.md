# Tier-1 instrumentation env vars

Extracted from CLAUDE.md 2026-05-24. Each env var here is a STABLE
instrumentation knob — opt-in (mostly default-off) probe that doesn't change
default behavior. Add new env-gated probes to this list when shipping.

Startup banner `[INSTR v1] enabled: ...` enumerates which probes fired at log
start. Grep schema versions with `\[SUBSYS v[0-9]+\]`.

## Feature gates (registered in RendererFeatureRegistry.h kFeatureTable)

- `MC2_SHADOW_ENABLE=1` — enable shadow-map pre-pass and PCF sampling in mech/static-prop paths. Default OFF.
- `MC2_IMGUI=1` — enable ImGui overlay (GuiRuntime/GuiRuntime.cpp). Default OFF. Editor sets this automatically.
- `MC2_IMGUI_INSPECTOR=1` — enable ImGui inspector panel (GuiRuntime/EditorInspector.cpp). Default OFF. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — enable debug overlay renderer (GuiRuntime/EditorInspector.cpp). Default OFF. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_STATIC_PROP_REGISTRY=1` — GpuStaticPropRegistry enable (default ON; editor sets `=0` via EditorMFC.cpp to bypass registry for edit-time mutations).

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
- Shader ABI (SSBO/UBO layout): `cmake --build build64 --target shader_schema`
  - Trigger: touched `RenderCore/MaterialGpu.h`, `gos_mech_batcher.h`, their GLSL
    mirrors, or `tools/shader_schema/manifest.json`
  - Pass: `[SHADER_SCHEMA v1] PASS interfaces=2`
  - Failure example (field offset drift): `[SHADER_SCHEMA v1] FAIL interface=GpuMechInstance field=materialIdx cppOffset=52 glslOffset=48`
  - Failure example (size mismatch): `[SHADER_SCHEMA v1] FAIL interface=MaterialGpu no SSBO with array_stride=32 in ...`
  - To add an interface: edit `tools/shader_schema/manifest.json`, run
    `py -3 tools/shader_reflect/reflect.py --update` if adding a new fixture,
    then verify `shader_schema` passes

## Firewall / no-raw-GL / VFX-no-objectId

Three CI scripts that lock the RenderWorld arc invariants — see `docs/renderworld_arc_status.md` § "CI / enforcement layer" for the full inventory.

- `sh scripts/check-include-firewall.sh` — SCOPE_DIRS layering
- `sh scripts/check-no-raw-gl-from-game.sh` — game-side raw GL prohibition (M6)
- `sh scripts/check-vfx-no-objectid.sh` — VFX attachment-2 prohibition (M4)

## MaterialKtx sidecar (Phase 0)

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader: try `.ktx2` alongside `.tga` when building static-prop texture array. Phase 0: RGBA8 (VK_FORMAT_R8G8B8A8_UNORM/SRGB) only; compressed/supercompressed formats return false silently. Default OFF. Requires `MC2_COALESCE=1`. Implementation: `RenderCore/KtxLoader.{h,cpp}`. Call site: `RenderCore::ktxLoadRgba8(path, out)` in `gos_static_prop_batcher.cpp`.

## MC2_MATERIAL_GPU

Default: **ON** (set `MC2_MATERIAL_GPU=0` to disable).

Controls static-prop MaterialGpu table upload, SSBO bind, and compare validation.
When enabled, every flush validates `materials[materialIdx].albedoTex == texArrayLayer` (load-bearing invariant).

## MC2_MATERIAL_GPU_SAMPLE

Default: **ON** (set `MC2_MATERIAL_GPU_SAMPLE=0` to disable shader sampling).

Routes static-prop albedo through the MaterialGpu table in `static_prop.frag`.
Set `=0` to fall back to `texArrayLayer` (legacy fallback / compare authority).
Requires `MC2_MATERIAL_GPU` also active (sampleOn gate checks both).

Log tag: `[MATERIAL_GPU v4]`

## Static-prop dispatch hierarchy (v7)

As of v7 the static-prop draw path is:

| Path | Trigger | Log tag |
|---|---|---|
| **Primary** — `DrawPacket[] + StaticPropDispatchMeta[]` | Default ON; `event=armed ... default=1` at process start | `[DRAW_PACKET_V6]` |
| **Fallback** — legacy `glMultiDrawElementsIndirect` coalesce | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | none |
| **Diagnostic** — v5 per-draw-call loop | `MC2_DRAW_PACKET_COALESCE_V5=1` (deprecated opt-in) | `[DRAW_PACKET_V5]` |
| **Historical** — v4A/v4B/v4C coverage probes | Removed in v7.1 | n/a |

`[DRAW_PACKET_V6]` is the live log tag for the primary dispatch path as of v7. It fires by
default — it is not an opt-in tag. The old `MC2_DRAW_PACKET_STATIC_PROP_V6` opt-in env var
is fully inert; gate plumbing removed in v7.1. Future path: v8 will normalize the tag to
`[STATIC_PROP_PACKET_DISPATCH v1]` if/when shadow-pass dispatch is added.

## DrawPacket v2 compare

- `MC2_DRAW_PACKET_COMPARE=1` — per-frame candidate vs batcher field check (summary log). Emits one `[DRAW_PACKET_COMPARE v1]` line per frame to stderr: `frame=%u packets=%u pipeline_invalid=%u pipeline_oob=%u geom_mismatch=%u type_mismatch=%u alpha_mismatch=%u`. Default OFF. Log tag `v1`; increment if fields are added/removed in future slices. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_COMPARE_VERBOSE=1` — also emit per-mismatch detail lines (`[DRAW_PACKET_COMPARE detail]`) for firstIndex, indexCount, owningType, and alpha disagreements. Requires `MC2_DRAW_PACKET_COMPARE=1` to have any effect (compare must be enabled for the per-candidate loop to run). Valid values: `1` to enable; omit or set to any other value to disable. Cached at process start (static initializer); must be set before launching mc2.exe.
- `MC2_DRAW_PACKET_V3=1` — enable DrawPacket v3 conversion + build log — diagnostic only; no GL state mutation, no pixel change. Emits one `[DRAW_PACKET v3]` line per frame to stderr with 9 build counters (input, emitted, invalid_pipeline, pipeline_oor, invalid_index, invalid_instance, overflow, object_sentinel, light_sentinel) plus sorted_packet_cap.
- `MC2_DRAW_PACKET_COALESCE_V5=1` — master gate for DrawPacket v5 substitutive per-draw-call dispatch. Replaces the two `glMultiDrawElementsIndirect` calls in `flush()` coalesce branch with a per-slot `glDrawElementsInstancedBaseVertexBaseInstance` loop. Requires `ARB_base_instance`; falls through to legacy multidraw if absent (logs `event=unsupported`). Emits `[DRAW_PACKET_V5]` to stderr: `event=armed` once at gate-on, `event=dispatch_summary` every 600 frames (slots_considered, draws_issued, zero_instance_skips, sorted_oob, packet_oob, type_oob, base_instance_missing, gl_errors, ok). `ok=1` iff all error counters are 0. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp` only — no gameosmain changes.
- `MC2_DRAW_PACKET_COALESCE_V5_TRACE=1` — per-slot verbose trace for v5. Emits one line per issued draw and one line per skip with reason. Requires `MC2_DRAW_PACKET_COALESCE_V5=1`. Default OFF. Cached at process start.
- `MC2_DRAW_PACKET_STATIC_PROP_V6=1` — **REMOVED in v7.1.** Gate plumbing deleted; setting this var has no effect whatsoever. Kill-switch for the primary path: `MC2_STATIC_PROP_LEGACY_DISPATCH=1`.
- `MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE=1` — per-slot verbose trace for v6. Emits one `[DRAW_PACKET_V6] slot=S pkt=P type=T group=G inst=I base=B drawID=D first=F count=C baseV=V` line per issued draw. Requires v6 path active (default in v7; no explicit env var needed). Default OFF. Cached at process start.
- `MC2_STATIC_PROP_LEGACY_DISPATCH=1` — v7 kill-switch. Reverts the v6 packet+meta dispatch path to legacy `glMultiDrawElementsIndirect` for this process. Use to isolate v6-specific rendering regressions. When set, `s_v6Enabled` returns false at process start; no `[DRAW_PACKET_V6]` lines appear in logs. Default OFF. Cached at process start.

## Snapshot-assisted dispatch (Extraction v2.3)

- `MC2_SNAP_CULL=1` — opt-in snapshot-assisted static-prop snap-cull. When enabled, the v6 dispatch loop uses the previous frame's RenderSnapshot to skip draw slots whose instanceCount was zero. Requires `snap->ok==1` and count-match validation. Warmup guard prevents frame-1 blank artifact. `spSnapCullSlotMismatch` is in the `ok` gate. Smoke tier1 passes with this set; skipped counts vary by mission density. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp` only.
- `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` — opt-in snapshot-owned slot identity dispatch (Extraction v3). When enabled, builds a second set of dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) from the previous frame's `RenderSnapshot` rows, compares against live-built arrays field-by-field, and dispatches snapshot-built arrays if compare passes (zero mismatch). Falls back to live arrays on any mismatch. Incompatible with `MC2_SNAP_CULL=1` (collision → live dispatch + log line). Counters: `spBuildAttempted`/`spBuildFallback` informational; `spBuildCountMismatch`/`spBuildPacketMismatch`/`spBuildMetaMismatch` in `ok` gate. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp`; accessor `batcher_getSnapshotBuildStats()`.

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
