# Asset Viewer Backend-A v2 — Engine-Shader Preview — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `ModelPreviewEngineShader` `PreviewSurface` that renders one static prop with the real `shaders/static_prop.{vert,frag}` + a standalone light/shadow stub, behind a UI toggle vs Backend-B, failing open to Backend-B on any error.

**Architecture:** Reference (do not copy, do not edit) the engine static-prop shaders, resolve their `#include`s against the real `shaders/` tree, compile them in a minimal `#define` config, satisfy their scene bindings with isolated `StandaloneSceneStubs` (the v3 seam), replicate only the `StaticPropOpaque` GL pipeline state, and render into the existing Backend-B FBO→ImGui::Image path. No RenderCore/batcher link.

**Tech Stack:** C++17, SDL2/GLEW/OpenGL 4.x, Dear ImGui. Existing substrate: `MeshGpu`, `MeshPreview3D` (Backend-B reference pattern), `TglMeshLoader`, `PreviewSurface`, the `runSmoke*`/`--smoke-*` idiom, `MeshPreview3D::renderToPixels` (headless FBO test hook).

---

## HARD CONSTRAINTS (from the spec — read before any task)

- **NO SHADER EDITS.** Never modify `shaders/static_prop.vert`, `.frag`, or any `shaders/include/*.hglsl` to make the tool compile. The only levers are (a) the `#define` prefix in the minimal config and (b) the stub bindings the tool provides. If a shader genuinely cannot compile standalone without a source change, **STOP and report BLOCKED** — do not patch the shader.
- **FAIL OPEN.** Any Backend-A failure (missing shader, unresolved include, compile/link error, missing texture) must surface in the contract report and fall back to rendering Backend-B for that frame. Never crash the viewer.
- **Backend-B stays the default** and is never modified behaviorally.
- **No pixel-parity claims** anywhere in UI or logs.

## Grounding facts (verified against current code)

