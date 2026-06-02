# Asset Viewer KTX2 Decoder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `.ktx2` (RGBA8 + stored BC7) browse + preview to `mc2_asset_viewer` behind a pluggable texture-decoder registry, reusing the GL-free `RenderCore/KtxLoader`, without disturbing the existing PNG/JPG/TGA/BMP path.

**Architecture:** A small `ITextureDecoder` seam returning a `DecodedTexture` (with an ownership flag). A `TextureDecoderRegistry` dispatches by file extension and is the single source of truth for the browser's supported-extension filter. Two decoders: `LegacyImageDecoder` (wraps the existing `UiEditorImageCache`, cache-owned textures) and `Ktx2Decoder` (wraps a viewer-side `loadKtx2Image` adapter over `RenderCore::ktxLoadRgba8`, uploads RGBA8 via `glTexImage2D` and BC7 via `glCompressedTexImage2D` with BPTC, preview-owned textures).

**Tech Stack:** C++17, SDL2 + GLEW + OpenGL (core 3.0 ctx; BC7 needs the `GL_ARB_texture_compression_bptc` extension), Dear ImGui. Fixture-based `--smoke*` harness (no external test framework). KTX2 fixtures hand-written by `tests/fixtures/asset_viewer/make_fixture.py` (no encoder dependency).

**Scope:** KTX2 only. No DDS, no Basis/supercompression transcode, no CPU BC7 transcode, no asset cooking. No RenderCore link beyond compiling `RenderCore/KtxLoader.cpp` into the viewer.

---

## File structure

New under `tools/asset_viewer/`:
- `TextureDecoder.h` — `DecodedTexture` struct + `ITextureDecoder` interface.
- `TextureDecoderRegistry.{h,cpp}` — ordered decoder list, dispatch by extension, `supportedExtensions()`/`isSupported()`, and the process-wide `textureDecoderRegistry()` accessor.
- `LegacyImageDecoder.{h,cpp}` — wraps `UiEditorImageCache_Get` (`ownsGlTexture=false`).
- `Ktx2Decoder.{h,cpp}` — `loadKtx2Image` adapter (+ header classify) and GL upload (`ownsGlTexture=true`).

Modified:
- `tools/asset_viewer/TextureExtensions.cpp` — `IsSupportedTextureFile` delegates to the registry.
- `tools/asset_viewer/TextureMetadata.{h,cpp}` — add `formatLabel` + `mipCount`.
- `tools/asset_viewer/TexturePreview2D.{h,cpp}` — load via registry; ownership-correct delete on replace/shutdown; render `error`.
- `tools/asset_viewer/TextureInspectorPanel.cpp` — show `formatLabel` + `mipCount`.
- `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` — add `--smoke-decoder`, `--smoke-ktx-parse`, `--smoke-ktx`.
- `tools/asset_viewer/CMakeLists.txt` — add new sources + `RenderCore/KtxLoader.cpp` + `RenderCore` include dir.
- `tools/asset_viewer/README.md` — document `.ktx2` + BC7/BPTC note.
- `tests/fixtures/asset_viewer/make_fixture.py` — emit KTX2 fixtures.

## Conventions (read once)

- **GL texture id** stored as `uint32_t`. Show in ImGui via `ImGui::Image((ImTextureID)(intptr_t)glTexture, size)`. Legacy cache returns `ImTextureID`; convert with `(uint32_t)(intptr_t)tex->textureId`.
- **Smoke pattern:** `main.cpp` parses `argv[1]`; `AssetViewerApp` static method runs assertions, prints `[smoke] PASS` / `[smoke] FAIL: <reason>` / `[smoke] SKIP <reason>`, returns `0` (PASS or SKIP) / `1` (FAIL). GL smokes copy the SDL/GL bring-up from the existing `runSmoke` (`AssetViewerApp.cpp:63-74`).
- **KtxImage** (`RenderCore/KtxLoader.h`) fields used: `pixels`, `width`, `height`, `isSrgb`, `mipCount`, `mipByteOffsets`, `vkFormat`, `isCompressed`. Per-level byte size = `(lvl+1<mipCount ? mipByteOffsets[lvl+1] : pixels.size()) - mipByteOffsets[lvl]`.

---

## Task 1: Decoder seam + registry + legacy decoder + filter delegation

**Files:**
- Create: `tools/asset_viewer/TextureDecoder.h`
- Create: `tools/asset_viewer/TextureDecoderRegistry.h`, `tools/asset_viewer/TextureDecoderRegistry.cpp`
- Create: `tools/asset_viewer/LegacyImageDecoder.h`, `tools/asset_viewer/LegacyImageDecoder.cpp`
- Modify: `tools/asset_viewer/TextureExtensions.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt`
- Modify: `tools/asset_viewer/AssetViewerApp.h`, `tools/asset_viewer/AssetViewerApp.cpp`, `tools/asset_viewer/main.cpp`

- [ ] **Step 1: Write the decoder seam header**

