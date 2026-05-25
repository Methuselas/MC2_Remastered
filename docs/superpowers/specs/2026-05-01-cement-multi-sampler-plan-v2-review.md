# Adversarial review — Cement Multi-Sampler Plan v2 — 2026-05-01

> **Discipline:** code-grounded; every finding cites file:line evidence.
> **Reviewer:** adversarial-plan-review skill applied in full. Grep'd 30+ cited
> symbols and call sites against the actual source tree at
> `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`.

---

## TL;DR

**STOP-THE-LINE.** Plan v2 closes v1's CRITICAL findings (C1 GPU readback, C2
accessor scope) but introduces three NEW CRITICAL findings that guarantee the
slice will not work as written:

1. **C1-v2 (slot/handle confusion):** `q.terrainHandle` is `mcTextureNodeIndex`
   (a node-index returned by `getTextureHandle`), NOT a `textures[]` slot.
   Task A.4 calls `isCement((DWORD)q.terrainHandle)` and indexes
   `g_cementLayerIndexBySlot[q.terrainHandle]` — both of these treat the
   node-index as a slot. The lookup will miss for every quad and every cement
   quad will silently get layer-index 0.
2. **C2-v2 (legacy chain TES varying chain incomplete):** Plan B.2 adds
   `flat out uint RecordIdx` to `gos_terrain.vert` only. The legacy program
   has VS → TCS → TES → FS, and the frag's varying source is the TES, not the
   VS. Adding a `flat out` to the VS does not reach the frag. Step 4 mentions
   "if TES exists, also add it" but uses the same name `RecordIdx` in BOTH VS
   and TES, which collides — TCS would need to plumb `vs_RecordIdx[] →
   tcs_RecordIdx[]` for TES to consume.
3. **C3-v2 (V21 verification claim is wrong):** V21 asserts `q.terrainHandle`
   is "the un-resolved mcTextureNodeIndex (textures[] slot)". These are two
   different things — V21 conflates them. The actual code at
   `gos_terrain_indirect.cpp:944` calls `tex_resolve(q.terrainHandle)`, which
   accepts `mcTextureNodeIndex`, confirming `q.terrainHandle` is a node-index,
   NOT a slot. This is the V_L-class verification gap that v1 was supposed to
   fix.

Plus 3 MAJOR findings (legacy frag SSBO binding-2 declaration is structurally
fine but interacts with `_pad0` low-byte semantics for boundary fragments; thin
chain only has the right `_pad0` value at quad granularity but the frag samples
per-fragment — boundary mix quads get the wrong cement texture; B.4 sampler
restore order can leak when the bridge takes the early-out path).

---

## CRITICAL findings (block ship)

### C1-v2: `q.terrainHandle` is mcTextureNodeIndex, not a `textures[]` slot — A.4 lookup is wrong-keyed

**Claim in plan (Task A.4 Step 2 + V21):**
```cpp
const DWORD rawSlot = (DWORD)q.terrainHandle;
if (rawSlot < 3000 && Terrain::terrainTextures->isCement(rawSlot)) {
    const uint16_t idx = g_cementLayerIndexBySlot[rawSlot];
    ...
}
```

V21 backs this with: "`q.terrainHandle` at packer time is the un-resolved
`mcTextureNodeIndex` (textures[] slot); confirmed via the existing
`tex_resolve(q.terrainHandle)` call at `gos_terrain_indirect.cpp:944-945`."