- Engine shaders: `shaders/static_prop.vert` (always `#include <include/lighting.hglsl>` at line 21, which declares `LightsData` SSBO at **binding 20**, struct `ObjectLights{ mat4 light_to_world[16]; vec4 light_dir[16]; vec4 light_color[16]; vec4 light_falloff[16]; ivec4 numLights; ... }`), `shaders/static_prop.frag` (always `#include <include/render_contract.hglsl>`). Includes use angle-bracket form `#include <include/...>` resolved relative to the `shaders/` root.
- Vert SSBOs: binding 0 `Instances` (`mat4 modelMatrix; uint typeID,firstColorOffset,flags,lightDataIndex; vec4 aRGBHighlight,fogRGB`), 1 `Colors` (uint[]), 2 `PerType` (`vec4 hotPinkRGB,hotYellowRGB,hotGreenRGB`), 3 `ParityOut` (gated by `u_parityWrite<=0`, harmless when bound empty/unused). Vert attrs: loc0 `vec3 a_position`, loc1 `vec3 a_normal`, loc2 `vec2 a_uv`, loc3 `uint a_localVertexID`, loc4 `uint a_aRGBLight`.
- Frag (legacy lane, NOT `MC2_COALESCE`): `sampler2D u_tex`, `int u_materialFlags`, `int u_maxLocalVertexID`, `int u_packetID`, `int u_materialGpuSample` (0=legacy), `MaterialTable` SSBO binding 5 (may be unbound when sampling off), `float u_fogValue`, `int u_debugAddrMode/u_debugMaterialMode`, `float u_pbrV1Strength` (0=off) + `u_pbrV1RoughnessOverride,u_pbrV1DiagSunFound`. **Outputs MRT: `layout(location=0) FragColor`, `layout(location=1) GBuffer1`** → the FBO needs 2 color attachments.
- Minimal-config candidate define set (hypothesis; Task 2 pins it empirically): **omit** `MC2_USE_VIEW_UNIFORMS`, `MC2_COALESCE`, `MC2_STATICPROP_PBR_SLOTS`, `MC2_OBJECT_ID_BUFFER`, `MC2_OBJECT_PARITY_CHECK`. (The vert's own `#define MC2_STATIC_PROP_LIGHTING` at line 20 stays — it is in the shader, not ours to remove.) Engine quirk: `uniform uint` crashes the shader compiler (`memory/uniform_uint_crash.md`) — the shaders already use `int`; do not introduce `uint` uniforms in any tool-side injected define.
- `StaticPropOpaque` pipeline state (`RenderCore/PipelineDesc.h:50-71`): depth test ON, depthFunc **GREATER_EQUAL (reverse-Z)**, depth write ON, cull BACK, blend Opaque (off), color attachments {Color0, GBuffer1}.
- Backend-B render pattern (`MeshPreview3D.cpp`): `compileShader`/`linkProgram` static helpers (165/181), `ensureGL` builds prog+FBO (217), `renderScene` binds FBO/sets state/clears/draws/restores (363), `renderToPixels` headless hook (550). It uses **normal-Z** (`glDepthFunc(GL_LESS)`, clear depth 1.0) — Backend-A must NOT copy this; see Task 5 pipeline-state checklist.
- `MeshGpu`: per-submesh VAO/VBO/EBO + albedo; `bmin()/bmax()` bounds; `drawLit(uHasAlbedoLoc, showLights)`. Vertex layout confirmed in Task 5 against `MeshGpu.cpp`.
- Static-prop panel host: `AssetViewerApp.cpp` `AssetType::StaticProps` case (~63/94) owns the `MeshPreview3D`.

## File structure

| File | Action | Responsibility |
|------|--------|----------------|
| `tools/asset_viewer/ShaderIncludeResolver.{h,cpp}` | create | recursively inline `#include <...>`/`"..."` from a `shaders/` root; cycle-guarded; report unresolved |
| `tools/asset_viewer/StandaloneSceneStubs.{h,cpp}` | create | build the binding-20 `LightsData` stub SSBO (1 `ObjectLights`: ambient+1 infinite), Instances/Colors/PerType stub SSBOs, 1×1 white shadow tex; the v3 seam |
| `tools/asset_viewer/ShaderContractReport.{h,cpp}` | create | struct holding shader paths/compile+link logs/active defines/introspected bindings+uniforms/stub flags; an ImGui draw fn |
| `tools/asset_viewer/ModelPreviewEngineShader.{h,cpp}` | create | `PreviewSurface` impl: resolve+compile engine shaders, bind stubs+albedo+geometry, render MRT FBO, fail-open |
| `tools/asset_viewer/MeshPreview3D.{h,cpp}` | reference only | Backend-B; unchanged |
| `AssetViewerApp.cpp` (StaticProps panel) | modify | backend toggle (default B), contract report, fail-open wiring |
| `AssetViewerApp.{h,cpp}`, `main.cpp` | modify | 3 new `--smoke-backend-a-*` hooks |
| `tools/asset_viewer/CMakeLists.txt` | modify | add the new `.cpp`s |
| `PreviewSurface.h` | modify | update the v2-name comment |
| `docs/asset-viewer-backend-a-shader-contract.md` | create (Task 2) | the empirical compile contract artifact |

If `ModelPreviewEngineShader.cpp` exceeds ~250 lines, split the shader-build half into `ModelPreviewEngineShader_compile.cpp`.

---

## Task 1: ShaderIncludeResolver + unit smoke

Dependency-correct ordering note: the compile-contract spike (Task 2) cannot run without include resolution, so the resolver lands first. Task 2 then produces the define/binding contract the user asked to pin before rendering — no rendering happens until Task 2.

**Files:** Create `tools/asset_viewer/ShaderIncludeResolver.{h,cpp}`; create fixtures `tools/asset_viewer/tests/fixtures/asset_viewer/shaderinc/{root.glsl,inc/a.hglsl,inc/b.hglsl,inc/cycle.hglsl}`; modify `CMakeLists.txt`, `main.cpp`, `AssetViewerApp.{h,cpp}`.

- [ ] **Step 1: Fixtures**

`shaderinc/inc/a.hglsl`:
```glsl
// a
#include <inc/b.hglsl>
int a_val() { return 1 + b_val(); }
```
`shaderinc/inc/b.hglsl`:
```glsl
int b_val() { return 10; }
```
`shaderinc/inc/cycle.hglsl`:
```glsl
#include <inc/cycle.hglsl>
int cyc() { return 0; }
```
`shaderinc/root.glsl`:
```glsl
#version 330
#include <inc/a.hglsl>
void main() { int x = a_val(); }
```
Expected resolve of `root.glsl`: include directives replaced by file contents (b before a's body), no remaining `#include` lines, `b_val`+`a_val` present. Resolving `cycle.hglsl` must terminate (cycle guard) and report the cycle, not infinite-loop.

- [ ] **Step 2: Header `ShaderIncludeResolver.h`**

```cpp
// tools/asset_viewer/ShaderIncludeResolver.h
// Minimal GLSL #include inliner rooted at a shaders/ directory. Resolves both
// #include <path> and #include "path" relative to the root. Cycle-guarded.
// Used by Backend-A to compile the real engine shaders without copying includes.
#pragma once
#include <string>
#include <vector>

struct ShaderResolveResult {
    bool ok = false;
    std::string source;                       // fully inlined GLSL (empty on failure)
    std::vector<std::string> includedFiles;   // resolved include paths, in first-seen order
    std::vector<std::string> unresolved;      // includes that could not be found
    std::string error;                        // first fatal error (cycle / missing root)
};

// Read `entryFile` and inline its includes, searching relative to `shaderRoot`.
// `entryFile` may be absolute or relative to shaderRoot.
ShaderResolveResult ResolveShaderIncludes(const std::string& shaderRoot,
                                          const std::string& entryFile);
```

- [ ] **Step 3: Failing smoke**

`AssetViewerApp.h` (near other smoke decls):
```cpp
    static int runSmokeShaderInclude(const char* fixtureDir);  // Backend-A v2
```
`main.cpp` dispatch (after the last existing smoke dispatch):
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-shader-include") == 0)
        return AssetViewerApp::runSmokeShaderInclude(argc >= 3 ? argv[2] : ".");
```
`AssetViewerApp.cpp` body (add `#include "ShaderIncludeResolver.h"`):
```cpp
int AssetViewerApp::runSmokeShaderInclude(const char* fixtureDir) {
    std::string root = std::string(fixtureDir) + "/shaderinc";
    ShaderResolveResult r = ResolveShaderIncludes(root, "root.glsl");
    bool ok = r.ok
           && r.source.find("#include") == std::string::npos
           && r.source.find("b_val") != std::string::npos
           && r.source.find("a_val") != std::string::npos
           && r.includedFiles.size() == 2
           && r.unresolved.empty();
    // Cycle must terminate and be reported, not hang.
    ShaderResolveResult c = ResolveShaderIncludes(root, "inc/cycle.hglsl");
    ok &= (!c.error.empty());
    printf("[smoke] shader-include %s (includes=%zu)\n", ok ? "PASS" : "FAIL",
           r.includedFiles.size());
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: Add to CMake + build → verify link FAIL**

Add `ShaderIncludeResolver.cpp` to `mc2_asset_viewer` sources in `CMakeLists.txt` (reconfigure since CMakeLists changed). Build:
```
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -G "Visual Studio 17 2022" -A x64 -S tools/asset_viewer -B build64 -DCMAKE_PREFIX_PATH="A:/Games/mc2-opengl-src/3rdparty/3rdparty" -DCMAKE_LIBRARY_PATH="A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64" -DGLEW_INCLUDE_DIR="A:/Games/mc2-opengl-src/3rdparty/3rdparty/include" -DGLEW_SHARED_LIBRARY_RELEASE="A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64/glew32.lib" -DGLEW_STATIC_LIBRARY_RELEASE="A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64/glew32s.lib"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: link error `ResolveShaderIncludes` unresolved.

- [ ] **Step 5: Implement `ShaderIncludeResolver.cpp`**

```cpp
// tools/asset_viewer/ShaderIncludeResolver.cpp
#include "ShaderIncludeResolver.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>

namespace fs = std::filesystem;

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

// Parse an #include directive's path from a line; returns "" if not an include.
static std::string includePath(const std::string& line) {
    size_t h = line.find('#');
    if (h == std::string::npos) return "";
    size_t inc = line.find("include", h);
    if (inc == std::string::npos) return "";
    size_t lt = line.find_first_of("<\"", inc);
    if (lt == std::string::npos) return "";
    char close = (line[lt] == '<') ? '>' : '"';
    size_t end = line.find(close, lt + 1);
    if (end == std::string::npos) return "";
    return line.substr(lt + 1, end - lt - 1);
}

static bool inlineFile(const fs::path& root, const fs::path& file,
                       std::string& out, std::set<std::string>& stack,
                       ShaderResolveResult& res) {
    std::string key = fs::weakly_canonical(file).string();
    if (stack.count(key)) { res.error = "include cycle at " + file.string(); return false; }
    std::string text;
    if (!readFile(file, text)) { res.unresolved.push_back(file.string()); return false; }
    res.includedFiles.push_back(file.string());
    stack.insert(key);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string inc = includePath(line);
        if (!inc.empty()) {
            fs::path target = root / inc;          // includes are root-relative
            if (!inlineFile(root, target, out, stack, res)) {
                if (!res.error.empty()) return false;   // cycle = fatal
                out += "// [unresolved include: " + inc + "]\n";
            }
            continue;
        }
        out += line; out += '\n';
    }
    stack.erase(key);
    return true;
}

ShaderResolveResult ResolveShaderIncludes(const std::string& shaderRoot,
                                          const std::string& entryFile) {
    ShaderResolveResult res;
    fs::path root = shaderRoot;
    fs::path entry = fs::path(entryFile).is_absolute() ? fs::path(entryFile) : root / entryFile;
    std::set<std::string> stack;
    std::string out;
    res.includedFiles.clear();
    bool ok = inlineFile(root, entry, out, stack, res);
    if (!res.error.empty()) { res.ok = false; return res; }
    res.source = out;
    res.ok = ok && res.unresolved.empty();
    return res;
}
```
Note: the entry file itself is counted in `includedFiles`; the smoke expects `includedFiles.size()==2` for `root.glsl`→a→b, so adjust the assertion to the actual count after Step 6 if the entry is counted (root,a,b = 3). **Pick one convention and make the smoke match it**: this impl counts the entry too, so change the Step-3 assertion to `r.includedFiles.size() == 3`. (Do this in Step 6 before running.)

- [ ] **Step 6: Fix the smoke count to match the impl, build, run → PASS**

Edit the Step-3 smoke: `r.includedFiles.size() == 3` (root.glsl + a.hglsl + b.hglsl). Rebuild and run:
```
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-shader-include tools/asset_viewer/tests/fixtures/asset_viewer
```
Expected: `[smoke] shader-include PASS (includes=3)`.

- [ ] **Step 7: Commit**

```bash
git add tools/asset_viewer/ShaderIncludeResolver.h tools/asset_viewer/ShaderIncludeResolver.cpp tools/asset_viewer/CMakeLists.txt tools/asset_viewer/main.cpp tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/tests/fixtures/asset_viewer/shaderinc
git commit -m "feat(asset-viewer): GLSL #include resolver for engine shaders (Backend-A v2)"
```

---

## Task 2: Compile-contract spike (`--smoke-backend-a-compile`) + contract doc

This is the plan-time spike the spec mandates. **No rendering, no UI.** It resolves + compiles + links the real `static_prop.{vert,frag}` in the minimal config, iterates the define set until link succeeds, introspects the program, and writes the contract artifact. Everything downstream consumes this.

**Files:** modify `AssetViewerApp.{h,cpp}`, `main.cpp`; create `docs/asset-viewer-backend-a-shader-contract.md`.

- [ ] **Step 1: Locate a shader root**

The spike takes a `shaderRoot` arg = a dir containing `static_prop.vert`, `static_prop.frag`, and `include/`. Use the repo `shaders/` dir (relative to the worktree root) or a deploy `shaders/`. Confirm it exists: `ls shaders/static_prop.vert shaders/include/lighting.hglsl shaders/include/render_contract.hglsl`.

- [ ] **Step 2: Smoke decl + dispatch**

`AssetViewerApp.h`:
```cpp
    static int runSmokeBackendACompile(const char* shaderRoot);  // Backend-A v2
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-backend-a-compile") == 0)
        return AssetViewerApp::runSmokeBackendACompile(argc >= 3 ? argv[2] : "shaders");
```

- [ ] **Step 3: Spike body**

Add to `AssetViewerApp.cpp`. It needs a GL context — reuse the same headless SDL/GL bring-up the existing GL smokes use (grep `runSmokeMeshRender` for the offscreen-context pattern and copy its context setup; e.g. it calls a shared helper or sets up SDL hidden window + GL 4.x). Prefix the resolved source with the minimal `#define` block + a `#version` line matching the engine (grep the top of another compiled engine shader or the gos shader loader for the version directive; static_prop.* may rely on the loader prepending `#version` — if so, prepend `#version 430` since the SSBOs/`std430` need ≥430).

```cpp
int AssetViewerApp::runSmokeBackendACompile(const char* shaderRoot) {
    // --- headless GL context (copy the pattern from runSmokeMeshRender) ---
    // ... create hidden SDL window + GL 4.x core context, glewInit ...   // [see runSmokeMeshRender]

    auto buildStage = [&](const char* file, GLenum type, std::string& logOut,
                          std::vector<std::string>& incOut) -> unsigned {
        ShaderResolveResult r = ResolveShaderIncludes(shaderRoot, file);
        incOut = r.includedFiles;
        if (!r.ok) { logOut = "resolve failed: " + r.error +
            (r.unresolved.empty()? "" : (" unresolved=" + r.unresolved.front())); return 0; }
        // Minimal-config define prefix. Inserted AFTER #version if the file has one,
        // else prepended. static_prop.* expect the loader to supply #version; prepend 430.
        std::string defs =
            "#version 430\n"
            "// Backend-A minimal config: legacy lane, no view-uniforms/coalesce/PBR-slots.\n";
        std::string src = defs + r.source;
        unsigned sh = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) { std::string l(len, '\0'); glGetShaderInfoLog(sh, len, nullptr, l.data()); logOut = l; }
        if (!ok) { glDeleteShader(sh); return 0; }
        return sh;
    };

    std::string vlog, flog; std::vector<std::string> vinc, finc;
    unsigned vs = buildStage("static_prop.vert", GL_VERTEX_SHADER,   vlog, vinc);
    unsigned fs = buildStage("static_prop.frag", GL_FRAGMENT_SHADER, flog, finc);
    unsigned prog = 0; std::string linkLog; bool linked = false;
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); linked = ok != 0;
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) { std::string l(len, '\0'); glGetProgramInfoLog(prog, len, nullptr, l.data()); linkLog = l; }
    }
    // Introspect active uniforms + SSBO blocks (for the contract doc).
    if (linked) {
        GLint nu = 0; glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
        for (GLint i = 0; i < nu; ++i) {
            char nm[128]; GLint sz; GLenum tp; GLsizei wr;
            glGetActiveUniform(prog, i, sizeof nm, &wr, &sz, &tp, nm);
            printf("[contract] uniform %s\n", nm);
        }
        GLint nb = 0; glGetProgramInterfaceiv(prog, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &nb);
        for (GLint i = 0; i < nb; ++i) {
            char nm[128]; GLsizei wr;
            glGetProgramResourceName(prog, GL_SHADER_STORAGE_BLOCK, i, sizeof nm, &wr, nm);
            printf("[contract] ssbo-block %s\n", nm);
        }
    }
    if (!vs) printf("[contract] VERT FAIL log:\n%s\n", vlog.c_str());
    if (!fs) printf("[contract] FRAG FAIL log:\n%s\n", flog.c_str());
    if (vs && fs && !linked) printf("[contract] LINK FAIL log:\n%s\n", linkLog.c_str());
    printf("[smoke] backend-a-compile %s\n", linked ? "PASS" : "FAIL");
    // ... teardown GL context ...
    return linked ? 0 : 1;
}
```

- [ ] **Step 4: Run, iterate the define set until link succeeds**

```
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-backend-a-compile shaders
```
Read the `[contract]` + FAIL logs. Adjust ONLY the `defs` `#define`/`#version` prefix (NEVER the shader files) until `[smoke] backend-a-compile PASS`. Likely iterations: add `#version 430` (done), possibly toggle a define the shader expects. **If a shader cannot link without editing shader source, STOP — report BLOCKED** with the exact compiler log (this is the escalation the constraint requires).

- [ ] **Step 5: Write the contract artifact**

Create `docs/asset-viewer-backend-a-shader-contract.md` capturing, verbatim from the passing run: the final `#version` + `#define` prefix; the full include list (vinc+finc); every `[contract] uniform` name; every `[contract] ssbo-block` name (expect `Instances`/`Colors`/`PerType`/`LightsData`/possibly `ParityOut`/`MaterialTable`); the MRT outputs (FragColor loc0, GBuffer1 loc1); and which defines were OMITTED. This doc is the input to Tasks 4–5. Add a line: "Regenerate with `--smoke-backend-a-compile shaders`."

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp docs/asset-viewer-backend-a-shader-contract.md
git commit -m "feat(asset-viewer): Backend-A compile-contract spike pins minimal shader config"
```

---

## Task 3: ShaderContractReport model

**Files:** create `tools/asset_viewer/ShaderContractReport.{h,cpp}`; modify `CMakeLists.txt`. No smoke (pure data + ImGui draw; exercised by Task 6).

- [ ] **Step 1: Header**

```cpp
// tools/asset_viewer/ShaderContractReport.h
// Diagnostic snapshot of a Backend-A shader build + bound stubs. Displayed in the
// static-prop panel so failures are visible (fail-open contract).
#pragma once
#include <string>
#include <vector>
struct ImVec2;

struct ShaderContractReport {
    std::string shaderRoot;
    std::string vertPath, fragPath;
    bool   compileOk = false;
    bool   linkOk    = false;
    std::string compileLog, linkLog;
    std::string activeDefines;
    std::vector<std::string> includedFiles;
    std::vector<std::string> unresolvedIncludes;
    std::string textureMode = "legacy sampler2D u_tex";
    bool   objectLightsStubActive = false;
    bool   shadowStubActive       = false;
    bool   fogDisabled            = true;
    std::string lastError;         // non-empty => Backend-A fell open to Backend-B

    bool ok() const { return compileOk && linkOk && lastError.empty(); }
};

// Render the report as a collapsible ImGui section.
void DrawShaderContractReport(const ShaderContractReport& r);
```

- [ ] **Step 2: Implementation**

```cpp
// tools/asset_viewer/ShaderContractReport.cpp
#include "ShaderContractReport.h"
#include "imgui.h"

void DrawShaderContractReport(const ShaderContractReport& r) {
    if (!ImGui::CollapsingHeader("Shader Contract (Backend-A)")) return;
    ImGui::Text("status: %s", r.ok() ? "OK" : "ERROR (fell open to Backend-B)");
    ImGui::TextWrapped("root: %s", r.shaderRoot.c_str());
    ImGui::TextWrapped("vert: %s  frag: %s", r.vertPath.c_str(), r.fragPath.c_str());
    ImGui::Text("compile: %s   link: %s", r.compileOk ? "ok" : "FAIL", r.linkOk ? "ok" : "FAIL");
    ImGui::TextWrapped("defines: %s", r.activeDefines.c_str());
    ImGui::Text("texture mode: %s", r.textureMode.c_str());
    ImGui::Text("stubs: ObjectLights=%s shadow=%s fog=%s",
        r.objectLightsStubActive ? "on" : "off",
        r.shadowStubActive ? "on" : "off",
        r.fogDisabled ? "disabled" : "on");
    if (!r.unresolvedIncludes.empty()) {
        ImGui::TextColored(ImVec4(1,0.5f,0.5f,1), "unresolved includes:");
        for (auto& u : r.unresolvedIncludes) ImGui::BulletText("%s", u.c_str());
    }
    if (!r.compileLog.empty()) { ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "compile log:"); ImGui::TextWrapped("%s", r.compileLog.c_str()); }
    if (!r.linkLog.empty())    { ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "link log:");    ImGui::TextWrapped("%s", r.linkLog.c_str()); }
    if (!r.lastError.empty())  ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "error: %s", r.lastError.c_str());
}
```

- [ ] **Step 3: CMake + build clean + commit**

Add `ShaderContractReport.cpp` to `CMakeLists.txt`. Build (`--build build64 ...`). Expected clean.
```bash
git add tools/asset_viewer/ShaderContractReport.h tools/asset_viewer/ShaderContractReport.cpp tools/asset_viewer/CMakeLists.txt
git commit -m "feat(asset-viewer): Backend-A shader contract report model + ImGui view"
```

---

## Task 4: StandaloneSceneStubs (the v3 seam)

**Files:** create `tools/asset_viewer/StandaloneSceneStubs.{h,cpp}`; modify `CMakeLists.txt`. **Consumes the SSBO block list + layouts from Task 2's contract doc.** The `ObjectLights` struct layout MUST mirror `shaders/include/lighting.hglsl` exactly (`mat4 light_to_world[16]; vec4 light_dir[16]; vec4 light_color[16]; vec4 light_falloff[16]; ivec4 numLights;` — confirm the full tail against the .hglsl, the dump was truncated). std430. No smoke (GL resource builder; exercised by Task 5's render smoke).

- [ ] **Step 1: Header**

```cpp
// tools/asset_viewer/StandaloneSceneStubs.h
// Backend-A v2 SEAM: the scene-coupled inputs static_prop.{vert,frag} require but a
// standalone tool has no mission for. v3 replaces these with real mission feeders
// WITHOUT touching the render path. Provides: a 1-entry LightsData SSBO (binding 20:
// ambient + one infinite directional), minimal Instances/Colors/PerType SSBOs
// (bindings 0/1/2: one identity instance, lightDataIndex 0), a 1x1 white shadow tex.
#pragma once
#include <cstdint>

