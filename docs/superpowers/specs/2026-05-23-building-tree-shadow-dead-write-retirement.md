# Building + Tree Blob Shadow Transform Retirement (Arc 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this spec.

**Date:** 2026-05-23
**HEAD at design time:** d15dab5
**Campaign:** MC_TextureManager dual-queue legacy retirement — Arc 3
**Predecessor:** Arc 2 (terrain DETAIL/ALPHA dead-write deletion, commit 6347f09)

---

## Goal

Delete the per-frame `TransformMultiShape` calls for building and tree blob shadow shapes in `mclib/bdactor.cpp`. These calls are gated by `!gos_IsTerrainTessellationActive()`, which is expected to be always `false` in normal in-mission gameplay (tessellation active, terrain material non-null). This arc retires the update-time building/tree blob shadow transform producers from the legacy path.

**Payoff framing (C1 — MUST be confirmed by preflight counter before claiming):**
- If the preflight counter shows **entries == 0** in-mission: this is a dead-code cleanup arc — syntactic removal of unreachable transform blocks. No runtime cost reduction claimed.
- If the preflight counter shows **entries > 0** in-mission: the `!gos_IsTerrainTessellationActive()` predicate is not always false as assumed. STOP and identify when tessellation is inactive before deleting.

The decision gate at preflight is load-bearing. Do not skip it.

---

## Background

`gos_IsTerrainTessellationActive()` (`GameOS/gameos/gameos_graphics.cpp:7068`) returns `g_gos_renderer && g_gos_renderer->getTerrainMaterial() != nullptr`. In normal in-mission gameplay the renderer is always initialized and the terrain material is always non-null, so this function is expected to unconditionally return `true` in-mission.

The building and tree shadow paths have **two independently-gated exits**, both of which are expected to fire in normal gameplay:

**Exit 1 — `update()` transform block** (the deletion target):
```
BldgAppearance::update()   [bdactor.cpp:2387]
  if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
    // → expected always false in-mission → TransformMultiShape never called
    // → shadowsVisible[] never set → addTriangle(ISSHADOWS) never fires
```

**Exit 2 — `renderShadows()` early-return** (already in place, NOT a deletion target):
```
BldgAppearance::renderShadows()   [bdactor.cpp:1862]
  if (gos_IsTerrainTessellationActive()) return NO_ERR
    // → fires independently → addVertices(ISSHADOWS) never fills
```

The same two-exit structure exists for trees (`TreeAppearance::update` at bdactor.cpp:4620 and `TreeAppearance::renderShadows` at bdactor.cpp:4450).

The `renderShadows()` early-returns are already correct and remain. Only the redundant transform blocks in `update()` are deleted — but only after the preflight counter confirms they are never entered.

### Editor / non-tessellation path decision (M1)

`gos_IsTerrainTessellationActive()` requires `g_gos_renderer` and a non-null terrain material. This may not hold in mission editor, loading screens, preview scenes, or tool modes where the terrain material has not been initialized.

**This arc explicitly does not preserve building/tree blob shadows in non-tessellation fallback paths (editor, loading screens, tool modes).** Those paths are unsupported for modern OpenGL release builds. The `renderShadows()` early-return gate is already in place for blob shadows in those paths; the transform block deletion is consistent with that existing policy. If a future path requires blob shadows in a non-tessellation context, that path must be gated independently.

---

## Architecture

**Files modified:**

| File | Change |
|---|---|
| `mclib/bdactor.cpp` | Delete 2 guarded transform blocks (buildings ~2387, trees ~4620) plus their preceding comment blocks |

**Files NOT modified:**

| File | Reason |
|---|---|
| `mclib/bdactor.cpp` — `renderShadows()` bodies | Already dead (tessellation early-return in place). Full body removal is a future cleanup arc. |
| `bldgShadowShape` / `treeShadowShape` lifecycle | Allocation, file-loading, destruction of shadow shape objects — separate arc. |
| `mclib/gvactor.cpp` — vehicle shadow path | Still live (`if (gvShadowShape && useShadows)` has no tessellation gate). Untouched. |
| `mclib/mech3d.cpp` — mech shadow path | Still live under default `g_useGpuMechs = false`. Untouched. |
| `mclib/txmmgr.cpp:2407` — ISSHADOWS flush loop | Still needed for vehicle/mech shadow draws. Untouched. |
| `mclib/tgl.cpp:3590` — shadow addTriangle loop | Shared with vehicles/mechs; gated by `totalShadows > 0`. Stays 0 for bldg/tree after this deletion. No change needed. |

**Scope statement (M3):** This arc retires the update-time building/tree shadow transform producers. It does NOT retire ISSHADOWS globally, does NOT retire shadow queue infrastructure, and does NOT retire vehicle/mech blob shadows.

---

## Deletion Targets (grep-verified at design time, HEAD d15dab5)

### Target 1: Building shadow transform — `bdactor.cpp:2387`

Delete the `if` block and the preceding comment block (lines ~2382-2393 total):

