# Water v2 - S3 (Terrain Reflection) HANDOFF for a fresh session

**Date:** 2026-05-17  **Author:** prior session (Opus)  **Status:** S1 shipped;
S3 is the remaining target, BLOCKED in its original form, REBORN below.

You are picking up cold. Read this top to bottom; it is self-contained.
Everything cited is grep-verifiable - obey Rule 0 (grep before trusting any
file:line; line numbers drift, symbols are stable).

---

## 0. One-paragraph situation

MechCommander 2 OpenGL engine. Water was modernized in two shipped slices on
an **isolated worktree**: v1 (GPU-resident depth-absorption material) and
water-v2 **S1 "living surface"** (camera-independent organic animated water
via dual counter-scroll fBm value-noise). Both are user-visually-approved and
committed. The remaining want is **S3: a reflection of the terrain in the
water.** S3's first design (per-frame second compute dispatch) was killed by
two independent adversarial reviews because it would corrupt load-bearing
terrain machinery. S3 must be re-approached via a mechanism that never touches
that machinery (Section 6). Your job: build S3 the clever way.

## 1. Worktree, branch, deploy (ISOLATED - do not deviate)

- **Worktree (source + own build64):** `A:/Games/mc2-opengl-src/.claude/worktrees/water-material-v1/`
- **Branch:** `claude/water-material-v1` (forked from `claude/gpu-driven-rendering@8c1c491`).
  22 commits ahead. **Keep isolated - do NOT merge.** The shared
  `claude/gpu-driven-rendering` branch has other active sessions; integration
  is the user's separate deliberate step. Never build/deploy from
  `.claude/worktrees/gpu-driven-rendering/`.
- **Dedicated deploy dir:** `A:/Games/mc2-opengl/mc2-win64-water/` (independent
  4.9G mirror). Deploy ONLY here. **NEVER deploy into `mc2-win64-v0.4/`** - a
  concurrent priority session smokes that live; clobbering it is the exact
  hazard the isolation removes.
- **No emoji in any file. No wall-clock time projections** (describe scope in
  code dimensions). These are hard project rules.

## 2. Build / deploy / smoke discipline (proven this session)

- **Configure (fresh worktree build64 needs it once):** the exact proven
  command (SDL2/GLEW/ZLIB dep vars from the shared `3rdparty`) is in
  `docs/superpowers/plans/2026-05-17-water-material-v1-gpu-driven.md` Task 5
  Step 1. Copy it verbatim.
- **C++ change => full relink** (`rm build64/RelWithDebInfo/mc2.exe` + changed
  .obj, or `--clean-first`), `--config RelWithDebInfo` always. **Shader-only
  change => hot-reload** (just redeploy the `.frag`; shader hot-reload FAILS
  SILENTLY on bad compile - always grep the engine log for `0(N): error` /
  link error after redeploy or you will tune a stale shader for an hour).
- **Deploy:** per-file `cp -f` + `diff -q` (never `cp -r`).
- **Smoke (kill-aware, the priority session contends):**
  ```
  cd <worktree>
  MC2_SMOKE_MODE=1 [PROBE_ENVS] py -3 <worktree>/scripts/run_smoke.py \
    --mission mc2_01 --duration 30 --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
  ```
  `mc2_01` ONLY, 30s (it is the heavy water map). `--keep-logs` is REQUIRED
  (the harness writes the per-mission engine log only on fail/keep-logs; the
  marker gate needs it on pass too). Before launching, check no `mc2.exe`
  from `*v0.4*` is running (priority session) - if it is, WAIT (background a
  poll), do NOT `--kill-existing` it, do NOT run concurrently.
- **Gate by MARKERS, not exit code** (a killed run can exit 0): require, in
  the latest `tests/smoke/artifacts/<ts>/mc2_01*.log`: `[SMOKE v1]
  event=summary result=pass` (run completed) AND your S3 positive probe AND
  clean compile (no `0(N): error`). Then **the real gate is the USER's visual
  judgment** - the smoke window is user-driven; they will tell you what they
  see. Never ask them to re-run; never claim a visual result yourself.

## 3. How to use agents / subagents (this matters - read it)

The `mc2-*` advisor files are NOT spawnable as `subagent_type` from a
root-launched session (verified: "Agent type not found"). The PROVEN pattern
(used ~10x this session, works) is: dispatch a **general-purpose** Agent and
in its prompt tell it to first **Read** the relevant advisor file(s) and the
skill file, adopt their work protocol (Rule 0), then do the task. Files:
- `<worktree>/.claude/agents/mc2-render-expert.md` (render pipeline, FBOs,
  state save/restore, hook ordering, feedback loops)
- `<worktree>/.claude/agents/mc2-terrain-indirect-expert.md` (the indirect
  terrain dispatch, thin records, ring/fence machinery, dispatch-MVP anchor)