class StandaloneSceneStubs {
public:
    // Build all GL resources. lightDirWorld = direction the directional light travels.
    void build(const float lightDirWorld[3], const float lightColor[3], float ambient);
    // Bind the SSBOs to their binding points (0,1,2,20) and the shadow tex (if the
    // shader samples it; bind to the unit the contract names). modelMatrix = the prop's
    // world transform (column-major 16 floats).
    void bind(const float modelMatrix[16]) const;
    void destroy();
    bool valid() const { return built_; }
private:
    unsigned instancesSsbo_ = 0, colorsSsbo_ = 0, perTypeSsbo_ = 0, lightsSsbo_ = 0;
    unsigned shadowTex_ = 0;
    bool built_ = false;
};
```

- [ ] **Step 2: Implementation**

Mirror the GLSL structs EXACTLY (verify against `shaders/include/lighting.hglsl` and the vert's `Instance`/`PerTypeData` structs). Build one `ObjectLights` with `numLights=1` ambient + a second infinite light if the contract shows `calc_light` needs an INFINITE entry for any visible shading; start with ambient + 1 INFINITE (type in `light_dir[i].w`). Bind Instances(0)/Colors(1)/PerType(2)/LightsData(20) via `glBindBufferBase`. Build a 1×1 white `GL_DEPTH_COMPONENT`/`GL_TEXTURE_2D` configured as `sampler2DShadow` (depth compare mode) if `render_contract.hglsl` samples a shadow map — confirm from the contract whether a shadow sampler is active; if not, skip the shadow tex and set `shadowStubActive=false`.

```cpp
// tools/asset_viewer/StandaloneSceneStubs.cpp
#include "StandaloneSceneStubs.h"
#include <GL/glew.h>
#include <cstring>

