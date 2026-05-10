# Handoff: Vehicle Sensor-Skip + Shadow-Skip (Combined Slice)

> **Purpose:** self-contained prompt for a fresh Claude Code session to plan + execute a combined slice that ports two mech-side optimizations (sensor-skip and shadow-shape-skip) to vehicles. Pre-loaded with campaign context, recon basis, things-that-will-transfer, things-that-won't-transfer, and discipline lessons learned the hard way during the mech campaign.

## TL;DR for the new session

1. **Branch:** `claude/gpu-mech-batcher` in `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\`. The mech slice campaign just finished (10 mech slices + default-on flip 2026-05-09). Read this worktree's `CLAUDE.md` for project rules first.
2. **Your job:** apply two patterns from the mech campaign to vehicles in `mclib/gvactor.cpp`:
   - **Sensor-skip** (mirror of D-sensor-skip — see [`memory/mech_sensor_skip_shipped.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_sensor_skip_shipped.md))
   - **Shadow-shape-skip** (mirror of D-shadow-skip — see [`memory/mech_shadow_skip_shipped.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_shadow_skip_shipped.md))
3. **Two new killswitches** following the post-flip default-on convention (set X=0 to opt out):
   - `MC2_GV_SENSOR_SKIP` — gate inversion at gvactor.cpp sensor block
   - `MC2_GV_SHADOW_SKIP` — gate the gvShadowShape transform call when modern engine has tessellation active
4. **Combined into ONE slice.** Patterns are tiny; recon is shared; single commit easier to review.
5. **Default-on from start** — convention established by the just-shipped default-on flip slice. Use the `envFlagDefaultOn(...)` helper in `gos_mech_batcher.cpp` (or factor a similar helper if you put the killswitch in a different file).

## Campaign context (what just shipped)

Branch state at handoff time (commit `9f267d4` is HEAD):

| Slice | Killswitch | What it does |
|---|---|---|
| Slice A | `MC2_GPU_MECHS` | GPU mech batcher foundation — replaces `mechShape->Render(true)` per-mech CPU vertex submit |
| B1 | `MC2_GPU_MECH_LIGHTING` | VS-side calc_light in mech.vert |
| C1 | `MC2_GPU_MECH_CULL` | render-only mech GPU cull |
| C2 | `MC2_GPU_MECH_SKIN` | weighted multi-bone skinning |
| C3-revised | `MC2_GPU_MECH_FAST_TRANSFORM` | body fast-transform (`_PositionsOnly` skips per-vertex lighting bake) |
| C3-shadow | `MC2_GPU_MECH_SHADOW_FAST_TRANSFORM` | shadow shape fast-transform |
| D-shadow-skip | `MC2_GPU_MECH_SHADOW_SKIP` | skip mechShadowShape->TransformMultiShape entirely |
| D-shadow-state-strip | `MC2_GPU_MECH_SHADOW_STATE_STRIP` | skip 4 state setters on mechShadowShape |
| D-leaf-skip-v2 | `MC2_GPU_MECH_LEAF_SKIP` | TransformMultiShape_HierarchyOnly for body |
| D-sensor-skip | `MC2_GPU_MECH_SENSOR_SKIP` | skip sensor block when sensorLevel ∈ {0, 5} |
| Default-on flip | (all 10 above) | flipped from default-off opt-in to default-on (X=0 opts out) |

Combined slice stack delta on `Mech3D.UpdateGeometry`: **71µs/call → 14.08µs/call (-80%)** on mc2_10 idle.

## Things that WILL transfer cleanly

### Sensor-skip (D-sensor-skip mirror)

**Mech precedent:** `mech3d.cpp:3637-3667` — sensor block wrapped in skip predicate `(sensorLevel == 0 || sensorLevel >= 5)`. Render gate at `mech3d.cpp:2948-2960` (`sensorLevel > 0 && sensorLevel < 5`).

**Vehicle target:** `gvactor.cpp:2660-2670` — sensorTriangleShape + sensorCircleShape TransformMultiShape calls. Note: vehicles use **`sensorCircleShape`** where mechs use `sensorSquareShape` (different name, same role).

**Vehicle render gate:** `gvactor.cpp:2329-2334`:
```cpp
if ((sensorLevel > 0) && (sensorLevel < 5))
{
    if (sensorLevel > 1)
        sensorCircleShape->Render();
    else
        sensorTriangleShape->Render();
}
```

**Identical pattern to mechs.** Skip gate inversion is the same: skip when `sensorLevel == 0 || sensorLevel >= 5`. PerPolySelect on sensor shapes does not exist for vehicles either (`GVAppearance::PerPolySelect` only calls `gvShape->PerPolySelect`, never the sensor shapes).

### Shadow-shape-skip (D-shadow-skip mirror)

**Mech precedent:** `mech3d.cpp:3398-3404` — `mechShadowShape->TransformMultiShape*` skipped under `MC2_GPU_MECH_SHADOW_SKIP=1 && g_useGpuMechs && gos_IsTerrainTessellationActive()`. Recon basis: `Mech3DAppearance::renderShadows` early-returns on tessellation at `mech3d.cpp:3054`.

**Vehicle target:** `gvactor.cpp:2489` — `gvShadowShape->TransformMultiShape (&xlatPosition, &rot)`.

**Vehicle render path:** `GVAppearance::renderShadows` at `gvactor.cpp:2065-2087`:
- Line 2068: `if (gos_IsTerrainTessellationActive()) return NO_ERR;` — **identical early-return to mech path**.
- Line 2082: `gvShadowShape->RenderShadows(true)` unreachable on tessellation, exactly like mech equivalent.

**Recon basis directly transfers.** Modern engine has terrain tessellation active by default → `gvShadowShape`'s TransformMultiShape outputs have zero consumer. Skip gate `g_useGV*Shadow* && gos_IsTerrainTessellationActive()` is identical to the mech version.

**Vehicle equivalent of MC2_GPU_MECHS gating:** vehicles do NOT have a GpuVehicleBatcher (see "won't transfer" below). The gate should NOT require `g_useGpuMechs` since this slice operates on vehicles, not mechs. The simpler pattern is just `g_useGVShadowSkip && gos_IsTerrainTessellationActive()`.

### Tracy zone instrumentation

The mech campaign added 8 Tracy sub-zones for `Mech3D.UpdateGeometry` attribution (D-gpu-pose-instrument). For vehicles, you may want a similar pass first to verify the cost-center claim. Pattern: see `mech3d.cpp` for `ZoneScopedN("Mech3D.UpdateGeometry.Sensors")` usage, mirror to GV.

**However** — vehicles are likely a much smaller per-frame cost center than mechs (fewer active vehicles per mission). You may decide instrumentation first, OR you may decide to ship the sensor+shadow skip patterns directly since they're known-clean copy-overs. Use your judgment based on what mc2_10 / mc2_24 actually shows for vehicle counts.

## Things that WON'T transfer

### Body leaf-skip / hierarchy-only path

**Why blocked:** vehicles have NO GpuVehicleBatcher equivalent of Slice A. `gvShape->Render(true)` at `gvactor.cpp:2131` IS in the live render path with no bypass — it consumes per-leaf state. Stripping per-leaf with leaf-skip semantics would silently render vehicles invisible (Render's null-check / lastTurnTransformed gate would fail).

**Don't try this.** It's the same class of CRIT (theoretical-vs-practical) that almost cost a slice for mechs, but for vehicles the consumer IS reachable in normal play.

### Body fast-transform (`_PositionsOnly`)

**Why blocked:** mech's `_PositionsOnly` strips the per-vertex CPU lighting kernel. Mechs have a GPU-side calc_light (Slice B1) replacing the work. Vehicles do NOT have a GPU vertex shader doing lighting — `gvShape->Render(true)` consumes the per-vertex `argb` field that the lighting kernel populates. Stripping it produces flat-color vehicles. **Don't try this.**

### Shadow-state-strip (D-shadow-state-strip mirror)

**Maybe applicable, low priority.** Vehicles likely have similar `setAnimation`/`SetFrameNum`/`SetNodeRotation`/`SetLightList` calls on `gvShadowShape`. Mech version saved ~1µs/call. For vehicles with much smaller population, gain is in the noise. **Defer.** Could add as a separate small slice if vehicle population turns out unexpectedly high.

### CPU mover-pick (PerPolySelect on vehicles)

Same finding as mechs: `findMoverByMouse` rect-only path catches all real vehicle selection. `GVAppearance::PerPolySelect` is theoretically reachable via fallback `findObjectByMouse` but only when click is geometrically NOT on any mover. **Theoretical-only.** Don't let any prior recon claim "vehicle pick requires per-leaf state" gate any slice without grep'ing the caller chain.

## Critical discipline lessons (don't repeat the mech campaign's mistakes)

### Lesson 1 — Negative call-chain claims need TWO grep passes

When a recon says "X requires Y so we can't strip Y," verify BOTH:
1. **Y is required by X** (positive — grep X's preconditions). 
2. **X is reachable in real play** (negative — grep X's caller chain from real entry points).

The mech campaign blew Lesson 1 the hard way: `Mech3DAppearance::PerPolySelect` requires per-leaf state (#1 true), but PerPolySelect is unreachable for mechs in real play because `findMoverByMouse` is rect-only (#2 — original review missed this; cost a rolled-back slice + a scrapped slice + a multi-day detour). See [`memory/mech_leaf_skip_v2_shipped.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_leaf_skip_v2_shipped.md) for the full backstory.

For vehicles: `GVAppearance::PerPolySelect` has the same property (theoretical-only reachability). Don't assume otherwise without grep'ing `findMoverByMouse` calling pattern at `objmgr.cpp:2468-2566`.

### Lesson 2 — Cost-center estimates need empirical confirmation

The mech campaign rolled back D-body-shadow-skip after the work it skipped (`MultiTransformShadows` on body) turned out to be smaller than estimated. Recon was correct (no consumer in modern + GPU mech mode), but the WORK was small. Slice didn't pay for itself.

For vehicles: estimate the work cost via instrumentation OR Tracy data BEFORE writing the slice. Don't ship code that retires X µs of work when X is small enough to be in measurement noise.

### Lesson 3 — Bimodality / trimodality drives slice choice

The mech instrument slice surfaced trimodal `Mech3D.UpdateGeometry` histogram (~17/30/70µs peaks). The trimodal pattern decomposed into separate stages (BodyXform bimodal, Sensors bimodal, Arms ≈ 0). That decomposition determined which stages to target.

For vehicles: if you instrument first, look at the histogram for the per-stage attribution. If sensors / shadow shape are the dominant cost, the slice is well-justified; if everything is roughly flat at small magnitudes, the slice's perf justification is weak.

### Lesson 4 — Default-on convention

The just-shipped default-on flip established the convention: `envFlagDefaultOn` helper, `X=0` opts out, header banner block documents the inversion. **Follow that convention from the start of this slice.** Don't ship default-off opt-in for vehicle flags then have to re-flip later.

If you put the vehicle killswitches in a new file (e.g., `gos_gv_killswitch.h`), the convention propagates: copy the banner block + use `envFlagDefaultOn`. If you put them in the same file as mech ones, just add to the existing pattern.

### Lesson 5 — Spec ceremony level

Mech campaign found that small slices (sensor-skip, shadow-skip) ship clean with fast-track ceremony (no formal spec/plan/review pass). The vehicle slice is even smaller (one file, two patterns, ~30 lines). **Fast-track is appropriate.**

If you want a spec for the historical record, write it after the smoke + Tracy data is in (slice's actual delta is the load-bearing claim).

## Worktree CLAUDE.md key rules (read in full at `.claude/worktrees/gpu-mech-batcher/CLAUDE.md`)

- Build: ALWAYS `--config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.
- Full relink before deploy if touching load-bearing functions: `rm -f build64/RelWithDebInfo/mc2.exe && find build64 -name "<changed>.obj" -delete` then build.
- Deploy: NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`.
- Smoke gate: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe ...`
- Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe`

## Recommended cadence for the vehicle slice

Mirror the mech sensor-skip + shadow-skip pattern but combined:

1. **Recon first.** Read `gvactor.cpp:2065-2087` (renderShadows), `:2329-2334` (sensor render gate), `:2489` (shadow shape transform), `:2660-2670` (sensor transforms). Confirm the patterns match what this handoff describes.

2. **Optional instrument pass.** Add 2 Tracy sub-zones for vehicle update geometry (`GVAppearance.UpdateGeometry.Sensors` and `.ShadowXform`) to quantify per-frame vehicle cost. Skippable if you trust the mech-precedent perf shape.

3. **Implement combined slice:**
   - Add 2 killswitch decls to `gos_mech_killswitch.h` (or a new `gos_gv_killswitch.h` — judgment call). Use `envFlagDefaultOn` for default-on semantics.
   - Add 2 env-var defs in `gos_mech_batcher.cpp` (or matching new file).
   - Wrap sensor block in `gvactor.cpp:2660-2670` with `(sensorLevel == 0 || sensorLevel >= 5)` skip predicate.
   - Wrap `gvShape*->TransformMultiShape` at `gvactor.cpp:2489` (the shadow shape one) with `gos_IsTerrainTessellationActive()` skip predicate.

4. **Smoke matrix:**
   - A: NEW DEFAULT (no env vars) — confirms default-on works.
   - B: `MC2_GV_SENSOR_SKIP=0 MC2_GV_SHADOW_SKIP=0` — legacy parity sentinel.
   - Tier1 5/5 at NEW DEFAULT — confirms no regression across all missions.

5. **Tracy A/B (optional given small expected delta):** mc2_10 90s with vs without the new flags. Look for any GVAppearance equivalent zone (or just `GameLogic.Units.Mechs` / outer scopes) showing the small delta.

6. **Memory pin** — write `gv_sensor_shadow_skip_shipped.md` with the slice's deltas. Link from `MEMORY.md` index.

## Files referenced (absolute paths)

- Worktree: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\`
- Vehicle code: `mclib/gvactor.cpp`
- Mech sensor-skip precedent: `mclib/mech3d.cpp:3637-3667`
- Mech shadow-skip precedent: `mclib/mech3d.cpp:3398-3404`
- Killswitch infrastructure: `GameOS/gameos/gos_mech_killswitch.h` + `gos_mech_batcher.cpp`
- `envFlagDefaultOn` helper: `gos_mech_batcher.cpp:23-29` (use this; do not reimplement)
- Mech sensor-skip memory: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_sensor_skip_shipped.md`
- Mech shadow-skip memory: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_shadow_skip_shipped.md`
- Default-on flip memory: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_default_on_flip_shipped.md`
- Leaf-skip-v2 memory (for the lesson-1 backstory): `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_leaf_skip_v2_shipped.md`

## Fresh-session entry prompt (copy-paste this)

```
Read the handoff at A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\docs\superpowers\plans\progress\2026-05-09-handoff-vehicle-sensor-shadow-skip.md and execute the combined vehicle sensor-skip + shadow-shape-skip slice per its cadence. Worktree is set up; killswitch infrastructure exists; precedent slices shipped. Default-on from start. Mirror the mech patterns; do NOT attempt body leaf-skip or fast-transform (vehicles have no GPU batcher equivalent). Discipline lessons in the handoff are load-bearing — read them.
```
