# Reverse-Z Float Depth - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan. The dispatch prompt for any execute/review subagent MUST include the verbatim strings "use the adversarial-plan-review skill" and "run the greybeard skill" (CLAUDE.md mandate for structural depth changes).

**Goal:** Convert the scene depth pipeline from forward-Z `[near->0, far->1]` to reverse-Z `[near->1, far->0]` to spread float depth precision evenly and retire the distance-nonlinear z-fight bug class at its root.

**Architecture:** `glClipControl(GL_ZERO_TO_ONE)` already shipped (`gameosmain.cpp:930`). Reverse-Z is the delta on top: flip the projection z-row (U1), the depth comparison direction (U2), and the depth-bias constant signs (U3); clear scene depth to 0 instead of 1. `Camera::projectZ` / the 7 `projectFor*` wrappers / `clipSpaceFrustumAdmit` are reverse-Z-INVARIANT (C1 ruling (ii), Section 9.1 - adversarial CONVERGENT) and need NO change. The legacy CPU-raster-water + `inverseProjectZ` path stays forward-Z behind a coordinate-transform fence seam. Scene flips; shadows (ortho, separate maps) stay forward-Z/LESS.

**Tech Stack:** C++ (mclib, GameOS), GLSL (lockstep depth-bias header), CMake RelWithDebInfo, run_smoke.py.

**Atomicity (LOAD-BEARING):** U1+U2+U3+constants-flip+fence-seam+consumer-rederivation+scene/shadow-partition is ONE non-divisible landing. Partial reverse-Z = broken depth everywhere. Steps may be staged for reviewability but the tree is NOT built/deployed/smoked until ALL steps land. U4/U5 shader `gl_FragDepth` re-derivation and U7 probe-code re-baseline are EXPLICITLY DEFERRED to follow against the landed engine (handoff Section "Atomic landing"); they are NOT in this plan.

**Rule 0 (LOAD-BEARING):** Every file:line below was grep-verified at write-time (2026-05-18). Symbols are stable; line numbers drift. The executor MUST re-grep every cited symbol immediately before editing and edit by symbol/context, never by trusting these line numbers.

---

## File Structure

| File | Responsibility | Change class |
|---|---|---|
| `mclib/camera.cpp` | `cameraToClip` z-row (ortho + perspective) | U1 matrix flip |
| `GameOS/gameos/gameos_graphics.cpp` | `applyRenderStates` ZCompare switch + scene `glDepthFunc` literals | U2 depthfunc partition |
| `GameOS/gameos/gos_postprocess.cpp` | shadow depthfunc/clears (NEGATIVE: must NOT flip) | U2 carve-out |
| `mclib/txmmgr.cpp` | shadow `gos_State_ZCompare=2` writers (NEGATIVE: must NOT flip) | U2 carve-out |
| `GameOS/gameos/gameosmain.cpp` | scene `glClear(GL_DEPTH)` -> add `glClearDepth(0)` | scene clear |
| `mclib/terrain_depth_bias.h` | depth-bias constants + `static_assert` | U3 sign flip |
| `shaders/include/terrain_depth_bias.hglsl` | GLSL lockstep mirror of the above | U3 lockstep (SAME commit) |
| `mclib/camera.cpp` (`inverseProjectZ` callers) | fence-seam reverse->forward Z transform | fence |
| `mclib/quad.cpp`, `code/gameobj.cpp`, `mclib/clouds.cpp`, `mclib/crater.cpp`, `mclib/weather.cpp` | live forward-Z post-divide `screen.z`/`pz` consumers | consumer re-derive |

---

## Task 1 (ATOMIC): Reverse-Z scene depth conversion

**This is the single non-divisible task. All steps below land together.**

### Step 1 - U1: flip the projection z-row in `Camera::*` matrix setup

**File:** Modify `mclib/camera.cpp` (grep `cameraToClip(FORWARD_AXIS` to relocate; verified 2026-05-18 at the parallel block ~2030-2037 and perspective block ~2086-2091).

