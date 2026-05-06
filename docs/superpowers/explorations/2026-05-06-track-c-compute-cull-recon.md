# Track C — GPU Compute Cull + Async-Readback Feedback (Recon Zero)

**Date:** 2026-05-06
**Mode:** Read-only recon. No code changes, no build, no smoke.
**Scope:** Architectural-endpoint recon for the GPU compute cull + async-readback
feedback track of the MC3 Rendering Modernization Roadmap.
**Track context:** `docs/superpowers/mc3-rendering-modernization-roadmap.md`
Track C ("the killshot," lines 128-156). NOT to be confused with the
**Lua-scripting** Track C audit at
`docs/superpowers/specs/2026-04-30-track-c-implementation-readiness-audit.md`,
which is a different track sharing the letter — naming collision noted.

> Required reading consumed: roadmap (full), `cull_gates_are_load_bearing.md`,
> `gpu_direct_renderer_bringup_checklist.md`, `feedback_data_flow_audit_asymmetry.md`,
> `docs/architecture.md`, `docs/amd-driver-rules.md`. All file:line citations
> below were grep-verified at write time per the "Documentation Discipline"
> rule in CLAUDE.md.

---

## 1. TL;DR

- **Compute shader infrastructure does not exist in the engine.** Zero
  `glDispatchCompute`, zero `GL_COMPUTE_SHADER`, zero `layout(local_size_x` in
  `shaders/`, `GameOS/`, `mclib/`, or `code/` (only the GLEW/SDL header
  declarations exist in `3rdparty/`). Track C is the first compute path.
- **All other Track C primitives are already in-tree as working precedents.**
  `glFenceSync`/`glClientWaitSync`, `GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT`,
  ring-buffered persistent-mapped SSBOs, indirect-draw plumbing, std430 layout
  asserts, AMD-7900-XTX-validated patterns — all shipped. The only missing
  piece is the compute path itself (shader infra, std430 in/out SSBOs for
  visibility, the dispatch site).
- **`glMultiDrawElementsIndirectCount` is loadable but the context will need
  to be bumped to 4.6 (or `GL_ARB_indirect_parameters` queried as an
  extension at 4.3).** The engine currently requests 4.3 core
  ([`gos_render.cpp:184-188`](../../../GameOS/gameos/gos_render.cpp:184)).
  AMD 7900 XTX supports up to 4.6, so the bump is mechanical.
- **The async-readback hypothesis works for `Terrain::objBlockInfo[].active` /
  `objVertexActive[]` (the load-bearing cull gates), with one caveat — see §5
  and §13.** The producer (terrain vertex projection at `terrain.cpp:1479-1483`)
  and the consumer (`objmgr.cpp:1760` `update()` gate) sit in different phases
  of the same frame; the gap is wide enough that a 1-frame async-readback can
  slot in without disturbing object lifecycle.
- **There IS a sync-frame-N consumer of `inView` that the async-readback model
  does NOT cleanly serve: gameplay node-position queries (weapon/smoke/dust
  node spawn locations) early-out on `!inView`** — see §5 row "weapon/smoke/dust
  node queries." This is a minor visual artifact (one frame of weapons firing
  from object root after a mech becomes visible), not a gameplay-critical
  correctness break. **Track C should still be ~2 weeks**; the artifact is
  documented, not blocking.

---

## 2. Compute shader infrastructure status

**Verdict: missing entirely. New surface to add.**

Negative-claim grep evidence (per `feedback_data_flow_audit_asymmetry.md`):

```
$ Grep "glDispatchCompute|GL_COMPUTE_SHADER|local_size_x" \
       --glob '!3rdparty/**'
→ Zero matches in code/, mclib/, GameOS/, shaders/.
   The only matches are in 3rdparty/include/SDL2/SDL_opengl_glext.h and
   3rdparty/include/GL/glew.h — the loader-side declarations only.
```

What this means: the engine's existing shader build path
(`makeProgram()` with VS/FS pairs, e.g.
[`shaders/gos_terrain_thin.vert`](../../../shaders/gos_terrain_thin.vert))
has no compute-shader counterpart. Adding compute requires:

1. A compute-shader compile path. The existing `makeProgram` accepts a
   `"#version 430\n"` prefix — same prefix is fine for compute shaders, since
   `local_size_*` and `gl_GlobalInvocationID` are 4.3-core. The new function
   would mirror `makeProgram` but take a single CS source and link
   `GL_COMPUTE_SHADER` only.
2. A compute-program object and dispatch site. New file or section in
   `gameos_graphics.cpp` / `gos_postprocess.cpp` — TBD where it slots best.
3. Memory-barrier discipline. Any compute output the next stage reads
   (indirect draw command buffer, or visibility SSBO read by a vertex shader)
   needs `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT |
   GL_COMMAND_BARRIER_BIT)` after dispatch.

**Cost estimate:** ~150-250 LoC of plumbing (compile path + program object +
dispatch wrapper + memory-barrier). Mechanical, no design risk — every other
modern engine has this exact shape.

**No `dispatch_compute_validate` vs `dispatch_compute_release` build divergence
exists** — the engine compiles a single config, so compute-shader compile
errors must surface via the existing `[INSTR v1]` startup banner / shader-error
paths.

---

## 3. `glMultiDrawElementsIndirectCount` availability

**Verdict: loadable via GLEW; needs context bump 4.3 → 4.6 OR runtime extension query.**

GLEW already exposes the entry point at
[`3rdparty/include/GL/glew.h:2545-2546`](../../../3rdparty/include/GL/glew.h:2545):

```c
#define glMultiDrawArraysIndirectCount   GLEW_GET_FUN(__glewMultiDrawArraysIndirectCount)
#define glMultiDrawElementsIndirectCount GLEW_GET_FUN(__glewMultiDrawElementsIndirectCount)
```

And `GL_ARB_indirect_parameters` is wrapped at
[`glew.h:5430-5446`](../../../3rdparty/include/GL/glew.h:5430).

The current GL context request is **4.3 core** at
[`gos_render.cpp:184-188`](../../../GameOS/gameos/gos_render.cpp:184), with
the comment explicitly noting "4.3 is the minimum feature level we need."
`glMultiDrawElementsIndirectCount` was promoted to **4.6 core** (or
`ARB_indirect_parameters` extension on earlier contexts).

Two options:

- **A. Bump context to 4.6.** Mechanical. AMD 7900 XTX driver 26.3.1 supports
  up to 4.6 (per `amd-driver-rules.md` header). Risk: any 4.3-strict
  validation lurking in third-party code would surface; nothing in the
  worktree currently caps below 4.6 deliberately.
