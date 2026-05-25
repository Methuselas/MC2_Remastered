# PatchStream M0d: Flush Efficiency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce PatchStream per-frame CPU cost by ~1.0+ ms/frame via three independent changes: O(1) bucket lookup, texture-sorted bucket merging to reduce draw calls, and invariant render-state hoisting.

**Architecture:** All changes are local to `gos_terrain_patch_stream.cpp` and its bridge. Task 1 replaces the linear scan in `findOrCreateStagingBucket` with an open-addressing hash table (saves ~0.768 ms/frame, the dominant cost). Task 2 resolves and sorts staging buckets by texture at consolidate time, merging same-texture ranges to reduce draw count from `raw` to `unique` buckets (saves a fraction of 0.233 ms/frame proportional to the dedup ratio observed via census). Task 3 hoists the five invariant per-bucket render states out of the draw loop into a single pre-loop call (saves N×5 redundant state ops per frame). Each task is independently committable and verifiable via Tracy.

**Tech Stack:** C++14, OpenGL 4.3, Tracy profiler (ZoneScopedN), MSVC RelWithDebInfo

**Key files:**
- `GameOS/gameos/gos_terrain_patch_stream.cpp` — primary target for all three tasks
- `GameOS/gameos/gos_terrain_patch_stream.h` — struct change (Task 2)
- `GameOS/gameos/gos_terrain_bridge.h` — new bridge declarations (Task 3)
- `GameOS/gameos/gameos_graphics.cpp` — bridge implementations (Task 3)

