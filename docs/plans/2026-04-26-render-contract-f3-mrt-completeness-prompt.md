# Prompt: Render Contract F3 — Close the undefined-MRT-output gap

Brainstorm and write the design spec for **F3** — closing the undefined-MRT-output gap surfaced by the just-landed Render Contract Registry phase 1.

## Working directory

`A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`. Branch: `claude/nifty-mendeleev` (HEAD `5256659` after the registry phase-1 closing report). Build: RelWithDebInfo. Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

## What to do

This is a brainstorming-then-spec task. **Use the `superpowers:brainstorming` skill first** — explore intent, requirements, and design before writing the spec. Then write a design spec at `docs/superpowers/specs/YYYY-MM-DD-render-contract-f3-mrt-completeness-design.md` following the pattern of the registry spec (`2026-04-26-render-contract-registry-design.md`).

## Why this exists

The Render Contract Registry phase 1 (commits `98d3b4f` → `5256659`) named the implicit GBuffer1.alpha post-shadow mask contract and routed every producer/reader through typed helpers. During that work, commit 7's audit produced a load-bearing escalation:

> **`enableMRT()` and `disableMRT()` in `gos_postprocess.cpp` are defined but NEVER CALLED from production code.** `beginScene()` (`gos_postprocess.cpp:518-525`) binds the MRT FBO with `glDrawBuffers(2, {COLOR0, COLOR1})` and the only subsequent `glDrawBuffers(1, ...)` calls are inside the post-process passes themselves (after the scene is fully rendered). The intent comment at line 519-520 — *"Start with single draw buffer — MRT only during terrain rendering (AMD RX 7900 corrupts color output if non-terrain shaders write location=1)"* — describes a behavior the implementation does not actually exhibit.

That elevates §3.2 of the registry spec from "latent risk" to **active production driver-dependency**. Five fragment shaders are confirmed to be drawn while the MRT FBO is bound but do not declare `layout(location=1) out vec4 GBuffer1`:

1. `shaders/gos_vertex.frag`
2. `shaders/gos_vertex_lighted.frag`
3. `shaders/gos_tex_vertex.frag` (the IS_OVERLAY bridge — Bucket D2 in `docs/render-contract.md`)
4. `shaders/gos_tex_vertex_lighted.frag`
5. `shaders/object_tex.frag`

(One additional shader, `gos_text.frag`, is "likely MRT-bound" but timing-dependent — needs further audit.)

For pixels these shaders rasterize, attachment-1 alpha is **driver-dependent**: typically the previous frame's value, or whatever the implementation chose to clear/preserve. `shaders/shadow_screen.frag:116` reads attachment-1 alpha via `rc_pixelHandlesOwnShadow` (the canonical threshold helper added by registry commit `df1c463`) to decide whether to apply post-process shadow. The current behavior on the developer's AMD/Windows configuration is *acceptable* but not *defined*. A driver update, GPU swap, or clear-policy change could flip shadow application on those pixels silently.

The intent comment makes the original design obvious: AMD specifically corrupts color output when non-terrain shaders write `location=1` (this is documented in `docs/amd-driver-rules.md`-style lore), so MRT was supposed to be enabled only for the terrain pass. The implementation that was supposed to enforce that intent — `enableMRT()` / `disableMRT()` — exists as helper functions but is never wired into the draw sequence.

The registry's promise — that a caller can reason about the contract — is undermined as long as five production shaders write driver-dependent values to a slot the post-process pass reads.

## What the spec needs to cover

At minimum (the brainstorm should refine):

1. **Confirm the commit-7 finding via fresh audit.** The closing report claims `enableMRT`/`disableMRT` are uncalled. Verify by grep, and trace one concrete frame's draw sequence in `gameos_graphics.cpp` and `gos_postprocess.cpp` to map which shaders run at what point relative to the MRT bind/unbind.

2. **Confirm or refute the AMD corruption claim.** The original intent comment names a specific failure mode. Is there a memory entry or commit history documenting the actual AMD corruption that motivated the comment? If yes, the chosen solution must respect it. If the comment is stale, the constraint may be looser than it appears.

