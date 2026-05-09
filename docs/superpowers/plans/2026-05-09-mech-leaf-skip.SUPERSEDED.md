# Mech Leaf-Skip Implementation Plan (Slice D-leaf-skip)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Strip the per-leaf body of `mechShape->TransformMultiShape*` (per-leaf pool alloc + per-vertex screen-space projection + per-face backface cull, plus `MultiTransformShadows` dispatch) when `MC2_GPU_MECH_LEAF_SKIP=1` AND `g_useGpuMechs` AND `gos_IsTerrainTessellationActive()`. Recon (spec §Q1-Q5 + D-shadow-skip's prior audit) proved every per-leaf field on `mechShape` has zero consumer in modern + GPU mech mode (Slice A bypasses `Render(true)`, `RenderShadows(true)` is unreachable on tessellation, `submitActor` and `getNodePosition` read only `listOfShapes[i].shapeToWorld`).

**Architecture:** Add a thin `TransformMultiShape_HierarchyOnly` wrapper that reuses the existing `s_buildRecipeOnly` mechanism. Extend the body callsite (mech3d.cpp:3430-3434) from 2-way to 3-way: LEAF_SKIP > FAST_TRANSFORM > legacy. No header include changes. Arms, sensors, and shadow callsites untouched.

**Plan-time adversarial review verdict (pre-execution):** to be filled after Task 0.

**Tech Stack:** existing `s_buildRecipeOnly` flag + `TransformMultiShape_BuildRecipe` wrapper at `msl.cpp:1745-1746, 1804-1810`; existing GPU mech batcher killswitch infrastructure; `gos_IsTerrainTessellationActive()` already in scope at multiple sites.

**Spec:** [docs/superpowers/specs/2026-05-09-mech-leaf-skip-design.md](../specs/2026-05-09-mech-leaf-skip-design.md)

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/msl.h` | declare `TransformMultiShape_HierarchyOnly` near existing variants |
| Modify | `mclib/msl.cpp` | define wrapper (reuses `s_buildRecipeOnly`) |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def |
| Modify | `mclib/mech3d.cpp` | extend body conditional at 3430-3434 to 3-way |

---

## Task 0: Adversarial plan review (pre-execution)

**Files:** none (review only)

- [ ] **Step 0.1: Dispatch adversarial review per worktree CLAUDE.md "Review Discipline"** with `adversarial-plan-review` skill verbatim. Reviewer must grep-verify:
  - All consumers of `mechShape` per-leaf state in modern + GPU mech mode (Q1 above) — only `Render(true)` (gated by Slice A) and `RenderShadows(true)` (unreachable on tessellation).
  - `submitActor` reads only `listOfShapes[i].shapeToWorld.entries`, not per-leaf state (Q3).
  - `getNodePosition` reads only `shapeToWorld` (Q4).
  - Arms, sensors are separate `TG_MultiShape*` instances; this slice operates only on `mechShape`.
  - `s_buildRecipeOnly` wrapper semantics match the slice's needs (msl.cpp:1745-1746 `continue` skips per-leaf + MultiTransformShadows).
  - Race-safety: `s_buildRecipeOnly` set+clear inside the wrapper, single-threaded actor update loop.
  - First-frame nullity: pool fields NULL-init via `CreateFrom`; no consumer dereferences when LEAF_SKIP=1.
  - 3-way conditional precedence (LEAF_SKIP > FAST_TRANSFORM > legacy) handles all flag combinations correctly.

- [ ] **Step 0.2: Address findings inline.**

---

## Task 1: New `TransformMultiShape_HierarchyOnly` wrapper

**Files:**
- Modify: `mclib/msl.h`
- Modify: `mclib/msl.cpp`

- [ ] **Step 1.1: Add declaration to msl.h**

In `mclib/msl.h`, immediately after the existing `TransformMultiShape_BuildRecipe` declaration at line 392, add:

```cpp

		// Slice D-leaf-skip (2026-05-09): runtime alias of _BuildRecipe for the
		// GPU mech body callsite. Reuses s_buildRecipeOnly mechanism to skip
		// per-leaf dispatch + MultiTransformShadows; preserves the OUTER
		// hierarchy walk that populates listOfShapes[i].shapeToWorld (which
		// the GPU mech batcher's submitActor and getNodePosition read).
		// Aliased rather than reusing _BuildRecipe directly because the latter
		// is named for static-prop registry init use; a self-documenting name
		// at the mech runtime callsite is preferable to symbol-name reuse.
		long TransformMultiShape_HierarchyOnly (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot);
