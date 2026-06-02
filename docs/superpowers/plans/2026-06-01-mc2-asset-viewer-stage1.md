# MC2 Asset Viewer — Stage 1 (Texture/Asset Shell) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a standalone `mc2_asset_viewer` app — window + ImGui shell, a folder/file browser, a working texture preview, a `PreviewSurface` seam, and an asset-type sidebar vocabulary — reusing @Methuselas's image cache.

**Architecture:** "A's footprint, B's architecture." Link only imgui + SDL2 + OpenGL + GLEW now (mirror `ui_editor`); never link RenderCore in stage 1. The app talks to a `PreviewSurface` interface and is render-backend-agnostic, so stages 2–3 add new `PreviewSurface` impls + a one-line CMake link rather than a rewire. Texture decode/upload is delegated to the existing `UiEditorImageCache` public API (compiled into this target as shared source).

**Tech Stack:** C++17, SDL2, OpenGL 3.0 + GLEW, Dear ImGui (SDL2 + OpenGL3 backends), `std::filesystem`. Build: CMake + MSVC RelWithDebInfo, vendored deps. Base branch: `claude/merge-methuselas-1`.

---

## Reused APIs (verified in tree, do NOT reimplement)

From `ui_editor/UiEditorImageCache.h` (compiled into our target):

```cpp
struct UiEditorImageTexture {
    bool loaded;
    bool unavailable;
    int width;
    int height;
    const char* resolvedPath;
    ImTextureID textureId;   // GL texture handle, usable by ImGui::Image
};
bool UiEditorImageCache_Initialize();
void UiEditorImageCache_Shutdown();
void UiEditorImageCache_Clear();
const UiEditorImageTexture* UiEditorImageCache_Get(const char* path);  // primary entry
const char* UiEditorImageCache_GetStatus();
```

`UiEditorImageCache_Get` decodes (stb_image: PNG/JPG/BMP/TGA) **and** uploads to GL — it requires a live GL context. Returns a pointer whose `loaded`/`unavailable` flags drive our error display.

SDL/GL/ImGui bring-up is copied/trimmed from `ui_editor/UiEditorMain.cpp:6570-6676` (GL 3.0 core, `#version 130`, SDL2+OpenGL3 ImGui backends).

---

## File Structure

All new files under `tools/asset_viewer/` unless noted.

| File | Responsibility |
|---|---|
| `tools/asset_viewer/CMakeLists.txt` | Target def; mirrors `ui_editor` link/include logic; compiles shared `UiEditorImageCache.cpp` + `Image.cpp` |
| `tools/asset_viewer/TextureExtensions.h/.cpp` | Pure util: is a path a supported texture? (testable, no GL) |
| `tools/asset_viewer/TextureMetadata.h/.cpp` | Pure util: format `{w,h,channels,bytes}` → display strings (testable, no GL) |
| `tools/asset_viewer/PreviewSurface.h` | Interface seam: `setSource(path)` / `draw()` / `label()` |
| `tools/asset_viewer/TexturePreview2D.h/.cpp` | Stage-1 `PreviewSurface`: holds path, calls `UiEditorImageCache_Get`, `ImGui::Image` |
| `tools/asset_viewer/FileBrowser.h/.cpp` | Folder path input → enumerate texture files → selected path |
| `tools/asset_viewer/AssetTypeSidebar.h/.cpp` | Asset-type vocabulary list (Textures live; rest disabled) |
| `tools/asset_viewer/TextureInspectorPanel.h/.cpp` | Draws active `PreviewSurface` + metadata readout |
| `tools/asset_viewer/AssetViewerApp.h/.cpp` | Owns selection + panels; `drawUi()`; `runSmoke()` |
| `tools/asset_viewer/main.cpp` | SDL/GL/ImGui lifecycle + frame loop; `--smoke` dispatch |
| `tests/fixtures/asset_viewer/test_rgba.png` | 4×2 RGBA fixture for the smoke test |
| `CMakeLists.txt` (modify) | Guarded `add_subdirectory("./tools/asset_viewer")` |

---

### Task 1: New target builds an empty ImGui window

