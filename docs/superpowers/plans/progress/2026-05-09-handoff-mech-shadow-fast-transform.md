# Handoff: Mech Shadow Fast-Transform (Slice C3-shadow)

> **Purpose:** self-contained prompt for a fresh Claude Code session to plan + execute the shadow callsite fast-transform slice. Pre-loaded with worktree state, Tracy evidence, recon questions, and the cadence pattern from the just-shipped body slice.

## TL;DR for the new session

1. Body fast-transform shipped (commits `4ab312c..` on `claude/gpu-mech-batcher`). Tracy on mc2_10 idle: per-call mean 71µs → 56µs (-21%); mode 71→41µs (-42%); per-frame mech work 1.35→1.13ms (-20%). Bimodal histogram: lower peak ~40µs (fast body) and **upper peak ~100µs (shadow path still full)**.
2. **Your job:** swap `mech3d.cpp:3377` (`mechShadowShape->TransformMultiShape`) to `_PositionsOnly` under a new killswitch, mirroring the body slice's spec/plan/review/execute/smoke/review pattern. Expected delta: ~30µs/call on mechs casting shadows → another ~500µs/frame mainloop on tier1.
3. **Critical recon question:** does `_PositionsOnly` correctly handle the shadow caster geometry path? `MultiTransformShadows` is dispatched alongside `MultiTransformShape` at `mclib/msl.cpp:1763-1766` — confirm that dispatch survives or is independent of the per-leaf flag.

## Worktree state

- **Branch:** `claude/gpu-mech-batcher` based on `claude/nifty-mendeleev`.
- **Path:** `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\`
- **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/` (junction-mirror; mc2.exe + shaders/ are real copies).
- **Killswitches in effect (all default-off, opt-in):**
  - `MC2_GPU_MECHS=1` — Slice A: GPU mech batcher path
  - `MC2_GPU_MECH_LIGHTING=1` — Slice B1: VS-side calc_light
  - `MC2_GPU_MECH_CULL=1` — Slice C1: render-only mech GPU cull
  - `MC2_GPU_MECH_SKIN=1` — Slice C2: weighted multi-bone skinning
  - `MC2_GPU_MECH_FAST_TRANSFORM=1` — Slice C3-revised: body `_PositionsOnly` (just shipped)
  - **You will add:** `MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1` (or similar — pick a name)

## Pattern to mirror

Just-shipped body slice has the exact precedent. Read these in order:

1. **Spec:** `docs/superpowers/specs/2026-05-09-mech-fast-transform-design.md` — body-only design, post-adversarial-review revision (originally body+arms; arms de-scoped at plan review per CRIT-1).
2. **Plan:** `docs/superpowers/plans/2026-05-09-mech-fast-transform.md` — 6-task body slice plan.
3. **Adversarial review trigger:** when you dispatch the plan-time review, the prompt should re-cite the worktree CLAUDE.md "Review Discipline" section and the "Load-Bearing Cull Infrastructure" warning. Specifically: any swap to `_PositionsOnly` inherits the same `listOfVertices[*].argb` consumer-enumeration question — but for shadow, the consumer is `mechShadowShape->RenderShadows()` not `mechShape->Render(true)`. Verify shadow's render path does NOT read `listOfVertices[*].argb` while fast-transform is on.

## Specific recon questions for the shadow slice

The reviewer of the body slice flagged shadow as out-of-scope pending these answers:

1. **`MultiTransformShadows` dispatch:** at `msl.cpp:1763-1766`, this is called alongside the per-leaf `MultiTransformShape` inside `TransformMultiShape`. Does `_PositionsOnly` mode (which flips `s_multiShapePositionsOnly`) preserve `MultiTransformShadows` dispatch, or does it skip it too? Read `tgl.cpp:2650-2730` (the `_PositionsOnly` per-leaf body) to confirm.
2. **Shadow caster registration:** `TG_Shape::MultiTransformShape` registers a shadow caster somewhere (TGL pool? shadow batcher?). Confirm `_PositionsOnly` either preserves that registration OR the registration isn't required when GPU mech batcher is rendering.
3. **`mechShadowShape->RenderShadows()` callsite:** find it (probably `mech3d.cpp` near the Render(true) callsites). Does it consume `listOfVertices[*].argb` (shadow color) the way mechShape->Render(true) does? If yes, stripping the shadow lighting bake would render garbage shadows — same hazard pattern as the arm callsites we deferred.
4. **`useShadows && d_useShadows` gate:** check if `mechShadowShape->TransformMultiShape` runs only when shadows are globally enabled. If yes, the slice's perf claim should account for shadow-disabled scenarios producing zero benefit.

