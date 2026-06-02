# Asset Viewer Resolution-Tier Switcher + Constant Display Size — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** (A) render every texture at the same on-screen size regardless of native resolution (fit-to-region × zoom), and (B) add a numeric-sibling-folder resolution-tier switcher (64/128/256/512) that keeps the same selected asset across tiers and only offers tiers that exist.

**Architecture:** A pure `FitTextureDisplaySize` helper drives preview sizing (decoupling display size from source pixels). `FileBrowser` gains `SiblingTiers()/CurrentTier()/SwitchTier()` (tier = numeric sibling dirs of the current folder) and a tier row in its UI; switching reuses the existing `hasSelection()` → `setSource` path so the preview reloads the same asset at the new tier. Viewer-only.

**Tech Stack:** C++17, SDL2 + GLEW + OpenGL, Dear ImGui. Build via `C:\mingw64\bin\cmake.exe` (VS2022 generator). Fixture-based `--smoke*` harness.

**Worktree:** `A:/Games/mc2-asset-viewer-res-tiers` (branch `claude/asset-viewer-res-tiers`, off nifty — has the KTX2 decoder + `data/tgl/128` default).

---

## Build / test recipe (this machine — cmake NOT on PATH)

PowerShell, quote `-D` args. Re-configure ONLY after editing `CMakeLists.txt` (this plan adds no new source files except via existing TUs — see each task):
```
& "C:\mingw64\bin\cmake.exe" -G "Visual Studio 17 2022" "-DCMAKE_PREFIX_PATH=A:/Games/mc2-opengl-src/3rdparty/3rdparty" "-DCMAKE_LIBRARY_ARCHITECTURE=x64" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -B build64
```
Build + run (copy DLLs beside the exe once, as below):
```
& "C:\mingw64\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
$dst="A:\Games\mc2-asset-viewer-res-tiers\build64\out\tools\asset_viewer\RelWithDebInfo"
Copy-Item "A:\Games\mc2-opengl-src\3rdparty\3rdparty\lib\x64\SDL2.dll","A:\Games\mc2-opengl-src\3rdparty\3rdparty\lib\x64\glew32.dll" $dst -Force
& "$dst\mc2_asset_viewer.exe" --smoke-fit
```
First build of this worktree needs the configure step above (no build64 yet). A smoke passes when it prints `[smoke] PASS …` and `$LASTEXITCODE` is 0.

Existing regression smokes that must stay green throughout:
```
--smoke tests\fixtures\asset_viewer    --smoke-decoder    --smoke-ktx-parse tests\fixtures\asset_viewer
--smoke-ktx tests\fixtures\asset_viewer    --smoke-preview tests\fixtures\asset_viewer
```

---

## Task 1: FitTextureDisplaySize helper (Part A core) + `--smoke-fit`

**Files:**
- Modify: `tools/asset_viewer/TextureMetadata.h`, `tools/asset_viewer/TextureMetadata.cpp`
- Modify: `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp`, `tools/asset_viewer/main.cpp`

- [ ] **Step 1: Declare the helper (imgui-free, headless-testable)**

In `tools/asset_viewer/TextureMetadata.h`, after the existing `Format*` declarations, add:

```cpp
struct FitSize { float w = 0.0f; float h = 0.0f; };

// Largest aspect-preserving size of (texW x texH) fit into (availW x availH),
// then multiplied by zoom. Two textures with the SAME aspect ratio but different
// native resolutions yield the SAME result at the same zoom (constant on-screen
// size). Safe (finite, non-negative) for degenerate inputs (returns {0,0} if the
// texture has no area; clamps non-positive avail/zoom).
FitSize FitTextureDisplaySize(int texW, int texH, float availW, float availH, float zoom);
```

- [ ] **Step 2: Write the failing `--smoke-fit`**

In `tools/asset_viewer/AssetViewerApp.h` (public): `static int runSmokeFit();`
In `tools/asset_viewer/main.cpp`, next to the other `--smoke*` branches:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-fit") == 0)
        return AssetViewerApp::runSmokeFit();
```

In `tools/asset_viewer/AssetViewerApp.cpp` (include `"TextureMetadata.h"`; reuse the file-local `smokeFail`; add `#include <cmath>`):

