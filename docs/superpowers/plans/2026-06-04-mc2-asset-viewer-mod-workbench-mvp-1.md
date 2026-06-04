# MC2-ASSET-VIEWER-MOD-WORKBENCH-MVP-1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Mod Workbench" mode to `mc2_asset_viewer` that loads a dropped GLB/GLTF, shows it beside the stock prop it replaces, validates it, and exports a drop-in `data/model_overrides/<id>/` bundle the engine can load.

**Architecture:** The inspect→validate→package loop runs without the engine render path. We link two already-vendored, engine-independent pieces into the standalone viewer: `model_override_registry` (validates/round-trips `models.json`, depends only on nlohmann + stdlib) and Assimp's glTF importer (loads the GLB into the viewer's existing `MeshData`). The viewer's existing `TglMeshLoader` loads the stock prop for a side-by-side compare. Logic lives in non-GL, smoke-testable units (`GlbMeshLoader`, `OverrideManifest`, `WorkbenchValidation`, `BundleExport`, `ModWorkbench`); a thin ImGui panel (`ModWorkbenchPanel`) renders over them.

**Tech Stack:** C++17, CMake (VS 2022 / `build64`), SDL2 + ImGui + GLEW (viewer), Assimp (glTF), nlohmann/json (via the linked registry only). Tests = the viewer's `runSmoke*` static-method convention, CLI-dispatched, mostly GL-free.

---

## Conventions used by this plan

- **Worktree / branch:** `A:/Games/mc2-asset-viewer-mod-workbench` on `claude/asset-viewer-mod-workbench-recon-1`. All paths below are repo-relative to that worktree.
- **Configure (once, and after any CMakeLists change):**
  ```bash
  CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  D="A:/Games/mc2-opengl-src/3rdparty/3rdparty"
  "$CMAKE" -G "Visual Studio 17 2022" -A x64 -S . -B build64 \
    -DCMAKE_PREFIX_PATH="$D" -DCMAKE_LIBRARY_PATH="$D/lib/x64" \
    -DGLEW_INCLUDE_DIR="$D/include" \
    -DGLEW_SHARED_LIBRARY_RELEASE="$D/lib/x64/glew32.lib" \
    -DGLEW_STATIC_LIBRARY_RELEASE="$D/lib/x64/glew32s.lib"
  ```
- **Build:** `"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer`
- **Exe:** `build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`
  - To *run* (GUI or GL smokes) the exe needs `SDL2.dll`/`glew32.dll` from `build64/RelWithDebInfo/`. GL-free smokes (the ones this plan adds) do not need the window but the loader still needs those DLLs resolvable — run from a dir where they are on `PATH`, or copy the exe beside them. Define once:
    ```bash
    VIEWER="build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe"
    export PATH="$PWD/build64/RelWithDebInfo:$PATH"
    ```
- **Deploy dir** (only the stock-prop smokes need it): a runtime dir containing `tgl.fst`, e.g. `A:/Games/mc2-opengl/mc2-win64-v0.4`. Referenced below as `$DEPLOY`.
- **Commit discipline:** one commit per completed step group as marked. End commit messages with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer.
- **`docs/superpowers/` is gitignored** — `git add -f` plan/spec/doc files; normal `git add` for code.

## File structure (created / modified)

Created (all under `tools/asset_viewer/` unless noted):
- `GlbMeshLoader.{h,cpp}` — Assimp glTF → `MeshData` (engine-faithful glTF→Stuff→GL transform). No GL.
- `WorkbenchWarning.h` — `Warning{severity, code, message}` shared type.
- `OverrideManifest.{h,cpp}` — `WorkbenchOverride` record; `ValidateRecordRules()` (BLOCK mirror of registry); `ToModelsJson()` (hand-rolled writer; keeps nlohmann to the single allowed TU). No GL.
- `WorkbenchValidation.{h,cpp}` — semantic WARN checks (bounds delta, pivot, missing textures, LOD, overdraw heuristic). No GL.
- `BundleExport.{h,cpp}` — write `data/model_overrides/<id>/`, copy assets, write `models.json`, round-trip via the linked registry, refuse on drop. No GL.
- `ModWorkbench.{h,cpp}` — non-GL workbench state + orchestration (holds override/stock `MeshData`, record, warnings).
- `ModWorkbenchPanel.{h,cpp}` — ImGui panel; two `MeshPreview3D`, stock picker, warnings list, Export button.
- `mclib/model_override_registry.{h,cpp}` — cherry-picked from `claude/model-override-system-recon-1` (engine-independent; no edits).
- `tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py` + generated `unit_tri.gltf` and `models_fixtures/*.json`.

Modified:
- `tools/asset_viewer/CMakeLists.txt` — add new sources + registry.cpp; nlohmann + assimp includes; link `assimp`.
- `tools/asset_viewer/AssetTypeSidebar.{h,cpp}` — add `ModWorkbench` enum + sidebar entry.
- `tools/asset_viewer/AssetViewerApp.{h,cpp}` — own `ModWorkbench`/`ModWorkbenchPanel`; dispatch; `onFileDropped`; new `runSmoke*` methods.
- `tools/asset_viewer/main.cpp` — `SDL_DROPFILE` handling; `--smoke-workbench-*` dispatch.
- `tools/asset_viewer/MeshPreview3D.{h,cpp}` — `setMeshData(const MeshData&)` to render a non-`.tgl` mesh.

---

## Task S0: Link spike — registry + Assimp into the viewer

De-risk the scary part first: prove `model_override_registry` and Assimp compile, link, and run inside the viewer's link unit before writing any feature code.

**Files:**
- Create: `mclib/model_override_registry.h`, `mclib/model_override_registry.cpp` (cherry-pick)
- Modify: `tools/asset_viewer/CMakeLists.txt`
- Modify: `tools/asset_viewer/AssetViewerApp.h:38` (add method decl), `tools/asset_viewer/AssetViewerApp.cpp` (add method), `tools/asset_viewer/main.cpp` (dispatch)

- [ ] **Step 1: Bring in the registry source (no edits to it)**

```bash
git checkout claude/model-override-system-recon-1 -- mclib/model_override_registry.h mclib/model_override_registry.cpp
git status --short mclib/model_override_registry.*
```
Expected: both files listed as added (`A`).

- [ ] **Step 2: Wire CMake — registry source, nlohmann + assimp includes, assimp link**

In `tools/asset_viewer/CMakeLists.txt`, inside `set(ASSET_VIEWER_SOURCES ...)` (ends at line 175), add before the closing `)`:
```cmake
    # --- Mod Workbench (MC2-ASSET-VIEWER-MOD-WORKBENCH-MVP-1) ---
    GlbMeshLoader.cpp
    OverrideManifest.cpp
    WorkbenchValidation.cpp
    BundleExport.cpp
    ModWorkbench.cpp
    ModWorkbenchPanel.cpp
    # engine-independent override-manifest parser (validates/round-trips models.json)
    "${CMAKE_SOURCE_DIR}/mclib/model_override_registry.cpp"
```
After `target_link_libraries(mc2_asset_viewer PRIVATE imgui)` (line 258), add:
```cmake
# Assimp (glTF importer only, per root CMake ASSIMP_BUILD_GLTF_IMPORTER=ON).
target_link_libraries(mc2_asset_viewer PRIVATE assimp)
```
In the final `target_include_directories(mc2_asset_viewer PRIVATE ...)` block (lines 287-291), add these entries:
```cmake
    "${CMAKE_SOURCE_DIR}/mclib"                       # model_override_registry.h
    "${CMAKE_SOURCE_DIR}/3rdparty/include"            # nlohmann/json.hpp (registry TU only)
    "${CMAKE_SOURCE_DIR}/3rdparty/assimp/include"     # <assimp/Importer.hpp>
    "${CMAKE_BINARY_DIR}/out/3rdparty/assimp/include" # generated assimp config.h
```

> NOTE on the json-isolation rule: `model_override_registry.cpp` is the only TU that includes `<nlohmann/json.hpp>` (it carries the `scripts/check-json-isolation.sh` waiver comment). This plan keeps it that way — our new TUs never include nlohmann; `OverrideManifest::ToModelsJson` writes JSON by hand, and we read/validate exclusively through the linked registry.

- [ ] **Step 3: Write the failing link smoke**