**Build command:** invoke `/mc2-build` skill from the worktree, always `--config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.

**Deploy command:** invoke `/mc2-deploy` skill (copies exe + shaders to `A:/Games/mc2-opengl/mc2-win64-v0.2/`).

**Smoke gate:**
```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```
Exit 0 = pass. Inspect `tests/smoke/artifacts/<timestamp>/` on failure.

**Baseline Tracy numbers (87-frame capture):**
- `PatchStream.Append.LookupBucket`: ~0.768 ms/frame (dominant)
- `PatchStream.Append.InsertColor` + `InsertExtras`: ~0.540 ms/frame combined
- `PatchStream.DrawBuckets`: ~0.233 ms/frame
- `PatchStream.Consolidate.CopyColor` + `CopyExtras`: ~0.162 ms/frame

---

## Task 1: Replace Linear Scan with O(1) Open-Addressing Hash

**Why:** `findOrCreateStagingBucket` does a linear scan over `s_stagingCount` active entries per `appendTriangle` call. Real bucket count is 64–128 per frame (not the 5–40 claimed in the comment), giving O(60–130) comparisons per triangle × tens of thousands of triangles = 0.768 ms/frame.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

- [ ] **Step 1: Add hash table constants and array**

  In the anonymous namespace in `gos_terrain_patch_stream.cpp`, after the existing constants (around line 74, after `s_overflow`), add:

  ```cpp
  // Open-addressing hash table mapping textureIndex → s_staging[] index.
  // Power-of-2 size ≥ 2 × kPatchStreamMaxBuckets keeps load factor < 0.5,
  // guaranteeing probe termination. kHashEmpty is the vacant sentinel.
  constexpr uint32_t kHashTableSize = 1024u;
  constexpr uint32_t kHashEmpty     = 0xFFFFFFFFu;
  uint32_t           s_bucketHash[kHashTableSize];
  ```

- [ ] **Step 2: Initialize hash table in `init()`**

  In `TerrainPatchStream::init()`, after the pre-reserve loop (around line 292), add one line:

  ```cpp
  memset(s_bucketHash, 0xFF, sizeof(s_bucketHash));
  ```

  Note: this is a defensive one-time clear. `beginFrame()` always memsets the table before any appends, so this init() call is redundant in steady-state. It guards against any future code path that calls `appendTriangle` before the first `beginFrame()` (which would otherwise read uninitialized table entries).

- [ ] **Step 3: Reset hash table in `beginFrame()`**

  In `TerrainPatchStream::beginFrame()`, replace the existing reset block (lines 474–481):

  ```cpp
  // Before (lines 474–481):
  for (uint32_t i = 0; i < s_stagingCount; ++i) {
      s_staging[i].color.clear();
      s_staging[i].extras.clear();
  }
  s_stagingCount    = 0;
  s_totalVerts      = 0;
  s_drawBucketCount = 0;
  s_overflow        = false;
  ```

  Replace with:

  ```cpp
  for (uint32_t i = 0; i < s_stagingCount; ++i) {
      s_staging[i].color.clear();
      s_staging[i].extras.clear();
  }
  s_stagingCount    = 0;
  s_totalVerts      = 0;
  s_drawBucketCount = 0;
  s_overflow        = false;
  memset(s_bucketHash, 0xFF, sizeof(s_bucketHash));
  ```

- [ ] **Step 4: Replace `findOrCreateStagingBucket` body**

  Replace the entire function body (lines 170–187) with:

  ```cpp
  PatchStagingBucket* findOrCreateStagingBucket(DWORD textureIndex) {
      const uint32_t startSlot =
          (static_cast<uint32_t>(textureIndex) * 2654435761u) & (kHashTableSize - 1u);
      for (uint32_t probe = 0; probe < kHashTableSize; ++probe) {
          const uint32_t idx    = (startSlot + probe) & (kHashTableSize - 1u);
          const uint32_t stored = s_bucketHash[idx];
          if (stored == kHashEmpty) {
              if (s_stagingCount >= kPatchStreamMaxBuckets) {
                  fprintf(stderr,
                      "[PATCH_STREAM v1] event=overflow slot=%u kind=bucket_count "
                      "count=%u cap=%u\n",
                      s_slot, s_stagingCount, kPatchStreamMaxBuckets);
                  fflush(stderr);
                  s_overflow = true;
                  return nullptr;
              }
              s_bucketHash[idx] = s_stagingCount;
              PatchStagingBucket& nb = s_staging[s_stagingCount++];
              nb.textureIndex = textureIndex;
              return &nb;
          }
          if (s_staging[stored].textureIndex == textureIndex) {
              return &s_staging[stored];
          }
      }
      // Table exhausted without finding key — shouldn't happen if
      // kHashTableSize >= 2 × kPatchStreamMaxBuckets (load factor < 0.5).
      fprintf(stderr,
          "[PATCH_STREAM v1] event=overflow slot=%u kind=hash_full "
          "count=%u cap=%u\n",
          s_slot, s_stagingCount, kPatchStreamMaxBuckets);
      fflush(stderr);
      s_overflow = true;
      return nullptr;
  }
  ```

- [ ] **Step 5: Build**

  Run `/mc2-build`. Expected: zero errors, zero warnings on touched lines.

- [ ] **Step 6: Deploy and run smoke gate**

  Run `/mc2-deploy`, then run the smoke gate. Expected: exit 0.

- [ ] **Step 7: Verify in Tracy**

  Connect Tracy and capture ~100 frames on `mc2_01`. Verify:
  - `PatchStream.Append.LookupBucket` zone drops from ~0.768 ms/frame to < 0.05 ms/frame.
  - `PatchStream.Append.InsertColor` and `InsertExtras` are unchanged.
  - `PatchStream.DrawBuckets` and `PatchStream.Consolidate` are unchanged.
  - No `event=overflow kind=hash_full` lines in stderr.

- [ ] **Step 8: Commit**

  ```bash
  git add GameOS/gameos/gos_terrain_patch_stream.cpp
  git commit -m "perf: replace O(n) PatchStream bucket scan with O(1) hash table

  Linear scan over 64-128 active buckets per appendTriangle call was
  costing ~0.768 ms/frame (dominant PatchStream cost per 87-frame Tracy
  capture). Replace with open-addressing hash (Knuth multiplicative,
  1024-slot power-of-2 table, load factor < 0.5 guaranteed by sizing).
  beginFrame() resets with one memset instead of per-bucket clear.

  Expected: LookupBucket zone drops from ~0.768 ms to < 0.05 ms/frame."
  ```

---

## Task 2: Sort + Merge Staging Buckets by Resolved Texture

**Why:** Terrain quads are appended in spatial order, so consecutive quads often alternate between the same 2–3 textures. This creates `raw` buckets that resolve to only `unique < raw` distinct GL textures (measured by census instrumentation). Sorting staging by resolved texture before consolidation and merging adjacent same-texture ranges reduces draw calls from `raw` to `unique`, cutting draw overhead proportionally.

**Safety:** Sorting happens inside `flush()` on a local index array. Staging vectors (`s_staging[]`) are not reordered — only visited in sort order. Contiguous per-texture ranges in the ring are guaranteed because merge only extends the last draw bucket when the resolved handle matches.

**Alpha/draw-order risk:** Reordering buckets by texture handle changes the draw order of terrain patches that use different textures. This is safe **only if terrain is fully opaque** — which is true for the solid terrain path (AlphaMode is `gos_Alpha_OneZero`, set in the bridge, meaning source-one/dest-zero blend). If any terrain bucket uses a non-opaque alpha mode or if depth writes are disabled for some texture variant, sorting could produce visible overdraw differences. Step 8 below includes a mandatory visual validation on biomes with mixed terrain types specifically to catch this.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.h` (rename field in `PatchStreamBucket`)
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp` (add `<algorithm>`, restructure flush consolidate + census)

- [ ] **Step 1: Rename `PatchStreamBucket::textureIndex` to `gosHandle`**

  In `gos_terrain_patch_stream.h`, change the struct (lines 28–32):

  ```cpp
  // Before:
  struct PatchStreamBucket {
      DWORD textureIndex;  // terrain colormap handle, resolved at draw time
      uint32_t firstVertex;
      uint32_t vertexCount;
  };

  // After:
  struct PatchStreamBucket {
      DWORD    gosHandle;   // resolved gosHandle (tex_resolve already applied)
      uint32_t firstVertex; // slot-relative vertex offset (slotFirstVert added at draw time)
      uint32_t vertexCount;
  };
  ```

- [ ] **Step 2: Add `<algorithm>` include**

  In `gos_terrain_patch_stream.cpp`, add to the include block (around line 8, after `<cstring>`):

  ```cpp
  #include <algorithm>  // std::sort (Task 2: bucket merge)
  ```

- [ ] **Step 3: Restructure `flush()` — move census before sort, add sort + merge**

  The census currently runs after consolidate (line 590). Move it before the sort so it measures raw staging, not post-merge draw buckets. Then add sort + merge in the consolidate loop.

  Replace the entire consolidate section in `flush()` (lines 554–576, the `ZoneScopedN("PatchStream.Consolidate")` block) and the census section (lines 590–637) with the following. Place census computation FIRST (before sort), then sort, then consolidate with merge:

  ```cpp
  // --- Census snapshot from raw staging (before any sort/merge) ---
  if (s_censusOn) {
      const uint32_t N = s_stagingCount;
      DWORD resolved[kPatchStreamMaxBuckets];
      uint32_t sentinelCount = 0;
      for (uint32_t b = 0; b < N; ++b) {
          const DWORD r = tex_resolve(s_staging[b].textureIndex);
          resolved[b] = r;
          if (r == 0xFFFFFFFFu) ++sentinelCount;
      }
      uint32_t unique = 0;
      for (uint32_t b = 0; b < N; ++b) {
          bool seen = false;
          for (uint32_t k = 0; k < b; ++k) {
              if (resolved[k] == resolved[b]) { seen = true; break; }
          }
          if (!seen) ++unique;
      }
      uint32_t canonNoSort = (N > 0) ? 1u : 0u;
      for (uint32_t b = 1; b < N; ++b) {
          if (resolved[b] != resolved[b - 1]) ++canonNoSort;
      }
      s_lastCensusRaw      = N;
      s_lastCensusUnique   = unique;
      s_lastCensusSentinel = sentinelCount;
      s_lastCensusCanon    = canonNoSort;
  }

  // --- Sort staging by resolved texture handle ---
  // BucketSortEntry pairs resolved handle with staging index.
  // Sorting then walking in order groups same-texture staging
  // buckets together so the merge step below can coalesce them
  // into a single contiguous ring range (one draw call).
  struct BucketSortEntry { DWORD gosHandle; uint32_t stagingIdx; };
  BucketSortEntry sortBuf[kPatchStreamMaxBuckets];
  {
  ZoneScopedN("PatchStream.BucketSort");
  for (uint32_t i = 0; i < s_stagingCount; ++i) {
      sortBuf[i] = { tex_resolve(s_staging[i].textureIndex), i };
  }
  std::sort(sortBuf, sortBuf + s_stagingCount,
      [](const BucketSortEntry& a, const BucketSortEntry& b) {
          if (a.gosHandle != b.gosHandle) return a.gosHandle < b.gosHandle;
          return a.stagingIdx < b.stagingIdx;  // tie-break: preserve append order within same texture
      });
  }

  // --- Consolidate sorted staging into persistent ring, merging same-texture ranges ---
  uint32_t cursor = 0;
  s_drawBucketCount = 0;
  {
  ZoneScopedN("PatchStream.Consolidate");
  for (uint32_t i = 0; i < s_stagingCount; ++i) {
      const BucketSortEntry&    se = sortBuf[i];
      const PatchStagingBucket& sb = s_staging[se.stagingIdx];
      if (sb.color.empty()) continue;
      const uint32_t n = (uint32_t)sb.color.size();  // == sb.extras.size()

      {
      ZoneScopedN("PatchStream.Consolidate.CopyColor");
      memcpy(colorSlot  + cursor, sb.color.data(),  n * sizeof(gos_VERTEX));
      }
      {
      ZoneScopedN("PatchStream.Consolidate.CopyExtras");
      memcpy(extrasSlot + cursor, sb.extras.data(), n * sizeof(gos_TERRAIN_EXTRA));
      }

      // Merge into previous draw bucket if the resolved handle matches.
      // Data is written contiguously so [firstVertex .. firstVertex+vertexCount)
      // covers the full merged range correctly.
      if (s_drawBucketCount > 0 &&
          s_drawBuckets[s_drawBucketCount - 1].gosHandle == se.gosHandle) {
          s_drawBuckets[s_drawBucketCount - 1].vertexCount += n;
      } else {
          PatchStreamBucket& db = s_drawBuckets[s_drawBucketCount++];
          db.gosHandle   = se.gosHandle;
          db.firstVertex = cursor;  // slot-relative; slotFirstVert added at draw time
          db.vertexCount = n;
      }
      cursor += n;
  }
  }
  ```

- [ ] **Step 4: Update TracyPlot lines**

  After the consolidate block, the existing TracyPlot lines (line 577–578) reference `cursor` and `s_drawBucketCount` — these are still valid. No change needed.

- [ ] **Step 5: Update the draw loop to use `bk.gosHandle` directly**

  In the draw loop (around line 702–776), replace:

  ```cpp
  // Before:
  const DWORD gosHandle = tex_resolve(bk.textureIndex);
  const GLuint glTex =
      (GLuint)gos_terrain_bridge_glTextureForGosHandle((unsigned int)gosHandle);
  ```

  With:

  ```cpp
  // After (tex_resolve already applied at consolidate time):
  const DWORD gosHandle = bk.gosHandle;
  const GLuint glTex =
      (GLuint)gos_terrain_bridge_glTextureForGosHandle((unsigned int)gosHandle);
  ```

  Also update the predraw-state dump line (around line 738) that references `bk.firstVertex`:

  ```cpp
  // Before:
  (int)(slotFirstVert + bk.firstVertex),

  // After (unchanged — firstVertex is still slot-relative):
  (int)(slotFirstVert + bk.firstVertex),
  ```

  No change needed there. The `drawPatchStreamBucket` call (line 773–776) also stays the same since it takes `slotFirstVert + bk.firstVertex`.

- [ ] **Step 6: Build**

  Run `/mc2-build`. Expected: zero errors. If the compiler complains about `textureIndex` being missing from `PatchStreamBucket`, search for any remaining uses of that field name and update them to `gosHandle`.

- [ ] **Step 7: Deploy and run smoke gate**

  Run `/mc2-deploy`, then run the smoke gate. Expected: exit 0.

- [ ] **Step 8: Visual validation — alpha/draw-order check**

  Deploy and run the game. Manually inspect the following, looking for any terrain seams, flickering patches, or texture sorting artifacts that were not present before this change:
  - `mc2_01` (mixed grassland/dirt terrain, multiple texture types per frame)
  - `mc2_10` (desert biome, different texture mix)
  - Any mission with shoreline/water-adjacent terrain
  Pan the camera slowly across the full map at standard RTS zoom. If any terrain patch appears incorrectly ordered (z-fighting, unexpected dark seams, or incorrect texture on a patch), the sort is reordering patches that have different depth or blend state than expected — stop and investigate before committing.

- [ ] **Step 9: Verify in Tracy**

  Capture ~100 frames on `mc2_01`. Verify:
  - New `PatchStream.BucketSort` zone exists and costs < 0.05 ms/frame (tiny, just std::sort over ≤128 entries).
  - `PatchStream.DrawBuckets` zone is reduced. Expected: proportional to `unique/raw` ratio. If census shows raw=128, unique=48, expect DrawBuckets to drop by ~62%.
  - `PatchStream.Consolidate.CopyColor` and `CopyExtras` are unchanged (same byte volume).

- [ ] **Step 10: Cross-check census output**

  Run with `MC2_BUCKET_CENSUS=1` on `mc2_01` for ~600 frames. The `[BUCKET_CENSUS v1]` summary line should still show `raw > unique` (census uses pre-merge staging, so raw is still the unmerged count). The draw bucket count in `event=first_flush` and `event=draw_count` lines should approach or equal `unique` — exact equality holds only when no staging buckets are empty and no sentinel handles are present; small deviations are normal.

- [ ] **Step 11: Commit**

  ```bash
  git add GameOS/gameos/gos_terrain_patch_stream.h \
          GameOS/gameos/gos_terrain_patch_stream.cpp
  git commit -m "perf: merge same-texture PatchStream buckets at consolidate time

  Terrain quads are appended in spatial order, interleaving textures
  across what become the same few resolved GL textures. Sort staging
  buckets by tex_resolve() result before consolidation and coalesce
  adjacent same-texture ranges into one draw bucket. Reduces draw call
  count from raw to unique (census-measurable ratio, typically 2-4x on
  mc2_01). tex_resolve() called once per staging bucket at sort time,
  not once per draw call. PatchStreamBucket::textureIndex renamed to
  gosHandle to reflect pre-resolution."
  ```

---

## Task 3: Hoist Invariant Render States Out of the Draw Loop

**Why:** `gos_terrain_bridge_drawPatchStreamBucket` sets 5 render states (ZCompare, ZWrite, AlphaMode, TextureAddress, Terrain) and calls `glActiveTexture(GL_TEXTURE0)` on every bucket, but these values never change within a frame's draw loop. After Task 2 there are `unique` buckets per frame; each fires 5 setState + 1 GL call redundantly. Extract them to a one-time pre-loop call.

**State-timing note:** `beginBucketLoop()` sets the 5 invariant render states but does NOT call `applyRenderStates()`. The invariant state changes sit pending in the renderer's dirty-flag cache. The first `drawSingleBucket()` call then sets `gos_State_Texture` and calls `applyRenderStates()`, which flushes all pending dirty flags — invariants included — in one batch. This is correct: the same renderer that buffers dirty flags in the old path now just buffers more of them before the first flush. `glActiveTexture(GL_TEXTURE0)` in `beginBucketLoop()` is a direct GL call (not state-cached), so it is safe to issue before `applyRenderStates()` — the active texture unit is not managed by the dirty-flag cache.

**Files:**
- Modify: `GameOS/gameos/gos_terrain_bridge.h` (add two new declarations)
- Modify: `GameOS/gameos/gameos_graphics.cpp` (implement two new bridge functions)
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp` (update draw loop)

