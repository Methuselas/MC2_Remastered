# PLAN: Static-lighting Option A — persistent deduped static UBO partition

> Branch `claude/gpu-driven-rendering`, worktree `…/gpu-driven-rendering`.
> Anchors grep-verified @ `2dca942` (clean D2 baseline; per-frame-bake WIP
> stashed as wrong abstraction). RE-GREP at edit-time.
> Recon: `docs/superpowers/plans/progress/2026-05-17-static-lighting-option-a-persistent-partition-recon.md`
> (read it — this plan is its execution). Supersedes the per-frame-bake plan.

## The endpoint (why this, not another nibble)

The real waste is not the FNV/memcmp or the gather — it is that the entire
`lightData_` UBO is torn down (`resetLightData`: count→0, maps cleared) and
rebuilt every frame, even though static-actor light is a mission-load
constant (no dynamic emitters — `lighting_is_mission_load_static_no_dynamic_emitters.md`).
Option A makes the static actor's light **physically persistent**: computed
once, living in an immutable UBO prefix `resetLightData` never touches,
`cachedGpuLightIndex_` pointing at it forever. Per-frame static light cost →
**zero** (no gather, decompose, FNV/memcmp, `addLightDataStructure`, slot
churn). D2 stays as the mech/dynamic floor AND the static-overflow fallback.

## The hard constraint (designed around, not ignored)

`shaders/include/lighting.hglsl:54` `ObjectLights light[64]` — the shader
addresses **64 actor-slots total per frame** (static+dynamic+mech combined),
indexed by an absolute `lightDataIndex` with NO region tag/base (verified:
`static_prop.vert:306`, `mech.vert:162`). The registry reserves 15000 recipes
(`gos_static_prop_registry.cpp:175`) — **far more than 64.** The system works
today only because per-frame dedup (`s_lightDataDedupMap`) collapses all
visible actors to ≤~57 distinct structs (`lighting.hglsl:41-43` observed
maxIdx). **Therefore the static partition MUST be deduped (NOT one slot per
recipe) and MUST self-bound:** if the static partition is full, the recipe
falls back to the per-frame D2 dynamic path — correct-by-construction, never
overflows the window, never corrupts. `S` is a tuning knob, not a blocking
pre-gate; the deploy+smoke measures coverage as a byproduct.

## Design

### A1. Partition: one buffer, fixed static prefix `[0..S)` + dynamic `[S..)`
- `S` = compile-time reserved static-prefix size. Start `S = 48`
  (leaves `[48..64)` ≥16 for dynamic/mech; tunable; the coverage counter
  validates). Static slots are stable absolute indices in `[0..S)`.
- `lightDataStructuresCount` semantics → "next free **dynamic** slot":
  init `= S` (`txmmgr.cpp:319`, was 0), reset `= S` in `resetLightData`
  (`txmmgr.cpp:1272`, was 0). `addLightDataStructure` returns `rv = count`
  → provably never `< S` → **no collision into static space, by
  construction.** The load-bearing co-reset invariant (`txmmgr.cpp:1273-1275`)
  is preserved verbatim, rebased 0→S (dynamic-region-relative).
- Every `lightDataStructuresCount`/dedup-map site rebased per recon §3
  (`:319`, `:1158`, `:1168`, `:1177-1179`, `:1272`, `:1286` peekLightSlot
  bound, `:1559` upload floor). `s_lightDataDedupMap` stays dynamic-only by
  construction (no code change, semantic note only).
- Buffer capacity must be ≥ `S + peak-dynamic`; ensure initial
  `lightDataStructuresCapacity` (`:318` =128) ≥ that (128 ≥ 64, fine).
- NO shader change (absolute index, region-agnostic — recon §1).

### A2. Persistent deduped static store (mission-scoped, NOT per-frame reset)
- `s_staticLightSlots`: `unordered_map<uint64_t structHash, uint32_t slot>`
  + `s_staticLightCount` (0-based within `[0..S)`). Keyed by
  `fnv1a_64_struct(&baked, sizeof(TG_HWLightsData))` (reuse existing hasher).
  Many recipes with identical terrain-light → ONE shared static slot.
- A per-recipe `recipeIndex -> staticSlot` map so invalidation/lookup is O(1)
  per multi.
- Cleared ONLY on mission-unload (`GpuStaticPropRegistry::destroy()`,
  `code/mission.cpp:3261`; reuse the stashed `mc2ClearAllBakedStaticLight`
  hook pattern). NEVER cleared by per-frame `resetLightData`.

### A3. Lazy permanent assign + per-frame skip (single chokepoint)
Inside `TG_MultiShape::CacheGpuLightData()` (`msl.cpp:1892`) — the one site
all four static call sites reach, past `SetLightList` (`bdactor.cpp:2444/4894`)
so `s_listOfLights` is valid:
1. New `TG_MultiShape` members: `staticClass_` (true for bdactor/tree
   multis, false for mechs — set owner-side, see Open Q2), `staticBaked_`,
   `staticSlot_`, `staticNeedsRebake_`.
2. `if (staticClass_ && staticBaked_ && !staticNeedsRebake_) return;` at the
   TOP — per-frame death for baked static actors. `cachedGpuLightIndex_`
   already holds `staticSlot_`, untouched.
