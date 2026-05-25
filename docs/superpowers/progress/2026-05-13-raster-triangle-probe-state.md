# Raster-triangle-on-fast-rotation — probe state

**Session:** 2026-05-13 (resumed from handoff "Raster triangles on fast camera motion")
**Branch:** claude/gpu-driven-rendering
**Status:** probes deployed, smoke clean, awaiting user manual repro log

## Visual signature (from user screenshot)

- One giant grey-banded triangle sweeping from the upper-left screen corner
- Parallel diagonal banding (matches atlas tile width × WorldPos gradient)
- Terrain only — props, mechs, HUD render normally
- Reproduces over both land and water tiles (so the bug is in the *terrain indirect path*, not the water path or a per-biome quirk)
- Fast camera ROTATION specifically; not pure pan

## Architectural facts established (grep-verified, 2026-05-13)

### Indirect terrain pipeline (per frame, GPU-active path)

```
ComputePreflight (terrain.cpp:1796)
  └─ FlushDirtyRecipeSlotsToGPU (gos_terrain_indirect.cpp:1870) — glBufferSubData, CPU→GPU
  └─ GPU branch (line 1872): set s_frameSolidArmed=true, return

gos_terrain_lighting::PackAndDispatch (terrain.cpp:1802) — Phase 1 lighting compute

ComputeDispatch (terrain.cpp:1806)
  ├─ Advance g_thinRingSlot (line 2004)
  ├─ Wait fence at new slot (line 2005-2010, 10ms timeout)
  ├─ Clear bucket header visibleCount=0 (line 2017)
  ├─ Dispatch cull/pack compute: atomicAdd hdr.visibleCount, write thin[outSlot]
  ├─ glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)
  ├─ Dispatch cmd-patch: cmd.count = min(visibleCount, kMaxThinRecords) * 6
  └─ glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)

… [many other GL ops between ComputeDispatch and DrawIndirect] …

DrawIndirect (txmmgr.cpp:1664)
  ├─ gos_terrain_bridge_drawIndirect (gameos_graphics.cpp:2456)
  │   ├─ Save GL state, rebind VAO (AMD VAO-0 trap), attr-0 hack
  │   ├─ terrainBindThinUniformsForPatchStream (uploads terrainMVP etc.)
  │   ├─ glBindBufferRange(SSBO 2, thinRecordSSBO, ringSlot*kThinRecordBytes, …)
  │   ├─ glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, cmdCount, 0)
  │   └─ Restore GL state
  └─ glFenceSync at ringSlot (line 2139) — populated only on success
```

- `kThinRingFrames = 3`, `kMaxThinRecords = 65536`.
- Ring slot advances exactly ONCE per frame in GPU mode (CPU pack path at line 1555 is mutually exclusive via the early-return at preflight:1872).
- Parity mode (`MC2_GPU_DRIVEN_PARITY=1`) advances the ring twice per frame via `ComputeDispatchParity_Check` calling `PackThinRecordsForFrame`. This disrupts fence/slot pairing — *but smoke-with-parity still renders correctly*, confirming the explicit fence is not load-bearing for correctness.

### Recipe SSBO is mission-static under rotation

Grepped all call sites of `InvalidateRecipeForVertexNum` / `InvalidateAllRecipes`:
- `mapdata.cpp:154,199,921` — destroy/newInit/calcLight (load/teardown only)
- `mapdata.cpp:1379` — setTerrain (gameplay terrain mutation)
- `mapdata.cpp:923` — at end of `calcLight()`, only when `Terrain::recalcLight && eye`
- `Terrain::recalcLight` set TRUE at: newInit, hotkey toggle (missiongui.cpp:2710), reCalcLight() (no callers — dead code)
- `Terrain::recalcShadows` set TRUE at: Terrain::save (terrain.cpp:2053) — save-game-only
- Recipe path **does not mutate** under camera rotation. H-E is dead.

## Hypotheses tested

| H | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H-A | 10ms fence timeout fires under rotation | **falsified under smoke** | 4185 frames, 650 unique MVPs, every wait returned `GL_ALREADY_SIGNALED` (0x911A), max wait was 4 µs |
| H-B | Fence-NULL on wrap → no wait → race | **partially falsified** | Parity-mode smoke runs with fence=0 every frame and still renders correctly — driver implicit sync covers it |
| H-C | Phase 1 lighting SSBO race | **not tested directly** but smoke parity (which checks lightRGB fields) showed 0 lightRGB mismatches across 1.8M quads-checked |
| H-D | Compute MVP ≠ bridge VS MVP | **likely dead** | Mech batcher uses same `gos_GetTerrainMVPMat4()` getter; mechs render fine; no setter between dispatch and draw |
| H-E | recalcShadows-driven recipe invalidate | **dead** | Save-game-only trigger |
| H-10 | atomicAdd overshoot past kMaxThinRecords | **falsified under smoke** | peak_visible=8595 / cap=65536 (factor of 7 headroom). Even 4x under rotation = ~34K still safe |
| H-cmdpatch | cmd-patch writes wrong cmd.count | **probe 3 ready** | Default-off until user repro |

## Parity check incidental findings (informational, NOT the rotation bug)

