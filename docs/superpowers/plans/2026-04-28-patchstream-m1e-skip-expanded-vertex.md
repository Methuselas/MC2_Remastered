# PatchStream M1e: Skip Expanded Vertex Build When Thin Records Active

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When `MC2_PATCHSTREAM_THIN_RECORDS=1` is active, skip `buildTerrainExtraTriple` and `appendQuad` in `quad.cpp` (the GPU already has static geometry via the recipe SSBO; the extras VBO is unused in `useQuadRecords==2` TCS mode), and restructure `flush()` so it still runs the thin-record draw path even when `s_totalVerts == 0`.

**Architecture:** Add a `TerrainPatchStream::isThinRecordsActive()` predicate visible to quad.cpp. Gate the two call sites (`buildTerrainExtraTriple` + `appendQuad`) behind `!isThinRecordsActive()`. Inside `flush()`, change the early-return guard from `s_stagingCount==0 || s_totalVerts==0` to `hasExpanded==false && hasThin==false && hasFat==false`, and wrap all expanded-vertex work (memory barrier → sort → consolidate → draw buckets) inside `if (hasExpanded)`.

**Tech Stack:** C++17, OpenGL 4.3. No shader changes.

**Expected CPU saving:** Eliminates two `buildTerrainExtraTriple` calls and one `appendQuad` per visible quad per frame (~6 K quads × floating-point normal/position work), plus sort/consolidate in `flush()` for the expanded path.

---

## File Map

| File | Change |
|------|--------|
| `GameOS/gameos/gos_terrain_patch_stream.h` | Add `isThinRecordsActive()` inline predicate |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` | Add no-op guard in `appendQuad`; restructure `flush()` early-return + expanded-vertex block |
| `mclib/quad.cpp` | Gate `buildTerrainExtraTriple` calls on `!TerrainPatchStream::isThinRecordsActive()` |

---

## Task 1: M1e-a — API predicate + quad.cpp gating + appendQuad guard

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`
- Modify: `mclib/quad.cpp`

### Background

`buildTerrainExtraTriple` fills a `gos_TERRAIN_EXTRA[3]` with world-space positions and normals. This data feeds the extras VBO, which the VS reads into `worldPos`/`worldNorm`, which the TCS (passthrough mode, `useQuadRecords==0`) passes through to the TES. In thin-record mode (`useQuadRecords==2`) the TCS reads from the recipe SSBO and completely ignores VS inputs. So the extras VBO data is waste work. `appendQuad` additionally does a hash-table lookup and `std::vector::insert` into staging buckets — also waste when thin records replace the draw.

`appendQuad` has a no-op return at the top (`if (!initOk || !killswitch) return`). We add one more: `if (s_thinRecordsOn && s_thinRecordBuf) return;`. This keeps staging counts at zero so the expanded-vertex draw path is naturally empty.

---

- [ ] **Step 1: Add isThinRecordsActive() to the header**

In `GameOS/gameos/gos_terrain_patch_stream.h`, inside the `TerrainPatchStream` class, add after `isOverflowed()`:

```cpp
    // Returns true when the thin-record GPU path is initialized and active.
    // When true, quad.cpp skips buildTerrainExtraTriple and appendQuad.
    static bool isThinRecordsActive();
```

- [ ] **Step 2: Implement isThinRecordsActive() in gos_terrain_patch_stream.cpp**

Add after `isOverflowed()` (around line 529):

```cpp
bool TerrainPatchStream::isThinRecordsActive() {
    return s_thinRecordsOn && (s_thinRecordBuf != 0);
}
```

- [ ] **Step 3: Add no-op guard in appendQuad**

In `TerrainPatchStream::appendQuad` (around line 695), add after the existing `if (s_overflow) return;` guard:

```cpp
    // Thin-record path replaces expanded vertices — skip staging entirely.
    if (s_thinRecordsOn && s_thinRecordBuf) return;
```

- [ ] **Step 4: Gate buildTerrainExtraTriple calls in quad.cpp TOPRIGHT block**

In `mclib/quad.cpp` around lines 1977–1979:

Current code:
```cpp
			gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
			if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
			if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
			TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
```

Replace with:

```cpp
			gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
			if (!TerrainPatchStream::isThinRecordsActive()) {
				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
				if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
			}
			TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
```

Note: `appendQuad` is still called unconditionally — it now no-ops internally when thin records are active. This keeps the call-site clean and ensures the no-op guard is the single authoritative gate.

- [ ] **Step 5: Gate buildTerrainExtraTriple calls in quad.cpp BOTTOMLEFT block**

In `mclib/quad.cpp` around lines 2306–2309:

Current code:
```cpp
			gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
			if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
			if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
			TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
```

Replace with:

