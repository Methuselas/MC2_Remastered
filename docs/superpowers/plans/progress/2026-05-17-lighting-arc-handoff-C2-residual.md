# HANDOFF: static-lighting substitutive arc — C5 shipped, C2 is the open residual

> Self-contained. Paste into a fresh session in the
> `gpu-driven-rendering` worktree
> (`A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\`),
> branch `claude/gpu-driven-rendering`. Created 2026-05-17 @ HEAD
> `a66db54`. **RE-GREP every file:line at read-time** (lines drift;
> documentation-discipline). **No guess-patching** — the prior session
> was wrong TWICE about the residual (first "mechs", then corrected to
> C2 by the user's Tracy). Evidence before fixes.

## What shipped this session (all behind kill-switches, committed)

- `b41baec` LightsData UBO -> unbounded std430 SSBO @ binding 20
  (64-slot ceiling removed; enabling-infra). Black-props during bring-up
  was a **shader-deploy omission**, not code -> memory
  `shader_exe_deploy_lockstep.md`. Deploy MUST ship shaders+exe in
  lockstep.
- `2db2a04` static-lighting bake (`MC2_LIGHTBAKE` default ON, `=0` =
  legacy `CacheGpuLightData`). Retired the per-frame **recompute**:
  `[LIGHTBRIDGE v1]` populate ~2000us -> ~80us **CONFIRMED**.
- `8c1c491` demoted 3 default-on traces (`[LIGHT_DEDUP v1]` etc.) to
  env-gated (frame-time pollution).
- `38d8720` **persistent static light table** — each static recipe owns
  a permanent CPU-mirrored slot == registry `recipeIndex`, written once
  (`MC_TextureManager::bakeStaticLightSlot`, `mc2WriteStaticLightSlot`);
  `TG_MultiShape::EmitBakedGpuLightData` is now a pure pointer
  assignment (NO `addLightDataStructure`); `mc2SubmitBakedLightSlot` +
  `s_bakedSlotByRecipe` DELETED; `resetLightData` count rebased to S
  (kill-switch-gated). Adversarial recon-review STOP->5 fixes->simplified
  (sparse glBufferSubData helper proven redundant -> NO gameos_graphics
  change). Impl-review PROCEED (caught/own-caught: C1 valid-gather guard,
  same-frame static/dynamic slot-collision bump). tier1 5/5 +0 destroys
  GL-clean ~141.5 FPS.
- `f3e3d8a` / `a66db54` render-perf-snapshot refreshed; memories
  appended (honest: 38d8720 NOT yet a confirmed substitutive win).

Deployed `A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe` is SHA256-verified
== `build64/RelWithDebInfo/mc2.exe` == the `38d8720` build (mtime
13:38:45). NOT the water version.

## The open thread (start here)

The arc optimized **C5** (the GPU-object path:
`CacheGpuLightData -> GatherGpuObjectLightDataOnly ->
addLightDataStructureWithPerActorColor`, RE-GREP `mclib/msl.cpp` ~:1892,
`mclib/tgl.cpp` ~:2858). `[LIGHTBRIDGE v1]` (env
`MC2_OBJECT_RECON_TRACY`) instruments **only C5** and reads ~52
calls/frame ON -> the C5 static zone-death is real and done.

**But the dominant cost on a legacy-object mission is C2, never in
scope.** User Tracy (heaviest mission): zone `GameLogic.Units.
TerrainObjects` (`code/objmgr.cpp` ~:1964 `GameObjectManager::update`)
-> `addLightDataStructure scan` **x1726/frame, ~826us (42.89% self)**,
siblings `TG.MultiShape.PerLeaf` (`msl.cpp` ~:1793) / `PerShapeLoop`
(`msl.cpp` ~:1452). Grep-verified C2 = `mclib/tgl.cpp:3113`
`rs.light_data_buffer_index_ = mcTextureManager->addLightDataStructure(
&lightData_)` — a DIRECT call (not via WithPerActorColor, hence
invisible to `[LIGHTBRIDGE]`), inside the legacy-leaf branch gated at
`tgl.cpp:2604` `if (bShadersDrawPathEnabled && !eligibleForGpuObjects(
this) && !isSpotlight && !isWindow && !textureAlpha && alpha==0xff)`.
(C6 sibling: `tgl.cpp:2865` `ResubmitCachedLightData` -> same direct
call.) `eligibleForGpuObjects` decl `tgl.cpp:~120`.

These static TerrainObjects render via the **legacy CPU
`MultiTransformShape` per-leaf path** (`!eligibleForGpuObjects`), NOT
C5. The bake/persistent-table cannot apply to them by construction. The
`addLightDataStructure-bridge-retirement` handoff explicitly called C2
"Bucket A — out of scope, GPU-porting that class is a separate slice."

### THE DECISIVE FORK (recon, not patch)

Why are these static TerrainObjects `!eligibleForGpuObjects`?
- **(a) Regression / mis-gating** — if they SHOULD be GPU-eligible and
  something forces C2 (candidates: the `[MECHBATCHER v1]
  event=shader_fail` cascade this run; `g_useGpuObjects` /
  `g_useGpuStaticProps` state; an `eligibleForGpuObjects()` criteria
  regression). Then fixing the gate routes them onto C5 and the
  **existing `38d8720` bake instantly applies** — cheapest, best.
- **(b) Inherent residual** — legitimately non-eligible class -> C2
  needs its OWN substitutive slice (persistent-slot/repoint
  `tgl.cpp:3113` by actor identity, same pattern as 38d8720). A genuine
  new slice.

First recon step: instrument/inspect `eligibleForGpuObjects()` return +
`g_useGpuObjects`/`g_useGpuStaticProps` for the TerrainObjects in the
heaviest mission, and whether the mech `shader_fail` cascades into
object eligibility. That answer picks (a) vs (b). Dispatch the
cpu-gpu-offload + render advisors per CLAUDE.md discipline.

## Confounders — do NOT conflate (disambiguate-by-subsystem memory)

1. `[MECHBATCHER v1] event=shader_fail` — `shader_builder.cpp(613):
   ERROR: Program with this name (mech) already exists` (occurred ONCE,
   this run). GPU mech path disabled -> mechs on legacy fallback. NOT
   the 38d8720 slice (C++-only, 0 shader/0 mech-program edits). LEAD
   suspect to check honestly: `b41baec` RF4 edit touched
   `gos_mech_batcher.cpp`'s mech-program-init region (removed the
   UBO-size gate that previously could skip mech init) — a candidate
   for changed init flow / double-register. Likely intermittent
   (GPU mechs were fine in the 2db2a04/SSBO captures). Evidence-first;
   separate item.
2. Half-black terrain + black squares — terrain lighting is a SEPARATE
   subsystem (not LightsData). User has seen it self-clear; matches the
   documented first-launch-black intermittency / `pause_unpause_
   diagnostic`. Confirm via pause or 2nd mission; not 38d8720.

## Operational context

- Build: `--config RelWithDebInfo`, full-relink (rm changed .obj + exe).
  PDB lock by running mc2.exe = LNK1201 -> kill mc2 first (PowerShell
  `Stop-Process -Name mc2 -Force`; MSYS `taskkill //F` mangles). User
  AUTHORIZED freely killing mc2 / `--kill-existing` (water session is in
  a SEPARATE worktree -> zero build/deploy/PDB/shared-file conflict).
- Deploy: this slice class is C++-only (deploy exe only) — BUT any
  shader-touching slice MUST deploy shaders+exe lockstep
  (`shader_exe_deploy_lockstep.md`). Verify v0.4 == build64 via
  `Get-FileHash`.
- Foreign uncommitted (leave for owner, NEVER `git add -A`):
  `GameOS/gameos/gos_terrain_indirect.cpp`, `.planning/PROJECT.md`,
  plus older untracked `docs/.../2026-05-16-*` handoffs.
- Methodology that worked: recon -> plan -> adversarial-PLAN-review
  (STOP/fixes) -> implement -> adversarial-IMPLEMENTATION-review (caught
  real C1) -> tier1 -> commit-behind-kill-switch -> user-driven proof.
  Use subagents for recon/review/tier1/closing. No wall-clock in
  commits/docs. No emoji.

## DONE-governor status (honest)

Per `feedback_offload_must_be_substitutive_not_additive.md`: confirmed
substitutive wins remain **minePass + drawPass only**. 2db2a04's C5
recompute-death is `[LIGHTBRIDGE]`-confirmed. 38d8720's zone-death is
real for C5 but UNPROVABLE via the `addLightDataStructure scan` Tracy
zone on legacy-object missions because C2 dominates that zone — the
clean proof needs a mission whose static objects are C5-eligible, OR
resolving the C2 fork above. Do not claim 38d8720 as a confirmed win
until that lands.

## User-driven proof commands (cmd; unchanged)

ON: fresh prompt, `cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"` ,
`set MC2_LIGHTBAKE=` , `mc2.exe > "%USERPROFILE%\Desktop\table_ON.log"
2>&1`. OFF: `set MC2_LIGHTBAKE=0` likewise. Tracy: read
`addLightDataStructure scan` calls/frame. (Confounded by C2 until the
fork is resolved — see above.)