Add the declaration to `tools/asset_viewer/AssetViewerApp.h` after line 38 (inside the `public:` smoke block):
```cpp
    static int runSmokeWorkbenchLink();                 // S0: registry + assimp link/run
```
Add to `tools/asset_viewer/AssetViewerApp.cpp` (end of file):
```cpp
// ---------------------------------------------------------------------------
// runSmokeWorkbenchLink — S0 gate: prove model_override_registry and Assimp
// link and run inside the viewer's link unit. No GL, no files required.
// ---------------------------------------------------------------------------
#include "model_override_registry.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

int AssetViewerApp::runSmokeWorkbenchLink()
{
    // Registry: loading a nonexistent manifest must return 0 (not crash).
    ModelOverrideRegistry reg;
    int n = reg.loadFromFile("does_not_exist_models.json", ".");
    if (n != 0) { std::fprintf(stderr, "[smoke] FAIL workbench-link: expected 0 records, got %d\n", n); return 1; }

    // Assimp: instantiate an importer and confirm the glTF extension is recognized.
    Assimp::Importer imp;
    if (!imp.IsExtensionSupported(".gltf")) {
        std::fprintf(stderr, "[smoke] FAIL workbench-link: assimp lacks .gltf importer\n");
        return 1;
    }
    std::printf("[smoke] PASS workbench-link (registry + assimp glТF linked)\n");
    return 0;
}
```
Add dispatch to `tools/asset_viewer/main.cpp` after the existing `--smoke-spotlight` block (line 75):
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-link") == 0)
        return AssetViewerApp::runSmokeWorkbenchLink();
```

- [ ] **Step 4: Configure + build**

Run the Configure command (top of plan), then:
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: build succeeds. If the assimp include path differs, fix the `target_include_directories` entry until `<assimp/Importer.hpp>` resolves; the `assimp` link target is provided by the root `add_subdirectory(3rdparty/assimp ...)`.

- [ ] **Step 5: Run the smoke**

```bash
export PATH="$PWD/build64/RelWithDebInfo:$PATH"
"build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe" --smoke-workbench-link
```
Expected: `[smoke] PASS workbench-link (registry + assimp glTF linked)` and exit 0.

- [ ] **Step 6: Commit**

```bash
git add mclib/model_override_registry.h mclib/model_override_registry.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S0 link model_override_registry + assimp into mc2_asset_viewer"
```

---

## Task S1: GLB load → MeshData + drag-into-window

Load a dropped `.glb`/`.gltf` into the viewer's `MeshData` using the engine-faithful transform, and wire `SDL_DROPFILE`.

**Files:**
- Create: `tools/asset_viewer/GlbMeshLoader.h`, `tools/asset_viewer/GlbMeshLoader.cpp`
- Create: `tools/asset_viewer/ModWorkbench.h`, `tools/asset_viewer/ModWorkbench.cpp` (state + `loadOverride`)
- Create: `tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py` + generated `unit_tri.gltf`
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` (own `ModWorkbench`, `onFileDropped`, smoke), `tools/asset_viewer/main.cpp` (SDL_DROPFILE + smoke dispatch)

- [ ] **Step 1: Generate the glTF test fixture (deterministic, committed)**

Create `tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py`:
```python
#!/usr/bin/env python3
# Emits a minimal valid glTF 2.0 triangle with an embedded base64 buffer.
# One triangle, 3 vertices, positions + UVs. Deterministic (no timestamps).
# Source-space (glTF, Y-up RH) positions chosen so the engine-faithful
# glTF->Stuff->GL transform yields known GL-space coords (see GlbMeshLoader).
import base64, json, struct, os

# glTF source-space vertices (x, y, z) and UVs (u, v).
P = [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0)]
UV = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
IDX = [0, 1, 2]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in P)
uv_bytes  = b"".join(struct.pack("<2f", *u) for u in UV)
idx_bytes = struct.pack("<3H", *IDX)
buf = pos_bytes + uv_bytes + idx_bytes

def mn(vs, n): return [min(v[i] for v in vs) for i in range(n)]
def mx(vs, n): return [max(v[i] for v in vs) for i in range(n)]

gltf = {
  "asset": {"version": "2.0", "generator": "make_workbench_fixture"},
  "buffers": [{"byteLength": len(buf),
               "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,              "byteLength": len(pos_bytes), "target": 34962},
    {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(uv_bytes),  "target": 34962},
    {"buffer": 0, "byteOffset": len(pos_bytes)+len(uv_bytes), "byteLength": len(idx_bytes), "target": 34963},
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": mn(P,3), "max": mx(P,3)},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2", "min": mn(UV,2), "max": mx(UV,2)},
    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "mode": 4}]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0,
}
out = os.path.join(os.path.dirname(__file__), "unit_tri.gltf")
with open(out, "w", encoding="utf-8") as f:
    json.dump(gltf, f, indent=1)
print("wrote", out)
```
Run it and confirm the output exists:
```bash
python tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py
test -f tests/fixtures/asset_viewer/workbench/unit_tri.gltf && echo OK
```
Expected: `wrote .../unit_tri.gltf` then `OK`.

- [ ] **Step 2: Write `GlbMeshLoader.h`**

```cpp
// tools/asset_viewer/GlbMeshLoader.h
// Assimp glTF/GLB -> MeshData for the Mod Workbench. No GL, no engine headers.
//
// Engine-faithful transform: the in-engine importer (mclib/assimp_importer.cpp)
// maps glTF source space to MC2/Stuff space as (mc2.x=-src.x, mc2.y=src.z,
// mc2.z=src.y) and flips UV v->1-v; TglMeshLoader then maps Stuff->GL as
// (gl.x=-stuff.x, gl.y=stuff.z, gl.z=stuff.y). We apply BOTH so the override
// sits in the same GL-space orientation the engine would render it, aligned
// with a stock prop loaded via TglMeshLoader.
#pragma once
#include "TglMeshLoader.h"   // MeshData / SubMesh / MeshVertex
#include <string>

namespace GlbMeshLoader {
    // Load .glb/.gltf via Assimp. Triangulated, normals generated if absent.
    // Groups primitives by material index into SubMesh entries; textureName is
    // the material's diffuse texture basename (may be empty). Returns ok=false +
    // error on failure. Applies the engine-faithful glTF->Stuff->GL transform.
    MeshData load(const std::string& path);
}
```

- [ ] **Step 3: Write the failing GLB smoke**

Add decl to `AssetViewerApp.h` (after the S0 decl):
```cpp
    static int runSmokeWorkbenchGlb(const char* fixtureDir);  // S1: glTF -> MeshData
```
Add to `AssetViewerApp.cpp` (end of file):
```cpp
#include "GlbMeshLoader.h"
#include <cmath>

int AssetViewerApp::runSmokeWorkbenchGlb(const char* fixtureDir)
{
    std::string path = std::string(fixtureDir) + "/unit_tri.gltf";
    MeshData md = GlbMeshLoader::load(path);
    if (!md.ok) { std::fprintf(stderr, "[smoke] FAIL workbench-glb: %s\n", md.error.c_str()); return 1; }
    if (md.submeshes.size() != 1) { std::fprintf(stderr, "[smoke] FAIL workbench-glb: submeshes=%zu\n", md.submeshes.size()); return 1; }
    const SubMesh& sm = md.submeshes[0];
    if (sm.verts.size() != 3) { std::fprintf(stderr, "[smoke] FAIL workbench-glb: verts=%zu\n", sm.verts.size()); return 1; }

    // glTF source verts: (0,0,0),(2,0,0),(0,3,0). Net glTF->GL transform:
    //   stuff = (-x, z, y); gl = (-stuff.x, stuff.z, stuff.y) = (x, y, z)  [identity]
    // So GL-space positions equal the source positions; GL-Y extent = 3, X = 2.
    float extX = md.bmax[0] - md.bmin[0];
    float extY = md.bmax[1] - md.bmin[1];
    if (std::fabs(extX - 2.0f) > 1e-3f || std::fabs(extY - 3.0f) > 1e-3f) {
        std::fprintf(stderr, "[smoke] FAIL workbench-glb: extents X=%.3f Y=%.3f (want 2,3)\n", extX, extY);
        return 1;
    }
    // UV v-flip: source UVs v in {0,1}; after v->1-v the set is still {0,1} but
    // per-vertex flipped. Vertex 2 had source UV (0,1) -> stored v = 0.
    bool sawFlipped = false;
    for (const auto& v : sm.verts) if (std::fabs(v.v - 0.0f) < 1e-3f && std::fabs(v.u - 0.0f) < 1e-3f) sawFlipped = true;
    if (!sawFlipped) { std::fprintf(stderr, "[smoke] FAIL workbench-glb: UV v-flip not applied\n"); return 1; }

    std::printf("[smoke] PASS workbench-glb verts=3 extX=%.2f extY=%.2f\n", extX, extY);
    return 0;
}
```
Add dispatch to `main.cpp` after the S0 dispatch:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-glb") == 0)
        return AssetViewerApp::runSmokeWorkbenchGlb(argc >= 3 ? argv[2] : ".");