**Files:**
- Create: `tools/asset_viewer/main.cpp`
- Create: `tools/asset_viewer/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add guarded subdirectory near the `ui_editor` block, ~line 283)

- [ ] **Step 1: Create the CMake target** (`tools/asset_viewer/CMakeLists.txt`)

```cmake
# MC2 Asset Viewer — standalone target. SDL include resolution mirrors ui_editor
# (the ImGui SDL backend includes "SDL.h", so the SDL2 include dir itself must be
# on the include path). Links imgui + SDL2 + GL + GLEW only — NO RenderCore in
# stage 1. Reuses @Methuselas's image cache as shared source.
set(ASSET_VIEWER_SOURCES
    "main.cpp"
    "TextureExtensions.cpp"
    "TextureMetadata.cpp"
    "TexturePreview2D.cpp"
    "FileBrowser.cpp"
    "AssetTypeSidebar.cpp"
    "TextureInspectorPanel.cpp"
    "AssetViewerApp.cpp"
    "${CMAKE_SOURCE_DIR}/ui_editor/UiEditorImageCache.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/Image.cpp"
)

add_executable(mc2_asset_viewer ${ASSET_VIEWER_SOURCES})
target_compile_features(mc2_asset_viewer PRIVATE cxx_std_17)
target_compile_definitions(mc2_asset_viewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()
if(NOT TARGET OpenGL::GL)
    find_package(OpenGL REQUIRED)
endif()

set(MC2R_AV_SDL_INCLUDE_DIRS)
if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()
if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()
foreach(_p IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_p}/include/SDL2/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_p}/include/SDL2")
    endif()
    if(EXISTS "${_p}/include/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_p}/include")
    endif()
endforeach()
if(MC2R_AV_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2R_AV_SDL_INCLUDE_DIRS)
endif()

target_include_directories(mc2_asset_viewer PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/ui_editor"
    ${MC2R_AV_SDL_INCLUDE_DIRS}
)