```

- [ ] **Step 1.2: Add definition to msl.cpp**

In `mclib/msl.cpp`, immediately after the existing `TransformMultiShape_BuildRecipe` definition at line 1810, add:

```cpp

//-------------------------------------------------------------------------------
// Slice D-leaf-skip (2026-05-09): thin runtime wrapper that reuses the
// s_buildRecipeOnly mechanism for the GPU mech body callsite. The flag's
// semantic effect (skip per-leaf dispatch + MultiTransformShadows) is exactly
// what the GPU mech path needs in modern + tessellation mode: Slice A bypasses
// mechShape->Render(true), and mechShape->RenderShadows(true) is unreachable
// on tessellation (mech3d.cpp:3054). All per-leaf state has zero reader in
// this configuration. Single-threaded-safe (set/clear inside the wrapper).
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_HierarchyOnly (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_buildRecipeOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_buildRecipeOnly = false;
    return result;
}
```

- [ ] **Step 1.3: Build clean**

```
/mc2-build
```

Expected: clean build; pure decl/def addition with no callers yet.

- [ ] **Step 1.4: Commit**

```bash
git add mclib/msl.h mclib/msl.cpp
git commit -m "feat(slice-d-leaf-skip): add TransformMultiShape_HierarchyOnly wrapper (alias of _BuildRecipe for GPU mech runtime)"
```

---

## Task 2: Killswitch declaration + definition

**Files:**
- Modify: `GameOS/gameos/gos_mech_killswitch.h`
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 2.1: Add extern decl to killswitch header**

In `GameOS/gameos/gos_mech_killswitch.h`, immediately after the existing `extern bool g_useGpuMechShadowStateStrip;` block, add:

```cpp

// Slice D-leaf-skip (2026-05-09): strip per-leaf body of mechShape->
// TransformMultiShape* (per-leaf pool alloc + per-vertex screen-space
// projection + per-face backface cull + MultiTransformShadows dispatch)
// when modern engine + GPU mech path is engaged. Recon proved every per-
// leaf field on mechShape has zero consumer in this configuration:
// Slice A bypasses mechShape->Render(true) (mech3d.cpp:2583), and
// mechShape->RenderShadows(true) is unreachable on tessellation
// (mech3d.cpp:3054). submitActor and getNodePosition read only the
// hierarchy-level shapeToWorld matrices, which the wrapper preserves.
// Independent of g_useGpuMechs / fast-transform / shadow-strip flags
// for bisect granularity. Requires g_useGpuMechs=true AND
// gos_IsTerrainTessellationActive() to take effect. NOT compatible
// with MC2_MECH_GPU_PARITY=1 — disable LEAF_SKIP if running parity
// diagnostic.
extern bool g_useGpuMechLeafSkip;
```

- [ ] **Step 2.2: Add global definition in gos_mech_batcher.cpp**

In `GameOS/gameos/gos_mech_batcher.cpp`, immediately after the existing `bool g_useGpuMechShadowStateStrip = ...;` line, add:

```cpp

