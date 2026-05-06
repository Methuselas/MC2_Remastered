# M2: Thin-Record CPU Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the per-quad CPU overhead in `quadSetupTextures` introduced by M1d–M1g by dropping `gos_VERTEX[6]` construction from the fast path, compacting `TerrainQuadThinRecord` from 48 B to 32 B, and moving `TerrainType` from a per-frame field into the cached recipe SSBO.

**Architecture:** Three cooperating changes: (1) `TerrainQuadThinRecord` drops `fogRGBs`; `TerrainQuadRecipe._wp0` stores 4 corner material types as a packed uint (bit-preserving write, `floatBitsToUint` in GLSL). (2) New patchstream entry points — `ensureRecipeForQuad` and `appendThinRecordDirect` — separate recipe allocation from per-frame emit. (3) A new fast-path branch in `quad.cpp` calls these directly, computing only 4-corner pz validity and `lightRGB` without ever constructing a `gos_VERTEX` struct. The legacy path is unchanged in render behavior.

**Tech Stack:** OpenGL 4.3 / GLSL 430, C++17. Worktree at `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. Deploy to `A:/Games/mc2-opengl/mc2-win64-v0.2/`.

---

## Context for implementers

**Build command (always `RelWithDebInfo`):**
```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo --target mc2 -j8
```

**Deploy:** Use `/mc2-deploy` skill or read `.claude/skills/mc2-deploy.md`.

**Smoke test:**
```
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

**Current struct sizes (before this plan):**
- `TerrainQuadThinRecord`: 48 B (3 × uvec4: control + lightRGBs + fogRGBs)
- `TerrainQuadRecipe`: 144 B (`_wp0` is currently zero-padding)

**Key existing methods in `TerrainPatchStream` (gos_terrain_patch_stream.h):**
- `appendThinRecord(terrainHandle, recipe, flags, lRGB0..3, fRGB0..3)` — current signature, 11 params
- `addThinRecordVertParity(n)` — increments parity counter by n verts

**Key locations in quad.cpp:**
- Line 63: `#define SELECTION_COLOR 0xffff7fff`
- Lines ~1670-1726: lightRGB reads from `vertices[N]->lightRGB`, selection overrides via `vertices[N]->pVertex->selected`, `terrainTextures2` override, isFastPathActive gate
- Lines ~2015-2020: TOPRIGHT diagonal `appendThinRecord` call site (passes `gvTri1[0..2].argb` + `gVertex[2].argb` as lightRGBs, `gvTri1[0..2].frgb` + `gVertex[2].frgb` as fogRGBs)
- There is a symmetric BOTTOMLEFT diagonal section — update both call sites

---

## Files

- **Modify:** `GameOS/gameos/gos_terrain_patch_stream.h` — struct compaction, new method declarations
- **Modify:** `GameOS/gameos/gos_terrain_patch_stream.cpp` — `ensureRecipeForQuad`, `appendThinRecordDirect`, updated `appendThinRecord`
- **Modify:** `mclib/quad.cpp` — terrainType packing into recipe, remove fogRGB args, M2 fast-path branch
- **Modify:** `shaders/gos_terrain_thin.vert` — drop FogValue + fogRGBs, TerrainType from recipe
- **Modify:** `shaders/gos_terrain.frag` — remove dead FogValue input
- **Modify:** `shaders/gos_terrain.vert` — remove vs_FogValue output
- **Modify:** `shaders/gos_terrain.tesc` — remove FogValue passthrough in all branches
- **Modify:** `shaders/gos_terrain.tese` — remove FogValue passthrough and output

---

## Task 1: Compact ThinRecord + add patchstream API + update quad.cpp call sites

These three files must change together to compile. All changes land in one commit.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`
- Modify: `mclib/quad.cpp`

- [ ] **Step 1: Compact TerrainQuadThinRecord in gos_terrain_patch_stream.h**

Read lines 102–111 and 121–123. Make these changes:

Remove the `fogRGB0..3` fields from `TerrainQuadThinRecord` and update the static_assert:

```cpp
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;
    uint32_t terrainHandle;
    uint32_t flags;       // bit0: uvMode, bit1: pzTri1Valid, bit2: pzTri2Valid
    uint32_t _pad0;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
};
static_assert(sizeof(TerrainQuadThinRecord) == 32,
    "TerrainQuadThinRecord must be 32 bytes for std430 alignment");