```cpp
// tools/asset_viewer/TextureDecoder.h
#pragma once
#include <cstdint>
#include <string>

// Result of decoding+uploading one texture file. glTexture==0 means failure
// (error holds a friendly message). ownsGlTexture decides who frees it:
//   true  -> the consumer (TexturePreview2D) must glDeleteTextures it.
//   false -> the GL texture is owned elsewhere (e.g. UiEditorImageCache); do NOT delete.
struct DecodedTexture {
    uint32_t    glTexture     = 0;
    int         width         = 0;
    int         height        = 0;
    int         mipCount      = 1;
    std::string formatLabel;            // "RGBA8", "RGBA8 (sRGB)", "BC7 (sRGB), 9 mips"
    bool        isCompressed  = false;
    bool        ownsGlTexture = false;
    std::string error;                  // empty on success
};

struct ITextureDecoder {
    virtual ~ITextureDecoder() = default;
    // extLower is the lowercased extension WITHOUT the dot, e.g. "ktx2".
    virtual bool          handles(const std::string& extLower) const = 0;
    virtual DecodedTexture load(const std::string& path) const = 0;
    // All lowercased extensions (without dot) this decoder handles. Drives the
    // registry's supportedExtensions() so the seam stays pluggable (no central
    // probe list). [added during Task 1 code review]
    virtual std::vector<std::string> extensions() const = 0;
};

// Lowercased extension without the dot ("a/b.KTX2" -> "ktx2"; "noext" -> "").
std::string TextureExtLower(const std::string& path);
```

- [ ] **Step 2: Write the registry header**

```cpp
// tools/asset_viewer/TextureDecoderRegistry.h
#pragma once
#include "TextureDecoder.h"
#include <memory>
#include <vector>

class TextureDecoderRegistry {
public:
    void add(std::unique_ptr<ITextureDecoder> d);
    const ITextureDecoder* find(const std::string& path) const;     // by extension; null if none
    DecodedTexture load(const std::string& path) const;             // find+load, or DecodedTexture{error}
    bool isSupported(const std::string& path) const;                // extension handled by some decoder
    std::vector<std::string> supportedExtensions() const;           // union, lowercased w/o dot
private:
    std::vector<std::unique_ptr<ITextureDecoder>> decoders_;
};

// Process-wide registry, lazily populated with the default decoders.
TextureDecoderRegistry& textureDecoderRegistry();
```

- [ ] **Step 3: Write the legacy decoder header**

```cpp
// tools/asset_viewer/LegacyImageDecoder.h
#pragma once
#include "TextureDecoder.h"

// Wraps the existing UiEditorImageCache (PNG/JPG/JPEG/BMP/TGA). The GL texture
// is cache-owned, so DecodedTexture::ownsGlTexture is false.
class LegacyImageDecoder : public ITextureDecoder {
public:
    bool          handles(const std::string& extLower) const override;
    DecodedTexture load(const std::string& path) const override;
    std::vector<std::string> extensions() const override;   // {"png","jpg","jpeg","bmp","tga"}
};
```

- [ ] **Step 4: Write the failing registry smoke (`--smoke-decoder`)**

Add to `tools/asset_viewer/AssetViewerApp.h` (public): `static int runSmokeDecoder();`
Add to `tools/asset_viewer/main.cpp` after the existing `--smoke` branch:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-decoder") == 0)
        return AssetViewerApp::runSmokeDecoder();
```

Add to `tools/asset_viewer/AssetViewerApp.cpp` (include `"TextureDecoderRegistry.h"` and `"TextureExtensions.h"` at top; reuse the file-local `smokeFail`):

```cpp
int AssetViewerApp::runSmokeDecoder()
{
    auto& reg = textureDecoderRegistry();
    if (!reg.isSupported("a.PNG"))      return smokeFail("png should be supported via registry");
    if (!reg.isSupported("a.tga"))      return smokeFail("tga should be supported via registry");
    if ( reg.isSupported("a.dds"))      return smokeFail("dds should NOT be supported");
    if ( reg.isSupported("noext"))      return smokeFail("extensionless should NOT be supported");
    // IsSupportedTextureFile must now agree with the registry.
    if (!IsSupportedTextureFile("a.png")) return smokeFail("IsSupportedTextureFile png");
    if ( IsSupportedTextureFile("a.dds")) return smokeFail("IsSupportedTextureFile dds");
    if (TextureExtLower("X/Y.KTX2") != "ktx2") return smokeFail("TextureExtLower");
    std::printf("[smoke] PASS decoder registry\n");
    return 0;
}
```

Run (after build): `mc2_asset_viewer --smoke-decoder`
Expected: build/link failure (registry not implemented yet).

- [ ] **Step 5: Implement the registry + ext helper + legacy decoder**

```cpp
// tools/asset_viewer/TextureDecoderRegistry.cpp
#include "TextureDecoderRegistry.h"
#include "LegacyImageDecoder.h"
#include <algorithm>
#include <cctype>

std::string TextureExtLower(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return ext;
}

void TextureDecoderRegistry::add(std::unique_ptr<ITextureDecoder> d)
{
    decoders_.push_back(std::move(d));
}

const ITextureDecoder* TextureDecoderRegistry::find(const std::string& path) const
{
    std::string ext = TextureExtLower(path);
    if (ext.empty()) return nullptr;
    for (const auto& d : decoders_)
        if (d->handles(ext)) return d.get();
    return nullptr;
}

DecodedTexture TextureDecoderRegistry::load(const std::string& path) const
{
    const ITextureDecoder* d = find(path);
    if (!d) { DecodedTexture r; r.error = "No decoder for this file type."; return r; }
    return d->load(path);
}

bool TextureDecoderRegistry::isSupported(const std::string& path) const
{
    return find(path) != nullptr;
}

std::vector<std::string> TextureDecoderRegistry::supportedExtensions() const
{
    // Union, in registration order. (Small N; linear is fine.)
    static const char* kProbe[] = {
        "png","jpg","jpeg","bmp","tga","ktx2","dds","basis"
    };
    std::vector<std::string> out;
    for (const char* e : kProbe) {
        std::string ext = e;
        for (const auto& d : decoders_)
            if (d->handles(ext)) { out.push_back(ext); break; }
    }
    return out;
}

