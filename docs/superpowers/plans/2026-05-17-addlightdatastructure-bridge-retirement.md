# PLAN: addLightDataStructure bridge retirement (substitutive perf slice)

> Branch `claude/gpu-driven-rendering`, worktree
> `A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\`.
> Anchors grep-verified 2026-05-17 @ commit `158c229` (descends from
> post-shadow/post-drawPass `51039af`). RE-GREP every file:line at
> edit-time. Source handoff:
> `docs/superpowers/plans/progress/2026-05-17-addlightdatastructure-bridge-retirement-handoff.md`.

## Status of prerequisites (DONE before this plan)

- **Anchors re-verified** stable at HEAD; consumer inventory (handoff
  Buckets A/B/C) confirmed by the cpu-gpu-offload advisor (verify-then-
  extend, not re-inventory).
- **Sizing-telemetry defect found + fixed.** Handoff prescribed
  `[OBJECT_RECON v1] shape_emit_ns`; its Scope (`tgl.cpp:2602`) wraps
  ONLY the C1 legacy leaf gated `!eligibleForGpuObjects(this)`
  (`tgl.cpp:2604`) — structurally dead for the GPU-batched target
  population. Landed env-gated `[LIGHTBRIDGE v1]` coarse per-frame
  accumulator on the true C5 entry (`addLightDataStructureWithPerActorColor`),
  committed `158c229`, full-relink built + deployed to `v0.4`.
- **STOP gate: PASS.** User-driven non-COST_SPLIT capture, heaviest
  mission, post-shadow: populate ≈ **2.1–2.5 ms/frame**, stable;
  `calls≈1632, tmpl_hit≈1621 (99.3%), tmpl_miss≈11, no_actor_light=0`.
  Real multi-ms lever, retirable-path-dominated.

## Goal

Retire the per-call `fnv1a_64_struct(light_data, 1792B)` (`txmmgr.cpp:1033`)
+ `memcmp(...,1792B)` (`txmmgr.cpp:1039`) for the C5/C6/C7 GPU-object
light-data populate by short-circuiting it with a tiny per-frame
(template-key + per-actor-color) -> slot cache. The render-time READ is
already GPU-owned (`cachedGpuLightIndex_` / `lightDataBuffer_` UBO,
Bucket B); this repoints the update-time POPULATE. Substitutive: no new
GPU path; an existing redundant CPU cache layer is removed.

## Why bit-identity holds (the corrected repoint key)

`decomposeFirstActiveLightColor` (`txmmgr.cpp:954-970`) mutates ONLY
`lights->lightColor[0][0..2]` from the CURRENT actor's
`listOfLights[firstActiveLightSourceIndex()]->GetaRGB()` (and `[0][3]=1`).
`sceneLightTemplateKey` (`txmmgr.cpp:986-1024`) deliberately EXCLUDES
that one light's aRGB (`:1017-1020`). Therefore two actors sharing a
template differ in their final `TG_HWLightsData` ONLY by that aRGB.
Hence:

- `actorLightSlot == 0xFFFFFFFFu` (no active per-actor light, decompose
  skipped at `txmmgr.cpp:1185-1186`): template-key alone fully
  determines the struct -> one slot per template-key.
- otherwise: the minimal sufficient slot key is
  `combinedKey = fold(templateKey, actorARGB)` where
  `actorARGB = listOfLights[firstActiveLightSourceIndex()]->GetaRGB()`.

A single cached slot per template-key (the handoff's literal "template
-> slot" wording) is **WRONG** — it collapses per-actor color. The
plan uses `combinedKey`. Collision-safety equivalent to today's FNV+
memcmp is preserved by storing the `(templateKey, actorARGB)` tuple
beside the slot and verifying the 12-byte tuple on hit (vs the current
1792-byte memcmp at `:1039`) before returning — same fail-safe-to-append
semantics, ~150x cheaper compare.

## Files / symbols (re-grep at edit-time)

- `mclib/txmmgr.cpp`
  - `addLightDataStructureWithPerActorColor` `:1160` (C5 entry; the
    repoint site)
  - `addLightDataStructure` `:1027` (FNV `:1033`, memcmp `:1039`) — kept
    intact; the slice routes AROUND it on the small-key hit, does not
    modify its body
  - `resetLightData` `:1191` (frame-start clear; add new cache clear +
    existing `lbDrainPerFrame`)
  - anon-namespace statics block `:903-...`: `s_sceneLightTemplateMap`
    `:911`, `CachedSceneLightTemplate` `:906`, `sceneLightTemplateKey`
    `:986`, `decomposeFirstActiveLightColor` `:954`,
    `firstActiveLightSourceIndex` `:972`, `[LIGHTBRIDGE v1]` accumulator
    (already landed)
- `mclib/tgl.cpp`
  - `GatherGpuObjectLightDataOnly` `:2858` -> `WithPerActorColor`
    `:2860` (C5/C7 route)
  - `ResubmitCachedLightData` `:2863` -> `addLightDataStructure` DIRECT
    `:2865` (C6 route — NOT through WithPerActorColor)
  - C1 `:2625` / C2 `:3113` gate `:2604` (Bucket A; out of scope —
    SHRINK not delete)
- `mclib/msl.cpp`: `CacheGpuLightData` `:1892` (call `:1918`),
  `ResubmitCachedGpuLightData` `:1923` (call `:1941`), aRGB-window doc
  `:1877-1887`
- `GameOS/gameos/gos_static_prop_batcher.cpp`: C7 fallback `:2539`
  (cache-miss only); render read `:2534-2536` (Bucket B, untouched)
- `mclib/mech3d.cpp`: `CacheGpuLightData` `:4501` (C5 path for mechs);
  `cachedGpuLightIndex_` read `:2530` (Bucket B, untouched)

## Design

### D1. New per-frame small-key slot cache (txmmgr.cpp anon ns)

```
struct LightSlotKey { uint64_t tmpl; uint32_t actorARGB; uint32_t pad; };
static std::unordered_map<uint64_t /*combined*/, std::pair<LightSlotKey,uint32_t/*slot*/>> s_lightSlotByActorKey;
```
`combined = fnv1a_64_u32(fnv1a over tmpl bytes, actorARGB)`; the stored
`LightSlotKey` is the collision-verify tuple. Cleared in
`resetLightData` alongside `s_lightDataDedupMap` /
`s_sceneLightTemplateMap` (same frame-start invariant — slot indices
restart at 0 each frame).

### D2. Repoint C5 (`addLightDataStructureWithPerActorColor`)

After the existing template resolve + per-actor decompose
(`txmmgr.cpp:1175-1186`, UNCHANGED — preserves the update()-time aRGB
window), and ONLY when `g_lightBridgeEnabled` (kill-switch ON):

1. `actorARGB = (actorLightSlot==0xFFFFFFFFu) ? 0 : listOfLights[firstActiveLightSourceIndex()]->GetaRGB()`
2. `combined = fold(key, actorARGB)`; lookup `s_lightSlotByActorKey`.
3. Hit + tuple matches: return cached slot. **Skips** the 1792B FNV +
   1792B memcmp + map-find in `addLightDataStructure` entirely.
4. Miss: `slot = addLightDataStructure(light_data)` (unchanged legacy
   append/dedup), then `s_lightSlotByActorKey[combined] = {{key,actorARGB},slot}`.

Kill-switch OFF: fall straight through to the existing
`return addLightDataStructure(light_data)` (`:1188`) — legacy path
bit-for-bit, zero behavior delta.

**Load-bearing symmetry invariant (do not break without re-deriving
the slot key).** `firstActiveLightSourceIndex()` (`txmmgr.cpp:972-984`),
the `actorLightSource` passed to `sceneLightTemplateKey` (`:1170`), and
the first-active scan inside `decomposeFirstActiveLightColor`
(`:954-970`) are the SAME light index: the template key excludes
exactly `iLight == actorLightSourceIndex`'s aRGB (`:1017-1020`) and
decompose mutates exactly that same light's color into
`lightColor[0][0..3]`. `combinedKey = fold(templateKey, actorARGB)` is
sufficient ONLY while this symmetry holds. If a future edit changes
which light decompose keys off, or widens per-actor mutation beyond
`lightColor[0]`, the template-key exclusion AND this slot key must be
widened in lockstep (the `:1013-1016` in-code comment already warns
this). Add a same-commit static check / comment at both sites.

### D3. All-callers contract (handoff review item b)

Every `cachedGpuLightIndex_` populate must reach the repointed path:

- **C3/C4 do not exist as distinct populate sites (grep-verified
  2026-05-17 @ `158c229`).** Every `CacheGpuLightData()` invocation —
  buildings `bdactor.cpp:2478,2521`, trees `bdactor.cpp:4912,4933`,
  generic `genactor.cpp:1219`, mechs `mech3d.cpp:4501` — calls
  `TG_MultiShape::CacheGpuLightData()` (`msl.cpp:1892`) whose SOLE
  populate is `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()`
  (`msl.cpp:1918`). The handoff's "C3/C4" (mech vs static-prop)
  converge into the single C5 leaf; there is no separate mech/tree/
  generic populate function. The all-callers contract is therefore
  satisfied by repointing C5 + C6 + C7 alone.
- **C5** `CacheGpuLightData` (`msl.cpp:1918`, `mech3d.cpp:4501`): via
  D2. ✓
- **C7** batcher render-time fallback (`gos_static_prop_batcher.cpp:2539`):
  calls the same `GatherGpuObjectLightDataOnly` -> D2. Follows C5 for
  free; KNOWN-FRAGILE (wrong color on non-first cache-miss frames) — NOT
  given a CPU safety net (handoff explicit; that is the negative-work
  pattern). ✓
- **C6** `ResubmitCachedGpuLightData` -> `ResubmitCachedLightData`
  (`tgl.cpp:2865`) calls `addLightDataStructure(&lightData_)` DIRECT —
  bypasses `WithPerActorColor`, so D2 does NOT cover it. `lightData_`
  here already holds the per-actor struct from the prior
  `CacheGpuLightData`. Repoint: `ResubmitCachedGpuLightData`
  (`msl.cpp:1923-1944`) already tracks `cachedGpuLightIndex_` +
  `cachedFrame_`. When `cachedFrame_ == g_mc2FrameCounter` and
  `cachedGpuLightIndex_ != 0xFFFFFFFFu`, the slot is still valid THIS
  frame (slots reset per frame by `resetLightData` `:1191`, invoked
  once-per-frame from `clearArrays()` `txmmgr.h:1242` at frame reset
  alongside `gvManager`/`rsManager` reset — the SAME exactly-once
  invariant `s_lightDataDedupMap` correctness already relies on. The
  registration path `gos_static_prop_registry.cpp:274` sets
  `cachedFrame_` WITHOUT a valid index, so the
  `cachedGpuLightIndex_ != 0xFFFFFFFFu` guard is load-bearing not
  redundant) — return
  `cachedGpuLightIndex_` without calling `ResubmitCachedLightData` at
  all. Otherwise fall back to the legacy direct
  `addLightDataStructure`. This is the C6 arm of the substitution and
  MUST be in the same slice (contract violation otherwise).
- **C2** `tgl.cpp:3113` direct (Bucket A, `!eligibleForGpuObjects` /
  spotlight / window / alpha): OUT OF SCOPE. Documented structural
  residual — SHRINK not delete (handoff "Shape" section).

### D4. Kill-switch + lifecycle (same commit)

- `g_lightBridgeEnabled` from env `MC2_LIGHTBRIDGE` (default **ON** per
  handoff DoD "tier1 5/5 with kill-switch default = repointed path
  active"; `MC2_LIGHTBRIDGE=0` restores legacy FNV/memcmp populate
  bit-for-bit). NOTE: this differs from the recon accumulator's
  `MC2_OBJECT_RECON_TRACY` gate (measure-only, independent).
- Extend the already-landed `[LIGHTBRIDGE v1]` schema with lifecycle
  events: `event=enabled mode=repoint|legacy` (init),
  `event=first_populate`, `event=c6_fastpath_hit` (first only),
  `event=c7_fallback_hit` (first only), `event=teardown` — gated,
  demote-not-delete, log at lifecycle boundaries only (per
  `debug_instrumentation_rule`).

## Risks / landmines

- **aRGB per-actor window**: D2 inserts AFTER `decompose` and does not
  move the call site; CacheGpuLightData still runs at update(). Window
  preserved by construction. Adversarial review must confirm no reorder.
- **Bit-identity**: rests on the D-section claim that
  `decompose` mutates only `lightColor[0][0..2]` and the template key
  excludes exactly that aRGB. Review must re-grep `:954-970` and
  `:1017-1020` and confirm no OTHER per-actor mutation exists between
  template resolve and `addLightDataStructure`.
- **C6 frame-validity**: D3-C6 assumes slots are stable within a frame
  and reset at `resetLightData`. Review must confirm `resetLightData`
  is exactly-once-per-frame at frame start (the existing
  `s_lightDataDedupMap` correctness already depends on this invariant —
  `txmmgr.cpp:1194-1199`).
- **Collision-safety**: 12-byte tuple verify replaces 1792B memcmp;
  fail path = fall through to legacy append (same as today).

## Verification (handoff Definition of done)

1. Adversarial-plan-review on THIS plan != STOP THE LINE; aRGB-window +
   all-callers (C5/C6/C7) + bit-identity accepted.
2. Implement D1–D4; tier1 5/5 with `MC2_LIGHTBRIDGE` default (repoint
   active) + `GL_INVALID_*`=0.
3. Fresh USER-DRIVEN non-COST_SPLIT Tracy, BOTH regimes
   (light + heaviest, incl. zoomed-out worst-case):
   `[LIGHTBRIDGE v1]` populate ns -> ~0 on the batched population AND
   total frame DOWN ON vs OFF (anti-mirage: ns->0 alone is not proof);
   confirm NO displaced cost in a new zone.
4. Visual canary: `MC2_LIGHTBRIDGE` on/off side-by-side, heaviest
   mission, static-prop + mech lighting bit-visually identical (aRGB
   landmine surfaces as wrong/hot colors).
5. Kill-switch + `[LIGHTBRIDGE v1]` lifecycle in the same commit.
6. Memory: append outcome to
   `feedback_offload_must_be_substitutive_not_additive.md` (third
   substitutive win, or honest "shrank, not shipped"). Refresh
   `docs/render-perf-snapshot.md`. Commit carries before/after Tracy
   delta both regimes, code dimensions only.

## Out of scope

C2 Bucket-A residual (windows/spotlights/alpha); flushShadow VAO
redesign; `docs/render-perf-snapshot.md` refresh is the closing step
not the slice; anything below the ~5-6ms light / ~8-10ms heavy render
floor (genuine `Mission::update` sim).
