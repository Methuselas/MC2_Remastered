# HANDOFF - Object/Prop Iteration GPU Port (residual TerrainObjects elimination)

Date: 2026-05-18. For: a fresh implementation session. From: recon+brainstorm
session (deliberately stopped before implementation per user scope + context).

## READ FIRST (non-negotiable discipline)

1. **Work ONLY in `.claude/worktrees/gpu-driven-rendering`** (branch
   claude/gpu-driven-rendering, deploy v0.4). The root CLAUDE.md pointer to
   nifty-mendeleev is STALE. A prior session wasted itself building this on
   nifty (175 commits behind, structurally different). See
   `memory/feedback_active_worktree_is_gpu_driven_rendering.md` (linked in
   MEMORY.md). Do NOT read/cite the nifty worktree.
2. **Trust commits/code over docs/plans/snapshots.** This recon repeatedly
   found the render-perf-snapshot and plan docs stale (e.g. it claimed VPL
   retirement "PLAN DRAFTED" when commits prove it COMPLETE). Re-grep every
   file:line at plan-write; symbols stable, lines drift ~1wk.
3. The branch carries concurrent-session foreign WIP. `git add` per file by
   exact name; NEVER `git add -A`/`.`.

## What is decided (do NOT re-derive)

- The residual ~1.43ms `GameLogic.Units.TerrainObjects` (post lighting-bake
  / C6 / LIGHTBRIDGE - lighting is DONE, not the residual) is the per-object
  `recalcBounds()` projection: the CPU-redundant TWIN of the GPU compute
  cull that already runs every frame.
- Elimination = the VPL arc's named, now-UNBLOCKED downstream "object/prop
  iteration GPU port": delete the per-object projection body, repoint the
  RENDER-consumer of `inView`/`canBeSeen()` to the already-shipped
  `gpu_cull::readback_isActorVisibleLagged`, made motion-safe by the
  already-shipped `89e35ac` conservative-OR + frustum-dilation primitive.
- Multithreading / job-system is OFF the table (no job system exists;
  explicitly SUPERSEDED - "VPL retired, not parallelized"). Elimination, not
  relocation, is the bar.
- Full Stage-0 design contract (delete-what, replace-with-what,
  cull-cascade-untouched proof, Path-A 1-frame-lag avoidance, proof gate):
  **`docs/superpowers/specs/2026-05-18-object-prop-iteration-gpu-port-stage0.md`**
  (commit `efb0713`). This is the authoritative input - read it fully.

## Gate status

- Preconditions 1-4 (VPL retired `5296de4`/`0c8e06b`; slimReduce owns
  cull-cascade `63a0b3e`; motion-safety `89e35ac`; readback API wired
  default-off objmgr.cpp:~1930) - commit-verified SATISFIED.
- Precondition 5 (measurement gate) - RESOLVED. The user's latest Tracy was
  no-env-vars on the latest 0.4 exe with C6 necessarily live; ~1.43ms is the
  real post-C6 worst-case residual. Size against ~1.43ms; no further capture
  needed to start.

## Next action for the implementation session

1. Invoke superpowers:writing-plans against the Stage-0 contract. Resolve
   the contract's 4 open questions in the plan, especially CRIT-1: what
   feeds `screenPos`/mouse-pick after the projection delete (lazy
   per-selected projection vs defer to the queued
   `gpu_mech_aware_mouse_pick` AABB precursor). Scope = terrain statics
   (Bldg/Tree) only; movers are a separate slice.
2. Mandated two-gate adversarial review (architectural endpoint +
   cull/visibility consumer chain = high-stakes). Dispatch prompt MUST
   contain verbatim "use the adversarial-plan-review skill". ~3 design +
   ~2 plan rounds, alternating opus/sonnet.
3. Substitutive proof gate (the campaign bar): user-driven non-COST_SPLIT
   total-frame Tracy, worst-case zoomed-out big-map - `TerrainObjects`
   self-time -> ~0 AND total frame drops (anti-mirage), plus a
   logic/counter superset parity (readback-visible MUST be a superset of
   legacy `inView`; zero `(legacyCanBeSeen && !readbackVisible)`), plus a
   visual canary (no prop pop/vanish on fast zoomed-out pan = CRITICAL).
   Soak waived per `memory/feedback_soak_waiver_with_probes_and_reviews_validated.md`.

## Hard constraints (from the contract - violating any = regression)

- Repoint the RENDER consumer only (terrobj.cpp:~796 render / :~866
  renderShadows via canBeSeen). NEVER gate `appearance->update()` or the
  per-block update loop - buildings/turrets need update() offscreen
  (gate/turret/power logic), objmgr.cpp:~2024-2031 + cull_gates_are_load_bearing.
- Do NOT touch terrain.cpp / slimReduce - it owns the cull-cascade writes
  and is a separate, already-shipped subsystem.
- Require `gpu_cull::readback_isEnabled()` + conservative-OR + dilation for
  the render repoint; fail-open to all-visible is the safe degradation.
- Over-inclusion is correctness-safe; a dropped/popped prop is catastrophic.

## Session log (what happened, briefly)

Recon+brainstorm only. Two advisor passes (render-perf-expert characterized
the residual; cpu-gpu-offload-expert produced the contract). VPL-complete
confirmed from commits. Stage-0 contract + this handoff committed; snapshot
de-staled. NOTE: an earlier part of this session built Stage 0 + Plan 2A on
the WRONG branch (nifty-mendeleev) - that work is abandoned, not merged, and
must be ignored; this gpu-driven-rendering contract supersedes it entirely.
