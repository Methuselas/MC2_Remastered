# Assimp Mech Asset Importer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow the game to ingest FBX and GLB mech assets and convert them to the same runtime TG structures the legacy ASE pipeline already produces, with no changes to the renderer.

**Architecture:** A new `LoadFromFile()` entry point replaces direct `LoadTGMultiShapeFromASE()` calls in `mech3d.cpp`. It probes for the best available source (GLB → FBX → ASE), checks a versioned `.tglc`/`.aglc` cooked cache, and either loads the cache or runs the Assimp importer and writes a new cache. The Assimp importer populates `TG_TypeMultiShape` and `TG_AnimateShape` in memory identically to the ASE path. All code paths terminate at the existing TG runtime structures; rendering is unchanged.

**Tech Stack:** Assimp 5.3.1 (FetchContent, static, FBX + GLTF importers only), C++14, MC2 `File` I/O abstraction, CMake `ENABLE_ASSIMP_IMPORTER` option.

**Spec:** `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`

---

## File Map

| Action | Path | Responsibility |
|---|---|---|
| Create | `mclib/assimp_importer.h` | Public importer interface (`#ifdef ENABLE_ASSIMP_IMPORTER` guarded) |
| Create | `mclib/assimp_importer.cpp` | Geometry loader, animation baker, gesture mapper, node validator |
| Create | `mclib/tgl_cook.h` | Cooked cache header struct, version stamps, magic |
| Create | `mclib/tgl_cook.cpp` | `SaveTglcCopy` / `LoadTglcCopy` / `SaveAglcCopy` / `LoadAglcCopy` for shape + animation |
| Modify | `CMakeLists.txt` | `ENABLE_ASSIMP_IMPORTER` option, Assimp FetchContent, mc2_assetcook target |
| Modify | `mclib/CMakeLists.txt` | Add `tgl_cook.cpp` (always), `assimp_importer.cpp` (conditional) |
| Modify | `mclib/msl.h` | Add `LoadFromFile()` to `TG_TypeMultiShape`; `LoadAnimationFromFile()` to `TG_AnimateShape`; declare `SaveTglcCopy` / `LoadTglcCopy` / `SaveAglcCopy` / `LoadAglcCopy` |
| Modify | `mclib/msl.cpp` | Implement `LoadFromFile()` and `LoadAnimationFromFile()` (format probe, cache check, dispatch) |
| Modify | `mclib/mech3d.cpp` | Parse `[Import]` / `[LOD]` INI sections; replace `LoadTGMultiShapeFromASE` calls with `LoadFromFile`; arm discovery |
| Create | `tools/mc2_assetcook/main.cpp` | Standalone cook + validate tool (integration test harness) |
| Create | `tools/mc2_assetcook/CMakeLists.txt` | Executable target |

---

## Task 1: CMake — Assimp dependency + build option

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `mclib/CMakeLists.txt`

- [ ] **Read current CMakeLists.txt top section to understand find_package pattern**

```bash
head -100 CMakeLists.txt
```

- [ ] **Add ENABLE_ASSIMP_IMPORTER option and FetchContent block to `CMakeLists.txt`**

Add after the existing `find_package` lines (around line 65), before `add_subdirectory`:

```cmake
option(ENABLE_ASSIMP_IMPORTER "Enable FBX/GLB import via Assimp" ON)

if(ENABLE_ASSIMP_IMPORTER)
    include(FetchContent)
    FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG        v5.3.1
        GIT_SHALLOW    TRUE
    )
    set(ASSIMP_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL                   OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_IMPORTER        ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER       ON  CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS                OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(assimp)
endif()
```

- [ ] **Wire Assimp to the mc2 target in `CMakeLists.txt`**

Find the `target_link_libraries(mc2 ...)` line (currently line 197) and add the conditional:

```cmake
if(ENABLE_ASSIMP_IMPORTER)
    target_link_libraries(mc2 PRIVATE assimp::assimp)
    target_compile_definitions(mc2 PRIVATE ENABLE_ASSIMP_IMPORTER)
endif()
```

- [ ] **Add tgl_cook.cpp (always) and assimp_importer.cpp (conditional) to `mclib/CMakeLists.txt`**

At the end of the `set(SOURCES ...)` block, append:

```cmake
    tgl_cook.cpp
)

if(ENABLE_ASSIMP_IMPORTER)
    target_sources(mclib PRIVATE assimp_importer.cpp)
    target_link_libraries(mclib PRIVATE assimp::assimp)
    target_compile_definitions(mclib PRIVATE ENABLE_ASSIMP_IMPORTER)
endif()
```

Note: read `mclib/CMakeLists.txt` first — find whether it uses `add_library(mclib ...)` or a parent-scope `set(SOURCES ...)`. Adjust accordingly.

- [ ] **Create stub files so the build does not fail yet**

Create `mclib/tgl_cook.h`:
```cpp
#pragma once
// tgl_cook.h — cooked cache format (.tglc / .aglc). See Task 2 for content.
```

Create `mclib/tgl_cook.cpp`:
```cpp
#include "tgl_cook.h"
// See Task 3.
```

Create `mclib/assimp_importer.h`:
```cpp
#pragma once
// assimp_importer.h — Assimp-backed geometry/animation importer. See Task 6.
#ifdef ENABLE_ASSIMP_IMPORTER
#endif
```

Create `mclib/assimp_importer.cpp`:
```cpp
#include "assimp_importer.h"
#ifdef ENABLE_ASSIMP_IMPORTER
// See Tasks 6–9.
#endif
```

- [ ] **Build to verify Assimp fetches and compiles clean**

```bash
# use the /mc2-build skill or:
cmake --build out --config RelWithDebInfo 2>&1 | tail -20
```

Expected: build succeeds. Assimp FetchContent output visible on first run.

- [ ] **Commit**

```bash
git add CMakeLists.txt mclib/CMakeLists.txt mclib/tgl_cook.h mclib/tgl_cook.cpp mclib/assimp_importer.h mclib/assimp_importer.cpp
git commit -m "build: add ENABLE_ASSIMP_IMPORTER option with Assimp 5.3.1 (stubs)"
```

---

## Task 2: Cooked cache format — header definition

**Files:**
- Modify: `mclib/tgl_cook.h`

The `.tglc`/`.aglc` format is: `TglcHeader` struct (fixed-size) prepended to the same binary data `SaveBinaryCopy` / the animation binary writer produces. The magic values distinguish cooked files from legacy `.tgl`/`.agl` at a glance.

- [ ] **Write `mclib/tgl_cook.h`**

```cpp
#pragma once
#include <stdint.h>

// Magic values — distinct from CURRENT_SHAPE_VERSION (0xBAFDECAF) and CURRENT_ANIM_VERSION (0xBADDECAF)
static constexpr uint32_t TGLC_MAGIC      = 0xC00KEDGE;  // .tglc geometry cache
static constexpr uint32_t AGLC_MAGIC      = 0xC00KEDAN;  // .aglc animation cache

// Version stamps — bump each independently when the corresponding logic changes.
// Cache is rejected if any stamp mismatches.
static constexpr uint32_t TGLC_CACHE_FMT_VERSION    = 1;
static constexpr uint32_t TGLC_IMPORTER_VERSION      = 1;
static constexpr uint32_t TGLC_COORD_CONV_VERSION    = 1;
static constexpr uint32_t TGLC_ANIM_BAKER_VERSION    = 1;

enum class TglcSourceFormat : uint32_t {
    GLB = 1,
    FBX = 2,
};

// Fixed-size header written at byte 0 of every .tglc and .aglc file.
// Followed immediately by the same binary payload SaveBinaryCopy / SaveAnimBinaryCopy writes.
#pragma pack(push, 1)
struct TglcHeader {
    uint32_t magic;                     // TGLC_MAGIC or AGLC_MAGIC
    uint32_t cache_fmt_version;         // TGLC_CACHE_FMT_VERSION
    uint32_t importer_version;          // TGLC_IMPORTER_VERSION
    uint32_t coord_conv_version;        // TGLC_COORD_CONV_VERSION
    uint32_t anim_baker_version;        // TGLC_ANIM_BAKER_VERSION
    TglcSourceFormat source_format;     // GLB or FBX
    uint64_t source_timestamp;          // last-write time of source file (platform seconds)
    char     source_path[512];          // null-terminated path of source file
    uint32_t assimp_version;            // ASSIMP_VERSION_MAJOR<<16|MINOR<<8|PATCH
};
#pragma pack(pop)

// Returns 0 on success, -1 on mismatch or I/O error.
int  ReadTglcHeader(const char* path, TglcHeader* out);

// Returns true if the header's stamps all match current compile-time values
// and source_timestamp matches the source file's actual modification time.
bool IsTglcFresh(const TglcHeader& hdr, const char* sourcePath);

// Get file modification time as uint64_t seconds since epoch. Returns 0 on failure.
uint64_t GetFileModTime(const char* path);
```

