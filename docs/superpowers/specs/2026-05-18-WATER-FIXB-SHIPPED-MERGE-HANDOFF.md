# Water - Fix B SHIPPED + reverse-Z scoped - MERGE HANDOFF (2026-05-18)

You are picking up cold after `claude/water-material-v1` is merged back into
`claude/gpu-driven-rendering`. Self-contained. Rule 0: grep before trusting
any file:line (symbols stable, lines drift). No emoji; no wall-clock
projections. Authoritative living record:
`memory/water_v2_s1_shipped_s3_blocked_reborn.md` (read it - it has the
corrected ruling + the parked-shoreline + greybeard state).

## 0. FIRST ACTION - verify Fix B survived the merge (do this before anything)

The merge is itself the risk surface. Fix B touched files the concurrent
gpu-driven session may also have changed: `GameOS/gameos/gameos_graphics.cpp`
(heavily), `GameOS/gameos/gos_terrain_water_stream.cpp`,
`GameOS/gameos/gos_terrain_indirect.cpp`, `mclib/terrain_depth_bias.h`,
`shaders/include/terrain_depth_bias.hglsl`, `shaders/gos_terrain_water_fast
*.vert`, `shaders/terrain_overlay.vert`. RESOLVE conflicts (never discard
either side blindly - the Fix B symmetric-mirror block + the two-constant
lockstep header are load-bearing; the gpu-driven side may have its own
hot changes). Then:
1. Confirm the 6-site symmetric-mirror is intact: grep
   `gos_terrain_indirect_getDispatchMvp16` in `gameos_graphics.cpp` - expect
   the canonical ternary `IsFrameSolidArmed() ? ...getDispatchMvp16() :
   gos_GetTerrainMVPMat4(); if(!p) p=gos_GetTerrainMVPMat4();` at the 2
   water binds + the 3 `uploadOverlayUniforms_` callers, byte-equal to the
   one in `gos_terrain_water_stream.cpp` (the cull-feed). The fwd-decls
   `extern "C" const float* gos_terrain_indirect_getDispatchMvp16();` +
   `extern const float* gos_GetTerrainMVPMat4();` MUST be TU-wide (top of
   `gameos_graphics.cpp`, before `renderWaterFastPath`) - the build will
   C3861-fail if a merge moved them after first use.
2. Lockstep header parity: `mclib/terrain_depth_bias.h` and
   `shaders/include/terrain_depth_bias.hglsl` must carry byte-equal
   `TERRAIN_DEPTH_FUDGE=0.002`, `WATER_DEPTH_BIAS=0.0005`,
   `OVERLAY_DEPTH_BIAS=-0.0005`; the C++ `static_assert` ordering invariant
   present.
3. Full relink (RelWithDebInfo; load-bearing inline/lockstep/static change -
   rm exe + the changed .obj or --clean-first). Deploy per the DESTINATION
   worktree's CLAUDE.md target (gpu-driven-rendering deploys
   `mc2-win64-v0.4`, NOT `mc2-win64-water` - that isolation was specific to
   this branch avoiding a concurrent v0.4 smoke; confirm no concurrent
   `*v0.4*` mc2.exe before a kill-aware smoke, never `--kill-existing`
   against one).
4. Re-run the Fix B probe smoke (kill-aware mc2_01, `--keep-logs`,
   `MC2_DEPTH_TRANSITION_PROBE=1 MC2_WATER_RENDERPROBE=1
   MC2_WATER_DEPTHPROBE=1`, point `--exe` at the deployed build). PASS gate:
   `[DEPTH_TRANSITION v1]` `dz_gpuw` flat ~`+0.0005` and `dz_decal` flat
   ~`-0.0025` (= OVERLAY-TERRAIN), both zoom-invariant incl. transition
   frames; `[WATER_RENDERPROBE v1] event=invA` `equal=1` on all real armed
   frames (lone `wframe=1 terrain_dispatch_fp=00000000 equal=0` = benign
   warmup); `[WATER_DEPTHPROBE v2] equal=1`; zero `0(N): error` shader
   lines. If `dz` is not flat or invA flips on real frames -> the merge
   broke Fix B; fix before any new work.

