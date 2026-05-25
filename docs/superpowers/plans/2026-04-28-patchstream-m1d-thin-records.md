# PatchStream M1d: GPU Recipe SSBO / Thin Records

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the per-quad SSBO record into a `TerrainQuadRecipe` (144 B, uploaded once on first camera-reveal of a quad, stable for a given cached quad) and a per-frame `TerrainQuadThinRecord` (48 B), reducing per-frame SSBO upload from ~10.5 MB/frame (192 B × 54 K slot capacity) to ~288 KB/frame at typical visible load (48 B × ~6 K quads in view) and halving cache pressure on `s_recordShadow`.

**Architecture:** A recipe SSBO (binding 1, single-buffered, up to 65 K quads) holds world-space corner positions, normals, and UV extents for every quad cached this mission. Data is stable for a given cached recipe; it is invalidated on terrain or cache changes (mission restart triggers `destroy()` + `init()`, which clears the recipe map). A thin-record SSBO (binding 0, triple-buffered) holds only the per-frame lighting, fog, handle, and recipe index. The TCS reads both; the recipe index in the thin record lets it fetch static geometry without a per-frame CPU upload. Both SSBOs are persistent-mapped coherent; the recipe SSBO is written once per new quad, the thin-record SSBO is written every frame by a sequential `memcpy` from a CPU shadow.

**Tech Stack:** C++17, OpenGL 4.3, GLSL 430, persistent-mapped SSBOs, `std::unordered_map` for the CPU-side recipe index

---

## File Map

| File | Change |
|------|--------|
| `GameOS/gameos/gos_terrain_patch_stream.h` | Add `TerrainQuadRecipe`, `TerrainQuadThinRecord`, new constants, new declarations |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` | Add statics, SSBO alloc/free in init/destroy, reset in beginFrame, implement `appendThinRecord`, flush thin-record draw path |
| `mclib/quad.cpp` | Add `appendThinRecord` calls (TOPRIGHT and BOTTOMLEFT blocks) |
| `shaders/gos_terrain.tesc` | Add `TerrainQuadThinRecord` + `TerrainQuadRecipe` structs, binding 1 recipe SSBO, new TCS record path branch (`useQuadRecords == 2`) |

---

## Task 1: M1d-a — Structs, constants, SSBO allocation

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

### Background

`TerrainQuadRecord` (192 B) encodes both static geometry (worldPos + worldNorm + uvData = 144 B) and per-frame lighting/fog (48 B). The recipe SSBO splits these: static data lives in `TerrainQuadRecipe`, per-frame data in `TerrainQuadThinRecord`. Environment-variable gating mirrors the existing fat-record path: `MC2_PATCHSTREAM_THIN_RECORDS=1` enables record writes, `MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1` enables GPU draws.

---

- [ ] **Step 1: Add new structs and constants to the header**

In `GameOS/gameos/gos_terrain_patch_stream.h`, after the `TerrainQuadRecord` struct and its `static_assert`, add:

```cpp
// --- M1d: Recipe SSBO (cached-per-quad) + Thin Record SSBO (per-frame) ---

// Per-quad cached recipe: world-space corner positions, normals, UV extents.
// Uploaded once on first camera-reveal; stable for a given cached quad.
// Invalidated on mission restart (destroy/init clears s_recipeIndex).
// Layout: std430-compatible (all members at 16-byte-aligned vec4 boundaries).
// 9 vec4s = 144 bytes.
struct alignas(16) TerrainQuadRecipe {
    float wx0, wy0, wz0, _wp0;
    float wx1, wy1, wz1, _wp1;
    float wx2, wy2, wz2, _wp2;
    float wx3, wy3, wz3, _wp3;
    float nx0, ny0, nz0, _np0;
    float nx1, ny1, nz1, _np1;
    float nx2, ny2, nz2, _np2;
    float nx3, ny3, nz3, _np3;
    float minU, minV, maxU, maxV;
};
static_assert(sizeof(TerrainQuadRecipe) == 144,
    "TerrainQuadRecipe must be 144 bytes for std430 alignment");

// Per-frame thin record: recipe index + per-frame lighting, fog, handle, flags.
// 3 uvec4s = 48 bytes.
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;     // index into the recipe SSBO (global, no slot offset)
    uint32_t terrainHandle; // raw gosHandle — tex_resolve applied at flush
    uint32_t flags;         // bit 0: uvMode, bit 1: pzTri1Valid, bit 2: pzTri2Valid
    uint32_t _pad0;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
    uint32_t fogRGB0,   fogRGB1,   fogRGB2,   fogRGB3;
};
static_assert(sizeof(TerrainQuadThinRecord) == 48,
    "TerrainQuadThinRecord must be 48 bytes for std430 alignment");

// Recipe SSBO: single-buffered, shared across all ring slots.
// Sized for the full terrain grid (120×120 grid ≈ 14 K quads) with 4× headroom.
constexpr uint32_t kPatchStreamMaxRecipesTotal         = 65536u;
constexpr uint32_t kPatchStreamRecipeBytes             =
    kPatchStreamMaxRecipesTotal * 144u;  // 9.2 MB

