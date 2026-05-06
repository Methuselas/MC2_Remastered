# Cement Multi-Sampler Implementation Plan (Stage 4 — indirect-terrain PR2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking. Verification Appendix at the end — every cited symbol
> grep'd against the actual source tree. **This plan IS Stage 4 of the indirect-terrain arc.**

**Goal:** Pure cement quads in indirect-terrain SOLID draw render with their authored CONCRETE
catalog texture (matching the legacy M2 path's per-bucket binding for cement quads), via a
single cement-catalog atlas bound at sampler unit 3 (`tex3` — currently declared but unused).
The fragment shader selects between the colormap atlas (unit 0, `tex1`) and cement catalog atlas
(unit 3, `tex3`) using `TerrainType` (already per-fragment via recipe `_wp0`). Per-quad
cement-layer-index lives in `TerrainQuadThinRecord._pad0` (currently `0u` — no schema growth).
Default-on flip happens in this same commit (Q6 bundle): `MC2_TERRAIN_INDIRECT` becomes
default-on; killswitch `=0` opt-out preserved.

**Commit tag:** `PR2` (PR1 = f221570+a29ff83, PR2 = this slice).

**Architecture:** The multi-sampler approach (Option C from user's decision tree). Three schema
changes avoided: no new fields on `TerrainQuadRecipe` (frozen at 144B/9-vec4, V_T), no new
varying to `gos_terrain.frag` (V_Y linker-compatibility constraint), no new sampler declaration
(V_X `tex3` already declared). The cement atlas is a GL_TEXTURE_2D packed grid, built at
`BuildDenseRecipe()` time while `tileRAMHeap` is still alive (see LIFECYCLE HAZARD note below).
Multi-layer generalization slot is `_pad0` low byte: cement layer-index occupies bits 7:0; upper
24 bits reserved for future layers (decals, overlays, scorch) per Q-Future guidance.

**LIFECYCLE HAZARD (load-bearing):** `Terrain::terrainTextures->tileRAMHeap` is freed by
`TerrainTextures::update()` called in `Mission::update()` at
`code/mission.cpp:497` — i.e., on the FIRST frame after mission start. This means
`types[i].textureData[0]` (raw pixel RAM for base cement types) is ONLY accessible during
`BuildDenseRecipe()` time (`primeMissionTerrainCache` at `code/mission.cpp:2218`), NOT in the
per-frame packer. The cement atlas MUST be built inside `BuildCementCatalogAtlas()`, called from
`BuildDenseRecipe()` — same lifecycle as `BuildColormapAtlas()`. The per-frame packer merely
reads the cement layer-index from each quad's cache entry (already populated at face-cache build
time).

---

## Source documents (read-first for executor)

- **Brainstorm (Q1–Q9 user-settled):** `docs/superpowers/brainstorms/2026-05-01-cement-multi-sampler-scope.md`
- **Parent plan (mirror structure):** `docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md`
- **Parent design (9 GPU-direct gotchas, AMD rules):** `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`
- **Water SSBO pattern template:** `memory/water_ssbo_pattern.md`
- **GPU-direct bring-up checklist:** `memory/gpu_direct_renderer_bringup_checklist.md`
- **Texture handle is live:** `memory/mc2_texture_handle_is_live.md`
- **ARGB packing:** `memory/mc2_argb_packing.md`
- **AMD driver rules:** `docs/amd-driver-rules.md`

---

## User decisions (FROZEN — do not relitigate)

| Q | Decision |
|---|---|
| Q1 | Pure cement only. Alpha-cement base is correct; overlay stays legacy. |
| Q2 | Single sampler at unit 3 (`tex3`). Repurpose existing declaration. No new sampler uniform. |
| Q3 | Dynamic enumeration via `TerrainTextures::isCement()` walk at atlas-build time. Runtime count printed via `MC2_TERRAIN_INDIRECT_TRACE`. |
| Q4 | Per-quad cement catalog index in `TerrainQuadThinRecord._pad0` low byte (0-indexed atlas column). |
| Q5 | Measure at runtime; small fraction expected (~5-15% airport missions). Print in trace. |
| Q6 | THIS slice IS Stage 4. Cement fix + default-on flip in one commit. |
| Q7 | Visual canary: mc2_01 airport tarmac, fixed-camera position. See Gate A specification below. |
| Q8 | M2 safe: `tex3` cement sampling gated on `useAtlasColormap != 0`. |
| Q9 | Atlas memory negligible (~1-2 MB). |
| Future-proof | Layer-index in `_pad0` low byte; upper 24 bits reserved. Architecture accepts N future layers. |

---

## Out of scope (explicit rejection list)

- **Alpha-cement overlay path.** Alpha-cement base is already correct on PR1; overlay stays legacy.
- **Runway markings / decals.** `gos_PushTerrainOverlay` stays legacy. Target 2 brainstorm.
- **Recipe field growth.** `TerrainQuadRecipe` stays frozen at 144B / 9 vec4. See V_T.
- **New varying to `gos_terrain.frag`.** Linker-compatibility constraint. See V_Y.
- **New sampler declaration.** `tex3` already declared; repurpose in place. See V_X.
- **M2 fast path changes.** M2 handles cement correctly via per-bucket binds. No M2 change.
- **sampler2DArray approach.** Blocked on AMD Canary B. See V_Z2.
- **Per-bucket draw for cement quads.** Defeats indirect; not Option C.
- **Baking cement art into cpuColorMap.** Option B; not selected.
- **Adding `>1` atlas layers in this slice.** Architecture generalizes; this slice ships 1 layer.
- **Physical deletion of legacy SOLID path.** Post-soak follow-up.
- **Mod content validation.** Per `memory/feedback_offload_scope_stock_only.md`.

---

## File structure (touched in this slice)

| File | Stage(s) | Responsibility |
|---|---|---|
| `GameOS/gameos/gos_terrain_indirect.cpp` | A, B, C | Stage A: `BuildCementCatalogAtlas()` + `g_cementAtlas*` statics + cement-layer-index map build; cement trace print. Stage B: bridge accessors for cement atlas. Stage C: default-on flip in `IsEnabled()`. |
| `GameOS/gameos/gos_terrain_indirect.h` | A | New `BuildCementCatalogAtlas` declaration; bridge accessor prototypes. |
| `GameOS/gameos/gameos_graphics.cpp` | B | `gos_terrain_bridge_drawIndirect`: bind cement atlas at unit 3, set `useCementAtlas` + `atlasCementOneOverDim` uniforms, save/restore unit-3 binding. Reset `useCementAtlas` after draw. |
| `shaders/gos_terrain.frag` | B | Add `uniform int useCementAtlas` (new) + `uniform float atlasCementOneOverDim` (new). Add cement atlas sampling branch in the `useAtlasColormap != 0` block: when `pureConcrete >= 0.99` and `useCementAtlas != 0`, sample `tex3` at atlas UV; else fall through to `tex1` colormap. |
| `GameOS/gameos/gos_terrain_indirect.cpp` (packer) | A | In `PackThinRecordsForFrame()`: populate `tr._pad0` low byte with cement layer-index from the per-quad cache entry (or 0 if not cement). |
| `shaders/gos_terrain_thin.vert` | A | Pass `_pad0` from thin record to frag via `thinRecs[recordIdx].control.w`. No new varying. The frag reads `_pad0` from the SSBO indirectly — see architecture note below. |
| `memory/indirect_terrain_solid_endpoint.md` (NEW) | C | Slice closeout memory. |
| `memory/MEMORY.md` | C | Index entry for new memory file. |

**Architecture note on `_pad0` → frag access:** The frag cannot read SSBOs (it runs after vertex
shading). The thin VS at `gos_terrain_thin.vert:84` reads `tr.control.w` (`_pad0`). Passing it
to the frag requires either (a) a new `flat int` varying (blocked — see V_Y linker constraint),
or (b) encoding the cement layer-index into an existing interpolated varying. The chosen approach:
encode the layer-index as a compact bias on `TerrainType`. Pure-cement fragments already have
`TerrainType` driven to 3.0 by the VS. We extend the encoding: for pure-cement quads, we set
`TerrainType = 3.0 + (float(_pad0) / 255.0)` — a sub-integer bias in (3.0, 4.0). The frag gates
on `floor(TerrainType) == 3.0` (i.e., `TerrainType >= 3.0 && TerrainType < 4.0`) for the cement
atlas branch. The layer-index is decoded as `round((TerrainType - 3.0) * 255.0)`. This avoids any
new varying and is compatible with the legacy non-thin VS chain (which never drives pure-cement
fragments to TerrainType >= 3 via the smoothstep gate, so `floor(TerrainType)` never reaches 3
for non-cement vertices on that path).

**Max cement texture slots:** `nextAvailable` is bounded by `MC_MAX_TERRAIN_TXMS = 3000` (V_W2).
After `purgeTransitions()`, at most `firstTransition` slots are occupied (the base-type slots;
typically 9 cement + non-cement base types). In worst case (many cement-cement transitions), the
cement slot count stays well under 255 (the layer-index range). Atlas packed as a 1D row of
`N` tiles, each `TERRAIN_TXM_SIZE × TERRAIN_TXM_SIZE`. For N≤255 and TERRAIN_TXM_SIZE=64: atlas
is at most 255×64 × 64×1 = 16320×64 → pack as 16×16 grid with padding for up to 256 entries.
Practical stock missions: N ≤ 30. A 6×5 (or 8×8) grid suffices for all stock missions.

---

## Stage A: Catalog enumeration, atlas build, layer-index wiring

**Scope:** Build the cement-catalog atlas GL texture at `BuildDenseRecipe()` time. Walk all
`TerrainTextures::textures[0..nextAvailable-1]` entries with `isCement()` flag set. Pack them
into a power-of-2 grid atlas. Build a dense `g_cementLayerIndexByTxmSlot[]` lookup (txm slot →
0-indexed atlas row in grid). Populate `tr._pad0` in `PackThinRecordsForFrame()` for cement quads
using the cache entry's `terrainHandle` (which resolves to a `mcTextureNodeIndex` = txm slot).
Emit a `MC2_TERRAIN_INDIRECT_TRACE` event with catalog count and atlas dimensions.

**Files:** `GameOS/gameos/gos_terrain_indirect.{h,cpp}`.

### Task A.1 — Add cement atlas static storage + helpers in `gos_terrain_indirect.cpp`

- [ ] **Step 1:** Add file-scope statics in the anonymous namespace (same region as `g_atlasGLTex`
  at `gos_terrain_indirect.cpp:372-377`):

```cpp
// Cement catalog atlas (single GL_TEXTURE_2D, packed grid of N cement tile textures).
// Built once per mission at BuildDenseRecipe() time while tileRAMHeap is alive.
// Grid: atlasGridSide × atlasGridSide cells, each TERRAIN_TXM_SIZE × TERRAIN_TXM_SIZE pixels.
static GLuint  g_cementAtlasGLTex          = 0;
static int     g_cementAtlasGridSide       = 0;   // cells per row/col (power of 2)
static float   g_cementAtlasOneOverDim     = 0.f; // 1.0 / (gridSide * TXM_SIZE)
static int     g_cementAtlasTileCount      = 0;   // distinct cement entries enumerated

// Dense lookup: textures[] slot → atlas column index (0-based) within the grid.
// Sized MC_MAX_TERRAIN_TXMS = 3000. 0xFFFF = "not cement / not in atlas".
static uint16_t g_cementLayerIndexBySlot[3000];
static bool     g_cementLayerMapReady      = false;
```

- [ ] **Step 2:** Initialize `g_cementLayerIndexBySlot` to `0xFFFF` at module start and at
  `ResetDenseRecipe()`. Add a line in `ResetDenseRecipe()` (already at
  `gos_terrain_indirect.cpp:479`) to clear:

```cpp
memset(g_cementLayerIndexBySlot, 0xFF, sizeof(g_cementLayerIndexBySlot));
g_cementLayerMapReady = false;
g_cementAtlasTileCount = 0;
if (g_cementAtlasGLTex != 0) {
    glDeleteTextures(1, &g_cementAtlasGLTex);
    g_cementAtlasGLTex = 0;
}
g_cementAtlasGridSide = 0;
g_cementAtlasOneOverDim = 0.f;
```

### Task A.2 — Implement `BuildCementCatalogAtlas()`

- [ ] **Step 1:** Add `BuildCementCatalogAtlas()` in the anonymous namespace after
  `BuildColormapAtlas()` (currently ending at `gos_terrain_indirect.cpp:421`):

```cpp
// BuildCementCatalogAtlas — must be called from BuildDenseRecipe() while
// tileRAMHeap is alive (freed by TerrainTextures::update() in Mission::update()).
// Walks terrainTextures->textures[0..nextAvailable-1] for cement-flagged entries.
// Reads pixel data from types[i].textureData[0] (tileRAMHeap-backed CPU RAM).
// Packs each cement tile into a power-of-2 grid atlas.
// Builds g_cementLayerIndexBySlot[] for PackThinRecordsForFrame() lookup.
void BuildCementCatalogAtlas() {
    ZoneScopedN("Terrain::IndirectCementAtlasUpload");

    // Guard: tileRAMHeap must be alive; types must be populated.
    if (!Terrain::terrainTextures) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_terrainTextures\n");
        return;
    }
    auto* tt = Terrain::terrainTextures;

    // TerrainTextures::nextAvailable and types are accessible because this runs
    // at primeMissionTerrainCache time, before Mission::update() frees tileRAMHeap.
    // nextAvailable is a static field — access via the live object.
    // types[] gives us baseTXMIndex for each terrain type (numTypes entries).

    const int txmSize = TERRAIN_TXM_SIZE;  // extern int, typically 64

    // Pass 1: enumerate all cement txm slots (isCement check on each slot).
    // NOTE: nextAvailable is a static member — read via the instance.
    // We cannot call TerrainTextures::nextAvailable directly from outside the
    // class — use the public getNumTypes() and types[i].baseTXMIndex, or
    // use the isCement() accessor per slot. isCement(slot) checks
    // textures[slot].flags & MC2_TERRAIN_CEMENT_FLAG which is O(1).
    // Upper bound is MC_MAX_TERRAIN_TXMS = 3000 (V_W2). In practice < 100.

    std::vector<int> cementSlots;        // txm slot indices with CEMENT_FLAG set
    std::vector<int> cementTypeIndices;  // corresponding types[] index for pixel data

    // Walk base types only for the pixel data source (tileRAMHeap-backed textureData[0]).
    // Transition textures (slots >= firstTransition) have their pixel data baked at
    // createTransition() time and also stored in textureData via tileRAMHeap; however
    // their types[] entry is not directly mapped. For pure-cement quads on stock missions,
    // only base-type slots appear (all 4 corners same cement type → baseTXMIndex returned
    // by setTexture). Include transition slots via a secondary pass that reads from GPU
    // readback only if needed. For PR2, enumerate base types first; extend to transitions
    // if the runtime count indicates they appear in practice.
    //
    // For each cement base type (types[i] where isCementType(types[i].terrainId)):
    const int numTypes = tt->getNumTypes();
    for (int i = 0; i < numTypes; ++i) {
        const long slot = tt->types[i].baseTXMIndex;
        if (slot < 0) continue;
        if (!tt->isCement((DWORD)slot)) continue;
        if (tt->types[i].textureData == nullptr) continue;
        if (tt->types[i].textureData[0] == nullptr) continue;  // tileRAMHeap pixel data
        cementSlots.push_back((int)slot);
        cementTypeIndices.push_back(i);
    }

    const int N = (int)cementSlots.size();
    if (N == 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0\n");
        return;
    }

    // Assign layer indices.
    memset(g_cementLayerIndexBySlot, 0xFF, sizeof(g_cementLayerIndexBySlot));
    for (int k = 0; k < N; ++k) {
        if (cementSlots[k] < 3000)
            g_cementLayerIndexBySlot[cementSlots[k]] = (uint16_t)k;
    }

    // Grid size: smallest power-of-2 side that fits N tiles in a square grid.
    int gridSide = 1;
    while (gridSide * gridSide < N) gridSide <<= 1;

    const int atlasPixelSide = gridSide * txmSize;

    // Build CPU atlas buffer: BGRA8 (matches MC2 texture memory encoding per
    // memory/mc2_argb_packing.md — textureData pixels are BGRA in memory).
    std::vector<uint32_t> atlasBuf((size_t)atlasPixelSide * atlasPixelSide, 0u);

    for (int k = 0; k < N; ++k) {
        const int typeIdx = cementTypeIndices[k];
        const MemoryPtr src = tt->types[typeIdx].textureData[0];
        if (!src) continue;

        const int col = k % gridSide;
        const int row = k / gridSide;
        const int dstX = col * txmSize;
        const int dstY = row * txmSize;

        const uint32_t* srcPx = reinterpret_cast<const uint32_t*>(src);
        for (int py = 0; py < txmSize; ++py) {
            for (int px = 0; px < txmSize; ++px) {
                atlasBuf[(dstY + py) * atlasPixelSide + (dstX + px)] =
                    srcPx[py * txmSize + px];
            }
        }
    }

    // Upload to GL.
    if (g_cementAtlasGLTex == 0) glGenTextures(1, &g_cementAtlasGLTex);
    glBindTexture(GL_TEXTURE_2D, g_cementAtlasGLTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 atlasPixelSide, atlasPixelSide, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   // cement tiles tile
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_cementAtlasGridSide   = gridSide;
    g_cementAtlasOneOverDim = 1.0f / (float)atlasPixelSide;
    g_cementAtlasTileCount  = N;
    g_cementLayerMapReady   = true;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_built tile_count=%d "
               "atlas_size=%dx%d grid_side=%d gltex=%u\n",
               N, atlasPixelSide, atlasPixelSide, gridSide,
               (unsigned)g_cementAtlasGLTex);
        fflush(stdout);
    }
}
```

- [ ] **Step 2:** Declare `BuildCementCatalogAtlas()` in the header (same section as `BuildColormapAtlas`
  pattern — no, BuildColormapAtlas is private/anonymous). Add a public Stage A bridge-accessor
  prototype in `gos_terrain_indirect.h` — or declare as extern in the bridge:

```cpp
// gos_terrain_indirect.cpp (anonymous namespace) — bridge accessors
GLuint gos_terrain_indirect_getCementAtlasGLTex()     { return g_cementAtlasGLTex; }
float  gos_terrain_indirect_getCementAtlasOneOverDim() { return g_cementAtlasOneOverDim; }
int    gos_terrain_indirect_getCementAtlasGridSide()   { return g_cementAtlasGridSide; }
bool   gos_terrain_indirect_getCementLayerMapReady()   { return g_cementLayerMapReady; }
```

### Task A.3 — Call `BuildCementCatalogAtlas()` from `BuildDenseRecipe()`

- [ ] **Step 1:** In `BuildDenseRecipe()` at `gos_terrain_indirect.cpp:476` (after
  `BuildColormapAtlas()` call), add:

```cpp
// Build cement catalog atlas (while tileRAMHeap is alive — see LIFECYCLE HAZARD in plan).
BuildCementCatalogAtlas();
```

The call order is: `BuildColormapAtlas()` then `BuildCementCatalogAtlas()`. Both depend on
`Terrain::terrainTextures` / `terrainTextures2` being alive, which is true at
`primeMissionTerrainCache` time.

### Task A.4 — Populate `tr._pad0` in `PackThinRecordsForFrame()`

The per-quad cache entry `entry->terrainHandle` gives us the raw `mcTextureNodeIndex` for the
cement catalog. At `PackThinRecordsForFrame()` time, `tileRAMHeap` is already freed (first frame
after mission start). The layer-index map `g_cementLayerIndexBySlot[]` was built at atlas-build
time and lives in process RAM — it is still valid.

- [ ] **Step 1:** In `PackThinRecordsForFrame()` at the thin-record construction block
  (currently at `gos_terrain_indirect.cpp:985-995`), after setting `tr.terrainHandle`:

```cpp
// Cement layer-index: encode in _pad0 low byte.
// If the quad is pure cement and the catalog slot maps to an atlas entry,
// set _pad0 = layer_index. Otherwise _pad0 = 0 (not cement or atlas miss).
// NOTE: th is the resolved terrainHandle (tex_resolve applied). We need the
// raw mcTextureNodeIndex (the slot in terrainTextures->textures[]) to look up
// the layer map. The packer already has q.terrainHandle (un-resolved) from the
// TerrainQuad. For cement quads, q.terrainHandle IS the slot (per brainstorm
// cross-reference: quad.cpp:438 calls getTextureHandle which returns
// mcTextureNodeIndex, not the gosHandle). But `th` is the result of
// tex_resolve(mcTextureNodeIndex) — that's the gosHandle, not the slot.
// We need the slot, not the gosHandle. Use q.terrainHandle directly (the
// DWORD from TerrainQuad) as the slot before tex_resolve.
uint32_t cementLayerIdx = 0u;
if (g_cementLayerMapReady) {
    // q.terrainHandle (un-resolved) is the mcTextureNodeIndex = textures[] slot.
    // isCement() checks the flags on that slot.
    const DWORD rawSlot = q.terrainHandle;
    if (rawSlot < 3000 && Terrain::terrainTextures &&
        Terrain::terrainTextures->isCement(rawSlot)) {
        const uint16_t idx = g_cementLayerIndexBySlot[rawSlot];
        if (idx != 0xFFFF) cementLayerIdx = (uint32_t)idx;
    }
}
tr._pad0 = cementLayerIdx;
```

**IMPORTANT:** The TerrainType bias encoding described in the architecture note (encoding
layer-index via `TerrainType = 3.0 + idx/255.0`) is the per-vertex mechanism. The thin VS
reads `_pad0` and needs to pass it to the frag. Since we cannot add a new varying (V_Y), the
bias approach uses the `TerrainType` output that the thin VS already emits. See Task A.5.

### Task A.5 — Thin VS: encode layer-index in `TerrainType` bias

In `gos_terrain_thin.vert` at line 151 (currently: `TerrainType = float(terrainTypes >> ...)`):

- [ ] **Step 1:** After computing `TerrainType` from the recipe `_wp0` corner material bits,
  apply the cement bias using `_pad0` from the thin record:

```glsl
// Base TerrainType from recipe corner materials (0=Rock, 1=Grass, 2=Dirt, 3=Concrete)
uint terrainTypes = floatBitsToUint(rec.worldPos0.w);
float baseTT = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);

// Cement layer-index bias: for pure-cement vertices (baseTT == 3.0),
// encode the atlas layer-index as a sub-integer bias in (3.0, 4.0).
// Frag decodes as: layerIdx = int(round((TerrainType - 3.0) * 255.0))
// Gate: only apply when useCementAtlas would matter (baseTT == 3.0).
uint cementLayerIdx = tr.control.w & 0xFFu;  // _pad0 low byte
if (baseTT == 3.0 && cementLayerIdx > 0u) {
    baseTT = 3.0 + float(cementLayerIdx) / 255.0;
}
TerrainType = baseTT;
```

This preserves full backward compatibility: the legacy non-thin VS chain never outputs
TerrainType > 3.0 for cement (it outputs the discrete material index directly), and the frag's
existing `pureConcrete = smoothstep(2.0, 3.0, TerrainType)` still saturates to 1.0 for
TerrainType ≥ 3.0 — no change to that gate. The new cement atlas branch gates on `floor(TerrainType) == 3.0`
which is both the base case (TerrainType exactly 3.0 for layer 0) and the biased case
(3.0 < TerrainType < 4.0 for layers 1-254).

### Task A.6 — Stage A smoke gate

- [ ] **Step 1:** Build and deploy, run trace:

```bash
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_TRACE=1 py -3 scripts/run_smoke.py \
  --tier tier1 --kill-existing --duration 20
```

Expected: `event=cement_catalog_built tile_count=N` appears in mc2_01 run (N > 0, expected
9-12 on stock). All other missions: `event=cement_atlas_skip reason=no_cement_tiles` (if no
cement) or `tile_count=...` if biome has cement.

- [ ] **Step 2:** tier1 5/5 PASS under `MC2_TERRAIN_INDIRECT=0` (baseline unchanged). Cement may
  still render wrong under `=1` until Stage B — this is expected and acceptable for Stage A.

---

## Stage B: Bridge + fragment shader wiring

**Scope:** Bridge binds cement atlas at unit 3. Frag adds `useCementAtlas` uniform + cement
branch that samples `tex3`. Bridge resets state after draw. M2 path untouched.

### Task B.1 — New uniforms in `gos_terrain.frag`

Current `tex3` declaration at `gos_terrain.frag:35` (V_X): "detail displacement, legacy, unused
with per-material POM". Repurpose as cement atlas sampler. Add two uniforms alongside the
existing atlas group.

- [ ] **Step 1:** Add after `uniform int useAtlasColormap;` (currently at `gos_terrain.frag:57`):

```glsl
// Cement catalog atlas (unit 3 = tex3, repurposed from legacy unused detail displacement).
// Set by the indirect bridge when a cement atlas has been built for this mission.
// When 0 (default — M2 path), cement quads fall through to colormap path.
uniform int   useCementAtlas;
uniform float atlasCementOneOverGridSide;  // 1.0 / gridSide (cells per row = g_cementAtlasGridSide)
uniform float atlasCementTxmSizeOverAtlasSize;  // txmSize / (gridSide * txmSize) = 1.0/gridSide
```

Note: `atlasCementOneOverGridSide` and `atlasCementTxmSizeOverAtlasSize` encode the same
value (1/gridSide). Use a single `atlasCementOneOverGridSide` uniform; tile UV within the
atlas is `(vec2(col, row) + fract(cementTileUV)) * atlasCementOneOverGridSide`.

### Task B.2 — Fragment shader cement branch

- [ ] **Step 1:** In `gos_terrain.frag`, after the existing `colormapUV` / `texColor` block
  (currently at lines 230-237), add the cement atlas override. This replaces `texColor` for
  pure-cement fragments when atlas mode is enabled.

The frag currently computes `texColor` from `tex1` (colormap atlas) whenever
`useAtlasColormap != 0`. For pure-cement quads we want `texColor` from `tex3` (cement atlas).
The cement atlas UV uses `WorldPos` for tile-relative position + layer-index for cell selection.

Replace the `texColor` assignment block with:

```glsl
PREC vec4 texColor;
if (useAtlasColormap != 0) {
    colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
    colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
    texColor = texture(tex1, colormapUV);

    // Cement atlas override: when useCementAtlas is set and this fragment is
    // pure cement (TerrainType encodes layer in [3.0, 4.0)), sample tex3.
    // TerrainType >= 3.0 is pure cement per vertex encoding.
    // Layer index: round((TerrainType - 3.0) * 255.0).
    if (useCementAtlas != 0 && TerrainType >= 3.0) {
        float cementTT = TerrainType - 3.0;  // [0.0, 1.0)
        float layerF   = cementTT * 255.0;
        int   layerIdx = int(layerF + 0.5);  // round to nearest

        // Atlas UV: tile position within the grid (col = layerIdx % gridSide,
        // row = layerIdx / gridSide) + per-pixel cement tile UV.
        // Cement tiles tile at detailNormalTiling.x scale (same as normal maps).
        // Use WorldPos for tile-relative UV so cement tiles repeat across tiles.
        float gridSideF = 1.0 / atlasCementOneOverGridSide;
        int   cCol = layerIdx % int(gridSideF + 0.5);
        int   cRow = layerIdx / int(gridSideF + 0.5);
        // Cement tile UV: WorldPos-based tiling at detailNormalTiling.x scale.
        PREC vec2 cTileUV = vec2(WorldPos.x, -WorldPos.y) * detailNormalTiling.x / float(TERRAIN_TXM_SIZE_F);
        cTileUV = fract(cTileUV);
        PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) * atlasCementOneOverGridSide;
        texColor = texture(tex3, cAtlasUV);
    }
} else {
    colormapUV = Texcoord;
    texColor = texture(tex1, colormapUV);
}
```

**Note on TERRAIN_TXM_SIZE_F:** Add `uniform float atlasCementTxmSize;` or hardcode via the
`atlasCementOneOverGridSide` computation. Simplest: the cement tile UV scale is driven by
`detailNormalTiling.x` (already a uniform, set by the bridge) — for cement tiles, a tiling
factor of 1.0 maps one tile exactly to each terrain quad. The actual tile size in world units
is `Terrain::worldUnitsPerVertex` (≈128 units per tile). Pass this as `atlasCementWorldScale`
uniform or derive from `Terrain::worldUnitsPerVertex`.

**Revised approach — simpler:** Use WorldPos-based UV where one cement tile repeats every
`worldUnitsPerVertex` units:

```glsl
// Cement tile UV: one repeat per terrain quad (128 world units typical)
PREC vec2 cTileUV = fract(vec2(WorldPos.x, -WorldPos.y) / atlasCementWorldUnitsPerTile);
PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) * atlasCementOneOverGridSide;
texColor = texture(tex3, cAtlasUV);
```

Add `uniform float atlasCementWorldUnitsPerTile;` (set to `Terrain::worldUnitsPerVertex` by
the bridge). This is the same value used by the VS for atlas UV reconstruction.

### Task B.3 — Bridge binds cement atlas at unit 3

In `gameos_graphics.cpp`, inside `gos_terrain_bridge_drawIndirect()` after the colormap atlas
bind block (currently ending at `gos_terrain_indirect.cpp:2323`):

- [ ] **Step 1:** Add extern declarations at the top of the bridge function:

```cpp
extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
extern float  gos_terrain_indirect_getCementAtlasOneOverDim();  // 1/(gridSide*txmSize)
extern int    gos_terrain_indirect_getCementAtlasGridSide();
extern bool   gos_terrain_indirect_getCementLayerMapReady();
```

- [ ] **Step 2:** Save and bind cement atlas at unit 3:

```cpp
// ---- Bind cement catalog atlas at unit 3 (tex3) ----------------------------
// tex3 is declared in gos_terrain.frag:35 as "legacy, unused with per-material POM".
// We repurpose it for the cement atlas. State save: current unit-3 binding.
GLint savedTex3Binding = 0;
const bool cementAtlasReady = gos_terrain_indirect_getCementLayerMapReady()
                              && (gos_terrain_indirect_getCementAtlasGLTex() != 0);
if (cementAtlasReady) {
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
    glActiveTexture(GL_TEXTURE0);
}

// Set cement atlas uniforms.
{
    const GLint locUCA   = glGetUniformLocation(prog, "useCementAtlas");
    const GLint locCOGS  = glGetUniformLocation(prog, "atlasCementOneOverGridSide");
    const GLint locCWUPT = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");
    const GLint locTex3  = glGetUniformLocation(prog, "tex3");

    if (locTex3  >= 0) glUniform1i(locTex3, 3);  // cement atlas at unit 3

    if (cementAtlasReady) {
        const int gridSide = gos_terrain_indirect_getCementAtlasGridSide();
        if (locUCA   >= 0) glUniform1i(locUCA,   1);
        if (locCOGS  >= 0) glUniform1f(locCOGS,  1.0f / (float)gridSide);
        if (locCWUPT >= 0) glUniform1f(locCWUPT, Terrain::worldUnitsPerVertex);
    } else {
        if (locUCA   >= 0) glUniform1i(locUCA,   0);
    }
}
```

- [ ] **Step 3:** After the draw call (after `glMultiDrawArraysIndirect`, at the reset block
  currently at `gos_terrain_indirect.cpp:2361-2367`), add cement atlas cleanup:

```cpp
// Reset useCementAtlas so M2 path doesn't inherit the cement atlas flag.
{
    const GLint locUCA = glGetUniformLocation(prog, "useCementAtlas");
    if (locUCA >= 0) glUniform1i(locUCA, 0);
}
// Restore unit-3 binding.
if (cementAtlasReady) {
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
    glActiveTexture(GL_TEXTURE0);
}
```

### Task B.4 — Stage B smoke gate

- [ ] **Step 1:** Build, deploy, launch mc2_01 with `MC2_TERRAIN_INDIRECT=1`.

Visual check: airport tarmac should now show grey/tan concrete tiles instead of grass/dirt.

- [ ] **Step 2:** Side-by-side comparison at fixed tarmac camera position (see Gate A below):
  `MC2_TERRAIN_INDIRECT=0` (legacy M2 path) vs `MC2_TERRAIN_INDIRECT=1` (indirect path).
  Textures must match visually.

- [ ] **Step 3:** tier1 5/5 PASS under `MC2_TERRAIN_INDIRECT=0` (baseline unchanged).

---

## Stage C: Default-on flip, quintuple validation, and slice closeout

**Scope:** Flip `IsEnabled()` to default-on (Q6 bundle). Validate all 5 tier1 smoke missions
quintuple (the N4 requirement from parent plan). Write closeout memory file.

### Task C.1 — Default-on flip in `IsEnabled()`

Per `gos_terrain_indirect.h:56-57`: "Stage 4 inverts `IsEnabled()` to default-on (only literal
`0` opts out)." Mirror the Shape C `aee39cc` flip pattern.