```cpp
int AssetViewerApp::runSmokeFit()
{
    auto approx = [](float a, float b){ return std::fabs(a - b) < 0.01f; };

    // Same aspect, different native res, same avail+zoom -> SAME display size (the caveat).
    FitSize a = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 1.0f);
    FitSize b = FitTextureDisplaySize(256, 256, 400.0f, 400.0f, 1.0f);
    FitSize c = FitTextureDisplaySize(512, 512, 400.0f, 400.0f, 1.0f);
    if (!approx(a.w, b.w) || !approx(a.h, b.h)) return smokeFail("128 vs 256 differ in display size");
    if (!approx(a.w, c.w) || !approx(a.h, c.h)) return smokeFail("128 vs 512 differ in display size");

    // zoom 1 fits inside the avail area (square -> 400x400).
    if (a.w > 400.01f || a.h > 400.01f || a.w < 1.0f) return smokeFail("fit overflows or empty");

    // Aspect preserved for non-square (256x128 -> w == 2*h, fits).
    FitSize r = FitTextureDisplaySize(256, 128, 400.0f, 400.0f, 1.0f);
    if (!approx(r.w, 2.0f * r.h)) return smokeFail("aspect not preserved");
    if (r.w > 400.01f || r.h > 400.01f) return smokeFail("non-square overflows");

    // zoom 2 == exactly 2x zoom 1.
    FitSize z = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 2.0f);
    if (!approx(z.w, 2.0f * a.w) || !approx(z.h, 2.0f * a.h)) return smokeFail("zoom not linear");

    // Degenerate inputs: finite, no divide-by-zero.
    FitSize d0 = FitTextureDisplaySize(0, 0, 400.0f, 400.0f, 1.0f);
    if (d0.w != 0.0f || d0.h != 0.0f) return smokeFail("zero-dim texture should yield {0,0}");
    FitSize d1 = FitTextureDisplaySize(128, 128, 0.0f, 0.0f, 1.0f);
    if (!std::isfinite(d1.w) || !std::isfinite(d1.h) || d1.w < 0.0f) return smokeFail("zero-avail not finite");
    FitSize d2 = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 0.0f);
    if (!std::isfinite(d2.w) || d2.w <= 0.0f) return smokeFail("zero-zoom not handled");

    std::printf("[smoke] PASS fit (size@1=%.1fx%.1f)\n", a.w, a.h);
    return 0;
}
```

Run: `mc2_asset_viewer --smoke-fit` → Expected: build failure (helper not implemented).

- [ ] **Step 3: Implement the helper**

In `tools/asset_viewer/TextureMetadata.cpp` add (`#include <algorithm>` and `#include <cmath>` at top):

```cpp
FitSize FitTextureDisplaySize(int texW, int texH, float availW, float availH, float zoom)
{
    FitSize r;
    if (texW <= 0 || texH <= 0) return r;                 // {0,0}: nothing to show
    float aw = availW > 1.0f ? availW : 1.0f;             // clamp non-positive avail
    float ah = availH > 1.0f ? availH : 1.0f;
    float z  = zoom   > 0.0f ? zoom   : 1.0f;             // clamp non-positive zoom
    float scale = std::min(aw / (float)texW, ah / (float)texH);   // aspect-preserving fit
    if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;   // guard NaN/inf
    r.w = (float)texW * scale * z;
    r.h = (float)texH * scale * z;
    return r;
}
```

- [ ] **Step 4: Build + run to verify it passes**

Run (configure first if this is the worktree's first build):
```
& "C:\mingw64\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-fit
```
Expected: `[smoke] PASS fit (size@1=400.0x400.0)`

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/TextureMetadata.h tools/asset_viewer/TextureMetadata.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): FitTextureDisplaySize (fit-to-region x zoom) + --smoke-fit

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Use fit-sizing in TexturePreview2D (Part A wiring)

**Files:**
- Modify: `tools/asset_viewer/TexturePreview2D.cpp`

- [ ] **Step 1: Replace native-pixel sizing with fit-sizing**

In `tools/asset_viewer/TexturePreview2D.cpp`, replace the body of `draw()` after the error/no-texture guards (the current lines that compute `imageSize` and draw the child) with:

```cpp
    // Zoom is now a multiple of the fit-to-region size, so source resolution
    // (128/256/512) no longer changes the on-screen size. 1.00x == fit.
    ImGui::SliderFloat("Zoom", &zoom_, 0.25f, 8.0f, "%.2fx (fit)");
    ImGui::BeginChild("tex_scroll", availableSize, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 region = ImGui::GetContentRegionAvail();
    FitSize fs = FitTextureDisplaySize(meta_.width, meta_.height, region.x, region.y, zoom_);
    ImGui::Image((ImTextureID)(intptr_t)current_.glTexture, ImVec2(fs.w, fs.h));
    ImGui::EndChild();
```

