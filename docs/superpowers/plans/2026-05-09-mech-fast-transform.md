# Mech Fast-Transform Implementation Plan (Slice C3-revised)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `TG_MultiShape::TransformMultiShape_PositionsOnly` for the GPU mech body when `MC2_GPU_MECH_FAST_TRANSFORM=1`, bypassing the per-vertex CPU lighting kernel that consumes a substantial portion of `Mech3DAppearance::updateGeometry`. Pixel-equivalent output guaranteed when `MC2_GPU_MECH_LIGHTING=1`.

**Architecture:** ONE call-site conditional swap in `mech3d.cpp` (`:3381` body). One new env-var-driven extern. No shader changes. Arm callsites (`:4459`, `:4543`) explicitly out of scope (their `Render(true)` runs unconditionally — stripping the lighting bake would render garbage; needs separate slice). Shadow callsite (`:3377`) and sensor callsites (`:3579`, `:3585`) also out of scope.

**Plan-time adversarial review verdict (pre-execution):** STOP-THE-LINE on original arm-included scope; the body-only scope below addresses CRIT-1 from that review.

**Tech Stack:** Existing `TG_MultiShape::TransformMultiShape_PositionsOnly` at `mclib/msl.cpp:1789`, existing GPU mech batcher killswitch infrastructure at `GameOS/gameos/gos_mech_killswitch.h`.

**Spec:** [docs/superpowers/specs/2026-05-09-mech-fast-transform-design.md](../specs/2026-05-09-mech-fast-transform-design.md)

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | Add `extern bool g_useGpuMechFastTransform` decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | Define globally from `MC2_GPU_MECH_FAST_TRANSFORM` env var |
| Modify | `mclib/mech3d.cpp` | ONE conditional swap at body callsite (:3381). Arms and shadow stay full per spec. |

---

## Task 1: Killswitch declaration + definition

**Files:**
- Modify: `GameOS/gameos/gos_mech_killswitch.h`
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 1.1: Add extern decl to killswitch header**

In `GameOS/gameos/gos_mech_killswitch.h`, immediately after the existing `extern bool g_useGpuMechSkin;` decl, add:

```cpp
// Slice C3-revised (2026-05-09): wire TransformMultiShape_PositionsOnly
// for the GPU mech path. Skips the per-vertex CPU lighting kernel that
// consumes ~65µs/mech and whose output (listOfVertices[j].argb) is only
// consumed by the legacy CPU Render(true) path that Slice A bypasses.
// Independent of g_useGpuMechs / g_useGpuMechLighting for bisect
// granularity. Requires g_useGpuMechs=true to take effect (when GPU
// mech path is off, the legacy path NEEDS the lighting bake's output).
extern bool g_useGpuMechFastTransform;
```

- [ ] **Step 1.2: Add global definition in gos_mech_batcher.cpp**

In `GameOS/gameos/gos_mech_batcher.cpp`, immediately after the existing `bool g_useGpuMechSkin = (getenv("MC2_GPU_MECH_SKIN") != nullptr);`, add:

```cpp
// Slice C3-revised: see gos_mech_killswitch.h.
bool g_useGpuMechFastTransform = (getenv("MC2_GPU_MECH_FAST_TRANSFORM") != nullptr);
```

- [ ] **Step 1.3: Build clean**

```
/mc2-build
```

Expected: clean build. Pure decl/def step — no callers yet.

- [ ] **Step 1.4: Commit**

```bash
git add GameOS/gameos/gos_mech_killswitch.h GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-c3-revised): add g_useGpuMechFastTransform killswitch (MC2_GPU_MECH_FAST_TRANSFORM env)"
```

---

## Task 2: Conditional swap at mech body callsite

**Files:**
- Modify: `mclib/mech3d.cpp`

- [ ] **Step 2.1: Confirm callsite**

```bash
grep -n "mechShape->TransformMultiShape" mclib/mech3d.cpp | head -5
```

Expected: line 3381 (mech body). NO swap at line 3377 (shadow shape — explicitly out of scope per spec).

- [ ] **Step 2.2: Apply conditional swap at line 3381**

In `mclib/mech3d.cpp`, find the line:

```cpp
		mechShape->TransformMultiShape (&xlatPosition,&qRotation);
```

Replace with:

```cpp
		// Slice C3-revised: when GPU mech path is on AND fast-transform
		// killswitch is on, use _PositionsOnly to skip the per-vertex
		// CPU lighting kernel. Output of that kernel (listOfVertices[j].argb)
		// is only consumed by mechShape->Render(true) which Slice A
		// bypasses; GPU shader does its own lighting via calc_light().
		if (g_useGpuMechs && g_useGpuMechFastTransform) {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		}
```

- [ ] **Step 2.3: Verify shadow callsite unchanged**

```bash
sed -n '3375,3385p' mclib/mech3d.cpp
```

