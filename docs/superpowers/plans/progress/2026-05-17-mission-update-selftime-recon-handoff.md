# HANDOFF: Mission::update self-time — attribution-gap recon (probe-first)

> Self-contained. Paste into a fresh session in the
> `gpu-driven-rendering` worktree
> (`A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\`),
> branch `claude/gpu-driven-rendering`. Created 2026-05-17 @ HEAD
> `e43842d` (8-commit C6 arc + arc docs). **RE-GREP every file:line at
> read-time** (symbols stable, lines drift — documentation-discipline).
> **No guess-patching, no blind slice** — this thread is explicitly
> probe-first; the prior arc was misattributed THREE times by trusting a
> conflated Tracy zone. Evidence before any optimization.

## What is DONE — do NOT re-investigate

The static-lighting arc (C2->C6 misattribution -> chicken-and-egg ->
`touch()` primed-slot repoint) is **CLOSED and proven**. Full evidence:
`docs/superpowers/plans/2026-05-17-c2-residual-fix-DECISION.md` ("C6
SUBSTITUTIVE PROOF") + memory
`c2_residual_was_conflated_zone_misattribution_real_producer_is_c6.md`.
Commits `6061e7b cd4c358 5487a3c 297c145 44aca12` (Plan A telemetry,
gate `MC2_LIGHT_COST_SPLIT` — permanent demote-not-delete instrument),
`4fed1b6 7289eae` (C6 fix, Bldg/Tree `touch()` repoint), `e43842d`
(docs). Confirmed substitutive wins: minePass, drawPass, C6 retirement,
38d8720 persistent static-light table (co-confirmed). Do not reopen.

## The open thread (start here)

With C6 retired, the new dominant in-frame SELF-time bucket on a
fix-on zoomed-out capture (heaviest mission, Frame ~9.03ms) is:

- **`GameLogic.Mission.Update` SELF = ~1.88 ms (~29.6% of its ~6.34ms
  inclusive; ~21% of frame).** `Mission::update`, `code/mission.cpp`
  (recon-verified 2026-05-17: zone opens ~`:470`, block CLOSES ~`:769`
  wrapping the termination/objective tail — RE-GREP `Mission::update`).

**Verdict from the mc2-render-perf-expert recon (2026-05-17): this is a
Tracy ATTRIBUTION GAP, not a hot mission-logic region. NOT slice-ready.
Probe-first.** Same failure mode as the documented `Camera.UpdateRenderers`
1.29ms precedent: opaque self-time hiding *outside* the instrumented
children.

### Grep-verified self-body composition (RE-GREP all; `code/mission.cpp`)

Child-zoned (NOT in the 1.88ms self — already attributed elsewhere):
Interface ~:474, Camera ~:476, VTol ~:478, Terrain ~:480, Weather ~:484,
Waypoints ~:486, PathManager ~:491, clearObjBlocksActive ~:501,
clearObjVerticesActive ~:502, terrainTextures->update ~:503,
TerrainGeometry ~:505, Craters ~:537, TextureManager ~:542, Sensors
~:547, Collisions ~:550, AI.BrainExecute ~:556.

Un-zoned inline work that COMPOSES the 1.88ms self:
1. **`mcTextureManager->clearArrays()` — `mission.cpp:~471`.** Un-zoned,
   O(N) master-node array clears every frame.
2. `gpu_cull::substrate_frameBegin()` — `~:520`. No-op when substrate
   disabled (default). Negligible.
3. **`ObjectManager->update(true,true,true)` — `mission.cpp:~525`
   (`updateAppearancesOnly` ~:523 when paused). THE BIG ONE.**
   `GameObjectManager::update` (`code/objmgr.cpp:~1896`) has **NO outer
   Tracy zone** — only interior child zones
   (`GameLogic.Units.TerrainObjects` ~`objmgr.cpp:1964`, Mechs, Movers...).
   Its un-zoned entry shell bills to `Mission::update` SELF:
   `readback_buildActorVisSnapshot` (~`objmgr.cpp:1909`), the **full
   O(maxObjects ~4096) `framesSinceActive` sweep ~`objmgr.cpp:1915-1941`**
   (3 virtual calls/object/frame —
   `inView_instr`/`canBeSeen_instr`/`blockActive_instr` — every frame,
   pause-independent), and `updateCaptureList()` (~`objmgr.cpp:1960`).
4. `compute_emitParitySummary()` — `~:534` — substrate-gated, skipped by
   default. Negligible.
5. Termination/objective tail — `mission.cpp:~565-731` (closes ~:769):
   two O(numMovers~tens) player-dead scans, input polling, `#ifndef
   FINAL` cheat/framerate block. Small, all self.

`Mission::update`'s OWN logic is light. The 1.88ms is the **un-zoned
outer shell of `GameObjectManager::update`** (prime suspect: the 4096-iter
`framesSinceActive` sweep) + `clearArrays()`.

## The decisive fork (probe, not patch)

The 1.88ms forks into THREE mutually-exclusive outcomes — two are NOT new
work. Cannot scope a slice until subdivided:

- **(a) `framesSinceActive` sweep dominates** -> it IS the already-PAUSED
  C3 lifecycle/readback instrumentation (`objmgr.cpp` C3 path; cross-ref
  `s_gpuCullLifecycle`/readback). May be inert at default env -> a cheap
  env-gate slice. The only genuinely-new outcome.
- **(b) `clearArrays()` dominates** -> folds into the EXISTING dual-queue
  legacy-retirement debt
  (`memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`).
  Do NOT re-scope as new — attach there.
- **(c) residual stays in true `Mission::update` self** -> gameplay /
  mission-tick irreducible (termination, input). Leave it.

## First step (the minimal probe — respects the 100ns floor)

Add THREE coarse `ZoneScopedN`, once-per-frame each, NO per-element
(strip-commit `fdc47bc` forbids per-iteration zones, NOT a per-call outer
wrapper):
1. `ZoneScopedN("GameObjectManager::update")` wrapping the whole body at
   `objmgr.cpp:~1896-1897` — instantly splits "GOM entry shell" vs "true
   Mission self".
2. `ZoneScopedN("GOM.framesSinceActive sweep")` around
   `objmgr.cpp:~1915-1941`.
3. `ZoneScopedN("Mission.clearArrays")` at `mission.cpp:~471`.

Then ONE user-driven Tracy capture, heaviest mission, zoomed-out
worst-case camera, **`MC2_TERRAIN_INDIRECT_TRACE` UNSET and NO
`MC2_LIGHT_COST_SPLIT`** — the recon flags the 1.88ms itself may be
TRACE/CostSplit-inflated (render-perf-snapshot stale-flag: the 2026-05-17
~11ms capture is TRACE-poisoned). **Re-confirm the 1.88ms on a clean
capture before committing probe effort.** Outcomes -> pick (a)/(b)/(c)
above.

## Discipline (load-bearing — same as the C6 arc)

- **Advisor-first:** spawn `mc2-render-perf-expert` (Tracy bucket / slice
  ROI / GameLogic domain — see `.claude/agents/DOMAINS.md`) before any
  plan; this recon was its output, but re-dispatch fresh per
  `feedback_dispatch_advisor_before_substantive_domain_work`.
- **Probe slice = the Plan-A pattern:** recon -> writing-plans ->
  adversarial-plan-review (mandated, this is a hot-path instrument whose
  mis-placement fabricates the wrong fork — `matrix_index_convention`
  class) -> subagent-driven execution -> user-driven clean capture ->
  branch on outcome. Do NOT slice before the capture lands.
- **Substitutive-not-additive done-governor** applies to any (a) slice:
  named CPU zone gone from a fresh capture + total-frame delta + consumer
  repointed; soak-waiver = env parity probe + adversarial impl review +
  code-proof (no calendar soak).
- **Foreign uncommitted — NEVER `git add -A` / wholefile:**
  `.planning/PROJECT.md`, `GameOS/gameos/gos_terrain_indirect.cpp`, and
  the older untracked `docs/.../2026-05-16-*` + `progress/2026-05-1*`
  handoffs are other-sessions' WIP. Stage ONLY the specific files/hunks
  you author; `git show <sha> --stat` must match intent. (The C6 arc hit
  this trap once — a subagent `git add <wholefile>` swept the foreign
  `gos_terrain_indirect.cpp` overlay WIP into a commit; it was surgically
  reverted. Per-file/per-hunk staging is mandatory.)
- **Build:** `--config RelWithDebInfo`, full relink (`rm` changed `.obj`
  + `mc2.exe`). PDB lock by running mc2 = LNK1201 -> `Stop-Process -Name
  mc2 -Force` first. Deploy: per-file `cp -f` + `Get-FileHash` verify,
  NEVER `cp -r`; this class is C++-only (exe-only deploy) unless a slice
  touches shaders (then shader+exe lockstep). Smoke gate: `py -3
  scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing` (no
  menu-canary). tier1 is STRUCTURALLY BLIND to the zoomed-out 1677-class
  regime — the real proof is user-driven worst-case-camera.
- No wall-clock in commits/docs. No emoji. Git: all work local, NEVER
  push `alariq/mc2`.

## Branch state

`claude/gpu-driven-rendering` HEAD `e43842d`, worktree preserved, NOT
merged to `claude/nifty-mendeleev` (user deferred the merge). The C6 arc
is committed and self-contained on this branch. This thread is net-new
work on top; it does not depend on the nifty merge.
