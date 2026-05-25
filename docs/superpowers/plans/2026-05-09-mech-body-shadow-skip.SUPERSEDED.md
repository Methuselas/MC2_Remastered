# Mech Body Shadow-Projection Skip Plan (Slice D-body-shadow-skip)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans.

**Goal:** Skip `MultiTransformShadows` dispatch on the body shape (`mechShape`) when `MC2_GPU_MECH_BODY_SHADOW_SKIP=1` AND `g_useGpuMechs` AND `gos_IsTerrainTessellationActive()`. Preserve all of `_PositionsOnly`'s per-leaf work (so PerPolySelect's mouse-pick contract stays intact).

**Architecture:** New static flag `s_skipMultiTransformShadows` checked at the `MultiTransformShadows` dispatch site (msl.cpp:1763). New wrapper `TransformMultiShape_PositionsOnlyNoShadowProj` composes existing `s_multiShapePositionsOnly` with the new flag. Body callsite (mech3d.cpp:3430) extended from 2-way to 3-way: BODY_SHADOW_SKIP > FAST_TRANSFORM > legacy. Arms/sensors/shadow callsites untouched.

**Replaces:** the superseded D-leaf-skip slice (its docs at `*.SUPERSEDED.md`) which would have broken `Mech3DAppearance::PerPolySelect` mouse-pick (CRIT-1 from plan-time review).

**Plan-time adversarial review verdict:** READY FOR EXECUTION (2026-05-09). Reviewer confirmed CRIT-1 mitigation holds (per-leaf pool allocs + per-vertex projection survive; only per-light × per-vertex shadow content writes skipped). 5 MINOR addressed; no CRITICAL/MAJOR.

**Spec:** [docs/superpowers/specs/2026-05-09-mech-body-shadow-skip-design.md](../specs/2026-05-09-mech-body-shadow-skip-design.md)

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/msl.h` | declare `TransformMultiShape_PositionsOnlyNoShadowProj` |
| Modify | `mclib/msl.cpp` | add `s_skipMultiTransformShadows` static; gate the `MultiTransformShadows` dispatch at line 1763; define wrapper |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def |
| Modify | `mclib/mech3d.cpp` | extend body callsite at 3430 to 3-way |

---

## Task 0: Adversarial plan review (pre-execution) — DONE

**Files:** none

- [x] Completed 2026-05-09. Verdict: READY FOR EXECUTION. Skip below — left for the historical record.

- [ ] **Step 0.1: Dispatch with `adversarial-plan-review` skill verbatim.** Reviewer must:
  - Confirm `_PositionsOnly` populates everything PerPolySelect needs (per-leaf pool alloc + per-vertex screen projection + per-face cull + lastTurnTransformed bump). Cross-check against tglpp.cpp:10-33.
  - Confirm `MultiTransformShadows` is the ONLY consumer of `mechShape`'s shadow vertex pools (`listOfShadowTVertices` etc.) reachable in modern + GPU mech mode. Re-grep.
  - Confirm gate `&& !s_skipMultiTransformShadows` at msl.cpp:1763 doesn't affect existing callers (Track B's `_BuildRecipe` already skips via `continue` at 1746, before reaching this gate; the body slice's `_PositionsOnly` doesn't set the new flag, so it dispatches normally; legacy `TransformMultiShape` path doesn't set the flag).
  - Confirm killswitch independence and 3-way precedence at the body callsite.
  - Confirm name disambiguation between `MC2_GPU_MECH_SHADOW_SKIP` (existing, for `mechShadowShape`) and the new `MC2_GPU_MECH_BODY_SHADOW_SKIP` (this slice, for `mechShape`'s MultiTransformShadows).

- [ ] **Step 0.2: Address findings inline.**

---

## Task 1: New flag + wrapper

**Files:**
- Modify: `mclib/msl.h`
- Modify: `mclib/msl.cpp`

- [ ] **Step 1.1: Add static flag in msl.cpp**

Find the existing `s_buildRecipeOnly` and `s_multiShapePositionsOnly` declarations near the file top (search `s_buildRecipeOnly\s*=`). Add:

```cpp
// Slice D-body-shadow-skip (2026-05-09): gates the unconditional
// MultiTransformShadows dispatch in TransformMultiShape's per-shape loop
// (msl.cpp:1763) so the body shape's per-light × per-vertex shadow projection
// can be skipped when no consumer exists (modern + GPU mech mode). Set/cleared
// inside the _PositionsOnlyNoShadowProj wrapper. Single-threaded.
static bool s_skipMultiTransformShadows = false;
```

- [ ] **Step 1.2: Gate the MultiTransformShadows dispatch at line 1763**

Find:

```cpp
        if (useShadows && d_useShadows)
        {
            listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld),yawRotation);
        }
```

Replace with:

```cpp
        if (useShadows && d_useShadows && !s_skipMultiTransformShadows)
        {
            listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld),yawRotation);
        }