- [ ] **1a. Ortho/parallel branch** (verified `camera.cpp:2032,2037`). Forward-Z maps `z:[near,far]->NDC[0,1]`. Reverse to `->[1,0]`:

```cpp
// :2032  was: cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = 1.0f / (far_clip-near_clip);
cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -1.0f / (far_clip - near_clip);
// :2037  was: cameraToClip(3, FORWARD_AXIS) = -near_clip / (far_clip-near_clip);
cameraToClip(3, FORWARD_AXIS) =  far_clip / (far_clip - near_clip);
```

- [ ] **1b. Perspective branch** (verified `camera.cpp:2086,2091`; `depth_range = APPLY_FORWARD_SIGN(1.0f)/(far_clip-near_clip)` at `:2065`). Reverse-Z solves `z=near->NDC 1`, `z=far->NDC 0`:

```cpp
// :2086  was: cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = far_clip * depth_range;
cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -near_clip * depth_range;
// :2091  was: cameraToClip(3, FORWARD_AXIS) = -far_clip * near_clip * depth_range;
cameraToClip(3, FORWARD_AXIS) =  far_clip * near_clip * depth_range;
```

Derivation (record in commit msg, not in code): for `NDC = (A*z + B)/z`, requiring `near->1, far->0` gives `A = -near/(far-near)`, `B = +near*far/(far-near)`; `depth_range` already carries `1/(far-near)` and the axis sign, so the edit is exactly: negate-and-near-swap the `FF` term, sign-flip the `3F` term.

- [ ] **1c.** No edit to `worldToClip.Multiply(...)` (`camera.cpp:2288`) or `clipToWorld.Invert(worldToClip)` (`:2291`): `projectZ`, `clipToWorld`, and the inverse path auto-follow the z-row swap (C1 ruling (ii); Section 9.1). Confirm by reading these two lines unchanged.

### Step 2 - U2: partition the depth comparison (scene flips, shadow stays)

**File:** Modify `GameOS/gameos/gameos_graphics.cpp`.

- [ ] **2a. `applyRenderStates` ZCompare switch** (grep `switch(renderStates_[gos_State_ZCompare])`; verified `:3948-3952`). Scene remap:

```cpp
case 0: glDepthFunc(GL_ALWAYS);  break;   // unchanged
case 1: glDepthFunc(GL_GEQUAL);  break;   // was GL_LEQUAL
case 2: glDepthFunc(GL_GREATER); break;   // was GL_LESS
```

- [ ] **2b. Shadow ZCompare=2 carve-out (NEGATIVE).** Grep `renderStates_[gos_State_ZCompare] = 2` and `gos_State_ZCompare, 2` across `mclib/txmmgr.cpp` (handoff floor `~2316/~2327` - re-grep, lines drift). For EACH shadow-pass writer that sets ZCompare=2, the GREATER remap in 2a is WRONG (shadows stay forward-Z/LESS). Resolution: shadow passes must NOT route through case 2 post-flip. Re-grep every `renderStates_[gos_State_ZCompare]` writer; for shadow writers, set their depth func explicitly via the literal path (Step 2c keeps shadow `glDepthFunc(GL_LESS)`), and confirm no shadow path depends on case-2==LESS. Document the full writer partition (scene=remap, shadow=literal-LESS) in the commit.

- [ ] **2c. Scene `glDepthFunc(GL_LEQUAL)` literals -> `GL_GEQUAL`.** Verified scene sites: `gameos_graphics.cpp:2231, 2628, 2966, 3153, 3313, 6711` (`6711` = `gos_ForceApplyRenderStates` hardcoded), `7095, 7206, 7272`. Re-grep `glDepthFunc(GL_LEQUAL)`; flip each to `GL_GEQUAL`.

