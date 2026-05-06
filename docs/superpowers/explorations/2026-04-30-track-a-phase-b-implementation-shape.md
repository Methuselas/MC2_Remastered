# Track A — Phase B Implementation Shape

**Date:** 2026-04-30
**Status:** design-only / handoff
**Predecessor:** [`2026-04-29-track-a-render-headroom-status.md`](2026-04-29-track-a-render-headroom-status.md)
**Predecessor (sibling slice):** [`../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md`](../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md)

This document specifies — at the byte / line / function-signature level — the shape of every pending Track A piece: **S0** (smoke env passthrough), **S1** (`quadSetupTextures` next slice), **B0** (VS-side diffuse), **B1** (16 B thin record). The next session should be able to pick any one of these slices and start writing code with no further design work.

---

## S0 — Smoke runner FASTPATH env passthrough (form)

The patch is *already authored in the working tree* (see `git diff scripts/run_smoke.py` — 9 added entries). It is not committed. S0 is therefore a commit-only slice.

**File:** `scripts/run_smoke.py`
**Region:** lines 232–247 (env-extra dict comprehension)
**Diff shape (already realized in working tree):** appends 9 keys to the `os.environ` filter — `MC2_PATCHSTREAM_THIN_RECORDS`, `MC2_PATCHSTREAM_THIN_RECORDS_DRAW`, `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH`, `MC2_THIN_DEBUG`, `MC2_WATER_DEBUG`, `MC2_WATER_STREAM_DEBUG`, `MC2_RENDER_WATER_FASTPATH`, `MC2_RENDER_WATER_PARITY_CHECK`.

**Sequencing.** Single commit. Subject: `fix(smoke): propagate THIN_RECORDS + water FASTPATH env to child mc2`. Validation gate: run `py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing` once with all three thin env vars set in the parent shell, confirm the child mc2 logs `[PATCH_STREAM v1] event=thin_records_enabled` and `[PATCH_STREAM v1] event=fast_path_enabled`. Smoke verdict must remain 5/5 PASS, +0 destroys delta.

**Body of commit message must explicitly state** that pre-S0 tier1 results were exercising the *legacy* path even when shape C was default-on, and that this is the gate-validation closeout for the M2 chain. Without this statement, the status snapshot remains misleading.

---

## S1 — `quadSetupTextures` next slice (form)

This slice is fully specified in [`../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md`](../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md). The decision is *measurement-gated*; design crystallizes once two Tracy captures are in hand. The shape of each branch:

**Branch A (cache-read drops zone ≥0.5 ms).** Single-file change.
- File: [`mclib/quad.cpp:58-389`](../../../../../../mclib/quad.cpp) — flip `s_shapeCEnabled` initializer so absence of `MC2_MODERN_TERRAIN_PATCHES` reads as `true` (mirror `aee39cc` for Shape C cache-read; that commit flipped `MC2_MODERN_TERRAIN_PATCHES` itself, but only for a different branch).
- Subject: `feat(shape-c): default-on the cache-read branch of setupTextures`.
- Gates: visual A; Tracy delta on `Terrain::geometry quadSetupTextures` (≥0.3 ms) B; `[SHAPE_C] MISMATCH` count = 0 over tier1 + Carver5O + Magic with `MC2_SHAPE_C_PARITY_CHECK=1` C; tier1 5/5 PASS D.

**Branch B (cache-read drops 0.1–0.5 ms; lift `addTriangle` reservation).** Two-file change.
- File 1: `mclib/mapdata.h:86-122` — extend `WorldQuadTerrainCacheEntry` with `uint8_t triReservation[MC_MAX_TERRAIN_LAYERS];` (typically 4 layers; pack to 4 B for a 4 B grow on the entry).
- File 2: `mclib/quad.cpp:394` (`addTerrainTriangles()`) — replace the four `mcTextureManager->addTriangle(...)` calls with one `addTriangleBulk(node, count)` per layer, driven by the cached reservation array; primed in `Terrain::primeMissionTerrainCache` (`mclib/terrain.cpp:561`).
- New signature: `void mcTextureManager::addTriangleBulk(MC_TextureNode* node, uint32_t count)` (declaration in `mclib/txmmgr.h`, definition in `mclib/txmmgr.cpp` near `addTriangle`). Bulk path increments the per-node triangle counter once instead of `count` times.
- Subject: `perf(shape-c): cache addTriangle reservations per quad`.