```cpp
		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active. BldgAppearance::renderShadows() at line 2010 already early-
		// returns under the same condition (gos_IsTerrainTessellationActive),
		// meaning bldgShadowShape's transformed state is never consumed in this
		// pipeline. Per-actor saving = 1 TransformMultiShape call per visible
		// building per frame (~3 µs of pure waste).
		if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			bldgShadowShape->SetRecalcShadows(checkShadows);
			bldgShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			bldgShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

### Target 2: Tree shadow transform — `bdactor.cpp:4620`

Same pattern — delete the `if` block and preceding comment block (lines ~4615-4626 total):

```cpp
		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active — same rationale as BldgAppearance::update. TreeAppearance::
		// renderShadows() at line 4544 already early-returns under the same
		// condition; treeShadowShape's transformed state is never consumed.
		if (treeShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			treeShadowShape->SetRecalcShadows(checkShadows);
			treeShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			treeShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

---

## Verification Gates

### Task 0: Pre-flight — confirm deletion targets and instrument entry counters (C1)

**Step 1 — Confirm targets exist:**
```bash
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```
Expected: exactly 2 matches (one in BldgAppearance::update, one in TreeAppearance::update).

**Step 2 — Instrument and count entries.**
Add temporary `[ARC3_PROBE v1]` counters gated by an env var (e.g. `MC2_ARC3_PROBE=1`) in both `if` blocks before the condition:

```cpp
// [ARC3_PROBE v1] entry counter — remove before commit
static long long s_bldg_shadow_entries = 0;
if (bldgShadowShape && useShadows) {
    ++s_bldg_shadow_entries;
    if (!gos_IsTerrainTessellationActive()) {
        // ... existing transform body
    }
}
```

Print the counter at frame intervals (e.g. every 300 frames) to stderr. Run a 30s tier1 smoke (`mc2_01`) with `MC2_ARC3_PROBE=1`.

**Decision gate:**
- `bldg_shadow_entries == 0` and `tree_shadow_entries == 0`: proceed. Restate goal as dead-code cleanup, not perf win.
- Any counter `> 0`: STOP. Tessellation is not always active. Identify the path before deleting.

Do NOT proceed to deletion until entries are confirmed zero.

**Step 3 — Remove instrumentation before deletion commit.**

### Task 1: Delete the two transform blocks

Delete both targets as described above (comment blocks + `if` blocks).

### Task 2: Post-deletion grep verification (M2)

Run all of the following; each must return 0 matches:

```bash
# Primary gate: active tessellation guard gone
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp

# Confirm TransformMultiShape removed from update paths
grep -n "bldgShadowShape->TransformMultiShape\|treeShadowShape->TransformMultiShape" mclib/bdactor.cpp

# Confirm SetRecalcShadows removed from update paths
grep -n "bldgShadowShape->SetRecalcShadows\|treeShadowShape->SetRecalcShadows" mclib/bdactor.cpp
```

Note: SetLightList grep is not listed because it may appear in other (non-shadow) lighting contexts. Focus on the shadow-specific methods.

### Task 3: Full clean build + smoke (m2, M4)

**Step 1 — Full relink** (shadow shape lifecycle structs may be affected by inline changes):
```bash
rm build64/RelWithDebInfo/mc2.exe
cmake --build build64 --config RelWithDebInfo
```

**Step 2 — Deploy:**
```bash
# per mc2-deploy skill: cp -f each file individually, never cp -r
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
cp -f build64/RelWithDebInfo/mc2.pdb A:/Games/mc2-opengl/mc2-win64-water/mc2.pdb
```

**Step 3 — Tier1 5/5 smoke:**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```
Pass criteria: exit 0.

**Step 4 — Visual canary (M4).** Use `mc2_01` or `mc2_03` (both have visible buildings and trees). Confirm:
- Buildings render normally (geometry, texture, lighting).
- Trees render normally.
- No building/tree blob shadow visual is expected — these were already suppressed by the `renderShadows()` tessellation early-return before this arc.
- If any vehicle or mech blob shadows are visible in the mission, confirm they are unchanged.

### Task 4: Commit (m3)

Commit message must reflect the preflight outcome:

- If entries were confirmed zero (expected path):
  ```
  retire dead building/tree blob shadow transform blocks (Arc 3)
  ```
- Only if entries were non-zero and an explicit gate was added instead:
  ```
  gate building/tree blob shadow transforms behind tessellation predicate
  ```

Do NOT claim `~3µs/object/frame` in the commit message unless the preflight counter showed entries > 0 running real work.

---

## What This Does NOT Retire

- `bldgShadowShape` / `treeShadowShape` objects: still allocated, loaded, and destroyed. Lifecycle retirement is a later arc.
- `renderShadows()` function bodies: already early-return; kept as documentation. Future cleanup arc.
- Vehicle and mech blob shadow paths (`gvactor.cpp`, `mech3d.cpp`): live, untouched.
- `renderLists()` ISSHADOWS flush loop (`txmmgr.cpp:2407`): live, needed for vehicles/mechs.
- Global ISSHADOWS legacy queue infrastructure: untouched.