- [ ] **Step 1:** Edit `IsEnabled()` in `gos_terrain_indirect.cpp:40-46` (currently: `return v && v[0] == '1'...`):

```cpp
bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        // Stage 4: default ON. Literal "0" opts out; anything else = on.
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}
```

- [ ] **Step 2:** Update `[INSTR v1]` banner in `gameosmain.cpp` to show the inverted default.
  No code change needed — the banner already reads `IsEnabled()` at call time so the output
  will change automatically.

- [ ] **Step 3:** Add a comment in `gos_terrain_indirect.h:56-57` updating "Stage 4 flip
  shipped" with the commit hash placeholder for the executor to fill in after commit.

### Task C.2 — Gate A: Visual canary at mc2_01 airport tarmac

**Gate A specification:**

Camera position: advance into mc2_01 to the airport tarmac area (objective area near the
starting landing pad, approximately 200-400 units north of starting position). The concrete
runway is a large grey/tan tiled area clearly distinct from the surrounding green grass.

Validation: with `MC2_TERRAIN_INDIRECT=1` (now default), the airport tarmac renders with
visible concrete tile texture (grey/tan rectangular tile pattern), matching the legacy M2 path
(`MC2_TERRAIN_INDIRECT=0`) within ±5% perceptual difference. Green grass bleeding through the
tarmac = FAIL. Uniform grey without tile detail = FAIL (atlas UV issue). Correct tile pattern =
PASS.