namespace {
struct GpuInstance { float modelMatrix[16]; uint32_t typeID, firstColorOffset, flags, lightDataIndex;
                     float aRGBHighlight[4]; float fogRGB[4]; };
struct GpuPerType  { float hotPink[4], hotYellow[4], hotGreen[4]; };
// MUST match shaders/include/lighting.hglsl ObjectLights EXACTLY (std430). Confirm tail.
struct GpuObjectLights {
    float light_to_world[16][16];   // mat4[16]
    float light_dir[16][4];
    float light_color[16][4];
    float light_falloff[16][4];
    int32_t numLights[4];           // ivec4
    // NOTE: if lighting.hglsl declares more tail members, ADD them here (lockstep).
};
}

void StandaloneSceneStubs::build(const float dir[3], const float col[3], float ambient) {
    GpuInstance inst{}; // identity model; lightDataIndex 0; everything else 0
    for (int i=0;i<16;++i) inst.modelMatrix[i] = (i%5==0)?1.0f:0.0f;
    GpuObjectLights L{};
    // light[0]: index 0 = AMBIENT (type 0 in .w), index 1 = INFINITE (type 1).
    L.numLights[0] = 2;
    L.light_color[0][0]=ambient; L.light_color[0][1]=ambient; L.light_color[0][2]=ambient; L.light_dir[0][3]=0; // AMBIENT
    L.light_dir[1][0]=dir[0]; L.light_dir[1][1]=dir[1]; L.light_dir[1][2]=dir[2]; L.light_dir[1][3]=1; // INFINITE
    L.light_color[1][0]=col[0]; L.light_color[1][1]=col[1]; L.light_color[1][2]=col[2];
    GpuPerType pt{}; uint32_t color0 = 0xFFFFFFFFu;
    auto mk=[&](unsigned& b, const void* d, GLsizeiptr n){ glGenBuffers(1,&b); glBindBuffer(GL_SHADER_STORAGE_BUFFER,b);
        glBufferData(GL_SHADER_STORAGE_BUFFER,n,d,GL_STATIC_DRAW); };
    mk(instancesSsbo_, &inst, sizeof inst);
    mk(colorsSsbo_,    &color0, sizeof color0);
    mk(perTypeSsbo_,   &pt,   sizeof pt);
    mk(lightsSsbo_,    &L,    sizeof L);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    // 1x1 white shadow depth tex (only used if the shader samples a shadow map).
    glGenTextures(1, &shadowTex_);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    float one = 1.0f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    built_ = true;
}