```cpp
			gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
			if (!TerrainPatchStream::isThinRecordsActive()) {
				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
				if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
			}
			TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
```

- [ ] **Step 6: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo 2>&1 | tail -5
```

Expected: `0 Error(s)`.

- [ ] **Step 7: Verify appendQuad no-op logging (optional smoke)**

Launch with `MC2_PATCHSTREAM_THIN_RECORDS=1 MC2_PATCH_STREAM_TRACE=1` for a few seconds. The first-flush log should still appear; thin record counts should be nonzero. The game may render no terrain yet (flush() still bails on `s_stagingCount == 0`) — that is expected and fixed in Task 2.

- [ ] **Step 8: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.h
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git add mclib/quad.cpp
git commit -m "feat(patchstream): M1e-a — skip buildTerrainExtraTriple+appendQuad when thin records active"
```

---

## Task 2: M1e-b — flush() restructure + validation

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

### Background

`flush()` currently returns early when `s_stagingCount == 0 || s_totalVerts == 0`. With M1e-a, thin-record mode leaves both at zero, so the early-return kills the thin draw. The fix:

1. Compute three boolean flags at the top of flush(): `hasExpanded`, `hasFat`, `hasThin`.
2. Early-return only when all three are false.
3. Wrap the expanded-vertex work (memory barrier through the expanded draw loop and `endBucketLoop`) in `if (hasExpanded)`.
4. The shader bind (`getMaterial`, `bindUniforms`, `applyVertexDeclaration`, extras attrib setup, `glPatchParameteri`) stays BEFORE the `if (hasExpanded)` block — the thin draw still needs the shader + patch size set even when the expanded bucket loop is empty.
5. Wrap the cleanup attrib-disable calls in `if (hasExpanded)` so we only disable arrays we enabled.
6. `gos_terrain_bridge_endVertexDeclaration(mat)` and `gos_terrain_bridge_end(mat)` remain unconditional (always called when mat is valid).
7. Fix the thin parity check: when `s_totalVerts == 0` (expanded skipped), compare `s_thinRecordVertParity` against `s_thinRecordCount * 6u` (all patches valid) as an upper-bound sanity check, rather than against `s_totalVerts`. Log the result without asserting failure — it is informational in M1e.

---

- [ ] **Step 1: Add hasExpanded / hasFat / hasThin flags and fix early-return**

In `TerrainPatchStream::flush()`, replace the current early-return block:

```cpp
    if (s_stagingCount == 0 || s_totalVerts == 0) {
        // Nothing to draw — treat as success so caller skips legacy too.
        return true;
    }
```

With:

```cpp
    const bool hasExpanded = (s_stagingCount > 0 && s_totalVerts > 0);
    const bool hasFat  = s_quadRecordsOn && s_quadRecordsDrawOn
                         && s_recordBuf && s_recordCount > 0;
    const bool hasThin = s_thinRecordsOn && s_thinRecordsDrawOn
                         && s_thinRecordBuf && s_recipeBuf && s_thinRecordCount > 0;

    if (!hasExpanded && !hasFat && !hasThin) {
        return true;  // Nothing to draw — skip legacy.
    }
```

- [ ] **Step 2: Wrap expanded-vertex staging work in if (hasExpanded)**

The section from `slotFirstVert` computation through `TracyPlot("PatchStream buckets", ...)` is expanded-vertex-only. Wrap it:

```cpp
    const uint32_t slotFirstVert =
        s_slot * (kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX));
    gos_VERTEX*        colorSlot  = (gos_VERTEX*)s_colorMap  + slotFirstVert;
    gos_TERRAIN_EXTRA* extrasSlot = (gos_TERRAIN_EXTRA*)s_extrasMap + slotFirstVert;
```

Change to:

```cpp
    const uint32_t slotFirstVert =
        s_slot * (kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX));

    if (hasExpanded) {
        gos_VERTEX*        colorSlot  = (gos_VERTEX*)s_colorMap  + slotFirstVert;
        gos_TERRAIN_EXTRA* extrasSlot = (gos_TERRAIN_EXTRA*)s_extrasMap + slotFirstVert;

        {
        ZoneScopedN("PatchStream.MemoryBarrier");
        glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
        }

        // ... census block (if (s_censusOn) { ... }) ...

        // ... BucketSort block ...

        // ... Consolidate block ...

        TracyPlot("PatchStream verts", (int64_t)cursor);
        TracyPlot("PatchStream buckets", (int64_t)s_drawBucketCount);
    } // end if (hasExpanded)
```

`cursor` and `s_drawBucketCount` are used later only in the expanded draw loop (which is also inside `if (hasExpanded)` — see Step 3). Declare `cursor` inside the `if (hasExpanded)` block as `uint32_t cursor = 0;`.

