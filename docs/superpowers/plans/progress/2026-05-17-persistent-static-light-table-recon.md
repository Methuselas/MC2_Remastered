# Persistent Static Light Table — RECON (the deferred Q2 follow-on)

- **Branch / HEAD:** `claude/gpu-driven-rendering` @ `8c1c491`
  (`git rev-parse HEAD` = `8c1c491df36f94ab231d506e2425053e9dbf79ab`).
- **Scope:** READ-ONLY recon. No code written. Verify-then-extend the
  three input plans; do NOT re-derive their settled findings.
- **What is already SHIPPED at HEAD (grep-confirmed):** the D-B Option-B
  bake. `s_bakedStaticLight` / `s_bakedSlotByRecipe` (`txmmgr.cpp:1160-1161`),
  `mc2SubmitBakedLightSlot` (`:1236`), `mc2CacheOrBakeStaticGpuLight`
  (`bdactor.cpp:2217`), `EmitBakedGpuLightData` (`msl.cpp:1953`),
  `MC2_LIGHTBAKE` default-ON (`txmmgr.cpp:1170`). D-B retired the
  per-frame *recompute* (`[LIGHTBRIDGE v1]` populate ~2000us->~80us,
  user-confirmed). It did NOT retire the per-frame *slot write*:
  `mc2SubmitBakedLightSlot` still calls `addLightDataStructure` once per
  static recipe per frame (`txmmgr.cpp:1242`), and `resetLightData` still
  clears `s_bakedSlotByRecipe` every frame (`:1385`) forcing re-insertion.