// Slice D-leaf-skip: see gos_mech_killswitch.h.
bool g_useGpuMechLeafSkip = (getenv("MC2_GPU_MECH_LEAF_SKIP") != nullptr);
```

- [ ] **Step 2.3: Build clean**

```
/mc2-build
```

Expected: clean build, no callers yet.

- [ ] **Step 2.4: Commit**

```bash
git add GameOS/gameos/gos_mech_killswitch.h GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-d-leaf-skip): add g_useGpuMechLeafSkip killswitch (MC2_GPU_MECH_LEAF_SKIP env)"
```

---

## Task 3: 3-way conditional at body callsite

**Files:**
- Modify: `mclib/mech3d.cpp`

- [ ] **Step 3.1: Confirm callsite**

```bash
grep -n "g_useGpuMechFastTransform" mclib/mech3d.cpp
```

Expected: line 3430 inside `Mech3DAppearance::updateGeometry`. Adjust line numbers below if drift.

- [ ] **Step 3.2: Replace 2-way with 3-way**

Find:

```cpp
		mechShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice C3-revised: when GPU mech path is on AND fast-transform
		// killswitch is on, use _PositionsOnly to skip the per-vertex
		// CPU lighting kernel. Output of that kernel (listOfVertices[j].argb)
		// is only consumed by mechShape->Render(true) which Slice A
		// bypasses; GPU shader does its own lighting via calc_light().
		// BODY ONLY — arms (4459, 4543) and shadow (3377) explicitly
		// stay full TransformMultiShape; their Render(true) callers
		// still depend on the lighting bake.
		if (g_useGpuMechs && g_useGpuMechFastTransform) {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		}
```

Replace with:

```cpp
		mechShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice D-leaf-skip: when GPU mech path is on AND leaf-skip
		// killswitch is on AND tessellation is active, use the
		// HierarchyOnly variant to also skip the per-leaf dispatch
		// (per-leaf pool alloc + per-vertex screen projection + per-face
		// backface cull) and the MultiTransformShadows dispatch. Recon
		// proved every per-leaf field on mechShape has zero consumer in
		// modern + GPU mech mode: Slice A bypasses mechShape->Render(true)
		// (mech3d.cpp:2583), and mechShape->RenderShadows(true) is
		// unreachable on tessellation (mech3d.cpp:3054). submitActor and
		// getNodePosition read only listOfShapes[i].shapeToWorld which
		// the OUTER hierarchy walk (preserved) populates. See spec §Q1-Q5
		// for the full grep-verified consumer enumeration.
		//
		// Slice C3-revised (FAST_TRANSFORM): when LEAF_SKIP is off but
		// FAST_TRANSFORM is on, use _PositionsOnly to skip only the
		// per-vertex CPU lighting kernel. The .argb writes have no
		// consumer (Render(true) bypassed by Slice A); GPU shader does
		// its own lighting via calc_light().
		//
		// BODY ONLY — arms (mech3d.cpp:4498, :4582) and shadow (gated by
		// the prior conditional at :3382) stay on their existing paths;
		// arms' Render(true) runs unconditionally per body-slice CRIT-1
		// hazard.
		if (g_useGpuMechs && g_useGpuMechLeafSkip && gos_IsTerrainTessellationActive()) {
			mechShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &qRotation);
		} else if (g_useGpuMechs && g_useGpuMechFastTransform) {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		}
```

- [ ] **Step 3.3: Verify other callsites unchanged**

```bash
grep -n "TransformMultiShape" mclib/mech3d.cpp
```

Expected: shadow conditional at ~3398-3404 (D-shadow-skip 3-way, unchanged), sensor (~3618, 3624) and arms (~4498, 4582) still naked `TransformMultiShape` calls.

- [ ] **Step 3.4: Build clean (full relink)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
find build64 -name "mech3d.obj" -delete
find build64 -name "msl.obj" -delete
/mc2-build
```

Expected: link log shows "performing full link." (msl.cpp also touched in Task 1 — both objs cleared.)

- [ ] **Step 3.5: Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat(slice-d-leaf-skip): extend body callsite to 3-way (LEAF_SKIP > FAST_TRANSFORM > legacy) at mech3d.cpp:3430"
```

---

## Task 4: Smoke verification (mc2_10 only, 30s per user direction)

**Files:** none (validation only)

- [ ] **Step 4.1: Deploy build**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
```