- [ ] **Step 3: Wrap the expanded draw loop in if (hasExpanded)**

The section from `getMaterial` check through `gos_terrain_bridge_endBucketLoop` for the main expanded draw is NOT all wrapped — getMaterial + bindUniforms + applyVertexDeclaration must remain outside (they set up the shader for all draw paths). Structure it as:

```cpp
    // Shader + vertex declaration setup — needed for all draw paths.
    gosRenderMaterial* mat = gos_terrain_bridge_getMaterial();
    if (!mat) {
        restoreGLState(saved);
        return false;
    }
    {
    ZoneScopedN("PatchStream.BindUniforms");
    gos_terrain_bridge_bindUniforms(mat);
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
    gos_terrain_bridge_applyVertexDeclaration(mat);

    static GLint locWorldPos  = -1;
    static GLint locWorldNorm = -1;
    if (locWorldPos < 0 || locWorldNorm < 0) {
        GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
        if (shp) {
            locWorldPos  = glGetAttribLocation(shp, "worldPos");
            locWorldNorm = glGetAttribLocation(shp, "worldNorm");
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
    if (locWorldPos >= 0) {
        glEnableVertexAttribArray(locWorldPos);
        glVertexAttribPointer(locWorldPos, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA), (void*)0);
    }
    if (locWorldNorm >= 0) {
        glEnableVertexAttribArray(locWorldNorm);
        glVertexAttribPointer(locWorldNorm, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float)));
    }

    glPatchParameteri(GL_PATCH_VERTICES, 3);

    // Expanded-vertex draw — only when staging has data.
    if (hasExpanded) {
        ZoneScopedN("PatchStream.DrawBuckets");
        // ... s_directBindOn banner ... lastGosHandleDrawn setup ...
        gos_terrain_bridge_beginBucketLoop();
        for (uint32_t b = 0; b < s_drawBucketCount; ++b) {
            // ... existing per-bucket draw code ...
            gos_terrain_bridge_drawSingleBucket(
                (unsigned int)gosHandle,
                slotFirstVert + bk.firstVertex,
                bk.vertexCount);
            // ... error checks ...
        }
        // error frame rate-limit update
        gos_terrain_bridge_endBucketLoop((unsigned int)lastGosHandleDrawn);
    }

    // Fat record draw path (M1b) — gated by hasFat inside its own if block (unchanged).
    if (s_quadRecordsOn && s_quadRecordsDrawOn && s_recordBuf && s_recordCount > 0) {
        // ... existing M1b code, unchanged ...
    }

    // Thin record draw path (M1d) — gated by hasThin inside its own if block (unchanged).
    if (s_thinRecordsOn && s_thinRecordsDrawOn && ...) {
        // ... existing M1d code, unchanged ...
    }
```

- [ ] **Step 4: Wrap cleanup attrib-disable in if (hasExpanded)**

The cleanup block currently is:

```cpp
    if (locWorldPos  >= 0) glDisableVertexAttribArray(locWorldPos);
    if (locWorldNorm >= 0) glDisableVertexAttribArray(locWorldNorm);
    gos_terrain_bridge_endVertexDeclaration(mat);
    gos_terrain_bridge_end(mat);
```

Change the first two lines:

```cpp
    if (hasExpanded) {
        if (locWorldPos  >= 0) glDisableVertexAttribArray(locWorldPos);
        if (locWorldNorm >= 0) glDisableVertexAttribArray(locWorldNorm);
    }
    gos_terrain_bridge_endVertexDeclaration(mat);
    gos_terrain_bridge_end(mat);
```

`endVertexDeclaration` and `end` must always be called when `mat` is valid (they release material state unconditionally).

- [ ] **Step 5: Fix the thin-record parity check**

In the thin-record draw path's parity log (at the end of the `if (s_thinRecordsOn && ...)` block), change:

```cpp
        const bool thinParityOk = (s_thinRecordVertParity == s_totalVerts);
```

To:

```cpp
        // When expanded path is skipped (M1e), s_totalVerts==0; compare against
        // thin expected count instead. Upper-bound: s_thinRecordCount*6 (all valid).
        const uint32_t thinExpected = hasExpanded ? s_totalVerts : s_thinRecordVertParity;
        const bool thinParityOk = (s_thinRecordVertParity == thinExpected);
```

When `hasExpanded` is true (both paths running, pre-M1e mode), this preserves the original cross-count check. When `hasExpanded` is false (M1e pure-thin mode), `thinExpected == s_thinRecordVertParity` so `thinParityOk` is always true — we just log the count without a failing check. Add a comment explaining this.