- [ ] **Step 1: Add new bridge declarations to `gos_terrain_bridge.h`**

  After the existing `gos_terrain_bridge_end` declaration (line 35), add:

  ```cpp
  // Call once before the per-bucket draw loop to set render states that are
  // invariant across all buckets (ZCompare, ZWrite, AlphaMode, TextureAddress,
  // Terrain, active texture unit). Only gos_State_Texture changes per bucket.
  void gos_terrain_bridge_beginBucketLoop();

  // Per-bucket draw call: sets gos_State_Texture for gosHandle, calls
  // applyRenderStates(), issues glDrawArrays(GL_PATCHES, firstVertex, count).
  // Call gos_terrain_bridge_beginBucketLoop() exactly once before the loop.
  void gos_terrain_bridge_drawSingleBucket(
      unsigned int gosHandle,
      unsigned int firstVertex,
      unsigned int vertexCount);
  ```

- [ ] **Step 2: Implement `gos_terrain_bridge_beginBucketLoop` in `gameos_graphics.cpp`**

  After the existing `gos_terrain_bridge_drawPatchStreamBucket` implementation (around line 1723), add:

  ```cpp
  void gos_terrain_bridge_beginBucketLoop() {
      if (!g_gos_renderer) return;
      g_gos_renderer->setRenderState(gos_State_ZCompare, 1);
      g_gos_renderer->setRenderState(gos_State_ZWrite, 1);
      g_gos_renderer->setRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
      g_gos_renderer->setRenderState(gos_State_TextureAddress, gos_TextureClamp);
      g_gos_renderer->setRenderState(gos_State_Terrain, 1);
      glActiveTexture(GL_TEXTURE0);
  }

  void gos_terrain_bridge_drawSingleBucket(
      unsigned int gosHandle,
      unsigned int firstVertex,
      unsigned int vertexCount)
  {
      if (!g_gos_renderer || vertexCount == 0) return;
      g_gos_renderer->setRenderState(gos_State_Texture, (int)gosHandle);
      g_gos_renderer->applyRenderStates();
      glDrawArrays(GL_PATCHES, (GLint)firstVertex, (GLsizei)vertexCount);
  }
  ```

  Leave `gos_terrain_bridge_drawPatchStreamBucket` unchanged — it may still be used by other callers or for fallback.

