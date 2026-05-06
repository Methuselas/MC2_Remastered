# PatchStream M0f: appendQuad Quad-Batched Append

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace two `appendTriangle` calls per terrain quad with one `appendQuad` call, halving bucket lookups and reducing vector-insert overhead with no change to pz semantics or rendered output.

**Architecture:** Add `appendQuad(terrainHandle, vColor1, vExtra1, tri1Valid, vColor2, vExtra2, tri2Valid)` to `TerrainPatchStream`. In `quad.cpp`, before each diagonal variant's destructive gVertex shuffle for triangle 2, save triangle 1's gVertex into a 96-byte stack copy, capture both pz booleans, strip `appendTriangle` from both per-triangle pz gates, and emit one `appendQuad` after both gates. The existing `appendTriangle` function is unchanged. No env-gate needed — `appendTriangle` remains as the fallback.

**Tech Stack:** C++14, Tracy profiler (ZoneScopedN), MSVC RelWithDebInfo

**Baseline (Tracy, ~209 frames, max zoom):**
- `PatchStream.Append.LookupBucket`: 141 µs (12 ns × 11,711 calls)
- `PatchStream.Append.InsertColor`: 236 µs (20 ns × 11,711)
- `PatchStream.Append.InsertExtras`: 247 µs (21 ns × 11,711)
- `PatchStream.AppendTriangle`: 599 µs (51 ns × 11,711)
- Expected: ~70 µs LookupBucket (halved to ~5,855 calls), ~150 µs InsertColor, ~155 µs InsertExtras — roughly −275 µs/frame

**Key files:**
- `GameOS/gameos/gos_terrain_patch_stream.h` — add `appendQuad` declaration
- `GameOS/gameos/gos_terrain_patch_stream.cpp` — implement `appendQuad`, keep `appendTriangle` unchanged
- `mclib/quad.cpp` — update diagonal A (TOPRIGHT) and diagonal B (BOTTOMLEFT) blocks

**Build:** `/mc2-build` skill, always `--config RelWithDebInfo`.
**Deploy:** `/mc2-deploy` skill (target: `A:/Games/mc2-opengl/mc2-win64-v0.2/`).
**Smoke gate:**
```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
```

---

## Task 1: Add `appendQuad` to PatchStream API and Implementation

Read both files before editing.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

---

- [ ] **Step 1: Add `appendQuad` declaration to `gos_terrain_patch_stream.h`**

  After the `appendTriangle` declaration (currently lines 41-43), add:

  ```cpp
  // Emit up to two triangles sharing the same terrain texture handle.
  // tri1Valid/tri2Valid carry the per-triangle pz gate result; a false flag
  // skips that triangle's vertex write without touching the bucket.
  // One bucket lookup when either triangle is valid; zero lookups when both
  // are clipped (early-out before findOrCreateStagingBucket).
  // vColor1/vExtra1 must point to 3 elements each; same for vColor2/vExtra2.
  static void appendQuad(DWORD terrainHandle,
                         const gos_VERTEX*        vColor1,
                         const gos_TERRAIN_EXTRA* vExtra1,
                         bool tri1Valid,
                         const gos_VERTEX*        vColor2,
                         const gos_TERRAIN_EXTRA* vExtra2,
                         bool tri2Valid);
  ```

  The exact old_string to find (lines 41-44):
  ```cpp
      static void appendTriangle(DWORD textureIndex,
                                 const gos_VERTEX* vColor,
                                 const gos_TERRAIN_EXTRA* vExtra);

      static bool flush();
  ```

  Replace with:
  ```cpp
      static void appendTriangle(DWORD textureIndex,
                                 const gos_VERTEX* vColor,
                                 const gos_TERRAIN_EXTRA* vExtra);

      // Emit up to two triangles sharing the same terrain texture handle in one
      // bucket lookup. tri1Valid/tri2Valid carry the per-triangle pz gate result;
      // a false flag skips that triangle's vertex write without touching the bucket.
      // One bucket lookup regardless of how many triangles are valid.
      // vColor1/vExtra1 must point to 3 elements each; same for vColor2/vExtra2.
      static void appendQuad(DWORD terrainHandle,
                             const gos_VERTEX*        vColor1,
                             const gos_TERRAIN_EXTRA* vExtra1,
                             bool tri1Valid,
                             const gos_VERTEX*        vColor2,
                             const gos_TERRAIN_EXTRA* vExtra2,
                             bool tri2Valid);

      static bool flush();
  ```

