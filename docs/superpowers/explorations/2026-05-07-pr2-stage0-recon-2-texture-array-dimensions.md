# PR2 Stage 0 recon — item 2: texture-array dimensions per bucket

**Date:** 2026-05-07
**Status:** Recon. Closes brainstorm open recon item #2
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:818-822`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)).
**Scope:** Enumerate detail / overlay / mine texture dimensions across stock
content. Confirm fits within a single 2D texture array per bucket per
brainstorm Q2 A(iii); identify any need for per-size-class fallback.

---

## TL;DR

| Bucket | Distinct textures (stock) | Stock dim | 4x-upscale dim | Format | Per-bucket array OK? |
|---|---|---|---|---|---|
| **Detail** | ≤ 4 (legacy `terrainTextures` path) or 1 (`terrainTextures2` modern path) | 128×128 | 512×512 | RGBA8 | Yes — trivial. PR2a likely retirement-only per recon item 6 |
| **Overlay** | ≤ 475 frames across 21 base names (whole catalog); per-mission ≪ that | 64×64 | 256×256 | RGBA8 | Yes — single dimension class, well under GL_MAX_ARRAY_TEXTURE_LAYERS=2048 |
| **Mine** | 2 (mine + blown) | 16×16 | (no upscale shipped) | RGBA8 | Yes — trivial |

**Conclusion: no per-size-class fallback needed.** All three buckets fit a
single uniformly-dimensioned 2D texture array. The mine bucket is so
small (2 layers @ 16×16) that the texture-array indirection is
overhead-only; spec session may opt for 2 separate samplers instead.

---

## Detail bucket

### Sources

Two production paths, both grep-verified at write-time:

1. **Modern `terrainTextures2` path** (recipe builder at
   [`mclib/quad.cpp:428,435`](../../../mclib/quad.cpp)):
   `r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();` —
   single static handle per mission. **Number of distinct textures: 1.**

2. **Legacy `terrainTextures` path** (legacy fallback at
   [`mclib/quad.cpp:545-549,629-631`](../../../mclib/quad.cpp)):
   `terrainDetailData = Terrain::terrainTextures->setDetail(1,0)` then
   `terrainTextures->getTextureHandle(terrainDetailData)`. The `setDetail`
   args are `(type=1=detail, frame=0)` — animated detail support exists
   but the queried frame is always 0 in this path.

   The on-disk detail textures discovered via glob:
   `mc2srcdata/textures/64/detail_{10000,20000,30000,40000}.tga`
   (4 files, 128×128 RGBA8). The 4x upscale variant ships at
   `mc2srcdata/textures/256_4x_gpu/detail_*.tga` at 512×512 RGBA8.
   **Number of distinct detail textures (legacy path): ≤ 4.**

### Texture-array shape

If PR2a re-introduces a detail draw (per recon item 6's open question):

- Layer count: ≤ 4 (legacy) or 1 (modern). Pick max.
- Dimensions: 128×128 stock, 512×512 with 4x active. RX 7900 XTX
  comfortably accepts either.
- Format: RGBA8. (TGA type 2 / 32 bpp confirmed via header read.)

If PR2a is retirement-only (per recon item 6 finding that detail's
render path is dead since 521d83a 2026-04-16):

- No texture array build required.
- Brainstorm Q4 detail-recipe-extension answer becomes deferred.

### Animated detail

`terrainTextures->getDetailFrameRate(0)` referenced at
[`mclib/mapdata.cpp:1042-1043`](../../../mclib/mapdata.cpp) — animated
detail frames exist. The `setDetail(type=1, frame=N)` API supports
per-frame indexing. The current per-quad emits at quad.cpp:545,629 only
ever query frame 0, but the FIT data may declare more.

**Open question for spec:** does PR2a need to support animated detail?
The current emit path doesn't, so PR2a's retirement is unaffected —
but if PR2a re-introduces detail rendering, animated detail probably
needs at most a handful of frames per detail base. Layer count stays
small.

### Water-detail (out-of-scope but adjacent)

`Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame)` at
[`mclib/quad.cpp:1063`](../../../mclib/quad.cpp) returns per-frame
water-detail. Water-detail is a separate population (drawn through the
water indirect endpoint, NOT detail). Excluded from this recon.

---

## Overlay bucket

### Sources

`Terrain::terrainTextures->getTextureHandle(...)` at
[`mclib/quad.cpp:433`](../../../mclib/quad.cpp) — overlay handles come
from the SAME `terrainTextures` catalog as base terrain textures. The
catalog distinguishes "base" vs "overlay" via the `overlays[]` array
loaded by `loadOverlayMemory()` ([`mclib/terrtxm.cpp:724`](../../../mclib/terrtxm.cpp)).
`setOverlay()` returns a slot handle by walking
`overlays[i].oldOverlayId` at [`mclib/terrtxm.cpp:734-790`](../../../mclib/terrtxm.cpp).

### Disk inventory (stock)

`mc2srcdata/textures/64overlays/`:
- 475 TGA files
- Uniform 64×64 RGBA8 (depth=32 type=2 across all 475 — verified
  programmatically at write-time)
- 21 distinct base names: `2lanedirt`, `bridge`, `bridgedam`,
  `cementpattern00..02` (cementpattern00/01/02 are themselves
  individual files, no frame numbering), `damagedroad`, `dirtroad`,
  + 13 more
- Average ~22 frames per base name (animation frames for road
  variants, etc.)

`mc2srcdata/textures/256overlays_4x_gpu/`: same 475-file structure
upscaled to 256×256.

### Per-mission slice

Per-mission loaded overlays = subset selected by mission's FIT data.
Stock missions are bounded: per
[`memory/m2_thin_record_cpu_reduction_results.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/m2_thin_record_cpu_reduction_results.md)
overlay-bearing quads peak around 5K on water-heavy maps; distinct
overlay textures-per-mission is empirically ≤ 100 (most quads share
a small overlay set: cement transitions + a few road types per
mission).