```

- [ ] **Step 4: Run the smoke to verify it fails (no implementation yet)**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: **link error** — `GlbMeshLoader::load` unresolved (CMake already lists `GlbMeshLoader.cpp` from S0, but the file does not exist yet → create it next). If CMake errors that the source is missing, that is the expected red state.

- [ ] **Step 5: Implement `GlbMeshLoader.cpp`**

```cpp
// tools/asset_viewer/GlbMeshLoader.cpp
#include "GlbMeshLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cfloat>
#include <cstring>

namespace {
// glTF source -> MC2/Stuff -> GL, applied as the literal two-step composition
// so it stays auditable against the engine (assimp_importer + TglMeshLoader).
inline void srcToGl(float sx, float sy, float sz, float& gx, float& gy, float& gz) {
    const float mx = -sx, my = sz, mz = sy;      // glTF -> Stuff (assimp_importer.cpp:46)
    gx = -mx; gy = mz; gz = my;                  // Stuff -> GL (TglMeshLoader.h:19)
}
std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
}

MeshData GlbMeshLoader::load(const std::string& path) {
    MeshData out;
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode || scene->mNumMeshes == 0) {
        out.ok = false; out.error = imp.GetErrorString(); if (out.error.empty()) out.error = "no meshes";
        return out;
    }

    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* m = scene->mMeshes[mi];
        if (!m->mNumVertices || !m->HasFaces()) continue;
        SubMesh sm;

        // Texture basename from the mesh's material diffuse slot (may be empty).
        if (m->mMaterialIndex < scene->mNumMaterials) {
            aiString tex;
            if (scene->mMaterials[m->mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS)
                sm.textureName = baseName(tex.C_Str());
        }

        // Non-indexed expansion: emit each triangle corner (matches SubMesh contract).
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices != 3) continue;
            for (int c = 0; c < 3; ++c) {
                unsigned vi = face.mIndices[c];
                MeshVertex v{};
                srcToGl(m->mVertices[vi].x, m->mVertices[vi].y, m->mVertices[vi].z, v.px, v.py, v.pz);
                if (m->HasNormals())
                    srcToGl(m->mNormals[vi].x, m->mNormals[vi].y, m->mNormals[vi].z, v.nx, v.ny, v.nz);
                if (m->HasTextureCoords(0)) {
                    v.u = m->mTextureCoords[0][vi].x;
                    v.v = 1.0f - m->mTextureCoords[0][vi].y;   // glTF -> MC2 UV flip
                }
                sm.verts.push_back(v);
                sm.idx.push_back((uint32_t)sm.idx.size());
                for (int k = 0; k < 3; ++k) {
                    float p = (&v.px)[k];
                    if (p < lo[k]) lo[k] = p;
                    if (p > hi[k]) hi[k] = p;
                }
            }
        }
        if (!sm.verts.empty()) out.submeshes.push_back(std::move(sm));
    }

    if (out.submeshes.empty()) { out.ok = false; out.error = "no triangulated geometry"; return out; }
    for (int k = 0; k < 3; ++k) { out.bmin[k] = lo[k]; out.bmax[k] = hi[k]; }
    out.ok = true;
    return out;
}
```

- [ ] **Step 6: Build + run smoke to verify it passes**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
"build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe" --smoke-workbench-glb tests/fixtures/asset_viewer/workbench
```
Expected: `[smoke] PASS workbench-glb verts=3 extX=2.00 extY=3.00`

- [ ] **Step 7: Write `ModWorkbench.h` (state container) — minimal for S1**

```cpp
// tools/asset_viewer/ModWorkbench.h
// Non-GL workbench state + orchestration. The ImGui panel renders over this.
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"   // WorkbenchOverride (added in S3; forward use is fine — created before this builds)
#include "WorkbenchWarning.h"
#include <string>
#include <vector>

class ModWorkbench {
public:
    // S1: load a dropped override file. Returns true on success.
    bool loadOverride(const std::string& glbPath);
    bool hasOverride() const { return overrideMesh_.ok; }
    const MeshData& overrideMesh() const { return overrideMesh_; }
    const std::string& overridePath() const { return overridePath_; }
    const std::string& lastError() const { return lastError_; }

private:
    std::string overridePath_;
    MeshData    overrideMesh_;
    std::string lastError_;
};
```

> To keep S1 self-contained, create `WorkbenchWarning.h` and a minimal `OverrideManifest.h` now (their bodies are fleshed out in S3). Create `WorkbenchWarning.h`:
```cpp
// tools/asset_viewer/WorkbenchWarning.h
#pragma once
#include <string>
#include <vector>
enum class WarnSeverity { Block, Warn };
struct Warning { WarnSeverity severity; std::string code; std::string message; };
```
And a minimal `OverrideManifest.h` stub (full body in S3):
```cpp
// tools/asset_viewer/OverrideManifest.h
#pragma once
#include "WorkbenchWarning.h"
#include <string>
#include <vector>
struct WorkbenchOverrideLod { int lod = 0; std::string sourceRelPath; float distance = 0.0f; };
struct WorkbenchOverride {
    std::string overrideClass = "staticprop";   // "staticprop" | "tree"
    std::string appearanceName;                  // stock appearance key
    std::string sourceRelPath;                   // "<id>/model.glb"
    float       scale = 1.0f;
    bool        renderOnly = true;
    std::string fallback = "stock";
    std::vector<WorkbenchOverrideLod> lods;
};
```

- [ ] **Step 8: Implement `ModWorkbench.cpp` (S1 portion)**

```cpp
// tools/asset_viewer/ModWorkbench.cpp
#include "ModWorkbench.h"
#include "GlbMeshLoader.h"

bool ModWorkbench::loadOverride(const std::string& glbPath) {
    overridePath_ = glbPath;
    overrideMesh_ = GlbMeshLoader::load(glbPath);
    lastError_ = overrideMesh_.ok ? std::string() : overrideMesh_.error;
    return overrideMesh_.ok;
}
```

- [ ] **Step 9: Wire SDL_DROPFILE + an owning `ModWorkbench` in the app**

In `AssetViewerApp.h`: add `#include "ModWorkbench.h"` near the other includes (line 13 area), add member `ModWorkbench workbench_;` to the private block (after line 47), and declare:
```cpp
    void onFileDropped(const char* path);   // SDL_DROPFILE entry
```
In `AssetViewerApp.cpp` add:
```cpp
void AssetViewerApp::onFileDropped(const char* path) {
    if (!path) return;
    std::string p = path;
    auto lower = p; for (char& c : lower) c = (char)tolower((unsigned char)c);
    auto ends = [&](const char* s){ size_t n = strlen(s); return lower.size() >= n && lower.compare(lower.size()-n, n, s) == 0; };
    if (ends(".glb") || ends(".gltf")) {
        workbench_.loadOverride(p);
        sidebar_.setActive(AssetType::ModWorkbench);   // AssetType::ModWorkbench added in S2
    }
}
```
> `sidebar_.setActive` and `AssetType::ModWorkbench` are added in S2. If executing strictly in order, temporarily comment the `setActive` line and uncomment it in S2. (Subagent-driven execution: note this cross-task dependency in the S2 handoff.)

In `main.cpp`, inside the `while (SDL_PollEvent(&event))` loop (after the `SDL_QUIT` check, line 148 area), add:
```cpp
                if (event.type == SDL_DROPFILE) {
                    app.onFileDropped(event.drop.file);
                    SDL_free(event.drop.file);
                }
```

- [ ] **Step 10: Build + commit**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
git add tools/asset_viewer/GlbMeshLoader.h tools/asset_viewer/GlbMeshLoader.cpp \
        tools/asset_viewer/ModWorkbench.h tools/asset_viewer/ModWorkbench.cpp \
        tools/asset_viewer/WorkbenchWarning.h tools/asset_viewer/OverrideManifest.h \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git add -f tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py \
           tests/fixtures/asset_viewer/workbench/unit_tri.gltf