## 1. What SHIPPED + user-validated (the payload of this merge)

**Fix B - zoom z-fight / matrix-share + two-constant depth bias.** Re-
adversarial #2 (opus+sonnet APPROVE) -> 6 subagent-driven tasks (each 2-stage
reviewed) -> final adversarial SHIP -> build (caught a use-before-decl C3861
that 4 review passes missed; fixed by hoisting fwd-decls TU-wide) -> isolated
deploy -> **USER visual gate PASSED** ("z-fighting fixed on decals AND
water, water at the right height, fixed the zoom-out recession") -> probe-
fidelity cleanup (the `[DEPTH_TRANSITION v1]` decal arm was modelling the
removed glPolygonOffset; rewritten to the real producer = probe==producer,
empirically reconfirmed dz_decal flat -0.0025). Authoritative spec:
`docs/superpowers/specs/2026-05-18-AUTHORITATIVE-zfight-matrix-share-
design.md` (Section 12 is operative). Implementation = symmetric-mirror MVP
at the 2 GPU-water binds + all 3 overlay/decal callers (the user-approved Fa
scope - fixes the DEFAULT-play decal pop, not just the dead static bake) +
two oppositely-signed single-sourced depth constants (water `+0.0005`
behind terrain, overlay `-0.0005` in front; terrain `0.002` unchanged
reference) + the `MC2_WATER_RENDERPROBE` invA-tripwire/invB-release-gate
probe. Pre-Fix-B fudge family (Fix A / distance-proportional unifier) is
DEAD - do not build it; reverse-Z (Section 3) finishes that story.

**Known instrumentation residual (NOT a defect):** `[WATER_RENDERPROBE v1]
event=invB_transition` is structurally unobservable in mc2_01 (water armed
from the first probe-visible frame; documented `s_rpPrevArmed=-1` swallow).
invA + dz canary + the USER visual gate cover it; the spec defers ultimate
authority to the visual gate. Do not chase invB-absent as a bug.

## 2. What's PARKED (in history, recoverable, REVIVED by the ruling change)

**BAR-style shoreline polish** (foam + soft/wavy edge). 3 pure-FS attempts
(`c766527`/`6057b9f`/`3c1a933`) did not pass the user visual gate; the
WATER->LAND boundary IS the per-terrain-quad water-mask polygon silhouette +
binary `discard` - a STRUCTURAL wall pure-FS noise cannot smooth. Reverted
to validated state (`d9516f3`); experiment preserved in branch history. The
real fix = Option A depth-buffer soft-contact, which is **now REVIVED** by
the ruling decision below. Full diagnosis in the living record
("BAR-style shoreline polish - ATTEMPTED then PARKED").

## 3. LOAD-BEARING: the camera-dependence ruling was CORRECTED (2026-05-18)

The old "water must have ZERO perceptible camera-dependence; S3/reflection
VOID in practice" was **NOT an invariant** - user verbatim: *"that decision
was mostly due to it being ugly as shit lmao, not some invariant
principle."* **Operative rule now: camera-dependent water terms ARE
permitted IF they look good (sub-perceptible / not-ugly, judged per-feature
at the USER visual gate). A QUALITY bar, not a ban.** Consequences a fresh
session MUST honor (do NOT cite the old "void"/"forbidden" framing):
- Depth-driven water (#2: real refraction, depth-accurate underwater fog,
  depth-soft-contact shoreline = the revived parked Option A) is PERMITTED,
  sequenced AFTER reverse-Z (reverse-Z makes the depth band ~zoom-invariant
  by precision -> camera-dependence drops sub-perceptible largely for free).
  Infra ~75% exists: `sceneDepthTex_` (`gos_postprocess.cpp:~292-300`) is
  built+bound; even a `shoreline.frag` declares but never samples it.
- S3 / planar reflection / specular are NO LONGER permanently void -
  re-pursuable if they clear the visual gate.
Authoritative: living record "CAMERA-DEPENDENCE RULING -
CORRECTED/QUALIFIED" (it explicitly supersedes the 2026-05-17 framing).

## 4. The direction map + the immediate next slice

**Greybeard enabler map:** `docs/superpowers/specs/2026-05-18-water-
enabling-infra-greybeard-map.md` - ranked foundational water infra. **#1
continuous water surface = USER-COMMITTED** ("I definitely want (1) done");
it is the keystone (water is per-terrain-quad + binary discard today, NOT a
surface). Headline: fast-path geometry is already std430-SSBO Vulkan-clean;
the only buffer gap is style-params -> a demand-gated UBO (#4, YAGNI).

**IMMEDIATE NEXT = reverse-Z + float depth (#3).** DESIGN DONE:
`docs/superpowers/specs/2026-05-18-reversed-z-float-depth-design.md`
(commit `a24e259`). Greenlightable (Fix B was the validated prereq;
`glClipControl ZERO_TO_ONE` already shipped at `gameosmain.cpp:~930`).
Gating decisions RESOLVED: D1 = FENCE the legacy scalar
`inverseProjectZ`/`projectZ` (TacMap + CPU-raster water) behind a forward-Z
compat transform, NOT port (= Fork P/F, co-decides #5); D2 = MINIMAL
correct POM `gl_FragDepth` re-derivation in `gos_terrain.frag` (early-Z
wart stays a SEPARATE future slice). Shadows EXCLUDED v1. **Load-bearing:
U1+U2+U3 (projection swap + glDepthFunc/glClearDepth/format + the
depth-bias lockstep sign-flip) must land as ONE atomic change** - a
half-flipped engine is globally depth-broken. The `[DEPTH_TRANSITION v1]`
probe is the canary: post-reverse-Z its dz must be zoom-invariant BY
PRECISION, not by formula (Fix A is gone). Reverse-Z FLIPS the sign of the
Fix B depth constants (U3) - coordinate the lockstep pair in one commit.

**Sequence after reverse-Z:** #5 retire/fence legacy CPU-raster water (Fork
P/F already = fence) -> #1 continuous water surface (the committed
keystone) -> #2 depth-driven water + the revived Option-A shoreline (now
permitted by the qualified ruling).

## 5. Discipline (proven this session - follow it)

Structural depth/render changes: spec -> 2 mandatory adversarials
(opus|sonnet, code-grounded, `adversarial-plan-review` skill, dispatch
prompt MUST say "use the adversarial-plan-review skill" verbatim) -> fold ->
plan (writing-plans skill) -> subagent-driven execute (fresh subagent per
task + 2-stage review: spec-compliance THEN code-quality) -> final
whole-impl adversarial -> isolated build/deploy (FULL relink for
load-bearing C++) -> kill-aware mc2_01 smoke with the probe canary ->
**USER visual gate is the ultimate authority** (it overruled the model 4x
this effort - trust first-hand visual evidence over any grounding claim;
disambiguate artifact->subsystem before root-causing; when an artifact is a
structural silhouette, reframe - do not blind-tune). The BUILD is the real
gate (it caught a use-before-decl every review missed). USER-driven
visual-iteration smokes use `--duration 60` not 30
(`memory/feedback_visual_iteration_smoke_60s.md`).

## 6. First actions for the new session

1. Section 0 - resolve the merge carefully + re-verify Fix B survived
   (probe smoke). Do NOT proceed until Fix B's canary passes post-merge.
2. Read the living record + the 3 spec docs (greybeard map, reverse-Z
   design, AUTHORITATIVE Fix B Section 12) + this handoff.
3. Reverse-Z: 2 adversarials on `2026-05-18-reversed-z-float-depth-
   design.md` (mandated foci in its Section 7) -> fold -> plan -> subagent
   execute -> isolated build/deploy -> smoke (DEPTH_TRANSITION
   zoom-invariant-by-precision) -> USER visual gate.
4. Keep the destination branch's isolation/integration discipline; confirm
   deploy target + kill-awareness from the destination worktree CLAUDE.md.