- [ ] **Step 1:** Manual visual gate. Screenshot both paths at the same camera angle and compare.
- [ ] **Step 2:** Document PASS/FAIL and the camera position in the Stage C commit message.

### Task C.3 — Gate B: Tracy delta on `Terrain::SetupSolidBranch`

Per the parent plan's Gate B requirement: `Terrain::SetupSolidBranch` should show ≥80% reduction
vs Stage 1 baseline (since the SOLID branch is now bypassed when armed). The cement fix adds
~zero CPU overhead (one atlas GL bind + 2 uniform sets per frame).

- [ ] **Step 1:** Tracy capture at mc2_01 with default-on (`MC2_TERRAIN_INDIRECT` unset = on).
  Confirm `Terrain::ThinRecordPack` zone exists and is ~0.5-2ms (healthy). Confirm
  `Terrain::SetupSolidBranch` zone shows near-zero (≤0.05ms) — solid branch is gated off.
  Gate B: `Terrain::SetupSolidBranch` ≤ 20% of Stage 1 baseline.

### Task C.4 — Gate C: PARITY_CHECK=1 zero mismatches

- [ ] **Step 1:**

```bash
MC2_TERRAIN_INDIRECT_PARITY_CHECK=1 py -3 scripts/run_smoke.py \
  --tier tier1 --kill-existing --duration 30
```

