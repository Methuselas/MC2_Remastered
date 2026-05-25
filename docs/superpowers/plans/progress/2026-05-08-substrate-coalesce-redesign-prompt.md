# Substrate Multi-Draw Coalesce — Redesign Execution Prompt

> **Purpose:** Self-contained prompt for a fresh Claude Code session to
> redesign and ship the substrate multi-draw coalesce slice that
> previously failed adversarial review. Paste into a new session at
> `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\` and run.

---

## Goal

Replace the per-actor-type `glDrawElementsIndirect` loop in
`GpuStaticPropBatcher::flush()` (sites at `gos_static_prop_batcher.cpp:1542/1652/1704`)
with a `glMultiDrawElementsIndirect` issue per state-class group. Today
323 separate draws inflate `Render.GpuStaticProps` from ~120 µs (legacy
CPU instanceCount overwrite path) to ~2 ms (substrate-on, GPU pipeline
serialization). Coalescing collapses 323 → 2 multi-draw issues (alpha-test
off / on, only state dimension that varies per the prior audit) and
should drop the zone to ≤200 µs.

This unblocks `MC2_GPU_CULL_SUBSTRATE=1` as the default-on path, which is
a hard prerequisite for any future GPU-driven render slice that wants
GPU-authored visibility (Track D mech rendering eventually, future
terrain-object batchers, lifecycle gate's wolfman-zoom value, etc.).

## Start here — read in order, do not skip

1. **`docs/superpowers/specs/2026-05-07-substrate-multidraw-coalesce-design.md`**
   — original design that failed adversarial review. Read for the shape
   of intent; do NOT copy the broken parts.

2. **`docs/superpowers/specs/2026-05-07-substrate-multidraw-coalesce-design-review.md`**
   — adversarial review verdict STOP THE LINE with 4 CRITICAL + 6 MAJOR
   + 4 MINOR. The 4 CRITICAL define what the redesign must NOT do. Each
   has a specific path forward listed below.

3. **`memory/track_c_substrate_regression.md`** — captures the
   measured 2 ms regression and root cause (323-bucket GPU
   serialization). Empirical evidence the coalesce is needed.

4. **`memory/mc2_texture_handle_is_live.md`** — load-bearing for
   CRITICAL-3 (bindless conflict). Texture handles mutate per-frame
   via `tex_resolve` because `TransformShape` rewrites them. Bindless
   handles cached at registration would point at stale slot content.

5. **`docs/superpowers/brainstorms/2026-05-01-detail-overlay-consolidation-scope.md`**
   — sibling brainstorm Q2 (lines 126, 135-139) explicitly rejected
   `gl_DrawIDARB` for similar work. The user has since stated tolerance
   for extensions if value justifies them; the rejection still informs
   how to scope and document.

## Critical worktree rules — read before any code change

- **`CLAUDE.md`** — worktree-root discipline file. Especially:
  - "Documentation Discipline — grep at write-time, not after"
  - "Review Discipline" — adversarial review by default for high-stakes
  - "Critical Rules" — Build = RelWithDebInfo, Deploy = `cp -f` +
    `diff -q` to BOTH v0.2 and v0.3, no `cp -r`, no time projections,
    100 ns Tracy floor, `#version 430` shader prefix
  - "Load-Bearing Cull Infrastructure" — substrate path is past the
    main cull gates but still depends on per-actor visibility correctness

## The 4 CRITICAL findings — must resolve in design before code starts

### CRITICAL-1: `u_packetID` lift breaks RAlt+9 debug-mode 2

**Problem:** Original design proposed lifting per-bucket `u_packetID`
uniform into a per-draw value via `gl_DrawID`. But `static_prop.frag:34/62`
consumes `u_packetID` for the RAlt+9 debug-mode 2 hash visualization.
Lifting it breaks the diagnostic.

**Resolution options for redesign:**
- **(a) Preserve via per-instance attribute.** Pack `packetID` into the
  instance buffer SSBO; shader reads it from there instead of the
  uniform. Costs +4 B per instance but works without `gl_DrawID`.
- **(b) Per-instance lookup via SSBO + gl_DrawID.** Use `gl_DrawID` to
  index a per-draw-command SSBO containing `packetID`. Requires the
  ARB_shader_draw_parameters extension or 4.6 core.
- **(c) Pack packetID into bucketGroupID (alpha vs non-alpha) via SSBO
  indexed by gl_BaseInstance.** Hybrid.
