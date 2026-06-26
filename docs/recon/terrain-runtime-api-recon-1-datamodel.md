# TERRAIN-RUNTIME-API-RECON-1 — Data Model & Render-Height Path

Read-only recon. Branch `claude/nifty-mendeleev`. All file:line verified against current code 2026-06-25.

## 1. Where terrain HEIGHT physically lives

**Single CPU source of truth: `MapData::blocks[]`, an array of `PostcompVertex`.**

- `struct PostcompVertex` — `mclib/vertex.h:33`. Field `float elevation` at byte offset 12 (`vertex.h:44`); 32-byte stride. Also holds `vertexNormal`, `textureData`, `localRGBLight`, `terrainType`, `water`, `shadow`.
- `.pak load`: `MapData::newInit(PacketFile*, numVertices)` — `mclib/mapdata.cpp:205` → `newFile->readPacket(getCurrentPacket(), (MemoryPtr)blocks)` `mapdata.cpp:209`. The packet is `memcpy`'d **directly** into `blocks[]` (PostcompVertex is the on-disk packet layout). Then `calcTransitions()` `mapdata.cpp:211`.
- Indexing is map-stable: `blocks[mx + my * Terrain::realVerticesMapSide]` (see `terrainElevation(y,x)` `mapdata.cpp:1412`). This is the `Vertex::vertexNum` key (`mapdata.cpp:1104`), NOT the camera-windowed `quadList` slot (memory: quadlist_is_camera_windowed).

**GPU uploads — all derived FROM `blocks[].elevation`, none is an independent source:**

| GPU resource | Binding | Upload site | Source |
|---|---|---|---|
| R32F height **texture** (sampler unit 11) | tex unit 11 | `gos_uploadTerrainHeightTex(...)` called `terrain.cpp:718` | `mapData->getBlocks()`, elev offset 12 |
| **TerrainHeightBuf SSBO** `float heights[]` | binding **23** (`TERRAIN_HEIGHT_SSBO_BINDING=23u`, `gos_terrain_lod_chunk.h:16`) | `gos_TerrainLodChunk_UploadHeightFull` `gos_terrain_lod_chunk.cpp:1014`; bound `:628/:1029`; filled `elev[i]=blks[i].elevation` `terrain.cpp:811` | `blocks[].elevation` |
| **Indirect recipe SSBO** (`TerrainQuadRecipe.wz0..wz3`) | binding 0 (compute) | `buildRecipeSlot` `gos_terrain_indirect.cpp:613`; `out.wzN = pN.elevation` `:653-656` | `Terrain::mapData->getBlocks()` `:627` |
| terrainType SSBO / cement SSBO | 24 / 25 (`gos_terrain_lod_chunk.h:17-18`) | same lod_chunk path | `blocks[].terrainType` / cement layer map |

Flow: `.pak packet-0` → `readPacket` → `blocks[] (PostcompVertex.elevation)` → {height tex unit 11 | SSBO@23 | recipe SSBO wzN}.

## 2. RENDER height == GAMEPLAY height? — YES, single source today.

- **Gameplay height:** `Terrain::getTerrainElevation(Vector3D)` `terrain.cpp:3638` → `mapData->terrainElevation(pos)` `mapdata.cpp:2189` (3-tri barycentric interp) and `getTerrainElevation(r,c)` `terrain.cpp:3645` → `terrainElevation(y,x)` `mapdata.cpp:1407` → `blocks[index].elevation` `:1413`.
- **Render height:** all three GPU paths above pull the SAME `blocks[].elevation`.
- **No numeric split exists.** The only "split" is documentary intent, not a divergent value: `terrain.cpp:711-716` states the R32F height tex is "Visual-only; gameplay height (getTerrainElevation) remains authoritative." But that tex is currently used only to derive *normals* (`computeTerrainNormalFromHeight`, `terrain_height_normal.hglsl:59`), not to displace render geometry, and its bytes are copies of the same elevations. The LOD-chunk vert (binding 23) DOES drive render geometry and reads the identical elevations. So render Z == gameplay Z by construction; any future TerrainRuntime API can treat `blocks[].elevation` as the one authoritative field.

## 3. MATERIAL / OVERLAY / texture-id fields in the data model

- **`PostcompVertex.textureData`** (DWORD) `vertex.h:46`: "Top word = Overlay TXM, Bottom word = Base TXM."
  - Base TXM = `textureData & 0x0000ffff` (`mapdata.cpp:279`).
  - Overlay TXM = `textureData >> 16` (`mapdata.cpp:540, 1464`).
  - Accessor `MapData::getTexture(y,x)` returns full DWORD `mapdata.cpp:1402`.
- **`PostcompVertex.terrainType`** (DWORD) `vertex.h:50` — concrete/terrain-type selection (cement boundary blend).
- **Cement-word** (derived, not stored in PostcompVertex): baked into `TerrainQuadRecipe._wp3` by `PopulateRecipeCementWords` `gos_terrain_indirect.cpp:846`, keyed off `getTexture(my,mx)` `:869` → `g_cementLayerIndexBySlot`. bit31 valid | low16 layer idx.
- **Sampling at render time:** indirect path → recipe `_wp3` → thin record `cementWord` → `gos_terrain.frag`. LOD-chunk path → terrainType SSBO@24 interpolated to frag (`terrain_lod_chunk.vert:83`), cement SSBO@25. Base/overlay TXM resolved CPU-side in `quad.cpp setupTextures` → `getTextureHandle` (`quad.cpp:~546`).

## 4. Render-path consumers reading height/material

| Consumer | file:line | height field | material field | Classification |
|---|---|---|---|---|
| `MapData::terrainElevation` (gameplay) | mapdata.cpp:1407 / :2189 | blocks[].elevation | — | **gameplay-height** |
| `terrain_lod_chunk.vert` (RenderWorld LOD draw) | terrain_lod_chunk.vert:45,82 | SSBO@23 heights[] (=elevation) | terrainTypes@24, cement@25 | **visual-height + material** |
| `gos_terrain_indirect` recipe (`buildRecipeSlot`) | gos_terrain_indirect.cpp:653-656 | pN.elevation | `_wp3` cementWord :846 | **visual-height + material** |
| thin records (`TerrainQuadThinRecord`) | gos_terrain_indirect.cpp (compute pack) | (none — carries recipeIdx) | cementWord, terrainHandle, flags | **material / feature-mask** |
| height R32F tex / `terrain_height_normal.hglsl` | terrain.cpp:718; hglsl:59 | tex unit 11 (=elevation) | — (normals only) | **visual-height** (normal-derivation only) |
| water fast path (`gos_terrain_water_fast.vert`) | water_fast.vert:198 | own per-vertex water vtx stream (elevation band) | water bits / alpha band | **legacy-direct-packet** (own VBO, not SSBO@23) |
| `quad.cpp setupTextures/draw` | quad.cpp:~540 | pVertex->elevation (parity check :1596) | textureData → getTextureHandle | **legacy-direct-packet** (CPU walk) |

**Verdict for TerrainRuntime API:** `blocks[]` (PostcompVertex) is the one authoritative struct; `elevation`+`textureData`+`terrainType` are the three queryable fields; every GPU height resource is a derived mirror, so a compat API wrapping `getTerrainElevation`/`getTexture`/`getTerrain` covers all current consumers with no render/gameplay split to reconcile.
