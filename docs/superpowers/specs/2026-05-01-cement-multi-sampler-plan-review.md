# Adversarial review — Cement Multi-Sampler Plan v1 — 2026-05-01

> **Discipline:** code-grounded; every finding cites file:line evidence.
> **Reviewer:** adversarial-plan-review skill applied in full. Grep'd 28 cited symbols,
> 6 call sites, 7 load-bearing constraints. See "Cross-references checked" below.

---

## TL;DR

**STOP-THE-LINE.** The plan has one CRITICAL finding that guarantees a silent no-op on every
stock gameplay session (`textureData[0]` is NULL in quickLoad mode — the plan's atlas build
function detects this and bails out, leaving N=0 cement slots, no atlas built, no cement
rendered). Secondary CRITICAL: `types` and `textureData` are `protected:` members — the plan's
`BuildCementCatalogAtlas()` code as written will not compile from `gos_terrain_indirect.cpp`.
Additionally, two MAJOR findings (sampler override risk for unit 3, `END_CEMENT_TYPE=20` type
mapping gap) must be addressed before the plan can land without silent visual regressions.

---

## CRITICAL findings (block ship)

### C1: `textureData[0]` is NULL in stock gameplay — atlas never built

**Claim in plan (V_L):**
> "Confirmed: `tileRAMHeap->Malloc(mipSize*mipSize*sizeof(DWORD))` stores into
> `types[i].textureData[j]`. Base types (mip 0) at index 0."
> (Verification Appendix V_L, plan line 763)

**Evidence (grep'd):**

`mclib/terrtxm.cpp:560-584` — The RAM allocation and `textureData[j] = ourRAM` assignment are
**inside** `if (InEditor || !quickLoad) { ... }`. In stock gameplay the engine sets
`quickLoad = true` (terrtxm.cpp:75) for every non-editor run. The block is never entered.

`mclib/terrtxm.cpp:241-244` — `textureData` is allocated as a `MemoryPtr` array and then
`memset` to zero: `memset(types[i].textureData, 0, sizeof(MemoryPtr) * MC_MAX_MIP_LEVELS)`.
`textureData[0]` remains NULL for every type entry in a stock session.

`BuildCementCatalogAtlas()` in the plan (Task A.2) contains:

```cpp
if (tt->types[typeIdx].textureData[0] == nullptr) continue;  // tileRAMHeap pixel data
```

This NULL guard correctly skips every entry — all of them — so `N=0` and the function returns:

```
[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0
```

The atlas is never built. Cement quads remain unrendered on the indirect path on every stock
gameplay session (as opposed to editor sessions).

**Issue:** V_L's verification is technically accurate (lines 576-581 do exist), but it cited
the lines WITHOUT reading the enclosing `if (InEditor || !quickLoad)` gate. The plan's entire
pixel-data approach is dead on the only target platform. The cement atlas will silently never
be built.

**Recommended fix:** The plan must choose an alternative pixel-data source. Two viable paths:

- **(a) GPU readback:** After the GoS texture is resident in GPU memory, readback via
  `glGetTexImage` with the resolved `gosHandle`. This is the only way to get pixel data for
  cement tiles in stock gameplay. It is slower (one glGetTexImage per cement tile at mission
  load, not per-frame) but acceptable given the plan's scope (9-30 tiles, one-time cost).
  The bridge already has the `mcTextureNodeIndex` → `gosHandle` resolve path via `tex_resolve`.

- **(b) Re-read from disk:** Call `tgaFile.open` / `loadTGATexture` directly in
  `BuildCementCatalogAtlas`, bypassing `tileRAMHeap`. Requires reconstructing the same path
  logic as `initTexture` (mipPath construction at terrtxm.cpp:536-553). More code but
  no GPU readback overhead.

Either path requires removing the `textureData[0]` dependency entirely.

---

### C2: `types` and `textureData` are `protected:` — compile error from `gos_terrain_indirect.cpp`

**Claim in plan (open ❓1, ❓2 acknowledged):**
> "types[] is a **protected** member of TerrainTextures. Direct access is not possible from
> outside the class." (plan line 780)

The plan acknowledges the issue but proposes a "public accessor" as the resolution in the open
item. The problem is more than a resolution path — the plan's complete Task A.2 code (lines
194-296 of the plan) is written using **direct member access** (`tt->types[i].baseTXMIndex`,
`tt->types[i].textureData[0]`) that will **not compile** as written.

