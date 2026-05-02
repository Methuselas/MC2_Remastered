# Slice 2 (object-offload) — Implementation hand-off prompt

> **Role for a fresh session reading this:** You are picking up the
> object-offload arc at slice 2 implementation. Recon Zero closed
> 2026-05-02 with all five pre-spec hardening items resolved; design
> spec is approved (this session). You are executing a 5-stage
> implementation per the spec. Do NOT redesign — execute.

---

## Worktree + branch

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `claude/nifty-mendeleev` (continuing the long-running nifty branch; slice 2 builds on slice 1's substrate)
- **Slice 2 design tip:** the design doc commit + this hand-off prompt commit (look for "object-offload slice 2 spec + hand-off" in `git log`).
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

## REQUIRED READS (in order — non-skippable)

1. **Slice 2 design spec:** `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md` — full architecture, stages 2.A through 2.E, gate ladders. **This is your primary work plan.** Each stage names files, edit shapes, and gates.

2. **Recon Zero:** `docs/superpowers/explorations/2026-05-02-object-offload-slice2-recon-zero.md` — especially Section 9 (pre-spec hardening resolved). Sections 1, 3, 4, 5, 7 contain the architectural reasoning the spec assumes.

3. **Slice 1 design:** `docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md` — the substrate slice 2 builds on. Pay attention to: R1 mutual exclusion (line 504-531), Layer-B per-child eligibility (line 135-167), late-registration accounting (line 346-359).

4. **Worktree CLAUDE.md** — `.claude/worktrees/nifty-mendeleev/CLAUDE.md`. Load-bearing project rules:
   - Documentation Discipline (grep at write-time)
   - Review Discipline (this slice qualifies as architectural-endpoint-class; FULL adversarial-plan-review skill before plan write — see Step 0 below)
   - Load-Bearing Cull Infrastructure (still applies — slice 2 does not bypass cull)
   - Critical Rules (build / deploy / shader version)
   - Tier-1 Instrumentation Env Vars (MC2_TGL_POOL_TRACE, MC2_DESTROY_TRACE, etc.)
   - Smoke Gate command

5. **Memory files (load-bearing — read before touching related code):**
   - `memory/cull_gates_are_load_bearing.md` ⭐
   - `memory/tgl_pool_exhaustion_is_silent.md` ⭐
   - `memory/mc2_texture_handle_is_live.md`
   - `memory/static_prop_projection.md`
   - `memory/gpu_direct_renderer_bringup_checklist.md`
   - `memory/render_order_post_renderlists_hook.md`
   - `memory/feedback_offload_scope_stock_only.md`
   - `memory/feedback_smoke_no_canary.md` (do NOT run menu canary)
   - `memory/feedback_pool_peak_compare_same_mission.md` (Gate E methodology)
   - `memory/feedback_subagent_no_cmake_configure.md` (build env safety — never `cmake -B build64`, only `cmake --build build64`)
   - `memory/deferred_vs_direct_uniforms.md`
   - `memory/blend_state_inheritance_in_post_process.md`
   - `memory/gpu_direct_depth_state_inheritance.md`

6. **Skill:** `.claude/skills/adversarial-plan-review.md` — required for Step 0. Also `.claude/skills/mc2-build.md`, `mc2-deploy.md`, `mc2-build-deploy.md`, `mc2-check.md` for the build/deploy cycle.

---

## Step 0 — Adversarial review of the design spec (MANDATORY before code edits)

Per worktree CLAUDE.md "Review Discipline" line 24-39, slice 2 is architectural-endpoint-class. Before writing any code:

1. Invoke `.claude/skills/adversarial-plan-review.md` against `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md`.
2. Grep every cited symbol at write-time. The spec already cites file:line for most claims; verify them live.
3. List CRITICAL / MAJOR / MINOR findings.
4. Surface CRITICAL findings to user before proceeding.

If review uncovers a blocker, surface to user. Do NOT silently rework the spec.

---

## Step 1 — Stage 2.A: substrate edits (no behavior change)

Per spec section "Stage 2.A — Substrate edits (no behavior change under `MC2_GPU_OBJECTS=0`)":

Files to modify:
- `mclib/tgl.h` — declare `MultiTransformShape_PositionsOnly`, `GatherGpuObjectLightDataOnly`. Extend `TG_HWLightsData` with `closeDistance`/`farDistance`/`oneOverDistance` per-light fields.
- `mclib/tgl.cpp` — define both functions. `_PositionsOnly` is a copy-and-strip of `MultiTransformShape` per spec architecture section. Add `eligibleForGpuObjects(TG_Shape*)` helper. Add `!eligibleForGpuObjects(this)` to the gate at line 2522.
- `mclib/txmmgr.cpp` `GatherLightsParameters` (line 938-1005) — populate the new falloff fields from `s_listOfLights[i]->closeDistance` etc.
- `GameOS/gameos/gos_static_prop_batcher.h` — declare `isMultiShapeEligibleForGpuObjects`. Repurpose `_pad0` slot in `GpuStaticPropInstance` as `lightDataIndex`. Update `static_assert` for offsets.
- `GameOS/gameos/gos_static_prop_batcher.cpp` — define `isMultiShapeEligibleForGpuObjects` (mirror slice 1's render-time per-child gates except late-registration, per Recon Section 9 Item 4).
- `mclib/bdactor.h`, `mclib/bdactor.cpp` — add `appearanceFlags_needsFullBakeNextFrame` 1-bit flag (pack into existing `appearanceFlags` byte) to `BldgAppearance` and `TreeAppearance`. Initialize false.
- `mclib/genactor.h`, `mclib/genactor.cpp` — same for `GenericAppearance`.

**No call sites are switched.** This stage adds infrastructure; existing code paths unchanged.

**Build**: `cmake --build build64 --config RelWithDebInfo --target mc2`. Per `memory/feedback_subagent_no_cmake_configure.md`: NEVER run `cmake -B build64 -S .` or any configure variant; the prefix paths get clobbered.

**Deploy**: per-file `cp -f` + `diff -q` to `A:/Games/mc2-opengl/mc2-win64-v0.3/`. NEVER `cp -r`. Files: `mc2.exe`, `mc2.pdb`, any modified shaders.

**Smoke gate**: `py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing`. Drop `--with-menu-canary` per `memory/feedback_smoke_no_canary.md`.

**Pass criteria**:
- tier1 5/5 PASS in two configs: unset, `MC2_GPU_OBJECTS=1`.
- +0 destroys delta (per `memory/feedback_pool_peak_compare_same_mission.md`).
- TGL pool peak unchanged.
- Tracy `appearanceUpdate` zone unchanged.

Commit Stage 2.A on green. HEREDOC commit message per CLAUDE.md "Critical Rules" / Git section.

---

## Step 2 — Stage 2.B: wire eligibility hoist + positions-only

Per spec section "Stage 2.B":

Files:
- `mclib/bdactor.cpp` `BldgAppearance::update` (line 1957) and `TreeAppearance::update` (line 4209) — replace the unconditional `TransformMultiShape` call with the eligibility branch (spec architecture section).
- `mclib/genactor.cpp` `GenericAppearance::update` (line 1049) — same pattern.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` — at the late-registration branch (around line 683-693), set `appearanceFlags_needsFullBakeNextFrame=true` on the actor and increment a new `late_register_recovery_skips` counter (separate from `cpu_fallback_by_pop`). Add this counter to the F-gate summary line.

**Visual behavior at this stage**: with `MC2_GPU_OBJECTS=1`, eligible static-prop actors run positions-only. Their `.argb` is stale or zero. Slice 1's batcher continues to memcpy `listOfVertices[j].argb` into the per-instance color SSBO and draw with stale colors. **This is intentional** — the kernel split is verified before the GPU lighting kernel comes online in Stage 2.C. PR description must call this out.

**Pass criteria**: tier1 5/5 PASS in three configs (unset, `MC2_GPU_OBJECTS=1`, `MC2_GPU_OBJECTS=1 + MC2_OBJBATCHER_TRACE=1`). +0 destroys. Render zone Tracy delta neutral. F-gate `late_register_recovery_skips` ≤ 2 per mission.

Commit on green.

---

## Step 3 — Stage 2.C: GPU vertex lighting (the meat)

Per spec section "Stage 2.C". This is the biggest stage. Consider splitting into 2.C.1 (shader work) + 2.C.2 (batcher wiring) for cleaner bisection.

Files:
- `shaders/include/lighting.hglsl` — set `ENABLE_VERTEX_LIGHTING 1` (line 3). Finish `calc_light()` (lines 119-137) with full 6-type dispatch: AMBIENT, INFINITE, INFINITEWITHFALLOFF, POINT, SPOT, TERRAIN. Add `GetFalloff` GLSL helper (linear interp per Recon Section 9 Item 1).
- `shaders/static_prop.vert` (or new `static_prop_lit.vert`) — invoke `calc_light()` per vertex with per-instance `lightDataIndex` and per-vertex `aRGBLight` tag.
- `shaders/static_prop.frag` — consume VS-produced lit ARGB.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` — at the top of the eligible-child loop (around line 698), call `multi->listOfShapes[0].node->GatherGpuObjectLightDataOnly()` (per-actor, NOT per-leaf — Recon Section 9 Item 5 confirmed all leaves see identical `lightData_`). Broadcast index into per-leaf `lightDataIndex`.
- `GameOS/gameos/gos_static_prop_batcher.cpp` — stop memcpying `listOfVertices[j].argb` into the per-instance color SSBO (now redundant; GPU lights it). **Note in Stage 2.C PR description**: this retires slice 1's color-stream memcpy. The slice 1 substrate code path becomes "memcpy-less" from slice 2 onwards. Does NOT affect slice 1's CPU-only path (which doesn't go through the batcher).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `registerType` (line 444-468) — write per-vertex `aRGBLight` at offset 36 (currently zero-padded). Source: `typeShape->listOfTypeVertices[localVertIdx].aRGBLight`.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `finalizeGeometry` — build per-type SSBO with hot-color fields (`hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB`); bind at draw time.

**Spec invariant** (per spec R5): a binary that contains slice 2 code unconditionally writes `aRGBLight` at registration time. Slice 1 binaries with the old VBO layout are not interop-compatible.

**Build, deploy, smoke** per Step 1. Test with `MC2_GPU_OBJECTS=1`.

**Pass criteria**:
- tier1 5/5 PASS in three configs.
- Visual canary at fixed camera (`mc2_01` airbase region recommended): no visible regression.
- `appearanceUpdate` Tracy zone shows ≥17% reduction with `MC2_GPU_OBJECTS=1` (per spec target). Use the `MC2_OBJECT_RECON_TRACY=1` instrumentation from commit `c4c4e96` to verify the slice-2-scoped per-population reduction.
- Render zone Tracy delta: no regression.
- +0 destroys.

Commit on green.

If Tracy reduction is below 10%, surface to user — the recon's perf prediction was wrong and the slice 2 framing needs reconsideration before merge.

---

## Step 4 — Stage 2.D: parity instrumentation

Per spec section "Stage 2.D":

**Scope warning**: Stage 2.D requires a GPU→CPU readback harness via PBO that doesn't exist in tree today. Budget accordingly — this is non-trivial: allocate PBO, dispatch async readback after the slice 1 batcher's draw, retain the readback for next-frame compare against fresh CPU recompute. Existing terrain/water arcs may have a similar pattern to crib from; check `GameOS/gameos/gos_terrain_indirect.cpp` and `gos_static_prop_batcher.cpp` for any existing PBO usage. If none exists, Stage 2.D's PBO harness is itself a sub-stage worth ~half the stage's effort.

Files:
- `GameOS/gameos/gos_static_prop_batcher.cpp` (or a new sidecar `gos_object_parity.cpp`) — implement P3 dual-emit at first frame post-mission-start: run BOTH `MultiTransformShape` and `_PositionsOnly` for all actors, bytewise-compare CPU `listOfTriangles[j].aRGBLight[i]` against GPU output (read back via PBO). Mismatch logs `[OBJECT_PARITY v1] event=lighting_mismatch actor=X tri=Y corner=Z cpu=ARGB gpu=ARGB`. ULP tolerance ±2 LSB per channel.
- P1 sampled bytewise in steady state: 1 actor per type per frame, round-robin. Compare GPU output (1-frame-stale OK via async PBO) against fresh CPU recompute.
- 600-frame summary: counts of compared/passed/mismatched.

**Pass criteria**: zero mismatches across tier1 stock with `MC2_OBJECT_PARITY_CHECK=1`.

If mismatches are nonzero but bounded (< 0.1% of compared corners) AND visible only at extreme corner cases (lighting transitions, etc.), surface to user with examples; spec may need to widen ULP tolerance or accept GPU as new ground truth.

Commit on green.

---

## Step 5 — Stage 2.E (separate PR): pinned-camera screenshot diff

Per spec section "Stage 2.E". This stage is a separate PR that gates the default-on flip. NOT a merge blocker for slice 2 PR itself.

Files: `tests/smoke/object_visual_diff.py` per spec.

Slice 1 may have a Stage 1.E in flight. If yes, slice 2 reuses it. If no, slice 2 builds it.

Default-on flip cannot happen until this PR clears. Slice 2 ships behind `MC2_GPU_OBJECTS=1` (default off) until then.

---

## Project constraints (load-bearing — re-confirm at every stage)

- **Stock missions only** for validation per `memory/feedback_offload_scope_stock_only.md`. Do NOT validate against Carver5O / Magic / MCO / Wolfman / MC2X.
- **Never run the menu canary** per `memory/feedback_smoke_no_canary.md`. Drop `--menu-canary` / `--with-menu-canary` from smoke commands.
- **Build**: `cmake --build build64 --config RelWithDebInfo --target mc2` ONLY. Never `cmake -B build64 -S .` or any configure variant — clobbers SDL2/GLEW prefix paths per `memory/feedback_subagent_no_cmake_configure.md`.
- **Deploy**: `A:/Games/mc2-opengl/mc2-win64-v0.3/`. Per-file `cp -f` + `diff -q`. NEVER `cp -r`.
- **Pool peak comparisons**: same-mission baseline-vs-test only per `memory/feedback_pool_peak_compare_same_mission.md`. Cross-mission comparisons produce false alarms; trust +0 destroys delta as the primary Gate-E proxy.
- **Git**: never push. HEREDOC for commit messages. NEVER amend.
- **Tracy zones with MTPC < 1µs**: don't add them; the recon's per-vertex/per-face zones are intentionally NOT split per-iteration. Use the MC2_OBJECT_RECON_TRACY accumulators from commit `c4c4e96` for finer measurements.
- **Shader #version**: never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (matches the 4.3 context required for SSBO / std430).
- **Uniform API**: `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **`GL_FALSE` for terrainMVP**: direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. Per-instance `modelMatrix` in slice 1's SSBO is `v*M`, terrain-style `worldToClip` uniform uploaded with `GL_TRUE`.
- **Shader hot-reload fails silently**: bad compile = old shader stays active. Check console for errors after every shader edit.

---

## Useful commands

```bash
# Build (run in worktree directory)
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2

# Deploy
cp -f build64/RelWithDebInfo/mc2.exe   A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe
cp -f build64/RelWithDebInfo/mc2.pdb   A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.pdb
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe

# Per-file shader deploy (when shader files change)
cp -f shaders/static_prop.vert         A:/Games/mc2-opengl/mc2-win64-v0.3/data/shaders/static_prop.vert
diff -q shaders/static_prop.vert       A:/Games/mc2-opengl/mc2-win64-v0.3/data/shaders/static_prop.vert
# (Confirm actual deploy shader paths via /mc2-deploy skill or by reading existing deploy state.)

# Smoke (tier1, NO menu canary, fast iteration)
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing

# Recon Tracy (verify per-stage perf)
MC2_OBJECT_RECON_TRACY=1 MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing

# Parity check (Stage 2.D onward)
MC2_OBJECT_PARITY_CHECK=1 MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing
```

---

## When you finish

After Stage 2.D lands (Stage 2.E is a separate PR):

1. Run final tier1 smoke in all three configs. Capture artifact.
2. Run `MC2_OBJECT_RECON_TRACY=1` smoke and capture the `[OBJECT_RECON v1] summary` line. Confirm the per-population reduction matches the spec target (~17-21% on `appearanceUpdate`).
3. Update memory:
   - `memory/object_update_cost_baseline.md` — capture the new post-slice-2 numbers (if not already created at recon time).
   - `memory/enum_mismatch_was_fabricated_claim.md` — note the recon-zero error so future arcs don't re-investigate.
4. Surface to user: "slice 2 ready to merge behind flag. Default-on flip blocked on Stage 1.E / 2.E pinned-camera diff."

If a stage gate fails:
- Do NOT push past the failure with hacks. Fix the root cause or surface the failure to user with the captured evidence.
- Pool exhaustion or destroys delta != 0: revert and investigate per `memory/cull_gates_are_load_bearing.md` and `memory/tgl_pool_exhaustion_is_silent.md`.
- Tracy delta < 10%: surface to user; spec's perf claim was wrong.
- Visual regression: surface to user with screenshots; do not merge.

---

## Out of arc (do not pursue without separate brainstorm)

- GPU shadow port (Task 13-14 from prior batcher) — separate slice or arc.
- Mover offload (Mech3D / GV) — separate arc per brainstorm Q4.
- General lighting refactor (per-face A/B options from Recon R-arch-1) — not viable on stock; separate arc only if mod-stable contract requires.
- Removal of legacy `g_useGpuStaticProps` and the 5 cull-bypass sites — post-arc cleanup; mechanical follow-up after default-on flip soaks.

---

## Hand-off

The slice 2 design spec is your work plan. The recon doc is your reasoning trail. The slice 1 spec is the substrate you build on. The memory files are the load-bearing constraints. Worktree CLAUDE.md is the project rules. Skills are the tooling.

Execute the 5 stages in order. Surface real blockers to user; never paper over them with hacks or skipped tests.

Slice 2 ships when Stages 2.A through 2.D pass their gates. Default-on flip ships when Stage 2.E (or slice 1's Stage 1.E, whichever comes first) clears.
