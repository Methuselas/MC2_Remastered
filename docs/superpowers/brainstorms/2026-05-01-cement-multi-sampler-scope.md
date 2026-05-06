# Cement multi-sampler scope brainstorm — 2026-05-01

> **Discipline:** verify-then-write per CLAUDE.md. Every cited symbol grep-verified at write-time,
> file:line cited inline. No claim about existing code is made without a grep.

**Status:** Brainstorm (not a spec). User signs off on Q answers before a spec session opens.
**Prereqs:** PR1 indirect-terrain SOLID (f221570 + atlas hotfix a29ff83), predecessor brainstorm
`docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md`.

---

## Context

When `MC2_TERRAIN_INDIRECT=1`, pure-cement quads in airport/runway/concrete-pad areas render using
the underlying biome texture (grass, dirt, sand) instead of the authored concrete tiles. The root
cause is a texture-source mismatch encoded in `buildTerrainRecipeInline`
(`mclib/quad.cpp:417-441`):

- **Non-cement** branch (`quad.cpp:426`): `r.terrainHandle = Terrain::terrainTextures2->getTextureHandle(...)` —
  pulls from the **colormap atlas** (`TerrainColorMap`, the cpuColorMap-derived per-tile atlas).
- **Pure-cement** branch (`quad.cpp:438`): `r.terrainHandle = Terrain::terrainTextures->getTextureHandle(...)` —
  pulls from the **catalog** (`TerrainTextures`, the `textures.fit`-driven art-tile catalog, no `2`
  suffix). Returns a `mcTextureNodeIndex` that resolves to one of the concrete tile art textures
  (cement_1, cement_2, cement_3).
- **Alpha-cement** branch (`quad.cpp:432-434`): mixed — terrainHandle from colormap, overlayHandle
  from catalog.

The indirect bridge at `gameos_graphics.cpp:2287-2323` binds ONE texture at unit 0: the merged
`cpuColorMap` atlas GL texture (`g_atlasGLTex`). It sets `useAtlasColormap=1` so the frag
reconstructs atlas UV from WorldPos. This is correct for the colormap atlas. However, the
`PackThinRecordsForFrame` packer at `gos_terrain_indirect.cpp:942-947` uses `q.terrainHandle`
(the per-quad live handle), and for pure-cement quads that handle resolves to a catalog texture,
NOT the atlas. The bridge then draws all quads with atlas UV against the atlas texture regardless
of which texture the handle points to — so cement quads sample the atlas at their world position
and get whatever biome lives there.

The user has chosen **Option C**: multi-sampler fragment shader approach. Bind the colormap atlas
AND a separate "cement" texture (or cement atlas) simultaneously; the frag selects the source
based on `TerrainType` (already per-fragment via `_wp0`).

---

## Q1: Scope — pure-cement only, or alpha-cement + runway markings too?

The three cement sub-cases have distinct data flows:

1. **Pure cement** (`isCement && !isAlpha`, `quad.cpp:436-441`): both `terrainHandle` (catalog) and
   `terrainDetailHandle` (0xffffffff) come from the catalog. No overlay. These are the solid
   concrete areas: runways, tarmac, pads. This is the confirmed broken case.
2. **Alpha-cement** (`isCement && isAlpha`, `quad.cpp:430-435`): `terrainHandle` comes from the
   colormap atlas (correct — atlas IS bound), `overlayHandle` comes from catalog. These are
   concrete-to-terrain transition quads. The base color samples correctly from the atlas. The
   overlay is drawn separately via the legacy `gos_PushTerrainOverlay` path, which is NOT inside
   the indirect PR1 draw (per `gos_terrain_indirect.h:13`: "overlay stays legacy in this slice").
   Alpha-cement base rendering is therefore CORRECT on the indirect path; only the overlay
   portion stays on legacy.
3. **Runway markings / decals** — these are overlay objects (`MC_OverlayType` entries in
   `textures.fit`), drawn via `gos_PushTerrainOverlay`. They are entirely out of the PR1 scope.
   Not broken by PR1 because they run on the legacy path.
