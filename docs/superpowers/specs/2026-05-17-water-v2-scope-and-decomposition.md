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

## 3. Active slice

**S1 - Living surface.** Spec: `2026-05-17-water-v2-s1-living-surface-design.md`.
Goal: continuous-world-space animated wave normal (no tile seams), driving the
already-wired Fresnel + specular, restoring surface life lost when v1 shipped
flat - camera-stable, zero new per-frame CPU, pure hot-reloadable FS edit,
reactivating the documented v1 seam (`NORMAL_STRENGTH` + the procedural-wave
block) but sourced from `WorldPos.xy` instead of `Texcoord`.
