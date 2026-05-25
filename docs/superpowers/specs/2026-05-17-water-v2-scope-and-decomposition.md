# Water v2 - Scope, Decomposition, and Residual Ledger

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated worktree; keep-as-is, integrate later)
**Predecessor:** Water v1 shipped (commits `fb75dad..a598013`): GPU-resident terrain-side
depth absorption (Beer-Lambert), camera-stable Fresnel/sky, flat calm surface,
legacy detail/spray suppressed. Marker-gated + user visual-approved on mc2_01.
**North-star anchor (`.planning/PROJECT.md`):** every slice GPU-driven, zero new
per-frame CPU, modern GL (SSBO/indirect), minimal-touch, named contracts, no
time projections, stock-playability preserved.

---

## 1. What v1 deliberately deferred (the v2 backlog)

From the v1 spec non-goals + in-code `TODO(water-v2)` seams + the visual-debug
session outcome:

| # | Slice | Why deferred from v1 | North-star fit |
|---|---|---|---|
| S1 | **Living surface** - animated wave normals from a CONTINUOUS world-space coord (`WorldPos.xy`), NOT the wrap-corrected `Texcoord` | v1's Texcoord-derived normal banded at UV-loop seams; shipped flat (`NORMAL_STRENGTH=0`) with the exact reactivation seam left in code | Pure FS, zero new per-frame CPU, no new pass. Highest value/risk. |
| S2 | **Screen-space refraction** - sample opaque scene color, perturb by wave normal | needs scene-color read; AMD feedback-loop (v1-rev1 analysis: blit `sceneColorTex` -> copy before water draw) | GPU; one per-frame blit (1 CPU call, GPU copy) - acceptable, documented hazard |
| S3 | **GPU-driven planar reflection** - re-issue the EXISTING indirect terrain dispatch into a quarter-res reflection FBO with a reflected MVP | v1 brainstorm rejected the original spec's CPU re-render; GPU-driven re-dispatch is the north-star-correct form | GPU; CPU cost = bind FBO + 1 indirect redispatch + 1 mat4. Highest scope. |
| S4 | **Detail/spray rework** - replace the suppressed legacy tiled `tex2` path with the S1 procedural surface (or a flow-map) | v1 suppressed it (user-chosen); `o_isWater==2` verbatim path kept dead | Folds into S1; removes a legacy path (north star 2) |
| S5 | **WaterStyle UBO** - promote v1 compile-time consts to std140/std430 UBO | infra-ahead-of-need was forbidden in v1 | ONLY when per-biome/mod config is actually wanted. NOT scheduled until that trigger exists. |

**Sequencing rationale:** S1 first (highest value-for-risk, pure shader,
restores the "life" v1 lacks, code seam already present). S4 folds into S1
(the reworked surface replaces the suppressed detail need). S2 then S3
(reflection is the largest scope + the original CPU-cost trap; do it last,
GPU-driven). S5 is demand-gated, not scheduled.

Each slice is an independent spec -> 2 adversarials (opus|sonnet,
adversarial-plan-review skill) -> plan -> subagent-driven execute ->
build/deploy to the isolated `mc2-win64-water` -> kill-aware `mc2_01` smoke
(marker gate, exit-code-agnostic) -> user visual tuning. Same discipline as v1.

---

## 2. Residual ledger - pre-existing, explicitly NOT v1-owned and NOT v2 features

Recorded so they are never silently conflated with water-material work. These
are separate bugs with their own (existing) ownership:

- **Intro pan still messed up** - the un-armed legacy water path
  (`water_fast_prog_` + `gos_terrain_water_fast.vert` + `gos_tex_vertex.frag`).
  v1/v2 material work is MDI-armed-path only and explicitly leaves the un-armed
  path untouched. Pre-existing; user confirmed "was messed up originally."
- **Zoom z-fight + depth-off + water-sits-low** - the constant screen-z
  depth-fudge distance-nonlinearity. Documented residual:
  `memory/water_fastpath_interim_fixes_and_residuals.md` +
  `memory/vulkan_aligned_depth_bias_ruling.md` (ruling-compliant clip-z fix
  pending, decided 2026-05-15). v1 Section 10 lists the z-bias as load-bearing
  UNTOUCHED. This is its own slice under the depth-bias ruling, NOT water-v2
  material scope.
- **Disappears 1-2 frames on pans** - matches the pre-existing arming/MVP-lag
  residual class (`water_fastpath_interim_fixes_and_residuals.md`). Material
  shader work is static-per-recipe and cannot introduce per-frame geometry
  visibility changes; not v1/v2-owned.

If any of these is to be fixed, it is a separate effort under its existing
documented ownership - do not fold into a water-v2 material slice.

---

## 3. LOAD-BEARING design ruling (user, 2026-05-17) - reshapes the backlog

