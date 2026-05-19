# MC2 OpenGL - GPU-Driven-Rendering Worktree

MechCommander 2 OpenGL port: tessellated terrain, PBR splatting, shadow maps, post-processing. This worktree is `claude/gpu-driven-rendering`. Canonical active-dev branch is `claude/nifty-mendeleev` (work pending merge from here). Root checkout (`terrain-pbr-mod`) is older; do not work there.

## Where to look first

- **Project direction** (three north stars + out-of-scope): `.planning/PROJECT.md`
- **MEMORY.md** (every load-bearing fact, linked to a topic file): `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- **Codebase maps** in `.planning/codebase/`: `ARCHITECTURE.md`, `STRUCTURE.md`, `STACK.md`, `INTEGRATIONS.md` (written 2026-05-14; grep before quoting line numbers)
- **Advisor fleet routing** (who answers what): `.claude/agents/DOMAINS.md` - canonical reference for the 12 MC2 advisor subagents + classification + gaps. Use this to find the right advisor for a question.
- **Performance state:** `docs/render-perf-snapshot.md` - bucket map + in-flight slice state + dependency graph (refresh when slices ship)
- **Render contract:** `docs/render-contract.md` (design) + `mclib/render_contract.*` (implementation) + `.claude/agents/mc2-render-contract-synthesizer.md` (refresh agent)
- **Meta-prompt** at `.claude/prompts/distill-session-into-advisor-agent.md` - paste at end of a substantive domain-work session to harvest a new advisor
- **Render-notes dump prompt** at `.claude/prompts/dump-render-observations.md` - produces dated notes in `docs/observations/` for the synthesizer to consume
- **Skills** in `.claude/skills/`: `/mc2-build`, `/mc2-deploy`, `/mc2-build-deploy`, `/mc2-check`, `/mc2-shader-diff`, `/mc2-amd-shader-review`, `adversarial-plan-review`, `greybeard`
- **Steering channel (shared, repo-root):** `A:/Games/mc2-opengl-src/.claude/STEERING.md` - out-of-band feedback for a barreling session in ANY worktree. `sh A:/Games/mc2-opengl-src/.claude/steer.sh "..."` (any terminal) blocks the session's next Bash/Agent/Task call and injects the text; agent runs `sh A:/Games/mc2-opengl-src/.claude/ack-steering.sh` to clear. Globally-registered hook `~/.claude/hooks` -> `.claude/hooks/steering_check.py`; walks up to repo root so one shared file serves every worktree.
- **Reference docs** in `docs/`: `architecture.md`, `amd-driver-rules.md`, `docs/plans/`, `docs/superpowers/specs/`
- **Maintenance hook:** `.claude/maintenance-rules.json` is consumed by the Stop hook (`~/.claude/hooks/gsd-staleness-monitor.js`); modifying load-bearing files surfaces related-update reminders

## Key paths

- Source: this worktree, `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/`
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.4/`
- CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Critical inline rules (every session reads these)

