# PLAN: Persistent static light table (eliminate the per-frame FNV+memcmp for the static class)

> Branch `claude/gpu-driven-rendering`, HEAD `8c1c491`. RE-GREP all
> file:line at edit-time. Recon:
> `docs/superpowers/plans/progress/2026-05-17-persistent-static-light-table-recon.md`.
> Builds on: SSBO `b41baec`, static-bake `2db2a04`. This is the TRUE
> Option-A endpoint, now simple post-SSBO (no 64-window/partition-size/
> fallback — the deleted Option-A layer is the adversarial checklist).

## Goal — design the work OUT, not relocate it

The bake (`2db2a04`) retired the per-frame light RECOMPUTE
(`[LIGHTBRIDGE v1]` populate ~2000us->~80us, confirmed). A Tracy
capture proved the SURVIVOR: `addLightDataStructure scan`
(`mclib/txmmgr.cpp`, the 1792B fnv1a + 1792B memcmp) is STILL ~1840
calls/frame, ~0.97ms/frame bake-ON, because `mc2SubmitBakedLightSlot`
re-inserts the mission-constant baked struct into the per-frame-rebuilt
`lightData_` array every frame to (re-)derive a slot. That FNV+memcmp
re-derives an identity the engine already owns upstream: the registry
`recipeIndex` (monotonic, never-reused; registry already deduped
actors->recipes). This slice gives each static recipe a PERMANENT slot
written ONCE; the per-frame call, FNV, memcmp, and re-insertion for the
static class become **structurally absent** (not optimized).
feedback_offload_must_be_substitutive_not_additive.md: this is the
slice that takes `addLightDataStructure scan` -> ~0 for the static
class (zone-death, the DONE governor).

## Design

### D1. Permanent static prefix `lightData_[0..S)`; staticSlot = recipeIndex
- `staticSlot == recipeIndex` DIRECTLY (no compaction map). `registerRecipe`
  (`GameOS/gameos/gos_static_prop_registry.cpp` ~:251, RE-GREP) assigns
  `regIdx = s_recipeRanges.size()` strictly monotonic, never reused.
- **M1 — S-growth ordering invariant (state it; do not let an executor
  add relocation logic):** `S` advances ONLY at a bake-write
  (`mc2EnsureStaticSlotCapacity`), grow `lightData_` BEFORE the
  `lightData_[recipeIndex]` write (realloc+copy = the existing grow
  pattern, preserves contents). The dynamic region `[S..)` is fully
  rebuilt every frame from `resetLightData` (`count=S`), so an S bump
  mid-mission strands NO live data — dynamic slots are per-frame-
  ephemeral. NEVER relocate dynamic slots on an S grow (that would be a
  bug, not a fix).
- `S` = high-water of `max(recipeIndex)+1` (NOT a fixed 15000 alloc).
  `lightData_` (`new TG_HWLightsData[capacity]`, init capacity 128,
  existing +128 grow at the `addLightDataStructure` append site) must be
  grown so `capacity >= S + peak_dynamic`. Add a `mc2EnsureStaticSlot
  Capacity(recipeIndex)` that grows `lightData_` (preserving contents,
  same realloc pattern as the existing grow) when `recipeIndex >=
  S`; advance `S`. ~recipeIndex_max*1808B CPU + same SSBO; 27MB worst
  case << 128MB GL_MAX_SHADER_STORAGE_BLOCK_SIZE, RAM non-concern
  (feedback_ram_cost_not_a_concern_below_500mb.md).
- Zero shader change: `static_prop.vert:306` / `lighting.hglsl:53-55`
  index `light[int(inst.lightDataIndex)]` as a raw absolute subscript;
  a permanent absolute slot in `[0..S)` reads identically (verified by
  the SSBO slice). C++-only + one new helper (D3).

### D2. `resetLightData` count-base rebase (the load-bearing edit)
`mclib/txmmgr.cpp` `resetLightData` (RE-GREP; was `lightDataStructuresCount
= 0`): **m1 — gate the rebase under the kill-switch**:
`lightDataStructuresCount = mc2LightBakeEnabled() ? S : 0;` (with
`MC2_LIGHTBAKE=0` the persistent table is off, so the dynamic allocator
must start at 0 or it collides into a non-rebased prefix). When enabled,
set `lightDataStructuresCount = S` so dynamic appends start above
the static prefix and `addLightDataStructure` never returns a slot `< S`
(by construction — `rv = count`, count never < S). `s_lightDataDedupMap`/
`s_sceneLightTemplateMap`/`s_lightSlotByActorKey`/`s_bakedSlotByRecipe`
`.clear()` STAY (they only ever indexed dynamic appends; never hold a
static `[0..S)` entry by construction). The co-reset invariant comment
(the "slot indices restart from 0 each frame" block — RE-GREP, ~the
s_lightDataDedupMap clear) is preserved VERBATIM in meaning, just
rebased 0->S; update its text to say "restart from S". `s_bakedStaticLight`
(mission map) and the static prefix are NOT per-frame cleared.