- [ ] **Build to verify header compiles**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: no errors.

- [ ] **Commit**

```bash
git add mclib/tgl_cook.h
git commit -m "feat: add .tglc/.aglc cooked cache header format"
```

---

## Task 3: Cooked cache read/write — geometry

**Files:**
- Modify: `mclib/tgl_cook.cpp`
- Modify: `mclib/msl.h` (declare new methods)
- Modify: `mclib/msl.cpp` (implement new methods)

The approach: `SaveTglcCopy` writes `TglcHeader` + then calls the existing `SaveBinaryCopy` logic inline (it must be a method on `TG_TypeMultiShape` to access private members). `LoadTglcCopy` reads and validates the header, then calls `LoadBinaryCopy` logic on the rest.

- [ ] **Read `msl.cpp:291-320` (SaveBinaryCopy) and `msl.cpp:182-230` (LoadBinaryCopy) to understand the binary layout**

```bash
sed -n '182,240p' mclib/msl.cpp
sed -n '291,320p' mclib/msl.cpp
```

- [ ] **Add utility functions to `mclib/tgl_cook.cpp`**

```cpp
#include "tgl_cook.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

uint64_t GetFileModTime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime;
}

int ReadTglcHeader(const char* path, TglcHeader* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, sizeof(TglcHeader), f);
    fclose(f);
    return (n == sizeof(TglcHeader)) ? 0 : -1;
}

bool IsTglcFresh(const TglcHeader& hdr, const char* sourcePath) {
    if (hdr.cache_fmt_version  != TGLC_CACHE_FMT_VERSION)  return false;
    if (hdr.importer_version   != TGLC_IMPORTER_VERSION)   return false;
    if (hdr.coord_conv_version != TGLC_COORD_CONV_VERSION)  return false;
    if (hdr.anim_baker_version != TGLC_ANIM_BAKER_VERSION)  return false;
    uint64_t srcTime = GetFileModTime(sourcePath);
    return srcTime != 0 && hdr.source_timestamp >= srcTime;
}
```

- [ ] **Add `SaveTglcCopy` / `LoadTglcCopy` declarations to `msl.h`**

Inside the `TG_TypeMultiShape` class body, after the existing `SaveBinaryCopy` / `LoadBinaryCopy` declarations (around line 223):

```cpp
// Cooked-cache variants (.tglc). Header + same binary payload as SaveBinaryCopy.
void SaveTglcCopy(const char* fileName, const char* sourcePath, TglcSourceFormat fmt);
long LoadTglcCopy(const char* fileName);
```