```

- [ ] **Step 1.3: Add declaration in msl.h**

Immediately after the existing `TransformMultiShape_BuildRecipe` declaration at line 392, add:

```cpp

		// Slice D-body-shadow-skip (2026-05-09): composes _PositionsOnly with
		// skipping the unconditional MultiTransformShadows dispatch. The body's
		// MultiTransformShadows outputs are consumed only by RenderShadows, which
		// is unreachable on tessellation (mech3d.cpp:3054 early-return).
		// PerPolySelect's contract is preserved by the underlying _PositionsOnly
		// mechanism (per-leaf pool alloc + per-vertex projection + per-face cull
		// + lastTurnTransformed bump). NOT compatible with MC2_MECH_GPU_PARITY=1.
		long TransformMultiShape_PositionsOnlyNoShadowProj (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot);
```

- [ ] **Step 1.4: Define wrapper in msl.cpp**

Immediately after the existing `TransformMultiShape_BuildRecipe` definition at line 1810, add:

```cpp

//-------------------------------------------------------------------------------
// Slice D-body-shadow-skip (2026-05-09): composes _PositionsOnly with skipping
// MultiTransformShadows dispatch. See msl.h for full rationale.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_PositionsOnlyNoShadowProj (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_multiShapePositionsOnly   = true;
    s_skipMultiTransformShadows = true;
    long result = TransformMultiShape(pos, rot);
    s_skipMultiTransformShadows = false;
    s_multiShapePositionsOnly   = false;
    return result;
}
```

- [ ] **Step 1.5: Build clean**

```
/mc2-build
```

Expected: clean build; no callers yet.

- [ ] **Step 1.6: Commit**

```bash
git add mclib/msl.h mclib/msl.cpp
git commit -m "feat(slice-d-body-shadow-skip): add TransformMultiShape_PositionsOnlyNoShadowProj wrapper + s_skipMultiTransformShadows flag"
```

---

## Task 2: Killswitch declaration + definition

**Files:**
- Modify: `GameOS/gameos/gos_mech_killswitch.h`
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 2.1: Add extern decl** after the existing `extern bool g_useGpuMechShadowStateStrip;` block:

```cpp

// Slice D-body-shadow-skip (2026-05-09): skip the MultiTransformShadows
// dispatch on the BODY shape (mechShape) when modern engine + GPU mech path
// is engaged. Recon proved mechShape's MultiTransformShadows outputs
// (listOfShadowTVertices etc.) are consumed only by RenderShadows, which
// is unreachable on tessellation (mech3d.cpp:3054 early-return). Preserves
// PerPolySelect's contract by keeping the per-leaf _PositionsOnly work
// (pool alloc + per-vertex projection + per-face cull + lastTurnTransformed).
//
// DISTINCT from MC2_GPU_MECH_SHADOW_SKIP: that flag (D-shadow-skip) skips
// mechShadowShape->TransformMultiShape* entirely (the dedicated shadow
// caster shape). THIS flag skips only mechShape's per-light × per-vertex
// shadow projection. Different shapes, different code paths.
//
// Independent of g_useGpuMechs / fast-transform / shadow-skip / state-strip
// flags for bisect granularity. Requires g_useGpuMechs=true AND
// gos_IsTerrainTessellationActive(). NOT compatible with MC2_MECH_GPU_PARITY=1.
extern bool g_useGpuMechBodyShadowSkip;
```

- [ ] **Step 2.2: Add global definition** after the existing `bool g_useGpuMechShadowStateStrip = ...;` line:

```cpp

// Slice D-body-shadow-skip: see gos_mech_killswitch.h.
bool g_useGpuMechBodyShadowSkip = (getenv("MC2_GPU_MECH_BODY_SHADOW_SKIP") != nullptr);
```

- [ ] **Step 2.3: Build clean**

- [ ] **Step 2.4: Commit**

```bash
git add GameOS/gameos/gos_mech_killswitch.h GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-d-body-shadow-skip): add g_useGpuMechBodyShadowSkip killswitch (MC2_GPU_MECH_BODY_SHADOW_SKIP env)"
```

---

## Task 3: 3-way conditional at body callsite

**Files:** Modify: `mclib/mech3d.cpp`

- [ ] **Step 3.1: Confirm callsite**

```bash
grep -n "g_useGpuMechFastTransform" mclib/mech3d.cpp
```

Expected: line 3430.

- [ ] **Step 3.2: Replace 2-way with 3-way**

Find:

```cpp
		if (g_useGpuMechs && g_useGpuMechFastTransform) {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		}