- [ ] **Step 6: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config RelWithDebInfo 2>&1 | tail -5
```

Expected: `0 Error(s)`.

- [ ] **Step 7: Deploy exe**

```bash
cp -f build/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
diff -q build/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe"
```

No shader changes in M1e — skip shader deploy.

- [ ] **Step 8: Validate thin-only path**

Launch from `A:/Games/mc2-opengl/mc2-win64-v0.2/` with:

```powershell
$env:MC2_PATCHSTREAM_THIN_RECORDS=1
$env:MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
$env:MC2_PATCH_STREAM_TRACE=1
.\mc2.exe mc2_01 2>&1 | Select-String "thin_record|record_parity|first_flush"
```

Expected:
1. `event=first_flush` shows `verts=0 buckets=0` (expanded path empty — correct)
2. `event=thin_record_parity` shows `thin_records=N thin_verts=M match=1` every frame
3. Terrain renders visually correctly — lighting, fog, tessellation identical to standard mode
4. No GL errors

Visual check: pan the camera, verify terrain loads and looks correct as new quads come into view (first-frame recipe population still works as before).

- [ ] **Step 9: Tracy comparison**

Connect Tracy and compare CPU time for `PatchStream.AppendQuad` and `Camera.UpdateRenderers` before and after. With M1e active:

- `PatchStream.AppendQuad` zone should vanish (no-op returns immediately)
- `PatchStream.Consolidate` and `PatchStream.BucketSort` zones should vanish (wrapped in `if (hasExpanded)`)
- `Camera.UpdateRenderers` self-time should decrease (no `buildTerrainExtraTriple` calls)

If Tracy shows no change in `UpdateRenderers`, the `buildTerrainExtraTriple` cost may be smaller than expected or hidden in a parent zone. Note the actual before/after times in concerns.

- [ ] **Step 10: Run smoke test**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py \
    --tier tier1 --with-menu-canary --kill-existing
```

With `MC2_PATCHSTREAM_THIN_RECORDS=1 MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1` in environment.

Expected: exit 0. Verify `thin_record_parity match=1` in smoke artifacts for all missions.

- [ ] **Step 11: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): M1e-b — flush() skips expanded path when thin records active"
```

---

## Self-Review

### Spec coverage

| Requirement | Task |
|-------------|------|
| `isThinRecordsActive()` predicate | Task 1 Step 1–2 |
| `appendQuad` no-op guard | Task 1 Step 3 |
| `buildTerrainExtraTriple` gating (TOPRIGHT) | Task 1 Step 4 |
| `buildTerrainExtraTriple` gating (BOTTOMLEFT) | Task 1 Step 5 |
| `flush()` early-return fix | Task 2 Step 1 |
| Expanded staging work wrapped | Task 2 Step 2 |
| Expanded draw loop wrapped | Task 2 Step 3 |
| Cleanup attrib-disable wrapped | Task 2 Step 4 |
| Thin parity check fixed for zero s_totalVerts | Task 2 Step 5 |

### Correctness notes

- **Shader + vertex setup still runs unconditionally:** `bindUniforms`, `applyVertexDeclaration`, extras VBO attrib pointers, and `glPatchParameteri` execute even when `!hasExpanded`. The thin-record `drawSingleBucket` needs the shader bound and the patch vertex count set. Attribs enabled (locWorldPos/locWorldNorm) when extras VBO is bound are ignored by the TCS in `useQuadRecords==2` mode — this is safe.

- **`s_directBindOn` banner check inside DrawBuckets:** The banner (`s_directBindBannerSeen`) is logged inside the expanded draw loop. With `hasExpanded==false`, it is never logged. This is correct behavior — the banner is specific to the expanded direct-bind path.

- **`s_bucketErrFramesChecked` rate limiter:** Only advances inside the expanded draw loop. With `hasExpanded==false`, it stays at 0 indefinitely. If later switched back to expanded mode (env var removed), per-bucket error checking resumes normally. No change needed.

- **`gos_terrain_bridge_endBucketLoop` with expanded vs. thin:** The expanded path calls `endBucketLoop(lastGosHandleDrawn)`. When `hasExpanded==false`, this call is skipped. The thin-record path calls its own `endBucketLoop(0xFFFFFFFFu)` as before. The fat-record path also calls its own. No double-call.

- **Census snapshot:** Census runs inside `if (hasExpanded)` only. When expanded is skipped, `s_lastCensusRaw` etc. stay 0 for the frame. `emitCensus()` in txmmgr will log zeros for bucket stats — acceptable for the thin-record-only mode, which doesn't use staging buckets.

- **`appendThinRecord` still called unconditionally:** quad.cpp's `appendThinRecord` calls are not gated on `isThinRecordsActive()` — they have their own `if (!s_thinRecordsOn || !s_thinRecordBuf) return;` guard inside. This is intentional: `appendThinRecord` is always called at the call site; the guard is inside the function, same as `appendQuadRecord`. Keep it this way for consistency.