// Default decoder set. Task 3 appends the Ktx2Decoder registration here.
static void buildDefaultRegistry(TextureDecoderRegistry& reg)
{
    reg.add(std::make_unique<LegacyImageDecoder>());
}

TextureDecoderRegistry& textureDecoderRegistry()
{
    static TextureDecoderRegistry reg = []{
        TextureDecoderRegistry r;
        buildDefaultRegistry(r);
        return r;
    }();
    return reg;
}
```

```cpp
// tools/asset_viewer/LegacyImageDecoder.cpp
#include "LegacyImageDecoder.h"
#include "UiEditorImageCache.h"
#include <cstdint>

bool LegacyImageDecoder::handles(const std::string& extLower) const
{
    return extLower == "png" || extLower == "jpg" || extLower == "jpeg"
        || extLower == "bmp" || extLower == "tga";
}

DecodedTexture LegacyImageDecoder::load(const std::string& path) const
{
    DecodedTexture d;
    const UiEditorImageTexture* tex = UiEditorImageCache_Get(path.c_str());
    if (!tex || !tex->loaded) {
        d.error = (tex && tex->unavailable)
            ? "Image format not supported or file unreadable."
            : "Failed to load image (not found or decode error).";
        return d;
    }
    d.glTexture     = (uint32_t)(intptr_t)tex->textureId;
    d.width         = tex->width;
    d.height        = tex->height;
    d.mipCount      = 1;
    d.formatLabel   = "RGBA8";
    d.isCompressed  = false;
    d.ownsGlTexture = false;   // cache-owned
    return d;
}
```

- [ ] **Step 6: Delegate the browser filter to the registry**

Replace the body of `tools/asset_viewer/TextureExtensions.cpp` with:

```cpp
#include "TextureExtensions.h"
#include "TextureDecoderRegistry.h"

bool IsSupportedTextureFile(const std::string& path)
{
    return textureDecoderRegistry().isSupported(path);
}
```

- [ ] **Step 7: Add the new sources to CMake**

In `tools/asset_viewer/CMakeLists.txt`, inside `set(ASSET_VIEWER_SOURCES ...)`, add:

```cmake
    TextureDecoderRegistry.cpp
    LegacyImageDecoder.cpp
```

- [ ] **Step 8: Build and run the smoke to verify it passes**

Run: `cmake --build <build-dir> --target mc2_asset_viewer` then `mc2_asset_viewer --smoke-decoder`
Expected: `[smoke] PASS decoder registry`
Also re-run the existing stage-1 smoke to confirm no regression: `mc2_asset_viewer --smoke tests/fixtures/asset_viewer` → `[smoke] PASS`

- [ ] **Step 9: Commit**

```bash
git add tools/asset_viewer/TextureDecoder.h tools/asset_viewer/TextureDecoderRegistry.h \
        tools/asset_viewer/TextureDecoderRegistry.cpp tools/asset_viewer/LegacyImageDecoder.h \
        tools/asset_viewer/LegacyImageDecoder.cpp tools/asset_viewer/TextureExtensions.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): pluggable texture-decoder registry + legacy image decoder"
```

---

## Task 2: KTX2 fixtures + loadKtx2Image adapter (parse + classify)

**Files:**
- Modify: `tests/fixtures/asset_viewer/make_fixture.py`
- Create: `tools/asset_viewer/Ktx2Decoder.h` (adapter declarations only this task)
- Create: `tools/asset_viewer/Ktx2Decoder.cpp` (adapter only this task; GL upload added in Task 3)
- Modify: `tools/asset_viewer/CMakeLists.txt`
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp`

- [ ] **Step 1: Generate KTX2 fixtures (hand-written, no encoder)**

Append to `tests/fixtures/asset_viewer/make_fixture.py`:

```python
# ---- KTX2 fixtures (hand-written; no encoder dependency) ----
KTX2_MAGIC = bytes([0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a])

def ktx2(vk_format, w, h, levels, super_scheme=0):
    # levels: list of bytes, mip0 first. Header layout mirrors RenderCore/KtxLoader.cpp:
    #   magic(12) + 9*u32(36) + dfd/kvd(4*u32=16) + sgd(2*u64=16) = 80, then level index.
    type_size = 1
    hdr = struct.pack("<9I", vk_format, type_size, w, h, 0, 0, 1, len(levels), super_scheme)
    desc = struct.pack("<4I", 0, 0, 0, 0) + struct.pack("<2Q", 0, 0)
    data_start = 80 + 24 * len(levels)
    entries = b""; data = b""; off = data_start
    for lv in levels:
        entries += struct.pack("<3Q", off, len(lv), len(lv))
        data += lv; off += len(lv)
    return KTX2_MAGIC + hdr + desc + entries + data

def write(name, blob):
    open(os.path.join(os.path.dirname(__file__), name), "wb").write(blob)
    print("wrote", name, len(blob), "bytes")

# RGBA8 unorm 4x2, single mip (32 bytes payload).
rgba = bytes([(i*8) % 256 for i in range(4*2*4)])
write("tex_rgba8.ktx2", ktx2(37, 4, 2, [rgba]))                     # vkFormat 37 = R8G8B8A8_UNORM

# BC7 sRGB 4x2 -> 1 block (16 bytes). Block contents are not color-validated by
# the smoke (it asserts upload success + isCompressed + clean glGetError only).
bc7_block = bytes([0x40] + [0x00]*15)                               # mode-6-ish; any 16 bytes upload fine
write("tex_bc7.ktx2", ktx2(146, 4, 2, [bc7_block]))                # vkFormat 146 = BC7_SRGB_BLOCK

# Supercompressed (scheme=1) -> classify as Basis/unsupported.
write("tex_super.ktx2", ktx2(37, 4, 2, [rgba], super_scheme=1))

# Unknown vkFormat -> classify as unsupported format.
write("tex_badfmt.ktx2", ktx2(999, 4, 2, [rgba]))
```

