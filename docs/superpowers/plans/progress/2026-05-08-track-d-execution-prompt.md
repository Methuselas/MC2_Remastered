# Track D — GPU Mech Batcher (Slice A) Execution Prompt

> **Purpose:** Self-contained prompt for a fresh Claude Code session to
> execute the GPU Mech Batcher Slice A plan. Not for in-place reading
> mid-session — paste this into a new session at
> `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\` and run.

---

## Goal

Replace `mechShape->Render(true)` per-mech CPU vertex submit with
SSBO-driven instanced GPU draws via a new standalone `GpuMechBatcher`,
while keeping CPU transform (`TransformMultiShape`) and CPU shadow
submission unchanged. Skinning-ready vertex format is included by
design from day 1 per the strategic note in
`memory/gpu_mech_skinning_alignment.md`.

## Start here — read in order, do not skip

1. **`docs/superpowers/plans/2026-05-03-gpu-mech-batcher.md`** — the
   12-task implementation plan. Tasks use `- [ ]` checkbox syntax.
   Walks through: header + types (Task 1), shaders mech.vert/frag
   (Task 2), batcher skeleton (Task 3), registration +
   finalizeGeometry (Task 4), ring SSBO (Task 5), submitActor with
   bone staging + texture capture (Task 6), flush with bucket-sorted
   compaction + draw loop (Task 7), measurement env-vars (Task 8),
   mission lifecycle hookup (Task 9), mech3d.cpp submit wire (Task
   10), parity counter hookup (Task 11), deploy + enable + smoke
   test (Task 12).

2. **`docs/superpowers/specs/2026-05-03-gpu-mech-batcher-design.md`**
   — full design spec. Section 6 covers parity gate. Stack: GL 4.3
   core, GLSL 430, GL_ARB_buffer_storage persistent mapping.

3. **`docs/superpowers/explorations/2026-05-03-mech-offload-recon.md`**
   — pre-design recon. Establishes the existing
   `Mech3DAppearanceType` model and TG_TypeShape/TG_MultiShape
   relationships the batcher consumes.

4. **`docs/superpowers/explorations/2026-04-29-track-d-assimp-importer-status.md`**
   — adjacent context on the Assimp importer track (separate slice;
   not blocked on Slice A but useful context).

5. **`docs/superpowers/explorations/2026-04-30-cross-track-perf-budget-audit.md`**
   — perf-budget audit mapping Track D's expected savings against
   the broader frame budget.

## Critical worktree rules — read before any code change

- **`CLAUDE.md`** — worktree-root discipline file. Especially:
  - "Documentation Discipline — grep at write-time, not after"
    (load-bearing).
  - "Review Discipline" — adversarial review by default for
    high-stakes plans.
  - "Critical Rules" — Build = RelWithDebInfo, Deploy = `cp -f` +
    `diff -q` to v0.2 AND v0.3, no `cp -r`, no time projections,
    100 ns Tracy floor.
  - "Load-Bearing Cull Infrastructure — READ BEFORE TOUCHING" — the
    inView/canBeSeen/objBlockInfo chain cascades into update,
    lifecycle, TGL pool budget, instance-state-refresh, and
    projection rhw guard.

- **`memory/gpu_mech_skinning_alignment.md`** — the strategic note
  arguing the GPU mech rendering optimization should land
  skinning-ready vertex format from day 1. The plan respects this.
  Do not strip skinning support to "ship faster."

## Parity gate decision (advisor sign-off — RESOLVED 2026-05-08)

Per plan Section 6, the design spec calls for dual-FBO
`MC2_MECH_GPU_PARITY=1` automated zero-mismatch parity as the Slice A
gate. The plan substitutes operator visual observation +
`[MECHBATCHER v1] event=summary fallback_total=0` as the Slice A
gate.

**Decision:** **Operator visual + counter-zero accepted as Slice A
gate.** Same precedent as renderWater Stage 2+3 (gate B Tracy delta +
operator visual canary), indirect-terrain SOLID PR1 (operator visual
+ legacy_solid_setup_quads counter), and PR2c mine static-bake
(operator visual + counter). Mech-rendering correctness is mostly
silhouette-stable; subtle blend / fog / texture-handle drift is
catchable by side-by-side `MC2_GPU_MECHS=0` vs `=1` operator pass on
mc2_01 (Centipede, Harasser, LRMC variety) and mc2_24 (different
mech mix).

**Conditions on the Slice A → A+ promotion:**
- Soak window: 7 days clean tier1 5/5 + zero `[MECHBATCHER v1]
  fallback_total > 0` events across all stock smoke missions.
- Operator visual canary explicit pass at full zoom out and full zoom
  in on each tier1 mission.
- `[MECHBATCHER v1] event=summary` shows non-zero
  `submitted_instances`, fallback_rate < 0.5%.

**Slice A+ adds dual-FBO parity** (estimated +3 tasks). Schedule when:
(a) any mech-related visual regression surfaces during soak that
operator visual missed, OR (b) before any default-on flip per the
"flip default-on after soak" pattern. Whichever comes first.

## Execution shape

This plan uses `superpowers:subagent-driven-development` (recommended)
or `superpowers:executing-plans`. Tasks have checkbox tracking. Each
task has step-by-step file:line edits with code excerpts in the plan.

Follow the plan task-by-task. Do NOT short-circuit. Do NOT bundle
tasks (e.g., "let me do tasks 4 and 5 together" loses the discipline
benefit). Each task ends with a self-test or checkpoint.

After every task that touches build:
1. `cmake --build build64 --config RelWithDebInfo --target mc2 --clean-first`
2. `cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`
   then same to v0.2; `diff -q` after each.
3. `py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing`
   — must PASS, +0 destroys.

After Task 12 (deploy + smoke), run the full operator visual canary
pass before opening adversarial review.

## Adversarial review (load-bearing per CLAUDE.md "Review Discipline")

After Task 12 ships and operator visual passes:
1. Dispatch adversarial review on the Slice A commit set using the
   `adversarial-plan-review` skill at `.claude/skills/adversarial-plan-review.md`.
   Specific scrutiny vectors:
   - SSBO layout consistency between `gos_mech_batcher.h` and
     `mech.vert` (per memory `cpp_glsl_ubo_struct_lockstep.md`).
   - Bone-matrix staging correctness — bones in skinning-ready format
     even if Slice A doesn't yet skin (vertex weights = identity).
   - Texture handle live-rebind handling (per memory
     `mc2_texture_handle_is_live.md`).
   - Cull gate respect (per memory `cull_gates_are_load_bearing.md`).
     Slice A should NOT bypass `inView`/`canBeSeen`.
   - Bridge function invalidation (per
     `docs/superpowers/specs/2026-05-08-applyrenderstates-shortcircuit-review.md`):
     mech batcher's flush function MUST call
     `gos_InvalidateRenderStateCache()` at end if it touches GL state
     outside `applyRenderStates`' tracked slot set.
2. Address all findings (CRITICAL must-fix; MAJOR fix or document;
   MINOR judge case-by-case).
3. Only after review verdict ≠ STOP-THE-LINE, the work is shippable
   to default-on.

## Default-on flip (separate slice, post-soak)

Slice A ships with `MC2_GPU_MECHS=1` opt-in only. Default-on flip is
a separate single-commit slice after the 7-day soak window passes
clean. Mirrors the renderWater + indirect-terrain SOLID flip cadence.

## Adjacent work in flight (do not touch)

The current session and one sibling session are concurrently working
on:
- PR2 (detail/overlay/mine indirect-terrain consolidation) — sibling.
- GPU cull frustum dilation + conservative-OR readback — parked on
  `claude/cull-dilation-pending` branch awaiting merge.
- Lifecycle gate (post-cull-dilation merge) — pending.

None of these touch the mech rendering path directly, so Track D
Slice A can proceed independently. But coordinate any merge to
`claude/nifty-mendeleev` to avoid mid-edit collisions.

## Out of scope for Slice A

- Skinning compute (vertex weights apply identity matrices Slice A;
  Slice A+ adds real skinning).
- Mech damage state visual differentiation (existing path stays).
- LOD switching (existing TG_MultiShape LOD selection stays
  CPU-side; batcher consumes whichever LOD CPU picks).
- Shadow path (CPU shadow submission unchanged per plan goal).
- Default-on flip (separate post-soak slice).

## Definition of Done

Slice A is done when:
- All 12 tasks have all checkboxes ticked.
- Tier1 5/5 PASS with `MC2_GPU_MECHS=1` set, +0 destroys delta.
- Operator visual canary pass (above).
- Adversarial review verdict PASS or PASS-WITH-FINDINGS-ADDRESSED.
- `[MECHBATCHER v1] event=summary` line emits with `fallback_total=0`
  across full smoke window.
- Memory file written: `memory/track_d_slice_a_shipped.md` with the
  ship state, env-var, and soak-start date.
- MEMORY.md index entry under "Modding / mod onboarding" or new
  section "Track D / GPU mech rendering" linked.
