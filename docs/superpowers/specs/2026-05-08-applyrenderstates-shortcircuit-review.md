# Adversarial Review — `applyRenderStates` State-Equality Early-Out (RENDER_STATES v1)

- Subject: uncommitted change in `nifty-mendeleev` (mirror of branch `worktree-agent-a82a44599d71cbb3a` commit `92b5352`)
- Files reviewed: `GameOS/gameos/gameos_graphics.cpp`, `GameOS/gameos/gos_postprocess.cpp`, `GameOS/include/gameos.hpp`
- Visual canary: PASS per user (kill-switch on vs. off pixel-identical)
- Review skill: `.claude/skills/adversarial-plan-review.md`

## Verdict

**STOP THE LINE.** 1 CRITICAL, 3 MAJOR, 4 MINOR. The CRITICAL finding is a sampler-state-inheritance regression that the visual canary does not exercise on the surface where it manifests; the MAJOR findings cover three additional fast paths that currently leak GL state past the new cache without invalidating it. The cache itself is well-formed; the issue is incomplete coverage of the invalidation contract on the caller side.

## CRITICAL

### CRITICAL-1 — `gos_static_prop_batcher` indirect path mutates per-texture WRAP without invalidation

**Site:** `GameOS/gameos/gos_static_prop_batcher.cpp:1700-1701`

```text
1700:                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
1701:                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
```