- **B. Keep 4.3, query `GLEW_ARB_indirect_parameters` at runtime** and treat
  `IndirectCount` as a feature-flag. Slightly more code, opt-out path on
  hardware lacking the extension (irrelevant for our single-target build).

The existing `glMultiDrawArraysIndirect` (NOT the `Count` variant) **is
already shipped in production** — used by `gos_terrain_indirect.cpp:2423`
([`gameos_graphics.cpp:2423`](../../../GameOS/gameos/gameos_graphics.cpp:2423))
and the related thin VS path. So the 4.3-core form is proven on AMD 7900
XTX. The `Count` variant adds only the GPU-written `drawCount` parameter.

**Recommendation:** Option A (bump to 4.6) — simpler, no runtime branch.

---

## 4. Async-readback / fence infrastructure

**Verdict: fully shipped pattern; mirror, don't invent.**

Existing fence-sync sites (`glFenceSync` + `glClientWaitSync` ring buffers):

- [`GameOS/gameos/gos_static_prop_batcher.cpp:248,1251,1667`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:248) —
  `s_fence[s_frameSlot]`, ring of `RING_FRAMES` slots, with
  `GL_SYNC_FLUSH_COMMANDS_BIT` wait + `GL_SYNC_GPU_COMMANDS_COMPLETE` post.
- [`GameOS/gameos/gos_terrain_indirect.cpp:1209,1496`](../../../GameOS/gameos/gos_terrain_indirect.cpp:1209) —
  `g_thinRingFences[g_thinRingSlot]`, indirect-terrain thin record path.
- [`GameOS/gameos/gos_terrain_patch_stream.cpp:629,1513`](../../../GameOS/gameos/gos_terrain_patch_stream.cpp:629) —
  patch-stream ring fences.

Persistent-mapped buffer pattern (`GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`):

- [`gos_static_prop_batcher.cpp:261`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:261)
- [`gos_terrain_patch_stream.cpp:269-295`](../../../GameOS/gameos/gos_terrain_patch_stream.cpp:269)

This is **the** template for the readback-ring. For compute-cull readback,
the only twist is GPU-writes-then-CPU-reads (vs the existing CPU-writes-then-
GPU-reads in patch_stream). The flag set is the same:
`GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` for the
readback buffer; storage-flag identical pattern.

**Cost estimate:** ~50-80 LoC for the readback-ring class (3-frame ring,
fence-per-slot, mapped-pointer accessor). Pure mirror of patch-stream.

---

## 5. `inView` consumer table — exhaustive, sync/async classification

The grep covered `\binView\b` across all of `code/` and `mclib/` (excluding
docs and 3rdparty). Each row below cites file:line and the consumer's
synchronicity requirement.

**Producer-side (write):** `appearance->recalcBounds()` is called in many
sites (43 occurrences across 14 files in `code/`). The canonical write path
is `XAppearance::recalcBounds()` body in
[`actor.cpp:280-343`](../../../code/actor.cpp:280),
[`mech3d.cpp:2076-2362`](../../../mclib/mech3d.cpp:2076),
[`gvactor.cpp:1555-1895`](../../../mclib/gvactor.cpp:1555),
[`bdactor.cpp:1103-1494,3893-4199`](../../../mclib/bdactor.cpp:1103),
[`genactor.cpp:590-773`](../../../mclib/genactor.cpp:590).

**Storage:** `bool Appearance::inView` at
[`mclib/appear.h:71`](../../../mclib/appear.h:71); accessor
`canBeSeen()` returns it at [`appear.h:171`](../../../mclib/appear.h:171),
mutator `setInView()` at [`appear.h:175-176`](../../../mclib/appear.h:175).

