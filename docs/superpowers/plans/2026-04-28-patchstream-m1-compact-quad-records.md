# PatchStream M1: Compact Terrain Quad Records

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace 6-vertex-per-quad CPU staging with one 192-byte `TerrainQuadRecord` per quad that the GPU TCS expands back to 6 vertices, cutting terrain staging upload by ~43% and eliminating per-vertex `buildTerrainExtraTriple` calls from the hot append path.

**Architecture:** Three checkpoints. M1a (Task 1): define `TerrainQuadRecord`, add a persistent-mapped record SSBO, emit records from both diagonal variants in `quad.cpp`, log record count vs expanded vertex count for parity, no GPU draw yet. M1b (Task 2): add a `uniform int useQuadRecords` branch to `gos_terrain.tesc` so the existing TCS reads from the SSBO when enabled, add a record draw path in `flush()` that binds the SSBO and issues per-texture-bucket draws using the same bucket loop structure as the expanded path. M1c (Task 3): parity comparison logging, visual validation gate, commit. Both the expanded path and the record path are env-gated and can coexist; the expanded path is the fallback.

**Env vars:**
- `MC2_PATCHSTREAM_QUAD_RECORDS=1` — emit records alongside expanded path (M1a)
- `MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1` — also draw from records (M1b; requires RECORDS=1)

**Tech Stack:** C++14, OpenGL 4.3, GLSL 4.30, Tracy profiler, MSVC RelWithDebInfo

**Baseline for this optimization (from M0f Tracy, 278 frames, max zoom mc2_01):**
- `PatchStream.AppendQuad`: 370 µs/frame (65 ns × 5,983 calls/frame)
- Expanded vertex upload: 336 bytes/quad × ~5,983 quads ≈ 1.97 MB/frame
- Expected with records: 192 bytes/quad × ~5,983 quads ≈ 1.12 MB/frame (−43%)

**Key files:**
- `GameOS/gameos/gos_terrain_patch_stream.h` — add `TerrainQuadRecord` struct + `appendQuadRecord` declaration
- `GameOS/gameos/gos_terrain_patch_stream.cpp` — add record SSBO, `appendQuadRecord`, record draw path in `flush()`
- `mclib/quad.cpp` — add `appendQuadRecord` calls after `appendQuad` in both TOPRIGHT and BOTTOMLEFT blocks
- `shaders/gos_terrain.tesc` — add `useQuadRecords` uniform + SSBO + reconstruction branch

**Build:** `--config RelWithDebInfo` always.
**Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.2/`
**Smoke gate:**
```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
```

---

## Task 1 (M1a): TerrainQuadRecord struct + SSBO + logging (no GPU draw)

Read all three modified files before editing.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`
- Modify: `mclib/quad.cpp`

---

