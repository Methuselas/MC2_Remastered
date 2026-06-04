# MC2-ASSET-VIEWER-MOD-WORKBENCH-MVP-1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Mod Workbench" mode to `mc2_asset_viewer` that loads a dropped GLB/GLTF, shows it beside the stock prop it replaces, validates it, and exports a drop-in `model_overrides` **draft** bundle the engine can load.

**Architecture:** The inspect→validate→package loop runs without the engine render path. We link two already-vendored, engine-independent pieces into the standalone viewer: `model_override_registry` (validates/round-trips `models.json`, depends only on nlohmann + stdlib) and Assimp's glTF importer (loads the GLB into the viewer's existing `MeshData`). The viewer's existing `TglMeshLoader` loads the stock prop for a side-by-side compare. Logic lives in non-GL, smoke-testable units; a thin ImGui panel renders over them.

**Tech Stack:** C++17, CMake (VS 2022 / `build64`), SDL2 + ImGui + GLEW (viewer), Assimp (glTF), nlohmann/json (via the linked registry only). Tests = the viewer's `runSmoke*` static-method convention, CLI-dispatched, mostly GL-free.

> **HOW TO READ THIS PLAN (post-review).** The code blocks are **reference implementations**, not literal patches. The viewer target drifts; line numbers and surrounding code may have moved. **The acceptance contract for each step is its smoke test's assertions** — implement whatever code makes that smoke pass against the *current* source, preserving the documented behavior. If a reference block fights the live file, keep the behavior, adapt the code.