- [ ] **2d. Shadow `glDepthFunc(GL_LESS)` literals - DO NOT FLIP (NEGATIVE).** Verified shadow sites that STAY `GL_LESS`: `gameos_graphics.cpp:4436, 4580, 4650, 7121, 7236, 7299`; `gos_postprocess.cpp:1139, 1163`. Confirm each remains `GL_LESS` post-edit (regression guard, not an edit).

- [ ] **2e. Shadow PCF sampler compare - DO NOT FLIP (NEGATIVE).** `glTexParameteri(..., GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL)` at `gameos_graphics.cpp:4608, 4673` and `gos_postprocess.cpp:1096, 1338` sample the SHADOW map (forward-Z). Stays `GL_LEQUAL`. Confirm unchanged.

- [ ] **2f. `savedDepthFunc` save/restore (NEUTRAL).** Sites `gameos_graphics.cpp:2436,2451,2531,2881,3079,3216,3335` restore a live-captured func. Read each: confirm the saved value is captured from live GL state (`glGetIntegerv(GL_DEPTH_FUNC,...)`), not a hardcoded literal. If live-captured: no edit. If any hardcodes LEQUAL on save: treat as a Step-2c scene site.

### Step 3 - scene depth clear -> 0 (reverse-Z far)

**File:** Modify `GameOS/gameos/gameosmain.cpp`.

- [ ] **3a.** Verified scene clears: `gameosmain.cpp:475` `glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)` and `:522` (`|GL_STENCIL_BUFFER_BIT`). No `glClearDepth` exists today (grep confirms). Add `glClearDepth(0.0f);` immediately before EACH scene depth clear (reverse-Z: far plane = depth 0):

```cpp
glClearDepth(0.0f);
glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );          // :475
glClearDepth(0.0f);
glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT ); // :522
```

- [ ] **3b. Shadow clears - DO NOT add glClearDepth(0) (NEGATIVE).** `gos_postprocess.cpp:1124,1135,1365` `glClear(GL_DEPTH_BUFFER_BIT)` clear the SHADOW FBO (forward-Z). They keep the GL default `glClearDepth(1.0)`. If Step 3a's `glClearDepth(0)` could leak into shadow state (global GL state), the shadow clear sites MUST set `glClearDepth(1.0f)` explicitly before their clear. Re-grep shadow clear sites; add explicit `glClearDepth(1.0f)` before each shadow `glClear(GL_DEPTH_BUFFER_BIT)` to make the partition state-safe.

### Step 4 - U3: flip depth-bias constants (lockstep, SAME commit)

**Files:** Modify `mclib/terrain_depth_bias.h` AND `shaders/include/terrain_depth_bias.hglsl` (byte-equal mirror; lockstep rule `memory/cpp_glsl_ubo_struct_lockstep.md` - edit in this same step).

- [ ] **4a.** Under reverse-Z the depth comparison is GEQUAL (larger NDC z wins). To preserve the tie-break invariants (decal wins over terrain; water loses), every signed bias inverts. Verified current values `terrain_depth_bias.h:55-65`:

```cpp
constexpr float TERRAIN_DEPTH_FUDGE      = -0.002f;   // was 0.002f
constexpr float WATER_DEPTH_BIAS         = -0.0005f;  // was +0.0005f (< 0: water loses GEQUAL tie)
constexpr float OVERLAY_DEPTH_BIAS       =  0.0005f;  // was -0.0005f (> 0: decals win GEQUAL tie)
constexpr float WATER_DEPTH_FUDGE_FAST   = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_BIAS;   // -0.0025f
constexpr float WATER_DEPTH_DELTA_RASTER = -0.0005f;  // was 0.0005f (RASTER fence-seam regime)
constexpr float WATER_DEPTH_FUDGE_RASTER = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_DELTA_RASTER; // -0.0025f
constexpr float WATER_DEPTH_FUDGE        = WATER_DEPTH_FUDGE_RASTER;
```

- [ ] **4b.** Rewrite the `static_assert` (verified `:67-70`) for the flipped sign convention:

