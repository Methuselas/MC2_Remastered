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
- Build:  `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/` — the root checkout's `build64/` is stale (terrain-pbr-mod branch); do NOT use it
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
- `MC2_RENDER_CONTRACT_ASSERT=1` — stderr `[RENDER_CONTRACT_ASSERT]` when GL draw-buffer/depth state mismatches `render_contract::stateContractFor()`; inits once after glewInit in gameosmain.cpp (Phase 2)
- `MC2_MECH_PICK=1` — enable mech-pick consumer (M2.6); requires `MC2_OBJECT_ID_BUFFER=1` substrate
- `MC2_MECH_PICK_DEBUG=1` — verbose mech-pick logging (M2.6)
- `MC2_MECH_PICK_PIERCE_FOG=1` — debug-only fog bypass (M2.6); respect fog by default

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
- **MLR-rendered mechs do not write object IDs (M2.5 known gap).** MLR-rendered mechs do not write object IDs in M2.5. M2.6 pickup will work only for GPU-batched mech pixels. Empirical tier1 data shows mlr_mech_draws=0 across all 5 missions, so the gap is rare-in-practice. If tier1 ever shows mlr_mech_draws>0, M2.6 must preserve mover-first legacy fallback for those mechs and cannot claim full mech GPU-pick coverage. Counter: `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M` on per-mission summary.
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
- **RenderWorld Slice M1** (SHIPPED 2026-05-23): static-prop adapter routes 5 audited call sites (`mclib/bdactor.cpp:1471,2802,4273,4859`, `code/warrior.cpp:7593`) through `GameAdapters::StaticPropRenderAdapter` -> `RenderWorld::upsertStaticProp` -> `GpuStaticPropRegistry::registerRecipe`. Three new modules at repo root: `RenderCore/` (Handle/RenderObjectDesc/DrawPacket/StaticPropInstanceDesc POD mirror), `RenderWorld/` (engine API + private `legacy/static_prop_backend.{h,cpp}` bridge — the ONLY engine TU that touches `gos_static_prop_batcher.h`), `GameAdapters/` (the ONLY module bridging both sides). New virtual `Appearance::getStaticRecipeIndex()` enables m5 late-spawn handle adoption. `[RENDER_WORLD v1]` banner emits per mission (init / frame=N objects=N / destroy); opt-in `MC2_RENDER_WORLD_TRACE=1` enables per-frame + per-event events. Firewall: `scripts/check-include-firewall.sh` (Phase 1 grep, full Section 12 SCOPE_DIRS with `[ -d ]` guards, forbidden-headers + forbidden-symbols, self-tested). Adapter is TEMPORARY per spec section 10 deletion criteria (greybeard PATCH ruling 2026-05-23): deletion lands when (1) game-side class refactored to call RenderWorld directly OR retired AND (2) tier1 5/5 + parity probe show zero pixel delta for one release without the adapter. Tier1 5/5 PASS; objects counts: mc2_01=997, mc2_03=2552, mc2_10=2611, mc2_17=1521, mc2_24=2641. M1.5 next (object-ID buffer); M2..M5 follow. Spec: `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`. Plan: `docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`.
- **RenderWorld Slice M1.5** (SHIPPED 2026-05-23): substrate-only ObjectID buffer. `MC2_OBJECT_ID_BUFFER=1` env-gated `R32_UINT` MRT attachment at `GL_COLOR_ATTACHMENT2` on the main scene FBO; static-prop fragment writes `Handle.raw()` via `layout(location=2) out uint v_objectId` (coalesce path consumes `PerDrawEntry.objectIdRaw` after atomic `_pad0` rename; legacy path declares `u_objectIdRaw` but upload is M1.6+). Three-owner chain: registry `getRecipeIndexForType(typeID)` -> RenderWorld `objectIdRawForStaticPropRecipe(recipe)` -> batcher `PerDrawEntry.objectIdRaw = handle.raw()`. RenderWorld API extension: `s_objectRecords` always-populated table indexed by handle.index() + `lookupAtPixel(x,y) -> LookupResult` synchronous readback (generation + alive check). C1 META-FIX: `setSceneDrawBuffers(SceneDrawBufferMode, bool objectIdAttachmentReady)` helper in `gos_postprocess.cpp` centralizes scene-FBO draw-buffer policy across 5 sites (createFBOs, beginScene, runScreenShadow, runGodRays, runShoreline); retires the "scattered glDrawBuffers policy drift" bug class — M2..M5 mech/terrain/VFX/overlay slices extend via single-helper edit not 5-site audit. Greybeard ruling 2026-05-23: META-FIX. Tier1 5/5 PASS env-OFF AND env-ON (0 fps delta avg, 0-2 fps p1% delta — well under spec 0.5ms p99 budget). Passive canary `[OBJECT_ID_SELFTEST v1] result=PASS sampled=10 valid_hits=4 invalid_hits=6` on mc2_03 (7900 XTX) replaces removed AMD integer-MRT doc claim with runtime evidence. Substrate self-test `[RENDER_WORLD_SELFTEST v1] result=PASS step=all` validates record-table generation/alive lifecycle. Debug-mode pixels return `Handle::invalid()` by design (early-return skips emit). Picking integration is M1.6 separate slice (substrate inspectable via debug API + log only; missiongui.cpp untouched). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-5-objectid-buffer-plan.md`.
- **RenderWorld Slice M1.6** (SHIPPED 2026-05-23): static-prop pick wiring on top of the M1.5 substrate. `MC2_STATIC_PROP_PICK=1` + `MC2_OBJECT_ID_BUFFER=1` enables Shift+left-click as an inspect-only static-prop selection gesture; legacy plain-LMB selection and Shift+LMB additive-select on a friendly mover are preserved verbatim. `MissionInterfaceManager::tryStaticPropPick` helper called from tails of both `updateOldStyle` and `updateAOEStyle`; mover-first fallback via 4-site `moverSelectedThisFrame` observable set at `code/missiongui.cpp:1460/1487/1690/1705` (the 4 `setSelected(true)` writer sites identified by spec Q6; sibling `setSelected(false)` sites at `:1462/:1483/:1692/:1701` explicitly NOT instrumented). On hit: emits `[STATIC_PROP_PICK v1] hit handle=N idx=N gen=N recipe=N screen=(x,y) gl=(x,y)` and updates `RenderWorld::StaticPropSelectionDebugState` (mutex-guarded single slot, cleared on per-mission `RenderWorld::destroy()`). On miss with `MC2_STATIC_PROP_PICK_DEBUG=1`: emits diagnostic miss log. Coord-translation: Win32 mouseX/Y (in UI-canvas viewport space via `mouseXPosition * viewMulX` from HUD-scene-split work) scaled to FBO-pixel via `gos_GetViewport()` then GL y-flipped. User-driven canary on mc2_03 PASS: 26 hits on distinct static-prop handles (idx range 68..2540); 11 misses on terrain/sky with full diagnostic; legacy Shift+mover toggle preserved with zero M1.6 log lines emitted. Tier1 5/5 PASS env-OFF (pixel-parity at idle vs M1.5 HEAD) AND env-ON (substrate active, 0 fps avg delta vs env-OFF baseline). Greybeard ruling 2026-05-23: PATCH (justified) — first gameplay-side consumer; named META-FIX (`tryGameplayPick(request)` + `screenToFboPixel` extraction) deferred to M2 mech-pickup trigger. Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-6-staticprop-pick-plan.md`. **Note:** Log schema `[STATIC_PROP_PICK v1]` and `StaticPropSelectionDebugState` were renamed to `[GAMEPLAY_PICK v1] kind=StaticProp` and `GameplaySelectionDebugState` in M2.6 META-FIX (commit ca08d0c). Archaeology: grep `[GAMEPLAY_PICK v1]`.
- **RenderWorld Slice M2-pre** (SHIPPED 2026-05-23): preemptive META-FIX refactor of M1.6 gameplay-pick machinery. Pure refactor; no behavior change. Pays the M1.6 greybeard debt before any mech-specific work starts. New TU `code/gameplay_pick.{h,cpp}` hosts shared types (`GameplayPickRequest`/`Context`/`Result` with `Outcome::skipped|gated|miss|hit`) + `tryGameplayPick(req)` dispatcher (env-substrate gate + 4 gesture gates + mover-first short-circuit + viewport query + bounds + coord scale + lookupAtPixel) + pure `screenToFboPixel(...)` coord transform. `MissionInterfaceManager::tryStaticPropPick` refactored to delegate: category env-flag gate -> request build -> dispatcher -> switch on outcome -> `[STATIC_PROP_PICK v1]` log + state updates. Substitutive proof: body-restricted grep on refactored tryStaticPropPick returns 0/0/0/1 for `IsObjectIdBufferEnabled`/`gos_GetViewport`/`lookupAtPixel`/`tryGameplayPick` — the M1.6 inline gate ladder is GONE. New automated validator `RunGameplayPickSelfTest()` gated by `MC2_GAMEPLAY_PICK_SELFTEST=1` + `MC2_OBJECT_ID_BUFFER=1` exercises 8 synthetic GameplayPickRequest inputs (gesture-gate FAILs + mover-gate + off-screen + clean spine) and asserts outcomes match expected; mirrors M1.5 `[RENDER_WORLD_SELFTEST v1]` shape; wired into `RenderWorld::init()` after `runSubstrateSelfTest()`. First engine-side -> game-side forward-decl reach in the codebase (firewall direction note: SCOPE_DIRS excludes `code/`; script does not enforce direction). Greybeard ruling 2026-05-23: META-FIX (both for the spine AND for `screenToFboPixel` sub-extraction); the substitutive grep proof clears the bug-class-retirement hinge criterion even at sample-size-one. Tier1 5/5 PASS env-OFF (pixel-parity at idle vs M1.6 HEAD `1d1d5d3`) AND env-ON `MC2_GAMEPLAY_PICK_SELFTEST=1 MC2_OBJECT_ID_BUFFER=1` (PASS=5 FAIL=0 SKIP=0). M2-pre satisfies the M1.6 META-FIX trigger preemptively; M2 (route-only MechRenderAdapter), M2.5 (mech object-ID substrate), M2.6 (mech pickup integration) follow. Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md`.
- **RenderWorld Slice M2** (SHIPPED 2026-05-23): route-only MechRenderAdapter. Every live Mech3DAppearance instance now has a RenderObjectHandle stored on it (mechRenderHandle field, protected; three public ForAdapter accessors). GameAdapters/MechRenderAdapter.{h,cpp} bridges BattleMech::init/destroy to RenderWorld::registerMech/destroyMech. Mechs allocate from the unified s_objectRecords table at kMechHandleBase=0x00010000 (65536) to avoid recipe-index collision with static props (max known: 2641). RenderObjectRecord gains kind (RenderObjectKind enum) and debugCookie fields. frameBannerTick extended: [RENDER_WORLD v1] now emits static_props=S mechs=M alongside objects=T. endMission force-clears leaked handles via clearAllMechRecords(). Three mandatory call sites in code/mech.cpp: pre-init destroyMech (re-init guard), syncSpawn after initFX(), destroyMech before delete. mission.cpp Mech::beginMission/endMission adjacent to StaticProp calls. Firewall clean; allowlist entry added. Tier1 5/5 PASS env-OFF and env-ON MC2_RENDER_WORLD_TRACE=1. Next: M2.5 (mech object-ID substrate: per-mech writes to R32_UINT attachment-2); then M2.6 (mech pickup via tryGameplayPick spine).
- **RenderWorld Slice M2.5** (SHIPPED 2026-05-23): mech object-ID substrate -- closes the M2 handle-to-GPU loop. `GpuMechInstance` (std430 SSBO) grows 48B->64B with `objectIdRaw` field + 3 generic `_padN` reserved uints (per Q2). `mech.vert` gains `flat out uint v_objectIdRaw` reading from per-instance SSBO; `mech.frag` adds `layout(location=2) out uint v_objectId` under `#ifdef MC2_OBJECT_ID_BUFFER`. GLSL prefix injection at `gos_mech_batcher.cpp:loadProgramsIfNeeded()` mirrors `gos_static_prop_batcher.cpp:510-521` pattern. Submit-site fill at `mclib/mech3d.cpp:2598` unconditional (`desc.objectIdRaw = getRenderWorldHandle().raw()`; per Q3 -- env gates shader output, not CPU prep). Always-on per-mission counters split across two log lines (per M1 amendment): `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` (from `gos_mech_batcher.cpp::onMapUnload()`) + `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M` (Q6 amendment 2: makes MLR/CPU-fallback gap MEASURABLE rather than assumed). MLR observability via file-scope `extern "C" consumeAndResetMlrMechDraws()` in `mclib/mech3d.cpp`. Tier1 5/5 PASS env-OFF AND env-ON `MC2_OBJECT_ID_BUFFER=1 MC2_MECH_OBJECT_ID_SELFTEST=1`. New `[MECH_OBJECT_ID_SELFTEST v1] result=PASS` self-test (per Q1 separate canary) wired into `RenderWorld::init()` after substrate + gameplay-pick self-tests. Per-mission gpu_mech_id_writes: mc2_01=63836, mc2_03=19230, mc2_10=53872, mc2_17=407061, mc2_24=1232. **MLR gap empirically rare:** all 5 tier1 missions show `mlr_mech_draws=0`, validating Q6 assumption -- M2.6 can ship without conditional MLR fallback warnings. Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-plan.md`. Next: M2.6 (mech pickup via `tryGameplayPick` spine from M2-pre; substrate proven so pickup is route-only).
- **RenderWorld Slice M2.6** (SHIPPED 2026-05-23): mech pickup integration (inspect-only v1). Closes the RenderWorld arc's first user-visible loop. Shift+LMB on a hostile mech visible to sensors emits `[GAMEPLAY_PICK v1] hit kind=Mech handle=N idx=N gen=N mech=PTR screen=(x,y) gl=(x,y)` — inspect-only, no gameplay mutation. Three new env vars: `MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG` (default off; respect fog). Handle→BattleMech reverse-lookup via linear scan over `ObjectManager` movers (Option B; partId-cookie rejected because `code/mission.cpp:2987` reassigns post-syncSpawn — CRITICAL-1 spec fix). Fog predicate respects ShowMovers + multiplayer-defeat carve-outs (full predicate from `code/missiongui.cpp:1272-1278`). META-FIX scope retired the M1.6 per-kind log+state pattern: `[STATIC_PROP_PICK v1]` → `[GAMEPLAY_PICK v1] kind=X`, `StaticPropSelectionDebugState` → `GameplaySelectionDebugState` with `RenderObjectKind kind` discriminator, `setLastStaticPropPick` → `setLastGameplayPick`. Latent post-M2.5 mislabel bug fixed simultaneously (M1.6 wrapper now kind-guards before consuming static-prop fields). New `RenderWorld::getMechsAliveCount()` substrate accessor; new `[MECH_PICK_SELFTEST v1]` validator hosted in `GameAdapters/MechRenderAdapter.cpp` (firewall: RenderWorld can't include game headers). Tier1 5/5 PASS env-OFF AND env-ON `MC2_OBJECT_ID_BUFFER=1 MC2_MECH_PICK=1 MC2_MECH_PICK_SELFTEST=1` AND env-ON `MC2_MECH_PICK_PIERCE_FOG=1`. Gate 6 substitutive-proof grep: zero hits for retired symbols across all source files. mlr_mech_draws=0 carries through (Q6 amendment empirical). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md`. Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m2-6-mech-pickup-plan.md`. Next: M2.7 (mech select/attack mutation) if needed; M3 (terrain pick) follows.
- **RenderWorld Slice M6** (SHIPPED 2026-05-24): firewall audit script —
  no raw GL from game side. Codifies the empirical finding (M6 recon)
  that `code/` has ZERO raw GL calls and `mclib/` has 3 diagnostic-only
  gated hits in `render_contract.cpp`. New
  `scripts/check-no-raw-gl-from-game.sh` (function-level grep, NOT
  include-level) with allowlist of exactly one TU
  (`mclib/render_contract.cpp`; gated by `MC2_RENDER_CONTRACT_ASSERT=1`).
  Migration guide §3.5 documents the rule. CI / pre-commit can wire the
  script into existing hook infrastructure. Tier1 5/5 PASS (no source
  changes). GameOS reviewer-discipline gap (M2.5 MAJOR-1 carry-over)
  deferred to optional M6.5. Turns the arc from "discipline by memory"
  to "discipline enforced by script." Spec:
  `docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md`.
  Recon:
  `docs/superpowers/explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md`.