void StandaloneSceneStubs::bind(const float modelMatrix[16]) const {
    // Re-upload the instance model matrix each frame (orbit doesn't change it, but the
    // prop's own transform might). Cheap; keeps the seam simple.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, instancesSsbo_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float)*16, modelMatrix);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instancesSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, colorsSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, perTypeSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, lightsSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void StandaloneSceneStubs::destroy() {
    unsigned b[4] = {instancesSsbo_, colorsSsbo_, perTypeSsbo_, lightsSsbo_};
    glDeleteBuffers(4, b);
    if (shadowTex_) glDeleteTextures(1, &shadowTex_);
    instancesSsbo_=colorsSsbo_=perTypeSsbo_=lightsSsbo_=shadowTex_=0; built_=false;
}
```

- [ ] **Step 3: CMake + build clean + commit**

Add to `CMakeLists.txt`, build clean.
```bash
git add tools/asset_viewer/StandaloneSceneStubs.h tools/asset_viewer/StandaloneSceneStubs.cpp tools/asset_viewer/CMakeLists.txt
git commit -m "feat(asset-viewer): StandaloneSceneStubs — Backend-A scene-binding seam (v3 swap point)"
```

---

## Task 5: ModelPreviewEngineShader + render smoke

**Files:** create `tools/asset_viewer/ModelPreviewEngineShader.{h,cpp}`; modify `CMakeLists.txt`, `AssetViewerApp.{h,cpp}`, `main.cpp`. Consumes Task 2 contract + Task 4 stubs. Reuses the Backend-B FBO pattern but with an **MRT (2-attachment) FBO** and the **`StaticPropOpaque` pipeline state**.

### Pipeline-state checklist (replicate StaticPropOpaque — do NOT copy Backend-B's normal-Z defaults)

```
glViewport(0,0,w,h)
glEnable(GL_DEPTH_TEST)
glDepthFunc(GL_GEQUAL)          // reverse-Z (verify GEQUAL vs GREATER from the contract / PipelineDesc)
glDepthMask(GL_TRUE)
glEnable(GL_CULL_FACE); glCullFace(GL_BACK)
glDisable(GL_BLEND)             // opaque
glClearDepth(0.0f)              // reverse-Z clear (NOT 1.0 — Backend-B uses 1.0; do not copy)
glClearColor(<panel bg>); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
// build the orbit worldToClipGL with a reverse-Z projection consistent with GEQUAL+clear0
```
The projection MUST be reverse-Z (near maps to depth 1, far to 0) to match `GEQUAL` + `glClearDepth(0)`. Save/restore all of these around the draw exactly as `MeshPreview3D::renderScene` saves/restores its state (it captures `prevDepthFunc`, `wasCull`, `wasBlend`, `prevClear`, `prevFbo`, `prevVp`). Add depth-write + clear-depth to that save/restore set.

- [ ] **Step 1: Header**

```cpp
// tools/asset_viewer/ModelPreviewEngineShader.h
// Backend-A v2: PreviewSurface rendering one static prop with the REAL engine
// static_prop.{vert,frag} (referenced, not copied) + StandaloneSceneStubs. Faithful
// to engine shading/material/pipeline-state; NOT mission-lighting exact. Fails open:
// on any compile/link/resource error, ok()==false and the caller renders Backend-B.
#pragma once
#include "PreviewSurface.h"
#include "MeshGpu.h"
#include "StandaloneSceneStubs.h"
#include "ShaderContractReport.h"
#include <string>
#include <vector>
#include <cstdint>

