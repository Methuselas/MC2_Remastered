# Lane C — Editor GL / Render Correctness Audit

**Date:** 2026-06-02  
**Auditor:** ED-AUDIT-GL-RENDER (Sonnet lane agent)  
**Scope:** `tools/asset_viewer/` + `ui_editor/UiEditorImageCache.cpp` + `editor/` (EditRel)  
**Worktree:** `A:/Games/mc2-trackv-ci-gate-restore`  
**Severity weighting:** Both targets are dev tools (not shipped to players). P0 = crash/UB, P1 = leak/state hazard, P2 = cosmetic/cleanup, P3 = deferred/low-risk.

---

## 1. Viewer GL Safety Table

| # | File:line | Call / Pattern | Safe? | Notes |
|---|-----------|----------------|-------|-------|
| V1 | `main.cpp:73` | `glGetError()` flush after `glewInit` | ✅ | Correctly drains spurious error glewInit may leave |
| V2 | `UiEditorImageCache.cpp:266` | `glGenTextures(1, &glTexture)` | ✅ | One gen per load, local scope |
| V3 | `UiEditorImageCache.cpp:267` | `glBindTexture(GL_TEXTURE_2D, glTexture)` | ✅ | Immediately used for upload |
| V4 | `UiEditorImageCache.cpp:284–290` | `if (glTexture == 0)` post-gen check | ⚠️ **MISLEADING** | `glGenTextures` fills the name before this check is reached; the check is always false for a valid context. The check has no `glGetError()` after `glTexImage2D` so actual upload failures are silently undetected. See Finding C-2. |
| V5 | `UiEditorImageCache.cpp:329` | `glDeleteTextures(1, &it->second.glTexture)` on Shutdown/Clear | ✅ | Paired with every `glGenTextures`, guarded `!= 0` |
| V6 | `main.cpp:112` | `glClear(GL_COLOR_BUFFER_BIT)` only | ✅ | Stage-1 viewer is 2D only; depth buffer allocated (`SDL_GL_DEPTH_SIZE 24`) but never written or cleared. No depth test enabled. No semantic hazard. |
| V7 | `main.cpp:81` | `ImGui_ImplOpenGL3_Init("#version 130")` | ✅ | GL 3.0 core context; `#version 130` is compatible |
| V8 | No FBO created/bound | — | ✅ | Viewer renders entirely to default FBO (window surface) |
| V9 | No `glBindTexture(0)` unbind after upload | ⚠️ minor | Texture stays bound to unit 0 after upload; ImGui backend saves/restores texture bindings so no functional hazard, but is untidy. See Finding C-5 (P3). |
| V10 | No capability check before `glTexImage2D` | ✅ | GL_RGBA / GL_UNSIGNED_BYTE is core 2.0; no extension needed |

---

## 2. Depth Convention Verdict

### Asset Viewer (`mc2_asset_viewer`)

- **Convention:** None used. Stage 1 is a **2D texture preview only**.
- `main.cpp` requests `SDL_GL_DEPTH_SIZE 24` and `SDL_GL_STENCIL_SIZE 8` but **never calls** `glEnable(GL_DEPTH_TEST)`, `glDepthFunc`, `glClearDepth`, or `glClipControl`.
- `glClear` only clears `GL_COLOR_BUFFER_BIT`.
- **Verdict: Not applicable (forward-Z, reverse-Z — irrelevant; no 3D draws).**
- **Internally consistent: YES.** No depth comparison occurs so no inconsistency is possible.
- **Isolation:** Viewer is a standalone process with its own SDL/GL context. It shares no state with the game. Depth convention is entirely self-contained.
- **Stage 2 note:** When `ModelPreviewRenderCore` is added (stage 3 spec), a depth convention decision will be required. The viewer does NOT currently use RenderCore. No issue today.

### Editor (`EditRel`)

- **Game convention:** `gameosmain.cpp:1065` sets `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` + `glClearDepth(0.0)` → **reverse-Z**.
- **Editor GL init path:** `EditorGameOS.cpp` calls `gos_CreateRenderer()` → `gosRenderer::init()` → `gosRenderer::initRenderStates()`. **None of these call `glClipControl`.** `gameosmain.cpp` is explicitly NOT linked to the editor (confirmed by comment in `EditorGosRender.cpp:19-22`).
- **Editor `glDepthFunc` call:** `EditorGameOS.cpp:498` sets `GL_LEQUAL` — forward-Z convention.
- **Conclusion:** The editor does NOT set `glClipControl(GL_ZERO_TO_ONE)`, so the GL depth range remains at OpenGL default `[-1, 1]` (forward-Z). Yet it calls `gosRenderer` / `gos_CreateRenderer` which loads the same terrain/prop shaders written for reverse-Z. This is a **pre-existing architectural tension** noted in the codebase — the editor is known to have depth rendering issues for 3D content, and this is the root cause. Not a regression introduced by this audit scope; flagged as P1 (arch issue).