(You'll need `#include "tgl_cook.h"` at the top of `msl.h`.)

- [ ] **Implement `SaveTglcCopy` in `msl.cpp`**

Add after the existing `SaveBinaryCopy` implementation:

```cpp
void TG_TypeMultiShape::SaveTglcCopy(const char* fileName, const char* sourcePath, TglcSourceFormat fmt) {
    // Write header to a temp memory block, then open file and write header + body.
    TglcHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic              = TGLC_MAGIC;
    hdr.cache_fmt_version  = TGLC_CACHE_FMT_VERSION;
    hdr.importer_version   = TGLC_IMPORTER_VERSION;
    hdr.coord_conv_version = TGLC_COORD_CONV_VERSION;
    hdr.anim_baker_version = TGLC_ANIM_BAKER_VERSION;
    hdr.source_format      = fmt;
    hdr.source_timestamp   = GetFileModTime(sourcePath);
#ifdef ENABLE_ASSIMP_IMPORTER
    hdr.assimp_version     = (aiGetVersionMajor() << 16) | (aiGetVersionMinor() << 8) | aiGetVersionPatch();
#endif
    strncpy(hdr.source_path, sourcePath, sizeof(hdr.source_path) - 1);

    // Write header via raw FILE* (before MC2 File object takes over for body).
    FILE* f = fopen(fileName, "wb");
    if (!f) {
        STOP(("SaveTglcCopy: cannot create %s", fileName));
        return;
    }
    fwrite(&hdr, sizeof(hdr), 1, f);
    fclose(f);

    // Append body using MC2 File in append mode.
    File binFile;
    binFile.open(fileName, CREATE | APPEND);
    binFile.writeInt(CURRENT_SHAPE_VERSION);
    binFile.writeInt(numTG_TypeShapes);
    binFile.writeInt(numTextures);
    if (numTextures)
        binFile.write((MemoryPtr)listOfTextures, sizeof(TG_Texture) * numTextures);
    for (long i = 0; i < numTG_TypeShapes; i++)
        listOfTypeShapes[i]->SaveBinaryCopy(binFile);
}
```

> **Note:** Verify that `File::open` supports an APPEND/create flag. If not, read `GameOS/gameos/` `File` implementation and adjust — you may need to use raw `fopen("ab")` for the body write too, mirroring the header write pattern above.

- [ ] **Implement `LoadTglcCopy` in `msl.cpp`**

Add after `SaveTglcCopy`:

```cpp
long TG_TypeMultiShape::LoadTglcCopy(const char* fileName) {
    TglcHeader hdr;
    if (ReadTglcHeader(fileName, &hdr) != 0 || hdr.magic != TGLC_MAGIC)
        return -1;

    // Open with MC2 File, skip past header bytes, then read like LoadBinaryCopy.
    File binFile;
    long result = binFile.open(fileName);
    if (result != NO_ERR) return -1;
    binFile.seek(sizeof(TglcHeader));   // skip header

    DWORD version = binFile.readInt();
    if (version != CURRENT_SHAPE_VERSION) return -1;

    // Delegate to the existing LoadBinaryCopy body logic.
    // LoadBinaryCopy re-opens the file internally, so we call it on the .tglc path
    // but skip the header via a wrapper. Simplest: duplicate the LoadBinaryCopy body here.
    // Copy msl.cpp:190-230 here, operating on the already-opened binFile.
    // (See LoadBinaryCopy for exact field order.)
    numTG_TypeShapes = binFile.readInt();
    numTextures      = binFile.readInt();
    // ... (copy remaining fields from LoadBinaryCopy exactly)
    return NO_ERR;
}
```

> **Implementation note:** `LoadBinaryCopy` opens the file itself (line 184: `binFile.open(fileName)`). `LoadTglcCopy` needs to skip the header first. The cleanest approach is to check whether `File` supports `seek()` — if yes, open the .tglc file, seek past the header, then read. If `File::seek()` is not available, read the full body of `LoadBinaryCopy` and duplicate it here after the header skip. Check `GameOS/gameos/File.cpp` or similar.

- [ ] **Build to verify no compile errors**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -20
```

- [ ] **Commit**

```bash
git add mclib/tgl_cook.h mclib/tgl_cook.cpp mclib/msl.h mclib/msl.cpp
git commit -m "feat: add SaveTglcCopy/LoadTglcCopy for cooked geometry cache"
```

---

## Task 4: Cooked cache read/write — animation

**Files:**
- Modify: `mclib/msl.h` (add SaveAglcCopy / LoadAglcCopy to TG_AnimateShape)
- Modify: `mclib/msl.cpp` (implement them)

Same pattern as Task 3 but for `.aglc`. The body payload is what the existing animation `SaveBinaryCopy` produces.

- [ ] **Read the animation binary writer/reader to understand the payload**

```bash
sed -n '1960,2060p' mclib/msl.cpp
```

- [ ] **Add declarations to `TG_AnimateShape` in `msl.h`** (near line 617)

```cpp
void SaveAglcCopy(const char* fileName, const char* sourcePath, TglcSourceFormat fmt);
long LoadAglcCopy(const char* fileName);
```

- [ ] **Implement `SaveAglcCopy` in `msl.cpp`**

After the existing animation `SaveBinaryCopy`:

```cpp
void TG_AnimateShape::SaveAglcCopy(const char* fileName, const char* sourcePath, TglcSourceFormat fmt) {
    TglcHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic              = AGLC_MAGIC;
    hdr.cache_fmt_version  = TGLC_CACHE_FMT_VERSION;
    hdr.importer_version   = TGLC_IMPORTER_VERSION;
    hdr.coord_conv_version = TGLC_COORD_CONV_VERSION;
    hdr.anim_baker_version = TGLC_ANIM_BAKER_VERSION;
    hdr.source_format      = fmt;
    hdr.source_timestamp   = GetFileModTime(sourcePath);
    strncpy(hdr.source_path, sourcePath, sizeof(hdr.source_path) - 1);

    FILE* f = fopen(fileName, "wb");
    if (!f) { STOP(("SaveAglcCopy: cannot create %s", fileName)); return; }
    fwrite(&hdr, sizeof(hdr), 1, f);
    fclose(f);

    File binFile;
    binFile.open(fileName, CREATE | APPEND);
    binFile.writeInt(CURRENT_ANIM_VERSION);
    binFile.writeInt(count);
    for (long i = 0; i < count; i++) {
        if (listOfAnimation[i])
            listOfAnimation[i]->SaveBinaryCopy(&binFile);
        else {
            // Write a null slot marker
            binFile.writeInt(0);  // numFrames = 0 → reader skips
        }
    }
}
```

> **Implementation note:** Read the existing animation `SaveBinaryCopy` at `msl.cpp:1966` to see exact field order per `_TG_Animation`. Match it exactly for the body. Null animation slots (missing gestures) need a sentinel so `LoadAglcCopy` can skip them correctly.

- [ ] **Implement `LoadAglcCopy` in `msl.cpp`**

```cpp
long TG_AnimateShape::LoadAglcCopy(const char* fileName) {
    TglcHeader hdr;
    if (ReadTglcHeader(fileName, &hdr) != 0 || hdr.magic != AGLC_MAGIC)
        return -1;
    // Skip header, read like LoadBinaryCopy.
    // (Mirror LoadBinaryCopy body, offsetting by sizeof(TglcHeader).)
    File binFile;
    if (binFile.open(fileName) != NO_ERR) return -1;
    binFile.seek(sizeof(TglcHeader));
    DWORD version = binFile.readInt();
    if (version != CURRENT_ANIM_VERSION) return -1;
    // ... copy remaining LoadBinaryCopy body
    return NO_ERR;
}
```

- [ ] **Build clean**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -10
```

- [ ] **Commit**

```bash
git add mclib/msl.h mclib/msl.cpp
git commit -m "feat: add SaveAglcCopy/LoadAglcCopy for cooked animation cache"
```

---

## Task 5: Format probe and file resolution

**Files:**
- Modify: `mclib/msl.h` (add `LoadFromFile` / `LoadAnimationFromFile`)
- Modify: `mclib/msl.cpp` (implement them)

These are the new entry points that replace the direct `LoadTGMultiShapeFromASE` / `LoadTGMultiShapeAnimationFromASE` calls in `mech3d.cpp`.

- [ ] **Add `LoadFromFile` declaration to `TG_TypeMultiShape` in `msl.h`**

```cpp
// Format-agnostic entry point. Probes for .glb / .fbx / .ase.
// baseName: name without extension (e.g. "madcat").
// Returns NO_ERR on success.
long LoadFromFile(const char* baseName, bool forceCook = false);
```

- [ ] **Implement `LoadFromFile` in `msl.cpp`**

```cpp
long TG_TypeMultiShape::LoadFromFile(const char* baseName, bool forceCook) {
    // Build candidate paths
    FullPathFileName glbPath, fbxPath, asePath, tglcPath;
    glbPath.init(tglPath,  baseName, ".glb");
    fbxPath.init(tglPath,  baseName, ".fbx");
    asePath.init(tglPath,  baseName, ".ase");
    tglcPath.init(tglPath, baseName, ".tglc");

    const char* srcPath  = nullptr;
    TglcSourceFormat fmt = TglcSourceFormat::GLB;

    if (fileExists(glbPath, FILE_ON_DISK)) {
        srcPath = glbPath; fmt = TglcSourceFormat::GLB;
    } else if (fileExists(fbxPath, FILE_ON_DISK)) {
        srcPath = fbxPath; fmt = TglcSourceFormat::FBX;
    } else if (fileExists(asePath, FILE_ON_DISK) || fileExists(asePath)) {
        // Fall through to legacy ASE path
        return LoadTGMultiShapeFromASE(asePath);
    } else {
        // No source on disk — try legacy cache path
        return LoadTGMultiShapeFromASE(asePath);
    }

    // Check for fresh .tglc cache
    if (!forceCook && fileExists(tglcPath, FILE_ON_DISK)) {
        TglcHeader hdr;
        if (ReadTglcHeader(tglcPath, &hdr) == 0 && IsTglcFresh(hdr, srcPath)) {
            return LoadTglcCopy(tglcPath);
        }
    }

#ifdef ENABLE_ASSIMP_IMPORTER
    // Import from source
    long result = ImportGeometryFromFile(srcPath, this);
    if (result != NO_ERR) return result;
    SaveTglcCopy(tglcPath, srcPath, fmt);
    return NO_ERR;
#else
    STOP(("LoadFromFile: %s requires ENABLE_ASSIMP_IMPORTER=ON", srcPath));
    return -1;
#endif
}
```

- [ ] **Add `LoadAnimationFromFile` declaration to `TG_AnimateShape` in `msl.h`**

```cpp
long LoadAnimationFromFile(const char* baseName, TG_TypeMultiShapePtr shape, bool forceCook = false);
```

- [ ] **Implement `LoadAnimationFromFile` in `msl.cpp`**

```cpp
long TG_AnimateShape::LoadAnimationFromFile(const char* baseName, TG_TypeMultiShapePtr shape, bool forceCook) {
    FullPathFileName glbPath, fbxPath, asePath, aglcPath;
    glbPath.init(tglPath,  baseName, ".glb");
    fbxPath.init(tglPath,  baseName, ".fbx");
    asePath.init(tglPath,  baseName, ".ase");
    aglcPath.init(tglPath, baseName, ".aglc");

    const char* srcPath  = nullptr;
    TglcSourceFormat fmt = TglcSourceFormat::GLB;

    // For GLB, the animation lives in the primary mesh file (baseName = mech base, not anim suffix)
    // For FBX/ASE, baseName already includes the animation suffix (e.g. "madcatwalk")
    if (fileExists(glbPath, FILE_ON_DISK)) {
        srcPath = glbPath; fmt = TglcSourceFormat::GLB;
    } else if (fileExists(fbxPath, FILE_ON_DISK)) {
        srcPath = fbxPath; fmt = TglcSourceFormat::FBX;
    } else {
        // Fall through to legacy ASE path
        FullPathFileName aglPath;
        aglPath.init(tglPath, baseName, ".agl");
        return LoadTGMultiShapeAnimationFromASE(asePath, shape);
    }

    if (!forceCook && fileExists(aglcPath, FILE_ON_DISK)) {
        TglcHeader hdr;
        if (ReadTglcHeader(aglcPath, &hdr) == 0 && IsTglcFresh(hdr, srcPath)) {
            return LoadAglcCopy(aglcPath);
        }
    }

#ifdef ENABLE_ASSIMP_IMPORTER
    long result = ImportAnimationFromFile(srcPath, baseName, shape, this);
    if (result != NO_ERR) return result;
    SaveAglcCopy(aglcPath, srcPath, fmt);
    return NO_ERR;
#else
    STOP(("LoadAnimationFromFile: %s requires ENABLE_ASSIMP_IMPORTER=ON", srcPath));
    return -1;
#endif
}
```

> **Implementation note on GLB animation paths:** For GLB, all gesture animations are clips within the same `.glb` file. The caller in `mech3d.cpp` currently passes a per-gesture suffix name (e.g. `"madcatwalk"`). When the primary source is a GLB, `LoadAnimationFromFile` must be called with the **base mech name** (e.g. `"madcat"`), not the per-gesture name. The gesture ID is passed via `ImportAnimationFromFile`. This requires a small change in the `mech3d.cpp` call site — see Task 11.

- [ ] **Build clean (stubs referenced by LoadFromFile don't exist yet — expect linker errors, not compile errors)**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "undefined|error:" | head -20
```

Expected: linker errors for `ImportGeometryFromFile` / `ImportAnimationFromFile` — these are implemented in Task 6.

- [ ] **Commit**

```bash
git add mclib/msl.h mclib/msl.cpp
git commit -m "feat: add LoadFromFile/LoadAnimationFromFile format-probe entry points"
```

---

## Task 6: Assimp geometry importer

**Files:**
- Modify: `mclib/assimp_importer.h`
- Modify: `mclib/assimp_importer.cpp`

Populates a `TG_TypeMultiShape` from an Assimp-loaded scene. Output is byte-for-byte equivalent to what `ParseASEFile` produces for the same geometry.

- [ ] **Write `mclib/assimp_importer.h`**

```cpp
#pragma once
#ifdef ENABLE_ASSIMP_IMPORTER

#include "msl.h"
#include "tgl_cook.h"

// Import geometry from a GLB or FBX file into an already-constructed TG_TypeMultiShape.
// Returns NO_ERR on success.
long ImportGeometryFromFile(const char* path, TG_TypeMultiShape* out);

// Import animation gesture from a GLB/FBX into an already-constructed TG_AnimateShape.
// gestureId: 0–26 from MechAnimationNames. clipBaseName: canonical clip name to look for.
// For GLB: path is the primary mech file. For FBX: path is the per-animation file.
long ImportAnimationFromFile(const char* path, const char* clipBaseName,
                             TG_TypeMultiShapePtr shape, TG_AnimateShape* out);

#endif // ENABLE_ASSIMP_IMPORTER
```

- [ ] **Write the coordinate transform helpers in `mclib/assimp_importer.cpp`**

```cpp
#include "assimp_importer.h"
#ifdef ENABLE_ASSIMP_IMPORTER

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/version.h>
#include "stuff/point3d.h"   // Stuff::Point3D
#include "stuff/vector3d.h"  // Stuff::Vector3D

// MC2 coordinate space: mc2.x = -src.x, mc2.y = src.z, mc2.z = src.y
// (matches ParseASEFile coord flip at tgl.cpp:806-814 and 875-882)
static Stuff::Point3D toMC2Pos(const aiVector3D& v) {
    return Stuff::Point3D{ -v.x, v.z, v.y };
}
static Stuff::Vector3D toMC2Vec(const aiVector3D& v) {
    return Stuff::Vector3D{ -v.x, v.z, v.y };
}
// UV V-flip: mc2.v = 1.0 - src.v  (matches tgl.cpp:1024)
static float toMC2V(float v) { return 1.0f - v; }
```

- [ ] **Write `ImportGeometryFromFile`**

```cpp
static long ImportShapeFromMesh(const aiScene* scene, const aiNode* node,
                                 TG_TypeMultiShape* out, long shapeIdx);

long ImportGeometryFromFile(const char* path, TG_TypeMultiShape* out) {
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate           |
        aiProcess_GenSmoothNormals      |
        aiProcess_CalcTangentSpace      |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ValidateDataStructure |
        aiProcess_SortByPType);

    if (!scene || !scene->mRootNode) {
        PAUSE(("ImportGeometryFromFile: Assimp failed on %s: %s", path, imp.GetErrorString()));
        return -1;
    }

    // Count renderable mesh nodes (exclude animation-only nodes, cameras, lights)
    long numShapes = (long)scene->mNumMeshes;
    if (numShapes == 0) {
        PAUSE(("ImportGeometryFromFile: no meshes in %s", path));
        return -1;
    }

    // Validate: check for duplicate node names, name length
    for (unsigned i = 0; i < scene->mRootNode->mNumChildren; i++) {
        const aiNode* n = scene->mRootNode->mChildren[i];
        if (strlen(n->mName.C_Str()) > 24) {
            STOP(("ImportGeometryFromFile: node name '%s' exceeds 24 chars in %s",
                  n->mName.C_Str(), path));
            return -1;
        }
    }

    // Allocate TG_TypeMultiShape storage
    out->numTG_TypeShapes = numShapes;
    out->listOfTypeShapes = (TG_TypeNodePtr*)TG_Shape::tglHeap->Malloc(
        sizeof(TG_TypeNodePtr) * numShapes);
    memset(out->listOfTypeShapes, 0, sizeof(TG_TypeNodePtr) * numShapes);

    // Build texture list from all materials
    // (see ValidateNodeNames helper below for warnings)
    ValidateNodePresence(scene, path);

    // Import each mesh as a TG_TypeShape
    for (unsigned i = 0; i < scene->mNumMeshes; i++) {
        long r = ImportShapeFromMesh(scene, scene->mRootNode, out, (long)i);
        if (r != NO_ERR) return r;
    }
    return NO_ERR;
}
```

- [ ] **Write `ImportShapeFromMesh` — populate one `TG_TypeShape`**

```cpp
static long ImportShapeFromMesh(const aiScene* scene, const aiNode* node,
                                 TG_TypeMultiShape* out, long shapeIdx) {
    const aiMesh* mesh = scene->mMeshes[shapeIdx];

    void* mem = TG_Shape::tglHeap->Malloc(sizeof(TG_TypeShape));
    TG_TypeShape* ts = ::new(mem) TG_TypeShape();
    out->listOfTypeShapes[shapeIdx] = ts;

    // Node name and parent name
    // Find the aiNode that references this mesh
    // (walk scene graph to find mMeshes[] reference)
    const aiNode* meshNode = FindNodeForMesh(scene->mRootNode, shapeIdx);
    if (meshNode) {
        strncpy(ts->nodeId,   meshNode->mName.C_Str(), TG_NODE_ID - 1);
        ts->nodeId[TG_NODE_ID - 1] = 0;
        if (meshNode->mParent && meshNode->mParent != scene->mRootNode)
            strncpy(ts->parentId, meshNode->mParent->mName.C_Str(), TG_NODE_ID - 1);
        else
            strncpy(ts->parentId, "None", TG_NODE_ID - 1);
    }

    // Node center: Assimp node transform translation component, in MC2 space
    if (meshNode) {
        aiVector3D pos, scale; aiQuaternion rot;
        meshNode->mTransformation.Decompose(scale, rot, pos);
        ts->nodeCenter = toMC2Pos(pos);
        ts->relativeNodeCenter = ts->nodeCenter;
    }

    // Vertices
    ts->numTypeVertices = mesh->mNumVertices;
    ts->listOfTypeVertices = (TG_TypeVertexPtr)TG_Shape::tglHeap->Malloc(
        sizeof(TG_TypeVertex) * mesh->mNumVertices);

    for (unsigned v = 0; v < mesh->mNumVertices; v++) {
        ts->listOfTypeVertices[v].position  = toMC2Pos(mesh->mVertices[v]);
        ts->listOfTypeVertices[v].normal    = toMC2Vec(mesh->mNormals[v]);
        ts->listOfTypeVertices[v].aRGBLight = 0xff000000;
    }

    // Triangles
    ts->numTypeTriangles = mesh->mNumFaces;
    ts->listOfTypeTriangles = (TG_TypeTrianglePtr)TG_Shape::tglHeap->Malloc(
        sizeof(TG_TypeTriangle) * mesh->mNumFaces);

    for (unsigned f = 0; f < mesh->mNumFaces; f++) {
        const aiFace& face = mesh->mFaces[f];
        TG_TypeTriangle& tri = ts->listOfTypeTriangles[f];

        tri.Vertices[0] = face.mIndices[0];
        tri.Vertices[1] = face.mIndices[1];
        tri.Vertices[2] = face.mIndices[2];
        tri.localTextureHandle  = mesh->mMaterialIndex;
        tri.renderStateFlags    = 0;

        // Compute face normal from vertex normals (average)
        Stuff::Vector3D fn = ts->listOfTypeVertices[face.mIndices[0]].normal;
        fn += ts->listOfTypeVertices[face.mIndices[1]].normal;
        fn += ts->listOfTypeVertices[face.mIndices[2]].normal;
        fn /= 3.0f;
        tri.faceNormal = fn;

        // UV coordinates — V flipped
        if (mesh->HasTextureCoords(0)) {
            tri.uvdata.u0 = mesh->mTextureCoords[0][face.mIndices[0]].x;
            tri.uvdata.v0 = toMC2V(mesh->mTextureCoords[0][face.mIndices[0]].y);
            tri.uvdata.u1 = mesh->mTextureCoords[0][face.mIndices[1]].x;
            tri.uvdata.v1 = toMC2V(mesh->mTextureCoords[0][face.mIndices[1]].y);
            tri.uvdata.u2 = mesh->mTextureCoords[0][face.mIndices[2]].x;
            tri.uvdata.v2 = toMC2V(mesh->mTextureCoords[0][face.mIndices[2]].y);
        }
    }

    // Texture list — one entry per material in scene
    // (build once per TG_TypeMultiShape, not per shape — move to ImportGeometryFromFile)
    ts->alphaTestOn = false;
    ts->filterOn    = true;
    return NO_ERR;
}
```

> **Implementation note:** Texture discovery (populating `listOfTextures` on `TG_TypeMultiShape`) should be done once in `ImportGeometryFromFile` across all meshes/materials, not per shape. Read `TG_Texture` definition in `msl.h` to see the exact struct fields (`char textureName[]`, `bool textureAlpha`, etc.). The `localTextureHandle` on each triangle is an index into that shared list.

- [ ] **Write `FindNodeForMesh` utility**

```cpp
static const aiNode* FindNodeForMesh(const aiNode* node, unsigned meshIdx) {
    for (unsigned i = 0; i < node->mNumMeshes; i++)
        if (node->mMeshes[i] == meshIdx) return node;
    for (unsigned i = 0; i < node->mNumChildren; i++) {
        const aiNode* found = FindNodeForMesh(node->mChildren[i], meshIdx);
        if (found) return found;
    }
    return nullptr;
}
```

- [ ] **Write `ValidateNodePresence` — warnings for missing gameplay nodes**

```cpp
static void ValidateNodePresence(const aiScene* scene, const char* path) {
    auto hasNode = [&](const char* prefix) -> bool {
        for (unsigned i = 0; i < scene->mRootNode->mNumChildren; i++) {
            if (S_strnicmp(scene->mRootNode->mChildren[i]->mName.C_Str(), prefix, strlen(prefix)) == 0)
                return true;
        }
        return false;
    };
    if (!hasNode("weapon_"))      gosASSERT(false || (SPEW(0,("[importer] warning: %s missing weapon_* nodes — weapon fire points won't work\n", path)), true));
    if (!hasNode("dust_lfoot"))   gosASSERT(false || (SPEW(0,("[importer] warning: %s missing dust_lfoot\n", path)), true));
    if (!hasNode("dust_rfoot"))   gosASSERT(false || (SPEW(0,("[importer] warning: %s missing dust_rfoot\n", path)), true));
    if (!hasNode("smoke_torso"))  gosASSERT(false || (SPEW(0,("[importer] warning: %s missing smoke_torso\n", path)), true));
}
```

> **Implementation note:** Replace `gosASSERT(false || (SPEW...))` with whatever the project uses for non-fatal warnings. Check `GameOS/gameos/gameos.hpp` for the correct logging macro — `SPEW`, `OUTPUT`, or similar.

- [ ] **Build and verify geometry importer compiles**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -20
```

- [ ] **Commit**

```bash
git add mclib/assimp_importer.h mclib/assimp_importer.cpp
git commit -m "feat: Assimp geometry importer (TG_TypeShape population from GLB/FBX)"
```

---

## Task 7: Gesture mapper

**Files:**
- Modify: `mclib/assimp_importer.cpp`

Resolves an Assimp animation clip name to a gesture index using the alias table from the spec.

- [ ] **Add gesture slot table to `assimp_importer.cpp`**

```cpp
struct GestureSlot {
    int  id;
    int  sharesWithId;  // -1 = independent; >=0 = copy from this slot if clip absent
    const char* canonicalName;
};

// Source of truth: MechAnimationNames[] at mech3d.cpp:143
static const GestureSlot kGestureSlots[] = {
    {  0, -1, "standtopark"      },
    {  1, -1, "parktostand"      },
    {  2, -1, "stand"            },  // no animation file
    {  3, -1, "standtowalk"      },
    {  4, -1, "walk"             },
    {  5,  0, "standtopark"      },  // reuses gesture 0 file
    {  6, -1, "walktorun"        },
    {  7, -1, "run"              },
    {  8, -1, "runtowalk"        },
    {  9,  4, "reverse"          },  // reuses Walk animation
    { 10,  3, "standtoreverse"   },  // reuses STtoWK animation (gesture 3)
    { 11, -1, "limpleft"         },
    { 12, -1, "limpright"        },
    { 13, -1, "idle"             },
    { 14, -1, "fallbackward"     },
    { 15, -1, "fallforward"      },
    { 16, -1, "hitfront"         },
    { 17, -1, "hitback"          },
    { 18, -1, "hitleft"          },
    { 19, -1, "hitright"         },
    { 20, -1, "jump"             },
    { 21, -1, "getupback"        },
    { 22, -1, "getupfront"       },
    { 23, 15, "fallenforward"    },  // reuses FallForward (gesture 15)
    { 24, 14, "fallenbackward"   },  // reuses FallBackward (gesture 14)
    { 25, -1, "fallbackwarddam"  },
    { 26, -1, "fallforwarddam"   },
};
static const int kNumGestureSlots = (int)(sizeof(kGestureSlots) / sizeof(kGestureSlots[0]));
```

- [ ] **Add alias table**

```cpp
struct GestureAlias {
    const char* alias;
    int gestureId;
};

static const GestureAlias kAliases[] = {
    { "stowk",             3 },
    { "st_to_wk",          3 },
    { "stand_to_walk",     3 },
    { "wktorn",            6 },
    { "wk_to_rn",          6 },
    { "walk_to_run",       6 },
    { "rntowk",            8 },
    { "rn_to_wk",          8 },
    { "run_to_walk",       8 },
    { "wktost",           10 },
    { "wk_to_st",         10 },
    { "walk_to_stand",    10 },
    { "getupback",        21 },
    { "getup_back",       21 },
    { "getupfront",       22 },
    { "getup_front",      22 },
    { "fallforwardpose",  23 },
    { "fallenforward",    23 },
    { "fallbackwardpose", 24 },
    { "fallenbackward",   24 },
};
static const int kNumAliases = (int)(sizeof(kAliases) / sizeof(kAliases[0]));
```

- [ ] **Write `MapClipToGestureId`**

```cpp
// Returns gesture index (0–26), or -1 if not recognized.
static int MapClipToGestureId(const char* clipName) {
    // Case-insensitive: lowercase the input
    char lower[256] = {};
    for (int i = 0; clipName[i] && i < 255; i++)
        lower[i] = (char)tolower((unsigned char)clipName[i]);

    // Check canonical names
    for (int i = 0; i < kNumGestureSlots; i++)
        if (strcmp(lower, kGestureSlots[i].canonicalName) == 0)
            return kGestureSlots[i].id;

    // Check aliases
    for (int i = 0; i < kNumAliases; i++)
        if (strcmp(lower, kAliases[i].alias) == 0)
            return kAliases[i].gestureId;

    return -1;
}
```

- [ ] **Build clean**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -10
```

- [ ] **Commit**

```bash
git add mclib/assimp_importer.cpp
git commit -m "feat: gesture mapper — clip name alias table and MapClipToGestureId"
```

---

## Task 8: Animation baker

**Files:**
- Modify: `mclib/assimp_importer.cpp`

Converts Assimp's sparse keyframe channels into the dense `quat[numFrames]` / `pos[numFrames]` arrays `TG_Animation` expects.

- [ ] **Write quaternion slerp helper (Assimp has one but bring our own for portability)**

```cpp
// Linear blend factor from a sorted keyframe array at time t.
// Returns lo_idx; alpha is blend from lo to lo+1.
static unsigned FindKeyframeLo(float t, const aiVectorKey* keys, unsigned count) {
    for (unsigned i = 0; i + 1 < count; i++)
        if (t < (float)keys[i + 1].mTime) return i;
    return count > 0 ? count - 1 : 0;
}
static unsigned FindRotKeyframeLo(float t, const aiQuatKey* keys, unsigned count) {
    for (unsigned i = 0; i + 1 < count; i++)
        if (t < (float)keys[i + 1].mTime) return i;
    return count > 0 ? count - 1 : 0;
}

static Stuff::UnitQuaternion aiQuatToMC2(const aiQuaternion& q) {
    // Assimp quaternion: w, x, y, z — right-handed Y-up (same as 3DS Max source space)
    // MC2 consumes quaternions directly from this space (ParseASEFile loads them without
    // coord transformation — only positions/normals are flipped).
    // Apply the same: no coordinate remap on quaternions for now.
    // If rotation axes appear wrong during integration test, apply:
    //   mc2.w = q.w, mc2.x = -q.x, mc2.y = q.z, mc2.z = q.y
    Stuff::UnitQuaternion out;
    out.w = q.w; out.x = q.x; out.y = q.y; out.z = q.z;
    out.Normalize();
    return out;
}
```

> **Coordinate note:** The correct quaternion remapping for the MC2 axis flip needs empirical verification during integration testing (Task 13). The comment above shows the likely correct remap. If mechs rotate on wrong axes, apply `{w, -x, z, y}` instead of `{w, x, y, z}`.

- [ ] **Write `BakeNodeAnimation`**

```cpp
// Bakes one aiNodeAnim channel into a TG_Animation for a given TG_TypeShape node.
static void BakeNodeAnimation(const aiNodeAnim* chan, int numFrames, float fps,
                               float ticksPerSec, _TG_Animation* out) {
    strncpy(out->nodeId, chan->mNodeName.C_Str(), TG_NODE_ID - 1);
    out->nodeId[TG_NODE_ID - 1] = 0;
    out->numFrames  = (DWORD)numFrames;
    out->frameRate  = fps;
    out->tickRate   = ticksPerSec / fps;

    out->quat = (_TG_Animation::QuatType*)TG_Shape::tglHeap->Malloc(
        sizeof(Stuff::UnitQuaternion) * numFrames);
    out->pos  = nullptr;

    bool hasPos = chan->mNumPositionKeys > 1;
    if (hasPos)
        out->pos = (Stuff::Point3D*)TG_Shape::tglHeap->Malloc(
            sizeof(Stuff::Point3D) * numFrames);

    for (int f = 0; f < numFrames; f++) {
        float t = (f / fps) * ticksPerSec;  // frame time in ticks

        // Rotation slerp
        unsigned lo = FindRotKeyframeLo(t, chan->mRotationKeys, chan->mNumRotationKeys);
        unsigned hi = (lo + 1 < chan->mNumRotationKeys) ? lo + 1 : lo;
        float alpha = 0.0f;
        if (hi != lo) {
            float dt = (float)(chan->mRotationKeys[hi].mTime - chan->mRotationKeys[lo].mTime);
            if (dt > 1e-6f)
                alpha = (t - (float)chan->mRotationKeys[lo].mTime) / dt;
            alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        }
        aiQuaternion blended;
        aiQuaternion::Interpolate(blended,
            chan->mRotationKeys[lo].mValue,
            chan->mRotationKeys[hi].mValue, alpha);
        blended.Normalize();
        out->quat[f] = aiQuatToMC2(blended);

        // Position lerp
        if (hasPos) {
            unsigned plo = FindKeyframeLo(t, chan->mPositionKeys, chan->mNumPositionKeys);
            unsigned phi = (plo + 1 < chan->mNumPositionKeys) ? plo + 1 : plo;
            float palpha = 0.0f;
            if (phi != plo) {
                float dt = (float)(chan->mPositionKeys[phi].mTime - chan->mPositionKeys[plo].mTime);
                if (dt > 1e-6f)
                    palpha = (t - (float)chan->mPositionKeys[plo].mTime) / dt;
                palpha = palpha < 0.0f ? 0.0f : (palpha > 1.0f ? 1.0f : palpha);
            }
            aiVector3D pblend = chan->mPositionKeys[plo].mValue * (1.0f - palpha)
                              + chan->mPositionKeys[phi].mValue * palpha;
            out->pos[f] = toMC2Pos(pblend);
        }
    }
}
```

- [ ] **Write `ImportAnimationFromFile`**

```cpp
long ImportAnimationFromFile(const char* path, const char* clipBaseName,
                              TG_TypeMultiShapePtr shape, TG_AnimateShape* out) {
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_SortByPType);

    if (!scene) {
        SPEW(0, ("[importer] %s: %s\n", path, imp.GetErrorString()));
        return -1;
    }

    int gestureId = MapClipToGestureId(clipBaseName);

    // Find the matching animation clip by name
    const aiAnimation* anim = nullptr;
    for (unsigned i = 0; i < scene->mNumAnimations; i++) {
        if (MapClipToGestureId(scene->mAnimations[i]->mName.C_Str()) == gestureId) {
            anim = scene->mAnimations[i];
            break;
        }
        // Also warn on unknown clip names
        if (MapClipToGestureId(scene->mAnimations[i]->mName.C_Str()) == -1) {
            SPEW(0, ("[importer] warning: clip '%s' in %s does not match any gesture — skipped\n",
                     scene->mAnimations[i]->mName.C_Str(), path));
        }
    }

    if (!anim) {
        // Missing optional gesture is OK
        SPEW(0, ("[importer] gesture '%s' (id=%d): no clip found in %s — mechAnim[%d]=null\n",
                 clipBaseName, gestureId, path, gestureId));
        return -1;  // caller sets mechAnim[i] = null
    }

    float ticksPerSec = (float)(anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0);
    float duration    = (float)anim->mDuration;
    int   numFrames   = (int)(duration / ticksPerSec * 25.0f) + 1;  // bake at 25 fps
    float fps         = 25.0f;

    out->count = shape->numTG_TypeShapes;
    out->listOfAnimation = (_TG_AnimationPtr)TG_Shape::tglHeap->Malloc(
        sizeof(_TG_Animation*) * out->count);
    memset(out->listOfAnimation, 0, sizeof(_TG_Animation*) * out->count);

    // Match animation channels to shape nodes
    for (unsigned c = 0; c < anim->mNumChannels; c++) {
        const aiNodeAnim* chan = anim->mChannels[c];
        for (long s = 0; s < shape->numTG_TypeShapes; s++) {
            if (S_strnicmp(shape->listOfTypeShapes[s]->nodeId,
                           chan->mNodeName.C_Str(), TG_NODE_ID) == 0) {
                void* mem = TG_Shape::tglHeap->Malloc(sizeof(_TG_Animation));
                _TG_Animation* ta = ::new(mem) _TG_Animation();
                BakeNodeAnimation(chan, numFrames, fps, ticksPerSec, ta);
                out->listOfAnimation[s] = ta;
                break;
            }
        }
    }
    return NO_ERR;
}
```

> **Implementation note:** `_TG_Animation` (with underscore) is the internal type used in `SaveBinaryCopy`. Check `msl.h` around line 329 for the exact type name — it may be `TG_Animation` without the underscore. Use whatever `listOfAnimation[]` elements are declared as.

- [ ] **Build clean**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -20
```

