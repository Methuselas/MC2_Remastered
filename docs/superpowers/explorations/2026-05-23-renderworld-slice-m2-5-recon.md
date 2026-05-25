# RenderWorld Slice M2.5 — Reconnaissance

**Date:** 2026-05-23
**Scope:** Per-mech `Handle.raw()` write to R32_UINT attachment-2 (object-ID buffer), mirroring M1.5 for the mech path. Inspect-only follow-on for M2.6 mech-pickup.

## Summary

- **One write path covers the on-screen mech population:** `GpuMechBatcher::flush()` (the Track-D GPU-driven path) issues every mech draw on the default code path; the legacy MLR/`ShapeRenderer`/`gos_tex_vertex_lighted` route is reached only as a per-actor CPU fallback when `g_useGpuMechs=0` or registration/finalize fails. M2.5 can target the batcher path and treat MLR as an inspect-only gap (mechs there will simply not produce IDs that frame; static-prop M1.5 already accepts a similar legacy/coalesce-path asymmetry).
- **The handle is already on `Mech3DAppearance::mechRenderHandle`**, but `flush()` does NOT have an `Mech3DAppearance*` per submit — `GpuMechSubmitDesc` (the per-actor record carried into the per-frame instance SSBO) has no appearance pointer or handle field. The cleanest M2.5 plumbing is: add `uint32_t objectIdRaw` to `GpuMechSubmitDesc`, have `mech3d.cpp:~2549` populate it from `getRenderWorldHandle().raw()`, and add a matching `objectIdRaw` field to `GpuMechInstance` (the std430 SSBO) which `mech.frag` already has straight-line access to via `inst`.
- **The single META-FIX shader edit** mirrors `static_prop.frag` exactly: `layout(location=2) out uint v_objectId` under `#ifdef MC2_OBJECT_ID_BUFFER`, fed by `inst.objectIdRaw`. The `mech.frag` already declares `layout(location=0/1)` outputs in the same MRT style, so the change is symmetric and one shader hop. No uniform upload needed (data rides the existing per-instance SSBO already bound in `flush()`).
- **`setSceneDrawBuffers` extension surface is not touched by mechs.** Mech draws inherit the scene FBO bound by `gosPostProcess::beginScene()`; the batcher never calls `glDrawBuffers` or rebinds the scene FBO. M1.5's helper covers the binding policy for the mech draw automatically — no new call site needs to route through it.
- **Per-mission max mech count is 46** (mc2_24). At sizeof(`GpuMechInstance`) currently 48B, adding a `uint32_t objectIdRaw` + 4-byte pad bumps to 56/64B; effectively zero memory cost (3 ring frames × 512 instance capacity × 16 extra bytes = 24 KB).

## Mech rendering write paths

### Path A — GpuMechBatcher (default; Track D)

| Stage | Citation |
|---|---|
| Per-actor submit | `mclib/mech3d.cpp:2586` (`GpuMechBatcher::instance().submitActor(desc)`) — gated by `g_useGpuMechs` (default ON since 2026-05-09; `gos_mech_batcher.cpp:45`) |
| Submit struct | `GameOS/gameos/gos_mech_batcher.h:88-106` (`GpuMechSubmitDesc`) |
| Per-instance GPU record | `GameOS/gameos/gos_mech_batcher.h:35-51` (`GpuMechInstance`, 48B std430) — written at `gos_mech_batcher.cpp:1096-1103` |
| Draw call | `gos_mech_batcher.cpp:1289` (`glDrawElementsInstancedBaseVertex` inside `flush()`) — one per (texture, material, packet) bucket |
| Vertex shader | `shaders/mech.vert` |
| Fragment shader | `shaders/mech.frag` — outputs `layout(location=0) FragColor`, `layout(location=1) GBuffer1`; the natural extension point for `layout(location=2) out uint v_objectId` |
| Program load | `gos_mech_batcher.cpp` (program ptr `s_mechProgram` cached at startup; no env-gated prefix today — adding `MC2_OBJECT_ID_BUFFER` prefix needs the same `RenderWorld::IsObjectIdBufferEnabled()` branch used at `gos_static_prop_batcher.cpp:510-521`) |
| FBO bind | NONE in mech batcher — inherits scene FBO from `gosPostProcess::beginScene()` at `gos_postprocess.cpp:475`; `setSceneDrawBuffers(MainSceneMRT, sceneObjectIdTex_ != 0)` at `:488` already extends draw-buffer policy to attachment-2 for the entire scene pass |