```

Update `kPatchStreamThinRecordBytesPerSlot` (line 122–123) from `* 48u` to `* 32u`:

```cpp
constexpr uint32_t kPatchStreamThinRecordBytesPerSlot  =
    kPatchStreamMaxThinRecordsPerSlot * 32u;
```

- [ ] **Step 2: Update appendThinRecord declaration and add new method declarations**

Find `appendThinRecord` declaration in the class body and replace it:

Old:
```cpp
    static void appendThinRecord(DWORD terrainHandle,
                                 const TerrainQuadRecipe& recipe,
                                 uint32_t flags,
                                 uint32_t lightRGB0, uint32_t lightRGB1,
                                 uint32_t lightRGB2, uint32_t lightRGB3,
                                 uint32_t fogRGB0,   uint32_t fogRGB1,
                                 uint32_t fogRGB2,   uint32_t fogRGB3);
```

New:
```cpp
    static void appendThinRecord(DWORD terrainHandle,
                                 const TerrainQuadRecipe& recipe,
                                 uint32_t flags,
                                 uint32_t lightRGB0, uint32_t lightRGB1,
                                 uint32_t lightRGB2, uint32_t lightRGB3);

    // Pack (wx0, wy0) float bits into a uint64_t recipe key.
    // Exposed here so quad.cpp can call ensureRecipeForQuad without duplicating the helper.
    static inline uint64_t makeRecipeKey(float wx0, float wy0) {
        uint32_t bx, by;
        memcpy(&bx, &wx0, 4);
        memcpy(&by, &wy0, 4);
        return ((uint64_t)bx << 32) | (uint64_t)by;
    }

    // Ensures recipe exists in SSBO (allocates on first encounter, no-op thereafter).
    // recipe._wp0 must be pre-filled with packed corner material types before calling.
    // Returns recipe slot index, or UINT32_MAX on overflow — caller must skip emit.
    static uint32_t ensureRecipeForQuad(uint64_t floatKey, const TerrainQuadRecipe& recipe);

    // Write pre-built thin record directly to shadow array. tr.recipeIdx must be valid.
    // Mirrors all guards of appendThinRecord; increments parity counter.
    static void appendThinRecordDirect(DWORD terrainHandle, const TerrainQuadThinRecord& tr);