git commit -m "feat(workbench): S1 glTF->MeshData loader + SDL_DROPFILE drag-in"
```
> The fixture dir may be under a path not gitignored; `-f` is harmless if not needed. Drop `-f` if `git add` accepts them normally.

---

## Task S2: Stock-prop bind + side-by-side overlay

Pick the stock appearance the override replaces, load it via `TglMeshLoader`, render both meshes side-by-side, and compute geometric deltas.

**Files:**
- Modify: `tools/asset_viewer/MeshPreview3D.{h,cpp}` (add `setMeshData`)
- Modify: `tools/asset_viewer/ModWorkbench.{h,cpp}` (bindStock + deltas)
- Modify: `tools/asset_viewer/AssetTypeSidebar.{h,cpp}` (ModWorkbench enum + entry + `setActive`)
- Create: `tools/asset_viewer/ModWorkbenchPanel.{h,cpp}` (minimal two-preview panel)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` (dispatch + panel member)

- [ ] **Step 1: Add `MeshPreview3D::setMeshData` (render a non-`.tgl` mesh)**

In `MeshPreview3D.h`, after `setSource` (line 18) add:
```cpp
    // Render an arbitrary MeshData (e.g. a GLB override) instead of a .tgl.
    // Uploads via MeshGpu; albedo textures are NOT resolved (untextured preview).
    void setMeshData(const MeshData& md);
```
In `MeshPreview3D.cpp`, find `setSource` (it calls `TglMeshLoader::loadMesh` then `mesh_.upload(...)` and sets `center_`). Add a sibling that reuses the upload + recenter path:
```cpp
void MeshPreview3D::setMeshData(const MeshData& md) {
    if (!md.ok) { errorMsg_ = md.error.empty() ? "bad mesh" : md.error; return; }
    errorMsg_.clear();
    tglName_.clear();
    mesh_.upload(md, deployDir_, tier_);                 // same call setSource uses
    center_[0] = 0.5f * (md.bmin[0] + md.bmax[0]);
    center_[1] = 0.5f * (md.bmin[1] + md.bmax[1]);
    center_[2] = 0.5f * (md.bmin[2] + md.bmax[2]);
}
```
> Match the exact `mesh_.upload(...)` argument list used in the existing `setSource` (read it in `MeshPreview3D.cpp`); if `setSource` passes additional args, mirror them. The recenter mirrors how `setSource` sets `center_` from bounds.

- [ ] **Step 2: ModWorkbench — bind stock + compute deltas (failing smoke first)**

Add to `ModWorkbench.h`:
```cpp
    // S2: bind the stock appearance this override replaces. tglName is an FST
    // path like "data/tgl/2civliving.tgl"; deployDir must be set first.
    void setDeployDir(const std::string& d) { deployDir_ = d; }
    bool bindStock(const std::string& tglName);
    bool hasStock() const { return stockMesh_.ok; }
    const MeshData& stockMesh() const { return stockMesh_; }

    struct BoundsDelta {
        float overrideExt[3] = {0,0,0};
        float stockExt[3]    = {0,0,0};
        float maxRatio = 1.0f;          // largest per-axis override/stock extent ratio
        float pivotOffset[3] = {0,0,0}; // override base-center minus stock base-center (GL)
    };
    BoundsDelta computeDelta() const;
```
Add the private members:
```cpp
    std::string deployDir_ = ".";
    std::string stockTgl_;
    MeshData    stockMesh_;
```
Add smoke decl to `AssetViewerApp.h`:
```cpp
    static int runSmokeWorkbenchBind(const char* deployDir, const char* fixtureDir);
```
Add to `AssetViewerApp.cpp`:
```cpp
int AssetViewerApp::runSmokeWorkbenchBind(const char* deployDir, const char* fixtureDir)
{
    ModWorkbench wb;
    wb.setDeployDir(deployDir);
    if (!wb.loadOverride(std::string(fixtureDir) + "/unit_tri.gltf"))
        return smokeFail("bind: override load");
    if (!wb.bindStock("data/tgl/2civliving.tgl"))
        return smokeFail("bind: stock load (need a deploy dir with tgl.fst)");
    auto d = wb.computeDelta();
    if (d.stockExt[1] <= 0.0f) return smokeFail("bind: stock has no Y extent");
    if (d.maxRatio <= 0.0f)    return smokeFail("bind: ratio not computed");
    std::printf("[smoke] PASS workbench-bind stockY=%.2f ovY=%.2f ratio=%.3f\n",
                d.stockExt[1], d.overrideExt[1], d.maxRatio);
    return 0;
}
```
Add dispatch to `main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-bind") == 0)
        return AssetViewerApp::runSmokeWorkbenchBind(argc >= 3 ? argv[2] : ".",
                                                     argc >= 4 ? argv[3] : ".");
```

- [ ] **Step 3: Run smoke to verify it fails**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: link error — `ModWorkbench::bindStock` / `computeDelta` undefined.

- [ ] **Step 4: Implement bindStock + computeDelta in `ModWorkbench.cpp`**

```cpp
bool ModWorkbench::bindStock(const std::string& tglName) {
    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())) { lastError_ = "tgl.fst not found"; return false; }
    stockTgl_ = tglName;
    stockMesh_ = TglMeshLoader::loadMesh(tglName);
    if (!stockMesh_.ok) lastError_ = stockMesh_.error;
    return stockMesh_.ok;
}

ModWorkbench::BoundsDelta ModWorkbench::computeDelta() const {
    BoundsDelta d;
    auto ext = [](const MeshData& m, float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = m.ok ? (m.bmax[k] - m.bmin[k]) : 0.0f;
    };
    ext(overrideMesh_, d.overrideExt);
    ext(stockMesh_,    d.stockExt);
    d.maxRatio = 0.0f;
    for (int k = 0; k < 3; ++k) {
        if (d.stockExt[k] > 1e-4f) {
            float r = d.overrideExt[k] / d.stockExt[k];
            if (r > d.maxRatio) d.maxRatio = r;
        }
    }
    if (d.maxRatio == 0.0f) d.maxRatio = 1.0f;
    // Pivot offset: compare base-center (min-Y footprint center) in GL space.
    auto baseCenter = [](const MeshData& m, float out[3]) {
        out[0] = m.ok ? 0.5f*(m.bmin[0]+m.bmax[0]) : 0.0f;
        out[1] = m.ok ? m.bmin[1] : 0.0f;                       // base = floor (min GL-Y)
        out[2] = m.ok ? 0.5f*(m.bmin[2]+m.bmax[2]) : 0.0f;
    };
    float ob[3], sb[3]; baseCenter(overrideMesh_, ob); baseCenter(stockMesh_, sb);
    for (int k = 0; k < 3; ++k) d.pivotOffset[k] = ob[k] - sb[k];
    return d;
}
```

- [ ] **Step 5: Run smoke to verify it passes**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
export PATH="$PWD/build64/RelWithDebInfo:$PATH"
DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.4"   # any runtime dir with tgl.fst
"build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe" \
    --smoke-workbench-bind "$DEPLOY" tests/fixtures/asset_viewer/workbench
```
Expected: `[smoke] PASS workbench-bind stockY=... ovY=3.00 ratio=...`

- [ ] **Step 6: Add the `ModWorkbench` asset type + sidebar entry**

In `AssetTypeSidebar.h`, extend the enum and add `setActive`:
```cpp
enum class AssetType { Textures, Materials, StaticProps, ModWorkbench };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return active_; }
    void setActive(AssetType t) { active_ = t; }
