# Water v2 S6 - Plan-stage Rule-0 Grounding Addendum (V1-V6 + M1a home)

**Date:** 2026-05-17
**Produced by:** Task 0 of `2026-05-17-water-v2-s6-armed-water-drawside-decouple.md`
**Scope:** READ-ONLY. The grep-verified line-ref source of truth for Tasks 1-3.
All line numbers below were grep/Read-verified in
`.claude/worktrees/water-material-v1/` at write-time (2026-05-17). Symbols are
stable; re-grep before each downstream task (line numbers drift).

**Verdict summary:** DONE_WITH_CONCERNS. One spec/code-structure ordering
mismatch flagged (non-blocking, see CONCERN-1); one M1a TU-visibility finding
that constrains the helper home (see V2/M1a). Substitutivity claims V3/V4/V5
all hold; `IsFrameMaskWaterArmed()` independently re-confirmed ZERO consumers.

---

## V1 - Per-sub-block KEEP/GATE map (`mclib/quad.cpp`)

`TerrainQuad::setupTextures()` water block opens at **quad.cpp:1006** (outer
`{` + `CostSplitWaterVertProjScope _csWvp;` at :1007). The water-corner
predicate `if ((vertices[0..3]->pVertex->water & 1))` is at **:1008-1011**.
`clipped1/clipped2` computed at **:1016-1023** (KEEP-(i): read-only of
`clipInfo`, drives the per-vertex `if (clipped1||clipped2)` admission).

The 4 per-vertex sub-blocks each have the shape
`if (!(vertices[N]->calcThisFrame & 2)) { if (clipped1 || clipped2) { ... } }`.

### Statement-level KEEP/GATE per sub-block

| Statement | Vert 0 | Vert 1 | Vert 2 | Vert 3 | Tag |
|---|---|---|---|---|---|
| `calcThisFrame & 2` guard open | :1027 | :1092 | :1159 | :1226 | KEEP-(i) |
| `if (clipped1 \|\| clipped2)` open | :1029 | :1094 | :1161 | :1228 | KEEP-(i) |
| water&128/&64 ourCos select | :1031-1038 | :1096-1103 | :1163-1170 | :1230-1237 | KEEP-(i) |
| `vertex3D.z = ourCos + waterElevation` (+x/y) | :1040 | :1105-1107 | :1172-1174 | :1239-1241 | KEEP-(i) |
| `bool clipData=false;` + `PROJECTZ_SITE` | :1042-1044 | :1109-1111 | :1176-1178 | :1243-1245 | KEEP-(i) |
| `clipData = eye->projectForTerrainAdmission(vertex3D,screenPos)` | :1045 | :1112 | :1179 | :1246 | KEEP-(i) |
| `pz_capture_vert_preds(N)` | :1046 | :1113 | :1180 | :1247 | KEEP-(i) |
| `isVisible` + `if(!isVisible) clipData=false` | :1047-1051 | :1114-1118 | :1181-1185 | :1248-1252 | KEEP-(i) |
| `clipInfo = clipData` if/else pair (M2a) | :1053-1056 | :1120-1123 | :1187-1190 | :1254-1257 | **KEEP-(i)** |
| `wx/wy/wz/ww = screenPos.*` | :1058-1061 | :1125-1128 | :1192-1195 | :1259-1262 | **GATE-(ii)** |
| `calcThisFrame \|= 2` | :1063 | :1130 | :1197 | :1264 | KEEP-(i) (see CONCERN-1) |
| 6-tuple min/max (`if (clipData){...}`) | :1065-1088 | :1132-1155 | :1199-1222 | :1266-1289 | KEEP-(i) |

After the 4 sub-blocks:

| Statement | Lines | Tag |
|---|---|---|
| `if (clipped1 \|\| clipped2)` (handle/bulk) open | :1293 | KEEP-(i) (the `if` head; body GATE-(ii)) |
| `if (!terrainTextures2){...} else {...}` handle resolution (`setDetail`/`getTextureHandle`/`getDetailHandle`/`getWaterTextureHandle`/`getWaterDetailHandle`) | :1295-1305 | **GATE-(ii)** |
| `addTriangleBulk(waterHandle, ...ISWATER...)` | :1307 | **GATE-(ii)** |
| `addTriangleBulk(waterDetailHandle, ...ISWATERDETAIL...)` | :1308 | **GATE-(ii)** |
| `else { waterHandle=0xffffffff; waterDetailHandle=0xffffffff; }` (clipped-false sentinel) | :1310-1314 | **KEEP unconditional** (sonnet MAJOR-2) |
| `else { waterHandle=0xffffffff; waterDetailHandle=0xffffffff; }` (water&1 false sentinel) | :1316-1320 | **KEEP unconditional** |
| outer block close `}` + `// close CostSplitWaterVertProjScope` | :1321 | KEEP-(i) |

### M2a clean (clipData wholly (i)-produced)

Confirmed: `clipData` is produced ONLY by `eye->projectForTerrainAdmission`
(:1045/1112/1179/1246) then conditionally reset by the `isVisible` test
(:1047-1051 etc.). It has ZERO dependency on any (ii) statement (`wx..ww`,
handles, `addTriangleBulk`). The `clipInfo = clipData` write sits physically
ABOVE the first GATE-(ii) statement (`clipInfo` at :1053-1056, `wx..ww` at
:1058-1061). "Keep clipInfo in (i), unconditional" is therefore
parity-identical-to-today by construction. **M2a clean.**

### MINOR-1 confirmed (8 clipInfo branch points, NOT gated)

The `clipInfo = clipData` pair is, in all 4 sub-blocks:
```cpp
if (eye->usePerspective)
    vertices[N]->clipInfo = clipData;   // first branch
else
    vertices[N]->clipInfo = clipData;   // second branch - IDENTICAL assignment
```
Both branches assign `clipData` identically (collapsed DX8/GL dead-code
pattern). That is **8 assignment points total** (2 branches x 4 sub-blocks).
All 8 STAY UNCONDITIONAL in (i). Task 2 must not gate any of them.

---

## V2 / M1a - `Terrain::renderWater` early-return + helper home

### The early-return conjunction (verbatim, `mclib/terrain.cpp`)