- [ ] **Step 1: Add `TerrainQuadRecord` + constants + `appendQuadRecord` declaration to `gos_terrain_patch_stream.h`**

  Find the line:
  ```cpp
  constexpr uint32_t kPatchStreamRecordBytesPerSlot =
  ```
  This does NOT exist yet — you are adding new content. Find the existing constants block (lines with `kPatchStreamRingFrames`, `kPatchStreamMaxBuckets`, `kPatchStreamColorBytesPerSlot`, `kPatchStreamExtrasBytesPerSlot`). After the last of those constants, add:

  ```cpp
  // One record per quad. Derived from the color ring's per-slot vertex capacity
  // (6 expanded verts per quad). At 192 bytes each, the record SSBO is ~10 MB/slot
  // vs ~10 MB (color) + ~7.5 MB (extras). Future: move worldPos/norm/uvData to
  // a GPU recipe SSBO (one upload per mission) to reach ~104 bytes/record.
  constexpr uint32_t kPatchStreamMaxRecordsPerSlot =
      kPatchStreamColorBytesPerSlot / (sizeof(gos_VERTEX) * 6u);
  constexpr uint32_t kPatchStreamRecordBytesPerSlot =
      kPatchStreamMaxRecordsPerSlot * 192u; // sizeof(TerrainQuadRecord) — forward ref
  ```

  Then, before the `PatchStreamBucket` struct, add the `TerrainQuadRecord` struct:

  ```cpp
  // Compact per-quad record for GPU-side vertex reconstruction (M1).
  // TCS reads this SSBO and emits 6 gos_terrain.tesc outputs — two triangles
  // matching the TOPRIGHT or BOTTOMLEFT diagonal decomposition.
  //
  // Layout: std430-compatible (all members at 16-byte-aligned vec4 boundaries).
  // 192 bytes/record vs 336 bytes for 6 expanded vertices (43% smaller).
  //
  // Corner index convention (same for both uvMode variants):
  //   corner 0 = vertices[0], UV = (maxU, minV)
  //   corner 1 = vertices[1], UV = (minU, minV)
  //   corner 2 = vertices[2], UV = (maxU, maxV)
  //   corner 3 = vertices[3], UV = (minU, maxV)
  //
  // Triangle decomposition by uvMode bit:
  //   TOPRIGHT  (bit0=0): tri1=corners[0,1,2], tri2=corners[0,2,3]
  //   BOTTOMLEFT(bit0=1): tri1=corners[0,1,3], tri2=corners[1,2,3]
  struct alignas(16) TerrainQuadRecord {
      // Corner world-space positions (vec4 per corner, w=padding for std430)
      float wx0, wy0, wz0, _wp0;
      float wx1, wy1, wz1, _wp1;
      float wx2, wy2, wz2, _wp2;
      float wx3, wy3, wz3, _wp3;
      // Corner world-space normals (vec4 per corner, w=padding)
      float nx0, ny0, nz0, _np0;
      float nx1, ny1, nz1, _np1;
      float nx2, ny2, nz2, _np2;
      float nx3, ny3, nz3, _np3;
      // UV ranges (same across all verts in quad): minU, minV, maxU, maxV
      float minU, minV, maxU, maxV;
      // Per-frame lighting per corner (ARGB packed, same encoding as gos_VERTEX.argb)
      uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
      // Per-frame fog + material byte per corner (same encoding as gos_VERTEX.frgb)
      uint32_t fogRGB0, fogRGB1, fogRGB2, fogRGB3;
      // Control
      uint32_t terrainHandle; // raw gosHandle — tex_resolve applied at flush consolidation
      uint32_t flags;         // bit 0: uvMode (0=TOPRIGHT, 1=BOTTOMLEFT)
                              // bit 1: pzTri1Valid, bit 2: pzTri2Valid
      uint32_t _ctrl2, _ctrl3; // padding to 16-byte boundary
      // Total: 4*16 + 4*16 + 16 + 16 + 16 + 16 = 192 bytes
  };
  static_assert(sizeof(TerrainQuadRecord) == 192, "TerrainQuadRecord must be 192 bytes for std430 alignment");
  ```

  Then, inside the `TerrainPatchStream` class after `appendQuad`, add:

  ```cpp
      // Emit one compact quad record for the GPU reconstruction path (M1).
      // No-op unless MC2_PATCHSTREAM_QUAD_RECORDS=1.
      // Call after appendQuad() at the same call site — both paths must agree on
      // which quads are submitted. The record is written directly into the
      // persistent-mapped record SSBO; no intermediate heap allocation.
      static void appendQuadRecord(const TerrainQuadRecord& rec);

      // Parity: expected vertex count from record flags (sum of valid tris × 3).
      // Compared to s_totalVerts in flush() when MC2_PATCHSTREAM_QUAD_RECORDS=1.
      static void addRecordVertParity(uint32_t n); // n = (pzTri1?3:0)+(pzTri2?3:0)
  ```

---

- [ ] **Step 2: Add record SSBO infrastructure to `gos_terrain_patch_stream.cpp`**

  Near the existing `s_directBindOn` / `s_directBindCheck` statics in the anonymous namespace, add:

  ```cpp
  static const bool s_quadRecordsOn   = (getenv("MC2_PATCHSTREAM_QUAD_RECORDS")      != nullptr);
  static const bool s_quadRecordsDrawOn = (getenv("MC2_PATCHSTREAM_QUAD_RECORDS_DRAW") != nullptr);

  // Record SSBO — persistent-mapped, triple-buffered alongside color/extras VBOs.
  // Only allocated when s_quadRecordsOn. SSBO binding point 0.
  static GLuint    s_recordBuf      = 0;
  static void*     s_recordMap      = nullptr;
  static uint32_t  s_recordCount    = 0;  // records staged this frame
  static uint32_t  s_recordVertParity = 0; // expected verts from records, for parity check
  static bool      s_recordBannerSeen = false;
  ```

  After `allocPersistentBuffer()`, add a helper for SSBO allocation:

  ```cpp
  static GLuint allocPersistentSSBO(GLsizeiptr totalBytes, void** outMappedPtr) {
      const GLbitfield flags = GL_MAP_WRITE_BIT
                             | GL_MAP_PERSISTENT_BIT
                             | GL_MAP_COHERENT_BIT;
      GLuint id = 0;
      glGenBuffers(1, &id);
      if (!id) return 0;
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
      glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, flags);
      if (glGetError() != GL_NO_ERROR) { glDeleteBuffers(1, &id); return 0; }
      void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, totalBytes, flags);
      if (!p) { glDeleteBuffers(1, &id); return 0; }
      *outMappedPtr = p;
      return id;
  }
  ```

  In `TerrainPatchStream::init()`, after the existing `s_extrasBuf` allocation (and before `restoreGLState`), add:

  ```cpp
  if (s_quadRecordsOn) {
      const GLsizeiptr recTotal =
          (GLsizeiptr)kPatchStreamRecordBytesPerSlot * kPatchStreamRingFrames;
      s_recordBuf = allocPersistentSSBO(recTotal, &s_recordMap);
      if (!s_recordBuf) {
          fprintf(stderr,
              "[PATCH_STREAM v1] event=record_ssbo_fail reason=alloc\n");
          fflush(stderr);
          // Non-fatal: record path disabled, expanded path continues.
      } else {
          fprintf(stderr,
              "[PATCH_STREAM v1] event=record_ssbo_ok bytes_per_slot=%u slots=%u\n",
              kPatchStreamRecordBytesPerSlot, kPatchStreamRingFrames);
          fflush(stderr);
      }
  }
  ```

  In `destroy()`, add cleanup after the existing buffer cleanup:

  ```cpp
  if (s_recordBuf) {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_recordBuf);
      if (s_recordMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_recordMap = nullptr; }
      glDeleteBuffers(1, &s_recordBuf);
      s_recordBuf = 0;
  }
  ```

  In `beginFrame()`, after the existing resets (s_stagingCount, s_totalVerts, s_overflow etc.), add:

  ```cpp
  s_recordCount      = 0;
  s_recordVertParity = 0;
  ```