**Evidence (grep'd):**

`mclib/terrtxm.h:111-119` — `TerrainTextures` class definition:
```cpp
class TerrainTextures {
    //Data Members
    //-------------
    protected:
        static long     numTxms;
        TerrainTXM      *textures;
        static long     nextAvailable;
        long            firstTransition;
        long            numTypes;
        MC_TerrainTypePtr types;          // <-- protected
        ...
        UserHeapPtr     tileRAMHeap;      // <-- protected
```

`mclib/terrtxm.h:143` — only a subset of methods are `public:`. There is no
friend declaration in the class for `gos_terrain_indirect.cpp`.

The plan's executor would hit a wall of compile errors on Task A.2 Step 1. The open ❓1/❓2
items call for adding public accessors, which is the right fix, but the plan as written
provides non-compilable code for the primary implementation task and leaves the resolution
as "executor must resolve." For an architectural-endpoint plan this is insufficient.

**Recommended fix:** The plan must define the exact accessor signatures that will be added to
`TerrainTextures` and rewrite Task A.2 to use them. The minimal set:

```cpp
// New public accessors in terrtxm.h:
long getBaseTXMIndexForType(int i) const {
    if (i < 0 || i >= numTypes) return -1;
    return types[i].baseTXMIndex;
}
// getTypePixelData: returns NULL in quickLoad/gameplay mode (see C1).
// Executor must NOT use this without resolving C1 first.
MemoryPtr getTypePixelDataForSlot(int i, int mipLevel) const {
    if (i < 0 || i >= numTypes) return nullptr;
    if (!types[i].textureData) return nullptr;
    return types[i].textureData[mipLevel];
}
```

C1 and C2 are linked — the right fix for C1 also makes `getTypePixelDataForSlot` moot (since
it returns NULL). The executor needs to resolve both together.

---

## MAJOR findings (revise before ship)

### M1: Unit-3 sampler object inheritance — `GL_REPEAT` may be overridden

**Claim in plan (Task A.2 Step 1 / Task B.3 Step 2):**
The cement atlas is uploaded with:
```cpp
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
```
And the plan expects tiling to work for the per-tile UV formula.

**Issue:** The indirect bridge at `gameos_graphics.cpp:2285` binds
`s_indirectTerrainSampler` (CLAMP_TO_EDGE) **only to unit 0**:

```cpp
glBindSampler(0, s_indirectTerrainSampler);  // gameos_graphics.cpp:2285
```

No sampler is bound to unit 3 (`glBindSampler(3, ...)` is absent from the plan). GL sampler
objects override `glTexParameteri` when a sampler is bound to the unit. If any prior rendering
operation bound a CLAMP sampler to unit 3, the cement atlas tiling will collapse to texture-edge
color. While the current code shows no sampler bound to unit 3 (`grep GL_TEXTURE3` finds no
hits), this is a fragile assumption — any future feature that touches unit 3 with a sampler
object will silently break cement tiling.

**Evidence:** `gameos_graphics.cpp:2276-2285` — sampler setup binds to unit 0 only.
`gameos_graphics.cpp:grep TEXTURE3` — zero hits (safe today, not guaranteed tomorrow).

**Recommended fix:** Add `glBindSampler(3, 0)` before the cement atlas bind to guarantee no
sampler object overrides the texture's wrap state. Or create a dedicated cement sampler with
`GL_REPEAT` and bind it to unit 3, following the pattern of `s_indirectTerrainSampler` for
unit 0.

---

### M2: `END_CEMENT_TYPE = 20` maps to Rock (not Concrete) in `terrainTypeToMaterialLocal`

**Claim in plan / brainstorm:** "9 cement source IDs: 10, 13-20." The textures.fit has cement
types 10, 13, 14, 15, 16, 17, 18, 19, 20. But:

**Evidence (grep'd):**

`GameOS/gameos/gos_terrain_indirect.cpp:240-248`:
```cpp
case 10: case 13: case 14: case 15: case 16:
case 17: case 18: case 19:                    return 3; // Concrete
default:                                      return 0; // Rock
```

Type 20 (`END_CEMENT_TYPE`) is **absent from the cement cases** and falls through to
`default: return 0 // Rock`. So when any vertex has `terrainType=20`, the thin VS writes
`TerrainType=0.0` (Rock) into the varying. The frag's cement branch gates on `TerrainType >= 3.0`
— type-20 quads will never enter the cement atlas path regardless of `isCement()` returning
true for their texture slot.

The plan's ❓4 item notes this for `quad.cpp`'s `terrainTypeToMaterial`, but the more immediate
issue is in `terrainTypeToMaterialLocal` in the same file that the cement atlas uses for
`TerrainType` encoding. If any stock mission has cement quads with terrainType=20, they will
render as "Rock" material (no concrete look) even before the cement atlas is consulted.

**Recommended fix:** Either add `case 20:` to the Concrete group in `terrainTypeToMaterialLocal`
and simultaneously verify `quad.cpp`'s `terrainTypeToMaterial` has the same mapping (to keep
the parity check clean), or confirm from `textures.fit` that type 20 is not a pure-cement base
type on any stock mission (if it's only a transition type, the issue may be benign).

---

### M3: Bridge comment cites wrong file for reset block

**Claim in plan (Task B.3 Step 3):**
> "After the draw call (after `glMultiDrawArraysIndirect`, at the reset block currently at
> `gos_terrain_indirect.cpp:2361-2367`)..."

**Evidence:**

`grep gos_terrain_bridge_drawIndirect` → `GameOS/gameos/gameos_graphics.cpp:2217`.
The reset block at lines 2361-2367 is in `gameos_graphics.cpp`, not `gos_terrain_indirect.cpp`.

This is a minor file-path error in the plan, but in the context of two files with similar
names, an executor navigating to the wrong file would miss the insertion site.

**Recommended fix:** Correct Task B.3 Step 3 to reference `gameos_graphics.cpp:2361-2367`.

---

### M4: `_pad0` / `control.w` access — plan mixes SSBO struct definition vs. plan correctness

**Claim in plan architecture note:**
> "The frag cannot read SSBOs (it runs after vertex shading)."

This is **technically inaccurate** — GL 4.3 allows fragment shaders to declare SSBOs. The frag
can declare and read the thin-record SSBO at binding 2, as the thin VS already does at binding 2.

**Evidence:**
`shaders/gos_terrain_thin.vert:9` — `layout(std430, binding = 2) readonly buffer ThinRecordBuf`.
The same declaration is valid GLSL in a fragment shader.

**Issue magnitude:** MAJOR only in the sense that the plan's architectural decision (bias
encoding vs. direct SSBO read) is based on a false premise. If the frag can read the SSBO
directly via a `recordIdx` derived from `gl_PrimitiveID` (which increments per-triangle in a
non-indexed draw), then `_pad0` can be read directly without any TerrainType bias encoding,
avoiding the precision concerns and the `floor(TerrainType) == 3.0` gate fragility entirely.