Expected: zero `event=mismatch` lines in all 5 missions. The parity check compares recipe
geometry fields only (positions, normals, UVs, `_wp0`) — cement handle mismatch is not
currently gated. This is acceptable per Q7 discussion.

### Task C.5 — Gate D: tier1 5/5 quintuple (N4)

Per the parent plan's N4 requirement: quintuple smoke with INDIRECT default-on:

- [ ] **Step 1:** Run 5 sequential smoke passes:

```bash
for i in 1 2 3 4 5; do
  py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
done
```

All 5 must PASS. Any single FAIL = do not flip default-on. Investigate before proceeding.

### Task C.6 — Slice closeout: memory file + MEMORY.md entry

- [ ] **Step 1:** Write `memory/indirect_terrain_solid_endpoint.md` documenting:
  - PR1 (f221570 + a29ff83) + PR2 (this commit hash) architecture
  - Default-on date (2026-05-01)
  - Tracy delta summary from Gate B
  - Cement atlas slot count (from trace) for mc2_01
  - Queued follow-ups: Target 2 brainstorm (multi-layer overlays/decals), post-soak
    legacy SOLID deletion

- [ ] **Step 2:** Add index entry in `memory/MEMORY.md` under the "Rendering / shaders" section.

### Task C.7 — Stage C commit

