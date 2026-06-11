# Asset Viewer S5 Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `mc2_asset_viewer`'s mod workbench modder-friendly — replace manual string fields with discoverable rosters, turn dead validation hooks into real warnings, and add a safe reversible central-manifest install path.

**Architecture:** All Backend-B (no engine render path). Two new engine-independent units (`AppearanceRoster`, `CentralManifestMerge`); the rest folds into `ModWorkbenchPanel`. Every backend unit gets a GL-free `--smoke-*` test in the existing `runSmoke*` idiom. Central merge (writes shared state) is built last.

**Tech Stack:** C++17, SDL2/GLEW/Dear ImGui, `nlohmann/json` (tool-side only — `tools/` is exempt from `check-json-isolation.sh`), Assimp (glTF). Existing substrate: `ModelBrowser`, `TglMeshLoader`, `MaterialSlots`, `OverrideManifest`, `ModWorkbench`, `mclib/model_override_registry`.

---

## Background facts (verified against current code)

- Override key `appearanceName` = the base shape name read from `data/tgl/{X}.ini` (`bdactor.cpp:288-340/357` staticProp, `:4087/4100` tree). **The real on-disk format is FIT-ini typed keys: `st FileName0="2civliving"`, `st FileName1="2civlivingL1"`, `st FileName="2civlivingdam"` — NOT bare `FileName=` (verified: 1806 files use `st FileName*`, zero use bare).** Engine resolve logic (bdactor.cpp:288): reads bare `st FileName` first; if absent, falls back to `st FileName0`. Replicating `readIdString`'s exact token semantics is fragile, so the **roster is intentionally inclusive: it collects ALL `st FileName`/`st FileName<N>` values across `<deploy>/data/tgl/*.ini`, deduped.** This is a superset of the override key — it guarantees the actual resolved key appears in the picker whatever the engine selects; the modder picks the right one (base names are obvious vs `...dam`/`...L1` variants).
- `WorkbenchOverride` (`OverrideManifest.h:6`): `overrideClass, appearanceName, appearanceVerified, sourceRelPath, scale, renderOnly, fallback, lods[]`. `WorkbenchOverrideLod{int lod; std::string sourceRelPath; float distance;}`.
- `ModWorkbench::revalidate(const std::vector<std::string>& missing={})` (`ModWorkbench.cpp:63`) already plumbs `missing` → `SemanticInputs.missingTextures` → `ValidateSemantics` emits `texture-missing` WARN (`WorkbenchValidation.cpp:11`). The UI just never passes a non-empty vector. **The hook is live; only the caller is missing.**
- `ValidateRecordRules` (`OverrideManifest.cpp`) BLOCKS: non-ascending lod, unsafe lod source, unverified appearance, etc. It does NOT currently warn on first-LOD-distance!=0 or LOD0-only — those are new (Task 5).
- `ModelOverrideRegistry` (`mclib/model_override_registry.h`): `int loadFromFile(path, dir)`, `const ModelOverrideRecord* resolve(class, name)`, `int count()`. No `records()` accessor, no `loadFromString`. Merge preserves records via tool-side nlohmann, then round-trips through `loadFromFile`.
- Smoke idiom: `main.cpp` dispatches `--smoke-foo` → `AssetViewerApp::runSmokeFoo(...)`; each prints `[smoke] PASS`/`FAIL` and returns 0/nonzero. Declarations in `AssetViewerApp.h`.
- Emoji ban (`docs/critical_inline_rules.md`): health strip uses ASCII tokens (`ok`/`--`/`warn`/`[B]`/`[W]`), never glyphs.

## File structure

| File | Action | Responsibility |
|------|--------|----------------|
| `tools/asset_viewer/AppearanceRoster.h/.cpp` | create | scan/parse/cache/dedupe `FileName` roster from `data/tgl/*.ini` |
| `tools/asset_viewer/CentralManifestMerge.h/.cpp` | create | safe reversible splice of central `models.json` |
| `tools/asset_viewer/ModWorkbenchPanel.h/.cpp` | modify | stock picker, appearance combo, LOD panel, texture panel, health strip, merge button |
| `tools/asset_viewer/ModWorkbench.h/.cpp` | modify | expose texture-slot state + merge entry point |
| `tools/asset_viewer/AssetViewerApp.h/.cpp` | modify | new `runSmoke*` functions |
| `tools/asset_viewer/main.cpp` | modify | new `--smoke-*` dispatch |
| `tools/asset_viewer/CMakeLists.txt` | modify | add new `.cpp` to target |
| `tools/asset_viewer/tests/fixtures/asset_viewer/` | extend | tgl ini fixtures + central manifest fixtures |
| `tools/asset_viewer/README.md` | modify | document new panels |

`ModWorkbenchPanel.cpp` is ~100 lines today; after these additions it will approach the ~250-line split threshold. If it exceeds ~250 lines during Task 5, extract the LOD + texture sub-panels into `ModWorkbenchPanels_lod_tex.cpp` behind `void DrawLodPanel(WorkbenchOverride&, bool& dirty)` / `void DrawTexturePanel(...)` free functions. Track this; do not pre-split.

---

## Task 1: AppearanceRoster backend + smoke

**Files:**
- Create: `tools/asset_viewer/AppearanceRoster.h`, `tools/asset_viewer/AppearanceRoster.cpp`
- Create fixture: `tools/asset_viewer/tests/fixtures/asset_viewer/tgl_ini/data/tgl/{alpha.ini,beta.ini,dup.ini,noname.ini}`
- Modify: `tools/asset_viewer/CMakeLists.txt`, `tools/asset_viewer/main.cpp`, `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp`

