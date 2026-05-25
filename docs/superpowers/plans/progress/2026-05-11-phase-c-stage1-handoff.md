# Phase C Stage 1 — Handoff Prompt

> **For the next session.** You are picking up Phase C of the MC2 renderer-modernization arc at Stage 1 (Water — precedent proof). Stage 0 design (4 revisions) and Stage 0.5 prerequisites are done. Stage 1 is the first substantive code work and the precedent for Stages 2–6.

---

## What you are

You are continuing the **Phase C GPU-driven indirect-cmd generation** slice for the MC2 OpenGL renderer-modernization arc. Phase C eliminates per-frame CPU thin-record pack loops across three Tracy zones (combined ~2.7 ms → ~500 µs at wolfman-mc2_10; frame is CPU-bound by ~15 ms so savings translate directly to frame time).

This session inherits a fully-specified, four-revision-reviewed design + a detailed task plan. Your job is mechanical execution. The architectural decisions are locked.

## Where you are

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/`
**Branch:** `claude/gpu-driven-rendering`
**Sibling Phase B worktree** (read-only from your perspective): `A:/Games/mc2-opengl-src/.claude/worktrees/pre-bake-terrain/`

**Commit graph at handoff:**

```
a23afbd  smoke: register MC2_GPU_DRIVEN* env vars in run_smoke.py passthrough        ← Stage 0.5.B
8b3f45d  feat(terrain_lighting): publish GetOutputSsbo() accessor for Phase C ...    ← Stage 0.5.A
63e73e6  plan(gpu-driven-rendering): Phase C v1 implementation plan — all stages     ← Plan
40037e8  spec(gpu-driven-rendering): Phase C Stage 0 v4 — Sonnet narrow review fixes  ← Design v4
e462c7e  spec: Phase C Stage 0 v3 — targeted-review fixes
802aca4  spec: merge Phase B session boundary corrections + Stage 3/4 OVERLAY cleanup
d8e5800  spec: Phase C Stage 0 v2 — adversarial-review fixes
13a0c06  spec: Phase C Stage 0 design doc                                            ← Design v1
5667023  (Phase B/C shared baseline; nifty-mendeleev merge)
```

## What you must read FIRST, in this order

1. **Operating discipline:** `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/CLAUDE.md` — full read. Especially the "Documentation Discipline — grep at write-time" section, "Review Discipline" section, "Critical Rules", and "Smoke Gate" section.

2. **The design doc:** `docs/superpowers/specs/2026-05-11-gpu-driven-indirect-cmd-gen-design.md` (v4 at commit `40037e8`). This is the locked architectural reference. Every implementation choice is justified there with grep evidence.

3. **The implementation plan:** `docs/superpowers/plans/2026-05-11-gpu-driven-indirect-cmd-gen-plan.md` (commit `63e73e6`). This is what you execute. Stage 0.5 is done — you start at **Stage 1.1**.

## How you operate

This session uses **subagent-driven development**. The flow per task:

1. Invoke `Skill superpowers:subagent-driven-development` once at session start.
2. For each task: dispatch implementer subagent (with full task text + context, never make subagent read the plan file). Use the prompt template from the skill.
3. Verify the commit before review (inspect the diff yourself; the implementer's report may be optimistic).
4. Dispatch spec compliance reviewer.
5. If issues, implementer fixes; re-spec-review.
6. Dispatch code quality reviewer.
7. If issues, implementer fixes; re-code-quality-review.
8. Mark task complete in TodoWrite.
9. Proceed to next task.

**Model selection per task:**
- Mechanical edits (1-2 files, clear spec) → `haiku`.
- Multi-file integration, parity scaffolding → `sonnet`.
- Architecture / cross-file refactors (Task 2.1's `ComputePreflight` split is the canonical one) → `sonnet`.

## Stage 0.5 outcomes (already done)

- **Task 0.5.A** (`8b3f45d`) — `gos_terrain_lighting::GetOutputSsbo()` accessor published. One-line getter returning the existing file-static `s_computeOutputSsbo`. Header adds `#include <GL/glew.h>` (consistent with project precedent in `gpu_cull_compute.h`, `gpu_cull_substrate.h`, etc.).
- **Task 0.5.B** (`a23afbd`) — `MC2_GPU_DRIVEN*` family (6 env vars) registered in `scripts/run_smoke.py` passthrough tuple, right after the existing `MC2_GPU_CULL_*` family. Smoke dry-run confirmed clean (mc2_01 PASS @ 128 fps).