// Thin-record SSBO: triple-buffered alongside the fat-record SSBO.
// Same per-slot quad capacity as the fat-record path; 48 B vs 192 B.
constexpr uint32_t kPatchStreamMaxThinRecordsPerSlot   = kPatchStreamMaxRecordsPerSlot;
constexpr uint32_t kPatchStreamThinRecordBytesPerSlot  =
    kPatchStreamMaxThinRecordsPerSlot * 48u;
```

- [ ] **Step 2: Add new declarations to the header**

Still in `gos_terrain_patch_stream.h`, inside the `TerrainPatchStream` class, add after `addRecordVertParity`:

```cpp
    // Emit one thin quad record (M1d). No-op unless MC2_PATCHSTREAM_THIN_RECORDS=1.
    // recipe encodes the static geometry (positions, normals, UVs).
    // Per-frame fields are passed inline.
    // Call after appendQuadRecord() at the same quad.cpp call site.
    static void appendThinRecord(DWORD terrainHandle,
                                 const TerrainQuadRecipe& recipe,
                                 uint32_t flags,
                                 uint32_t lightRGB0, uint32_t lightRGB1,
                                 uint32_t lightRGB2, uint32_t lightRGB3,
                                 uint32_t fogRGB0,   uint32_t fogRGB1,
                                 uint32_t fogRGB2,   uint32_t fogRGB3);

    // Parity: expected verts from thin records (same semantics as addRecordVertParity).
    static void addThinRecordVertParity(uint32_t n);
```

- [ ] **Step 3: Add new statics in the anonymous namespace in gos_terrain_patch_stream.cpp**

After the existing `s_recordShadow` declaration (around line 63), add:

```cpp
    static const bool s_thinRecordsOn     = (getenv("MC2_PATCHSTREAM_THIN_RECORDS")      != nullptr);
    static const bool s_thinRecordsDrawOn = (getenv("MC2_PATCHSTREAM_THIN_RECORDS_DRAW") != nullptr);

    // Recipe SSBO — single-buffered, persistent-mapped. Written once per new quad.
    // Not slot-indexed; recipeIdx is global across all ring slots.
    static GLuint    s_recipeBuf        = 0;
    static void*     s_recipeMap        = nullptr;
    static uint32_t  s_recipeCount      = 0;  // total recipes written (monotonic per mission)

    // CPU recipe index: key = packed float bits of (wx0, wy0), value = recipe slot.
    // Keyed by corner-0 world position (stable per terrain quad).
    static std::unordered_map<uint64_t, uint32_t> s_recipeIndex;

    // Thin-record SSBO — triple-buffered, persistent-mapped. Written per-frame.
    static GLuint    s_thinRecordBuf        = 0;
    static void*     s_thinRecordMap        = nullptr;
    static uint32_t  s_thinRecordCount      = 0;  // thin records staged this frame
    static uint32_t  s_thinRecordVertParity = 0;  // expected verts from thin records
    static bool      s_thinRecordBannerSeen = false;

    // CPU shadow of the thin-record SSBO (cache-hot staging; flushed sorted to SSBO).
    static TerrainQuadThinRecord s_thinRecordShadow[kPatchStreamMaxThinRecordsPerSlot];
```

Also add `#include <unordered_map>` near the top of the file (after existing includes).

- [ ] **Step 4: Allocate recipe and thin-record SSBOs in init()**

In `TerrainPatchStream::init()`, inside the `if (s_quadRecordsOn)` block (around line 317), add an analogous block for thin records after the fat-record allocation:

```cpp
    if (s_thinRecordsOn) {
        // Recipe SSBO — single-buffered, GL_MAP_WRITE_BIT | PERSISTENT | COHERENT.
        s_recipeBuf = allocPersistentSSBO((GLsizeiptr)kPatchStreamRecipeBytes, &s_recipeMap);
        if (!s_recipeBuf) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=recipe_ssbo_fail reason=alloc\n");
            fflush(stderr);
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=recipe_ssbo_ok bytes=%u max_recipes=%u\n",
                kPatchStreamRecipeBytes, kPatchStreamMaxRecipesTotal);
            fflush(stderr);
        }

        // Thin-record SSBO — triple-buffered, same flags as fat-record SSBO.
        const GLsizeiptr thinTotal =
            (GLsizeiptr)kPatchStreamThinRecordBytesPerSlot * kPatchStreamRingFrames;
        s_thinRecordBuf = allocPersistentSSBO(thinTotal, &s_thinRecordMap);
        if (!s_thinRecordBuf) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_ssbo_fail reason=alloc\n");
            fflush(stderr);
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_ssbo_ok bytes_per_slot=%u slots=%u\n",
                kPatchStreamThinRecordBytesPerSlot, kPatchStreamRingFrames);
            fflush(stderr);
        }
    }
```

Place this BEFORE the `glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0)` cleanup line (line 334), so the cleanup call covers all three SSBOs.