> MC2 has **no real sun "for now"**, so the fake Fresnel/specular sky terms
> are removed. The water base is **100% camera-independent** (`f(WorldPos,
> time)` only). The **ONLY** legitimate camera-dependent term in the entire
> water material is **S3 terrain planar reflection**. Any future camera-
> dependent code in water MUST be terrain reflection (S3) and nothing else.

This is a named contract: "water-base-is-camera-independent; S3-reflection-is-
the-sole-camera-dependent-term." It supersedes S1 rev2's Fresnel/sine approach
(`SKY_TINT`/`fres`/`spec` deleted from the shader, commit `8ee5d12`).

## 4. Status and re-prioritised plan (post-ruling)

- **S1 - DONE** (`8ee5d12`, user-approved "beautiful"). As-built: BAR-style
  dual counter-scroll fBm value-noise, fully camera-independent, brighten-only
  crest + near-white additive glint, seam-free, precision-safe, pure FS. The
  S1 spec's rev2 design (continuous-coord sine + Fresnel) is superseded; the
  commit `8ee5d12` message + this doc are the authoritative as-built record.
- **S4 - DONE (subsumed by S1).** The legacy `o_isWater==2` tiled detail stays
  suppressed; S1's procedural surface is the replacement. No separate slice.
- **S3 - GPU-driven planar terrain reflection = THE active slice ("the rest").**
  Explicitly user-wanted (the ruling: terrain reflection is the one thing that
  should be camera-dependent). Largest scope; must be GPU-driven (re-issue the
  existing indirect terrain dispatch into a quarter-res reflection FBO with a
  reflected MVP - the original spec's CPU re-render is rejected per north-star).
  Spec: `2026-05-17-water-v2-s3-planar-reflection-design.md` (next).
- **S2 - screen-space refraction: DEPRIORITISED / reassess.** Not user-
  requested; the absorption water reads acceptably opaque; refraction
  reintroduces the v1-rev1 scene-color feedback-loop hazard for marginal gain
  and is mildly view-dependent (tension with the ruling). Park unless the user
  asks; not scheduled ahead of S3.
- **S5 - WaterStyle UBO: demand-gated**, unchanged (only when per-biome/mod
  config is actually wanted).
- **S6 - armed-water `setupTextures` decouple (CPU-offload / legacy-retirement;
  NOT a visual slice).** Distinct from S3 (visual reflection) and S4 (subsumed).
  Ladders up north-star #1 (minimize per-frame CPU via GPU offload) + #2
  (retire legacy). Grep-grounded finding (2026-05-17, producer-side proven):
  the armed GPU water DRAW is fully decoupled from `TerrainQuad::setupTextures`
  (`mclib/quad.cpp:704`; Tracy zone `quadSetupTextures`, sole caller
  `mclib/terrain.cpp:1780`) - armed water = mission-static recipe SSBO
  (`WaterStream::Build`, `terrain.cpp:622`) + per-frame compute + MDI, consumes
  none of `setupTextures`' `waterHandle`/`uvData` output. BUT the armed water
  path is NOT "just the upload": it still pays the full unconditional
  per-frame `setupTextures()` loop over all windowed quads because the armed
  narrow-candidate harvest (`WaterStream::AppendNarrowCandidate`,
  `terrain.cpp:1804`) is bolted INTO that loop, and the per-water-quad
  projection block (`quad.cpp:1006-1321`, up to 4x `projectForTerrainAdmission`)
  is only SOLID-arm-gated, not water-arm-gated. **Slice goal:** make armed
  water truly upload-only. Two grounded changes: (i) arm-gate the
  `setupTextures` water-projection block when armed; (ii) replace the in-loop
  narrow-candidate harvest with mission-static recipe data + the EXISTING
  GPU-side `pzOk` visibility gate (`gpu_driven_water.comp:236`,
  `water_stream.cpp:1141`). Un-armed legacy water remains a real
  `setupTextures` consumer by construction - leave intact (un-armed path is
  out of scope, same as S1/S3). Methodology: `mc2-cpu-gpu-offload-expert`
  (recon cost-split -> design + adversarial -> stage -> parity -> default-on);
  observer-effect caution per `memory/cost_split_instrumentation_is_observer_
  effect_dominated.md` (use clean total-frame Tracy, not per-quad chrono).
  Not scheduled ahead of the user's stated S2/S3 priorities; captured so it
  is not lost. Full grounded trace is in this session's audit (the producer-
  side proof + the adjacent `addTriangleBulk(waterHandle...)`-still-fires-armed
  trap at `quad.cpp:1307-1308`).

Discipline per slice unchanged: spec -> 2 adversarials (opus|sonnet,
adversarial-plan-review skill) -> plan -> subagent execute -> isolated
`mc2-win64-water` build/deploy -> kill-aware `mc2_01` marker-gated smoke ->
user visual tuning.
