# PatchStream M0e: Direct Texture Bind Fast Path

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bypass `applyRenderStates()` per terrain bucket by issuing `glBindTexture` directly, cutting ~189 µs/frame at max zoom where `applyRenderStates` is the dominant DrawBuckets cost.

**Architecture:** Single env-gated fast path inside `gos_terrain_bridge_drawSingleBucket`. When `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND=1`, each bucket resolves its gosHandle to a raw GL texture ID and calls `glBindTexture` directly instead of routing through the renderer's dirty-flag state machine. A new `gos_terrain_bridge_endBucketLoop` bridge function re-syncs the state cache after the loop completes so subsequent renderers see coherent state. The standard path (env var unset) is unchanged. This is a narrow probe — no ring-write changes, no grouping rewrite.

**Tech Stack:** C++14, OpenGL 4.3, Tracy profiler, MSVC RelWithDebInfo

**Context (why this is safe):**
- The terrain render contract (ZCompare, ZWrite, AlphaMode, TextureAddress, Terrain, shader program, VBO binds) is fully established by `beginBucketLoop()` + `bindUniforms()` before the bucket loop. Only `gos_State_Texture` changes per bucket.
- `applyRenderStates()` walks the full dirty-flag set and issues GL calls for every pending state. At 325 buckets it costs 189 µs/frame (581 ns × 325). The actual per-bucket GL work needed is one `glBindTexture` call (~50 ns).
- State-cache drift risk: after direct binds, the cache's recorded texture differs from what GL has bound. Fix: call `setRenderState(gos_State_Texture, lastHandle)` + `applyRenderStates()` once after the loop — one extra `glBindTexture`, but the cache is coherent for all subsequent renderers.
- Shadow object bypass precedent: this codebase already bypasses `applyRenderStates` for object shadows (direct `glUseProgram` + `glDrawElements`). Same pattern.

**Baseline (Tracy, max zoom mc2_01):**
- `ApplyRenderStates` in DrawBuckets: 189 µs total, 581 ns × 325 calls
- `PatchStream.DrawBuckets`: 215.73 µs/frame
- Expected win: ApplyRenderStates → ~16 µs (325 × ~50 ns `glBindTexture`), DrawBuckets → ~40 µs

**Key files:**
- `GameOS/gameos/gos_terrain_bridge.h` — add `gos_terrain_bridge_endBucketLoop` declaration
- `GameOS/gameos/gameos_graphics.cpp` — modify `drawSingleBucket`, add `endBucketLoop`
- `GameOS/gameos/gos_terrain_patch_stream.cpp` — track `lastGosHandle`, call `endBucketLoop`, add debug logging

**Build:** `/mc2-build` skill, always `--config RelWithDebInfo`.
**Deploy:** `/mc2-deploy` skill (target: `A:/Games/mc2-opengl/mc2-win64-v0.2/`).
**Smoke gate:**
```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
```

---

## Task 1: Direct Bind Fast Path + Cache Sync + Debug Logging

**Files:**
- Modify: `GameOS/gameos/gos_terrain_bridge.h`
- Modify: `GameOS/gameos/gameos_graphics.cpp`
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

Read all three files before making any changes.

---

- [ ] **Step 1: Add `gos_terrain_bridge_endBucketLoop` declaration to `gos_terrain_bridge.h`**

  After the `gos_terrain_bridge_drawSingleBucket` declaration, add:

  ```cpp
  // Call once after the per-bucket draw loop to synchronize the render-state
  // cache with the last texture directly bound by the fast path. No-op when
  // MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND is not set. Issues one redundant
  // glBindTexture to prevent cache drift from affecting subsequent renderers.
  void gos_terrain_bridge_endBucketLoop(unsigned int lastGosHandle);
  ```

---

- [ ] **Step 2: Add env-var flag in `gameos_graphics.cpp`**

  Near the top of `gameos_graphics.cpp`, before the bridge function implementations (find an existing `static const bool` pattern for env vars, e.g. near other `getenv` calls), add a file-scope static:

  ```cpp
  static const bool s_patchStreamDirectBind =
      (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND") != nullptr);
  ```

  This must be file-scope (not inside a function) so both `drawSingleBucket` and `endBucketLoop` share the same flag.

---