(`FitSize`/`FitTextureDisplaySize` come from `TextureMetadata.h`, already included via `TexturePreview2D.h`. `zoom_` stays a member initialized to `1.0f` and is NOT reset in `setSource`, so tier switches preserve the on-screen size.)

- [ ] **Step 2: Build to verify it compiles + no smoke regression**

```
& "C:\mingw64\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-preview tests\fixtures\asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke tests\fixtures\asset_viewer
```
Expected: clean build; both still `[smoke] PASS`. (`draw()` is GUI-only — not exercised by smokes; the sizing math is covered by `--smoke-fit`. Visual confirmation is the manual check in Task 4.)

- [ ] **Step 3: Commit**

```bash
git add tools/asset_viewer/TexturePreview2D.cpp
git commit -m "feat(asset-viewer): preview fits texture to region (constant on-screen size across resolutions)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: FileBrowser tier detection + switch (Part B core) + fixtures + `--smoke-tiers`

**Files:**
- Modify: `tools/asset_viewer/FileBrowser.h`, `tools/asset_viewer/FileBrowser.cpp`
- Modify: `tests/fixtures/asset_viewer/make_fixture.py`
- Modify: `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp`, `tools/asset_viewer/main.cpp`

- [ ] **Step 1: Generate the tier fixture tree**

Append to `tests/fixtures/asset_viewer/make_fixture.py` (after the KTX2 section). It copies the existing `tex_rgba8.ktx2` bytes into a `tiers/{128,256}` tree; `sample.ktx2` exists in both, `only128.ktx2` exists only in 128:

```python
# ---- resolution-tier fixtures (for --smoke-tiers) ----
import shutil
_here = os.path.dirname(__file__)
_src = os.path.join(_here, "tex_rgba8.ktx2")
for _tier in ("128", "256"):
    _d = os.path.join(_here, "tiers", _tier)
    os.makedirs(_d, exist_ok=True)
    shutil.copyfile(_src, os.path.join(_d, "sample.ktx2"))
shutil.copyfile(_src, os.path.join(_here, "tiers", "128", "only128.ktx2"))
print("wrote tiers/{128,256}/sample.ktx2 + tiers/128/only128.ktx2")
```

Run: `python tests\fixtures\asset_viewer\make_fixture.py` → confirms the tier files written.

- [ ] **Step 2: Extend the FileBrowser public API**

In `tools/asset_viewer/FileBrowser.h`, add to the `public:` section:

```cpp
    void selectFile(const std::string& fullPath);          // (made public) select a known file
    const std::string& selectionPath() const { return selectionPath_; }
    void setFolder(const std::string& path);               // set folder + rescan
    // Resolution tiers = numeric-named sibling folders of the current folder.
    std::vector<std::string> SiblingTiers() const;         // ascending, e.g. {"64","128","256"}
    std::string CurrentTier() const;                        // current folder leaf if numeric, else ""
    void SwitchTier(const std::string& tier);               // repoint to <parent>/<tier>, keep same filename if present
```

Remove the now-duplicate `selectFile` declaration from the `private:` section (it moves to `public:`).

- [ ] **Step 3: Write the failing `--smoke-tiers`**

In `tools/asset_viewer/AssetViewerApp.h` (public): `static int runSmokeTiers(const char* fixtureDir);`
In `tools/asset_viewer/main.cpp`:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-tiers") == 0)
        return AssetViewerApp::runSmokeTiers(argc >= 3 ? argv[2] : ".");
```

In `tools/asset_viewer/AssetViewerApp.cpp` (include `"FileBrowser.h"`, `<filesystem>`):