---

- [ ] **Step 2: Implement `appendQuad` in `gos_terrain_patch_stream.cpp`**

  Add the implementation immediately after the closing brace of `appendTriangle` (which ends at approximately the line containing `s_totalVerts += vertsPerTri;`). Find the end of `appendTriangle`:

  ```cpp
      s_totalVerts += vertsPerTri;
  }
  ```

  Replace with:

  ```cpp
      s_totalVerts += vertsPerTri;
  }

  void TerrainPatchStream::appendQuad(
      DWORD terrainHandle,
      const gos_VERTEX* vColor1, const gos_TERRAIN_EXTRA* vExtra1, bool tri1Valid,
      const gos_VERTEX* vColor2, const gos_TERRAIN_EXTRA* vExtra2, bool tri2Valid)
  {
      ZoneScopedN("PatchStream.AppendQuad");
      if (!s_initOk || !s_killswitch) return;
      if (s_overflow) return;
      if (!tri1Valid && !tri2Valid) return;  // zero lookups when both clipped

      const uint32_t numVerts = (tri1Valid ? 3u : 0u) + (tri2Valid ? 3u : 0u);
      const uint32_t maxVertsThisSlot =
          kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX);
      if (s_totalVerts + numVerts > maxVertsThisSlot) {
          fprintf(stderr,
              "[PATCH_STREAM v1] event=overflow slot=%u kind=byte_budget cursor=%u cap=%u\n",
              s_slot, s_totalVerts, maxVertsThisSlot);
          fflush(stderr);
          s_overflow = true;
          return;
      }

      PatchStagingBucket* bk = findOrCreateStagingBucket(terrainHandle);
      if (!bk) return;

      if (tri1Valid) {
          bk->color.insert(bk->color.end(), vColor1, vColor1 + 3);
          bk->extras.insert(bk->extras.end(), vExtra1, vExtra1 + 3);
      }
      if (tri2Valid) {
          bk->color.insert(bk->color.end(), vColor2, vColor2 + 3);
          bk->extras.insert(bk->extras.end(), vExtra2, vExtra2 + 3);
      }
      s_totalVerts += numVerts;
  }
  ```

---

- [ ] **Step 3: Build to verify `appendQuad` compiles in isolation**

  Run:
  ```bash
  CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
  ```

  Expected: zero errors. Only `gameos_graphics.cpp` (or nothing — depends on incremental state) and `gos_terrain_patch_stream.cpp` should recompile. The function is declared and defined but not yet called, so `quad.cpp` does not recompile here.

---

## Task 2: Update `quad.cpp` — Diagonal A (TOPRIGHT) and Diagonal B (BOTTOMLEFT)

Read `mclib/quad.cpp` before editing. The file has two diagonal variants, each with two triangles. We update both in this task.

**Files:**
- Modify: `mclib/quad.cpp`

---

### Diagonal A (TOPRIGHT variant)

Diagonal A sits inside `if (uvMode == TOPRIGHT)`. Triangle 1 uses vertices[0,1,2]; triangle 2 uses vertices[0,2,3].

