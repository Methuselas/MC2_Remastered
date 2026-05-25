# PatchStream M0b — Bisect Handoff

**Status:** modern path produces black terrain + GL_INVALID_OPERATION. Two bugs identified and partially fixed; latest build hangs/crashes. **Do not retry "manual A/B" claims** — the previous PASS report was running legacy by accident (cmd `set VAR = 1` with spaces creates a different env var name).

**Branch:** `claude/nifty-mendeleev`. **Tip commit at handoff time:** uncommitted (HEAD = `3a85c04`, unpushed working-tree changes in flush() and bridge). Reproduce the broken state by building HEAD source.

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.2/`
**Repro:** `cmd /c "set MC2_MODERN_TERRAIN_SURFACE=1 && set MC2_PATCH_STREAM_TRACE=1 && mc2.exe"` (no spaces around `=`)

## What was wrong before this debug session

`docs/superpowers/explorations/2026-04-27-patchstream-m0b-perf-analysis.md` claimed M0b's modern path was visually verified PASS at killswitch=1. **That claim was falsified by the bucket-census session.** The user later reproduced black terrain + GL_INVALID_OPERATION on every modern-path frame across clean rebuilds. The earlier "manual visual A/B" report likely ran legacy (cmd syntax `set MC2_MODERN_TERRAIN_SURFACE = 1` with spaces creates env var named with trailing space — getenv sees nothing → killswitch=0 → legacy path).

## Bug 1 (identified + attempted fix in working tree)

`tex_resolve(textureIndex)` returns the engine's **gosTextureHandle** (e.g. 56), not a GL texture object name. The legacy `gosRenderer::applyRenderStates()` at `gameos_graphics.cpp:2129–2135` always converts gos→GL via:

```c++
gosTexture* tex = getTexture(gosHandle);
glBindTexture(GL_TEXTURE_2D, tex->getTextureId());
```

The original modern flush() at `gos_terrain_patch_stream.cpp` did `glBindTexture(GL_TEXTURE_2D, tex_resolve(idx))` — binding GL texture name 56 directly when gos handle 56 actually maps to some other GL ID. That's an unrelated GL object (or invalid), and tessellation draws sampling from it produce GL_INVALID_OPERATION on AMD.

**Fix attempted (uncommitted):** added `gos_terrain_bridge_glTextureForGosHandle(gosHandle)` to `gos_terrain_bridge.h` + impl in `gameos_graphics.cpp` near the other bridge functions. flush() now does:

```c++
const DWORD gosHandle = tex_resolve(bk.textureIndex);
const GLuint glTex = (GLuint)gos_terrain_bridge_glTextureForGosHandle(gosHandle);
glBindTexture(GL_TEXTURE_2D, glTex);
```

## Bug 2 (identified + attempted fix in working tree)

`tex_resolve()` on a CACHED_OUT texture cascades to `MC_TextureNode::get_gosTextureHandle()` (mclib/txmmgr.cpp:2368) which **lazily LZ-decompresses + uploads the texture**, calling `glBindTexture(GL_TEXTURE_2D, newGLid)` internally. If that happens mid-loop in flush(), it clobbers our previous bucket's texture binding before that bucket's draw consumes it. The bucket-error diagnostic confirmed this: `tex0_bound` was 100, 371, 372 (increasing GL IDs from new allocations) instead of the expected GL handle for that bucket's texture.

**Fix attempted (uncommitted):** moved all `tex_resolve()` + `glTextureForGosHandle()` calls into a **pre-pass** before the draw loop. After the pre-pass any lazy loads have settled; the draw loop then does pure bind+draw without any further tex_resolve calls.

```c++
GLuint resolvedGL[kPatchStreamMaxBuckets] = { 0 };
for (uint32_t b = 0; b < s_drawBucketCount; ++b) {
    const DWORD gosHandle = tex_resolve(s_drawBuckets[b].textureIndex);
    resolvedGL[b] = (GLuint)gos_terrain_bridge_glTextureForGosHandle(gosHandle);
}
// ... then draw loop uses resolvedGL[b] for bind ...
```

## Current state — latest build hangs/crashes

After Bug 1 + Bug 2 fixes attempted, user reports the build **hangs and crashes** "around the time it would call the bucket error." The `predraw_state` diagnostic still fires once at first flush with correct attribs, but execution never reaches the per-bucket error capture — likely because something in the new pre-pass + draw loop deadlocks or AVs.

**Plausible causes for hang/crash, by likelihood:**

1. **Stack overflow from the pre-pass array.** `GLuint resolvedGL[kPatchStreamMaxBuckets]` = `GLuint[512]` = 2 KB on stack. Should be fine, but worth checking. Easy fix: make it `static GLuint resolvedGL[kPatchStreamMaxBuckets];` (single-threaded; flush() not re-entrant).

2. **`tex_resolve()` outside its frame-active window.** Per `tex_resolve_table.h`, `tex_resolve()` falls through to legacy `mcTextureManager->get_gosTextureHandle()` when `g_texResolveTable.frameActive` is false. flush() is called from `Render.TerrainSolid` which runs DURING the terrain frame, so frameActive should be true. But the pre-pass calls many tex_resolves in tight succession — if any of them triggers a lazy load that stalls/blocks (e.g. fastfile I/O, LZ decompress under contention), the loop could appear hung.

3. **`getTexture(gosHandle)` returns null for some handle, and the calling code dereferences.** The bridge function checks `if (!tex) return 0;` so it returns 0 for missing textures. `glBindTexture(GL_TEXTURE_2D, 0)` is valid. Should not crash. But verify.

4. **Lazy load inside the pre-pass triggers a re-entrant call into Render.TerrainSolid** (e.g. by allocating a texture that requires evicting another, which queues a GL command that runs another pass). Less likely but possible.

5. **The pre-pass exhausts texture cache or hits `flushCache()` failure** (`txmmgr.cpp:2386`) — `MAX_MC2_GOS_TEXTURES` cap reached, can't load more, calls PAUSE which might block waiting for input.

## Diagnostic state in working tree

- `event=predraw_state` — one-shot dump on first flush bucket: program, VAO, array/element buffer, patch verts, active texture, tex0, FBOs, expected colorBuf/extrasBuf, first/count
- `event=predraw_attrib loc=0..5` — six lines per first flush, attrib state per location
- `event=predraw_pending_err` — drains queued GL errors before draw
- `event=bucket_err` — rate-limited (8 buckets) post-draw error capture, includes `glTex=<actual GL ID>` (after Bug 1 fix relabeled from `tex_resolve=`)
- `event=texture_bind_check` — first-bucket-of-first-flush sanity check (still in code, fires once)

All gated on `MC2_PATCH_STREAM_TRACE=1`.

## Reproduction sequence the next session should run

1. Verify build is what's in working tree:
   ```bash
   cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev && git status -s
   # Should show modified gos_terrain_patch_stream.cpp, gos_terrain_bridge.h, gameos_graphics.cpp
   ```

2. Build + deploy:
   ```bash
   CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
   "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -3
   cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe
   ```

3. Repro the hang/crash via direct cmd launch:
   ```cmd
   cd /d A:\Games\mc2-opengl\mc2-win64-v0.2
   set MC2_MODERN_TERRAIN_SURFACE=1
   set MC2_PATCH_STREAM_TRACE=1
   mc2.exe
   ```

4. Click into mc2_01. Observe whether terrain is black, whether errors fire, whether process hangs.

## Next-session hypothesis ladder

In order of expected effort:

### Step 1 — Make the resolved-GL array static (defensive)

```c++
static GLuint resolvedGL[kPatchStreamMaxBuckets];
// (flush() is not re-entrant; static is safe and avoids any stack issue.)
```

If the hang/crash goes away, it was stack-related. If not, continue.

### Step 2 — Bisect: revert the pre-pass to confirm it's the culprit

Temporarily put `tex_resolve` + `glTextureForGosHandle` calls back inline in the per-bucket loop. If the build runs (with errors but no hang), the pre-pass IS the problem — likely a lazy load that doesn't behave well in a tight pre-pass. The fix would then be: keep the gos→GL conversion (Bug 1 fix) but resolve only one bucket at a time, accepting that lazy loads mid-loop clobber the binding (and re-bind after each `glTextureForGosHandle` call).

### Step 3 — Architectural pivot per advisor recommendation

The advisor's earlier note suggested replacing PatchStream's direct GL calls with a bridge that mirrors the legacy private-helper sequence:

```c++
gos_terrain_bridge_preparePatchStreamDraw(colorVbo, extrasVbo, slotFirstVert);
// then per-bucket: gos_terrain_bridge_drawPatchStreamBucket(gosHandle, firstVertex, count);
```

Implementation lives inside `gameos_graphics.cpp` next to `terrainDrawIndexedPatches`, where it can call private helpers + `applyRenderStates()` directly. The more direct GL setup we do from outside the renderer, the more invariants we miss (we've now hit at least two: deferred-state-bypass and gos-vs-GL-handle confusion).

This is the right repair shape. M0b's flush() should NOT do its own `glBindTexture` + `glDrawArrays` — it should call into a small engine-side helper that mirrors `terrainDrawIndexedPatches` minus the per-batch VBO upload.

### Step 4 — If even the bridge-helper approach errors

The remaining suspects are:
- vertex buffer attribute layout (gos_VERTEX layout in the persistent ring vs. what the legacy mesh provides — they should be identical, verify byte-for-byte)
- tessellation shader expects something the modern path doesn't provide (a uniform we missed in `terrainBindUniformsForPatchStream`)
- AMD-specific tessellation + persistent-mapped buffer interaction (per-driver bug; would explain why grass pass works with non-persistent buffers but ours doesn't)

If you reach Step 4, capture an apitrace or RenderDoc capture of one frame at killswitch=1 and one at killswitch=0, diff the GL command streams.

## Process corrections to apply to the next session

- **Do not trust "manual A/B PASS" reports without verifying the env var actually took**. cmd `set VAR=1` (no spaces) is the only correct syntax. Verify with `set MC2_MODERN` after.
- **Smoke-runner FPS metrics tell us nothing about visual correctness**. The frame counter advances regardless of whether the screen is black. Always require eyes on the screen for visual verification.
- **The perf-analysis closeout doc claims "5/5 PASS modern path firing reliably" — that PASS was the smoke runner's `result=pass` (no crash, FPS measured). It is NOT a visual pass.** Update the doc when you have time. The bucket-count and TGL pool numbers in that doc are still factually correct.

## What to update at the END of the next session

If terrain renders correctly:

1. Remove the `event=predraw_state` / `predraw_attrib` / `bucket_err` / `texture_bind_check` diagnostics (or demote them to silent unless MC2_PATCH_STREAM_TRACE=1, but most can just be deleted).
2. Commit the fix(es). Conventional message:
   `fix(patchstream): translate gos→GL handle for texture bind; pre-resolve before draw loop`
3. Re-run the perf table at killswitch=1 (the perf-analysis doc's numbers were measured against a broken visual path — they may shift now that the path is correct).
4. Update memory entry `~/.claude/projects/A--Games-mc2-opengl-src/memory/patchstream_m0b.md` with corrected status.
5. Retract the falsified visual-PASS claim in the perf-analysis closeout doc.
6. Reconsider whether B' (canonical bucket key) is still the right next slice. The bucket-census proved canonicalization can't reduce the bucket count; B' as designed is moot. The real next question is: now that modern is correct, is its perf still a regression vs. legacy? If yes, what's the cause?

If terrain still doesn't render correctly:

1. Pursue the hypothesis ladder above.
2. Update this handoff doc with new findings before the session ends.

## Key files and line anchors

- `GameOS/gameos/gos_terrain_patch_stream.cpp` — flush() body, ~line 670 onwards
- `GameOS/gameos/gos_terrain_bridge.{h,cpp-block-in-gameos_graphics.cpp:1670–1715}` — bridge accessors
- `GameOS/gameos/gameos_graphics.cpp:2031–2200` — applyRenderStates (reference for what legacy does)
- `GameOS/gameos/gameos_graphics.cpp:2685–2860` — terrainDrawIndexedPatches (legacy draw, our reference)
- `mclib/txmmgr.cpp:2368–2440` — MC_TextureNode::get_gosTextureHandle (the lazy-load function)
- `mclib/tex_resolve_table.h:57–103` — tex_resolve inline implementation

## Don't forget

- The smoke runner's DEFAULT_EXE was wrong (was v0.1.1, now v0.2 in commit `9cf3d4f`). Don't accidentally test against v0.1.1.
- Smoke runner now propagates `MC2_MODERN_TERRAIN_SURFACE` / `MC2_PATCH_STREAM_TRACE` / `MC2_PATCH_STREAM_FORCE_INIT_FAIL` from parent env (same commit).
- M0b's `static_assert` requires color and extras rings have equal per-slot vertex count. Don't tune one without the other.
- killswitch=0 path is bit-identical-equivalent to legacy — confirmed across multiple tier1 runs. Don't break that during repair.