---

## 3. GL Object Leak Table (glGen* vs glDelete*)

**Viewer + UiEditorImageCache:**

| Object type | glGen* location | glDelete* location | Per-reload risk |
|-------------|-----------------|-------------------|-----------------|
| `GL_TEXTURE_2D` | `UiEditorImageCache.cpp:266` (per cache miss) | `UiEditorImageCache.cpp:329` (Shutdown/Clear) | **None** — cache key deduplication means the same path is never re-uploaded; once loaded, `image.attempted = true` prevents re-entry. On asset **Browse** (new selection), `TexturePreview2D::setSource` calls `UiEditorImageCache_Get` on the new path, which may gen a new texture. Old textures remain in `g_cache` until `Shutdown()`. This is a **growing cache** (never evicted during session). See Finding C-1. |

**Count summary (viewer + ui_editor):**  
- `glGenTextures` calls: **1** (per unique path, lazy)  
- `glDeleteTextures` calls: **1** (at Shutdown, per loaded entry)  
- FBO gen/delete: **0** / **0**  
- VBO/VAO gen/delete: **0** / **0** (viewer uses ImGui which manages its own)  
- **Net leak at normal shutdown: 0** (Shutdown → Clear → deletes all)

**Leak risk is only if process crashes before Shutdown.** Not a correctness issue for a dev tool.

---

## 4. ImGui Integration Verdict

**Asset viewer (`main.cpp`):**

| Check | Result |
|-------|--------|
| `IMGUI_CHECKVERSION()` called before `CreateContext` | ✅ |
| `CreateContext` before `InitForOpenGL` | ✅ |
| `ImGui_ImplSDL2_InitForOpenGL(window, gl_context)` | ✅ |
| `ImGui_ImplOpenGL3_Init("#version 130")` | ✅ |
| `NewFrame` order: `OpenGL3_NewFrame` → `SDL2_NewFrame` → `ImGui::NewFrame` | ✅ |
| `Render` → `GetDrawData` → `RenderDrawData` | ✅ |
| Shutdown order: `OpenGL3_Shutdown` → `SDL2_Shutdown` → `DestroyContext` | ✅ |
| GL state leak between ImGui and scene | **None** — viewer has no scene render; ImGui IS the entire render pass |
| Multiple contexts | **1 context only** — no collision risk |

**Smoke path ImGui risk:**  
`AssetViewerApp::runSmoke` creates its own SDL window + GL context, calls `UiEditorImageCache_Initialize/Shutdown`, and constructs `TexturePreview2D`. `surface.setSource()` calls `UiEditorImageCache_Get` which stores `(ImTextureID)(intptr_t)glTexture` — a type-cast only, no ImGui API call. `surface.draw()` is **never called** in the smoke path. No ImGui context is needed or created in smoke. **Safe.**

**Editor (`EditRel`):**  
Uses `GuiRuntime::Init/Shutdown` wrapping ImGui init, gated by `g_imguiInitialized` flag. `glBindFramebuffer(GL_FRAMEBUFFER, 0)` is explicitly set before `GuiRuntime::Render()` to ensure ImGui renders to the default FB (belt-and-suspenders after `pp_editor->endScene()` already does it). Pattern is correct.

---

## 5. Texture / KTX / sRGB Handling

- **Viewer does NOT use KtxLoader or any engine KTX path.** It uses `utils/Image.cpp` (GameOS image decoder) to load PNG/JPG/BMP/TGA formats only.
- Supported formats decoded to RGBA8, RGB8, I8/A8 → converted to `GL_RGBA / GL_UNSIGNED_BYTE`.
- **sRGB:** `glTexImage2D` uses `GL_RGBA` (linear internal format), not `GL_SRGB8_ALPHA8`. Preview textures that are authored as sRGB (most UI textures) will display with slightly wrong gamma. This is **cosmetic only** for a preview tool — viewer is not a color-accurate renderer. See Finding C-4 (P2).
- No KTX/BC7 compressed texture support in stage 1. That is by design; stage 1 scope is PNG/TGA preview only.
- No `GL_SRGB` or `GL_SRGB8_ALPHA8` anywhere in viewer source.