### Path B — Legacy MLR / `ShapeRenderer` (CPU fallback)

| Stage | Citation |
|---|---|
| Per-actor submit | `mclib/mech3d.cpp:2608` (`mechShape->Render(true)`) — only when `!gpuMechSubmitted && !mechGpuCullSkip` |
| Render queue | `mclib/txmmgr.cpp:1727` (`MC_TextureManager::renderLists`) — enqueued through `masterHardwareVertexNodes`, drained at `:1820-1881` (Render.3DObjects zone) |
| Per-shape draw | `mclib/txmmgr.cpp:1868-1870` (`ShapeRenderer::render(rs->vb_, rs->ib_, ...)`) — at `mclib/txmmgr.cpp:1499` (`class ShapeRenderer`) |
| Material/shader | `mclib/txmmgr.cpp:1528` — `gos_getRenderMaterial("gos_tex_vertex_lighted")`, files `shaders/gos_tex_vertex_lighted.{vert,frag}` |
| FBO bind | NONE — same scene FBO inherited from `beginScene()` |

The MLR path is reached on the default build only when `submitActor` rejects an actor (unregistered type, ring overflow, late registration, etc.); per-mission mech counts ≤46 plus the dual-queue retirement campaign means this path is documentary for M2.5 — it can be left unmodified and accept that fallback mechs do not produce IDs that frame. Static-prop M1.5 shipped with an analogous asymmetry (legacy vs. coalesce paths share one fragment shader but only the in-use binding controls the env-flag macro).

### Per-mech draw state setup site (the M2.5 implant point)

The CPU-side point where per-mech state is bundled is `mech3d.cpp:2549-2585` (`GpuMechSubmitDesc desc{}` ... `submitActor(desc)`). This is the unique point where the appearance instance and its handle are both in scope — `Mech3DAppearance::this` owns `mechRenderHandle` via `getRenderWorldHandle()` (`mclib/mech3d.h:487-489`). Adding one line —

```
desc.objectIdRaw = getRenderWorldHandle().raw();
```

— at line ~2585 closes the entire CPU side of the chain.

## Handle-to-shader chain (recommended)

```
Mech3DAppearance::mechRenderHandle              [mech3d.h:478]
  via getRenderWorldHandle().raw()              [mech3d.h:487]
  -> GpuMechSubmitDesc::objectIdRaw [NEW]       [gos_mech_batcher.h:88-106]
  -> PendingSubmit.desc.objectIdRaw             [gos_mech_batcher.cpp:156]
  -> GpuMechInstance::objectIdRaw [NEW]         [gos_mech_batcher.h:35-51]
  -> SSBO binding=0 in mech.vert                [shaders/mech.vert:30-40]
  -> forward as `flat out uint v_objectId`      [shaders/mech.vert NEW]
  -> mech.frag: layout(location=2) out uint     [shaders/mech.frag NEW]
```

Alternative: a per-flush `glUniform1ui` upload before each `glDrawElementsInstancedBaseVertex` at `gos_mech_batcher.cpp:1289`. **Rejected** — `flush()` already buckets across many actors per draw call (one draw per packet+texture, all instances of that bucket share a uniform load), so a single uniform cannot carry per-instance IDs. The per-instance SSBO is already the right vehicle.

Static-prop legacy path uses `u_objectIdRaw` (`shaders/static_prop.frag:59`, `gos_static_prop_batcher.cpp:1864` upload) precisely because it draws one instance at a time. The mech batcher is the *opposite* shape — bucketed instanced draws — so the SSBO route is mandatory.

