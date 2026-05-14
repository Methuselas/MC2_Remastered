# Raster-triangle bug — handoff to next session

**Date:** 2026-05-14
**Branch:** `claude/gpu-driven-rendering`
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/`
**Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.4/`
**Predecessor:** `docs/superpowers/progress/2026-05-13-raster-triangle-probe-state.md`

## TL;DR — ROOT CAUSE FOUND, FIX NOT YET LANDED

Under fast camera rotation, a single giant grey-banded terrain triangle appears with varying screen position. The root cause is **MVP / thin-record temporal misalignment caused by the intentional one-frame lag in the indirect terrain pipeline**:

- `ComputeDispatch` writes thin records for frame N, but they are CONSUMED by the bridge VS on frame N+1 (by-design one-frame lag for some GPU feature the user mentioned).
- The bridge VS uses **current frame's** `terrain_mvp_` for projection (`terrainBindThinUniformsForPatchStream` at `gameos_graphics.cpp:4612-4613`).
- Under fast rotation, `MVP_N != MVP_{N-1}`. Compute's pzOk gate decisions baked into thin records assume `MVP_{N-1}`. The VS then projects through `MVP_N`.
- Quads that were CORRECTLY culled by compute under MVP_{N-1} now produce wild gl_Position values when VS projects them through MVP_N — giant screen-spanning triangle.

**Disable signal that confirms the mechanism:** `MC2_GPU_DRIVEN_TERRAIN_SOLID=0` fixes the bug. The CPU pack path (`PackThinRecordsForFrame`) is *not* frame-lagged — it writes thin records this frame using this frame's data, with no MVP-vs-thin-record misalignment.

## Direct evidence

**Probe 8 (`[RING_MVP_DELTA v1]`)** in the file-sink log shows byte-level divergence between MVP at compute time vs MVP at draw time:

```
[RING_MVP_DELTA v1] bridge_frame=1215 dispatch_frame=1216 dispatch_fp=0x... draw_fp=0x...
    disp_mvp[0..3]=[1.449867, -0.042841,  0.255729,  0.257043]
    draw_mvp[0..3]=[1.185752, -0.126905,  0.757537,  0.761431]
```

`dispatch_frame` is consistently 1 ahead of `bridge_frame` — this is the **intentional one-frame lag** the user confirmed exists "for a GPU feature we needed."

Frame `1718` (a high-rotation frame in the smoke):
```
disp_mvp[0..3]=[-0.970420,  0.767111, -0.647702, -0.650977]
draw_mvp[0..3]=[-0.576164,  0.858405, -0.724785, -0.728450]
```

Deltas of 0.3–0.5 between successive frames' MVPs under fast smoke-rotation. Quads at frustum edges shift in/out of view between compute and draw → giant rasterized triangles.

## What was tried that did NOT fix it (~6 hours of probe work)

Probes 1–7 silent on user-confirmed bug-reproducing runs:

| Probe | What it tests | Result |
|---|---|---|
| 1 (`[RING_TRIPWIRE]`) | ring fence missing OR `glClientWaitSync` timeout | clean |
| 2 (`[RING_OVERSHOOT]`) | atomicAdd overshoot past kMaxThinRecords | peak 9604 / cap 65536 |
| 3 (`[RING_CMDPATCH]`) | indirect cmd.count ≠ min(vis,cap)×6 | clean |
| 4 (`[RING_NEARW]`) | quads passing pzOk with abs(clip.w) < 0.05 | clean |
| 5 (`[RING_SPREAD]`) | recipe corners spanning > 2.5 * worldUnitsPerVertex | clean |
| 6 (`[RING_CANARY]`) | thin SSBO recipeIdx clobber, full-coverage | clean |
| 7 (`MC2_RING_FORCE_FINISH`) | force `glFinish()` between compute and bridge | no fix |
| 7a (canary widened to flags field) | bytes 8-11 of thin record clobber | clean |
| 8 (`[RING_MVP_DELTA]`) | MVP delta between compute and bridge | **FIRED 300-700×/run, byte-level confirmed** |
| 8b (byte-level dispatch vs draw mvp) | verify probe-8 is not a fingerprint artifact | values genuinely differ |