| Consumer site | Use | Sync vs async-OK |
|---|---|---|
| `objmgr.cpp:1693-1702` — `inView_instr() \|\| canBeSeen_instr() \|\| blockActive_instr()` for `framesSinceActive` accumulator | Stability instrumentation only. Saturating uint8 counter. | **Async-OK.** Lag = ±1 frame on a counter that saturates at 255. |
| `mech3d.cpp:721,759,795,833,5099,5136` — `getWeaponNodePosition`, `getNodeNamePosition`, `getNodeIdPosition`, `getNodePosition` early-out on `!inView` | Gameplay node-position queries. Caller in `mech.cpp:5339` consumes for **weapon-bolt spawn position**. | **⚠️ SYNC-FRAME-N for gameplay correctness.** With async-lag: a mech that just became visible spawns its weapon bolt at object root for one frame instead of the proper hardpoint node. **Visual artifact, not gameplay-breaking** — bolt then flies the right direction next frame; no AI/damage calc consumes node positions. Documented risk; does not block Track C. |
| `gvactor.cpp:451,500,533` — `getWeaponNodePosition`, `getSmokeNodePosition`, `getDustNodePosition` early-out on `!inView` | Same gameplay-query pattern as mech3d. Caller in `gvehicl.cpp:2524`. | **⚠️ SYNC-FRAME-N for gameplay correctness.** Same artifact class as above. |
| `mech3d.cpp:2055-2069` (`update()` body) — `if (inView) { ... }` block | Per-frame appearance update. | **Async-OK.** This is the gate that Track C explicitly aims to flip — `inView_GPU(frame N-1)` feeds this gate. |
| `mech3d.cpp:2377` — `if (inView \|\| g_useGpuStaticProps) { transformGeometry(); }` | TGL-pool allocation guard. Cull gate that makes silent-pool-exhaustion safe. | **Async-OK** with the cull cascade preserved. (Roadmap §C explicitly preserves the GATE; only its INPUT moves to GPU.) |
| `mech3d.cpp:2881` — `if (inView && visible) { ... }` | Render-time path. | **Async-OK** — render uses the same cull semantics; 1-frame lag = 1 frame of pop-in/pop-out at the visibility boundary. Matches existing pop-in latency (per roadmap risk register). |
| `mech3d.cpp:3740,3797` — animation-state branches | Anim-state selection. | **Async-OK.** |
| `mech3d.cpp:4180-4196` — `if (turn < 3 \|\| inView \|\| ... \|\| g_useGpuStaticProps) updateGeometry()` then forced `inView=true` for `baseRootNodeHeight` init | First-frame init special case + `g_useGpuStaticProps` bypass. | **Async-OK** — initialization runs at `turn < 3` regardless of `inView`. |
| `gvactor.cpp:1946,1982,2022,2039,2711` — render/update gates | Same family as mech3d. | **Async-OK.** |
| `bdactor.cpp:1082-1494,1561,2012,2112,2258,2268,3872-4199,4205,4208,4454,4555,4570` — Building/TerrainObject gates | Static-prop family. Already serves GpuStaticPropRegistry via `\|\| g_useGpuStaticProps`. | **Async-OK.** This is exactly what GpuStaticPropRegistry has been validating in soak. |
| `genactor.cpp:569-1204` — Generic-actor gates (fences, artillery towers) | Same. | **Async-OK.** |
| `terrobj.cpp:610-698` — TerrainObject::primeAppearanceForMissionLoad + update body | Mission-load + per-frame. | **Async-OK** for per-frame; mission-load is one-shot before any compute dispatch. |
| `bldng.cpp:791-805,1066,1071` — Building::update + render | Static prop. | **Async-OK.** |
| `gate.cpp:335-355,596,599` — Gate::update + render | Static prop. | **Async-OK.** |
| `artlry.cpp:870,1183-1242,1334,1407,1746,1764` — Artillery towers | Includes save/load (1746,1764) — `data->inView = inView` for savefile chunk. | **Async-OK** for runtime; save/load is a freeze-frame snapshot — value at that frame is fine. |
| `turret.cpp:575-808,2034,2048` — Turret update / render / canBeSeen consumers | Same. | **Async-OK.** |
| `gvehicl.cpp:3183-3736` — Ground-vehicle update body, `recalcBounds` polled multiple times | Standard vehicle. | **Async-OK.** |
| `mover.cpp:3470-3471` — `getLOSPosition` saves `oldInView`, forces `setInView(true)`, queries node, restores. | **Sensor-pipeline LOS calculation.** Gameplay-critical. | **Already explicitly forces `inView=true`** before the query — this consumer **doesn't read `inView` as a real gate**; it bypasses it entirely. **Async-OK** because the consumer pre-flips the bit. |
| `mech.cpp:6038-6184` — Player-mech update wraps recalcBounds for HUD/audio gating | UI/audio-only side-effects. | **Async-OK.** |
| `gvehicl.cpp:3261,3312,3473,3477,3699` — Vehicle internal recalcBounds checks | Internal animation/withdrawal logic. | **Async-OK.** |
| `weaponbolt.cpp:526-1601` — Weapon bolt's own `inView` (LOCAL variable, not Appearance::inView). | Local `bool inView` for whether weapon-bolt visual itself is on-screen. | **Not the Appearance::inView — different symbol.** Out of scope for Track C. |
| `light.cpp:123-124` — `bool inView = onScreen(); lightAppearance->setInView(inView)` | Dynamic-light visibility. Drives light update. | **Sync-frame-N producer side, but the value FEEDS Appearance::inView.** Track C feeds GPU output through; the producer here is `light->onScreen()` not `recalcBounds()`. Special case: lights need their own classification (out of static-prop registry scope). |
| `clouds.cpp:212,222` — Cloud vertex `clipInfo = onScreen && inView` (LOCAL `bool inView` from `projectForEffectAdmission`) | Local cloud-vertex variable. | **Not Appearance::inView.** Out of scope. |
| `terrain.cpp:1430-1452,1485,1590-1632` — Terrain vertex projection. **`bool inView` LOCAL variable** populated by `eye->projectForTerrainAdmission()`. Sets `objBlockInfo[].active` and `objVertexActive[]`. | **Producer for the entire object-cull cascade.** | **This is what Track C's compute shader replaces.** Sync requirement of downstream consumers covered in §6. |

**Summary of async-OK vs sync-frame-N consumers:**

- **Async-OK (1-frame lag harmless):** every visibility/render/transform gate.
  ~95% of all consumers. The cull cascade is async-tolerant by design (the
  whole point of the cascade is "don't pay update cost for offscreen objects"
  — 1-frame stale offscreen is still safely offscreen for budget purposes).
- **Sync-frame-N risk:** node-position queries on mechs/GVs that just became
  visible. Visual artifact only — one frame of weapon bolts spawning at root.
  Mitigation if needed: keep CPU `recalcBounds` as a cheap fallback for
  these specific accessors only (treat them as sync-frame-N readers reading
  CPU's inView, while the rest of the consumers read GPU's frame-N-1
  visibility). Cost: zero — `recalcBounds` already runs.
- **Not actually `Appearance::inView`:** `weaponbolt.cpp`, `clouds.cpp`,
  `terrain.cpp` use local `bool inView` symbols. Out of Track C scope.

---

## 6. `canBeSeen` / `objBlockInfo.active` / `objVertexActive` consumer table

### `canBeSeen()` — alias for `Appearance::inView` accessor

Defined at [`appear.h:171`](../../../mclib/appear.h:171) (`return inView;`).
All consumers read the same backing field as `inView`, so the sync/async
classification is identical to §5. Notable callsites:

| Site | Use | Class |
|---|---|---|
| `bldng.cpp:1071`, `gate.cpp:599`, `terrobj.cpp:799` — `if (canBeSeen() \|\| g_useGpuStaticProps)` | Render gate, mirror of `inView`. | Async-OK. |
| `artlry.cpp:1407`, `turret.cpp:2034,2048` — render/destroy gates | Static-prop family. | Async-OK. |
| `gvehicl.cpp:3928,3936`, `mech.cpp:6448,6466,6497` — combat AI conditional ("if I can be seen, fire/play sound/attack") | **Combat AI consumes `canBeSeen` for firing-decision logic** — `if (attackRange == FIRERANGE_CURRENT && !isDisabled() && appearance->canBeSeen())` at `mech.cpp:6497`. | **⚠️ SYNC-FRAME-N for gameplay AI.** With 1-frame lag, an enemy that just became visible may not return fire for one frame — minor gameplay artifact, not correctness break (next frame fires). Documented risk; mitigation: same as §5 — keep CPU `recalcBounds` value available for combat AI specifically while render path uses GPU N-1. |
| `objmgr.cpp:1695,2173,2242,2285` — `canBeSeen_instr()` for stability instrumentation + render ordering | Active-block accumulator + sort. | Async-OK. |
| `mover.cpp:3470` — `oldInView = canBeSeen()` then forces true | Pre-flips the bit. | Async-OK. |

### `objBlockInfo[].active` (per-block active gate)

**Storage:** `static ObjBlockInfo* Terrain::objBlockInfo` at
[`terrain.h:179`](../../../mclib/terrain.h:179),
allocated in [`terrain.cpp:484-487`](../../../mclib/terrain.cpp:484), size
`numObjBlocks` (a fixed mission-load value).