- [ ] **Commit**

```bash
git add mclib/assimp_importer.cpp
git commit -m "feat: animation baker — sparse Assimp keyframes to dense TG_Animation quat arrays"
```

---

## Task 9: INI [Import] and [LOD] section parsing

**Files:**
- Modify: `mclib/mech3d.cpp`

Adds parsing of the new `[Import]` and `[LOD]` sections. The `[Import]` section provides source file, shadow node, LOD node names, and arm node/file overrides. The `[LOD]` section provides LOD transition distances.

- [ ] **Read the existing `[TGLData]` parsing block in `mech3d.cpp` to understand the INI API**

```bash
sed -n '240,310p' mclib/mech3d.cpp
```

Note the pattern: `mechFile.readIdString("FileName0", buf, 511)`, `mechFile.readIdFloat("Distance0", &val)`. Use the same pattern for new keys.

- [ ] **Add `[Import]` section parsing in `Mech3DAppearanceType::init()`**

After the existing `[TGLData]` block reads, add:

```cpp
// [Import] section — new-format asset overrides
char importSource[512]   = "";
char shadowNodeName[256] = "shadow";
char lod0NodeName[256]   = "";
char lod1NodeName[256]   = "";
char lod2NodeName[256]   = "";
char leftArmNode[256]    = "";
char rightArmNode[256]   = "";
char leftArmSource[512]  = "";
char rightArmSource[512] = "";

mechFile.readIdString("Source",        importSource,  511);
mechFile.readIdString("ShadowNode",    shadowNodeName, 255);
mechFile.readIdString("LOD0",          lod0NodeName,  255);
mechFile.readIdString("LOD1",          lod1NodeName,  255);
mechFile.readIdString("LOD2",          lod2NodeName,  255);
mechFile.readIdString("LeftArmNode",   leftArmNode,   255);
mechFile.readIdString("RightArmNode",  rightArmNode,  255);
mechFile.readIdString("LeftArmSource", leftArmSource, 511);
mechFile.readIdString("RightArmSource",rightArmSource,511);
```