- **(d) Compile-time gate the debug-mode 2 hash to legacy per-bucket
  draws** — debug builds pay the un-coalesced cost; release ships
  coalesced. Likely simplest.

Recommend (a) — minimal extension surface, works at `#version 430` core.

### CRITICAL-2: `gl_DrawIDARB` violates `#version 430` rule

**Problem:** Worktree CLAUDE.md "Shader #version" rule pins shaders at
430 core. `gl_DrawIDARB` requires `ARB_shader_draw_parameters` extension
(GL 4.0+) or 4.6 core. Adding it would be the first such extension in
this codebase.

**User stance (recorded 2026-05-08):** "I don't care about the 4.3
restriction lol." Extensions are tolerated when value justifies them.
But the precedent matters — every shader file's `#extension` directives
become reviewable surface.

**Resolution options for redesign:**
- **(a) Skip `gl_DrawID` entirely.** If CRITICAL-1 resolves via
  per-instance attribute, `gl_DrawID` isn't needed. Single multi-draw
  per state group with shared everything-else.
- **(b) Adopt `ARB_shader_draw_parameters` deliberately.** Requires
  `#extension GL_ARB_shader_draw_parameters : require` in every shader
  using it. Document the precedent at `docs/architecture.md` and
  CLAUDE.md "Shader #version" rule. Add an opt-out path for hardware
  that doesn't support it (extension probe at startup).

Recommend (a). If CRITICAL-1 picks (b)/(c), then (b) here is forced;
otherwise stay at 430 core.

### CRITICAL-3: Bindless texture has no precedent in codebase

**Problem:** Original design proposed bindless textures for
texture-binding orthogonality across grouped multi-draw. Zero matches
for `bindless`/`GetTextureHandleARB` in `GameOS/`. First introduction
means: residency lifecycle unspecified, conflicts with
`mc2_texture_handle_is_live.md` (handles mutate per-frame), AMD-driver
first-use surface.

**Resolution: skip bindless.** The audit found state-class grouping
collapses 323 → 2 buckets (alpha-test off / on). With only 2 groups,
sampler-array per group is sufficient and proven (PR1 indirect-terrain
SOLID precedent at `gameos_graphics.cpp:2410-ish`, sampler-array
binding in PR1 bridge). No bindless required for this slice.

**Defer bindless** to a hypothetical follow-up if mech LOD permutations
or future content explodes the bucket count past sampler-array
limits. Not on this slice's roadmap.

### CRITICAL-4: `glGetBufferSubData` per-frame on `GL_DRAW_INDIRECT_BUFFER` is a stall

**Problem:** Original design's Section 5.2 (a) audit included a
per-frame `glGetBufferSubData` to validate the indirect command buffer.
That's a full GPU pipeline stall — contradicts the C2 readback ring
discipline that explicitly used persistent-mapped + fence to avoid
exactly this.

**Resolution:**
- **Eliminate the readback entirely** for the production path. The
  indirect command buffer is GPU-written (compute) and GPU-consumed
  (multi-draw); CPU never reads it post-build.
- **Validation in dev builds only.** A `MC2_SUBSTRATE_COALESCE_VALIDATE=1`
  env-gated path can do the readback for one-shot at-startup or every
  600 frames diagnostic. Default off in shipping.
- **Counter-only validation** as the always-on production gate: the
  existing `[GPU_CULL v1] event=indirect_draw buckets=N overflow=K
  elapsed_us=T` summary captures correctness without a stall.

## Cross-cutting constraints