```

**Note:** `makeRecipeKey` moves to the header as an inline. Remove the `static inline uint64_t makeRecipeKey(...)` definition from the .cpp body (or it will conflict); the header inline replaces it.

- [ ] **Step 3: Implement ensureRecipeForQuad in gos_terrain_patch_stream.cpp**

Find the `appendThinRecord` implementation (around line 761). The recipe allocation block (lines ~785–829) will be extracted into `ensureRecipeForQuad`. Add this function before `appendThinRecord`.

**Note:** `makeRecipeKey` was a file-local `static inline` in this .cpp. It now lives in the header (Step 2). Delete the local definition here before adding `ensureRecipeForQuad` — the header inline replaces it.

`ensureRecipeForQuad` uses the same `s_recipeIndex` unordered_map for both allocation (miss) and lookup (hit). The flat-array cache from the spec is deferred; the hash is the O(1) lookup. Expected timing win is still large (gos_VERTEX construction eliminated); the hash adds ~50–100 ns/quad which is secondary.

```cpp
uint32_t TerrainPatchStream::ensureRecipeForQuad(uint64_t key,
                                                  const TerrainQuadRecipe& recipe) {
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return UINT32_MAX;
    if (!s_initOk || !s_killswitch) return UINT32_MAX;

    auto it = s_recipeIndex.find(key);
    if (it != s_recipeIndex.end()) {
        // Debug: if the cached recipe's positions differ, the key is non-unique.
        if (s_traceOn) {
            const TerrainQuadRecipe* base = (const TerrainQuadRecipe*)s_recipeMap;
            const TerrainQuadRecipe& cached = base[it->second];
            if (cached.wx0 != recipe.wx0 || cached.wy0 != recipe.wy0 ||
                cached.wz0 != recipe.wz0 || cached.wx1 != recipe.wx1) {
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=recipe_key_collision slot=%u "
                    "key=0x%llx cached=(%.3f,%.3f) incoming=(%.3f,%.3f)\n",
                    it->second, (unsigned long long)key,
                    cached.wx0, cached.wy0, recipe.wx0, recipe.wy0);
                fflush(stderr);
            }
        }
        return it->second;
    }

    if (s_recipeCount >= kPatchStreamMaxRecipesTotal) {
        static bool s_recipeOverflowLogged = false;
        if (!s_recipeOverflowLogged) {
            s_recipeOverflowLogged = true;
            fprintf(stderr,
                "[PATCH_STREAM v1] event=recipe_overflow cap=%u\n",
                kPatchStreamMaxRecipesTotal);
            fflush(stderr);
        }
        return UINT32_MAX;
    }

    uint32_t slot = s_recipeCount++;
    TerrainQuadRecipe* recipeBase = static_cast<TerrainQuadRecipe*>(s_recipeMap);
    recipeBase[slot] = recipe;  // _wp0 carries packed terrainTypes
    s_recipeIndex[key] = slot;
    return slot;
}
```

- [ ] **Step 4: Implement appendThinRecordDirect in gos_terrain_patch_stream.cpp**

Add immediately after `ensureRecipeForQuad`. Mirror all guards from the existing `appendThinRecord` (lines 768–779) exactly — same guard order, same overflow log format:

```cpp
void TerrainPatchStream::appendThinRecordDirect(DWORD terrainHandle,
                                                 const TerrainQuadThinRecord& tr) {
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return;
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;
    if (s_thinRecordCount >= kPatchStreamMaxThinRecordsPerSlot) {
        if (s_thinRecordCount == kPatchStreamMaxThinRecordsPerSlot) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_overflow slot=%u cap=%u\n",
                s_slot, kPatchStreamMaxThinRecordsPerSlot);
            fflush(stderr);
        }
        return;
    }
    s_thinRecordShadow[s_thinRecordCount++] = tr;
    uint32_t pzTri1 = (tr.flags >> 1u) & 1u;
    uint32_t pzTri2 = (tr.flags >> 2u) & 1u;
    addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
}
```

- [ ] **Step 5: Update appendThinRecord implementation in gos_terrain_patch_stream.cpp**

Find the full `appendThinRecord` function body. Replace it with a version that:
- Drops fogRGB parameters from signature
- Delegates recipe allocation to `ensureRecipeForQuad`
- Writes no fogRGB fields (they don't exist anymore)

Replace the full function body. The existing function (lines 761–841) checks `!s_initOk||!s_killswitch` and `s_overflow` before the thin-record overflow; preserve that guard order. The thin-record overflow only logs + returns — it does NOT set `s_overflow` (that flag gates the whole expanded-vertex path). Delegate recipe allocation to `ensureRecipeForQuad` and record write to `appendThinRecordDirect`:

```cpp
void TerrainPatchStream::appendThinRecord(DWORD terrainHandle,
                                           const TerrainQuadRecipe& recipe,
                                           uint32_t flags,
                                           uint32_t lightRGB0, uint32_t lightRGB1,
                                           uint32_t lightRGB2, uint32_t lightRGB3) {
    // ensureRecipeForQuad and appendThinRecordDirect each re-check guards;
    // calling them unconditionally here is safe and avoids duplication.
    const uint64_t key = makeRecipeKey(recipe.wx0, recipe.wy0);
    uint32_t recipeSlot = ensureRecipeForQuad(key, recipe);
    if (recipeSlot == UINT32_MAX) return;

    TerrainQuadThinRecord tr;
    tr.recipeIdx     = recipeSlot;
    tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
    tr.flags         = flags;
    tr._pad0         = 0u;
    tr.lightRGB0     = lightRGB0;
    tr.lightRGB1     = lightRGB1;
    tr.lightRGB2     = lightRGB2;
    tr.lightRGB3     = lightRGB3;
    appendThinRecordDirect(terrainHandle, tr);
}
```

- [ ] **Step 6: Update quad.cpp appendThinRecord call sites — add terrainType packing, remove fogRGB args**

In quad.cpp, find both the TOPRIGHT and BOTTOMLEFT `appendThinRecord` call sites (around lines 2015–2020 and the symmetric BOTTOMLEFT section). For each:

**Before the call**, add the terrainType packing:
```cpp
{
    uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
    uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
    uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
    uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
    uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
    memcpy(&recipe._wp0, &tpacked, 4);
}
```

**The call itself** — remove the 4 fogRGB arguments. Example for TOPRIGHT (adapt variable names to match what's actually in the file):
```cpp
TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
    gvTri1[0].argb, gvTri1[1].argb, gvTri1[2].argb, gVertex[2].argb);
