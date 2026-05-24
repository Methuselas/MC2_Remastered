# MC2 OpenGL — nifty-mendeleev (canonical worktree)

MechCommander 2 OpenGL port: tessellated terrain, PBR splatting, shadow maps,
post-processing. Active branch is `claude/nifty-mendeleev` (the 0.4
gpu-driven-rendering arc merged back here 2026-05-18; split collapsed). Root
checkout `terrain-pbr-mod` is older — do not work there.

This file is a **router**. Detailed content lives in topic docs under `docs/`,
memory files under `~/.claude/projects/A--Games-mc2-opengl-src/memory/`, and
the planning artifacts under `docs/superpowers/`. Keep this file under 100
lines; extract growth.

## Topic tree (read the relevant branch when starting work)

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
└── ~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md
    └── INDEX-{RENDERING,SHADERS,TERRAIN,MECH,BUILD-DEPLOY,
                MISSION-DATA,SMOKE-TEST,PROCESS}.md
                                       — read matching INDEX-TOPIC when
                                         starting domain work; individual
                                         memories linked from there
```

## Where to look first (orientation)

- **Project direction:** `.planning/PROJECT.md` (three north stars + out-of-scope)
- **Codebase maps:** `.planning/codebase/{ARCHITECTURE,STRUCTURE,STACK,INTEGRATIONS}.md` (2026-05-14; grep before quoting line numbers)
- **Advisor routing:** `.claude/agents/DOMAINS.md` (12 MC2 advisor subagents + classification + gaps)
- **Perf state:** `docs/render-perf-snapshot.md` (bucket map + slice state + deps)
- **Render contract:** `docs/render-contract.md` (design) + `mclib/render_contract.*` (impl, Phase 2 active under `MC2_RENDER_CONTRACT_ASSERT=1`)
- **Meta-prompts:** `.claude/prompts/distill-session-into-advisor-agent.md`, `.claude/prompts/dump-render-observations.md`
- **Skills:** `.claude/skills/` — `/mc2-build`, `/mc2-deploy`, `/mc2-build-deploy`, `/mc2-check`, `/mc2-shader-diff`, `/mc2-amd-shader-review`, `adversarial-plan-review`, `greybeard`
- **Steering channel:** `A:/Games/mc2-opengl-src/.claude/STEERING.md` (repo-root; `sh A:/Games/mc2-opengl-src/.claude/steer.sh "..."` blocks next Bash/Agent and injects text; agent runs `ack-steering.sh` to clear)

## Key paths (inline — every session needs these)

- Source:  `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- Build:   `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/` — the root checkout's `build64/` is STALE (terrain-pbr-mod branch); do NOT use it
- Deploy:  `A:/Games/mc2-opengl/mc2-win64-v0.4/`
- CMake:   `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Smoke gate (inline — used every session)

Default regression gate for render/init/cull/asset changes. **ALWAYS**
`--keep-logs`, NEVER `--with-menu-canary`, NEVER `--duration` >30s, NEVER
concurrent with another smoke or direct mc2.exe trace.

**Canonical invocation (copy-paste; subagents must use verbatim):**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

- `tier1` = 5 hand-picked missions (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`). 30s/mission. Isolated.
- Exit `0` = pass. Nonzero = inspect `tests/smoke/artifacts/<timestamp>/`.
- Inner-loop dev: 2-mission subset (`--missions mc2_01,mc2_24`); tier1 5/5 for slice coverage gates only.
- Visual iteration (water/shoreline/foam tuning): `--duration 60` so user has time to drive camera.
- Faster fail: add `--fail-fast`. Full policy: `memory/feedback_smoke_*.md` cluster.

## Key source files (starting points; grep to confirm current line)

- `GameOS/gameos/gameos_graphics.cpp` — renderer core (terrain draw, shadow draw, uniform caching)
- `GameOS/gameos/gos_postprocess.cpp` — FBOs, bloom, shadows, post-process
- `mclib/txmmgr.cpp` — `renderLists()` flush, master-node arrays, shadow pre-pass
- `mclib/mech3d.cpp` — engine-side mech appearance / rendering (5139 lines; engine, NOT game-side AI)
- `code/gamecam.cpp` — frame loop, render-call sequence
- `shaders/gos_terrain.frag` — terrain splatting, POM, shadow sampling, distance LOD
- `shaders/include/shadow.hglsl` — `calcShadow()` with variable-tap Poisson PCF

## Profiling

Tracy always compiled in (`TRACY_ENABLE`). GPU zones on
shadow/terrain/3D/post-process. AMD RGP works externally via Radeon Developer
Panel. **100ns floor:** never instrument a region <100ns (Tracy overhead
~20-50 ns); per-element/per-quad/per-vertex zones in hot loops are FORBIDDEN.
Coarse per-pass zones only. Origin: commit `fdc47bc` 2026-05-07.

## Model routing

- **haiku:** lookups, summaries, simple edits, renaming, formatting
- **sonnet:** standard implementation, debugging, code review. Always diff changes from haiku.
- **opus:** architecture, deep analysis, complex refactors only. Always diff changes from sonnet/haiku. Give other agents isolated context.

## Memory & CLAUDE.md discipline

- **Auto-memory index:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- **No session narratives in CLAUDE.md.** Dated logs go in commit messages or memory files.
- **Root CLAUDE.md is a thin pointer ONLY.** This worktree CLAUDE.md is authoritative. Root enforced by `scripts/check-claude-md-pointer.sh`.
- **New durable finding → memory file + INDEX-TOPIC.md entry.** Unlinked memories are invisible.
- **Superseded facts → update or delete, don't append.**
- **Before writing a new memory:** `grep -i <keyword> memory/*.md` to dedupe.
- **Keep this file under 100 lines.** If it grows, extract to a topic doc under `docs/` and update the topic tree above.