- **No emoji in any file, ever.** Em-dash, en-dash, standard Unicode punctuation OK; pictographic emoji not. Full scope and allowed-set list: `memory/feedback_no_emoji_in_files.md`.
- **No wall-clock time projections.** Describe complexity in code dimensions (subsystems touched, files modified, parity gates, soak windows), not time. Agent-SDK rates don't track human estimates.
- **Grep before citing file:line.** Every file:line in any output must be verified at write-time. Symbols are stable; line numbers drift. Full rationale: `memory/brainstorm_code_grounding_lesson.md`. Plan-stage formalization: `.claude/skills/adversarial-plan-review.md`.
- **Negative claims need opposite-direction grep.** "X is NOT consumed by Y" requires grep'ing Y, not the obvious-named consumer. See `memory/feedback_data_flow_audit_asymmetry.md`.
- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- **Full relink before deploy** when load-bearing functions change: `rm build64/RelWithDebInfo/mc2.exe` (+ changed `.obj`) before `cmake --build`, or `--clean-first`. CMake incremental builds leak stale linkage when inline funcs / templates / static state change.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2.
- **Git:** NEVER push to `alariq/mc2` origin. All work is local.
- **Shader `#version`:** Never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (4.3 context required for SSBO / std430).
- **Uniform API:** `setFloat` / `setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **GL_FALSE for terrainMVP:** direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. The `gamecam.cpp` comment claiming `GL_TRUE` is wrong; do not "fix" it.
- **Shader hot-reload fails silently:** bad compile = old shader stays active. Always check console after editing.
- **Vulkan-prep (forward-compat):** new GPU-resource code uses explicit device-mediated binding (`device.bindVertexBuffer(vb)`, NOT `vb.bind()`); assume zero implicit cross-call GL state. Do not regress the already-Vulkan-friendly patterns (enqueue/flush, std430 lockstep, [0,1] depth). PREP not a port - no full RHI ahead of need. Full rule set: `memory/vulkan_prep_explicit_device_discipline.md`.
- **Change discipline:** don't touch it if you don't have to (every touch has blast radius); when you must, bring it to the modern standard rather than matching surrounding legacy. This is the WHEN governor on the modernization triad; standalone "cleanup" slices need a blocking/debt justification. Full rule: `memory/minimal_touch_modern_when_touched.md`.

## Load-bearing memory pointers (read before touching the area)

- **Cull gates** (`memory/cull_gates_are_load_bearing.md`): `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive` gate `update()` AND allocation AND lifecycle. Bypass cascades into streaks, destruction, silent shape drop-outs. Mechs are the canary because they iterate last.
- **TGL pool exhaustion is silent** (`memory/tgl_pool_exhaustion_is_silent.md`): `getVerticesFromPool` returns NULL = shapes vanish. Pools at 500K.
- **Render funcs are enqueuers** (`memory/render_functions_are_enqueuers_not_submitters.md`): `XXX::render()` enqueues into `MC_TextureManager` master arrays. Actual GL submission = `gos_RendererEndFrame -> renderLists()`. MLR objects are the immediate-draw exception.
- **Dual-queue retirement debt** (`memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`): legacy `masterVertexNodes` + modern `masterHardwareVertexNodes` coexist; every mod slice has been ADDITIVE; retire legacy soon.
- **GPU-direct bring-up checklist** (`memory/gpu_direct_renderer_bringup_checklist.md`): 9 traps every new fast path hits. READ FIRST before any GPU-direct renderer.
- **Stock install must remain playable** (`memory/stock_install_must_remain_playable.md`): renderer modernization data must degrade to stock-compatible generation; no savegame depends on render caches.
- **Path separator** (`memory/mc2_path_separator_linux_build.md`): engine builds with `-DLINUX_BUILD` globally; `PATH_SEPARATOR` is `/`. Never hardcode `\\` against `_WIN32`.
- **PAUSE / UNPAUSE diagnostic** (`memory/pause_unpause_diagnostic_for_static_render_bugs.md`): if a render bug clears on pause and re-appears on unpause, it's `mcTextureManager->update()` cache eviction without `objectManager->update` re-cache.

## Review discipline (load-bearing)

When the user asks for "review", "code review", "second opinion", or any equivalent: **adversarial, code-grounded by default**. Read `.claude/skills/adversarial-plan-review.md`. High-stakes plans (architectural endpoints, legacy retirement, SSBO schemas, perf gates >=30%) get the full skill: grep every cited symbol, cross-reference every load-bearing constraint, list findings as CRITICAL / MAJOR / MINOR. Lower-stakes work gets prose-only review. When dispatching a review subagent, the dispatch prompt MUST include "use the adversarial-plan-review skill" verbatim. Origin: `memory/brainstorm_code_grounding_lesson.md`.

## Meta-fix discipline (load-bearing)

Before proposing OR writing any fix, rework, or offload: run the `greybeard`
skill (`.claude/skills/greybeard.md`). It forces an explicit `META-FIX` vs
`PATCH (justified)` ruling - the graybeard finds the one upstream change that
retires the bug *class* (usually deleting a legacy mechanism whose original
constraint no longer holds), not the local symptom patch. A patch with no
named meta-fix and no debt justification is not allowed. This codebase has a
documented history of additive slices netting ~0ms because the old path was
never deleted - greybeard is the guard against that. When dispatching an
advisor or fix subagent, the dispatch prompt MUST include "run the greybeard
skill" verbatim (same convention as adversarial-plan-review). Applies to the
main agent and every advisor; not required for trivial lookups.

## Documentation discipline (load-bearing)

Every cited symbol must be grep-verified AT WRITE-TIME, not after. Applies at every stage: brainstorm Q&A, recon, design, plan, review. Don't write prose then verify after; verify-then-write is the same wall-clock cost with no fictional content. Carve-out: intentions ("we will create X") need no grep. Full rationale + indirect-terrain v1 case study: `memory/brainstorm_code_grounding_lesson.md`.

## Advisor invocation discipline (load-bearing)

For any substantive question whose domain has a dedicated advisor per `.claude/agents/DOMAINS.md`, **spawn the advisor first**. The advisor reads MEMORY.md + topic files + current code with fresh context; the main agent's compiled knowledge is stale by definition.

**This applies to:**
- Pipeline questions (rendering, shaders, mech rendering / animation / runtime, terrain-indirect, GameOS platform)
- Methodology questions (CPU-to-GPU offload, render-contract refresh, perf-slice sizing, cull-cascade safety)
- Asset-format questions (mission data, FST / .fit / .tga / .wav, file IO, init order)
- Build-system questions (CMake, vcpkg, FFmpeg delay-load, link libraries, full-relink)

**Does NOT apply to:**
- Trivial lookups ("what file is X in?", "what's the build command?")
- Follow-up clarification on something already established this session
- The user explicitly says "answer directly, don't spawn"

The routing table in `.claude/agents/DOMAINS.md` is the authoritative match. When a question straddles two advisors, spawn the most-specific one; advisors have explicit `<cross_references>` and `<limits>` DEFER sections that route cross-domain questions to siblings.

**Why this matters:** advisors carry tacit knowledge in their `<known_pitfalls>` blocks that does NOT live in MEMORY.md (session-derived patterns, gotchas, "we hit this twice" lessons). Advisors grep-verify file:line citations during invocation per their Rule 0 ("grep before line numbers"). Answering from main-agent context bypasses both protections and silently produces stale answers. The 2026-05-14 pipeline-matrix build proved the value: the build's grep discipline caught the substrate_frameBegin pause-bug live on this branch and a stale line citation (`mcTextureManager->update` is at `mission.cpp:527`, not the `:509` cited in this file and the pause-diagnostic memory). (A third example once cited here -- `gpu_driven_terrain_solid.comp` "doesn't exist on disk" -- was itself retired 2026-05-15: the file exists at `shaders/gpu_driven_terrain_solid.comp` and is Fix B's sole terrain-quad projection authority; the stale-example removal is the same grep discipline applied recursively.)

## Model routing

- **haiku:** lookups, summaries, simple edits, renaming, formatting
- **sonnet:** standard implementation, debugging, code review. Always diff changes from haiku.
- **opus:** architecture, deep analysis, complex refactors only. Always diff changes from sonnet/haiku. Give other agents isolated context.

## Key source files (starting points; always grep to confirm current line)

- `GameOS/gameos/gameos_graphics.cpp` - renderer core (terrain draw, shadow draw, uniform caching)
- `GameOS/gameos/gos_postprocess.cpp` - FBOs, bloom, shadows, post-process
- `mclib/txmmgr.cpp` - `renderLists()` flush, master-node arrays, shadow pre-pass
- `mclib/mech3d.cpp` - engine-side mech appearance / rendering (5139 lines, biggest file in mclib; engine, NOT game-side AI)
- `code/gamecam.cpp` - frame loop, render-call sequence
- `shaders/gos_terrain.frag` - terrain splatting, POM, shadow sampling, distance LOD
- `shaders/include/shadow.hglsl` - `calcShadow()` with variable-tap Poisson PCF

## Profiling

- Tracy always compiled in (`TRACY_ENABLE`). GPU zones on shadow / terrain / 3D / post-process. AMD RGP works externally via Radeon Developer Panel for shader-level analysis.
- **100 ns floor:** never instrument a region whose work is <100 ns. Tracy zone overhead is ~20-50 ns. Per-element / per-quad / per-vertex / per-call zones in hot loops are FORBIDDEN (`gos_getTextureHandle` ~20 ns is the canonical example). Coarse per-pass zones (one zone per phase, one call per frame) are correct. Origin: commit `fdc47bc` 2026-05-07.

## Debug instrumentation rule (for reworks)

Any rework touching object lifecycle, cull / visibility gates, render path, resource lifetime, or cross-system control flow must land env-gated `[SUBSYSTEM]` lifecycle prints in the same commit. Instrumentation stays gated off by default - do not delete after the bug is fixed; demote to silent. Log at lifecycle boundaries only (init, register, first-use, teardown, fallback). Canonical macro pattern matches existing `MC2_DEBUG_SHADOW_COLLECT`. Full rationale, naming conventions, anti-patterns: `memory/debug_instrumentation_rule.md`.

## Tier-1 instrumentation env vars (current set)

- `MC2_TGL_POOL_TRACE=1` - per-frame TGL pool NULL trace; monotonic summary every 600 frames always-on
- `MC2_DESTROY_TRACE=1` - per-destruction cull / lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` - suppress first-error prints (PRINT-ON by default; drain loop always runs)
- `MC2_ASSET_SCALE_TRACE=1` - per-key runtime lookup events; first `oob_blit` per `(path, callerTag)` always-on
- `MC2_ASSET_SCALE_SELFTEST=1` - synthetic 2x/4x/8x/1.5x golden tests at startup
- `MC2_HEARTBEAT=1` - stderr `[HEARTBEAT]` per second; detect renderer freezes during mod load
- `MC2_REVERSE_Z_TRACE=1` - `[REVERSE_Z v1]` one-shot lifecycle prints: scene projection-matrix build (near/far + sampled NDC z of near/far; reverse-Z expects near->1, far->0) and the inverseProjectZ fence-seam first use