- [ ] **Step 4: Remove `appendTriangle` from Diagonal A tri1 pz gate and add `pzTri1` + `gvTri1` save**

  Find this exact block (the "sebi: beware" comment is a unique anchor):

  ```cpp
  			if ((gVertex[0].z >= 0.0f) &&
  				(gVertex[0].z < 1.0f) &&
  				(gVertex[1].z >= 0.0f) &&
  				(gVertex[1].z < 1.0f) &&
  				(gVertex[2].z >= 0.0f) &&
  				(gVertex[2].z < 1.0f))
  			{
  				{
  					// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);

  						// Modern mirror — gated by killswitch via TerrainPatchStream::isReady().
  						// INSIDE the pz gate brace by construction. BR4.
  						if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  							gos_TERRAIN_EXTRA tx3[3];
  							buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx3);
  							TerrainPatchStream::appendTriangle(terrainHandle, gVertex, tx3);
  						}
  					}
  ```

  Replace with:

  ```cpp
  			// Capture tri1 pz result and vertex data before the destructive
  			// gVertex shuffle for tri2. appendQuad (below) uses both.
  			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
  			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
  			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
  			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
  			if (pzTri1)
  			{
  				{
  					// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
  						// PatchStream append moved to appendQuad after both pz gates.
  					}
  ```

---

- [ ] **Step 5: Remove `appendTriangle` from Diagonal A tri2 pz gate and add `pzTri2` + `appendQuad` call**

  Find this exact block (the "//Bottom Triangle" comment is the unique anchor):

  ```cpp
  			//--------------------------
  			//Bottom Triangle
  			//
  			// gVertex[0] same as above gVertex[0].
  			// gVertex[1] is same as above gVertex[2].
  			// gVertex[2] is calced from vertex[3].
  			DWORD lightRGB3 = vertices[3]->lightRGB;
  			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
  				lightRGB3 = 0xffffffff;

  			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

  			gVertex[1].x		= gVertex[2].x;	
  			gVertex[1].y		= gVertex[2].y;	
  			gVertex[1].z		= gVertex[2].z;	
  			gVertex[1].rhw		= gVertex[2].rhw;
  			gVertex[1].u		= gVertex[2].u;
  			gVertex[1].v		= gVertex[2].v;	
  			gVertex[1].argb		= gVertex[2].argb;
  			gVertex[1].frgb		= gVertex[2].frgb;

  			gVertex[2].x		= vertices[3]->px;
  			gVertex[2].y		= vertices[3]->py;
  			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
  			gVertex[2].rhw		= vertices[3]->pw;
  			gVertex[2].u		= minU;
  			gVertex[2].v		= maxV;
  			gVertex[2].argb		= lightRGB3;
  			gVertex[2].frgb		= vertices[3]->fogRGB;
  			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

  			if ((gVertex[0].z >= 0.0f) &&
  				(gVertex[0].z < 1.0f) &&
  				(gVertex[1].z >= 0.0f) &&
  				(gVertex[1].z < 1.0f) &&
  				(gVertex[2].z >= 0.0f) &&
  				(gVertex[2].z < 1.0f))
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);

  						// Modern mirror — gated by killswitch via TerrainPatchStream::isReady().
  						// INSIDE the pz gate brace by construction. BR4.
  						if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  							gos_TERRAIN_EXTRA tx3[3];
  							buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx3);
  							TerrainPatchStream::appendTriangle(terrainHandle, gVertex, tx3);
  						}
  					}
  ```

  Replace with:

  ```cpp
  			//--------------------------
  			//Bottom Triangle
  			//
  			// gVertex[0] same as above gVertex[0].
  			// gVertex[1] is same as above gVertex[2].
  			// gVertex[2] is calced from vertex[3].
  			DWORD lightRGB3 = vertices[3]->lightRGB;
  			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
  				lightRGB3 = 0xffffffff;

  			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

  			gVertex[1].x		= gVertex[2].x;	
  			gVertex[1].y		= gVertex[2].y;	
  			gVertex[1].z		= gVertex[2].z;	
  			gVertex[1].rhw		= gVertex[2].rhw;
  			gVertex[1].u		= gVertex[2].u;
  			gVertex[1].v		= gVertex[2].v;	
  			gVertex[1].argb		= gVertex[2].argb;
  			gVertex[1].frgb		= gVertex[2].frgb;

  			gVertex[2].x		= vertices[3]->px;
  			gVertex[2].y		= vertices[3]->py;
  			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
  			gVertex[2].rhw		= vertices[3]->pw;
  			gVertex[2].u		= minU;
  			gVertex[2].v		= maxV;
  			gVertex[2].argb		= lightRGB3;
  			gVertex[2].frgb		= vertices[3]->fogRGB;
  			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

  			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
  			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
  			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
  			if (pzTri2)
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
  						// PatchStream append moved to appendQuad below.
  					}
  ```