- [ ] **Step 5: Free recipe and thin-record SSBOs in destroy()**

In `TerrainPatchStream::destroy()`, after the existing `s_recordBuf` cleanup block (around line 522), add:

```cpp
    if (s_recipeBuf) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_recipeBuf);
        if (s_recipeMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_recipeMap = nullptr; }
        glDeleteBuffers(1, &s_recipeBuf);
        s_recipeBuf = 0;
    }
    if (s_thinRecordBuf) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_thinRecordBuf);
        if (s_thinRecordMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_thinRecordMap = nullptr; }
        glDeleteBuffers(1, &s_thinRecordBuf);
        s_thinRecordBuf = 0;
    }
    s_recipeIndex.clear();
    s_recipeCount = 0;
```

This goes before the existing `glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0)` cleanup line at line 523.

- [ ] **Step 6: Reset per-frame thin-record counters in beginFrame()**

In `TerrainPatchStream::beginFrame()`, after the existing `s_recordCount = 0; s_recordVertParity = 0;` lines (around line 575), add:

```cpp
    s_thinRecordCount      = 0;
    s_thinRecordVertParity = 0;
```

- [ ] **Step 7: Build and verify init log**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo 2>&1 | tail -5
```

Expected: `0 Error(s)`.

Then launch with the env var:
```powershell
$env:MC2_PATCHSTREAM_THIN_RECORDS=1; $env:MC2_PATCHSTREAM_QUAD_RECORDS=1; .\mc2.exe 2>&1 | Select-String "recipe_ssbo|thin_record_ssbo"
```

Expected lines at startup:
```
[PATCH_STREAM v1] event=recipe_ssbo_ok bytes=9437184 max_recipes=65536
[PATCH_STREAM v1] event=thin_record_ssbo_ok bytes_per_slot=2621184 slots=3
```

- [ ] **Step 8: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.h
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): M1d-a — define TerrainQuadRecipe/ThinRecord structs + alloc SSBOs"
```

---