Startup banner `[INSTR v1] enabled: ...` appears at log start. Grep schema-version with `\[SUBSYS v[0-9]+\]`. Asset-scale spec: `docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md`.

Pre-commit invariant scripts (run if you touched the relevant area):
- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas / `code/mechicon.cpp`: `sh scripts/check-asset-scale-callers.sh`

## Smoke sessions are USER-DRIVEN (load-bearing)

**The user can see and control every smoke session.** `run_smoke.py` launches mc2.exe in a real game window the user is watching live. They can drive the camera (mouse/keyboard), observe visual bugs (triangles, flicker, missing geometry), and terminate early. Smoke feedback like "I saw the triangle," "still doing it," "the second smoke had it" is **first-hand visual observation**, not their reading of a log.

**Anti-patterns the agent must NOT do:**
- DO NOT tell the user "please run mc2.exe manually and reproduce." They are *already* doing that during the smoke command you just ran.
- DO NOT ask "can you confirm by re-running with X env var." The user is the visual observer; each smoke run is the user-driven repro.
- DO NOT say "the smoke isn't reproducing the bug for me" when the user reports it IS happening. Their visual evidence outranks any silent probe.

**What to do instead:**
- After every smoke the user reports a bug in: read `tests/smoke/artifacts/<latest>/{mission}.ring_trace.log` for that run's probe events. The runner snapshots the file-sink per mission.
- When the user says "still doing it" they mean the most recent smoke. Find the latest artifact dir, analyze its ring_trace.log.

