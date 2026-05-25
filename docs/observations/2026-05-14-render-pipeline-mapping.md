# Render observations - 2026-05-14 - pipeline-mapping

**Source:** Session-end handoff. This session built the MC2 advisor fleet (12 subagents + DOMAINS.md routing), wrote the canonical render-pipeline matrix (`docs/render-pipeline-matrix.md`), split mc2-render-perf-expert into methodology advisor + snapshot doc, audited `docs/render-contract.md` against current code, generalized the project-framing memory triad (speed/structure/interface axes), and wired the staleness Stop hook. Three findings surfaced from the matrix's grep-verification pass that affect rendering specifically.

**Scope:** The full pipeline as seen from `MC_TextureManager::renderLists`, both queues (legacy `masterVertexNodes` + modern `masterHardwareVertexNodes`), the substrate / Track A/B/C/D shipped slice landscape, and the GPU-direct fast-path ecosystem. Did NOT touch render code; the work was structural / documentation. Grep verification of render-cluster anchors via the matrix subagent.

---

## Confirmed facts about current code

These were grep-verified during the pipeline-matrix build on 2026-05-14:

- `gpu_cull::substrate_frameBegin()` is called at `code/objmgr.cpp:1933` INSIDE `GameObjectManager::update()`. Context lines 1920-1942 confirm the call sits after a cull-loop inside `update()`. The comment at 1932 ("Called unconditionally") refers to the call site within `update()`, not to whether `update()` itself runs every frame.
- `mcTextureManager->update()` is at `code/mission.cpp:527` (not `:509` as cited in CLAUDE.md "Critical inline rules" and `memory/pause_unpause_diagnostic_for_static_render_bugs.md`). Symbol stable; line drifted ~+18.
- `shaders/` contains four `.comp` files: `gos_terrain_lighting.comp`, `gpu_cull.comp`, `gpu_cull_block_rollup.comp`, `gpu_cull_patch.comp`. The file `gpu_driven_terrain_solid.comp` does NOT exist on disk despite being referenced in `mc2-terrain-indirect-expert`'s `<file_locations>` block.
- `MC_TextureManager::renderLists` (mclib/txmmgr.cpp - grep `renderLists` for current line; was ~900-1000 on 2026-05-14) walks BOTH `masterVertexNodes` (legacy flat `gos_VERTEX` stream for terrain/water/craters) AND `masterHardwareVertexNodes` (modern `TG_RenderShape` for buildings/mechs/static-props). Both flush every frame; legacy never retired.
- The render-call sequence in `code/gamecam.cpp` (around line 215) is `land->render()` -> `craterManager->render()` -> `ObjectManager->render()` -> `land->renderWater()` -> `mcTextureManager->renderLists()`. First, second, and fourth enqueue. Third draws immediately (MLR exception). Fifth flushes the queue.

---

## New observations not currently in MEMORY.md or render-contract.md

- **The substrate_frameBegin pause-bug is the SECOND known instance of the pause-pattern family.** The first instance (texture-cache eviction during pause) is captured in `pause_unpause_diagnostic_for_static_render_bugs.md`. The substrate_frameBegin instance was fixed on `claude/gpu-driven-rendering` (commit `f8d6b17`) but the fix is NOT yet on nifty-mendeleev (await merge). The pattern family is now provably broader than "static-render bugs": *any per-frame mutation inside `ObjectManager::update()` that is consumed by `render()` is pause-broken*. Where this should live: generalize the existing memory file's title and intro to claim the broader family; cite both instances as case studies. (User noted this in their other-session insight 2026-05-13.)

- **The 4-stage pipeline framing (vertex projection -> enqueue -> upload -> GPU execution) is the right abstraction for organizing render knowledge.** Multiple sessions have re-derived this from scratch. The new `docs/render-pipeline-matrix.md` materializes the framing as a thing x stage map. Where this should live: the matrix doc IS the home. The `render-contract.md` design contract should reference it as the operational view of who-touches-what.

- **MLR is the immediate-draw exception** and future sessions will keep mistaking it as following the deferred-flush convention. Specifically: `ObjectManager::render` for MLR-appearance objects DOES draw immediately at the call site (no enqueue, no `renderLists` flush needed). This is documented in `memory/render_functions_are_enqueuers_not_submitters.md` but not in `render-contract.md`. Where this should live: render-contract.md should add an "Exceptions" section that names MLR explicitly.