- **Single-PR ship rule** per worktree precedent. The state-class
  grouping (CPU side: bucket sort + indirect buffer build) and the
  multi-draw issue (CPU side: replace per-bucket loop) MUST land in one
  commit. Partial landing → either no perf win (legacy loop still runs)
  or visual breakage (groups don't have correct shared state).

- **Kill-switch** required: `MC2_SUBSTRATE_COALESCE_LEGACY=1` falls back
  to the per-bucket `glDrawElementsIndirect` loop. Default-off. Lazy-eval
  static pattern same as `gos_terrain_indirect::IsEnabled()` at
  `gos_terrain_indirect.cpp:47-62`.

- **Parity gate** before merge:
  - Visual canary: side-by-side `MC2_SUBSTRATE_COALESCE_LEGACY=1` vs
    unset, both with `MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=1`. Pixels
    must match.
  - Tracy delta: `Render.GpuStaticProps` must drop from ~2 ms to
    ≤200 µs (≥90% reduction; stretch ≤100 µs).
  - DESTROY parity: `[DESTROY v1]` count delta = 0 across tier1 5/5.
  - Counter-only validation per CRITICAL-4 confirms `overflow=0` and
    `elapsed_us` reasonable across tier1 5/5.

- **Adversarial review BEFORE merge** to `claude/nifty-mendeleev`. Per
  worktree CLAUDE.md "Review Discipline" — high-stakes, broad caller
  surface, modifies core renderer state machine. Review the
  redesigned spec doc + the implementation commit. Use the
  `adversarial-plan-review` skill at `.claude/skills/adversarial-plan-review.md`.

- **Cull-cascade safety** per `memory/cull_gates_are_load_bearing.md`:
  the substrate path is past the inView/canBeSeen gates but the
  multi-draw coalesce must not silently lose actors that the per-bucket
  draws would have rendered. Group ordering preserves draw call
  semantics; document explicitly.

## Stage structure (suggested)

Mirror the indirect-terrain SOLID PR1 four-stage ladder:

- **Stage 0 — recon.** Re-grep every cited file:line in the original
  design + review (citations decay; per
  `memory/brainstorm_code_grounding_lesson.md` they decayed in a week
  during PR2 stage 0). Confirm `u_packetID` consumers, the shared-state
  inventory across 323 buckets, the bucket-sort key today, the indirect
  command buffer structure.

- **Stage 1 — spec rewrite.** Address all 4 CRITICAL + 6 MAJOR + relevant
  MINORs from the original review. New design doc at
  `docs/superpowers/specs/2026-05-XX-substrate-coalesce-design-v2.md`.
  Run adversarial-plan-review on the spec BEFORE writing code.

- **Stage 2 — bucket grouping + indirect buffer build.** Refactor
  `gos_static_prop_batcher.cpp` to sort buckets by state class (alpha
  vs non-alpha; verify if any other state varies). Build the indirect
  command buffer in group-contiguous order. NO multi-draw issue yet —
  legacy loop still runs.

- **Stage 3 — multi-draw issue + legacy gate-off.** Replace the
  per-bucket `glDrawElementsIndirect` loop with one
  `glMultiDrawElementsIndirect` per state group. Gate the legacy loop
  off when coalesce is armed (mirror PR1's
  `BeginLegacySolidCluster()/EndLegacySolidCluster()` pattern). Same-PR
  rule: Stage 2 + Stage 3 land together.

- **Stage 4 — soak.** 7-day clean tier1 with substrate ON and coalesce
  ON. Then default-on flip slice (separate, single-commit, post-soak).

## Out of scope for this slice

- `gl_DrawID` / `ARB_shader_draw_parameters` adoption (defer to follow-up if
  needed; first redesign should land at 430 core via per-instance attribute).
- Bindless textures (deferred per CRITICAL-3 resolution).
- Lifecycle gate default-on flip (separate slice, depends on this one
  shipping clean + soak).
- Track D's GPU mech batcher — different render path, doesn't depend on
  this slice's outcome.

## Definition of Done

- All 4 CRITICAL + relevant MAJOR/MINOR review findings resolved in the
  v2 design doc.
- Adversarial review verdict on v2 spec ≠ STOP THE LINE.
- Stage 2 + Stage 3 in one commit with kill-switch.
- Visual canary side-by-side passes.
- `Render.GpuStaticProps` Tracy zone ≤200 µs at mc2_01 normal zoom with
  substrate+coalesce on.
- Tier1 5/5 PASS with `MC2_GPU_CULL=1 MC2_GPU_CULL_SUBSTRATE=1` and
  coalesce default-on; +0 destroys.
- Adversarial review on the implementation commit ≠ STOP THE LINE.
- Memory file written: `memory/substrate_coalesce_shipped.md` with ship
  state, env-var, soak-start date, and counters showing the 90%+
  reduction.
- `MEMORY.md` index entry added under "Rendering / shaders". The prior
  `track_c_substrate_regression.md` entry should be updated with a
  "RESOLVED by [commit hash]" cross-reference.

## Adjacent work in flight (do not touch)

- PR2 session is concurrently working on detail/overlay/mine
  consolidation in `mclib/quad.cpp`, `gos_terrain_indirect.cpp/.h`.
  Different render path; no overlap.
- Track D Slice A is being executed in a separate worktree session.
  Different render path; no overlap.
- Lifecycle gate is paused (architectural stopgap, obsoleted by future
  GPU-driven paths).

Coordinate any merge to `claude/nifty-mendeleev` to avoid mid-edit
collisions; check `git status` before any commit.
