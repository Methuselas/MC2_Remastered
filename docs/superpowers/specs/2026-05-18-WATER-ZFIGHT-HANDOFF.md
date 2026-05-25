# Water / Z-Fight / Depth - HANDOFF for a fresh session (2026-05-18)

You are picking up cold. Self-contained. Everything cited is grep-verifiable
(Rule 0: grep before trusting any file:line; symbols stable, lines drift).
Branch/worktree are ISOLATED - `claude/water-material-v1` at
`A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1/`; deploy ONLY
`A:/Games/mc2-opengl/mc2-win64-water/`; NEVER `mc2-win64-v0.4` (concurrent
priority session). No emoji; no wall-clock projections; full relink for
load-bearing C++; kill-aware mc2_01 30s smoke (wait if a `*v0.4*` mc2.exe
runs, never --kill-existing). Authoritative living record:
`memory/water_v2_s1_shipped_s3_blocked_reborn.md`.

## 1. What shipped / was decided this session

- **Water visual (commit `24190b9`, user-approved "looks pretty damn
  good"):** camera-independent S1 surface + depth absorption + mild
  transparency (`WATER_MAX_ALPHA=0.87`) + brighter glint
  (`GLINT_GAIN 0.30`/`GLINT_THRESH 0.36`). S3 terrain reflection
  IMPLEMENTED then SHELVED (`S3_REFLECTION_ENABLED=false`) - user rejects
  ANY perceptible water camera-dependence; the ruling's "S3 is the one
  sanctioned cam-dep term" carve-out is VOID in practice. Do NOT
  re-attempt S3/Option B/any reflection without re-confirming with user.
- **S6 armed-water (ii) draw-side decouple (commits up to `06ec374`):**
  M1a single-source `WaterFastPathOwnsArmedDraw()` (retired the fragile
  `:1184` hand-copy contract) = the real value. The (ii) arm-gate is
  CORRECT but PROVEN NON-SUBSTITUTIVE (~0ms; clean isolation A/B). Kept,
  relabeled non-perf (spec Sec 10). `MC2_WATER_S6_COST`/`[WATER_S6 v1]`
  retained env-gated probes; `MC2_S6_FORCE_LEGACY_II` measurement hack
  removed. CPU-retirement opportunity here = the kept (i) projection,
  minimap-consumer-locked (harder, separate, NOT S6).

## 2. THE ACTIVE SLICE: zoom-step z-fight / jump - Fix B (matrix-share)

**Root, numerically PROVEN** by the `[DEPTH_TRANSITION v1]` probe (committed
`f886b6c`, env `MC2_DEPTH_TRANSITION_PROBE`): terrain renders from
PRE-BAKED clip (`tr.clipPos[]`, baked w/ `g_dispatchMvp16`); GPU water +
decals project LIVE from `terrain_mvp_` + a constant screen-z fudge that
differs from terrain's. `dz_gpuw` flat `+0.0010000`, `dz_decal` flat
`-0.0020001` while `z_terr` drifts -> constant screen-z = distance-varying
world depth -> discrete zoom step = the visible jump + transient z-fight.

**Authoritative spec:** `2026-05-18-AUTHORITATIVE-zfight-matrix-share-
design.md` (commit `26a5178`; supersedes `64f265b`/`85d9d17`/`2d2cccb` -
those are DEAD/superseded). Read its **Section 11 (REVISED DESIGN)** - it
is authoritative over Sections 3/4-R-a.

**The fix (Section 11):** SYMMETRIC-MIRROR. Bind water + the static-decal
bake `terrainMVP` to `IsFrameSolidArmed() ? gos_terrain_indirect_
getDispatchMvp16() : gos_GetTerrainMVPMat4()` (nullptr-safety as
`gos_terrain_water_stream.cpp:~1413`) - the SAME switch terrain itself
flips on the same gate the same frame (un-armed terrain falls to the
legacy live path), so water/terrain move together -> ZERO relative
discontinuity. "No-flip pin" is WRONG (creates the discontinuity).
4 corrected edit sites:
1. `gameos_graphics.cpp:~2153` (non-MDI water - NOT dead, shared pre-amble)
2. `gameos_graphics.cpp:~2308` (MDI water)
3. `uploadOverlayUniforms_()` `~:7011-7014`: add `const float*
   terrainMvpOverride=nullptr` param; ONLY `drawDecalStaticBatch` (`~:7176`,
   uses `overlayLocs_`/`overlayProg_` - NOT decalLocs_) passes the
   symmetric-mirror. Live callers `drawTerrainOverlays`(`~:7066`)/
   `drawDecals`(`~:7240`) pass nothing -> byte-unchanged (the
   shared-helper trap).
4. Remove `WATER_DEPTH_FUDGE_FAST` (`gos_terrain_water_fast_mdi.vert:~291`
   + `gos_terrain_water_fast.vert:~365`); remove the 3 decal
   `glPolygonOffset(-1,-1)` (`gameos_graphics.cpp:~7062/7171/7236`, all
   decal-family); replace ALL with ONE non-zero shared small constant
   (M1: zero REGRESSES the shoreline LEQUAL scar
   `gos_terrain_water_fast.vert:~328-364`; mandatory in
   `terrain_overlay.vert:~36` too or live overlay loses all ordering),
   single-sourced in lockstep `terrain_depth_bias.hglsl`/`.h`.

**Mandatory new probe (same commit):** `MC2_WATER_RENDERPROBE` -
Invariant A: FNV(matrix uploaded at the render binds) == FNV(cull-feed
matrix `gos_terrain_water_stream.cpp:~1416`) every armed frame (canary for
the ONE real residual hazard: `terrain_mvp_` mutated between the early
cull-feed read and the late render-bind read in one armed frame).
Invariant B: latched arming-transition frame, water-bind FP ==
terrain's this-frame source FP. Passing Invariant B on a captured
transition frame = release gate (RenderDoc can't catch a 1-frame
transient).

**STATUS: spec plan-ready, BLOCK resolved, PENDING a RE-ADVERSARIAL** (the
revision was substantial; prior adversarials mandated re-review). Then ->
plan -> subagent-driven execute -> isolated build/deploy -> kill-aware
mc2_01 smoke (`[WATER_RENDERPROBE]` Invariant A/B + `[DEPTH_TRANSITION v1]`
`dz`->~0 zoom-invariant) -> USER visual gate. Re-adversarial mandated foci:
(a) symmetric-mirror relative-co-planarity (prove water==terrain MVP both
regimes + transition), (b) Invariant-A intra-frame-mutation hazard, (c)
`uploadOverlayUniforms_` per-caller-arg leaving the 2 live callers
byte-unchanged, (d) M1 non-zero shared constant preserves the shoreline
LEQUAL invariant at all zooms.

## 3. THE ROOT-CLASS META: reversed-Z + float depth (SCOPED, greenlightable)

`2026-05-18-SCOPING-reversed-z-float-depth-modernization.md` (commit
`a389979`). Verdict TRACTABLE-BUT-LARGE - the hard prereq
(`glClipControl ZERO_TO_ONE`, `gameosmain.cpp:~930`) is ALREADY shipped,
so reverse-Z is the delta on top (~20+ C++ + ~10 shaders + sign-flipped
`terrain_depth_bias` lockstep; shadows EXCLUDED v1). **Reverse-Z LARGELY
OBVIATES the shelved Fix A AND the `vulkan_aligned_depth_bias` distance-
proportional unifier** (do NOT build that unifier). Fix B is NOT obviated
(temporal desync, orthogonal; stepping stone - reverse-Z verification
needs Fix B's water==terrain MVP invariant). Sequence: Fix B -> reverse-Z
-> cancel the distance-bias slice. 2 gating design Qs before any code:
(1) `gos_terrain.frag` POM `gl_FragDepth` re-derivation (~12 writes + a
`+0.0005` + a `max()` that all flip; also disables early-Z on 7900 XTX);
(2) legacy CPU `inverseProjectZ`/raster `projectZ` port-vs-fence (TacMap +
CPU water; water->full-GPU favours fencing the dying path). GREENLIGHT
deferred to AFTER Fix B ships; tracked, NOT active.

## 4. Tracked separate sub-roots (NOT Fix B / NOT reverse-Z)

- **CPU-water ~0.05 projection-path divergence:** probe-proven
  `dz_cpuw` non-constant, sign-flipping (~+0.055 -> -0.006). Legacy
  `eye->projectForTerrainAdmission`/`projectZ` Stuff pipeline, a ~50x
  larger PROJECTION-PATH mismatch (NOT a fudge issue), un-armed-only
  (intro/deploy). No `terrainMVP` uniform to repoint. Its own future
  slice; reverse-Z does NOT fix it (orthogonal).
- **Sym1:** constant water-sits-low at all zooms = `waterElevation`/wave
  baseline. Orthogonal. Separate.

## 5. Discipline that worked (follow it)

Grounding (advisor) -> spec -> user spec-review -> 2 adversarials
(opus|sonnet, adversarial-plan-review skill, code-grounded) -> fold ->
plan -> subagent-driven execute (per-task spec+quality review) -> isolated
build/deploy -> kill-aware mc2_01 marker-gated smoke -> USER visual gate.
This session the diagnosis was WRONG 3x and each time USER FIRST-HAND
VISUAL EVIDENCE (capped-FPS invalid; CPU-water-also-jumps; decals-too)
outranked the model - trust it over any grounding claim; probe before
spec-lock. Memory lessons added:
`capped_fps_is_not_a_cpu_cost_ab_signal.md`,
`rule0_applies_to_reviewer_proposed_fixes_too.md`,
`mc2_selection_picking_model_water_terrain_never_picked.md`.

## 6. First actions for the new session

1. `cd` the worktree; read `memory/water_v2_s1_shipped_s3_blocked_reborn.md`
   + the AUTHORITATIVE spec Section 11 + this doc.
2. Run the Fix B RE-ADVERSARIAL (2x, foci in Sec 2 above), fold.
3. -> plan -> subagent execute -> smoke (probes) -> USER visual gate.
4. Keep isolated; user integrates separately (the cross-branch push is the
   user's explicit deliberate step - confirm scope, do not blind-merge an
   experimental branch into main/shared).