```

The fogRGB args (`gvTri1[0].frgb, ...`) are simply removed. No replacement — that data is now either dead (FogValue) or in the recipe (TerrainType).

- [ ] **Step 7: Build**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -20
```

Expected: exit 0. Common errors:
- `too many arguments` on `appendThinRecord` call: you missed one of the two TOPRIGHT/BOTTOMLEFT call sites
- `no member fogRGB0` in appendThinRecord impl: the old impl body still references them — replace fully

- [ ] **Step 8: Gate 1 — thin records parity check (no fast path)**

Deploy the build to `A:/Games/mc2-opengl/mc2-win64-v0.2/`:
```
(follow /mc2-deploy or .claude/skills/mc2-deploy.md)
```

Run:
```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```
Load mc2_01. Check console for:
```
[PATCH_STREAM v1] event=thin_record_parity ... match=1
```
`match=1` = correct. `match=0` = thin record count disagrees — likely a parity counter bug in the new `appendThinRecord` or `appendThinRecordDirect`.

- [ ] **Step 9: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.h \
        GameOS/gameos/gos_terrain_patch_stream.cpp \
        mclib/quad.cpp
git commit -m "feat(m2): compact ThinRecord 48→32B, add ensureRecipeForQuad/appendThinRecordDirect, pack TerrainType into recipe"
```

---

## Task 2: Shader changes — remove FogValue, read TerrainType from recipe

Five shaders. All changes are dead-code removal + one new GLSL read in the thin VS.

**Files:**
- Modify: `shaders/gos_terrain_thin.vert`
- Modify: `shaders/gos_terrain.frag`
- Modify: `shaders/gos_terrain.vert`
- Modify: `shaders/gos_terrain.tesc`
- Modify: `shaders/gos_terrain.tese`

- [ ] **Step 1: Update gos_terrain_thin.vert**

Read the full file. Make these changes:

**In the `TerrainQuadThinRecord` struct at the top of the file**, remove the `uvec4 fogRGBs;` field:
```glsl
struct TerrainQuadThinRecord {
    uvec4 control;    // x=recipeIdx, y=terrainHandle, z=flags, w=_pad0
    uvec4 lightRGBs;  // corners 0-3, packed ARGB
    // fogRGBs removed — TerrainType now in recipe._wp0, FogValue was dead
};
```

**Remove** `out float FogValue;` from the output declarations.

**In the pz-culled early-return block**, remove:
```glsl
        FogValue       = 0.0;
```

**In the main computation body**, remove the entire fogRGB decode block:
```glsl
        uint frgb = uvec4Idx(tr.fogRGBs,   cornerIdx);
```
and remove:
```glsl
        FogValue    = float((frgb >> 24u) & 0xFFu) / 255.0;
        TerrainType = float(frgb & 0xFFu);
```

**Replace the TerrainType assignment** with:
```glsl
        uint terrainTypes = floatBitsToUint(rec.worldPos0.w);
        TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);
```

`rec.worldPos0.w` contains the packed 4-corner material values written by CPU into `recipe._wp0`.

The `lrgb` computation stays as-is (`uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);`).

- [ ] **Step 2: Update gos_terrain.frag**

Find and remove the line:
```glsl
in PREC float FogValue;
```
(or `in highp float FogValue;` — whichever form it takes). This is a dead input; the shader compiles and behaves identically without it.

No other changes.

- [ ] **Step 3: Update gos_terrain.vert**

Find and remove:
- Line 13: `out float vs_FogValue;`
- Line 26: `vs_FogValue = fog.w;`

No other changes.

- [ ] **Step 4: Update gos_terrain.tesc**

Find and remove all occurrences of `FogValue` / `tcs_FogValue`:
- Line 6: `in float vs_FogValue[];`
- Line 13: `out float tcs_FogValue[];`
- Line 63 (passthrough branch): `tcs_FogValue[gl_InvocationID] = vs_FogValue[gl_InvocationID];`
- The fat-record branch (useQuadRecords==1) also passes it through — remove that assignment too.

Search for `FogValue` in the file after editing and confirm zero occurrences remain.

- [ ] **Step 5: Update gos_terrain.tese**

Find and remove:
- Line 6: `in float tcs_FogValue[];`
- Line 13: `out float FogValue;`
- Lines 69–71: the barycentric interpolation block for FogValue:
  ```glsl
  FogValue = bary.x * tcs_FogValue[0]
           + bary.y * tcs_FogValue[1]
           + bary.z * tcs_FogValue[2];
  ```

Search for `FogValue` after editing; confirm zero occurrences.

- [ ] **Step 6: Build**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -20
```

