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

## 8. Adversarial fold - BLOCK resolved (2026-05-18, opus+sonnet, code-grounded vs merged nifty)

Both passes BLOCK; root defect = the design under-scoped which LIVE CPU
paths assume forward-Z. Greybeard (both): reverse-Z IS the root-class
meta-fix; concept sound, scoping was the defect. Resolution = per-consumer:
RE-DERIVE live paths for reversed-Z; FENCE only genuinely-dying paths.

**Fence vs re-derive (resolves C1 / U6 + the quad.cpp fork):**
- FENCE (forward-Z compat transform at the seam; genuinely dying):
  `Camera::inverseProjectZ` scalar path - sole consumers grep-confirmed =
  TacMap minimap footprint (`gametacmap.cpp` via `inverseProjectForPicking`)
  + CPU-raster water (un-armed intro/deploy). These only.
- RE-DERIVE for reversed-Z (LIVE, not dying - fencing would be debt in a
  hot/default-ON path):
  - `quad.cpp` terrain admission `pz_adj = osp.z + FUDGE; pzc = [0,1)` at
    `:2222` (+ `:2258` parity-probe twin) AND the CPU-raster gVertex.z
    `[0,1)` tests `:2547,2604,2753,2971,3117`. Confirmed (grep + history
    `206990f`): NO `(-1,1)` form exists anywhere - all are real forward-Z
    `[0,1)`. Re-derive bounds/fudge-sign for near=1/far=0; add an
    admission-count parity probe pre/post (silent-drop guard).
  - `txmmgr.cpp:~1894` dynamic shadow-cone `ndc[8][3]` z-literals
    `{0.0 near, 1.0 far}` -> flip to `{1.0 near, 0.0 far}` (LIVE GPU path
    that replaced the old CPU shim; update, do not fence).

**Folded mechanical findings (into U2/U3/U4/U7 + sequencing):**
- U2 +sites (CRITICAL, were omitted): `gameos_graphics.cpp:6711`
  `gos_ForceApplyRenderStates` hardcoded `glDepthFunc(GL_LEQUAL)` (frame
  reset - defeats GEQUAL globally); `gameos_graphics.cpp:3949-3952`
  `applyRenderStates` ZCompare switch has only ALWAYS/LEQUAL/LESS +
  `default:gosASSERT(0)` - the `gos_State_ZCompare` enum semantics for
  case1/2 must be REMAPPED to GEQUAL/GREATER (not a constant swap).
- U2 clears: zero `glClearDepth` calls exist today; ~7 `GL_DEPTH_BUFFER_BIT`
  clears. Partition rule (explicit): scene-depth clears -> add
  `glClearDepth(0)` + flip; shadow-pass clears stay 1.0 / GL_LESS (shadows
  EXCLUDED v1). Same scene/shadow partition applies to the ~22 glDepthFunc
  literal sites - shadow `GL_LESS` sites must NOT flip.
- U4: exactly ONE biased `gl_FragDepth` write (`gos_terrain.frag:~772`):
  `max->min`, `+0.0005 -> -0.0005`. The other ~10 writes are bare
  `gl_FragCoord.z` pass-throughs - NO change. The `+0.0005` is a DECOUPLED
  literal (not sourced from `terrain_depth_bias.hglsl`) - flip it
  independently of U3. Count is ~11 not ~12; debug-mode depth range
  `lo=0.85,hi=1.0` also inverts (cosmetic, note in plan).
- U3: rewrite the static_assert INEQUALITIES (`OVERLAY<0 && WATER>0` ->
  `OVERLAY>0 && WATER<0`), not just constant values; preserve the ordering
  invariant under GEQUAL (decal wins tie = larger reversed-Z; water loses
  = smaller). Explicitly partition: FAST-regime constants flip; RASTER /
  `WATER_DEPTH_FUDGE_RASTER` belong to the FENCED path and do NOT flip.
- Sequencing (corrects Section 4 ordering): U6 fence + ALL live-path
  re-derivations (quad.cpp, txmmgr.cpp, the two gameos_graphics depthfunc
  sites) must land ATOMIC with U1+U2+U3. The design's "U6 after U4/U5" is
  wrong - a CPU path seeing reversed-Z with a forward fudge mid-landing =
  global break. U4/U5 shader re-derivation may follow against the landed
  engine; U7 canary last.
- U7: `[DEPTH_TRANSITION v1]` dz formula itself sign-inverts under
  reverse-Z - the probe needs a sign-aware code update, not just a new
  expected value, or it fires spuriously. Re-baseline includes the probe
  fix.

Status: BLOCK -> folded. Plan stage re-greps every site (Rule 0) and
treats (U1+U2+U3 + U6-fence + live re-derivations) as ONE atomic task.

## 9. Re-fold v2 - closure-adversarial RE-BLOCK (2026-05-18, opus, code-grounded)