> **Implementation note:** Check how `mechFile.readIdString` handles missing keys — it should no-op if the key is absent. If it errors on missing keys, wrap with a try or use a default-value variant. Read nearby INI parsing code to confirm the idiom.

- [ ] **Add `[LOD]` section distance overrides**

In the LOD distance reading block, after the existing `Distance0/1/2` reads from `[TGLData]`, add a fallback read from `[LOD]`:

```cpp
// [LOD] section overrides [TGLData] distances if present
float lodDist0 = -1.0f, lodDist1 = -1.0f, lodDist2 = -1.0f;
mechFile.readIdFloat("Distance0", &lodDist0);  // from [LOD] section
mechFile.readIdFloat("Distance1", &lodDist1);
mechFile.readIdFloat("Distance2", &lodDist2);
if (lodDist0 >= 0.0f) lodDistance[0] = lodDist0;
if (lodDist1 >= 0.0f) lodDistance[1] = lodDist1;
if (lodDist2 >= 0.0f) lodDistance[2] = lodDist2;
```

- [ ] **Build clean**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -10
```

- [ ] **Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat: parse [Import] and [LOD] INI sections in Mech3DAppearanceType::init"
```

---

## Task 10: Wire mech3d.cpp load calls to new entry points

**Files:**
- Modify: `mclib/mech3d.cpp`