- **This recon designs the explicitly-deferred Q2 follow-on** (rescope
  doc lines 239-255: "give the baked struct a genuinely persistent SSBO
  index that resetLightData does not clear … documented deferred
  optimization"). The Tracy trigger: `addLightDataStructure scan` zone
  (`txmmgr.cpp:1252`) is STILL ~1840 calls/frame, ~0.97 ms/frame bake-ON
  (1808 B fnv1a + 1808 B memcmp per call) because every static recipe
  re-inserts its mission-constant struct each frame. This slice designs
  that work OUT.

All file:line grep-verified at `8c1c491` this invocation. Line numbers
DRIFTED from the input docs (written @ `b41baec`); symbols stable.

---

## Q1. Partition for the persistent static table

### resetLightData's per-frame teardown (unchanged at HEAD)

`MC_TextureManager::resetLightData()` `txmmgr.cpp:1372`:
- `lightDataStructuresCount = 0;` (`:1378`)
- `s_lightDataDedupMap.clear();` (`:1382`)
- `s_sceneLightTemplateMap.clear();` (`:1383`)
- `s_lightSlotByActorKey.clear();` (`:1384`)
- `s_bakedSlotByRecipe.clear();` (`:1385`)
- `s_sceneLightTemplateFrame = 0xFFFFFFFFu;` (`:1386`)

The load-bearing co-reset comment (`:1379-1381`): "slot indices restart
from 0 each frame, so any stale hash->slot entries from the prior frame
are invalid." This is correct for the DYNAMIC class and MUST stay.

### The partition: a reserved static prefix `[0 .. S)`

The persistent model needs a region `resetLightData` does NOT clear and
the per-frame allocator never hands out. Design:

- `lightData_[0 .. S)` = **permanent static slots**, written ONCE per
  recipe at bake/invalidate, never per-frame-reset.
- `lightData_[S .. count)` = **per-frame dynamic region** (mech / C2 /
  no-actor / template), reset every frame.
- `resetLightData`: `lightDataStructuresCount = S;` (not 0). The
  per-frame allocator (`addLightDataStructure` `:1278-1280`) appends from
  `S` upward; the dedup map / template map / actor-key map / baked-slot
  map still `clear()` (they only ever indexed dynamic appends).
- `start()` (`txmmgr.cpp:319`) initializes `lightDataStructuresCount = 0`
  pre-mission (no static recipes yet); the static prefix grows as recipes
  bake. `S` is therefore **not a fixed compile-time constant** — it is
  `= (high-water static slot count)`, growing monotonically within a
  mission and reset to 0 on mission unload.

### recipeIndex as the static slot index — DIRECT, with one caveat

`registerRecipe` (`gos_static_prop_registry.cpp:239`):
`regIdx = (int32_t)s_recipeRanges.size()` (`:251`) then
`s_recipeRanges.push_back(rng)` (`:252`) — **strictly monotonic,
0-based, never reused** (`invalidate` `:300` sets `rng.count=0` tombstone,
`:308`; the vector entry is NEVER erased, so the index space is dense and
never recycled). `s_recipeRanges.reserve(15000)` (`:181`).

**recipeIndex is dense and monotonic — usable as a DIRECT static slot
index, BUT not every recipe is a baked static-light recipe.** recipeIndex
covers ALL GPU static-prop recipes (bldg + tree); the bake snapshot only
populates `s_bakedStaticLight[recipeIndex]` for actors that pass the
`registered && recipeIndex>=0 && gather-ran` guard
(`bdactor.cpp:2224,2241`). A recipe whose actor never baked has no slot
content. That is fine for a DIRECT mapping (`staticSlot == recipeIndex`):
the slot simply stays zero-initialized and is never read (the batcher
only reads `cachedGpuLightIndex_` for actors that went through the bake
path, which sets it to the permanent slot). **No compaction map needed.**
A `recipeIndex -> staticSlot` compaction map would only save memory
(skipping holes); per `feedback_ram_cost_not_a_concern_below_500mb.md`
the ~27 MB worst case (below) does not justify the compaction-map
complexity for the first cut. **Chosen: `staticSlot = recipeIndex`,
direct, no map.**

Caveat to document in the plan: `S` (the cleared-floor) must be
`max(recipeIndex)+1` over baked recipes, not the baked *count*, because
direct mapping leaves holes. Track `S = max(S, recipeIndex+1)` at each
bake-write. Worst case `S == s_recipeRanges.size()`.

### Size: SSBO-reasonable

`sizeof(TG_HWLightsData)` = `tgl.h:304-310`: `lightToWorld[16][16]` +
`lightDir[16][4]` + `lightColor[16][4]` + `lightFalloff[16][4]` +
`numLights_` + `pad[3]` = (256+64+64+64)*4 + 16 = **1808 bytes**
(`MAX_HW_LIGHTS_IN_WORLD=16` `tgl.h:282`; matches the SSBO doc's 1808 B
and the prompt's "1792B" payload + 16 B tail).

Worst case `s_recipeRanges.reserve(15000)` -> 15000 * 1808 =
**~27.1 MB**. `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` is queried at
`gos_mech_batcher.cpp:234` / `gos_render.cpp:378`; the comment
`gos_mech_batcher.cpp:225` states it is "typically >=128MB". 27 MB is
~21% of the conservative floor — **SSBO-reasonable, no headroom
concern.** The C++ `lightData_` array already grows by +128 chunks
(`txmmgr.cpp:1271-1275`); the static prefix rides the same growth (a
mission with 15000 recipes already needs ~15000-entry capacity for the
existing per-frame path, so this adds NO new allocation pressure — the
prefix REPLACES per-frame re-appends of the same structs).

### Shader: ZERO change (confirmed, same as the SSBO slice)

The shader indexes `light[int(inst.lightDataIndex)]` as a **raw absolute
subscript, no base, no region tag**: `static_prop.vert:306`
`ObjectLights ld = light[int(inst.lightDataIndex)];`, also `:249`
`calc_light(int(inst.lightDataIndex), …)`; `mech.vert:162`. The SSBO
block is unbounded: `lighting.hglsl:53-55`
`layout (binding = LIGHT_DATA_SSBO_BINDING, std430) buffer LightsData {
ObjectLights light[]; }`, `LIGHT_DATA_SSBO_BINDING 20` (`:15`). A
permanent static absolute slot `j < S` is read identically to any
per-frame slot. **Zero shader edit — exactly like the UBO->SSBO slice
(`lighting.hglsl:43-50` confirms the absolute-index invariant).**

---

## Q2. Sparse write — the in-scope new dependency (the central hazard)

### The existing upload path (grep-verified)

Per-frame whole-buffer upload, `txmmgr.cpp:1673-1678`:
```
constexpr uint32_t kLightUploadFloor = 64u;
const size_t lightUploadCount =
    std::max<uint32_t>(lightDataStructuresCount, kLightUploadFloor);
gos_LightDataSsbo_Upload(lightData_, lightUploadCount * sizeof(TG_HWLightsData));
```
`gos_LightDataSsbo_Upload` (`gameos_graphics.cpp:6471`):
- first call: `glGenBuffers` + `glBufferData(...,data,GL_DYNAMIC_DRAW)`
  + `glBindBufferBase(...,20,...)` (`:6474-6486`).
- grow (bytes > current): `glBufferData(...,data,...)` whole reupload
  (`:6489-6502`).
- **same-size: `glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, bytes,
  data)` (`:6504`)** — a sub-range primitive ALREADY EXISTS in the
  helper, but it uploads the WHOLE `[0,bytes)` from CPU `lightData_`
  every frame.

Eager create at `txmmgr.cpp:329` (`MC_TextureManager::start()`,
capacity=128 -> 128*1808 bytes). `kLightUploadFloor=64` backs the
engine's deliberate over-count tolerance for cull-stale actors
(`:1664-1672` — falsified-as-removable 2026-05-17, kept on purpose).

### The minimal sparse path

Add a slot-scoped writer in the existing helper file
(`gameos_graphics.cpp`, alongside `gos_LightDataSsbo_Upload` ~`:6508`):

```c
void __stdcall gos_LightDataSsbo_WriteSlot(uint32_t slot,
                                           const void* data, size_t slotBytes)
{
    if (s_lightDataSsbo == 0) return;            // eager-create guarantees !=0
    const GLsizeiptr off = (GLsizeiptr)slot * (GLsizeiptr)slotBytes;
    if (off + (GLsizeiptr)slotBytes > s_lightDataSsboBytes) return; // grow first
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, off, (GLsizeiptr)slotBytes, data);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
```
- `glBufferSubData` is valid on the existing buffer: it was created
  `GL_DYNAMIC_DRAW` (`:6477`) and the helper already calls
  `glBufferSubData` on it (`:6504`) — same buffer, same target, this is
  the proven pattern, just a non-zero offset.
- Interaction with eager-create: BENIGN. The buffer is created at
  `start()` before any recipe bakes; `s_lightDataSsbo != 0` always holds
  by the time a bake-write fires (the bake happens during gameplay
  update, far after `start()`).
- Interaction with `kLightUploadFloor`: the floor only affects the
  per-frame whole-buffer upload SIZE, not slot writes. A static slot
  `j < S` and `S` is itself <= count, so `max(count,64)` always covers
  the static prefix. No conflict — but see the clobber hazard below.
- Capacity / grow ordering: a slot-write must be preceded by a buffer
  large enough. The static prefix grows with `S`; the buffer grows via
  the per-frame whole-buffer upload's grow branch (`:6489`). **Ordering
  rule (plan must enforce):** on a bake that pushes `S` past current
  `lightDataStructuresCapacity`, the C++ `lightData_` realloc
  (`txmmgr.cpp:1271`) and the next whole-buffer upload grow the SSBO
  FIRST; only then is the slot-write in-bounds. The `off+slotBytes >
  s_lightDataSsboBytes` guard above makes an early write a safe no-op
  (it will be covered by the mirrored CPU value on the next whole-buffer
  upload — see the clobber resolution, which makes this self-healing).

### THE CENTRAL CORRECTNESS HAZARD: per-frame whole-buffer upload
### clobbers the persistently-written static slots

`txmmgr.cpp:1676` re-uploads `lightData_[0 .. max(count,64))` from CPU
memory EVERY frame. If a static slot lives ONLY in GPU memory (written by
`gos_LightDataSsbo_WriteSlot` but absent from CPU `lightData_[j]`), the
next frame's whole-buffer `glBufferData`/`glBufferSubData` from
`lightData_` **overwrites slot `j` with stale/zero CPU bytes** — black
or wrong static lighting from frame 2.

Two candidate resolutions:

**(R-A) Scope the per-frame whole-buffer upload to `[S .. count)`
only.** Change `txmmgr.cpp:1676` to upload from offset `S*sizeof`,
length `(count-S)*sizeof`. Rejected: (1) it breaks the
`kLightUploadFloor` over-count tolerance contract (`:1664-1672`,
load-bearing, explicitly falsified-as-removable and KEPT) — the floor
assumes a contiguous `[0,max(count,64))` upload so cull-stale indices
read valid memory; carving `[0,S)` out changes that invariant and
re-arms the black-props regression the floor was kept to prevent;
(2) it adds dynamic-region offset arithmetic to the hottest upload site.

**(R-B) Mirror static slots in CPU `lightData_[0 .. S)` too (CHOSEN).**
The bake-write writes BOTH: (1) `lightData_[j] = baked` (CPU mirror) and
(2) `gos_LightDataSsbo_WriteSlot(j, &baked, sizeof)` (GPU immediate).
Then the per-frame whole-buffer upload re-uploading `[0, max(count,64))`
from `lightData_` is a **harmless idempotent rewrite of the same bytes**
— the CPU mirror holds the identical baked constant, so the per-frame
upload neither clobbers nor diverges. `resetLightData` sets count=`S`
(NOT 0) so it never zeroes the mirror; the static prefix `[0,S)` of
`lightData_` is simply never touched by the per-frame dynamic allocator.

**Why R-B is correct and minimal:**
- The CPU mirror is written ONCE per recipe (bake / invalidate), not per
  frame — the per-frame WRITE (the retired zone) is gone.
- The GPU immediate write (`gos_LightDataSsbo_WriteSlot`) makes the slot
  valid the SAME frame it bakes, before the next whole-buffer upload —
  no one-frame black flash.
- The per-frame whole-buffer upload still runs unchanged (no
  `kLightUploadFloor` contract change, no offset math at the hot site) —
  it just re-ships bytes that already match. This is the SAME idempotence
  the SSBO slice's `glBufferSubData(...,0,bytes,...)` already relies on.
- Self-healing under the grow-ordering edge: if a slot-write is skipped
  by the in-bounds guard (buffer not yet grown), the CPU mirror still
  holds the value and the next whole-buffer upload (post-grow) ships it.
  The immediate GPU write is a latency optimization, not a correctness
  requirement, once the CPU mirror exists.
- RAM cost: zero new — `lightData_[0,S)` already exists (it is the same
  array the per-frame path appends into); R-B just stops zeroing the
  prefix and stops re-appending the same structs. Per
  `feedback_ram_cost_not_a_concern_below_500mb.md` even a notional cost
  would be irrelevant; here it is genuinely net-neutral-to-negative
  (fewer per-frame appends).

**Resolved: R-B. The per-frame whole-buffer upload is NOT clobbering
because the static prefix is CPU-mirrored; the GPU immediate write is a
same-frame-validity optimization layered on the authoritative CPU
mirror.** This is the central design decision of the slice.

### CONCURRENCY NOTE (plan must flag)

`gos_LightDataSsbo_WriteSlot` is added to
`GameOS/gameos/gameos_graphics.cpp` — a file a concurrent water-only
session also edits (per the worktree's "Known issues": water fast-path /
full-GPU rewrite touches this TU). The plan MUST: (a) keep the new
function a minimal, self-contained append directly after
`gos_LightDataSsbo_Upload` (`:6508`), touching NO existing function body;
(b) flag the merge-coordination risk explicitly and recommend the water
session run in its own worktree, or that this slice and the water slice
serialize through the active branch with a rebase checkpoint. The
addition is ~10 lines, no shared-state edit beyond reading the existing
file-scope `s_lightDataSsbo`/`s_lightDataSsboBytes` statics
(`:6466-6467`) — minimal footprint, but the TU collision is real.

---

## Q3. The CPU-zone-death proof

### What this slice takes to zero

For the static (bldg/tree) class, `mc2SubmitBakedLightSlot`
(`txmmgr.cpp:1236`) currently calls `addLightDataStructure`
(`:1242`) once per recipe per frame (after the per-frame
`s_bakedSlotByRecipe.clear()` at `:1385`). Under this slice, the bake
path sets `cachedGpuLightIndex_ = permanentSlot` ONCE (at bake /
invalidate); the per-frame helper early-returns (the slot is already
permanent, `cachedFrame_` re-stamped O(1)). `mc2SubmitBakedLightSlot` is
**not called at all** for the static class -> its
`addLightDataStructure` call vanishes.

### The armed counter and the named residual

`addLightDataStructure scan` Tracy zone (`txmmgr.cpp:1252`) is the armed
lever (no new instrumentation). Today ~1840 calls/frame, ~0.97 ms/frame
bake-ON. The slice drops the static contribution to **0**. The residual
`addLightDataStructure` calls come from the surviving callers:

- `addLightDataStructureWithPerActorColor` (`txmmgr.cpp:1302`) — three
  internal paths each ending in `addLightDataStructure`:
  - `:1316` no-actor-light passthrough (generic props /
    `genactor SetLightList(NULL,0)` -> `actorLightSource==0xFFFFFFFFu`
    `:1313`).
  - `:1358` the `s_lbRepointEnabled` populate-miss append (mech / C2
    dynamic-lit actors whose template+actorARGB key is new this frame).
  - `:1369` the repoint-disabled fallback append.
- These are reached by **mechs** (`mech3d.cpp` calls `CacheGpuLightData`
  directly, never the bake helper — confirmed: bake helper is only at
  `bdactor.cpp:2522/2565/4956/4977`) and **generic props** (no-actor
  path). That is the **genuine dynamic remainder**: mech per-frame light
  (the D2 floor) + generic-prop no-actor light + any C2 dynamic-lit
  actor. It is NOT a hidden static path — the four static call sites all
  route through `mc2CacheOrBakeStaticGpuLight` (`bdactor.cpp:2217`)
  which, post-slice, never reaches `mc2SubmitBakedLightSlot` (HIT path
  sets the permanent slot once; MISS path still gathers on frame 1 /
  post-invalidate only, then bakes — frame-1 transient, not steady
  state).

Per `feedback_offload_must_be_substitutive_not_additive.md`: D-B retired
the *recompute* (`[LIGHTBRIDGE v1]` populate ~0 for static — already
done). THIS slice is the one that finally takes the `addLightDataStructure
scan` zone to ~(dynamic-only) for the static class — the FNV+memcmp and
the per-frame re-insertion are designed OUT, not relocated. Done =
`addLightDataStructure scan` calls/frame drops from ~1840 to the
mech/generic-prop residual; the static recipes contribute ZERO calls.

---

## Q4. Invalidation + cull-gate + lifetime

### The once-write path replaces the per-frame emit

Current per-frame emit chain (steady state, bake HIT):
`mc2CacheOrBakeStaticGpuLight` (`bdactor.cpp:2229-2230`) ->
`shape->EmitBakedGpuLightData` (`msl.cpp:1953`) ->
`mc2SubmitBakedLightSlot` (`txmmgr.cpp:1956`/`:1236`) -> per-frame
`addLightDataStructure`.

Post-slice:
- **Bake-write (once, at MISS in `mc2CacheOrBakeStaticGpuLight`
  `bdactor.cpp:2231-2242`):** after the snapshot
  `mc2SetBakedStaticLight(recipeIndex, *leaf)` (`:2242`), additionally:
  (1) `permanentSlot = recipeIndex` (direct map, Q1); (2) write
  `lightData_[permanentSlot] = *leaf` (CPU mirror, R-B); (3)
  `gos_LightDataSsbo_WriteSlot(permanentSlot, leaf, sizeof)` (GPU
  immediate); (4) `S = max(S, permanentSlot+1)`. This is the same
  cull-gated call site (`bdactor.cpp:2522/2565/4956/4977`, all inside
  `inView || g_useGpuStaticProps` — unchanged by this slice).
- **HIT path (`bdactor.cpp:2229-2230`):** instead of
  `EmitBakedGpuLightData` -> per-frame slot, set
  `cachedGpuLightIndex_ = permanentSlot` and `cachedFrame_ =
  g_mc2FrameCounter` directly (O(1), no `addLightDataStructure`). The
  trailing `staticReg.lightDataIndex = shape->getCachedGpuLightIndex()`
  capture (`bdactor.cpp` per-instance, unchanged) ferries the permanent
  slot through `markVisible(regIdx, lightDataIndex)`
  (`gos_static_prop_registry.cpp:287`, `:296`) exactly as today — the
  batcher reads `cachedGpuLightIndex_` (`gos_static_prop_batcher.cpp:2538-2540`)
  / `rng.lightDataIndex` (`registry:403-404`) with NO change. The slot
  value is now permanent instead of per-frame, but the ferry semantics
  are byte-identical.

### Cull-gate adjacency (load-bearing)

`cull_gates_are_load_bearing.md`: the once-write happens at the SAME
gated call site the per-frame emit used (`bdactor.cpp:2522/2565/4956/4977`,
inside `inView || g_useGpuStaticProps`). A baked struct is written for an
actor only when it first passes the gate (frame-1 MISS). The HIT path
(permanent slot already set) is also gated — it just re-stamps
`cachedFrame_`. The batcher/ferry path
(`markVisible`/`flush`/`gos_static_prop_batcher.cpp:2538`) is UNCHANGED,
so the cull cascade is unchanged. The slice does NOT reach past any gate.

### Invalidation (destruction / LOD swap)

`GpuStaticPropRegistry::invalidate(regIdx)` (`gos_static_prop_registry.cpp:300`)
already calls `mc2EraseBakedStaticLight(regIdx)` (`:313`). Post-slice,
extend `mc2EraseBakedStaticLight` (`txmmgr.cpp:1216`) to ALSO zero the
permanent slot mirror (`lightData_[recipeIndex]` -> default
`TG_HWLightsData`) and the GPU slot
(`gos_LightDataSsbo_WriteSlot(recipeIndex, &zero, sizeof)`), so a stale
constant is not read before the lazy re-bake. **recipeIndex is
monotonic-never-reused** (Q1: `registry:251-252`, invalidate tombstones
count=0 `:308`, never frees the index): a post-invalidate re-bake gets a
FRESH recipeIndex -> a fresh permanent slot -> **no stale-slot
aliasing**. The OLD slot is zeroed and never read again (its actor is
destroyed; its `rng.count==0` tombstone -> `markVisible` early-returns
`:291`). `mc2ClearAllBakedStaticLight` (`txmmgr.cpp:1224`, mission
unload, called via `GpuStaticPropRegistry::destroy()`
`mission.cpp:3261`) additionally resets `S=0` (next mission's
recipeIndex restarts at 0; the prefix is rebuilt fresh).

### nightFactor / lifetime caveat (carry verbatim)

Bake validity assumes `eye->nightFactor` mission-constant
(`lighting_is_mission_load_static_no_dynamic_emitters.md`). Unchanged by
this slice (it changes WHERE the constant lives, not WHAT it is). Name it
the documented revisit trigger, same as the SIMPLIFIED plan B4/#6.

---

## Q5. Biggest risks ranked + Option-A-complexity check

Ranked:

1. **(a) Per-frame whole-buffer upload clobbers static slots (Q2).**
   THE central hazard. Resolved by R-B (CPU-mirror the static prefix so
   the per-frame upload is an idempotent rewrite of identical bytes; GPU
   immediate write is a same-frame-validity optimization on top). This
   resolution also preserves the load-bearing `kLightUploadFloor`
   over-count contract (`txmmgr.cpp:1664-1672`) untouched — the reason
   R-A (scope upload to `[S..)`) is rejected. Residual risk: the
   `resetLightData` count=`S` change (`txmmgr.cpp:1378`) is a load-bearing
   edit; adversarial review MUST verify the dedup/template/actor-key maps
   only ever indexed `>= S` appends (they `clear()` every frame so this
   holds by construction, but the negative claim needs an opposite-
   direction grep: confirm NOTHING reads `lightData_[0..S)` via a slot
   the per-frame allocator handed out).
2. **(b) `gameos_graphics.cpp` concurrent-session edit.** Real TU
   collision with the water session. Mitigated by minimal-footprint
   append (one ~10-line function after `:6508`, no existing-body edit)
   + explicit plan flag recommending worktree isolation or a serialized
   rebase checkpoint. MAJOR (coordination), not CRITICAL (correctness).
3. **(c) recipeIndex-as-direct-index density / size.** ~27 MB worst
   case (15000 * 1808 B) vs >=128 MB `GL_MAX_SHADER_STORAGE_BLOCK_SIZE`
   floor — comfortable. Holes (non-baked recipes) waste memory but cost
   nothing else; compaction map rejected as unjustified complexity
   (`feedback_ram_cost_not_a_concern_below_500mb.md`). LOW.
4. **(d) Genuinely simpler than the STOP-THE-LINE Option-A?** YES,
   verified. Option-A's complexity layer (rescope doc Q2, lines 126-171)
   was: fixed `[0..S)` partition + per-window static dedup + self-bound
   fallback-to-D2 + partition-full canary + abandoned-slot exhaustion
   bound + `S`-sizing BLOCKING measurement pre-gate + count-base `0->S`
   rebase + the ">64 lockstep UBO->SSBO" hard dependency. EVERY one of
   those existed ONLY to survive the 64-slot UBO window — which shipped
   away as `b41baec`. This slice has: a static prefix `[0,S)` that
   `resetLightData` doesn't clear (count=`S` not 0), a direct
   `recipeIndex` slot, a CPU mirror, one new `glBufferSubData`-slot
   helper. **NO partition-size measurement gate** (no `S` ceiling — the
   SSBO is unbounded; `S` just grows). **NO fallback-to-D2** (no
   overflow possible). **NO self-bound/canary/exhaustion bound** (no
   finite window). **NO count-base rebase across multiple sites** —
   `resetLightData` count=`S` is ONE line, and the co-reset invariant
   (`txmmgr.cpp:1379-1381`) stays VERBATIM (the maps still `clear()`;
   they only ever held dynamic appends). Adversarial review MUST verify
   none of the Option-A machinery creeps back in (the rescope doc lines
   27-36 / 426-432 enumerate the deleted list — it is the review
   checklist).

### C++-only or +1 shader?

**C++-only.** Confirmed: the shader reads `light[absolute]`
(`static_prop.vert:306`, `mech.vert:162`, `lighting.hglsl:53-55`
unbounded) with no base/region — a permanent static slot is read
identically to a per-frame slot, exactly as the UBO->SSBO slice
established (`lighting.hglsl:43-50`). **Zero `shaders/` edit.**
Deploy-lockstep (`shader_exe_deploy_lockstep.md`) does NOT trigger;
deploy is exe-only (still deploy the rebuilt exe). Adversarial review
must verify no `shaders/` file is in the diff.

---

## Summary (what dies vs survives vs is new)

- **DIES:** the per-frame `mc2SubmitBakedLightSlot` ->
  `addLightDataStructure` call for the static class (`txmmgr.cpp:1242`)
  — 1808 B FNV + 1808 B memcmp + slot append, ~1840 calls/frame
  contribution. The static slot is written ONCE (bake/invalidate),
  persists.
- **SURVIVES:** the bake skeleton (`s_bakedStaticLight`,
  `mc2CacheOrBakeStaticGpuLight` chokepoint, recipeIndex key,
  `firstShapeNodeLeaf->lightData_` snapshot, invalidate hook, cull-gate
  adjacency, the bdg/tree-vs-mech-vs-genactor split, nightFactor
  assumption); the batcher/ferry path (UNCHANGED — `cachedGpuLightIndex_`
  now permanent but ferried identically); the per-frame whole-buffer
  upload (UNCHANGED — idempotent over the CPU-mirrored prefix); the
  `kLightUploadFloor` contract (UNCHANGED); the dynamic D2 path for
  mechs/generic props (the genuine residual).
- **NEW (in-scope dependency):** `gos_LightDataSsbo_WriteSlot` slot-scoped
  `glBufferSubData` writer in `gameos_graphics.cpp` (~10 lines, after
  `:6508`); `resetLightData` count=`S` (not 0) (`txmmgr.cpp:1378`); the
  bake-write CPU-mirror + GPU-immediate-write + `S` high-water in
  `mc2CacheOrBakeStaticGpuLight` MISS path / `mc2EraseBakedStaticLight`
  zeroing; `S` reset in `mc2ClearAllBakedStaticLight`.
- **NOT reintroduced (Option-A deleted layer):** no partition-size gate,
  no fallback-to-D2, no self-bound canary, no abandoned-slot exhaustion
  bound, no count-base multi-site rebase, no compaction map, no shader
  change, no window of any kind.

*Recon only. No code written. All citations grep-verified at `8c1c491`.*