Single commit bundles cement fix + default-on flip + memory file:

```bash
git add GameOS/gameos/gos_terrain_indirect.{h,cpp} \
        GameOS/gameos/gameos_graphics.cpp \
        shaders/gos_terrain.frag \
        shaders/gos_terrain_thin.vert \
        memory/indirect_terrain_solid_endpoint.md \
        memory/MEMORY.md
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): Stage 4 — cement catalog atlas + default-on flip (PR2)

Fixes pure-cement quads (airport tarmac) rendering grass/dirt on the
indirect SOLID path. Root cause: indirect bridge bound the colormap
atlas at unit 0 for all quads; cement quads sample the per-catalog
texture (TerrainTextures, no '2' suffix), not the colormap.

Fix: build a cement-catalog atlas (GL_TEXTURE_2D, packed grid) at
BuildDenseRecipe() time while tileRAMHeap is live. Bind at unit 3
(tex3 — declared but unused since per-material POM landed). Fragment
shader selects tex3 when useCementAtlas=1 + TerrainType>=3.0; falls
through to colormap when useCementAtlas=0 (M2 path safe). Layer-index
(which cement tile) encoded in TerrainQuadThinRecord._pad0 low byte;
propagated to frag via TerrainType bias (3.0+idx/255.0 — no new
varying, preserves legacy VS linker compatibility).

Default-on flip: IsEnabled() now returns true unless MC2_TERRAIN_INDIRECT=0.
Killswitch preserved. Quintuple N4 gate: tier1 5/5 × 5 runs PASS.
Gate A (visual): mc2_01 tarmac concrete tiles match legacy M2 path.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## 4-Gate Ladder (this slice)

| Gate | Condition | Measured by |
|---|---|---|
| **A — Visual** | mc2_01 airport tarmac renders concrete tiles (not grass). Side-by-side with legacy M2 path. | Manual screenshot comparison at fixed tarmac camera. |
| **B — Perf** | `Terrain::SetupSolidBranch` ≤ 20% of Stage 1 baseline. No Tracy regression on `Terrain::ThinRecordPack`. | Tracy capture, mc2_01, 60s. |
| **C — Parity** | `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` tier1 5/5: zero `event=mismatch` lines. | Smoke run, 30s per mission. |
| **D — Regression** | tier1 5/5 PASS × 5 sequential runs (N4 quintuple). Menu canary clean. | `run_smoke.py --tier tier1 --with-menu-canary` × 5. |

All 4 gates must pass before the Stage C commit lands.

---

## Verification Appendix

> Every cited symbol grep-verified at write-time per CLAUDE.md "Documentation Discipline."
> Status: ✅ = found at cited location. ⚠️ = found but with caveats. ❓ = requires executor
> verification at implementation time.

| ID | Symbol / Claim | File:line | Status | Note |
|---|---|---|---|---|
| V_X | `tex3` declared `sampler2D` as "legacy, unused with per-material POM" | `shaders/gos_terrain.frag:35` | ✅ | Exact text confirmed at read-time. Unit 3 = `tex3` = safe to repurpose. |
| V_Y | Varying linker-compatibility warning: new varying → silent linker fail → transparent terrain | `shaders/gos_terrain_thin.vert:22-31` (comment block) | ✅ | Comment explicitly warns against adding varyings. Confirmed at read-time. |
| V_Z | `TerrainQuadThinRecord._pad0` at `gos_terrain_patch_stream.h:107` | `GameOS/gameos/gos_terrain_patch_stream.h:107` | ✅ | `uint32_t _pad0;` confirmed. Currently written as `tr._pad0 = 0u;` in packer at `gos_terrain_indirect.cpp:991`. |
| V_Z2 | `sampler2DArray` Canary B status: NOT yet run | `docs/amd-driver-rules.md:9` | ✅ | Brainstorm confirmed Canary B not run; AMD Canary A passed. Blocked for production. |
| V_W | Stage 4 default-on flip site in `IsEnabled()` | `GameOS/gameos/gos_terrain_indirect.cpp:40-46` | ✅ | Currently returns `v && v[0] == '1'`. Stage 4 inverts per header comment at `gos_terrain_indirect.h:56-57`. |
| V_W2 | `MC_MAX_TERRAIN_TXMS = 3000` (cement slot upper bound) | `mclib/terrtxm.h:34` | ✅ | Confirmed `#define MC_MAX_TERRAIN_TXMS 3000`. Used to size `g_cementLayerIndexBySlot[3000]`. |
| V_V | `MC2_TERRAIN_CEMENT_FLAG = 0x00000001` | `mclib/terrtxm.h:53` | ✅ | Confirmed. Used by `isCement()` at `mclib/terrtxm.h:337-342`. |
| V_U | `pureConcrete = smoothstep(2.0, 3.0, TerrainType)` in frag | `shaders/gos_terrain.frag:330` | ✅ | Confirmed exact line. Also `concreteColorBlend = sqrt(clamp(pureConcrete,0,1))` at line 333. Both survive the cement atlas branch (they are used for normal blending, not colormap selection). |
| V_T | `TerrainQuadRecipe` is 9 vec4 = 144 bytes; `_wp0` is the w-component of `worldPos0` | `GameOS/gameos/gos_terrain_patch_stream.h:87-99` | ✅ | `static_assert(sizeof(TerrainQuadRecipe) == 144)` confirmed. `_wp0` is the 4th field: `float wx0, wy0, wz0, _wp0;`. Currently packed with 4-corner material types at `gos_terrain_indirect.cpp:344`. Confirmed: `TerrainQuadRecipe._wp0` is NOT `TerrainQuadThinRecord._pad0` — different structs. |
| V_S | M2 fast path: `terrainBindThinUniformsForPatchStream` does NOT set `tex3` explicitly | `GameOS/gameos/gameos_graphics.cpp:3608-3629` | ✅ | Unit 3 (`tex3`) not bound in that function. M2 leaves whatever was previously bound at unit 3. The bridge's save/restore pattern is required. |
| V_R | `buildTerrainRecipeInline` pure-cement branch at `quad.cpp:436-441` | `mclib/quad.cpp:436-441` | ✅ | Confirmed exact lines: `r.terrainHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff)`. Returns `mcTextureNodeIndex`. |
| V_Q | `getTextureHandle(DWORD)` returns `mcTextureNodeIndex` (NOT gosHandle) | `mclib/terrtxm.h:281-288` | ✅ | `tex_resolve(textures[texture].mcTextureNodeIndex)` is called (lazy memoize), then `mcTextureNodeIndex` is returned — NOT the gosHandle. Cement atlas layer lookup uses raw slot. |
| V_P | `PackThinRecordsForFrame()` line where `tr._pad0 = 0u;` is currently set | `GameOS/gameos/gos_terrain_indirect.cpp:991` | ✅ | Confirmed: `tr._pad0 = 0u;` at line 991. This is where Task A.4 inserts the cement layer-index. |
| V_O | `BuildColormapAtlas()` called inside `BuildDenseRecipe()` at line 476 | `GameOS/gameos/gos_terrain_indirect.cpp:476` | ✅ | Confirmed `BuildColormapAtlas();` call at line 476. `BuildCementCatalogAtlas()` will follow immediately after. |
| V_N | `TerrainTextures::tileRAMHeap` freed by `update()` called at `code/mission.cpp:497` | `code/mission.cpp:497` | ✅ | Confirmed `land->terrainTextures->update();` at line 497 inside `Mission::update()`. `update()` calls `delete tileRAMHeap` (confirmed at `mclib/terrtxm.cpp:1504`). |
| V_M | `primeMissionTerrainCache` called before `Mission::update()` — tileRAMHeap alive at BuildDenseRecipe time | `code/mission.cpp:2218` | ✅ | `primeMissionTerrainCache` called during `Mission::init`. `Mission::update()` is the per-frame call. tileRAMHeap is alive at init time. |
| V_L | `types[i].textureData[0]` is `MemoryPtr` backed by `tileRAMHeap` | `mclib/terrtxm.cpp:576-581` | ✅ | Confirmed: `tileRAMHeap->Malloc(mipSize*mipSize*sizeof(DWORD))` stores into `types[i].textureData[j]`. Base types (mip 0) at index 0. |
| V_K | `BASE_CEMENT_TYPE=10`, `START_CEMENT_TYPE=13`, `END_CEMENT_TYPE=20` | `mclib/terrtxm.h:42-44` | ✅ | Confirmed macros. 9 cement source IDs: 10, 13-20. |
| V_J | `TerrainTextures::nextAvailable` static; stores count of allocated slots | `mclib/terrtxm.cpp:59` | ✅ | `long TerrainTextures::nextAvailable = 0;` confirmed as static. Post-init = all allocated. |
| V_I | `isCement(DWORD typeInfo)` public method checks `textures[typeInfo].flags & MC2_TERRAIN_CEMENT_FLAG` | `mclib/terrtxm.h:337-342` | ✅ | Confirmed exact implementation. Bounds check on `nextAvailable`. |
| V_H | `TerrainType` bias encoding via `gos_terrain_thin.vert:149-151`: packed corner material types at 8 bits each | `shaders/gos_terrain_thin.vert:149-151` | ✅ | Confirmed: `uint terrainTypes = floatBitsToUint(rec.worldPos0.w); TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);` |
| V_G | `useAtlasColormap` reset after draw at `gameos_graphics.cpp:2365-2367` | `GameOS/gameos/gameos_graphics.cpp:2364-2367` | ✅ | Confirmed reset block. `useCementAtlas` reset is analogous and must follow immediately after. |
| V_F | `TERRAIN_TXM_SIZE` is `extern int` set to 64 in `mclib/terrtxm.cpp:51` | `mclib/terrtxm.cpp:51` | ✅ | Confirmed `int TERRAIN_TXM_SIZE = 64;`. Available via `extern int TERRAIN_TXM_SIZE;` from `terrtxm.h:46`. |
| V_E | `gos_terrain_indirect.cpp:344` packs 4 corner materials into `out._wp0` via `memcpy` | `GameOS/gameos/gos_terrain_indirect.cpp:340-345` | ✅ | Confirmed: `const uint32_t tpacked = m0 | (m1<<8) | (m2<<16) | (m3<<24); memcpy(&out._wp0, &tpacked, 4);`. The `TerrainQuadRecipe._wp0` field carries material data, not to be confused with `TerrainQuadThinRecord._pad0`. |
| V_D | `terrainTypeToMaterialLocal` at `gos_terrain_indirect.cpp:240-248`: cement types 10,13-19 → material 3 (Concrete) | `GameOS/gameos/gos_terrain_indirect.cpp:244-245` | ✅ | Confirmed: `case 10: case 13: case 14: case 15: case 16: case 17: case 18: case 19: return 3; // Concrete`. END_CEMENT_TYPE=20 maps to Rock(0) by default — check this matches quad.cpp. |
| V_C | `ResetDenseRecipe()` at `gos_terrain_indirect.cpp:479` — where to add cement atlas teardown | `GameOS/gameos/gos_terrain_indirect.cpp:479` | ✅ | Confirmed. Atlas teardown (`g_atlasGLTex` delete) already at lines 497-501. Cement atlas teardown mirrors this pattern. |
| V_B | `gos_terrain_bridge_drawIndirect` unit 0 bind + unit 3 not touched (currently) | `GameOS/gameos/gameos_graphics.cpp:2287-2388` | ✅ | Confirmed: only unit 0 (`GL_TEXTURE0`) and sampler 0 are touched. Unit 3 save/restore needed. |
| V_A | `getNumTypes()` public accessor for TerrainTextures | `mclib/terrtxm.h:234` | ✅ | Confirmed: `long getNumTypes() const { return numTypes; }`. Used in Task A.2 to iterate base types. |