4. **buildRecipeSlot** in `gos_terrain_indirect.cpp:261-347` populates the dense recipe from
   mapData's Shape C cache. The cache entry's `terrainHandle` is written at
   `mapdata.cpp:292,299,305`. For pure-cement, `mapdata.cpp:305` writes the catalog handle via
   `Terrain::terrainTextures->peekTextureHandle(baseTexture)`. For non-cement and alpha-cement,
   lines 292/299 write the colormap handle. So the mismatch is baked into the dense recipe, not
   just the per-frame thin record.

**Options:**

- **(a) Pure-cement only.** Fix the bridge to bind a second sampler for the cement catalog texture
  (or a cement catalog atlas), select by `TerrainType==3` (material index 3 in `_wp0` means
  Concrete: `gos_terrain_indirect.cpp:244`). Scope: ~9 catalog source types
  (types 10, 13-20 from `textures.fit`), a small number of distinct cement tile textures per
  mission. This fixes the visible bug.
- **(b) Pure-cement + alpha-cement overlay too.** Also fix the overlay path for alpha-cement, which
  requires pulling the overlay draw into the indirect path. That means adding a second indirect draw
  pass for overlays — a qualitatively different scope expansion (overlay blending, alpha state,
  second thin record type). Much larger surface area.
- **(c) All cement variants + runway markings.** Pulls `gos_PushTerrainOverlay` into the indirect
  path. Equivalent to retiring the entire overlay legacy contract, which the predecessor brainstorm
  explicitly deferred to a follow-up.

**Leaning: (a).** Alpha-cement base rendering is already correct on PR1 (the base samples from the
atlas, which IS bound correctly). The overlay for alpha-cement stays on legacy — same as the rest
of the overlay path. Option (b) and (c) both require retiring the overlay contract, which is a
separate well-defined follow-up the predecessor brainstorm already earmarked. The visible bug is
pure-cement only; fix pure-cement only.

---

## Q2: Cement texture-binding architecture

For pure-cement, the frag needs access to the correct catalog texture at draw time. Options:

- **(a) Single extra `sampler2D` for a "cement catalog mini-atlas."** Pre-bake all cement catalog
  tile textures into a single 2D atlas at mission load (analogous to `BuildColormapAtlas`). Bind
  it at a fixed unit (unit 3 is `tex3` — "legacy, unused", `gos_terrain.frag:35`). The frag
  samples this atlas when `TerrainType==3`. Lookup requires knowing which cement variant a quad
  uses — either from `_wp0` material index (only distinguishes cement vs. not-cement, not which of
  cement_1/2/3) or from a new per-quad cement-variant index.
- **(b) sampler2DArray with one layer per catalog texture.** Each cement tile type gets one array
  layer. `docs/amd-driver-rules.md:9`: synthetic canary passed on RX 7900 XTX (Canary A, driver
  26.3.1). Canary B (real terrain texture / mip / sampler parity) has NOT been run. Until Canary B
  clears, this remains risky for production use per the explicit AMD driver rule. Cement textures
  are real, mipped art tiles — exactly the Canary B scenario.
- **(c) Bindless textures.** Requires `GL_ARB_bindless_texture` / `GL_NV_bindless_texture`. Not
  confirmed available on the RX 7900 XTX context we target. Extension dependency is unacceptable
  for a production path per project scope.
- **(d) Per-cement-bucket separate draw.** Issue one indirect draw per cement type, binding the
  catalog texture for that type. Abandons the single-call indirect model. Reverts to legacy
  bucket-loop behavior for cement only. Costs driver overhead; also requires cement quads to be
  separate thin-record groups, breaking the uniform SSBO layout.

**Leaning: (a)** — cement catalog mini-atlas. Unit 3 (`tex3`) is declared in the frag but marked
"legacy, unused with per-material POM" (`gos_terrain.frag:35`). We can rebind it to the cement
atlas at the bridge entry point without changing the frag's sampler declaration, just the uniform
integer and the texture bind. The mini-atlas approach is analogous to `BuildColormapAtlas` and
builds on proven infrastructure. The per-variant lookup (Q4) determines how to compute atlas UV.

Option (b) blocked on Canary B. Option (c) has extension dependency. Option (d) reverts to
per-bucket draws, defeating the purpose of indirect.