class ModelPreviewEngineShader : public PreviewSurface {
public:
    ~ModelPreviewEngineShader() override;
    void setShaderRoot(const std::string& dir) { shaderRoot_ = dir; }
    void setDeployDir(const std::string& dir)  { deployDir_ = dir; }
    void setSource(const std::string& tglName) override;   // loads prop into MeshGpu
    void draw(const ImVec2& availableSize) override;       // renders; falls open if !ok
    const char* label() const override { return "Engine Shader (Backend-A)"; }

    bool ok() const { return report_.ok(); }               // false => caller draws Backend-B
    const ShaderContractReport& report() const { return report_; }

    // Headless test hook (no ImGui): ensure built, render to an FBO, read back RGBA.
    bool renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut);

    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
private:
    bool ensureProgram();                 // resolve+compile+link; populates report_
    void ensureFbo(int w, int h);         // MRT: Color0 + GBuffer1 + depth
    void renderScene(int w, int h);
    void buildReverseZViewProj(int w, int h, float out[16], float camPos[3]) const;

    std::string shaderRoot_ = "shaders", deployDir_ = ".", tglName_;
    unsigned prog_ = 0;
    unsigned fbo_ = 0, color0_ = 0, gbuffer1_ = 0, depthRbo_ = 0;
    int fboW_ = 0, fboH_ = 0;
    MeshGpu mesh_;
    StandaloneSceneStubs stubs_;
    ShaderContractReport report_;
    bool triedProgram_ = false;
    float yaw_ = 0.6f, pitch_ = 0.35f, dist_ = 3.0f;
    float modelRotDeg_[3] = { -90.0f, 0.0f, 0.0f };  // same upright convention as Backend-B
};
```

- [ ] **Step 2: Failing render smoke**

`AssetViewerApp.h`:
```cpp
    static int runSmokeBackendARender(const char* deployDir, const char* shaderRoot);  // Backend-A v2
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-backend-a-render") == 0)
        return AssetViewerApp::runSmokeBackendARender(argc >= 3 ? argv[2] : ".",
                                                      argc >= 4 ? argv[3] : "shaders");