Subsystem bisects (all "still bug"):

| Test | Env | Outcome |
|---|---|---|
| A | `MC2_TERRAIN_LIGHTING_GPU=0` | bug GONE but terrain black (cascade self-disables terrain compute via lightSsbo==0) |
| B | `MC2_GPU_CULL_SUBSTRATE=0` | bug stays (static props absent) |
| C | `MC2_GPU_CULL=0` | bug stays |
| D | `MC2_GPU_DRIVEN_WATER=0` | bug stays |
| E | `MC2_GPU_OBJECTS=0` | bug stays |

Structural fixes attempted (all kept in tree, none of them THE fix):

- `cd9f853` (REVERTED via `c4fbd8c`): added `layout(location=0)` dummy attribute to thin VS — turned out to introduce NaN UB via unbound attribute 0 array (shader expert finding).
- `53cd157`: array-index cornerIdx selectors in thin VS (consumer side — wpsArr/wnsArr).
- `ee06c60`: constant `kCornerTable[12]` cornerIdx producer in thin VS.
- `coherent` qualifier on thin SSBO writer (compute), reader (VS), and reader (frag) — added because gpu_cull*.comp consistently uses it, was the outlier in terrain path. Pattern improvement, not the fix.
- `--clean-first` full rebuild done. Stale linkage ruled out.

Three subagents dispatched twice across the session — converged on hypotheses (a) flags-field corruption, (b) cornerIdx mis-lower, (c) coherent qualifier — all falsified by probes 7a and the coherent fix landing without effect. They did NOT identify the temporal-misalignment hypothesis.

## The actual hypothesis (newly identified)

Compute and bridge are intentionally separated by one frame as part of a GPU pipeline feature (user confirmation). The thin records the bridge reads were generated using `MVP_{N-1}`, but the bridge uploads `MVP_N` to the VS. Under fast rotation:

```
Frame N-1: setMVP(M_{N-1}) → compute culls + writes thin slot S using M_{N-1}
Frame N:   setMVP(M_N) → compute culls + writes thin slot S+1 using M_N
           bridge draws THIN SLOT S (frame N-1's records) USING M_N — MVP mismatch
```

Quads that compute placed in slot S under M_{N-1} expected to be projected with M_{N-1}. The VS projects them with M_N. The pzOk gate's `[0,1)` decisions don't transfer.

## Suggested fixes (next session)

In priority order:

### Fix A (smallest, most targeted): MVP snapshot per ring slot