```cpp
static_assert(
    OVERLAY_DEPTH_BIAS > 0.0f && 0.0f > WATER_DEPTH_BIAS &&
    fabsf(TERRAIN_DEPTH_FUDGE + WATER_DEPTH_BIAS) < 2.0f * fabsf(TERRAIN_DEPTH_FUDGE),
    "Reverse-Z depth ordering invariant: OVERLAY>0>WATER and |water-abs|<2*|terrain|");
```
(If `fabsf` is not constexpr-usable in this TU, use the literal magnitude comparison `(... ) > -2.0f*TERRAIN_DEPTH_FUDGE` re-expressed for the now-negative `TERRAIN_DEPTH_FUDGE`; pick the form that compiles and preserves `|water| < 2*|terrain|`.)

- [ ] **4c.** Update the header comment block (`:21-53`) so the ordering prose matches the flipped signs (`OVERLAY_DEPTH_BIAS > 0 > WATER_DEPTH_BIAS`; "water loses the shoreline GEQUAL tie"; "decals win the GEQUAL tie"). Mirror byte-equal into `terrain_depth_bias.hglsl` in this same step. No emoji.

- [ ] **4d. RASTER is NOT un-flipped.** Per Section 9, `WATER_DEPTH_DELTA_RASTER`/`WATER_DEPTH_FUDGE_RASTER`/`WATER_DEPTH_FUDGE` ALSO flip (they feed the legacy CPU-raster path which is handled by the fence-seam coordinate transform in Step 5, NOT by keeping a constant un-flipped). The header's Fix-B-era "RASTER ... do NOT touch" / "do NOT change" comments are SUPERSEDED by reverse-Z; update them to say the RASTER regime flips and is reconciled at the `inverseProjectZ` fence seam.

### Step 5 - fence seam: keep `inverseProjectZ` + CPU-raster water forward-Z

**File:** Modify `inverseProjectZ` callers (grep `inverseProjectZ` / `inverseProjectForPicking`; TacMap + CPU-raster/intro water). `Camera::inverseProjectZ` body verified `camera.cpp:1941-1985`.

- [ ] **5a.** `inverseProjectZ` uses `clipToWorld` (`:1977`) which auto-inverts the reverse-Z `worldToClip` - the inverse MATH auto-follows. The hazard is (1) the degenerate synth branch `:1957-1966` (`if (startZInverse>1.0f) startZInverse=1.0f; screen.z = startZInverse - zPerPixel*screen.y`) which hardcodes a forward-Z `[0,1]` NDC z, and (2) callers (TacMap minimap, CPU-raster/intro water) that pass a screen.z they assume is forward-Z. Decision: `inverseProjectZ` stays forward-Z internally (do NOT edit its body). At each call site that now sources a reverse-Z screen-space z, insert the seam transform `z_fwd = 1.0f - z_rev` (NDC) BEFORE the call, and the inverse (`z_rev = 1.0f - z_fwd`) on any z returned back into the reverse-Z scene.

- [ ] **5b.** Re-grep ALL `inverseProjectZ` / `inverseProjectForPicking` call sites (Rule 0; do not trust this list - the policy-split wrapper trap means raw grep misses typed wrappers, see `memory/policy_split_wrapper_grep_trap.md`). For each, classify: (i) pure screen-XY unproject (z unused) -> no seam needed; (ii) consumes/produces scene-space z -> insert the seam transform. Document the per-callsite classification in the commit. TacMap minimap and CPU-raster/intro water are the known fence consumers (handoff); they must render byte-identical to pre-reverse-Z (USER visual gate).

### Step 6 - re-derive live forward-Z post-divide z consumers

