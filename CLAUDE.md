# MC2 OpenGL — nifty-mendeleev (canonical worktree)

MC2 OpenGL port: tessellated terrain, PBR splatting, shadow maps, post-processing. Active branch `claude/nifty-mendeleev` (0.4 gpu-driven-rendering arc merged 2026-05-18). Root `terrain-pbr-mod` = STALE, do NOT use. Detail in `docs/`, memory in `~/.claude/projects/A--Games-mc2-opengl-src/memory/`, planning in `docs/superpowers/`.

## Topic tree (read relevant branch before starting work)

```
CLAUDE.md (this file)
├── docs/critical_inline_rules.md      — emoji ban, build/deploy, shaders/GL,
│                                         change discipline, C++17
├── docs/known_issues.md               — shadow stutter, water z-fight,
│                                         options.cfg drift, blocked slices,
│                                         RenderWorld arc residuals
├── docs/tier1_env_vars.md             — MC2_* instrumentation knobs +
│                                         pre-commit invariant scripts
├── docs/disciplines.md                — review / meta-fix / documentation /
│                                         advisor invocation / smoke session /
│                                         subagent dispatch (lean intake)
├── docs/load_bearing_pointers.md      — cull gates, TGL pools, GPU-direct
│                                         checklist, etc.
├── docs/active_campaigns.md           — in-flight + shipped slice ledger
│                                         (RenderWorld arc, CXX17, etc.)
├── docs/renderworld_arc_status.md     — RenderWorld arc steady-state ledger
│                                         (M1..M6 + decisions for M3/M4/M5)
├── docs/renderworld_migration_guide.md — contributor onboarding for the arc
├── docs/cxx17-coding-rules.md         — allowed/cautioned/avoided C++17 features
├── docs/asset-pipeline.md             — CANONICAL asset inventory + pipeline:
│                                         every asset's location/loader/res/
│                                         upscale-cook state + render-vs-gamedata
│                                         owner. UPDATE when assets are touched
│                                         (scripts/check-asset-pipeline-doc.sh)
├── docs/asset-modernization-pipeline.md — asset mod plan + status (P1-F shipped)
└── ~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md
    └── INDEX-{RENDERING,SHADERS,TERRAIN,MECH,BUILD-DEPLOY,
                MISSION-DATA,SMOKE-TEST,PROCESS}.md
                                       — read matching INDEX-TOPIC when
                                         starting domain work; individual
                                         memories linked from there
```

## Orientation (where to look first)

- **Project direction:** `.planning/PROJECT.md`
- **Codebase maps:** `.planning/codebase/{ARCHITECTURE,STRUCTURE,STACK,INTEGRATIONS}.md` (2026-05-14; grep before quoting line numbers)
- **Advisor routing:** `.claude/agents/DOMAINS.md` (12 MC2 advisor subagents)
- **Perf state:** `docs/render-perf-snapshot.md`
- **Render contract:** `docs/render-contract.md` + `mclib/render_contract.*` (Phase 2: `MC2_RENDER_CONTRACT_ASSERT=1`)
- **Skills:** `.claude/skills/` — `/mc2-build`, `/mc2-deploy`, `/mc2-build-deploy`, `/mc2-check`, `/mc2-shader-diff`, `/mc2-amd-shader-review`, `/mc2-validate`, `/mc2-render-spine-advisor`, `/mc2-gsd-planner-executor`, `adversarial-plan-review`, `greybeard`, `cost-split-recon-bucket-design`
- **Steering:** `A:/Games/mc2-opengl-src/.claude/STEERING.md` (`sh .claude/steer.sh "..."` blocks next Bash/Agent; agent runs `ack-steering.sh` to clear)
- **MCP-first workflow:**
  1. `get_render_health()` — check liveness (`live=true` = periodic dump + pid alive + recent)
  2. `get_latest_smoke_report()` — structured smoke pass/fail with fps per mission
  3. `get_diagnostic_events(tag, 50)` — query specific JSONL diagnostic tag
  4. Read raw logs ONLY when MCP unavailable/stale/corrupt, crash-handler output needed, or a tool points to a specific artifact log.
  - Requires engine running with `MC2_DEBUG_STATE_DUMP=1` for live state.
  - Stale = `seconds_since_update > 30` or `pid_alive=false`.

## Key paths

- Source:  `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- Build:   `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/` — root `build64/` is STALE; do NOT use
- Deploy:  `A:/Games/mc2-opengl/mc2-win64-v0.4/`
- CMake:   `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Shell/tooling note

- PowerShell sessions here may load a blocked profile script and print an execution-policy error. Use `powershell -NoProfile` for shell commands that need a clean startup.
- `cmake` may not be on PATH in this environment. Prefer the explicit Visual Studio or CMake full path from the toolchain when invoking builds.
- If a tool is missing from PATH, use the full executable path instead of assuming shell resolution.

## Smoke gate

Default regression gate. **ALWAYS** `--keep-logs`, NEVER `--with-menu-canary`, NEVER `--duration` >30s, NEVER concurrent with another smoke or mc2.exe trace.

**Canonical invocation (verbatim; subagents must copy-paste):**

```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

- Canonical smoke enables CONFIG, BUILD, and DEVICE so MCP can verify diagnostic trace startup, build provenance, and GL device identity.
- NEVER `--kill-existing`: taskkills concurrent mc2.exe (false `crash_silent`). run_smoke holds concurrency-safe lock. Enforced by `scripts/check-smoke-matrices.py`.
- `tier1` = 5 missions (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`). 30s each.
- Exit `0` = pass. Nonzero → inspect `tests/smoke/artifacts/<timestamp>/`.
- Inner-loop: 2-mission subset (`--mission mc2_01 --mission mc2_24`). tier1 5/5 for slice gates only.
- Visual iteration: `--duration 60`. Faster fail: `--fail-fast`. Full policy: `memory/feedback_smoke_*.md`.

## Key source files (grep to confirm line)

- `GameOS/gameos/gameos_graphics.cpp` — renderer core (terrain draw, shadow draw, uniform caching)
- `GameOS/gameos/gos_postprocess.cpp` — FBOs, bloom, shadows, post-process
- `mclib/txmmgr.cpp` — `renderLists()` flush, master-node arrays, shadow pre-pass
- `mclib/mech3d.cpp` — engine-side mech appearance/rendering (5139 lines; engine NOT game AI)
- `code/gamecam.cpp` — frame loop, render-call sequence
- `shaders/gos_terrain.frag` — terrain splatting, POM, shadow sampling, distance LOD
- `shaders/include/shadow.hglsl` — `calcShadow()` Poisson PCF

## Profiling

Tracy compiled in (`TRACY_ENABLE`). GPU zones on shadow/terrain/3D/post-process. AMD RGP via Radeon Developer Panel. **100ns floor:** never instrument region <100ns; per-element/per-quad/per-vertex zones in hot loops FORBIDDEN. Coarse per-pass zones only.

## Model routing

- **haiku:** lookups, summaries, simple edits. **sonnet:** impl, debug, review. **opus:** architecture, deep analysis only — give isolated context.

## Memory & CLAUDE.md discipline

- Auto-memory index: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- No session narratives in CLAUDE.md. Dated logs → commit messages or memory files.
- Root CLAUDE.md = thin pointer only (enforced: `scripts/check-claude-md-pointer.sh`). This file is authoritative.
- New finding → memory file + INDEX-TOPIC.md. Before adding: `grep -i <keyword> memory/*.md` to dedupe. Superseded → update or delete.
- Keep under 100 lines. Growth → extract to `docs/` + update topic tree.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