```
`AssetViewerApp.cpp` (headless GL context like Task 2; pick a known prop name from `tgl.fst` — reuse whatever `runSmokeMeshRender` loads):
```cpp
int AssetViewerApp::runSmokeBackendARender(const char* deployDir, const char* shaderRoot) {
    // ... headless GL context (same helper as runSmokeMeshRender) ...
    ModelPreviewEngineShader p;
    p.setShaderRoot(shaderRoot);
    p.setDeployDir(deployDir);
    p.setSource("<KNOWN_PROP_FROM_runSmokeMeshRender>");   // use the same prop that smoke uses
    std::vector<uint8_t> px;
    bool rendered = p.renderToPixels(128, 128, px);
    // Non-empty == not all equal to the clear color. Compute simple variance.
    bool nonEmpty = false;
    if (rendered && px.size() >= 4) {
        for (size_t i = 4; i + 3 < px.size(); i += 4)
            if (px[i] != px[0] || px[i+1] != px[1] || px[i+2] != px[2]) { nonEmpty = true; break; }
    }
    bool ok = p.ok() && rendered && nonEmpty;
    printf("[smoke] backend-a-render %s (compiled=%d nonEmpty=%d)\n",
           ok ? "PASS" : "FAIL", (int)p.ok(), (int)nonEmpty);
    // ... teardown ...
    return ok ? 0 : 1;
}
```

- [ ] **Step 3: Build → verify link FAIL** (`ModelPreviewEngineShader` symbols unresolved). Add the `.cpp` to CMake first.

- [ ] **Step 4: Implement `ModelPreviewEngineShader.cpp`**

Implement per the header. Key points (full code authored by the implementer, guided here):
- `ensureProgram()`: `ResolveShaderIncludes(shaderRoot_, "static_prop.vert"/".frag")` with the **exact define+version prefix from `docs/asset-viewer-backend-a-shader-contract.md`** (Task 2). Compile/link; fill `report_` (logs, defines, includes, textureMode, stub flags). On failure set `report_.lastError` and return false. NEVER edit shader files.
- `ensureFbo()`: MRT FBO — `color0_` (GL_RGBA8) at `GL_COLOR_ATTACHMENT0`, `gbuffer1_` (GL_RGBA8) at `GL_COLOR_ATTACHMENT1`, `depthRbo_` (GL_DEPTH_COMPONENT24) — `glDrawBuffers(2, {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1})`. Without the 2nd attachment the frag's `GBuffer1` write is undefined.
- `renderScene()`: apply the pipeline-state checklist above (reverse-Z!); `stubs_.bind(model)`; set uniforms from the contract (`u_worldToClipGL`, `u_tex`=unit 0, `u_materialFlags`=0, `u_fogValue`=1.0, `u_materialGpuSample`=0, `u_pbrV1Strength`=0, `u_debugAddrMode`=0, plus any others the contract lists — set every active uniform to a sane default so none are left undefined); set loc3/loc4 integer vertex attribs as constants via `glVertexAttribI4ui(3,0,0,0,0)` / `glVertexAttribI4ui(4,0,0,0,0)` after disabling those arrays (the prop geometry has no per-vertex localVertexID/aRGBLight); bind albedo per submesh and draw via `mesh_` (reuse its VAO; you may need to re-point attribs 0/1/2 to the static_prop locations — confirm `MeshGpu`'s VBO stride in `MeshGpu.cpp` and bind a Backend-A VAO over the same VBO if the attribute locations differ). Restore all saved state.
- `draw()`: `if (!ensureProgram()) { /* caller handles fallback via ok() */ ImGui::TextDisabled("Backend-A unavailable; see Shader Contract"); return; }` then render to FBO + `ImGui::Image`. The actual Backend-B fallback happens in the panel (Task 6) by checking `ok()`.
- `renderToPixels()`: headless — ensureProgram + ensureFbo + renderScene + `glReadPixels`. Return false if program/fbo failed.

- [ ] **Step 5: Build + run render smoke → PASS**

```
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-backend-a-render mc2-win64-v0.4 shaders
```
(Use the deploy dir that has `tgl.fst`, e.g. `A:/Games/mc2-opengl/mc2-win64-v0.4`, and the repo `shaders`.) Expected: `[smoke] backend-a-render PASS (compiled=1 nonEmpty=1)`. If the prop renders black/empty, debug the light stub (numLights/type) or the reverse-Z projection — NOT the shader source.

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/ModelPreviewEngineShader.h tools/asset_viewer/ModelPreviewEngineShader.cpp tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): ModelPreviewEngineShader renders prop with real engine shaders"
```

---

## Task 6: UI toggle + fail-open fallback + contract panel

**Files:** modify `AssetViewerApp.cpp` (StaticProps panel), `PreviewSurface.h`.

- [ ] **Step 1: Update the PreviewSurface comment**

In `PreviewSurface.h`, change the "stage 3 adds ModelPreviewRenderCore" comment to:
```cpp
// Stage 2 adds MaterialPreviewPBR; Backend-A v2 adds ModelPreviewEngineShader
// (shader + pipeline-state faithful — real static_prop shaders, matching
// depth/blend/cull — NOT full engine scene render: no batcher, no mission lights,
// no shadow cascade). The app and inspector panel depend ONLY on this interface.
```

- [ ] **Step 2: Add the toggle + fallback to the StaticProps panel**

In `AssetViewerApp.cpp` where `AssetType::StaticProps` renders the `MeshPreview3D` (~line 63/94 region — read it), add an instance of `ModelPreviewEngineShader` alongside the existing `MeshPreview3D`, a backend enum (default Backend-B), and the toggle. Pseudostructure (adapt to the actual member/locals):
```cpp
    // members (or static within the panel fn, matching existing style):
    //   MeshPreview3D backendB_;  ModelPreviewEngineShader backendA_;  int backend_ = 0; // 0=B,1=A
    ImGui::TextUnformatted("Preview backend:");
    ImGui::SameLine(); ImGui::RadioButton("Backend-B: Approximate", &backend_, 0);
    ImGui::SameLine(); ImGui::RadioButton("Backend-A: Engine Shader", &backend_, 1);
    if (backend_ == 1) {
        backendA_.setShaderRoot(shaderRoot);   // "shaders" or deploy shaders
        backendA_.setDeployDir(deployDir);
        backendA_.setSource(currentTgl);       // keep both backends pointed at the same prop
        if (backendA_.ok()) {
            ImGui::TextDisabled("Engine shader preview: real static_prop shader with standalone "
                                "lighting stubs. Not mission-lighting exact.");
            backendA_.draw(avail);
        } else {
            ImGui::TextColored(ImVec4(1,0.6f,0.3f,1),
                "Backend-A unavailable — showing Backend-B. See Shader Contract below.");
            backendB_.draw(avail);             // FAIL OPEN
        }
        DrawShaderContractReport(backendA_.report());
    } else {
        backendB_.draw(avail);
    }
```
Wire `setSource` on whichever backend is active so switching keeps the same prop. Keep Backend-B the default (`backend_=0`).

- [ ] **Step 3: Build clean, manual launch sanity** (optional if no display): build must be clean. Commit.
```bash
git add tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/PreviewSurface.h
git commit -m "feat(asset-viewer): Backend-A/B toggle with fail-open fallback + contract panel"
```

---

## Task 7: Fallback smoke (`--smoke-backend-a-fallback`) — proves Backend-B rendered

**Files:** modify `AssetViewerApp.{h,cpp}`, `main.cpp`. Per the guardrail: not merely "no crash" — must prove the Backend-B fallback produced a non-empty frame and the contract recorded the error.

- [ ] **Step 1: Decl + dispatch**

`AssetViewerApp.h`:
```cpp
    static int runSmokeBackendAFallback(const char* deployDir);  // Backend-A v2
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-backend-a-fallback") == 0)
        return AssetViewerApp::runSmokeBackendAFallback(argc >= 3 ? argv[2] : ".");
```

- [ ] **Step 2: Body — bad shader root → A fails, B renders**