Expected: exit 0. If GLSL linkage errors appear (linking between tesc/tese FogValue), re-check that both files have the declaration removed.

**Important:** Shader compile errors fail silently at runtime — the old cached shader stays active and the game may appear correct while running stale code. Always check console for `[THIN_TERRAIN] Thin terrain shader loaded:` after deploy to confirm the new shader compiled.

- [ ] **Step 7: Deploy and verify shaders loaded**

Deploy. Start the game and check console:
- `[THIN_TERRAIN] Thin terrain shader loaded: prog=N` — new thin shader compiled
- No `WARNING: failed to compile` lines
- With `MC2_PATCHSTREAM_THIN_RECORDS=1 MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1`, load mc2_01 and confirm terrain renders (no black/missing terrain)

- [ ] **Step 8: Visual check — cement tiles**

Load mc2_01 with thin records active. Pan to a concrete/cement area (industrial terrain, roads). The material blend on cement tiles must look identical to the pre-M2 baseline. If cement tiles look like dirt/grass, the TerrainType packing in `recipe._wp0` or the GLSL `floatBitsToUint` read is wrong.

- [ ] **Step 9: Commit**

```bash
git add shaders/gos_terrain_thin.vert shaders/gos_terrain.frag \
        shaders/gos_terrain.vert shaders/gos_terrain.tesc shaders/gos_terrain.tese
git commit -m "feat(m2): remove dead FogValue from all shaders, read TerrainType from recipe in thin VS"
```

---

## Task 3: quad.cpp — M2 direct thin-record emit branch

The primary performance win. The new branch fires when both `isFastPathActive()` and `isThinRecordsActive()` are true, computes only what the thin record needs (4-corner pz + lightRGB), and emits directly without constructing `gos_VERTEX`.

**Files:**
- Modify: `mclib/quad.cpp`

- [ ] **Step 1: Add the M2 fast-path branch to quad.cpp**

**Where to insert:** After `minU/minV/maxU/maxV` are computed (around line 1664) and BEFORE the `if (uvMode == BOTTOMRIGHT)` split (line 1669). The branch handles both diagonal modes, replacing the need to enter either conditional for the fast path.

**Exact field names** (confirmed from the actual code):
- pz check: `vertices[c]->pz + TERRAIN_DEPTH_FUDGE >= 0.0f && < 1.0f` — pre-projected, no `projectZ()` call
- world positions: `vertices[c]->vx`, `->vy`, `->pVertex->elevation`
- normals: `vertices[c]->pVertex->vertexNormal.x/y/z`
- isCement/isAlpha: from corner 0 textureData only: `Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff)`
- lightRGB priority: `terrainTextures2 && (!isCement||isAlpha)` → 0xffffffff, THEN `->pVertex->selected` → SELECTION_COLOR
- uvMode constants: `BOTTOMRIGHT` (→ thin flags bit0=0) and `BOTTOMLEFT` (→ thin flags bit0=1)

