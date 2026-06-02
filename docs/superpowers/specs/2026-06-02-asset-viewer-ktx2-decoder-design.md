# MC2 Asset Viewer — KTX2 Texture Decoder (pluggable seam) Design

**Date:** 2026-06-02
**Slice:** `MC2-ASSET-VIEWER-KTX2-0`
**Base:** `claude/asset-viewer-stage2` off `claude/nifty-mendeleev` (asset-viewer stage 1 landed `29aebfe5`).
**Status:** SPEC — brainstormed + approved 2026-06-02. Next: writing-plans.

## Goal

Let `mc2_asset_viewer` browse and preview `.ktx2` textures (uncompressed RGBA8 and
stored BC7), without disturbing the existing PNG/JPG/TGA/BMP path. KTX2 enters
through a small **pluggable texture-decoder registry** so DDS / Basis can be added
later without reworking call sites. This slice ships KTX2 only.

## Locked decisions

- Add `.ktx2` support to `mc2_asset_viewer`.
- Reuse `RenderCore/KtxLoader` (it already exposes the needed metadata for both
  RGBA8 and BC7 — see "Loader API"). Compile its GL-free `.cpp` into the viewer.
- Existing PNG/JPG/TGA/BMP path remains through `UiEditorImageCache`.
- KTX2 becomes a new decoder behind a pluggable texture-decoder registry.
- BC7 preview uses **native GPU compressed upload** (no CPU transcode).
- BC7 requires `GL_ARB_texture_compression_bptc`; if unavailable, show a friendly
  unsupported error.
- **Out of scope this slice:** CPU BC7 transcoder; Basis / supercompression
  transcoder; DDS; RenderCore link beyond compiling the GL-free KTX loader source;
  any asset cooking.

## Loader API (patch note 1)

`RenderCore/KtxLoader.h` already returns a general `KtxImage` carrying everything
BC7 dispatch needs:

| Need | `KtxImage` field |
|---|---|
| format enum | `uint32_t vkFormat` (37/43 RGBA8, 145/146 BC7) |
| width / height | `int width, height` (mip-0) |
| mip count | `int mipCount` |
| byte offsets | `std::vector<uint64_t> mipByteOffsets` (level 0 == 0) |
| byte sizes | derived: `size[i] = (i+1<mipCount ? mipByteOffsets[i+1] : pixels.size()) - mipByteOffsets[i]` |
| sRGB flag | `bool isSrgb` |
| compressed flag | `bool isCompressed` (+ `uint32_t blockSizeBytes`, 16 for BC7) |
| pixel data | `std::vector<uint8_t> pixels` (full mip chain concatenated; raw RGBA or raw BC7 blocks) |

The engine entry point is misleadingly named `bool ktxLoadRgba8(const char*, KtxImage&)`,
but its own header documents that it also loads stored BC7 (inspect `isCompressed` /
`vkFormat` to dispatch upload). **We do NOT rename the RenderCore symbol** (that is
NS3 / engine-standalone territory, out of scope here). Instead the viewer wraps it
behind a correctly-named seam so BC7 never flows through an `Rgba8`-named call:

```cpp
// tools/asset_viewer/Ktx2Decoder.h
struct Ktx2DecodedImage {
    RenderCore::KtxImage img;     // metadata + raw pixel/block stream
    bool ok = false;
    std::string error;            // empty on success
};
// Thin viewer-side adapter over RenderCore::ktxLoadRgba8 (which also handles BC7).
Ktx2DecodedImage loadKtx2Image(const std::string& path);
```

If `RenderCore/KtxLoader` is later extended/renamed (e.g. `ktxLoadImage`), only this
adapter changes; the registry and preview do not.

## Decoder seam

```cpp
// tools/asset_viewer/TextureDecoder.h
struct DecodedTexture {
    uint32_t    glTexture   = 0;        // 0 == failure
    int         width       = 0;
    int         height      = 0;
    int         mipCount    = 1;
    std::string formatLabel;            // e.g. "RGBA8", "RGBA8 (sRGB)", "BC7 (sRGB), 9 mips"
    bool        isCompressed = false;
    bool        ownsGlTexture = false;  // patch note 2 — see "Texture lifetime"
    std::string error;                  // empty on success; friendly message on failure
};

struct ITextureDecoder {
    virtual ~ITextureDecoder() = default;
    virtual bool          handles(const std::string& extLower) const = 0;  // ".ktx2" etc.
    virtual DecodedTexture load(const std::string& path) const = 0;
};
```