Replaces `LoadTGMultiShapeFromASE` calls with `LoadFromFile`. For animations, adjusts the call site to pass the base mech name (for GLB) or per-gesture name (for ASE/FBX).

- [ ] **Read all `LoadTGMultiShapeFromASE` call sites in `mech3d.cpp`**

```bash
grep -n "LoadTGMultiShapeFromASE\|LoadTGMultiShapeAnimationFromASE" mclib/mech3d.cpp
```

- [ ] **Replace geometry load calls with `LoadFromFile`**

For each call like:
```cpp
mechShape[i]->LoadTGMultiShapeFromASE(mechName);
```
Replace with:
```cpp
mechShape[i]->LoadFromFile(mechName);
```

Same for `mechShadowShape`, `leftArm`, `rightArm`, and damage shapes.

- [ ] **Replace animation load calls with `LoadAnimationFromFile`**

The animation loading loop (around `mech3d.cpp:393`) currently builds:
```cpp
sprintf(animName, "%s%s", fileName, MechAnimationNames[i]);
animPath.init(tglPath, animName, ".ase");
mechAnim[i]->LoadTGMultiShapeAnimationFromASE(animPath, mechShape[0]);
```

The new call needs to determine whether the source is GLB (pass just the base name + gesture canonical alias) or FBX/ASE (pass the existing suffix name). Detect by probing for a `.glb` first:

```cpp
FullPathFileName glbCheck;
glbCheck.init(tglPath, fileName, ".glb");
bool isGLB = (fileExists(glbCheck, FILE_ON_DISK) != 0);

for (long i = 0; i < MaxGestures; i++) {
    if (MechAnimationNames[i][0] == '\0') continue;  // gesture 2 (Stand) has no file

    if (isGLB) {
        // For GLB: pass the gesture canonical name; ImportAnimationFromFile
        // will find the clip in the primary mech GLB by gesture alias matching.
        // Use a per-gesture base name that LoadAnimationFromFile can resolve:
        // "[mechbase]:[gestureName]" — or pass baseName + gestureId via a new overload.
        // Simplest: pass baseName and let the GLB importer handle gesture selection.
        char gestureName[512];
        sprintf(gestureName, "%s", fileName);  // base name; gesture resolved in importer
        mechAnim[i] = new TG_AnimateShape();
        long r = mechAnim[i]->LoadAnimationFromFile(gestureName, mechShape[0]);
        if (r != NO_ERR) { delete mechAnim[i]; mechAnim[i] = nullptr; }
    } else {
        // Legacy ASE/FBX path: pass suffix name as before
        char animName[512];
        sprintf(animName, "%s%s", fileName, MechAnimationNames[i]);
        mechAnim[i] = new TG_AnimateShape();
        long r = mechAnim[i]->LoadAnimationFromFile(animName, mechShape[0]);
        if (r != NO_ERR) { delete mechAnim[i]; mechAnim[i] = nullptr; }
    }
}
```

> **Implementation note:** The GLB animation path needs `LoadAnimationFromFile` to know *which gesture* to look for in the GLB's clip list. The simplest solution: add a `gestureId` parameter to `LoadAnimationFromFile` (and `ImportAnimationFromFile`) so it can search for the right clip without relying on the base name alone. Update the signature in `msl.h` and `assimp_importer.h` if you take this approach. Alternatively, use the `MechAnimationNames[i]` suffix as the clip alias — it already matches the canonical names in `kGestureSlots`.

- [ ] **Handle arm discovery from INI [Import] overrides**

After parsing `[Import]` in Task 9, the arm load call becomes:

```cpp
// Left arm: try INI override, then conventional names, then legacy derived name
if (leftArmSource[0]) {
    leftArm = new TG_TypeMultiShape();
    leftArm->LoadFromFile(leftArmSource);  // explicit file override
} else if (leftArmNode[0]) {
    // Embedded in primary GLB: leftArmNode is a mesh node name, not a separate file.
    // The geometry importer already handles this if the node was discovered.
    // Store leftArmNode for use during import (pass to ImportGeometryFromFile).
    // For now: fall through to conventional derived name.
    char leftName[512]; sprintf(leftName, "%sLeftArm", fileName);
    leftArm = new TG_TypeMultiShape();
    leftArm->LoadFromFile(leftName);
} else {
    // Legacy: derives "MadCatLeftArm" from "MadCat"
    char leftName[512]; sprintf(leftName, "%sLeftArm", fileName);
    leftArm = new TG_TypeMultiShape();
    if (leftArm->LoadFromFile(leftName) != NO_ERR) {
        delete leftArm; leftArm = nullptr;
    }
}
```

Same pattern for `rightArm`.

- [ ] **Build clean**

```bash
cmake --build out --config RelWithDebInfo 2>&1 | grep -E "error:" | head -20
```

- [ ] **Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat: wire mech3d.cpp to LoadFromFile/LoadAnimationFromFile"
```

---

## Task 11: Offline cook tool (mc2_assetcook)

**Files:**
- Create: `tools/mc2_assetcook/CMakeLists.txt`
- Create: `tools/mc2_assetcook/main.cpp`
- Modify: `CMakeLists.txt` (add subdirectory)

This tool serves as both a standalone offline cooker and the primary integration test harness.

- [ ] **Create `tools/mc2_assetcook/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
if(ENABLE_ASSIMP_IMPORTER)
    add_executable(mc2_assetcook main.cpp)
    target_link_libraries(mc2_assetcook PRIVATE mclib assimp::assimp)
    target_compile_definitions(mc2_assetcook PRIVATE ENABLE_ASSIMP_IMPORTER)
    target_include_directories(mc2_assetcook PRIVATE
        ${CMAKE_SOURCE_DIR}/mclib
        ${CMAKE_SOURCE_DIR}/GameOS/include)
endif()
```

- [ ] **Add subdirectory to root `CMakeLists.txt`**

After the `add_subdirectory` lines:
```cmake
add_subdirectory("./tools/mc2_assetcook" "./out/mc2_assetcook")
```

- [ ] **Create `tools/mc2_assetcook/main.cpp`**

```cpp
#include <stdio.h>
#include <string.h>
#include "msl.h"
#include "tgl_cook.h"
#include "assimp_importer.h"

