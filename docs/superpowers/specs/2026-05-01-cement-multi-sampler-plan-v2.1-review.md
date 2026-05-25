# Adversarial review — Cement Multi-Sampler Plan v2.1 — 2026-05-01

> **Discipline:** code-grounded; every finding cites file:line evidence.
> **Reviewer:** adversarial-plan-review skill applied in full. Re-greped
> v2.1's new claims (validity bit, nodeIdx-keyed map, TES varying, GL
> state save/restore) plus the second-order failure modes the dispatch
> prompt called out. Worktree at
> `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`.

---

## TL;DR

**STOP-THE-LINE (one new CRITICAL).** Plan v2.1 closes ALL three v2.0
CRITICALs (C1, C2, C3) and BOTH advisor CRITICALs (validity bit, ACTIVE_TEXTURE
restore) cleanly and with the right Tasks/Steps. The architecture pivot
(nodeIdx-keyed map, validity bit at `_pad0` bit 31, TES-only varying) is
correct.

But v2.1 introduces ONE new CRITICAL by re-using the SAME wrong upper
bound the texture-handle-cap memory note implies:

1. **C1-v21 (nodeIdx array undersized — silent cement miss):**
   `g_cementLayerIndexByNodeIdx[3000]` and the `if (nodeIdx < 3000u)`
   gates assume `mcTextureNodeIndex` is bounded by 3000. **It is not.**
   Node indices are allocated from `masterTextureNodes[MC_MAXTEXTURES]`,
   and `MC_MAXTEXTURES = 4096` ([`mclib/txmmgr.h:44`](mclib/txmmgr.h:44)).
   The 3000 cap is `MAX_MC2_GOS_TEXTURES` — a `currentUsedTextures` *usage
   counter*, not the array size. Any cement texture loaded with nodeIdx
   in [3000, 4096) is silently skipped (atlas-build) and silently
   unmapped (packer). Stock missions may not exercise the high range,
   but mod content / late mission load can — and the failure mode is
   identical to v2.0 C1 (cement quad samples colormap fallback). V21's
   "the V_L lesson failing twice" is repeating a third time, just with
   a different conflated symbol.

Plus 2 MAJOR (counter explodes by design; TES early-out paths skip the
new `RecordIdx = 0u;` assignment) and 4 MINOR (uvData scale, `_wp0`
parity drift, `truncated` event enforcement, frag SSBO read on legacy
path).

**Recommendation:** revise — fix the 3000 → 4096 sizing in one diff,
move the `RecordIdx = 0u;` to the top of TES `main()` so it precedes
both early-outs, then ship.

---

## CRITICAL findings (block ship)

### C1-v21: `g_cementLayerIndexByNodeIdx[3000]` is undersized — node-index range is 4096

**Claims in plan v2.1:**

- Task A.2 Step 1: `static uint16_t g_cementLayerIndexByNodeIdx[3000];`
  with comment "Sized `MC_MAX_TERRAIN_TXMS = 3000` (terrtxm.h:34) — same
  upper bound applies to nodeIdx (mcTextureManager nodes share that cap
  per memory note `texture_handle_cap.md`)."
- V7: `MC_MAX_TERRAIN_TXMS = 3000` — upper bound on both `nextAvailable`
  AND `mcTextureNodeIndex`.
- A.3 Step 2: `if (nodeIdx >= 3000u) continue;` (atlas build)
- A.4 Step 2: `if (nodeIdx < 3000u) { ... }` (packer lookup)

**Evidence (greped):**

`mclib/txmmgr.h:44` — `#define MC_MAXTEXTURES 4096`.

`mclib/txmmgr.cpp:206-211` — masterTextureNodes is sized
`MC_MAXTEXTURES`:
```cpp
long nodeRAM = MC_MAXTEXTURES * sizeof(MC_TextureNode);
masterTextureNodes = (MC_TextureNode *)systemHeap->Malloc(nodeRAM);
for (long i=0;i<MC_MAXTEXTURES;i++)
    masterTextureNodes[i].init();
```

