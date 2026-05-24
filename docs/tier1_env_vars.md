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