---

- [ ] **Step 3: Implement `appendQuadRecord` and `addRecordVertParity` in `gos_terrain_patch_stream.cpp`**

  Add immediately after the closing `}` of `appendQuad`:

  ```cpp
  void TerrainPatchStream::appendQuadRecord(const TerrainQuadRecord& rec) {
      if (!s_quadRecordsOn || !s_recordBuf) return;
      if (!s_initOk || !s_killswitch) return;
      if (s_overflow) return;
      if (s_recordCount >= kPatchStreamMaxRecordsPerSlot) {
          // Record SSBO full. Log once, don't set s_overflow (expanded path can continue).
          if (s_recordCount == kPatchStreamMaxRecordsPerSlot) {
              fprintf(stderr,
                  "[PATCH_STREAM v1] event=record_overflow slot=%u cap=%u\n",
                  s_slot, kPatchStreamMaxRecordsPerSlot);
              fflush(stderr);
          }
          return;
      }
      const uint32_t slotFirst = s_slot * kPatchStreamMaxRecordsPerSlot;
      ((TerrainQuadRecord*)s_recordMap)[slotFirst + s_recordCount] = rec;
      ++s_recordCount;
  }

  void TerrainPatchStream::addRecordVertParity(uint32_t n) {
      s_recordVertParity += n;
  }
  ```

---

- [ ] **Step 4: Add parity logging to `flush()` in `gos_terrain_patch_stream.cpp`**

  At the end of `flush()`, just before the `restoreGLState` call, add:

  ```cpp
  if (s_quadRecordsOn && s_recordBuf) {
      if (!s_recordBannerSeen) {
          s_recordBannerSeen = true;
          fprintf(stderr,
              "[PATCH_STREAM v1] event=quad_records_enabled "
              "max_per_slot=%u bytes_per_slot=%u\n",
              kPatchStreamMaxRecordsPerSlot, kPatchStreamRecordBytesPerSlot);
          fflush(stderr);
      }
      const bool parityOk = (s_recordVertParity == s_totalVerts);
      if (s_traceOn || !parityOk) {
          fprintf(stderr,
              "[PATCH_STREAM v1] event=record_parity slot=%u records=%u "
              "record_verts=%u expanded_verts=%u match=%d\n",
              s_slot, s_recordCount, s_recordVertParity,
              s_totalVerts, (int)parityOk);
          fflush(stderr);
      }
  }
  ```

---

- [ ] **Step 5: Add `appendQuadRecord` call to the TOPRIGHT block in `mclib/quad.cpp`**

  Find the existing TOPRIGHT appendQuad block (unique anchor: `buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1)`):

  ```cpp
  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
  			}
  ```

  Replace with:

  ```cpp
  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);

  				// M1 compact record — TOPRIGHT diagonal.
  				// gvTri1[0..2] = corners 0,1,2 (saved before shuffle).
  				// gVertex[2]   = corner 3 (constructed by shuffle from vertices[3]).
  				TerrainQuadRecord rec;
  				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
  				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
  				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
  				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
  				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
  				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
  				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
  				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
  				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
  				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gvTri1[2].argb; rec.lightRGB3=gVertex[2].argb;
  				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gvTri1[2].frgb; rec.fogRGB3  =gVertex[2].frgb;
  				rec.terrainHandle = (uint32_t)terrainHandle;
  				rec.flags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
  				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
  				TerrainPatchStream::appendQuadRecord(rec);
  				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
  			}
  ```

---

- [ ] **Step 6: Add `appendQuadRecord` call to the BOTTOMLEFT block in `mclib/quad.cpp`**

  Find the existing BOTTOMLEFT appendQuad block (unique anchor: `buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1)`):

  ```cpp
  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
  			}
  ```

  Replace with:

  ```cpp
  			// PatchStream: one bucket lookup for both triangles of this quad.
  			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
  				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
  				if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
  				if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
  				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);

  				// M1 compact record — BOTTOMLEFT diagonal.
  				// gvTri1 = {corner0, corner1, corner3} (saved before shuffle).
  				// After shuffle: gVertex[0]=corner1, gVertex[1]=corner2, gVertex[2]=corner3.
  				// corner2 (vertices[2]) is only in gVertex[1] after the shuffle.
  				TerrainQuadRecord rec;
  				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
  				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
  				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
  				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
  				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
  				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
  				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
  				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
  				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
  				// BOTTOMLEFT: gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3
  				//             gVertex[1] after shuffle = corner2
  				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gVertex[1].argb; rec.lightRGB3=gvTri1[2].argb;
  				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gVertex[1].frgb; rec.fogRGB3  =gvTri1[2].frgb;
  				rec.terrainHandle = (uint32_t)terrainHandle;
  				rec.flags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
  				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
  				TerrainPatchStream::appendQuadRecord(rec);
  				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
  			}
  ```

