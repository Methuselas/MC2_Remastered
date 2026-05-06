# PatchStream M1f: Skip Legacy Solid Expanded Staging Under Thin-Record Draw

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1` and the thin-record draw path is fully active, skip `mcTextureManager->addVertices(DRAWSOLID)` and `fillTerrainExtra()` in `TerrainQuad::draw()` — the two calls that feed the legacy expanded-vertex ring buffers — while leaving `gVertex` construction, pzTri admission, overlays, detail textures, alpha draws, and all PatchStream emit calls unchanged.

**Expected win:** Modest-to-medium reduction in `quadSetupTextures` self-time. This eliminates the expanded solid staging calls but still builds `gVertex`, runs diagonal logic, builds thin records, and handles overlay/detail paths. The remaining diagonal/build logic is the next bigger slice.

**Architecture:** Add `TerrainPatchStream::isFastPathActive()` (true only when thin records are active AND the draw path is enabled). Gate the four `if (terrainHandle != 0)` blocks that call `addVertices(DRAWSOLID)` + `fillTerrainExtra` behind `!isFastPathActive()`. No other changes. No shader changes.

**Tech Stack:** C++17. Files: `GameOS/gameos/gos_terrain_patch_stream.h`, `GameOS/gameos/gos_terrain_patch_stream.cpp`, `mclib/quad.cpp`.

---

## Background: what gets skipped and why it's safe

`TerrainQuad::draw()` has two UV-mode branches — `BOTTOMRIGHT` and `BOTTOMLEFT` — each with two triangle sub-sections (`pzTri1`, `pzTri2`). In each sub-section the structure is:

```cpp
if (pzTriN) {
    {
        if (terrainHandle != 0) {
            mcTextureManager->addVertices(terrainHandle, gVertex, MC2_ISTERRAIN | MC2_DRAWSOLID);  // ← SKIP
            fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, ...);                    // ← SKIP
        }
        if (useOverlayTexture ...) { ... }      // leave alone
        if (useWaterInterestTexture ...) { ... } // leave alone
    }
}
// ... later, after both pzTri sections ...
if (terrainHandle != 0 && TerrainPatchStream::isReady() ...) {
    appendQuad(...);         // already no-op when thin records active
    appendQuadRecord(...);   // only active if MC2_PATCHSTREAM_QUAD_RECORDS=1
    appendThinRecord(...);   // always emit — unchanged
}
```

`gVertex` construction (`.x/.y/.z/.argb/.frgb` assignments) happens before the `if (pzTriN)` blocks and still runs — `.z` is what the pz check tests, and `.argb/.frgb` feed `gvTri1` which `appendThinRecord` reads. Only the two staging calls inside the `if (terrainHandle != 0)` block are gated.

`fillTerrainExtra` writes world-space positions/normals into the extras ring buffer for the expanded-vertex draw path. When thin records are active and drawing, the expanded draw path is empty (`flush()` `hasExpanded == false`), so extras data is waste.

**Why `isFastPathActive()` must require `s_thinRecordsDrawOn`:** If someone sets `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1` + `MC2_PATCHSTREAM_THIN_RECORDS=1` but omits `MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1`, thin records are emitted but never drawn. Gating solid staging under those conditions makes terrain invisible. The draw flag is the correct activation guard.

---

## File Map

| File | Change |
|------|--------|
| `GameOS/gameos/gos_terrain_patch_stream.h` | Add `static bool isFastPathActive()` declaration |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` | Add `s_fastPathOn` static + `isFastPathActive()` implementation |
| `mclib/quad.cpp` | Gate 4 `if (terrainHandle != 0)` solid-draw blocks (lines ~1726, ~1869, ~2077, ~2218) |

---

## Task 1: isFastPathActive() predicate

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

- [ ] **Step 1: Add declaration to header**

In `GameOS/gameos/gos_terrain_patch_stream.h`, inside the `TerrainPatchStream` class, after the `isThinRecordsActive()` declaration:

```cpp
    // Returns true when thin records are active and drawing AND MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1.
    // When true, quad.cpp skips addVertices(DRAWSOLID) and fillTerrainExtra().
    // Requires s_thinRecordsDrawOn so terrain solid never disappears without a working GPU draw path.
    static bool isFastPathActive();
```

- [ ] **Step 2: Add static + implementation to .cpp**

In `GameOS/gameos/gos_terrain_patch_stream.cpp`, in the anonymous namespace alongside the other `s_*On` statics (around line 66, after `s_thinRecordsDrawOn`):

```cpp
    static const bool s_fastPathOn = (getenv("MC2_PATCHSTREAM_THIN_RECORD_FASTPATH") != nullptr);
```