If recon answer is "shadow `_PositionsOnly` is safe": straightforward 3-file slice (killswitch header + cpp def + mech3d.cpp:3377 swap).

If recon answer is "shadow needs additional plumbing" (e.g. `MultiTransformShadows` must run before swap): document and either (a) add the plumbing or (b) defer like arms.

## Tracy evidence motivating this slice

User-captured Tracy on mc2_10 idle (~21 active mechs, ~1-2 selected, no blown arms), full bore including FAST_TRANSFORM=1:

- **Zone:** `GameLogic.Mech3D.UpdateGeometry` at `mech3d.cpp:3184`
- **Samples:** 18,864 over 1,048 frames
- **Mean:** 56.51µs · **Median:** 47.99µs · **Mode:** 40.91µs · σ: 25.78µs
- **Bimodal:** lower peak ~40µs (fast body + bone walk), upper peak ~100µs (fast body + shadow CPU bake + light cache)
- **`Units.Mechs` zone:** 1.13ms/frame mean total mech work

Pre-FAST_TRANSFORM observation (this session, earlier): mc2_10 ~19 calls × 71µs = ~1.35ms/frame.

The ~270µs/frame savings (1.35→1.13ms) match the body's per-vertex lighting kernel cost. Shadow slice should yield another similar magnitude on the upper peak.

## Pre-existing pending items (do NOT mark done; for context)

- **Slice B verification:** Slice B1+B2+B+ shipped functionally, but user explicitly flagged "needs more verification" — `track_d_slice_b1_shipped.md` carries the IN SOAK marker. Do not promote to default-on without user sign-off.
- **Mech collision broken on parent branch:** confirmed by user — collision is broken on `claude/nifty-mendeleev` HEAD too (not from this session). Suspect commits per RCA: `89e35ac` gpu-cull frustum dilation, `bea7169` track-c3b canBeSeen routing, `a1d3190` track-c3a per-actor visibility snapshot. Out of scope for this slice; flagged for whoever owns nifty-mendeleev.
- **Dynamic shadow pass not catching mechs:** pre-existing regression user flagged 2026-05-09. Different code path (CPU shadow caster eligibility); separate slice.
- **Arm GPU-route slice:** deferred per user choice 2026-05-09. Estimated ~50-100µs/frame savings during heavy combat. Lower priority than shadow.
- **Default-on flip:** all GPU mech killswitches still default-off pending soak window completion.

## Suggested cadence for next session

Mirror the body slice precisely:

1. Read this handoff + the body spec + body plan to load context.
2. Recon `MultiTransformShadows` dispatch + `mechShadowShape->RenderShadows()` consumer of `listOfVertices[*].argb` (answer the four questions above).
3. Write spec at `docs/superpowers/specs/YYYY-MM-DD-mech-shadow-fast-transform-design.md`.
4. Write plan at `docs/superpowers/plans/YYYY-MM-DD-mech-shadow-fast-transform.md`.
5. Dispatch adversarial-plan-review per worktree CLAUDE.md "Review Discipline" — explicit prompt to use the skill verbatim.
6. Address review findings inline.
7. Execute Tasks 1-N (mechanical, follows plan).
8. Smoke matrix (CPU baseline / no FT / FT on, plus tier1 5/5 + mc2_24 stress).
9. Hand back to user for Tracy A/B + operator visual.
10. After user reports Tracy delta, dispatch implementation adversarial review.
11. Memory pin to `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/` + MEMORY.md index.

## Files referenced (absolute paths)

- Spec/plan precedent: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\docs\superpowers\specs\2026-05-09-mech-fast-transform-design.md` and the matching plan
- Body slice commit: `4ab312c` (and revision commit `c0ac087` updating spec+plan post-review)
- Recon target: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\mclib\msl.cpp:1763-1779` (`MultiTransformShadows` dispatch)
- Recon target: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\mclib\tgl.cpp:2650-2730` (`MultiTransformShape_PositionsOnly` per-leaf body)
- Modify target: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\mclib\mech3d.cpp:3377` (the swap site)
- Killswitch header: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\GameOS\gameos\gos_mech_killswitch.h`
- Env-var def: `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\GameOS\gameos\gos_mech_batcher.cpp`

## Fresh-session entry prompt (copy-paste this)

```
Read the handoff at A:\Games\mc2-opengl-src\.claude\worktrees\gpu-mech-batcher\docs\superpowers\plans\progress\2026-05-09-handoff-mech-shadow-fast-transform.md and execute the shadow fast-transform slice per its cadence. Worktree is set up; deploy folder is set up; precedent slice is shipped. Start with the recon questions, then spec, plan, review, execute.
```