### Open ❓ items (executor must resolve at implementation time)

| ID | Item | Action required |
|---|---|---|
| ❓1 | `types[i].baseTXMIndex` access from `gos_terrain_indirect.cpp`. `types` is a **protected** member of `TerrainTextures`. Direct access is not possible from outside the class. | **Resolution:** Either add a new `getBaseTXMIndexForType(int i)` accessor to `TerrainTextures`, or use the existing `isCement(slot)` + `getNumTypes()` combination differently. The plan's Task A.2 code calls `tt->types[i].baseTXMIndex` directly — this is a friend-access issue. The public API currently exposes only `getTextureHandle(DWORD texture)` (V_Q) taking a slot, not a type index. **Executor must** either add an accessor or change the enumeration approach to walk slots 0..nextAvailable-1 calling `isCement(slot)` and then reading pixel data via a new `getTypePixelData(slot, mipLevel)` public method. |
| ❓2 | `types[i].textureData[0]` is also protected. Same issue as ❓1. | Same resolution as ❓1. Add `getTypePixelDataForSlot(long txmSlot, int mipLevel)` returning `MemoryPtr` (NULL if not a base type). Alternatively, if `quickLoad=true` (stock missions), transitions are pre-baked to `.txm` files and `textureData` may be NULL — verify by checking `quickLoad` state at `BuildCementCatalogAtlas()` time and handling the NULL gracefully (skip that slot). |
| ❓3 | Cement tile UV tiling formula. The plan uses `WorldPos / worldUnitsPerVertex` for cement tile UV. Need to verify the correct scale so cement tiles look like the legacy M2 path (which binds a single cement texture and uses per-tile Texcoord [0,1]). | Executor should test with scale = 1 tile per quad (UV range [0,1] per `worldUnitsPerVertex` world units) and compare to legacy. The legacy frag for cement uses `Texcoord` directly (per-tile UV from recipe `uvData`, which is already [0,1] within a tile). For the atlas branch, matching this requires one repeat per `worldUnitsPerVertex` = `Terrain::worldUnitsPerVertex`. Add `Terrain::worldUnitsPerVertex` access in bridge (it's a public static float at `mclib/terrain.h`). |
| ❓4 | `terrainTypeToMaterialLocal` maps `case 20` (END_CEMENT_TYPE) to Rock(0) by default. `quad.cpp`'s `terrainTypeToMaterial` should agree. | Executor: grep `terrainTypeToMaterial` in `quad.cpp` and confirm case 20 maps the same way. If mismatch, `_wp0` parity check would already catch it. Low-risk but worth confirming. |
| ❓5 | `g_cementLayerIndexBySlot` array uses `q.terrainHandle` (un-resolved) as the slot index. Confirm that `TerrainQuad::terrainHandle` at packer time equals the `mcTextureNodeIndex` (i.e., the slot), NOT the resolved gosHandle. | Executor: grep `q.terrainHandle` vs `tex_resolve` in the packer. The existing packer code at `gos_terrain_indirect.cpp:944` already does `tex_resolve(q.terrainHandle)` to get `th` — so `q.terrainHandle` IS the `mcTextureNodeIndex` (pre-resolve value). Confirmed by brainstorm V_Q. |

---

## Cross-references

| Symbol | File:line | Plan section |
|---|---|---|
| `tex3` declaration | `shaders/gos_terrain.frag:35` | V_X, Stage B Task B.1 |
| `_pad0` in ThinRecord | `GameOS/gameos/gos_terrain_patch_stream.h:107` | V_Z, Stage A Task A.4 |
| `pureConcrete = smoothstep` | `shaders/gos_terrain.frag:330` | V_U, Stage B Task B.2 |
| `TerrainType` bias VS | `shaders/gos_terrain_thin.vert:149-151` | V_H, Stage A Task A.5 |
| `BuildColormapAtlas()` pattern | `GameOS/gameos/gos_terrain_indirect.cpp:379-421` | Stage A Task A.2 |
| `BuildDenseRecipe()` call site | `GameOS/gameos/gos_terrain_indirect.cpp:436-477` | Stage A Task A.3 |
| `PackThinRecordsForFrame()` pad0 | `GameOS/gameos/gos_terrain_indirect.cpp:985-995` | Stage A Task A.4 |
| `IsEnabled()` flip site | `GameOS/gameos/gos_terrain_indirect.cpp:40-46` | Stage C Task C.1 |
| `MC_MAX_TERRAIN_TXMS` | `mclib/terrtxm.h:34` | V_W2, Task A.1 |
| `MC2_TERRAIN_CEMENT_FLAG` | `mclib/terrtxm.h:53` | V_V |
| `isCement()` method | `mclib/terrtxm.h:337-342` | V_I, Task A.2 |
| `tileRAMHeap` lifecycle | `code/mission.cpp:497`, `mclib/terrtxm.cpp:1504` | V_N, LIFECYCLE HAZARD |
| `getNumTypes()` | `mclib/terrtxm.h:234` | V_A |
| `terrainTypeToMaterialLocal` | `GameOS/gameos/gos_terrain_indirect.cpp:240-248` | V_D |
| `gos_terrain_bridge_drawIndirect` | `GameOS/gameos/gameos_graphics.cpp:2270-2388` | Stage B Task B.3 |
| `useAtlasColormap` reset | `GameOS/gameos/gameos_graphics.cpp:2364-2367` | V_G, Stage B Task B.3 |

---

## Post-review advisor input (2026-05-01)

> **Status:** Plan v1 was stop-the-lined at adversarial review (`docs/superpowers/specs/2026-05-01-cement-multi-sampler-plan-review.md`, 2 CRITICAL + 5 MAJOR + 4 MINOR). The advisor input below confirms the review's verdict, sharpens the resolution paths, and adds requirements that plan v2 must satisfy before execution. **All items in this section are blockers for execution unless explicitly marked otherwise.**

### Advisor-required blockers

#### B1. C1 must be re-architected, not patched

Plan v1's core pixel source is dead: `textureData[0]` remains NULL in stock gameplay because the RAM allocation/write at `mclib/terrtxm.cpp:561-581` is inside `if (InEditor || !quickLoad)` and stock gameplay has `quickLoad=true`. The review showed `BuildCementCatalogAtlas()` would silently skip every cement entry and build an empty atlas (`event=cement_atlas_skip reason=no_cement_tiles count=0`).

**Advisor recommendation:** **Disk re-read** — unless there is already a trivial, reliable `mcTextureNodeIndex → GL texture → glGetTexImage` path nearby. Disk re-read avoids GL readback stall and keeps atlas construction deterministic. It does duplicate path/loading logic, but that's the lesser cost.

**GPU readback is acceptable only if v2 spells out exact texture-resolution and format handling** (which GL texture object the cement node resolves to, what internal format, what mip level, how to handle compressed formats if applicable, error handling on stalls).

**Plan v2 requirement:** pick ONE path explicitly. Remove all reliance on `textureData[0]`. Re-architect Stage A's atlas-build entirely around the chosen pixel source.

#### B2. C2 needs exact APIs, not "executor resolves"

Plan v1 directly accesses `tt->types[i].baseTXMIndex` and `tt->types[i].textureData[0]` from `gos_terrain_indirect.cpp`, but `types` and `tileRAMHeap` are `protected:`. Task A.2 won't compile.

**Plan v2 requirement:** define the narrow public surface explicitly. Suggested signatures:

```cpp
// Add to mclib/terrtxm.h public section:
long getBaseTXMIndexForType(int typeIndex) const;
bool isValidTextureSlot(DWORD slot) const;
long getNextAvailableTextureSlot() const;            // if dynamic slot walk is kept
const char* getTextureSourcePathForSlot(DWORD slot) const;  // if disk re-read chosen
```

**Avoid exposing raw `types` or `textureData`** if the new design (per B1) no longer depends on pixel RAM. Tailor the accessor surface to the chosen B1 resolution.

The advisor specifically rejects the "executor resolves" framing for compile-blocking issues in an architectural-endpoint plan.

#### B3. Sampler state on unit 3

Cement atlas relies on REPEAT wrapping (cement tiles tile across world space). Sampler objects override texture parameters. Plan v1 binds the indirect clamp sampler at unit 0 but doesn't address unit 3; a future or previous bind elsewhere could break cement tiling silently.

**Plan v2 requirement:** before binding cement atlas to `tex3`, EITHER:
- `glBindSampler(3, 0)` (clear sampler object, fall back to texture-object wrap state which `BuildCementCatalogAtlas` will set to `GL_REPEAT`), OR
- Bind a dedicated REPEAT sampler created at first-use (mirrors the indirect clamp sampler at unit 0).

Plan v2 must document which choice and why. Restore on draw exit.

#### B4. Resolve terrain type 20

Plan v1 (and brainstorm) cite cement source IDs as `10, 13-20`. But `terrainTypeToMaterialLocal` at `gos_terrain_indirect.cpp:240-248` maps only `10, 13-19` to Concrete (material 3). Type 20 falls through `default → Rock (0)`. Any type-20 cement quad would miss the cement frag branch entirely (`pureConcrete = smoothstep(2.0, 3.0, 0.0) = 0`).

**Plan v2 requirement:** EITHER add `case 20:` to Concrete, OR prove (via grep + reasoning) that type 20 never appears as PURE cement in stock missions. If added, mirror change in `quad.cpp`'s `terrainTypeToMaterial` to keep parity.

#### B5. Correct the SSBO/fragment-shader rationale

Plan v1 says the fragment shader cannot read SSBOs. **This is false** for GL 4.3 (which this codebase uses — `#version 430` per CLAUDE.md). The frag CAN read the thin-record SSBO via the same SSBO binding the VS uses; only the FRAG-SIDE access pattern needs to be wired (e.g., `gl_PrimitiveID` to compute `recordIdx` from `(gl_PrimitiveID / 2) + ssboRecordBase`).

**Plan v2 requirement:** rewrite the rationale. The choice of TerrainType-bias is a PREFERENCE (avoid adding a new frag SSBO contract), not a forced workaround. Defending the choice on its merits is fine; the false claim must be removed.

#### B6. Explicitly compare three options for the cement index transport

The handoff already flagged the TerrainType-bias approach (`3.0 + idx/255.0`) as fragile under TES interpolation. Plan v2 must explicitly compare:

| Option | Mechanism | Pros | Cons |
|---|---|---|---|
| **(a) TerrainType bias** | VS packs `idx/255.0` into fractional part of TerrainType varying | No new varying, no SSBO contract change, works with both indirect and M2 | TES interpolation may bleed across the integer boundary at triangle edges → wrong layer at boundary fragments. 1/255 precision floor. Decode (`floor(TerrainType)`) sensitive to FP precision near 3.0 |
| **(b) Color.a repurpose** | VS packs `idx` into `Color.a` (currently overwritten by frag for some paths) | 8-bit value preserved through interpolation per-corner (cement quads have all-corners-same → no interpolation issue); no new varying | Need to confirm `Color.a` isn't load-bearing on any consumer; legacy chain compatibility check needed |
| **(c) Frag SSBO read via gl_PrimitiveID** | Frag computes `recordIdx = ssboRecordBase + (gl_PrimitiveID/2)`, reads `_pad0` from thin record | Most robust (per-quad data, no interpolation, full 32-bit width); future-proof for multi-layer (decals/footprints can encode richer data); zero precision concerns | Adds frag SSBO binding contract; legacy non-thin VS chain emits triangles via TES — gl_PrimitiveID is per-tessellation-output-triangle, not per-quad; the math `recordIdx = primID/2` only holds for the indirect path which generates 2 triangles per quad in known order. Legacy chain would need a different formula or the frag's SSBO read path would be gated on `useAtlasColormap` |

**Plan v2 requirement:** evaluate each, document the trade-offs, pick one with explicit justification. The handoff's guidance ("don't break path forward for multiple layers") leans toward (c) but (b) is also future-extensible if `Color.a` is provably unused on the consumers.

#### B7. Mission-restart validation gate

Plan v1 deletes the cement atlas in `ResetDenseRecipe()` but does not include a "restart-without-quit" validation gate. The colormap atlas already has this lifecycle pattern, so risk is low — but because this feature is mission-load stateful (atlas built per-mission, deleted per-mission), it should be a gate.

**Plan v2 requirement:** add to Stage C validation:
- Cross-mission warm-boot run via `--tier tier1` (already in N4 quintuple).
- Per-mission verification: `event=cement_catalog_built` fires once per mission entry, `event=cement_catalog_reset` fires once per mission exit.
- RSS flat across mission boundaries (no GL texture leak).

### Smaller cleanups to fold into v2

These are MINOR/cleanup items; not individually blocking but worth resolving in the v2 pass:

- **Redundant cement atlas uniforms.** Plan v1 introduces multiple uniforms where one or two would suffice. Audit the uniform set and prune.
- **Pass `atlasCementGridSide` as `int`, not float.** Plan v1 reconstructs grid side from a reciprocal float; the grid side is integer at construction time. Pass the int directly.
- **Correct the reset-block file path.** Plan v1's M3 — bridge reset block lives in `gameos_graphics.cpp`, not `gos_terrain_indirect.cpp`. Fix the citation.
- **Document unit-3 wrap state contract.** After sampler reset (per B3), the cement texture's own `glTexParameteri(GL_TEXTURE_WRAP_S/T, GL_REPEAT)` is what governs tiling. Document this explicitly as a contract so future maintainers don't add a sampler binding that overrides it.

### Plan v2 shape (advisor recommendation)

Plan v2 should open with a short **"Delta from v1"** section before any stage detail, summarizing:

1. **Pixel source:** disk re-read OR GPU readback chosen; all `textureData[0]` references removed (B1).
2. **`TerrainTextures` accessors:** exact signatures listed (B2).
3. **Cement slot enumeration:** dynamic enumeration target preserved (per brainstorm Q3 — cement handles include base + transition slots in same `textures[]` space).
4. **Index transport:** TerrainType bias replaced or explicitly justified after the (a)/(b)/(c) comparison (B6).
5. **Bridge state:** bind tex3, set uniforms before `apply()`, reset `useCementAtlas`, restore unit 3, clear sampler state (B3).
6. **Validation:** mc2_01 fixed-camera cement canary, tier1 quintuple, killswitch path, parity, **restart-without-quit (B7)**, trace showing `cement_catalog_built tile_count > 0`.

After this delta section, the rest of plan v2 is a normal task-by-task plan with V1..VN verification appendix mirroring parent plan v2's V1-V17.

### Status assessment for execution gating

**The existing handoff prompt is good enough to start a fresh session.** The implementation plan is NOT — plan v2 rewrite is required before any code changes.

The fresh session should:
1. Read plan v1 + this advisor section + the adversarial review.
2. Make the architectural choices the advisor input demands (B1 disk vs GPU; B6 transport option).
3. Write plan v2 with the "delta from v1" section as preamble + V1..VN verification appendix.
4. Re-run adversarial review on plan v2.
5. Execute only after plan v2 is review-clean.