**Branch C (cache-read <0.1 ms — pivot).** New spec; out of scope for this design doc.

---

## B0 — VS-side diffuse, no struct change (form)

**Goal:** Validate the math end-to-end before mutating the SSBO record. This is the M0e-equivalent for Phase B — fully reversible, gated by a new env var, no perf gain expected.

**New env gate:** `MC2_PATCHSTREAM_GPU_LIGHTING=1`. Read once at TU init in `gos_terrain_patch_stream.cpp` near line 66 (`s_thinRecordsOn`):
```
static const bool s_gpuLightingOn = (getenv("MC2_PATCHSTREAM_GPU_LIGHTING") != nullptr);
```
Plumb to the thin VS via a new `int gpuLightingMode` uniform (cached in `cacheThinTerrainUniformLocations` at `gameos_graphics.cpp:1625`, set at `terrainBindThinUniformsForPatchStream` near line 3053 alongside the existing `terrainLightDir` upload). One `glUniform1i(loc, s_gpuLightingOn ? 1 : 0)` per draw bucket loop entry — uniform location cached, no per-call cost.

**Shader change.** [`shaders/gos_terrain_thin.vert`](../../../../../../shaders/gos_terrain_thin.vert):
- Add at the top-of-file uniform block (after the existing `mvp` declaration, ~line 34):
  ```
  uniform vec4 terrainLightDir;     // already plumbed; was FS-only on this VS
  uniform int  gpuLightingMode;     // 0 = legacy CPU lightRGB, 1 = VS diffuse
  ```
  (`terrainLightDir` is already bound by `terrainBindThinUniformsForPatchStream:3053`; the location is cached but the VS has not been declaring it. Add the declaration.)
- Replace line 125 (`Color = unpackARGB(lrgb);`) with:
  ```
  if (gpuLightingMode == 1) {
      float diffuse  = max(dot(worldNorm, normalize(terrainLightDir.xyz)), 0.0);
      float lit      = mix(0.35, 1.20, diffuse);   // matches gos_terrain.frag:491
      float existing = unpackARGB(lrgb).a;          // preserve selection / terrainTextures2 alpha
      Color = vec4(vec3(lit), existing);
  } else {
      Color = unpackARGB(lrgb);
  }
  ```