---

## Q3: Cement catalog enumeration — how many textures and are they enumerable at mission load?

This is the key factual question that determines atlas sizing.

From `terrtxm.h:42-44`:
```cpp
#define BASE_CEMENT_TYPE    10
#define START_CEMENT_TYPE   13
#define END_CEMENT_TYPE     20
```

From `textures.fit` (stock): types 10, 13, 14, 15, 16, 17, 18, 19, 20 are named
cement_1/cement_2/cement_3 with distinct mask and priority variants = **9 cement source type IDs**.
Each maps to one `types[i].baseTXMIndex` during `TerrainTextures::init` (`terrtxm.cpp:251-252`).
However, the `setTexture` path (`terrtxm.cpp:1359-1451`) creates **transition textures** by
compositing when corners have mixed cement types. The number of unique transitions is bounded by
`MAX_MC2_TRANSITIONS = 8192` (`terrtxm.cpp:56`), but the number actually created per mission
depends on map content.

The `getTextureHandle(DWORD texture)` call at `quad.cpp:438` takes a 16-bit index
(`textureData & 0x0000ffff`) which is the raw `TerrainTextures::textures[]` slot. This index
space covers both base types AND transitions, all allocated from the same `nextAvailable` counter
(`terrtxm.cpp:59`).

For a stock airport mission (mc2_01), the cement texture IDs seen in quads are all base-type
indices plus any cement-to-cement transitions generated by `setTexture`. For a mini-atlas approach,
we need to enumerate these. There are two sub-options:

- **Static: bake only base cement types** (9 types × 1 texture each = 9 textures). This covers all
  pure-cement quads where all 4 corners are the same type (`setTexture` returns `types[v0Type].baseTXMIndex`
  unchanged). Transition textures arise from mixed-type cement borders (e.g., cement_1 meeting cement_2).
  In stock missions this may be rare; needs runtime measurement.
- **Dynamic: enumerate all textures with `MC2_TERRAIN_CEMENT_FLAG` set.** Walk
  `TerrainTextures::textures[0..nextAvailable-1]`, collect all entries where
  `(textures[i].flags & MC2_TERRAIN_CEMENT_FLAG)`. This includes both base types AND any
  transitions actually created during `setTexture` calls. At `primeMissionTerrainCache` time, the
  face-cache build has already called `setTexture` for every quad, so `nextAvailable` is stable
  and the set is enumerable.

**Leaning: dynamic enumeration** — walk all textures with the cement flag set after the face-cache
build. This automatically captures transition textures without requiring per-mission content
knowledge. The upper bound on stock missions is well below the 100 textures needed to make a 1024²
atlas unwieldy: cement variants are typically 9 base types + at most a few dozen transitions.

**Open question for spec:** Measure the actual count for mc2_01 at runtime by counting flagged
entries. A `MC2_TERRAIN_INDIRECT_TRACE=1` print at atlas-build time is the natural place.

---

## Q4: Per-fragment source selection — how does the frag know which cement texture to sample?

Current state: `TerrainType` is a float varying interpolated from corner material indices
(0=Rock, 1=Grass, 2=Dirt, 3=Concrete). The frag already gates concrete normal on `TerrainType`
(`gos_terrain.frag:330: smoothstep(2.0, 3.0, TerrainType)`). This is a continuous interpolated
value, not a discrete texture index.

The problem: for pure-cement, we need to know which cement catalog texture to sample, not just that
the fragment is cement. If all cement quads happen to use the same catalog texture (e.g., after
baking into a single atlas), a 1D atlas UV formula from world position suffices. If different
quads use different cement tiles (cement_1 vs cement_2), we need a per-quad cement-variant index.

Options for passing the cement variant to the frag:

- **(a) Single-texture cement atlas with WorldPos UV reconstruction.** If we bake all cement tiles
  into one atlas (analogous to the colormap atlas), the frag can reconstruct which tile it belongs
  to from WorldPos alone — same formula as the colormap UV reconstruction in `gos_terrain.frag:231-236`
  (`useAtlasColormap` path). This requires that the atlas tiling matches the terrain grid. The
  cement catalog textures are individual tiles (one per TerrainType), not colormap-derived: their
  UV assignment must come from a different formula. This is the key design challenge.