```

Replace with:

```cpp
		// Slice D-body-shadow-skip: when GPU mech path is on AND body-shadow-skip
		// killswitch is on AND tessellation is active, use _PositionsOnlyNoShadowProj
		// to additionally skip the MultiTransformShadows per-light × per-vertex
		// shadow projection on the body shape. Recon proved zero consumer in this
		// configuration (mech3d.cpp:3054 tessellation early-return). PerPolySelect's
		// contract is preserved by the underlying _PositionsOnly mechanism.
		//
		// Slice C3-revised (FAST_TRANSFORM): when BODY_SHADOW_SKIP is off but
		// FAST_TRANSFORM is on, use _PositionsOnly to skip only the per-vertex
		// CPU lighting kernel.
		if (g_useGpuMechs && g_useGpuMechBodyShadowSkip && gos_IsTerrainTessellationActive()) {
			mechShape->TransformMultiShape_PositionsOnlyNoShadowProj(&xlatPosition, &qRotation);
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

Shadow conditional at 3398-3404 (D-shadow-skip 3-way) unchanged. Sensor (3618, 3624) and arm (4498, 4582) untouched.

- [ ] **Step 3.4: Build clean (full relink — both msl.cpp and mech3d.cpp touched)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
find build64 -name "mech3d.obj" -delete
find build64 -name "msl.obj" -delete
/mc2-build
```

- [ ] **Step 3.5: Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat(slice-d-body-shadow-skip): extend body callsite to 3-way (BODY_SHADOW_SKIP > FAST_TRANSFORM > legacy) at mech3d.cpp:3430"
```

---

## Task 4: Smoke verification (mc2_10 only, 30s)

- [ ] **Step 4.1: Deploy build**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
```

- [ ] **Step 4.2: A — BODY_SHADOW_SKIP=0 sentinel**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_SKIP=1 MC2_GPU_MECH_SHADOW_STATE_STRIP=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

- [ ] **Step 4.3: B — BODY_SHADOW_SKIP=1 target**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_SKIP=1 MC2_GPU_MECH_SHADOW_STATE_STRIP=1 MC2_GPU_MECH_BODY_SHADOW_SKIP=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_10 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys.

---

## Task 5: 90s mc2_10 Tracy A/B (USER prompts)

**Files:** none. **DO NOT run unprompted.**

When user prompts: same B env as Task 4.3 at 90s. Expected:
- `Mech3D.UpdateGeometry` mean drops by ≥2µs/call from 22.52µs.
- `Units.Mechs` outer zone: ≥30µs/frame additional reduction.
- σ tightens.
- **Mouse-pick canary:** during the 90s run, click on a few friendly mechs. Selection must work (CRIT-1 mitigation validation).

---

## Task 6: Implementation review + memory pin

- [ ] **Step 6.1: Dispatch implementation adversarial review** with skill verbatim. Scrutiny vectors:
  - Wrapper composes `_PositionsOnly` semantics correctly (sets BOTH flags, clears in reverse order).
  - Gate at msl.cpp:1763 doesn't affect Track B `_BuildRecipe` (which `continue`s before reaching this site).
  - PerPolySelect contract preserved (re-grep tglpp.cpp preconditions vs `_PositionsOnly` outputs). Confirm pools (`shadowPool`, `facePool`) are still being allocated by `_PositionsOnly`, not gated off by some future change. The pool-alloc vs content-write distinction (spec §Q3 "Load-bearing distinction") is the canonical risk: CRIT-1 reopens if pool allocation is later skipped.
  - No new readers of `mechShape`'s shadow vertex pools introduced.
  - Killswitch precedence at body callsite: BODY_SHADOW_SKIP > FAST_TRANSFORM > legacy.
  - Confirms naming disambiguation from `MC2_GPU_MECH_SHADOW_SKIP`.

- [ ] **Step 6.2: Write memory file** with measured deltas.

- [ ] **Step 6.3: Add MEMORY.md index entry** after D-shadow-state-strip:

```markdown
- ⭐ [Mech body shadow-projection skip shipped 2026-05-09 (Slice D-body-shadow-skip)](mech_body_shadow_skip_shipped.md) — skip MultiTransformShadows on mechShape via MC2_GPU_MECH_BODY_SHADOW_SKIP; preserves PerPolySelect contract via underlying _PositionsOnly; <FILL IN µs/call delta>; aggressive leaf-skip deferred until GPU-mech-aware mouse-pick precursor lands
```

- [ ] **Step 6.4: Commit memory updates.**

---

## Spec Coverage Check

| Spec section | Covered by task |
|---|---|
| New static flag + dispatch gate | Task 1.1, 1.2 |
| New wrapper | Task 1.3, 1.4 |
| Killswitch | Task 2 |
| 3-way conditional | Task 3 |
| Tessellation runtime gate | Task 3.2 |
| PerPolySelect preservation | Validated by Task 5 mouse-pick canary |
| Verification gate | Tasks 4, 5 |

## Type / Symbol Consistency

- `s_skipMultiTransformShadows` — declared 1.1, read at 1.2, written at 1.4 ✓
- `TransformMultiShape_PositionsOnlyNoShadowProj` — declared 1.3, defined 1.4, called 3.2 ✓
- `g_useGpuMechBodyShadowSkip` — declared 2.1, defined 2.2, read 3.2 ✓
- `MC2_GPU_MECH_BODY_SHADOW_SKIP` env — defined only in 2.2 ✓
- `gos_IsTerrainTessellationActive` — already in scope ✓

## Placeholder Scan

- No "TBD"s.
- Every code step has actual code.
- Every commit has its message.
- Memory file has explicit measurement placeholder.

Plan ready for adversarial review (Task 0).