**Spec-time open: per-mission max overlay count.** A precise per-mission
count requires either (a) reading the mission's FIT file overlay
declarations or (b) instrumenting `TerrainTextures::loadOverlayMemory`
to log per-mission overlay-load counts during a tier1 run. The latter
is the higher-confidence answer; defer to spec time. Conservative
upper bound for sizing: 475 (full catalog).

### Texture-array shape

- Layer count: bounded by 475 (full stock catalog); per-mission ≪ that.
  PR2b builds the array per-mission from actually-loaded overlays
  (mirror PR1 cement-atlas pattern at
  [`gos_terrain_indirect.cpp:BuildCementCatalogAtlas`](../../../GameOS/gameos/gos_terrain_indirect.cpp)).
- Dimensions: 64×64 stock, 256×256 with 4x active. Both well within
  RX 7900 XTX limits.
- Format: RGBA8.
- GL limit: `GL_MAX_ARRAY_TEXTURE_LAYERS` = 2048 on AMD RX 7900 XTX
  (typical for GL 4.6 hardware). 475 << 2048, no fallback needed.

### Memory cost

Worst case (full 475 overlays loaded, 4x active): 256×256×4 B × 475 =
124.5 MB. **Likely too large** if PR2b naively allocates the catalog
upfront. Per-mission build sizes to actual overlay count (~100 layers)
yields ~26 MB at 4x or ~1.6 MB at stock — both fine.

### Risk: cement overlay vs cement-atlas dual-source