**Producer:** [`terrain.cpp:1479`](../../../mclib/terrain.cpp:1479)
inside `Terrain::geometry()` (the per-vertex projection loop):
`objBlockInfo[blockNum].active = true` when any vertex with that
`blockNumber` passes the cull. Cleared at frame start by
`Mission::update`'s `land->clearObjBlocksActive()` at
[`mission.cpp:496`](../../../code/mission.cpp:496).

**Consumers:**

| Site | Use | Class |
|---|---|---|
| `objmgr.cpp:1493,1624,1760,2329,2779` — `if (Terrain::objBlockInfo[terrainBlock].active) { ...iterate objects in block... }` | **Drives `update()` and `render()` iteration.** Per `cull_gates_are_load_bearing.md` Trap #1 + #2: skipped blocks → no `update()` call → no destroy → safe. **But:** `update()` returning false → `setExists(false)` → object permanently destroyed. The cascade safety relies on a CONSISTENT producer — if frame N-1's GPU bit says block X is active but the actual objects in X have already moved offscreen, force-running `update()` could trigger their destroy via stale state. | **The sync requirement is subtle.** A 1-frame-stale "active=true" for a block whose objects moved out of view is OK — `update()` runs, computes nothing damaging (objects don't suddenly fail their internal sanity checks because they're not visible). **A 1-frame-stale "active=false" is the more interesting case** — for a frame, `update()` doesn't run for objects that should be active. For mechs/GVs this is the existing pop-in latency. For `update()`-returns-false destroy paths, the destroy just happens one frame later. **Async-OK** with the cascade preserved. |
| `objmgr.h:423-435` — accessors that index `objBlockInfo[blockNumber]` for `firstHandle` / `numObjects` / `numCollidableObjects` | Read shape data. **NOT the `.active` field** — these are mission-load-immutable counts. | N/A — no synchronicity concern. |
| `objmgr.cpp:278-280,859-909,3192-3196,3231` — initialization / load — `firstHandle`/`numObjects`/`active=false` reset | Mission-load setup. One-shot. | N/A. |

### `objVertexActive[]` (per-vertex active flag)

**Storage:** `static bool* Terrain::objVertexActive` at
[`terrain.h:181`](../../../mclib/terrain.h:181), size
`realVerticesMapSide²` (mission-load).

**Producer:** [`terrain.cpp:1483`](../../../mclib/terrain.cpp:1483) and
`:1917` and `:1923` (memset reset). Set true by the same vertex-projection
loop that sets `objBlockInfo[].active`.

**Consumers:**

| Site | Use | Class |
|---|---|---|
| `objmgr.cpp:1500,1631,1768,2786` — `objVertexActive[objList[objIndex]->getVertexNum()]` per-object gate inside the per-block iteration | Second cull gate (block AND vertex). | **Async-OK** with same cascade caveat as `objBlockInfo[].active`. |

**Verdict for §6:** All three flag sets are async-OK at the cascade level
(roadmap §C correctly preserved). The sync-frame-N risk is the **combat AI
gate** at `mech.cpp:6497` (and similar) — combat decisions read
`canBeSeen()` directly. Mitigation = keep CPU `recalcBounds` for the AI
side; let render/transform consumers read GPU N-1.

---

## 7. Persistent instance SSBO precedents

Two existing in-tree templates Track C should mirror:

### 7.1 Terrain dense-recipe SSBO (`gos_terrain_indirect`)

- **Recipe struct:**
  [`gos_terrain_patch_stream.h:87-99`](../../../GameOS/gameos/gos_terrain_patch_stream.h:87)
  `struct alignas(16) TerrainQuadRecipe` — 144 bytes (4 corners × pos +
  normal in `vec3+pad`, plus a UV rect). `static_assert(sizeof(...) == 144)`.
- **Container:** `std::vector<TerrainQuadRecipe> g_denseRecipes` at
  [`gos_terrain_indirect.cpp:228`](../../../GameOS/gameos/gos_terrain_indirect.cpp:228),
  sized to `mapSide²` (one slot per map vertex).
- **Indexing convention:** by **map-stable `vertexNum = mapY * mapSide +
  mapX`** — see comment block at
  [`gos_terrain_indirect.h:117`](../../../GameOS/gameos/gos_terrain_indirect.h:117)
  ("`vn (vertexNum) ∈ [0, mapSide²) → g_denseRecipes[vn] is the slot.`").
  This is the lesson from `gpu_direct_renderer_bringup_checklist.md` trap #8
  ("map-stable indexing") in production form.
- **Upload:** single `glBufferSubData` after build at line `:732`; no per-frame
  re-upload — recipe contents are mission-immutable.
- **Per-frame "thin record":** a separate ring-buffered SSBO emits per-frame
  `{recipeIdx, terrainHandle, flags, lightRGB[4]}` (32 bytes) at
  [`gos_terrain_patch_stream.h:103-111`](../../../GameOS/gameos/gos_terrain_patch_stream.h:103).

**Track C parallel:** mission-load builds a persistent **instance SSBO** keyed
by `instanceID ∈ [0, N)`. Per-frame compute emits a visible-instance ID list
+ a visibility bitmask. The instance SSBO itself never changes after
mission-load (transforms, AABB, baked light index — pre-baked once).

### 7.2 Static-prop instance SSBO (`GpuStaticPropRegistry` + Batcher)

- **Instance struct:**
  [`gos_static_prop_batcher.h:13-35`](../../../GameOS/gameos/gos_static_prop_batcher.h:13)
  `struct alignas(16) GpuStaticPropInstance` — **112 bytes**:
  - `float modelMatrix[16]` (offset 0, 64 B, mat4 row-major, GL_FALSE upload)
  - `uint32_t typeID` (64)
  - `uint32_t firstColorOffset` (68) — into per-frame color SSBO
  - `uint32_t flags` (72) — bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight
  - `uint32_t lightDataIndex` (76) — index into per-frame light-data UBO
  - `float aRGBHighlight[4]` (80, 16 B)
  - `float fogRGB[4]` (96, 16 B)
- `static_assert`s at lines 29-35 enforce std430 offsets.
- **Registry:**
  [`gos_static_prop_registry.h`](../../../GameOS/gameos/gos_static_prop_registry.h)
  is the lifecycle wrapper — `frameBegin()` / `markVisible()` / `flush()`.
  Called from `gamecam.cpp:201` (frameBegin) and `txmmgr.cpp:1497` (flush).