### D3. NO sparse GPU helper — proven redundant (simplification, removes
### the gameos_graphics.cpp / concurrent-water-session conflict)
The recon proposed a `gos_LightDataSsbo_WriteSlot` glBufferSubData
helper for an immediate GPU write. **It is redundant and is NOT added.**
Argument (the new load-bearing correctness claim — implementation review
MUST validate it): with D2's `count=S` and `S=max(recipeIndex)+1`, the
EXISTING per-frame whole-buffer upload (`gos_LightDataSsbo_Upload(
lightData_, max(count,64)*sizeof)`, RE-GREP) sources CPU `lightData_`
and ships `[0..max(count,64)) ⊇ [0..S) ⊇ every static slot` EVERY frame
— including the bake frame (the upload runs in renderLists/flush, AFTER
update()/CacheGpuLightData where the bake writes `lightData_[recipeIndex]`).
So the CPU-mirror (D4.2) + the unchanged whole-buffer upload deliver
every static slot to the GPU each frame with zero new GL code.
Consequences: **zero `gameos_graphics.cpp` change -> the concurrent
water-only session shared-file conflict is fully eliminated**; the
substitutive win is unaffected (it was never "stop uploading static
bytes" — the upload was whole-buffer pre-slice and PROJECT.md scores
CPU-zone-death, not upload bytes; the win is killing the ~1840/frame
1792B FNV+memcmp, done by D4's `EmitBakedGpuLightData` rewrite). The
slice is now C++-only in `mclib/` (txmmgr/msl/bdactor) +
`gos_static_prop_registry.cpp` — no shaders, no gameos_graphics.cpp.

### D4. Bake-time once-write + per-frame no-op (R-B mirror)

**C1 (review blocker) — the substitutive edit locus is
`TG_MultiShape::EmitBakedGpuLightData` (`mclib/msl.cpp` ~:1953-1958,
RE-GREP).** TODAY it UNCONDITIONALLY calls `mc2SubmitBakedLightSlot`
(which calls `addLightDataStructure` -> the 1792B FNV+memcmp) every
HIT frame. THAT is the per-frame zone that must die. The substitutive
edit is to rewrite `EmitBakedGpuLightData`'s body to:
`cachedGpuLightIndex_ = recipeIndex; cachedFrame_ = g_mc2FrameCounter;`
(the permanent slot == recipeIndex; NO `mc2SubmitBakedLightSlot`, NO
`addLightDataStructure`). If this body is left as-is the slice
compiles, passes tier1 + visual parity (R-B idempotence makes it
*correct*) yet achieves ZERO zone-death — the exact additive-not-
substitutive failure `feedback_offload_must_be_substitutive_not_
additive.md` exists to prevent. Naming this edit IS the deliverable.
After it, `mc2SubmitBakedLightSlot` + `s_bakedSlotByRecipe` are
unreferenced -> delete them + their `resetLightData` clear
(minimal-touch: dead code goes).

The MISS branch (first bake per recipe), in `mc2CacheOrBakeStaticGpuLight`
(`mclib/bdactor.cpp`, RE-GREP) — **preserve the 2db2a04 C1 guard**:
only proceed if the gather actually ran
(`shape->getCachedGpuLightIndex() != 0xFFFFFFFFu` AND non-null leaf;
never persist a no-op snapshot — this guard is UNCHANGED, the
CPU-mirror/GPU-write goes INSIDE it). When valid, after the existing
post-decompose snapshot:
1. `mc2EnsureStaticSlotCapacity(recipeIndex)` (grow lightData_/advance S;
   see D1 — grow BEFORE any `lightData_[recipeIndex]` write).
2. `lightData_[recipeIndex] = baked;` (CPU MIRROR — R-B: persists
   because `resetLightData` never memsets contents; the unchanged
   per-frame whole-buffer upload (D3) ships it to GPU every frame; no
   sparse GL call needed).
3. `cachedGpuLightIndex_ = recipeIndex;` PERMANENT (via
   `EmitBakedGpuLightData(recipeIndex)` so MISS and HIT end identically).
On a bake HIT (every subsequent frame): `EmitBakedGpuLightData` (now)
just sets `cachedGpuLightIndex_ = recipeIndex` — NO
`addLightDataStructure`, NO FNV/memcmp, NO re-insertion.
The trailing `staticReg.lightDataIndex = shape->getCachedGpuLightIndex()`
ferry stays correct (permanent recipeIndex).

### D5b. Mission teardown (M2 — S reset co-located with recipeIndex reset)
`recipeIndex` space resets ONLY in `GpuStaticPropRegistry::destroy`
(`gos_static_prop_registry.cpp` ~:227 `s_recipeRanges.clear()`), which
also calls `mc2ClearAllBakedStaticLight` (~:231), reached per-mission
via `mission.cpp:3261` (RE-GREP all three). `mc2ClearAllBakedStaticLight`
(`mclib/txmmgr.cpp`) must ALSO reset `S = 0` (and the static prefix is
naturally re-zeroed because next mission's recipeIndex restarts at 0 and
each slot is overwritten at its first bake before any read — but assert
it: tier1's 5 sequential missions ARE the mission-transition gate; a
prior mission's prefix must not bleed into the next). Co-located by
construction (same `destroy` call) — cite `:227`+`:231`+`mission.cpp:3261`
at edit-time, do not merely assert.

### D5. Invalidation (in place, stable slot)
`GpuStaticPropRegistry::invalidate(recipeIndex)` already calls
`mc2EraseBakedStaticLight(recipeIndex)` (`2db2a04`). Extend: the next
bake recomputes and re-`WriteSlot`s the SAME `recipeIndex` slot in place
(destruction/LOD => same position => same constant; recipeIndex stable).
No slot free/reshuffle, no batcher/ferry change. Cull-gate adjacency:
the once-write happens at the existing gated bake call site (inside
`inView || g_useGpuStaticProps`) — unchanged from `2db2a04`.

### D6. Kill-switch
Reuse `MC2_LIGHTBAKE` (default ON; `=0` => the bdactor helper
`mc2CacheOrBakeStaticGpuLight` early-returns to the unchanged legacy
`CacheGpuLightData()` per-frame path BEFORE any bake/Emit/persistent-
slot code runs — full legacy, the safe code-proof fallback.
[Corrected: the old `2db2a04` `mc2SubmitBakedLightSlot` path is DELETED
by this slice; `=0` is the legacy path, not that path.]). `[LIGHTBAKE
v1]` lifecycle:
add `event=static_slot_written recipe=` (first only) +
`event=persistent_table mode=on|off` at init.

## CRITICAL adversarial-review targets
1. **R-B clobber correctness (the central hazard).** Prove the per-frame
   whole-buffer `glBufferData` upload (`txmmgr.cpp`, RE-GREP, the
   `gos_LightDataSsbo_Upload` per-frame call + `kLightUploadFloor`) does
   NOT clobber persistently-written static slots: because step D4.2
   mirrors into CPU `lightData_[0..S)` and `resetLightData` never
   memsets contents (only rebases count), the per-frame upload re-ships
   identical static bytes (idempotent). Verify resetLightData truly does
   not clear lightData_ contents; verify kLightUploadFloor over-count
   contract (load-bearing, kept) is untouched. Reject R-A (scope upload
   to [S..)) — it breaks the floor contract.
2. **count-base 0->S rebase**: every read/write of
   `lightDataStructuresCount` + every dedup-map site (RE-GREP all in
   txmmgr.cpp) correct rebased; an off-by-S = wrong-light/OOB.
3. **recipeIndex density / grow**: lightData_ grown before any
   `[recipeIndex]` write; `mc2EnsureStaticSlotCapacity` preserves
   contents (realloc+copy like the existing grow); peak recipeIndex vs
   capacity.
4. **cull-gate adjacency** unchanged from 2db2a04 (the once-write at the
   gated site).
5. **No Option-A regression** (no partition-size gate, no fallback-to-D2,
   no canary, no multi-site rebase beyond D2's single resetLightData
   line). C++-only + the one gameos_graphics.cpp helper (confirm zero
   `shaders/` diff -> shader_exe_deploy_lockstep N/A; still deploy exe).
6. **concurrency**: gameos_graphics.cpp shared with water session —
   minimal footprint, explicit-file commit, flag worktree isolation.

## Verification
- Adversarial-plan-review != STOP THE LINE (targets 1-6).
- Implementation adversarial review (as the bake had — caught C1).
- tier1 5/5, +0 destroys, GL-clean, FPS >= b41baec/2db2a04 baseline.
- USER substitutive proof (the DONE governor): `addLightDataStructure
  scan` Tracy zone calls/frame ~1840 -> dynamic-only (~mech/C2/no-actor
  residual) bake-ON; `[LIGHTBRIDGE v1]` stays ~0 for static; visual
  parity static/mech/HUD bake-on/off; total-frame ON vs OFF (anti-mirage).
- Commit (exe only, C++-only); append outcome to
  feedback_offload_must_be_substitutive_not_additive.md (the static
  zone-death finally measured if the user capture confirms — else honest
  "shipped behind flag, proof pending"); refresh render-perf-snapshot.

## Out of scope
Mech/dynamic remainder (small per-frame region; its own future slice if
sized). GPU-side light *generation* (compute-shader gather — north-star
eventual, not this). Generic props (no-actor-light path).
