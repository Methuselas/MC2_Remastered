# GPU-RESOURCE-MANAGER-SEAM-RECON-1 — first true backend seam (recon)

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC option C · recon only, no code · 2026-06-22
**Purpose:** confirm exact GPU texture/buffer/material slot-table ownership + external callsites, so a future `GPU-RESOURCE-MANAGER-GL-ONLY-1` could move resource *creation/ownership* behind a GL-only object while keeping the `gos_*` API. Do NOT propose `IRenderBackend`/`RenderDevice`/`CommandContext` (premature).

## Ownership map

### Textures — TWO layers, clean split already exists
- **Layer A (game-facing, NO raw GL):** `MC_TextureManager` (`mclib/txmmgr.cpp`). Owns `masterTextureNodes` (`MC_TextureNode[4096]`, alloc `:419`, free `:547`). Its only GL-adjacent field is `DWORD gosTextureHandle` (`txmmgr.h:152`, **protected**, accessed via `get_gosTextureHandle(nodeId)` `txmmgr.h:609`) — an OPAQUE index, not a GL id. Cache eviction in `update()` (`txmmgr.cpp:834-862`, PAUSE/UNPAUSE).
- **Layer B (engine, the GL owner):** `gosRenderer::textureList_` (`std::vector<gosTexture*>`, `gameos_graphics.cpp:2027`). `GLuint` lives in `gosTexture` (`:875`); `glGenTextures/glDeleteTextures` happen inside `gosTexture::createHardwareTexture()`. API: `addTexture` `:1404`, `getTexture` `:1492`, `deleteTexture` `:1508`. `gos_*` entry points are thin wrappers.

### Buffers — FRAGMENTED (the opposite of textures)
- `HGOSBUFFER` (`gos_CreateBuffer` `:8467` / `gos_UpdateBuffer` `:8515` / `gos_DestroyBuffer`) wraps only a small minority (`gosRenderer::bufferList_`).
- ~15 **private file-static `GLuint`s** self-manage the hot paths: light SSBO (`s_lightDataSsbo:8609`), terrain/surface/indirect, static-prop batcher (`s_instanceSsbo`/`s_materialGpuSsbo:3872` binding 5/etc.), mech batcher (`s_instanceSsbo`/`s_boneSsbo`/`s_mechMaterialSsbo`), mech-profile (`gos_materials.cpp` `s_ssbo:58` binding 7).

### Materials — SPLIT-BRAIN
- Static-prop table binding **5** (`gos_static_prop_batcher.cpp` `s_materialGpuSsbo`); mech-profile table binding **7** (`gos_materials.cpp`). Header flags debt **D-material-unify**. `RenderCore::MaterialGpu` struct is shared (std430, `check-material-gpu-mirror.sh`) but `albedoTex` semantics diverge per consumer.

### RenderResourceRegistry — descriptive only, NOT a lifetime owner
Explicit (`RenderResourceRegistry.h:9`): "descriptive only; owners retain GL lifetimes", `glName` is debug-only. Populated by GameOS RT owners (postprocess, terrain-height). `MaterialGpuBuffer` slot declared but unpopulated.

## External raw-GL leaks — effectively NONE
`code/` is clean of raw GL (M6 firewall `check-no-raw-gl-from-game.sh` holds). `mclib/txmmgr.cpp` allowlisted only for diagnostic-gated probes. No raw `GLuint` crosses into game logic — mclib deals in the `gosTextureHandle` DWORD. The "external leak" class the seam was meant to encapsulate **does not exist** for textures/HGOSBUFFER; the real fragmentation is *internal to GameOS* (the private buffer owners).

## Recommended MINIMAL first seam: TEXTURES
`gosRenderer::textureList_` → `gosTexture`. Most centralized ownership (one vector, one create/get/delete API), zero external leakers (firewall-clean + gosHandle indirection), clean policy/owner split already present. A GL-only `GlTextureManager` would formalize `gosRenderer`'s texture half behind the unchanged `gos_NewTexture*/gos_DestroyTexture` API + `gosTextureHandle` DWORD contract → `mclib`/`code` need ZERO changes.

**Do NOT start with buffers** (fragmented across ~15 owners + bespoke onMapLoad/Unload lifecycles in foreign-WIP-dirty files) **or materials** (split-brain bindings 5/7, unresolved D-material-unify, per-consumer texture semantics — a design milestone, not a mechanical extraction).

## Risks / caveats
- **The texture seam may already be ~done**: the gosHandle indirection + M6 firewall already deliver most of the GL-only encapsulation value. Confirm a `GlTextureManager` extraction earns its relink (Vulkan-prep clarity) before spending it — or it is churn like the rejected flat-binding-enum.
- **Cache-eviction coupling**: `MC_TextureManager::update()` evicts/recreates GL textures mid-frame (CACHED_OUT sentinel `0xFFFFFACE`). Any GL-only owner must preserve the cache-in/out protocol or resurrect the static-render-bug class.
- **`gosTexture` constructor variety**: from-memory TGA / compressed BC7 / from-file / asset-viewer raw-GL-id — all must be covered.
- **Foreign-WIP hazard**: touching `txmmgr.h`'s `MC_TextureNode`/`get_gosTextureHandle` collides with the dirty `txmmgr.h`. Run `slice-preflight --symbols get_gosTextureHandle,gosTextureHandle,addTexture,deleteTexture` first and prefer extracting on the `gameos_graphics.cpp` side.
- **Do NOT extend RenderResourceRegistry for lifetime** — it would break its "descriptive only" invariant; publish into it from the new owner instead.

## Verdict
Texture GL-only extraction is the lowest-risk first true seam, but is gated on proving it earns its keep. The contract-manifest arc (slices 1–5 + A) should be banked first; this is the next *implementation* arc, not part of the manifest arc.