Run: `python tests/fixtures/asset_viewer/make_fixture.py`
Expected: prints `wrote test_rgba.png ...`, `wrote tex_rgba8.ktx2 ...`, `wrote tex_bc7.ktx2 ...`, `wrote tex_super.ktx2 ...`, `wrote tex_badfmt.ktx2 ...`

- [ ] **Step 2: Write the adapter header**

```cpp
// tools/asset_viewer/Ktx2Decoder.h
#pragma once
#include "TextureDecoder.h"
#include "KtxLoader.h"      // RenderCore::KtxImage / ktxLoadRgba8 (RenderCore on include path)
#include <string>

// Viewer-side adapter. Loads a KTX2 file into a RenderCore::KtxImage and, on
// failure, classifies WHY into a friendly message. BC7 is loaded here too --
// we deliberately do NOT expose the engine's Rgba8-named symbol to callers.
struct Ktx2DecodedImage {
    RenderCore::KtxImage img;
    bool        ok = false;
    std::string error;     // empty on success
};

Ktx2DecodedImage loadKtx2Image(const std::string& path);

// The .ktx2 decoder. GL upload implemented in Task 3.
class Ktx2Decoder : public ITextureDecoder {
public:
    bool          handles(const std::string& extLower) const override;
    DecodedTexture load(const std::string& path) const override;
    std::vector<std::string> extensions() const override;   // {"ktx2"}
};
```

- [ ] **Step 3: Write the failing parse smoke (`--smoke-ktx-parse`)**

Add to `AssetViewerApp.h`: `static int runSmokeKtxParse(const char* fixtureDir);`
Add to `main.cpp`:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-ktx-parse") == 0)
        return AssetViewerApp::runSmokeKtxParse(argc >= 3 ? argv[2] : ".");
```

Add to `AssetViewerApp.cpp` (include `"Ktx2Decoder.h"`, `<filesystem>`):

```cpp
int AssetViewerApp::runSmokeKtxParse(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* f){ return (fs::path(dir) / f).string(); };

    Ktx2DecodedImage rgba = loadKtx2Image(p("tex_rgba8.ktx2"));
    if (!rgba.ok)                       return smokeFail("rgba8 ktx2 should parse");
    if (rgba.img.width != 4 || rgba.img.height != 2) return smokeFail("rgba8 dims");
    if (rgba.img.isCompressed)          return smokeFail("rgba8 should not be compressed");

    // Validate the hand-written BC7 fixture against the REAL loader here (no GL).
    // The GL smoke's BC7 path is BPTC-gated and may SKIP, so this is the only
    // guard that a malformed BC7 fixture cannot ship green. (review fix: KTX2 #1)
    Ktx2DecodedImage bc7 = loadKtx2Image(p("tex_bc7.ktx2"));
    if (!bc7.ok)                        return smokeFail("bc7 ktx2 should parse");
    if (!bc7.img.isCompressed)          return smokeFail("bc7 should be compressed");
    if (bc7.img.width != 4 || bc7.img.height != 2) return smokeFail("bc7 dims");

    Ktx2DecodedImage sup = loadKtx2Image(p("tex_super.ktx2"));
    if (sup.ok || sup.error.find("Supercompressed") == std::string::npos)
        return smokeFail("supercompressed should be classified");

    Ktx2DecodedImage bad = loadKtx2Image(p("tex_badfmt.ktx2"));
    if (bad.ok || bad.error.find("Unsupported KTX2 format") == std::string::npos)
        return smokeFail("bad format should be classified");

    Ktx2DecodedImage missing = loadKtx2Image(p("does_not_exist.ktx2"));
    if (missing.ok || missing.error.find("not found") == std::string::npos)
        return smokeFail("missing file should be classified");

    std::printf("[smoke] PASS ktx parse+classify\n");
    return 0;
}
```

Run: `mc2_asset_viewer --smoke-ktx-parse tests/fixtures/asset_viewer`
Expected: build failure until the adapter exists.

- [ ] **Step 4: Implement the adapter (classify + load); add a temporary `load()` stub**

```cpp
// tools/asset_viewer/Ktx2Decoder.cpp
#include "Ktx2Decoder.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

static const uint8_t kKtx2Magic[12] = {
    0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a
};

Ktx2DecodedImage loadKtx2Image(const std::string& path)
{
    Ktx2DecodedImage r;

    // Peek header ourselves to classify failures (ktxLoadRgba8 returns a bare bool).
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = "KTX2 file not found: " + path; return r; }
    uint8_t magic[12];
    uint32_t fields[9];   // vkFormat, typeSize, w, h, depth, layers, faces, levels, super
    bool readOk = std::fread(magic, 1, 12, f) == 12
               && std::fread(fields, 4, 9, f) == 9;
    std::fclose(f);
    if (!readOk || std::memcmp(magic, kKtx2Magic, 12) != 0) {
        r.error = "Not a KTX2 file: " + path;
        return r;
    }
    const uint32_t vkFormat = fields[0];
    const uint32_t superScheme = fields[8];
    if (superScheme != 0) {
        r.error = "Supercompressed KTX2 (Basis) not supported yet.";
        return r;
    }
    const bool known = (vkFormat == 37 || vkFormat == 43 || vkFormat == 145 || vkFormat == 146);
    if (!known) {
        r.error = "Unsupported KTX2 format (vkFormat=" + std::to_string(vkFormat)
                + "). Viewer supports RGBA8 and BC7.";
        return r;
    }

    // Real load via the engine parser (also handles BC7 despite the name).
    if (!RenderCore::ktxLoadRgba8(path.c_str(), r.img)) {
        r.error = "Corrupt KTX2: mip level size/offset out of range.";
        return r;
    }
    r.ok = true;
    return r;
}