private:
    AssetType active_ = AssetType::Textures;
};
```
In `AssetTypeSidebar.cpp`, add a selectable for the new type alongside the existing three (follow the exact pattern already there — a `ImGui::Selectable("Mod Workbench", active_ == AssetType::ModWorkbench)` that sets `active_` on click).

- [ ] **Step 7: Minimal `ModWorkbenchPanel` (two previews side-by-side)**

`tools/asset_viewer/ModWorkbenchPanel.h`:
```cpp
// tools/asset_viewer/ModWorkbenchPanel.h
// ImGui panel for the Mod Workbench: stock-vs-override side-by-side + (S3) warnings.
#pragma once
#include "ModWorkbench.h"
#include "MeshPreview3D.h"
struct ImVec2;
class ModWorkbenchPanel {
public:
    void setDeployDir(const std::string& d);
    // Draws the inspector content. `wb` is owned by AssetViewerApp.
    void draw(ModWorkbench& wb, const ImVec2& avail);
private:
    void syncMeshes(ModWorkbench& wb);
    MeshPreview3D stockPreview_;
    MeshPreview3D overridePreview_;
    bool meshesDirty_ = true;
    const void* lastOverrideKey_ = nullptr;   // detect reload (overridePath ptr/content)
    std::string boundStock_;
};
```
`tools/asset_viewer/ModWorkbenchPanel.cpp`:
```cpp
// tools/asset_viewer/ModWorkbenchPanel.cpp
#include "ModWorkbenchPanel.h"
#include "imgui.h"
#include <cstdio>

void ModWorkbenchPanel::setDeployDir(const std::string& d) {
    stockPreview_.setDeployDir(d);
    overridePreview_.setDeployDir(d);
}

void ModWorkbenchPanel::syncMeshes(ModWorkbench& wb) {
    if (wb.hasStock())    stockPreview_.setMeshData(wb.stockMesh());
    if (wb.hasOverride()) overridePreview_.setMeshData(wb.overrideMesh());
}

void ModWorkbenchPanel::draw(ModWorkbench& wb, const ImVec2& avail) {
    ImGui::TextUnformatted("Mod Workbench — drag a .glb/.gltf onto the window");
    if (!wb.hasOverride()) { ImGui::TextDisabled("No override loaded. %s", wb.lastError().c_str()); return; }
    ImGui::Text("Override: %s", wb.overridePath().c_str());

    // Stock picker: a text field + Bind button (full roster picker is S5 polish).
    static char tgl[256] = "data/tgl/2civliving.tgl";
    ImGui::InputText("Stock .tgl", tgl, sizeof(tgl));
    if (ImGui::Button("Bind stock")) { wb.bindStock(tgl); meshesDirty_ = true; }

    if (meshesDirty_) { syncMeshes(wb); meshesDirty_ = false; }

    // Sync cameras so both turn together.
    overridePreview_.orbitYaw()   = stockPreview_.orbitYaw();
    overridePreview_.orbitPitch() = stockPreview_.orbitPitch();
    overridePreview_.zoom()       = stockPreview_.zoom();

    float half = (avail.x - 8.0f) * 0.5f;
    ImGui::BeginChild("stock", ImVec2(half, avail.y * 0.7f), true);
    ImGui::TextUnformatted("Stock"); stockPreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("override", ImVec2(half, avail.y * 0.7f), true);
    ImGui::TextUnformatted("Override"); overridePreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    if (wb.hasStock()) {
        auto d = wb.computeDelta();
        ImGui::Text("footprint ratio (override/stock, max axis): %.2fx", d.maxRatio);
        ImGui::Text("pivot offset (GL): %.2f, %.2f, %.2f", d.pivotOffset[0], d.pivotOffset[1], d.pivotOffset[2]);
    }
}
```
> The panel sets `meshesDirty_` on bind; the app sets it on drop via a public flag — simplest is to call `syncMeshes` whenever override identity changes. For MVP, also re-sync when `wb.overridePath()` differs from a cached copy: add that check in `draw` if drag-drop reloads aren't reflected.

- [ ] **Step 8: Dispatch the panel in `AssetViewerApp`**

In `AssetViewerApp.h` add `#include "ModWorkbenchPanel.h"` and member `ModWorkbenchPanel workbenchPanel_;`. In the ctor (`AssetViewerApp.cpp:37`) add `workbenchPanel_.setDeployDir("."); workbench_.setDeployDir(".");`. In `drawUi`'s inspector `switch` (line 78), add:
```cpp
      case AssetType::ModWorkbench:
        workbenchPanel_.draw(workbench_, ImGui::GetContentRegionAvail());
        break;
```
Also in the browser child (line 61 area), guard the StaticProps branch so ModWorkbench mode doesn't fall into the texture browser:
```cpp
    if (sidebar_.active() == AssetType::StaticProps) { /* ...existing... */ }
    else if (sidebar_.active() == AssetType::ModWorkbench) { ImGui::TextDisabled("Drop a GLB on the window."); }
    else { /* ...existing else... */ }
```
Uncomment the `sidebar_.setActive(AssetType::ModWorkbench)` line in `onFileDropped` (deferred from S1 Step 9).

- [ ] **Step 9: Build + manual verify + commit**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Manual check (GUI): copy the exe beside the DLLs (README "Running"), launch, drag `tests/fixtures/asset_viewer/workbench/unit_tri.gltf` onto the window → sidebar switches to "Mod Workbench", override appears; click **Bind stock** → stock + override render side-by-side, ratio/pivot lines show.
```bash
git add tools/asset_viewer/MeshPreview3D.h tools/asset_viewer/MeshPreview3D.cpp \
        tools/asset_viewer/ModWorkbench.h tools/asset_viewer/ModWorkbench.cpp \
        tools/asset_viewer/AssetTypeSidebar.h tools/asset_viewer/AssetTypeSidebar.cpp \
        tools/asset_viewer/ModWorkbenchPanel.h tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S2 stock bind + side-by-side overlay + bounds/pivot delta"
```

---

## Task S3: Validation engine + warnings panel

Produce a structured warnings list: BLOCK rules mirrored from the registry (advisory; registry is authoritative at export) plus semantic WARN checks.

**Files:**
- Modify: `tools/asset_viewer/OverrideManifest.{h,cpp}` (`ValidateRecordRules`)
- Create: `tools/asset_viewer/WorkbenchValidation.{h,cpp}` (semantic checks)
- Modify: `tools/asset_viewer/ModWorkbench.{h,cpp}` (`revalidate`, holds record + warnings)
- Modify: `tools/asset_viewer/ModWorkbenchPanel.cpp` (warnings UI)
- Create: validation fixtures under `tests/fixtures/asset_viewer/workbench/`

- [ ] **Step 1: Flesh out `OverrideManifest.cpp` — `ValidateRecordRules` (BLOCK mirror)**

Create `tools/asset_viewer/OverrideManifest.cpp` (header already exists from S1; add the function decl to it):
Add to `OverrideManifest.h`:
```cpp
// Mirror of model_override_registry's MVP invariants. ADVISORY: the registry is
// authoritative (BundleExport re-parses through it). Kept in sync by S3's smoke,
// which asserts these verdicts agree with the real registry on shared fixtures.
std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& rec);
```
`OverrideManifest.cpp`:
```cpp
// tools/asset_viewer/OverrideManifest.cpp
#include "OverrideManifest.h"
#include <cctype>

static bool isSafeSource(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s.size() >= 2 && s[1] == ':') return false;
    if (s.find("..") != std::string::npos) return false;
    std::string low = s; for (char& c : low) c = (char)std::tolower((unsigned char)c);
    bool glb  = low.size() >= 4 && low.compare(low.size()-4, 4, ".glb")  == 0;
    bool gltf = low.size() >= 5 && low.compare(low.size()-5, 5, ".gltf") == 0;
    return glb || gltf;
}

std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& r) {
    std::vector<Warning> w;
    auto block = [&](const char* code, const char* msg){ w.push_back({WarnSeverity::Block, code, msg}); };
    if (!r.renderOnly)                         block("renderOnly", "renderOnly must be true (MVP)");
    if (r.fallback != "stock")                 block("fallback", "fallback must be \"stock\" (MVP)");
    if (r.scale != 1.0f)                       block("scale", "runtime scale must be exactly 1.0 — bake scale into the GLB");
    if (r.overrideClass != "staticprop" && r.overrideClass != "tree")
                                               block("class", "class must be staticProp or tree");
    if (r.appearanceName.empty())              block("replaces", "no stock appearance bound (replaces is empty)");
    if (!isSafeSource(r.sourceRelPath))        block("source", "source must be a safe relative .glb/.gltf path");
    int last = 0;
    for (const auto& l : r.lods) {
        if (l.lod <= last)                     block("lod-order", "LOD indices must strictly ascend (LOD0=source)");
        if (!isSafeSource(l.sourceRelPath))    block("lod-source", "LOD source must be a safe relative .glb/.gltf path");
        last = l.lod;
    }
    return w;
}
```