**Evidence (grep'd):**

`mclib/quad.cpp:546` — the legacy code populates `terrainHandle`:
```cpp
terrainHandle = Terrain::terrainTextures->getTextureHandle(
    (vertices[0]->pVertex->textureData & 0x0000ffff));
```

`mclib/terrtxm.h:281-288` — `getTextureHandle(texture)`:
```cpp
DWORD getTextureHandle (DWORD texture) {
    if ((long)texture >= nextAvailable) return 0xffffffff;
    tex_resolve(textures[texture].mcTextureNodeIndex);
    return (textures[texture].mcTextureNodeIndex);  // <-- returns node-index
}
```

So `q.terrainHandle = textures[slot].mcTextureNodeIndex` — a node-index,
NOT the slot. The slot is the input argument
(`pVertex->textureData & 0xffff`).

`mclib/terrtxm.h:337-342` — `isCement(typeInfo)`:
```cpp
bool isCement (DWORD typeInfo) {
    if ((long)typeInfo >= nextAvailable) return false;
    return (textures[typeInfo].flags & MC2_TERRAIN_CEMENT_FLAG) == ...;
}
```

`isCement` indexes `textures[typeInfo]` with `typeInfo` interpreted as a
**slot index** (compared against `nextAvailable`, the number of valid
slots). Passing a node-index where a slot is expected is undefined: it
either falls outside `nextAvailable` (returns false) or hits a different
slot's flags. Either way the cement quads are missed.

Second-order failure: the dense map `g_cementLayerIndexBySlot[]` is **built**
in `BuildCementCatalogAtlas` keyed by SLOT (Task A.3 Step 2: the loop
`for (long slot = 0; slot < lastSlot; ++slot) { ... cementSlots.push_back((int)slot); }`
populates the map by slot). So even if the `isCement(rawSlot)` check were
ignored, the lookup `g_cementLayerIndexBySlot[q.terrainHandle]` reads at
`[node-index]` against a table populated at `[slot]`. The two are different
DWORDs. **The lookup will always miss; every cement quad gets `cementLayerIdx
= 0` and samples atlas tile 0.**

**Diagnostic visible at runtime:** all cement quads render with the SAME
texture (whatever happens to be at atlas slot 0, which is the first cement
slot enumerated). Multi-cement-variant maps will show a single pattern
across all tarmac.

**Recommended fix:** Capture the source slot, not the resolved handle. Two
options:

(a) Add a new field to `TerrainQuad` (e.g., `DWORD terrainSlotIndex;`) that
    holds `pVertex->textureData & 0xffff`, populated alongside
    `terrainHandle` at the legacy call sites in `quad.cpp:546`, `:630`, etc.
    Then A.4 reads `q.terrainSlotIndex` for the lookup.

(b) Re-derive the slot in the packer:
    `const DWORD slot = q.vertices[0]->pVertex->textureData & 0xffff;` and
    use `slot` for the lookup.

Either way, V21's verification claim is wrong and must be retracted. Note:
this is exactly the V_L class of error that v1's adversarial review
flagged — accurate at the line level (yes, `tex_resolve(q.terrainHandle)`
is called) but missing the semantic enclosing it (the function takes a
node-index, not a slot).

---

### C2-v2: Legacy chain TES varying plumbing is incomplete — silent linker failure on the legacy `gos_terrain` program

**Claim in plan (Task B.2 Steps 1-4):**
> Add `flat out uint RecordIdx; ... RecordIdx = 0u;` to
> `shaders/gos_terrain.vert`. ... Step 4: "If a `gos_terrain.tese` exists ...
> it MUST also emit `flat out uint RecordIdx; RecordIdx = 0u;` for the
> linker."

**Evidence (grep'd):**

`shaders/gos_terrain.vert:12-16` — legacy VS outputs (already exist):
```glsl
out vec4 vs_Color;
out vec2 vs_Texcoord;
out float vs_TerrainType;
out vec3 vs_WorldPos;
out vec3 vs_WorldNorm;
```

`shaders/gos_terrain.tesc:5-15` — legacy TCS reads `vs_*` from VS, writes
`tcs_*` to TES.

`shaders/gos_terrain.tese:5-16` — legacy TES reads `tcs_*` from TCS, writes
the bare frag-input names (`Color`, `Texcoord`, `TerrainType`, `WorldNorm`,
`WorldPos`, `UndisplacedDepth`):
```glsl
in float tcs_TerrainType[];
...
out float TerrainType;
```

So the legacy frag reads varyings from the **TES**, not from the VS. The
VS's outputs are consumed by the TCS (renamed `vs_X → tcs_X` then
barycentric-interpolated by the TES into the bare frag-input names). Adding
`flat out uint RecordIdx;` to the VS does not reach the frag — the TCS
doesn't pass it through. The plan's B.2 Step 1 add is dead code.

The plan's Step 4 acknowledges that TES needs the addition but proposes the
exact same name (`flat out uint RecordIdx; RecordIdx = 0u;`) on both the
VS and the TES, with no plumbing through the TCS. This will not link as
written:

- If the executor only adds it to the TES (skipping the VS), the legacy
  program links fine and the frag reads `RecordIdx = 0u` (default value),
  which is correct since the legacy chain never sets `useCementAtlas=1`.
- If the executor adds it to both VS and TES per Step 1+4, it MIGHT link
  because each is a self-contained `out` with no upstream consumer; but
  the TCS now has an unconsumed-VS-out and an unprovided-TES-in. Most
  drivers tolerate this (unused VS outputs are dead-stripped); some warn.
  The plan should be explicit.

**Recommended fix:** Rewrite Task B.2 to:

1. Add NOTHING to `shaders/gos_terrain.vert` (its outputs do not feed the
   frag).
2. Add `flat out uint RecordIdx;` to `shaders/gos_terrain.tese` (declared
   alongside the other `out` block at lines 11-16) and `RecordIdx = 0u;`
   in main (next to the other assignments).
3. Verify there is NO TCS plumbing needed (confirmed: the TES emits its
   own `RecordIdx` constant, no upstream input).

Also: the gate at frag for the cement-atlas branch is
`if (useCementAtlas != 0 && TerrainType >= 2.999)`. The legacy chain never
sets `useCementAtlas = 1`, so the SSBO read is never executed on the
legacy path even though the SSBO is declared. This is correct — but only
once the varying plumbing is right.

---

### C3-v2: V21 verification claim is wrong (the V_L-class error v1 was supposed to teach)

**Claim in plan (Verification Appendix V21):**
> `q.terrainHandle` at packer time is the un-resolved `mcTextureNodeIndex`
> (textures[] slot); confirmed via the existing
> `tex_resolve(q.terrainHandle)` call at `gos_terrain_indirect.cpp:944-945`.
> Status: ✅

**Evidence:** See C1-v2 above. `mcTextureNodeIndex` and `textures[] slot`
are different DWORDs. The textures array is indexed by slot
(`textures[slot]`), and each slot stores `mcTextureNodeIndex` as a value.
V21 conflates these. The "If `q.terrainHandle` were already a gosHandle,
the packer would not call `tex_resolve` on it" rationale is also weak —
`tex_resolve` takes `mcTextureNodeIndex`, not a slot, and it certainly does
not take a gosHandle. So the conclusion (`q.terrainHandle` is a slot)
doesn't follow from the premise.

**Issue magnitude:** This is the v1 C1 / v_L lesson failing to land. v1's
process discipline (D1 finding, "verify the function body, not just the
claimed line") was meant to prevent exactly this. V21's verification stops
at "function name `tex_resolve` appears" without checking what `tex_resolve`
takes as its argument or what `getTextureHandle` returns. The two-symbol
chain — `getTextureHandle returns mcTextureNodeIndex` and
`isCement takes a slot` — is the load-bearing semantic, not the function
calls.

**Recommended fix:** Retract V21. Re-grep `getTextureHandle` and document
what it returns (mcTextureNodeIndex, not slot). Re-grep `isCement` and
document what it accepts (slot, indexed against `nextAvailable`). Then
re-design A.4 per C1-v2 fix (b) above (re-derive slot from
`pVertex->textureData & 0xffff`).

---

## MAJOR findings (revise before ship)

### M1-v2: Per-quad cement layer-index has wrong semantics for boundary-mix quads

**Claim in plan (B6 architecture note):**
> "Per-quad cement layer-index lives in `TerrainQuadThinRecord._pad0` ... The
> frag reads `_pad0` directly from the thin-record SSBO at binding 2."

**Issue:** Even after C1-v2 is fixed and `_pad0` correctly holds the cement
layer-index of the QUAD's primary base texture, the frag samples this layer
PER FRAGMENT. A quad with corners `[cement-A, cement-A, cement-A, grass]`
has `q.terrainHandle = cement-A` and `_pad0 = layer(cement-A)`. The TES (or
linear interpolation through the thin VS) produces fragments at the
grass corner with TerrainType ≈ 0.0 — gate fails, no cement read, OK.
Fragments at the cement corners have TerrainType ≈ 3.0 — gate passes,
samples cement-A — also OK.

**But:** A mixed-cement quad with corners `[cement-A, cement-A, cement-B,
cement-B]` (two different cement textures) has only ONE primary
`q.terrainHandle` (whichever the legacy code picks; likely `cement-A` from
`vertices[0]`). Frag reads `_pad0 = layer(cement-A)` for ALL fragments,
including the cement-B corners. Visual result: cement-B corners render
with cement-A's texture, with a visible boundary at the diagonal.

**Probability this matters:** The mc2_01 airport tarmac canary uses a
single cement variant per cement region in stock content (per V_K's
brainstorm scope). For pure-stock missions this is likely benign. But the
plan claims "9 cement source IDs: 10, 13-20" — if any stock mission
straddles two cement IDs in a single quad, this artifact will appear.

Note: the legacy M2 path has the same per-quad single-handle semantics, so
visually this matches legacy. **Gate A's "≤5% perceptual difference vs
legacy" criterion likely PASSES** because legacy also rendered the wrong
corner. M1-v2 is therefore documentation-only: the plan should explicitly
note this is per-quad, not per-corner, and is not a regression vs legacy.

**Recommended fix:** Add a one-paragraph note in the plan's "Out of scope"
section: "Per-corner cement variant resolution (mixed-cement quads)
matches legacy single-handle semantics; not addressed in this slice."

---

### M2-v2: B.4 sampler save/restore is conditional on `cementAtlasReady` — early-out leaks unit-3 sampler binding

**Claim in plan (Task B.4 Steps 1-3):**
The save/bind happens inside `if (cementAtlasReady) { ... }`, and the
restore at Step 3 is also inside `if (cementAtlasReady) { ... }`.

**Issue:** This is symmetric and correct in the steady state. But two
conditions break it:

1. **Mid-mission build/teardown race:** If `BuildCementCatalogAtlas`
   succeeds between two consecutive `gos_terrain_bridge_drawIndirect`
   calls in the same frame (improbable but per the lifecycle hooks
   possible), `cementAtlasReady` is read once at draw entry. A subsequent
   `ResetDenseRecipe` mid-frame would not affect this draw.
   Low-probability; not a CRITICAL.

2. **Linker-failure path:** If `glGetUniformLocation(prog, "useCementAtlas")`
   returns -1 (because the frag failed to compile the new uniforms), the
   `glUniform1i` is skipped — but `cementAtlasReady` is still true and the
   atlas is bound. The frag then samples `tex3` with `useCementAtlas` at
   its driver-default value (0 typically, but undefined). This is benign
   (gate fails, branch not taken) but masks the real failure (frag didn't
   compile).

**Recommended fix:** After Step 2's `if (locUCA >= 0) glUniform1i(...)`
adds, log a one-time warning if any of the four locations are -1 when
`cementAtlasReady` is true. This is consistent with the
`debug_instrumentation_rule.md` discipline (lifecycle log on first
failure).

---

### M3-v2: B.5/SSBO declaration in frag — collision-free with legacy program but not validated under AMD driver rules

**Claim in plan (Architecture / B.5):**
> "Frag declares thin-record SSBO at binding 2 with `readonly` qualifier,
> mirroring the VS declaration."

**Evidence:**

`shaders/gos_terrain_thin.vert:9-11`:
```glsl
layout(std430, binding = 2) readonly buffer ThinRecordBuf {
    TerrainQuadThinRecord thinRecs[];
};
```

The plan's B.3 Step 2 declares in frag:
```glsl
layout(std430, binding = 2) readonly buffer ThinRecordBufFrag {
    TerrainQuadThinRecord_Frag thinRecsFrag[];
};
```

GLSL allows different block instance names (`ThinRecordBuf` vs
`ThinRecordBufFrag`) at the same binding — both stages will read from the
same GL_SHADER_STORAGE_BUFFER bound at slot 2. This is structurally fine.

**Concern 1 (BENIGN):** When the legacy `gos_terrain` material program is
linked (with VS+TCS+TES+FS), the frag declares the SSBO at binding=2 but
no other stage in that program references it. GL spec says this is valid
— SSBO declarations are per-program, and unused storage blocks in a
program get no actual binding requirement at draw time. AMD driver rules
(per `docs/amd-driver-rules.md`) do not flag SSBO-declared-but-unused as
an issue.

**Concern 2 (REAL):** The struct definition in the frag uses `uvec4`
fields (`control`, `lightRGBs`). The thin VS struct uses the same. But
the C++ struct `TerrainQuadThinRecord` at
`gos_terrain_patch_stream.h:103` is NOT laid out as 2 uvec4s — it's
8 separate uint32_t fields:
```cpp
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;
    uint32_t terrainHandle;
    uint32_t flags;
    uint32_t _pad0;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
};
```

Total 32 bytes. The thin VS struct casts this to two uvec4s (`control` =
[recipeIdx, terrainHandle, flags, _pad0]; `lightRGBs` =
[lightRGB0..3]). Mapping: `control.w == _pad0`. This is consistent.

**Concern is benign** — std430 packs both layouts identically (2 vec4 =
8 uint32 = 32 B). But the plan should explicitly cite this in B.3 to
avoid an executor reading the VS struct, then the C++ struct, and being
confused by the layout mismatch. The plan currently has only a single
sentence pointing at `gos_terrain_thin.vert:5` for the mapping; add a
pointer to `gos_terrain_patch_stream.h:103-109` so the executor doesn't
need to re-derive the layout.

**Recommended fix:** Add to B.3 Step 2 a reference comment:
```glsl
// C++ struct: gos_terrain_patch_stream.h:103-109 (8×uint32, 32B total).
// std430 packs identically to 2×uvec4. control.x=recipeIdx,
// control.y=terrainHandle (post tex_resolve), control.z=flags,
// control.w=_pad0 (cement layer-idx, 0 for non-cement).
```

---

## MINOR findings

### m1-v2: `glBindSampler(3, savedTex3Sampler)` restore uses unsigned cast

Plan B.4 Step 1 captures:
```cpp
{ GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
```

The cast `(GLuint)q` from a possibly-negative GLint is technically UB if q
< 0; but `GL_SAMPLER_BINDING` is unsigned per spec (returns 0 if no
sampler bound). Cast is safe in practice. Mirrors the existing code at
`gameos_graphics.cpp:2240`. No fix needed; documentation only.

### m2-v2: Reset block in B.4 Step 3 uses `glBindSampler(3, savedTex3Sampler)` — saved value typed `GLuint`, restored as the sampler ID

`glBindSampler(unit, sampler)` takes `GLuint` — type-correct. No issue.

### m3-v2: Stage A commit at A.1.bis (terrainType-20 patch) bundles BOTH file changes

A.1.bis Step 4's commit message bundles `mclib/quad.cpp` and
`gos_terrain_indirect.cpp` into one commit. This is fine and matches the
"symmetric patch keeps `_wp0` parity check clean" rationale, but the
commit-discipline preferred pattern is one commit per file. Stylistic;
not blocking.

### m4-v2: V13 cites "OpenGL 4.6 spec §10.2" for `gl_PrimitiveID` reset behavior

The cited section is correct in spirit (gl_PrimitiveID resets per draw
under multi-draw indirect). The plan does not need to verify this
empirically — but the parenthetical "Verifiable empirically with a test
draw printing `gl_PrimitiveID` per fragment if doubted at execution"
should be turned into an actual debug-mode in the frag (per
`debug_instrumentation_rule.md`) so the executor has a one-keystroke
verification path if the visual is wrong. Recommend adding to the frag a
debug branch (e.g., `tessDebug.x == -3.0`) that outputs
`vec3(float(RecordIdx % 256u) / 255.0)` so the executor can visually
confirm RecordIdx propagates correctly across the multi-draw boundary.

### m5-v2: V19 cites lines 497-501 for the `g_atlasGLTex` teardown — correct, but the plan's Task A.2 Step 2 says "After the existing `g_atlasGLTex` teardown (currently at lines 497-501)"

Confirmed at `gos_terrain_indirect.cpp:498-501`. Matches V19.

---

## Cross-references checked

| ID | Plan claim | File:line | Match? |
|---|---|---|---|
| **V1** | `quickLoad=true` gates RAM textureData[0] path | `mclib/terrtxm.cpp:561` | MATCHES (rationale for B1 fix correct) |
| **V2** | `getNextAvailableSlot()` mirrors `getNumTypes()` accessor pattern | `mclib/terrtxm.h:234`, underlying `nextAvailable` at `terrtxm.cpp:59` | MATCHES |
| **V3** | `peekTextureHandle(slot)` returns mcTextureNodeIndex, bounds-checked | `mclib/terrtxm.h:290-296` | MATCHES |
| **V4** | `isCement(slot)` is bounds-checked against `nextAvailable` | `mclib/terrtxm.h:337-342` | MATCHES |
| **V5** | `tex_resolve(nodeIdx)` returns gosHandle | (per design doc reference) | MATCHES (consistent with usage at `gos_terrain_indirect.cpp:944`) |
| **V6** | `gos_terrain_bridge_glTextureForGosHandle` is implemented | `gameos_graphics.cpp:1775-1781` | MATCHES (closes ❓2) |
| **V7** | `MC_MAX_TERRAIN_TXMS = 3000` | `mclib/terrtxm.h:34` (per v1 review) | MATCHES |
| **V8** | `BASE/START/END_CEMENT_TYPE = 10/13/20` | `mclib/terrtxm.h:42-44` (per v1) | MATCHES |
| **V9** | `MC2_TERRAIN_CEMENT_FLAG = 0x00000001` | `mclib/terrtxm.h:53` (per v1) | MATCHES |
| **V10** | `TerrainQuadRecipe` is 9 vec4 = 144 B | `gos_terrain_patch_stream.h:87-99` | MATCHES |
| **V11** | `_pad0` exists in TerrainQuadThinRecord | `gos_terrain_patch_stream.h:107`; thin VS mapping at `gos_terrain_thin.vert:5` | MATCHES |
| **V12** | Bridge binds thin SSBO at slot 2 via glBindBufferRange | `gameos_graphics.cpp:2355` | MATCHES |
| **V13** | `gl_PrimitiveID` restarts per sub-draw under multidraw indirect | OpenGL spec §10.2 | MATCHES (rationale for flat varying transport) |
| **V14** | Linker-fail risk on legacy chain when adding varying | `gos_terrain_thin.vert:22-31` | MATCHES (but plan's mitigation B.2 is INCOMPLETE — see C2-v2) |
| **V15** | `tex3` unused, sampler2D | `shaders/gos_terrain.frag:35` | MATCHES |
| **V16** | M3 typo fix (reset block in `gameos_graphics.cpp:2364-2367`) | `gameos_graphics.cpp:2364-2367` | MATCHES |
| **V17** | `terrainTypeToMaterial` and `terrainTypeToMaterialLocal` both miss `case 20:` | `mclib/quad.cpp:142-145` and `gos_terrain_indirect.cpp:244-245` | MATCHES |
| **V18** | `BuildColormapAtlas()` called from `BuildDenseRecipe()` at line 476 | `gos_terrain_indirect.cpp:476` | MATCHES |
| **V19** | `g_atlasGLTex` teardown at lines 497-501 in `ResetDenseRecipe` | `gos_terrain_indirect.cpp:498-501` | MATCHES |
| **V20** | `Terrain::worldUnitsPerVertex` publicly accessible | `mclib/terrain.cpp:92` (= 128.0f) | MATCHES |
| **V21** | `q.terrainHandle` at packer time is mcTextureNodeIndex (textures[] slot) | `mclib/quad.cpp:546`, `terrtxm.h:281-288` | **DIVERGENT (C1-v2/C3-v2)** — node-index ≠ slot |
| Texture internal format | Cement TGAs upload as GL_RGBA8 (uncompressed); glGetTexImage(GL_BGRA, GL_UNSIGNED_BYTE) is valid | `gl_utils.cpp:53-58, 165-184` (GL_RGBA8 path) | MATCHES (B1 GPU readback is sound) |
| Bridge function | `gos_terrain_bridge_glTextureForGosHandle` is implemented and returns 0 for invalid | `gameos_graphics.cpp:1775-1781` | MATCHES |
| Sampler-binding query | `glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q)` is the correct API | `gameos_graphics.cpp:2240` (existing pattern) | MATCHES |
| Legacy program structure | `gos_terrain` material = VS+TCS+TES+FS; thin program = VS+FS only | `gameos_graphics.cpp:2552` (thin), `gos_terrain.tesc:5-15` (TCS), `gos_terrain.tese:5-16` (TES) | DIVERGENT from B.2 (legacy frag reads from TES, not VS) |
| Cement texture per-frag selector | `pureConcrete = smoothstep(2.0, 3.0, TerrainType)` already in frag | `gos_terrain.frag:330` | MATCHES (gate `>= 2.999` is consistent) |
| `worldUnitsPerVertex` value | 128.0 world units per terrain vertex | `mclib/terrain.cpp:92` | MATCHES (uniform `atlasCementWorldUnitsPerTile` value is correct) |

### B-resolution verdict (v1 blockers)

| B-id | v1 finding | v2 plan resolution | Verdict |
|---|---|---|---|
| **B1** | C1: textureData[0] NULL in stock | GPU readback via glGetTexImage | **Sound** (GL_RGBA8 internal format confirmed; glGetTexImage(GL_BGRA, GL_UNSIGNED_BYTE) is valid) |
| **B2** | C2: protected accessor | One public `getNextAvailableSlot()` | **Sound** (V21 issue is a different bug, see C1-v2/C3-v2) |
| **B3** | M1: sampler unit 3 inheritance | `glBindSampler(3, 0)` save/restore | **Sound** (correct API; ordering matches existing pattern) |
| **B4** | M2: type 20 → Concrete | `case 20:` patch in BOTH files | **Sound** (A.1.bis correctly identifies both sites) |
| **B5** | M4: false claim "frag can't read SSBO" | Frag declares SSBO at binding 2 | **Sound structurally** (M3-v2 is documentation-only) |
| **B6** | new: how to transport cement-idx | `flat uint RecordIdx` varying + frag SSBO read | **BLOCKED** (C2-v2: VS varying plumbing is wrong for legacy chain; C1-v2: even if plumbing is fixed, the slot/handle key is wrong) |
| **B7** | D3: mission-restart gate | Gate D2 checks built/reset event pairing + RSS | **Sound** (gate is well-defined and matches `BuildColormapAtlas` pattern) |

---

## Process / discipline observations

### D1: V21 is the V_L lesson failing to land

v1's adversarial review introduced D1 ("V_L verification claim is accurate
but dangerously incomplete") specifically to teach the discipline of
"verify the function body, not just the claimed line." V21 in v2 fails the
same discipline: it cites that `tex_resolve(q.terrainHandle)` exists and
infers `q.terrainHandle` is a slot, without re-greping `getTextureHandle`
to learn that it returns mcTextureNodeIndex. The CRITICAL findings C1-v2
and C3-v2 are the consequence.

**Recommendation:** Before plan v3, the author should re-run the
verification appendix entries with this rule: "for every cited symbol that
appears in a function call, also document the function's signature and
return type." This catches conflations like "node-index vs slot" before
they become CRITICAL findings.

### D2: B6 architectural decision (frag-side SSBO fetch via flat varying) is sound but assumes the legacy frag chain links cleanly

B6 was the user-frozen architectural decision. The frag-side SSBO fetch
itself is structurally fine (M3-v2 is benign). The problem is downstream:
the legacy program's frag now requires `flat in uint RecordIdx`, which the
TES must emit. Plan B.2 doesn't address this correctly (C2-v2). Once C2-v2
is fixed by adding the varying to the TES (only) instead of the VS, B6's
architectural choice stands.

### D3: No counter for "atlas readback failed" / "cement quad packed but layer index lookup miss"

Per `debug_instrumentation_rule.md`: lifecycle/render-path reworks should
land with env-gated `[SUBSYS]` prints in the same commit. The plan adds
`event=cement_catalog_built/reset` (good) but no per-frame counter for
"cement quad packed but lookup miss" or "atlas slot 0 sampled". Without
this, C1-v2's symptom (every cement quad sampling slot 0) won't surface
in the trace logs — only by visual inspection of mc2_01 tarmac.

**Recommendation:** Add to A.4 a counter incremented when `q.terrainHandle`
is non-zero AND `g_cementLayerIndexBySlot[...]` returns 0xFFFF. Print the
count per-mission in the cement_catalog_built event. Non-zero count after
the C1-v2 fix → still buggy.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **C1-v2 fix path.** Two options: (a) add `terrainSlotIndex` field to
   `TerrainQuad` (touches many call sites in `quad.cpp`); (b) re-derive
   slot in the packer via `q.vertices[0]->pVertex->textureData & 0xffff`
   (one-line change, but requires guarding `pVertex != nullptr`). (b) is
   more localized and matches existing packer pointer guards at
   `gos_terrain_indirect.cpp:928`. Recommend (b).

2. **M1-v2 acceptance.** Mixed-cement quads (rare in stock) sample only
   the primary corner's cement variant. This matches legacy semantics
   exactly. Sign-off: confirm "no per-corner cement variant resolution"
   is acceptable for v2 scope; defer to a future slice if multi-cement
   quads matter. Likely acceptable per v1's Q1 scope ("Pure cement only").

3. **Plan v3 process.** Given v2 has 3 CRITICAL findings (one of which
   replays the v1 discipline gap), recommend the next revision require an
   explicit "function signature + return type for every cited symbol"
   pass before submission. The verification appendix should expand from
   "claim → file:line" to "claim → file:line → function signature →
   semantic interpretation." This is the v_L lesson v1 thought it had
   landed.