bool Ktx2Decoder::handles(const std::string& extLower) const
{
    return extLower == "ktx2";
}

// GL upload implemented in Task 3. This stub keeps the file compiling for the
// parse smoke; it is replaced in Task 3.
DecodedTexture Ktx2Decoder::load(const std::string& path) const
{
    DecodedTexture d;
    d.error = "KTX2 GL upload not yet implemented.";
    (void)path;
    return d;
}
```

- [ ] **Step 5: Add to CMake (sources + RenderCore include + KtxLoader.cpp)**

In `tools/asset_viewer/CMakeLists.txt`:
- add to `ASSET_VIEWER_SOURCES`:

```cmake
    Ktx2Decoder.cpp
    "${CMAKE_SOURCE_DIR}/RenderCore/KtxLoader.cpp"
```

- add to `target_include_directories(mc2_asset_viewer PRIVATE ...)`:

```cmake
    "${CMAKE_SOURCE_DIR}/RenderCore"
```

- [ ] **Step 6: Build and run the parse smoke to verify it passes**

Run: `python tests/fixtures/asset_viewer/make_fixture.py` then build, then
`mc2_asset_viewer --smoke-ktx-parse tests/fixtures/asset_viewer`
Expected: `[smoke] PASS ktx parse+classify`

- [ ] **Step 7: Commit**

```bash
git add tests/fixtures/asset_viewer/make_fixture.py tests/fixtures/asset_viewer/tex_rgba8.ktx2 \
        tests/fixtures/asset_viewer/tex_bc7.ktx2 tests/fixtures/asset_viewer/tex_super.ktx2 \
        tests/fixtures/asset_viewer/tex_badfmt.ktx2 tools/asset_viewer/Ktx2Decoder.h \
        tools/asset_viewer/Ktx2Decoder.cpp tools/asset_viewer/CMakeLists.txt \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): loadKtx2Image adapter (parse + friendly classify) + KTX2 fixtures"
```

---

## Task 3: Ktx2Decoder GL upload (RGBA8 + BC7/BPTC) + register it

**Files:**
- Modify: `tools/asset_viewer/Ktx2Decoder.cpp` (replace the `load()` stub)
- Modify: `tools/asset_viewer/TextureDecoderRegistry.cpp` (register the decoder)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` (`--smoke-ktx`)
- Modify: `tools/asset_viewer/AssetViewerApp.cpp` (`runSmokeDecoder` gains `.ktx2` assertion)

- [ ] **Step 1: Write the failing GL smoke (`--smoke-ktx`)**

Add to `AssetViewerApp.h`: `static int runSmokeKtx(const char* fixtureDir);`
Add to `main.cpp`:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-ktx") == 0)
        return AssetViewerApp::runSmokeKtx(argc >= 3 ? argv[2] : ".");