The `mix(0.35, 1.20, ...)` ceiling is load-bearing — see [`memory/terrain_lighting_range_ceiling.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/terrain_lighting_range_ceiling.md). Match it bit-for-bit; do not invent a new range.

**No CPU change required.** `effectiveLightRGB` (`mclib/quad.cpp:1789-1795`) keeps writing `tr.lightRGB[0..3]`; the alpha bit survives B0 untouched and the shader simply ignores RGB when the mode is on.

**Sequencing — single commit.**
1. Add env gate + uniform plumbing.
2. Add shader branch.
3. Run with `MC2_PATCHSTREAM_GPU_LIGHTING=1` on `mc2_01`, `mc2_03`, `mc2_24` at Wolfman zoom; visual A/B against env-off side-by-side.
4. Document **at minimum** any pixel-level divergence in the commit body — the next slice (B1) cannot land until B0 is visually clean or the divergence sources (point lights, lightning glow, fog tint, selection highlight) are inventoried.

**Parity counter:** none new — B0 is a pure shader-side branch. The existing `s_thinRecordVertParity` already covers per-frame quad-count parity.

---

## B1 — 16 B thin record (form)

Depends on B0 visual-clean. This is the slice that actually lands the CPU win: drop the 4× `lightRGBc` lambda calls per fast-path quad (4 × 14,000 quads/frame ≈ 56,000 lambda invocations dropped) and shrink the SSBO 32 B → 16 B (halves the upload bandwidth on the thin record buffer).

### Post-B1 `TerrainQuadThinRecord` byte layout (16 B exact)

```
offset  size  field           encoding
------  ----  --------------  --------------------------------------------------
  0     4     recipeIdx       uint32 — index into recipe SSBO
  4     4     terrainHandle   uint32 — raw gosHandle, tex_resolve at flush
  8     4     flags           uint32 — see flag layout below
 12     4     selectionMask   uint32 — 4 corners × 8 bits override metadata
                              corner c bits: 0 = selected, 1 = alphaOverride,
                                             2..7 reserved
------  ----  --------------  --------------------------------------------------
total: 16 bytes  (alignas(16) preserved; matches std430 vec4 alignment)
```

`flags` layout is unchanged from current 32 B record:
```
bit  0       uvMode (0=TOPRIGHT, 1=BOTTOMLEFT)
bit  1       pzTri1Valid
bit  2       pzTri2Valid
bits 3..31   reserved (zero)
```

**Why a separate `selectionMask` field instead of stuffing into `flags`:** flags is per-quad-once; selection state is per-corner-per-frame. Mixing them invites a future bug where a flags rewrite stomps selection bits or vice versa. 4 free bytes already exist (the current `_pad0` slot at offset 12); promoting that pad to selectionMask is a zero-cost rename in std430.

### File-by-file changes

**`GameOS/gameos/gos_terrain_patch_stream.h:103-111`.** Replace `TerrainQuadThinRecord` definition with the 16 B layout above. Update the static_assert:
```
static_assert(sizeof(TerrainQuadThinRecord) == 16,
    "TerrainQuadThinRecord must be 16 bytes for std430 alignment");
```
Update `kPatchStreamThinRecordBytesPerSlot` (line 122–123): `* 32u` → `* 16u`. The `kPatchStreamMaxThinRecordsPerSlot` constant is unchanged (it is record-count-derived, not byte-derived).

**`GameOS/gameos/gos_terrain_patch_stream.cpp`.**
- `appendThinRecord(...)` signature drops the four `uint32_t lightRGB*` parameters and gains one `uint32_t selectionMask`. Header signature in `.h:183-187` updates in lockstep.
- `appendThinRecordDirect(const TerrainQuadThinRecord&)` signature unchanged (still passes the struct by ref); only the struct content changed.
- `s_thinDebug` first-record print at `gos_terrain_patch_stream.cpp:1390-1404` drops the `lightRGB*` fields from the format string; add `selectionMask=0x%08x`.

**`mclib/quad.cpp:1789-1846`.** Replace the `lightRGBc` lambda with a `selBitsFor` helper:
```cpp
const bool isCement       = Terrain::terrainTextures->isCement(...);
const bool isAlpha        = Terrain::terrainTextures->isAlpha(...);
const bool alphaOverride  = Terrain::terrainTextures2 && (!isCement || isAlpha);
auto selBitsFor = [&](int c) -> uint32_t {
    uint32_t b = 0;
    if (vertices[c]->pVertex->selected) b |= 0x1u;
    if (alphaOverride)                  b |= 0x2u;
    return b;
};
uint32_t selMask =
       (selBitsFor(0)      )
     | (selBitsFor(1) <<  8)
     | (selBitsFor(2) << 16)
     | (selBitsFor(3) << 24);
```
Then `tr.selectionMask = selMask;` replaces the four `tr.lightRGB* = lightRGBc(c);` lines (1842–1845).

**`shaders/gos_terrain_thin.vert`.**
- Update SSBO struct declaration (lines 4–8) to mirror the new C++ layout: `uvec4 control` → split into 4× `uint` named fields, OR keep as single `uvec4` where `.x=recipeIdx, .y=terrainHandle, .z=flags, .w=selectionMask`. Recommend the latter for one-line std430 alignment.
- Drop `uvec4 lightRGBs;` from the struct.
- Remove `uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);` (line 119).
- Replace the `Color = ...` block with corner-extracted selection logic:
  ```
  uint selBits = (tr.control.w >> (cornerIdx * 8u)) & 0xFFu;
  bool selected      = (selBits & 0x1u) != 0u;
  bool alphaOverride = (selBits & 0x2u) != 0u;
  // diffuse-from-normal as in B0
  float diffuse = max(dot(worldNorm, normalize(terrainLightDir.xyz)), 0.0);
  float lit     = mix(0.35, 1.20, diffuse);
  vec3  rgb     = selected      ? SELECTION_RGB
                : alphaOverride ? vec3(1.0)
                : vec3(lit);
  Color = vec4(rgb, 1.0);
  ```
  `SELECTION_RGB` is a new GLSL `const vec3` matching `SELECTION_COLOR` from `mclib`. Pull the constant value from `mclib/terrain.cpp` at code-time and hardcode in the shader header.

### Sequencing within B1

Single commit. The struct mutation, CPU-side emit change, and shader change *must* land together — a partial commit leaves the GPU reading 32 B records into a 16 B layout and silently corrupts every quad. Order within the diff:
1. Header struct + constants.
2. CPU emit site.
3. Shader struct + body.

**Parity counter assertion:** existing `s_thinRecordVertParity` continues to hold (verts-per-record unchanged). Add **one new** runtime invariant inside `appendThinRecordDirect`:
```cpp
assert((tr.flags & ~0x7u) == 0 && "flag bits 3+ must be zero");
```
Catches accidental flag stomping when bits get reused in a future slice.

**Perf gate (B1-specific).** Tracy delta on `Terrain::render drawPass` at Wolfman zoom on `mc2_01`: expect ≥0.3 ms drop vs. M2d baseline (1.46 ms). If the delta is <0.1 ms, the lambda inlining was already free and B1's win is solely the upload-bandwidth halving — note that in commit message and proceed (still correct, still smaller, still fewer cache lines touched per frame).

---

## Render-contract registry coordination

[`../specs/2026-04-26-render-contract-registry-design.md`](../specs/2026-04-26-render-contract-registry-design.md) defines the registry that future composite slots (e.g. ImGui in Track B) plug into. Phase B does **not** add a new composite slot — terrain remains the existing terrain pass, drawn through `terrainBindThinUniformsForPatchStream`. No registry entry is added or modified.

The registry coordination *is* relevant for two indirect reasons:
1. **`terrainLightDir` becomes a VS uniform contract**, not just an FS one. Any future split (e.g. shadow-only vs. lit terrain passes) must keep the VS form of the uniform consistent with the FS form. Document this in the registry's "Terrain solid pass" entry as a `vs_uniforms_required: [terrainLightDir]` field — coordinate with the Track A render-contract-registry doc owner before B0 lands.
2. **The 16 B thin record's `selectionMask` field is a new modder-stable contract.** If/when modders ever inject custom selection rendering (e.g. faction-coloured highlight), the bit layout in `selectionMask` becomes the wire format. Document the bit allocation (bits 0..1 used, 2..7 reserved) in the registry under a new `terrain_selection_mask_v1` entry. Bump only when bits are reassigned; new bits are append-only.

Neither item blocks B0 or B1 landing. They are documentation tasks that pair with the slice commits.

---

## Sequencing across all four pieces

```
S0 (smoke env passthrough)        ← independent; lands first
   │
   ├──────────────► tier1 now exercises the fast path
   │