```cpp
int AssetViewerApp::runSmokeTiers(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* rel){ return (fs::path(dir) / rel).make_preferred().string(); };

    // Select sample.ktx2 in the 128 tier (selectFile sets folder = parent).
    FileBrowser fb;
    fb.selectFile(p("tiers/128/sample.ktx2"));
    if (fb.CurrentTier() != "128")            return smokeFail("CurrentTier should be 128");
    auto tiers = fb.SiblingTiers();
    if (tiers.size() != 2 || tiers[0] != "128" || tiers[1] != "256")
        return smokeFail("SiblingTiers should be {128,256}");

    // Switch to 256: same filename exists -> selection preserved, now under tiers/256.
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("CurrentTier should be 256 after switch");
    if (!fb.hasSelection())                   return smokeFail("selection should persist (sample exists in 256)");
    if (fb.selectionPath().find("256") == std::string::npos ||
        fb.selectionPath().find("sample.ktx2") == std::string::npos)
        return smokeFail("selectionPath should point at tiers/256/sample.ktx2");

    // Missing tier -> no-op (folder unchanged).
    fb.SwitchTier("999");
    if (fb.CurrentTier() != "256")            return smokeFail("missing tier should be a no-op");

    // Switch to a tier lacking the selected file -> folder switches, no selection.
    fb.selectFile(p("tiers/128/only128.ktx2"));
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("should switch folder even when file absent");
    if (fb.hasSelection())                    return smokeFail("selection should drop when file absent in new tier");

    std::printf("[smoke] PASS tiers (detect/switch/continuity)\n");
    return 0;
}
```

Run: `mc2_asset_viewer --smoke-tiers tests\fixtures\asset_viewer` → Expected: build failure (methods not implemented).

- [ ] **Step 4: Implement the tier methods + `setFolder`**

In `tools/asset_viewer/FileBrowser.cpp` add `#include <algorithm>` at top, and add these definitions (anywhere after `refresh()`):

```cpp
static bool fb_isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

void FileBrowser::setFolder(const std::string& path) {
    std::snprintf(folderPath_, sizeof(folderPath_), "%s", path.c_str());
    refresh();
}

std::string FileBrowser::CurrentTier() const {
    std::string leaf = fs::path(folderPath_).filename().string();
    return fb_isAllDigits(leaf) ? leaf : std::string();
}

std::vector<std::string> FileBrowser::SiblingTiers() const {
    std::vector<std::string> tiers;
    fs::path parent = fs::path(folderPath_).parent_path();
    std::error_code ec;
    if (parent.empty() || !fs::is_directory(parent, ec)) return tiers;
    for (auto it = fs::directory_iterator(parent, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string name = it->path().filename().string();
        if (fb_isAllDigits(name)) tiers.push_back(name);
    }
    std::sort(tiers.begin(), tiers.end(),
              [](const std::string& a, const std::string& b){ return std::stoi(a) < std::stoi(b); });
    return tiers;
}

void FileBrowser::SwitchTier(const std::string& tier) {
    fs::path dst = fs::path(folderPath_).parent_path() / tier;
    std::error_code ec;
    if (!fs::is_directory(dst, ec)) return;   // missing tier -> no-op

    std::string keepName;                     // remember selected filename
    if (selectedIndex_ >= 0 && selectedIndex_ < (int)entries_.size())
        keepName = entries_[selectedIndex_];

    setFolder(dst.string());                  // repoint + rescan (clears entries_/selectedIndex_)
    selectedIndex_ = -1;
    hasSelection_  = false;
    if (!keepName.empty()) {                   // restore selection if same file exists here
        for (int i = 0; i < (int)entries_.size(); ++i) {
            if (entries_[i] == keepName) {
                selectedIndex_ = i;
                selectionPath_ = (fs::path(folderPath_) / keepName).string();
                hasSelection_  = true;
                break;
            }
        }
    }
}
```

(Note: `refresh()` already resets `selectedIndex_ = -1`; the explicit resets in `SwitchTier` keep the intent obvious and guard against future `refresh()` changes.)

- [ ] **Step 5: Build + run to verify it passes**

```
& "C:\mingw64\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-tiers tests\fixtures\asset_viewer
```
Expected: `[smoke] PASS tiers (detect/switch/continuity)`

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/FileBrowser.h tools/asset_viewer/FileBrowser.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp tests/fixtures/asset_viewer/make_fixture.py
git add -f tests/fixtures/asset_viewer/tiers/128/sample.ktx2 \
           tests/fixtures/asset_viewer/tiers/128/only128.ktx2 \
           tests/fixtures/asset_viewer/tiers/256/sample.ktx2
git commit -m "feat(asset-viewer): FileBrowser resolution-tier detect/switch (numeric sibling folders) + --smoke-tiers

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Tier-switcher UI row + final verification