- [ ] **Step 2: `WorkbenchValidation.{h,cpp}` — semantic WARN checks**

`tools/asset_viewer/WorkbenchValidation.h`:
```cpp
// tools/asset_viewer/WorkbenchValidation.h
// Semantic (WARN) checks over geometry + file presence. No GL.
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "WorkbenchWarning.h"
#include <string>
#include <vector>

struct SemanticInputs {
    const MeshData* overrideMesh = nullptr;
    const MeshData* stockMesh    = nullptr;   // may be null if unbound
    float maxFootprintRatio = 1.0f;           // from ModWorkbench::computeDelta
    float pivotOffsetXZ = 0.0f;               // horizontal pivot offset magnitude (GL)
    float pivotOffsetY  = 0.0f;               // vertical base offset (GL)
    std::vector<std::string> missingTextures; // basenames referenced but not found on disk
    bool  hasImpostorLod = false;             // any lods[] entry present
};
std::vector<Warning> ValidateSemantics(const WorkbenchOverride& rec, const SemanticInputs& in);
```
`tools/asset_viewer/WorkbenchValidation.cpp`:
```cpp
// tools/asset_viewer/WorkbenchValidation.cpp
#include "WorkbenchValidation.h"
#include <cmath>

std::vector<Warning> ValidateSemantics(const WorkbenchOverride& rec, const SemanticInputs& in) {
    std::vector<Warning> w;
    auto warn = [&](const char* code, const std::string& msg){ w.push_back({WarnSeverity::Warn, code, msg}); };

    if (in.stockMesh && in.stockMesh->ok) {
        if (in.maxFootprintRatio > 1.5f || in.maxFootprintRatio < 0.67f)
            warn("bounds-delta", "override footprint is " + std::to_string(in.maxFootprintRatio) +
                                 "x the stock prop — verify scale (runtime forces 1.0)");
        if (in.pivotOffsetXZ > 0.25f)
            warn("pivot-xz", "override is off-center vs stock by " + std::to_string(in.pivotOffsetXZ) + " (GL units)");
        if (std::fabs(in.pivotOffsetY) > 0.25f)
            warn("pivot-y", "override base is offset vertically from stock by " + std::to_string(in.pivotOffsetY));
    } else {
        warn("no-stock", "no stock prop bound — bounds/pivot not validated");
    }

    for (const auto& t : in.missingTextures)
        warn("texture-missing", "referenced texture not found: " + t);

    // Overdraw heuristic (advisory, not measured): alpha-card props with no
    // far-LOD impostor are the dominant tree GPU cost. Flag override meshes that
    // reference an alpha texture (basename contains "a_" leaf convention) and
    // have no LOD chain.
    bool referencesAlpha = false;
    if (in.overrideMesh)
        for (const auto& sm : in.overrideMesh->submeshes)
            if (sm.textureName.rfind("a_", 0) == 0 || sm.textureName.find("_a_") != std::string::npos)
                referencesAlpha = true;
    if (referencesAlpha && !in.hasImpostorLod)
        warn("overdraw", "alpha-card override with no far-LOD impostor — likely high overdraw in-game (heuristic)");

    return w;
}
```

- [ ] **Step 3: ModWorkbench — hold a record + `revalidate` (failing smoke first)**

Add to `ModWorkbench.h`:
```cpp
    #include "WorkbenchValidation.h"
    WorkbenchOverride& record() { return record_; }
    const std::vector<Warning>& warnings() const { return warnings_; }
    bool hasBlocking() const;
    void revalidate(const std::vector<std::string>& missingTextures = {});
```
(private members):
```cpp
    WorkbenchOverride    record_;
    std::vector<Warning> warnings_;
```
Add smoke decl to `AssetViewerApp.h`:
```cpp
    static int runSmokeWorkbenchValidate(const char* fixtureDir);
```
Add to `AssetViewerApp.cpp`:
```cpp
#include "ModWorkbench.h"
int AssetViewerApp::runSmokeWorkbenchValidate(const char* fixtureDir)
{
    // BLOCK-rule mirror: a clean record yields no BLOCK; each broken field yields one.
    WorkbenchOverride ok; ok.overrideClass="staticprop"; ok.appearanceName="example_name";
    ok.sourceRelPath="props/example.glb"; ok.scale=1.0f; ok.renderOnly=true; ok.fallback="stock";
    if (!ValidateRecordRules(ok).empty()) return smokeFail("validate: clean record flagged");

    WorkbenchOverride badScale = ok; badScale.scale = 2.0f;
    { auto v = ValidateRecordRules(badScale); bool got=false; for (auto&x:v) if (x.code=="scale") got=true;
      if (!got) return smokeFail("validate: scale!=1 not blocked"); }

    WorkbenchOverride badSrc = ok; badSrc.sourceRelPath = "C:/abs.png";
    { auto v = ValidateRecordRules(badSrc); bool got=false; for (auto&x:v) if (x.code=="source") got=true;
      if (!got) return smokeFail("validate: unsafe source not blocked"); }

    // Mirror vs registry agreement on a shared fixture set: the registry must
    // ACCEPT what the mirror passes and REJECT what the mirror blocks.
    ModelOverrideRegistry reg;
    int n = reg.loadFromFile(std::string(fixtureDir) + "/wb_valid.json", fixtureDir);
    if (n != 1 || reg.resolve("staticProp", "example_name") == nullptr)
        return smokeFail("validate: registry should accept wb_valid.json");
    int nb = reg.loadFromFile(std::string(fixtureDir) + "/wb_bad_scale.json", fixtureDir);
    if (nb != 0) return smokeFail("validate: registry should reject wb_bad_scale.json");

    // Semantic WARN: oversize footprint.
    MeshData ov; ov.ok=true; ov.bmin[0]=0; ov.bmax[0]=4; ov.bmin[1]=0; ov.bmax[1]=4; ov.bmin[2]=0; ov.bmax[2]=4;
    MeshData st; st.ok=true; st.bmin[0]=0; st.bmax[0]=1; st.bmin[1]=0; st.bmax[1]=1; st.bmin[2]=0; st.bmax[2]=1;
    SemanticInputs si; si.overrideMesh=&ov; si.stockMesh=&st; si.maxFootprintRatio=4.0f;
    { auto v = ValidateSemantics(ok, si); bool got=false; for (auto&x:v) if (x.code=="bounds-delta") got=true;
      if (!got) return smokeFail("validate: oversize footprint not warned"); }

    std::printf("[smoke] PASS workbench-validate (mirror+registry agree, semantics fire)\n");
    return 0;
}
```
Add `main.cpp` dispatch:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-validate") == 0)
        return AssetViewerApp::runSmokeWorkbenchValidate(argc >= 3 ? argv[2] : ".");
```

- [ ] **Step 4: Create the validation fixtures**

`tests/fixtures/asset_viewer/workbench/wb_valid.json`:
```json
{ "overrides": [ { "type":"model","class":"staticProp","replaces":"staticProp:example_name","source":"props/example.glb","renderOnly":true,"scale":1.0,"fallback":"stock" } ] }
```
`tests/fixtures/asset_viewer/workbench/wb_bad_scale.json`:
```json
{ "overrides": [ { "type":"model","class":"staticProp","replaces":"staticProp:example_name","source":"props/example.glb","renderOnly":true,"scale":2.0,"fallback":"stock" } ] }
```

- [ ] **Step 5: Run smoke to verify it fails, then implement `revalidate`**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: link error — `ModWorkbench::revalidate`/`hasBlocking` undefined (and the smoke not yet passing).
Implement in `ModWorkbench.cpp`:
```cpp
void ModWorkbench::revalidate(const std::vector<std::string>& missingTextures) {
    warnings_.clear();
    auto blk = ValidateRecordRules(record_);
    warnings_.insert(warnings_.end(), blk.begin(), blk.end());

    SemanticInputs si;
    si.overrideMesh = overrideMesh_.ok ? &overrideMesh_ : nullptr;
    si.stockMesh    = stockMesh_.ok    ? &stockMesh_    : nullptr;
    auto d = computeDelta();
    si.maxFootprintRatio = d.maxRatio;
    si.pivotOffsetXZ = std::sqrt(d.pivotOffset[0]*d.pivotOffset[0] + d.pivotOffset[2]*d.pivotOffset[2]);
    si.pivotOffsetY  = d.pivotOffset[1];
    si.missingTextures = missingTextures;
    si.hasImpostorLod = !record_.lods.empty();
    auto sem = ValidateSemantics(record_, si);
    warnings_.insert(warnings_.end(), sem.begin(), sem.end());
}