3. **Decide between three resolutions.** The registry spec listed three viable approaches (in increasing intrusiveness):

   - **(a) Declare `GBuffer1` in every MRT-bound shader and write the appropriate `rc_*` helper** (likely `rc_gbuffer1_screenShadowEligible` for non-terrain content, treating those pixels as receiving post-process shadow). Most explicit; touches the most files. Plays well with the registry vocabulary.
   - **(b) Route undefined shaders through a non-MRT pass.** Bind a single-attachment FBO, draw, rebind MRT. Most invasive structurally; potentially highest performance cost (FBO rebinds).
   - **(c) Toggle `glDrawBuffers` per-draw.** Wire `enableMRT()` / `disableMRT()` into the draw sequence so they actually fire — call `disableMRT()` before object/text draws, `enableMRT()` before terrain. Lowest source touch but introduces a state-machine invariant the codebase has historically been bad at maintaining (see registry spec §3.5 / RAlt+P overlay bug).

   The brainstorm should weigh these against the registry's lessons:
   - Phase-1 inert constraint: option (a) is the only one that's behavior-preserving on its first commit. Options (b) and (c) change the FBO state machine and may shift pixel output on shaders that currently get whatever attachment-1 alpha happens to be present.
   - AMD constraint: option (a) writes `location=1` from non-terrain shaders. **If the AMD corruption claim is real and current, option (a) is dangerous and (b) or (c) is required.** Verifying this is gating.

4. **Decide what value MRT-incomplete shaders should write.** Today they're (probably) producing alpha=0 by accident, which `shadow_screen.frag` interprets as "apply post-shadow." For object passes (mechs, vehicles, buildings), is that the *right* behavior? The registry says `OpaqueObject`/`AlphaObject` should be `skipsPostScreenShadow=false` — i.e., yes, post-shadow should darken them. So writing `rc_gbuffer1_screenShadowEligible(N)` would preserve current accidental behavior with explicit semantic. But: the IS_OVERLAY bridge uses `gos_tex_vertex.frag` for things that conceptually are terrain-overlay-like. Those should opt out. The decision is per-shader, not blanket.

5. **The `gos_text.frag` ambiguity.** HUD text may be drawn after the post-shadow pass has already run, in which case its attachment-1 writes don't matter. Resolve this with a fresh audit and document.

6. **The `markTerrainDrawn()` contract.** `gos_postprocess.cpp:516` resets `sceneHasTerrain_` per frame; `gameos_graphics.cpp:2995` calls `markTerrainDrawn()` after the indexed-patches terrain draw. This is the existing signal that "terrain has been drawn this frame." If option (c) is chosen, the natural place to call `disableMRT()` is right after `markTerrainDrawn()` (or right after the last terrain-pass draw, including overlays/decals that opt into shadow-handled). Worth documenting either way.

## Constraints (load-bearing, do not violate)

- **Phase-1 inert if option (a) is chosen.** The first commits should be byte-equivalent to current rendered output. Behavior (e.g., changing whether mech pixels receive post-shadow) lands in a follow-up.
- **The 8 projectZ wrappers in `mclib/camera.h` are load-bearing.** Don't rename/repurpose.
- **The Render Contract Registry helpers are load-bearing.** Any new shader writes to `GBuffer1` must use the existing `rc_*` API (or extend it via a follow-up to the registry spec, not as part of F3).
- **The grep census in `scripts/check-render-contract-gbuffer1.sh` must continue to pass.** If F3 adds new shaders writing GBuffer1, those writes go through `rc_*`. If F3 changes the FBO state machine, the census still passes (it only checks shader writes, not FBO config).
- **Smoke-tier1 5/5 PASS gate** between commits.
- **Worktree CLAUDE.md rules apply:** RelWithDebInfo build, never `cp -r` on deploy, deploy to `A:/Games/mc2-opengl/mc2-win64-v0.2/`.
- **AMD driver rules apply** (`docs/amd-driver-rules.md`) — sampler2DArray crash, attribute 0 active, gl_FragDepth explicit, no texture feedback loops, deferred-vs-direct uniform discipline.
- **Pre-existing user lesson:** "Stale shader cache mimics shader regression" (auto-memory `stale_shader_cache_symptom.md`). After deploy, expect transient regression-looking symptoms after multi-mission sessions. A/B isolate before reverting.

## Reference