The plan's chosen approach (TerrainType bias) still works on the thin path (no TES, no
interpolation, exact discrete values per quad), but the plan is incorrect in stating that
a direct SSBO read is architecturally impossible. This is a false constraint driving
unnecessary encoding complexity.

**Recommended fix:** The plan should acknowledge that the frag COULD read the SSBO, but
the TerrainType bias is preferred to avoid adding a new SSBO declaration to the frag's
interface (which could affect the frag's binding-point contract). This is a valid reason
to prefer the bias approach — but it should be stated accurately.

---

### M5: Transition textures — `textureData[0]` is NULL even in editor mode for transition slots

**Claim in plan (Task A.2 architecture note):**
> "Walk base types only for the pixel data source (tileRAMHeap-backed textureData[0]).
> Transition textures (slots >= firstTransition) have their pixel data baked at
> createTransition() time and also stored in textureData via tileRAMHeap..."

**Evidence:**

`mclib/terrtxm.cpp:1237` — `createTransition` calls `combineTxm` which reads
`types[priTypes[0]].textureData[kmp]`. This is only valid in editor/non-quickLoad mode
(same gate as initTexture). `createTransition` itself is not gated — but the `textureData`
values it reads will be NULL in quickLoad mode.

The plan already scopes "base types first" and defers transitions to "if runtime count
indicates they appear in practice" — but this comment suggests pixel data for transitions
exists in tileRAMHeap. It does not in stock gameplay.

This is a secondary consequence of C1: the entire `textureData` approach fails in quickLoad
mode regardless of whether base types or transition types are targeted.

---

## MINOR findings (nice-to-fix; don't block)

### m1: `atlasCementOneOverGridSide` vs `atlasCementTxmSizeOverAtlasSize` redundancy

Plan Task B.1 declares two uniforms that encode the same value:
```glsl
uniform float atlasCementOneOverGridSide;
uniform float atlasCementTxmSizeOverAtlasSize;  // 1.0/gridSide
```
Then the Note immediately says "both encode the same value (1/gridSide). Use a single
`atlasCementOneOverGridSide` uniform." Task B.1 should be revised to declare only
`atlasCementOneOverGridSide` and eliminate the redundant uniform before handing to executor.

### m2: Plan reference to `gos_terrain_indirect.cpp:2361-2367` (wrong file, see M3)

Same as M3 — a copy-paste of the wrong file path.

### m3: `gridSideF = 1.0 / atlasCementOneOverGridSide` — integer precision in GLSL

Task B.2 frag code:
```glsl
float gridSideF = 1.0 / atlasCementOneOverGridSide;
int   cCol = layerIdx % int(gridSideF + 0.5);
```
`1.0f / (1.0f / N)` in floating-point may not recover exactly N for large N due to rounding.
Pass `gridSide` as a `uniform int atlasCementGridSide` directly to avoid the round-trip.

### m4: `GL_REPEAT` wrap mode for cement atlas — cement tiles are a repeating texture

Plan sets `GL_REPEAT` on the cement atlas texture object and uses `fract(cTileUV)` in the frag
for per-tile UV. This is correct. But the plan doesn't verify that the sampler inheritance
issue (M1) won't cancel this. m4 is the positive-confirmation of M1 at MINOR severity: even
if M1 is fixed by adding `glBindSampler(3, 0)`, the executor should explicitly document in the
commit message that unit 3 relies on texture-object wrap state (no sampler object bound).

---

## Process / discipline observations

### D1: V_L verification claim is accurate but dangerously incomplete

V_L confirms the existence of `tileRAMHeap->Malloc` at lines 576-581 but does not cite
the enclosing `if (InEditor || !quickLoad)` gate at line 561. A verification entry that
cites a line without its enclosing conditional is a documentation discipline violation —
it reads as "this code runs" when in fact it never runs in production. The
`verify-then-write` discipline in CLAUDE.md requires reading the function body, not just
the claimed line. CRITICAL finding C1 was caused by this gap.

### D2: Open ❓ items for compile-blocking issues are insufficient

The plan's open ❓1 and ❓2 acknowledge `types` and `textureData` are protected, but list
them as "executor must resolve at implementation time" rather than blocking findings. Per the
adversarial-plan-review skill: compile-blocking issues are CRITICAL, not open items. The plan
author should have promoted ❓1/❓2 to CRITICAL findings and provided the resolution path
(accessor signatures + revised Task A.2 code) before handing to executor.

### D3: No per-mission teardown verification

The plan adds `ResetDenseRecipe()` teardown for the cement atlas (Task A.1 Step 2). But the
plan does not verify what happens on **mission restart without quitting** — i.e., the code path
where `ResetDenseRecipe()` is called, then `BuildDenseRecipe()` is called again for the second
mission. The pattern is mirrored from `BuildColormapAtlas`, which already handles this correctly
(glDeleteTextures then glGenTextures on next call). This is low-risk (the pattern is established)
but the plan should confirm `ResetDenseRecipe` is called on the restart path.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **C1 pixel-data source.** Two options: GPU readback (simple, one glGetTexImage per cement
   tile at mission load) or disk re-read (no GPU stall but code duplication from `initTexture`).
   Both work. GPU readback is simpler. Does the project accept a `glGetTexImage` call during
   mission load for 9-30 cement tiles?

2. **M2 type-20 resolution.** Is terrainType=20 (`END_CEMENT_TYPE`) an active pure-cement base
   type on any stock mission, or is it a legacy ID that never appears in practice? If it never
   appears, M2 is benign and the fix is just a doc comment. If it does appear, `case 20:` must
   be added to `terrainTypeToMaterialLocal` (and `quad.cpp`'s copy kept in sync).

3. **M4 SSBO-in-frag.** The plan chose TerrainType bias to avoid a SSBO declaration in the
   frag. Accept this decision explicitly or pivot to direct SSBO read? The bias approach is
   workable once C1 and C2 are resolved, but the rationale should be documented accurately
   (not "frag can't read SSBO").

---

## Cross-references checked

| Symbol / Claim | File:line | Plan claim | Match? |
|---|---|---|---|
| `quickLoad = true` in stock game | `mclib/terrtxm.cpp:75` | not cited | (C1 root cause) |
| `if (InEditor || !quickLoad)` gate on textureData | `mclib/terrtxm.cpp:561` | V_L omits gate | DIVERGENT (C1) |
| `types[typeNum].textureData[j] = ourRAM` | `mclib/terrtxm.cpp:581` | V_L cites 576-581 | accurate but incomplete |
| `memset(types[i].textureData, 0, ...)` | `mclib/terrtxm.cpp:244` | not cited | (C1 supporting) |
| `TerrainTextures::types` is `protected:` | `mclib/terrtxm.h:119` | ❓1 acknowledges | CONFIRMED (C2) |
| `TerrainTextures::tileRAMHeap` is `protected:` | `mclib/terrtxm.h:131` | ❓2 acknowledges | CONFIRMED (C2) |
| `getNumTypes()` public accessor | `mclib/terrtxm.h:234` | V_A ✅ | MATCHES |
| `isCement(DWORD)` public method | `mclib/terrtxm.h:337-342` | V_I ✅ | MATCHES |
| `MC_MAX_TERRAIN_TXMS = 3000` | `mclib/terrtxm.h:34` | V_W2 ✅ | MATCHES |
| `MC2_TERRAIN_CEMENT_FLAG = 0x00000001` | `mclib/terrtxm.h:53` | V_V ✅ | MATCHES |
| `BASE_CEMENT_TYPE=10, START=13, END=20` | `mclib/terrtxm.h:42-44` | V_K ✅ | MATCHES |
| `TERRAIN_TXM_SIZE = 64` (extern int) | `mclib/terrtxm.cpp:51` | V_F ✅ | MATCHES |
| `tileRAMHeap` deleted in `update()` | `mclib/terrtxm.cpp:1504` | V_N ✅ | MATCHES |
| `TerrainTextures::nextAvailable` static | `mclib/terrtxm.cpp:59` | V_J ✅ | MATCHES |
| `terrainTypeToMaterialLocal` type-20 → Rock | `gos_terrain_indirect.cpp:240-248` | V_D ⚠️ | M2 FINDING |
| `BuildColormapAtlas()` ends at line 421 | `gos_terrain_indirect.cpp:421` | plan line 301 | MATCHES (anonymous namespace closes line 421) |
| `BuildDenseRecipe()` at line 436 | `gos_terrain_indirect.cpp:436` | plan line 311 | MATCHES |
| `BuildColormapAtlas()` called at line 476 | `gos_terrain_indirect.cpp:476` | V_O ✅ | MATCHES |
| `ResetDenseRecipe()` at line 479 | `gos_terrain_indirect.cpp:479` | V_C ✅ | MATCHES |
| `tr._pad0 = 0u;` at line 991 | `gos_terrain_indirect.cpp:991` | V_P ✅ | MATCHES |
| `IsEnabled()` at lines 40-46 | `gos_terrain_indirect.cpp:40-46` | V_W ✅ | MATCHES |
| Bridge function `gos_terrain_bridge_drawIndirect` | `gameos_graphics.cpp:2217` | plan says 2287-2388 for body | MINOR: start is 2217, body 2229+ |
| Reset block `useAtlasColormap=0` | `gameos_graphics.cpp:2364-2367` | plan says `gos_terrain_indirect.cpp:2361` | WRONG FILE (M3) |
| `s_indirectTerrainSampler` CLAMP bound to unit 0 only | `gameos_graphics.cpp:2285` | not addressed for unit 3 | M1 FINDING |
| No sampler bound to unit 3 | `grep GL_TEXTURE3 → 0 hits` | not addressed | M1 FINDING |
| `tex3` declared `sampler2D` at frag:35 | `shaders/gos_terrain.frag:35` | V_X ✅ | MATCHES |
| `useAtlasColormap` at frag:57 | `shaders/gos_terrain.frag:57` | V_X, Stage B Task B.1 | MATCHES |
| `pureConcrete = smoothstep(2.0, 3.0, TerrainType)` | `shaders/gos_terrain.frag:330` | V_U ✅ | MATCHES |
| No SSBO declarations in `gos_terrain.frag` | `grep SSBO/binding/layout.*buffer → 0 hits` | plan says "frag cannot read SSBO" | INACCURATE premise (M4): frag has no SSBO today but could |
| Thin VS shader uses `makeProgram` (no TES) | `gameos_graphics.cpp:2552-2556` | plan assumes no TES on thin path | CONFIRMED CORRECT |
| `TerrainType` bias in thin VS line 149-151 | `shaders/gos_terrain_thin.vert:149-151` | V_H ✅ | MATCHES |
| `_pad0` in `TerrainQuadThinRecord` at gos_terrain_patch_stream.h:107 | confirmed `uint32_t _pad0;` | V_Z ✅ | MATCHES |
| `tr._pad0` read in thin VS as `tr.control.w & 0xFFu` | struct mapping: `control.w` = `_pad0` per thin VS line 5 | plan arch note | MATCHES |
| `getTextureHandle` returns `mcTextureNodeIndex` (not gosHandle) | `mclib/terrtxm.h:281-288` | V_Q ✅ | MATCHES |
| `terrainBindThinUniformsForPatchStream` does NOT set tex3 | `gameos_graphics.cpp:3608-3616` | V_S ✅ | MATCHES |
| `quad.cpp:436-441` pure-cement branch | `mclib/quad.cpp:436-441` (confirmed) | V_R ✅ | MATCHES |