- [ ] **Step 1: Create the fixture .ini files**

`tools/asset_viewer/tests/fixtures/asset_viewer/tgl_ini/data/tgl/alpha.ini`:
```ini
[TGLData]
FileName=Tree_Oak
```
`.../beta.ini`:
```ini
[TGLData]
FileName="atlas_building"
```
`.../dup.ini`:
```ini
[TGLData]
FileName=tree_oak
```
`.../noname.ini`:
```ini
[TGLData]
Foo=bar
```
Expected roster from this fixture: display-preserving, case-dedup, sorted →
`["atlas_building", "Tree_Oak"]` (dup.ini's `tree_oak` collapses into `Tree_Oak`; first-seen spelling wins; `noname.ini` contributes nothing).

- [ ] **Step 2: Write the header `AppearanceRoster.h`**

```cpp
// tools/asset_viewer/AppearanceRoster.h
// S5: enumerable roster of valid override appearance keys.
// Source = unique FileName= values across <deploy>/data/tgl/*.ini
// (the same string the engine uses as the override lookup key; see bdactor.cpp).
// Read-only, cached, refreshable.
#pragma once
#include <string>
#include <vector>

class AppearanceRoster {
public:
    // Scan <deployDir>/data/tgl/*.ini, parse FileName=, trim quotes/whitespace,
    // dedupe case-insensitively (first spelling wins), sort. Idempotent cache;
    // refresh() forces a rescan.
    void load(const std::string& deployDir);
    void refresh(const std::string& deployDir);     // = clear + load
    const std::vector<std::string>& names() const { return names_; }
    int scannedFileCount() const { return scannedFiles_; }   // for tooltip/debug
    // Case-insensitive membership (used to derive appearanceVerified on free-type).
    bool contains(const std::string& name) const;

private:
    std::vector<std::string> names_;
    std::string loadedDir_;
    int  scannedFiles_ = 0;
    bool loaded_ = false;
};
```

- [ ] **Step 3: Write the failing smoke first**

Add declaration to `AssetViewerApp.h` (next to the other workbench smokes, ~line 47):
```cpp
    static int runSmokeAppearanceRoster(const char* fixtureDir);  // S5
```
Add dispatch to `main.cpp` (after the `--smoke-workbench-reload` block, ~line 95):
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-appearance-roster") == 0)
        return AssetViewerApp::runSmokeAppearanceRoster(argc >= 3 ? argv[2] : ".");
```
Add the smoke body to `AssetViewerApp.cpp` (with the other `runSmokeWorkbench*` bodies). Include `"AppearanceRoster.h"` at the top of the file if not present:
```cpp
int AssetViewerApp::runSmokeAppearanceRoster(const char* fixtureDir) {
    AppearanceRoster r;
    std::string dir = std::string(fixtureDir) + "/tgl_ini";
    r.load(dir);
    const auto& n = r.names();
    bool ok = (n.size() == 2)
           && (n[0] == "atlas_building")           // sorted, case-insensitive
           && (n[1] == "Tree_Oak")                 // first spelling preserved
           && r.contains("TREE_OAK")               // case-insensitive membership
           && !r.contains("nonexistent")
           && r.scannedFileCount() == 4;           // alpha,beta,dup,noname
    printf("[smoke] appearance-roster %s (names=%zu files=%d)\n",
           ok ? "PASS" : "FAIL", n.size(), r.scannedFileCount());
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: Add `AppearanceRoster.cpp` to CMake**

In `tools/asset_viewer/CMakeLists.txt`, add `AppearanceRoster.cpp` to the `mc2_asset_viewer` source list (alongside `ModWorkbench.cpp` etc.). Grep for `ModWorkbench.cpp` in that file to find the list.

- [ ] **Step 5: Build to verify the smoke FAILS to link**

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: FAIL — unresolved `AppearanceRoster::load` / `names` / `contains` (no `.cpp` body yet). (If `AppearanceRoster.cpp` is empty it compiles but link fails.)

- [ ] **Step 6: Implement `AppearanceRoster.cpp`**

```cpp
// tools/asset_viewer/AppearanceRoster.cpp
#include "AppearanceRoster.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string trimmed(std::string s) {
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void AppearanceRoster::load(const std::string& deployDir) {
    if (loaded_ && loadedDir_ == deployDir) return;
    names_.clear();
    scannedFiles_ = 0;
    loadedDir_ = deployDir;
    loaded_ = true;

    fs::path tglDir = fs::path(deployDir) / "data" / "tgl";
    std::error_code ec;
    if (!fs::is_directory(tglDir, ec)) return;        // empty roster, never throw

    std::vector<std::string> seenLower;
    for (auto& de : fs::directory_iterator(tglDir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (lower(de.path().extension().string()) != ".ini") continue;
        ++scannedFiles_;
        std::ifstream f(de.path());
        std::string line;
        while (std::getline(f, line)) {
            // Match "FileName" key (case-insensitive), value after '='.
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trimmed(line.substr(0, eq)));
            if (key != "filename") continue;
            std::string val = trimmed(line.substr(eq + 1));
            if (val.empty()) continue;
            std::string lv = lower(val);
            if (std::find(seenLower.begin(), seenLower.end(), lv) != seenLower.end()) break;
            seenLower.push_back(lv);
            names_.push_back(val);                    // preserve original spelling
            break;                                    // one FileName per ini
        }
    }
    std::sort(names_.begin(), names_.end(),
              [](const std::string& a, const std::string& b){ return lower(a) < lower(b); });
}

void AppearanceRoster::refresh(const std::string& deployDir) {
    loaded_ = false;
    loadedDir_.clear();
    load(deployDir);
}

bool AppearanceRoster::contains(const std::string& name) const {
    std::string ln = lower(name);
    for (const auto& n : names_) if (lower(n) == ln) return true;
    return false;
}
```

- [ ] **Step 7: Build + run the smoke; verify PASS**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-appearance-roster tools/asset_viewer/tests/fixtures/asset_viewer
```
Expected: `[smoke] appearance-roster PASS (names=2 files=4)`, exit 0.

- [ ] **Step 8: Commit**

```bash
git add tools/asset_viewer/AppearanceRoster.h tools/asset_viewer/AppearanceRoster.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/main.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/tests/fixtures/asset_viewer/tgl_ini
git commit -m "feat(asset-viewer): AppearanceRoster scans data/tgl/*.ini FileName keys"
```

---

## Task 2: Appearance-key roster combo (replace manual field + checkbox)

**Files:**
- Modify: `tools/asset_viewer/ModWorkbenchPanel.h`, `tools/asset_viewer/ModWorkbenchPanel.cpp`

This is a UI task (no smoke — the derive-verified logic is covered by `AppearanceRoster::contains` in Task 1's smoke). Verify by build + manual launch.

- [ ] **Step 1: Add roster + filter state to `ModWorkbenchPanel.h`**

Add include and members:
```cpp
#include "AppearanceRoster.h"
```
Inside the `private:` block, after `char appe_[128]`:
```cpp
    AppearanceRoster roster_;
    char             apFilter_[128] = {0};   // appearance combo filter
    std::string      deployDir_ = ".";       // remembered for roster load/refresh
```

- [ ] **Step 2: Capture deployDir in `setDeployDir`**

In `ModWorkbenchPanel.cpp`, extend `setDeployDir`:
```cpp
void ModWorkbenchPanel::setDeployDir(const std::string& d) {
    stockPreview_.setDeployDir(d);
    overridePreview_.setDeployDir(d);
    deployDir_ = d;
    roster_.load(d);
}
```

- [ ] **Step 3: Replace the appearance InputText + checkbox block**

In `ModWorkbenchPanel.cpp`, find and DELETE:
```cpp
    auto& rec = wb.record();
    if (appe_[0]=='\0' && !rec.appearanceName.empty()) strncpy(appe_, rec.appearanceName.c_str(), sizeof(appe_)-1);
    ImGui::InputText("Appearance key", appe_, sizeof(appe_));
    rec.appearanceName = appe_;
    ImGui::Checkbox("Appearance key verified (matches engine)", &rec.appearanceVerified);
```
Replace with:
```cpp
    auto& rec = wb.record();
    if (appe_[0]=='\0' && !rec.appearanceName.empty()) strncpy(appe_, rec.appearanceName.c_str(), sizeof(appe_)-1);
    // Free-type field; verified state is DERIVED (roster pick or roster match = true).
    if (ImGui::InputText("Appearance key", appe_, sizeof(appe_))) {
        rec.appearanceName = appe_;
        rec.appearanceVerified = roster_.contains(rec.appearanceName);
    } else {
        rec.appearanceName = appe_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh roster")) roster_.refresh(deployDir_);
    ImGui::TextDisabled("appearance source: %d data/tgl/*.ini scanned, %zu keys%s",
        roster_.scannedFileCount(), roster_.names().size(),
        rec.appearanceVerified ? "  [verified: roster match]" : "  [unverified: not in roster]");
    // Filterable roster picker.
    ImGui::InputText("filter##appearance", apFilter_, sizeof(apFilter_));
    if (ImGui::BeginListBox("##appearance_roster", ImVec2(-FLT_MIN, 120))) {
        std::string lf; for (char* p = apFilter_; *p; ++p) lf += (char)std::tolower((unsigned char)*p);
        for (const auto& n : roster_.names()) {
            std::string ln; for (char c : n) ln += (char)std::tolower((unsigned char)c);
            if (!lf.empty() && ln.find(lf) == std::string::npos) continue;
            if (ImGui::Selectable(n.c_str(), n == rec.appearanceName)) {
                strncpy(appe_, n.c_str(), sizeof(appe_)-1); appe_[sizeof(appe_)-1]='\0';
                rec.appearanceName = n;
                rec.appearanceVerified = true;          // explicit roster pick
            }
        }
        ImGui::EndListBox();
    }
```
Add `#include <cfloat>` and `#include <cctype>` near the top of `ModWorkbenchPanel.cpp` if not already present (for `FLT_MIN`, `tolower`).

- [ ] **Step 4: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/ModWorkbenchPanel.h tools/asset_viewer/ModWorkbenchPanel.cpp
git commit -m "feat(asset-viewer): appearance-key roster combo with derived verified state"
```

---

## Task 3: Stock roster picker via ModelBrowser

**Files:**
- Modify: `tools/asset_viewer/ModWorkbenchPanel.h`, `tools/asset_viewer/ModWorkbenchPanel.cpp`

UI task; verify by build + launch.

- [ ] **Step 1: Add a `ModelBrowser` member**

In `ModWorkbenchPanel.h`, add include + member:
```cpp
#include "ModelBrowser.h"
```
private block:
```cpp
    ModelBrowser stockBrowser_;
```

- [ ] **Step 2: Replace the manual stock InputText with the browser**

In `ModWorkbenchPanel.cpp`, find and DELETE:
```cpp
    static char tgl[256] = "data/tgl/2civliving.tgl";
    ImGui::InputText("Stock .tgl", tgl, sizeof(tgl));
    if (ImGui::Button("Bind stock")) wb.bindStock(tgl);
```
Replace with:
```cpp
    ImGui::TextUnformatted("Stock prop (click to bind):");
    stockBrowser_.draw();   // filter box + scrollable .tgl list; tooltip shows full path
    if (stockBrowser_.hasSelection()) {
        std::string pick = stockBrowser_.takeSelection();
        wb.bindStock(pick);
    }
    if (wb.hasStock())
        ImGui::TextDisabled("bound stock: %s", wb.stockMesh().ok ? "ok" : "--");
```

- [ ] **Step 3: Add the disambiguating tooltip to `ModelBrowser::draw`**

`ModelBrowser` rows currently show the name. In `ModelBrowser.cpp`, find the `ImGui::Selectable(...)` row render and add an `IsItemHovered` tooltip with the full path. Locate the loop over `filtered_` and after the `Selectable` call add:
```cpp
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", filtered_[i].c_str());   // full relative path
```
(If the loop variable is not `i`, use the loop's element expression — show the full entry string.)

- [ ] **Step 4: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/ModWorkbenchPanel.h tools/asset_viewer/ModWorkbenchPanel.cpp tools/asset_viewer/ModelBrowser.cpp
git commit -m "feat(asset-viewer): stock roster picker via ModelBrowser with path tooltip"
```

---

## Task 4: Texture missing-warning plumbing

**Files:**
- Modify: `tools/asset_viewer/ModWorkbench.h`, `tools/asset_viewer/ModWorkbench.cpp`, `tools/asset_viewer/ModWorkbenchPanel.h`, `tools/asset_viewer/ModWorkbenchPanel.cpp`, `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp`, `tools/asset_viewer/main.cpp`

The `revalidate(missing)` hook is live; we add a texture-slot model to `ModWorkbench` and a panel that feeds it. Texture-missing is WARN (already so in `ValidateSemantics:11`), never BLOCK.

- [ ] **Step 1: Add a texture-slot model to `ModWorkbench.h`**

In `ModWorkbench`'s public section:
```cpp
    // S5: texture-set slots a modder assigns; unresolved paths feed missing-texture WARNs.
    struct TextureSlot { std::string label; std::string path; };
    std::vector<TextureSlot>& textureSlots() { return textureSlots_; }
    // Recomputes warnings, passing slot paths that don't exist on disk as missing.
    void revalidateWithTextures();
```
private section:
```cpp
    std::vector<TextureSlot> textureSlots_{
        {"Base Color", ""}, {"Normal", ""}, {"ORM", ""}, {"Emissive", ""}};
```

- [ ] **Step 2: Write the failing smoke**

`AssetViewerApp.h`:
```cpp
    static int runSmokeTextureMissingWarn(const char* fixtureDir);  // S5
```
`main.cpp` dispatch:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-texture-missing-warn") == 0)
        return AssetViewerApp::runSmokeTextureMissingWarn(argc >= 3 ? argv[2] : ".");
```
`AssetViewerApp.cpp` body (include `"ModWorkbench.h"` if needed):
```cpp
int AssetViewerApp::runSmokeTextureMissingWarn(const char* fixtureDir) {
    ModWorkbench wb;
    // Minimal valid record so only texture state varies.
    auto& rec = wb.record();
    rec.overrideClass = "staticProp";
    rec.appearanceName = "smoke_prop";
    rec.appearanceVerified = true;
    rec.sourceRelPath = "model.glb";
    wb.textureSlots()[0].path = std::string(fixtureDir) + "/does_not_exist_basecolor.png";
    wb.revalidateWithTextures();
    int missingWarns = 0, blocks = 0;
    for (const auto& w : wb.warnings()) {
        if (w.code == std::string("texture-missing")) ++missingWarns;
        if (w.severity == WarnSeverity::Block) ++blocks;
    }
    bool ok = (missingWarns == 1) && (blocks == 0);   // WARN, never BLOCK
    printf("[smoke] texture-missing-warn %s (missing=%d blocks=%d)\n",
           ok ? "PASS" : "FAIL", missingWarns, blocks);
    return ok ? 0 : 1;
}
```
(Confirm the `Warning` struct field is `.code`; in `OverrideManifest.cpp` warnings are pushed as `{WarnSeverity::Block,c,m}` — verify the member order/name in `WorkbenchWarning.h` and adjust `w.code`/`w.message` accessors if different.)

- [ ] **Step 3: Build; verify smoke FAILS to link**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: FAIL — `ModWorkbench::revalidateWithTextures` unresolved.

- [ ] **Step 4: Implement `revalidateWithTextures` in `ModWorkbench.cpp`**

Add `#include <filesystem>` at the top if absent, then:
```cpp
void ModWorkbench::revalidateWithTextures() {
    std::vector<std::string> missing;
    for (const auto& s : textureSlots_) {
        if (s.path.empty()) continue;
        std::error_code ec;
        if (!std::filesystem::exists(s.path, ec)) missing.push_back(s.path);
    }
    revalidate(missing);
}
```

- [ ] **Step 5: Build + run smoke; verify PASS**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-texture-missing-warn tools/asset_viewer/tests/fixtures/asset_viewer
```
Expected: `[smoke] texture-missing-warn PASS (missing=1 blocks=0)`, exit 0.

- [ ] **Step 6: Add the texture panel UI to `ModWorkbenchPanel.cpp`**

Replace the bare `wb.revalidate();` call in `draw()` with a texture panel + the texture-aware revalidate. Find:
```cpp
    wb.revalidate();
    ImGui::Separator(); ImGui::TextUnformatted("Warnings:");
```
Replace with:
```cpp
    ImGui::Separator();
    ImGui::TextUnformatted("Textures (assign to surface missing-texture warnings):");
    for (auto& slot : wb.textureSlots()) {
        char buf[260]; strncpy(buf, slot.path.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        ImGui::PushID(slot.label.c_str());
        if (ImGui::InputText(slot.label.c_str(), buf, sizeof(buf))) slot.path = buf;
        ImGui::PopID();
    }
    wb.revalidateWithTextures();
    ImGui::Separator(); ImGui::TextUnformatted("Warnings:");
```
(This replaces the previous unconditional `wb.revalidate()` — `revalidateWithTextures` calls `revalidate(missing)` internally, so validation still runs every frame.)

- [ ] **Step 7: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 8: Commit**

```bash
git add tools/asset_viewer/ModWorkbench.h tools/asset_viewer/ModWorkbench.cpp \
        tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): texture-set panel feeds missing-texture WARNs"
```

---

## Task 5: LOD-chain panel + smoke

**Files:**
- Modify: `tools/asset_viewer/OverrideManifest.cpp` (new WARN rules), `tools/asset_viewer/WorkbenchValidation.cpp` (first-LOD/LOD0-only WARNs), `tools/asset_viewer/ModWorkbenchPanel.cpp` (table UI), `tools/asset_viewer/AssetViewerApp.{h,cpp}`, `tools/asset_viewer/main.cpp`

Note: the existing ascending-and-safe-source rules already BLOCK in `ValidateRecordRules`. The NEW rules (first-LOD-distance!=0, LOD0-only) are WARNs and belong in `ValidateSemantics` (which emits `WarnSeverity::Warn`). Put them there.

- [ ] **Step 1: Write the failing smoke**

`AssetViewerApp.h`:
```cpp
    static int runSmokeLodEditValidate();  // S5
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-lod-edit-validate") == 0)
        return AssetViewerApp::runSmokeLodEditValidate();
```
`AssetViewerApp.cpp` body:
```cpp
int AssetViewerApp::runSmokeLodEditValidate() {
    auto countCode = [](const std::vector<Warning>& ws, const char* code, WarnSeverity sev){
        int n=0; for (auto& w: ws) if (w.code==std::string(code) && w.severity==sev) ++n; return n;
    };
    bool ok = true;
    // Case A: ascending lods, first distance 0 -> no lod-order BLOCK, no first-lod WARN.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        r.lods = {{1,"lod1.glb",0.0f},{2,"lod2.glb",50.0f}};
        auto br = ValidateRecordRules(r);
        SemanticInputs si; si.hasImpostorLod = !r.lods.empty();
        auto sm = ValidateSemantics(r, si);
        ok &= countCode(br,"lod-order",WarnSeverity::Block)==0;
        ok &= countCode(sm,"lod-first-distance",WarnSeverity::Warn)==1; // lod[0].distance must be 0; here it's 0 -> OK? see rule
    }
    // Case B: non-ascending -> lod-order BLOCK fires.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        r.lods = {{2,"lod2.glb",50.0f},{1,"lod1.glb",10.0f}};
        auto br = ValidateRecordRules(r);
        ok &= countCode(br,"lod-order",WarnSeverity::Block)>=1;
    }
    // Case C: LOD0-only (no entries) -> lod0-only WARN.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        SemanticInputs si; si.hasImpostorLod = !r.lods.empty();
        auto sm = ValidateSemantics(r, si);
        ok &= countCode(sm,"lod0-only",WarnSeverity::Warn)==1;
    }
    printf("[smoke] lod-edit-validate %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
```
NOTE on Case A: the first-LOD-distance rule fires when the FIRST LOD entry's
`distance != 0`. In Case A `lods[0].distance==0`, so `lod-first-distance` should
NOT fire — fix the Case A assertion to `==0` once the rule below is written. (The
draft asserts `==1` to force a first failing run; correct it in Step 3 after
implementing the rule so Case A reflects "distance 0 = no warn".)

- [ ] **Step 2: Build; verify smoke FAILS**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-lod-edit-validate
```
Expected: FAIL (new WARN codes don't exist yet).

- [ ] **Step 3: Add the new LOD WARN rules to `WorkbenchValidation.cpp`**

In `ValidateSemantics`, after the existing texture/overdraw warns, add:
```cpp
    // S5 LOD authoring guidance (WARN, never BLOCK).
    if (rec.lods.empty()) {
        warn("lod0-only", "only LOD0 present — no impostor/far LODs (acceptable, but no distance falloff)");
    } else if (rec.lods.front().distance != 0.0f) {
        warn("lod-first-distance", "first LOD entry distance should be 0 (LOD0 baseline); got "
             + std::to_string(rec.lods.front().distance));
    }
```
Then correct the Case A assertion in the smoke to:
```cpp
        ok &= countCode(sm,"lod-first-distance",WarnSeverity::Warn)==0; // distance 0 = no warn
```

- [ ] **Step 4: Build + run smoke; verify PASS**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe --smoke-lod-edit-validate
```
Expected: `[smoke] lod-edit-validate PASS`, exit 0.

- [ ] **Step 5: Add the LOD-chain table UI to `ModWorkbenchPanel.cpp`**

Insert before the export block (before `ImGui::Separator(); static char bundleId...`):
```cpp
    ImGui::Separator();
    ImGui::TextUnformatted("LOD chain (LOD0 = the dropped GLB; add lower-detail entries):");
    {
        auto& lods = rec.lods;
        if (ImGui::BeginTable("lods", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("LOD"); ImGui::TableSetupColumn("Source GLB");
            ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int removeIdx = -1, moveUp = -1, moveDown = -1;
            for (int i = 0; i < (int)lods.size(); ++i) {
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", lods[i].lod);
                ImGui::TableSetColumnIndex(1);
                char sb[260]; strncpy(sb, lods[i].sourceRelPath.c_str(), sizeof(sb)-1); sb[sizeof(sb)-1]='\0';
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##src", sb, sizeof(sb))) lods[i].sourceRelPath = sb;
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputFloat("##dist", &lods[i].distance, 0,0,"%.1f");
                ImGui::TableSetColumnIndex(3);
                bool ascOk = (i == 0) || (lods[i].distance > lods[i-1].distance);
                ImGui::TextColored(ascOk?ImVec4(0.6f,1,0.6f,1):ImVec4(1,0.5f,0.5f,1), ascOk?"ok":"order");
                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton("Up"))   moveUp = i; ImGui::SameLine();
                if (ImGui::SmallButton("Dn"))   moveDown = i; ImGui::SameLine();
                if (ImGui::SmallButton("X"))    removeIdx = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (removeIdx >= 0) lods.erase(lods.begin()+removeIdx);
            if (moveUp > 0)               std::swap(lods[moveUp], lods[moveUp-1]);
            if (moveDown >= 0 && moveDown+1 < (int)lods.size()) std::swap(lods[moveDown], lods[moveDown+1]);
            // Reindex lod by row order: entries start at 1 (LOD0 = record.source).
            for (int i = 0; i < (int)lods.size(); ++i) lods[i].lod = i + 1;
        }
        if (ImGui::Button("Add LOD")) rec.lods.push_back({(int)rec.lods.size()+1, "", 0.0f});
    }
```
Add `#include <utility>` (for `std::swap`) if not present.

- [ ] **Step 6: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add tools/asset_viewer/WorkbenchValidation.cpp tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): LOD-chain panel + first-LOD/LOD0-only WARNs"
```

---

## Task 6: Record / health strip

**Files:**
- Modify: `tools/asset_viewer/ModWorkbenchPanel.cpp`

Pure aggregation of existing state; no new logic, no smoke. ASCII tokens (emoji ban).

- [ ] **Step 1: Add the strip at the top of `draw()`**

Right after the `ImGui::Separator();` that follows the Backend-A caveat line (before the `if (!wb.hasOverride())` early-return), insert:
```cpp
    {
        auto& rec = wb.record();
        int blocks = 0, warns = 0;
        for (const auto& w : wb.warnings())
            (w.severity == WarnSeverity::Block ? blocks : warns)++;
        ImGui::TextUnformatted("Health:");
        ImGui::SameLine();
        ImGui::Text("Key: %s:%s  Stock: %s  Appearance: %s  LODs: %zu  Validation: %d[B] %d[W]",
            rec.overrideClass.c_str(),
            rec.appearanceName.empty()? "--" : rec.appearanceName.c_str(),
            wb.hasStock()? "ok" : "--",
            rec.appearanceName.empty()? "--" : (rec.appearanceVerified? "roster" : "free"),
            rec.lods.size(), blocks, warns);
        ImGui::Separator();
    }
```
(Placed before the early return so it shows even pre-override with `--` tokens. `wb.warnings()` is empty until the first `revalidate*` call; that's fine — it reads 0[B] 0[W] until an override loads.)

- [ ] **Step 2: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add tools/asset_viewer/ModWorkbenchPanel.cpp
git commit -m "feat(asset-viewer): record/health strip aggregating validation state"
```

---

## Task 7: CentralManifestMerge + smoke (LAST — writes shared state)

**Files:**
- Create: `tools/asset_viewer/CentralManifestMerge.h`, `tools/asset_viewer/CentralManifestMerge.cpp`
- Create fixtures: `tools/asset_viewer/tests/fixtures/asset_viewer/central/models.json`
- Modify: `tools/asset_viewer/CMakeLists.txt`, `tools/asset_viewer/ModWorkbench.{h,cpp}`, `tools/asset_viewer/ModWorkbenchPanel.cpp`, `tools/asset_viewer/AssetViewerApp.{h,cpp}`, `tools/asset_viewer/main.cpp`

- [ ] **Step 1: Create the central fixture**

`tools/asset_viewer/tests/fixtures/asset_viewer/central/models.json`:
```json
{
  "overrides": [
    {"type":"model","class":"staticProp","replaces":"staticProp:propA","source":"a.glb","renderOnly":true,"scale":1.0,"fallback":"stock"},
    {"type":"model","class":"tree","replaces":"tree:propB","source":"b.glb","renderOnly":true,"scale":1.0,"fallback":"stock"}
  ]
}
```

- [ ] **Step 2: Write the header `CentralManifestMerge.h`**

```cpp
// tools/asset_viewer/CentralManifestMerge.h
// S5: safe reversible install of one override record into a central models.json.
// Preserves all unrelated records; replaces a same-key record. Writes <file>.bak
// before any change; round-trips the result through ModelOverrideRegistry.
// tools/ is exempt from check-json-isolation.sh, so this TU may use nlohmann.
#pragma once
#include <string>
#include "OverrideManifest.h"   // WorkbenchOverride

struct MergeResult {
    bool ok = false;
    std::string message;
    int recordCount = 0;       // records in the merged file
    bool replacedExisting = false;
};

// manifestPath = path to the central models.json (created if absent).
// On any failure the original file is left untouched (a half-written temp is
// discarded) and .bak — if it was written — remains. Never throws.
MergeResult MergeIntoCentralManifest(const std::string& manifestPath,
                                     const WorkbenchOverride& rec);
```

- [ ] **Step 3: Write the failing smoke**

`AssetViewerApp.h`:
```cpp
    static int runSmokeCentralMergePreserve(const char* fixtureDir, const char* tmpDir);  // S5
```
`main.cpp`:
```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-central-merge-preserve") == 0)
        return AssetViewerApp::runSmokeCentralMergePreserve(
            argc >= 3 ? argv[2] : ".", argc >= 4 ? argv[3] : ".");
```
`AssetViewerApp.cpp` (include `"CentralManifestMerge.h"`, `"OverrideManifest.h"`, `<filesystem>`, and `"model_override_registry.h"` — the bare name the existing workbench smokes use; `AssetViewerApp.cpp:1291` already includes it, so mclib is on the include path):
```cpp
int AssetViewerApp::runSmokeCentralMergePreserve(const char* fixtureDir, const char* tmpDir) {
    namespace fs = std::filesystem;
    fs::path src = fs::path(fixtureDir) + "/central/models.json";
    fs::path dir = fs::path(tmpDir) / "central_merge_smoke";
    std::error_code ec; fs::create_directories(dir, ec);
    fs::path dst = dir / "models.json";
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);

    auto mk = [](const char* cls, const char* app, const char* src){
        WorkbenchOverride r; r.overrideClass=cls; r.appearanceName=app;
        r.appearanceVerified=true; r.sourceRelPath=src; return r;
    };
    bool ok = true;
    // Merge C (new) -> A,B,C present.
    auto rC = MergeIntoCentralManifest(dst.string(), mk("staticProp","propC","c.glb"));
    ok &= rC.ok && rC.recordCount==3 && !rC.replacedExisting;
    {
        ModelOverrideRegistry g; g.loadFromFile(dst.string(), dir.string());
        ok &= g.count()==3;
        ok &= g.resolve("staticProp","propA")!=nullptr;
        ok &= g.resolve("tree","propB")!=nullptr;
        ok &= g.resolve("staticProp","propC")!=nullptr;
    }
    // Merge B replacement (new source) -> A,new-B,C; count stays 3.
    auto rB = MergeIntoCentralManifest(dst.string(), mk("tree","propB","b2.glb"));
    ok &= rB.ok && rB.recordCount==3 && rB.replacedExisting;
    {
        ModelOverrideRegistry g; g.loadFromFile(dst.string(), dir.string());
        ok &= g.count()==3;
        const auto* b = g.resolve("tree","propB");
        ok &= (b && b->sourceRelPath=="b2.glb");
        ok &= g.resolve("staticProp","propA")!=nullptr;
        ok &= g.resolve("staticProp","propC")!=nullptr;
    }
    // .bak exists after writes.
    ok &= fs::exists(dst.string()+".bak");
    // Failure path: merging a BLOCK-invalid record (unverified) leaves file intact.
    {
        std::string before; { std::ifstream f(dst); std::stringstream ss; ss<<f.rdbuf(); before=ss.str(); }
        WorkbenchOverride bad = mk("staticProp","propD","d.glb"); bad.appearanceVerified=false;
        auto rBad = MergeIntoCentralManifest(dst.string(), bad);
        std::string after; { std::ifstream f(dst); std::stringstream ss; ss<<f.rdbuf(); after=ss.str(); }
        ok &= !rBad.ok && (before==after);
    }
    printf("[smoke] central-merge-preserve %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
```
Add `#include <fstream>` and `#include <sstream>` to `AssetViewerApp.cpp` if absent.

- [ ] **Step 4: Add `CentralManifestMerge.cpp` to CMake + build to verify smoke FAILS**

Add `CentralManifestMerge.cpp` to the target in `CMakeLists.txt`, then:
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: FAIL — `MergeIntoCentralManifest` unresolved.

- [ ] **Step 5: Implement `CentralManifestMerge.cpp`**

```cpp
// tools/asset_viewer/CentralManifestMerge.cpp
#include "CentralManifestMerge.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "model_override_registry.h"   // mclib on include path (see AssetViewerApp.cpp:1291)

using nlohmann::json;
namespace fs = std::filesystem;

static std::string lower(std::string s){ for(char&c:s) c=(char)std::tolower((unsigned char)c); return s; }

MergeResult MergeIntoCentralManifest(const std::string& manifestPath,
                                     const WorkbenchOverride& rec) {
    MergeResult out;
    // 1. BLOCK pre-check (same authority as exportBundle).
    for (const auto& w : ValidateRecordRules(rec)) {
        if (w.severity == WarnSeverity::Block) {
            out.message = std::string("fix BLOCK before merge: ") + w.message;
            return out;   // original file untouched
        }
    }

    const std::string key = lower(rec.overrideClass) + ":" + lower(rec.appearanceName);

    // 2. Parse existing overrides array (preserve unknown fields verbatim).
    json root;
    if (fs::exists(manifestPath)) {
        std::ifstream f(manifestPath);
        try { f >> root; }
        catch (...) { out.message = "central models.json is not valid JSON — refusing to overwrite"; return out; }
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array())
        root = json{{"overrides", json::array()}};

    // 3. Build this record's JSON object via the existing serializer (single-record).
    std::string oneJson = ToModelsJson(std::vector<WorkbenchOverride>{rec});
    json oneRoot = json::parse(oneJson);
    json newObj = oneRoot["overrides"][0];

    // 4. Splice by key: replace same "replaces", else append.
    bool replaced = false;
    for (auto& e : root["overrides"]) {
        if (e.contains("replaces") && lower(e["replaces"].get<std::string>()) == key) {
            e = newObj; replaced = true; break;
        }
    }
    if (!replaced) root["overrides"].push_back(newObj);

    // 5. Write .bak (if a file exists), then atomic temp -> rename.
    std::error_code ec;
    if (fs::exists(manifestPath)) fs::copy_file(manifestPath, manifestPath + ".bak",
                                                fs::copy_options::overwrite_existing, ec);
    std::string tmp = manifestPath + ".tmp";
    { std::ofstream o(tmp, std::ios::binary);
      if (!o) { out.message = "cannot open temp for write"; return out; }
      o << root.dump(2) << "\n";
      if (!o.good()) { o.close(); fs::remove(tmp, ec); out.message="write failed"; return out; } }
    fs::rename(tmp, manifestPath, ec);
    if (ec) { fs::remove(tmp, ec); out.message="atomic rename failed"; return out; }

    // 6. Round-trip verify via the engine-faithful registry.
    ModelOverrideRegistry g;
    g.loadFromFile(manifestPath, fs::path(manifestPath).parent_path().string());
    if (g.resolve(rec.overrideClass.c_str(), rec.appearanceName.c_str()) == nullptr) {
        // Roll back to .bak so the modder's prior file is restored.
        if (fs::exists(manifestPath + ".bak"))
            fs::copy_file(manifestPath + ".bak", manifestPath,
                          fs::copy_options::overwrite_existing, ec);
        out.message = "round-trip verify failed — rolled back to .bak";
        return out;
    }

    out.ok = true;
    out.replacedExisting = replaced;
    out.recordCount = g.count();
    out.message = replaced ? "replaced existing record" : "appended new record";
    return out;
}
```

- [ ] **Step 6: Build + run smoke; verify PASS**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe \
  --smoke-central-merge-preserve tools/asset_viewer/tests/fixtures/asset_viewer "$TEMP"
```
Expected: `[smoke] central-merge-preserve PASS`, exit 0.

- [ ] **Step 7: Add the "Append/Merge to Central models.json" button**

In `ModWorkbenchPanel.cpp`, after the existing "Export draft bundle" block (after the `ImGui::TextDisabled("Writes <out>/...")` line), add:
```cpp
    ImGui::Separator();
    static char centralPath[260] = "data/model_overrides/models.json";
    static std::string mergeMsg;
    ImGui::InputText("Central models.json", centralPath, sizeof(centralPath));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Append/Merge to Central models.json")) {
        MergeResult r = MergeIntoCentralManifest(centralPath, wb.record());
        mergeMsg = (r.ok ? "OK: " : "FAILED: ") + r.message;
    }
    ImGui::EndDisabled();
    if (!mergeMsg.empty()) ImGui::TextWrapped("%s", mergeMsg.c_str());
    ImGui::TextDisabled("Writes a .bak first, preserves your other overrides, round-trip verified. Draft export above stays available.");
```
Add `#include "CentralManifestMerge.h"` to the top of `ModWorkbenchPanel.cpp`.

- [ ] **Step 8: Build; verify compiles**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2_asset_viewer
```
Expected: builds clean.

- [ ] **Step 9: Commit**

```bash
git add tools/asset_viewer/CentralManifestMerge.h tools/asset_viewer/CentralManifestMerge.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/ModWorkbenchPanel.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp \
        tools/asset_viewer/tests/fixtures/asset_viewer/central
git commit -m "feat(asset-viewer): safe reversible central models.json merge"
```

---

## Task 8: Full smoke sweep + docs

**Files:**
- Modify: `tools/asset_viewer/README.md`

- [ ] **Step 1: Run every viewer smoke; confirm no regression**

Run all four new smokes plus the 6 pre-existing workbench smokes + 4 viewer smokes (decoder/sphere/fit-material/mesh-orient). Confirm each prints `PASS`, exit 0:
```bash
EXE=build64/out/tools/asset_viewer/RelWithDebInfo/mc2_asset_viewer.exe
FX=tools/asset_viewer/tests/fixtures/asset_viewer
$EXE --smoke-appearance-roster $FX
$EXE --smoke-texture-missing-warn $FX
$EXE --smoke-lod-edit-validate
$EXE --smoke-central-merge-preserve $FX "$TEMP"
$EXE --smoke-workbench-link
$EXE --smoke-workbench-validate $FX
# ...plus the remaining workbench + decoder/sphere/fit/mesh-orient smokes
```
Expected: all PASS.

- [ ] **Step 2: Update `README.md`**

Add a "Mod Workbench S5" section documenting: stock roster picker, appearance roster (source = `data/tgl/*.ini` FileName), LOD-chain panel, texture-set panel, health strip, and the two export paths (draft bundle vs central merge with .bak safety). Note Backend-A v2 is still deferred.

- [ ] **Step 3: Commit**

```bash
git add tools/asset_viewer/README.md
git commit -m "docs(asset-viewer): document S5 workbench panels"
```

---

## Self-review notes

- **Spec coverage:** stock picker (T3), appearance roster (T1+T2), LOD panel (T5), texture panel (T4), central merge (T7), health strip (T6), all four `--smoke-*` (T1/T4/T5/T7), build order matches spec, READMEs (T8). Covered.
- **WARN-not-BLOCK textures:** confirmed — `ValidateSemantics:11` emits `WarnSeverity::Warn`; smoke asserts `blocks==0`.
- **Merge safety:** `.bak` before write, temp+rename atomic, round-trip rollback, BLOCK pre-check, failure-leaves-intact — all asserted in T7 smoke.
- **Emoji ban:** health strip uses ASCII tokens only.
- **Verified:** `Warning{severity, code, message}` (`WorkbenchWarning.h`) matches the `.code`/`.severity`/`.message` usage in T4/T5/T7 smokes. Registry include is the bare `"model_override_registry.h"` (`AssetViewerApp.cpp:1291`); T7 reuses it.