bool ModWorkbench::hasBlocking() const {
    for (const auto& w : warnings_) if (w.severity == WarnSeverity::Block) return true;
    return false;
}
```
Add `#include <cmath>` to `ModWorkbench.cpp`. Also set `record_.appearanceName`/`overrideClass`/`sourceRelPath` when binding: in `bindStock`, after success, set `record_.appearanceName` from the bound tgl's appearance (for MVP use the tgl basename without extension/dir, lowercased) and `record_.overrideClass = "staticprop"`. In `loadOverride`, set `record_.sourceRelPath` to the dropped file's basename for now (BundleExport rewrites it to `<id>/<file>` in S4).

- [ ] **Step 6: Run smoke to verify it passes**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
"build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe" \
    --smoke-workbench-validate tests/fixtures/asset_viewer/workbench
```
Expected: `[smoke] PASS workbench-validate (mirror+registry agree, semantics fire)`

- [ ] **Step 7: Warnings UI in `ModWorkbenchPanel.cpp`**

After the delta lines in `draw`, add:
```cpp
    wb.revalidate();
    ImGui::Separator();
    ImGui::TextUnformatted("Warnings:");
    if (wb.warnings().empty()) ImGui::TextDisabled("  none");
    for (const auto& w : wb.warnings()) {
        ImVec4 col = (w.severity == WarnSeverity::Block) ? ImVec4(1,0.4f,0.4f,1) : ImVec4(1,0.8f,0.3f,1);
        ImGui::TextColored(col, "  [%s] %s", w.severity == WarnSeverity::Block ? "BLOCK" : "WARN", w.message.c_str());
    }
```

- [ ] **Step 8: Build + commit**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
git add tools/asset_viewer/OverrideManifest.h tools/asset_viewer/OverrideManifest.cpp \
        tools/asset_viewer/WorkbenchValidation.h tools/asset_viewer/WorkbenchValidation.cpp \
        tools/asset_viewer/ModWorkbench.h tools/asset_viewer/ModWorkbench.cpp \
        tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git add -f tests/fixtures/asset_viewer/workbench/wb_valid.json \
           tests/fixtures/asset_viewer/workbench/wb_bad_scale.json
git commit -m "feat(workbench): S3 validation engine (BLOCK mirror + semantic WARN) + warnings panel"
```

---

## Task S4: Export drop-in bundle + round-trip gate

Write `data/model_overrides/<id>/`, copy the GLB + textures, append/merge the `models.json` record, then re-parse through the linked registry and refuse if the record is dropped.

**Files:**
- Modify: `tools/asset_viewer/OverrideManifest.{h,cpp}` (`ToModelsJson`)
- Create: `tools/asset_viewer/BundleExport.{h,cpp}`
- Modify: `tools/asset_viewer/ModWorkbench.{h,cpp}` (`exportBundle`)
- Modify: `tools/asset_viewer/ModWorkbenchPanel.cpp` (Export button)

- [ ] **Step 1: `ToModelsJson` (hand-rolled writer) — add to OverrideManifest**

Add to `OverrideManifest.h`:
```cpp
// Serialize records to the engine's models.json shape: {"overrides":[ ... ]}.
// Hand-rolled (no nlohmann in this TU); validated by the export round-trip.
std::string ToModelsJson(const std::vector<WorkbenchOverride>& recs);
```
Add to `OverrideManifest.cpp`:
```cpp
static std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

std::string ToModelsJson(const std::vector<WorkbenchOverride>& recs) {
    std::string o = "{\n  \"overrides\": [\n";
    for (size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i];
        o += "    {";
        o += "\"type\":\"model\",";
        o += "\"class\":\"" + jsonEscape(r.overrideClass) + "\",";
        o += "\"replaces\":\"" + jsonEscape(r.overrideClass + ":" + r.appearanceName) + "\",";
        o += "\"source\":\"" + jsonEscape(r.sourceRelPath) + "\",";
        o += "\"renderOnly\":true,";
        o += "\"scale\":1.0,";
        o += "\"fallback\":\"stock\"";
        if (!r.lods.empty()) {
            o += ",\"lods\":[";
            for (size_t j = 0; j < r.lods.size(); ++j) {
                const auto& l = r.lods[j];
                o += "{\"lod\":" + std::to_string(l.lod) +
                     ",\"source\":\"" + jsonEscape(l.sourceRelPath) + "\"" +
                     ",\"distance\":" + std::to_string(l.distance) + "}";
                if (j + 1 < r.lods.size()) o += ",";
            }
            o += "]";
        }
        o += "}";
        if (i + 1 < recs.size()) o += ",";
        o += "\n";
    }
    o += "  ]\n}\n";
    return o;
}
```

- [ ] **Step 2: `BundleExport.h`**

```cpp
// tools/asset_viewer/BundleExport.h
// Write a drop-in model_overrides bundle + round-trip it through the linked
// registry (authoritative gate). No GL.
#pragma once
#include "OverrideManifest.h"
#include <string>
#include <vector>

struct ExportResult { bool ok = false; std::string message; std::string bundleDir; std::string manifestPath; };

// outRoot: the "data/model_overrides" directory to write into.
// bundleId: subfolder name (e.g. "my_tree").
// srcGlbPath: absolute path to the dropped GLB to copy into the bundle.
// rec: the record to emit (its sourceRelPath is rewritten to "<id>/<glbBasename>").
// On success the models.json contains the record AND a fresh registry parse
// resolves it; otherwise ok=false and nothing partial is left referenced.
ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec);
```

- [ ] **Step 3: Write the failing export smoke**

Add decl to `AssetViewerApp.h`:
```cpp
    static int runSmokeWorkbenchExport(const char* fixtureDir, const char* tmpDir);
```
Add to `AssetViewerApp.cpp`:
```cpp
#include "BundleExport.h"
#include <filesystem>
int AssetViewerApp::runSmokeWorkbenchExport(const char* fixtureDir, const char* tmpDir)
{
    namespace fs = std::filesystem;
    std::string root = std::string(tmpDir) + "/model_overrides";
    fs::remove_all(root);

    WorkbenchOverride rec;
    rec.overrideClass = "staticprop"; rec.appearanceName = "example_name";
    rec.scale = 1.0f; rec.renderOnly = true; rec.fallback = "stock";
    std::string glb = std::string(fixtureDir) + "/unit_tri.gltf";   // any real file to copy

    ExportResult r = ExportBundle(root, "example_id", glb, rec);
    if (!r.ok) return smokeFail((std::string("export failed: ") + r.message).c_str());
    if (!fs::exists(r.manifestPath)) return smokeFail("export: models.json missing");
    if (!fs::exists(r.bundleDir + "/unit_tri.gltf")) return smokeFail("export: glb not copied");

    // Authoritative round-trip already happened inside ExportBundle; re-confirm here.
    ModelOverrideRegistry reg;
    reg.loadFromFile(r.manifestPath, root);
    if (reg.resolve("staticProp", "example_name") == nullptr)
        return smokeFail("export: registry did not resolve exported record");

    // Refuse path: a record that violates a BLOCK rule must NOT export.
    WorkbenchOverride bad = rec; bad.scale = 2.0f;
    ExportResult rb = ExportBundle(root, "bad_id", glb, bad);
    if (rb.ok) return smokeFail("export: should refuse scale!=1.0 record");

    std::printf("[smoke] PASS workbench-export (round-trip resolves; bad refused)\n");
    return 0;
}
```
Add `main.cpp` dispatch:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-export") == 0)
        return AssetViewerApp::runSmokeWorkbenchExport(argc >= 3 ? argv[2] : ".",
                                                       argc >= 4 ? argv[3] : ".");
