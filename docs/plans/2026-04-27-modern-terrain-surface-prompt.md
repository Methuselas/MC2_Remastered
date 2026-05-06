# Prompt: ModernTerrainSurface / Terrain Patch Submission — Initial Exploration

Explore the surface area for **ModernTerrainSurface / Terrain Patch Submission** (advisor's "B" / registry follow-up F7). **This is a research-and-mapping task, not a spec-writing task yet.** The output is a findings document that informs a future brainstorming → spec → plan sequence.

## Working directory

`A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`. Branch: `claude/nifty-mendeleev` (HEAD `e0b34d8` after the post-F3 cleanup). Build: RelWithDebInfo. Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

## What to do

Spawn a **fresh agent** (no inherited context from the F3 thread) and have them produce a **codebase-mapping findings doc** at `docs/superpowers/explorations/YYYY-MM-DD-modern-terrain-surface-findings.md`. Do **not** invoke `superpowers:brainstorming` or `superpowers:writing-plans` for this task — those skills produce specs/plans, and we are not ready for a spec yet. Use `superpowers:systematic-debugging` only if the agent needs to bisect a perf claim; otherwise plain Read/Grep/Glob exploration.

The agent should produce a structured findings document that another (future) agent will use as input to the design-brainstorm phase. Think of this as the analog of the `render-contract-callsite-inventory.md` step that preceded the registry spec — the goal is to surface *what is actually there*, not to redesign it yet.

## Why this exists now

Three forces converge:

1. **CPU cost is the ceiling.** Per worktree memory (`perf_profiling_results.md`, `MEMORY.md`): MC2 is 100% CPU-bound at Wolfman zoom and meaningfully CPU-bound at standard zoom. `Camera.UpdateRenderers` is 6 ms/frame, with 3.66 ms self-time spent in MC2 terrain vertex building. The note explicitly says "Not fixable without engine rewrite." Shader-side work has effectively zero remaining headroom for visible perf gains; the next leverage point is the terrain submission pipeline itself.

2. **F3 just unlocked the ground.** The render-contract registry, projectZ admission seam, and visible-pixel `GBuffer1` coherence are all in place. ModernTerrainSurface lands into a renderer with explicit pass identity, typed `rc_*` helpers, and a refuted AMD MRT claim — none of which existed during the previous failed cleanup attempts (the projectZ saga, the 743efd6 misdiagnosis, the cement-overlay rewrite that bounced). Doing this *before* F3 would have meant fighting the implicit-overload problem; doing it *now* lets the new abstraction inherit the registry's vocabulary.

3. **Every prior renderer cleanup that didn't establish a seam first regressed.** `docs/plans/cement-overlay-rewrite.md`, `docs/plans/2026-04-15-world-space-overlay-impl.md`, the registry spec's "name what is, before changing what should be" framing — they all converge on the same lesson. ModernTerrainSurface is the next-largest implicit-overload after the registry, and it is the one that gates real perf work.

## What the findings doc needs to cover

These are the questions the exploration must answer. The agent has full latitude on structure, but every numbered question below should be answered explicitly with file:line references.

### 1. Map the legacy MC2 terrain admission pipeline end-to-end

For one frame at Wolfman zoom (the worst case) and one frame at standard zoom, trace the path from "MC2 mission/heightmap data" → "terrain pixels rasterized."

- Where does the heightmap actually live in memory at runtime? File:line for the data structure.
- Who decides which patches/tiles to draw this frame? (`Environment.UpdateRenderers` body — what does it actually do?)
- How does a terrain vertex get from MC2 elevation data into the GPU? Identify the CPU-side build step (the 3.66 ms self-time hot path per `perf_profiling_results.md`) precisely. File:line of the inner loop.
- What does the per-frame data-flow look like for vertex buffers? Are vertex buffers persistent, orphaned and re-uploaded each frame, or rebuilt from scratch?
- Where are draw calls actually dispatched? `terrainDrawIndexedPatches` at `gameos_graphics.cpp:2995` is the registry-tagged entry; trace upward and downward from there.
- How does the cull pipeline (`inView`/`canBeSeen`/`objBlockInfo` per worktree memory `cull_gates_are_load_bearing.md`) interact with terrain admission specifically? Terrain has different cull semantics than props — surface them.
- What is the relationship between MC2's tile system, MLR's vertex format, and the OpenGL submission layer?

### 2. Identify what's actually CPU-expensive

- Validate or refute the `perf_profiling_results.md` claim that vertex building dominates. The memory is 2 days old; verify against current code. If still true, instrument the inner loop with Tracy zones (do not commit; just confirm).
- Break the 3.66 ms self-time into sub-costs: heightmap interpolation, normal calculation, UV computation, color computation, MLR vertex packing, GL buffer upload. Which sub-cost is biggest?
- Are there per-frame allocations? Per-frame `memcpy`s? Per-vertex string-formatted uniform lookups (the kind that bit us in the registry session)?
- Identify any per-frame work that could be hoisted to per-mission-load (heightmap sample tables, terrain texture indices, slope/normal pre-computation).

### 3. Identify what's GPU-expensive in the terrain path

- The shader-cost memory says "GPU has massive headroom" at 11–15% util. Confirm or refute.
- If GPU is genuinely idle, the bottleneck is submission overhead, not GPU work — confirms the modernization direction is "fewer, larger draw calls" (MDI / GPU-driven culling) not "smaller shaders."
- If GPU is *not* idle on certain content (heavy POM, multi-tex blending), enumerate the cases.

### 4. Catalog every existing terrain-related GPU buffer and its lifecycle

- Static (allocated once, never updated): list them.
- Per-mission (allocated at mission load): list them.
- Per-frame (rebuilt every frame): **this is the modernization target — list them with sizes and update frequencies.**
- Streaming (LOD-driven page in/out): if any.

### 5. Identify load-bearing constraints

What can the modernization NOT break? Concrete examples:

- The cull-gate cascade (`cull_gates_are_load_bearing.md`).
- The TGL pool exhaustion silent-NULL behavior (`tgl_pool_exhaustion_is_silent.md`).
- The MC2 ARGB packing convention (`mc2_argb_packing.md`).
- The terrainMVP `GL_FALSE` upload + row-major cancellation (`terrain_mvp_gl_false.md`).
- The terrain TES projection chain (`terrain_tes_projection.md`).
- The deferred-vs-direct uniform discipline.
- The Render Contract Registry — every modern terrain producer must continue to write `GBuffer1` via `rc_gbuffer1_*` helpers; the contract is now load-bearing.
- Anything else surfaced during the audit that's quietly load-bearing.

For each, name the consequence of breaking it (what user-visible bug appears).

### 6. Identify the existing modernization beachheads

The codebase isn't a clean blackbox — there's already partial modernization. Surface it:

- `static_prop_batcher` at `GameOS/gameos/gos_static_prop_batcher.cpp` — what GPU-driven techniques does it use? Could the terrain pipeline borrow from it?
- The GPU static-prop killswitch (`g_useGpuStaticProps`, RAlt+0) and the cautions in worktree CLAUDE.md about it. What did the GPU-mode attempt get right; what did it get wrong; what can ModernTerrainSurface learn from that history?
- The terrain tessellation pipeline (`gos_terrain.tcs`/`.tes`) — is the CPU-side patch submission already partially GPU-friendly, or is it just dressing on top of the legacy per-vertex pipeline?
- Tracy zones — are there enough GPU/CPU markers to characterize the modernization target, or do we need to add some?

### 7. Identify the natural "seam" for ModernTerrainSurface

This is the load-bearing design question. Three plausible places to draw the boundary:

- **High seam (data-shaped):** `ModernTerrainSurface` accepts a heightmap + tile metadata + texture index map; everything below the API is implementation. `LegacyTerrainImport` converts MC2 mission data into that input shape at mission load.
- **Mid seam (patch-shaped):** `ModernTerrainSurface` accepts a list of "patches to draw this frame" + camera/lighting context; the CPU still does culling but draw submission is in the modern path. `LegacyTerrainImport` produces patch lists.
- **Low seam (draw-shaped):** `ModernTerrainSurface` accepts already-built vertex buffers + draw parameters; only the GL submission is modern. `LegacyTerrainImport` is thin.

Each has different perf-win potential, different blast radius, and different gating dependencies. The exploration should describe each option's evidence base from the codebase — what's already friendly to which seam, what's hostile — but should NOT recommend one yet. That's the brainstorm phase.

### 8. Surface the dependency graph

What blocks ModernTerrainSurface? What does it block?

- Does it depend on F1 (water/shoreline material-alpha overload)? (Likely yes if water flows through terrain.)
- Does it depend on F2 (legacy "terrain flag" terminology cleanup)?
- Does it depend on F8 (overlay/decal unification)? (Probably yes — overlays are part of the terrain submission family.)
- Does it block native-modern sidecars (advisor's D / F6)? (Likely yes — F6 is the post-Modern step.)
- Are there mid-flight features that would conflict (the texture upscaling pipeline, the asset_scale subsystem, the GPU grass pass, the terrain-shadow accumulation)?

Diagram or table form is fine.

### 9. Risk register

Concrete failure modes, ordered by severity:

- "Mech shadows broke" / "trees vanished" / "terrain seams reappeared" — anything where the modernization could cascade into a non-terrain visible regression.
- "Wolfman zoom now stutters worse" — perf regression at the very case we're trying to fix.
- "Breaks on AMD" — the canary refuted the location=1 claim, but the modernization will exercise new GPU code paths.
- "Breaks on a fresh install" / "breaks for someone with mods" — content-side risks.
- Mod-content compatibility (Magic, Carver5O, MCO Omnitech per worktree memory): does ModernTerrainSurface need to preserve any specific terrain-data handling for them?

For each, name the canary or regression test that would catch it.

### 10. Out-of-scope (for *this exploration only*)

- Do not write a design spec.
- Do not propose an architecture decision.
- Do not write code.
- Do not deploy.
- Do not promise a perf number.
- Do not enumerate F1's water-alpha decision tree (that's F1's job).
- Do not pre-empt the brainstorm by picking a seam from §7.

## Constraints (load-bearing for the exploration)

- **Read-only.** No source code modifications. No commits beyond the findings doc. No build/deploy.
- **Cite file:line.** Every architectural claim is grounded in current code, not memory or stale doc text. Memories from auto-memory are a starting point, not gospel — they're 2+ days old in many cases.
- **The 8 projectZ wrappers in `mclib/camera.h` are load-bearing** — note this for any seam discussion that might touch them.
- **The Render Contract Registry helpers and the F3 coherence guarantee are load-bearing** — any seam decision that proposes a new shader writing `GBuffer1` must route through `rc_*` helpers.
- **`terrainMVP` is `GL_FALSE` per `terrain_mvp_gl_false.md`** — note as a constraint for any new submission path.
- **Worktree CLAUDE.md rules apply** — RelWithDebInfo, deploy discipline (not relevant for read-only, but the agent should acknowledge), AMD driver rules.

## Reference

- F3 closing report: [`docs/superpowers/specs/render-contract-f3-report.md`](../superpowers/specs/render-contract-f3-report.md) — the just-landed coherence guarantee
- F3 pass audit: [`docs/superpowers/specs/render-contract-f3-pass-audit.md`](../superpowers/specs/render-contract-f3-pass-audit.md) — pattern for callsite enumeration
- Render Contract Registry spec: [`docs/superpowers/specs/2026-04-26-render-contract-registry-design.md`](../superpowers/specs/2026-04-26-render-contract-registry-design.md) — vocabulary
- ProjectZ callsite inventory: [`docs/superpowers/specs/projectz-callsite-inventory.md`](../superpowers/specs/projectz-callsite-inventory.md) — template for the kind of inventory to produce here
- Worktree CLAUDE.md: critical rules, debug instrumentation rule, smoke gate
- Worktree memory:
  - `MEMORY.md` index — read first, then drill into:
  - `perf_profiling_results.md` — the CPU-bound claim and the 3.66 ms self-time
  - `cull_gates_are_load_bearing.md` — discipline pattern
  - `tgl_pool_exhaustion_is_silent.md`
  - `terrain_tes_projection.md`
  - `terrain_mvp_gl_false.md`
  - `mc2_argb_packing.md`
  - `cpu_displacement_done.md` — terrain elevation sampling on CPU
  - `terrain_texture_tuning.md`
  - `static_prop_projection.md` — the existing GPU-driven beachhead's lessons
  - `gpu-static-prop-cull-lessons.md` (in `docs/`)
- Architecture overview: [`docs/architecture.md`](../architecture.md)
- Tracy profiler integration: `memory/tracy_profiler.md` — for any perf claim needing verification
- The hot path entry point to characterize: `Environment.UpdateRenderers()` invoked from `gameosmain.cpp:464`
- The terrain draw entry: `terrainDrawIndexedPatches` in `GameOS/gameos/gameos_graphics.cpp:2995`

## Deliverable

One file: `docs/superpowers/explorations/YYYY-MM-DD-modern-terrain-surface-findings.md`. Structured around the §1–§9 questions above. Self-contained — a future agent picking up the brainstorm should be able to read this doc cold and have everything they need to start the design discussion.

If the exploration uncovers something that materially changes the *premise* of ModernTerrainSurface (e.g., the perf claim is refuted, or the actual bottleneck is somewhere else entirely, or a load-bearing constraint makes one of §7's seams clearly impossible), surface it loudly at the top of the doc rather than burying it. The exploration's value is in catching premise-failures *before* a spec is written, not after.

When the findings doc is committed, ping the operator with a short summary (a few bullets max). The operator will decide whether to proceed to the brainstorm phase, request a deeper dive on a specific finding, or shelve the work.