S1 (quadSetupTextures)            ← independent of B0/B1; gated on Tracy measurement
   │
B0 (VS-side diffuse, env-gated)   ← depends on S0 (regression coverage)
   │
   ├──────────────► visual A/B clean OR divergence inventoried
   │
B1 (16 B record)                  ← depends on B0 visual-clean
```

S0 and S1 can run in either order. B0 must precede B1. B1 should not land until S0 is committed (otherwise the smoke runner does not regress-cover the path B1 actually mutates).

---

## Open questions (resolve before any code)

1. **Point lights and lightning glow.** The Phase B preview note in [`../specs/2026-04-29-m2-thin-record-cpu-reduction.md`](../specs/2026-04-29-m2-thin-record-cpu-reduction.md) §"Phase B Preview" describes a per-frame point-light SSBO. Most MC2 missions have zero point lights most of the time; lightning is event-driven. **Question:** does B0's pure diffuse-from-normal visually reproduce the existing per-corner CPU lighting in the presence of an active lightning strike or weapon flash? If not, B0's visual gate fails and B0 expands to also bind the point-light SSBO. Resolve by booting `mc2_24` (storms) at Wolfman zoom with `MC2_PATCHSTREAM_GPU_LIGHTING=1` and watching the lightning frames before declaring B0 visual-clean.

2. **Fog tint.** `gos_terrain.frag` line 491 (`mix(0.35, 1.20, diffuse)`) is the diffuse range. Fog tint is applied separately in the FS. **Question:** does the current CPU `lightRGB` *also* encode any fog/atmospheric tint that the FS will double-apply if we drop CPU encoding? The terrain-shader-input-map exploration suggests no, but verify by `grep -n fog mclib/quad.cpp` and reading the lighting-write sites at quad.cpp:1047/1098/1201/1252/1355/1406/1509/1560 before B0 lands.

3. **`SELECTION_COLOR` value.** The exact RGB used by the legacy selection highlight needs to be hardcoded into the thin VS as a `const vec3`. Locate the C++ definition (search `SELECTION_COLOR` in `mclib`) and copy the channel values; do not paraphrase.

4. **`terrainLightDir.w` semantics.** The uniform is `vec4`; the `.w` component is currently unused on the FS side at line 430. **Question:** is `.w` reserved for a future light-intensity scalar or is it pure padding? If reserved, B0 should multiply `lit *= terrainLightDir.w` (with a documented default of 1.0) to future-proof. If pad, ignore. Confirm by reading `gos_SetTerrainLightDir` in the `code/` tree.

5. **B1 `selectionMask` bit layout vs registry.** Does the renderer-contract-registry owner accept the proposed `bits 0..1 used, 2..7 reserved` allocation, or does the registry mandate a different format (e.g. one byte per corner is enough but a future faction-tint feature wants 4 bits per channel)? Resolve in a 5-minute coordination message before B1 lands; do not block B0 on it.

---

## File-touch summary (exact paths)

| Slice | File | Lines | Action |
|---|---|---|---|
| S0 | `scripts/run_smoke.py` | 232–247 | commit working-tree diff |
| S1-A | `mclib/quad.cpp` | 58–389 | flip `s_shapeCEnabled` default |
| S1-B | `mclib/mapdata.h` | 86–122 | extend cache entry |
| S1-B | `mclib/quad.cpp` | 394 | replace addTriangle with bulk |
| S1-B | `mclib/txmmgr.{h,cpp}` | new method | `addTriangleBulk` |
| S1-B | `mclib/terrain.cpp` | 561 | prime triReservation |
| B0 | `shaders/gos_terrain_thin.vert` | 34, 125 | uniform decl + Color branch |
| B0 | `GameOS/gameos/gos_terrain_patch_stream.cpp` | ~66 | env gate + plumb mode |
| B0 | `GameOS/gameos/gameos_graphics.cpp` | 1625, 3053 | cache `gpuLightingMode` loc + upload |
| B1 | `GameOS/gameos/gos_terrain_patch_stream.h` | 103–123 | 16 B struct + constants |
| B1 | `GameOS/gameos/gos_terrain_patch_stream.cpp` | 1390–1404, signatures | drop lightRGB params, log selectionMask |
| B1 | `mclib/quad.cpp` | 1789–1846 | replace lambda + emit selectionMask |
| B1 | `shaders/gos_terrain_thin.vert` | 4–8, 119, 125 | struct shrink + selBits decode |

---

## References

- [`2026-04-29-track-a-render-headroom-status.md`](2026-04-29-track-a-render-headroom-status.md) — predecessor status snapshot
- [`../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md`](../specs/2026-04-29-quadsetuptextures-next-slice-handoff.md) — S1 full handoff
- [`../specs/2026-04-29-m2-thin-record-cpu-reduction.md`](../specs/2026-04-29-m2-thin-record-cpu-reduction.md) — M2 spec, "Phase B Preview"
- [`../specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §6 — Track A framing and outcome gate
- [`../specs/2026-04-26-render-contract-registry-design.md`](../specs/2026-04-26-render-contract-registry-design.md) — registry coordination surface
- [`../plans/2026-04-29-patchstream-m1g-thin-vs-draw.md`](../plans/2026-04-29-patchstream-m1g-thin-vs-draw.md) — `bindThinUniforms` plumbing
- [`../explorations/2026-04-27-terrain-shader-input-map.md`](2026-04-27-terrain-shader-input-map.md) — uniform inventory
- [`memory/terrain_lighting_range_ceiling.md`](../../../../../../../C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/terrain_lighting_range_ceiling.md) — `mix(0.35, 1.20, diffuse)` ceiling, load-bearing