Then after `isThinRecordsActive()` (around line 600):

```cpp
bool TerrainPatchStream::isFastPathActive() {
    return s_fastPathOn &&
           s_thinRecordsOn &&
           s_thinRecordsDrawOn &&
           (s_thinRecordBuf != 0);
}
```

All four conditions must be true: the fast-path env gate, thin record emission enabled, thin record draw enabled, and the SSBO actually allocated. This prevents terrain from disappearing if only a subset of the thin-record flags are set.

- [ ] **Step 3: Build**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: `mc2.exe` built, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.h GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): M1f — add isFastPathActive() predicate for solid staging fast path"
```

---

## Task 2: Gate BOTTOMRIGHT solid draws

**Files:**
- Modify: `mclib/quad.cpp` lines ~1726–1729 and ~1869–1872

### Context

`uvMode == BOTTOMRIGHT` is the first branch in `TerrainQuad::draw()` (starting around line 1669). It has two `if (terrainHandle != 0)` blocks — one for tri1 (corners 0,1,2) and one for tri2 (corners 0,2,3 after gVertex shuffle). Each contains exactly `addVertices(DRAWSOLID)` + `fillTerrainExtra`.

**Before any edits**, confirm all DRAWSOLID sites in quad.cpp:

```bash
grep -n "MC2_DRAWSOLID" mclib/quad.cpp
```

Expected output — exactly 4 lines, one per solid-draw block:
```
1727:					mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
1870:					mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
2078:					mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
2219:					mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
```

Line numbers may differ by a few lines; what matters is exactly 4 hits. If there are more, do not proceed — investigate the additional sites before editing.

- [ ] **Step 1: Confirm MC2_DRAWSOLID site count**

Run the grep above. Confirm exactly 4 hits. Record the actual line numbers for the edits below.

- [ ] **Step 2: Gate tri1 solid draw (BOTTOMRIGHT)**

Find this block (around line 1726, inside `if (pzTri1)`, BOTTOMRIGHT branch):

```cpp
				if(terrainHandle!=0) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
						// PatchStream append moved to appendQuad after both pz gates.
					}
```

Replace with:

```cpp
				if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
						// PatchStream append moved to appendQuad after both pz gates.
					}
```

- [ ] **Step 3: Gate tri2 solid draw (BOTTOMRIGHT)**

Find this block (around line 1869, inside `if (pzTri2)`, BOTTOMRIGHT branch):

```cpp
					if(terrainHandle!=0) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}
```

Replace with:

```cpp
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}
```

- [ ] **Step 4: Build**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: 0 errors.

- [ ] **Step 5: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat(patchstream): M1f — gate BOTTOMRIGHT solid draws behind isFastPathActive()"
```

---

## Task 3: Gate BOTTOMLEFT solid draws

**Files:**
- Modify: `mclib/quad.cpp` lines ~2077–2081 and ~2218–2222

### Context

`uvMode == BOTTOMLEFT` is the second branch (starting around line 2022). tri1 uses corners 0,1,3; tri2 uses corners 1,2,3 after the shuffle. The `fillTerrainExtra` calls name different vertex combinations but the `if (terrainHandle != 0)` structure is identical.

- [ ] **Step 1: Gate tri1 solid draw (BOTTOMLEFT)**

Find this block (around line 2077, inside `if (pzTri1)`, BOTTOMLEFT branch):

```cpp
					if(terrainHandle!=0) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
						// PatchStream append moved to appendQuad after both pz gates.
					}
```

Replace with:

```cpp
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
						// PatchStream append moved to appendQuad after both pz gates.
					}
```

- [ ] **Step 2: Gate tri2 solid draw (BOTTOMLEFT)**

Find this block (around line 2218, inside `if (pzTri2)`, BOTTOMLEFT branch):

```cpp
					if(terrainHandle!=0) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}
```

Replace with:

```cpp
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}
```

- [ ] **Step 3: Confirm all 4 sites gated**

```bash
grep -n "MC2_DRAWSOLID" mclib/quad.cpp
```

All 4 lines should now be preceded by `&& !TerrainPatchStream::isFastPathActive()` in the enclosing `if`. Visually confirm.

- [ ] **Step 4: Build**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: 0 errors.