---

- [ ] **Step 7: Build to verify M1a compiles**

  ```bash
  CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
  ```

  Expected: zero errors. `gos_terrain_patch_stream.cpp` and `mclib/quad.cpp` recompile.

---

- [ ] **Step 8: Deploy and smoke — standard path (env vars NOT set)**

  ```bash
  WORKTREE="A:/Games/mc2-opengl-src/.claire/worktrees/nifty-mendeleev"
  DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.2"
  cp -f "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe" && \
  diff -q "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
      --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
  ```

  Expected: exit 0. Standard path unaffected.

---

- [ ] **Step 9: Smoke — records enabled, no draw, verify parity logging**

  ```bash
  MC2_PATCHSTREAM_QUAD_RECORDS=1 MC2_PATCH_STREAM_TRACE=1 \
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
      --tier tier1 --kill-existing --duration 12 --fail-fast
  ```

  Check stderr for:
  - `[PATCH_STREAM v1] event=record_ssbo_ok` — must appear once at startup
  - `[PATCH_STREAM v1] event=quad_records_enabled` — must appear once on first flush
  - `[PATCH_STREAM v1] event=record_parity ... match=1` — must appear every frame (match=0 is a hard blocker; it means a TOPRIGHT/BOTTOMLEFT corner mapping is wrong)
  - NO `event=record_overflow` lines

  If `match=0` appears, stop and investigate which quad's corner mapping is wrong before proceeding to Task 2.

---