`MC2_GPU_DRIVEN_PARITY=1` smoke on mc2_01 produced:
- **0 `cpu_only` mismatches** — GPU set ⊇ CPU set (GPU iterates full recipe range; CPU iterates camera-windowed quadList — this is structural and expected)
- **44,197 `gpu_only=1` mismatches** — same structural cause as above (GPU sees quads outside CPU's quadList window)
- **43 `field=flags` mismatches** — same recipeIdx in both, but `pzTri1Valid`/`pzTri2Valid` differ between GPU and CPU. Reproducible per-recipe (e.g., recipe=110 flags GPU=0x7 CPU=0x5 for ~25 consecutive frames). These are **sub-ULP projection precision differences at frustum edges**, NOT a race — same MVP, same recipe data, but GPU's MVP*pos and CPU's pre-projected pz disagree by epsilon at the [0,1) gate boundary. Unrelated to the rotation bug. Worth a separate bug to flag, but not load-bearing.

## Visual mechanism (from VS shader audit — gos_terrain_thin.vert)

Two-stage projection:
```glsl
vec4 clip = terrainMVP * vec4(worldPos, 1.0);
float rhw = 1.0 / clip.w;
vec3 screen = { clip.x*rhw*viewport.x + …, clip.y*rhw*… , clip.z*rhw + 0.002 };
vec4 ndc = mvp * vec4(screen, 1.0);
float absW = abs(clip.w);
gl_Position = vec4(ndc.xyz * absW, absW);
```

For ONE triangle to span half the screen with grey atlas-banded fill:
- WorldPos varies linearly between corners across MANY atlas tiles (atlas-tile gradient → grey bands)
- At least one corner has `clip.w ≈ 0` (tiny), producing wild `rhw`, sending screen.xy to extreme values, but `absW` rescaling brings gl_Position back to near-corner of NDC — pinning one vertex to a screen corner

The pz-cull gate (`if pzValid==0 → degenerate`) is designed to suppress this — it culls triangles whose corners project outside [0,1). The bug = pzValid says "OK" for a corner whose actual projected position is wild.

**This implies the cull decision (in compute) and the projection (in VS) disagree on at least one frame under fast rotation.** Either:
- Compute reads MVP_A, VS reads MVP_B (mutation between dispatch and draw)
- Compute reads worldPos_A from recipe, VS reads worldPos_B (recipe mutation between dispatch and draw — but recipe is mission-static; ruled out)
- Compute writes thin record at slot S; VS reads same slot S but the data is partially overwritten by a NEXT-frame compute write (ring violation)

The third option is the strongest remaining candidate. But probes 1-2 show ring discipline holds under smoke. The user's manual fast-rotation has not yet been instrumented.

## Probes deployed in `mc2.exe` (already in `mc2-win64-v0.4/`)

All in `gos_terrain_indirect.cpp::ComputeDispatch()`. Gated by `MC2_RING_TRACE=1` env (per-frame trace); tripwires are always-on.

| Tripwire | Triggers when |
|---|---|
| `[RING_TRIPWIRE v1]` | fence missing past frame `2*kThinRingFrames` OR `glClientWaitSync` timeout |
| `[RING_OVERSHOOT v1]` | prev-frame `hdr.visibleCount >= kMaxThinRecords` |
| `[RING_CMDPATCH v1]` (MC2_RING_TRACE only) | indirect cmd's `count` != `min(visibleCount, kMaxThinRecords) * 6` |
| `[RING_PEAK v1]` | every 600 frames — peak visibleCount + overshoot count (always-on tripwire summary) |
| `[RING v1]` (MC2_RING_TRACE only) | per-frame full state |

## Recommended next steps (in order)

1. **Get the user's manual-repro log.** Without it, every probe is silent and we're guessing. `MC2_RING_TRACE=1 mc2.exe 2> ring_trace.log`, spinny-thing until streak, share `ring_trace.log`. **This is the bottleneck.**
2. If log shows `[RING_CMDPATCH v1]` at bug frames: cmd-patch race, fix via atomic clamp or barrier reorder.
3. If log shows `[RING_OVERSHOOT v1]`: bounded atomicAdd via compareAndSwap in compute shader.
4. If log shows `[RING_TRIPWIRE v1]`: extend timeout, OR grow ring (kThinRingFrames=4 or 5), OR always-create-fence-on-slot regardless of DrawIndirect success.
5. **If log is silent on all tripwires under a confirmed bug frame:** add probe 4 — fingerprint thin SSBO content per frame (small hash of first/last/middle records) and the recipe SSBO content. If thin hash differs between compute-write and VS-read, ring violation despite fence; if recipe hash drifts, something *is* mutating recipes I haven't found yet.

## Files touched this session

- `GameOS/gameos/gos_terrain_indirect.cpp` — added probes 1/2/3 around the fence-wait + cmd-patch barrier in `ComputeDispatch`. ~100 lines, all env-gated or tripwire-only. Default behavior unchanged.
- `scripts/run_smoke.py` — added `MC2_RING_TRACE` to env-passthrough list.
- `docs/superpowers/progress/2026-05-13-raster-triangle-probe-state.md` — this file.