Expected: line 3377 still reads `mechShadowShape->TransformMultiShape (&xlatPosition,&qRotation);` (NO conditional). Spec explicitly preserves this — `MultiTransformShadows` dispatch dependency is out of scope.

- [ ] **Step 2.4: Build clean**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 2.5: Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat(slice-c3-revised): conditional swap to TransformMultiShape_PositionsOnly at mech body callsite (mech3d.cpp:3381)"
```

---

## Task 3: Verify body-only swap (no arm changes)

**Files:**
- Read-only verification

- [ ] **Step 3.1: Confirm only body callsite is swapped**

```bash
grep -n "TransformMultiShape\b" mclib/mech3d.cpp | grep -v "_PositionsOnly\|_BuildRecipe"
```

Expected: lines 3377 (shadow — full, intentional), 4459 (leftArm — full, intentional, OUT OF SCOPE per CRIT-1), 4543 (rightArm — full, OUT OF SCOPE per CRIT-1), 3579 (sensorTriangleShape — full, intentional), 3585 (sensorSquareShape — full, intentional). Body goes through the conditional at the swap site (now reads `TransformMultiShape_PositionsOnly` inside the if-branch and `TransformMultiShape` inside the else — no longer matches the `TransformMultiShape\b` pattern at the swapped site).

- [ ] **Step 3.2: Confirm arm callsites still naked**

```bash
sed -n '4455,4465p' mclib/mech3d.cpp
sed -n '4540,4548p' mclib/mech3d.cpp
```

Expected: both arm callsites still read `leftArm->TransformMultiShape(&xlatPosition,&qRotation);` and `rightArm->TransformMultiShape(&xlatPosition,&qRotation);` — NO conditional. This is intentional per the spec's arm-out-of-scope decision.

---

## Task 4: Smoke matrix verification

**Files:** none (validation only)

- [ ] **Step 4.1: Deploy build**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
```

Expected: no diff output.

- [ ] **Step 4.2: A — CPU baseline regression sentinel**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys. CPU mech path must remain identical (when `g_useGpuMechs=false` the slice gates fall through to the original `TransformMultiShape` call).

- [ ] **Step 4.3: B — GPU mech batcher with fast transform OFF (Slice A baseline)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, fallback_total=0. Identical to pre-slice behavior since `g_useGpuMechFastTransform=false`.

- [ ] **Step 4.4: C — Full bore (this slice's target config)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, fallback_total=0, no new GL errors.

- [ ] **Step 4.5: D — Tier1 sweep at full bore**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 \
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: tier1 5/5 PASS, all +0 destroys.

- [ ] **Step 4.6: E — mc2_24 stress test (different mech mix, larger active population)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 \
py -3 scripts/run_smoke.py --mission mc2_24 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, fallback_total=0.

- [ ] **Step 4.7: No commit step** — validation only.

---

## Task 5: Operator visual A/B canary + Tracy timing comparison

**Files:** none (validation only — user runs manually)

- [ ] **Step 5.1: Operator visual A/B**

User runs the deployed exe twice on mc2_10 (the mission where the original 71µs/call observation was made):

  - **A.** `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=0`
  - **B.** Same env BUT `MC2_GPU_MECH_FAST_TRANSFORM=1`

Mechs must look pixel-identical to operator-visual confidence in B vs A. Any visible drift signals a missed `listOfVertices[j].argb` consumer (would force a CRITICAL revisit).

- [ ] **Step 5.2: Tracy comparison**

User attaches Tracy GUI in both runs, captures `GameLogic.Mech3D.UpdateGeometry` zone average (verified zone name at `mclib/mech3d.cpp:3184`). The zone wraps the WHOLE function — body + shadow + arms (when blown off) + sensors (when selected). Body-only swap retires only ~half of the zone's cost. Expected:
  - **A** (fast transform OFF): ~71µs/call (matches the original observation).
  - **B** (fast transform ON): **≥30µs/call delta**, i.e. zone average drops to ~40µs or less.
  - **Frame time delta:** **≥0.5ms reduction** on mc2_10's ~19 mech actors per frame. (The ~1.0–1.2ms estimate from the spec's first draft assumed body+arms; arms were de-scoped at adversarial review per CRIT-1.)

If the tracy delta is smaller than expected (<0.3ms total or <20µs/call), shadow callsite may be dominating the zone cost. Flag for follow-up profiling pass with finer-grained Tracy zones (`mech3d.updateGeometry.body` vs `.shadow` sub-zones) before declaring the perf gate met.

- [ ] **Step 5.3: No commit step** — validation only.

---

## Task 6: Adversarial review + memory pin