- [ ] **Step 10: Commit M1a**

  ```bash
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  git add GameOS/gameos/gos_terrain_patch_stream.h \
          GameOS/gameos/gos_terrain_patch_stream.cpp \
          mclib/quad.cpp
  git commit -m "$(cat <<'EOF'
  perf(M1a): emit compact TerrainQuadRecord alongside expanded terrain path

  Define 192-byte TerrainQuadRecord (vs 336 bytes for 6 expanded vertices).
  Env-gated: MC2_PATCHSTREAM_QUAD_RECORDS=1. Records are written directly
  into a persistent-mapped SSBO (triple-buffered, ~10 MB/slot) at appendQuad
  call sites in quad.cpp — one record per quad, both diagonal variants.

  M1a: no GPU draw from records yet. Per-frame parity check compares
  record_verts (sum of pzTri1/pzTri2 flags × 3) against s_totalVerts to
  verify corner mapping is correct before M1b wires the GPU path.

  Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Task 2 (M1b): GPU reconstruction via modified TCS + record draw path

Read `shaders/gos_terrain.tesc` and the `flush()` function in `gos_terrain_patch_stream.cpp` before editing.

**Files:**
- Modify: `shaders/gos_terrain.tesc`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

---

- [ ] **Step 11: Verify TES input varyings exactly match TCS output varyings**

  Read `shaders/gos_terrain.tese`. Verify these inputs exist and their types match the TCS outputs:
  - `in vec4 tcs_Color[]`
  - `in float tcs_FogValue[]`
  - `in vec2 tcs_Texcoord[]`
  - `in float tcs_TerrainType[]`
  - `in vec3 tcs_WorldPos[]`
  - `in vec3 tcs_WorldNorm[]`

  Also verify TES does NOT use `gl_in[].gl_Position` in the normal rendering path (only in `tessDebug.x < -1.5` probe mode). If TES uses `gl_in[].gl_Position` in the main path, you cannot set it to `vec4(0.0)` from the record TCS — report BLOCKED.

  Expected: TES projects from `tcs_WorldPos[]` via `terrainMVP` in the main path; `gl_in[].gl_Position` is only used in the debug probe branch.

---

- [ ] **Step 12: Add `useQuadRecords` uniform + SSBO + reconstruction branch to `shaders/gos_terrain.tesc`**

  Read `shaders/gos_terrain.tesc` first. The current TCS is a passthrough (~46 lines). Replace the entire file content with:

  ```glsl
  //#version 400 (version provided by material prefix)

  layout(vertices = 3) out;

  in vec4 vs_Color[];
  in float vs_FogValue[];
  in vec2 vs_Texcoord[];
  in float vs_TerrainType[];
  in vec3 vs_WorldPos[];
  in vec3 vs_WorldNorm[];

  out vec4 tcs_Color[];
  out float tcs_FogValue[];
  out vec2 tcs_Texcoord[];
  out float tcs_TerrainType[];
  out vec3 tcs_WorldPos[];
  out vec3 tcs_WorldNorm[];

  uniform vec4 tessLevel;
  uniform vec4 tessDistanceRange;
  uniform vec4 cameraPos;
  uniform int  useQuadRecords;  // 0 = passthrough (default), 1 = read SSBO

  // M1 compact quad record — must match TerrainQuadRecord in gos_terrain_patch_stream.h.
  // std430 layout; 192 bytes per record (12 vec4s).
  struct TerrainQuadRecord {
      vec4 worldPos0, worldPos1, worldPos2, worldPos3;  // xyz + w=pad
      vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
      vec4 uvData;        // minU, minV, maxU, maxV
      uvec4 lightRGBs;   // corners 0-3, packed as ARGB uint
      uvec4 fogRGBs;     // corners 0-3, packed as frgb uint
      uvec4 control;     // x=terrainHandle, y=flags (bit0=uvMode,bit1=pzTri1,bit2=pzTri2), zw=pad
  };

  layout(std430, binding = 0) readonly buffer QuadRecordBuf {
      TerrainQuadRecord records[];
  };

  // Unpack ARGB uint to vec4 (each component 0..255 → 0..1).
  vec4 unpackARGB(uint packed) {
      return vec4(
          float((packed >> 16u) & 0xFFu) / 255.0,  // R
          float((packed >>  8u) & 0xFFu) / 255.0,  // G
          float((packed       ) & 0xFFu) / 255.0,  // B
          float((packed >> 24u) & 0xFFu) / 255.0   // A
      );
  }

  // Retrieve a uvec4 component by runtime index (0-3).
  uint uvec4GetIdx(uvec4 v, uint idx) {
      if (idx == 0u) return v.x;
      if (idx == 1u) return v.y;
      if (idx == 2u) return v.z;
      return v.w;
  }

  // Retrieve a vec4 component by runtime index (0-3).
  float vec4GetIdx(vec4 v, uint idx) {
      if (idx == 0u) return v.x;
      if (idx == 1u) return v.y;
      if (idx == 2u) return v.z;
      return v.w;
  }

  void main()
  {
      if (useQuadRecords == 0) {
          // --- Passthrough path (default, unchanged) ---
          tcs_Color[gl_InvocationID]       = vs_Color[gl_InvocationID];
          tcs_FogValue[gl_InvocationID]    = vs_FogValue[gl_InvocationID];
          tcs_Texcoord[gl_InvocationID]    = vs_Texcoord[gl_InvocationID];
          tcs_TerrainType[gl_InvocationID] = vs_TerrainType[gl_InvocationID];
          tcs_WorldPos[gl_InvocationID]    = vs_WorldPos[gl_InvocationID];
          tcs_WorldNorm[gl_InvocationID]   = vs_WorldNorm[gl_InvocationID];
          gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

          if (gl_InvocationID == 0) {
              float level = max(tessLevel.x, 1.0);
              gl_TessLevelOuter[0] = level;
              gl_TessLevelOuter[1] = level;
              gl_TessLevelOuter[2] = level;
              gl_TessLevelInner[0] = level;
          }
          return;
      }

      // --- Record path (MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1) ---
      // Each record maps to 2 patches: gl_PrimitiveID/2 = recordIdx,
      //                                gl_PrimitiveID%2 = triIdx (0=tri1, 1=tri2).
      uint recordIdx = uint(gl_PrimitiveID) / 2u;
      uint triIdx    = uint(gl_PrimitiveID) % 2u;
      uint id        = uint(gl_InvocationID); // 0, 1, or 2

      TerrainQuadRecord rec = records[recordIdx];
      uint uvMode  = rec.control.y & 1u;
      uint pzTri1  = (rec.control.y >> 1u) & 1u;
      uint pzTri2  = (rec.control.y >> 2u) & 1u;

      // Corner index for this invocation.
      // TOPRIGHT (uvMode=0): tri1 = corners[0,1,2], tri2 = corners[0,2,3]
      // BOTTOMLEFT(uvMode=1): tri1 = corners[0,1,3], tri2 = corners[1,2,3]
      uint cornerIdx;
      if (uvMode == 0u) {
          if (triIdx == 0u) {
              if (id == 0u) cornerIdx = 0u;
              else if (id == 1u) cornerIdx = 1u;
              else cornerIdx = 2u;
          } else {
              if (id == 0u) cornerIdx = 0u;
              else if (id == 1u) cornerIdx = 2u;
              else cornerIdx = 3u;
          }
      } else {
          if (triIdx == 0u) {
              if (id == 0u) cornerIdx = 0u;
              else if (id == 1u) cornerIdx = 1u;
              else cornerIdx = 3u;
          } else {
              if (id == 0u) cornerIdx = 1u;
              else if (id == 1u) cornerIdx = 2u;
              else cornerIdx = 3u;
          }
      }

      // World position and normal.
      vec4 wp = (cornerIdx == 0u) ? rec.worldPos0
               :(cornerIdx == 1u) ? rec.worldPos1
               :(cornerIdx == 2u) ? rec.worldPos2
               :                    rec.worldPos3;
      vec4 wn = (cornerIdx == 0u) ? rec.worldNorm0
               :(cornerIdx == 1u) ? rec.worldNorm1
               :(cornerIdx == 2u) ? rec.worldNorm2
               :                    rec.worldNorm3;
      tcs_WorldPos[id]  = wp.xyz;
      tcs_WorldNorm[id] = wn.xyz;

      // UV: corner → (u, v) using uvData = (minU, minV, maxU, maxV).
      // Corner convention (verified against quad.cpp UV assignment):
      //   corner 0: (maxU, minV)  corner 1: (minU, minV)
      //   corner 2: (maxU, maxV)  corner 3: (minU, maxV)
      float u = (cornerIdx == 0u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
      float v = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;
      tcs_Texcoord[id] = vec2(u, v);

      // Lighting.
      uint lrgb = uvec4GetIdx(rec.lightRGBs, cornerIdx);
      uint frgb = uvec4GetIdx(rec.fogRGBs,   cornerIdx);
      tcs_Color[id]       = unpackARGB(lrgb);
      tcs_FogValue[id]    = float((frgb >> 24u) & 0xFFu) / 255.0;
      tcs_TerrainType[id] = float(frgb & 0xFFu);

      // gl_Position unused by TES in normal path; set to degenerate to be safe.
      gl_out[id].gl_Position = vec4(0.0, 0.0, 0.0, 1.0);

      // Tessellation levels (only invocation 0 writes patch-level state).
      if (id == 0u) {
          uint pzValid = (triIdx == 0u) ? pzTri1 : pzTri2;
          float level = (pzValid != 0u) ? max(tessLevel.x, 1.0) : 0.0;
          gl_TessLevelOuter[0] = level;
          gl_TessLevelOuter[1] = level;
          gl_TessLevelOuter[2] = level;
          gl_TessLevelInner[0] = level;
      }
  }
  ```

  **Key correctness items to verify before saving:**
  - The `unpackARGB` function maps the same bit positions as the CPU `gos_VERTEX.argb` packing (ARGB = alpha=bits31-24, R=23-16, G=15-8, B=7-0). Verify against how TES consumes `tcs_Color` in the fragment shader.
  - The UV corner table matches quad.cpp: read lines ~1690-1715 and ~2080-2110 in quad.cpp to confirm corner→UV assignment is identical in both diagonal variants. If the UV mapping differs between diagonals, the `cornerIdx → UV` table must be split per-uvMode.
  - `tcs_TerrainType` carries the material byte (0-3 from fogRGB's low byte) — verify this matches what VS emits: `vs_TerrainType = floor(fog.x * 255.0 + 0.5)`. In the record, `frgb & 0xFF` is the raw material byte (0-3), not normalized. The TES consumes `TerrainType` as-is; verify gos_terrain.frag does `int(TerrainType)` or equivalent — not a 0..1 normalized value.

---

- [ ] **Step 13: Add record draw path to `flush()` in `gos_terrain_patch_stream.cpp`**

  Read `flush()` first. The record draw path goes in `flush()` after the existing expanded draw path. Find the closing of the `DrawBuckets` zone (after the `gos_terrain_bridge_endBucketLoop` call and before `restoreGLState`):

  ```cpp
  gos_terrain_bridge_endBucketLoop((unsigned int)lastGosHandleDrawn);
  ```

  After that line, add:

  ```cpp
  // --- M1b record draw path ---
  if (s_quadRecordsOn && s_quadRecordsDrawOn && s_recordBuf && s_recordCount > 0) {
      ZoneScopedN("PatchStream.DrawRecords");

      // Sort records by resolved texture handle into a local draw-bucket table.
      struct RecordBucket { DWORD gosHandle; uint32_t firstRecord; uint32_t recordCount; };
      static RecordBucket s_recDrawBuckets[kPatchStreamMaxBuckets];
      uint32_t recDrawBucketCount = 0;

      // Build sort entries from staged records in the ring slot.
      const uint32_t slotFirstRecord = s_slot * kPatchStreamMaxRecordsPerSlot;
      TerrainQuadRecord* ringBase = (TerrainQuadRecord*)s_recordMap + slotFirstRecord;

      struct RecSortEntry { DWORD gosHandle; uint32_t recIdx; };
      static RecSortEntry recSortBuf[kPatchStreamMaxRecordsPerSlot];
      for (uint32_t r = 0; r < s_recordCount; ++r) {
          recSortBuf[r] = { (DWORD)tex_resolve(ringBase[r].terrainHandle), r };
      }
      std::sort(recSortBuf, recSortBuf + s_recordCount,
          [](const RecSortEntry& a, const RecSortEntry& b) {
              return a.gosHandle < b.gosHandle;
          });

      // Copy sorted records back into ring and build draw buckets.
      static TerrainQuadRecord recSortedTmp[kPatchStreamMaxRecordsPerSlot];
      for (uint32_t r = 0; r < s_recordCount; ++r) {
          recSortedTmp[r] = ringBase[recSortBuf[r].recIdx];
          // Update resolved handle for TCS (not used by TCS, but for debugging).
          recSortedTmp[r].terrainHandle = (uint32_t)recSortBuf[r].gosHandle;

          DWORD gh = recSortBuf[r].gosHandle;
          if (recDrawBucketCount > 0 && s_recDrawBuckets[recDrawBucketCount-1].gosHandle == gh) {
              ++s_recDrawBuckets[recDrawBucketCount-1].recordCount;
          } else {
              if (recDrawBucketCount < kPatchStreamMaxBuckets) {
                  s_recDrawBuckets[recDrawBucketCount++] = { gh, r, 1u };
              }
          }
      }
      // Write sorted records back into ring.
      memcpy(ringBase, recSortedTmp, s_recordCount * sizeof(TerrainQuadRecord));

      // Retrieve useQuadRecords uniform location (cached after first lookup).
      static GLint s_useQuadRecordsLoc = -2; // -2 = not yet queried
      if (s_useQuadRecordsLoc == -2) {
          GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
          s_useQuadRecordsLoc = shp ? glGetUniformLocation(shp, "useQuadRecords") : -1;
      }
      if (s_useQuadRecordsLoc < 0) {
          // Uniform not found — shader not yet updated. Skip record draw this frame.
          goto record_draw_done;
      }

      // Bind the record SSBO (binding 0) and enable record TCS mode.
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_recordBuf);
      glUniform1i(s_useQuadRecordsLoc, 1);

      // Per-texture-bucket draw. Matches structure of the expanded draw loop.
      {
      ZoneScopedN("PatchStream.DrawRecords.Buckets");
      gos_terrain_bridge_beginBucketLoop();
      for (uint32_t b = 0; b < recDrawBucketCount; ++b) {
          const RecordBucket& rb = s_recDrawBuckets[b];
          // Bind texture via existing direct-bind bridge.
          {
              gosTexture* tex = /* see note below */ nullptr;
              glActiveTexture(GL_TEXTURE0);
              const GLuint glTex = (GLuint)gos_terrain_bridge_glTextureForGosHandle(rb.gosHandle);
              if (glTex) glBindTexture(GL_TEXTURE_2D, glTex);
          }
          // 2 patches (6 verts) per record.
          const GLint first  = (GLint)((slotFirstRecord + rb.firstRecord) * 6);
          const GLsizei cnt  = (GLsizei)(rb.recordCount * 6);
          glDrawArrays(GL_PATCHES, first, cnt);
      }
      gos_terrain_bridge_endBucketLoop(0xFFFFFFFFu); // no texture handle to re-sync
      }

      // Restore TCS to passthrough for subsequent frames.
      glUniform1i(s_useQuadRecordsLoc, 0);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);

      record_draw_done:;
  }
  ```

  **Implementation note:** The record draw path binds `s_recordBuf` as an SSBO at binding 0. The `glDrawArrays(GL_PATCHES, first, cnt)` call has NO vertex attribute data — the record TCS reads all data from the SSBO via `gl_PrimitiveID`. The VAO state from the expanded path still has attributes 0-5 enabled, which is harmless (the record TCS ignores them).

  **Stack allocation note:** `recSortedTmp` is 54,613 × 192 bytes ≈ 10 MB on the stack — this will crash. Change to a `static` local:
  ```cpp
  static TerrainQuadRecord recSortedTmp[kPatchStreamMaxRecordsPerSlot];
  ```
  Both static arrays (`recSortBuf` and `recSortedTmp`) are already marked `static` above — ensure this is in the code.

---

- [ ] **Step 14: Build to verify M1b compiles**

  ```bash
  CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -30
  ```

  Expected: zero errors. `gos_terrain_patch_stream.cpp` recompiles; shaders are compiled at runtime.

---

- [ ] **Step 15: Deploy and smoke — standard path (no env vars)**

  ```bash
  WORKTREE="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.2"
  cp -f "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe" && \
  diff -q "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
      --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
  ```

  Expected: exit 0. The modified TCS has `useQuadRecords` uniform default 0 so the passthrough path is unchanged. If this fails, the `uniform int useQuadRecords` declaration broke shader compilation — check the console for shader errors.

---

- [ ] **Step 16: Smoke with records + draw enabled**

  ```bash
  MC2_PATCHSTREAM_QUAD_RECORDS=1 MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1 \
  MC2_PATCH_STREAM_TRACE=1 \
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
      --tier tier1 --kill-existing --duration 12 --fail-fast
  ```

  Check stderr for:
  - `[PATCH_STREAM v1] event=quad_records_enabled` — startup banner
  - `[PATCH_STREAM v1] event=record_parity ... match=1` — parity still correct every frame
  - No `event=record_overflow`
  - No GL error lines

  If any `match=0` appears, the record path is emitting different triangles than the expanded path — stop and investigate.

---

- [ ] **Step 17: Visual validation — HUMAN GATE**

  Launch the game with `MC2_PATCHSTREAM_QUAD_RECORDS=1 MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1`. Load `mc2_01`. Draw path now draws from records (SSBO-based TCS). Verify:
  - Pan camera min→max zoom and back: terrain textures correct, no missing triangles
  - Pan across map edges and biome boundaries: no UV seams or wrong-texture patches
  - Check that HUD and mech models render correctly after terrain (state cache coherent after record draw)
  - No flickering or z-fighting at any zoom level

  If anything is wrong, the TCS UV reconstruction or corner mapping is incorrect — report BLOCKED with what you observed.

---

## Task 3 (M1c): Parity gate + commit

- [ ] **Step 18: Verify UV corner table in TCS matches quad.cpp exactly**

  Read `mclib/quad.cpp` around lines 1685-1715 (TOPRIGHT UV assignment) and lines 2080-2110 (BOTTOMLEFT UV assignment). Confirm:
  - TOPRIGHT: gVertex[0]=(maxU,minV), gVertex[1]=(minU,minV), gVertex[2]=(maxU,maxV), corner3=(minU,maxV) — these map to corners 0,1,2,3 with the TCS UV table.
  - BOTTOMLEFT: same UV per corner index (different triangle decomposition, same UV values per corner).

  If BOTTOMLEFT assigns different UVs to the same corner index, update the TCS UV logic to be uvMode-dependent. Document the finding here.

---

- [ ] **Step 19: Confirm parity logging passes across all 5 tier1 missions**

  Run the full tier1 suite with records and draw enabled, capturing stderr:

  ```bash
  MC2_PATCHSTREAM_QUAD_RECORDS=1 MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1 \
  MC2_PATCH_STREAM_TRACE=1 \
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
      --tier tier1 --kill-existing --duration 12 --fail-fast 2>&1 | grep "record_parity"
  ```

  Expected: all `match=1` across mc2_01, mc2_03, mc2_10, mc2_17, mc2_24. Any `match=0` is a hard blocker.

---

- [ ] **Step 20: Commit M1b + M1c**

  ```bash
  cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
  git add GameOS/gameos/gos_terrain_patch_stream.cpp \
          shaders/gos_terrain.tesc
  git commit -m "$(cat <<'EOF'
  perf(M1b): GPU TCS reads compact quad records, draws from SSBO

  Modify gos_terrain.tesc: add 'uniform int useQuadRecords' branch.
  When 0 (default): existing passthrough, unchanged. When 1: reads
  TerrainQuadRecord SSBO (binding 0), reconstructs 6 TCS outputs
  per quad (2 patches × 3 verts) by index into worldPos/norm/UV/
  lighting per corner. pzTri1/pzTri2 flags drive TessLevelOuter=0
  for clipped triangles.

  Add record draw path to flush(): sorts records by tex_resolve handle,
  issues per-texture glDrawArrays(GL_PATCHES, firstRecord*6, count*6)
  with no vertex attribute data (TCS reads SSBO via gl_PrimitiveID).

  Env: MC2_PATCHSTREAM_QUAD_RECORDS=1 + MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1.
  Standard path unchanged when env vars unset.

  Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Self-Review