- **The `gpu_driven_terrain_solid.comp` reference in `mc2-terrain-indirect-expert` is stale on disk.** Either the shader was renamed (most likely candidate: split into `gos_terrain_lighting.comp` + others), removed (would need git history), or never landed under that name. The advisor's other file_locations entries (e.g. `gos_terrain_indirect.cpp`) DO exist. Where this should live: investigate via `git log -- shaders/gpu_driven_terrain_solid.comp` AND `git log -S gpu_driven_terrain_solid`; correct the advisor.

- **The render-contract design doc and the render_contract.{cpp,h,hglsl} implementation registry are two unrelated artifacts sharing the name "render contract."** Neither references the other. Future sessions trying to read about "the render contract" hit ambiguity in <30 seconds. Surfaced in the audit (`docs/render-contract-audit-2026-05-14.md`). Where this should live: a user decision (merge them, cross-reference them, or rename one). Synthesizer will FLAG this rather than resolve.

- **The render contract's "Priority 1" (terrain.cpp using projectZ as visibility producer) is stale.** Grep returns zero `projectZ` references in `terrain.cpp` on nifty-mendeleev; the call-site has been contained to `quad.cpp` per the projectz-containment design. The contract's flagship violation has substantially moved or been resolved. Where this should live: render-contract.md Priority 1 should be marked CONTAINED or REMOVED (synthesizer's first run will propose this).

- **The Bucket A/B/C/D (contract) vs Track A/B/C (shipped slices) vocabulary collision is a real source of confusion.** Buckets describe submission spaces; Tracks describe modernization slices. Different axes, same letters. Where this should live: render-contract.md needs a vocab-disambiguation section (or rename one set of letters).

- **The substrate-coalesce path's two-bind-two-multidraw addressing model is the canonical v2.** v1's "SSBO addressing problem" (`r.instanceByteOffset` per-frame compacted vs `cmds[t].baseInstance` mission-static capacity-based) is structurally incompatible. Per the 2026-05-08 redesign prompt. Where this should live: noted in render-perf-snapshot's pattern-families section but should also live in `mc2-cpu-gpu-offload-expert` as a v1->v2 case study of the readback-sync-stall lesson.

---

## Contradictions found

- **render-contract.md D3 (post-process shadow as bridge-to-retire) vs implementation registry.** The design doc treats post-process shadow as a bridge that should retire; `mclib/render_contract.{cpp,h}` codifies screen-space shadow as the chosen path for non-terrain geometry. Direct disagreement. Synthesizer FLAGs; user resolves. Origin: advisor-landscape audit + render-contract audit, both 2026-05-14.

- **CLAUDE.md cites `mission.cpp:509` for mcTextureManager->update; actual line is `:527`.** Same drift in `memory/pause_unpause_diagnostic_for_static_render_bugs.md`. Both cite the same wrong line. Suggested resolution: update both citations; per the Rule 0 discipline this should have been caught earlier on any session that grep-verified.

- **`mc2-terrain-indirect-expert.md` cites `gpu_driven_terrain_solid.comp`; file does not exist on disk.** See "New observations" above.

- **The advisor-landscape audit found `mc2-render-expert` did not list `mc2-terrain-indirect-expert` in cross-references.** Sharpening edit applied this session (commit `ad014fd`); the advisor now correctly defers cement/runway/decal questions to terrain-indirect-expert. Recording this as a resolved contradiction for future synthesizer runs to skip.

---

## Open questions

- **Should the pause-pattern memory file generalize its scope and title?** User has the insight that the family is broader than "static-render bugs." Mechanical: edit the existing memory file's frontmatter description + body. Operational: also rename the file (e.g., `pause_gated_update_consumed_by_render_pattern.md`) - but renaming breaks any code that grep'd for the old name. Recommendation: keep filename, broaden description + body, add a "Family members" section listing both instances.

- **Should `mc2-terrain-indirect-expert` be updated now that `gpu_driven_terrain_solid.comp` is confirmed missing?** Depends on whether the file was renamed (correct the reference) or removed (decide whether to retain a "historical" note). Needs git history check before edit.

- **The render-contract design doc names "Priority 1, 2, 3" and "D1, D2, D3" with specific meanings.** When the synthesizer's first run lands, does it preserve those numberings as-is even if the content under them shifts? Or renumber? Recommendation: preserve numbers; let the synthesizer's CLARIFY action add annotations rather than renumber.

- **Is the dump-prompt's "Suggested render-contract edits" section meant to be consumed directly by the synthesizer, or filtered through user review first?** The synthesizer's <work_protocol> says it consumes notes files in `docs/observations/`. Recommendation: synthesizer consumes them directly. The user can review the synthesizer's changelog after the fact (not before, or the workflow stalls).

- **What's the right cadence for the pipeline matrix refresh?** When fast paths ship, the matrix's "Status" column drifts. The render-perf-snapshot has a "Last refreshed" header; the matrix doesn't. Recommendation: add a "Last refreshed" header to the matrix on next touch; aim for a quarterly refresh + ad-hoc when major slices ship.

---

## Suggested render-contract edits

For the synthesizer's first run. Treat this as the primary corpus (joined with `docs/render-contract-audit-2026-05-14.md`, which contains 5 STALE + 10 MISSING + 5 AMBIGUOUS findings):

- **Priority 1: mark CONTAINED.** Terrain projectZ visibility producer is contained to `quad.cpp`. Source: this notes file's Confirmed-facts grep + audit.

- **ADD: Dual-queue architectural debt section.** Both `masterVertexNodes` (legacy) and `masterHardwareVertexNodes` (modern) coexist in `MC_TextureManager::renderLists`. Every modernization slice has been additive; legacy retirement is the next-needed structural move. Source: `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`.

- **ADD: "Render functions are enqueuers, not submitters" foundational rule.** XXX::render() pushes into master arrays; actual GL submission happens in `gos_RendererEndFrame -> mcTextureManager->renderLists()`. Source: `memory/render_functions_are_enqueuers_not_submitters.md`.

- **ADD: Post-renderLists hook rule for GPU-direct fast paths.** Any GPU-direct renderer that bypasses mcTextureManager must hook AFTER `renderLists()` flushes legacy enqueues. Source: `memory/render_order_post_renderlists_hook.md`.

- **ADD: MLR immediate-draw exception.** `ObjectManager::render` for MLR-appearance objects draws immediately at the call site, not through the deferred queue. Source: foundational-rule memory + audit gap.

- **ADD: Vocabulary disambiguation section.** Bucket A/B/C/D (submission spaces, contract level) vs Track A/B/C (modernization slices, ship level). Source: audit gap.

- **FLAG (user resolve): merge or cross-reference the design contract and implementation registry.** Source: audit finding #1.

- **FLAG (user resolve): D3 disagreement between high-level doc (bridge-to-retire) and implementation registry (chosen path).** Source: audit + advisor-landscape audit.

- **CLARIFY: `[SUBSYS v1]` log-banner pattern is half-discipline.** The `vN` is a contract version-pin already. Future bumps to v2 become a migration boundary. Source: `memory/named_contracts_at_intersections.md`. Where in contract: under the operating-principle framing.

- **ADD: Reference to `docs/render-pipeline-matrix.md` as the operational view of submission-space ownership.** The contract describes rules; the matrix shows current state. Source: new artifact this session.

---

## Methodology notes

This session did NOT do deep render-code work. The pipeline-matrix subagent did the grep heavy-lifting; the session's own context spent on structural artifacts (advisor fleet, planning docs, audit reports). Three findings emerged from the matrix subagent's grep-verify discipline on first run:

1. substrate_frameBegin pause-bug live on this branch
2. gpu_driven_terrain_solid.comp non-existence
3. mcTextureManager->update line drift

All three were structural facts about the codebase that were drifting silently; the matrix subagent caught them because its work_protocol forces grep-verification per cell. This is exactly the "named contracts at intersections" principle materialized as a documentation discipline. Future render-cluster sessions should treat the matrix's grep-discipline as the canonical "I'm not allowed to lie about file:line" enforcement.

The render-contract synthesizer agent (`mc2-render-contract-synthesizer`) has not been invoked yet. The audit doc (`docs/render-contract-audit-2026-05-14.md`) plus this notes file together form the seed corpus for the synthesizer's first run. Whenever that runs, the synthesizer should consume both files and produce the first refresh of `docs/render-contract.md` plus a `docs/render-contract-changelog-YYYY-MM-DD.md`.

No render code was modified this session; all changes are in `docs/`, `.claude/agents/`, `.claude/prompts/`, `.claude/skills/`, `.planning/`, and CLAUDE.md. The three findings above are documented but UNTOUCHED in code per the user's branch-merge discipline (gpu-driven-rendering + mech-skinning-import branches are pending merge into nifty-mendeleev).