When compute writes thin records to ring slot S using MVP_X, **stash MVP_X alongside the slot**. When the bridge draws from slot S, **upload MVP_X to the VS** (not the current frame's MVP).

Implementation sketch:
- Add `g_thinSlotMVP[kThinRingFrames]` array of `mat4` (3 × 64 bytes = 192 bytes total).
- In `ComputeDispatch`, after fence wait + slot advance, copy current MVP into `g_thinSlotMVP[g_thinRingSlot]`.
- In `gos_terrain_bridge_drawIndirect`, override `terrainBindThinUniformsForPatchStream`'s MVP upload to use the stashed MVP for the current ring slot (via a new accessor `gos_terrain_indirect_getRingSlotMvp()`).

This ensures the VS projects with the same MVP that compute culled with. The "wrong on edge" quads still get drawn, but they project to their correct positions for that MVP — same as they would have if the camera hadn't moved.

**Risk:** Visual artifact is now "one frame stale terrain geometry" instead of "garbage triangle." Under fast rotation that's still visible (the terrain lags the camera by 1 frame) — but it's *correct geometry*, not screen-spanning trash.

### Fix B (better visual): clip-space positions in thin records

Have compute pre-project quad corners to clip space and store them in the thin record. VS skips projection entirely. No MVP at all in the bridge.

**Risk:** Thin record grows from 32 to 32 + 4×16 = 96 bytes per quad. `kMaxThinRecords × 3 slots × 96` = ~19 MB SSBO instead of ~6 MB. Tolerable.

### Fix C (probably wrong direction): eliminate the one-frame lag

User said the lag is intentional for a GPU feature. Don't undo it.

## Files / commits left in the tree

```
56caca5  probe(terrain_indirect): bridge_frame counter for compute-vs-bridge alignment
d9c2126  probe(terrain_indirect): probe 8b — byte-level MVP comparison on mismatch
5856271  probe(terrain_indirect): probe 7a (canary covers flags) + probe 8 (MVP delta)
... [earlier probes 1-6 and structural fixes] ...
c3f14ad  merge: claude/pre-bake-terrain — mask-dispatch Stage 1a-1c
```

All probes are **default-off-by-tripwire** with file-sink to `ring_trace.log` next to mc2.exe. They survive user-launched smoke runs.

The smoke runner (`scripts/run_smoke.py`) snapshots `ring_trace.log` per mission to `tests/smoke/artifacts/<timestamp>/{stem}.ring_trace.log` (commit `fdb8f53`).

## How to verify the fix once landed

1. With current binary + `MC2_RING_TRACE=1`: `[RING_MVP_DELTA v1]` fires 300+ times per smoke (current state, baseline).
2. After Fix A or B: `[RING_MVP_DELTA v1]` will still fire (the MVP_N vs MVP_{N-1} delta is intrinsic to the pipeline lag), but the user should NO LONGER see giant triangles visually because the VS now uses the correct MVP for the records it's drawing.
3. **User must visually confirm** — the probe alone doesn't tell us if the fix works; the smoke harness doesn't read pixels. User drives smoke, looks at the screen, reports yes/no triangle.

## Important context for fresh session

- **The user drives every smoke session and watches the screen live.** Their visual reports ("still does it", "appeared on 7a", "got a good screenshot") are first-hand observation, not log interpretation. Documented in `CLAUDE.md` "Smoke sessions are USER-DRIVEN (load-bearing)" section (commit `4c17b59`).
- The user knows the codebase well and provided the critical clue ("intentional one frame lag, GPU feature we needed") that unlocked the diagnosis. **Use them as a sounding board for hypotheses BEFORE going deep on probes.**
- 12 subagent definitions copied from nifty-mendeleev into this worktree's `.claude/agents/`. `mc2-terrain-indirect-expert`, `mc2-shader-expert`, `mc2-cpu-gpu-offload-expert` were all dispatched twice during this session and did not identify the temporal-misalignment hypothesis. They might do better with this fresh context.

## Unstaged in working tree (parent-session WIP, NOT mine)

```
M GameOS/gameos/gos_terrain_lighting.cpp
M GameOS/gameos/gos_terrain_patch_stream.cpp
M GameOS/gameos/gos_terrain_water_stream.cpp
M mclib/terrain.cpp
M shaders/gos_terrain_lighting.comp
?? scripts/run_smoke_local.py
```

These were modified by the parent session before this investigation began. Stash-and-popped through the `pre-bake-terrain` merge cleanly. Do NOT commit them in the fix session — they are parent-session WIP. The probe + fix work can be staged independently.

## One-shot recipe for next session

1. Read this file in full.
2. Read `docs/superpowers/progress/2026-05-13-raster-triangle-probe-state.md` for the earlier probe history.
3. Confirm the MVP-delta hypothesis is well-supported by looking at any recent `tests/smoke/artifacts/*/mc2_01.ring_trace.log` — should see `[RING_MVP_DELTA v1]` lines with consistent 1-frame off-by-one.
4. Implement Fix A (smallest first). Land in one commit.
5. User drives a smoke. If triangle is gone, ship. If triangle persists in some new form, Fix A's snapshot mechanism doesn't fully cover the issue and we need Fix B.