3. First call (or rebake): run existing `GatherGpuObjectLightDataOnly()`
   ONCE; take the post-decompose struct from `firstShapeNodeLeaf->lightData_`
   (TG_Shape member, TG_MultiShape is friend — `tgl.h:714`); hash it; dedup
   into `s_staticLightSlots`. If a slot exists or `s_staticLightCount < S`:
   write the struct to `lightData_[slot]` (once, or reuse), set
   `cachedGpuLightIndex_ = staticSlot_ = slot`, `staticBaked_=true`. **If the
   static partition is FULL (`s_staticLightCount == S` and no dedup hit):
   set `staticClass_=false`-equivalent fallback — this recipe takes the
   unchanged per-frame D2 path forever (no early-return, normal
   `GatherGpuObjectLightDataOnly`).** Self-bounding; never overflows.
4. Mechs: `staticClass_==false` → unchanged D2 path (their floor).

### A4. Invalidation in place (stable slot)
- `invalidate(int32_t)` (`gos_static_prop_registry.cpp:291`) hook (reuse the
  stashed `mc2EraseBakedStaticLight` pattern): set the multi's
  `staticNeedsRebake_` (via the recipe→multi map / `invalidateStaticRegistration`
  `bdactor.cpp:3002-3005` — confirm exact hook at edit-time).
- Next `CacheGpuLightData()`: re-gather ONCE, overwrite the SAME
  `staticSlot_` in place (`lightData_[staticSlot_] = newStruct`),
  `cachedGpuLightIndex_` unchanged, clear flag. NO slot free/reshuffle →
  batcher (`gos_static_prop_batcher.cpp:2536`) + ferry (`registry:391-392`)
  read an unchanged value, **zero batcher/ferry change.**
- Multi-identity swap (destruction→stump/LOD): the NEW multi bakes its own
  slot lazily (dedup likely reuses the same terrain-light slot → no growth).
  Old slot abandoned-in-place; bounded because dedup collapses identical
  terrain-light and the self-bound fallback caps `[0..S)`. (No free-list
  first cut; RAM is a non-concern — `feedback_ram_cost_not_a_concern_below_500mb.md`.)

### A5. GPU upload (honest first cut)
`gos_UpdateBuffer` is `glBufferData` full-orphan, no sub-range
(`gameos_graphics.cpp:6383-6391`). First cut: **keep whole-buffer per-frame
upload** — it now memcpys *constant* bytes for `[0..S)` (no CPU recompute).
The lever is CPU-recompute death, not the memcpy
(`feedback_offload_must_be_substitutive_not_additive.md`). `cpu_buf_size`
floor (`txmmgr.cpp:1559`) must cover `[0..S)` (already floored to
`64*sizeof`, fine if `S≤64`). A `glBufferSubData` sub-range upload is a
deferred follow-on, NOT required for the substitutive win.

### A6. Kill-switch + lifecycle (same commit)
`MC2_LIGHTBAKE` env (reuse stashed scaffolding name), **default ON** (commit
to the bigger move per user direction; instant revert via `=0` → unchanged
D2/legacy path bit-for-bit, the reversibility safety for a UBO-contract
change). `[LIGHTBAKE v1]` lifecycle: `event=enabled mode=bake|passthrough`,
`event=first_bake recipe=`, `event=static_partition_full` (first; the
fallback canary), `event=rebake_on_invalidate` (first), plus a per-600-frame
`event=coverage baked=<n> fallback=<n> static_distinct=<s>/S
dyn_peak=<d>` summary — this IS the recon's Stage-0.5 measurement, captured
as a byproduct of the validation smoke, not a blocking pre-gate.

## Verification (soak waived; probes substitute)
- Adversarial-plan-review on THIS plan ≠ STOP THE LINE; the CRITICAL items:
  (a) count-base 0→S rebase correctness (recon §6.2 — every site, off-by-S =
  wrong-light corruption); (b) cull-gate adjacency (the per-frame
  `markVisible(regIdx, staticSlot_)` ferry MUST stay inside the existing
  `inView || g_useGpuStaticProps` gate — `cull_gates_are_load_bearing.md`);
  (c) static/dynamic distinct total ≤ 64 window (self-bound fallback makes
  this safe, but review must confirm dynamic can't be starved by `S` too
  large); (d) the abandoned-slot exhaustion bound vs self-bound fallback.
- Parity probe: env-gated assert that for a baked actor the persistent slot
  struct is bit-equal to a fresh `GatherGpuObjectLightDataOnly` this frame
  (sample 1/N; substitutive-correctness in lieu of soak —
  `feedback_soak_waiver_with_probes_and_reviews_validated`).
- tier1 5/5, `MC2_LIGHTBAKE` default + `GL_INVALID_*`=0 + `+0` destroys.
- **mc2_03 controlled gate (independent-bug check, recon §6.4 / Open Q7):**
  user-driven mc2_03 `MC2_LIGHTBAKE=0` vs default, same camera —
  `Render.GpuStaticProps` must NOT regress AND the `[LIGHTBAKE v1]
  coverage` line must show C7-fallback→~0 for baked actors. Do NOT assume
  Option A fixes the stashed WIP's mc2_03 blowup; prove it.
- Visual canary: bake on/off side-by-side heaviest + mc2_03 — bdg/tree
  lighting bit-visually identical (aRGB landmine = wrong/hot static colors).
- Memory: append outcome to
  `lighting_is_mission_load_static_no_dynamic_emitters.md` /
  `feedback_offload_must_be_substitutive_not_additive.md`. Commit carries
  the retired-CPU-chain in code dimensions; no wall-clock; records the
  user-waived total-frame proof + the mc2_03 controlled-gate result.

## Out of scope
`glBufferSubData` sub-range upload (deferred follow-on). UBO→SSBO window
enlargement (only if coverage counter shows distinct-total >64 even with
dedup — then it becomes a hard dependency, separate slice). Generic props
(no-actor-light path). Mechs (D2 floor). Total-frame Tracy proof (user-waived).
