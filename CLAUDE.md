# MC2 OpenGL — nifty-mendeleev (canonical worktree)

MechCommander 2 OpenGL port: tessellated terrain, PBR splatting, shadow maps, post-processing. Active branch is `claude/nifty-mendeleev` (the 0.4 gpu-driven-rendering arc merged back here 2026-05-18; split collapsed). Root checkout `terrain-pbr-mod` is older — do not work there.

## Where to look first

- **Project direction:** `.planning/PROJECT.md` (three north stars + out-of-scope)
- **MEMORY.md:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` — router only (3 handoffs + table of topical sub-indexes). **Read the matching `INDEX-<TOPIC>.md` (in same dir) when starting domain work**: RENDERING / SHADERS / TERRAIN / MECH / BUILD-DEPLOY / MISSION-DATA / SMOKE-TEST / PROCESS. Router lists them with scopes.
- **Codebase maps:** `.planning/codebase/{ARCHITECTURE,STRUCTURE,STACK,INTEGRATIONS}.md` (2026-05-14; grep before quoting line numbers)
- **Advisor routing:** `.claude/agents/DOMAINS.md` (12 MC2 advisor subagents + classification + gaps)
- **Perf state:** `docs/render-perf-snapshot.md` (bucket map + slice state + deps; refresh when slices ship)
- **Render contract:** `docs/render-contract.md` (design) + `mclib/render_contract.*` (impl) + `.claude/agents/mc2-render-contract-synthesizer.md` (refresh agent)
- **Meta-prompts:** `.claude/prompts/distill-session-into-advisor-agent.md` (harvest new advisor at end of session), `.claude/prompts/dump-render-observations.md` (dated notes -> `docs/observations/` for synthesizer)
- **Skills:** `.claude/skills/` — `/mc2-build`, `/mc2-deploy`, `/mc2-build-deploy`, `/mc2-check`, `/mc2-shader-diff`, `/mc2-amd-shader-review`, `adversarial-plan-review`, `greybeard`
- **Steering channel:** `A:/Games/mc2-opengl-src/.claude/STEERING.md` (shared, repo-root; out-of-band feedback for a barreling session). `sh A:/Games/mc2-opengl-src/.claude/steer.sh "..."` blocks next Bash/Agent/Task and injects text; agent runs `ack-steering.sh` to clear. Globally-registered hook walks up to repo root.
- **Reference docs:** `docs/{architecture,amd-driver-rules}.md`, `docs/plans/`, `docs/superpowers/specs/`
- **Maintenance hook:** `.claude/maintenance-rules.json` (consumed by Stop hook `~/.claude/hooks/gsd-staleness-monitor.js`; surfaces related-update reminders when load-bearing files change)

## Key paths

- Source: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.4/`
- CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Critical inline rules (every session reads these)