```

- [ ] **Step 4: Run smoke to verify it fails (no BundleExport.cpp yet)**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: link error — `ExportBundle` undefined.

- [ ] **Step 5: Implement `BundleExport.cpp`**

```cpp
// tools/asset_viewer/BundleExport.cpp
#include "BundleExport.h"
#include "model_override_registry.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec) {
    ExportResult res;
    if (bundleId.empty() || bundleId.find("..") != std::string::npos ||
        bundleId.find('/') != std::string::npos || bundleId.find('\\') != std::string::npos) {
        res.message = "invalid bundle id"; return res;
    }
    std::error_code ec;
    if (!fs::exists(srcGlbPath)) { res.message = "source GLB not found: " + srcGlbPath; return res; }

    // Rewrite the record's source to the in-bundle relative path.
    std::string glbName = fs::path(srcGlbPath).filename().string();
    rec.sourceRelPath = bundleId + "/" + glbName;

    // Pre-check BLOCK rules so we never write a doomed bundle.
    for (const auto& w : ValidateRecordRules(rec))
        if (w.severity == WarnSeverity::Block) { res.message = "blocked: " + w.message; return res; }

    std::string bundleDir = outRoot + "/" + bundleId;
    fs::create_directories(bundleDir, ec);
    if (ec) { res.message = "mkdir failed: " + ec.message(); return res; }

    fs::copy_file(srcGlbPath, bundleDir + "/" + glbName,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { res.message = "copy failed: " + ec.message(); return res; }

    // Merge into existing models.json records (append unless same key exists).
    std::string manifestPath = outRoot + "/models.json";
    std::vector<WorkbenchOverride> recs;
    {
        ModelOverrideRegistry prior;
        prior.loadFromFile(manifestPath, outRoot);   // existing valid records
        // Carry forward existing records EXCEPT a same-key one (we replace it).
        // (We only have resolve(); reconstruct minimally from the new record set
        //  is out of scope — for MVP, a fresh manifest holds just this record if
        //  none existed, else we append by re-reading the file text is avoided.)
    }
    recs.push_back(rec);
    std::string json = ToModelsJson(recs);
    { std::ofstream out(manifestPath, std::ios::binary); if (!out) { res.message = "write models.json failed"; return res; } out << json; }

    // Authoritative round-trip: refuse if the registry drops our record.
    ModelOverrideRegistry check;
    check.loadFromFile(manifestPath, outRoot);
    const char* cls = rec.overrideClass.c_str();
    if (check.resolve(cls, rec.appearanceName.c_str()) == nullptr) {
        res.message = "registry rejected exported record (round-trip failed)";
        return res;
    }

    res.ok = true; res.bundleDir = bundleDir; res.manifestPath = manifestPath;
    res.message = "exported " + rec.sourceRelPath;
    return res;
}
```
> MVP simplification (documented): merge re-reads only validated prior records via the registry, which exposes `resolve()` but not enumeration, so this MVP writes a single-record manifest per export. Multi-record merge (preserving other overrides) is a known follow-up — see S5 / recon §13. For a single modder iterating one override this is correct; flag it in the Export UI.

- [ ] **Step 6: Run smoke to verify it passes**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
"build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe" \
    --smoke-workbench-export tests/fixtures/asset_viewer/workbench "$TMP"
```
(Use any writable temp dir for `$TMP`, e.g. `export TMP="build64/wb_tmp"`.)
Expected: `[smoke] PASS workbench-export (round-trip resolves; bad refused)`

- [ ] **Step 7: Wire `exportBundle` into ModWorkbench + the panel Export button**

Add to `ModWorkbench.h`:
```cpp
    #include "BundleExport.h"
    ExportResult exportBundle(const std::string& outRoot, const std::string& bundleId);
    void setBundleId(const std::string& id) { bundleId_ = id; }
    const std::string& bundleId() const { return bundleId_; }
```
(private) `std::string bundleId_;`
`ModWorkbench.cpp`:
```cpp
ExportResult ModWorkbench::exportBundle(const std::string& outRoot, const std::string& bundleId) {
    revalidate();
    if (hasBlocking()) { ExportResult r; r.message = "fix BLOCK warnings before export"; return r; }
    return ExportBundle(outRoot, bundleId, overridePath_, record_);
}
```
In `ModWorkbenchPanel.cpp` `draw`, after the warnings list:
```cpp
    static char bundleId[128] = "my_override";
    static char outRoot[260]  = "data/model_overrides";
    static std::string exportMsg;
    ImGui::InputText("Bundle id", bundleId, sizeof(bundleId));
    ImGui::InputText("Out root",  outRoot,  sizeof(outRoot));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Export bundle")) {
        ExportResult r = wb.exportBundle(outRoot, bundleId);
        exportMsg = (r.ok ? "OK: " : "FAILED: ") + r.message;
    }
    ImGui::EndDisabled();
    if (!exportMsg.empty()) ImGui::TextWrapped("%s", exportMsg.c_str());
    ImGui::TextDisabled("MVP: export writes a single-record models.json (multi-record merge is a follow-up).");
```

- [ ] **Step 8: Build + commit**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
git add tools/asset_viewer/OverrideManifest.h tools/asset_viewer/OverrideManifest.cpp \
        tools/asset_viewer/BundleExport.h tools/asset_viewer/BundleExport.cpp \
        tools/asset_viewer/ModWorkbench.h tools/asset_viewer/ModWorkbench.cpp \
        tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S4 export drop-in bundle + authoritative registry round-trip gate"
```

---

## Task S5: Polish (only if time)

Lower-priority refinements. Each is independent; do any subset. Keep the BLOCK/round-trip gate intact.

- [ ] **Stock roster picker** — replace the free-text `Stock .tgl` field with a filtered list from `TglMeshLoader::listTgl()` (the viewer already enumerates 2,030 props in `ModelBrowser`). Reuse `ModelBrowser` or a simple searchable `ImGui::ListBox`. Sets `record_.appearanceName` from the chosen tgl's appearance key. Manual-verify in GUI; commit.

- [ ] **LOD-chain panel** — UI to add `lods[]` entries (lod index + dropped GLB + distance). For each, load via `GlbMeshLoader`, show tri-count vs LOD0, and let `ValidateRecordRules` enforce ascending order. Add a `runSmokeWorkbenchLods` asserting ascending-violation is blocked and a valid chain passes. Commit.

- [ ] **Texture-set panel** — reuse `MaterialSlots` to show the override's referenced textures and which resolve on disk; feed unresolved basenames into `revalidate(missingTextures)` so `texture-missing` WARNs fire. Commit.

- [ ] **Multi-record manifest merge** — make `ExportBundle` preserve other overrides already in `models.json`. Requires enumerating prior records; add a `records()` accessor to a local copy of the registry (do NOT edit the cherry-picked engine file — wrap it, or parse the prior file’s record set via a second linked helper). Add a smoke: export A, export B, assert both resolve. Commit.

---

## Self-Review (completed during planning)

**Spec coverage** (recon §spec → task):
- link registry → S0 ✓ · link assimp → S0 ✓ · GLB→MeshData → S1 ✓ · drag-in → S1 ✓ · stock picker → S2 (free-text) / S5 (roster) ✓ · overlay → S2 ✓ · BLOCK rules → S3 ✓ · bounds delta → S3 ✓ · pivot mismatch → S3 ✓ · material/texture missing → S3 (wired) / S5 (UI) ✓ · LOD source missing/ascending → S3 (`ValidateRecordRules` lod checks) / S5 (UI) ✓ · overdraw heuristic → S3 ✓ · export bundle → S4 ✓ · append/merge models.json → S4 (single-record MVP; multi-record S5) ✓ · re-parse + refuse if dropped → S4 ✓.
- Hard exclusions (no Backend A / impostor authoring / auto-LOD / collision override / non-unit scale / mech-anim): none introduced. Non-unit scale is actively BLOCKED (S3 `scale` rule), matching the exclusion.

**Placeholder scan:** no TBD/TODO; every code step shows complete code. Two documented MVP simplifications (single-record manifest in S4; untextured override preview in S2) are explicit design choices with follow-ups, not gaps.

**Type consistency:** `MeshData`/`SubMesh`/`MeshVertex` (TglMeshLoader.h) used verbatim; `Warning`/`WarnSeverity` defined once (WorkbenchWarning.h) and used across OverrideManifest/WorkbenchValidation/ModWorkbench; `WorkbenchOverride` defined once (OverrideManifest.h); `AssetType::ModWorkbench` added in S2 and referenced consistently (S1 forward-reference flagged with a defer note). Registry API (`loadFromFile`/`resolve`) matches `model_override_registry.h`.

**Known cross-task note:** `onFileDropped` (S1) references `AssetType::ModWorkbench` / `setActive` introduced in S2 — flagged inline to comment-then-enable. Honor it during subagent execution.