---

- [ ] **Step 6: Add `appendQuad` call after both Diagonal A pz gates**

  Find the end of the Diagonal A tri2 pz gate. The unique anchor is the closing sequence after the detail/overlay texture draws inside the TOPRIGHT branch:

  ```cpp
  						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
  					}
  				}
  			}
  		}
  		else if (uvMode == BOTTOMLEFT)
  ```

  Replace with:

  ```cpp
  						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
  					}
  				}
  			}

  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
  			}
  		}
  		else if (uvMode == BOTTOMLEFT)
  ```

---

### Diagonal B (BOTTOMLEFT variant)

Diagonal B sits inside `else if (uvMode == BOTTOMLEFT)`. Triangle 1 uses vertices[0,1,3]; triangle 2 uses vertices[1,2,3].

- [ ] **Step 7: Remove `appendTriangle` from Diagonal B tri1 pz gate and add `pzTri1` + `gvTri1` save**

  Find this exact block (unique because of `vertices[0], vertices[1], vertices[3]` in both `fillTerrainExtra` and `buildTerrainExtraTriple`):

  ```cpp
  			if ((gVertex[0].z >= 0.0f) &&
  				(gVertex[0].z < 1.0f) &&
  				(gVertex[1].z >= 0.0f) &&
  				(gVertex[1].z < 1.0f) &&
  				(gVertex[2].z >= 0.0f) &&
  				(gVertex[2].z < 1.0f))
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);

  						// Modern mirror — gated by killswitch via TerrainPatchStream::isReady().
  						// INSIDE the pz gate brace by construction. BR4.
  						if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  							gos_TERRAIN_EXTRA tx3[3];
  							buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx3);
  							TerrainPatchStream::appendTriangle(terrainHandle, gVertex, tx3);
  						}
  					}
  ```

  Replace with:

  ```cpp
  			// Capture tri1 pz result and vertex data before the destructive
  			// gVertex shuffle for tri2. appendQuad (below) uses both.
  			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
  			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
  			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
  			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
  			if (pzTri1)
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
  						// PatchStream append moved to appendQuad after both pz gates.
  					}
  ```

---