> **CMake discipline (review fix #2).** Never list a source file in `CMakeLists.txt` before it exists — the VS generator fails at *configure* time, not link time. **Each unit is added to CMake in the same step that first creates its `.cpp`,** initially as a compiling stub that fails its smoke; the next step replaces the stub body. This keeps every build green-or-red-by-assertion, never red-by-missing-file.

---

## Conventions used by this plan

- **Worktree / branch:** `A:/Games/mc2-asset-viewer-mod-workbench` on `claude/asset-viewer-mod-workbench-recon-1`. All paths are repo-relative to that worktree.
- **Commits:** follow the **project's existing commit convention** (Conventional Commits prefixes are used throughout this repo). Do not add any specific co-author trailer unless the project already requires one.
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
- **Exe:** `build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`. Define once for runs:
  ```bash
  export PATH="$PWD/build64/RelWithDebInfo:$PATH"      # SDL2.dll / glew32.dll
  VIEWER="build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe"
  ```
- **Deploy dir** (only stock-prop smokes need it): a runtime dir with `tgl.fst`, e.g. `DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.4"`.
- **`docs/superpowers/` is gitignored** — `git add -f` for plan/spec/doc files; normal `git add` for code.

## File structure (created / modified)

Created (all under `tools/asset_viewer/` unless noted), each **added to CMake in its creating task**:
- `WorkbenchWarning.h` — `Warning{severity, code, message}` shared type (S3, header-only).
- `OverrideManifest.{h,cpp}` — `WorkbenchOverride` record; `ValidateRecordRules()` (advisory BLOCK mirror); `ToModelsJson()` (hand-rolled writer, escaping-tested). No GL. (S1 header/struct; S3 validate; S4 emit.)
- `GlbMeshLoader.{h,cpp}` — Assimp glTF → `MeshData` (engine-faithful glTF→Stuff→GL). No GL. (S1)
- `ModWorkbench.{h,cpp}` — non-GL workbench state + orchestration + generation counter. (S1→S4)
- `WorkbenchValidation.{h,cpp}` — semantic WARN checks. No GL. (S3)
- `BundleExport.{h,cpp}` — write bundle-local `models.generated.json` + round-trip via registry. No GL. (S4)
- `ModWorkbenchPanel.{h,cpp}` — ImGui panel. (S2→S4)
- `mclib/model_override_registry.{h,cpp}` — cherry-picked, **no edits**. (S0)
- `tests/fixtures/asset_viewer/workbench/` — `make_workbench_fixture.py` + generated `unit_tri.gltf`; validation JSON fixtures.

Modified: `tools/asset_viewer/CMakeLists.txt` (incrementally), `AssetTypeSidebar.{h,cpp}`, `AssetViewerApp.{h,cpp}`, `main.cpp`, `MeshPreview3D.{h,cpp}`.

## Locked conventions (review fixes #4, #5, #6, #7)

- **Class spelling (#5):** emit the documented camelCase `staticProp` / `tree` in `replaces`/`class`. Internal comparisons normalize to lowercase (the registry lowercases too). Tests resolve with the engine spelling.
- **Appearance key (#4):** the runtime key is `"<class>:<appearanceName>"`. The workbench **never silently treats a `.tgl` basename as an engine appearance name.** `appearanceName` is a required, user-editable field. `bindStock` may *suggest* a value from the basename but sets `appearanceVerified = false`; **export is BLOCKED until the user confirms the appearance key.** Output is always labeled a **draft**.
- **Export safety (#6):** export writes a **bundle-local** `data/model_overrides/<id>/models.generated.json` (single record) + the copied GLB. It **never rewrites the central `data/model_overrides/models.json`** — that would destroy a modder's other overrides. Safe central merge is a deferred follow-up.
- **Authority (#7):** the linked registry round-trip is **authoritative**. `ValidateRecordRules` is an **advisory mirror** for live UI only. UI labels: advisory findings → `BLOCK (mirror, advisory)`; an export refusal because the registry dropped the record → `EXPORT BLOCKED BY REGISTRY (authoritative)`.

---

## Task S0: Link spike — registry + Assimp into the viewer

De-risk the scary part first: prove `model_override_registry` and Assimp compile, link, and run inside the viewer before any feature code.

**Files:** Create `mclib/model_override_registry.{h,cpp}` (cherry-pick). Modify `tools/asset_viewer/CMakeLists.txt`, `AssetViewerApp.{h,cpp}`, `main.cpp`.

- [ ] **Step 1: Bring in the registry source (no edits)**
```bash
git checkout claude/model-override-system-recon-1 -- mclib/model_override_registry.h mclib/model_override_registry.cpp
git status --short mclib/model_override_registry.*
```
Expected: both listed as added (`A`).

- [ ] **Step 2: Include-audit gate (review fix: engine-isolation)**
Confirm the registry pulls only nlohmann + stdlib (no GameOS/mclib engine headers), so linking it adds no engine surface beyond the loader the viewer already links:
```bash
grep -E '#include' mclib/model_override_registry.cpp
```
Expected includes: `model_override_registry.h`, `<nlohmann/json.hpp>`, `<cctype>`, `<cstdio>`, `<fstream>`, `<mutex>`, `<utility>`. If any engine header (`tgl.h`, `gameos*.h`, `heap.h`, …) appears, STOP — the cherry-pick pulled an unexpected revision.

- [ ] **Step 3: Wire CMake — registry source only (NOT future files)**
In `tools/asset_viewer/CMakeLists.txt`, inside `set(ASSET_VIEWER_SOURCES ...)`, add **only**:
```cmake
    # engine-independent override-manifest parser (validates/round-trips models.json)
    "${CMAKE_SOURCE_DIR}/mclib/model_override_registry.cpp"
```
After `target_link_libraries(mc2_asset_viewer PRIVATE imgui)`, add:
```cmake
# Assimp (glTF importer only, per root CMake ASSIMP_BUILD_GLTF_IMPORTER=ON).
target_link_libraries(mc2_asset_viewer PRIVATE assimp)
```
In the final `target_include_directories(mc2_asset_viewer PRIVATE ...)` block, add:
```cmake
    "${CMAKE_SOURCE_DIR}/mclib"                       # model_override_registry.h
    "${CMAKE_SOURCE_DIR}/3rdparty/include"            # nlohmann/json.hpp (registry TU only)
    "${CMAKE_SOURCE_DIR}/3rdparty/assimp/include"     # <assimp/Importer.hpp>
    "${CMAKE_BINARY_DIR}/out/3rdparty/assimp/include" # generated assimp config.h
```
> The 6 workbench `.cpp` files are added to this `set(...)` later, each in the task that creates it.

- [ ] **Step 4: Write the failing link smoke**
`AssetViewerApp.h` — add decl in the smoke block:
```cpp
    static int runSmokeWorkbenchLink();   // S0: registry + assimp link/run
```
`AssetViewerApp.cpp` — add at end of file:
```cpp
#include "model_override_registry.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

int AssetViewerApp::runSmokeWorkbenchLink()
{
    ModelOverrideRegistry reg;
    if (reg.loadFromFile("does_not_exist_models.json", ".") != 0)
        return smokeFail("workbench-link: expected 0 records");
    Assimp::Importer imp;
    if (!imp.IsExtensionSupported(".gltf"))
        return smokeFail("workbench-link: assimp lacks .gltf importer");
    std::printf("[smoke] PASS workbench-link (registry + assimp glTF linked)\n");
    return 0;
}
```
`main.cpp` — add after the `--smoke-spotlight` block:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-link") == 0)
        return AssetViewerApp::runSmokeWorkbenchLink();
```

- [ ] **Step 5: Configure + build**
Run Configure (top), then build. Expected: success. If `<assimp/Importer.hpp>` does not resolve, adjust the assimp include entries until it does; the `assimp` link target is provided by the root `add_subdirectory(3rdparty/assimp ...)`.

- [ ] **Step 6: Run the smoke → PASS**
```bash
"$VIEWER" --smoke-workbench-link
```
Expected: `[smoke] PASS workbench-link (registry + assimp glTF linked)`.

- [ ] **Step 7: Commit** (project commit convention)
```bash
git add mclib/model_override_registry.h mclib/model_override_registry.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S0 link model_override_registry + assimp into mc2_asset_viewer"
```

---

## Task S1: GLB load → MeshData + drag-into-window

**Files:** Create `GlbMeshLoader.{h,cpp}`, `ModWorkbench.{h,cpp}`, `WorkbenchWarning.h`, `OverrideManifest.h`, fixture + generator. Modify `CMakeLists.txt`, `AssetViewerApp.{h,cpp}`, `main.cpp`.

- [ ] **Step 1: Generate the glTF fixture (all-axes, review fix #8)**
Create `tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py`:
```python
#!/usr/bin/env python3
# Minimal valid glTF 2.0 triangle, embedded base64 buffer, deterministic.
# Source verts deliberately use distinct nonzero Y and Z so a half-applied or
# wrong axis swap cannot pass the transform smoke (review fix #8).
import base64, json, struct, os
P  = [(0.0, 0.0, 0.0), (2.0, 0.0, 5.0), (0.0, 3.0, 7.0)]   # glTF source space
UV = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
IDX = [0, 1, 2]
pos = b"".join(struct.pack("<3f", *p) for p in P)
uv  = b"".join(struct.pack("<2f", *u) for u in UV)
idx = struct.pack("<3H", *IDX)
buf = pos + uv + idx
mn = lambda vs,n:[min(v[i] for v in vs) for i in range(n)]
mx = lambda vs,n:[max(v[i] for v in vs) for i in range(n)]
gltf = {
 "asset":{"version":"2.0","generator":"make_workbench_fixture"},
 "buffers":[{"byteLength":len(buf),"uri":"data:application/octet-stream;base64,"+base64.b64encode(buf).decode()}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":len(pos),"target":34962},
   {"buffer":0,"byteOffset":len(pos),"byteLength":len(uv),"target":34962},
   {"buffer":0,"byteOffset":len(pos)+len(uv),"byteLength":len(idx),"target":34963}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":mn(P,3),"max":mx(P,3)},
   {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2","min":mn(UV,2),"max":mx(UV,2)},
   {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
 "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"mode":4}]}],
 "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0}
out=os.path.join(os.path.dirname(__file__),"unit_tri.gltf")
open(out,"w",encoding="utf-8").write(json.dumps(gltf,indent=1)); print("wrote",out)
```
Run + confirm:
```bash
python tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py
test -f tests/fixtures/asset_viewer/workbench/unit_tri.gltf && echo OK
```

- [ ] **Step 2: Headers (`GlbMeshLoader.h`, `WorkbenchWarning.h`, `OverrideManifest.h` struct)**
`GlbMeshLoader.h`:
```cpp
// tools/asset_viewer/GlbMeshLoader.h
// Assimp glTF/GLB -> MeshData. No GL. Engine-faithful transform:
//   step1 glTF->Stuff: (mc2.x=-src.x, mc2.y=src.z, mc2.z=src.y)  [assimp_importer.cpp]
//   step2 Stuff->GL:   (gl.x=-mc2.x, gl.y=mc2.z, gl.z=mc2.y)     [TglMeshLoader.h]
// plus UV v->1-v. Applied as the literal two-step composition so it stays
// auditable against the engine even though it reduces to identity for positions.
#pragma once
#include "TglMeshLoader.h"
#include <string>
namespace GlbMeshLoader { MeshData load(const std::string& path); }
```
`WorkbenchWarning.h`:
```cpp
// tools/asset_viewer/WorkbenchWarning.h
#pragma once
#include <string>
#include <vector>
enum class WarnSeverity { Block, Warn };
struct Warning { WarnSeverity severity; std::string code; std::string message; };
```
`OverrideManifest.h` (struct now; `ValidateRecordRules`/`ToModelsJson` decls added in S3/S4):
```cpp
// tools/asset_viewer/OverrideManifest.h
#pragma once
#include "WorkbenchWarning.h"
#include <string>
#include <vector>
struct WorkbenchOverrideLod { int lod = 0; std::string sourceRelPath; float distance = 0.0f; };
struct WorkbenchOverride {
    std::string overrideClass = "staticProp";    // EMIT spelling: "staticProp" | "tree" (review fix #5)
    std::string appearanceName;                  // engine appearance key (user-verified; review fix #4)
    bool        appearanceVerified = false;      // export blocked until true
    std::string sourceRelPath;                   // "<id>/model.glb"
    float       scale = 1.0f;
    bool        renderOnly = true;
    std::string fallback = "stock";
    std::vector<WorkbenchOverrideLod> lods;
};
```

- [ ] **Step 3: `GlbMeshLoader.cpp` STUB + add to CMake (red state)**
Create `GlbMeshLoader.cpp` as a failing stub:
```cpp
#include "GlbMeshLoader.h"
MeshData GlbMeshLoader::load(const std::string&) { MeshData m; m.ok = false; m.error = "stub"; return m; }
```
Add `GlbMeshLoader.cpp` (and `ModWorkbench.cpp`, `OverrideManifest.cpp` — created this task) to `ASSET_VIEWER_SOURCES` in `tools/asset_viewer/CMakeLists.txt`:
```cmake
    GlbMeshLoader.cpp
    ModWorkbench.cpp
    OverrideManifest.cpp
```
Create `OverrideManifest.cpp` as a near-empty TU for now (body lands S3/S4):
```cpp
#include "OverrideManifest.h"
// ValidateRecordRules / ToModelsJson implemented in S3 / S4.
```

- [ ] **Step 4: Write the failing GLB smoke**
`AssetViewerApp.h`:
```cpp
    static int runSmokeWorkbenchGlb(const char* fixtureDir);   // S1
```
`AssetViewerApp.cpp`:
```cpp
#include "GlbMeshLoader.h"
#include <cmath>
int AssetViewerApp::runSmokeWorkbenchGlb(const char* fixtureDir)
{
    MeshData md = GlbMeshLoader::load(std::string(fixtureDir) + "/unit_tri.gltf");
    if (!md.ok) return smokeFail((std::string("workbench-glb: ") + md.error).c_str());
    if (md.submeshes.size() != 1) return smokeFail("workbench-glb: submeshes!=1");
    const SubMesh& sm = md.submeshes[0];
    if (sm.verts.size() != 3) return smokeFail("workbench-glb: verts!=3");

    // Engine two-step reduces to identity for positions, so GL == source coords.
    // Assert the EXACT set {(0,0,0),(2,0,5),(0,3,7)} so a wrong/half transform fails.
    auto has = [&](float x,float y,float z){
        for (auto& v : sm.verts)
            if (std::fabs(v.px-x)<1e-3f && std::fabs(v.py-y)<1e-3f && std::fabs(v.pz-z)<1e-3f) return true;
        return false; };
    if (!has(0,0,0) || !has(2,0,5) || !has(0,3,7))
        return smokeFail("workbench-glb: transformed positions wrong (axis swap?)");
    // Extents: X=2, Y=3, Z=7.
    float ex=md.bmax[0]-md.bmin[0], ey=md.bmax[1]-md.bmin[1], ez=md.bmax[2]-md.bmin[2];
    if (std::fabs(ex-2)>1e-3f || std::fabs(ey-3)>1e-3f || std::fabs(ez-7)>1e-3f)
        return smokeFail("workbench-glb: extents wrong");
    // UV v-flip: source vert2 UV (0,1) -> stored (0,0).
    bool flipped=false; for (auto& v: sm.verts) if (std::fabs(v.u)<1e-3f && std::fabs(v.v)<1e-3f) flipped=true;
    if (!flipped) return smokeFail("workbench-glb: UV v-flip missing");
    std::printf("[smoke] PASS workbench-glb verts=3 ext=%.1f,%.1f,%.1f\n", ex,ey,ez);
    return 0;
}
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-glb") == 0)
        return AssetViewerApp::runSmokeWorkbenchGlb(argc >= 3 ? argv[2] : ".");
```
Configure (new sources) + build + run → expect FAIL (`stub`).

- [ ] **Step 5: Implement `GlbMeshLoader.cpp` (replace stub)**
```cpp
#include "GlbMeshLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cfloat>
namespace {
inline void srcToGl(float sx,float sy,float sz,float& gx,float& gy,float& gz){
    const float mx=-sx,my=sz,mz=sy;          // glTF -> Stuff
    gx=-mx; gy=mz; gz=my;                     // Stuff -> GL
}
std::string baseName(const std::string& p){ size_t s=p.find_last_of("/\\"); return s==std::string::npos?p:p.substr(s+1); }
}
MeshData GlbMeshLoader::load(const std::string& path){
    MeshData out;
    Assimp::Importer imp;
    const aiScene* sc = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices);
    if (!sc || (sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !sc->mRootNode || sc->mNumMeshes==0){
        out.ok=false; out.error=imp.GetErrorString(); if(out.error.empty()) out.error="no meshes"; return out; }
    float lo[3]={FLT_MAX,FLT_MAX,FLT_MAX}, hi[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for (unsigned mi=0; mi<sc->mNumMeshes; ++mi){
        const aiMesh* m=sc->mMeshes[mi];
        if (!m->mNumVertices || !m->HasFaces()) continue;
        SubMesh smsh;
        if (m->mMaterialIndex < sc->mNumMaterials){
            aiString tex;
            if (sc->mMaterials[m->mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE,0,&tex)==AI_SUCCESS)
                smsh.textureName = baseName(tex.C_Str());
        }
        for (unsigned f=0; f<m->mNumFaces; ++f){
            const aiFace& fc=m->mFaces[f];
            if (fc.mNumIndices!=3) continue;
            for (int c=0;c<3;++c){
                unsigned vi=fc.mIndices[c];
                MeshVertex v{};
                srcToGl(m->mVertices[vi].x,m->mVertices[vi].y,m->mVertices[vi].z, v.px,v.py,v.pz);
                if (m->HasNormals())
                    srcToGl(m->mNormals[vi].x,m->mNormals[vi].y,m->mNormals[vi].z, v.nx,v.ny,v.nz);
                if (m->HasTextureCoords(0)){ v.u=m->mTextureCoords[0][vi].x; v.v=1.0f-m->mTextureCoords[0][vi].y; }
                smsh.verts.push_back(v); smsh.idx.push_back((uint32_t)smsh.idx.size());
                for (int k=0;k<3;++k){ float p=(&v.px)[k]; if(p<lo[k])lo[k]=p; if(p>hi[k])hi[k]=p; }
            }
        }
        if (!smsh.verts.empty()) out.submeshes.push_back(std::move(smsh));
    }
    if (out.submeshes.empty()){ out.ok=false; out.error="no triangulated geometry"; return out; }
    for (int k=0;k<3;++k){ out.bmin[k]=lo[k]; out.bmax[k]=hi[k]; }
    out.ok=true; return out;
}
```
Build + run → expect `[smoke] PASS workbench-glb verts=3 ext=2.0,3.0,7.0`.

- [ ] **Step 6: `ModWorkbench.{h,cpp}` (S1 portion: load + generation counter, review fix #9)**
`ModWorkbench.h`:
```cpp
// tools/asset_viewer/ModWorkbench.h
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "WorkbenchWarning.h"
#include <cstdint>
#include <string>
class ModWorkbench {
public:
    bool loadOverride(const std::string& glbPath);   // bumps generation on success
    bool hasOverride() const { return overrideMesh_.ok; }
    const MeshData& overrideMesh() const { return overrideMesh_; }
    const std::string& overridePath() const { return overridePath_; }
    const std::string& lastError() const { return lastError_; }
    // Monotonic generation: panel re-syncs previews when this changes (NOT pointer identity).
    uint64_t generation() const { return generation_; }
private:
    std::string overridePath_;
    MeshData    overrideMesh_;
    std::string lastError_;
    uint64_t    generation_ = 0;
};
```
`ModWorkbench.cpp`:
```cpp
#include "ModWorkbench.h"
#include "GlbMeshLoader.h"
bool ModWorkbench::loadOverride(const std::string& glbPath){
    overridePath_ = glbPath;
    overrideMesh_ = GlbMeshLoader::load(glbPath);
    lastError_ = overrideMesh_.ok ? std::string() : overrideMesh_.error;
    if (overrideMesh_.ok) ++generation_;
    return overrideMesh_.ok;
}
```

- [ ] **Step 7: SDL_DROPFILE + owning `ModWorkbench`**
`AssetViewerApp.h`: add `#include "ModWorkbench.h"`, member `ModWorkbench workbench_;`, decl `void onFileDropped(const char* path);`.
`AssetViewerApp.cpp`:
```cpp
void AssetViewerApp::onFileDropped(const char* path){
    if (!path) return;
    std::string p = path, low = p; for (char& c: low) c=(char)tolower((unsigned char)c);
    auto ends=[&](const char* s){ size_t n=strlen(s); return low.size()>=n && low.compare(low.size()-n,n,s)==0; };
    if (ends(".glb") || ends(".gltf")){
        workbench_.loadOverride(p);
        sidebar_.setActive(AssetType::ModWorkbench);   // both added in S2; see note
    }
}
```
> **Cross-task note:** `AssetType::ModWorkbench` and `setActive` are introduced in S2. When executing strictly in order, comment the `sidebar_.setActive(...)` line here and enable it in S2 Step 6. (Subagent execution: pass this note forward.)

`main.cpp` — inside the `SDL_PollEvent` loop:
```cpp
                if (event.type == SDL_DROPFILE){ app.onFileDropped(event.drop.file); SDL_free(event.drop.file); }
```

- [ ] **Step 8: Build + commit**
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
"$VIEWER" --smoke-workbench-glb tests/fixtures/asset_viewer/workbench   # PASS
git add tools/asset_viewer/GlbMeshLoader.* tools/asset_viewer/ModWorkbench.* \
        tools/asset_viewer/WorkbenchWarning.h tools/asset_viewer/OverrideManifest.* \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.* tools/asset_viewer/main.cpp
git add -f tests/fixtures/asset_viewer/workbench/make_workbench_fixture.py \
           tests/fixtures/asset_viewer/workbench/unit_tri.gltf
git commit -m "feat(workbench): S1 glTF->MeshData loader + SDL_DROPFILE drag-in + generation counter"
```

---

## Task S2: Stock bind + side-by-side overlay

**Files:** Modify `MeshPreview3D.{h,cpp}`, `ModWorkbench.{h,cpp}`, `AssetTypeSidebar.{h,cpp}`, `AssetViewerApp.{h,cpp}`. Create `ModWorkbenchPanel.{h,cpp}` (add to CMake here).

- [ ] **Step 1: `MeshPreview3D::setMeshData`**
`MeshPreview3D.h` after `setSource`:
```cpp
    // Render an arbitrary MeshData (e.g. a GLB override). Untextured preview
    // (albedo not resolved) — geometry/scale/pivot is what the workbench validates.
    void setMeshData(const MeshData& md);
```
`MeshPreview3D.cpp` — mirror the upload + recenter that `setSource` performs (read its exact `mesh_.upload(...)` argument list and copy it):
```cpp
void MeshPreview3D::setMeshData(const MeshData& md){
    if (!md.ok){ errorMsg_ = md.error.empty()?"bad mesh":md.error; return; }
    errorMsg_.clear(); tglName_.clear();
    mesh_.upload(md, deployDir_, tier_);                    // SAME call setSource uses
    center_[0]=0.5f*(md.bmin[0]+md.bmax[0]);
    center_[1]=0.5f*(md.bmin[1]+md.bmax[1]);
    center_[2]=0.5f*(md.bmin[2]+md.bmax[2]);
}
```

- [ ] **Step 2: ModWorkbench bind + delta (failing smoke first)**
`ModWorkbench.h` add:
```cpp
    void setDeployDir(const std::string& d){ deployDir_ = d; }
    bool bindStock(const std::string& tglName);              // bumps generation
    bool hasStock() const { return stockMesh_.ok; }
    const MeshData& stockMesh() const { return stockMesh_; }
    struct BoundsDelta { float overrideExt[3]{}, stockExt[3]{}; float maxRatio=1.0f; float pivotOffset[3]{}; };
    BoundsDelta computeDelta() const;
```
private: `std::string deployDir_="."; std::string stockTgl_; MeshData stockMesh_;`
`AssetViewerApp.h`:
```cpp
    static int runSmokeWorkbenchBind(const char* deployDir, const char* fixtureDir);
```
`AssetViewerApp.cpp`:
```cpp
int AssetViewerApp::runSmokeWorkbenchBind(const char* deployDir, const char* fixtureDir){
    ModWorkbench wb; wb.setDeployDir(deployDir);
    if (!wb.loadOverride(std::string(fixtureDir)+"/unit_tri.gltf")) return smokeFail("bind: override load");
    uint64_t g0 = wb.generation();
    if (!wb.bindStock("data/tgl/2civliving.tgl")) return smokeFail("bind: stock load (need deploy dir w/ tgl.fst)");
    if (wb.generation() == g0) return smokeFail("bind: generation did not advance");
    auto d = wb.computeDelta();
    if (d.stockExt[1] <= 0.0f) return smokeFail("bind: stock has no Y extent");
    if (d.maxRatio <= 0.0f)    return smokeFail("bind: ratio not computed");
    std::printf("[smoke] PASS workbench-bind stockY=%.2f ovY=%.2f ratio=%.3f\n",
                d.stockExt[1], d.overrideExt[1], d.maxRatio);
    return 0;
}
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-bind") == 0)
        return AssetViewerApp::runSmokeWorkbenchBind(argc>=3?argv[2]:".", argc>=4?argv[3]:".");
```
Build → expect link FAIL (undefined `bindStock`/`computeDelta`).

- [ ] **Step 3: Implement bind + delta**
`ModWorkbench.cpp` (add `#include <cmath>`):
```cpp
bool ModWorkbench::bindStock(const std::string& tglName){
    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())){ lastError_="tgl.fst not found"; return false; }
    stockTgl_ = tglName;
    stockMesh_ = TglMeshLoader::loadMesh(tglName);
    if (!stockMesh_.ok){ lastError_=stockMesh_.error; return false; }
    ++generation_;
    return true;
}
ModWorkbench::BoundsDelta ModWorkbench::computeDelta() const {
    BoundsDelta d;
    auto ext=[](const MeshData& m,float o[3]){ for(int k=0;k<3;++k) o[k]=m.ok?(m.bmax[k]-m.bmin[k]):0.0f; };
    ext(overrideMesh_,d.overrideExt); ext(stockMesh_,d.stockExt);
    d.maxRatio=0.0f;
    for(int k=0;k<3;++k) if(d.stockExt[k]>1e-4f){ float r=d.overrideExt[k]/d.stockExt[k]; if(r>d.maxRatio)d.maxRatio=r; }
    if(d.maxRatio==0.0f) d.maxRatio=1.0f;
    auto base=[](const MeshData& m,float o[3]){ o[0]=m.ok?0.5f*(m.bmin[0]+m.bmax[0]):0; o[1]=m.ok?m.bmin[1]:0; o[2]=m.ok?0.5f*(m.bmin[2]+m.bmax[2]):0; };
    float ob[3],sb[3]; base(overrideMesh_,ob); base(stockMesh_,sb);
    for(int k=0;k<3;++k) d.pivotOffset[k]=ob[k]-sb[k];
    return d;
}
```
Build + run (needs deploy):
```bash
"$VIEWER" --smoke-workbench-bind "$DEPLOY" tests/fixtures/asset_viewer/workbench   # PASS
```

- [ ] **Step 4: `ModWorkbench` asset type + sidebar**
`AssetTypeSidebar.h`:
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
`AssetTypeSidebar.cpp`: add a `ImGui::Selectable("Mod Workbench", active_==AssetType::ModWorkbench)` that sets `active_`, matching the existing selectables' pattern.

- [ ] **Step 5: `ModWorkbenchPanel.{h,cpp}` (generation-synced; banner) + add to CMake**
`ModWorkbenchPanel.h`:
```cpp
// tools/asset_viewer/ModWorkbenchPanel.h
#pragma once
#include "ModWorkbench.h"
#include "MeshPreview3D.h"
#include <cstdint>
struct ImVec2;
class ModWorkbenchPanel {
public:
    void setDeployDir(const std::string& d);
    void draw(ModWorkbench& wb, const ImVec2& avail);
private:
    void syncMeshes(ModWorkbench& wb);
    MeshPreview3D stockPreview_, overridePreview_;
    uint64_t lastSyncedGen_ = (uint64_t)-1;   // re-sync when wb.generation() changes (review fix #9)
};
```
`ModWorkbenchPanel.cpp`:
```cpp
#include "ModWorkbenchPanel.h"
#include "imgui.h"
void ModWorkbenchPanel::setDeployDir(const std::string& d){ stockPreview_.setDeployDir(d); overridePreview_.setDeployDir(d); }
void ModWorkbenchPanel::syncMeshes(ModWorkbench& wb){
    if (wb.hasStock())    stockPreview_.setMeshData(wb.stockMesh());
    if (wb.hasOverride()) overridePreview_.setMeshData(wb.overrideMesh());
}
void ModWorkbenchPanel::draw(ModWorkbench& wb, const ImVec2& avail){
    ImGui::TextUnformatted("Mod Workbench — drag a .glb/.gltf onto the window");
    ImGui::TextColored(ImVec4(0.7f,0.8f,1,1),
        "Preview validates geometry/package. In-game lighting/material may differ (Backend A = v2).");
    ImGui::Separator();
    if (!wb.hasOverride()){ ImGui::TextDisabled("No override loaded. %s", wb.lastError().c_str()); return; }
    ImGui::Text("Override: %s", wb.overridePath().c_str());

    // Stock bind (free-text in MVP; roster picker is S5).
    static char tgl[256] = "data/tgl/2civliving.tgl";
    ImGui::InputText("Stock .tgl", tgl, sizeof(tgl));
    if (ImGui::Button("Bind stock")) wb.bindStock(tgl);

    if (wb.generation() != lastSyncedGen_){ syncMeshes(wb); lastSyncedGen_ = wb.generation(); }

    overridePreview_.orbitYaw()=stockPreview_.orbitYaw();
    overridePreview_.orbitPitch()=stockPreview_.orbitPitch();
    overridePreview_.zoom()=stockPreview_.zoom();
    float half=(avail.x-8.0f)*0.5f, h=avail.y*0.55f;
    ImGui::BeginChild("stock", ImVec2(half,h), true);
    ImGui::TextUnformatted("Stock"); stockPreview_.draw(ImGui::GetContentRegionAvail()); ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("override", ImVec2(half,h), true);
    ImGui::TextUnformatted("Override"); overridePreview_.draw(ImGui::GetContentRegionAvail()); ImGui::EndChild();

    if (wb.hasStock()){
        auto d=wb.computeDelta();
        ImGui::Text("footprint ratio (override/stock, max axis): %.2fx", d.maxRatio);
        ImGui::Text("pivot offset (GL): %.2f, %.2f, %.2f", d.pivotOffset[0],d.pivotOffset[1],d.pivotOffset[2]);
    }
}
```
Add `ModWorkbenchPanel.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 6: Dispatch + enable drop-switch**
`AssetViewerApp.h`: add `#include "ModWorkbenchPanel.h"`, member `ModWorkbenchPanel workbenchPanel_;`. Ctor: `workbenchPanel_.setDeployDir("."); workbench_.setDeployDir(".");`. In `drawUi` inspector switch add:
```cpp
      case AssetType::ModWorkbench:
        workbenchPanel_.draw(workbench_, ImGui::GetContentRegionAvail()); break;
```
In the browser child, add an `else if (sidebar_.active()==AssetType::ModWorkbench) ImGui::TextDisabled("Drop a GLB on the window.");` branch so it doesn't fall into the texture browser. Enable the deferred `sidebar_.setActive(AssetType::ModWorkbench)` in `onFileDropped`.

- [ ] **Step 7: Build + manual verify + commit**
Build; GUI check (copy exe beside DLLs): drag `unit_tri.gltf` → switches to Mod Workbench, override shows; Bind stock → side-by-side + ratio/pivot.
```bash
git add tools/asset_viewer/MeshPreview3D.* tools/asset_viewer/ModWorkbench.* \
        tools/asset_viewer/AssetTypeSidebar.* tools/asset_viewer/ModWorkbenchPanel.* \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.* tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S2 stock bind + side-by-side overlay + bounds/pivot delta + approximate-preview banner"
```

> **REVIEW CHECKPOINT (reviewer recommendation): stop after S2 and review before S3/S4.** S4 writes files into a mod folder; confirm S0–S2 behavior first.

---

## Task S3: Validation engine + warnings panel

**Files:** Modify `OverrideManifest.{h,cpp}`, create `WorkbenchValidation.{h,cpp}` (add to CMake), modify `ModWorkbench.{h,cpp}`, `ModWorkbenchPanel.cpp`, add fixtures.

- [ ] **Step 1: `ValidateRecordRules` (advisory BLOCK mirror, case-insensitive class — review #5/#7)**
Add to `OverrideManifest.h`:
```cpp
// ADVISORY mirror of the registry's MVP invariants for live UI. The registry
// round-trip (BundleExport) is AUTHORITATIVE. S3's smoke asserts agreement.
std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& rec);
```
Implement in `OverrideManifest.cpp`:
```cpp
#include "OverrideManifest.h"
#include <cctype>
static std::string lower(std::string s){ for(char& c:s) c=(char)std::tolower((unsigned char)c); return s; }
static bool isSafeSource(const std::string& s){
    if (s.empty()||s[0]=='/'||s[0]=='\\') return false;
    if (s.size()>=2 && s[1]==':') return false;
    if (s.find("..")!=std::string::npos) return false;
    std::string l=lower(s);
    return (l.size()>=4 && l.compare(l.size()-4,4,".glb")==0) || (l.size()>=5 && l.compare(l.size()-5,5,".gltf")==0);
}
std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& r){
    std::vector<Warning> w; auto block=[&](const char* c,const char* m){ w.push_back({WarnSeverity::Block,c,m}); };
    if (!r.renderOnly)                       block("renderOnly","renderOnly must be true (MVP)");
    if (r.fallback!="stock")                 block("fallback","fallback must be \"stock\" (MVP)");
    if (r.scale!=1.0f)                       block("scale","runtime scale must be exactly 1.0 — bake scale into the GLB");
    { std::string c=lower(r.overrideClass); if (c!="staticprop" && c!="tree") block("class","class must be staticProp or tree"); }
    if (r.appearanceName.empty())            block("replaces","no stock appearance bound");
    if (!r.appearanceVerified)               block("appearance-unverified","appearance key not confirmed — verify it matches the engine appearance name");
    if (!isSafeSource(r.sourceRelPath))      block("source","source must be a safe relative .glb/.gltf path");
    int last=0; for (const auto& l: r.lods){
        if (l.lod<=last)                     block("lod-order","LOD indices must strictly ascend (LOD0=source)");
        if (!isSafeSource(l.sourceRelPath))  block("lod-source","LOD source must be a safe relative .glb/.gltf path");
        last=l.lod; }
    return w;
}
```
> Note the extra `appearance-unverified` BLOCK (review #4): the mirror also enforces the verified-key gate even though the registry can't know it.

- [ ] **Step 2: `WorkbenchValidation.{h,cpp}` (semantic WARN) + add to CMake**
`WorkbenchValidation.h`:
```cpp
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "WorkbenchWarning.h"
#include <string>
#include <vector>
struct SemanticInputs {
    const MeshData* overrideMesh=nullptr; const MeshData* stockMesh=nullptr;
    float maxFootprintRatio=1.0f, pivotOffsetXZ=0.0f, pivotOffsetY=0.0f;
    std::vector<std::string> missingTextures; bool hasImpostorLod=false;
};
std::vector<Warning> ValidateSemantics(const WorkbenchOverride& rec, const SemanticInputs& in);
```
`WorkbenchValidation.cpp`:
```cpp
#include "WorkbenchValidation.h"
#include <cmath>
std::vector<Warning> ValidateSemantics(const WorkbenchOverride&, const SemanticInputs& in){
    std::vector<Warning> w; auto warn=[&](const char* c,const std::string& m){ w.push_back({WarnSeverity::Warn,c,m}); };
    if (in.stockMesh && in.stockMesh->ok){
        if (in.maxFootprintRatio>1.5f || in.maxFootprintRatio<0.67f)
            warn("bounds-delta","override footprint "+std::to_string(in.maxFootprintRatio)+"x stock — verify scale (runtime forces 1.0)");
        if (in.pivotOffsetXZ>0.25f) warn("pivot-xz","override off-center vs stock by "+std::to_string(in.pivotOffsetXZ));
        if (std::fabs(in.pivotOffsetY)>0.25f) warn("pivot-y","override base vertically offset by "+std::to_string(in.pivotOffsetY));
    } else warn("no-stock","no stock bound — bounds/pivot not validated");
    for (const auto& t: in.missingTextures) warn("texture-missing","referenced texture not found: "+t);
    bool alpha=false;
    if (in.overrideMesh) for (const auto& sm: in.overrideMesh->submeshes)
        if (sm.textureName.rfind("a_",0)==0 || sm.textureName.find("_a_")!=std::string::npos) alpha=true;
    if (alpha && !in.hasImpostorLod) warn("overdraw","alpha-card override with no far-LOD impostor — likely high overdraw in-game (heuristic)");
    return w;
}
```
Add `WorkbenchValidation.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 3: Validation fixtures**
`tests/fixtures/asset_viewer/workbench/wb_valid.json`:
```json
{ "overrides": [ { "type":"model","class":"staticProp","replaces":"staticProp:example_name","source":"props/example.glb","renderOnly":true,"scale":1.0,"fallback":"stock" } ] }
```
`tests/fixtures/asset_viewer/workbench/wb_bad_scale.json`:
```json
{ "overrides": [ { "type":"model","class":"staticProp","replaces":"staticProp:example_name","source":"props/example.glb","renderOnly":true,"scale":2.0,"fallback":"stock" } ] }
```

- [ ] **Step 4: Failing validate smoke (mirror⇔registry agreement, semantics fire)**
`AssetViewerApp.h`: `static int runSmokeWorkbenchValidate(const char* fixtureDir);`
`AssetViewerApp.cpp`:
```cpp
#include "ModWorkbench.h"
int AssetViewerApp::runSmokeWorkbenchValidate(const char* fixtureDir){
    WorkbenchOverride ok; ok.overrideClass="staticProp"; ok.appearanceName="example_name"; ok.appearanceVerified=true;
    ok.sourceRelPath="props/example.glb"; ok.scale=1.0f; ok.renderOnly=true; ok.fallback="stock";
    if (!ValidateRecordRules(ok).empty()) return smokeFail("validate: clean record flagged");
    auto hasCode=[&](const std::vector<Warning>& v,const char* c){ for(auto&x:v) if(x.code==c) return true; return false; };
    { auto bs=ok; bs.scale=2.0f; if(!hasCode(ValidateRecordRules(bs),"scale")) return smokeFail("validate: scale!=1 not blocked"); }
    { auto bsrc=ok; bsrc.sourceRelPath="C:/abs.png"; if(!hasCode(ValidateRecordRules(bsrc),"source")) return smokeFail("validate: unsafe source not blocked"); }
    { auto unv=ok; unv.appearanceVerified=false; if(!hasCode(ValidateRecordRules(unv),"appearance-unverified")) return smokeFail("validate: unverified appearance not blocked"); }
    // mirror vs registry agreement on shared fixtures
    ModelOverrideRegistry reg;
    if (reg.loadFromFile(std::string(fixtureDir)+"/wb_valid.json",fixtureDir)!=1 || reg.resolve("staticProp","example_name")==nullptr)
        return smokeFail("validate: registry should accept wb_valid.json");
    if (reg.loadFromFile(std::string(fixtureDir)+"/wb_bad_scale.json",fixtureDir)!=0)
        return smokeFail("validate: registry should reject wb_bad_scale.json");
    // semantic WARN fires
    MeshData ov; ov.ok=true; ov.bmax[0]=4; ov.bmax[1]=4; ov.bmax[2]=4;
    MeshData st; st.ok=true; st.bmax[0]=1; st.bmax[1]=1; st.bmax[2]=1;
    SemanticInputs si; si.overrideMesh=&ov; si.stockMesh=&st; si.maxFootprintRatio=4.0f;
    if(!hasCode(ValidateSemantics(ok,si),"bounds-delta")) return smokeFail("validate: oversize not warned");
    std::printf("[smoke] PASS workbench-validate\n"); return 0;
}
```
`main.cpp`: `--smoke-workbench-validate` dispatch. Build → expect FAIL (undefined `revalidate` if referenced; here the smoke uses free functions, so it fails on assertions only after stubs — implement next).

- [ ] **Step 5: `ModWorkbench` revalidate + record accessors**
`ModWorkbench.h` add `#include "WorkbenchValidation.h"`:
```cpp
    WorkbenchOverride& record(){ return record_; }
    const std::vector<Warning>& warnings() const { return warnings_; }
    bool hasBlocking() const;
    void revalidate(const std::vector<std::string>& missingTextures = {});
```
private: `WorkbenchOverride record_; std::vector<Warning> warnings_;`
`ModWorkbench.cpp`:
```cpp
void ModWorkbench::revalidate(const std::vector<std::string>& missing){
    warnings_.clear();
    auto b=ValidateRecordRules(record_); warnings_.insert(warnings_.end(),b.begin(),b.end());
    SemanticInputs si;
    si.overrideMesh = overrideMesh_.ok?&overrideMesh_:nullptr;
    si.stockMesh    = stockMesh_.ok?&stockMesh_:nullptr;
    auto d=computeDelta(); si.maxFootprintRatio=d.maxRatio;
    si.pivotOffsetXZ=std::sqrt(d.pivotOffset[0]*d.pivotOffset[0]+d.pivotOffset[2]*d.pivotOffset[2]);
    si.pivotOffsetY=d.pivotOffset[1]; si.missingTextures=missing; si.hasImpostorLod=!record_.lods.empty();
    auto s=ValidateSemantics(record_,si); warnings_.insert(warnings_.end(),s.begin(),s.end());
}
bool ModWorkbench::hasBlocking() const { for(const auto& w:warnings_) if(w.severity==WarnSeverity::Block) return true; return false; }
```
In `loadOverride` set `record_.sourceRelPath = <basename of glbPath>` (BundleExport rewrites to `<id>/<file>`). In `bindStock` set `record_.overrideClass="staticProp"` and **suggest** `record_.appearanceName` from the tgl basename (no extension/dir) but leave `record_.appearanceVerified=false`.

- [ ] **Step 6: Build + run validate smoke → PASS**
```bash
"$VIEWER" --smoke-workbench-validate tests/fixtures/asset_viewer/workbench
```

- [ ] **Step 7: Warnings UI + appearance-verify control (review #4/#7 labels)**
In `ModWorkbenchPanel.cpp` `draw`, after the delta lines:
```cpp
    // Appearance key the override replaces (must match engine appearance name).
    auto& rec = wb.record();
    static char appe[128] = "";
    if (appe[0]=='\0' && !rec.appearanceName.empty()) strncpy(appe, rec.appearanceName.c_str(), sizeof(appe)-1);
    ImGui::InputText("Appearance key", appe, sizeof(appe));
    rec.appearanceName = appe;
    ImGui::Checkbox("Appearance key verified (matches engine)", &rec.appearanceVerified);

    wb.revalidate();
    ImGui::Separator(); ImGui::TextUnformatted("Warnings:");
    if (wb.warnings().empty()) ImGui::TextDisabled("  none");
    for (const auto& w : wb.warnings()){
        bool blk = w.severity==WarnSeverity::Block;
        ImGui::TextColored(blk?ImVec4(1,0.4f,0.4f,1):ImVec4(1,0.8f,0.3f,1),
                           "  [%s] %s", blk?"BLOCK (mirror, advisory)":"WARN", w.message.c_str());
    }
```

- [ ] **Step 8: Build + commit**
```bash
git add tools/asset_viewer/OverrideManifest.* tools/asset_viewer/WorkbenchValidation.* \
        tools/asset_viewer/ModWorkbench.* tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.* tools/asset_viewer/main.cpp
git add -f tests/fixtures/asset_viewer/workbench/wb_valid.json tests/fixtures/asset_viewer/workbench/wb_bad_scale.json
git commit -m "feat(workbench): S3 validation (advisory BLOCK mirror + semantic WARN) + warnings/appearance UI"
```

---

## Task S4: Export DRAFT bundle + authoritative round-trip gate

**Files:** Modify `OverrideManifest.{h,cpp}` (`ToModelsJson`), create `BundleExport.{h,cpp}` (add to CMake), modify `ModWorkbench.{h,cpp}`, `ModWorkbenchPanel.cpp`.

> **Safety (review #6):** export writes ONLY `data/model_overrides/<id>/models.generated.json` + the copied GLB. It does **not** read or rewrite a central `data/model_overrides/models.json`. No existing modder data can be lost.

- [ ] **Step 1: `ToModelsJson` (hand-rolled, escaping-correct — review #3) + camelCase class (#5)**
Add to `OverrideManifest.h`:
```cpp
// Serialize to the engine's models.json shape {"overrides":[...]}. Hand-rolled
// (keeps nlohmann to the registry TU). Correctness is guaranteed at export by
// the registry round-trip; escaping is covered by S4's smoke.
std::string ToModelsJson(const std::vector<WorkbenchOverride>& recs);
```
Add to `OverrideManifest.cpp`:
```cpp
static std::string esc(const std::string& s){
    std::string o; o.reserve(s.size()+8);
    for(char c:s){ switch(c){
        case '"': o+="\\\""; break; case '\\': o+="\\\\"; break;
        case '\n': o+="\\n"; break; case '\t': o+="\\t"; break; case '\r': o+="\\r"; break;
        default: o+=c; } }
    return o;
}
std::string ToModelsJson(const std::vector<WorkbenchOverride>& recs){
    std::string o="{\n  \"overrides\": [\n";
    for (size_t i=0;i<recs.size();++i){ const auto& r=recs[i];
        o+="    {\"type\":\"model\",\"class\":\""+esc(r.overrideClass)+"\",";
        o+="\"replaces\":\""+esc(r.overrideClass+":"+r.appearanceName)+"\",";
        o+="\"source\":\""+esc(r.sourceRelPath)+"\",\"renderOnly\":true,\"scale\":1.0,\"fallback\":\"stock\"";
        if(!r.lods.empty()){ o+=",\"lods\":[";
            for(size_t j=0;j<r.lods.size();++j){ const auto& l=r.lods[j];
                o+="{\"lod\":"+std::to_string(l.lod)+",\"source\":\""+esc(l.sourceRelPath)+"\",\"distance\":"+std::to_string(l.distance)+"}";
                if(j+1<r.lods.size()) o+=","; }
            o+="]"; }
        o+="}"; if(i+1<recs.size()) o+=","; o+="\n"; }
    o+="  ]\n}\n"; return o;
}
```

- [ ] **Step 2: `BundleExport.{h,cpp}` STUB + add to CMake (red)**
`BundleExport.h`:
```cpp
#pragma once
#include "OverrideManifest.h"
#include <string>
struct ExportResult { bool ok=false; std::string message, bundleDir, manifestPath; };
// outRoot: the "data/model_overrides" dir. bundleId: subfolder. srcGlbPath: GLB to copy.
// Writes <outRoot>/<id>/{<glb>, models.generated.json}; rewrites rec.sourceRelPath to
// "<id>/<glb>"; refuses if a BLOCK rule trips OR the registry drops the record.
ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec);
```
`BundleExport.cpp` stub:
```cpp
#include "BundleExport.h"
ExportResult ExportBundle(const std::string&,const std::string&,const std::string&,WorkbenchOverride){
    ExportResult r; r.message="stub"; return r; }
```
Add `BundleExport.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 3: Failing export smoke (round-trip + escaping divergence — review #3/#7)**
`AssetViewerApp.h`: `static int runSmokeWorkbenchExport(const char* fixtureDir, const char* tmpDir);`
`AssetViewerApp.cpp`:
```cpp
#include "BundleExport.h"
#include <filesystem>
int AssetViewerApp::runSmokeWorkbenchExport(const char* fixtureDir, const char* tmpDir){
    namespace fs=std::filesystem;
    std::string root=std::string(tmpDir)+"/model_overrides"; fs::remove_all(root);
    std::string glb=std::string(fixtureDir)+"/unit_tri.gltf";
    WorkbenchOverride rec; rec.overrideClass="staticProp"; rec.appearanceName="example_name";
    rec.appearanceVerified=true; rec.scale=1.0f; rec.renderOnly=true; rec.fallback="stock";

    ExportResult r=ExportBundle(root,"example_id",glb,rec);
    if(!r.ok) return smokeFail((std::string("export: ")+r.message).c_str());
    if(!fs::exists(r.manifestPath)) return smokeFail("export: models.generated.json missing");
    if(!fs::exists(r.bundleDir+"/unit_tri.gltf")) return smokeFail("export: glb not copied");
    ModelOverrideRegistry reg; reg.loadFromFile(r.manifestPath, root);
    if(reg.resolve("staticProp","example_name")==nullptr) return smokeFail("export: registry did not resolve record");

    // BLOCK refusal: scale!=1.
    { auto bad=rec; bad.scale=2.0f; if(ExportBundle(root,"bad_scale",glb,bad).ok) return smokeFail("export: should refuse scale!=1"); }
    // Unverified appearance refusal (review #4).
    { auto bad=rec; bad.appearanceVerified=false; if(ExportBundle(root,"unverified",glb,bad).ok) return smokeFail("export: should refuse unverified appearance"); }
    // Invalid bundle id refusal.
    { if(ExportBundle(root,"../escape",glb,rec).ok) return smokeFail("export: should refuse bad bundle id"); }
    // Escaping divergence (review #3/#7): a quote/backslash in the appearance must
    // either escape correctly (registry resolves) — proving the writer is sound —
    // and must NEVER produce a malformed file that silently exports.
    { auto q=rec; q.appearanceName="a\"b\\c"; q.appearanceVerified=true;
      ExportResult er=ExportBundle(root,"escape_id",glb,q);
      if(er.ok){ ModelOverrideRegistry rr; rr.loadFromFile(er.manifestPath, root);
                 if(rr.resolve("staticProp","a\"b\\c")==nullptr) return smokeFail("export: escaped key did not round-trip"); }
      // if er.ok==false that's acceptable (refused), but it must not have written a resolvable-but-wrong record. }
    std::printf("[smoke] PASS workbench-export (round-trip + refusals + escaping)\n"); return 0;
}
```
`main.cpp`: `--smoke-workbench-export` dispatch. Build → FAIL (stub).

- [ ] **Step 4: Implement `BundleExport.cpp`**
```cpp
#include "BundleExport.h"
#include "model_override_registry.h"
#include <filesystem>
#include <fstream>
namespace fs=std::filesystem;
ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec){
    ExportResult res;
    if (bundleId.empty()||bundleId.find("..")!=std::string::npos||
        bundleId.find('/')!=std::string::npos||bundleId.find('\\')!=std::string::npos){ res.message="invalid bundle id"; return res; }
    if (!fs::exists(srcGlbPath)){ res.message="source GLB not found: "+srcGlbPath; return res; }
    std::string glbName=fs::path(srcGlbPath).filename().string();
    rec.sourceRelPath=bundleId+"/"+glbName;
    for (const auto& w: ValidateRecordRules(rec))
        if (w.severity==WarnSeverity::Block){ res.message="blocked: "+w.message; return res; }
    std::error_code ec;
    std::string dir=outRoot+"/"+bundleId;
    fs::create_directories(dir,ec); if(ec){ res.message="mkdir failed: "+ec.message(); return res; }
    fs::copy_file(srcGlbPath, dir+"/"+glbName, fs::copy_options::overwrite_existing, ec);
    if(ec){ res.message="copy failed: "+ec.message(); return res; }
    // Bundle-local manifest only — never touch a central models.json (review #6).
    std::string manifest=dir+"/models.generated.json";
    { std::vector<WorkbenchOverride> v{rec}; std::ofstream out(manifest, std::ios::binary);
      if(!out){ res.message="write manifest failed"; return res; } out<<ToModelsJson(v); }
    // Authoritative round-trip: source paths are relative to the manifest's dir.
    ModelOverrideRegistry check; check.loadFromFile(manifest, dir);
    if (check.resolve(rec.overrideClass.c_str(), rec.appearanceName.c_str())==nullptr){
        res.message="EXPORT BLOCKED BY REGISTRY (round-trip failed)"; return res; }
    res.ok=true; res.bundleDir=dir; res.manifestPath=manifest; res.message="exported draft "+rec.sourceRelPath;
    return res;
}
```
> Source paths are relative to the manifest dir, so the in-engine `models.json` that references this bundle must use `"<id>/<glb>"` relative to `data/model_overrides`. The generated bundle-local manifest validates the record in isolation; wiring it into the central manifest is the modder's explicit step (or the S5 safe-merge follow-up). The Export UI states this.
Build + run:
```bash
export TMP="build64/wb_tmp"
"$VIEWER" --smoke-workbench-export tests/fixtures/asset_viewer/workbench "$TMP"   # PASS
```

- [ ] **Step 5: ModWorkbench `exportBundle` + panel Export button**
`ModWorkbench.h` add `#include "BundleExport.h"`:
```cpp
    ExportResult exportBundle(const std::string& outRoot, const std::string& bundleId);
```
`ModWorkbench.cpp`:
```cpp
ExportResult ModWorkbench::exportBundle(const std::string& outRoot, const std::string& bundleId){
    revalidate();
    if (hasBlocking()){ ExportResult r; r.message="fix BLOCK warnings before export"; return r; }
    return ExportBundle(outRoot, bundleId, overridePath_, record_);
}
```
`ModWorkbenchPanel.cpp` draw, after warnings:
```cpp
    static char bundleId[128]="my_override";
    static char outRoot[260]="data/model_overrides";
    static std::string exportMsg;
    ImGui::InputText("Bundle id", bundleId, sizeof(bundleId));
    ImGui::InputText("Out root (model_overrides dir)", outRoot, sizeof(outRoot));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Export draft bundle")){
        ExportResult r=wb.exportBundle(outRoot,bundleId);
        exportMsg=(r.ok?"OK: ":"FAILED: ")+r.message;
    }
    ImGui::EndDisabled();
    if (!exportMsg.empty()) ImGui::TextWrapped("%s", exportMsg.c_str());
    ImGui::TextDisabled("Writes <out>/<id>/{model.glb, models.generated.json}. Does NOT edit your central models.json.");
```

- [ ] **Step 6: Build + commit**
```bash
git add tools/asset_viewer/OverrideManifest.* tools/asset_viewer/BundleExport.* \
        tools/asset_viewer/ModWorkbench.* tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.* tools/asset_viewer/main.cpp
git commit -m "feat(workbench): S4 export draft bundle (bundle-local models.generated.json) + registry round-trip gate"
```

---

## Task S5: Polish (only if time)

Independent; do any subset. Keep the BLOCK + round-trip gate and the bundle-local-only export intact.

- [ ] **Stock roster picker** — replace the free-text stock field with a list from `TglMeshLoader::listTgl()` / `ModelBrowser`. Still requires the user to confirm the **appearance key** (#4 stays). Commit.
- [ ] **Appearance-key roster (closes #4 properly)** — if an engine appearance-name list is reachable, offer it as a dropdown so `appearanceVerified` can be set with confidence instead of a manual checkbox. Commit.
- [ ] **LOD-chain panel** — UI to add `lods[]` (lod index + dropped GLB + distance); load each via `GlbMeshLoader`, show tri-count vs LOD0; `ValidateRecordRules` enforces ascending. Add `runSmokeWorkbenchLods`. Commit.
- [ ] **Texture-set panel** — reuse `MaterialSlots`; feed unresolved basenames into `revalidate(missingTextures)` so `texture-missing` WARNs fire. Commit.
- [ ] **Safe central merge (closes #6 fully)** — a registry-adjacent TU that parses the existing `models.json` with nlohmann, replaces/appends only the matching key, preserves all other records + unknown fields, and writes atomically. Smoke: export A, merge; export B, merge; assert both resolve and A survived. Only after this is proven should any UI offer "merge into central models.json". Commit.

---

## Self-Review (post-review-patch)

**Spec + review coverage:**
- link registry/assimp → S0 ✓; include-audit gate added (engine-isolation) ✓
- GLB→MeshData → S1 ✓; **fixture strengthened to all-axes (#8)** ✓; **generation counter (#9)** ✓
- drag-in → S1 ✓
- stock bind + overlay → S2 ✓; **approximate-preview banner (should-patch)** ✓; **review checkpoint after S2** ✓
- BLOCK rules (advisory) + authoritative round-trip, **labels distinguished (#7)** ✓; **mirror⇔registry agreement + escaping-divergence smokes (#3/#7)** ✓
- bounds/pivot/material/LOD/overdraw → S3 ✓
- export → S4 **draft, bundle-local only, no central rewrite (#6)** ✓; **refusals: BLOCK, unverified appearance, bad id (#4)** ✓
- **class casing locked to camelCase emit + case-insensitive compare (#5)** ✓
- **JSON hand-rolled with escaping tests; registry round-trip is the guarantee (#3)** ✓
- **commit trailer mandate removed (#1)** ✓
- **CMake sources added per-creating-task, never pre-listed (#2)** ✓
- hard exclusions intact (Backend A / impostor authoring / auto-LOD / collision override / non-unit scale [actively BLOCKED] / mech-anim) ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code or an explicit failing stub. Documented MVP simplifications: untextured override preview (S2) and bundle-local manifest (S4) — both deliberate, with S5 follow-ups.

**Type consistency:** `MeshData`/`SubMesh`/`MeshVertex` verbatim; `Warning`/`WarnSeverity` single definition; `WorkbenchOverride` single definition with `appearanceVerified`; `AssetType::ModWorkbench`/`setActive` introduced S2, forward-use in S1 flagged; registry `loadFromFile`/`resolve` per header. `ModWorkbench::generation()` used consistently by the panel.