- [ ] **6a. quad.cpp terrain NDC-z bias (auto-handled, VERIFY).** Verified `quad.cpp` sites `vertices[N]->pz + TERRAIN_DEPTH_FUDGE` at `:2527,2544,2584,2594,2745,2941,2951,2961,3109`. These add the U3 constant to post-divide NDC z; the Step-4 sign flip of `TERRAIN_DEPTH_FUDGE` makes them correct automatically. NO per-site edit - but READ each to confirm it consumes `TERRAIN_DEPTH_FUDGE` (or `WATER_DEPTH_FUDGE`) and not a hardcoded `0.002f`.

- [ ] **6b. quad.cpp hardcoded `pz - 0.002f` block.** Verified `quad.cpp:3999-4124` uses literal `vertices[N]->pz - 0.002f` (NOT the named constant). Re-grep `pz - 0.002f` / `pz-0.002f`; read the enclosing function to classify (terrain-shadow projection vs scene). If scene reverse-Z: flip to `pz + 0.002f` (or route via the now-negative `TERRAIN_DEPTH_FUDGE`). If shadow/forward path: leave. Document the classification.

- [ ] **6c. quad.cpp `projectForTerrainAdmission` bool sites (NEGATIVE).** Verified `:1076,1146,1216,1286` and the `pz_adj`/`pzc` admission probe `:2257`. Per C1 ruling (ii) the admission BOOL is screen-XY-rect-only and reverse-Z-INVARIANT. NO edit. The `pz_adj_old = vertices[c]->pz + TERRAIN_DEPTH_FUDGE` probe at `:2257` follows 6a (constant auto-flips). Confirm no admission gate compares `screen.z` against a forward-Z literal.

- [ ] **6d. Other live consumers.** Re-grep and read context for the Section 9 floor: `code/gameobj.cpp:2090` (object cull / `canBeSeen`), `mclib/clouds.cpp:212`, `mclib/crater.cpp:~323+`, `mclib/weather.cpp:~489+`, and the `camera.cpp` shadow projector (`projectForLightingShadow` consumers ~`1882/1910`). For EACH: determine if it compares/derives a forward-Z post-divide `screen.z` (e.g. `z < 1.0`, `z >= 0`, depth-sort, `+FUDGE`). If yes and it is a SCENE consumer -> re-derive for reverse-Z (invert the comparison/sign). If it is a shadow consumer or only uses `screen.xy` -> no edit. Opposite-direction grep for completeness (negative claims need it - `memory/feedback_data_flow_audit_asymmetry.md`). Document every classification + edit in the commit.

### Step 7 - debug instrumentation (same-commit, gated-off)

- [ ] **7a.** Per CLAUDE.md "Debug instrumentation rule" (this rework touches render path + depth state), land an env-gated `[REVERSE_Z v1]` lifecycle print at the projection-matrix build (one print at first reverse-Z matrix construction: near/far, sample NDC z of near & far) and at the fence seam (first seam transform application). Gated behind `MC2_REVERSE_Z_TRACE=1`, silent by default, demote-not-delete. Match the existing `MC2_DEBUG_SHADOW_COLLECT` macro pattern. Add to the `[INSTR v1]` startup banner + the Tier-1 env-var list in CLAUDE.md.

### Step 8 - build, deploy, smoke (ONLY after Steps 1-7 all land)

- [ ] **8a. Full relink** (load-bearing depth-state/inline funcs changed): delete `build64/RelWithDebInfo/mc2.exe` + changed `.obj`s (or `--clean-first`), then `cmake --build ... --config RelWithDebInfo`. RelWithDebInfo mandatory (Release crashes `GL_INVALID_ENUM`).

- [ ] **8b. Deploy to `mc2-win64-water`** (NOT v0.4 - other trees use v0.4; handoff decision). `cp -f` per file + `diff -q`, never `cp -r`.

- [ ] **8c. Kill-aware smoke.** `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing` (never `--kill-existing` against a `*v0.4*` mc2.exe). exit 0 = pass.

