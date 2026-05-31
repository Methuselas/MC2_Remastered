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
1. **Retire the 400-tile GL path** (lowest risk; pure dead-weight). Risk: the
   legacy non-indirect raster path (`quad.cpp:640/749`) still binds tile handles
   — gate removal behind indirect-always-on or delete the legacy raster terrain.
2. **Displacement → GPU**, then delete `cpu_sampleColormap` + `cpuColorMap`.
   RISKIEST: HSV classification at `mapdata.cpp:2437` gates elevation; needs a
   CPU-vs-GPU displaced-height parity probe before flipping (grounding is a known
   fragile area — see terrain-grounding-audit).
3. **Atlas → BC7** in `BuildColormapAtlas` (`GL_COMPRESSED_RGBA_BPTC`). Risk:
   BGRA channel order (gos_terrain_indirect.cpp:854) must survive BC7 encode.
4. **Bake as KTX2/BC7 or GPU-regenerate**; delete `.burnin.tga` + `.burnin.jpg`
   + `saveTGAFile`. RISKIEST: GPU burn-in must reproduce legacy `burnInShadows`
   or every mission's ground shading shifts. This step actually retires the 3.6 GB.

## Interim discipline
Keep the JPEG interim (gated `MC2_BURNIN_NO_JPG`, clean `.tga` fallback,
render-neutral modulo q90). When the BC7/KTX2 bake lands, delete the JPEG AND
TGA paths together — do NOT let q90 JPEG become the permanent colormap source of
truth.