- **RenderWorld Slice M3** (SHIPPED 2026-05-24): terrain reservation/deferral. Adds `RenderObjectKind::Terrain = 2` enum value + `kTerrainHandleBase = 0x40000` constant + defensive `lookupAtPixel` tripwire (warn if `kind=Terrain` ever returned — no writer should produce it). Recon proved GPU terrain identity has no current consumer; CPU `Terrain::worldToTile` already returns tile R/C + type + elevation. Forward-compat: if M3.1 ever ships per-quad terrain identity (editor-driven), use `subKind = Base/Water/Decal/Mine` payload (NOT separate enum values). `Terrain::IsGameSelectTerrainPosition` preserved (ground-click path is canonical). No shaders edited, no adapter, no env var, no consumer. Tier1 5/5 PASS (no source path fires). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`.
- **RenderWorld Slice M5** (DEFERRED INDEFINITELY 2026-05-24): "overlay" had 7 in-tree meanings without identity-needing consumers. Enum slot `Overlay` un-reserved (comment-only deferral note in `RenderObjectKind`). If a future use case emerges, ship as a new named slice (HoverKindIndicator / RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT as "M5 Overlay"). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`.
- **RenderWorld Slice M4** (SHIPPED 2026-05-24): VFX prohibition + scaffold. Adds `RenderObjectKind::Vfx = 3` enum value + `kVfxHandleBase = 0x00080000u` constant (reserved, unused) + NEW `scripts/check-vfx-no-objectid.sh` firewall grep gate. **VFX shaders are PROHIBITED from writing attachment-2** — additive/translucent blending + R32_UINT last-write-wins would clobber M2.6 mech-pick under particles (muzzle flashes, smoke, tracers, impacts). Migration guide §3.6 documents the rule + rationale; allowlist scripted but expected empty forever. NO adapter, registerEffect, per-emitter handles, objectIdRaw fields, or shader writes. Source-game-object lookup ("which mech fired this explosion?") stays in game logic (source known at fire-event time; GPU should not rediscover via pixel). Caveat: gosFX dev-override (`MC2_DISABLE_GOSFX=0`) remains broken under unified-projection F1 (see known-issues section) — future VFX work must NOT use that path as a substrate proof. Tier1 5/5 PASS env-OFF. Greybeard ruling: META-FIX (retires bug-class "future contributor adds VFX objectID write, breaks M2.6 mech-pick" before it can occur; substitutive proof = firewall returns 0 hits + future commit re-introducing one fails CI). Spec: `docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md`. Resolutions: `docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md`. Plan: `docs/superpowers/plans/2026-05-24-renderworld-slice-m4-plan.md`.