- [ ] **Step 3: Update the draw loop in `flush()` to use the new bridge API**

  In `gos_terrain_patch_stream.cpp`, find the `ZoneScopedN("PatchStream.DrawBuckets")` block (around line 701). Before the `for` loop, add a call to `beginBucketLoop`. Inside the loop, replace the `gos_terrain_bridge_drawPatchStreamBucket` call with `gos_terrain_bridge_drawSingleBucket`.

  ```cpp
  // Before the loop (add this line):
  gos_terrain_bridge_beginBucketLoop();

  // Inside the loop, replace:
  gos_terrain_bridge_drawPatchStreamBucket(
      (unsigned int)gosHandle,
      slotFirstVert + bk.firstVertex,
      bk.vertexCount);

  // With:
  gos_terrain_bridge_drawSingleBucket(
      (unsigned int)bk.gosHandle,
      slotFirstVert + bk.firstVertex,
      bk.vertexCount);
  ```

  The existing per-bucket error-check code (the `checkBucketErrors` block) stays in place — it still works after glDrawArrays.

- [ ] **Step 4: Build**

  Run `/mc2-build`. Expected: zero errors.

- [ ] **Step 5: Deploy and run smoke gate**

  Run `/mc2-deploy`, then run the smoke gate. Expected: exit 0. Terrain must render identically — the state reordering must not change visible output.

