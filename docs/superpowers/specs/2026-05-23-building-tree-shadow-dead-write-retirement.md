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
Replace the existing `if` block for buildings with the probe below. Same pattern for trees. The probe wraps the OUTER candidate check and separately tracks whether the INNER transform body (`!gos_IsTerrainTessellationActive()`) was entered:

```cpp
// [ARC3_PROBE v1] temporary probe — remove before commit
static long long s_bldg_shadow_candidates    = 0;
static long long s_bldg_shadow_xform_entries = 0;
static long long s_bldg_tess_true            = 0;
static long long s_bldg_tess_false           = 0;

if (bldgShadowShape && useShadows) {
    ++s_bldg_shadow_candidates;

    const bool tessActive = gos_IsTerrainTessellationActive();
    if (tessActive) {
        ++s_bldg_tess_true;
    } else {
        ++s_bldg_tess_false;
        ++s_bldg_shadow_xform_entries;

        bldgShadowShape->SetRecalcShadows(checkShadows);
        bldgShadowShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
        bldgShadowShape->TransformMultiShape(&xlatPosition, &rot);
    }
}
```

Print all four counters to stderr every 300 frames. Run a 30s smoke of `mc2_01` (buildings and trees visible):
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```

**Decision gate (C1 — corrected):**

| Condition | Action |
|---|---|
| `candidates == 0` | Smoke did not exercise the path. Run a mission with visible buildings/trees in camera view. Do NOT treat as proof of safety. |
| `candidates > 0` AND `xform_entries == 0` AND `tess_false == 0` | **PROCEED.** Transform body was never entered. Deletion is dead-code cleanup. |
| `xform_entries > 0` OR `tess_false > 0` | **STOP.** Tessellation was inactive at least once. Identify the non-tessellation path before deleting. |

The key distinction: `candidates` counts objects that reached the outer check; `xform_entries` counts objects that entered the transform body. The probe must show `candidates > 0` (path exercised) AND `xform_entries == 0` (inner body never ran).

**Step 3 — Record probe results** in the commit message and spec (M1 update):
After a successful probe run, record the outcome verbatim in the commit message:
```
ARC3_PROBE result: candidates=N xform_entries=0 tess_false=0
Conclusion: dead-code cleanup only; no runtime CPU win claimed.
```

**Step 4 — Remove instrumentation** before the deletion commit. The deletion commit must contain no `[ARC3_PROBE v1]` markers.

### Task 1: Delete the two transform blocks

Delete both targets as described above (comment blocks + `if` blocks).

### Task 2: Post-deletion grep verification (M2)

Run all of the following. Expected match counts are noted per gate.

```bash
# Gate A: tessellation guard gone from bdactor.cpp — expect 0 matches
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp

# Gate B: TransformMultiShape removed from bdactor.cpp update paths — expect 0 matches
# (If TransformMultiShape appears in other bdactor functions unrelated to shadows, scope
#  the check: confirm no match inside BldgAppearance::update or TreeAppearance::update)
grep -n "bldgShadowShape->TransformMultiShape\|treeShadowShape->TransformMultiShape" mclib/bdactor.cpp

# Gate C: SetRecalcShadows removed from bdactor.cpp update paths — expect 0 matches
# (Same scoping note: only flag matches inside the update() functions, not lifecycle setup)
grep -n "bldgShadowShape->SetRecalcShadows\|treeShadowShape->SetRecalcShadows" mclib/bdactor.cpp
```

If any of Gates B or C return matches outside the `update()` functions (e.g. in a lifecycle setup path added later), that is acceptable — confirm the match is NOT inside `BldgAppearance::update` or `TreeAppearance::update` before marking the gate as passed.

Note: SetLightList is not gated because it legitimately appears in non-shadow lighting contexts.

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

Commit message must include the probe result recorded in Task 0 Step 3, and must NOT claim a runtime CPU win unless `xform_entries > 0` was observed:

```
retire dead building/tree blob shadow transform blocks (Arc 3)

ARC3_PROBE result: bldg candidates=N xform_entries=0 tess_false=0
                   tree candidates=N xform_entries=0 tess_false=0
Conclusion: dead-code cleanup only; no runtime CPU win claimed.
```

Do NOT use the phrase `~3µs/object/frame` or any runtime win language. The spec's background section already contains the original comment with that estimate; that comment is deleted along with the block.

---

## What This Does NOT Retire

- `bldgShadowShape` / `treeShadowShape` objects: still allocated, loaded, and destroyed. Lifecycle retirement is a later arc.
- `renderShadows()` function bodies: already early-return; kept as documentation. Future cleanup arc.
- Vehicle and mech blob shadow paths (`gvactor.cpp`, `mech3d.cpp`): live, untouched.
- `renderLists()` ISSHADOWS flush loop (`txmmgr.cpp:2407`): live, needed for vehicles/mechs.
- Global ISSHADOWS legacy queue infrastructure: untouched.