- [ ] **Step 3: Modify `gos_terrain_bridge_drawSingleBucket` in `gameos_graphics.cpp`**

  Replace the existing implementation with:

  ```cpp
  void gos_terrain_bridge_drawSingleBucket(
      unsigned int gosHandle,
      unsigned int firstVertex,
      unsigned int vertexCount)
  {
      if (!g_gos_renderer || vertexCount == 0) return;

      if (s_patchStreamDirectBind) {
          // Fast path: direct GL texture bind, bypassing applyRenderStates.
          // Terrain contract already established by beginBucketLoop(); only
          // the texture changes per bucket. glActiveTexture is per-bucket
          // here (not just in beginBucketLoop) because we cannot guarantee
          // applyRenderStates has not run since beginBucketLoop set it.
          gosTexture* tex = g_gos_renderer->getTexture((DWORD)gosHandle);
          const GLuint glTex = tex ? (GLuint)tex->getTextureId() : 0u;
          glActiveTexture(GL_TEXTURE0);
          if (glTex) glBindTexture(GL_TEXTURE_2D, glTex);
          glDrawArrays(GL_PATCHES, (GLint)firstVertex, (GLsizei)vertexCount);
      } else {
          // Standard path: full state machine flush.
          g_gos_renderer->setRenderState(gos_State_Texture, (int)gosHandle);
          g_gos_renderer->applyRenderStates();
          glActiveTexture(GL_TEXTURE0);
          glDrawArrays(GL_PATCHES, (GLint)firstVertex, (GLsizei)vertexCount);
      }
  }
  ```

---

- [ ] **Step 4: Implement `gos_terrain_bridge_endBucketLoop` in `gameos_graphics.cpp`**

  After `gos_terrain_bridge_drawSingleBucket`, add:

  ```cpp
  void gos_terrain_bridge_endBucketLoop(unsigned int lastGosHandle) {
      if (!s_patchStreamDirectBind || !g_gos_renderer || lastGosHandle == 0) return;
      // Re-sync state cache: marks gos_State_Texture as the last handle we
      // directly bound. applyRenderStates() will issue one redundant
      // glBindTexture, but subsequent renderers see a coherent cache.
      g_gos_renderer->setRenderState(gos_State_Texture, (int)lastGosHandle);
      g_gos_renderer->applyRenderStates();
  }
  ```

---

- [ ] **Step 5: Update `flush()` in `gos_terrain_patch_stream.cpp`**

  **5a — Track `lastGosHandle` across the draw loop.**

  Before the `ZoneScopedN("PatchStream.DrawBuckets")` block, declare:

  ```cpp
  DWORD s_lastGosHandleDrawn = 0;
  ```

  Inside the draw loop, after `const DWORD gosHandle = bk.gosHandle;`, add:

  ```cpp
  s_lastGosHandleDrawn = (DWORD)gosHandle;
  ```

  **5b — Call `endBucketLoop` after the draw loop.**

  After the closing brace of the `ZoneScopedN("PatchStream.DrawBuckets")` block, add:

  ```cpp
  gos_terrain_bridge_endBucketLoop((unsigned int)s_lastGosHandleDrawn);
  ```

---

- [ ] **Step 6: Add debug logging in `gos_terrain_patch_stream.cpp`**

  **6a — One-shot startup banner when direct bind is active.**

  Add a file-scope static near the other `s_*` statics in the anonymous namespace:

  ```cpp
  static const bool s_directBindOn =
      (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND") != nullptr);
  static bool s_directBindBannerSeen = false;
  ```

  In `flush()`, before the draw loop, add:

  ```cpp
  if (s_directBindOn && !s_directBindBannerSeen) {
      s_directBindBannerSeen = true;
      fprintf(stderr,
          "[PATCH_STREAM v1] event=direct_bind_enabled buckets_this_frame=%u\n",
          s_drawBucketCount);
      fflush(stderr);
  }
  ```

  **6b — Per-frame trace log (under existing `s_traceOn` gate).**

  In the existing `if (s_traceOn)` trace block near the end of `flush()`, extend the draw-count log line to include direct-bind status:

  ```cpp
  fprintf(stderr,
      "[PATCH_STREAM v1] event=draw_count slot=%u verts=%u buckets=%u direct_bind=%d\n",
      s_slot, cursor, s_drawBucketCount, (int)s_directBindOn);
  fflush(stderr);
  ```

  **6c — First-draw GL error drain (env-gated `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK=1`).**

  Add two more statics alongside `s_directBindOn` and `s_directBindBannerSeen` in the anonymous namespace:

  ```cpp
  static const bool s_directBindCheck =
      (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK") != nullptr);
  static bool s_directBindFirstDrawChecked = false;
  ```

  Inside the draw loop in `flush()`, immediately after the `gos_terrain_bridge_drawSingleBucket(...)` call, add:

  ```cpp
  if (s_directBindOn && s_directBindCheck && !s_directBindFirstDrawChecked) {
      s_directBindFirstDrawChecked = true;
      GLenum glErr;
      bool hadErr = false;
      while ((glErr = glGetError()) != GL_NO_ERROR) {
          hadErr = true;
          fprintf(stderr,
              "[PATCH_STREAM v1] event=direct_bind_first_draw_err "
              "bucket=%u err=0x%X\n",
              b, (unsigned)glErr);
          fflush(stderr);
      }
      if (!hadErr) {
          fprintf(stderr,
              "[PATCH_STREAM v1] event=direct_bind_first_draw_ok bucket=%u\n", b);
          fflush(stderr);
      }
  }
  ```

  This fires exactly once per process (first fast-path draw) and drains the full GL error queue, catching invalid active unit, wrong texture target, or VAO-state sequencing errors from the AMD driver before visual inspection runs.