- [ ] **Step 5: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat(patchstream): M1f — gate BOTTOMLEFT solid draws behind isFastPathActive()"
```

---

## Task 4: Deploy, parity gate, visual gate, smoke test

**Files:** No code changes.

- [ ] **Step 1: Deploy exe**

```bash
WT="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.2"
cp -f "$WT/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
diff -q "$WT/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
```

Expected: no output (files match).

- [ ] **Step 2: Standard-path smoke (no fast-path env vars)**

Run smoke with no PatchStream env vars to confirm the baseline is unbroken:

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0. All tier1 missions pass, menu canary clean. If this fails, the implementation has broken the standard path — investigate before proceeding.

- [ ] **Step 3: Parity baseline — thin records WITHOUT fast path**

```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCH_STREAM_TRACE=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Load mc2_01. Let it run ~10 seconds. Close. In stderr output, find lines matching `event=thin_record_parity`. Record the `thin_records=N` value from a typical frame.

- [ ] **Step 4: Parity check — thin records WITH fast path**

```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& set MC2_PATCH_STREAM_TRACE=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Load mc2_01. Check `event=thin_record_parity`. Expected:
- `thin_records=N` matches baseline from Step 3
- `match=1` every frame

If `match=0`: `appendThinRecord` is being skipped for quads that the old path emitted, or pzTri results differ. Investigate before continuing.

- [ ] **Step 5: Visual gate**

Launch with fast path ON (no trace):

```
set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
```

Check:
1. Terrain base color, lighting, and tessellation render correctly
2. Terrain overlays (roads, craters) still draw — the overlay path is NOT gated
3. Detail textures still draw — the detail path is NOT gated
4. Pan camera to newly revealed areas — first-frame recipe population works
5. No visual holes, seams, or missing tiles

- [ ] **Step 6: Tracy comparison**

Connect Tracy. Compare with fast path OFF vs ON (both with `MC2_PATCHSTREAM_THIN_RECORDS=1 MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1`). Expected:
- `quadSetupTextures` self-time decreases (no `addVertices(DRAWSOLID)` or `fillTerrainExtra` in hot loop)
- `Render.TerrainSolid` or equivalent legacy solid-draw zone shrinks/vanishes
- `PatchStream.DrawThinRecords.*` zones unchanged

Report actual before/after `quadSetupTextures` times.

- [ ] **Step 7: Fast-path smoke**

```bash
set MC2_PATCHSTREAM_THIN_RECORDS=1
set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0. All tier1 missions pass, menu canary clean.

---

## Self-Review

### Spec coverage

| Requirement | Task |
|-------------|------|
| `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1` env gate | Task 1 |
| `isFastPathActive()` requires all four conditions (fastPath + thinRecords + thinRecordsDraw + buf allocated) | Task 1 Step 2 |
| MC2_DRAWSOLID site count confirmed before editing | Task 2 Step 1 |
| Gate BOTTOMRIGHT tri1 solid draw | Task 2 Step 2 |
| Gate BOTTOMRIGHT tri2 solid draw | Task 2 Step 3 |
| Gate BOTTOMLEFT tri1 solid draw | Task 3 Step 1 |
| Gate BOTTOMLEFT tri2 solid draw | Task 3 Step 2 |
| All 4 gates confirmed after editing | Task 3 Step 3 |
| Leave overlay/detail/alpha/legacy paths alone | All tasks — only `if (terrainHandle!=0)` DRAWSOLID blocks touched |
| Standard-path smoke (no fast-path vars) | Task 4 Step 2 |
| Parity gate (thin record count matches) | Task 4 Steps 3–4 |
| Visual gate | Task 4 Step 5 |
| Fast-path smoke | Task 4 Step 7 |

### Correctness notes

- **gvTri1 capture is safe:** `gvTri1[3]` is captured from `gVertex[0..2]` before `if (pzTri1)`. Fast path doesn't change anything before that capture. `appendThinRecord` reads `gvTri1[i].argb/.frgb` which are populated by gVertex construction — unchanged.

- **Overlay frgb dependency:** The overlay draw does `memcpy(oVertex, gVertex, ...)` then reads `oVertex[k].frgb` for fog. `gVertex[k].frgb` is set in the gVertex construction block (before pzTri) — unchanged by the fast path. Safe.

- **appendQuad is already a no-op:** When thin records are active, `appendQuad` returns immediately at its internal guard. Fast path doesn't change this.

- **Env var spelling:** The trace env var is `MC2_PATCH_STREAM_TRACE` (underscore between PATCH and STREAM, unlike other `MC2_PATCHSTREAM_*` vars). This is how it is defined in `gos_terrain_patch_stream.cpp:309`. Do not use `MC2_PATCHSTREAM_TRACE`.

- **Scope of this milestone:** M1f skips `addVertices(DRAWSOLID)` and `fillTerrainExtra`. `gVertex` construction, diagonal logic, and `appendThinRecord` still run per quad. The next larger win requires skipping more of the triangle build, which is a bigger restructuring.