Both passed implementer + spec reviewer + code quality reviewer with no issues found. Use them as the precedent shape for what a clean Stage 1 task looks like.

## What you are about to do

**Stage 1 — Water bucket compute port (precedent proof).**

Stage 1 is the first time the Phase C compute pattern lands. It is the precedent every other Phase C stage mirrors. Get this right and Stages 2/3 are mechanical extensions; get it wrong and the bugs cascade.

**Anchor target:** drop the `GameCamera::render waterFastPath` Tracy zone (`code/gamecam.cpp:255-256` wrapping `land->renderWaterFastPath()`) by ≥80% at wolfman-mc2_01 (water-heavy mission). Current cost: ~814 µs at the wrapping `render water` zone (live cost is in the fast-path zone when armed). Target: ~150 µs after Stage 1 flips default-on.

**Seven sub-tasks** (per the plan, sequential):

- **1.1** — `gpu_driven_common.h/.cpp` — shared header with `GpuDrivenBucketHeader` struct + env-var caches. **Start here.** Sonnet model.
- **1.2** — `shaders/gpu_driven_water.comp` — cull/pack compute shader. Has `[grep at task time]` placeholders for the recipe struct + thin-record layout — the implementer MUST grep the actual current layouts and paste them verbatim before compiling. Sonnet.
- **1.3** — `shaders/gpu_driven_cmd_patch.comp` — single-invocation cmd-count writer, shared across buckets. Haiku.
- **1.4** — host-side water dispatch in `gos_terrain_water_stream.cpp` — Beta two-dispatch pattern (cull/pack → patch → barrier sequence ending in `GL_COMMAND_BARRIER_BIT`). Sonnet.
- **1.5** — water fast-path bridge in `gameos_graphics.cpp` (around `:2215`/`:2242`) — convert 2× `glDrawArrays` to single `glMultiDrawArraysIndirect` with `drawcount=2` + per-cmd uniforms via `WaterPerCmd` SSBO indexed by `gl_DrawID`. Sonnet.
- **1.6** — water parity check infrastructure. Sonnet.
- **1.7** — verification gates: visual canary, parity gate, Tracy delta, tier1 5/5 PASS triple. Mostly procedural; controller-driven (not a subagent task).

## Load-bearing context the design has burned in (lessons from prior review passes)

These are the bugs the v1 → v2 → v3 → v4 review cycles caught. Internalize them so you don't reintroduce them:

1. **GREP AT WRITE-TIME.** Every cited symbol, struct field, function signature, file:line — verify against current code AS you write, not after. Stale memory files decay. The implementer should grep before pasting any cited symbol; the spec reviewer should grep before approving any cited symbol. This is enforced by CLAUDE.md "Documentation Discipline" and was the load-bearing fix-process across v1→v4.

2. **No `glGetBufferSubData` on the hot path** against any GPU-written buffer (per `memory/substrate_coalesce_sync_point_lesson.md`). Implicit GPU sync stall. CPU-side counters or fenced-ring lagged readback only.

3. **C++/GLSL struct lockstep** (per `memory/cpp_glsl_ubo_struct_lockstep.md`). Any C++ struct mirroring a GLSL SSBO/UBO MUST byte-match the GLSL declaration. Same commit, both sides. Mc2_24 desert-mission is the content-diversity canary.

