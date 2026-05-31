# DEBT: terrain colormap modernization (greybeard-flagged)

`BURNIN-DISK-JPEG` (branch `claude/burnin-disk-1`, commit `2089eabc`) is a
**justified temp patch**, NOT the fix. It shrinks `.burnin.tga` on disk
(3.6 GB → ~200 MB via q90 JPEG, decoded by stb_image on load) but leaves the
underlying design intact.

## The disease (none of this moved)
The terrain colormap is materialized THREE ways every mission load
(`mclib/terrtxm2.cpp` TerrainColorMap::init / resetBaseTexture):
1. **Legacy 400×256² GL tiles** — `textureFromMemoryRaw` loop (~terrtxm2.cpp:1886-1910),
   sampled via `quad.cpp` `getTextureHandle`. **Dead weight when the indirect
   terrain path is on** (default since 2026-05-17) — that path binds the atlas,
   only borrowing the tile UV-grid math.
2. **Modern single atlas** — `cpuColorMap` (terrtxm2.h:93) →
   `gos_terrain_indirect.cpp:840 BuildColormapAtlas` → `g_atlasGLTex`.
3. **Full-res CPU copy** — `cpuColorMap`, also read per-vertex on the CPU by
   `mapdata.cpp:2437 cpu_sampleColormap` for terrain DISPLACEMENT.

JPEG decodes straight back into (2)+(3) and still builds (1). RAM/VRAM footprint
byte-identical; substitutive test FAILS (the `.tga` remains as fallback).

## Root smells
- **CPU-resident colormap in a GPU renderer** — forced to exist solely by CPU
  displacement sampling (`cpu_sampleColormap`) = the real vestige.
- **Dual representation** — legacy tiles + modern atlas built every load.
- **Caching a derived bake as 108 MB uncompressed RGBA** (`burnInShadows` →
  `saveTGAFile`) — a CPU-era choice.

## True modernization (end state)
Colormap exists ONLY as one compressed GPU texture (BC7 in KTX2): burn-in baked
on GPU or cached as KTX2/BC7; displacement samples the atlas in-shader;
`cpuColorMap` retired; 400-tile path deleted. Then BC7 IS correct (it lives on
the GPU). Disk AND VRAM both drop; the whole bug class becomes impossible.

## Prioritized sequence (greybeard; risks flagged)
1. **[DONE — 0b2b3a95] Retire the 400-tile GL path** (lowest risk; pure dead-weight).
   Gate `MC2_SETUPTEXTURES_LEGACY_FORCE` kill-switch. numTexturesAcross/cpuColorMap
   still set unconditionally; resolveTextureHandle returns 0xFFFFFFFF sentinel.
   gpu_driven_terrain_solid.comp: split nodeId==0 (edge/invalid) from nodeId>=LUT
   (sentinel thSlot=1u passthrough). Tier1 5/5 PASS.
2. **[DONE — f9c12b53] Displacement → GPU** (retire cpuColorMap+cpuDispAlpha).
   `BuildColormapAtlas` frees cpuColorMap+cpuDispAlpha after GPU atlas upload.
   mapdata.cpp:2435 null-guard already skips the CPU displacement block. The
   indirect path (default-ON) has no geometry displacement (thin vert, no TES),
   so CPU grounding now matches the visual surface (both undisplaced). Gate
   `MC2_COLORMAP_CPU_RETIRE` default ON; kill-switch =0; probe
   `MC2_COLORMAP_DISPLACE_PROBE`. Static helpers cpu_sampleColormap/cpu_getDirtWeight/
   cpu_rgb2hsv/cpu_smoothstep/cpu_sampleAlpha remain as dead code — cleanup deferred.
   Tier1 5/5 PASS. Merged to nifty (90c1c4c8).
3. **[DONE — 3a21fba0] Atlas → BC7 KTX2 (combined with step 4)**. Pre-bake
   `.burnin.tga` -> `.burnin.ktx2` via `scripts/bake_colormap_ktx2.py` (Pillow
   BGRA->RGBA + ktx create uastc + ktx transcode bc7, `--levels 1`). Runtime:
   BuildColormapAtlas probes ktx2ColormapPath; if present + BPTC cap: uploads
   via glCompressedTexImage2D(GL_COMPRESSED_RGBA_BPTC_UNORM). VRAM 108 MB → ~27 MB.
   BGRA handled by Pillow (automatic BGRA→RGBA on TGA load). Fallback to RGBA8
   when sidecar absent or cap missing. Gate `MC2_COLORMAP_KTX2` default ON.
4. **[PARTIALLY DONE — same commit] KTX2 as authority**. Sidecar exists and works.
   NOT YET: `.burnin.tga` + `.burnin.jpg` not deleted from game data (soak period).
   `saveTGAFile` preserved in editor path. JPEG fallback path intact. Deletion of
   bake files is a future soak slice after validation period.

## Remaining debt (open)
- Delete dead CPU helper functions in mapdata.cpp: `cpu_sampleColormap`,
  `cpu_getDirtWeight`, `cpu_rgb2hsv`, `cpu_smoothstep`, `cpu_sampleAlpha`.
  All unreachable when `MC2_COLORMAP_CPU_RETIRE=1` (default). Low risk cleanup.
- Delete `.burnin.tga` + `.burnin.jpg` from shipped game data package after
  `.burnin.ktx2` soak period validates. Also delete `MC2_BURNIN_NO_JPG` gate
  and JPEG load path in terrtxm2.cpp. Ships 3.6 GB → 27 MB total.

## Interim discipline
`MC2_BURNIN_NO_JPG` gate and JPEG path remain until `.burnin.ktx2` is the
validated primary source. Do NOT add new consumers of `.burnin.jpg` or `.burnin.tga`.
When soak period passes, delete both together.

## Current state (2026-05-31)
All 4 steps shipped and merged to nifty (deploy mc2-win64-v0.3/v0.4):
- 400-tile GL upload retired (COLORMAP-TILES-RETIRE-1)
- cpuColorMap freed after atlas upload (COLORMAP-CPU-RETIRE-1)
- BC7 KTX2 atlas upload active when sidecar present (COLORMAP-BC7-KTX2-1)
Remaining: cleanup dead helpers + delete bake files after soak.