- **No emoji in any file, ever.** Unicode punctuation (em/en-dash, ellipsis) OK; pictographic emoji not. Full scope: `memory/feedback_no_emoji_in_files.md`.
- **No wall-clock time projections.** Describe complexity in code dimensions (subsystems, files, parity gates, soak windows), not time.
- **Grep before citing file:line.** Every file:line in any output must be verified at write-time. Symbols stable; line numbers drift. Rationale: `memory/brainstorm_code_grounding_lesson.md`. Plan-stage: `.claude/skills/adversarial-plan-review.md`.
- **Negative claims need opposite-direction grep.** "X is NOT consumed by Y" requires grep'ing Y, not the obvious-named consumer. See `memory/feedback_data_flow_audit_asymmetry.md`.
- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- **Full relink before deploy** when load-bearing functions change: `rm build64/RelWithDebInfo/mc2.exe` (+ changed `.obj`) before `cmake --build`, or `--clean-first`. Incremental leaks stale linkage when inline funcs / templates / static state change. Class-layout changes: `memory/feedback_class_layout_change_needs_clean_first.md`.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2.
- **Shaders deploy in lockstep with exe.** Any slice touching a shader MUST redeploy the shader tree, not just mc2.exe. See `memory/shader_exe_deploy_lockstep.md`.
- **Git:** NEVER push to `alariq/mc2` origin. All work is local.
- **Shader `#version`:** Never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (4.3 for SSBO / std430).
- **Uniform API:** `setFloat` / `setInt` BEFORE `apply()`, direct `glUniform*` AFTER. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. The `gamecam.cpp` comment claiming `GL_TRUE` is wrong; do not "fix" it.
- **Explicit-program uniform upload:** any GOS API or helper that takes a `GLuint program` and calls `glGetUniformLocation(program, ...)` MUST upload via `glProgramUniformMatrix4fv(program, loc, ...)` (or its sibling for the type). `glUniformMatrix4fv(loc, ...)` uploads to the currently-bound program (`glUseProgram` state), NOT the named one — silent wrong-shader upload bug. Engine uses GL 4.5; the explicit-program family is unconditionally available. Full: `memory/glprogramuniform_vs_gluniform_explicit_program_trap.md`.
- **GLSL macros do NOT inherit C++ build flags.** `-DMY_FLAG` in `CMAKE_CXX_FLAGS` reaches only `.cpp` compilation. To gate a GLSL `#ifdef`, extend the `makeProgram()` prefix at C++ level: `prefix += "#define MY_FLAG 1\n"` inside `#ifdef MY_FLAG`. Verify by dumping compiled shader source before `glCompileShader`. Full: `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`.
- **Shader hot-reload fails silently:** bad compile = old shader stays active. Always check console after editing.
- **Vulkan-prep:** new GPU-resource code uses explicit device-mediated binding (`device.bindVertexBuffer(vb)`, NOT `vb.bind()`); assume zero implicit cross-call GL state. PREP not a port. Full: `memory/vulkan_prep_explicit_device_discipline.md`.
- **Change discipline:** don't touch what you don't have to (every touch has blast radius); when you must, bring it to modern standard. Standalone cleanup slices need a blocking/debt justification. Full: `memory/minimal_touch_modern_when_touched.md`.

## Load-bearing memory pointers (read before touching the area)

- **Cull gates** (`memory/cull_gates_are_load_bearing.md`): `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive` gate `update()` AND allocation AND lifecycle. Mechs are the canary.
- **TGL pool exhaustion is silent** (`memory/tgl_pool_exhaustion_is_silent.md`): `getVerticesFromPool` returns NULL = shapes vanish. Pools at 500K.
- **Render funcs are enqueuers** (`memory/render_functions_are_enqueuers_not_submitters.md`): `XXX::render()` enqueues into `MC_TextureManager`. Actual GL submission = `gos_RendererEndFrame -> renderLists()`. MLR objects are the immediate-draw exception.
- **Dual-queue retirement debt** (`memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`): legacy `masterVertexNodes` + modern `masterHardwareVertexNodes` coexist; every mod slice has been ADDITIVE; retire legacy soon.
- **GPU-direct bring-up checklist** (`memory/gpu_direct_renderer_bringup_checklist.md`): 10 traps every new fast path hits. READ FIRST before any GPU-direct renderer.
- **Stock install must remain playable** (`memory/stock_install_must_remain_playable.md`): renderer modernization data must degrade to stock-compatible; no savegame depends on render caches.
- **Path separator** (`memory/mc2_path_separator_linux_build.md`): engine builds with `-DLINUX_BUILD`; `PATH_SEPARATOR` is `/`. Never hardcode `\\` against `_WIN32`.
- **PAUSE / UNPAUSE diagnostic** (`memory/pause_unpause_diagnostic_for_static_render_bugs.md`): bug clears on pause and returns on unpause = `mcTextureManager->update()` cache eviction without `objectManager->update` re-cache.
- **GPU cull predicate is HELPER** (`memory/gpu_cull_predicate_is_helper_real_consumer_is_cullubo.md`): `shaders/gpu_cull_predicate.glsl` declares no uniforms; takes `vec4 clip` as input. Real matrix consumer is `CullUBO.viewProj` at `gpu_cull.comp:169-170`, written from `gpu_cull_compute.cpp:831` via cache. Migration work targeting the predicate file is fictional.

## Review discipline (load-bearing)