---

## 6. RenderCore / SimpleCamera Usage

- **Current shipped viewer:** Does NOT link or touch RenderCore. CMakeLists links: `imgui`, `SDL2`, `OpenGL::GL`, `GLEW`. No RenderCore target.
- `PreviewSurface.h:5` documents future stage 3 `ModelPreviewRenderCore` slot — architectural seam only, not implemented.
- README.md notes SimpleCamera is planned for stage 3.
- **No RenderCore collision risk today.**

---

## 7. Shared Global/Static State Risk

| State | File | Scope | Collision risk |
|-------|------|-------|----------------|
| `g_cache` (map) | `ui_editor/UiEditorImageCache.cpp` | Anonymous namespace (TU-local) | None — viewer and editor each compile this TU independently; no link-time collision |
| `g_status` (string) | same | Anonymous namespace | None |
| `static UiEditorImageTexture result` | `UiEditorImageCache.cpp:337` | Function-local static | **Single-threaded only** — not thread-safe but viewer is single-threaded; safe. The returned `const char* resolvedPath` points into the static's internal string; stale if caller saves the pointer across a second `Get()` call. `TexturePreview2D::setSource` does NOT store `resolvedPath`. Safe in current usage. See Finding C-3 (P2). |
| ImGui context | `main.cpp` | Process-scoped | No sharing with editor (separate executable) |

---

## Findings

### C-1 — Growing unbounded texture cache (no eviction) [P2]
**Severity:** P2 (dev tool; session-duration leak only, freed at shutdown)  
**Finding:** `UiEditorImageCache` never evicts entries. Every unique texture path browsed during a session accumulates a `GL_TEXTURE_2D` object in `g_cache` until `Shutdown()`. For a live session browsing hundreds of textures this accumulates VRAM monotonically.  
**Evidence:** `ui_editor/UiEditorImageCache.cpp:56` (`std::map<std::string, CachedImage> g_cache`) — no eviction logic anywhere in file.  
**Risk:** VRAM exhaustion for power-user sessions browsing large asset trees. Low severity for a dev tool.  
**Fix slice:** S — add a simple LRU cap (e.g. 128 textures) calling `glDeleteTextures` on eviction. No arch change needed.

### C-2 — Texture upload error check is dead code [P2]
**Severity:** P2  
**Finding:** After `glGenTextures(1, &glTexture)`, the code checks `if (glTexture == 0)` to detect failure. `glGenTextures` fills names before returning; in a valid context it never returns 0. The `glTexImage2D` call that follows is never checked via `glGetError()`. An out-of-memory or context-lost upload failure is silently swallowed — `image.loaded = true` is set unconditionally after the dead check.  
**Evidence:** `ui_editor/UiEditorImageCache.cpp:264–295`  
**Risk:** Viewer shows a successfully "loaded" image that is actually the black/uninitialized texture (no GL error reported to user). Cosmetic incorrect state.  
**Fix slice:** XS — replace dead `glTexture == 0` check with `glGetError()` after `glTexImage2D`.  
**SPECULATIVE:** On any modern driver this never fires for small textures; risk is real only with VRAM exhaustion or context loss.

### C-3 — Static result pointer lifetime ambiguity in UiEditorImageCache_Get [P2]
**Severity:** P2  
**Finding:** `UiEditorImageCache_Get` returns `&result` where `result` is a function-local static that is overwritten on every call. The `resolvedPath` field is a `const char*` pointing into `result`'s internal `std::string`. Any caller that stores this pointer across a subsequent `Get()` call gets a dangling reference. The header comment warns of this but the type is still `const char*` not a value string.  
**Evidence:** `ui_editor/UiEditorImageCache.h` (return type + comment); `UiEditorImageCache.cpp:337`.  
**Risk:** Currently safe because `TexturePreview2D::setSource` only reads `tex->loaded`, `tex->unavailable`, `tex->width`, `tex->height`, `tex->textureId` — none store `resolvedPath`. Future callers may not notice the restriction.  
**Fix slice:** XS — change `resolvedPath` field to `std::string` and return by value, or document clearly. Low urgency.

