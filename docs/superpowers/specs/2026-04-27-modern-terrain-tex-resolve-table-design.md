# Shape A — `TexResolveTable` Design (M0a)

**Date:** 2026-04-27
**Worktree:** `.claude/worktrees/nifty-mendeleev` (branch `claude/nifty-mendeleev`)
**Status:** spec — design pass, not yet a plan. Read-only audit; no code changes.
**Predecessor:** [brainstorm](../brainstorms/2026-04-27-modern-terrain-surface-seam-m0-shapes.md) §"Shape A — `TexResolveTable`" + Q-A1..Q-A4. [findings](../explorations/2026-04-27-modern-terrain-surface-findings.md) §1.4–§1.6, baseline table, §9.
**Successor:** implementation plan, gated by the "Ready for implementation plan?" question at the end of this doc.

---

## 1. Thesis

Initialize a per-frame flat memoization table `uint32_t[MC_MAXTEXTURES]` at the top of `GameCamera::render` (a single sentinel-fill plus a generation bump). Each converted `tex_resolve(nodeId)` lazily resolves through the legacy `MC_TextureManager::get_gosTextureHandle(nodeId)` accessor on the first read of that node this frame, stores the result, and returns indexed loads for every subsequent read. Repeated calls for the same `textureIndex` collapse to one legacy resolution per node per frame, which is exactly the work being duplicated 96M times per capture today (per the [Tracy baseline](../explorations/2026-04-27-modern-terrain-surface-findings.md#tracy-baseline-snapshot--self-only-3060-frames)).

The table caches *one frame*, never longer. It does not cache across frames. It does not change any node-storage shape, any GPU buffer, any shader, or any submission code. It does not pre-resolve unused textures: a node that is referenced zero times this frame has zero entries written, exactly as today. It is purely a memoization of repeated reads of a value that is stable for the duration of a frame.

**Design choice (advisor-corrected from earlier draft).** An earlier revision of this spec proposed eager populate-all of every live texture node at frame start. That was wrong: `MC_TextureNode::get_gosTextureHandle()` updates `lastUsed` and may take a cache-miss path that realizes or uploads a texture, so eager populate-all would (a) realize textures the frame never drew and (b) rewrite `lastUsed` across the LRU for nodes that legacy-mode would have left alone. That violates the spirit of C10 — resolve handles at draw time, do not broaden who gets resolved. Lazy first-touch memoization gives the same call-collapse win without changing *which* nodes get realized or *when*.

## 2. Empirical motivation

From the [Tracy baseline snapshot](../explorations/2026-04-27-modern-terrain-surface-findings.md#tracy-baseline-snapshot--self-only-3060-frames):

| Zone | Calls / capture | Self / capture | Self / frame |
|---|---|---|---|
| `MC_TextureNode::get_gosTextureHandle` | 96,536,216 | 1.40 s | **0.46 ms** |
| `TerrainQuad::setupTextures resolveFallback` | 22,094,306 | 1.65 s | **0.54 ms** |
| `TerrainQuad::setupTextures cachedVisibleSubmission` | 16,168,244 | 358.84 ms | 0.12 ms |
| `TerrainColorMap::getTextureHandle realizeTexture` | 15,701,723 | 445.94 ms | 0.15 ms |

The 96M-call zone is the body at [`mclib/txmmgr.cpp:2287`](../../../mclib/txmmgr.cpp). Most reads are dispatched through the manager wrapper at [`mclib/txmmgr.h:526–532`](../../../mclib/txmmgr.h) or directly through `masterTextureNodes[textureIndex].get_gosTextureHandle()`. Per-call body at HEAD is two compares + a per-call `lastUsed = turn` write + a return — already cheap. The win is collapsing 96M function calls (with a Tracy zone, an LRU-touch write, and a branch on `CACHED_OUT_HANDLE`) to one pointer-bump + 96M indexed loads.

Conservative estimate: ~0.2–0.4 ms/frame self-time reduction at Wolfman. **Must be measured before promotion.** §11 defines the falsifiable threshold.

## 3. Audit of `MC_TextureNode` and the call graph

### 3.1 Node shape (verified)

[`mclib/txmmgr.h:128–203`](../../../mclib/txmmgr.h) declares `MC_TextureNode`:

- `protected DWORD gosTextureHandle` (line 133) — the handle; only `MC_TextureManager` (friend) and the public `get_gosTextureHandle()` may read it.
- Public fields: `nodeName`, `uniqueInstance`, `neverFLUSH`, `numUsers`, `key`, `hints`, `width`, `lzCompSize`, `uvScale`, `logicalWidth`, `logicalHeight`, `lastUsed`, `textureData`, three `MC_VertexArrayNode*`, three `MC_HardwareVertexArrayNode*`.
- **No `flags` field. No `MC2_ISTERRAIN` bit. No domain marker.**
- Public method declared at [`mclib/txmmgr.h:201`](../../../mclib/txmmgr.h); body at [`mclib/txmmgr.cpp:2287–2434`](../../../mclib/txmmgr.cpp). Cache-hit fast path is `txmmgr.cpp:2298–2302` (returns `gosTextureHandle`, updates `lastUsed`); cache-miss path runs `gos_NewEmptyTexture`/`gos_LockTexture`/LZ-decompress, etc.

### 3.2 The `MC2_ISTERRAIN` flag is on the *vertex array node*, not the *texture node*

[`mclib/txmmgr.h:72–100`](../../../mclib/txmmgr.h) declares `MC_VertexArrayNode`:
- `DWORD textureIndex` (line 77) — index into `masterTextureNodes[]`.
- `DWORD flags` (line 78) — comment: "Marks texture render state and terrain or not, etc." This is where `MC2_ISTERRAIN | MC2_DRAWSOLID | MC2_ISWATER | …` live ([`mclib/txmmgr.h:46–66`](../../../mclib/txmmgr.h)).

**This resolves Q-A4 in the brainstorm: a `MC_TextureNode` is *domain-agnostic*. The same texture node may be referenced by multiple `MC_VertexArrayNode` submissions in a frame, with different flags (terrain solid, terrain alpha, water, decal, overlay, etc.).** A "walk only `MC2_ISTERRAIN`-flagged nodes" plan, as written in the brainstorm §A.3 step 2, is not directly expressible against `MC_TextureNode`.

This has a strong design implication (§4).

### 3.3 The 96M-call hot path is dominated by `renderLists` flush, not the per-quad reads

Greppable inventory of `get_gosTextureHandle` callers (mclib/, terrain-relevant):

| Site | Form | Phase | Call rate hint |
|---|---|---|---|
| [`mclib/txmmgr.cpp:1114, 1125`](../../../mclib/txmmgr.cpp) | `masterTextureNodes[textureIndex].get_gosTextureHandle()` | `Render.3DObjects` per-shape arm | per-shape × per-batch |
| [`mclib/txmmgr.cpp:1228`](../../../mclib/txmmgr.cpp) | same | `Shadow.StaticAccum` (only when camera moves >100 units) | rare |
| [`mclib/txmmgr.cpp:1316, 1321`](../../../mclib/txmmgr.cpp) | same | `Render.TerrainSolid` per-`masterVertexNodes[i]` | per-batch × per-frame |
| [`mclib/txmmgr.cpp:1429, 1434, 1489, 1494, 1563, 1568, 1623, 1628, 1692, 1697, 1736, 1741, 1784, 1789, 1828, 1833`](../../../mclib/txmmgr.cpp) | same | per-flag arms in `renderLists` (water, alpha, decals, overlays) | per-batch × per-frame |
| [`mclib/quad.cpp:185, 193, 314, 322, 387, 395`](../../../mclib/quad.cpp) | `mcTextureManager->get_gosTextureHandle(mineTextureHandle/blownTextureHandle)` | `setupTextures` mine/overlay paths | per-quad × per-frame |
| [`mclib/quad.cpp:1647, 1791, 2005, 2147`](../../../mclib/quad.cpp) | `mcTextureManager->get_gosTextureHandle(overlayHandle)` | `TerrainQuad::draw` overlay clusters | per-quad × per-layer × per-frame |
| [`mclib/mapdata.cpp:317, 323, 329, 335, 434`](../../../mclib/mapdata.cpp) | same | `ensureTerrainFaceCacheEntryResident` (called from `quad.cpp:434`) | per-resolved-quad × per-frame |
| [`mclib/terrtxm.h:277, 285, 305, 313`](../../../mclib/terrtxm.h) | same | `getTextureHandle` / `getDetailHandle` | per-quad × per-frame |
| [`mclib/terrtxm2.h:134, 142, 150, 157, 166`](../../../mclib/terrtxm2.h) | same | normal/detail/water handle accessors | per-quad × per-frame |
| [`mclib/terrtxm2.cpp:2384`](../../../mclib/terrtxm2.cpp) | same | result-texture path | per-quad × per-frame |
| [`mclib/crater.cpp:284, 289, 554`](../../../mclib/crater.cpp) | same | crater rendering | per-crater |
| [`mclib/cellip.cpp:62`](../../../mclib/cellip.cpp), [`mclib/gvactor.cpp:1134, 1270`](../../../mclib/gvactor.cpp), [`mclib/mech3d.cpp:1629, 1784`](../../../mclib/mech3d.cpp) | same | non-terrain mech/vehicle/ellipse path | per-shape |
| [`mclib/mlr/gosimage.cpp:108, 124`](../../../mclib/mlr/gosimage.cpp), [`mclib/mlr/gosimage.hpp:64`](../../../mclib/mlr/gosimage.hpp) | same | image/UI render | non-terrain |
| [`mclib/utilities.cpp:167, 174, 217, 308`](../../../mclib/utilities.cpp), [`mclib/tgl.cpp:1534`](../../../mclib/tgl.cpp) | same | mission-load / type-registration | rare |

The "hot path" is therefore **all `renderLists` arms** (terrain *and* non-terrain) plus the per-quad reads in `quad.cpp` / `terrtxm{,2}.h` / `mapdata.cpp`. Limiting the table to "terrain-flagged reads only" would still leave the non-terrain `renderLists` arms on the legacy path and might not deliver the full Tracy-zone collapse — and, more importantly, has no clean test to write because the flag isn't on the node.

### 3.4 Static-prop batcher precedent (the in-tree pattern Shape A inherits)

[`GameOS/gameos/gos_static_prop_batcher.cpp:782–794`](../../../GameOS/gameos/gos_static_prop_batcher.cpp) is the canonical in-tree precedent for "store slot, resolve handle at draw time, never cache across frames":

```cpp
uint32_t gosHandle = 0;
const TG_TypeShape* src = type.source;
if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
    gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
}
const uint32_t glTexId = gos_GetGLTextureId(gosHandle);
```

Different layer of the system (TG shape, not MC texture manager) but the same C10 rule (memory: `mc2_texture_handle_is_live.md`). Shape A applies the same discipline at the `MC_TextureManager` layer. The crucial difference: TG shape handles are mutated by `TransformMultiShape` at game-logic time, *before* `GameCamera::render`. MC texture handles are mutated by `MC_TextureManager` cache-eviction inside `get_gosTextureHandle` itself, which runs synchronously on the render thread. Once the table is populated, no eviction can occur until the next call to `get_gosTextureHandle` — so the table is stable for the rest of the frame.

## 4. Scope

### 4.1 In scope (M0a)

- A single CPU-side flat table sized to `MC_MAXTEXTURES` entries (~12 KB at the 3000-cap from memory `texture_handle_cap.md`).
- A per-frame `beginFrameTexResolve()` initializer called once from `GameCamera::render` between `Camera.BuildMVP` and `land->render()`.
- A read-side helper (single inline) that the converted callsites use in place of the existing accessors.
- Killswitch (`MC2_MODERN_TEX_RESOLVE`) and validate-mode (`MC2_MODERN_TEX_RESOLVE_VALIDATE`).
- Env-gated `[TEX_RESOLVE v1]` lifecycle and counter prints in the same commit.
- Conversion of the **terrain-solid read path only** (per §6) — water, decal, overlay, mech, UI reads stay on the legacy direct accessor.

### 4.2 Out of scope (deferred to other shapes / sessions)

- Buffer changes, shader changes, submission consolidation (Shape B).
- `patchTable` / `quadSetupTextures` mission-load hoist (Shape C).
- Static-shadow accum migration (B' or later).
- Retiring the legacy `MC_TextureNode::get_gosTextureHandle` callers — the killswitch and validate-mode require both paths to coexist.
- Q-A3 (`TerrainColorMap::getTextureHandle realizeTexture`, 15.7M calls): **decision deferred to §13.** The function lives in [`mclib/terrtxm{,2}.{h,cpp}`](../../../mclib/terrtxm.h) and ultimately calls the same `mcTextureManager->get_gosTextureHandle(nodeId)` wrapper, so its calls *are already counted* inside the 96M number for the node-method zone. Folding it in costs nothing extra at the table layer; it is folded in (see §6) for free.

### 4.3 Untouched-by-construction (carry-forward from brainstorm Shape A.7/A.8/A.12)

Compressed paragraph: CPU `projectZ` admission (the 8 `projectFor*` wrappers in [`mclib/camera.h:433–610`](../../../mclib/camera.h)); the `clipInfo` + `setObjBlockActive` / `setObjVertexActive` cull cascade (C1); the `pz` gate at [`mclib/quad.cpp:1597–1602`](../../../mclib/quad.cpp) and sister clusters (C6); the F3 `rc_gbuffer1_*` GBuffer1 contract (no shaders touched); the non-tess fallback path; static-shadow accumulation in [`mclib/txmmgr.cpp:1184–1242`](../../../mclib/txmmgr.cpp) (it can stay legacy in M0a; spec opts to leave it legacy because it re-renders only every >100-unit camera move and is not a per-frame hotspot); the `terrainMVP` `GL_FALSE` upload (C4); `gos_VERTEX` and `gos_TERRAIN_EXTRA` shapes; mod-content compatibility (Magic / Carver5O / MCO Omnitech).

## 5. Data structure

### 5.1 Type and field layout

```cpp
// mclib/tex_resolve_table.h
struct TexResolveTable {
    static constexpr DWORD kSentinel = 0xFFFFFFFFu;  // unpopulated / invalid
    static constexpr DWORD kEmpty    = 0x0u;          // resolved-to-zero (legitimate; node was 0xffffffff)

    uint32_t   handles[MC_MAXTEXTURES]; // resolved gosTextureHandle, lazily filled on first read this frame
    uint64_t   buildGeneration;         // monotonic frame counter; mismatch ⇒ stale read
    uint32_t   resolvedThisFrame;       // count of distinct nodeIds first-touched this frame; for instrumentation
    bool       enabled;                 // mirror of MC2_MODERN_TEX_RESOLVE for debugger-friendly inspection
};

extern TexResolveTable g_texResolveTable;
```

Sizing rationale: `MC_MAXTEXTURES` is the existing `MC_TextureManager::masterTextureNodes[]` cap (`mclib/txmmgr.h` `MC_MAXTEXTURES`; raised to 3000 per memory `texture_handle_cap.md`). 3000 × 4 bytes = 12 KB; trivially small, fits in L1 of any modern CPU. Sized once at startup; no per-mission growth needed because `masterTextureNodes` is itself sized to `MC_MAXTEXTURES` at engine init.

Key choice rationale: `DWORD textureIndex` (the integer index into `masterTextureNodes[]`) is **already** the universal currency in this codebase. Every consumer of `get_gosTextureHandle` either has the index in hand (e.g. `masterVertexNodes[i].textureIndex`) or accepts an index argument (the wrapper at [`txmmgr.h:526`](../../../mclib/txmmgr.h)). Using a `MC_TextureNode*` pointer key would require a separate registration step. Using the index is zero-friction.

`buildGeneration` is the load-bearing safety field. Reads assert in validate-mode that `g_texResolveTable.buildGeneration == g_currentFrameId` — a stale read serves a sentinel and the validation print fires. In production builds the field is logged in `[TEX_RESOLVE v1] event=summary` once per frame.

### 5.2 What the table does *not* store

- No node-metadata duplicates (width, key, flags, etc.) — only the resolved handle.
- No domain flag (terrain vs water vs decal). The table is domain-agnostic; it serves whichever node-index any caller asks about. This is a **deliberate departure from the brainstorm's flag-gated walk plan** — see §3.2.
- No cross-frame state. `buildGeneration` is checked, not preserved.
- No per-pass entries. One handle per node, period.

## 6. Frame initialization + lazy resolve algorithm

### 6.1 When (verified phase placement)

`Terrain::geometry` runs in the **mission update phase** (game-logic), called from `mission->update()` long before `GameCamera::render`. By the time `GameCamera::render` is invoked, `masterVertexNodes[]` already contains this frame's terrain-flagged submissions but **no GL submission has happened yet** (verified by reading [`code/gamecam.cpp:140–256`](../../../code/gamecam.cpp): the activeScene block at 145 is the first place that calls into `land->render()` → `MC_TextureManager::renderLists()`).

Init site: top of `GameCamera::render activeScene` immediately after `Camera.BuildMVP` (between [`code/gamecam.cpp:189`](../../../code/gamecam.cpp) and the `theSky->render(1)` call at line 194). Place is chosen because:
- All consumers (`renderLists`, `craterManager->render`, `ObjectManager->render`, `land->renderWater`, `ObjectManager->renderShadows`) run after this point.
- `Camera.BuildMVP` does not touch texture handles.
- Static-shadow accumulation (`Shadow.StaticAccum`) is invoked from inside `renderLists()` and so will see an initialized table.
- The init zone (`Camera.BeginFrameTexResolve`) is a single `memset` plus a generation bump — well under 5 µs at the 3000-entry cap.

### 6.2 How (algorithm)

Two functions: a per-frame initializer that resets the table, and the inline read accessor that lazily resolves on first touch.

```
beginFrameTexResolve(frameId):
    g_texResolveTable.buildGeneration = frameId
    memset(g_texResolveTable.handles, 0xFF, sizeof(handles))   // fill with kSentinel
    g_texResolveTable.resolvedThisFrame = 0
    [TEX_RESOLVE v1] event=begin_frame frame=frameId           (trace mode only)

tex_resolve(nodeId):
    if (!g_texResolveTable.enabled)
        return mcTextureManager->get_gosTextureHandle(nodeId)        // killswitch OFF
    if (nodeId == 0xffffffff)
        return nodeId                                                // matches txmmgr.h:528–531
    DWORD h = g_texResolveTable.handles[nodeId]
    if (h == TexResolveTable::kSentinel) {
        h = mcTextureManager->get_gosTextureHandle(nodeId)           // legacy resolve, exactly once per nodeId per frame
        g_texResolveTable.handles[nodeId] = h
        g_texResolveTable.resolvedThisFrame++
    }
    return h
```

Cost model:
- **Per-frame init:** one `memset` of `MC_MAXTEXTURES * 4` bytes (~12 KB at the 3000-cap). On a modern CPU this is a single L1 streaming-store loop, well under 5 µs/frame. The `[TEX_RESOLVE v1] event=summary` reports the measured cost.
- **Per-read fast path (cache hit):** one bounds-checked array load + one compare + one return. The compiler should fully inline this; net cost ≈ 1–2 ns/call vs the ~14 ns/call of `get_gosTextureHandle()` with its Tracy zone, `lastUsed` write, and `CACHED_OUT_HANDLE` branch.
- **Per-read slow path (first touch this frame):** identical to today — one full `get_gosTextureHandle()` call, with whatever cache-miss work that entails. The slow path is invoked **at most once per `textureIndex` per frame**, vs hundreds-of-thousands of times today.

The 96M-call hot path collapses to "≤ N calls per frame, where N is the number of distinct `textureIndex` values touched by terrain-solid reads this frame." From the residual-call census we expect N ≤ a few hundred (one per distinct terrain material per frame, plus overlays).

### 6.3 Why lazy memoization instead of eager populate-all (or flag-gated walk)

Three candidate population strategies were considered:

1. **Eager populate-all live nodes** (an earlier draft of this spec). Rejected: `MC_TextureNode::get_gosTextureHandle()` writes `lastUsed = turn` and may take a cache-miss path that realizes or uploads a texture. Walking every live node at frame start would (a) realize textures the frame never references, costing GL bandwidth and address-space the frame would not otherwise spend, and (b) rewrite `lastUsed` across the LRU for nodes legacy mode would not have touched, perturbing eviction order. Both behaviors are silent — they would not show up in tier1 visual A/B but could surface as later-mission slowdowns or different eviction-pattern bugs.
2. **Flag-gated walk** (the brainstorm's original proposal). Rejected: `MC_TextureNode` has no flags field (§3.2). The walk would have to iterate `masterVertexNodes[]`, project to unique `textureIndex`, then resolve. This is more code than lazy memoization, has the same cache-perturbation risk as populate-all (still resolves nodes that may not be drawn this frame, just a smaller subset of them), and serves only terrain — leaving water/decal/overlay reads on legacy and complicating the residual-call census.
3. **Lazy first-touch memoization** (this spec). Resolves exactly the nodes that legacy mode would have resolved, exactly when legacy mode would have resolved them, exactly once per frame instead of N times. No `lastUsed` perturbation beyond what legacy already does. No flag predicate needed. No "out-of-domain read" hazard (AR3) because every read goes through the same accessor; the table is just a single-frame cache in front of it.

The lazy design is also strictly safer for static-shadow accum at [`txmmgr.cpp:1228`](../../../mclib/txmmgr.cpp): if a future slice converts that callsite, the accum's >100-unit re-render trigger naturally first-touches whatever nodes it needs, exactly as today. No "did populate-all walk our nodes early?" question to answer.

### 6.4 Threading and lifetime

All MC2 rendering is single-threaded on the render thread (verified by lack of any thread-pool/async-render plumbing in `mclib/txmmgr.cpp` and `code/gamecam.cpp`). Mission-load texture loads happen during `Mission::loadAssets`, which completes synchronously before any `GameCamera::render` call. There is no async texture loader writing to `gosTextureHandle` while the render thread reads.

**Cache eviction inside `get_gosTextureHandle` itself** is the only way `gosTextureHandle` can mutate during a render frame. Lazy memoization preserves this by-construction: the first `tex_resolve(nodeId)` call this frame goes through the legacy accessor, which performs whatever eviction is needed; subsequent calls for the same `nodeId` read the memoized result. There is no window where the memoized value could go stale within a frame, because the accessor that mutates it is the same one that produced it.

If a future slice migrates the static-shadow accum's read at [`txmmgr.cpp:1228`](../../../mclib/txmmgr.cpp), it will simply first-touch through `tex_resolve` like any other reader; if it stays legacy under killswitch-OFF, the legacy path is unchanged. No lock or barrier needed.

## 7. Read-site conversion list

The terrain-solid in-scope conversions, with file:line and before/after. The table-read helper is a single inline:

```cpp
// mclib/tex_resolve_table.h
inline DWORD tex_resolve(DWORD nodeId) {
    if (!g_texResolveTable.enabled) {
        // Killswitch OFF: bit-exact legacy semantics.
        return mcTextureManager->get_gosTextureHandle(nodeId);
    }
    if (nodeId == 0xffffffff) return nodeId;                   // matches txmmgr.h:528–531
    DWORD h = g_texResolveTable.handles[nodeId];
    if (h != TexResolveTable::kSentinel) return h;             // memoized hit — fast path
    h = mcTextureManager->get_gosTextureHandle(nodeId);        // first touch this frame — legacy resolve
    g_texResolveTable.handles[nodeId] = h;
    g_texResolveTable.resolvedThisFrame++;
    return h;
}
```

`tex_resolve` is the **only** new symbol callers reference. It encapsulates the killswitch, the sentinel-fill check, the `0xffffffff` passthrough, and the lazy first-touch resolve.

### 7.1 Sites converted in M0a (terrain-solid path)

For each, the change is mechanical: replace the existing `mcTextureManager->get_gosTextureHandle(...)` or `masterTextureNodes[...].get_gosTextureHandle()` call with `tex_resolve(...)`.

| File:line | Before | After (under MC2_MODERN_TEX_RESOLVE=1) |
|---|---|---|
| [`mclib/quad.cpp:185`](../../../mclib/quad.cpp) | `mcTextureManager->get_gosTextureHandle(TerrainQuad::mineTextureHandle);` | `tex_resolve(TerrainQuad::mineTextureHandle);` |
| [`mclib/quad.cpp:193`](../../../mclib/quad.cpp) | `mcTextureManager->get_gosTextureHandle(TerrainQuad::blownTextureHandle);` | `tex_resolve(TerrainQuad::blownTextureHandle);` |
| [`mclib/quad.cpp:314, 322, 387, 395`](../../../mclib/quad.cpp) | same shape (mine/blown handles) | `tex_resolve(...)` |
| [`mclib/quad.cpp:1647, 1791, 2005, 2147`](../../../mclib/quad.cpp) | `mcTextureManager->get_gosTextureHandle(overlayHandle);` | `tex_resolve(overlayHandle);` |
| [`mclib/mapdata.cpp:317, 323, 329, 335, 434`](../../../mclib/mapdata.cpp) | `mcTextureManager->get_gosTextureHandle(entry.terrainHandle);` (etc.) | `tex_resolve(entry.terrainHandle);` |
| [`mclib/terrtxm.h:277, 285, 305, 313`](../../../mclib/terrtxm.h) | `return mcTextureManager->get_gosTextureHandle(textures[texture].mcTextureNodeIndex);` | `return tex_resolve(textures[texture].mcTextureNodeIndex);` |
| [`mclib/terrtxm2.h:134, 142, 150, 157, 166`](../../../mclib/terrtxm2.h) | same | `tex_resolve(...)` |
| [`mclib/terrtxm2.cpp:2384`](../../../mclib/terrtxm2.cpp) | same | `tex_resolve(...)` |
| [`mclib/txmmgr.cpp:1316, 1321`](../../../mclib/txmmgr.cpp) | `masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle()` | `tex_resolve(masterVertexNodes[i].textureIndex)` |

The `txmmgr.cpp:1316/1321` sites are inside `Render.TerrainSolid` flag-gate (`MC2_ISTERRAIN | MC2_DRAWSOLID`); they belong to the terrain-solid path explicitly.

### 7.2 Sites NOT converted in M0a (out-of-scope by design)

| File:line | Reason left on legacy path |
|---|---|
| [`txmmgr.cpp:1114, 1125`](../../../mclib/txmmgr.cpp) | `Render.3DObjects` arm (mech/building shapes), non-terrain |
| [`txmmgr.cpp:1228`](../../../mclib/txmmgr.cpp) | `Shadow.StaticAccum` re-render (deferred — runs >100-unit camera-move only) |
| [`txmmgr.cpp:1429–1833`](../../../mclib/txmmgr.cpp) | water / alpha / decals / overlays — non-terrain-solid |
| [`crater.cpp:284, 289, 554`](../../../mclib/crater.cpp) | crater pass — separate path; future shape |
| [`cellip.cpp:62`](../../../mclib/cellip.cpp), [`gvactor.cpp:1134, 1270`](../../../mclib/gvactor.cpp), [`mech3d.cpp:1629, 1784`](../../../mclib/mech3d.cpp) | non-terrain |
| [`mlr/gosimage.{cpp,hpp}`](../../../mclib/mlr/gosimage.cpp) | UI/image; non-terrain |
| [`utilities.cpp:167, 174, 217, 308`](../../../mclib/utilities.cpp), [`tgl.cpp:1534`](../../../mclib/tgl.cpp) | mission-load / type-registration; not per-frame |

Each line in this table will be checked into the residual-call census (§12) so future audit can prove the omissions are intentional.

### 7.3 Q-A3 (`TerrainColorMap::getTextureHandle realizeTexture`)

The 15.7M-call zone at [`mclib/terrtxm2.cpp:2384`](../../../mclib/terrtxm2.cpp) and the `terrtxm{,2}.h` accessors are *already* in the conversion list above (§7.1 rows for `terrtxm.h` and `terrtxm2.h`). It is the same hot path — the function ultimately resolves through `mcTextureManager->get_gosTextureHandle(nodeIndex)`. Folding it into Shape A is free.

## 8. Killswitch and validate-mode

### 8.1 `MC2_MODERN_TEX_RESOLVE` (default OFF in initial commit; promotion gated)

Single env var. Read once at process start; cached in `g_texResolveTable.enabled`. When OFF:
- `beginFrameTexResolve()` is a no-op (does not touch the table).
- `tex_resolve()` falls through to `mcTextureManager->get_gosTextureHandle(nodeId)` — bit-exact legacy semantics.
- No partial state. No "table initialized but not consumed" or "consumed but not initialized" half-state, because both the init and the read gate on the same flag.

**Initial default: OFF.** Deliberately. Promotion to default-ON requires:
1. At least one Wolfman-zoom + Magic-canary capture run with `MC2_MODERN_TEX_RESOLVE_VALIDATE=1` showing zero `event=mismatch` lines over a 60-second mission.
2. Tracy A/B showing a measured ≥0.2 ms/frame self-time reduction at Wolfman across the three target zones (§11), with no regression on `Terrain.DrawPatches` or `Render.TerrainSolid`.
3. Tier1 + menu canary + Magic mission canary green.

Until those three are satisfied the env stays OFF and the converted callsites still take the legacy fall-through.

### 8.2 `MC2_MODERN_TEX_RESOLVE_VALIDATE` (default OFF; debug-only)

When set, every `tex_resolve(nodeId)` call also computes the legacy result (`mcTextureManager->get_gosTextureHandle(nodeId)`) and asserts equality. On mismatch:

```
[TEX_RESOLVE v1] event=mismatch frame=N nodeId=X table=H1 legacy=H2 site=<file:line>
```

Validate mode is a debug build mode, not a perf measurement mode (the legacy path defeats the win). It must be run at least once before promotion, and may be left on for the first few sessions after promotion to catch regressions during the bake.

The implementation plan must include: at least one Wolfman + at least one Magic mission validate-mode run before flipping the default to ON.

### 8.3 Frame-generation safety

`tex_resolve()` does **not** assert against `buildGeneration` in production (cost on the hot path defeats the win). Validate mode does:

```cpp
if (g_texResolveTable.buildGeneration != g_currentFrameId) {
    [TEX_RESOLVE v1] event=stale_generation expected=N got=M nodeId=X
    return mcTextureManager->get_gosTextureHandle(nodeId);  // safe fallback
}
```

This catches "table never initialized this frame" (forgot to call `beginFrameTexResolve()`) and "two frames running concurrently" (impossible today, but cheap to detect should anyone introduce a thread-pool render path).

## 9. Instrumentation — `[TEX_RESOLVE v1]` event vocabulary

Per worktree CLAUDE.md "Debug Instrumentation Rule for reworks", Shape A lands with env-gated lifecycle prints in the same commit. Default OFF; demote-don't-delete after promotion.

```
[TEX_RESOLVE v1] event=startup mode=<off|on|validate> max_textures=N
[TEX_RESOLVE v1] event=begin_frame frame=N                                 (trace mode only)
[TEX_RESOLVE v1] event=mismatch frame=N nodeId=X table=H1 legacy=H2 site=<file:line>
[TEX_RESOLVE v1] event=stale_generation expected=N got=M nodeId=X
[TEX_RESOLVE v1] event=residual_legacy_read site=<file:line> count=K       (validate-only; §12)
[TEX_RESOLVE v1] event=summary frames=N resolved_per_frame_avg=R reset_us_avg=T
[TEX_RESOLVE v1] event=shutdown total_frames=N
```

`event=startup` and `event=summary` (every 600 frames + on shutdown) are always-on regardless of env. The summary line carries `resolved_per_frame_avg` (the count of distinct `nodeId`s first-touched per frame, expected ≤ a few hundred) and `reset_us_avg` (per-frame `memset` cost, expected <5 µs). Everything else is gated on `MC2_MODERN_TEX_RESOLVE_TRACE` (lifecycle events) or `MC2_MODERN_TEX_RESOLVE_VALIDATE` (mismatch / stale / residual).

Print rule per CLAUDE.md: lifecycle boundaries only, never per-frame at 60 FPS in default production. The 600-frame summary is the standard floor.

## 10. Risk register (Shape A)

Carries forward AR1–AR3 from the brainstorm; adds AR4–AR6 from the audit.

| # | Risk | Canary | Mitigation |
|---|---|---|---|
| AR1 | **Live-handle violation (C10) — table caches across frames.** | Stale or zero handle on a node that was evicted; black/wrong terrain patch mid-mission. | Per-frame `beginFrameTexResolve` re-fills with sentinel (§6.2). `buildGeneration` field + validate-mode `event=stale_generation` detects a missed init call. |
| AR2 | **Coverage gap — some terrain reads stay on legacy path.** | Tracy `get_gosTextureHandle` zone delta smaller than expected (~0.2 ms instead of ~0.4 ms). | Conversion list (§7.1) checked against grep output by the implementation plan. Residual-call census (§12) runs in validate-mode before promotion. |
| AR3 | **Cache-behavior perturbation — table eagerly resolves nodes the frame would not have touched.** *(Mitigated by design.)* | Different LRU-eviction order; later-mission slowdowns or texture-realization patterns that don't match legacy. | The lazy first-touch design (§6.3) eliminates this risk by construction — `tex_resolve` calls the legacy accessor for exactly the same `nodeId`s, in exactly the same draw-time order, that legacy mode would. The only difference is repeated calls collapse to one. |
| AR4 | **`MC_TextureNode` not terrain-specific** — assumed in brainstorm but contradicted by source. | Conversion designed for `MC2_ISTERRAIN` filter would have under-covered. | Resolved at spec time by reading `txmmgr.h:128–203` (no flag field). Spec rejects flag-gated walk; chooses index-keyed lazy memoization. |
| AR5 | **Stale shader cache mimic (R12 from findings §9).** | Frozen-cloud or over-darkened terrain after deploy that bisects to a non-shader commit. | Force shader cache clear during smoke before bisecting any Shape A regression. Same mitigation as every render-touching commit. |
| AR6 | **Cap miss — `MC_MAXTEXTURES` raised in some build but `g_texResolveTable.handles[]` not.** | OOB write/read on `nodeId >= MC_MAXTEXTURES`. | Static-assert: `static_assert(sizeof(g_texResolveTable.handles)/sizeof(uint32_t) >= MC_MAXTEXTURES)`. `tex_resolve()` bounds-checks in validate mode. |
| AR7 | **`kSentinel` collision — a legitimate resolved handle equals `0xFFFFFFFF`.** | A node whose real handle is `0xFFFFFFFF` would be re-resolved every read instead of memoized. | `MC_TextureNode::get_gosTextureHandle()` at `txmmgr.cpp:2290–2296` explicitly returns `0x0` (not `0xFFFFFFFF`) for the bad-handle case; legitimate handles are GOS texture IDs which are never `0xFFFFFFFF`. Validate-mode `event=mismatch` would catch any real instance. If a real collision is ever observed, swap the sentinel encoding to a side-channel bit in `buildGeneration`-tagged storage. |

Risks **not** specific to Shape A (carried forward from findings §9 for completeness): R4 (AMD breakage — but Shape A touches no shaders or GL state so is at lowest risk class), R6/R7 (mod-content; Magic canary covers), R12 (shader-cache mimic; AR5 above).

## 11. Measurement plan

### 11.1 Tracy zones to watch

Capture before and after over identical mission + zoom + duration:

| Zone | Expected change | Pass threshold |
|---|---|---|
| `MC_TextureNode::get_gosTextureHandle` | Self-time drops; call count drops by ≥80% | ≥0.15 ms/frame self-time reduction at Wolfman |
| `TerrainQuad::setupTextures cachedVisibleSubmission` | Self-time stable (the work moved, not eliminated) | ±10% |
| `TerrainQuad::setupTextures resolveFallback` | Self-time drops ≤0.1 ms | non-negative delta |
| `TerrainColorMap::getTextureHandle realizeTexture` | Self-time drops (folded into table; §7.3) | ≥0.05 ms/frame reduction |
| `Terrain.DrawPatches` (GPU) | Unchanged | within ±5% (no GPU change) |
| `Render.TerrainSolid` (CPU) | Marginal drop from converted reads at `txmmgr.cpp:1316,1321` | non-negative delta |
| `Camera.BeginFrameTexResolve` (new) | New zone; sentinel-fill memset only | <5 µs/frame |

**Combined pass threshold for Shape A delivered:** sum of (`get_gosTextureHandle` + `resolveFallback` + `realizeTexture`) self-time delta ≥ 0.20 ms/frame at Wolfman, with no other tracked zone regressing >5%.

### 11.2 Capture protocol

Two missions, each with a 60-second tier1-style passive smoke. Tracy capture, exported as `.tracy` snapshot (no need for CSV at this slice).

| Mission | Zoom mode | Duration | Why |
|---|---|---|---|
| `mc2_01` | Standard | 60 s | Baseline regression (canonical first-mission visual check) |
| `mc2_01` | Wolfman | 60 s | Perf claim target — 200²=40000 visible verts is the hot scenario |

Both runs with `MC2_GL_ERROR_DRAIN_SILENT=0` and `MC2_HEARTBEAT=1` per CLAUDE.md "Tier-1 Instrumentation". Validate mode run *additionally* before promotion (§8.1).

### 11.3 If the threshold is missed

The implementation plan must define the bail criterion. Recommended: if combined delta is <0.10 ms/frame (half of the pass threshold) after a clean Wolfman capture, **revert and re-evaluate** rather than promote. Possible explanations would be:
- The `lastUsed = turn` write inside `get_gosTextureHandle` was the dominant cost, not the function call (Shape A would still help by removing 96M of those writes — but the rest of the function body is so cheap that the win is smaller than estimated).
- The 96M number was inflated by multiple-overload counting in the screenshot extraction.
- A different shape (Shape B or C) is the actual win.

A failed measurement does not mean the design is wrong; it means the prioritization in the brainstorm was. That is information, not failure.

## 12. Residual-call census

A grep + counter approach to prove which `get_gosTextureHandle` callsites remain on the legacy path after Shape A lands, and that they are intentionally out of scope (per §7.2) rather than missed conversions.

### 12.1 Grep census (mechanical)

```bash
# After Shape A lands, this count should equal exactly the §7.2 list:
grep -rn 'get_gosTextureHandle\s*(' mclib/ GameOS/gameos/ code/ \
  | grep -v 'tex_resolve_table' \
  | grep -v 'event=residual'
```

Baseline (today): all callsites in §3.3. Post-Shape-A: only the §7.2 list. The diff between the two lists must equal the §7.1 conversion list — checked into the implementation plan as a test.

### 12.2 Runtime counter (validate mode)

Each remaining legacy callsite gets a per-callsite counter. Validate mode emits `[TEX_RESOLVE v1] event=residual_legacy_read site=<file:line> count=K` once per 600 frames per site. The plan must include reading this output and confirming each entry maps to a §7.2 row.

This is a one-shot instrumentation; it is removed from the legacy callsites *after* the residual-call census is documented in the plan's closing report. The instrumentation in `tex_resolve()` itself stays gated.

## 13. Smoke gate

Standard worktree smoke (per CLAUDE.md "Smoke Gate"):

```
py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
    --tier tier1 --with-menu-canary --kill-existing
```

Plus three Shape-A-specific canaries:
1. **`mc2_01` Wolfman zoom 60-s heartbeat** — perf claim target.
2. **Magic install canary** — covers R6/R7 (mod-content interaction). Per CLAUDE.md "Memory & CLAUDE.md Discipline" + memory `magic_abl_contamination_rule.md`, do **not** ship loose `corebrain.abx` in the test deploy.
3. **Validate-mode run on `mc2_01` standard + Wolfman** — at least one full run with `MC2_MODERN_TEX_RESOLVE_VALIDATE=1` showing zero mismatch lines, before promotion.

Static-shadow A/B (`RAlt+F3`) is *not* required at M0a because Shape A doesn't touch the static-shadow read path (per §4.3). It becomes required if a future slice migrates `txmmgr.cpp:1228`.

## 14. Carry-forward open questions for the implementation plan

The spec has answered the brainstorm's intake questions; the implementation plan inherits these residuals:

- **OQ-Plan-1.** Should the `tex_resolve()` inline live in a new header (`mclib/tex_resolve_table.h`) or be folded into `mclib/txmmgr.h` next to the existing manager wrapper at line 526? Spec recommends new header for clean ownership and minimal `txmmgr.h` churn.
- **OQ-Plan-2.** Should `beginFrameTexResolve` live in `mclib/tex_resolve_table.cpp` (called from `code/gamecam.cpp`) or be a static method on `MC_TextureManager`? Spec recommends standalone in the new TU for separation of concerns; Shape A is a sidecar, not an extension of the manager.
- **OQ-Plan-3.** Naming: `tex_resolve()` vs `mcTextureManager->resolve()` vs a `MC_TextureManager` member? Spec recommends standalone free function to make grep-replacement of legacy callers mechanical and to make the residual-call census trivial.
- **OQ-Plan-4.** The lazy memoization is sentinel-keyed by `0xFFFFFFFF`. AR7 documents why this is safe today. The plan should still grep for any `gosTextureHandle == 0xFFFFFFFF` write paths it has not seen and confirm the assumption holds, or switch to a side-channel "resolved this frame" bitset if any real collision is found.
- **OQ-Plan-5.** Static-shadow accumulation (`txmmgr.cpp:1228`) is left on legacy in M0a (§4.3). Once the §11.1 measurement passes, it can be folded in opportunistically — one extra `tex_resolve` swap, zero added risk because lazy first-touch handles whatever read order the accum chooses.

---

## Ready for implementation plan?

**Spec author's recommendation: Yes.**

- Structural questions are resolved (key choice, table shape, init timing, lazy resolve algorithm, killswitch and validate-mode contract, instrumentation vocabulary, residual-call census method, smoke gate, measurement threshold).
- The population strategy is a deliberate departure from the brainstorm in two steps: rejecting flag-gated walk (because `MC_TextureNode` has no flags field, §3.2/§6.3), then rejecting the spec's earlier eager populate-all (because it perturbs the LRU and realizes unused textures, §1/§6.3 advisor correction). Lazy first-touch memoization is the design.
- The perf claim is bounded by the measurement plan (§11). If the threshold is missed, the plan must include a clean revert path — Shape A is small enough that revert is one commit.
- OQ-Plan-1..5 are local code-organization residuals; none block plan-writing.

**Operator answer:** _________________________________________

(Yes / No / What's missing — fill in before transitioning to `superpowers:writing-plans`.)
