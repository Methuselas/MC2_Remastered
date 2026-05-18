# Reverse-Z - FRESH SESSION HANDOFF (2026-05-18)

Self-contained. Pick up cold. Rule 0: grep before trusting any file:line
(symbols stable, lines drift). No emoji. No wall-clock projections.
Documents are very low priority - this handoff exists because the user
explicitly asked for it; do not expand it.

## State entering this session

**Merges DONE + verified (do not redo):**
- `water-material-v1` -> `gpu-driven-rendering` (`e0ab6fa`); WIP committed
  first (`60f2ef8` drawPass default-ON flip + PROJECT.md).
- `gpu-driven-rendering` -> `nifty-mendeleev` (`b1a6d2e`, `-X theirs`
  gpu-priority per user rule "in almost all ways gpu takes priority over
  nifty"). Split-brain orphans repaired (`10eb372`). `uint packed`->`bits`
  reserved-keyword fix on both trees (`cd06cd4` gpu / `136b875` nifty).
- Nifty clean build REAL_EXIT=0. Deployed `mc2-win64-v0.4`, smoke mc2_01
  PASS, **Fix B canary PASSED post-merge**: `[DEPTH_TRANSITION v1]`
  dz_gpuw flat ~+0.0005, dz_decal flat ~-0.0025 (incl. transition
  frames), `[WATER_RENDERPROBE v1]` invA 4350 equal=1 / 1 warmup
  (wframe=1 fp=0), `[WATER_DEPTHPROBE v2]` 399/399, 0 shader errors. Fix
  B survived the full chain - it is the validated reverse-Z prerequisite.

**Decisions locked (user, 2026-05-18):**
- Reverse-Z executes in the **`nifty-mendeleev` worktree** (nifty is main).
- Deploy/smoke to **`mc2-win64-water`** (NOT v0.4 - other trees use v0.4).
  Kill-aware mc2_01; never `--kill-existing` against a `*v0.4*` mc2.exe.
- Pipeline: re-fold done -> (this handoff) -> resolve the one C1 unknown
  -> writing-plans -> subagent execute -> build/deploy/smoke canary ->
  USER visual gate. User wants to STOP before execution this far; a fresh
  session owns C1-resolution + plan + execute.

## The design + where it stands

Spec: `docs/superpowers/specs/2026-05-18-reversed-z-float-depth-design.md`.
Read it fully, especially **Section 8 (fold v1)** then **Section 9
(re-fold v2 - authoritative; supersedes Section 8 where they conflict)**.

Pipeline so far: spec -> 2 adversarials (opus+sonnet) BOTH BLOCK ->
fold (Section 8) -> closure-adversarial (opus) FOLD-INSUFFICIENT ->
re-fold v2 (Section 9). Section 9 is the current authoritative state.

CLOSED, do NOT re-open: C2 (quad.cpp admission re-derive set complete),
U4 (one biased `gl_FragDepth` at `gos_terrain.frag:~772`, ~10
pass-throughs untouched, decoupled `+0.0005`), f1 (`[DEPTH_TRANSITION
v1]` dz formula sign-inverts = a probe CODE fix not a baseline change).

Greybeard (3 passes consistent): reverse-Z IS the root-class meta-fix;
concept sound; the fence is a seam coordinate-transform, NOT a forward-Z
patch island - stays substitutive. `glClipControl(ZERO_TO_ONE)` already
shipped (`gameosmain.cpp:~930`) - reverse-Z is the delta on top.

## THE ONE OPEN DESIGN UNKNOWN (resolve FIRST, blocking the plan)

C1 root: `Camera::projectZ` (`camera.h:~436-460`) reads `worldToClip`,
so it AUTO-follows the U1 matrix swap (no math change). All 7
`projectFor*Admission/ScreenXY/...` wrappers (`camera.h:~526-624`)
delegate to it. The forward-Z `[0,1)` assumption lives in a CONSUMER,
not projectZ's math. **Read projectZ's full body and decide:**
- (i) projectZ's returned BOOL embeds a forward-Z z-range test ->
  re-derive ONCE in projectZ -> all 7 wrappers fixed (clean); OR
- (ii) the bool is screen-XY-rect only, z-assumption is per-consumer ->
  enumerate+re-derive each: quad.cpp terrain cluster (`:2221/2222`,
  `:2257/2258` parity twin, `:2547/2604/2753/2971/3117`),
  `gameobj.cpp:2090` (object cull/`canBeSeen`), `clouds.cpp:212`,
  `crater.cpp:~323+`, `weather.cpp:~489+` (effect admission),
  `camera.cpp:1882/1910` (`projectForLightingShadow`).
This (i)-vs-(ii) ruling sets C1's true blast radius. Everything else is
mechanical plan-stage enumeration.

## Mechanical folds already specified (Section 9) - just enumerate at plan

- U3: ALL depth-bias constants flip (no FAST/RASTER partition -
  `TERRAIN_DEPTH_FUDGE` is shared, `terrain_depth_bias.h:55/59/62/65`);
  dying legacy path = coordinate transform at the fence seam, not an
  un-flipped constant; rewrite static_assert inequalities to
  `OVERLAY>0 && WATER<0`. Header "RASTER do NOT change" comment is
  Fix-B-era, superseded.
- U2 CRITICAL omissions now in scope: `gameos_graphics.cpp:6711`
  `gos_ForceApplyRenderStates` hardcoded LEQUAL; `:3949-3952`
  `applyRenderStates` ZCompare switch (REMAP case1/2 -> GEQUAL/GREATER,
  carve OUT shadow ZCompare=2 writers `txmmgr.cpp:~2316/~2327`).
- Scene/shadow partition is per-site EXHAUSTIVE (V2): scene depthfunc/
  clear flip; shadow sites (`4436/4580/4650/7121/7236/7299`,
  `gos_postprocess.cpp:1139/1163`, shadow clears) stay forward/LESS/1.0.
  No `glClearDepth` exists today (~7 GL_DEPTH clears; add
  `glClearDepth(0)` scene-only).
- Atomic landing: U1+U2+U3 + all-constants-flip + fence-seam-transform +
  ALL live forward-Z consumer re-derivations + scene/shadow partition =
  ONE non-divisible task. U4/U5 shader re-derivation may follow against
  the landed engine; U7 probe-code fix + canary re-baseline last.

## Next-session steps

1. Resolve C1 (i)-vs-(ii) (read `Camera::projectZ` body; grep all 7
   wrappers' consumers - the list above is the floor, opposite-direction
   grep for completeness). Fold the ruling into Section 9.
2. Optional: ONE more closure-adversarial ONLY on the C1 resolution
   (convergent-fold confidence; not a full re-review - the rest is
   closed). Dispatch prompt MUST say "use the adversarial-plan-review
   skill" and "run the greybeard skill" verbatim.
3. writing-plans skill -> the single atomic task. Plan re-greps every
   site (Rule 0).
4. Subagent-driven execute (fresh subagent/task + 2-stage review:
   spec-compliance THEN code-quality) -> final whole-impl adversarial ->
   FULL relink (RelWithDebInfo; load-bearing depth-state/inline) ->
   deploy `mc2-win64-water` -> kill-aware mc2_01 smoke.
5. Smoke gate: `[DEPTH_TRANSITION v1]` dz zoom-invariant BY PRECISION
   (Fix A is gone - no compensating formula); `[WATER_RENDERPROBE/
   DEPTHPROBE]` still equal=1; 0 shader errors.
6. **USER visual gate (authority):** no z-fight at ANY zoom (headline);
   terrain/POM no self-occlusion/punch-through; TacMap minimap +
   CPU-raster water (intro/deploy) unchanged (fence correct); Fix B/S1/
   transparency water unchanged.

## Pointers

- Living record (read it): `memory/water_v2_s1_shipped_s3_blocked_reborn.md`
  (reverse-Z is enabler #3; the camera-dependence ruling is QUALIFIED
  not void).
- Merge state + gpu-priority rule: `memory/merge_chain_water_to_gpu_to_
  nifty_and_depthbias_header_landmine.md`.
- SCOPING grounding (do not re-derive): `docs/superpowers/specs/
  2026-05-18-SCOPING-reversed-z-float-depth-modernization.md`.
- Discipline: structural depth change = adversarial-plan-review + greybeard
  mandatory; USER visual gate is ultimate authority; isolated deploy.