- [ ] **Step 4.2: A — LEAF_SKIP=0 sentinel (current shipped baseline)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_SKIP=1 MC2_GPU_MECH_SHADOW_STATE_STRIP=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys.

- [ ] **Step 4.3: B — LEAF_SKIP=1 (this slice's target config)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_SKIP=1 MC2_GPU_MECH_SHADOW_STATE_STRIP=1 MC2_GPU_MECH_LEAF_SKIP=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, fallback_total=0.

- [ ] **Step 4.4: No commit step** — validation only.

---

## Task 5: 90s mc2_10 Tracy A/B (USER prompts)

**Files:** none. **DO NOT run unprompted.**

When the user prompts, run smoke B at 90s for Tracy capture. User attaches Tracy and posts histograms.

Expected:
- `Mech3D.UpdateGeometry` mean drops by ≥6µs/call from 22.52µs (D-shadow-state-strip baseline) toward ~16µs or lower.
- Lower bimodal peak compresses; mode shifts further down from 16.4µs.
- σ tightens (per-leaf variable-cost retired).
- `Units.Mechs` outer zone: ≥100µs/frame additional reduction.

---

## Task 6: Implementation adversarial review + memory pin

**Files:**
- Create: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_leaf_skip_shipped.md`
- Modify: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 6.1: Dispatch implementation adversarial review** with `adversarial-plan-review` skill verbatim. Scrutiny vectors:
  - Wrapper added correctly to msl.h + msl.cpp; reuses `s_buildRecipeOnly`.
  - 3-way conditional at body callsite; precedence LEAF_SKIP > FAST_TRANSFORM > legacy.
  - Body callsite only — shadow conditional at 3398-3404 unchanged; sensor + arm callsites untouched.
  - Killswitch independence with `g_useGpuMechs=0`, tessellation off.
  - MC2_MECH_GPU_PARITY incompatibility flagged in killswitch comment.

- [ ] **Step 6.2: Write memory file** with measured deltas. (DO NOT proceed to 6.3 until measurements substituted.)

- [ ] **Step 6.3: Add MEMORY.md index entry** after the D-shadow-state-strip entry under Rendering / shaders:

```markdown
- ⭐ [Mech leaf-skip shipped 2026-05-09 (Slice D-leaf-skip)](mech_leaf_skip_shipped.md) — strip mechShape->TransformMultiShape per-leaf (pool alloc + per-vertex projection + per-face cull + MultiTransformShadows) via MC2_GPU_MECH_LEAF_SKIP; <FILL IN µs/call delta>; combined stack pre-body→today now <FILL %>
```

- [ ] **Step 6.4: Commit memory updates.**

---

## Spec Coverage Check

| Spec section | Covered by task |
|---|---|
| Wrapper definition + decl | Task 1 |
| Killswitch | Task 2 |
| 3-way conditional | Task 3 |
| Tessellation runtime gate | Task 3.2 |
| Pixel-equivalence | Validated by Task 5.1 |
| Failure modes | Verified by Task 0 + Task 6.1 |
| Verification gate (mc2_10 30s + 90s Tracy) | Tasks 4, 5 |

## Type / Symbol Consistency

- `TransformMultiShape_HierarchyOnly` — declared 1.1, defined 1.2, called 3.2 ✓
- `g_useGpuMechLeafSkip` — declared 2.1, defined 2.2, read 3.2 ✓
- `MC2_GPU_MECH_LEAF_SKIP` env — defined only in 2.2 ✓
- `s_buildRecipeOnly` — pre-existing static; reused, not redeclared ✓
- `gos_IsTerrainTessellationActive` — already in scope at mech3d.cpp:3054, :3398 ✓

## Placeholder Scan

- No "TBD"s.
- Every code step has actual code.
- Every commit has its message.
- Memory file has explicit measurement placeholder.

Plan ready for adversarial review (Task 0).