When user asks for "review" / "second opinion": **adversarial, code-grounded by default**. Read `.claude/skills/adversarial-plan-review.md`. High-stakes plans (architectural endpoints, legacy retirement, SSBO schemas, perf gates >=30%) get the full skill (grep every cited symbol; findings as CRITICAL/MAJOR/MINOR). Dispatch prompt MUST include "use the adversarial-plan-review skill" verbatim. Always dispatch without asking: `memory/feedback_always_dispatch_adversarial_review.md`.

## Meta-fix discipline (load-bearing)

Before proposing/writing any fix: run the `greybeard` skill (`.claude/skills/greybeard.md`). It forces an explicit `META-FIX` vs `PATCH (justified)` ruling — the upstream change that retires the bug *class*, not the local symptom patch. A patch with no named meta-fix and no debt justification is not allowed. Documented history of additive slices netting ~0ms (`memory/feedback_offload_must_be_substitutive_not_additive.md`). Dispatch prompts MUST include "run the greybeard skill" verbatim.

## Documentation discipline (load-bearing)

Every cited symbol grep-verified AT WRITE-TIME. Applies at every stage. Carve-out: intentions ("we will create X") need no grep. Full: `memory/brainstorm_code_grounding_lesson.md`.

## Advisor invocation discipline (load-bearing)

For any substantive question whose domain has a dedicated advisor per `.claude/agents/DOMAINS.md`, **spawn the advisor first**. The advisor reads MEMORY.md + topic files + current code with fresh context; main-agent compiled knowledge is stale by definition.

**Applies to:** pipeline questions (rendering, shaders, mech, terrain-indirect, GameOS), methodology (CPU→GPU offload, render-contract refresh, perf-slice sizing, cull-cascade safety), asset-format (mission data, FST/.fit/.tga/.wav, file IO, init order), build-system (CMake, vcpkg, FFmpeg delay-load, full-relink).

**Does NOT apply to:** trivial lookups, follow-up clarification on something this-session, or when user says "answer directly".

Why: advisors carry tacit knowledge in `<known_pitfalls>` that doesn't live in MEMORY.md, and grep-verify file:line per their Rule 0. Answering from main-agent context bypasses both protections.

## Model routing

- **haiku:** lookups, summaries, simple edits, renaming, formatting
- **sonnet:** standard implementation, debugging, code review. Always diff changes from haiku.
- **opus:** architecture, deep analysis, complex refactors only. Always diff changes from sonnet/haiku. Give other agents isolated context.

## Key source files (starting points; always grep to confirm current line)

- `GameOS/gameos/gameos_graphics.cpp` — renderer core (terrain draw, shadow draw, uniform caching)
- `GameOS/gameos/gos_postprocess.cpp` — FBOs, bloom, shadows, post-process
- `mclib/txmmgr.cpp` — `renderLists()` flush, master-node arrays, shadow pre-pass
- `mclib/mech3d.cpp` — engine-side mech appearance / rendering (5139 lines; engine, NOT game-side AI)
- `code/gamecam.cpp` — frame loop, render-call sequence
- `shaders/gos_terrain.frag` — terrain splatting, POM, shadow sampling, distance LOD
- `shaders/include/shadow.hglsl` — `calcShadow()` with variable-tap Poisson PCF

## Profiling

Tracy always compiled in (`TRACY_ENABLE`). GPU zones on shadow/terrain/3D/post-process. AMD RGP works externally via Radeon Developer Panel. **100ns floor:** never instrument a region <100ns (Tracy overhead ~20-50 ns); per-element/per-quad/per-vertex zones in hot loops are FORBIDDEN (`gos_getTextureHandle` ~20ns is canonical). Coarse per-pass zones only. Origin: commit `fdc47bc` 2026-05-07.

## Debug instrumentation rule (for reworks)

Any rework touching object lifecycle, cull/visibility gates, render path, resource lifetime, or cross-system control flow lands env-gated `[SUBSYSTEM]` lifecycle prints in the same commit. Stays gated off; demote-don't-delete after fix. Full rule: `memory/debug_instrumentation_rule.md`.

## Tier-1 instrumentation env vars (current set)