- Render contract registry spec: [`docs/superpowers/specs/2026-04-26-render-contract-registry-design.md`](../superpowers/specs/2026-04-26-render-contract-registry-design.md)
- Registry closing report (with operator A/B confirmation): [`docs/superpowers/specs/render-contract-registry-report.md`](../superpowers/specs/render-contract-registry-report.md)
- Callsite inventory (with §3.2 confirmation status): [`docs/superpowers/specs/render-contract-callsite-inventory.md`](../superpowers/specs/render-contract-callsite-inventory.md)
- Existing render-contract doc (submission-space; orthogonal): [`docs/render-contract.md`](../render-contract.md)
- AMD driver rules: [`docs/amd-driver-rules.md`](../amd-driver-rules.md)
- Architecture overview: [`docs/architecture.md`](../architecture.md)
- C++ registry types: [`mclib/render_contract.h`](../../mclib/render_contract.h), [`mclib/render_contract.cpp`](../../mclib/render_contract.cpp)
- GLSL helpers: [`shaders/include/render_contract.hglsl`](../../shaders/include/render_contract.hglsl)
- Grep census: [`scripts/check-render-contract-gbuffer1.sh`](../../scripts/check-render-contract-gbuffer1.sh)
- FBO state machine (the surface F3 changes): [`GameOS/gameos/gos_postprocess.cpp`](../../GameOS/gameos/gos_postprocess.cpp) — `beginScene` (line 510), `enableMRT`/`disableMRT` (583-595), `runScreenShadow` (598-)
- The 5 confirmed-MRT-incomplete shaders:
  - [`shaders/gos_vertex.frag`](../../shaders/gos_vertex.frag)
  - [`shaders/gos_vertex_lighted.frag`](../../shaders/gos_vertex_lighted.frag)
  - [`shaders/gos_tex_vertex.frag`](../../shaders/gos_tex_vertex.frag)
  - [`shaders/gos_tex_vertex_lighted.frag`](../../shaders/gos_tex_vertex_lighted.frag)
  - [`shaders/object_tex.frag`](../../shaders/object_tex.frag)
- The reader: [`shaders/shadow_screen.frag:129`](../../shaders/shadow_screen.frag) (`rc_pixelHandlesOwnShadow`)
- The marker call: [`GameOS/gameos/gameos_graphics.cpp:2995`](../../GameOS/gameos/gameos_graphics.cpp) (`markTerrainDrawn`)
- Memory pointers (in `~/.claude/projects/A--Games-mc2-opengl-src/memory/`):
  - `stale_shader_cache_symptom.md` — A/B before reverting
  - `cull_gates_are_load_bearing.md` — discipline pattern for load-bearing implicit gates
  - `shadow_caster_eligibility_gate.md` — the 743efd6 misdiagnosis lesson (don't blanket-add gates without per-asset audit)
  - `debug_instrumentation_rule.md` — env-gated `MC2_*` instrumentation conventions

## Out of scope for this spec

- **The Render Contract Registry follow-ups F1, F2, F4–F9.** Each is its own spec. F3 is structural cleanup of the FBO/shader-write gap; it doesn't resolve the §3.1 water/shoreline material-alpha overload (F1), the legacy "terrain flag" terminology cleanup (F2), shadow-eligibility centralization (F4), debug-overlay enforcement (F5), or any of the renderer modernization directions (F6–F8).
- **`ModernTerrainSurface` (advisor's B), overlay/decal unification (advisor's C), native-modern sidecars (advisor's D).** Each is its own spec, sequenced after F3 lands.
- **Behavior changes to the post-shadow pass itself.** F3 closes the producer-side gap; the consumer (`shadow_screen.frag`) keeps its current threshold and logic.
- **Performance optimization of MRT bind/unbind.** If option (b) or (c) introduces FBO-rebind cost, document it; don't optimize in the same spec.

## Deliverables

1. A brainstorm summary covering: which option (a/b/c) is chosen, why, AMD constraint verification, and per-shader contract decisions.
2. A design spec at `docs/superpowers/specs/YYYY-MM-DD-render-contract-f3-mrt-completeness-design.md` covering: thesis, audit (confirm enableMRT/disableMRT uncalled, confirm AMD constraint), per-shader contract assignments, chosen mechanism with rationale, frozen surfaces, commit sequence, exit criteria (including grep census still passes), open questions, follow-up tickets, references.
3. **Optionally** an implementation plan + first commit if the spec is unambiguous and the user signs off on it. Default: spec-only and pause for review (matches the registry pattern).