---

- [x] **Step 7: Build**

  Run `/mc2-build`. Expected: zero errors. Both `gameos_graphics.cpp` and `gos_terrain_patch_stream.cpp` should recompile; the rest of the build is incremental.

---

- [x] **Step 8: Smoke gate — standard path (env var NOT set)**

  Deploy with `/mc2-deploy`. Run smoke gate without the env var:

  ```bash
  py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
  ```

  Expected: exit 0. This validates the standard path is unaffected.

---

- [x] **Step 9: Smoke gate — fast path with GL error check**

  Run smoke gate with both env vars enabled:

  ```bash
  MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND=1 MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK=1 py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 12 --fail-fast
  ```

  Expected: exit 0. Check stderr for:
  - `[PATCH_STREAM v1] event=direct_bind_enabled` — must appear once
  - `[PATCH_STREAM v1] event=direct_bind_first_draw_ok` — must appear once; if absent or replaced by `event=direct_bind_first_draw_err`, stop and report BLOCKED with the error code
  - No `event=overflow` lines
  - No other GL error lines

  A `direct_bind_first_draw_err` with error code `0x0502` (GL_INVALID_OPERATION) indicates a bad texture target or VAO state sequence — a hard blocker. Any non-zero error from the first draw is a hard blocker; do not proceed to visual validation or commit.

---

- [x] **Step 10: Visual validation with fast path**

  Launch the game with `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND=1`. Load `mc2_01`. Pan the camera from min zoom to max zoom and across biome boundaries. Verify:
  - Terrain textures are correct at all zoom levels
  - No flickering, seaming, or wrong-texture patches
  - HUD and UI render correctly after terrain (state cache sync)
  - Mechs and objects render correctly after terrain

  If anything is wrong, the state-cache sync is incomplete — report BLOCKED, do not commit the fast path.

---

- [x] **Step 11: Commit** (585fca6 initial impl, 90c62d9 correctness fixes: sampler object, sentinel, glActiveTexture, unconditional bind)

  ```bash
  git add GameOS/gameos/gos_terrain_bridge.h \
          GameOS/gameos/gameos_graphics.cpp \
          GameOS/gameos/gos_terrain_patch_stream.cpp
  git commit -m "perf: env-gated direct texture bind fast path for PatchStream buckets

  MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND=1 bypasses applyRenderStates() per
  terrain bucket, replacing the full dirty-flag state machine flush with a
  direct glBindTexture call. At max zoom on mc2_01, applyRenderStates was
  581 ns x 325 calls = 189 us/frame; direct bind is ~50 ns per call.

  State-cache coherency restored via gos_terrain_bridge_endBucketLoop():
  one setRenderState+applyRenderStates call after the loop syncs the cache
  to the last bound texture, preventing drift from affecting subsequent
  renderers. Standard path (env var unset) is unchanged.

  Follows shadow-object bypass precedent (direct glUseProgram + glDrawElements
  without applyRenderStates)."
  ```

---

## Self-Review

**Spec coverage:**
- ✅ Env gate: `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND=1`
- ✅ Per bucket: gosHandle → `getTexture()` → `getTextureId()` → `glBindTexture`
- ✅ `glActiveTexture(GL_TEXTURE0)` per bucket in fast path (cannot guarantee active unit after beginBucketLoop in all code paths)
- ✅ State cache sync: `endBucketLoop` calls `setRenderState` + `applyRenderStates` once after the loop
- ✅ Standard path unchanged: same `setRenderState` + `applyRenderStates` + `glActiveTexture` + `glDrawArrays` sequence
- ✅ Debug logging: startup banner + per-frame trace extension
- ✅ First-draw GL error drain: `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK=1`, fires once, drains full error queue, logs ok/err, blocks commit on any non-zero error
- ✅ Both smoke passes required: standard path first, then fast path with check var

**Placeholder scan:** No TBD, TODO, or "similar to" references.

**Type consistency:**
- `lastGosHandle` is `DWORD` in flush() (matching `bk.gosHandle` type), cast to `unsigned int` at the bridge call site — consistent with other bridge call sites in the same file.
- `s_directBindOn` in `gos_terrain_patch_stream.cpp` reads the same env var string as `s_patchStreamDirectBind` in `gameos_graphics.cpp`. They are independent statics that must agree — both check `MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND`.