Two decoders register at startup, in order:

| Decoder | File | Handles | Upload | `ownsGlTexture` |
|---|---|---|---|---|
| `LegacyImageDecoder` | `LegacyImageDecoder.{h,cpp}` | `.png .jpg .jpeg .bmp .tga` | delegates to `UiEditorImageCache_Get` (unchanged) | **false** (cache-owned) |
| `Ktx2Decoder` | `Ktx2Decoder.{h,cpp}` | `.ktx2` | `loadKtx2Image` → GL upload (RGBA8 or BC7/BPTC) | **true** (preview-owned) |

`TextureDecoderRegistry` owns the ordered decoder list, dispatches `load(path)` by
lowercased extension, and exposes `supportedExtensions()` (the union of all
decoders' handled extensions) so the browser filter has a single source of truth.

## Texture lifetime / ownership (patch note 2)

`DecodedTexture` carries `bool ownsGlTexture`:

- `LegacyImageDecoder` returns `ownsGlTexture = false` — the GL texture lives in
  `UiEditorImageCache` and must **not** be deleted by the preview surface.
- `Ktx2Decoder` returns `ownsGlTexture = true` — the GL texture was created by the
  decoder (`glGenTextures`) and the preview surface owns its lifetime.

`TexturePreview2D` holds the current `DecodedTexture`. Required behavior:

1. On `setSource`, **before** assigning the new result: if the currently-held
   texture has `ownsGlTexture == true`, `glDeleteTextures(1, &current.glTexture)`.
   (Loading a new KTX2 must not leak the previous KTX2 texture.)
2. Never delete a texture with `ownsGlTexture == false` (legacy cache textures).
3. On `TexturePreview2D` destruction/shutdown: delete the held texture iff
   `ownsGlTexture == true`.

## Color-space policy (patch note 3)

The viewer follows KTX color-space metadata for preview:

- RGBA8 KTX2, `isSrgb == true`  → internal format `GL_SRGB8_ALPHA8`.
- RGBA8 KTX2, `isSrgb == false` → internal format `GL_RGBA8`.
- BC7 KTX2, `isSrgb == true`    → `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM`.
- BC7 KTX2, `isSrgb == false`   → `GL_COMPRESSED_RGBA_BPTC_UNORM`.

> The asset viewer follows KTX color-space metadata for preview. This does not
> change the in-game static-prop BC7 runtime policy, which is audited separately.

## Upload detail

- **RGBA8:** `glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels+offset0)`.
  Upload mip 0 for preview (full chain optional; mip 0 is sufficient and simplest).
- **BC7:** require `GLEW_ARB_texture_compression_bptc`. For each level `i`:
  `glCompressedTexImage2D(GL_TEXTURE_2D, i, internal, max(1,w>>i), max(1,h>>i), 0, size[i], pixels.data()+mipByteOffsets[i])`
  using the derived per-level `size[i]`. Set `GL_TEXTURE_MAX_LEVEL = mipCount-1` and
  `GL_TEXTURE_MIN_FILTER` accordingly (mip-aware if `mipCount>1`, else `GL_LINEAR`).
- ImGui shows the result via `ImGui::Image((ImTextureID)(intptr_t)glTexture, size)`
  exactly as today; BPTC is sampled natively by the GL sampler.

## Browser filter + inspector

- `TextureExtensions.cpp` — `IsSupportedTextureFile(path)` consults
  `TextureDecoderRegistry::supportedExtensions()` instead of a hard-coded list.
  Result: `.ktx2` now shows in the browser; `.dds` does not (no decoder registered).
- `TextureMetadata` / `TextureInspectorPanel` — add `formatLabel` and `mipCount`
  fields, populated from `DecodedTexture`. Legacy path fills
  `formatLabel = "RGBA8"` (or channel-derived), `mipCount = 1`.

## Error handling (patch note 4)

All surfaced as the `DecodedTexture.error` string and rendered in the preview
region (matching stage-1's error display). Friendly messages:

| Condition | Message |
|---|---|
| Unsupported KTX2 `vkFormat` | `"Unsupported KTX2 format (vkFormat=<n>). Viewer supports RGBA8 and BC7."` |
| Supercompressed / Basis KTX2 | `"Supercompressed KTX2 (Basis) not supported yet."` |
| BC7 but no BPTC on GPU | `"BC7 preview requires GL_ARB_texture_compression_bptc, unavailable on this GPU/context."` |
| Bad mip offsets/sizes | `"Corrupt KTX2: mip level <i> size/offset out of range."` |
| GL upload failure (`glGetError`) | `"GL upload failed (0x<err>) for <path>."` |

`loadKtx2Image` maps `ktxLoadRgba8 == false` to the unsupported/supercompressed/
corrupt buckets where distinguishable (e.g. detect Basis via header inspection if
cheap; otherwise the generic unsupported message). `Ktx2Decoder` validates derived
per-level sizes against `pixels.size()` before any upload and checks `glGetError`
immediately after upload.

## Architecture (files)

New under `tools/asset_viewer/`:
- `TextureDecoder.h` — `DecodedTexture` + `ITextureDecoder`.
- `TextureDecoderRegistry.{h,cpp}` — ordered list, dispatch, `supportedExtensions()`.
- `LegacyImageDecoder.{h,cpp}` — wraps `UiEditorImageCache_Get` (ownsGlTexture=false).
- `Ktx2Decoder.{h,cpp}` — `loadKtx2Image` adapter + GL upload (ownsGlTexture=true).

Modified:
- `TextureExtensions.cpp` — filter driven by the registry.
- `TexturePreview2D.{h,cpp}` — load via registry; ownership-correct delete on
  replace/shutdown; render `error`.
- `TextureMetadata.{h,cpp}` + `TextureInspectorPanel.cpp` — add `formatLabel`/`mipCount`.
- `CMakeLists.txt` — add new sources + `RenderCore/KtxLoader.cpp`; add `RenderCore`
  to the include path. (No other RenderCore objects linked.)

## Data flow

```
browser file (.ktx2 | .png | ...)
  → TextureDecoderRegistry.load(path)            (dispatch by lowercased ext)
      → Ktx2Decoder    → loadKtx2Image → RenderCore::ktxLoadRgba8 → GL upload (RGBA8 | BC7)
      → LegacyImageDecoder → UiEditorImageCache_Get
  → DecodedTexture{ glTexture, dims, mipCount, formatLabel, isCompressed, ownsGlTexture, error }
  → TexturePreview2D: delete previous if owned; ImGui::Image; inspector reads metadata
```

## Testing (patch note 5)

Extend the existing fixture-based smoke harness (`mc2_asset_viewer --smoke*`,
`[smoke] PASS/FAIL`, exit 0/1). All GL checks assert `glGetError() == GL_NO_ERROR`.

- **Registry test** (`--smoke-decoder`, no GL needed):
  `supportedExtensions()` contains `.ktx2`; `IsSupportedTextureFile("x.ktx2")==true`;
  `IsSupportedTextureFile("x.dds")==false`.
- **RGBA8 KTX2 smoke** (`--smoke-ktx <dir>`, GL context): load a small RGBA8
  `.ktx2` fixture → assert `glTexture!=0`, dims match the fixture, `formatLabel`
  starts with `"RGBA8"`, `glGetError()` clean. Fixture is generated by
  `tests/fixtures/asset_viewer/make_fixture.py` (hand-written KTX2 header + RGBA8
  payload — no encoder dependency).
- **BC7 KTX2 smoke** (same `--smoke-ktx`): use a known shipped BC7 `.ktx2` sidecar
  if one exists in the data tree (e.g. the colormap atlas) — assert `isCompressed`,
  `glTexture!=0`, `glGetError()` clean. If `GLEW_ARB_texture_compression_bptc` is
  unavailable, **skip with a clear message** (`[smoke] SKIP bc7: no BPTC`) and do
  not fail. If no BC7 fixture is locatable, skip with
  `[smoke] SKIP bc7: no fixture` (RGBA8 path still gates the slice).
- Do NOT add DDS or Basis tests (out of scope).

## Risks

1. **BPTC availability** — mitigated by the extension check + friendly error +
   smoke skip; present on the dev 7900 XTX.
2. **Hand-written RGBA8 KTX2 fixture correctness** — keep it minimal (1 mip, small)
   and validate against `RenderCore::ktxLoadRgba8` in the smoke itself.
3. **BC7 fixture sourcing** — reuse a shipped atlas asset; skip cleanly if absent.

## Deferred (explicitly not this slice)

- DDS loader; Basis/supercompression transcode; CPU BC7 transcode.
- Asset cooking; mip-level scrubber UI; cubemap/array KTX2.
- Renaming the RenderCore `ktxLoadRgba8` symbol (NS3 / engine-standalone arc).

## Implementation note

If implementation follows immediately, keep it to the **decoder seam + KTX2 only**
(the files listed in "Architecture"). No DDS, no Basis, no cooking.