- `MC2_TGL_POOL_TRACE=1` — per-frame TGL pool NULL trace; monotonic summary every 600 frames always-on
- `MC2_DESTROY_TRACE=1` — per-destruction cull/lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppress first-error prints (PRINT-ON by default; drain loop always runs)
- `MC2_ASSET_SCALE_TRACE=1` — per-key runtime lookup events; first `oob_blit` per `(path, callerTag)` always-on
- `MC2_ASSET_SCALE_SELFTEST=1` — synthetic 2x/4x/8x/1.5x golden tests at startup
- `MC2_HEARTBEAT=1` — stderr `[HEARTBEAT]` per second; detect renderer freezes during mod load
- `MC2_REVERSE_Z_TRACE=1` — `[REVERSE_Z v1]` lifecycle prints (projection-matrix build + inverseProjectZ fence-seam first use)
- `MC2_GL_DEBUG_FATAL=1` — abort() on GL_DEBUG_SEVERITY_HIGH; opt-in safety net for smoke (Tier 1.2)

Startup banner `[INSTR v1] enabled: ...` at log start. Grep schema-version with `\[SUBSYS v[0-9]+\]`. Pre-commit invariant scripts (run if you touched the area):
- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas / `code/mechicon.cpp`: `sh scripts/check-asset-scale-callers.sh`

## Smoke sessions are USER-DRIVEN (load-bearing)

**The user can see and control every smoke session.** `run_smoke.py` launches mc2.exe in a real game window the user is watching live. Smoke feedback like "I saw the triangle" is **first-hand visual observation**.

Anti-patterns: DO NOT ask user to "re-run manually to reproduce" or "confirm with X env var"; they're already the visual observer. When user says "still doing it" they mean the most recent smoke — find the latest artifact dir and analyze its `ring_trace.log`.

After every smoke the user reports a bug in: read `tests/smoke/artifacts/<latest>/{mission}.ring_trace.log` for that run's probe events.

## Smoke gate ("did I break it")

Default regression gate for render/init/cull/asset changes. **ALWAYS** `--keep-logs`, NEVER `--with-menu-canary`, NEVER `--duration` >30s, NEVER concurrent with another smoke or direct mc2.exe trace.

**Canonical invocation (copy-paste this; subagents must use it verbatim):**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