Closure pass verdict FOLD-INSUFFICIENT. Section 8 CLOSED C2/U4/f1 but
NARROWED (not closed) the root C1, and its U3 FAST/RASTER partition is
structurally impossible. Section 8's partition wording is SUPERSEDED by
this section where they conflict.

**C1 - corrected framing (the actual closure task).** `Camera::projectZ`
(`camera.h:~436-460`) does `xformCoords.Multiply(coords, worldToClip)`
then `screen.z = xformCoords.z * rhw`. It READS `worldToClip` -> it
AUTO-FOLLOWS the U1 matrix swap; it does NOT hand-roll near/far. So
projectZ needs NO math change. The defect is the forward-Z ASSUMPTION
that downstream consumers make on the returned `screen.z`/bool. All 7
`projectFor*` wrappers (`camera.h:526-624`) delegate to this one
projectZ. The fence stays ONLY `inverseProjectZ` (the INVERSE path;
TacMap + CPU-raster water). **The one open item that needs fresh-context
deep work (plan-stage, blocking):** read projectZ's FULL body and
determine where the forward-Z `[0,1)` z-assumption lives:
  (i) if projectZ's returned BOOL embeds a forward-Z z-range test ->
      re-derive it ONCE inside projectZ -> all 7 wrappers fixed
      consistently (clean, meta-fix-consistent); OR
  (ii) if the bool is screen-XY-rect only and the z-range/`+FUDGE`
      assumption lives solely in specific consumers (quad.cpp terrain
      admission cluster C2; `gameobj.cpp:2090` object-admission;
      `clouds.cpp:212`/`crater.cpp:323+`/`weather.cpp:489+` effect-
      admission) -> enumerate + re-derive each consumer's z assumption.
  Resolve (i) vs (ii) FIRST; it determines C1's true blast radius. The
  consumer list above (opposite-direction grep of all 7 wrappers) is the
  enumeration floor.

**U3 - corrected (resolves the impossible partition).** `TERRAIN_DEPTH_
FUDGE` is shared by FAST (`WATER_DEPTH_FUDGE_FAST`), RASTER
(`WATER_DEPTH_FUDGE_RASTER`, alias `WATER_DEPTH_FUDGE`, the quad.cpp
consumer) AND quad.cpp's `pz_adj` admission (`terrain_depth_bias.h:
55/59/62/65`). There is NO FAST/RASTER partition. **ALL constants flip
for reversed-Z.** The dying legacy path (CPU-raster water +
`inverseProjectZ`) is handled by a COORDINATE TRANSFORM at the fence
seam (reversed-Z -> forward-Z before the legacy fudge math), NOT by
keeping any constant un-flipped. The header comment "RASTER ... do NOT
change" is a Fix-B-era instruction EXPLICITLY SUPERSEDED by reverse-Z.
static_assert inequalities rewritten: `OVERLAY_DEPTH_BIAS > 0 &&
WATER_DEPTH_BIAS < 0` (under GEQUAL: decal wins tie = larger reversed-Z;
water loses = smaller); keep the `< 2*TERRAIN` abs bound (re-expressed
for the flipped sign).

**Shadow ZCompare carve-out (new MAJOR, fold this).** The
`applyRenderStates` ZCompare case1/2 -> GEQUAL/GREATER remap (Section 8)
MUST exclude shadow-pass writers. Grounded shadow ZCompare=2 writers:
`txmmgr.cpp:~2316/~2327`. Shadows are EXCLUDED v1 (ortho, stay
forward-Z/LESS). Plan-stage V2: enumerate EVERY `renderStates_[gos_
State_ZCompare]` writer and partition scene (remap) vs shadow (leave
forward). Likewise the depthfunc-literal partition is per-site
EXHAUSTIVE, not "~22 generic" - the literal LEQUAL set incl.
`gameos_graphics.cpp:2231/2628/2966/3153/3313/6711/7095/7206/7272`,
mech/static-prop batchers `~1063/~2899`, and the LESS shadow sites
`4436/4580/4650/7121/7236/7299/gos_postprocess.cpp:1139/1163` which must
NOT flip.

CLOSED clean (do not re-open): C2 (re-derive set complete), U4 (one
biased write `:772`, ~10 pass-throughs, decoupled `+0.0005`), f1 (probe
dz sign-invert = code fix). Greybeard: reverse-Z remains the correct
root-class meta-fix; the fence is a seam coordinate-transform (NOT a
forward-Z patch island), so the meta-fix stays substitutive.

**Plannable-state gate:** resolve the C1 (i)-vs-(ii) projectZ-body
question (the only remaining design unknown); everything else above is a
mechanical plan-stage enumeration. Then writing-plans -> ONE atomic task
(U1+U2+U3 + all-constants-flip + fence-seam-transform + all live
forward-Z consumer re-derivations + scene/shadow partition) -> subagent
execute. Recommend a FRESH SESSION owns the C1 resolution + plan +
execute (deep projectZ-body read + clean context); see the handoff doc.