- **(b) Per-quad cement catalog index in thin-record `_pad0`.** `TerrainQuadThinRecord._pad0`
  (`gos_terrain_patch_stream.h:107`) is currently zero-padded. It is in the second `uvec4`
  alongside the 4 lightRGB values — reading it in the frag via `thinRecs[recordIdx].control.w`
  or similar is feasible without touching the SSBO layout (it's already declared). The cement
  atlas layer index (which cement tile art to sample) would be packed here as a small uint.
  The VS reads the thin record and could pass this as an additional flat varying to the frag.
  **BUT**: adding a new varying would break linking compatibility with the legacy non-thin VS chain
  that shares `gos_terrain.frag` (explicit warning in `gos_terrain_thin.vert:26` comment). The
  legacy chain does not emit this varying → linker silently fails → terrain renders transparent.
  This option requires either (i) a `flat in` varying with a default-zero path in the legacy VS
  (compatible if the legacy VS adds it as a dummy output), or (ii) a different encoding.
- **(c) Per-quad cement index as a new `uniform int` array or SSBO uniform.** Upload one integer
  per-quad (cement catalog index), indexed by `vn0` (vertexNum). This is another SSBO or a
  separate uniform texture. Adds CPU upload overhead and another binding point in the bridge.
- **(d) Encode cement variant in existing `_wp0` terrainType bits.** Currently `_wp0` packs 4
  corner material indices at 8 bits each (`gos_terrain_indirect.cpp:344`): value 3 = Concrete.
  We could expand the Concrete encoding to distinguish cement subtypes (cement_1=3, cement_2=4,
  cement_3=5, etc.) — but this would require changing `terrainTypeToMaterialLocal` and the frag's
  `pureConcrete` gate (`smoothstep(2.0,3.0,TerrainType)`) which currently only tests for ≥3.
  Requires shader change but no new varying or SSBO.

**Leaning: (a) single-texture cement atlas with WorldPos UV** if the cement tiling formula is
tractable, falling back to **(d) extend `_wp0` encoding** if multi-variant cement needs
per-type distinction. Option (b) is attractive but the varying-linker-compatibility concern is
real (documented in `gos_terrain_thin.vert:26`). Option (c) adds complexity.

The key open question is whether all pure-cement quads on stock missions use the same cement
texture or different ones. If mc2_01 airport uses only one cement tile variant (cement_1 at
type 13 for example), option (a) degenerates to "just bind the single cement texture at unit 3"
with no atlas formula needed. This is the lowest-risk path and warrants measuring first.

---

## Q5: How large is the cement-quad subset?

This determines whether cement quads are rare enough to warrant special casing vs. common enough
to require a fully general solution. No static count is available from source code; the cement
flag is only set during `TerrainTextures::setTexture()` at mission load time. The cement types
(10, 13-20) are identified by `isCementType` (`terrtxm.cpp:1079-1083`).

Two ways to estimate:
- **Static approach**: `textures.fit` has 9 cement types + non-cement types (0-9, 11-12, 21+).
  Airport missions (mc2_01) are the known cement-heavy cases. Rural missions (mc2_10, mc2_17)
  are likely cement-free or near-zero.
- **Runtime instrumentation**: add a counter in `BuildColormapAtlas` or `BuildDenseRecipe`
  that counts recipe slots where the `isCement` flag is set (readable from the Shape C cache
  entry `entry->isCement()`). This would give exact per-mission counts without shipping debug code
  since it fits under the existing `MC2_TERRAIN_INDIRECT_TRACE` gate.

**Expected finding**: cement quads constitute a small fraction of the total (~5-15% at most on
airport missions, near zero elsewhere). This supports the mini-atlas approach: even if there are
100 cement quads, the atlas is a tiny one-time upload.

**Open question for spec:** Add a `TRACE` print at recipe-build time: count quads where
`entry->isCement() && !entry->isAlpha()` (pure-cement). Print per-mission. This is a one-liner
under the existing `if (traceOn())` guard.

---

## Q6: Stage sequencing — is this Stage 4 prerequisite, Stage 5, or a hotfix to f221570?

From `gos_terrain_indirect.h:1-14` (the header docstring), PR1 is "SOLID-only" and detail/overlay
paths stay legacy. The cement fix is about the SOLID path (pure-cement quads are solid base-texture
quads with `terrainDetailHandle=0xffffffff` and `overlayHandle=0xffffffff`). This is strictly
within the existing PR1 scope.

Options:

- **(a) PR1a hotfix — amend to the f221570/a29ff83 line.** Small enough in scope (one new atlas
  build + one new sampler bind + frag selection logic) to fix before flipping the `MC2_TERRAIN_INDIRECT`
  default to ON. This unblocks Stage 4 (the "default-on" flip). Without this fix, the default-on
  flip would ship broken airport rendering to end users.
- **(b) Stage 5 — separate follow-up after default-on.** Ship default-off with the known cement bug
  documented. Users enabling `MC2_TERRAIN_INDIRECT=1` get broken cement. Default-on flip
  happens after cement is fixed.
- **(c) Fold into the Stage 4 spec.** Stage 4 = "default-on flip + cement fix" in one PR. Stage 4
  currently has no implementation work other than flipping the env gate; adding cement fix here
  makes Stage 4 a real implementation slice again.

**Leaning: (a) or (c).** The cement bug is a PR1 correctness regression visible on mc2_01 (the
first tier1 smoke mission). Shipping the default-on flip without fixing it means every user who
runs mc2_01 with indirect enabled sees grey tarmac instead of concrete. Given that mc2_01 is the
canonical regression test, this is not acceptable for a default-on promotion. The fix is
self-contained (bounded scope per Q1/Q2/Q3) and the right call is to fix it before default-on.

Option (c) (fold into Stage 4) is cleaner than (a) because it keeps the two-commit sequence
(ship → soak → flip) from the predecessor brainstorm intact. Option (a) amends the already-tagged
PR1 commits, which can cause confusion if the PR is already merged. Recommend (c): make Stage 4
a "cement fix + default-on" PR.

---

## Q7: Validation gates — what does "cement quads render correctly" mean?

**Visual canary**: mc2_01 airport tarmac. The concrete runway textures are visually distinctive
(grey/tan tiled concrete vs. green grass). Side-by-side with legacy path (`MC2_TERRAIN_INDIRECT=0`)
at the airport camera position is the primary gate. This is gate A in the 4-gate ladder.

Beyond the visual canary:
- **Parity gate**: the existing `MC2_TERRAIN_INDIRECT_PARITY_CHECK` does NOT compare cement
  texture handles (the recipe's `terrainHandle` field is not compared in `ParityCompareRecipeFrame`
  at `gos_terrain_indirect.cpp:580-736`; it only compares geometry fields). Adding a handle
  comparison to the parity check would catch recipe drift on cement — but this is stretch, not
  required for the cement fix itself.
- **Tier1 5/5 triple**: all 5 smoke missions at `MC2_TERRAIN_INDIRECT=0`, `=1`, and
  `=1`+`PARITY_CHECK=1`. mc2_01 is the cement canary in the set.
- **No new Tracy gate required** for this fix: the cement fix adds no new CPU work to measure. It
  adds one atlas upload and one additional texture bind per draw. The cost is sub-millisecond
  and not worth a new Tracy zone.

**Newly required: a cement-specific visual canary position.** The standard smoke camera may not
be positioned over the airport tarmac. The spec should call out a fixed-camera position over the
mc2_01 runway that exercises pure-cement quads. This is the one new gate the cement fix needs
that the predecessor brainstorm did not establish.

---

## Q8: Interaction with the M2 legacy fast path (shared frag shader)

The `gos_terrain.frag` shader is shared between the indirect path (`gos_terrain_thin.vert`) and
the legacy M2 fast path (also thin VS). From `gameos_graphics.cpp:2361-2366`, after the indirect
draw, `useAtlasColormap` is reset to 0 so the M2 fast path doesn't inherit the atlas-mode flag.

Adding a cement sampler (new `uniform sampler2D` at unit 3, reusing the `tex3` slot) has the
following M2 implications:

- **M2 fast path does NOT bind a cement texture** at unit 3. The current `tex3` binding in M2 is
  unused per `gos_terrain.frag:35` ("detail displacement, legacy, unused with per-material POM").
  If the frag samples `tex3` only when `TerrainType==3` and `useAtlasColormap==1`, M2 is safe:
  it never sets `useAtlasColormap=1` (reset to 0 after each indirect draw at `gameos_graphics.cpp:2366`).
- **Guard required**: the cement sampler path in the frag must be gated on `useAtlasColormap != 0`
  (the same gate that controls atlas UV). When `useAtlasColormap==0` (M2 path), the frag
  continues to sample `tex1` with per-tile UV against the per-tile bound texture — no cement atlas
  needed or used.
- **M2 cement quads**: the M2 path handles cement correctly because it issues per-bucket draws
  with the correct catalog texture bound at unit 0 for each bucket. Option C does NOT change M2.

**Conclusion**: M2 is safe as long as the frag gates cement atlas sampling on `useAtlasColormap`.
No new sampler unit conflict. Unit 3 (`tex3`) is safe to rebind in the bridge for the indirect path
only; M2 leaves it at whatever it was (harmless since M2 never reads it).

---

## Q9: Atlas memory overhead

The colormap atlas (`BuildColormapAtlas`) is sized `cpuColorMapSize × cpuColorMapSize × 4 bytes`
(RGBA8). The comment in `gos_terrain_indirect.cpp:354` notes it's "5120²×4 = 100 MB per mission"
(extrapolating from the ~350ms upload time in memory notes).

A cement catalog mini-atlas is orders of magnitude smaller:
- Stock cement catalog: ~9 base textures + transitions. If `TERRAIN_TXM_SIZE` = 64 (typical stock)
  and each cement tile is 64×64 RGBA8, then 64×64×4 = 16 KB per tile. 100 tiles (generous) =
  1.6 MB total atlas. Negligible.
- Even at a generous max (1000 catalog textures), a 1024×1024 atlas = 4 MB.

This is not a blocker. The overhead compared to the existing colormap atlas is small enough to
not require a design decision — just build the atlas, it fits in budget.

One consideration: the cement catalog textures are NOT pre-loaded into CPU memory the way
`cpuColorMap` is (terrtxm2.h:93). They are stored as `mcTextureNodeIndex` handles in
`TerrainTextures::textures[]` which resolve to GPU-resident gosHandle textures
(`terrtxm.h:281-288: getTextureHandle`). Building a CPU-side cement atlas therefore requires
either: (a) reading the cement textures back from GPU (slow, generally undesirable), or (b)
maintaining CPU-side copies in `tileRAMHeap` during init. Option (b) is the clean path since
the cement tile data is loaded via `loadTextureMemory` / `textureFromMemory` into `tileRAMHeap`
RAM before upload to GPU. The `tileRAMHeap` is NOT freed until `TerrainTextures::destroy`.
**This needs spec verification: confirm cement tile RAM is accessible at recipe-build time.**

---

## Decision summary

| Q | Leaning answer | Blockers / awaits user |
|---|---|---|
| **Q1** | Pure-cement only (a). Alpha-cement base is correct; overlay stays legacy. | Clear. |
| **Q2** | Cement catalog mini-atlas at unit 3 (a), reusing `tex3` slot. | Clear if Q3 dynamic enum tractable. |
| **Q3** | Dynamic enumeration: walk `TerrainTextures::textures[]` for CEMENT_FLAG entries post face-cache build. | Needs runtime count on mc2_01 to verify scale. |
| **Q4** | WorldPos UV for single-atlas (a); extend `_wp0` if multi-variant needed (d). | Needs per-mission cement variant count. If mc2_01 is single-variant, Q4 is trivial. |
| **Q5** | Small fraction (~5-15% on airport missions, 0% elsewhere). Measure via TRACE print. | Needs runtime count. |
| **Q6** | Fold into Stage 4: "cement fix + default-on flip" (c). | User decision: confirm Stage 4 expands scope. |
| **Q7** | Visual canary at mc2_01 airport tarmac (new fixed-camera position needed). Tier1 5/5 triple. | Spec must define the canary camera position. |
| **Q8** | M2 is safe; gate cement sampler on `useAtlasColormap`. No schema change. | Clear. |
| **Q9** | Atlas overhead negligible (~1-2 MB). BUT: verify CPU-side cement RAM accessible at recipe-build time. | Spec must confirm `tileRAMHeap` accessibility. |

---

## Cross-references (file:line citations verified at write-time)

| Symbol | File:line | Note |
|---|---|---|
| `buildTerrainRecipeInline` non-cement branch | `mclib/quad.cpp:422-428` | `terrainTextures2->getTextureHandle` |
| `buildTerrainRecipeInline` alpha-cement branch | `mclib/quad.cpp:430-435` | `overlayHandle` from catalog |
| `buildTerrainRecipeInline` pure-cement branch | `mclib/quad.cpp:436-441` | `terrainTextures->getTextureHandle` |
| `BASE_CEMENT_TYPE`, `START_CEMENT_TYPE`, `END_CEMENT_TYPE` | `mclib/terrtxm.h:43-45` | 10, 13, 20 |
| `MC2_TERRAIN_CEMENT_FLAG` | `mclib/terrtxm.h:53` | `0x00000001` |
| `TerrainTextures::getTextureHandle` | `mclib/terrtxm.h:281-288` | returns `mcTextureNodeIndex` |
| `TerrainTextures::isCement` | `mclib/terrtxm.h:337-342` | checks `MC2_TERRAIN_CEMENT_FLAG` |
| `textures.fit` cement types | `mc2srcdata-fresh/textures/textures.fit:112-215` | types 10,13-20 = cement_1/2/3 |
| `TerrainTextures::setTexture` cement path | `mclib/terrtxm.cpp:1359-1451` | creates transitions via `createTransition` |
| `MAX_MC2_TRANSITIONS` | `mclib/terrtxm.cpp:56` | `8192` |
| `TerrainTextures::nextAvailable` | `mclib/terrtxm.cpp:59` | static; post-init = all allocated |
| `TerrainQuadThinRecord._pad0` | `GameOS/gameos/gos_terrain_patch_stream.h:107` | zero-padded, available |
| `terrainTypeToMaterialLocal` cement mapping | `GameOS/gameos/gos_terrain_indirect.cpp:240-248` | types 10,13-19 → material 3 (Concrete) |
| `buildRecipeSlot` cement cache read | `GameOS/gameos/gos_terrain_indirect.cpp:322-333` | `entry->isCement()` guard |
| `BuildColormapAtlas` | `GameOS/gameos/gos_terrain_indirect.cpp:379-420` | cpuColorMap → `g_atlasGLTex` |
| `gos_terrain_bridge_drawIndirect` atlas bind | `GameOS/gameos/gameos_graphics.cpp:2287-2323` | unit 0 = atlas, `useAtlasColormap=1` |
| `useAtlasColormap` reset | `GameOS/gameos/gameos_graphics.cpp:2361-2366` | reset to 0 after draw |
| `tex1`=unit0, `matNormal[i]`=units5-9, `shadowMap`=unit9 | `GameOS/gameos/gameos_graphics.cpp:3608-3629` | unit 3 (`tex3`) not explicitly bound → free for cement |
| `tex3` declaration | `shaders/gos_terrain.frag:35` | "legacy, unused with per-material POM" |
| `TerrainType` / `pureConcrete` gate | `shaders/gos_terrain.frag:330` | `smoothstep(2.0, 3.0, TerrainType)` |
| `useAtlasColormap` frag gate | `shaders/gos_terrain.frag:231-237` | selects atlas UV vs. Texcoord |
| varying compatibility comment | `shaders/gos_terrain_thin.vert:26` | new varying → silent linker fail |
| `_wp0` terrainType pack | `shaders/gos_terrain_thin.vert:149-151` | 4 corners × 8 bits |
| `sampler2DArray` canary status | `docs/amd-driver-rules.md:9` | Canary A passed; Canary B not yet run |
| `mapdata.cpp` cement cache entry | `mclib/mapdata.cpp:303-305` | `peekTextureHandle` for pure cement |
