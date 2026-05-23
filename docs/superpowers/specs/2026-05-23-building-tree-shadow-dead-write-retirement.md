# Building + Tree Blob Shadow Dead-Write Retirement

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this spec.

**Date:** 2026-05-23
**HEAD at design time:** d15dab5
**Campaign:** MC_TextureManager dual-queue legacy retirement — Arc 3
**Predecessor:** Arc 2 (terrain DETAIL/ALPHA dead-write deletion, commit 6347f09)

---

## Goal

Delete the per-frame `TransformMultiShape` calls for building and tree blob shadow shapes in `mclib/bdactor.cpp`. These calls are gated dead by `!gos_IsTerrainTessellationActive()`, which is always `false` in-mission (tessellation is always active in the modern OpenGL path). The deletion removes ~3µs/visible-building-or-tree/frame of wasted CPU work and eliminates the final building/tree `MC2_ISSHADOWS` write paths to the legacy `masterVertexNodes` queue.

---

## Background

`gos_IsTerrainTessellationActive()` (`GameOS/gameos/gameos_graphics.cpp:7068`) returns `g_gos_renderer && g_gos_renderer->getTerrainMaterial() != nullptr`. In normal in-mission gameplay the renderer is always initialized and the terrain material is always non-null, so this function unconditionally returns `true`.

The building and tree shadow paths have **two independently-gated exits**, both of which already fire:

**Exit 1 — `update()` transform block** (the deletion target):
```
BldgAppearance::update()   [bdactor.cpp:2387]
  if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
    // → always false in-mission → TransformMultiShape never called
    // → shadowsVisible[] never set on bldgShadowShape / its children
    // → TG_Shape::Render() loop: totalShadows = 0 → addTriangle(ISSHADOWS) never fires
```

**Exit 2 — `renderShadows()` early-return** (already in place, NOT a deletion target):
```
BldgAppearance::renderShadows()   [bdactor.cpp:1862]
  if (gos_IsTerrainTessellationActive()) return NO_ERR
    // → always fires → RenderShadows() never called → addVertices(ISSHADOWS) never fills
```

The same two-exit structure exists for trees (`TreeAppearance::update` at bdactor.cpp:4620 and `TreeAppearance::renderShadows` at bdactor.cpp:4450).

The `renderShadows()` early-returns are already correct and remain. Only the redundant transform blocks in `update()` are deleted.

---

## Architecture

**Files modified:**

| File | Change |
|---|---|
| `mclib/bdactor.cpp` | Delete 2 guarded transform blocks (buildings ~2387, trees ~4620) |

**Files NOT modified:**

| File | Reason |
|---|---|
| `mclib/bdactor.cpp` — `renderShadows()` bodies | Already dead (tessellation early-return in place). Full body removal is a future cleanup arc. |
| `bldgShadowShape` / `treeShadowShape` lifecycle | Allocation, file-loading, and destruction of the shadow shape objects — separate arc. |
| `mclib/gvactor.cpp` — vehicle shadow path | Still live (`if (gvShadowShape && useShadows)` has no tessellation gate). Untouched. |
| `mclib/mech3d.cpp` — mech shadow path | Still live under default `g_useGpuMechs = false`. Untouched. |
| `mclib/txmmgr.cpp:2407` — ISSHADOWS flush loop | Still needed for vehicle/mech shadow draws. Untouched. |
| `mclib/tgl.cpp:3590` — shadow addTriangle loop | Shared with vehicles/mechs; gated by `totalShadows > 0` which stays 0 for bldg/tree after this deletion. No change needed. |

---

## Deletion Targets (grep-verified at design time, HEAD d15dab5)

### Target 1: Building shadow transform — `bdactor.cpp:2387`

Surrounding context (the outer `if` and its body, 4 lines total to delete including the `if` line):

```cpp
		if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			bldgShadowShape->SetRecalcShadows(checkShadows);
			bldgShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			bldgShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

The comment block immediately before this `if` (lines 2382-2386) is also deleted — it documents the rationale for the guard that is being removed.

### Target 2: Tree shadow transform — `bdactor.cpp:4620`

Same pattern:

```cpp
		if (treeShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			treeShadowShape->SetRecalcShadows(checkShadows);
			treeShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			treeShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

The comment block immediately before this `if` (lines 4615-4619) is also deleted.

---

## Verification Gates

### Pre-flight (Task 0)
Confirm the deletion targets exist at expected locations:
```bash
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```
Expected: exactly 2 matches (one in BldgAppearance::update, one in TreeAppearance::update).

### Hard gate after deletion (Task 3)
```bash
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```
Expected: **0 matches**. If any match remains, the deletion is incomplete.

### Smoke gate (Task 4)
Tier1 5/5 smoke with the modified exe:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```
Pass criteria: exit 0, buildings and trees render normally, no new failures vs HEAD d15dab5.

---

## What This Does NOT Retire

- The `bldgShadowShape` and `treeShadowShape` objects themselves: still allocated, loaded from `.tga`/shape files, and destroyed. Their memory footprint and file-load time are unaffected by this arc. Full shadow shape lifecycle retirement is a later arc.
- The `renderShadows()` function bodies in both appearance classes: already early-return, kept as documentation. Can be collapsed to `return NO_ERR` in a future cleanup arc.
- Vehicle and mech blob shadow paths: live, untouched. A future arc will address these after GPU shadow coverage for dynamic actors is confirmed.
- The `renderLists()` ISSHADOWS flush loop (`txmmgr.cpp:2407`): still runs, still needed for vehicle/mech draws.