`Terrain::renderWater(void)` opens at **:1160**. The "byte-identical"
contract comment is **:1183-1193** (the "This gate MUST stay byte-identical
to renderWaterFastPath()'s s_fastPath" block; the `:1184`-class comment).

`s_fastPath` definition (function-static, **:1194-1196**):
```cpp
static const bool s_fastPath =
    (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr) ||
    gpu_driven::IsWaterEnabled();
```

Early-return conjunction (**:1209-1217**), verbatim, in order:
```cpp
if (s_fastPath
    && gos_terrain_indirect::IsFrameSolidArmed()
    && WaterStream::IsReady()
    && WaterStream::GetRecipeCount() > 0
    && Terrain::terrainTextures2 != nullptr)
{
    // Skip legacy loop entirely; renderWaterFastPath() does the work.
    return;
}
```
Sub-terms, exact order:
1. `s_fastPath` = `(getenv("MC2_RENDER_WATER_FASTPATH") != nullptr) || gpu_driven::IsWaterEnabled()`
2. `gos_terrain_indirect::IsFrameSolidArmed()`
3. `WaterStream::IsReady()`
4. `WaterStream::GetRecipeCount() > 0`
5. `Terrain::terrainTextures2 != nullptr`

The structural twin in `renderWaterFastPath()` (:1306) re-states the same
predicate as guard clauses at :1310-1316 (the second hand-copy the M1a
single-source retires conceptually; only the renderWater early-return is
refactored by Task 1, per plan Task 1 Step 2).

### M1a helper home DECISION (load-bearing TU-visibility finding)

**Finding:** `mclib/quad.cpp` does NOT include `gos_terrain_water_stream.h`
or `gpu_driven_common.h` (verified: quad.cpp includes terminate at
:41-49; no WaterStream / gpu_driven header). It DOES include
`../GameOS/gameos/gos_terrain_indirect.h` at **quad.cpp:43**.

So `WaterStream::` and `gpu_driven::IsWaterEnabled()` are **NOT visible in
quad.cpp's TU**. Symbol-visibility audit of candidate definition TUs:

| Symbol | terrain.cpp | gos_terrain_indirect.cpp | quad.cpp |
|---|---|---|---|
| `WaterStream::IsReady/GetRecipeCount` | YES (`#include gos_terrain_water_stream.h` :35) | NO (no such include) | NO |
| `gpu_driven::IsWaterEnabled` | YES (`#include gpu_driven_common.h` :36) | YES (`#include "gpu_driven_common.h"` :17) | NO |
| `gos_terrain_indirect::IsFrameSolidArmed` | YES (decl `gos_terrain_indirect.h:414`) | YES (native) | YES (via :43 include) |
| `Terrain::terrainTextures2` | YES (def `terrain.cpp:88`) | via `terrain.h` | YES (via `terrain.h` quad.cpp:22) |

**Only `mclib/terrain.cpp` sees all four symbols.** The helper MUST be a
NON-inline function whose DEFINITION lives in a TU that sees all four
(terrain.cpp), and whose DECLARATION lives in a header already included by
BOTH quad.cpp and terrain.cpp with zero new includes.

**DECIDED home:**

- **Declaration:** add `bool WaterFastPathOwnsArmedDraw();` inside
  `namespace gos_terrain_indirect { ... }` in
  `GameOS/gameos/gos_terrain_indirect.h` (near the other free fns
  `IsFrameSolidArmed()`/`DrawIndirect()`, currently :414-416, before the
  `}  // namespace gos_terrain_indirect` at :445). This header is ALREADY
  included by quad.cpp (:43) and terrain.cpp (:37). Zero new includes in
  any consumer TU. No `WaterStream`/`gpu_driven` types appear in the
  signature (returns `bool`), so the declaration adds no header coupling.
- **Definition:** `bool gos_terrain_indirect::WaterFastPathOwnsArmedDraw()`
  in `mclib/terrain.cpp` (a namespace-qualified out-of-line definition in
  terrain.cpp is legal C++; terrain.cpp already sees all four symbols and
  is the file that owns `renderWater`). Body = the full conjunction with
  the `s_fastPath` getenv-once semantics preserved via a function-static
  inside the helper:
  ```cpp
  bool gos_terrain_indirect::WaterFastPathOwnsArmedDraw()
  {
      static const bool s_fastPath =
          (getenv("MC2_RENDER_WATER_FASTPATH") != nullptr) ||
          gpu_driven::IsWaterEnabled();
      return s_fastPath
          && gos_terrain_indirect::IsFrameSolidArmed()
          && WaterStream::IsReady()
          && WaterStream::GetRecipeCount() > 0
          && Terrain::terrainTextures2 != nullptr;
  }
  ```
- **Signature:** `bool gos_terrain_indirect::WaterFastPathOwnsArmedDraw()`
  (no args; returns `bool`).

**Rejected alternatives:**
- A `Terrain::` static method: would put the decl in `terrain.h`; viable
  (terrain.h already in both TUs) and ABI-clean, but `gos_terrain_indirect`
  is the natural namespace for arm-state predicates (siblings
  `IsFrameSolidArmed`/`IsFrameMineArmed` live there) - chosen home is more
  cohesive. Recorded as the fallback if the planner prefers a `Terrain::`
  static (decl in terrain.h, def in terrain.cpp - same TU constraints hold).
- A `gos_terrain_indirect.cpp` definition: REJECTED - that TU does NOT
  include `gos_terrain_water_stream.h`, so `WaterStream::` is invisible
  there; would force a new include into that file (avoidable blast radius).

---

## V3 - Symbol reachability + IsFrameMaskWaterArmed zero-consumer re-confirm

From the chosen helper TU (`terrain.cpp`) all four are reachable (table
above). From `quad.cpp`: only `gos_terrain_indirect::IsFrameSolidArmed()`
is directly reachable; `WaterStream::`/`gpu_driven::IsWaterEnabled` are NOT
- which is exactly why the helper is a non-inline fn declared in
`gos_terrain_indirect.h` (quad.cpp calls the opaque `bool` helper; it never
needs WaterStream/gpu_driven types itself). Reachability is satisfied.

- `gos_terrain_indirect::IsFrameSolidArmed()` - decl `gos_terrain_indirect.h:414`.
- `gpu_driven::IsWaterEnabled()` - used `terrain.cpp:1196`, header
  `gpu_driven_common.h` (terrain.cpp:36, gos_terrain_indirect.cpp:17).
- `WaterStream::IsReady()` / `GetRecipeCount()` - used `terrain.cpp:1211-1212`,
  header `gos_terrain_water_stream.h` (terrain.cpp:35).
- `Terrain::terrainTextures2` - def `terrain.cpp:88`, decl via `terrain.h`.

**IsFrameMaskWaterArmed() re-confirmed ZERO consumers (independent grep):**
Repo-wide grep of `IsFrameMaskWaterArmed` across `*.{cpp,h,hpp,cc}` returns
exactly TWO hits:
- declaration: `GameOS/gameos/gos_terrain_mask_dispatch.h:55`
- definition: `GameOS/gameos/gos_terrain_mask_dispatch.cpp:170`

ZERO call sites anywhere. Confirmed DEAD. Do NOT gate S6 on it (gating on it
would skip (ii) on un-armed intro/deployment frames where `drawWater` STILL
runs - the stale-`wx..ww` regression `water_fastpath_interim_fixes_and_
residuals.md` fix #2 prevents).

---

## V4 - Readers of `->wx/->wy/->wz/->ww` and water-quad `clipInfo`

- **`wx/wy/wz/ww` sole reader = `TerrainQuad::drawWater()`**
  (`quad.cpp:3264`). The `wx..ww` reads are at :3313-3316, :3323-3326,
  :3333-3336, :3472-3475, :3601-3604, :3611-3614, :3621-3624, :3760-3763
  (all `gVertex[*].x/y/z/rhw = vertices[*]->wx/wy/wz/ww`). No other reader
  of `->wx..ww` exists in quad.cpp. `drawWater()` is called ONLY from
  `Terrain::renderWater()` at **terrain.cpp:1250**, inside the per-quad
  loop that the **:1209-1217 early-return short-circuits when armed**. So
  (ii) (the `wx..ww` writer) and its sole consumer `drawWater` are skipped
  under the IDENTICAL predicate -> paired, no stale read. **V4 holds.**

- **`drawLine()` (`quad.cpp:3891`) reads `clipInfo`, NOT `wx..ww`.** It
  forms `clipped1/clipped2` from `vertices[*]->clipInfo` (:3893-3899). It
  is called from `Terrain::render()` at **terrain.cpp:1139**, gated by
  `if (drawTerrainGrid || DrawDebugCells || drawLOSGrid)` (:1130) - a
  debug-overlay flag, **NOT the armed-water predicate**. Since `clipInfo`
  stays UNCONDITIONAL in (i) under M2a, `drawLine`'s `clipInfo` reads are
  unaffected by the S6 gate regardless of whether the debug overlay is on.
  It is out-of-scope debug; it does not run on the normal armed gameplay
  path (requires a debug grid/cells/LOS toggle). **No same-predicate
  coupling; safe.**

- **slimReduce `clipInfo` write runs BEFORE the setupTextures loop.** The
  unconditional per-vertex `rv->clipInfo = clipR;` is at **terrain.cpp:1668**
  (inside the `Terrain::geometry slimReduce` zone, :1562). The
  `quadSetupTextures` loop opens at **:1785** (`ZoneScopedN("Terrain::
  geometry quadSetupTextures")`) and the per-quad `currentQuad->
  setupTextures();` call is at **:1810**. slimReduce (:1668) is strictly
  ABOVE the setupTextures loop (:1808-1810) in `Terrain::geometry`. The
  cull-driving `clipInfo` is the slimReduce write; (ii)'s water-corner
  `clipInfo` rewrite was redundant for the cascade (and under M2a stays in
  (i) unconditional anyway). **Cull-cascade immune confirmed.**

---

## V5 - `addTriangleBulk` legacy path + reservation/fill pairing

`addTriangleBulk` is the legacy `masterVertexNodes` immediate path
(`mclib/txmmgr.h`; the two water enqueues are `quad.cpp:1307-1308`:
`addTriangleBulk(waterHandle, MC2_ISTERRAIN|MC2_DRAWALPHA|MC2_ISWATER, 2)`
and `addTriangleBulk(waterDetailHandle, ...|MC2_ISWATERDETAIL, 2)`). These
two RESERVE the triangle-pair slots in `setupTextures` (the GATE-(ii)
statements). The FILL is `drawWater()` (`quad.cpp:3264`, the sole `wx..ww`
reader) driven from `Terrain::renderWater` (terrain.cpp:1250). Both
reservation (setupTextures (ii)) and fill (`drawWater` via `renderWater`)
are skipped together under the SAME predicate
(`WaterFastPathOwnsArmedDraw()` for (ii); the verbatim conjunction at
terrain.cpp:1209-1217, refactored to call the same helper by Task 1, for
the fill side). No half-pair / no desynced master-node counter when (ii) is
skipped. **V5 holds.**

---

## V6 - Env-gated probe idiom + setupTextures hotness (for Task 3 `[WATER_S6 v1]`)

**Idiom (from `[WATER_INVPROJ v1]`, `mclib/terrain.cpp`):**
- Cached env bool: `static const bool s_waterInvprojParity =
  (getenv("MC2_WATER_INVPROJ_PARITY") != nullptr);` (**terrain.cpp:1770-1771**).
- Edge latch: `static int s_lastState = -1;` (**:1893**), `const int state
  = identical ? 1 : 0;` (:1894), `if (state != s_lastState) { ...
  printf(...); fflush(stdout); s_lastState = state; }` (:1895-1925).
- Print: raw `printf("[WATER_INVPROJ v1] event=parity result=...\n");`
  + `fflush(stdout);` (:1899/1903/1923).
- Sibling cached-bool idiom: `static const bool s_waterDebugOn =
  (getenv("MC2_WATER_DEBUG") != nullptr);` (terrain.cpp:1169, :1323).
  `[WATER_REFL v1]` referenced as the same family in the comment at :1870.

So Task 3's `[WATER_S6 v1]` follows: `static const bool s_waterS6Trace =
(getenv("MC2_WATER_S6_TRACE") != nullptr);` + `static int s_lastS6 = -1;`
edge latch + raw `printf("[WATER_S6 v1] ...\n"); fflush(stdout);`.

**setupTextures hotness (probe-hoist guidance):** `currentQuad->
setupTextures();` is called once **per quad** in the
`for (i=0;i<numberQuads;i++)` loop at **terrain.cpp:1808-1810** (inside
the `quadSetupTextures` zone :1785). It is **per-quad HOT**. The predicate
`IsFrameSolidArmed()` is **frame-stable** for all setupTextures() calls
(comment terrain.cpp:1786-1787: "preflight arming walks live quadList
BEFORE the loop so IsFrameSolidArmed() is stable for all setupTextures()
calls"), so the gate decision does not change within a frame.

Task 3 guidance: the env-gated + edge-latched probe is O(1) amortized
per quad (cached `static const bool` short-circuits the body; the
`WaterFastPathOwnsArmedDraw()` call only fires when the env bool is set and
only does work on the latch edge). It is acceptable in-loop per the plan,
BUT the cleanest hoist (recommended) is a **once-per-frame site that
observes the same frame-stable predicate** - e.g. immediately before the
setupTextures loop in `Terrain::geometry` (terrain.cpp ~:1806-1808,
alongside `WaterStream::BeginFrameNarrow()`), or at the `renderWater`
early-return (terrain.cpp:1209). Hoisting avoids any per-quad call to
`WaterFastPathOwnsArmedDraw()` entirely while still proving (ii) is skipped
on armed frames. The planner should pick the hoisted site since
setupTextures is confirmed per-quad hot.

---

## CONCERN-1 (flagged for planner; NON-BLOCKING) - statement interleave order

The spec/plan describe (i) and (ii) as cleanly separable, but the ACTUAL
per-vertex sub-block ordering interleaves a KEEP-(i) statement BETWEEN the
two GATE-(ii) regions:

```
:1053-1056  clipInfo = clipData            <- KEEP-(i)  (M2a)
:1058-1061  wx/wy/wz/ww = screenPos.*      <- GATE-(ii)
:1063       calcThisFrame |= 2             <- ORDERING-SENSITIVE (see below)
:1065-1088  6-tuple min/max                <- KEEP-(i)
```

`vertices[N]->calcThisFrame |= 2` (:1063/1130/1197/1264) sits physically
BETWEEN the GATE-(ii) `wx..ww` writes and the KEEP-(i) 6-tuple reduction.
`calcThisFrame & 2` is the per-vertex "already projected this frame" skip
guard read at the TOP of each sub-block (:1027/1092/1159/1226) AND is the
`calcThisFrame&2` fast-skip the spec 8c-UPDATE relies on for the
arm-transition reasoning.

**The split is still mechanically clean** because the KEEP/GATE boundary is
per-STATEMENT, not a contiguous range: Task 2 wraps ONLY the `wx..ww` writes
(:1058-1061 etc.) and ONLY the `if(clipped1||clipped2){handle+bulk}` body
(:1295-1308) in `if (!WaterFastPathOwnsArmedDraw()) { ... }`, leaving
`clipInfo` (:1053-1056), `calcThisFrame |= 2` (:1063), and the 6-tuple
(:1065-1088) UNCONDITIONAL. **BUT the planner/executor MUST be explicit that
`calcThisFrame |= 2` (:1063/1130/1197/1264) is KEEP-(i) / unconditional** -
if it were accidentally swept into the (ii) gate, then on armed frames the
per-vertex "projected this frame" flag would never be set, the `!(calc
&2)` guard would re-enter the (i) projection every frame for every water
vertex (defeating the self-skip the spec Section 3 calls "<=4
self-skipping projects per quad"), and worse, cross-frame `calcThisFrame&2`
state would diverge from the legacy path. The (ii) gate must wrap exactly
`{ wx; wy; wz; ww; }` and the post-loop `{ handle-resolution; bulk; bulk; }`
body - and NOTHING between or around them. This is a precision requirement
for Task 2 Step 1, not a design defect. Flagged so the planner states the
exact per-statement wrap boundaries (the V1 table above is the authority).

No other spec/code mismatch found. The 8c-UPDATE boundary rule (clipInfo
stays in (i) unconditional) is consistent with the actual code (clipInfo
write physically precedes the first GATE-(ii) statement). sonnet MAJOR-2
(else-sentinel unconditional) matches actual structure (:1310-1314 and
:1316-1320 both stay unconditional).

---

## Downstream task line-ref quick index (re-grep before use)

- Task 1: helper decl `gos_terrain_indirect.h` (~:414-445 region, in the
  `namespace gos_terrain_indirect`); helper def `terrain.cpp` (new fn);
  refactor early-return `terrain.cpp:1209-1217`; retire contract comment
  `terrain.cpp:1183-1196`.
- Task 2: gate `wx..ww` writes `quad.cpp:1058-1061 / 1125-1128 /
  1192-1195 / 1259-1262`; gate handle+bulk body `quad.cpp:1295-1308`;
  KEEP unconditional: clipInfo `:1053-1056/1120-1123/1187-1190/1254-1257`,
  `calcThisFrame|=2` `:1063/1130/1197/1264`, 6-tuple
  `:1065-1088/1132-1155/1199-1222/1266-1289`, both sentinels
  `:1310-1314` + `:1316-1320`. Stale comment fix `terrain.cpp` ~:1821
  (re-grep: the `IsFrameSolidArmed()` block near :1820).
- Task 3: `[WATER_S6 v1]` - recommended once-per-frame hoist near
  `terrain.cpp:1806-1808` (setupTextures loop preamble) or :1209
  (renderWater early-return), NOT per-quad in setupTextures (:1810 is
  per-quad hot).