`GpuMechInstance` is already a std430 SSBO with three uint32 fields (`typeLodRecordIndex`, `baseBoneOffset`, `lightDataIndex`, `renderFlags`) before its two vec4s. Adding a fifth uint32 keeps the natural-alignment story intact; the struct grows from 48 to 64 bytes (one std430 slot bump after the two vec4s' 16-byte alignment is honored — verify against `static_assert(sizeof(GpuMechInstance) == ?)` at `gos_mech_batcher.h:44`). Lockstep edit warning at `gos_mech_batcher.h:14` applies: vertex shader struct in `mech.vert:30-37` must add the field in identical position.

## setSceneDrawBuffers extension surface

`setSceneDrawBuffers(SceneDrawBufferMode, bool objectIdAttachmentReady)` at `GameOS/gameos/gos_postprocess.cpp:31` already handles the mech case correctly — the mech batcher does not call `glDrawBuffers` itself, does not rebind the scene FBO, and is drawn during the scene phase that `beginScene()` set up. The 5 call sites that route through the helper today (`gos_postprocess.cpp:339,488,497,586,696,729`) cover createFBOs / beginScene / runScreenShadow / runGodRays / runShoreline. None of them are mech-specific; M2.5 needs no new entry. **The helper's bug-class retirement claim from M1.5 survives M2.5.**

Mech-side grep confirmation (negative claim — opposite-direction check):

```
grep -n glDrawBuffers GameOS/gameos/gos_mech_batcher.cpp  -> no matches
grep -n glDrawBuffers GameOS/gameos/gos_static_prop_batcher.cpp -> no matches
grep -n glDrawBuffers mclib/txmmgr.cpp -> no matches
```

The only engine-side `glDrawBuffers` callers are the 5 in `gos_postprocess.cpp`.

## META-FIX vs PATCH analysis

**META-FIX surface (recommended):** one `uint32_t` SSBO-field add + one fragment-shader output add + one CPU-side handle copy at submit time.

| Edit | Location | Type |
|---|---|---|
| `GpuMechSubmitDesc::objectIdRaw` | `gos_mech_batcher.h:88-106` | struct field add |
| `GpuMechInstance::objectIdRaw` | `gos_mech_batcher.h:35-51` (+ static_assert update) | std430 schema add (lockstep with mech.vert) |
| `mech.vert` SSBO struct + forward | `shaders/mech.vert:30-40` + `flat out uint v_objectId` | shader struct add |
| `mech.frag` MRT output | `shaders/mech.frag:36-37` | `layout(location=2) out uint v_objectId` under `#ifdef MC2_OBJECT_ID_BUFFER` |
| Env-gated GLSL prefix at program load | `gos_mech_batcher.cpp` (load site near `s_mechProgram = ...`) | mirror `gos_static_prop_batcher.cpp:510-521` |
| Submit-time fill | `mclib/mech3d.cpp:2549-2585` | one assignment |
| Instance writer | `gos_mech_batcher.cpp:1096-1103` (`inst.objectIdRaw = ps.desc.objectIdRaw;`) | one assignment |

This retires the "mechs invisible to picking" bug class with **one symmetric per-instance schema add** — identical shape to the M1.5 coalesce static-prop path (per-instance PerDrawEntry-style ID), not the M1.5 legacy uniform path (per-draw uniform).

**Alternative additive patches considered and rejected:**

1. **Separate mech-ID FBO.** Adds a second R32_UINT attachment, doubles the readback indirection in `lookupAtPixel`, splits picking results across two textures, forces `MissionInterfaceManager::tryStaticPropPick` (M2.6 → `tryGameplayPick` spine from M2-pre) to merge two lookups. Negates the M1.5 unified-table win.
2. **CPU-side mech-rect lookup table.** Maintain per-mech screen-space bounding boxes from the prior frame; resolve clicks against the CPU table. Skips the GPU read entirely but loses per-pixel precision (mechs occlude each other and have non-rectangular silhouettes), and duplicates the cull/visibility/projection chain on the CPU. Strict regression vs. M1.5's correctness contract.
3. **Per-draw `glUniform1ui` upload at each bucket draw call.** Cannot carry per-instance data; the mech batcher's whole point is that one `glDrawElementsInstancedBaseVertex` covers many actors. Would force one draw call per mech, undoing Track D's primary win.

None qualify under the greybeard discipline — the SSBO-field add is strictly substitutive against the static-prop META-FIX precedent set in M1.5.

## Recommended M2.5 scope

Add `uint32_t objectIdRaw` to both `GpuMechSubmitDesc` (CPU-only carrier) and `GpuMechInstance` (std430 SSBO record). Fill from `Mech3DAppearance::getRenderWorldHandle().raw()` at `mech3d.cpp:~2585`. Forward through `mech.vert` as `flat out uint v_objectId` from `inst.objectIdRaw`; emit at `mech.frag` `layout(location=2) out uint v_objectId` under `#ifdef MC2_OBJECT_ID_BUFFER`. Gate the GLSL macro via the same `RenderWorld::IsObjectIdBufferEnabled()`-driven program-prefix mechanism used at `gos_static_prop_batcher.cpp:510-521`. No FBO, draw-buffer, or render-pass changes — `setSceneDrawBuffers` from M1.5 already covers it. Discardable scope-creep: MLR fallback path (mechs that took CPU path that frame produce no ID — accept as known asymmetry, document in spec). Validator: per-mission `[RENDER_WORLD v1]` banner extended with `mech_id_writes=N` count from the batcher; pair with an `lookupAtPixel` self-test on a known mech screen position in mc2_24 (46 mechs — densest target).

## File:line citations

| Claim | File:line | Verified |
|---|---|---|
| `kMechHandleBase = 0x10000` | `RenderWorld/RenderWorld.cpp:109` | grep |
| `mechRenderHandle` field on Mech3DAppearance | `mclib/mech3d.h:478` | read |
| `getRenderWorldHandle()` accessor | `mclib/mech3d.h:487` | read |
| Default-on `g_useGpuMechs` | `GameOS/gameos/gos_mech_batcher.cpp:45` | read |
| GpuMechSubmitDesc struct | `GameOS/gameos/gos_mech_batcher.h:88-106` | read |
| GpuMechInstance std430 (48B) | `GameOS/gameos/gos_mech_batcher.h:35-51` | read |
| Mech batcher submit call | `mclib/mech3d.cpp:2586` | grep |
| GpuMechSubmitDesc population | `mclib/mech3d.cpp:2549-2585` | read |
| Instance SSBO write | `GameOS/gameos/gos_mech_batcher.cpp:1096-1103` | read |
| `glDrawElementsInstancedBaseVertex` in `flush()` | `GameOS/gameos/gos_mech_batcher.cpp:1289` | grep |
| `glDrawElementsInstancedBaseVertex` in `flushShadow()` | `GameOS/gameos/gos_mech_batcher.cpp:479` | grep |
| Mech FS outputs (loc 0,1) | `shaders/mech.frag:36-37` | read |
| Mech VS SSBO struct/binding | `shaders/mech.vert:30-40` | read |
| MLR fallback `mechShape->Render(true)` | `mclib/mech3d.cpp:2608` | grep |
| ShapeRenderer::render call | `mclib/txmmgr.cpp:1868-1870` | read |
| ShapeRenderer material = `gos_tex_vertex_lighted` | `mclib/txmmgr.cpp:1528` | read |
| Render.3DObjects loop | `mclib/txmmgr.cpp:1820-1881` | read |
| `setSceneDrawBuffers` helper | `GameOS/gameos/gos_postprocess.cpp:31-56` | read |
| Helper call sites (5) | `gos_postprocess.cpp:339,488,497,586,696,729` | grep |
| Scene FBO bind in beginScene | `GameOS/gameos/gos_postprocess.cpp:475-499` | read |
| Static-prop env-gated GLSL prefix template | `GameOS/gameos/gos_static_prop_batcher.cpp:510-521` | read |
| Static-prop FS attachment-2 output | `shaders/static_prop.frag:71` | read |
| Static-prop FS `u_objectIdRaw` (legacy path) | `shaders/static_prop.frag:59` | read |
| Static-prop PerDrawEntry `objectIdRaw` (coalesce) | `GameOS/gameos/gos_static_prop_batcher.h:53` | read |
| Static-prop instance-time `objectIdRaw` fill | `GameOS/gameos/gos_static_prop_batcher.cpp:2058-2059` | read |
| Mech batcher does NOT call `glDrawBuffers` | `GameOS/gameos/gos_mech_batcher.cpp` (no match) | grep negative |
| FBO attachment-2 R32_UINT texture | `GameOS/gameos/gos_postprocess.cpp:323-334` | read |

RECON STATUS: COMPLETE