- [ ] **Step 8: Remove `appendTriangle` from Diagonal B tri2 pz gate and add `pzTri2` + `appendQuad` call**

  Find this exact block (unique because of `vertices[1], vertices[2], vertices[3]` in both `fillTerrainExtra` and `buildTerrainExtraTriple`, and the "Bottom Triangle / gVertex[0] is same as above gVertex[1]" comment):

  ```cpp
  			//---------------------------------------
  			// Bottom Triangle.
  			// gVertex[0] is same as above gVertex[1]
  			// gVertex[1] is new and calced from vertex[2].
  			// gVertex[2] is same as above.
  			DWORD lightRGB2 = vertices[2]->lightRGB;
  			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
  				lightRGB2 = 0xffffffff;

  			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

  			gVertex[0].x		= gVertex[1].x;	
  			gVertex[0].y		= gVertex[1].y;	
  			gVertex[0].z		= gVertex[1].z;	
  			gVertex[0].rhw		= gVertex[1].rhw;	
  			gVertex[0].u		= gVertex[1].u;	
  			gVertex[0].v		= gVertex[1].v;	
  			gVertex[0].argb		= gVertex[1].argb;
  			gVertex[0].frgb		= gVertex[1].frgb;

  			gVertex[1].x		= vertices[2]->px;
  			gVertex[1].y		= vertices[2]->py;
  			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
  			gVertex[1].rhw		= vertices[2]->pw;
  			gVertex[1].u		= maxU;
  			gVertex[1].v		= maxV;
  			gVertex[1].argb		= lightRGB2;
  			gVertex[1].frgb		= vertices[2]->fogRGB;
  			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

  			if ((gVertex[0].z >= 0.0f) &&
  				(gVertex[0].z < 1.0f) &&
  				(gVertex[1].z >= 0.0f) &&
  				(gVertex[1].z < 1.0f) &&
  				(gVertex[2].z >= 0.0f) &&
  				(gVertex[2].z < 1.0f))
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);

  						// Modern mirror — gated by killswitch via TerrainPatchStream::isReady().
  						// INSIDE the pz gate brace by construction. BR4.
  						if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  							gos_TERRAIN_EXTRA tx3[3];
  							buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx3);
  							TerrainPatchStream::appendTriangle(terrainHandle, gVertex, tx3);
  						}
  					}
  ```

  Replace with:

  ```cpp
  			//---------------------------------------
  			// Bottom Triangle.
  			// gVertex[0] is same as above gVertex[1]
  			// gVertex[1] is new and calced from vertex[2].
  			// gVertex[2] is same as above.
  			DWORD lightRGB2 = vertices[2]->lightRGB;
  			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
  				lightRGB2 = 0xffffffff;

  			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

  			gVertex[0].x		= gVertex[1].x;	
  			gVertex[0].y		= gVertex[1].y;	
  			gVertex[0].z		= gVertex[1].z;	
  			gVertex[0].rhw		= gVertex[1].rhw;	
  			gVertex[0].u		= gVertex[1].u;	
  			gVertex[0].v		= gVertex[1].v;	
  			gVertex[0].argb		= gVertex[1].argb;
  			gVertex[0].frgb		= gVertex[1].frgb;

  			gVertex[1].x		= vertices[2]->px;
  			gVertex[1].y		= vertices[2]->py;
  			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
  			gVertex[1].rhw		= vertices[2]->pw;
  			gVertex[1].u		= maxU;
  			gVertex[1].v		= maxV;
  			gVertex[1].argb		= lightRGB2;
  			gVertex[1].frgb		= vertices[2]->fogRGB;
  			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

  			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
  			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
  			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
  			if (pzTri2)
  			{
  				{
  					if(terrainHandle!=0) {
  						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
  						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
  						// PatchStream append moved to appendQuad below.
  					}
  ```

---

- [ ] **Step 9: Add `appendQuad` call after both Diagonal B pz gates**

  Find the closing-brace sequence that ends the BOTTOMLEFT block. The unique anchor is the 4-tab `}` that closes `if (pzTri2)`, followed by the 3-tab `}` that closes the `else if (uvMode == BOTTOMLEFT)` block, followed by the 2-tab `}` that closes the outer enclosing block, then a blank line and `#ifdef _DEBUG`. In the file after Step 8, this looks like (using `→` to show tabs):

  ```cpp
  			}
  		}
  	}

  #ifdef _DEBUG
  	if (selected )
  ```

  (4 tabs closes `if(pzTri2)`, 3 tabs closes BOTTOMLEFT, 2 tabs closes outer block.)

  This sequence is unique in the file because it is immediately followed by `#ifdef _DEBUG`. Replace with:

  ```cpp
  			}

  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
  			}
  		}
  	}

  #ifdef _DEBUG
  	if (selected )
  ```

  The `appendQuad` call goes at 3-tab indent (inside the BOTTOMLEFT block, after `if(pzTri2)` closes at 4-tab).

  > **If the Edit tool rejects the old_string:** Read the file around the `#ifdef _DEBUG` line (currently ~line 2308) and adjust indentation to match exactly. The structural intent is: append the appendQuad block immediately after the closing `}` of `if(pzTri2)`, before the closing `}` of the BOTTOMLEFT block.

---

## Task 3: Build, Smoke Gate, and Commit

- [ ] **Step 10: Build**

  ```bash
  CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
  ```

  Expected: zero errors. `gos_terrain_patch_stream.cpp` and `mclib/quad.cpp` recompile; the rest is incremental.

---