- `<worktree>/.claude/agents/mc2-shader-expert.md` (GLSL, the water FS,
  camera-independence, precision)
- `A:/Games/mc2-opengl-src/.claude/skills/adversarial-plan-review.md` (the
  adversarial review skill - dispatch prompts for reviews MUST say "use the
  adversarial-plan-review skill" verbatim, code-grounded, CRITICAL/MAJOR/MINOR)
- `.claude/agents/DOMAINS.md` (routing table)

**Model routing:** opus for architecture / grounding / adversarial reviews;
sonnet for mechanical implementation, spec-compliance review, code-quality
review. Always give subagents isolated, fully self-contained prompts (they
have zero context).

**The discipline that worked all session (follow it for S3):**
brainstorming (if creative scoping needed) -> spec doc -> **2 adversarial
reviews (opus + sonnet, code-grounded, adversarial-plan-review skill)** ->
fold findings -> implementation plan -> **subagent-driven execution**
(per task: fresh implementer subagent -> spec-compliance reviewer ->
code-quality reviewer, loop until clean) -> final whole-impl review ->
build/deploy isolated -> kill-aware mc2_01 smoke -> user visual tuning loop
(hot-reload consts; iterate fast; "clean up after" tuning is expected).
The 2-adversarial gate is non-negotiable for S3 (it caught the original
S3 BLOCK and every real bug this session).

## 4. What is already shipped (do NOT redo; inherit, don't re-derive)

- **Water v1** (depth absorption, Beer-Lambert, GPU-resident terrain-side
  thickness) - commits up to `a598013`.
- **Water-v2 S1** (living surface) FINAL = commit `89d329b`. As-built:
  `shaders/gos_terrain_water_mdi.frag`, the armed MDI program
  `s_waterMdiProg` (built in `GameOS/gameos/gameos_graphics.cpp`
  `renderWaterFastPath()`), `o_isWater==1` base branch. It is **100%
  camera-independent**: dual counter-scroll fBm value-noise (`h21`/`vnoise`/
  `fbm3` helpers in the .frag), brighten-only crest + near-white additive
  glint, `waveLOD` distance fade (anti-alias, distance NOT angle). Final
  tuned consts live at the top of the FS. Pure FS, hot-reloadable.
- **S4 (legacy detail/spray) subsumed** by S1; `o_isWater==2` discard stays.
- Authoritative design records: `docs/superpowers/specs/2026-05-17-water-v2-
  scope-and-decomposition.md` (Section 3-4 = the LOAD-BEARING ruling),
  `...-s1-living-surface-design.md` (rev2 superseded; as-built = commit
  `89d329b` + scope doc), `...-water-material-v1-gpu-driven-design.md`.

## 5. LOAD-BEARING ruling S3 MUST obey (user, 2026-05-17)

> MC2 has **no real sun "for now."** The water base is **100% camera-
> independent** (`f(WorldPos,time)` only). The **ONLY** legitimate camera-
> dependent term in the entire water material is the **S3 terrain
> reflection**, Fresnel-weighted. Any camera-dependent code you add MUST be
> the reflection and nothing else; do not reintroduce fake Fresnel/specular
> sky terms (v1's `SKY_TINT`/`fres`/`spec` were deleted for this reason).

Other inherited invariants (regressing any is a failure): z-bias untouched
(water `WATER_DEPTH_FUDGE_FAST`; the on-screen water/terrain zoom z-fight is a
PRE-EXISTING residual, NOT S3 scope - see Section 8); `[WATER_MAT v1]` and
`[WATER_DEPTHPROBE v2]` probes untouched; MRT `GBuffer1` (location 1,
`rc_gbuffer1_screenShadowEligible`) untouched; no GPU readback /
`glClientWaitSync`-after-write (sync-stall lesson); AMD feedback-loop rule
(`docs/amd-driver-rules.md` - a sampled texture must not be a bound FBO
attachment); the dispatch-MVP frame anchor lesson: any terrain-MVP you
consume must be `gos_terrain_indirect_getDispatchMvp16()` **gated on
`IsFrameSolidArmed()`** (a freshly-sampled `terrain_mvp_` causes a 1-frame
lag - water already paid for this once,
`memory/water_fastpath_interim_fixes_and_residuals.md`).

## 6. S3: why the first design is DEAD, and the REBORN approach

Read `docs/superpowers/specs/2026-05-17-water-v2-s3-planar-reflection-design.md`
in full - especially the "DUAL ADVERSARIAL OUTCOME" section. Summary:

**DEAD approach (do NOT revive without the full refactor):** per-frame second
`gos_terrain_indirect::ComputeDispatch()` with a mirrored MVP. Both reviews
proved it mutates global ring/fence/`g_indirectCmdBuffer` state and
**unconditionally overwrites `g_dispatchMvp16`** (which would re-break the
water 1-frame-lag bug v2 just fixed), the cmd count is primary-camera
cull-windowed, and the thin VS is a two-stage pixel-grid projector
(`terrainViewport`+`mvp`) that quarter-res breaks. Real S3 down this path =
a dedicated terrain-indirect-expert-led refactor of the giant-triangle Fix-A/B
machinery (the 4 requirements are enumerated at the end of that spec).

**REBORN approach (recommended - the BLOCK was mechanism-specific, not
reflection-specific). Brainstorm/choose between, then run the full discipline:**

- **(A) SSPR - screen-space planar reflection.** A post-process **compute**
  pass that scatters the already-rendered on-screen pixels to their
  water-plane-mirrored screen positions into a reflection buffer, using ONLY
  the existing scene color + depth (`gos_postprocess` `getSceneColorTexture()`
  / `getSceneDepthTexture()`). **Zero terrain-pipeline contact, zero second
  geometry pass** - completely orthogonal to everything the adversarials
  blocked. Limitation: misses occluded / off-screen / behind-camera terrain
  (inherent to screen-space); at MC2's oblique ~30deg camera this misses some
  near-shore terrain - acceptable for a first S3, far better than basic SSR.
  Watch the feedback loop: cannot sample the scene color attachment while
  it is the bound draw target - needs a blit/copy or correct pass ordering
  (the v1-rev1 analysis already worked this out).
- **(B) Low-rate / region-cached planar reflection.** Exploit that **terrain
  is static**: render the mirrored terrain via a DEDICATED simple offscreen
  path only on camera-region change / every N frames (not per frame), cache
  it, sample per-frame in the water FS. Because it is low-rate and uses its
  own dedicated minimal render (not the hot per-frame ring), it never touches
  `ComputeDispatch`/ring/fence/`g_dispatchMvp16`. North-star "bake whatever."
  More plumbing than (A) but a true terrain reflection (no screen-space
  misses). The dedicated render still needs a terrain draw path - design it
  to NOT route through `gos_terrain_bridge_drawIndirect` (which binds
  primary-camera shadow/Fix-A state).

Start S3 by grounding (advisor: render + terrain-indirect) which of (A)/(B)
is the smaller correct first slice, write the S3 spec around it (supersede
the dead one), then 2 adversarials -> plan -> execute. Likely (A) SSPR is
the smallest viable first cut; (B) is the higher-fidelity follow-up.

## 7. Where S3 plugs into the water FS (the consumer side - stable)

`shaders/gos_terrain_water_mdi.frag`, the `o_isWater==1` branch, just before
`FragColor = vec4(col, shore);`. Add the ONLY camera-dependent term: project
`WorldPos` (or screen-space, for SSPR) into the reflection buffer, sample,
Fresnel-weight by `normalize(cameraPos.xyz - WorldPos)` vs world-up, `mix`
into `col` (the S1 color is the base). New sampler on **texture unit 2**
(units 0/1 = tex1/tex2 in `s_waterMdiProg`; verified free; save/restore the
unit-2 binding AND sampler). New uniform(s) via the existing `setM*` lambda
block in `renderWaterFastPath`. Tunable consts (`REFL_STRENGTH`, `REFL_F0`)
hot-reloaded in the visual loop like S1. Graceful fallback when reflection
unavailable: skip the term via a `reflectionOn` int uniform (NOT a black
texture - at grazing angles a black mix darkens water; use the skip).

## 8. Residual ledger (PRE-EXISTING, NOT S3 scope - do not "fix" in S3)

- Intro pan glitch = the un-armed legacy water path (S1/S3 are MDI-armed-path
  only; untouched by design).
- Zoom z-fight + depth-off + water-sits-low = the constant screen-z
  depth-fudge distance-nonlinearity; its own slice under the depth-bias
  ruling (`memory/vulkan_aligned_depth_bias_ruling.md`,
  `memory/water_fastpath_interim_fixes_and_residuals.md`). Reflection
  terrain rendered offscreen does not interact with it - do not attempt it
  in S3.
- 1-2 frame water vanish on pan = pre-existing arming/MVP-lag class.

## 9. First actions for the new session

1. `cd` the worktree; `git log --oneline 8c1c491..HEAD` to see the 22-commit
   trail; read the 3 spec docs + the S3 spec's dual-adversarial section.
2. Dispatch a grounding advisor (general-purpose reading
   mc2-render-expert + mc2-terrain-indirect-expert) to decide SSPR-vs-low-rate
   as the smallest correct first S3 and map the exact integration points
   (scene color/depth access + feedback-loop ordering for SSPR; or the
   dedicated low-rate render path).
3. Write the new S3 spec (supersede the dead one), 2 adversarials, plan,
   subagent-execute, build+deploy isolated, kill-aware mc2_01 smoke + a
   `[WATER_REFL v1]` env-gated positive probe, then the user visual loop.
4. Keep the branch isolated; the user integrates separately.