```cpp
// === M2: direct thin-record emit — no gos_VERTEX construction ===
if (TerrainPatchStream::isFastPathActive()
        && TerrainPatchStream::isThinRecordsActive()
        && TerrainPatchStream::isReady()
        && !TerrainPatchStream::isOverflowed()
        && terrainHandle != 0)
{
    // pz validity — vertices[c]->pz is already projected by the camera transform pass
    bool pzc[4];
    for (int c = 0; c < 4; c++) {
        float pz_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
        pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
    }

    bool pzTri1, pzTri2;
    if (uvMode == BOTTOMLEFT) {
        // tri1 = corners [0,1,3], tri2 = corners [1,2,3]
        pzTri1 = pzc[0] && pzc[1] && pzc[3];
        pzTri2 = pzc[1] && pzc[2] && pzc[3];
    } else {
        // BOTTOMRIGHT (= TOPRIGHT diagonal): tri1 = [0,1,2], tri2 = [0,2,3]
        pzTri1 = pzc[0] && pzc[1] && pzc[2];
        pzTri2 = pzc[0] && pzc[2] && pzc[3];
    }

    if (!pzTri1 && !pzTri2) return;  // both culled — skip entirely

    // isCement/isAlpha derive from corner 0 textureData (same as legacy)
    bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
    bool isAlpha  = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
    bool alphaOverride = Terrain::terrainTextures2 && (!isCement || isAlpha);

    // effectiveLightRGB: terrainTextures2 override first, then selected (matching legacy priority)
    auto lightRGBc = [&](int c) -> uint32_t {
        DWORD lc = vertices[c]->lightRGB;
        if (alphaOverride) lc = 0xffffffffu;
        if (vertices[c]->pVertex->selected) lc = static_cast<DWORD>(SELECTION_COLOR);
        return static_cast<uint32_t>(lc);
    };

    // Build recipe (same fields as the existing per-call-site constructions)
    TerrainQuadRecipe recipe;
    recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
    recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
    recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
    recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
    recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
    recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
    recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
    recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
    recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;

    // Pack 4 corner material types into _wp0 (bit-preserving write, floatBitsToUint in shader)
    {
        uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
        uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
        uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
        uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
        uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
        memcpy(&recipe._wp0, &tpacked, 4);
    }

    const uint64_t recipeKey = TerrainPatchStream::makeRecipeKey(recipe.wx0, recipe.wy0);
    uint32_t recipeIdx = TerrainPatchStream::ensureRecipeForQuad(recipeKey, recipe);
    if (recipeIdx == UINT32_MAX) return;  // SSBO full — skip gracefully

    TerrainQuadThinRecord tr;
    tr.recipeIdx     = recipeIdx;
    tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
    tr.flags         = (uvMode == BOTTOMLEFT ? 1u : 0u)
                     | (pzTri1 ? 2u : 0u)
                     | (pzTri2 ? 4u : 0u);
    tr._pad0         = 0u;
    tr.lightRGB0     = lightRGBc(0);
    tr.lightRGB1     = lightRGBc(1);
    tr.lightRGB2     = lightRGBc(2);
    tr.lightRGB3     = lightRGBc(3);
    TerrainPatchStream::appendThinRecordDirect(terrainHandle, tr);
    return;
}
// === end M2 branch — legacy path continues below ===
```

`makeRecipeKey` is now a public static inline in the header (Task 1 Step 2) — no duplication needed.

- [ ] **Step 2: Build**

```
cmake.exe --build build --config RelWithDebInfo --target mc2 -j8 2>&1 | tail -20
```

Expected: exit 0. Common issues:
- `TerrainPatchStream::makeRecipeKey` not found — check the header inline was added in Task 1 Step 2
- `TERRAIN_DEPTH_FUDGE` not visible at the insertion point — it's defined at line 1622; the branch is inserted after line 1664, so it IS in scope
- `BOTTOMRIGHT` / `BOTTOMLEFT` enum not visible — they're defined in the same TU; confirm by grepping

- [ ] **Step 3: Gate 2 — fast-path parity check**

Deploy. Run with all three env vars:
```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```
Load mc2_01. Check console for:
```
[PATCH_STREAM v1] event=thin_record_parity ... match=1
```
`match=1` = new branch produces correct vertex counts.

- [ ] **Step 4: Gate 3 — visual check at both zoom levels**

With the fast path active, visually verify:
- Terrain renders with correct PBR splatting (grass, rock, dirt, concrete all distinguishable)
- Cement/industrial zones look identical to baseline
- No UV tearing or misaligned terrain patches
- Shadows visible on terrain
- Pan camera across terrain — no popping or missing quads

Screenshot at standard zoom and at max Wolfman zoom and compare to pre-M2 reference if available.

- [ ] **Step 5: Gate 4 — recipe key-collision check**

Gate 4 checks for recipe key collisions — two different quads hashing to the same (wx0, wy0) key. The `recipe_key_collision` event is already implemented in `ensureRecipeForQuad` (Task 1 Step 3), but it's gated on `s_traceOn`. Enable it:

```
set MC2_PATCH_STREAM_TRACE=1& set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```

Scan console for:
```
[PATCH_STREAM v1] event=recipe_key_collision
```
Zero occurrences expected on standard missions. A collision indicates two terrain quads share the same corner-0 world position — benign but worth logging. (`s_traceOn` is set by `MC2_PATCH_STREAM_TRACE` — confirmed in init().)