- [ ] **Step 11: Deploy**

  Use `/mc2-deploy` skill (or manually):
  ```bash
  WORKTREE="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.2"
  cp -f "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
  diff -q "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
  ```

  Expected: diff reports files are identical (no output = match).

---

- [ ] **Step 12: Smoke gate**

  ```bash
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
  ```

  Expected: exit 0, all tier1 missions PASS, menu canary PASS. Any FAIL is a hard blocker — do not commit.

---

- [ ] **Step 13: Visual validation**

  Launch the game with no extra env vars (standard path). Load `mc2_01`. This step is gated by the human — save it for last and stop here if you are running autonomously. The human will run the visual check and confirm before the commit step proceeds.

  When the human confirms, verify:
  - Pan camera from minimum zoom to maximum zoom and back
  - Pan across map edges and clip boundaries (corners of the playable area)
  - Look for: missing half-quads (one triangle of a pair disappearing), visible seams between same-texture quads, wrong texture on any patch
  - HUD and mech models render correctly after the terrain pass
  - No flickering at any zoom level

  If anything is wrong, the pzTri1/pzTri2 capture or the gvTri1 save is incorrect — report BLOCKED, do not commit.

---

- [ ] **Step 14: Commit**

  ```bash
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  git add GameOS/gameos/gos_terrain_patch_stream.h \
          GameOS/gameos/gos_terrain_patch_stream.cpp \
          mclib/quad.cpp
  git commit -m "perf: appendQuad halves bucket lookups in PatchStream terrain emit

  Replace two appendTriangle() calls per terrain quad with one appendQuad()
  call. One findOrCreateStagingBucket() lookup instead of two; per-triangle
  pz validity passed as bool flags so triangles at clip boundaries are still
  emitted independently. The 96-byte gvTri1 stack copy (before the in-place
  gVertex shuffle) is the only added cost per quad.

  Tracy baseline (209 frames, max zoom): LookupBucket 141 µs × 11,711 calls.
  Expected: ~71 µs × 5,855 calls. Total append path saving: ~275 µs/frame.

  appendTriangle() is unchanged. appendQuad() is the future seam where
  CPU-expanded terrain can become compact GPU-expanded records."
  ```

---

## Self-Review

**Spec coverage:**
- ✅ One bucket lookup when either triangle is valid; zero when both clipped: `appendQuad` early-returns before `findOrCreateStagingBucket` when `!tri1Valid && !tri2Valid`
- ✅ pz semantics preserved: each triangle's validity is captured from the same condition as before, passed as `tri1Valid`/`tri2Valid`
- ✅ Both diagonal variants updated: Diagonal A (TOPRIGHT, steps 4-6) and Diagonal B (BOTTOMLEFT, steps 7-9)
- ✅ `appendTriangle` unchanged: no call sites removed from other paths
- ✅ Legacy path (addVertices, fillTerrainExtra) unchanged inside pz gates
- ✅ `gvTri1` saved before destructive gVertex shuffle in both variants
- ✅ `tx1`/`tx2` zero-initialized: uninitialized reads impossible even if pzTri1/pzTri2 false

**Placeholder scan:** No TBD, TODO, or "similar to" references.

**Type consistency:**
- `appendQuad` header declares `DWORD terrainHandle` — matches the call sites in quad.cpp which pass `terrainHandle` (a `DWORD`)
- `appendQuad` impl calls `findOrCreateStagingBucket(terrainHandle)` — `findOrCreateStagingBucket` takes `DWORD textureIndex`; in this codebase the "textureIndex" arg IS the engine gosTextureHandle (a DWORD), so the type matches
- `gvTri1` is `const gos_VERTEX[3]`, passed to `vColor1` which is `const gos_VERTEX*` — correct
- `tx1`/`tx2` are `gos_TERRAIN_EXTRA[3]`, passed to `vExtra1`/`vExtra2` which are `const gos_TERRAIN_EXTRA*` — correct
- `pzTri1`/`pzTri2` are `bool`, matching `tri1Valid`/`tri2Valid` params — correct