### C-4 — No sRGB internal format for texture preview [P2]
**Severity:** P2 (cosmetic)  
**Finding:** `glTexImage2D` uploads with `GL_RGBA` (linear) internal format. MC2 UI/game textures are typically authored as sRGB. Preview will display ~10% brighter than intended.  
**Evidence:** `ui_editor/UiEditorImageCache.cpp:270–280`  
**Risk:** Cosmetic only. Viewer is a dev tool, not a color-accurate reference.  
**Fix slice:** XS — use `GL_SRGB8_ALPHA8` internal format and set framebuffer to linear. Defer unless color accuracy is needed.  
**SPECULATIVE:** Depends on whether MC2 source textures are sRGB-tagged.

### C-5 — Texture left bound after upload [P3]
**Severity:** P3  
**Finding:** No `glBindTexture(GL_TEXTURE_2D, 0)` after upload in `LoadTextureFromPath`. The last-loaded texture name stays bound to unit 0.  
**Evidence:** `ui_editor/UiEditorImageCache.cpp:267–282`  
**Risk:** ImGui `ImplOpenGL3` backend saves and restores `GL_TEXTURE_2D` binding. No functional hazard. Style-only.  
**Fix slice:** XS — add `glBindTexture(GL_TEXTURE_2D, 0)` after upload. Defer.

### C-6 — Editor depth convention mismatch: `GL_LEQUAL` set but reverse-Z shaders in use [P1 — ARCH]
**Severity:** P1 (arch, editor-scoped, pre-existing)  
**Finding:** `EditorGameOS.cpp:498` sets `glDepthFunc(GL_LEQUAL)` (forward-Z). `gameosmain.cpp` is NOT linked to the editor. Therefore `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` is never called in the editor init path. But the editor calls `gos_CreateRenderer()` which loads the same reverse-Z shaders used by the game (terrain, props, static props). The GL depth range defaults to `[-1,1]` (OpenGL default) while the projection matrices and shaders are designed for `[0,1]` clip-space depth. This produces incorrect depth testing for all 3D content in the editor — terrain and props z-fight or render through each other.  
**Evidence:**  
- `editor/EditorGameOS.cpp:498` — `glDepthFunc(GL_LEQUAL)`  
- `GameOS/gameos/gameosmain.cpp:1064–1065` — `glClipControl` in game-only path, not editor  
- `editor/EditorGosRender.cpp:19–22` — comment confirms gameosmain not linked  
- `GameOS/gameos/gameos_graphics.cpp:3991–4002` — `gosRenderer::init()` sets up matrices for reverse-Z but does not call `glClipControl`  
**Risk:** Editor 3D depth testing is broken for all reverse-Z shaders. Manifests as terrain/prop z-fighting. Pre-existing, not a new regression. Mitigated by fact the editor is used for mission editing not as a final renderer reference.  
**Fix slice:** M — add `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` + `glClearDepth(0.0)` in `EditorGameOS::InitGameOS` after `glewInit`, mirroring `gameosmain.cpp:1064–1068`. Change `GL_LEQUAL` to `GL_GEQUAL` in `EditorGameOS.cpp:498`. Requires editor build validation.  
**SPECULATIVE:** May be intentionally left as forward-Z if editor terrain uses an old pre-reverse-Z shader variant. Verify before fixing.

---

## Summary Table

| ID | Sev | Title | Fix slice |
|----|-----|-------|-----------|
| C-1 | P2 | Unbounded texture cache (no eviction) | S |
| C-2 | P2 | Dead `glTexture==0` check; no post-upload error check | XS |
| C-3 | P2 | Static result pointer / `resolvedPath` lifetime trap | XS |
| C-4 | P2 | No sRGB internal format in viewer preview | XS / defer |
| C-5 | P3 | Texture left bound after upload | XS / defer |
| C-6 | P1 | Editor: `GL_LEQUAL` + no `glClipControl` → reverse-Z shaders with forward-Z depth | M |

**P0 findings: 0**  
**P1 findings: 1** (C-6, editor arch, pre-existing)  
**P2 findings: 4**  
**P3 findings: 1**

---

## Verdict

`mc2_asset_viewer` stage 1 is **GL-safe for its scope** (2D texture preview, single SDL/GL context, no FBO, no RenderCore). No P0 issues. The main actionable items are the dead error-check (C-2, XS) and cache eviction (C-1, S). The editor's reverse-Z mismatch (C-6) is a pre-existing P1 arch issue that predates this audit and should be tracked separately.
