# Next Orchestrator Session — Frame Budget 9.5 ms → 4.5 ms (Wolfman Zoom)

> **Purpose:** Self-contained prompt for a fresh Claude Code session
> to orchestrate the next wave of frame-budget reduction at wolfman
> zoom. Paste into a new session at
> `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\` and
> drive multi-agent parallelism the same way the 2026-05-08 session
> did for the ~11.12 → ~6.76 ms zoom-in / ~9.5 ms zoom-out wave.

---

## Goal

Cut wolfman-zoom CPU frame time from ~9.5 ms to ~4.5 ms. That's a
~5 ms reduction; achievable only by stacking multiple in-flight
slices that each address a different bucket. Single-slice wins of
this magnitude don't exist anymore — the easy CPU-to-GPU offloads
in the obvious places have shipped. Remaining work is structural
(architectural unblocks) and population-specific (per-actor render
prep at high actor count).

**Calibration: smoke at zoom-in is frame-cap-limited at ~150 fps
(target 165 fps). Real measurement window is wolfman zoom Tracy
captures during real play, NOT smoke.**

## Start here — read in order, do not skip

### Session-immediate context

1. **Recent commit log** — `git log --oneline -20` from
   `claude/nifty-mendeleev`. Multiple sessions shipped during
   2026-05-07/08; understand the state of the branch before
   dispatching anything. Especially:
   - PR2a delete (`feat(pr2a): delete dead M2c detail emit`)
   - PR2b scaffold (`feat(pr2b): scaffold overlay indirect endpoint`)
   - PR2c MINE static-bake (`feat(pr2c): flip MC2_TERRAIN_INDIRECT_MINE default-on`)
   - Cull-dilation + conservative-OR (`feat(gpu-cull): frustum dilation + conservative-OR`)
   - ApplyRenderStates state-equality cache + invalidation contract docs
   - Hot-reload gate (`fix(shader): hot-reload sweep now actually obeys 500ms cadence`)
   - Sub-zones in Mission::render, Vehicles::update, GameCamera::render terrain, renderWaterFastPath

2. **Three queued execution prompts** in
   `docs/superpowers/plans/progress/`:
   - `2026-05-08-track-d-execution-prompt.md` — GPU mech batcher
     Slice A. **Parity-gate sign-off received 2026-05-08.** May
     already be in flight in a parallel worktree session by the
     time you read this; check `git log` for `track-d` /
     `mech-batcher` commits.
   - `2026-05-08-substrate-coalesce-redesign-prompt.md` — addresses
     the 4 CRITICAL findings from the prior STOP-THE-LINE review.
     Hard prerequisite for substrate ship.
   - `2026-05-08-next-orchestrator-9p5-to-4p5.md` — this file.

3. **Brainstorm with a strategic timing recommendation:**
   - `docs/superpowers/brainstorms/2026-05-08-job-system-parallel-for-scope.md`
     — recommends parallel-for AFTER substrate redesign, with
     `vertexProjectLoop` as first slice candidate.

4. **Worktree CLAUDE.md** — load-bearing rules:
   - "Documentation Discipline — grep at write-time, not after"
   - "Review Discipline" — adversarial review by default for
     architectural / cull / SSBO-schema / "X retires Y" slices
   - "Critical Rules" — Build = RelWithDebInfo, Deploy = `cp -f` +
     `diff -q` to BOTH `A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`
     AND `A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe`, no `cp -r`,
     no time projections, full relink before deploy on
     load-bearing changes
   - "Profiling — 100 ns floor" on Tracy zones
   - "Load-Bearing Cull Infrastructure" — read before any change
     that affects `inView` / `canBeSeen` / `objBlockInfo.active`
     consumers

5. **Memory pointers (bare minimum to orient):**
   - `track_c_compute_cull.md` — substrate / readback / lifecycle ship state
   - `track_c_substrate_regression.md` — 323-bucket GPU serialization
   - `cull_gates_are_load_bearing.md` — load-bearing cull chain
   - `mc2_texture_handle_is_live.md` — per-frame handle remap
   - `gpu_mech_skinning_alignment.md` — skinning-ready vertex format

## Frame-budget map at wolfman zoom (entering this session)

Approximate, from 2026-05-08 session's last Tracy + counter data.
Re-baseline at session start; numbers will have drifted.

| Bucket | Approx ms | Target slice |
|---|---|---|
| `Terrain::render drawPass` (post-PR2c MINE retire) | ~1.5-1.7 | PR2b overlay landing further; PR2 cleanup |
| `GameLogic.Units.TerrainObjects` (mix render-prep + game logic) | ~1.27 | Lifecycle gate (post-substrate) — render-prep portion |
| `Terrain::geometry quadSetupTextures` (residual after MINE retire) | ~0.6-0.8 | PR2b overlay completion |
| `GameCamera::render objects` (per-actor MLR submit) | ~0.9-1.0 | Track D Slice A |
| `GameLogic.Units.Vehicles AppearanceUpdate` (per-actor render-prep) | ~0.3-0.4 | Track D analog OR lifecycle gate |
| `Terrain::geometry vertexProjectLoop` (vestigial after C3 routing complete) | ~0.4 | Job-system parallel-for OR full deletion |
| `GameLogic.Mech3D.UpdateGeometry ×6` (per-visible-mech bone solve) | ~0.3 | Track D Slice A |
| `Render.GpuStaticProps` when substrate-on (CURRENT BROKEN PATH) | ~2.0 | Substrate multi-draw coalesce |
| Other / misc / overhead | ~1.0 | Various |
| **Total wolfman** | **~9.5** | |

To hit ~4.5 ms = need ~5 ms of cuts. Reasonable composition:
- Track D Slice A: ~0.8-1.5 ms (Mech3D + portion of render objects)
- Substrate coalesce: ~1.8 ms (only if substrate goes default-on
  post-coalesce; otherwise this isn't realized)
- Lifecycle gate (post-substrate): ~0.7-1.5 ms (TerrainObjects
  render-prep + Vehicles AppearanceUpdate + Mech3D.UpdateGeometry)
- vertexProjectLoop deletion or parallel-for: ~0.3-0.4 ms
- PR2b overlay completion: ~0.3-0.5 ms

Stacked optimistically: ~5-6 ms. Tight but plausible.

## Orchestration shape — mirror the 2026-05-08 pattern

### Session phase 0: state assessment

1. `git log --oneline -25` — what's landed since 2026-05-08.
2. `git status --short` — what's WIP from sibling sessions.
3. Inventory the three queued execution prompts and check whether
   they've shipped or are still pending.
4. Capture a fresh wolfman-zoom Tracy from real play (user task)
   to re-baseline the bucket map. Do NOT assume the table above
   is current.

### Session phase 1: parallel agent dispatch

Multiple slices can run in parallel agents in isolated worktrees.
The 2026-05-08 session ran 4-6 agents concurrently. Coordinate
to avoid overlapping file edits.

**Dispatch candidates (depending on which prompts have/haven't
shipped):**

- If Track D Slice A hasn't shipped: dispatch the Track D execution
  prompt as a dedicated subagent (or a separate user-driven
  session — Track D is large enough to warrant its own session).
- If substrate coalesce hasn't shipped: dispatch the redesign
  per the queued prompt.
- If both have shipped, dispatch lifecycle gate consumer-routing
  extension (the work paused 2026-05-08 with rationale "obsoleted
  by future GPU paths" — but post-Track-D, the rationale shifts:
  Track D obsoletes mech render-prep, but TerrainObjects + Vehicles
  AppearanceUpdate are still CPU-driven and gate-able).
- vertexProjectLoop: small follow-up after substrate coalesce
  unlocks compute-record vs GL-submit seam (per job-system
  brainstorm Q9). Dispatch as parallel-for slice OR full deletion
  slice depending on whether C3 routing completes during this
  session.

### Session phase 2: integration

After agents land, cherry-pick / merge to `claude/nifty-mendeleev`.
Per worktree CLAUDE.md "Full relink before deploy" — clean rebuild
between major merges. Deploy `cp -f` + `diff -q` to BOTH v0.2 and
v0.3 every cycle.

### Session phase 3: adversarial review

Each load-bearing slice (substrate coalesce, lifecycle gate
extensions, Track D Slice A integration) needs adversarial review
before merge. Use the `adversarial-plan-review` skill explicitly
in dispatch prompts; prose-only review fails the discipline.

### Session phase 4: measurement

User runs wolfman-zoom Tracy. Read deltas vs baseline. If gap to
4.5 ms target remains, identify next wave (likely: parallel-for
on next-biggest pure-CPU loop, or PR2b overlay completion).

## Critical execution constraints

- **Per worktree CLAUDE.md "No wall-clock time projections"** —
  describe complexity in code dimensions, not duration.
- **Per CLAUDE.md "Documentation Discipline — grep at write-time"**
  — citations decay measurably within a week in active worktrees
  per memory `brainstorm_code_grounding_lesson.md`. Re-grep
  aggressively on any cited symbol older than ~3 days.
- **Per CLAUDE.md "Critical Rules — Full relink before deploy"** —
  CMake's incremental build leaks stale linkage when headers
  change inline functions / templates / static state. Before any
  deploy that touches a load-bearing function: delete the exe and
  pass `--clean-first`.
- **Per CLAUDE.md "Profiling — 100 ns floor"** — Tracy zone
  overhead is ~20-50 ns; do not instrument sub-100 ns work.
  Coarse per-pass zones only.
- **Per CLAUDE.md "Load-Bearing Cull Infrastructure"** — any
  change touching `inView` / `canBeSeen` / `objBlockInfo.active`
  consumers must respect the full cascade (per-object update,
  lifecycle, TGL pool, instance-state-refresh, projection rhw
  guard). Read `cull_gates_are_load_bearing.md` first.
- **Adjacent-session safety:** if PR2 session or Track D session
  is committing concurrently, check `git status` and `git log`
  before any merge. The 2026-05-08 session demonstrated several
  near-collisions where agents fell back to editing
  `nifty-mendeleev` directly even with `isolation: "worktree"`;
  expect this and plan to handle it.

## Out of scope

- Full task-graph rewrite (idTech-shape) per
  `2026-05-08-job-system-parallel-for-scope.md` Q1 — too big, fights
  the architecture. Targeted parallel-for is in scope; full graph
  is not.
- Vulkan migration — orthogonal; not a precondition for any of
  the queued slices.
- Mod / content sidecar work — Track D Slice A explicitly stays
  on stock content per `memory/stock_install_must_remain_playable.md`.

## Definition of Done

- Wolfman-zoom Tracy shows CPU frame ≤4.5 ms at typical play camera
  on mc2_01.
- Tier1 5/5 PASS with all default-on flags.
- Visual canary on each newly default-on slice (operator visual
  side-by-side with kill-switch on/off).
- All adversarial reviews on shipped slices ≠ STOP THE LINE.
- Memory updated:
  - `MEMORY.md` index entries for newly shipped major slices.
  - Topic memory files for each big slice (substrate coalesce
    shipped, lifecycle gate extended, Track D Slice A shipped, etc.).
  - Stale citations corrected as found.
- Session-wrap commit / doc summarizing what landed and the new
  Tracy baseline at wolfman zoom.

## Closing prompt for the orchestrator

> Run a session in this worktree that reduces wolfman-zoom CPU frame
> from ~9.5 ms to ~4.5 ms by orchestrating the queued execution
> prompts (Track D Slice A, substrate multi-draw coalesce redesign,
> lifecycle gate extension post-substrate, vertexProjectLoop
> parallel-for or deletion). Mirror the 2026-05-08 multi-agent
> parallel-dispatch pattern. Respect the worktree CLAUDE.md
> Documentation Discipline + Review Discipline + No-Time-Projections
> rules. Adversarial-review every load-bearing slice before merge.
> Wrap with a memory-file update and a Tracy delta report.