## Smoke gate ("did I break it")

Default regression gate for render / init / cull / asset changes:

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing
```

- `tier1` = 5 hand-picked missions covering different biomes (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`). 30s per mission. Isolated/clean conditions.
- exit `0` = pass. Nonzero = inspect `tests/smoke/artifacts/<timestamp>/`.
- DO NOT add `--with-menu-canary` to the default gate (desktop / screen-coord-bound, unreliable). Manual menu test: `--menu-canary`.
- Tier1 perf numbers are NOT comparable to tier2 (isolated vs sequenced / stress). See "Measurement semantics" in `tests/smoke/README.md`.
- Faster loop: add `--fail-fast`.

## Known issues (current)

- Shadow re-render stutter when camera moves >500 units. Fix: static world-fixed shadow map (design ready).
- **Water shoreline z-fight on zoom/elevation-change (NOT pan); water sits slightly low (pre-existing).** Interim fast-path fixes shipped 2026-05-17 (gate-asymmetry +538us, un-armed legacy guard, MVP 1-frame-lag consistency verified 926/0 — recede/flicker/intro-vanish resolved). Residual z-fight = constant screen-z depth-fudge distance-nonlinearity (ruling-compliant clip-z fix pending); water-low is a separate pre-existing baseline issue. Water is slated full-GPU; that rewrite inherits both. Full state: `memory/water_fastpath_interim_fixes_and_residuals.md`.
- Shadow banding shifts with camera rotation (view-dependent terrain geometry).
- **First-launch black terrain intermittency** - tier1 first mission occasionally renders black; second mission normal. Suspected: GPU/shader state dirty from previous mission teardown or first-frame ordering. Repro: tier1 with `--fail-fast`.
- **Options menu writes bad ResolutionX / Y to options.cfg** - opening options dialog may re-save non-800x600 res (observed 4096x2160 on 4K). Engine UI canvas is 800x600 and self-scales; other values break HUD scale + video positioning. Diagnostic and fix candidates: `memory/options_cfg_resolution_drift.md`.
- **drawPass-retirement decal static-bake (`MC2_TERRAIN_INDIRECT_OVERLAY`): DEFAULT-ON since the `60f2ef8` Stage-6 flip (only `=0` reverts).** `IsOverlayEnabled()` returns true unless the var is literally "0"; on the stock/default path both SOLID and OVERLAY are armed so the per-quad `draw()` loop in `Terrain::render drawPass` is SKIPPED (the zone is ~empty by default; the per-quad terrain draw identity is retired in stock play). Slice A+B WIRED & objectively validated 2026-05-17 (clip-safe `px.z in [0,1)` guard in `terrain_overlay.vert`; `DrawDecalStatic` call site `3056f0e`; `decal_corner_probe` demoted `66f1ad5`; armed+TRACE tier1 5/5, `GL_INVALID_OPERATION`=0, `decal_vbo_built` 4668-vert parity). Remaining endpoint = the inherently USER-DRIVEN substitutive non-COST_SPLIT total-frame Tracy proof (drawPass->~0 armed, no displaced cost, both regimes incl zoomed-out) + decal visual canary. **2026-05-19: any code/comment claiming this is "default-OFF" is STALE — fixed in `gos_terrain_indirect.h`, `terrain.cpp`; see `memory/water_invproj_parity_is_DIVERGENT_not_freebie.md` and the quadSetupTextures orphan-walk-retirement effort.** DO NOT re-derive. Full state: `memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`.