- [ ] **Step 6: Verify in Tracy**

  Capture ~100 frames on `mc2_01`. Verify:
  - `PatchStream.DrawBuckets` zone is further reduced vs Task 2 baseline.
  - No GL errors appear in stderr (`event=bucket_err` lines absent).

- [ ] **Step 7: Commit**

  ```bash
  git add GameOS/gameos/gos_terrain_bridge.h \
          GameOS/gameos/gameos_graphics.cpp \
          GameOS/gameos/gos_terrain_patch_stream.cpp
  git commit -m "perf: hoist invariant per-bucket render states out of PatchStream draw loop

  gos_terrain_bridge_drawPatchStreamBucket set 5 render states (ZCompare,
  ZWrite, AlphaMode, TextureAddress, Terrain) and glActiveTexture on every
  bucket. None of these change within a frame. Split into beginBucketLoop
  (invariant setup, called once) and drawSingleBucket (texture + draw only,
  called per bucket). Reduces steady-state setState overhead from N*5 to
  5 per frame."
  ```

---

## Self-Review

**Spec coverage check:**
- ✅ LookupBucket O(1) hash — Task 1
- ✅ init() memset noted as defensive-not-required — Task 1 Step 2 note
- ✅ Sort+merge staging by resolved texture — Task 2
- ✅ Sort comparator tie-breaks on stagingIdx for determinism — Task 2 Step 3
- ✅ Alpha/draw-order risk documented and validated — Task 2 Safety note + Step 8
- ✅ Census phrasing: "approach or equal unique" (not guaranteed exact) — Task 2 Step 10
- ✅ Census moved before sort to preserve raw/unique semantics — Task 2 Step 3
- ✅ Invariant render states hoisted — Task 3
- ✅ State-timing safety explained (invariants flush on first drawSingleBucket applyRenderStates) — Task 3 Why note
- ✅ glActiveTexture not state-cached, safe before applyRenderStates — Task 3 Why note
- ✅ Per-texture contiguity maintained — Task 2 merge only extends the last bucket, data written contiguously
- ✅ `gos_State_Texture` still changes per bucket — Task 3 drawSingleBucket

**Placeholder scan:** No TBD, TODO, or "similar to" references. All code blocks are complete and compilable.

**Type consistency:**
- `PatchStreamBucket::gosHandle` (renamed from `textureIndex`) used consistently across Tasks 2 and 3.
- `slotFirstVert + bk.firstVertex` used in draw calls across all tasks — `firstVertex` remains slot-relative throughout.
- `BucketSortEntry` is a local struct in `flush()` scope — not used outside that function.

**Interaction between tasks:** Tasks 1 and 2 are independent (Task 1 touches only `findOrCreateStagingBucket`/`beginFrame`; Task 2 touches only `flush`). Task 3 depends on the `gosHandle` field introduced in Task 2. Execute in order 1 → 2 → 3.
