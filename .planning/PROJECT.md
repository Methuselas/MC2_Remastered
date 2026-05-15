# MechCommander 2 / MC3 OpenGL Engine

The open-source MechCommander 2 engine, originally released by Microsoft and now being modernized to OpenGL 4.3 Core. Modernization preserves gameplay feel while incrementally rewriting engine internals. The codebase is framed as an open RTS engine with MC3 as the flagship content pack.

This file captures **direction**. Status and progress live in commit messages, memory files, per-milestone docs, and `docs/render-perf-snapshot.md` - not here.

## The three north stars

Directional commitments. Every modernization slice should ladder up to at least one.

### 1. Minimize CPU time per frame via GPU offload

The engine started CPU-bound. Frame-budget reduction is the central performance goal.

- **Measurable in:** Tracy CPU-frame budget at wolfman zoom. Current baseline and target tracked in `docs/render-perf-snapshot.md`. As of 2026-05-08: ~9.5ms wolfman; near-term target ~4.5ms per the orchestrator playbook.
- **Direction:** at every CPU-vs-GPU fork, choose GPU. Methodology in `.claude/agents/mc2-cpu-gpu-offload-expert.md`. The shipped Track A/B/C/D slices are reference precedents for the recon -> design -> stage -> soak -> default-on flip pattern.
- **Adversarial review required** for any new "all callers must X" contract introduced by a slice. See `.claude/skills/adversarial-plan-review.md`.

### 2. Fully exploit modern OpenGL features

The engine targets GL 4.3 Core today. Modernization continues toward SSBO/compute/indirect-draw breadth, then bindless when AMD support stabilizes, then mesh shaders as the technology matures.

- **Measurable in:** retired-legacy-paths count, SSBO/compute/indirect-draw usage breadth, bindless adoption rate.
- **Direction:** keep up with the GL spec; retire legacy paths rather than letting them coexist forever. The canonical legacy debt is documented in `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md` - both `masterVertexNodes` (legacy `gos_VERTEX` stream) and `masterHardwareVertexNodes` (modern `TG_RenderShape` queue) coexist today; the legacy queue should retire.
- **Constraint:** never break stock-install playability. Modern paths MUST degrade to stock-compatible generation, never fail. Full rationale: `memory/stock_install_must_remain_playable.md`. This is the architectural ceiling on modernization.
- **Forward-compat (Vulkan-prep):** new GPU-resource code uses explicit device-mediated binding (`device.bindVertexBuffer(vb)`, not `vb.bind()`) and assumes zero implicit cross-call GL state. Protect the already-Vulkan-friendly patterns (deferred enqueue/flush, std430 lockstep, `gl_ClipControl` [0,1] depth) from regression. PREP, not a port - no full RHI ahead of need. Full rule set: `memory/vulkan_prep_explicit_device_discipline.md`.

### 3. Engine/data separation - everything as API

The long-term endpoint is a stable engine layer that compiles standalone, with game data and game logic consuming it via API. MC2 content remains the reference content pack; MC3 is the flagship modernization target; future content packs become possible.

- **Measurable in:** count of `mclib/` symbols whose call-sites cross into `code/` (should trend down); engine/game boundary cleanliness.
- **Direction:** factor out the engine substrate; treat MC3 as flagship CONTENT on top of an open RTS engine. Memory: `open_rts_engine_framing.md`.
- **Why this matters:** unblocks modding (clean engine API), eventual reuse for other RTS content packs, disentangles engine modernization (mature, GPU-driven) from content work (gameplay, missions, narrative).

## Operating principle: named contracts at intersections

The three north stars converge on a single operating principle: every intersection between subsystems must become a NAMED contract, not an implicit reach-through. Implicit intersections are where bugs live; named ones are versionable, testable, auditable by grep, and predictably-failing.