## Do NOT upscale these atlases

`code/mechicon.cpp` hardcodes `unitIconX/Y` (32/38) and reads source-pixel offsets against `s_MechTextures->width` / `s_pilotTextures->width`. Oversized source TGAs (via upscaler `*_4x_gpu/` pass or Magic's Unofficial Expansion overlay) scramble icon sub-rectangles. Keep all 9 icon atlases at FST-archive resolution (3 kinds x 3 tiers - mech damage schematic, pilot face panel, hardpoint icons) plus `mcl_pr_pilotskillicons.tga`. Pre-commit guard: `sh scripts/check-asset-scale-callers.sh`.

## Memory & CLAUDE.md discipline

- **Auto-memory index:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- **No session narratives in CLAUDE.md.** Dated "what was proven / changed" logs go in commit messages or memory files.
- **Root CLAUDE.md is a thin pointer ONLY.** This worktree CLAUDE.md is authoritative. Root enforced by `scripts/check-claude-md-pointer.sh`.
- **New durable finding -> memory file + MEMORY.md index entry.** Unlinked memories are invisible. Group under matching section.
- **Superseded facts -> update or delete, don't append.** Memories decay; the index entry must reflect current truth.
- **Before writing a new memory:** `grep -i <keyword> memory/*.md` to dedupe.
- **Keep this file under 200 lines.** Currently ~180. If it grows past 200, extract to memory and link.

## Pending durable artifacts (write-when-ready)

- **Render contract document** at `docs/render-contract.md` (or `.planning/codebase/RENDER-CONTRACT.md`) - enumerate at the function / symbol level: who enqueues into which master array, who flushes when, what state is inherited at each hook point, what each Track A/B/C/coalesce slice consumes and emits. Currently we burn context re-deriving this every render slice. Captured 2026-05-14 from the codebase-architecture mapping session; promote to a real artifact when the next render slice plan lands.