```

Add to `AssetViewerApp.cpp` (needs `<GL/glew.h>`, `<SDL.h>`, `"TextureDecoderRegistry.h"`, `<filesystem>`). Copy the SDL/GL bring-up from `runSmoke`:

```cpp
int AssetViewerApp::runSmokeKtx(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-ktx", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    auto& reg = textureDecoderRegistry();

    // RGBA8 path
    {
        DecodedTexture d = reg.load((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (!d.error.empty())           rc = smokeFail("rgba8 ktx upload error");
        else if (d.glTexture == 0)      rc = smokeFail("rgba8 ktx no texture");
        else if (d.width != 4 || d.height != 2) rc = smokeFail("rgba8 ktx dims");
        else if (d.formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("rgba8 label");
        else if (!d.ownsGlTexture)      rc = smokeFail("rgba8 should be owned");
        else if (glGetError() != GL_NO_ERROR) rc = smokeFail("rgba8 glGetError");
        if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
    }

    // BC7 path (skip cleanly if no BPTC)
    if (rc == 0) {
        if (!GLEW_ARB_texture_compression_bptc) {
            std::printf("[smoke] SKIP bc7: no BPTC\n");
        } else {
            DecodedTexture d = reg.load((fs::path(dir) / "tex_bc7.ktx2").string());
            if (!d.error.empty())       rc = smokeFail("bc7 upload error");
            else if (d.glTexture == 0)  rc = smokeFail("bc7 no texture");
            else if (!d.isCompressed)   rc = smokeFail("bc7 should be compressed");
            else if (glGetError() != GL_NO_ERROR) rc = smokeFail("bc7 glGetError");
            if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS ktx upload\n");
    return rc;
}
```

Run: `mc2_asset_viewer --smoke-ktx tests/fixtures/asset_viewer`
Expected: FAIL — `Ktx2Decoder::load` still returns the "not yet implemented" stub error.

- [ ] **Step 2: Implement the GL upload (replace the `load()` stub in `Ktx2Decoder.cpp`)**

Add `#include <GL/glew.h>` at the top of `tools/asset_viewer/Ktx2Decoder.cpp`, then replace the stub `Ktx2Decoder::load` with:

```cpp
DecodedTexture Ktx2Decoder::load(const std::string& path) const
{
    DecodedTexture d;
    Ktx2DecodedImage k = loadKtx2Image(path);
    if (!k.ok) { d.error = k.error; return d; }
    const RenderCore::KtxImage& img = k.img;

    d.width         = img.width;
    d.height        = img.height;
    d.mipCount      = img.mipCount;
    d.isCompressed  = img.isCompressed;
    d.ownsGlTexture = true;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!img.isCompressed) {
        const GLenum internal = img.isSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;     // color-space per metadata
        glTexImage2D(GL_TEXTURE_2D, 0, internal, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE,
                     img.pixels.data() + img.mipByteOffsets[0]);
        d.formatLabel = img.isSrgb ? "RGBA8 (sRGB)" : "RGBA8";
    } else {
        if (!GLEW_ARB_texture_compression_bptc) {
            glDeleteTextures(1, &tex);
            d.error = "BC7 preview requires GL_ARB_texture_compression_bptc, "
                      "unavailable on this GPU/context.";
            return d;
        }
        const GLenum internal = img.isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                                           : GL_COMPRESSED_RGBA_BPTC_UNORM;
        for (int lvl = 0; lvl < img.mipCount; ++lvl) {
            const int lw = (img.width  >> lvl) ? (img.width  >> lvl) : 1;
            const int lh = (img.height >> lvl) ? (img.height >> lvl) : 1;
            const uint64_t off  = img.mipByteOffsets[lvl];
            const uint64_t next = (lvl + 1 < img.mipCount) ? img.mipByteOffsets[lvl + 1]
                                                           : (uint64_t)img.pixels.size();
            const GLsizei size  = (GLsizei)(next - off);
            glCompressedTexImage2D(GL_TEXTURE_2D, lvl, internal, lw, lh, 0,
                                   size, img.pixels.data() + off);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, img.mipCount - 1);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "BC7%s, %d mip%s",
                      img.isSrgb ? " (sRGB)" : "", img.mipCount, img.mipCount > 1 ? "s" : "");
        d.formatLabel = buf;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    img.mipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "GL upload failed (0x%x) for %s", e, path.c_str());
        d.error = buf;
        return d;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    d.glTexture = tex;
    return d;
}
```

(Add `#include <cstdio>` is already present from Task 2.)

- [ ] **Step 3: Register the decoder**

In `tools/asset_viewer/TextureDecoderRegistry.cpp`, add `#include "Ktx2Decoder.h"` near the top, and add the registration to `buildDefaultRegistry`:

```cpp
static void buildDefaultRegistry(TextureDecoderRegistry& reg)
{
    reg.add(std::make_unique<LegacyImageDecoder>());
    reg.add(std::make_unique<Ktx2Decoder>());
}
```

- [ ] **Step 4: Extend `--smoke-decoder` to assert `.ktx2` is now supported**

In `AssetViewerApp.cpp` `runSmokeDecoder`, add before the final `printf`:

```cpp
    if (!reg.isSupported("a.ktx2"))     return smokeFail("ktx2 should be supported now");
    {
        auto exts = reg.supportedExtensions();
        bool hasKtx2 = false; for (auto& e : exts) if (e == "ktx2") hasKtx2 = true;
        if (!hasKtx2) return smokeFail("supportedExtensions missing ktx2");
    }
```

- [ ] **Step 5: Build and run all decoder/ktx smokes to verify they pass**

Run:
```
mc2_asset_viewer --smoke-decoder
mc2_asset_viewer --smoke-ktx tests/fixtures/asset_viewer
```
Expected:
- `[smoke] PASS decoder registry`
- `[smoke] PASS ktx upload` (or, if the GPU lacks BPTC, the BC7 portion prints `[smoke] SKIP bc7: no BPTC` and the run still ends `[smoke] PASS ktx upload`).

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/Ktx2Decoder.cpp tools/asset_viewer/TextureDecoderRegistry.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): KTX2 GL upload (RGBA8 + BC7/BPTC, color-space aware) + register decoder"
```

---

## Task 4: Wire registry into TexturePreview2D (ownership) + inspector metadata

**Files:**
- Modify: `tools/asset_viewer/TextureMetadata.h`, `tools/asset_viewer/TextureMetadata.cpp`
- Modify: `tools/asset_viewer/TexturePreview2D.h`, `tools/asset_viewer/TexturePreview2D.cpp`
- Modify: `tools/asset_viewer/TextureInspectorPanel.cpp`
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` (`--smoke-preview`)

- [ ] **Step 1: Add metadata fields**

In `tools/asset_viewer/TextureMetadata.h`, extend the struct + add a formatter:

```cpp
struct TextureMetadata {
    int width = 0;
    int height = 0;
    int channels = 0;          // 0 if unknown
    std::uintmax_t fileBytes = 0;
    std::string formatLabel;   // "RGBA8", "BC7 (sRGB), 9 mips", ... ("" if unknown)
    int mipCount = 1;
};

std::string FormatDimensions(const TextureMetadata& m);   // "256 x 256"
std::string FormatFileSize(const TextureMetadata& m);     // "1.5 MB" / "812 KB" / "300 B"
std::string FormatChannels(const TextureMetadata& m);     // "RGBA"/"RGB"/"Gray+A"/"Gray"/"unknown"
std::string FormatTextureFormat(const TextureMetadata& m);// formatLabel or "unknown"
```

In `tools/asset_viewer/TextureMetadata.cpp`, add:

```cpp
std::string FormatTextureFormat(const TextureMetadata& m) {
    return m.formatLabel.empty() ? "unknown" : m.formatLabel;
}
```

`#include <string>` is already pulled via the header; no further include needed.

- [ ] **Step 2: Write the failing preview ownership smoke (`--smoke-preview`)**

This proves: (a) loading a KTX2 then loading something else does not crash and leaves GL clean (owned texture freed on replace); (b) metadata carries the format label. It uses `TexturePreview2D` directly.

Add to `AssetViewerApp.h`: `static int runSmokePreview(const char* fixtureDir);`
Add to `main.cpp`:

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-preview") == 0)
        return AssetViewerApp::runSmokePreview(argc >= 3 ? argv[2] : ".");
```

Add to `AssetViewerApp.cpp` (GL bring-up copied from `runSmokeKtx`; include `"TexturePreview2D.h"`):

```cpp
int AssetViewerApp::runSmokePreview(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-preview", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError();

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;

        surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (surface.hasError())                                  rc = smokeFail("ktx2 preview load");
        else if (surface.metadata().formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("ktx2 format label");

        // Replace with a PNG (legacy, not owned) -> must free the prior KTX texture, no crash/leak-error.
        if (rc == 0) surface.setSource((fs::path(dir) / "test_rgba.png").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("png after ktx2");

        // Replace with another KTX2 -> exercises owned->owned replacement path.
        if (rc == 0) surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("ktx2 after png");

        if (rc == 0 && glGetError() != GL_NO_ERROR)             rc = smokeFail("preview glGetError");
        UiEditorImageCache_Shutdown();
    }   // ~TexturePreview2D here must free its owned KTX texture without error

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS preview ownership+metadata\n");
    return rc;
}
```

Run: `mc2_asset_viewer --smoke-preview tests/fixtures/asset_viewer`
Expected: FAIL/garbage — `TexturePreview2D` still uses the old `UiEditorImageCache`-only path (no `formatLabel`, no KTX2 support).

- [ ] **Step 3: Refactor `TexturePreview2D` to the registry + ownership-correct lifetime**

Replace `tools/asset_viewer/TexturePreview2D.h` with:

```cpp
#pragma once
#include "PreviewSurface.h"
#include "TextureMetadata.h"
#include "TextureDecoder.h"
#include <string>

class TexturePreview2D : public PreviewSurface {
public:
    ~TexturePreview2D() override;
    void setSource(const std::string& path) override;
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Texture"; }
    bool hasError() const { return hasError_; }
    const std::string& errorText() const { return errorText_; }
    const TextureMetadata& metadata() const { return meta_; }
    const std::string& sourcePath() const { return path_; }
private:
    void releaseOwned();   // delete the held GL texture iff owned

    std::string     path_;
    TextureMetadata meta_;
    DecodedTexture  current_;     // holds glTexture + ownsGlTexture
    bool            hasError_ = false;
    std::string     errorText_;
    float           zoom_ = 1.0f;
};
```

Replace `tools/asset_viewer/TexturePreview2D.cpp` with:

```cpp
#include "TexturePreview2D.h"
#include "TextureDecoderRegistry.h"
#include "imgui.h"
#include <GL/glew.h>
#include <filesystem>
#include <system_error>

void TexturePreview2D::releaseOwned()
{
    if (current_.ownsGlTexture && current_.glTexture) {
        GLuint t = current_.glTexture;
        glDeleteTextures(1, &t);
    }
    current_ = DecodedTexture{};
}

TexturePreview2D::~TexturePreview2D() { releaseOwned(); }

void TexturePreview2D::setSource(const std::string& path)
{
    releaseOwned();                 // free previous owned texture before replacing
    path_ = path;
    meta_ = TextureMetadata{};
    hasError_ = false;
    errorText_.clear();

    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) meta_.fileBytes = sz;

    current_ = textureDecoderRegistry().load(path);
    if (!current_.error.empty() || current_.glTexture == 0) {
        hasError_ = true;
        errorText_ = current_.error.empty() ? "Failed to load texture." : current_.error;
        // Do not hold a half-result that could be mistaken for a live texture.
        current_ = DecodedTexture{};
        return;
    }
    meta_.width       = current_.width;
    meta_.height      = current_.height;
    meta_.channels    = 0;
    meta_.formatLabel = current_.formatLabel;
    meta_.mipCount    = current_.mipCount;
}

void TexturePreview2D::draw(const ImVec2& availableSize)
{
    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", errorText_.c_str());
        ImGui::TextWrapped("Path: %s", path_.c_str());
        return;
    }
    if (current_.glTexture == 0) {
        ImGui::TextDisabled("No texture selected.");
        return;
    }
    ImGui::SliderFloat("Zoom", &zoom_, 0.1f, 8.0f, "%.1fx");
    ImVec2 imageSize((float)meta_.width * zoom_, (float)meta_.height * zoom_);
    ImGui::BeginChild("tex_scroll", availableSize, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image((ImTextureID)(intptr_t)current_.glTexture, imageSize);
    ImGui::EndChild();
}
```

- [ ] **Step 4: Show format + mips in the inspector**

In `tools/asset_viewer/TextureInspectorPanel.cpp`, inside the `if (!surface.hasError())` block, after the Channels line add:

```cpp
        ImGui::Text("Format:     %s", FormatTextureFormat(m).c_str());
        if (m.mipCount > 1) ImGui::Text("Mips:       %d", m.mipCount);
```

- [ ] **Step 5: Build and run the preview smoke (and re-run prior smokes)**

Run:
```
mc2_asset_viewer --smoke-preview tests/fixtures/asset_viewer
mc2_asset_viewer --smoke tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-ktx tests/fixtures/asset_viewer
```
Expected:
- `[smoke] PASS preview ownership+metadata`
- `[smoke] PASS` (stage-1 still green through the registry)
- `[smoke] PASS ktx upload`

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/TextureMetadata.h tools/asset_viewer/TextureMetadata.cpp \
        tools/asset_viewer/TexturePreview2D.h tools/asset_viewer/TexturePreview2D.cpp \
        tools/asset_viewer/TextureInspectorPanel.cpp tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): preview loads via decoder registry (ownership-correct) + format/mips in inspector"
```

---

## Task 5: README + final verification

**Files:**
- Modify: `tools/asset_viewer/README.md`

- [ ] **Step 1: Document KTX2 support + BC7/BPTC note**

Append to `tools/asset_viewer/README.md`:

```markdown
## KTX2 textures (RGBA8 + BC7)

The browser now lists and previews `.ktx2` files alongside PNG/JPG/TGA/BMP.
Supported KTX2 formats: uncompressed RGBA8 (unorm/sRGB) and stored BC7
(unorm/sRGB). The preview follows the file's KTX color-space metadata
(sRGB vs linear).

> BC7 preview requires the `GL_ARB_texture_compression_bptc` OpenGL extension.
> On GPUs/drivers without it, BC7 files show a friendly "unsupported" message
> instead of a preview. RGBA8 KTX2 always previews.

> The asset viewer follows KTX color-space metadata for preview only. This does
> not change the in-game static-prop BC7 runtime policy, which is audited
> separately.

Not yet supported (deferred): DDS, Basis/supercompressed KTX2, CPU BC7
transcoding, asset cooking.
```

- [ ] **Step 2: Run the full smoke suite**

```
mc2_asset_viewer --smoke           tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-decoder
mc2_asset_viewer --smoke-ktx-parse tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-ktx       tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-preview   tests/fixtures/asset_viewer
```
Expected: every line ends in `[smoke] PASS ...` (BC7 may print `[smoke] SKIP bc7: no BPTC` before the ktx PASS on GPUs without BPTC).

- [ ] **Step 3: Manual check**

Launch `mc2_asset_viewer`, browse a folder containing a `.ktx2` file (e.g. point the Browse picker at `tests/fixtures/asset_viewer`), select `tex_rgba8.ktx2`.
Expected: the texture previews; the inspector shows `Format: RGBA8` and dimensions. If a real BC7 `.ktx2` is available, it previews (or shows the friendly BPTC message on unsupported GPUs).

- [ ] **Step 4: Commit**

```bash
git add tools/asset_viewer/README.md
git commit -m "docs(asset-viewer): document KTX2 (RGBA8 + BC7) preview support"
```

---

## Self-review against spec

- **Add `.ktx2` to viewer (spec Goal / locked):** Tasks 2-4. ✓
- **Reuse `RenderCore/KtxLoader` if metadata sufficient (spec "Loader API"):** confirmed sufficient; compiled in via CMake (Task 2); wrapped as `loadKtx2Image`/`Ktx2DecodedImage` so BC7 doesn't flow through an `Rgba8`-named call; RenderCore symbol not renamed. ✓ (patch note 1)
- **Existing PNG/JPG/TGA/BMP via `UiEditorImageCache`:** `LegacyImageDecoder` wraps it unchanged (Task 1). ✓
- **Pluggable registry seam (spec "Decoder seam"):** `TextureDecoder.h` + `TextureDecoderRegistry` (Task 1); Ktx2 registered Task 3. ✓
- **BC7 native GPU compressed upload (locked):** `glCompressedTexImage2D` + BPTC (Task 3). ✓
- **BC7 requires BPTC; friendly error if absent (locked):** extension check → message (Task 3); smoke SKIP (Task 3). ✓
- **No CPU BC7 / no Basis / no DDS (locked):** none added; classify rejects them with messages. ✓
- **Texture lifetime / ownership (patch note 2):** `DecodedTexture.ownsGlTexture`; `TexturePreview2D` frees owned on replace + in dtor, never frees cache-owned (Task 4); `--smoke-preview` exercises owned→owned and owned→cache replacement. ✓
- **Color-space policy (patch note 3):** RGBA8 sRGB→`GL_SRGB8_ALPHA8`, linear→`GL_RGBA8`; BC7 sRGB→`GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM`, linear→`GL_COMPRESSED_RGBA_BPTC_UNORM` (Task 3); README note about runtime policy (Task 5). ✓
- **Error handling — 5 buckets (patch note 4):** unsupported format / supercompressed / no BPTC / bad mips (corrupt) / GL upload failure — all produced in Tasks 2-3 and surfaced via `errorText_` (Task 4 draw). ✓
- **Testing (patch note 5):** registry test `.ktx2` yes/`.dds` no (Tasks 1+3); RGBA8 KTX2 fixture smoke (Tasks 2-3); BC7 smoke using a hand-written BC7 fixture with clean SKIP if no BPTC (Task 3); `glGetError()==GL_NO_ERROR` checks (Tasks 3-4). DDS/Basis tests intentionally absent. ✓
- **Browser filter single source of truth (spec):** `IsSupportedTextureFile` delegates to registry (Task 1). ✓
- **Inspector shows format/mips (spec):** Task 4. ✓
- **CMake: only KtxLoader.cpp from RenderCore, no other link (locked):** Task 2 adds `RenderCore/KtxLoader.cpp` + include dir only. ✓

Type-consistency check: `DecodedTexture` (fields `glTexture`/`ownsGlTexture`/`formatLabel`/`isCompressed`/`mipCount`/`error`) used identically across Tasks 1, 3, 4. `loadKtx2Image`/`Ktx2DecodedImage` names consistent Tasks 2-3. `TextureExtLower`, `textureDecoderRegistry()`, `runSmokeDecoder`/`runSmokeKtxParse`/`runSmokeKtx`/`runSmokePreview` consistent across their tasks. No drift found.