- `tier1` = 5 hand-picked missions (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`). 30s/mission. Isolated.
- Exit `0` = pass. Nonzero = inspect `tests/smoke/artifacts/<timestamp>/`.
- Inner-loop dev: use a 2-mission subset (`--missions mc2_01,mc2_24`); tier1 5/5 for slice coverage gates only.
- Visual-iteration smokes (water/shoreline/foam stylistic tuning): `--duration 60` so user has time to drive camera.
- Tier1 perf is NOT comparable to tier2 (isolated vs sequenced/stress) — see "Measurement semantics" in `tests/smoke/README.md`.
- Faster fail: add `--fail-fast`.
- Full smoke-policy rules: `memory/feedback_smoke_*.md` cluster.

## Known issues (current)

- Shadow re-render stutter when camera moves >500 units. Fix: static world-fixed shadow map (design ready).
- Water shoreline z-fight on zoom/elevation-change (NOT pan); water sits slightly low (pre-existing). Interim fast-path fixes shipped 2026-05-17. Full state: `memory/water_fastpath_interim_fixes_and_residuals.md`.
- Shadow banding shifts with camera rotation (view-dependent terrain geometry).
- First-launch black terrain intermittency — tier1 first mission occasionally renders black; second normal. Suspected: GPU/shader state dirty from previous mission teardown. Repro: tier1 with `--fail-fast`.
- Options menu writes bad ResolutionX/Y to options.cfg (observed 4096x2160 on 4K). Engine UI canvas is 800x600 and self-scales; other values break HUD scale + video positioning. Diagnostic: `memory/options_cfg_resolution_drift.md`.
- **Stage 0.5 §4 (renderVisible repoint) BLOCKED — empirically NO-GO 2026-05-20 EVENING.** Tentative ship `40a54b7` reverted as `dc2e8f6` (popping + black-textures + resurrected 2026-05-05 black-tree class). §2.5 sticky-bit (`91b6991`) shipped independently and is durable. §4 deferred to alpha-Stage 1 OR pivot to v4 (gate render on `blockVisBits[]` directly under sticky). Full: `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.
- **drawPass-retirement decal static-bake (`MC2_TERRAIN_INDIRECT_OVERLAY`): DEFAULT-ON since Stage-6 flip `60f2ef8`** (only `=0` reverts). On default path both SOLID and OVERLAY are armed so per-quad `draw()` loop in `Terrain::render drawPass` is SKIPPED. Slice A+B WIRED & validated 2026-05-17. Remaining endpoint: user-driven substitutive non-COST_SPLIT Tracy proof + decal visual canary. Full state: `memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`.
- **gosFX dev-override broken under unified projection (F1 ship).** Running with
  `MC2_DISABLE_GOSFX=0` after F1 will render gosFX particles wrong:
  MLR's `mlrclipper.cpp:206-209,305,321,347` reads of `cameraToClip(2,2)` and
  `(3,2)` use stale MC2-pixel-homogeneous convention while the runtime uniforms
  drive shaders via the new GL convention (clip.w > 0 for in-front; polarity
  folded into kAxisSwapMC2toGL per addendum-rclipw-polarity.md). Default
  `MC2_DISABLE_GOSFX=1` (gate-dead MLR work-leaves) is unaffected. Runtime
  guard prints `[UNIFIED_PROJ v1] WARN:` stderr once per startup. Dev-override
  path re-enabled when MLR retirement Slices 1-5 ship.

## Do NOT upscale these atlases

`code/mechicon.cpp` hardcodes `unitIconX/Y` (32/38) and reads source-pixel offsets against `s_MechTextures->width` / `s_pilotTextures->width`. Oversized source TGAs (upscaler `*_4x_gpu/` or Magic's Unofficial Expansion overlay) scramble icon sub-rectangles. Keep all 9 icon atlases at FST-archive resolution + `mcl_pr_pilotskillicons.tga`. Pre-commit guard: `sh scripts/check-asset-scale-callers.sh`.

## Memory & CLAUDE.md discipline

- **Auto-memory index:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- **No session narratives in CLAUDE.md.** Dated logs go in commit messages or memory files.
- **Root CLAUDE.md is a thin pointer ONLY.** This worktree CLAUDE.md is authoritative. Root enforced by `scripts/check-claude-md-pointer.sh`.
- **New durable finding → memory file + MEMORY.md index entry.** Unlinked memories are invisible. Group under matching section.
- **Superseded facts → update or delete, don't append.** Index entry must reflect current truth.
- **Before writing a new memory:** `grep -i <keyword> memory/*.md` to dedupe.
- **Keep this file under 200 lines.** If it grows past 200, extract to memory and link.

## Pending durable artifacts (write-when-ready)

- **Render contract document** at `docs/render-contract.md` (or `.planning/codebase/RENDER-CONTRACT.md`) — enumerate at function/symbol level: who enqueues into which master array, who flushes when, what state is inherited at each hook point, what each Track A/B/C/coalesce slice consumes/emits. Currently we burn context re-deriving this every render slice. Captured 2026-05-14 from codebase-architecture mapping; promote to real artifact when next render slice plan lands.

## Active campaigns

- **Unified-projection F1** (design + plan complete; ready for execution): Spec v2.8 greybeard-signed at `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`. Plan v1.1 codex-signed at `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`. Handoff at `~/.claude/projects/A--Games-mc2-opengl-src/memory/HANDOFF_2026_05_22_unified_projection_F1_ready_for_execution.md`. Correctness-only (CPU budget already met per F3 ~55us total post-A2). Collapses inline `axisSwap*worldToClip` at `gamecam.cpp:165-187` into `Camera::worldToClipGL()`; renames `terrainMVP` → `u_worldToClipGL` across 14 vert + 3 compute/frag + 10 CPU bind sites; deletes SSAO runtime entirely; Stage A-pre parity probe + Stage A atomic single-commit flip. 21 tasks across 4 phases. Execute via `superpowers:subagent-driven-development` skill.