- [ ] **Step 6: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat(m2): add direct thin-record emit branch in quad.cpp — skip gos_VERTEX[6] construction in fast path"
```

---

## Task 4: Deploy + validate all gates + Tracy timing

**Files:** None (deploy + measurement only)

- [ ] **Step 1: Full deploy**

Follow `/mc2-deploy` or `.claude/skills/mc2-deploy.md`. Verify:
```bash
ls "A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_terrain_thin.vert"
```
Expected: file exists and is newer than before.

- [ ] **Step 2: Gate 6 — tier1 smoke (standard path)**

```
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```
Expected: exit 0, all 5 missions pass. Run WITHOUT thin-record env vars — the legacy path must be unaffected.

- [ ] **Step 3: Gate 5 — Tracy timing comparison**

Connect Tracy profiler. Load mc2_01 in the game with all three env vars active and pan to max Wolfman zoom. Record `quadSetupTextures` zone duration. Compare to pre-M2 baseline of ~9 ms at max zoom.

Expected improvement: **9 ms → 5–7 ms**. The primary win is eliminating gos_VERTEX[6] construction per quad. The recipe hash (`s_recipeIndex.find()`) still runs per-frame but costs ~50–100 ns/quad — secondary. The flat-array cache from the spec is deferred; if a Phase A.5 pass implements it the estimate improves further.

If less than ~1 ms improvement, the M2 branch likely isn't being hit — check console for `isFastPathActive()`-related logs, and confirm `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1` is set. If improvement exceeds 3 ms, note the achieved reduction in the commit message.

If nearly all recipes are misses (hash allocation on every call), `beginFrame()` or `destroy()` may be clearing `s_recipeIndex` unexpectedly — grep for `.clear()` calls on it.

- [ ] **Step 4: Report results**

Report: what `quadSetupTextures` measured at max zoom before and after, and whether Gates 1–6 all passed. Flag any gate that did not pass `match=1` or showed unexpected console output.

---

## Self-Review Checklist

**Spec coverage:**
- [x] TerrainQuadThinRecord drops fogRGBs (48→32 B) — Task 1
- [x] TerrainQuadRecipe._wp0 stores packed terrainTypes — Task 1 Step 6
- [x] kPatchStreamThinRecordBytesPerSlot updated (×32u) — Task 1 Step 1
- [x] ensureRecipeForQuad: allocates on miss, returns slot, UINT32_MAX on overflow — Task 1 Step 3
- [x] appendThinRecordDirect: writes pre-built record, increments parity — Task 1 Step 4
- [x] appendThinRecord updated: no fogRGB params, delegates to ensureRecipeForQuad — Task 1 Step 5
- [x] quad.cpp existing call sites: terrainType packed, fogRGB args removed — Task 1 Step 6
- [x] gos_terrain_thin.vert: fogRGBs removed from struct + decoding, TerrainType from recipe — Task 2 Step 1
- [x] gos_terrain.frag: FogValue input removed — Task 2 Step 2
- [x] gos_terrain.vert: vs_FogValue removed — Task 2 Step 3
- [x] gos_terrain.tesc: FogValue passthrough removed all branches — Task 2 Step 4
- [x] gos_terrain.tese: FogValue in/out removed — Task 2 Step 5
- [x] quad.cpp M2 branch: 4-corner pz, effectiveLightRGB, ensureRecipeForQuad, appendThinRecordDirect — Task 3 Step 2
- [x] effectiveLightRGB uses vertices[c]->pVertex->selected (per-vertex) — Task 3 Step 2
- [x] pz-culled (both false): early return, no record emitted — Task 3 Step 2
- [x] SSBO overflow: graceful return, no crash — Task 3 Step 2
- [x] Gate 1 (thin parity, no fast path) — Task 1 Step 8
- [x] Gate 2 (fast path parity) — Task 3 Step 4
- [x] Gate 3 (visual cement canary) — Task 3 Step 5
- [x] Gate 4 (recipe collision log) — Task 3 Step 6
- [x] Gate 5 (Tracy timing) — Task 4 Step 3
- [x] Gate 6 (tier1 smoke, standard path) — Task 4 Step 2

**Type consistency:** `TerrainQuadThinRecord` defined in Task 1 (32 B, no fogRGB fields) matches the struct used in Task 3 (`tr.lightRGB0..3`, no `tr.fogRGB*`). `ensureRecipeForQuad` returns `uint32_t` and is consumed as `recipeIdx` in Task 3. `makeRecipeKey` is referenced in both Task 1 (patchstream.cpp) and Task 3 (quad.cpp) — ensure it is either exposed publicly or duplicated inline.