// Usage: mc2_assetcook <source.glb|source.fbx> [--output-dir <dir>]
// Cooks geometry + all animations found in the file.
// Prints validation summary and exits 0 on success, 1 on error.

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: mc2_assetcook <source.glb|source.fbx> [--output-dir <dir>]\n");
        return 1;
    }

    const char* srcPath  = argv[1];
    const char* outDir   = ".";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--output-dir") == 0 && i+1 < argc)
            outDir = argv[++i];
    }

    printf("mc2_assetcook: cooking %s\n", srcPath);

    // Cook geometry
    TG_TypeMultiShape shape;
    long r = ImportGeometryFromFile(srcPath, &shape);
    if (r != NO_ERR) {
        fprintf(stderr, "ERROR: geometry import failed\n");
        return 1;
    }

    char tglcOut[1024];
    snprintf(tglcOut, sizeof(tglcOut), "%s/cooked.tglc", outDir);
    shape.SaveTglcCopy(tglcOut, srcPath, TglcSourceFormat::GLB);
    printf("  Geometry: %d shapes, written to %s\n", shape.numTG_TypeShapes, tglcOut);

    // Cook animations
    // For each recognized clip in the source, bake and write .aglc
    // (enumerate scene->mAnimations, map to gesture IDs)
    printf("mc2_assetcook: done. Exit 0.\n");
    return 0;
}
```

> **Note:** This tool requires `mclib` to be built as a static library (or object library) that can be linked standalone without the full game engine. Check whether `mclib/CMakeLists.txt` currently produces a `STATIC` or `OBJECT` library, and whether it can be linked without `gameos`, `SDL2`, etc. If it has hard dependencies on the full engine, this tool may need to be implemented as a game-mode invocation instead (e.g., a command-line flag like `mc2.exe --cook madcat.glb`).

- [ ] **Build the tool**

```bash
cmake --build out --config RelWithDebInfo --target mc2_assetcook 2>&1 | tail -20
```

- [ ] **Commit**

```bash
git add tools/mc2_assetcook/ CMakeLists.txt
git commit -m "feat: mc2_assetcook offline cook tool and integration test harness"
```

---

## Task 12: Integration test — cook and load MadCat GLB

This is the end-to-end validation. Cook the real MadCat GLB asset, verify the .tglc/.aglc output, then load it in the game and compare visually against the ASE-loaded version.

- [ ] **Run mc2_assetcook on the MadCat GLB**

```bash
./out/mc2_assetcook/RelWithDebInfo/mc2_assetcook.exe \
  "A:/Games/mc2-opengl/MC2 Conversions/MadCat/MadCat.FBX" \
  --output-dir "A:/Games/mc2-opengl/mc2-win64-v0.2/data/tgl/"
```

Expected output:
```
mc2_assetcook: cooking MadCat.FBX
  Geometry: N shapes, written to .../madcat.tglc
mc2_assetcook: done. Exit 0.
```

If errors appear: read importer log output, fix the issue (usually coordinate transform or node name length).

- [ ] **Verify cache header is correct**

Write a quick hex check:
```bash
# First 4 bytes should be TGLC_MAGIC = 0xC00KEDGE
xxd "A:/Games/mc2-opengl/mc2-win64-v0.2/data/tgl/madcat.tglc" | head -4
```

- [ ] **Deploy the game binary and run**

Use `/mc2-deploy` skill or:
```bash
cp -f out/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```

- [ ] **Load a mission with a MadCat and observe**

In-game checklist:
- [ ] Mech appears on screen (geometry imported correctly)
- [ ] Mech is positioned at correct world location (node hierarchy correct)
- [ ] Mech textures load (texture name discovery working)
- [ ] Walk/Run animation plays and looks correct (animation baker correct)
- [ ] Torso twist works (baseRotation still applied correctly)
- [ ] No console error output about node names or cook failures

If rotation is wrong: apply the quaternion coord remap noted in Task 8 (`{w, -x, z, y}`), rebuild, re-test.

If mech is scaled wrong: verify nodeCenter is being set correctly from the Assimp node transform.

- [ ] **Verify cache is used on second load (no Assimp on warm start)**

Add a temporary `SPEW` in `LoadFromFile` that logs "loading from .tglc cache" vs "cooking from Assimp". On second game start, should see cache path.

Remove the SPEW before committing.

- [ ] **Commit passing integration state**

```bash
git add mclib/ tools/
git commit -m "feat: Assimp mech importer — MadCat GLB integration test passing"
```

---

## Self-review

### Spec coverage check

| Spec section | Covered by task |
|---|---|
| Section 1 — layers/boundary | Design only; enforced by `#ifdef ENABLE_ASSIMP_IMPORTER` in Task 1 |
| Section 2 — compile flag | Task 1 |
| Section 3 — file resolution / cache-first | Task 5 |
| Section 3 — version stamps in cache header | Task 2 |
| Section 3 — cook-at-startup | Task 5 / Task 10 |
| Section 4 — gesture names, aliases, case-insensitive | Task 7 |
| Section 4 — unknown clip warnings | Task 8 (`ImportAnimationFromFile`) |
| Section 4 — missing gesture = null | Task 8 + Task 10 |
| Section 5 — INI [Import] / [LOD] parsing | Task 9 |
| Section 5 — arm discovery (embedded vs separate) | Task 10 |
| Section 6 — geometry contract (all TG_TypeShape fields) | Task 6 |
| Section 6 — coordinate transform | Task 6 |
| Section 6 — UV V-flip | Task 6 |
| Section 7 — animation baker contract | Task 8 |
| Section 8 — out of scope (no renderer changes) | Enforced by plan scope |
| Section 9.1 — .tglc/.aglc with versioned header | Tasks 2, 3, 4 |
| Section 9.2 — owned animation slot copies | Task 8 (each slot gets its own alloc) |
| Section 9.3 — arm discovery conventions | Task 10 |
| Node validator hard errors | Task 6 (`ImportGeometryFromFile`) |
| Node validator warnings | Task 6 (`ValidateNodePresence`) |

### Missing from plan (gaps found during review)

- **Shared-file gesture copying** (gestures 9, 10, 23, 24 that reuse another gesture's data): `ImportAnimationFromFile` returns -1 for missing clips, which sets `mechAnim[i] = null`. But gestures 9/10/23/24 should *copy* from their source gesture, not be null. Add a post-loop copy step in Task 10 after all gestures are loaded:

```cpp
// After the gesture load loop in mech3d.cpp:
// Copy shared gestures (owned copies, per spec Section 9.2)
// Gesture 9 (Reverse) copies Walk (gesture 4)
if (!mechAnim[9] && mechAnim[4]) mechAnim[9] = mechAnim[4]->Clone();
// Gesture 10 (StandToReverse) copies STtoWK (gesture 3)
if (!mechAnim[10] && mechAnim[3]) mechAnim[10] = mechAnim[3]->Clone();
// Gesture 23 (FallenForward) copies FallForward (gesture 15)
if (!mechAnim[23] && mechAnim[15]) mechAnim[23] = mechAnim[15]->Clone();
// Gesture 24 (FallenBackward) copies FallBackward (gesture 14)
if (!mechAnim[24] && mechAnim[14]) mechAnim[24] = mechAnim[14]->Clone();
```

  `TG_AnimateShape::Clone()` needs to be added to `msl.h`/`msl.cpp` — a deep copy of `count`, `listOfAnimation`, and all `quat[]`/`pos[]` arrays. Add this to Task 10.

- **Texture list population in ImportGeometryFromFile**: Task 6 notes this should be done "once per TG_TypeMultiShape" but the `ImportShapeFromMesh` code only sets `localTextureHandle = mesh->mMaterialIndex`. The actual `listOfTextures` allocation and population (iterating `scene->mMaterials`, extracting `aiTextureType_DIFFUSE` filename) is not written. Add a `BuildTextureList(const aiScene*, TG_TypeMultiShape*)` helper call at the top of `ImportGeometryFromFile` before the per-mesh loop.

Both gaps should be addressed during implementation of Tasks 6 and 10 respectively.