```cpp
int AssetViewerApp::runSmokeBackendAFallback(const char* deployDir) {
    // ... headless GL context ...
    ModelPreviewEngineShader a;
    a.setShaderRoot("___nonexistent_shader_root___");   // force resolve/compile failure
    a.setDeployDir(deployDir);
    a.setSource("<KNOWN_PROP>");
    std::vector<uint8_t> apx;
    a.renderToPixels(128, 128, apx);
    bool aFailed = !a.ok() && !a.report().lastError.empty();   // A failed AND recorded it

    // The fallback the panel would render: Backend-B on the same prop, non-empty.
    MeshPreview3D b;
    b.setDeployDir(deployDir);
    b.setSource("<KNOWN_PROP>");
    std::vector<uint8_t> bpx;
    bool bRendered = b.renderToPixels(128, 128, bpx);
    bool bNonEmpty = false;
    for (size_t i = 4; bRendered && i + 3 < bpx.size(); i += 4)
        if (bpx[i]!=bpx[0]||bpx[i+1]!=bpx[1]||bpx[i+2]!=bpx[2]) { bNonEmpty = true; break; }

    bool ok = aFailed && bRendered && bNonEmpty;   // A failed gracefully AND B rendered
    printf("[smoke] backend-a-fallback %s (aFailed=%d bNonEmpty=%d)\n",
           ok ? "PASS" : "FAIL", (int)aFailed, (int)bNonEmpty);
    // ... teardown ...
    return ok ? 0 : 1;
}
```

- [ ] **Step 3: Build + run → PASS**

```
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-backend-a-fallback mc2-win64-v0.4
```
Expected: `[smoke] backend-a-fallback PASS (aFailed=1 bNonEmpty=1)`.

- [ ] **Step 4: Commit**
```bash
git add tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): Backend-A fallback smoke proves Backend-B renders on failure"
```

---

## Task 8: UV-V verification (artifact-based)

**Files:** create `docs/asset-viewer-backend-a-uvv-check.md`; possibly modify `ModelPreviewEngineShader.cpp` / the GLB UV handling if a flip is found.

- [ ] **Step 1: Pick a known asymmetric-texture prop**

Choose a specific prop whose albedo is visually asymmetric in V (text/logo/clearly-oriented detail). Record its exact `.tgl` name. Render it both ways:
```
EXE=build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe
$EXE --smoke-backend-a-render mc2-win64-v0.4 shaders   # Backend-A
# Backend-B view is the existing --smoke-mesh-render / interactive
```
For a recordable artifact, extend `renderToPixels` usage: in a throwaway run (or a tiny added `--smoke-backend-a-uvv` that dumps the FBO to a PPM/PNG for both backends) capture both images. Do NOT commit large images — commit a small crop or a per-row luminance summary / hash, plus the written verdict.

- [ ] **Step 2: Compare orientation, write the verdict**

In `docs/asset-viewer-backend-a-uvv-check.md` record: the prop name, whether Backend-A's albedo V matches Backend-B and the in-game convention (`static_prop.frag` UV usage vs `mclib/assimp_importer.cpp`'s manual `1-v`), and the conclusion. If FLIPPED: fix the tool's UV handling (the loader/UV feed, NOT the shader) and note the fix; re-render to confirm. If it cannot be rendered at all, state that explicitly (the only permitted deferral per acceptance criterion 8).

- [ ] **Step 3: Commit**
```bash
git add docs/asset-viewer-backend-a-uvv-check.md   # + any UV-fix code
git commit -m "docs(asset-viewer): Backend-A UV-V convention verification"
```

---

## Task 9: Full regression sweep + README

**Files:** modify `tools/asset_viewer/README.md`.

- [ ] **Step 1: Run the full smoke set — no regression**

Build fresh, run all 4 new Backend-A smokes + the 4 shader/other new ones + the existing 26. The Backend-A render/fallback smokes need the deploy dir + `shaders`. Record a PASS/FAIL table. Any pre-existing smoke regression → STOP, report BLOCKED.
```
EXE=build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe
$EXE --smoke-shader-include tools/asset_viewer/tests/fixtures/asset_viewer
$EXE --smoke-backend-a-compile shaders
$EXE --smoke-backend-a-render mc2-win64-v0.4 shaders
$EXE --smoke-backend-a-fallback mc2-win64-v0.4
# ...then the existing 26 (see Task 8 of the S5 plan for the discovery grep)
grep -oE '"--smoke-[a-z-]+"' tools/asset_viewer/main.cpp | sort -u
```

- [ ] **Step 2: README**

Add a "## Backend-A: Engine Shader preview (v2)" section: what it is (real `static_prop` shaders + standalone stubs), what it is NOT (no mission lights/shadows/fog — v3 seam in `StandaloneSceneStubs`), the toggle (default Backend-B), the shader-contract panel, fail-open behavior, and the dependency on a readable `shaders/` root. No pixel-parity claims. Note Backend-B remains default.

- [ ] **Step 3: Commit**
```bash
git add tools/asset_viewer/README.md
git commit -m "docs(asset-viewer): document Backend-A engine-shader preview (v2)"
```

---

## Self-review notes

- **Spec coverage:** ModelPreviewEngineShader (T5), reference-not-copy shaders via resolver (T1) compiled in minimal config pinned by the contract spike (T2), StandaloneSceneStubs seam (T4), contract report (T3), toggle default-B + fail-open (T6), 3 GL smokes compile/render/fallback (T2/T5/T7), UV-V (T8), no-shader-edits constraint repeated (header + T2/T4/T5), v2/v3 split (T4 + README T9), non-regression (T9). All 8 acceptance criteria mapped.
- **Empirical dependency (by design):** Tasks 4–5 consume `docs/asset-viewer-backend-a-shader-contract.md` produced by Task 2 — the exact define set + SSBO layouts can only be pinned by compiling the real shader. This is the user-mandated "compile-contract spike first, no rendering until known." The `ObjectLights`/`Instance`/`PerTypeData` struct layouts given in T4 are the best read of `lighting.hglsl`/`static_prop.vert` at plan time and MUST be reconciled against the contract before relying on them (noted in T4).
- **Ordering deviation (flagged):** resolver (T1) precedes the compile spike (T2) — compiling requires include resolution. This honors "compile-contract before rendering" (T2 is still before any render in T5) while respecting the hard dependency.
- **Reverse-Z trap:** T5's pipeline-state checklist explicitly does NOT copy Backend-B's `GL_LESS`+clear-1.0; uses `GEQUAL`+clear-0.0 + reverse-Z projection. Called out per review tweak #1.
- **Fallback proves B rendered:** T7 asserts `aFailed && bNonEmpty`, not just no-crash (review tweak #4).
- **UV-V artifact-based:** T8 names a specific prop + records a verdict doc (review tweak #5).
- **uniform uint trap:** noted in grounding + T2 — never inject `uint` uniforms.