`mclib/txmmgr.h:44, 46`:
```cpp
#define MC_MAXTEXTURES         4096   // node-index space (masterTextureNodes[])
#define MAX_MC2_GOS_TEXTURES   3000   // currentUsedTextures resident-cap
```

`mclib/txmmgr.cpp:560` and `:2381` use `MAX_MC2_GOS_TEXTURES` to gate
`currentUsedTextures` (a usage counter, not a slot-allocation cap).
Many callsites bound-check `nodeId` against `MC_MAXTEXTURES`
(txmmgr.cpp:637, txmmgr.h:447,454,550,556,608,691,743,834,898,1158).

So `mcTextureNodeIndex` ∈ [0, 4096), **not** [0, 3000). The two caps are
distinct: 3000 limits how many nodes are *resident* at once
(LRU-cache-style); 4096 is the *raw slot count* in the master array.
A node at index 3500 is perfectly legal.

The memory note `texture_handle_cap.md` ("`MAX_MC2_GOS_TEXTURES` raised
750→3000") refers to the resident-cap, not the array size. The plan and
V7 conflate them.

**Failure mode:**

- `BuildCementCatalogAtlas` Pass 1: when `peekTextureHandle(slot)` returns
  a nodeIdx ≥ 3000, `if (nodeIdx >= 3000u) continue;` silently drops the
  cement entry. Atlas missing tiles for those textures.
- Packer A.4: when `q.terrainHandle` ≥ 3000, `if (nodeIdx < 3000u)` is
  false; `cementWord = 0u`; validity bit not set; frag falls back to
  colormap. Cement quad renders the colormap blob.
- The packer's `g_cementPackUnmappedCount` never increments for these
  (the increment is INSIDE the `if (nodeIdx < 3000u)` guard branch — the
  `else` of the inner check, not the outer one). So the failure is
  **invisible to instrumentation**.

**Probability this matters in stock:** unknown but non-zero. Terrain
textures are loaded early at mission start, so the first ~30-60 nodes
typically hold them — likely in [0, 200) for stock content. But there's
no enforcing code making this true; any subsystem that pre-loads
textures before terrain (UI atlases, splash screens, mech bay assets)
pushes terrain nodeIds higher. Mc2_01 has historically loaded terrain
late enough that crossing 3000 is plausible on warm-restart paths.

**Recommended fix:** Replace every `3000` in the cement-atlas/packer code
with `MC_MAXTEXTURES` (4096):

- A.2 Step 1: `static uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];`
  (and update the comment to cite `txmmgr.h:44`, not `terrtxm.h:34`).
- A.3 Step 2: `if (nodeIdx >= (DWORD)MC_MAXTEXTURES) continue;`
- A.4 Step 2: `if (nodeIdx < (DWORD)MC_MAXTEXTURES)`.
- V7: rewrite to distinguish `MC_MAX_TERRAIN_TXMS` (terrain-slot cap, slot
  index against `nextAvailable`) from `MC_MAXTEXTURES` (mcTextureManager
  node-index cap). Add a new V-entry citing `txmmgr.h:44` and
  `txmmgr.cpp:206-211` for the array sizing.

Memory cost: 4096 × 2 B = 8 KB vs the current 3000 × 2 B = 6 KB — 2 KB
extra. Negligible.

This is V21 → V22 → **V7-v21** — the same V_L-class verification gap
firing for the third time, just on a different conflated symbol pair
(this time `MC_MAX_TERRAIN_TXMS` vs `MC_MAXTEXTURES` / "node array" vs
"resident cap"). The discipline lesson from D1 has not yet landed.

---

## MAJOR findings (revise before ship)

### M1-v21: `g_cementPackUnmappedCount` instrumentation is structurally misleading — increments for every non-cement quad on most maps

**Claim in plan (Task A.4 Step 2):**
```cpp
if (g_cementLayerMapReady) {
    const DWORD nodeIdx = (DWORD)q.terrainHandle;
    if (nodeIdx < 3000u) {
        const uint16_t idx = g_cementLayerIndexByNodeIdx[nodeIdx];
        if (idx != 0xFFFFu) {
            cementWord = kCementLayerValidBit | (uint32_t)idx;
        } else if (nodeIdx != 0u) {
            // ... ++g_cementPackUnmappedCount
        }
    }
}
```

**Issue:** The map `g_cementLayerIndexByNodeIdx` only stores entries for
**cement** textures (the atlas-build only walks cement slots and stores
those nodeIds). Every NON-cement quad's nodeIdx maps to `0xFFFF`. So the
counter increments on every grass quad, dirt quad, water quad whose
`q.terrainHandle != 0` — which is essentially every visible quad on the
map. On mc2_01, that's ~10,000+ quads per frame. The counter is
incremented during `PackThinRecordsForFrame` which runs per-frame, so
across a 25-second smoke run it'll hit hundreds of millions.

The intent (per the comment) was to flag "a cement-classified quad that
should have mapped but didn't." But the packer has no `isCement(slot)`
check before the lookup — it can't, because the slot isn't on the quad
anyway. So the counter conflates "any quad with non-zero terrainHandle"
with "missing cement enumeration."

**Failure mode:** the trace event prints
`unmapped_pack_count=<huge_number>` on every mission, drowning out the
real signal. Gate A's "Large N → enumeration miss in
`BuildCementCatalogAtlas`" criterion is structurally untestable.

**Recommended fix (one of):**

(a) Remove the counter entirely. The validity-bit gate already ensures
    no false-positive cement render; the counter has no signal to
    contribute.

(b) Cap the counter and only increment if `nodeIdx` corresponds to a
    cement-flagged slot. This requires re-deriving the slot in the
    packer (option (b) from v2.0's C1-v2 fix recommendation), which
    v2.1 chose NOT to do. Without slot access, no clean way to filter.

(c) Move the counter to atlas-build time only: count slots where
    `isCement(slot)` is true but `peekTextureHandle(slot) >=
    MC_MAXTEXTURES` (i.e., out-of-range nodeIdx) or
    `gos_terrain_bridge_glTextureForGosHandle(...) == 0` (resolve
    failed). That captures the actual enumeration miss, and runs once
    per mission load.

Recommend (c). It also incidentally surfaces C1-v21 if any cement
texture has nodeIdx ≥ 4096 in the future.

---

### M2-v21: TES `RecordIdx = 0u;` placement collides with two early-out paths in `gos_terrain.tese`

**Claim in plan (Task B.1 Step 3):**
> "In `main()`, alongside the other `out`-variable assignments, add:
> `RecordIdx = 0u;`"

**Evidence (greped `shaders/gos_terrain.tese:34-90`):** the TES `main()`
has TWO early-out paths that both `return` before reaching the end of
main:

```glsl
if (tessDebug.x < -2.5) {
    Color = vec4(1.0); Texcoord = vec2(0.0); TerrainType = 0.0;
    WorldNorm = vec3(0.0,0.0,1.0); WorldPos = vec3(0.0); UndisplacedDepth = 0.0;
    // ... gl_Position = ...
    return;     // <-- early-out 1, line 49
}
// ... barycentric interpolation ...
if (tessDebug.x < -1.5) {
    WorldNorm = worldNorm; WorldPos = worldPos; UndisplacedDepth = 0.0;
    // ... gl_Position = ...
    return;     // <-- early-out 2, line 81
}
// ... rest of main, including final TES output writes ...
```

If the executor follows "alongside the other out-variable assignments"
literally, they'll likely add `RecordIdx = 0u;` near `WorldNorm =
worldNorm; WorldPos = worldPos;` at line 115 (the only spot where
WorldPos/WorldNorm are written in the final-path). Both early-out
returns then leave `RecordIdx` undefined.

The legacy program will rarely hit those early-out paths in production
(`tessDebug.x` is normally 0+; the `-2.5` and `-1.5` branches are
debug-mode probes for screen-space projection diagnostics). But "rarely"
≠ "never," and an undefined varying read in the frag is UB. AMD drivers
have been observed to randomize undefined varyings frame-to-frame,
producing flicker — exactly the kind of "intermittent, nobody can
reproduce" failure mode that costs days.

**Recommended fix:** Move `RecordIdx = 0u;` to the **first line of
`main()`** (above all conditionals), and add an explicit comment:

```glsl
void main()
{
    RecordIdx = 0u;  // legacy chain: must be set BEFORE any early-out
                     // returns so the frag's flat read is defined under
                     // tessDebug debug-mode paths.
    vec3 bary = gl_TessCoord;
    if (tessDebug.x < -2.5) { ... return; }
    // ...
}
```

Same problem doesn't exist in the thin VS (it has no early-out paths
per [`shaders/gos_terrain_thin.vert`](shaders/gos_terrain_thin.vert) main),
but applying the same "set-it-first" discipline there is also good
hygiene.

---

## MINOR findings

### m1-v21: Cement UV scale `fract(WorldPos / worldUnitsPerVertex)` assumes one cement TXM == one terrain quad — legacy `uvData` may differ

**Plan (B.2 Step 3):**
```glsl
PREC vec2 cTileUV = fract(vec2(WorldPos.x, -WorldPos.y) / atlasCementWorldUnitsPerTile);
```
with `atlasCementWorldUnitsPerTile = Terrain::worldUnitsPerVertex (128.0)`.

**Issue:** The legacy cement render uses per-quad `uvData.{minU,maxU}`
([`mclib/quad.cpp:395-406, 753, 1785-1788`](mclib/quad.cpp:395)). For
single-tile cement quads, `[minU,maxU] = [0,1]` and the legacy sample
covers one full TXM_SIZE (64 px) cement tile — so
`fract(world / 128.0)` matches. For *transition* tiles or scaled
tarmac art that uses sub-tile UVs (e.g., `[0, 0.5]`), the indirect path
will sample the wrong sub-region.

Stock cement (per V_K's brainstorm, "9 cement source IDs: 10, 13-20")
appears to use full-tile UVs, so this is **likely OK for stock**. But
the plan claims parity with legacy "per-bucket binding"; the per-bucket
path would have respected `uvData`. Document the assumption in B.2
Step 3 comment so a future executor doesn't ship a sub-tile cement
variant that silently breaks.

**Recommended fix:** Add to B.2 Step 3 comment:
```glsl
// UV math assumes per-quad cement uses full tile (uvData=[0..1]).
// Sub-tile cement (transition tiles) would need uvData reconstructed
// per-quad — out of scope; gate A's distance screenshot will catch
// this if any stock mission exercises it.
```

### m2-v21: `truncated=1` Gate A FAIL is a manual-grep — no machinery enforces it

Plan v2.1 says (Delta table row "255-cap silent truncation"): "Gate A
treats this trace event as FAIL." Gate A's checklist (Task C.2) lists
the trace check, but `run_smoke.py` has no automated `grep -q
event=cement_catalog_truncated` step. If the operator skips the manual
log-grep, the truncation goes undetected.

**Recommended fix:** Add to Task C.2 Step 1 (or add a Step 3) an
explicit `grep -q "event=cement_catalog_truncated"` check on the smoke
artifacts directory; non-zero exit = Gate A FAIL. Or more simply: have
the trace event abort the process under
`MC2_TERRAIN_INDIRECT_TRACE=1` (analogous to existing parity-check
`event=mismatch` handling) so the smoke run exits non-zero.

### m3-v21: Frag's `thinRecsFrag[RecordIdx].control.w` read happens even when `cementValid` is false

Plan v2.1 B.2 Step 3:
```glsl
if (useCementAtlas != 0) {
    uint cementWord  = thinRecsFrag[RecordIdx].control.w;  // <-- read happens here
    bool cementValid = (cementWord & 0x80000000u) != 0u;
    if (cementValid) { ... }
}
```

Under the indirect program, the SSBO is bound at binding 2 with the
correct sub-range — read is safe for any `RecordIdx` in
[0, kMaxThinRecords). Under the legacy program, `useCementAtlas` is
always 0 (bridge resets at end of indirect draw, and legacy bridge never
sets it), so the inner `if` block doesn't execute — read is dead-coded.
Safe.

**One concern:** if the legacy program is somehow drawn with
`useCementAtlas=1` left set (e.g., a code path that sets the uniform
but then takes a renderer detour before the reset block at
`gameos_graphics.cpp:2364-2367`), the legacy frag would attempt to read
the SSBO at binding 2 — which is `glBindBufferBase(... 2, 0)` (unbound)
per `gameos_graphics.cpp:2372`. UB on most drivers; AMD typically
returns zeros, which would mean `cementWord=0`, `cementValid=false`,
no harm done — but no spec guarantee.

This is **benign in current code** because the bridge's reset always
runs before the SSBO unbind. But if a future change reorders or
short-circuits the bridge teardown, this becomes a latent failure. The
plan should explicitly document the "useCementAtlas reset before SSBO
unbind" invariant in the bridge code so a future maintainer doesn't
break it.

**Recommended fix:** Add a one-line comment at B.3 Step 3 above the
useCementAtlas reset:
```cpp
// INVARIANT: useCementAtlas must be reset to 0 BEFORE the SSBO at
// binding 2 is unbound below.  Otherwise a subsequent legacy draw with
// stale useCementAtlas=1 would read SSBO[2]=0 (UB).
```

### m4-v21: `_wp0` parity drift — A.1.bis Step 1 fix to `quad.cpp` and A.1.bis Step 2 fix to `gos_terrain_indirect.cpp` must land in same commit

Plan v2.1 A.1.bis Step 3 already does this (single bundled commit). MINOR
because the discipline is already explicit; but Stage A's task ordering
(A.1 → A.1.bis → A.2 → ...) means a partial cherry-pick that takes
A.2/A.3/A.4 without A.1.bis would silently drift `_wp0` parity for
TerrainType=20 quads. Recommend adding to V17 a callout that bundling
is mandatory.

---

## Cross-references checked

| ID | Plan claim | File:line | Match? |
|---|---|---|---|
| v2.0 C1 fix | nodeIdx-keyed map (not slot-keyed) | A.2 Step 1, A.3 Step 2, A.4 Step 2 | **CLOSED** (semantically correct now; sizing is wrong — see C1-v21) |
| v2.0 C2 fix | varying lives in TES, not VS | B.1 Steps 1-4 | **CLOSED** (TES `out` block correctly identified at gos_terrain.tese:11-16; VS/TCS untouched) |
| v2.0 C3 fix | V21 retracted, V22 added with correct semantics | Verification Appendix | **CLOSED** (V22 cites quad.cpp:546 + terrtxm.h:281-288) |
| advisor C2 fix | validity bit at `_pad0` bit 31 | A.4 Step 2, B.2 Step 3 (`& 0x80000000u`) | **CLOSED** (correct encoding; gate is now `useCementAtlas != 0 && (cementWord & 0x80000000u)` not `... && TerrainType >= 2.999`) |
| advisor M2 fix | save/restore GL_ACTIVE_TEXTURE | A.3 Step 2 (savedActive) | **CLOSED** (also adds GL_UNPACK_ALIGNMENT save/restore — bonus) |
| advisor M3 fix | mip strategy documented + Gate A distance shot | A.3 Step 2 comment + C.2 Step 1 | **CLOSED** (no-mip rationale = atlas cells lack gutters; Gate A near + distance/oblique screenshots required) |
| v2.0 M2-v2 fix | uniform-loc lifecycle warning | B.3 Step 2 (s_warnedCementUniforms) | **CLOSED** |
| v2.0 M1-v2 fix | per-quad mixed-cement noted as legacy-equivalent | Out of scope section | **CLOSED** (documented as not-a-regression) |
| v2.0 M3-v2 fix | C++ struct → SSBO layout cross-ref in frag | B.2 Step 2 comment | **CLOSED** (cites `gos_terrain_patch_stream.h:103-111`) |
| advisor minor | 255-cap warning | A.3 Step 2 (`event=cement_catalog_truncated`) | **PARTIALLY CLOSED** (event emitted; enforcement is manual-grep — see m2-v21) |
| **NEW: nodeIdx range** | nodeIdx ≤ 3000 | `mclib/txmmgr.h:44` (MC_MAXTEXTURES=4096), `txmmgr.cpp:206-211` (masterTextureNodes sized 4096) | **DIVERGENT (C1-v21)** — array is 4096, not 3000 |
| TES early-out paths | `RecordIdx=0u` placement | `shaders/gos_terrain.tese:38-49, 74-81` | **DIVERGENT (M2-v21)** — two early-out returns precede "alongside other out assignments" |
| `_pad0` bit-flag collisions | only one write site (line 991); validity bit safe | `gos_terrain_indirect.cpp:991` is sole writer | MATCHES (no collision risk) |
| SSBO binding 2 unbind path | unbound at end of indirect draw | `gameos_graphics.cpp:2372` (`glBindBufferBase(... 2, 0)`) | MATCHES (legacy frag's SSBO read is safe-because-gated, but see m3-v21) |
| `worldUnitsPerVertex = 128.0` | atlasCementWorldUnitsPerTile uniform value | `mclib/terrain.cpp:92` | MATCHES |
| `MC_MAXTEXTURES = 4096` | node-index space | `mclib/txmmgr.h:44` + `txmmgr.cpp:206-211` | MATCHES (this is the value the plan should be using, not 3000) |
| Cement legacy UV scale | per-quad `uvData.{min,max}{U,V}` | `mclib/quad.cpp:395-406, 753, 1785-1788` | MATCHES (full-tile assumption is plausible for stock; m1-v21 documents) |

### v2.0 / advisor blocker resolution verdict

| ID | Source | v2.1 plan resolution | Verdict |
|---|---|---|---|
| C1-v2 | subagent (slot vs nodeIdx) | nodeIdx-keyed map throughout | **Sound** (semantics fixed; cap is wrong — C1-v21) |
| C2-v2 | subagent (legacy TES varying) | TES-only varying add, NOT VS | **Sound** structurally; placement risk M2-v21 |
| C3-v2 | subagent (V21 conflation) | V21 retracted, V22 corrects | **Sound** (lesson partially landed — C1-v21 reprises it on a different symbol pair) |
| C1-adv | advisor (validity bit) | `_pad0` bit 31 = CEMENT_LAYER_VALID | **Sound** |
| C2-adv | advisor (alpha-cement boundary) | TerrainType no longer a frag gate | **Sound** |
| M1-v2 | subagent (per-quad mix) | documented as legacy-equivalent | **Sound** (acknowledged out-of-scope) |
| M2-v2 | subagent (uniform-loc warn) | one-time warn print | **Sound** |
| M3-v2 | subagent (frag SSBO struct doc) | comment in B.2 cites C++ struct | **Sound** |
| M2-adv | advisor (ACTIVE_TEXTURE) | save/restore added | **Sound** |
| M3-adv | advisor (mip strategy) | documented + Gate A expanded | **Sound** |

---

## Process / discipline observations

### D1-v21: V_L lesson has now failed THREE times on the same architectural endpoint

- v1 → v2.0: V_L (forall(slot in cement) ≠ forall(slot)). Caught by adversarial review.
- v2.0 → v2.1: V21 (mcTextureNodeIndex ≠ slot — function call site without checking signature). Caught by adversarial review.
- v2.1 (this review): V7 (MC_MAX_TERRAIN_TXMS ≠ MC_MAXTEXTURES — cited a memory note's *meaning* without re-greping the actual `#define` for the bound being asserted). Caught by adversarial review.

The pattern: every revision retires the *specific* prior conflation but
introduces a *new* conflation in the same family — "I trusted the cited
symbol's name to mean what I thought it meant, instead of greping for
the actual numeric value / signature / array-size at write-time."

**Recommendation for plan v2.2:** every numeric constant cited in the
plan (3000, 4096, 65536, 32, 144, 255, 128.0, all the bit masks) gets
its OWN V-entry citing the actual `#define` or initialization
expression, plus the file containing the array/struct it bounds. The
discipline rule "for every cited symbol that's a function call result,
verify the function signature AND return type" should expand to: "for
every cited symbol that's a numeric constant or `#define`, verify the
constant's actual value AND the array/struct it bounds AND any sibling
constants that might be confused with it."

### D2: counter instrumentation is structurally wrong, not just imprecise

M1-v21 (`g_cementPackUnmappedCount` increments on every non-cement
quad) is not a "log spam" issue — it's a "counter has no information"
issue. The lifecycle-instrumentation discipline
(`debug_instrumentation_rule.md`) says: "Log at lifecycle boundaries
only — never per-frame at 50-60 FPS." The plan adds a per-frame counter
that increments per-quad-per-frame, which is exactly the anti-pattern
the discipline forbids. Even if C1-v21 didn't exist, this counter would
have to be removed or moved to atlas-build time before ship.

### D3: gate-A-FAIL trace events should abort the smoke run, not require manual grep

m2-v21 (truncation event) generalizes: any `event=cement_*` that is
declared "Gate A FAIL" should cause the smoke run to exit non-zero
without operator intervention. Adding a generic mechanism (e.g.,
`MC2_TERRAIN_INDIRECT_TRACE_FATAL=cement_catalog_truncated`) that
makes specified events fatal is one slice; for now, plumb it manually
in C.2 Step 1 with a `grep -q ... && exit 1` postcheck.

---

## Architectural decisions that need user/advisor sign-off before revision pass

1. **C1-v21 fix path.** Replace `3000` with `MC_MAXTEXTURES` (= 4096) in
   the cement-atlas/packer code and rewrite V7 to distinguish the two
   caps. One-diff fix; recommend (a) include `mclib/txmmgr.h` from
   `gos_terrain_indirect.cpp` for the `MC_MAXTEXTURES` symbol, (b) add
   `static_assert(MC_MAXTEXTURES <= 65535, "g_cementLayerIndexByNodeIdx uint16_t overflow");`.

2. **M1-v21 counter resolution.** Either delete entirely (option a) or
   move to atlas-build (option c). Recommend (c): the count of
   "isCement(slot) AND nodeIdx out-of-range OR resolve-failed" is the
   load-bearing signal and is also the canary for C1-v21 if anyone
   bumps `MC_MAXTEXTURES`.

3. **M2-v21 TES `RecordIdx=0u` placement.** Move to first line of
   `main()` to precede early-outs. One-line change; uncontroversial.

4. **m2-v21 trace enforcement.** Manual-grep is acceptable for v2.1
   ship-clean if Gate A discipline notes the operator MUST grep for
   `cement_catalog_truncated`. Better: add the postcheck to Task C.2.

---

## Bottom line

v2.1 made real progress: the architectural pivots are correct, the
verification appendix grew V22-V25, and 5 of 6 prior CRITICALs are
genuinely closed at the file:line level. The remaining gap is a single
mis-sized array driven by the same "trust the symbol name" discipline
failure that C3-v2 was supposed to teach.

A v2.2 with a one-paragraph fix (s/3000/MC_MAXTEXTURES/, move
`RecordIdx=0u` to top-of-main, delete or relocate the unmapped
counter) would ship clean. No deeper architectural revision needed.