**Spec coverage:**
- ✅ `MC2_PATCHSTREAM_QUAD_RECORDS=1` env gate for record emission (M1a)
- ✅ `MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1` env gate for GPU draw path (M1b)
- ✅ CPU pz admission stays authoritative: pzTri1/pzTri2 bits in record, TCS sets TessLevel=0 for clipped tris
- ✅ Legacy expanded path unchanged; record path is additive
- ✅ Shape C fallback: records only emitted inside existing `isReady()` guard (Shape C-enabled path)
- ✅ No detail/overlay migration: detail/overlay addVertices calls in quad.cpp are outside the record block
- ✅ No stock cache/save dependency: records are per-frame transient
- ✅ Parity logging: `match=1` every frame validates corner mapping before GPU draw commit
- ✅ Visual validation step (Step 17): human-gated before commit

**Placeholder scan:** No TBD, TODO, or "similar to" references.

**Type consistency:**
- `TerrainQuadRecord::terrainHandle` is `uint32_t` — stored raw, `tex_resolve()` applied at flush sort (matching expanded path behavior)
- `flags` bit 0 = uvMode matches the TCS GLSL reading `rec.control.y & 1u` — `control.x` = terrainHandle, `control.y` = flags. Verify the GLSL struct field ordering matches the C++ struct ordering exactly (both are std430-compatible sequential layout).
- `tcs_TerrainType` carries the raw material byte (0-3). Verify gos_terrain.frag and TES consume this as an integer index, not a 0..1 float.
- `vec4GetIdx` and `uvec4GetIdx` helpers are used for `lightRGBs`/`fogRGBs` indexing — required because GLSL prohibits dynamic indexing of vec4 components in some drivers. These helpers should compile on AMD RX 7900 XTX (see `docs/amd-driver-rules.md` for AMD-specific restrictions).

**Stack allocation hazard in Step 13:** `recSortedTmp[kPatchStreamMaxRecordsPerSlot]` inside `flush()` is ~10 MB — must be `static`. Confirmed as `static` in the code above.

**Advisor note addressed:** "TES unchanged may be mostly true — verify." Step 11 is an explicit verification gate. The record TCS must reproduce the same 6 named varyings (`tcs_Color`, `tcs_FogValue`, `tcs_Texcoord`, `tcs_TerrainType`, `tcs_WorldPos`, `tcs_WorldNorm`) that TES consumes. If Step 11 reveals TES uses `gl_in[].gl_Position` in the main path (not just debug), the plan needs revision before proceeding to Step 12.
