# DESIGN: Reversed-Z + Float-Depth Buffer (root-class depth precision)

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated; user integrates separately)
**Status:** DESIGN - user-approved 2026-05-18. Greenlightable (Fix B shipped,
which was the validated prerequisite). Next per user plan: this spec -> NEW
SESSION for 2 adversarials -> plan -> subagent execute. (User explicitly
deferred plan/execute to a fresh session.)
**Supersedes for implementation intent:** `2026-05-18-SCOPING-reversed-z-
float-depth-modernization.md` (the tractability/blast-radius recon; still the
authoritative grounding source - this design does NOT re-derive it). Context:
`2026-05-18-water-enabling-infra-greybeard-map.md` (#3 in the enabler map).
**Discipline:** high-stakes structural depth change -> spec -> 2 mandatory
adversarials (opus|sonnet, code-grounded, adversarial-plan-review skill) ->
plan -> subagent-driven execute -> isolated build/deploy -> kill-aware mc2_01
smoke (probe canary) -> USER visual gate. Rule 0: every file:line below is
from the code-grounded scoping recon; plan stage re-greps (symbols stable,
lines drift).

---

## 1. Goal and what it subsumes

Replace the perspective-nonlinear 24-bit integer depth buffer with
**reversed-Z (near=1, far=0) + 32F depth** so depth precision is ~uniform
across the whole range. This eliminates the *distance-worsening z-fight
class* at the root, engine-wide, instead of per-consumer symptom patches.

- **Hard prerequisite ALREADY shipped:** `glClipControl(GL_LOWER_LEFT,
  GL_ZERO_TO_ONE)` (`gameosmain.cpp:~930`, fail-closed). The engine already
  emits D3D-style `[0,1]` clip-z; reverse-Z is the delta on top, not a
  ground-up rework.
- **OBVIATES** the shelved Fix A (distance-proportional clip-z bias) AND the
  `vulkan_aligned_depth_bias_ruling` distance-proportional unifier. Post
  reverse-Z a single small symmetric constant suffices; the
  `terrain_depth_bias` FAST/RASTER dual-regime lakebed-punch-through-at-zoom
  fragility dissolves. **Do NOT build the distance-proportional unifier -
  this slice replaces it.**
- **Fix B (matrix-share) is independent and survives.** Fix B fixed a
  TEMPORAL water/terrain MVP desync (shipped this session, `be5fb0d`/
  `d9516f3` baseline). Reverse-Z changes matrix CONTENT, not Fix B's sharing
  plumbing. Fix B is the STEPPING STONE: reverse-Z verification depends on
  the water==terrain MVP invariant Fix B establishes.
- **Orthogonal / untouched:** CPU-water ~0.05 projection-path divergence
  (different pipeline, not precision); Sym1 `waterElevation` baseline.

## 2. Resolved gating decisions (user, 2026-05-18)

These were the two scoping-doc questions that gated any code. Both decided:

- **D1 - Legacy scalar depth path: FENCE (not port).** `Camera::
  inverseProjectZ` / raster `projectZ` (`camera.cpp:~1941-1985`, `quad.cpp`)
  - hand-rolled scalar forward-`[0,1]` math used by TacMap (minimap
  footprint) + legacy CPU-raster water (un-armed intro/deploy). It is
  **fenced behind a forward-Z compat transform at the seam, NOT
  re-derived.** Rationale: water is committed full-GPU (#1) so the
  CPU-raster path is dying; TacMap is cosmetic; re-deriving fragile
  matrix-seamless scalar math on a dying path is risk for transitional
  value. Co-decides Fork P/F shared with enabler #5 (legacy-water
  retirement): fence.
- **D2 - `gos_terrain.frag` POM gl_FragDepth: MINIMAL CORRECT
  RE-DERIVATION ONLY.** Re-derive the ~12 `gl_FragDepth` writes + the
  hardcoded `+0.0005` + the `max(UndisplacedDepth, gl_FragCoord.z)` for
  near=1/far=0 (max->min, +const sign flips); verify vs terrain
  self-occlusion / POM punch-through. The `gl_FragDepth`-disables-early-Z
  wart on the 7900 XTX is **left exactly as-is** (status quo) - it is
  orthogonal to precision and is a SEPARATE future perf slice. Do not bundle
  an opportunistic perf refactor into this correctness migration.

## 3. Implementation units

| U | Unit | Grounded sites (re-grep at plan) | Notes |
|---|------|----------------------------------|-------|
| U1 | Projection swap | `camera.cpp:~2074-2092` perspective `cameraToClip`; `~:2020-2038` ortho/TacMap | swap near/far. Single CPU source; GPU dispatch MVP follows automatically |
| U2 | Depth state + clear + format | `glDepthFunc` LEQUAL/LESS->GEQUAL/GREATER ~25 sites + `gameos_graphics.cpp:~3929-3931` switch + `gos_mech_batcher.cpp:~1063` + `gos_static_prop_batcher.cpp:~2872` + `gos_postprocess.cpp:~1139/1163`; add explicit `glClearDepth(0)` ~4 clear sites; scene depth `GL_DEPTH24_STENCIL8`->32F `gos_postprocess.cpp:~294` | |
| U3 | Depth-bias lockstep sign-flip | `mclib/terrain_depth_bias.h` + `shaders/include/terrain_depth_bias.hglsl` + ~13 consumers | flips sign/meaning of every fudge; **intersects the shipped Fix B constants** (`TERRAIN_DEPTH_FUDGE 0.002`, `WATER_DEPTH_BIAS +0.0005`, `OVERLAY_DEPTH_BIAS -0.0005`) - they flip; Fix A/distance-bias cancelled -> one small symmetric constant. Lockstep pair, same commit (Fix B discipline) |
| U4 | `gos_terrain.frag` POM re-derivation | ~12 `gl_FragDepth` writes + `+0.0005` + `max(Undisplaced, gl_FragCoord.z)` | D2: minimal correct re-derivation; early-Z unchanged |
| U5 | Post-process depth reconstruction | `shadow_screen.frag`, `ssao.frag:~43` | re-derive for near=1; mechanical (~2-3 shaders) |
| U6 | Fence legacy scalar path | `Camera::inverseProjectZ`/raster `projectZ` (`camera.cpp:~1941-1985`, `quad.cpp`) | D1: forward-Z compat transform at the seam; NOT re-derived |
| U7 | Probes re-baseline (canary) | `[DEPTH_TRANSITION v1]`, `[WATER_DEPTHPROBE]`, `[WATER_RENDERPROBE]` | post-reverse-Z the `[DEPTH_TRANSITION v1]` distance-nonlinearity must be GONE by PRECISION not formula (dz zoom-invariant); this is the release canary |

**Scope exclusions (explicit):** shadows EXCLUDED v1 (ortho - no perspective
nonlinearity, zero precision benefit, doubles surface + acne sign-flip risk
for nothing). Early-Z restoration EXCLUDED (separate future perf slice, D2).
Legacy scalar path NOT ported (fenced, D1). CPU-water 0.05 divergence + Sym1
untouched (orthogonal).

## 4. Load-bearing constraint: atomic landing

U1 + U2 must land **together in one atomic change** (projection swap +
`glDepthFunc` flip + `glClearDepth(0)` + depth format). A half-flipped engine
(matrix reversed but `glDepthFunc` still LEQUAL, or clear still 1.0) is
globally depth-broken - this is NOT incrementally shippable site-by-site.
U3 is a lockstep pair in the same commit (Fix B discipline). Plan sequencing:
**(U1+U2+U3) one atomic landing -> U4/U5 shader re-derivation verified
against it -> U6 fence -> U7 canary re-baseline.** The plan must treat
U1+U2+U3 as a single non-divisible task with no intermediate "working"
state; verification is end-to-end post-landing, not per-site.

## 5. Verification and gates

- **Canary (U7):** `[DEPTH_TRANSITION v1]` re-baselined. Pre-reverse-Z it
  measured a distance-nonlinear dz; **post-reverse-Z dz must be
  zoom-invariant because precision is now uniform, NOT because a formula
  compensates** (Fix A is gone). If dz is still distance-varying, reverse-Z
  did not take. `[WATER_DEPTHPROBE]`/`[WATER_RENDERPROBE]` re-baselined
  (they assert MVP consistency - unchanged by reverse-Z content swap;
  equal=1 must still hold).
- **Build/deploy:** RelWithDebInfo, full relink (load-bearing depth-state +
  inline/lockstep change), deploy isolated `mc2-win64-water` ONLY, never
  v0.4. Kill-aware mc2_01 smoke (wait if a `*v0.4*` mc2.exe runs, never
  `--kill-existing` against it).
- **Smoke gate:** `[DEPTH_TRANSITION v1]` dz zoom-invariant; zero shader
  compile errors (`0(N): error`); no crash/regression.
- **USER visual gate (authority):** no z-fight at ANY zoom (the precision
  win - this is the headline); terrain + POM/displacement correct (no
  self-occlusion / punch-through - U4 risk); TacMap minimap footprint +
  CPU-raster water (intro/deploy) unchanged (U6 fence correct); shipped
  Fix B / S1 / transparency water unchanged.

## 6. Plan-stage Rule-0 re-verification (fold into the plan)

- V1: re-grep U1 `camera.cpp` projection sites + confirm GPU MVP is
  CPU-supplied (no GPU-side near/far hardcode).
- V2: enumerate ALL `glDepthFunc`/`glClearDepth`/depth-format sites
  (opposite-direction grep - a missed LEQUAL = a depth-broken pass).
- V3: enumerate ALL `terrain_depth_bias` consumers (~13) + confirm the Fix B
  constants are the current values to sign-flip; lockstep `.h`/`.hglsl`
  byte-parity post-flip.
- V4: enumerate the `gos_terrain.frag` `gl_FragDepth` writes + the `+0.0005`
  + the `max(...)`; confirm count vs scoping (~12).
- V5: locate the legacy scalar `inverseProjectZ`/`projectZ` seam(s) for the
  U6 compat-transform boundary; confirm TacMap + CPU-raster water are the
  only consumers (opposite-direction grep).
- V6: `/mc2-amd-shader-review` on every changed `.frag`/`.vert`
  (depth-state + gl_FragDepth = AMD-sensitive: early-Z, depth-clamp).

## 7. Discipline

This spec -> user review -> 2 adversarials (opus|sonnet, adversarial-plan-
review skill, code-grounded; mandated foci: (a) U1+U2+U3 atomic-landing
completeness - any missed depthfunc/clear/bias site = global depth break,
(b) U4 POM re-derivation correctness incl. the `+0.0005`/`max` sign flips,
(c) U6 fence boundary - the compat transform must leave TacMap + CPU-water
bit-unchanged, (d) U3 Fix B constant sign-flip + lockstep parity) -> fold ->
plan -> subagent-driven execute -> isolated build/deploy -> kill-aware
mc2_01 smoke (`[DEPTH_TRANSITION v1]` zoom-invariant-by-precision canary) ->
USER visual gate. Branch isolated; user integrates separately. Per the user
plan this spec is the session deliverable; adversarials/plan/execute are the
NEXT session.