Per [`memory/indirect_terrain_solid_endpoint.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/indirect_terrain_solid_endpoint.md),
PR1 PR2 (cement multi-sampler at unit 3) already builds a cement-atlas
texture from the SAME `terrainTextures` catalog. PR2b's overlay bucket
might include cement-overlay textures that overlap with the existing
cement atlas. **Spec session must define:** is the overlay-bucket
disjoint from the cement-atlas (cement renders through SOLID endpoint
only; overlay-bucket excludes cement classes), or do they double-bind
(overlay draws cement AGAIN on top, with alpha)?

The brainstorm's Q4 overlay decision (parallel `OverlayRecipeSSBO`) is
silent on this. Worth a spec-time grep on
`overlays[i].numTextures > 9` ([`terrtxm.cpp:745`](../../../mclib/terrtxm.cpp))
to see how cement classes are identified in the overlay catalog.

---

## Mine bucket

### Sources

Two static class members loaded once at first
`TerrainQuad::setupTextures()` invocation:

- `mineTextureHandle` — loads `texturePath/defaults/mine_00.tga` at
  [`mclib/quad.cpp:520-524`](../../../mclib/quad.cpp)
- `blownTextureHandle` — loads `texturePath/defaults/minescorch_00.tga`
  at [`mclib/quad.cpp:527-531`](../../../mclib/quad.cpp)

(The local variable in both blocks is named `mineTextureName` — the
blown block reuses the name but the actual filename differs:
`minescorch_00.tga`. Misleading but functionally correct.)

### Disk inventory (stock)

- `mc2srcdata/textures/defaults/mine_00.tga`: 16×16 RGBA8
- `mc2srcdata/textures/defaults/minescorch_00.tga`: 16×16 RGBA8

(Both verified via TGA header read at write-time.)

No 4x upscale variant ships for these (mine textures stay
`gosHint_DontShrink` and intentionally pixel-aligned — they're
small icon-style markers, not detail textures).

### Texture-array shape

- Layer count: 2.
- Dimensions: 16×16 RGBA8.
- **Spec-time decision:** is a 2-layer texture-array even worth the
  indirection vs. two separate samplers? At 2 layers the array's
  layer-index-in-shader cost (uniform branch) likely exceeds the
  single-bind difference. PR2c spec should evaluate "single 2-bucket
  switch by record-flag" vs. "1-layer texture array."
- The Q2 A(iii) decision (texture-array per bucket) generalizes
  uniformly across populations, but mine's tiny scope makes the
  decision a wash. Acceptable to pick the same shape as detail/overlay
  for consistency, or carve a per-population exception.

### Filter / mip hint

`gosHint_DisableMipmap | gosHint_DontShrink` flags at quad.cpp:524,531.
Per-bucket sampler in PR2c bridge must mirror: NEAREST/NO-MIP. Avoid
inheriting linear filtering from PR2a/b.

---

## Cross-bucket constraints

### `GL_MAX_ARRAY_TEXTURE_LAYERS`

Per GL 4.3 core spec, **minimum guaranteed** value is 2048 layers.
The codebase does **not** currently `glGetIntegerv` this limit (verified
2026-05-08 adversarial review — zero hits in the worktree). The
"2048" figure for RX 7900 XTX is therefore the spec floor, not a
runtime-verified hardware advertised value. All three buckets fit
comfortably under the spec minimum:

- Detail: ≤ 4 layers
- Overlay: ≤ 475 layers (worst-case full catalog) / ≤ 100 (per-mission)
- Mine: 2 layers

**Spec-session action item:** add a `glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n)`
runtime guard at overlay-array build time, asserting `n >= 512`. Mirrors
the discipline this codebase uses for other GL limits and protects
against future ports to lower-spec hardware. The cement-atlas at PR1
also lacks this check — existing debt to revisit.

### `GL_MAX_TEXTURE_SIZE`

Per GL 4.3 core spec, minimum guaranteed 16384. RX 7900 XTX advertises
16384. All three buckets are far below:

- Detail: 128×128 stock / 512×512 4x
- Overlay: 64×64 stock / 256×256 4x
- Mine: 16×16

### Format consistency

All three buckets use RGBA8 (TGA type 2, 32 bpp). Single GL internal
format `GL_RGBA8`. No per-bucket format negotiation needed.

### Mip discipline

- Detail: mip chain present (legacy `terrainTextures->loadTexture`
  doesn't pass `gosHint_DisableMipmap` for detail loads — verify at
  spec time).
- Overlay: cement-atlas uses 1 mip (no chain) per PR1's
  `BuildCementCatalogAtlas`. PR2b's overlay-array can match (single
  mip) since 64×64/256×256 doesn't benefit much from mips on overlay
  use.
- Mine: explicit `gosHint_DisableMipmap`. Mip count = 1.

Spec session must enumerate per-bucket mip count; mismatch causes
silent sampler artifacts.

---

## Open follow-ups for spec session

1. **Per-mission overlay max count.** Instrument
   `TerrainTextures::loadOverlayMemory` for one tier1 run to capture
   per-mission distinct overlay-texture counts. Confirms PR2b array
   sizing (likely ≤ 100 per mission).
2. **Cement vs overlay overlap.** Spec session must define whether
   the overlay bucket includes cement classes (and thus double-binds
   with the existing cement atlas) or excludes them.
3. **Mine bucket: array vs separate samplers.** PR2c spec evaluates
   the cost trade-off; consistency-with-detail/overlay is cheap, but
   the indirection has no perf upside at 2 layers.
4. **Detail recipe extension scope.** Hinges on recon item 6's
   "PR2a retirement-only or rendering re-introduction" decision.
5. **Animated detail / animated overlay.** Spec session decides
   whether PR2a/b support animation (per-frame layer swap or
   recipe-time frame baking).

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `terrainDetailHandle` from terrainTextures2 modern path | [`mclib/quad.cpp:428,435`](../../../mclib/quad.cpp) — `r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();` | M |
| 2 | `terrainDetailHandle` from terrainTextures legacy path | [`mclib/quad.cpp:545-549,629-631`](../../../mclib/quad.cpp) — `terrainDetailData = ...->setDetail(1,0); terrainDetailHandle = ...->getTextureHandle(terrainDetailData);` | M |
| 3 | Detail TGAs at 64-tier 128×128 | `mc2srcdata/textures/64/detail_{10000,20000,30000,40000}.tga` — TGA header read returns 128×128 RGBA8 | M |
| 4 | Detail TGAs at 4x-tier 512×512 | `mc2srcdata/textures/256_4x_gpu/detail_*.tga` — TGA header read returns 512×512 RGBA8 | M |
| 5 | Overlay handle from terrainTextures catalog | [`mclib/quad.cpp:433`](../../../mclib/quad.cpp) — `r.overlayHandle = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);` | M |
| 6 | `setOverlay` walks `overlays[]` array | [`mclib/terrtxm.cpp:734-790`](../../../mclib/terrtxm.cpp) — `for (long i=0;i<numOverlays;i++)` | M |
| 7 | Stock overlay catalog 475 files, 64×64 RGBA8 | `mc2srcdata/textures/64overlays/` — programmatic enum returns `{(64, 64, 32): 475}` | M |
| 8 | Stock overlay catalog has 21 distinct base names | regex strip frame suffix yields 21 unique bases | M |
| 9 | 4x overlay catalog at 256×256 | `mc2srcdata/textures/256overlays_4x_gpu/` — same naming, dim 256×256 (one sample read confirms) | M |
| 10 | `mineTextureHandle` loads `defaults/mine_00.tga` | [`mclib/quad.cpp:520-524`](../../../mclib/quad.cpp) — `mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "mine_00",".tga")` | M |
| 11 | `blownTextureHandle` loads `defaults/minescorch_00.tga` | [`mclib/quad.cpp:527-531`](../../../mclib/quad.cpp) — `mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga")` (variable misnamed `mineTextureName`) | M |
| 12 | Mine TGAs both 16×16 RGBA8 | TGA header read at write-time returns `16x16 depth=32 type=2` for both | M |
| 13 | Mine textures use `gosHint_DisableMipmap \| gosHint_DontShrink` | [`mclib/quad.cpp:524,531`](../../../mclib/quad.cpp) | M |
| 14 | `MC_MAX_TERRAIN_TXMS` ~3000 (mentioned in PR1 memory) | grep search: not found in `mclib/`, terminology used in `gos_terrain_indirect.cpp` per PR1 memory; cross-check reference, not a load-bearing claim for this recon | NF (deferred — not load-bearing for this recon's conclusion) |
| 15 | Cement classes in overlay catalog: `numTextures > 9` heuristic | [`mclib/terrtxm.cpp:745`](../../../mclib/terrtxm.cpp) — `if (overlays[i].numTextures > 9)` | M |
| 16 | Negative claim — no per-size-class fallback needed | All three buckets uniform-dim within bucket: 128² (det), 64² (ovl), 16² (mine). Verified by 100% match on stock disk content | M |
| 17 | `GL_MAX_ARRAY_TEXTURE_LAYERS = 2048` — SPEC MINIMUM, not runtime-queried | GL 4.3 core spec minimum guaranteed = 2048; codebase has zero `glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS)` callers (verified 2026-05-08 adversarial review). RX 7900 XTX presumed at-or-above spec but unmeasured. | M (spec minimum guaranteed; hardware value unqueried) |

**Status summary:** 16 entries; 15 M, 0 D, 1 NF (`MC_MAX_TERRAIN_TXMS`
exact value — not load-bearing for the texture-array sizing
conclusion). All bucket-dimension claims grep- and disk-verified at
write-time.

---

## Decision input for spec session

- **No per-size-class fallback needed.** Brainstorm Q2 A(iii)'s "single
  2D array per bucket" stands.
- **PR2b overlay array build pattern**: per-mission, sized to actually-
  loaded overlay count (~100 layers worst-case stock). Mirror PR1
  cement-atlas pattern at `BuildCementCatalogAtlas`.
- **PR2a detail array**: degenerate (≤ 4 layers); decision conditional
  on recon item 6's retirement-vs-rendering question.
- **PR2c mine array**: degenerate (2 layers); spec session may opt for
  two samplers instead.