4. **Texture slot, not handle** (per `memory/mc2_texture_handle_is_live.md`). Store the slot index in compute output; resolve to a live `gosTextureHandle` on the CPU bridge side just before `glBindTexture`. Caching handles at registration time goes stale by frame 2.

5. **`GL_COMMAND_BARRIER_BIT`** is the load-bearing flag for the compute-writes-cmd → MDI-reads-cmd sync. Without it on AMD drivers, the MDI reads stale data. Precedent: `gpu_cull_compute.cpp:1018-1033` (Track C's patch dispatch sequence).

6. **Phase 1 lighting frame-pipelined.** Phase 1's `IsParityCheckEnabled()` at `gos_terrain_lighting.cpp:85-87` caches the env in a `static const bool`; cannot be toggled at runtime. Parity gate is the smoke-runner-sets-both-env-vars contract: `MC2_GPU_DRIVEN_PARITY=1` AND `MC2_TERRAIN_LIGHTING_PARITY=1` at process startup, never later.

7. **MINE is OUT** of Phase C scope. MINE uses `glDrawArrays` (not MDI) and has no per-frame thin-record pack — just dirty-flag-gated lazy rebuild. Lives in `Render.TerrainMines` zone (`mclib/txmmgr.cpp:1812`), not Z1. If you see MINE work creeping in, you've drifted off-plan.

8. **OVERLAY is CONDITIONAL** on Phase B publishing an overlay recipe SSBO. Default planning assumption: Phase B does NOT ship one in this window, OVERLAY remains scaffold-only, Stage 3 falls out of v1. Check Phase B's worktree before starting Stage 3.

9. **Stage 2.1 is a CRITICAL refactor.** The current `ComputePreflight()` at `gos_terrain_indirect.cpp:1605` is called from `terrain.cpp:1792` — BEFORE `PackAndDispatch()` at `terrain.cpp:1798`. Phase C SOLID compute dispatch MUST run AFTER Phase 1's dispatch. v4 design splits `ComputePreflight` into preflight (arming, stays at `:1605`) + new `ComputeDispatch` (new symbol, called from a new line inserted at `terrain.cpp:1800`, after `CopyResultsToVertexPool`). Do not skip this split.

10. **The 4.22 ms GPU-side cost of terrain solid is NOT the Phase C target.** That's GPU work. Phase C targets the **CPU side** of three Tracy zones: 1.35 ms `render textureManagerRenderLists` + 814 µs `render water` + 533 µs `render objects` = ~2.7 ms. Some sub-zones (e.g., `PatchStream.Flush` 617 µs) are inside Z1 but Z1 is the headline target, not any single consumer. Frame is CPU-bound by ~15 ms; CPU savings show up as frame time savings.

## Smoke gate (the regression-detection reference)

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

- tier1 = 5 missions: `mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`. Different biomes/content classes; mc2_24 is the content-diversity canary.
- Exit 0 = clean PASS.
- After Stage 1.7's parity gate, run the **PASS triple**: unset / `MC2_GPU_DRIVEN_WATER=1` / `MC2_GPU_DRIVEN_WATER=1 MC2_GPU_DRIVEN_PARITY=1 MC2_TERRAIN_LIGHTING_PARITY=1`. All three must PASS + 0 destroys delta per mission.

## Build discipline

- Always `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- For load-bearing changes (renderer core, batcher, state-cache, draw-path): force a full relink first. Either delete `build64/RelWithDebInfo/mc2.exe` (and the changed file's `.obj`) before `cmake --build`, OR pass `--clean-first`. Stale linkage has burned us before.
- Deploy: `/mc2-deploy` skill (in `.claude/skills/mc2-deploy.md`) — NEVER `cp -r`, always `cp -f` per file + `diff -q`.
- Combined: `/mc2-build-deploy` skill.

## Stop conditions (per the design doc)

- Per-bucket parity diff non-zero after 3 iteration rounds → STOP, surface findings.
- Per-bucket Tracy delta < 200 µs → STOP that bucket, surface to user. Other buckets may still ship.
- Sync stall surfaces in profiling → STOP, switch to non-blocking ring + skip-frame fallback per `gpu_cull_readback.cpp` precedent.
- Any tier1 mission FAIL under `MC2_GPU_DRIVEN_WATER=1` → STOP, revert to parity-only, bisect.
- AMD driver compute-dispatch-before-MDI ordering bug surfaces → STOP, surface to user; may need explicit fence between dispatch and draw.

## What you should NOT do

- DO NOT make subagents read the plan file. Provide full task text + context in the implementer prompt.
- DO NOT skip the two-stage review (spec then code quality). Both are required per the discipline rule.
- DO NOT proceed to Stage 2 until Stage 1's verification gates have ALL passed (parity zero mismatches across tier1 5/5; Z2 drop ≥80%; tier1 5/5 PASS triple; 0 destroys delta).
- DO NOT delete legacy CPU pack paths in v1. Gate them off (per `gpu_driven::IsWaterEnabled()`), leave the code in tree. Deletion is a separate post-soak slice.
- DO NOT extend any existing thin-record struct layouts. Phase C's compute shader writes the SAME byte layout the CPU pack writes today. Parity check enforces this mechanically.
- DO NOT introduce a new SSBO struct without lockstep C++ AND GLSL changes in the same commit (per `cpp_glsl_ubo_struct_lockstep.md`).
- DO NOT push to alariq/mc2 origin. All work is local on `claude/gpu-driven-rendering`.

## What you SHOULD do

- Invoke `Skill superpowers:subagent-driven-development` at session start.
- Read CLAUDE.md, design doc, plan in that order before dispatching any subagent.
- Start with Task 1.1 (smallest scope; establishes the common header + env-var pattern for the rest of Stage 1).
- After each task: verify the commit diff yourself before dispatching reviewers (catch over-eager additions early).
- After Task 1.4 (host-side dispatch), pause and consider an adversarial-review checkpoint before continuing to 1.5 — this is the most complex task in Stage 1 and the precedent for Stage 2.3.
- After Task 1.7 (verification gates pass), tag `stage-1-water-gates-pass` and pause for a checkpoint before starting Stage 2.

## Adversarial review checkpoints (per the plan's discipline)

These are mandatory per CLAUDE.md "Review Discipline":

1. **After Stage 1.4** — first compute-shader bucket fully wired; check pattern correctness before scaling to Stage 2.
2. **After Stage 1.7** — Stage 1 fully shipped + gates passed; sign-off review before Stage 2.

Dispatch the reviewer with the verbatim instruction: `use the adversarial-plan-review skill in .claude/skills/`.

## Your first action

1. Invoke `Skill superpowers:subagent-driven-development`.
2. Read CLAUDE.md (full).
3. Read the design doc (focus on "Per-bucket binding-point allocation", "Stage 1 water draw shape", "Parity-gate reframe", and the entire "Architecture" section).
4. Read the plan's Stage 1.1 task text.
5. Dispatch the implementer subagent for Task 1.1 using the implementer-prompt.md template, with full task text + the scene-setting context that Stage 1 is the precedent-proof bucket and Task 1.1 establishes the shared infrastructure that Tasks 1.2-1.6 (and Stages 2-3) build on.

## Worktree state at handoff

- Clean (no uncommitted changes expected).
- The two Stage 0.5 commits are the last on the branch.
- TodoWrite from the previous session is gone (fresh session); rebuild it from the plan's Stage list.

## Communication style

The user prefers tight, code-grounded updates. State results and decisions directly. Match response length to task: a "task complete" summary should be 2-3 lines; a finding worth flagging should be a brief bulleted list with file:line citations. Don't narrate internal deliberation.

When you complete a sub-task: state what shipped, the commit SHA, and what's next. Don't restate the design rationale (it's in the design doc).

---

**Ready signal:** if you've read this handoff + CLAUDE.md + the design doc + the plan, you have enough to start. Dispatch Task 1.1 implementer.