**Files:**
- Create: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_fast_transform_shipped.md`
- Modify: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 6.1: Dispatch adversarial review**

Use the `superpowers:code-reviewer` subagent with the dispatch prompt requiring `adversarial-plan-review` skill verbatim. Specific scrutiny vectors per the spec's verification gate:

- Conditional placement matches the spec exactly (no swap at shadow `:3377`, sensor `:3579`/`:3585`).
- `_PositionsOnly` populates `shapeToWorld` for the GPU mech batcher's bone SSBO upload to read.
- File-static flag race-safety holds (single-threaded actor update loop).
- No code path reads `listOfVertices[j].argb` while `g_useGpuMechs && g_useGpuMechFastTransform` is set — grep for all consumers.
- Killswitch independence: `MC2_GPU_MECH_FAST_TRANSFORM=1` with `MC2_GPU_MECHS=0` must NOT invoke `_PositionsOnly`.

Address all CRITICAL / MAJOR findings inline before writing memory.

- [ ] **Step 6.2: Write the memory file**

Create `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mech_fast_transform_shipped.md`:

```markdown
---
name: Track D Slice C3-revised — Mech Fast-Transform shipped 2026-05-09
description: Wires TransformMultiShape_PositionsOnly for GPU mech body + arms; delivers the actual ~1ms/frame CPU savings Slice A's framing implied but didn't deliver
type: project
---

⚠️ STATUS: [shipped | in soak] — populate at slice close

## What shipped
- mech3d.cpp:3381 (body), :4459 (left arm), :4543 (right arm) conditional
  swap to TransformMultiShape_PositionsOnly when MC2_GPU_MECHS=1 AND
  MC2_GPU_MECH_FAST_TRANSFORM=1.
- Shadow callsite (:3377) and sensor callsites (:3579, :3585) untouched.
- New killswitch g_useGpuMechFastTransform from MC2_GPU_MECH_FAST_TRANSFORM env.

## What this fixes
Slice A's perf claim "GPU mech batcher saves CPU per-actor cost" was
narrowly true (saved the vertex submit cost) but ~80% of per-actor cost
was in the per-vertex CPU lighting kernel inside MultiTransformShape,
which Slice A explicitly preserved. This slice wires the existing
_PositionsOnly variant (built for slice 2 buildings/trees) for mechs.

## Verification
- Tier1 5/5 PASS at full bore + this slice
- mc2_24 stress PASS
- Operator visual A/B (FAST_TRANSFORM=0 vs =1) pixel-identical at user
  inspection
- Tracy: mech3d.updateGeometry drops from ~71µs/call to ~Xµs/call;
  frame time saves ~Yms on mc2_10
- Adversarial review verdict

## Deferred
- Shadow callsite: needs separate recon on _PositionsOnly impact on
  MultiTransformShadows dispatch
- Default-on flip: separate slice after soak

## Pre-existing followups still open
- Dynamic shadow pass not catching mechs (Slice B-era flag)
- Slice B verification consolidation (still in soak)
```

Replace `[shipped | in soak]` and the `Xµs` / `Yms` with measured values from Tasks 4–5.

- [ ] **Step 6.3: Add MEMORY.md index entry**

In `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`, immediately after the existing Slice C-related entry (or wherever the chronological/topical ordering fits), add:

```markdown
- ⭐ [Mech Fast-Transform shipped 2026-05-09](mech_fast_transform_shipped.md) — wires TransformMultiShape_PositionsOnly for GPU mech body+arms via MC2_GPU_MECH_FAST_TRANSFORM; delivers the ~1ms/frame CPU savings Slice A's framing implied; pixel-equivalent when MC2_GPU_MECH_LIGHTING=1
```

- [ ] **Step 6.4: Commit memory updates**

Memory dir is outside the worktree; commit in its own location if it's a git repo, otherwise leave as untracked workspace state per existing convention.

---

## Spec Coverage Check (post-revision)

| Spec section | Covered by task |
|---|---|
| Architecture: body-only swap, arms+shadow untouched | Task 2 (swap), Task 3 (read-only verification) |
| New killswitch `g_useGpuMechFastTransform` | Task 1 |
| Pixel-equivalence reasoning | Validated by Task 5.1 operator A/B |
| Failure modes covered (incl. arm hazard caught at plan review) | Verified by review (Task 6.1) |
| Verification gate (tier1 + tracy ≥30µs + visual) | Tasks 4, 5 |

## Type / Symbol Consistency

- `g_useGpuMechFastTransform` — declared Task 1.1, defined Task 1.2, read Tasks 2.2, 3.2, 3.3 ✓
- `MC2_GPU_MECH_FAST_TRANSFORM` env — defined only in Task 1.2 ✓
- `mechShape->TransformMultiShape_PositionsOnly` — already exists at `mclib/msl.cpp:1789` (verified pre-spec); called at Task 2.2 ✓
- Arm `_PositionsOnly` calls — explicitly NOT used in this slice per spec arm-out-of-scope decision (CRIT-1) ✓

## Placeholder Scan

- No "TBD"s in the plan.
- Every code step has the actual code.
- Every commit has its message.
- Memory file template has explicit measurement placeholders (`Xµs`, `Yms`) — these are runtime values, not plan-level gaps.

Plan ready for execution.