target_link_libraries(mc2_asset_viewer PRIVATE imgui)
if(TARGET SDL2::SDL2)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(mc2_asset_viewer PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 not found for mc2_asset_viewer")
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(mc2_asset_viewer PRIVATE OpenGL::GL)
if(WIN32)
    target_link_libraries(mc2_asset_viewer PRIVATE ole32 windowscodecs uuid)
endif()
if(TARGET GLEW::GLEW)
    target_link_libraries(mc2_asset_viewer PRIVATE GLEW::GLEW)
endif()
```

- [ ] **Step 2: Register the target** — in root `CMakeLists.txt`, immediately after the existing guarded `ui_editor` block (the `if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui_editor/CMakeLists.txt")` ... `endif()` around line 282-284), add:

```cmake
# MC2 Asset Viewer (standalone modder tool). Guarded like ui_editor so a partial
# checkout without the source dir does not break configure.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/asset_viewer/CMakeLists.txt")
    add_subdirectory("./tools/asset_viewer" "./out/tools/asset_viewer")
endif()
```

- [ ] **Step 3: Minimal `main.cpp`** (empty window; `--smoke` stub returns 0 for now)

```cpp
// MC2 Asset Viewer — stage 1 shell. SDL2 + OpenGL3 + Dear ImGui.
// Bring-up trimmed from ui_editor/UiEditorMain.cpp.
#include <cstdio>
#include <cstring>
#include <SDL.h>
#include <GL/glew.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "AssetViewerApp.h"

int main(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--smoke") == 0) {
        const char* fixtureDir = (argc >= 3) ? argv[2] : "tests/fixtures/asset_viewer";
        return AssetViewerApp::runSmoke(fixtureDir);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* window = SDL_CreateWindow(
        "MC2 Asset Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) { std::fprintf(stderr, "GL context failed: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    glewExperimental = GL_TRUE;
    glewInit();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    AssetViewerApp app;   // ctor calls UiEditorImageCache_Initialize()

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        app.drawUi();

        ImGui::Render();
        int w = 0, h = 0; SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.06f, 0.065f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
```

This references `AssetViewerApp` (Task 7). To build Task 1 in isolation, create a temporary stub `AssetViewerApp.h/.cpp` with an empty `drawUi()` and `static int runSmoke(const char*) { return 0; }`; Task 7 replaces them. (If executing in order with subagents, build verification for Task 1 happens after the stub exists.)

- [ ] **Step 4: Configure + build**

Run (from worktree root):
```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
D="A:/Games/mc2-opengl-src/3rdparty/3rdparty"
"$CMAKE" -G "Visual Studio 17 2022" -A x64 -S . -B build64 \
  -DCMAKE_PREFIX_PATH="$D" -DCMAKE_LIBRARY_PATH="$D/lib/x64" \
  -DGLEW_INCLUDE_DIR="$D/include" -DGLEW_SHARED_LIBRARY_RELEASE="$D/lib/x64/glew32.lib" \
  -DGLEW_STATIC_LIBRARY_RELEASE="$D/lib/x64/glew32s.lib" > cfg.log 2>&1; echo "rc=$?" >> cfg.log
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer > b.log 2>&1; echo "rc=$?" >> b.log
grep -E "rc=|error" b.log | tail
```
Expected: `Generating done`, `mc2_asset_viewer.vcxproj -> ...mc2_asset_viewer.exe`, `rc=0`.

- [ ] **Step 5: Manual verify** — run `build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe`; a 1280×720 dark window opens and closes cleanly.

- [ ] **Step 6: Commit**

```bash
git add -f tools/asset_viewer/CMakeLists.txt tools/asset_viewer/main.cpp CMakeLists.txt
git commit -m "feat(asset-viewer): stage-1 target + empty ImGui window shell

Co-Authored-By: Methuselas <16720094+Methuselas@users.noreply.github.com>"
```
(`-f` because new files under `tools/` are gitignored by the repo's `Tools` rule.)

---

### Task 2: Supported-texture filter (pure util, TDD)

**Files:**
- Create: `tools/asset_viewer/TextureExtensions.h`, `tools/asset_viewer/TextureExtensions.cpp`
- Test: fold assertions into `AssetViewerApp::runSmoke` (Task 8). Here, write a tiny temporary `assert`-driven check in `main.cpp`'s `--smoke` path is NOT used; instead define the function and its expected behavior precisely.

- [ ] **Step 1: Write the failing usage** — add to `tools/asset_viewer/TextureExtensions.h`:

```cpp
#pragma once
#include <string>
// True if `path` ends in a stage-1 supported texture extension (case-insensitive):
// .png .jpg .jpeg .bmp .tga
bool IsSupportedTextureFile(const std::string& path);
```

- [ ] **Step 2: Reference it from the smoke checks** (will be wired in Task 8). For now verify it fails to link: a throwaway `tools/asset_viewer/_t.cpp` with `int main(){return IsSupportedTextureFile("a.png")?0:1;}` compiled ad hoc.
Run: attempt compile of `_t.cpp`. Expected: link error "unresolved external IsSupportedTextureFile". Delete `_t.cpp` after.

- [ ] **Step 3: Implement** (`tools/asset_viewer/TextureExtensions.cpp`)

```cpp
#include "TextureExtensions.h"
#include <algorithm>
#include <array>

bool IsSupportedTextureFile(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    static const std::array<const char*, 5> kExts = {"png", "jpg", "jpeg", "bmp", "tga"};
    return std::any_of(kExts.begin(), kExts.end(),
                       [&](const char* e){ return ext == e; });
}
```

- [ ] **Step 4: Verify** via the Task 8 smoke (asserts: `IsSupportedTextureFile("a.PNG")==true`, `"a.dds"==false`, `"noext"==false`). Build target, run `--smoke`, expect those asserts to pass once Task 8 lands.

- [ ] **Step 5: Commit**

```bash
git add -f tools/asset_viewer/TextureExtensions.h tools/asset_viewer/TextureExtensions.cpp
git commit -m "feat(asset-viewer): supported-texture extension filter"
```

---

### Task 3: Texture-metadata formatting (pure util, TDD)

**Files:**
- Create: `tools/asset_viewer/TextureMetadata.h`, `tools/asset_viewer/TextureMetadata.cpp`

- [ ] **Step 1: Header / contract** (`TextureMetadata.h`)

```cpp
#pragma once
#include <string>
#include <cstdint>

struct TextureMetadata {
    int width = 0;
    int height = 0;
    int channels = 0;          // 0 if unknown
    std::uintmax_t fileBytes = 0;
};

// "256 x 256"
std::string FormatDimensions(const TextureMetadata& m);
// "1.5 MB", "812 KB", "300 B"
std::string FormatFileSize(const TextureMetadata& m);
// "RGBA" / "RGB" / "Gray+A" / "Gray" / "unknown"
std::string FormatChannels(const TextureMetadata& m);
```

- [ ] **Step 2: Verify it fails** — referenced from Task 8 smoke; before implementing, the target fails to link with "unresolved external FormatDimensions". (Same ad-hoc compile check as Task 2 if desired.)

- [ ] **Step 3: Implement** (`TextureMetadata.cpp`)

```cpp
#include "TextureMetadata.h"
#include <cstdio>

std::string FormatDimensions(const TextureMetadata& m) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d x %d", m.width, m.height);
    return buf;
}

std::string FormatFileSize(const TextureMetadata& m) {
    char buf[64];
    double b = (double)m.fileBytes;
    if (b >= 1024.0 * 1024.0) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024.0)     std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else                      std::snprintf(buf, sizeof(buf), "%ju B", (std::uintmax_t)m.fileBytes);
    return buf;
}

std::string FormatChannels(const TextureMetadata& m) {
    switch (m.channels) {
        case 1: return "Gray";
        case 2: return "Gray+A";
        case 3: return "RGB";
        case 4: return "RGBA";
        default: return "unknown";
    }
}
```

- [ ] **Step 4: Verify** via Task 8 smoke asserts (`FormatFileSize({.fileBytes=1572864})=="1.5 MB"`, `FormatChannels({.channels=4})=="RGBA"`, `FormatDimensions({.width=256,.height=128})=="256 x 128"`).

- [ ] **Step 5: Commit**

```bash
git add -f tools/asset_viewer/TextureMetadata.h tools/asset_viewer/TextureMetadata.cpp
git commit -m "feat(asset-viewer): texture metadata formatting helpers"
```

---

### Task 4: `PreviewSurface` interface + `TexturePreview2D`

**Files:**
- Create: `tools/asset_viewer/PreviewSurface.h`
- Create: `tools/asset_viewer/TexturePreview2D.h`, `tools/asset_viewer/TexturePreview2D.cpp`

- [ ] **Step 1: The seam** (`PreviewSurface.h`)

```cpp
#pragma once
#include <string>
// The render-backend-agnostic preview seam. Stage 1 has one impl (TexturePreview2D).
// Stage 2 adds MaterialPreviewPBR; stage 3 adds ModelPreviewRenderCore. The app and
// inspector panel depend ONLY on this interface.
class PreviewSurface {
public:
    virtual ~PreviewSurface() = default;
    // Point the surface at a source asset (a file path in stage 1).
    virtual void setSource(const std::string& path) = 0;
    // Draw the preview into the current ImGui window/region.
    virtual void draw() = 0;
    // Short human label of what this surface previews (e.g. "Texture").
    virtual const char* label() const = 0;
};
```

- [ ] **Step 2: Texture impl header** (`TexturePreview2D.h`)

```cpp
#pragma once
#include "PreviewSurface.h"
#include "TextureMetadata.h"
#include <string>

class TexturePreview2D : public PreviewSurface {
public:
    void setSource(const std::string& path) override;
    void draw() override;
    const char* label() const override { return "Texture"; }

    bool hasError() const { return hasError_; }
    const std::string& errorText() const { return errorText_; }
    const TextureMetadata& metadata() const { return meta_; }
    const std::string& sourcePath() const { return path_; }

private:
    std::string path_;
    TextureMetadata meta_;
    void* textureId_ = nullptr;   // ImTextureID for the loaded image, null if none
    bool hasError_ = false;
    std::string errorText_;
    float zoom_ = 1.0f;
};
```

- [ ] **Step 3: Texture impl** (`TexturePreview2D.cpp`) — uses the cache's public API + `std::filesystem` for file size

```cpp
#include "TexturePreview2D.h"
#include "UiEditorImageCache.h"
#include "imgui.h"
#include <filesystem>
#include <system_error>

void TexturePreview2D::setSource(const std::string& path)
{
    path_ = path;
    meta_ = TextureMetadata{};
    textureId_ = nullptr;
    hasError_ = false;
    errorText_.clear();

    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) meta_.fileBytes = sz;

    const UiEditorImageTexture* tex = UiEditorImageCache_Get(path.c_str());
    if (!tex || !tex->loaded) {
        hasError_ = true;
        errorText_ = tex && tex->unavailable
            ? "Image format not supported or file unreadable."
            : "Failed to load image (not found or decode error).";
        return;
    }
    meta_.width = tex->width;
    meta_.height = tex->height;
    meta_.channels = 0; // cache does not expose channel count; left unknown in stage 1
    textureId_ = (void*)tex->textureId;
}

void TexturePreview2D::draw()
{
    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", errorText_.c_str());
        ImGui::TextWrapped("Path: %s", path_.c_str());
        return;
    }
    if (!textureId_) {
        ImGui::TextDisabled("No texture selected.");
        return;
    }
    ImGui::SliderFloat("Zoom", &zoom_, 0.1f, 8.0f, "%.1fx");
    ImVec2 size((float)meta_.width * zoom_, (float)meta_.height * zoom_);
    ImGui::BeginChild("tex_scroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image((ImTextureID)textureId_, size);
    ImGui::EndChild();
}
```

Note: `meta_.channels` stays 0/"unknown" in stage 1 — `UiEditorImageCache` does not expose channel count. Do not invent it. (Surfacing channels is a stage-1.1 follow-up if wanted: would require a cache API addition — out of scope.)

- [ ] **Step 4: Build** the target (`--target mc2_asset_viewer`). Expected: compiles, `rc=0`. (Visual verification deferred to Task 7 wiring.)

- [ ] **Step 5: Commit**

```bash
git add -f tools/asset_viewer/PreviewSurface.h tools/asset_viewer/TexturePreview2D.h tools/asset_viewer/TexturePreview2D.cpp
git commit -m "feat(asset-viewer): PreviewSurface seam + TexturePreview2D (uses UiEditorImageCache)

Co-Authored-By: Methuselas <16720094+Methuselas@users.noreply.github.com>"
```

---

### Task 5: `FileBrowser` panel

**Files:**
- Create: `tools/asset_viewer/FileBrowser.h`, `tools/asset_viewer/FileBrowser.cpp`

- [ ] **Step 1: Header** (`FileBrowser.h`)

```cpp
#pragma once
#include <string>
#include <vector>

// Minimal dependency-free browser: a folder path field. On load, lists supported
// texture files in that folder. Selecting one returns its full path via takeSelection().
class FileBrowser {
public:
    void draw();                       // renders the ImGui panel
    bool hasSelection() const { return hasSelection_; }
    std::string takeSelection();       // returns selected path, clears the flag

private:
    void refresh();                    // re-scan folderPath_ into entries_
    char folderPath_[1024] = {0};
    std::vector<std::string> entries_; // file names (not full paths)
    std::string scanError_;
    int selectedIndex_ = -1;
    bool hasSelection_ = false;
    std::string selectionPath_;
};
```

- [ ] **Step 2: Implementation** (`FileBrowser.cpp`)

```cpp
#include "FileBrowser.h"
#include "TextureExtensions.h"
#include "imgui.h"
#include <filesystem>
#include <system_error>
namespace fs = std::filesystem;

void FileBrowser::refresh()
{
    entries_.clear();
    scanError_.clear();
    selectedIndex_ = -1;
    std::error_code ec;
    fs::path dir(folderPath_);
    if (!fs::is_directory(dir, ec)) { scanError_ = "Not a folder."; return; }
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        if (IsSupportedTextureFile(name)) entries_.push_back(name);
    }
    if (entries_.empty() && scanError_.empty())
        scanError_ = "No supported textures (.png/.jpg/.bmp/.tga) here.";
}

void FileBrowser::draw()
{
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputText("##folder", folderPath_, sizeof(folderPath_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) refresh();

    if (!scanError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", scanError_.c_str());

    ImGui::Separator();
    ImGui::BeginChild("file_list", ImVec2(0, 0), true);
    for (int i = 0; i < (int)entries_.size(); ++i) {
        bool selected = (i == selectedIndex_);
        if (ImGui::Selectable(entries_[i].c_str(), selected)) {
            selectedIndex_ = i;
            selectionPath_ = (fs::path(folderPath_) / entries_[i]).string();
            hasSelection_ = true;
        }
    }
    ImGui::EndChild();
}

std::string FileBrowser::takeSelection()
{
    hasSelection_ = false;
    return selectionPath_;
}
```

- [ ] **Step 3: Build** the target. Expected `rc=0`.

- [ ] **Step 4: Commit**

```bash
git add -f tools/asset_viewer/FileBrowser.h tools/asset_viewer/FileBrowser.cpp
git commit -m "feat(asset-viewer): folder file browser (texture filter)"
```

---

### Task 6: `AssetTypeSidebar` vocabulary

**Files:**
- Create: `tools/asset_viewer/AssetTypeSidebar.h`, `tools/asset_viewer/AssetTypeSidebar.cpp`

- [ ] **Step 1: Header** (`AssetTypeSidebar.h`)

```cpp
#pragma once
// Left sidebar: the modder-facing asset-type vocabulary. Only "Textures" is
// enabled in stage 1; the rest are visible-but-disabled (directional, no logic).
enum class AssetType { Textures };   // only the live one needs an enum value in stage 1

class AssetTypeSidebar {
public:
    void draw();                          // renders the list
    AssetType active() const { return AssetType::Textures; }  // fixed in stage 1
};
```

- [ ] **Step 2: Implementation** (`AssetTypeSidebar.cpp`)

```cpp
#include "AssetTypeSidebar.h"
#include "imgui.h"

void AssetTypeSidebar::draw()
{
    ImGui::TextDisabled("Implemented");
    ImGui::Selectable("Textures", true);   // active, selected

    ImGui::Spacing();
    ImGui::TextDisabled("Deferred");
    static const char* kDeferred[] = {
        "Materials", "Static Props", "Trees", "Mechs",
        "Vehicles", "VFX", "Terrain Materials", "Mod Package"
    };
    ImGui::BeginDisabled(true);
    for (const char* name : kDeferred)
        ImGui::Selectable(name, false);
    ImGui::EndDisabled();
}
```

- [ ] **Step 3: Build** the target. Expected `rc=0`.

- [ ] **Step 4: Commit**

```bash
git add -f tools/asset_viewer/AssetTypeSidebar.h tools/asset_viewer/AssetTypeSidebar.cpp
git commit -m "feat(asset-viewer): asset-type sidebar vocabulary (textures live, rest deferred)"
```

---

### Task 7: `TextureInspectorPanel` + `AssetViewerApp` wiring (end-to-end)

**Files:**
- Create: `tools/asset_viewer/TextureInspectorPanel.h`, `tools/asset_viewer/TextureInspectorPanel.cpp`
- Create/replace: `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp` (replaces the Task-1 stub)

- [ ] **Step 1: Inspector panel header** (`TextureInspectorPanel.h`)

```cpp
#pragma once
class TexturePreview2D;   // stage 1 inspects a texture surface concretely

// Draws the active texture preview + its metadata readout.
class TextureInspectorPanel {
public:
    void draw(TexturePreview2D& surface);
};
```

- [ ] **Step 2: Inspector panel impl** (`TextureInspectorPanel.cpp`)

```cpp
#include "TextureInspectorPanel.h"
#include "TexturePreview2D.h"
#include "TextureMetadata.h"
#include "imgui.h"

void TextureInspectorPanel::draw(TexturePreview2D& surface)
{
    if (surface.sourcePath().empty()) {
        ImGui::TextDisabled("Select a texture from the browser.");
        return;
    }
    ImGui::TextWrapped("%s", surface.sourcePath().c_str());
    if (!surface.hasError()) {
        const TextureMetadata& m = surface.metadata();
        ImGui::Text("Dimensions: %s", FormatDimensions(m).c_str());
        ImGui::Text("Channels:   %s", FormatChannels(m).c_str());
        ImGui::Text("File size:  %s", FormatFileSize(m).c_str());
    }
    ImGui::Separator();
    surface.draw();   // image (or error text)
}
```

- [ ] **Step 3: App header** (`AssetViewerApp.h`)

```cpp
#pragma once
#include "FileBrowser.h"
#include "AssetTypeSidebar.h"
#include "TextureInspectorPanel.h"
#include "TexturePreview2D.h"

class AssetViewerApp {
public:
    AssetViewerApp();      // UiEditorImageCache_Initialize()
    ~AssetViewerApp();     // UiEditorImageCache_Shutdown()

    void drawUi();         // called once per frame

    // Headless self-check: asserts on pure utils + offscreen-GL texture load.
    // Returns 0 on success, nonzero on failure. Defined in AssetViewerApp.cpp.
    static int runSmoke(const char* fixtureDir);

private:
    FileBrowser browser_;
    AssetTypeSidebar sidebar_;
    TextureInspectorPanel inspector_;
    TexturePreview2D surface_;
};
```

- [ ] **Step 4: App impl** (`AssetViewerApp.cpp`) — `runSmoke` body is filled in Task 8; here it returns 0

```cpp
#include "AssetViewerApp.h"
#include "UiEditorImageCache.h"
#include "imgui.h"

AssetViewerApp::AssetViewerApp()  { UiEditorImageCache_Initialize(); }
AssetViewerApp::~AssetViewerApp() { UiEditorImageCache_Shutdown(); }

void AssetViewerApp::drawUi()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MC2 Asset Viewer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sidebarW = 180.0f, browserW = 300.0f;
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
    sidebar_.draw();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("browser", ImVec2(browserW, 0), true);
    browser_.draw();
    if (browser_.hasSelection())
        surface_.setSource(browser_.takeSelection());
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("inspector", ImVec2(0, 0), true);
    inspector_.draw(surface_);
    ImGui::EndChild();

    ImGui::End();
}

int AssetViewerApp::runSmoke(const char* /*fixtureDir*/) { return 0; } // filled in Task 8
```

- [ ] **Step 5: Build + manual verify (end-to-end)** — build target, run the exe. In the Folder field paste the deployed art dir, e.g. `A:/Games/mc2-opengl/mc2-win64-v0.4/data/art/gui/test`, click **Load**, select `flat-ass plane_BaseColor.png`. Expected: image renders in the inspector with dimensions + file size; zoom slider works; sidebar shows Textures enabled and the rest greyed out. Select a non-existent path → error text, no crash.

- [ ] **Step 6: Commit**

```bash
git add -f tools/asset_viewer/TextureInspectorPanel.h tools/asset_viewer/TextureInspectorPanel.cpp tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp
git commit -m "feat(asset-viewer): wire app shell (sidebar + browser + texture inspector)

Co-Authored-By: Methuselas <16720094+Methuselas@users.noreply.github.com>"
```

---

### Task 8: Offscreen-GL smoke test (`--smoke`)

**Files:**
- Create: `tests/fixtures/asset_viewer/test_rgba.png` (4×2 RGBA)
- Modify: `tools/asset_viewer/AssetViewerApp.cpp` (`runSmoke` body)

- [ ] **Step 1: Create the fixture** (4×2 RGBA PNG) — generate with Python:

Run:
```bash
python -c "import struct,zlib,os; os.makedirs('tests/fixtures/asset_viewer',exist_ok=True); \
w,h=4,2; raw=b''.join(b'\x00'+bytes([(x*60)%256,(y*120)%256,128,255])*1 for y in range(h) for x in range(w) if False)"
```
If that one-liner is awkward, instead write `tests/fixtures/asset_viewer/make_fixture.py`:
```python
import struct, zlib, os
os.makedirs(os.path.dirname(__file__), exist_ok=True)
w, h = 4, 2
rows = bytearray()
for y in range(h):
    rows.append(0)  # filter byte
    for x in range(w):
        rows += bytes([(x*60) % 256, (y*120) % 256, 128, 255])
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows)))
png += chunk(b"IEND", b"")
open(os.path.join(os.path.dirname(__file__), "test_rgba.png"), "wb").write(png)
print("wrote test_rgba.png", w, h)
```
Run: `python tests/fixtures/asset_viewer/make_fixture.py`
Expected: `wrote test_rgba.png 4 2`

- [ ] **Step 2: Write the smoke body** — replace `runSmoke` in `AssetViewerApp.cpp`:

```cpp
// at top of file add:
#include "TextureExtensions.h"
#include "TextureMetadata.h"
#include "TexturePreview2D.h"
#include <SDL.h>
#include <GL/glew.h>
#include <cstdio>
#include <string>

static int smokeFail(const char* msg) { std::fprintf(stderr, "[smoke] FAIL: %s\n", msg); return 1; }

int AssetViewerApp::runSmoke(const char* fixtureDir)
{
    // 1) pure-util asserts (no GL needed)
    if (!IsSupportedTextureFile("a.PNG"))  return smokeFail("PNG should be supported");
    if ( IsSupportedTextureFile("a.dds"))  return smokeFail("dds should NOT be supported");
    if ( IsSupportedTextureFile("noext"))  return smokeFail("extensionless should NOT be supported");
    if (FormatDimensions(TextureMetadata{256,128,4,0}) != "256 x 128") return smokeFail("dims format");
    if (FormatChannels(TextureMetadata{0,0,4,0})       != "RGBA")      return smokeFail("channels format");
    { TextureMetadata m; m.fileBytes = 1572864; if (FormatFileSize(m) != "1.5 MB") return smokeFail("size format"); }

    // 2) offscreen GL: hidden window + context, then load fixture via the cache
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE; glewInit();

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;
        std::string path = std::string(fixtureDir) + "/test_rgba.png";
        surface.setSource(path);
        if (surface.hasError())               rc = smokeFail("fixture failed to load");
        else if (surface.metadata().width  != 4) rc = smokeFail("fixture width != 4");
        else if (surface.metadata().height != 2) rc = smokeFail("fixture height != 2");
        UiEditorImageCache_Shutdown();
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS\n");
    return rc;
}
```

- [ ] **Step 3: Build + run the smoke**

Run:
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
./build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke tests/fixtures/asset_viewer
echo "smoke_rc=$?"
```
Expected: prints `[smoke] PASS`, `smoke_rc=0`.

- [ ] **Step 4: Verify it can fail** — temporarily point `--smoke` at an empty dir; expect `[smoke] FAIL: fixture failed to load` and `smoke_rc=1`. Restore.

- [ ] **Step 5: Commit**

```bash
git add -f tests/fixtures/asset_viewer/make_fixture.py tests/fixtures/asset_viewer/test_rgba.png tools/asset_viewer/AssetViewerApp.cpp
git commit -m "test(asset-viewer): offscreen-GL smoke + pure-util asserts"
```

---

### Task 9: Doc + deploy note

**Files:**
- Create: `tools/asset_viewer/README.md`

- [ ] **Step 1: Write `tools/asset_viewer/README.md`**

```markdown
# mc2_asset_viewer (stage 1: texture/asset shell)

Standalone modder tool. Stage 1 = app shell + folder browser + texture preview +
PreviewSurface seam + asset-type sidebar (Textures live; rest deferred).

## Build
Configure the worktree (vendored deps; note lib/x64):
    cmake -G "Visual Studio 17 2022" -A x64 -S . -B build64 \
      -DCMAKE_PREFIX_PATH=<deps> -DCMAKE_LIBRARY_PATH=<deps>/lib/x64
    cmake --build build64 --config RelWithDebInfo --target mc2_asset_viewer

Exe: build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe

## Smoke
    mc2_asset_viewer.exe --smoke tests/fixtures/asset_viewer   # prints [smoke] PASS

## Deferred (stages 2-3)
RenderCore link, lit PBR material preview, real model preview, FIT parsing,
validation flags, cooking. SimpleCamera is assumed dead (mech-bay regression);
stage 3 uses RenderCore.

Reuses @Methuselas's UiEditorImageCache.cpp + Image.cpp.
```

- [ ] **Step 2: Commit**

```bash
git add -f tools/asset_viewer/README.md
git commit -m "docs(asset-viewer): stage-1 build/smoke/deferred README"
```

---

## Self-Review

**Spec coverage:**
- App skeleton (window+GL+ImGui+loop) → Task 1 ✓
- File/folder browser → Task 5 ✓
- Texture preview → Task 4 (surface) + Task 7 (panel) ✓
- `PreviewSurface` seam → Task 4 ✓
- Asset-type sidebar vocabulary (Textures live; 8 deferred) → Task 6 ✓
- Pure viewer, no validation → enforced (no validation code anywhere) ✓
- Reuse `UiEditorImageCache` shared source → Task 1 CMake + Task 4 usage ✓
- Smoke (offscreen GL) + fixture → Task 8 ✓
- New target = A footprint (no RenderCore link) → Task 1 CMake ✓
- Deferred items + SimpleCamera-dead noted → Task 9 README ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". `runSmoke` is intentionally a return-0 stub in Task 7 and fully implemented in Task 8 (sequenced, not a placeholder). Task 1's `AssetViewerApp` stub is explicitly called out as temporary and replaced in Task 7.

**Type consistency:** `PreviewSurface{setSource,draw,label}` used identically in Task 4 and Task 7. `UiEditorImageTexture{loaded,unavailable,width,height,textureId}` matches the real header. `TextureMetadata{width,height,channels,fileBytes}` consistent across Tasks 3/4/7/8. `IsSupportedTextureFile`, `FormatDimensions/FormatChannels/FormatFileSize`, `FileBrowser::{hasSelection,takeSelection}`, `AssetViewerApp::{drawUi,runSmoke}` consistent across all referencing tasks.

**Known stage-1 limitation (intentional):** `meta_.channels` is always 0/"unknown" because `UiEditorImageCache` exposes no channel count. Documented in Task 4; surfacing it would require a cache API change (deferred).