The principle drives investment in artifacts. Each named-intersection (the pipeline matrix's cells, DOMAINS.md's routing entries, render-contract's submission-space rules) removes a class of context-burn that sessions would otherwise pay reconstructing structure from scratch. The `[SUBSYS v1]` log-banner pattern (e.g. `[STATIC_PROP_REGISTRY v1]`, `[INSTR v1]`, `[GPU_CULL v1]`) is already half-discipline: a `v1` is an implicit version-pin of a contract surface. Future bumps become clean migration boundaries.

This principle MAKES the structure axis (north star 3) achievable. Declaring "we want the engine separated from content" forever doesn't separate it; the intersections become named one at a time, each naming is a slice of separation work.

**Project framing triad:**
- `memory/mc3_modernization_philosophy.md` (SPEED axis) - GPU/modern by default
- `memory/open_rts_engine_framing.md` (STRUCTURE axis) - engine/content layer separation
- `memory/named_contracts_at_intersections.md` (INTERFACE axis) - every intersection becomes a named contract; the mechanism by which structure becomes real

Read together they shape every architectural decision. The canonical example: the black-tree-bug (resolved 2026-05-05) lived in the unnamed gap between `update()` and `render()`; once the producer/consumer contract was named (the `cachedFrame_` stamp), the bug couldn't recur.

## Architectural endpoint

The three north stars converge on one direction:

> A modern, GPU-driven RTS engine - separated from its data layer, with stock MechCommander 2 content remaining fully playable as the reference content pack and MC3 as the flagship modernization target.

Intermediate milestones to flesh out as we ship them (no time projections per CLAUDE.md - described in code dimensions only):

- Dual-queue retirement complete (only `masterHardwareVertexNodes` remains; legacy `masterVertexNodes` deleted along with its callers)
- Wolfman CPU budget steady-state target hit (current target ~4.5ms tracked in render-perf-snapshot.md)
- Engine layer compiles independent of `code/` game-logic layer (engine compiles standalone with clear API surface)
- First non-MC2 content pack proof of concept (validates the engine/content split)

## Out of scope (explicit non-goals)

- **Networking / multiplayer transport.** The MP UI screens in `code/mp*.cpp` are stubs; no transport code exists. Re-implementing networking is not currently planned. Audit finding 2026-05-14; documented in `.planning/codebase/INTEGRATIONS.md`.
- **Re-implementing legacy content as new content.** Stock MC2 content is preserved as-is. Modernization is engine-side; new content is content-side work, not engine work.
- **Wall-clock time projections.** Per CLAUDE.md, all scope is described in code dimensions (subsystems touched, files modified, parity gates, soak windows) - not hours/days/weeks.

## Where this fits

- **Fleet routing:** `.claude/agents/DOMAINS.md` - canonical reference for the 12 MC2 advisor subagents + classification + gaps
- **Code map:** `.planning/codebase/` (ARCHITECTURE, STRUCTURE, STACK, INTEGRATIONS) - 2026-05-14 snapshot
- **Performance state:** `docs/render-perf-snapshot.md` - bucket map, in-flight slice state, dependency graph (refresh when slices ship)
- **Render contract:** `docs/render-contract.md` (design) + `mclib/render_contract.*` (implementation) + `shaders/include/render_contract.hglsl` (GLSL side) + `scripts/check-render-contract-gbuffer1.sh` (invariant check) + `.claude/agents/mc2-render-contract-synthesizer.md` (refresh agent)
- **Rules and conventions:** `CLAUDE.md`
- **Memory index:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
- **Maintenance hooks:** `.claude/maintenance-rules.json` (consumed by `~/.claude/hooks/gsd-staleness-monitor.js` Stop hook)

## Update cadence

Refresh this doc when:
- A north star's measurement target shifts (then also update `docs/render-perf-snapshot.md` if applicable)
- An architectural endpoint milestone is achieved (then document in the relevant memory file or the matching `.planning/milestones/` doc if that exists)
- An out-of-scope item becomes in-scope (then create the advisor / memory entries that the new scope needs)
- A new top-level direction emerges (rare - this should be the most stable doc in the project)

The maintenance hook (`.claude/maintenance-rules.json` -> `gsd-staleness-monitor.js`) surfaces a reminder when this file is touched, prompting the related-document updates.