**Files:**
- Modify: `tools/asset_viewer/FileBrowser.cpp`
- Modify: `tools/asset_viewer/README.md`

- [ ] **Step 1: Render the tier row in FileBrowser::draw()**

In `tools/asset_viewer/FileBrowser.cpp` `draw()`, at the very top (before the `"Folder"` label), add a tier row that appears only when more than one tier exists:

```cpp
    std::vector<std::string> tiers = SiblingTiers();
    if (tiers.size() > 1) {
        std::string cur = CurrentTier();
        ImGui::TextUnformatted("Resolution:");
        for (const auto& t : tiers) {
            ImGui::SameLine();
            bool active = (t == cur);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.80f, 1.0f));
            if (ImGui::Button(t.c_str())) SwitchTier(t);
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }
```

No `AssetViewerApp` change is needed: after `SwitchTier` sets `hasSelection_`, the existing `if (browser_.hasSelection()) surface_.setSource(browser_.takeSelection());` in `drawUi()` reloads the same asset at the new tier; Part A keeps the on-screen size constant.

- [ ] **Step 2: Document in README**

Append to `tools/asset_viewer/README.md`:

```markdown
## Resolution tiers + display sizing

When the current folder sits in a set of numeric sibling folders (e.g.
`data/tgl/{128,256,512}` or `data/textures/{64,128,256}`), a **Resolution** row
appears with a button per available tier. Switching tiers keeps the selected
texture (same filename) and reloads it at the new resolution. Only tiers that
exist on disk are shown.

The preview now fits each texture to the view area (1.00× = fit); the zoom slider
multiplies that. Switching resolution tiers — or opening a higher-resolution
texture — no longer changes the on-screen size, only the detail.
```

- [ ] **Step 3: Build + full smoke suite + manual check**

```
& "C:\mingw64\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-fit
& "$dst\mc2_asset_viewer.exe" --smoke-tiers tests\fixtures\asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-decoder
& "$dst\mc2_asset_viewer.exe" --smoke-ktx-parse tests\fixtures\asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-ktx tests\fixtures\asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke-preview tests\fixtures\asset_viewer
& "$dst\mc2_asset_viewer.exe" --smoke tests\fixtures\asset_viewer
```
Expected: every line `[smoke] PASS …`, exit 0.

Manual (optional, GUI): launch the exe; with only `data/tgl/128` present the Resolution row is hidden (correct — one tier). Open a texture; it fills the preview area; the zoom slider scales it; opening another texture keeps the same on-screen size.

- [ ] **Step 4: Commit**

```bash
git add tools/asset_viewer/FileBrowser.cpp tools/asset_viewer/README.md
git commit -m "feat(asset-viewer): resolution-tier switcher UI row + docs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review against spec

- **Part A constant size (spec):** Task 1 `FitTextureDisplaySize` + `--smoke-fit` asserting 128==256==512 display size; Task 2 wires it into `draw()`; `zoom_` persists (no reset in `setSource`). ✓
- **Zoom semantics shift to ×fit (spec):** Task 2 slider `0.25×…8×`, `1×`=fit. ✓
- **Part B tier = numeric sibling folders (spec):** Task 3 `SiblingTiers()`/`CurrentTier()`, digit-only names, sorted numerically. ✓
- **Same-asset continuity (spec):** `SwitchTier` preserves selected filename → existing `hasSelection()` path reloads it (Task 3 + Task 4 note). ✓
- **Only existing tiers offered; ≤1 tier hides row (spec):** Task 4 `if (tiers.size() > 1)`. ✓
- **Missing tier / absent file graceful (spec):** `SwitchTier` no-ops on missing dir; drops selection when file absent — both asserted in `--smoke-tiers`. ✓
- **Generic (tgl + terrain) (spec):** no `tgl` hardcoding; pure parent/numeric-sibling logic. ✓
- **Headless tests (spec):** `--smoke-fit` (pure math), `--smoke-tiers` (filesystem, no GL). ✓
- **No cooking / no geometry / viewer-only (spec):** nothing outside `tools/asset_viewer` + fixtures touched. ✓

Type/name consistency: `FitSize`/`FitTextureDisplaySize`, `SiblingTiers`/`CurrentTier`/`SwitchTier`/`setFolder`/`selectionPath`/`selectFile`, `runSmokeFit`/`runSmokeTiers` used identically across tasks. `selectFile` moved private→public (Task 3 Step 2) — no remaining private declaration.