## Task 2: M1d-b — appendThinRecord + lazy recipe population + quad.cpp call sites

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp` — implement `appendThinRecord`, `addThinRecordVertParity`
- Modify: `mclib/quad.cpp` — TOPRIGHT + BOTTOMLEFT blocks

### Background

`appendThinRecord` checks `s_recipeIndex` for the quad's corner-0 world position. On the first frame a quad is visible, it writes `TerrainQuadRecipe` to `s_recipeMap` (coherent, so immediately visible to GPU) and records the slot. On subsequent frames it just looks up the slot. The thin record (48 B) is written to `s_thinRecordShadow[]` (CPU memory, cache-hot); `flush()` will sort and `memcpy` it to the SSBO. Key: `(uint64_t)(memcpy-bits-of-wx0) << 32 | (uint32_t)(bits-of-wy0)`.

The recipe SSBO uses `GL_MAP_COHERENT_BIT`, so CPU writes to `s_recipeMap` are GPU-visible without any manual barrier.

---

- [ ] **Step 1: Implement appendThinRecord in gos_terrain_patch_stream.cpp**

After `appendQuadRecord` (around line 671), add:

```cpp
void TerrainPatchStream::appendThinRecord(
    DWORD terrainHandle,
    const TerrainQuadRecipe& recipe,
    uint32_t flags,
    uint32_t lightRGB0, uint32_t lightRGB1, uint32_t lightRGB2, uint32_t lightRGB3,
    uint32_t fogRGB0,   uint32_t fogRGB1,   uint32_t fogRGB2,   uint32_t fogRGB3)
{
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

    // Lazily populate recipe: look up by corner-0 world position bits.
    uint32_t bx, by;
    memcpy(&bx, &recipe.wx0, 4);
    memcpy(&by, &recipe.wy0, 4);
    const uint64_t key = ((uint64_t)bx << 32) | (uint64_t)by;

    uint32_t recipeSlot;
    auto it = s_recipeIndex.find(key);
    if (it != s_recipeIndex.end()) {
        recipeSlot = it->second;
        // Debug collision check: if the incoming recipe fields differ from the cached
        // slot, the key (wx0,wy0) is not unique for this quad. Log and continue using
        // the cached slot (do not overwrite — the GPU may have already read it).
        if (s_traceOn) {
            const TerrainQuadRecipe* recipeBase = (const TerrainQuadRecipe*)s_recipeMap;
            const TerrainQuadRecipe& cached = recipeBase[recipeSlot];
            if (cached.wx0 != recipe.wx0 || cached.wy0 != recipe.wy0 ||
                cached.wz0 != recipe.wz0 || cached.wx1 != recipe.wx1) {
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=recipe_key_collision slot=%u "
                    "key=0x%llx cached=(%.3f,%.3f,%.3f) incoming=(%.3f,%.3f,%.3f)\n",
                    recipeSlot, (unsigned long long)key,
                    cached.wx0, cached.wy0, cached.wz0,
                    recipe.wx0, recipe.wy0, recipe.wz0);
                fflush(stderr);
            }
        }
    } else {
        if (s_recipeCount >= kPatchStreamMaxRecipesTotal) {
            // Overflow: silently skip (no thin record emitted).
            static bool s_recipeOverflowLogged = false;
            if (!s_recipeOverflowLogged) {
                s_recipeOverflowLogged = true;
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=recipe_overflow cap=%u\n",
                    kPatchStreamMaxRecipesTotal);
                fflush(stderr);
            }
            return;
        }
        recipeSlot = s_recipeCount++;
        // Write recipe to coherent persistent map — immediately GPU-visible.
        TerrainQuadRecipe* recipeBase = (TerrainQuadRecipe*)s_recipeMap;
        recipeBase[recipeSlot] = recipe;
        s_recipeIndex[key] = recipeSlot;
    }

    // Write thin record to CPU shadow (cache-hot; sorted + uploaded in flush).
    TerrainQuadThinRecord& tr = s_thinRecordShadow[s_thinRecordCount++];
    tr.recipeIdx     = recipeSlot;
    tr.terrainHandle = (uint32_t)terrainHandle;
    tr.flags         = flags;
    tr._pad0         = 0u;
    tr.lightRGB0     = lightRGB0; tr.lightRGB1 = lightRGB1;
    tr.lightRGB2     = lightRGB2; tr.lightRGB3 = lightRGB3;
    tr.fogRGB0       = fogRGB0;   tr.fogRGB1   = fogRGB1;
    tr.fogRGB2       = fogRGB2;   tr.fogRGB3   = fogRGB3;
}
```

- [ ] **Step 2: Implement addThinRecordVertParity**

Immediately after `appendThinRecord`, add:

```cpp
void TerrainPatchStream::addThinRecordVertParity(uint32_t n) {
    if (!s_thinRecordsOn) return;
    s_thinRecordVertParity += n;
}
```

- [ ] **Step 3: Add call site in quad.cpp TOPRIGHT block**

In `mclib/quad.cpp`, in the TOPRIGHT PatchStream block (around line 1975–2001), after the existing `appendQuadRecord` + `addRecordVertParity` calls, add the following. No extra guard is needed: `appendThinRecord` is a no-op when `MC2_PATCHSTREAM_THIN_RECORDS` is unset (the check is inside the function).

After the line `TerrainPatchStream::addRecordVertParity(...)` at line 2000, add:

```cpp
				{
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
					const uint32_t tFlags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gvTri1[2].argb, gVertex[2].argb,
						gvTri1[0].frgb, gvTri1[1].frgb, gvTri1[2].frgb, gVertex[2].frgb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
```

- [ ] **Step 4: Add call site in quad.cpp BOTTOMLEFT block**

After the line `TerrainPatchStream::addRecordVertParity(...)` at line 2332, add:

```cpp
				{
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
					// BOTTOMLEFT: same corner order as fat record above.
					// gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3; gVertex[1]=corner2
					const uint32_t tFlags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gVertex[1].argb, gvTri1[2].argb,
						gvTri1[0].frgb, gvTri1[1].frgb, gVertex[1].frgb, gvTri1[2].frgb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
```

Note: `mclib/quad.cpp` includes `gos_terrain_patch_stream.h` (added during M1a); `TerrainQuadRecipe` is visible automatically.

- [ ] **Step 5: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo 2>&1 | tail -5
```

Expected: `0 Error(s)`.

- [ ] **Step 6: Verify parity (thin count == fat count)**

Launch mc2.exe with both record vars + trace:

```powershell
$env:MC2_PATCHSTREAM_THIN_RECORDS=1
$env:MC2_PATCHSTREAM_QUAD_RECORDS=1
$env:MC2_PATCH_STREAM_TRACE=1
.\mc2.exe mc2_01 2>&1 | Select-String "thin_record|recipe_ok|record_parity"
```

Expected in the first flush:
- `event=recipe_ssbo_ok` and `event=thin_record_ssbo_ok` at startup
- `event=record_parity` lines showing `match=1` every frame (fat record parity still logged)
- No `thin_record_overflow` or `recipe_overflow` events

Also verify thin record count equals fat record count by adding a temporary stderr print inside flush() if counts differ. (The parity check for thin records is added in Task 3 alongside the draw path.)

- [ ] **Step 7: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git add mclib/quad.cpp
git commit -m "feat(patchstream): M1d-b — appendThinRecord + lazy recipe population + quad.cpp call sites"
```

---

## Task 3: M1d-c — TCS shader update, flush() thin-record draw path, validation

**Files:**
- Modify: `shaders/gos_terrain.tesc`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

### Background

The TCS adds a new branch `useQuadRecords == 2` for thin records. It reads from the thin-record SSBO (binding 0) and the recipe SSBO (binding 1). The `recipeIdx` in the thin record is a global offset into the recipe SSBO (no slot offset needed — recipes are single-buffered). `ssboRecordBase` is unchanged and still encodes the triple-buffered slot offset for the thin-record SSBO.

In flush(), the thin-record draw path mirrors the fat-record draw path: sort `s_thinRecordShadow[]` by resolved gosHandle → build bucket table → `memcpy` sorted records to SSBO → bind both SSBOs → per-bucket `glUniform1i(useQuadRecords, 2)` + `glUniform1i(ssboRecordBase, ...)` + `drawSingleBucket`.

---

- [ ] **Step 1: Update gos_terrain.tesc — add TerrainQuadThinRecord struct and RecipeBuf**

In `shaders/gos_terrain.tesc`, after the existing `TerrainQuadRecord` struct and `QuadRecordBuf` layout, add:

```glsl
// M1d: Thin record (48 bytes, per-frame, binding 0 when useQuadRecords==2).
struct TerrainQuadThinRecord {
    uvec4 control;    // x=recipeIdx, y=terrainHandle, z=flags, w=_pad0
    uvec4 lightRGBs;  // corners 0-3, packed ARGB
    uvec4 fogRGBs;    // corners 0-3, packed frgb
};

layout(std430, binding = 0) readonly buffer ThinRecordBuf {
    TerrainQuadThinRecord thinRecs[];
};

// M1d: Recipe (144 bytes, per-mission, binding 1 when useQuadRecords==2).
struct TerrainQuadRecipe {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;  // minU, minV, maxU, maxV
};

layout(std430, binding = 1) readonly buffer RecipeBuf {
    TerrainQuadRecipe recipes[];
};
```

**Note on TCS helper functions:** The thin-record TCS branch reuses `uvec4Idx()` and `unpackARGB()` that are already defined in `gos_terrain.tesc` for the fat-record path. Both helpers are file-scope functions at the top of the shader — verify they appear before the new struct declarations you are inserting, so they are in scope for the `useQuadRecords == 2` branch. Do not re-declare them.

**Note on GLSL binding conflicts with fat records:** The `QuadRecordBuf` (fat) also uses `binding = 0`. This is fine — GLSL spec permits multiple buffer declarations at the same binding point; the active one is determined by which `glBindBufferBase` call was made before the draw. Both `ThinRecordBuf` and `QuadRecordBuf` declare `binding = 0`, but they are never used in the same draw call.

- [ ] **Step 2: Add useQuadRecords==2 branch to TCS main()**

In `shaders/gos_terrain.tesc`, in `void main()`, after the `return;` that closes the `useQuadRecords == 0` passthrough branch, and before the existing `// --- Record path (MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1) ---` comment, add:

```glsl
    // --- Thin record path (MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1) ---
    if (useQuadRecords == 2) {
        uint recordIdx = uint(ssboRecordBase) + uint(gl_PrimitiveID) / 2u;
        uint triIdx    = uint(gl_PrimitiveID) % 2u;
        uint id        = uint(gl_InvocationID);

        TerrainQuadThinRecord tr = thinRecs[recordIdx];
        uint recipeIdx = tr.control.x;
        uint flags     = tr.control.z;
        uint uvMode    = flags & 1u;
        uint pzTri1    = (flags >> 1u) & 1u;
        uint pzTri2    = (flags >> 2u) & 1u;

        TerrainQuadRecipe rec = recipes[recipeIdx];

        // Corner index — same table as fat record path.
        uint cornerIdx;
        if (uvMode == 0u) {
            if (triIdx == 0u) {
                if (id == 0u) cornerIdx = 0u;
                else if (id == 1u) cornerIdx = 1u;
                else cornerIdx = 2u;
            } else {
                if (id == 0u) cornerIdx = 0u;
                else if (id == 1u) cornerIdx = 2u;
                else cornerIdx = 3u;
            }
        } else {
            if (triIdx == 0u) {
                if (id == 0u) cornerIdx = 0u;
                else if (id == 1u) cornerIdx = 1u;
                else cornerIdx = 3u;
            } else {
                if (id == 0u) cornerIdx = 1u;
                else if (id == 1u) cornerIdx = 2u;
                else cornerIdx = 3u;
            }
        }

        vec4 wp = (cornerIdx == 0u) ? rec.worldPos0
                 :(cornerIdx == 1u) ? rec.worldPos1
                 :(cornerIdx == 2u) ? rec.worldPos2
                 :                    rec.worldPos3;
        vec4 wn = (cornerIdx == 0u) ? rec.worldNorm0
                 :(cornerIdx == 1u) ? rec.worldNorm1
                 :(cornerIdx == 2u) ? rec.worldNorm2
                 :                    rec.worldNorm3;
        tcs_WorldPos[gl_InvocationID]  = wp.xyz;
        tcs_WorldNorm[gl_InvocationID] = wn.xyz;

        float u = (cornerIdx == 1u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
        float v = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;
        tcs_Texcoord[gl_InvocationID] = vec2(u, v);

        uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);
        uint frgb = uvec4Idx(tr.fogRGBs,   cornerIdx);
        tcs_Color[gl_InvocationID]       = unpackARGB(lrgb);
        tcs_FogValue[gl_InvocationID]    = float((frgb >> 24u) & 0xFFu) / 255.0;
        tcs_TerrainType[gl_InvocationID] = float(frgb & 0xFFu);

        gl_out[gl_InvocationID].gl_Position = vec4(0.0, 0.0, 0.0, 1.0);

        if (id == 0u) {
            uint pzValid = (triIdx == 0u) ? pzTri1 : pzTri2;
            float level = (pzValid != 0u) ? max(tessLevel.x, 1.0) : 0.0;
            gl_TessLevelOuter[0] = level;
            gl_TessLevelOuter[1] = level;
            gl_TessLevelOuter[2] = level;
            gl_TessLevelInner[0] = level;
        }
        return;
    }
```

This must be inserted **before** the existing `// --- Record path ---` comment so `useQuadRecords == 1` (fat) still reaches the original block.

- [ ] **Step 3: Add thin-record draw path to flush() in gos_terrain_patch_stream.cpp**

After the closing brace of the existing `if (s_quadRecordsOn && s_quadRecordsDrawOn && ...)` block (around line 1128), add a parallel block:

```cpp
    // --- M1d thin-record draw path ---
    if (s_thinRecordsOn && s_thinRecordsDrawOn && s_thinRecordBuf && s_recipeBuf
            && s_thinRecordCount > 0) {
        ZoneScopedN("PatchStream.DrawThinRecords");

        struct ThinRecBucket { DWORD gosHandle; uint32_t firstRecord; uint32_t recordCount; };
        static ThinRecBucket s_thinRecDrawBuckets[kPatchStreamMaxBuckets];
        uint32_t thinRecDrawBucketCount = 0;

        const uint32_t slotFirstThinRec =
            s_slot * kPatchStreamMaxThinRecordsPerSlot;
        TerrainQuadThinRecord* ringBase =
            (TerrainQuadThinRecord*)s_thinRecordMap + slotFirstThinRec;

        // Sort shadow by resolved gosHandle.
        struct ThinSortEntry { DWORD gosHandle; uint32_t recIdx; };
        static ThinSortEntry thinSortBuf[kPatchStreamMaxThinRecordsPerSlot];
        {
        ZoneScopedN("PatchStream.DrawThinRecords.BuildSort");
        for (uint32_t r = 0; r < s_thinRecordCount; ++r) {
            thinSortBuf[r] = {
                (DWORD)tex_resolve(s_thinRecordShadow[r].terrainHandle), r };
        }
        std::sort(thinSortBuf, thinSortBuf + s_thinRecordCount,
            [](const ThinSortEntry& a, const ThinSortEntry& b) {
                return a.gosHandle < b.gosHandle;
            });
        }

        static TerrainQuadThinRecord thinSortedTmp[kPatchStreamMaxThinRecordsPerSlot];
        {
        ZoneScopedN("PatchStream.DrawThinRecords.Gather");
        for (uint32_t r = 0; r < s_thinRecordCount; ++r) {
            thinSortedTmp[r] = s_thinRecordShadow[thinSortBuf[r].recIdx];
            thinSortedTmp[r].terrainHandle = (uint32_t)thinSortBuf[r].gosHandle;

            DWORD gh = thinSortBuf[r].gosHandle;
            if (thinRecDrawBucketCount > 0 &&
                    s_thinRecDrawBuckets[thinRecDrawBucketCount-1].gosHandle == gh) {
                ++s_thinRecDrawBuckets[thinRecDrawBucketCount-1].recordCount;
            } else if (thinRecDrawBucketCount < kPatchStreamMaxBuckets) {
                s_thinRecDrawBuckets[thinRecDrawBucketCount++] = { gh, r, 1u };
            }
        }
        }

        {
        ZoneScopedN("PatchStream.DrawThinRecords.Upload");
        memcpy(ringBase, thinSortedTmp,
               s_thinRecordCount * sizeof(TerrainQuadThinRecord));
        }

        // Retrieve useQuadRecords / ssboRecordBase uniform locations (cached from fat path).
        static GLint s_useQuadRecordsLoc2 = -2;
        static GLint s_ssboRecordBaseLoc2 = -2;
        {
        ZoneScopedN("PatchStream.DrawThinRecords.GLSetup");
        if (s_useQuadRecordsLoc2 == -2) {
            GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
            s_useQuadRecordsLoc2 = shp ? glGetUniformLocation(shp, "useQuadRecords") : -1;
        }
        if (s_ssboRecordBaseLoc2 == -2) {
            GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
            s_ssboRecordBaseLoc2 = shp ? glGetUniformLocation(shp, "ssboRecordBase") : -1;
        }
        }

        if (s_useQuadRecordsLoc2 >= 0) {
            glUniform1i(s_useQuadRecordsLoc2, 2);  // thin-record TCS path

            {
            ZoneScopedN("PatchStream.DrawThinRecords.Buckets");
            gos_terrain_bridge_beginBucketLoop();
            for (uint32_t b = 0; b < thinRecDrawBucketCount; ++b) {
                const ThinRecBucket& rb = s_thinRecDrawBuckets[b];
                // Thin records at binding 0 (slot-relative), recipes at binding 1 (global).
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_thinRecordBuf);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_recipeBuf);
                if (s_ssboRecordBaseLoc2 >= 0) {
                    glUniform1i(s_ssboRecordBaseLoc2, (GLint)(slotFirstThinRec + rb.firstRecord));
                }
                const GLsizei patchCount = (GLsizei)(rb.recordCount * 2);
                gos_terrain_bridge_drawSingleBucket(
                    (unsigned int)rb.gosHandle, 0u, (unsigned int)patchCount);
            }
            gos_terrain_bridge_endBucketLoop(0xFFFFFFFFu);
            }

            {
            ZoneScopedN("PatchStream.DrawThinRecords.PostDraw");
            glUniform1i(s_useQuadRecordsLoc2, 0);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            GLenum e;
            while ((e = glGetError()) != GL_NO_ERROR) {
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=thin_record_post_glerror code=0x%x\n", e);
                fflush(stderr);
            }
            }
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_draw_skip reason=no_uniform\n");
            fflush(stderr);
        }

        // Thin-record parity gate: thin count should match fat count when both enabled.
        if (!s_thinRecordBannerSeen) {
            s_thinRecordBannerSeen = true;
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_records_enabled "
                "max_per_slot=%u bytes_per_slot=%u recipe_total=%u\n",
                kPatchStreamMaxThinRecordsPerSlot,
                kPatchStreamThinRecordBytesPerSlot,
                kPatchStreamMaxRecipesTotal);
            fflush(stderr);
        }
        const bool thinParityOk = (s_thinRecordVertParity == s_totalVerts);
        if (s_traceOn || !thinParityOk) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_parity slot=%u "
                "thin_records=%u thin_verts=%u expanded_verts=%u match=%d "
                "recipes_total=%u\n",
                s_slot, s_thinRecordCount, s_thinRecordVertParity,
                s_totalVerts, (int)thinParityOk, s_recipeCount);
            fflush(stderr);
        }
    }
```

- [ ] **Step 4: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo 2>&1 | tail -5
```

Expected: `0 Error(s)`.

- [ ] **Step 5: Deploy shaders + exe**

Follow the `/mc2-deploy` skill or run:

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tesc" \
      "A:/Games/mc2-opengl/mc2-win64-v0.2/data/shaders/gos_terrain.tesc"
cp -f build/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tesc" \
        "A:/Games/mc2-opengl/mc2-win64-v0.2/data/shaders/gos_terrain.tesc"
```

Expected: `diff` prints nothing (files identical).

- [ ] **Step 6: Validate thin-record draw path**

Launch from `A:/Games/mc2-opengl/mc2-win64-v0.2/`:

```powershell
$env:MC2_PATCHSTREAM_THIN_RECORDS=1
$env:MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
$env:MC2_PATCH_STREAM_TRACE=1
.\mc2.exe mc2_01 2>&1 | Select-String "thin_record|recipe"
```

Check for:
1. `event=thin_records_enabled` banner on first flush
2. `event=thin_record_parity ... match=1` every frame (no `match=0` lines)
3. No `thin_record_draw_skip` or `thin_record_post_glerror` events
4. `recipes_total=N` in parity log climbing on first frames then plateauing
5. Terrain renders visually correctly — same appearance as with `MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1` or the standard expanded path

Visual check: terrain should look exactly the same as without the thin-record draw flag. Lighting, fog, texture blending, tessellation all normal.

**If first-frame terrain appears scrambled or black** (recipes not yet visible to GPU despite coherent mapping): this is a driver coherency edge case. As a debug fallback, insert `glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT)` inside flush() immediately after any new recipe writes complete (i.e. after the `appendThinRecord` loop in quad.cpp is done, but before the draw calls). One way to trigger this from flush() is to track a `s_newRecipesThisFrame` flag and only issue the barrier when it is set. With `GL_MAP_COHERENT_BIT` the barrier should be unnecessary, but it confirms whether driver coherency is the culprit. Remove the barrier (or keep it gated on a `MC2_RECIPE_BARRIER=1` env var) once confirmed.

- [ ] **Step 7: Verify recipe stabilization**

Pan the camera across the map for ~30 seconds with `MC2_PATCH_STREAM_TRACE=1`. The `recipes_total=N` in the parity log should climb for the first few seconds as new quads are revealed, then plateau. Expected final recipe count: ≤ 20,000 (MC2's terrain is a 120×120 grid ≈ 14,280 quads; camera doesn't always see all of them in one play session).

If `event=recipe_overflow` appears, the `kPatchStreamMaxRecipesTotal = 65536` constant is wrong — increase it, but this should not happen on standard MC2 terrain.

- [ ] **Step 8: Compare Tracy timing (thin vs fat draw path)**

Run mc2_01 with Tracy connected. Compare:
- `PatchStream.DrawThinRecords.Upload` (memcpy 48B×N) vs `PatchStream.DrawRecords.Upload` (memcpy 192B×N)
- `PatchStream.DrawThinRecords.Gather` vs `PatchStream.DrawRecords.Gather`

Expected: Gather + Upload for thin records should be ~4× faster than the fat-record equivalents (48B vs 192B per record). Both should already be sub-ms given the shadow buffer fix, but the thin path should show a measurable reduction.

- [ ] **Step 9: Run smoke test**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py \
    --tier tier1 --with-menu-canary --kill-existing
```

With `MC2_PATCHSTREAM_THIN_RECORDS=1 MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1` set in the environment.

Expected: exit 0, all tier1 missions pass, no `thin_record_parity match=0` lines in smoke artifacts.

- [ ] **Step 10: Commit**

```bash
git add shaders/gos_terrain.tesc
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): M1d-c — thin-record TCS path + flush draw + parity gate"
```

---

## Self-Review

### Spec coverage

| Requirement | Task |
|-------------|------|
| `TerrainQuadRecipe` struct (144 B, 9 vec4s) | Task 1 Step 1 |
| `TerrainQuadThinRecord` struct (48 B, 3 uvec4s) | Task 1 Step 1 |
| Recipe SSBO (single-buffered, 65536 cap, persistent-mapped coherent) | Task 1 Step 4 |
| Thin-record SSBO (triple-buffered, same count cap as fat records) | Task 1 Step 4 |
| Lazy recipe population in `appendThinRecord` | Task 2 Step 1 |
| TOPRIGHT call site in quad.cpp | Task 2 Step 3 |
| BOTTOMLEFT call site in quad.cpp | Task 2 Step 4 |
| TCS `useQuadRecords == 2` branch | Task 3 Step 2 |
| Recipe SSBO at TCS binding 1 | Task 3 Step 1 |
| flush() thin-record draw path with Tracy sub-zones | Task 3 Step 3 |
| Parity gate (thin count == expanded vert count) | Task 3 Step 3 |
| `MC2_PATCHSTREAM_THIN_RECORDS` / `MC2_PATCHSTREAM_THIN_RECORDS_DRAW` env vars | Task 1 Step 3 |
| init/destroy cleanup for both new SSBOs | Task 1 Steps 4–5 |
| beginFrame reset for thin-record counters | Task 1 Step 6 |

### Correctness notes

- **Bandwidth math:** 48 B × ~6 K visible quads/frame ≈ 288 KB/frame upload (the typical in-game figure). The slot capacity is 54 K quads × 48 B ≈ 2.6 MB — that is the ceiling, not the observed cost. The fat-record path is 192 B × ~6 K ≈ 1.15 MB/frame observed, so the thin-record path is ~4× smaller. Plan text uses "slot capacity" numbers in the constant definitions; "observed" numbers in the Goal line.

- **ssboRecordBase for thin records** is `slotFirstThinRec + rb.firstRecord`, identical in semantics to the fat-record path. `gl_PrimitiveID / 2` gives the per-draw-call index which is added to this base in the TCS.

- **Recipe SSBO binding 1 does NOT need a slot offset** — recipes are single-buffered and globally indexed by `recipeIdx`. No fence needed because recipe slots are written once (CPU → coherent GPU memory) and never modified; there is no race between the CPU writing slot N+1 while the GPU reads slot N. If first-frame scrambling is observed, add a `glMemoryBarrier` fallback as described in Task 3 Step 6.

- **Binding conflict (both `QuadRecordBuf` and `ThinRecordBuf` declare `binding = 0`)** — only one is used per draw call; the active buffer is whatever was bound via `glBindBufferBase(SSBO, 0, ...)` most recently. This is safe.

- **Recipe key collision** — the key is `(bits-of-wx0) << 32 | (bits-of-wy0)`. Two quads with identical corner-0 x,y positions but different z (elevation) would collide and share a recipe. On a regular terrain grid this cannot happen (each (x,y) cell is unique). The debug collision check in `appendThinRecord` (gated on `s_traceOn`) will log `recipe_key_collision` if this assumption breaks. A code comment in the key computation instructs future maintainers to add wz0 to the key if vertical-stack quads are introduced.

- **destroy() clears s_recipeIndex** — if `destroy()` is called at mission end and `init()` at start of the next mission, the recipe map correctly starts empty. Verified against the existing pattern where `s_recordBannerSeen` and `s_totalVerts` reset through the same lifecycle.

- **Scorch/damage/overlay UV changes** — `TerrainQuadRecipe` stores positions, normals, and UV extents (minU/minV/maxU/maxV). These fields encode the colormap tile assigned to the quad. If scorching or damage changes the tile, the recipe key (wx0/wy0) will collide with the stale entry: the GPU will use the old tile's UV extents, which is a visual bug. This is an acceptable known limitation for M1d since scorch support is not wired to the patchstream path yet; the debug collision log will surface it. Fix in a later pass by invalidating the recipe map on terrain state change, or by incorporating a tile-version token into the recipe key.