**Track C parallel:** the GPU compute shader's input SSBO **is** Track B's
widened registry (per roadmap dependency: Track B is Track C's prereq). The
compute shader also needs a per-frame **frustum-plane UBO** (6 planes × 4
floats = 96 B; trivial). Output is the visibility bitmask + visible-ID list
+ atomic count — three SSBO bindings.

### 7.3 Common AMD-validated patterns

Both precedents pass on AMD 7900 XTX in production. Patterns to inherit:

- `alignas(16)` + `static_assert(sizeof(...) == N)` + per-field offset asserts.
- `[CPP_GLSL_LOCKSTEP]` rule from
  `memory/cpp_glsl_ubo_struct_lockstep.md`: extending C++ side without GLSL
  side corrupts std430 stride for `arr[i>0]`. Both struct definitions go in
  the same commit.
- `gos_RendererRebindVAO()` at the start of any GL-direct draw bridge to
  defeat the AMD VAO 0 trap (per `gpu_direct_renderer_bringup_checklist.md`
  trap #4). Compute dispatch itself doesn't need this, but the consuming
  indirect draw does.

---

## 8. Compute dispatch frame-position candidates

Frame ordering (verified live-grep):

```
Mission::update                                       [code/mission.cpp:465]
  ├─ eye->update                                      [471]
  ├─ land->update (Terrain::update; calls makeLists)  [475]
  ├─ ...weather/waypoints/path...                     [479-486]
  ├─ land->clearObjBlocksActive / clearObjVertices    [496-497]
  ├─ land->terrainTextures->update                    [498]
  ├─ land->geometry (vertex projection, sets cull)    [500]
  ├─ ObjectManager->update (consumes objBlockInfo)    [505]
  ├─ ...sensors/collisions/AI brain...                [517-526]
  └─ (mission tick complete)

GameCamera::render                                    [code/gamecam.cpp:~199]
  ├─ GpuStaticPropRegistry::frameBegin                [201]
  ├─ land->render (queues terrain)                    [202]
  ├─ ...craterManager / ObjectManager->render...      [drawn via MLR]
  ├─ land->renderWater                                [218]
  ├─ mcTextureManager->renderLists (FLUSH)            [drains queue]
  │   └─ GpuStaticPropRegistry::flush                 [txmmgr.cpp:1497]
  └─ land->renderWaterFastPath                        [256, post-flush]
```

**Compute dispatch slot candidates:**

| Candidate | Pros | Cons |
|---|---|---|
| **A. End of `Mission::update` after `geometry()` and before `ObjectManager->update`** | Same-frame cull. CPU and GPU produce in parallel; `ObjectManager` reads CPU's flags this frame, GPU readback feeds frame N+1 (free 1-frame slack). Frustum and instance SSBO are stable here. | Compute and the existing CPU vertex-projection loop overlap — perfectly fine, no contention. |
| **B. Top of `GameCamera::render` (alongside `frameBegin`)** | Camera state final, frustum derivable. | Misses the `ObjectManager->update` slot — too late to feed CPU gates this frame. |
| **C. After `eye->update` only** | Earliest possible (after camera matrix is finalized). | Instance SSBO might still be churning if Track B widens the registry to mission-load mutables (artillery spawns, etc.). Avoid. |

**Recommendation: Candidate A.** Dispatch the compute shader at the end of
`Mission::update`, between `land->geometry()` and `ObjectManager->update`.
The fence is queued; the readback is consumed by the **next frame's**
`ObjectManager->update` (frame N+1 reads frame N's compute output). Frame N
itself uses CPU `recalcBounds` for the sync-frame-N consumers identified in
§5 (combat AI, weapon-spawn nodes) — those keep their existing cost
because we don't gain anything by GPU-aliasing them.

The indirect draw consumes the compute output **same-frame** — compute
dispatch → memory barrier → indirect draw issue. That's well-trodden ground;
no fence needed for compute→draw on the same queue (`glMemoryBarrier` is
sufficient).

---

## 9. Bitmask format / size budget

Static-prop populations per mission per
`memory/gpu_direct_renderer_bringup_checklist.md` and the static-prop
registry context: roadmap §B targets ~10K instances at the upper bound
(buildings, trees, fences, generics, gates, artillery towers).

**Bitmask sizing math (verified):**

- **10K instances → 10000 / 8 = 1250 B per frame** (1 bit per instance).
  Round up to 4-byte words = 1.25 KB. Trivial readback.
- **100K (10× upper bound, future-proof) → 12.5 KB.** Still trivial.
- **3-frame ring → 3 × 1.25 KB = 3.75 KB total readback buffer.** Negligible.

**Layout candidates:**

- **A. Bit-packed `uint[]`.** 32 instances per uint. Compute writes via
  `atomicOr(mask[id/32], 1u << (id%32))`. CPU reads via word + bit-extract.
  Compact, cache-friendly.
- **B. Byte-flag `uint8_t[]`.** 1 byte per instance. Compute writes
  `mask[id] = 1`. No atomic needed (each invocation owns one byte). 8× size
  but still trivial (10 KB). Simplest.
- **C. Visible-ID list (compaction).** Compute appends `visibleIDs[atomicAdd(count, 1)] = id`
  for indirect-draw `gl_BaseInstance`. Required ANYWAY for the indirect-draw
  consumer (§3 hypothesis); the bitmask is then the **secondary** output for
  the CPU-readback consumer.

**Recommendation:** Ship **both** B and C. Compute writes byte-flag mask
(for CPU readback consumer) and visible-ID list with atomic count (for
indirect-draw consumer). Total: ~20 KB. Two SSBO bindings; one fence ring
on the byte-flag side only (the visible-ID list is consumed same-frame by
the draw, no readback fence needed).

---

## 10. Baseline Tracy zones (the cost we're replacing)

Live-grep verified at write time. The "half-frame CPU 'should this be
culled?' cost" Track C aims to replace decomposes into:

| Tracy zone | Site | What it measures |
|---|---|---|
| `Terrain::geometry` | [`terrain.cpp:1306`](../../../mclib/terrain.cpp:1306) | Whole vertex-projection pass — the producer of `objBlockInfo[].active` and `objVertexActive[]`. |
| `Terrain::geometry vertexProjectLoop` | [`terrain.cpp:1359,1500`](../../../mclib/terrain.cpp:1359) | Inner-loop tight cull. Two zones because there's a fast path + parity path. |
| `MapData::makeLists` | [`mapdata.cpp:1089`](../../../mclib/mapdata.cpp:1089) | Camera-windowed quadList rebuild. NOT directly the cull, but the surrounding work that feeds it. |
| `MapData::makeLists vertices` | [`mapdata.cpp:1103`](../../../mclib/mapdata.cpp:1103) | Per-vertex inner loop. |
| `Camera.UpdateRenderers` | [`mechcmd2.cpp:692`](../../../code/mechcmd2.cpp:692) | Top-level — includes `recalcBounds` calls cascading through every actor. Per `memory/perf_profiling_results.md`: 6ms/frame, 3.66ms self-time. |
| `TerrainObject::update recalcBounds` | [`terrobj.cpp:694`](../../../code/terrobj.cpp:694) | Per-terrain-object `recalcBounds` cost. |
| `TerrainObject::primeAppearanceForMissionLoad recalcBounds` | [`terrobj.cpp:612`](../../../code/terrobj.cpp:612) | One-shot mission-load; not per-frame. |
| `GameLogic.Mission.Terrain` | [`mission.cpp:475`](../../../code/mission.cpp:475) | Frames `land->update()`. |
| `GameLogic.Units.TerrainObjects` | [`objmgr.cpp:1709`](../../../code/objmgr.cpp:1709) | Frames the post-projection iterator that consumes `objBlockInfo[].active`. |
| `GameLogic.Units.Mechs` / `Vehicles` / `Turrets` | [`objmgr.cpp:1808,1826,1852`](../../../code/objmgr.cpp:1808) | Per-actor update loops. The recalcBounds calls inside live here. |

**Track C exit criterion (from roadmap §C):** "half-frame CPU 'should this
be culled?' cost replaced by ~50µs compute dispatch." Concrete measurable
form: **sum of `Terrain::geometry vertexProjectLoop` + recalcBounds-share
of `Camera.UpdateRenderers` should drop ≥30% at wolfman zoom**, replaced by
a new `Cull.ComputeDispatch` zone of ~50µs.

**Per `cpu_to_gpu_offload_orchestrator.md` and `perf_profiling_results.md`,
the M2 baseline for `Camera.UpdateRenderers` is 6 ms; the 3.66 ms self-time
is dominated by the per-vertex projection loop.** That 3.66 ms is the
target. Replacing it with ~50 µs is ~70× speedup on that zone — absolutely
within reach for a frustum+distance test on 10K instances on a 7900 XTX
(measured wavefront throughput easily 100 ns/instance for a trivial test).

---

## 11. AMD-specific risks (per `docs/amd-driver-rules.md`)

Live-grep of `amd-driver-rules.md`. Items relevant to compute / SSBO / fence:

- **Attribute 0 must be active.** Compute dispatch doesn't bind attributes;
  the consuming **indirect draw** does — same rule as existing indirect
  terrain path (already proven on 7900 XTX). No new risk.
- **`uniform uint` crashes shader builder.** `gpu_direct_renderer_bringup_checklist.md`
  trap #1. Use `uniform int` and cast inside shader for bitwise. Compute
  shaders that use bitwise on uniform ints would hit this; mitigate by
  declaring uniforms as `int` and casting `uint(u)` for atomicOr / etc.
  **Or** use SSBO for the parameters (compute typically does) — SSBO uint is
  fine.
- **`sampler2DArray`** softened post-Canary A but not used by compute cull
  (compute reads the instance SSBO and writes the visibility SSBO; no
  texture sampling).
- **`gl_FragDepth` workaround** — irrelevant to compute (no fragment).
- **Texture feedback loops** — irrelevant.
- **Matrix transpose: `GL_FALSE` for direct upload** — relevant if compute
  reads model matrices from the instance SSBO. Pattern is already in
  `GpuStaticPropInstance` (mat4 row-major, `GL_FALSE` upload). Mirror.
- **MRT location=1 corruption** — refuted post-F3 canary on 7900 XTX 26.3.1.
  Compute cull doesn't use MRT.
- **`std430` layout** — proven via static-prop and terrain-recipe SSBOs in
  production. No known AMD issue.
- **Indirect draw extension behavior** — `glMultiDrawArraysIndirect` ships
  default-on for terrain on 7900 XTX. The `Count` variant is a strict
  superset; risk of regression is "AMD's `IndirectCount` driver path has a
  different bug than its `Indirect` path" — historically AMD has had separate
  bugs for `IndirectCount`, but the 26.3.1 driver baseline used here has not
  been negatively reported on this entry. **Canary build before flip** is in
  the roadmap risk register — preserve.

**No new known AMD blocker for compute cull on 7900 XTX 26.3.1.**

---

## 12. Risk register (Track C-specific)

| Risk | Likelihood | Mitigation |
|---|---|---|
| **R1.** Combat AI gate (`mech.cpp:6497` reads `canBeSeen()` for fire decisions) sees stale visibility for one frame after a mech becomes visible. | High (will happen at every transition). | **Visual artifact only — enemy waits one frame to fire on newly-revealed target.** Acceptable. Canary at first-pop transitions. If unacceptable: keep CPU `recalcBounds` for combat AI consumers; GPU only feeds render+update gates. Cost: ~0 ms saved on AI side, but the recalcBounds was running anyway under existing cull cost. **No code change needed if accepted; one-line gate-routing change if rejected.** |
| **R2.** Weapon-spawn nodes (`mech3d.cpp:721`, `gvactor.cpp:445`) early-out on `inView` — bolt spawns at object root for one frame on visibility transition. | High at transitions. | Visual artifact only. Same mitigation as R1 if rejected. |
| **R3.** `objBlockInfo[].active` set by GPU N-1 disagreeing with frame-N's actual block contents → `update()` runs / doesn't run on the wrong objects → cascade per `cull_gates_are_load_bearing.md`. | Medium-Low. | The cascade trap is specifically about the gate being **bypassed** — Track C **preserves** the gate, only changes its INPUT. Block contents don't shuffle frame-to-frame (mission-immutable per `objmgr.cpp` load); the only change is which blocks are flagged active. 1-frame stale active-bit = 1 frame of `update()` running on offscreen objects (cheap) or not running on visible objects (matches existing pop-in latency). Verify with `MC2_DESTROY_TRACE=1` in soak. |
| **R4.** Compute shader compile failure surfaces silently. | Medium. | The existing `[INSTR v1]` startup banner + shader-error pattern catches this for VS/FS; mirror for CS. Fail-loud on compile error; killswitch (`MC2_GPU_CULL=0`) falls back to CPU cull. |
| **R5.** AMD 7900 XTX driver bug specific to compute + persistent-mapped readback. | Low (driver 26.3.1 widely tested for compute by other titles). | Canary build per roadmap risk register. Add to `amd-driver-rules.md` if anything surfaces. |
| **R6.** Context bump 4.3 → 4.6 surfaces a 4.3-strict expectation in third-party code. | Low. | Bump in a separate commit; tier1 5/5 PASS gate. Easy revert. |
| **R7.** Memory barrier discipline (compute → indirect draw) wrong; draws read stale visibility. | Medium during bring-up. | `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT \| GL_COMMAND_BARRIER_BIT)` between compute dispatch and `glMultiDrawElementsIndirectCount`. Validate with `MC2_GPU_CULL_PARITY=1` env-gated byte-compare against CPU cull output. |
| **R8.** Fence-readback latency variance (sometimes 1 frame, sometimes 2 if the GPU is slow). | Medium. | The existing static-prop batcher's ring already handles this — `glClientWaitSync` with `GL_TIMEOUT_IGNORED` blocks until the fence is signaled, so the worst case is the readback frame stalls instead of showing stale data. CPU never reads ahead of the fence. |
| **R9.** Track B (widened registry) ships with subtly different lifecycle assumptions than Track C expects. | Medium (Track B is the prereq). | Track C **plan-stage** must read Track B's design at write time, not at execution time. Adversarial review on Track C plan covers this. |
| **R10.** "First-launch black/no terrain intermittency" (CLAUDE.md known issue) interacts with new compute path. | Low. | Already a TODO; orthogonal. Track C doesn't add a new init-order surface — the compute program is built at startup like every other shader. |

---

## 13. Open questions for brainstorm

1. **Should the gameplay-AI / node-position consumers (R1, R2) read CPU
   recalcBounds while render/update reads GPU N-1, or accept the 1-frame
   visual artifact for code simplicity?** The cost of keeping CPU
   `recalcBounds` is the existing `Camera.UpdateRenderers` cost (3.66 ms);
   if we keep it for AI/nodes, we don't reach the roadmap exit criterion
   "`Camera.UpdateRenderers` becomes a stub." If we drop it entirely, the
   1-frame artifact lands. **Likely answer: drop it entirely; document the
   artifact; revisit if testers complain.** But this is a real Q for the
   brainstorm.
2. **Context bump 4.3 → 4.6 in the same commit as compute infra, or as a
   separate prep commit?** Recommendation: separate, lands first.
3. **One compute dispatch per material bucket (roadmap §C) vs one combined
   dispatch with bucket-keyed visible-ID lists?** Track B's bucketing
   determines this. If Track B has N material buckets (cement multi-sampler
   already shipped — N=1 for that bundle), a single dispatch is simpler.
   N≫1 (per-texture) → per-bucket dispatches let `gl_BaseInstance` index
   smaller per-bucket arrays. Defer until Track B's bucket count is final.
4. **Distance test in compute shader: per-instance LOD selection or fixed
   draw-distance?** Roadmap §C says "frustum + distance test." Distance
   could feed LOD switch (Track B widened registry might carry per-instance
   LOD ranges) — design Q for Track B↔C handoff.
5. **`MC2_GPU_CULL_PARITY=1` parity check granularity:** byte-compare full
   visibility bitmask vs CPU `recalcBounds` output? Per-frame summary line?
   Establish at plan time; mirror `MC2_*_PARITY_CHECK` precedent
   (substrate-bug discovery rate per `memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`).
6. **Ever-visible pre-cull at mission load (roadmap §B):** Track B drops
   instances no camera state can ever see. Track C's compute then runs
   over only the survivors. Is the pre-cull deterministic enough that it
   can ship in Track B without a Track-C-side runtime fallback? (If the
   pre-cull is too aggressive, props pop in/out at zoom-out edges.)
7. **GPU readback ring depth: 2 frames or 3?** Static-prop batcher uses a
   ring (count not yet read at write time — verify at plan stage).
   Terrain-indirect uses... also TBD. Pick the depth that matches existing
   precedent.
8. **Compute output for the indirect-draw consumer: visible-ID list
   compacted via atomic, OR sparse `MultiDrawIndirect` command struct
   written directly?** Roadmap suggests `gl_BaseInstance` consumed via
   visible-ID list — simpler, well-understood. Alternative
   (compute-writes-DrawIndirect-commands) is more flexible but more
   substrate to debug.

---

## 14. References (file:line, all grep-verified at write time)

**Engine code:**
- [`code/gamecam.cpp:201-202,256`](../../../code/gamecam.cpp:201) — render-order: `GpuStaticPropRegistry::frameBegin`, `land->render`, `land->renderWaterFastPath`.
- [`code/mission.cpp:465-526`](../../../code/mission.cpp:465) — `Mission::update` frame ordering; `land->geometry()`@500 → `ObjectManager->update`@505.
- [`code/objmgr.cpp:1493,1624,1693-1702,1760-1768,2173,2329,2779`](../../../code/objmgr.cpp:1493) — `objBlockInfo[].active` and `canBeSeen()` consumers.
- [`code/objmgr.h:423-435`](../../../code/objmgr.h:423) — `objBlockInfo` accessors (mission-immutable shape data).
- [`code/gameobj.h:344-345,917-920`](../../../code/gameobj.h:344) — `framesSinceActive` accumulator.
- [`code/gameobj.cpp:106-108,157`](../../../code/gameobj.cpp:106) — `canBeSeen_instr`.
- [`code/mech.cpp:5339,6038-6184,6448,6466,6497`](../../../code/mech.cpp:5339) — combat AI `canBeSeen()`, `getWeaponNodePosition` consumer.
- [`code/gvehicl.cpp:2524,3183-3736,3928,3936`](../../../code/gvehicl.cpp:2524) — vehicle weapon-node consumer + AI gates.
- [`code/mover.cpp:3470-3471`](../../../code/mover.cpp:3470) — `getLOSPosition` pre-flips inView.
- [`code/light.cpp:123-124`](../../../code/light.cpp:123) — light onScreen → setInView.
- [`code/actor.cpp:280-343`](../../../code/actor.cpp:280) — `VFXAppearance::recalcBounds`.
- [`code/bldng.cpp:791-805,1066,1071`](../../../code/bldng.cpp:791) — Building cull.
- [`code/gate.cpp:335-355,596,599`](../../../code/gate.cpp:335) — Gate cull.
- [`code/terrobj.cpp:610-698,797,799,869`](../../../code/terrobj.cpp:610) — TerrainObject cull.
- [`code/turret.cpp:575-808,2034,2048`](../../../code/turret.cpp:575) — Turret cull.
- [`code/artlry.cpp:870,1183-1242,1334,1407,1746,1764`](../../../code/artlry.cpp:870) — Artillery cull + save/load.
- [`code/mechcmd2.cpp:692,709,729,735,747,759,767`](../../../code/mechcmd2.cpp:692) — `Camera.UpdateRenderers` Tracy zone family.

**Engine library (mclib):**
- [`mclib/appear.h:71,87,99,171-176,205-206`](../../../mclib/appear.h:71) — `Appearance::inView` storage + accessor.
- [`mclib/terrain.cpp:484-492,784-793,1306,1359,1430-1495,1500,1590-1632,1721,1757,1903,1917,1923`](../../../mclib/terrain.cpp:484) — terrain producer + Tracy zones.
- [`mclib/terrain.h:179-181`](../../../mclib/terrain.h:179) — `objBlockInfo` and `objVertexActive` storage.
- [`mclib/mech3d.cpp:721-833,2055-2362,2377,2881,3740,3797,4180-4196,5099,5136`](../../../mclib/mech3d.cpp:721) — mech recalcBounds, node-pos queries, render.
- [`mclib/gvactor.cpp:445-1895,1946,1982,2022,2039,2711`](../../../mclib/gvactor.cpp:445) — GV recalcBounds + node-pos + render.
- [`mclib/bdactor.cpp:1082-1494,1561,2012,2112,2258-2268,3872-4199,4205,4208,4454,4555,4570`](../../../mclib/bdactor.cpp:1082) — BldgAppearance + GenericAppearance.
- [`mclib/genactor.cpp:569-1204`](../../../mclib/genactor.cpp:569) — Generic-actor cull.
- [`mclib/mapdata.cpp:953,1089-1181`](../../../mclib/mapdata.cpp:953) — `MapData::makeLists` Tracy zones.

**GPU substrate (GameOS):**
- [`GameOS/gameos/gos_render.cpp:184-188`](../../../GameOS/gameos/gos_render.cpp:184) — GL 4.3 core context request.
- [`GameOS/gameos/gameosmain.cpp:798`](../../../GameOS/gameos/gameosmain.cpp:798) — `glewInit`.
- [`GameOS/gameos/gos_static_prop_batcher.h:13-48`](../../../GameOS/gameos/gos_static_prop_batcher.h:13) — `GpuStaticPropInstance` (112 B).
- [`GameOS/gameos/gos_static_prop_batcher.cpp:248,261,1251,1334,1371,1667-1674`](../../../GameOS/gameos/gos_static_prop_batcher.cpp:248) — fence ring + persistent-mapped pattern.
- [`GameOS/gameos/gos_static_prop_registry.h`](../../../GameOS/gameos/gos_static_prop_registry.h) — `frameBegin/markVisible/flush`.
- [`GameOS/gameos/gos_terrain_indirect.cpp:228,715-741,807-908,1209,1496`](../../../GameOS/gameos/gos_terrain_indirect.cpp:228) — `g_denseRecipes` + thin record + ring fence.
- [`GameOS/gameos/gos_terrain_indirect.h:117`](../../../GameOS/gameos/gos_terrain_indirect.h:117) — `vertexNum` map-stable indexing comment.
- [`GameOS/gameos/gos_terrain_patch_stream.h:87-129`](../../../GameOS/gameos/gos_terrain_patch_stream.h:87) — `TerrainQuadRecipe` (144 B) + thin record (32 B).
- [`GameOS/gameos/gos_terrain_patch_stream.cpp:269,294,629,1513`](../../../GameOS/gameos/gos_terrain_patch_stream.cpp:269) — patch-stream persistent-mapped + fence.
- [`GameOS/gameos/gameos_graphics.cpp:2226,2304,2423`](../../../GameOS/gameos/gameos_graphics.cpp:2226) — `glMultiDrawArraysIndirect` site (production).
- [`mclib/txmmgr.cpp:1497,1505`](../../../mclib/txmmgr.cpp:1497) — `GpuStaticPropRegistry::flush` site.

**Loader headers (3rdparty, declarations only — confirmation that the symbols are reachable):**
- [`3rdparty/include/GL/glew.h:2530,2545-2546,4215,4227,4237-4256,5430-5446,22604-22605,23044-23045,25606`](../../../3rdparty/include/GL/glew.h:2530) — compute / IndirectCount / ARB_indirect_parameters declarations.
- [`3rdparty/include/SDL2/SDL_opengl_glext.h:2481,2498,2771-2772,3156,3172-3173,3314`](../../../3rdparty/include/SDL2/SDL_opengl_glext.h:2481) — SDL-side declarations.

**Required reading consumed:**
- `docs/superpowers/mc3-rendering-modernization-roadmap.md` (full, 289 lines).
- `memory/cull_gates_are_load_bearing.md` (5 cascade modes).
- `memory/gpu_direct_renderer_bringup_checklist.md` (9 traps).
- `memory/feedback_data_flow_audit_asymmetry.md` (negative-claim grep discipline applied throughout §2 and §5).
- `docs/architecture.md` (frame ordering, Tracy zone map).
- `docs/amd-driver-rules.md` (relevant rules summarized in §11).

**Adjacent prior recons (referenced for context, NOT source-of-truth — superseded by this recon's grep-at-write-time work):**
- `docs/superpowers/specs/2026-04-30-track-c-implementation-readiness-audit.md` — **DIFFERENT TRACK C** (Lua scripting). Naming collision noted.
- `docs/superpowers/explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md` — **DIFFERENT TRACK C** (Lua mod NO-list).

---

## Conclusion (for the recon brief)

Track C is **architecturally feasible at ~2 weeks** as the roadmap claims.
The compute infrastructure is the only genuinely new surface to add; every
other primitive (fence ring, persistent-mapped buffers, std430 SSBO layout,
indirect draw, AMD-validated patterns) is shipped and proven. The async-
readback hypothesis works for the load-bearing cull cascade
(`objBlockInfo[].active`, `objVertexActive[]`, `inView`-as-render-gate).

The only finding that could shift Track C scope is **R1 (combat AI sees
stale visibility for one frame)**. This is a documented visual artifact, not
a correctness break, and the mitigation (keep CPU `recalcBounds` for AI-
specific consumers while GPU drives render + lifecycle gates) is a one-line
gate-routing decision, not architectural rework. **It does not push Track C
past 2 weeks.** It does deserve an explicit decision in the brainstorm
(Q1 in §13).