- [ ] **8d. Probe gates** (read `tests/smoke/artifacts/<latest>/`): `[DEPTH_TRANSITION v1]` dz must be zoom-INVARIANT BY PRECISION (Fix A is gone - there is no compensating formula; this is the headline reverse-Z success signal); `[WATER_RENDERPROBE v1]` / `[WATER_DEPTHPROBE v2]` still `equal=1`; `0` shader errors; `GL_INVALID_*` count `0`.

### Step 9 - commit

- [ ] **9a.** Single atomic commit (or a tightly-ordered series NOT deployed partially). Stage exactly the edited files (no `git add -A`). Commit message records: the U1 derivation, the scene/shadow partition writer list (Step 2b), the fence-seam per-callsite classification (Step 5b), the consumer classifications (Step 6), and `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`. No emoji. Do NOT push (`alariq/mc2` is never pushed).

---

## Verification gate (post-commit, USER authority)

Automated (Step 8d) is necessary but NOT sufficient. **USER visual gate is the ultimate authority** (handoff Section "Next-session steps" 6):
- No z-fight at ANY zoom (the headline reverse-Z payoff).
- Terrain / POM: no self-occlusion or punch-through.
- TacMap minimap + CPU-raster water (intro/deploy) UNCHANGED (fence correct).
- Fix B / S1 / transparency water UNCHANGED.

A smoke PASS does not verify a depth fix (`memory/smoke_pass_does_not_verify_gpu_driven_shader_fix.md`): require BOTH probe-gate (8d) AND USER visual confirmation before SHIPPED.

## Deferred (NOT this task - follow against the landed engine, BEFORE the USER visual gate)

The atomic engine landing is internally sign-consistent, but scene-depth
SAMPLING shaders read the now-reversed depth buffer and will visibly
regress until re-derived. This arc MUST complete before the USER visual
gate is meaningful (the gate's "no punch-through / fix B unchanged"
criteria cannot be judged through a godray/SSAO regression). Enumerated
2026-05-18 from the implementer's concern (opposite-direction grep of
`sceneDepthTex` / `gl_FragDepth` / depth-sky-test consumers):

- U4: `gos_terrain.frag:~772` biased `gl_FragDepth` (one biased write +
  ~10 pass-throughs; decoupled `+0.0005`) - re-derive for reverse-Z.
- U5 (scene-depth-sampling shaders, each needs U4/U5-class re-derivation
  + `/mc2-amd-shader-review`): `godray.frag` (`step(0.999,depth)` sky
  test inverts - far is now 0), `shoreline.frag`, `ssao.frag`,
  `ssao_blur.frag` (sample `sceneDepthTex`). Re-grep every
  `sceneDepthTex` / scene-depth sampler at that stage (Rule 0).
- U7: `[DEPTH_TRANSITION v1]` probe-code dz sign-inversion fix + canary
  re-baseline (LAST; against the re-derived shaders).

## Self-review (done at write-time)

- **Spec coverage:** U1 (Step 1), U2 (Step 2), U3 (Step 4), fence (Step 5), consumers (Step 6), scene/shadow partition (Steps 2b/2d/2e/3b/6), constants-flip (Step 4), instrumentation (Step 7) - all Section 9 / 9.1 items mapped. C1 invariants (projectZ/wrappers/clipSpaceFrustumAdmit) explicitly NEGATIVE (no edit) per ruling (ii). `clipSpaceFrustumAdmitSphere` comment-debt is filed/out-of-scope (Section 9.1) - correctly absent from edit steps.
- **Placeholder scan:** no TBD/"handle edge cases"; every edit has exact old->new or an explicit re-grep+classify mandate (the latter is required, not a placeholder - lines drift and the consumer set needs control-flow classification that must happen at execute against current code).
- **Type consistency:** constant names (`TERRAIN_DEPTH_FUDGE`, `WATER_DEPTH_BIAS`, `OVERLAY_DEPTH_BIAS`, `WATER_DEPTH_FUDGE_FAST/RASTER`) match `terrain_depth_bias.h` verbatim; `gos_State_ZCompare` case mapping consistent between Step 2a and 2b.