These `glTexParameteri` calls are persistent state on the **texture object** itself — they survive past the save-and-restore block at `gos_static_prop_batcher.cpp:1738-1759` (which restores depth, blend, cull, program, VAO, SSBOs, but **cannot** restore per-texture wrap because there's no save).

Memory note `sampler_state_inheritance_in_fast_paths.md` is exactly this trap. Pre-RENDER_STATES v1, the next `applyRenderStates()` call that touched the same texture would re-run `setSamplerParams` (line 3056) and rewrite the wrap. With v1's early-out, if all tracked render-state slots happen to match cached values, `applyRenderStates` returns at line 2946 without re-running `setSamplerParams` — wrap stays at `REPEAT` even though `gos_State_TextureAddress=gos_TextureClamp` was requested.

The static prop batcher **does not** call `gos_InvalidateRenderStateCache()` after its draw. Grep:

```text
A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameOS\gameos\gos_static_prop_batcher.cpp
  (no occurrences of invalidateRenderStateCache or gos_InvalidateRenderStateCache)
```

Why the visual canary missed it: the affected texture must (a) be the static-prop atlas, (b) be re-bound via `gos_State_Texture` after the static-prop pass, and (c) be sampled with `gos_TextureClamp` while the rest of state matches the cached slot exactly. If any of those don't line up, the wrap latency is invisible. v0.3 deploys do not exercise this (`g_useGpuStaticProps` defaults off per worktree CLAUDE.md "Current state 2026-04-20").

**Recommendation:** add `if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();` at the end of `GpuStaticPropBatcher::flush()` (after the state-restore block, before the parity tick at line 1761). This is the same pattern the diff already applies to `gos_terrain_bridge_drawIndirect` (`gameos_graphics.cpp:2493`) and `renderWaterFastPath` (`gameos_graphics.cpp:2240`).

## MAJOR

### MAJOR-1 — `drawTerrainOverlays` mutates depth/blend/cull/program/textures without invalidation

**Site:** `GameOS/gameos/gameos_graphics.cpp:5964-6011`

The function ends at line 6010 with `glUseProgram(0)` and `glDepthFunc(GL_LESS)` (line 6003 — note: cache may have `gos_State_ZCompare=1` which maps to `GL_LEQUAL` at line 2988). It also overwrites the unit-0 texture binding inside the per-entry loop (`glBindTexture` at 5998).

After `drawTerrainOverlays()` returns, GL state is: depth-func `GL_LESS`, no program, unit-0 unknown texture. The next `applyRenderStates()` may short-circuit because `curStates_[]` and `renderStates_[]` are unchanged from the renderer's perspective. Result: GL stays at `GL_LESS` even though the cached value was `GL_LEQUAL`.

This is the same trap as `gosPostProcess::endScene` (which got an invalidation call at `gos_postprocess.cpp:982`). Verified absence of invalidation:

```text
gameos_graphics.cpp:5964..6011 — no invalidateRenderStateCache call in drawTerrainOverlays
```

**Recommendation:** add `invalidateRenderStateCache();` at end of `drawTerrainOverlays()` before line 6011, and a matching call in `drawDecals()` (see MAJOR-2).

### MAJOR-2 — `drawDecals` mutates depth-mask/blend/cull/program/textures without invalidation

**Site:** `GameOS/gameos/gameos_graphics.cpp:6017-6066`

Identical class to MAJOR-1. The function disables blend, sets `glDepthMask(GL_FALSE)` mid-flight (line 6032), then resets to `GL_TRUE` at 6061 and `glDepthFunc(GL_LESS)` at 6057, finally `glUseProgram(0)`. The cache cannot detect that depth-func was driven from cached `GL_LEQUAL` to actual `GL_LESS`.

**Recommendation:** `invalidateRenderStateCache();` before `decalBatch_.verts.clear();` (line 6064), or before the `}` closing the function.

### MAJOR-3 — `endBucketLoop` direct-bind path: invalidate-then-apply pattern only safe if the apply is guaranteed

**Site:** `GameOS/gameos/gameos_graphics.cpp:1901-1915`

The diff's invalidation at `gos_terrain_bridge_drawSingleBucket` (line 1891) lands inside the per-bucket loop body. `endBucketLoop` (line 1901) is what closes the loop and does the cache re-sync via the explicit `applyRenderStates()` at line 1911. But `endBucketLoop` early-returns at line 1907 when `lastGosHandle == 0xFFFFFFFFu` (no draws issued — bucket loop empty, but the direct-bind path could also have been entered for a single bucket and then skipped further work).

**Walk:** if `s_patchStreamDirectBind` is on AND zero buckets actually drew, neither `applyRenderStates` nor a sampler restore runs. The cache is still valid (no draws happened). Acceptable. If at least one bucket drew, line 1891 invalidated the cache, and line 1911 forces a full re-apply. Acceptable.

But: **line 1904 `glBindSampler(0, 0)` runs unconditionally before the early-return.** If `lastGosHandle == 0xFFFFFFFFu` and zero draws happened, `glBindSampler(0, 0)` still ran. That's a no-op on a clean state, but it does mean unit-0 sampler binding can change between the apply that filled the cache and the early-return. Cache is consulted next without invalidation — though sampler-binding doesn't affect any tracked render-state slot (only per-texture-object wrap/filter, which is different state). Verified safe.

This is more architectural caution than a defect: **the invalidation contract relies on every direct-bind code path either invalidating or bracketing to a guaranteed apply.** A future change that inserts a `return` between line 1891's invalidation and line 1911's apply would silently leave `stateCacheValid_=false` — which forces a full apply on the next caller, which is benign — but a future change that skips `gos_terrain_bridge_drawSingleBucket`'s invalidation while still binding state directly would silently corrupt. Document the invariant.

**Recommendation:** add a comment on `gos_terrain_bridge_drawSingleBucket`'s invalidation site stating "any direct-bind GL mutation in this file MUST end with invalidateRenderStateCache; do not delete." Same for the other four bridge invalidation sites. Treat as the new "load-bearing" invariant.

## MINOR

### MINOR-1 — Cache survives across frame boundary; not invalidated by `beginFrame`/`endFrame`

**Site:** `gameos_graphics.cpp:3095-3108` (`beginFrame`), `3112-3140` (`endFrame`)

The cache is not invalidated at frame boundaries. This is OK because the engine doesn't change GL state between `gos_RendererEndFrame` and `gos_RendererBeginFrame` in any way that would diverge from the cached snapshot — `endFrame` only runs counters and the (now dev-gated) hot-reload sweep, and `beginFrame` only does `glBindVertexArray(gVAO)`.

But: **the SDL window swap, ImGui (if present), and any external GL hook between frames could disturb state.** The risk is latent. Suggest invalidating the cache from `beginFrame()` as cheap insurance — it forces one redundant full-apply per frame at startup, which the existing summary counter would surface immediately if it became expensive.

**Recommendation:** call `stateCacheValid_=false;` at the top of `beginFrame()` for defensive correctness. Cost: at most one extra full-apply per frame.

### MINOR-2 — Trace gating uses `getenv` at every call (not `static const bool`)

**Site:** `gameos_graphics.cpp:2918`

```text
2918:        static const bool s_rsTrace = (getenv("MC2_RENDERSTATES_TRACE") != nullptr);
```

This `static const bool` initialized lazily is correct — it runs `getenv` exactly once. Compare against the kill-switch pattern at line 2873 which uses an immediately-invoked lambda. Both are static-init-order-safe. No defect, just a style inconsistency. Move on.

### MINOR-3 — `cachedResolvedTexId_` truncation note

**Site:** `gameos_graphics.cpp:1497`, `3054`

```text
1497:        uint32_t cachedResolvedTexId_[3] = {0u, 0u, 0u};
3054:           const uint32_t glId = (uint32_t)tex->getTextureId();
```

`gosTexture::getTextureId()` returns a `GLuint` (which is `unsigned int`, 32-bit on Win64). Cast is identity, no truncation. Verified safe.

### MINOR-4 — Summary counter print location and reset semantics

**Site:** `gameos_graphics.cpp:3128-3135` (in `endFrame`)

The print fires every 600 frames and resets all four counters. `rsCalls_` increments at top of `applyRenderStates`; `rsApplied_` and `rsSkipped_` increment in their respective branches. Invariant: `rsApplied_ + rsSkipped_ == rsCalls_`. Verified — both branches are exclusive (early-out `return` vs. fall-through to apply body).

Output line uses `event=summary` which matches the codebase pattern. Format is grep-friendly. Good.

The 600-frame cadence isn't justified vs. the existing `[TGL_POOL v1]` 600-frame summary cadence — they use the same constant by coincidence. If they're intended to align, document it; if not, prefer one shared cadence constant.

## Verification appendix (M / D / NF status)

| Symbol / claim | Status | Evidence |
|---|---|---|
| `gos_State_Texture==1`, `gos_MaxState` is loop bound | M | `gameos.hpp:2060`, `gameos.hpp:2132` |
| `RenderState` is `uint32_t[gos_MaxState]` | M | `gameos_graphics.cpp:1085` |
| `cachedResolvedTexId_[3]` zero-initialized | M | `gameos_graphics.cpp:1497` |
| `stateCacheValid_=false` initial | M | `gameos_graphics.cpp:1496` |
| Cache loop skips Texture/2/3 | M | `gameos_graphics.cpp:2900-2903` |
| Texture-bind loop records resolved id | M | `gameos_graphics.cpp:3057, 3068` |
| End-of-apply memcpy snapshot of `renderStates_` into `curStates_` | M | `gameos_graphics.cpp:3080` |
| Auto-reset of `gos_State_Terrain`/`gos_State_Water` after snapshot | M | `gameos_graphics.cpp:3086-3087` |
| Skip-path also runs Terrain/Water/Overlay bookkeeping | M | `gameos_graphics.cpp:2940-2944` |
| `MC2_RENDERSTATES_LEGACY=1` skips early-out | M | `gameos_graphics.cpp:2873-2879, 2891` |
| Bridge invalidation: direct-bind path | M, AT END | `gameos_graphics.cpp:1891` (after `glDrawArrays`) |
| Bridge invalidation: water fast path | M, AT END | `gameos_graphics.cpp:2240` (after final state restore) |
| Bridge invalidation: indirect terrain | M, AT END | `gameos_graphics.cpp:2493` (after final state restore) |
| Bridge invalidation: shadow prepass static | M, AT END | `gameos_graphics.cpp:3637` (after FBO/viewport restore) |
| Bridge invalidation: shadow prepass dynamic | M, AT END | `gameos_graphics.cpp:3700` (after FBO/viewport restore) |
| Bridge invalidation: postprocess endScene | M, AT END | `gos_postprocess.cpp:982` (after depth restore) |
| Force-apply hook invalidates cache | M | `gameos_graphics.cpp:5616` (before direct GL setup) |
| `gos_InvalidateRenderStateCache()` declared in public header | M | `gameos.hpp:2278` |
| **Static prop batcher invalidation present** | **NF** | `gos_static_prop_batcher.cpp` — no `invalidateRenderStateCache` call (CRITICAL-1) |
| **`drawTerrainOverlays` invalidation present** | **NF** | `gameos_graphics.cpp:5964-6011` (MAJOR-1) |
| **`drawDecals` invalidation present** | **NF** | `gameos_graphics.cpp:6017-6066` (MAJOR-2) |
| Static prop batcher save/restore covers wrap state | NF | `gos_static_prop_batcher.cpp:1700-1701` mutates wrap, no save (CRITICAL-1) |
| Memory `sampler_state_inheritance_in_fast_paths.md` honored | D | Honored for the 6 invalidation sites; violated for static prop / overlay / decal paths |
| Memory `mc2_texture_handle_is_live.md` honored | M | Cache compares resolved GL id, not slot |
| `gos_State_IsHUD` cache interaction | M | Toggling IsHUD diverges curStates from renderStates → forces full apply |
| Counter invariant `rsApplied + rsSkipped == rsCalls` | M | Branches mutually exclusive (`return` vs. fall-through) |
| Kill-switch leaves cache coherent for mid-run flip | M | Bottom of `applyRenderStates` always updates cache regardless of legacy flag |

Legend: **M** = matches claim, **D** = divergent (with detail), **NF** = not found in code.

## Architectural decisions that need user/advisor sign-off before revision pass

1. **Treat `invalidateRenderStateCache` as the new fast-path invariant.** Either (a) every code path that performs raw GL state mutation outside `applyRenderStates` MUST end with an invalidation call (audit + add for static-prop / overlay / decal / any future bridge), or (b) reframe the cache as "best-effort, callers expected to break it" and accept the regression class. (a) is the right answer; treat the three findings above as the audit completion list. Add a CLAUDE.md "Critical Rules" line — "Any new fast path that bypasses applyRenderStates MUST call gos_InvalidateRenderStateCache() at end" — once landed.

2. **Per-frame defensive invalidation in `beginFrame()`.** Cheap insurance vs. complexity. MINOR-1 advocates for it; sign-off needed on whether the cost (<1 full-apply / frame) is acceptable in exchange for eliminating the cross-frame risk class.

3. **Default-on flip discipline.** The kill-switch is wired correctly, but the default is "short-circuit on." Visual canary covered the static rendering paths but not the static-prop batcher path (CRITICAL-1) at the time of canary because `g_useGpuStaticProps` defaults off. Decision: should the v1 default-on flip be gated behind a soak that exercises `g_useGpuStaticProps=1` in tier1 explicitly, or is the kill-switch sufficient as the rollback?
