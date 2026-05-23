# RenderWorld Slice M1.5 -- ObjectID Buffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the Tier 1.5 mandatory inspection substrate -- a single `GL_R32UI` MRT attachment on the main scene FBO, a per-slot `RenderObjectRecord` table inside RenderWorld, a `RenderWorld::lookupAtPixel(x,y)` debug API, and a static-prop fragment shader emit of `Handle::raw()` at `layout(location=2)`. Env-OFF default: byte-identical pixels vs M1 HEAD. Env-ON (`MC2_OBJECT_ID_BUFFER=1`): every static-prop pixel resolves to its `RenderObjectHandle` via `glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)`, with mesh / material / lod / pipeline (sentinel) / packet (sentinel) / path (sentinel) chain populated. Picking integration (missiongui click wiring) is DEFERRED to slice M1.6.

**Architecture:** No new modules. M1.5 extends three existing surfaces. (1) `GameOS/gameos/gos_postprocess.cpp` gains `sceneObjectIdTex_` + a file-scope `setSceneDrawBuffers(SceneDrawBufferMode)` helper through which ALL `sceneFBO_` `glDrawBuffers` calls route (C1 fix from spec adversarial review). (2) `RenderWorld/RenderWorld.{h,cpp}` gains a `RenderObjectRecord` table indexed by `handle.index()` (always populated, M1 decision), the `lookupAtPixel` API, and population hooks inside `upsertStaticProp` / `adoptStaticPropRecipe` / `destroy`. (3) `shaders/static_prop.frag` + the producer `gos_static_prop_batcher.cpp` gain a `_pad0 -> objectIdRaw` rename of the existing `PerDrawEntry` SSBO field, and the fragment shader gains `layout(location=2) out uint v_objectId` under a C++-driven `#ifdef MC2_OBJECT_ID_BUFFER` macro. No FBO is added (the attachment lives on the existing `sceneFBO_`).

**Tech Stack:** C++17, MSVC `--config RelWithDebInfo`, CMake (no new targets), existing `[RENDER_WORLD v1]` banner schema (`objectid_buffer=on|off` token added), existing tier1 smoke harness, env var `MC2_OBJECT_ID_BUFFER` cached at startup, in-binary self-tests gated by `MC2_OBJECT_ID_BUFFER_SELFTEST=1` and `MC2_RENDER_WORLD_SELFTEST=1`. Producer uses `glProgramUniform*` family (explicit-program upload trap; CLAUDE.md). GLSL macro propagation through `makeProgram()` prefix string (GLSL preprocessor does not inherit C++ build flags; CLAUDE.md). All struct fields uploaded as `int` and reinterpreted as `uint` in-shader (uniform-uint-crash trap).

**Spec:** `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md` (EXECUTABLE-READY).
**Adversarial review applied:** `docs/superpowers/reviews/2026-05-23-renderworld-slice-m1-5-spec-adversarial-review.md` (1 CRIT / 3 MAJOR / 6 MINOR; all CRIT + MAJORs resolved in the spec; m1/m2/m5 documented inline; m3/m4/m6 noted for future slices).

---

## Decisions resolved before execution (from spec adversarial review 2026-05-23)

All decisions below are baked into the spec at Appendix D and reflected in
this plan verbatim. Do NOT relitigate during execution -- if a decision
appears wrong during a task, STOP, escalate to the user, and amend the
spec before proceeding.

```
Decisions resolved before execution (from spec adversarial review 2026-05-23):
  C1: setSceneDrawBuffers() helper. All sceneFBO_ glDrawBuffers calls route through it.
  M1: s_objectRecords always populated (mission/upsert-time RenderWorld metadata, ~85KB peak).
  M2: AMD integer-MRT claim REMOVED; OBJECT_ID_SELFTEST canary added.
  M3: Rename PerDrawEntry._pad0 -> objectIdRaw on both C++ and GLSL sides.
  m4: glTexImage2D for sceneObjectIdTex_ (matches M1 pattern; defer glTexStorage2D).

Plan adversarial review applied 2026-05-23 (3 CRIT / 4 MAJOR / 6 MINOR):
  C1: per-type handle accessor split across registry + RenderWorld helper (NEW Task 6)
  C2: original Tasks 5+6+7 merged to single Phase A substrate-API commit (now Task 5)
  C3: original Tasks 8+9+10 merged to single atomic rename commit (now Task 7)
  M2: std::string prefix builders for BOTH legacy + coalesce programs; BOTH static_prop.frag output sites updated (Task 8)
  M3: setSceneDrawBuffers takes objectIdAttachmentReady; callers pass sceneObjectIdTex_ != 0
  M4: visual canary = mc2_03; stress = mc2_24; optional = mc2_10 (Task 11 three-tier)
  Task count: 16 -> 13 after merges.
```

Plan-level implications:

- C1 means the helper lives at FILE SCOPE in `gos_postprocess.cpp` (NOT in a header; the spec is explicit it is `gos_postprocess.cpp`-local `static`). Five sites route through it: `createFBOs()`, `beginScene()`, `runScreenShadow()`, `runGodRays()` Pass 2, `runShoreline()`. A hard grep gate at the end of Phase A verifies no raw `glDrawBuffers(...)` calls remain against `sceneFBO_` outside the helper body.
- M1 means `s_objectRecords` is the SINGLE always-on data structure introduced by this slice. There is NO env-gated table; every `upsertStaticProp` / `adoptStaticPropRecipe` / `destroy` call writes to it regardless of `MC2_OBJECT_ID_BUFFER`. Memory cost is bounded by mission upsert count (~85 KB peak at tier1 mc2_24 = 2641 props). One write per upsert; not paid per frame.
- M2 means there is NO doc claim about AMD driver behavior. The `OBJECT_ID_SELFTEST` (env-gated startup canary, Task 9) is the load-bearing runtime evidence that integer-MRT clear-then-write-then-read works correctly on the target hardware (7900 XTX). Failure here BLOCKS promoting `MC2_OBJECT_ID_BUFFER=1` to default; M2+ slices that depend on attachment-2 are blocked behind it too.
- M3 means the `PerDrawEntry._pad0` slot at offset 24 is renamed to `objectIdRaw` on BOTH the C++ side (`GameOS/gameos/gos_static_prop_batcher.h:53` and the matching `static_assert` at `:63`) AND the GLSL side (`shaders/static_prop.frag:44` -- the `MC2_COALESCE` struct mirror). The struct stays 32 bytes; the offset stays 24; `_pad1` stays at offset 28. This is a substitutive-not-additive change (one pad becomes content; struct size unchanged).
- m4 means `sceneObjectIdTex_` uses `glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr)` -- matching the existing `sceneNormalTex_` pattern at `gos_postprocess.cpp:265`, NOT the more modern `glTexStorage2D`. Migrating all four scene-FBO textures to immutable storage is a separate modernization slice with its own justification.

---

## Open items surfaced for user sign-off BEFORE Task 1 executes

These are NOT decisions to make during execution. They are operational
choices the plan author flags so the user can rule (or accept the
plan's lean) before Task 1.

### O1. Env-flag accessor location and shape

The plan assumes a new free function `bool IsObjectIdBufferEnabled()`
implemented in a new TU `RenderWorld/ObjectIdBuffer.cpp` (or
`gos_postprocess.cpp`-local file-scope if simpler), reading
`std::getenv("MC2_OBJECT_ID_BUFFER")` ONCE at first call and caching
into a `static const bool` (process-lifetime cache; flip requires
restart). This matches existing `MC2_*` flag conventions
(`MC2_HEARTBEAT`, `MC2_REVERSE_Z_TRACE`, `MC2_GL_DEBUG_FATAL`).

**Lean:** declare the accessor in `RenderWorld/RenderWorld.h` adjacent
to `frameBannerTick()` so any TU that wants the cached value (post-
process, batcher, RenderWorld itself) reaches it via one include. The
banner-tick in `RenderWorld.cpp` consumes it to emit
`objectid_buffer=on|off` per frame. Confirm before Task 1.

### O2. Visual canary mission selection (RESOLVED per plan review M4)

Three-tier canary defined (replaces the prior single-mission lean):

```
M1.5 visual canary (primary):
  run mc2_03 with MC2_OBJECT_ID_BUFFER=1 and MC2_OBJECT_ID_BUFFER_CANARY=1
  sweep cursor over visible buildings and trees
  confirm [RENDER_WORLD_INSPECT v1] lines resolve nonzero handles

M1.5 stress/perf (secondary):
  run mc2_24 with MC2_OBJECT_ID_BUFFER=1
  verify tier1 pass and <= budget frame-time delta

M1.5 mixed check (optional):
  run mc2_10 as fallback if mc2_03 lacks tree+building mix
```

Hotkey wiring for the inspect log: do NOT modify `missiongui.cpp`
(M1.6 scope per spec Section 1 non-goals). Instead, the canary mode
prints `[RENDER_WORLD_INSPECT v1]` on EVERY mouse-move event when
`MC2_OBJECT_ID_BUFFER_CANARY=1` is set -- this lives behind its own
env var so the production env-ON path is not log-spammed. See Task 11.

### O3. Shader recompile-on-env-change semantics

`MC2_OBJECT_ID_BUFFER` is read at program-load (when `makeProgram()`
runs for the static-prop fragment shader). Toggling the env var at
runtime has NO effect; the linked shader either has the
`#define MC2_OBJECT_ID_BUFFER 1` prefix or does not. Restarting the
process is required to flip the flag.

This is the same discipline as every other GLSL-preprocessor-gated
flag in the codebase (see `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`).
Document in the slice CLAUDE.md bullet (Task 13). No code change.

---

## Pre-flight reading (engineer MUST read before Task 1)

1. Spec entirely. Especially Sections 3 (FBO architecture + setSceneDrawBuffers helper), 4 (RenderObjectRecord), 5 (shader contract + PerDrawEntry rename), 9 (env gating), 12 (validation gates), Appendix D (resolved decisions).
2. Adversarial review entirely. The CRITICAL finding (C1) and the three MAJORs (M1/M2/M3) are now spec decisions, but the WRITE-TIME grep evidence in the review (file:line evidence at `gos_postprocess.cpp:416-419`, `:503-506`, `:613-616`, `:646-649`) is what every Phase A code edit must reverify before touching.
3. Worktree CLAUDE.md -- full file. Especially: NO emoji, grep-before-cite, build `--config RelWithDebInfo`, full-relink discipline (`rm build64/RelWithDebInfo/mc2.exe` before `cmake --build` on layout / static-state / inline-template changes), canonical smoke gate command, Vulkan-prep discipline, substitutive-not-additive rule, GLSL macro propagation rule, explicit-program uniform-upload rule, uniform-uint-crash rule, shader-exe deploy lockstep rule.
4. M1 plan format (`docs/superpowers/plans/2026-05-22-renderworld-slice-m1-static-prop-adapter-plan.md`). M1.5 mirrors its structure exactly.
5. Verify every cited file:line in this plan is still at the cited offset BEFORE starting any task that touches that file. The audit was performed 2026-05-23; if shipping HEAD has drifted, adjust line numbers in this plan AND in the commit message, then proceed.

---

## File structure

**Created files:** NONE. M1.5 is substitutive into existing files;
the only "new" thing is one scene-FBO texture (a `GLuint` member,
not a file).

**Modified files (grep-verified 2026-05-23):**

- `GameOS/gameos/gos_postprocess.{h,cpp}` -- add `sceneObjectIdTex_` member, `setSceneDrawBuffers()` file-scope helper, glClearBufferuiv at frame entry, FBO setup/teardown for the new attachment. Route 5 sites through helper.
- `RenderWorld/RenderWorld.h` -- declare `RenderObjectRecord` + `LookupResult` types and `lookupAtPixel(x,y)` + `IsObjectIdBufferEnabled()` accessor.
- `RenderWorld/RenderWorld.cpp` -- add `s_objectRecords` table; populate from `upsertStaticProp` / `adoptStaticPropRecipe`; mark dead on `destroy`; implement `lookupAtPixel`; extend `frameBannerTick` with `objectid_buffer=on|off` token; add OBJECT_ID_SELFTEST canary; add substrate self-test.
- `GameOS/gameos/gos_static_prop_batcher.h` -- rename `PerDrawEntry._pad0` -> `objectIdRaw`; update offset `static_assert` name.
- `GameOS/gameos/gos_static_prop_batcher.cpp` -- producer writes `entry.objectIdRaw = handle.raw()` when env-ON, `0u` otherwise.
- `shaders/static_prop.frag` -- rename `_pad0` -> `objectIdRaw` in the `MC2_COALESCE` `PerDrawEntry` struct; add `layout(location=2) out uint v_objectId` under `#ifdef MC2_OBJECT_ID_BUFFER`; write `v_objectId = uint(perDraw_.entries[...].objectIdRaw)` for coalesce or `uint(u_objectIdRaw)` for non-coalesce.
- C++-side `makeProgram()` call site for the static-prop fragment shader (grep at write time; lives in `gos_static_prop_batcher.cpp` or `GameOS/gameos/shader_loader.cpp` -- TBV by grep): add `#ifdef MC2_OBJECT_ID_BUFFER` `prefix += "#define MC2_OBJECT_ID_BUFFER 1\n"`. Per GLSL-macro propagation rule.
- `.claude/worktrees/nifty-mendeleev/CLAUDE.md` -- add Active campaigns bullet for M1.5 shipped state.

**Untouched (load-bearing -- confirm via grep, not assumption):**

- `code/missiongui.cpp` -- M1.6 scope; M1.5 does NOT modify it. Hard non-goal per spec Section 1.
- `shaders/shadow_static_prop.vert` -- shadow pass has its own depth-only FBO; attachment-2 work is N/A there.
- Every fragment shader other than `static_prop.frag` -- they MUST NOT declare a `layout(location=2) out` output. Phase C grep gate (and Goal-backward verification item 6) enforces.
- `RenderWorld/legacy/static_prop_backend.{h,cpp}` -- M1's bridge surface; no M1.5 changes (the M1.5 work happens above the legacy bridge: in `RenderWorld.cpp` for the record table and `lookupAtPixel`, in `gos_postprocess.cpp` for the FBO, in the static-prop shader for emission).
- `RenderCore/Handle.h` -- already exposes `raw()` (M1 SHIPPED). M1.5 consumes; does not touch.

---

## Phase A -- Substrate scaffolding (build green, env-OFF default, no behavior change)

**Phase A goal:** the FBO attachment, helper, record table, and lookup
API all exist. Env-OFF: tier1 5/5 byte-identical to M1 HEAD. Env-ON
(if anyone flips it manually): the attachment exists, is cleared
every frame, but no shader writes to it yet (so every pixel reads
back as 0 / `Handle::invalid()`). The shader emit + producer fill
arrive in Phase B.

**Phase A gate (must pass before Phase B starts):** tier1 5/5 PASS
env-OFF default with `[RENDER_WORLD v1]` banner showing
`objectid_buffer=off`. Then a single ad-hoc spot-check with
`MC2_OBJECT_ID_BUFFER=1` confirms (a) the banner flips to
`objectid_buffer=on`, (b) the attachment-2 clear runs without
`GL_INVALID_VALUE`, (c) `glReadPixels` of any pixel returns 0
(because no shader writes yet). Phase A does NOT require Phase A's
env-ON to ship visually-correct ID data; it requires the substrate
to be safely dormant.

### Task 1: Add `MC2_OBJECT_ID_BUFFER` env-flag accessor + startup banner

**Files:**
- Modify: `RenderWorld/RenderWorld.h`
- Modify: `RenderWorld/RenderWorld.cpp`

- [ ] **Step 1: Re-grep the current banner emit shape**

```bash
grep -n "RENDER_WORLD v1" RenderWorld/RenderWorld.cpp
```

Expected: hits at the `init()`, `destroy()`, `upsertStaticProp`, and
`frameBannerTick` emit lines. Confirm the format string we will be
extending in Step 4.

- [ ] **Step 2: Add the accessor declaration in `RenderWorld/RenderWorld.h`**

Existing (verbatim at the file's bottom, after `frameBannerTick()`):

```cpp
void frameBannerTick();

} // namespace RenderWorld
```

Replace with:

```cpp
void frameBannerTick();

// M1.5: object-ID buffer env-flag accessor. Reads
// MC2_OBJECT_ID_BUFFER once at first call; subsequent calls return the
// cached value. Flipping the env var requires a process restart (the
// linked shader's GLSL macro is fixed at program-load time per
// memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
//
// Consumed by:
//   - gos_postprocess.cpp setSceneDrawBuffers() / createFBOs() / beginScene()
//   - gos_static_prop_batcher.cpp producer (objectIdRaw fill)
//   - RenderWorld.cpp lookupAtPixel() guard
//   - RenderWorld.cpp frameBannerTick() banner token
//   - C++ side of static-prop makeProgram() (gates the GLSL #ifdef prefix)
bool IsObjectIdBufferEnabled();

} // namespace RenderWorld
```

- [ ] **Step 3: Implement `IsObjectIdBufferEnabled()` in `RenderWorld/RenderWorld.cpp`**

Add inside the existing anonymous namespace block (right next to
`envFlag()` at top of the file; grep for the existing helper):

```cpp
// M1.5: cached env-flag accessor. First call reads MC2_OBJECT_ID_BUFFER
// from getenv(); subsequent calls return the cached bool. Designed so
// the hot static-prop draw loop sees ONE branch-not-taken per draw on
// the env-OFF default path.
bool readObjectIdBufferEnv() {
    return envFlag("MC2_OBJECT_ID_BUFFER");
}
```

And expose the public accessor in the `RenderWorld` namespace:

```cpp
bool IsObjectIdBufferEnabled() {
    // Function-local static: thread-safe initialization (C++11 magic
    // statics); init runs exactly once at first call.
    static const bool s_enabled = readObjectIdBufferEnv();
    return s_enabled;
}
```

- [ ] **Step 4: Emit `[OBJECT_ID v1] event=enabled` startup banner in `init()`**

Existing (verbatim, grep-confirm offset at write time):

```cpp
void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init\n");
}
```

Replace with:

```cpp
void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    const bool oid = IsObjectIdBufferEnabled();
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        std::fprintf(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
}
```

- [ ] **Step 5: Build (header-only changes; full relink not required)**

```powershell
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The added function is unreferenced by Phase A
callers; LTO may inline it away. The startup banner does not yet emit
the `objectid_buffer=` token because `init()` is only called via
M1's wiring; the token IS emitted there.

- [ ] **Step 6: Verify env-OFF tier1 smoke**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. `grep -h "RENDER_WORLD v1" tests/smoke/artifacts/<latest>/*.log | head -5`
shows `event=init objectid_buffer=off` -- the new token is present and
correctly OFF (since `MC2_OBJECT_ID_BUFFER` is not set in the smoke
harness env).

- [ ] **Step 7: Commit**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): add MC2_OBJECT_ID_BUFFER env flag accessor (M1.5 Task 1)

Process-lifetime cached accessor IsObjectIdBufferEnabled() reads
MC2_OBJECT_ID_BUFFER once at first call. [RENDER_WORLD v1] init
banner gains objectid_buffer=on|off token; one-shot [OBJECT_ID v1]
event=enabled startup line emits when env-ON so log readers can
correlate banner state with attachment-2 lifecycle in subsequent
tasks. No consumers yet; substrate-only.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 9

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 2: Add `setSceneDrawBuffers()` helper to `gos_postprocess.cpp` (no callers yet)

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp`

C1 fix from spec: the helper is `gos_postprocess.cpp`-local
(file-scope `static`). It is NOT a header export; the post-process TU
owns scene FBO draw-buffer policy in one place.

- [ ] **Step 1: Re-grep all `glDrawBuffers` call sites in the file**

```bash
grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp
```

Expected (per adversarial review write-time grep, lines may drift):
hits at approximately `:274`, `:418`, `:505`, `:615`, `:648`. Five
sites total. If the grep returns a different count, STOP and reconcile
with the spec C1 finding before proceeding (a sixth site means the
audit predicate was incomplete; the spec must be amended first).

- [ ] **Step 2: Locate the file-scope anonymous namespace (or create one)**

```bash
grep -n "^namespace {" GameOS/gameos/gos_postprocess.cpp | head -3
```

If a file-scope anonymous namespace exists at top-of-file, insert the
helper there. Otherwise add a fresh anonymous namespace block AFTER
the `#include` lines but BEFORE the first `gosPostProcess::` method
body.

- [ ] **Step 3: Add the helper + include**

At the top of the file (with the other `#include` lines):

```cpp
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled
```

Inside the file-scope anonymous namespace (or a new one):

```cpp
namespace {

// M1.5 C1 fix + M3 plan-review fix: centralized scene-FBO draw-buffer
// policy. Every site that calls glDrawBuffers against sceneFBO_ routes
// through this helper. The caller passes objectIdAttachmentReady so the
// helper does not have to guess whether sceneObjectIdTex_ has been
// allocated yet (avoids GL_INVALID_VALUE when env-ON but FBO setup
// hasn't run). Callers pass `sceneObjectIdTex_ != 0` for MRT sites;
// SingleColor sites pass false.
//
// glClearBufferuiv(GL_COLOR, 2, ...) at frame-entry is ONLY safe
// after setSceneDrawBuffers(MainSceneMRT, true) has bound the 3-entry list.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3.
enum class SceneDrawBufferMode { MainSceneMRT, SingleColor };

static void setSceneDrawBuffers(SceneDrawBufferMode mode,
                                bool objectIdAttachmentReady) {
    const bool oid =
        RenderWorld::IsObjectIdBufferEnabled() && objectIdAttachmentReady;

    if (mode == SceneDrawBufferMode::SingleColor) {
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
        return;
    }

    if (oid) {
        GLenum bufs[3] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2
        };
        glDrawBuffers(3, bufs);
    } else {
        GLenum bufs[2] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1
        };
        glDrawBuffers(2, bufs);
    }
}

} // namespace
```

- [ ] **Step 4: Build (no callers yet)**

```powershell
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The helper compiles but is unreferenced (the
compiler MAY warn `unused function`; the `static` storage class
silences this in most MSVC configurations -- if it warns, leave it,
Task 3 wires the callers immediately).

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_postprocess.cpp
git commit -m "$(cat <<'EOF'
feat(postprocess): add setSceneDrawBuffers() helper (M1.5 Task 2)

File-scope helper centralizes sceneFBO_ draw-buffer policy per
spec C1 fix. Env-OFF: returns existing 1-entry (single-color) or
2-entry (MRT) shapes. Env-ON: 3-entry list including attachment-2
for MainSceneMRT. No callers wired yet; Task 3 routes the five
existing sites through it.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 3: Route 5 sites through `setSceneDrawBuffers()`

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp`

- [ ] **Step 1: Re-grep the five sites to confirm offsets**

```bash
grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp
```

Match each hit to one of:
- `createFBOs()` (initial setup; was 2-entry `{C0,C1}`)
- `beginScene()` (per-frame rebind; was 2-entry `{C0,C1}`)
- `runScreenShadow()` (postprocess single-color)
- `runGodRays()` Pass 2 composite (postprocess single-color)
- `runShoreline()` (postprocess single-color)

- [ ] **Step 2: Replace `createFBOs()` site**

Existing (verbatim, around `:272-274`):

```cpp
    // MRT: draw to both color attachments
    GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffers);
```

Replace with:

```cpp
    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when MC2_OBJECT_ID_BUFFER=1 AND the
    // caller passes objectIdAttachmentReady=true (M1.5 C1 + M3
    // plan-review fix). At createFBOs() time we have just allocated
    // sceneObjectIdTex_ (if env-ON), so pass `sceneObjectIdTex_ != 0`.
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);
```

- [ ] **Step 3: Replace `beginScene()` site**

Existing (verbatim, around `:416-419`):

```cpp
    if (sceneNormalTex_) {
        GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
    }
```

Replace with:

```cpp
    if (sceneNormalTex_) {
        // M1.5 C1 + M3 plan-review fix: route through centralized helper.
        // attachment-2 is re-asserted every frame when env-ON AND the
        // attachment texture exists. The raw glDrawBuffers(2,...) above
        // structurally DROPPED slot 2 from the active write mask -- caught
        // by adversarial review.
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                            sceneObjectIdTex_ != 0);
    }
```

- [ ] **Step 4: Replace `runScreenShadow()` site**

Existing (verbatim, around `:503-506`):

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);
```

Replace with:

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite. Helper preserves env-OFF/ON parity
    // (the postprocess composite never writes attachment-2 regardless).
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
```

- [ ] **Step 5: Replace `runGodRays()` Pass 2 composite site**

Existing (verbatim, around `:613-616`):

```cpp
    // Pass 2: Additive composite onto scene at full res
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);
```

Replace with:

```cpp
    // Pass 2: Additive composite onto scene at full res
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite (additive); helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
```

- [ ] **Step 6: Replace `runShoreline()` site**

Existing (verbatim, around `:646-649`):

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);
```

Replace with:

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color multiplicative composite; helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
```

- [ ] **Step 7: Hard grep gate -- verify only the helper body references `glDrawBuffers`**

```bash
grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp
```

Expected: THREE hits ONLY, all inside the `setSceneDrawBuffers()` body
(one SingleColor 1-entry, one MainSceneMRT 3-entry env-ON branch, one
MainSceneMRT 2-entry fallback branch). Any other hit outside the helper
body is a slice failure -- the C1 helper invariant is broken.

- [ ] **Step 8: Full relink (changed function body in a TU that the link graph caches)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 9: Tier1 smoke env-OFF (byte-identical invariant)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Pixel parity vs M1 HEAD: because env-OFF the
helper returns the EXACT same `glDrawBuffers(2, ...)` shape as before
at the two MRT sites and the EXACT same `glDrawBuffers(1, ...)` shape
at the three single-color sites. The behavior is bit-for-bit
unchanged.

- [ ] **Step 10: Commit**

```bash
git add GameOS/gameos/gos_postprocess.cpp
git commit -m "$(cat <<'EOF'
feat(postprocess): route 5 sceneFBO_ glDrawBuffers sites through helper (M1.5 Task 3)

Five sites converted: createFBOs / beginScene (MainSceneMRT shape)
and runScreenShadow / runGodRays Pass 2 / runShoreline (SingleColor
shape). Hard grep gate: only setSceneDrawBuffers body references
glDrawBuffers now. Env-OFF tier1 5/5 byte-identical to M1 HEAD --
the helper returns the same lists the raw calls did, the only
difference is centralized policy.

This is the C1 mitigation. Without it the per-frame beginScene
rebind structurally dropped attachment-2 from the active write
mask every frame when env-ON, making the object-ID write a no-op.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 4: Create `sceneObjectIdTex_` attachment + per-frame clear

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h` (add member)
- Modify: `GameOS/gameos/gos_postprocess.cpp` (create / destroy / per-frame clear)

- [ ] **Step 1: Re-grep the gosPostProcess member list**

```bash
grep -n "sceneNormalTex_\|sceneColorTex_\|sceneDepthTex_" GameOS/gameos/gos_postprocess.h
```

Expected: each member appears once in the header (declaration) and
multiple times in the cpp (lifetime / use). Confirm the declaration
form (`GLuint sceneNormalTex_ = 0;` or similar) to match style.

- [ ] **Step 2: Add `sceneObjectIdTex_` member**

In `GameOS/gameos/gos_postprocess.h`, immediately after `sceneNormalTex_`:

```cpp
    GLuint sceneObjectIdTex_ = 0;   // M1.5 R32UI MRT attachment-2 (gated on MC2_OBJECT_ID_BUFFER)
```

- [ ] **Step 3: Create the texture + attach in `createFBOs()`**

In `gos_postprocess.cpp` immediately AFTER the existing
`sceneNormalTex_` attachment block (`:262-270`) and BEFORE the
`setSceneDrawBuffers()` call that Task 3 just routed:

Existing (verbatim, around `:262-274`):

```cpp
    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    glGenTextures(1, &sceneNormalTex_);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);

    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when MC2_OBJECT_ID_BUFFER=1 AND
    // sceneObjectIdTex_ has been created (M1.5 C1 + M3 fix).
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);
```

Replace with:

```cpp
    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    glGenTextures(1, &sceneNormalTex_);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);

    // M1.5: object-ID attachment-2 (GL_R32UI). Gated on
    // MC2_OBJECT_ID_BUFFER; when env-OFF we skip the texture
    // creation entirely so env-OFF runtime cost is exactly zero on
    // the FBO side. glTexImage2D matches the sceneNormalTex_ pattern
    // above (decision m4); glTexStorage2D migration deferred.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        glGenTextures(1, &sceneObjectIdTex_);
        glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                               GL_TEXTURE_2D, sceneObjectIdTex_, 0);
    }

    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when env-ON AND sceneObjectIdTex_
    // exists (M1.5 C1 + M3 fix).
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);
```

- [ ] **Step 4: Destroy in `destroyFBOs()`**

```bash
grep -n "glDeleteTextures.*sceneNormalTex_" GameOS/gameos/gos_postprocess.cpp
```

Locate the destroy site (around `:332-349`). Add immediately after the
`sceneNormalTex_` delete:

```cpp
    if (sceneObjectIdTex_) {
        glDeleteTextures(1, &sceneObjectIdTex_);
        sceneObjectIdTex_ = 0;
    }
```

- [ ] **Step 5: Add per-frame attachment-2 clear in `beginScene()`**

Existing (verbatim, around `:405-420` AFTER Task 3's `setSceneDrawBuffers` route):

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // Bind both draw buffers so the upcoming glClear in gameosmain.cpp clears
    // both COLOR0 and COLOR1 (the GBuffer1 normal/post-shadow-mask attachment).
    // ...
    if (sceneNormalTex_) {
        // M1.5 C1 + M3 fix: route through centralized helper so
        // attachment-2 is re-asserted every frame when env-ON AND
        // sceneObjectIdTex_ exists.
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                            sceneObjectIdTex_ != 0);
    }
    glViewport(0, 0, width_, height_);
```

Replace with:

```cpp
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // Bind both draw buffers so the upcoming glClear in gameosmain.cpp clears
    // both COLOR0 and COLOR1 (the GBuffer1 normal/post-shadow-mask attachment).
    // ...
    if (sceneNormalTex_) {
        // M1.5 C1 + M3 fix: helper takes readiness flag explicitly.
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                            sceneObjectIdTex_ != 0);
    }
    // M1.5 m1 clear-order rule + M3 plan-review fix: glClearBufferuiv
    // at INDEX 2 only safe AFTER the env-ON 3-entry list is bound.
    // Guarded by the same readiness predicate that selects the 3-entry
    // list above; env-OFF byte-identical.
    if (RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_) {
        static const GLuint kClearZero[4] = { 0u, 0u, 0u, 0u };
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT, true);
        glClearBufferuiv(GL_COLOR, 2, kClearZero);
    }
    glViewport(0, 0, width_, height_);
```

- [ ] **Step 6: Full relink**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 7: Tier1 smoke env-OFF (byte-identical)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. The env-OFF branch never creates
`sceneObjectIdTex_` and never calls `glClearBufferuiv`. Byte-identical
to M1 HEAD.

- [ ] **Step 8: Ad-hoc env-ON spot-check (single mission)**

```bash
MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_01
```

Expected: mc2_01 PASS, no GL errors related to `INVALID_VALUE` from
the `glClearBufferuiv` call, banner shows `objectid_buffer=on`. Pixel
output is identical to env-OFF (no shader writes attachment-2 yet --
that arrives in Phase B).

- [ ] **Step 9: Commit**

```bash
git add GameOS/gameos/gos_postprocess.h GameOS/gameos/gos_postprocess.cpp
git commit -m "$(cat <<'EOF'
feat(postprocess): add sceneObjectIdTex_ R32UI attachment (M1.5 Task 4)

GL_R32UI attachment-2 created/destroyed/cleared gated on
MC2_OBJECT_ID_BUFFER. env-OFF: zero FBO delta, zero clear cost,
texture is not allocated. env-ON: created at createFBOs(),
destroyed at destroyFBOs(), per-frame clear via glClearBufferuiv
AFTER setSceneDrawBuffers(MainSceneMRT) has bound the 3-entry
draw-buffer list (m1 clear-order rule). glTexImage2D matches M1
sceneNormalTex_ pattern (m4 decision).

No shader writes the attachment yet; Phase A is dormant-substrate
only. Phase B (Task 8) lands the static_prop.frag emit.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 5: Phase A substrate API -- atomic combined commit (C2 fix)

**C2 plan-review fix:** original Tasks 5+6+7 merged into ONE
"Phase A substrate API" commit. No dangling `lookupAtPixel`
declaration in a standalone committed state. The single commit ships:
- RenderObjectRecord + LookupResult declarations in `RenderWorld.h`
- s_objectRecords table + populate/retire in `RenderWorld.cpp`
- lookupAtPixel implementation
- All compile + link green at this commit

Substeps below remain visible for clarity but the commit + gate
boundary is atomic. Do NOT commit between substeps; one commit at
Step 14 covers all of Phase A's substrate API.

**Files:**
- Modify: `RenderWorld/RenderWorld.h`
- Modify: `RenderWorld/RenderWorld.cpp`
- Modify: `GameOS/gameos/gos_postprocess.h` (accessor expose)

- [ ] **Step 1: Re-confirm the header bottom shape**

```bash
grep -n "^} // namespace RenderWorld$" RenderWorld/RenderWorld.h
```

Confirm there is exactly ONE closing namespace brace. Insert types and
function declarations IMMEDIATELY BEFORE it.

- [ ] **Step 2: Add the types + lookupAtPixel declaration**

Existing (verbatim, the file's tail):

```cpp
// M1.5: object-ID buffer env-flag accessor. ...
bool IsObjectIdBufferEnabled();

} // namespace RenderWorld
```

Replace with:

```cpp
// M1.5: object-ID buffer env-flag accessor. ...
bool IsObjectIdBufferEnabled();

// M1.5: per-slot inspection record. Indexed by handle.index().
// Always populated (M1 decision: mission/upsert-time RenderWorld
// metadata; ~85 KB peak at tier1 mc2_24 = 2641 props). Slot recycle
// bumps generation; alive=false marks a retired slot.
//
// Most fields are documentary in M1.5 (PipelineId / DrawPacket /
// pathReasonCode have no real consumers yet); their sentinels are
// returned through LookupResult so M2+ slices can fill them without
// API churn.
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
};

static constexpr uint16_t kRenderObjectFlagAlive = 1u << 0;

// M1.5: result of lookupAtPixel. isValid=false on background pixel or
// generation mismatch (stale-pixel-after-destroy). Caller MUST check
// isValid before consuming any other field.
struct LookupResult {
    bool                            isValid          = false;
    RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
    uint32_t                        meshHandleBits   = 0;
    uint32_t                        materialHandleBits = 0;
    uint8_t                         lodLevel         = 0xFFu;
    uint16_t                        pipelineId       = 0;
    uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
    uint32_t                        pathReasonCode   = 0;
    uint32_t                        gameObjectId     = 0;
};

// M1.5: synchronous pixel -> handle lookup. screenX/Y in GL convention
// (origin bottom-left). Returns LookupResult{isValid=false} when env-OFF,
// FBO not initialized, pixel==0, or generation mismatch. Stalls the GPU
// to read the prior frame's attachment-2 -- intended for click-time
// (max ~10/sec) debug; not per-frame.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 7
LookupResult lookupAtPixel(int screenX, int screenY);

} // namespace RenderWorld
```

- [ ] **Step 3: (Build defer to combined commit at Step 14; declarations alone link clean because no caller yet, but we will not commit until all substrate API is in place per C2.)**

- [ ] **Step 4: Re-grep the existing emit sites in RenderWorld.cpp**

```bash
grep -n "upsertStaticProp\|adoptStaticPropRecipe\|^void destroy" RenderWorld/RenderWorld.cpp
```

Confirm the three function bodies are intact and writable.

- [ ] **Step 5: Add include and table declaration**

At the top of `RenderWorld.cpp` (with the other includes):

```cpp
#include <vector>
#include <mutex>
```

In the anonymous namespace, near the existing `std::atomic<uint64_t>`
state declarations:

```cpp
// M1.5: per-slot inspection table. Always populated (M1 decision);
// ~85 KB peak at tier1 mc2_24 = 2641 props. Indexed by
// handle.index(); resized lazily on upsert. mutex guards resize +
// write; reads in lookupAtPixel acquire the same lock (cheap,
// click-rate).
std::mutex                            s_objectRecordsMutex;
std::vector<RenderWorld::RenderObjectRecord> s_objectRecords;
```

- [ ] **Step 6: Add population helper**

```cpp
void populateRecord(uint32_t handleIndex,
                    uint16_t generation,
                    uint32_t gameObjectId)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) {
        // Grow with headroom; doubling beyond demand to amortize.
        const size_t want = static_cast<size_t>(handleIndex) + 1;
        const size_t cap  = (want * 3) / 2 + 16;
        s_objectRecords.resize(cap);
    }
    auto& rec = s_objectRecords[handleIndex];
    rec.generation         = generation;
    rec.flags              = RenderWorld::kRenderObjectFlagAlive;
    rec.meshHandleBits     = 0;           // M1.5: unknown (no MeshHandle producer)
    rec.materialHandleBits = 0;           // M1.5: unknown
    rec.lodLevel           = 0xFFu;       // M1.5: unknown
    rec.pipelineId         = 0;           // M1.5 sentinel
    rec.drawPacketIndex    = 0xFFFFFFFFu; // M1.5 sentinel
    rec.pathReasonCode     = 0;           // M1.5 sentinel
    rec.gameObjectId       = gameObjectId;
}

void retireRecord(uint32_t handleIndex)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) return;
    auto& rec = s_objectRecords[handleIndex];
    rec.flags &= static_cast<uint16_t>(~RenderWorld::kRenderObjectFlagAlive);
    // Bump generation so the next upsert at this index produces a
    // distinct handle and stale pixels read back as invalid via the
    // generation check in lookupAtPixel.
    rec.generation = static_cast<uint16_t>(rec.generation + 1u);
}
```

- [ ] **Step 7: Call `populateRecord` from `upsertStaticProp`**

Existing (verbatim, the body of `upsertStaticProp`):

```cpp
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(r), 1u);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=upsert_ok recipe=%d handle.index=%u\n",
            r, (unsigned)h.index());
    }
    return h;
```

Replace with:

```cpp
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(r), 1u);
    // M1.5: populate the always-on record table (mission/upsert-time
    // metadata, ~85 KB peak; M1 decision).
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   desc.gameObjectId);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=upsert_ok recipe=%d handle.index=%u\n",
            r, (unsigned)h.index());
    }
    return h;
```

Note: `desc.gameObjectId` is the existing `StaticPropDesc::gameObjectId`
field (M1 `RenderCore/RenderObjectDesc.h:499`). Confirm by grep:

```bash
grep -n "gameObjectId" RenderCore/RenderObjectDesc.h
```

- [ ] **Step 8: Call `populateRecord` from `adoptStaticPropRecipe`**

Existing (verbatim):

```cpp
RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex) {
    if (recipeIndex < 0) {
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    return RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(recipeIndex), 1u);
}
```

Replace with:

```cpp
RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex) {
    if (recipeIndex < 0) {
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(recipeIndex), 1u);
    // M1.5: late-spawn populates the record table too. gameObjectId
    // is unknown at this seam (the adapter does not pass one through
    // syncStaticPropLateSpawn); 0 is the canonical "no cookie".
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   0u);
    return h;
}
```

- [ ] **Step 9: Call `retireRecord` from `destroy(Handle)`**

Existing (verbatim):

```cpp
void destroy(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    legacy::invalidateStaticProp(handleToRecipeIndex(h));
    s_destroyCalls.fetch_add(1, std::memory_order_relaxed);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=destroy handle.index=%u\n",
            (unsigned)h.index());
    }
}
```

Replace with:

```cpp
void destroy(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    legacy::invalidateStaticProp(handleToRecipeIndex(h));
    s_destroyCalls.fetch_add(1, std::memory_order_relaxed);
    // M1.5: retire the record (clears alive flag, bumps generation).
    retireRecord(h.index());
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=destroy handle.index=%u\n",
            (unsigned)h.index());
    }
}
```

- [ ] **Step 10: Decide the postprocess reach-through shape**

Two options:
- **Option A** (lean): Add public accessors `GLuint getSceneFBO() const;`
  and `GLuint getSceneObjectIdTex() const;` to `gos_postprocess.h`.
  Cheap, idiomatic.
- **Option B:** Move the readback into a postprocess method
  `gosPostProcess::readObjectIdAtPixel(x, y)` and have RenderWorld
  call it via the existing global `pp` accessor. More layers.

**Lean: Option A.** Confirm before Step 11.

- [ ] **Step 11: Add accessors to `gos_postprocess.h`**

```cpp
    // M1.5: readback hooks for RenderWorld::lookupAtPixel.
    GLuint getSceneFBO()         const { return sceneFBO_; }
    GLuint getSceneObjectIdTex() const { return sceneObjectIdTex_; }
```

- [ ] **Step 12: Implement `lookupAtPixel` in `RenderWorld.cpp`**

```bash
grep -n "getGosPostProcess" GameOS/gameos/gos_postprocess.h
```

The postprocess accessor is `getGosPostProcess()` (free function;
grep-confirmed at GameOS/gameos/gameosmain.cpp:193 and many sites).
This was previously documented as `extern gosPostProcess* g_pp` --
that symbol does NOT exist; use the accessor (m2 plan-review fix).

Implement in `RenderWorld.cpp` (at the bottom of the namespace, after
`frameBannerTick`):

```cpp
LookupResult lookupAtPixel(int screenX, int screenY) {
    LookupResult out;
    if (!IsObjectIdBufferEnabled()) {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0\n");
            warned = true;
        }
        return out;
    }
    // Reach into postprocess for the FBO + attachment-2 texture. If
    // the FBO is not yet initialized (pre-first-frame, mid-resize),
    // return invalid with a one-shot WARN.
    // m2 plan-review fix: use getGosPostProcess() accessor, NOT
    // extern g_pp (which does not exist as a symbol).
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called before postprocess init\n");
            warned = true;
        }
        return out;
    }
    const GLuint fbo = pp->getSceneFBO();
    const GLuint tex = pp->getSceneObjectIdTex();
    if (!fbo || !tex) {
        return out;
    }

    // Synchronous single-pixel readback. Per spec section 7: stalls
    // the GPU until prior-frame attachment-2 writes are visible.
    // GL_RED_INTEGER + GL_UNSIGNED_INT is the integer-format pair
    // (using GL_RED + GL_FLOAT would silently reinterpret bits).
    uint32_t raw = 0u;
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    glReadPixels(screenX, screenY, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &raw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));

    if (raw == 0u) {
        // Background / cleared pixel.
        return out;
    }

    RenderCore::RenderObjectHandle h;
    h.bits = raw;

    // Look up the record under the mutex; copy out under the lock.
    RenderWorld::RenderObjectRecord rec;
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (h.index() >= s_objectRecords.size()) {
            return out;  // out-of-range index: treat as invalid
        }
        rec = s_objectRecords[h.index()];
    }

    // Generation check: stale pixel (rendered before slot recycle)
    // returns invalid even though the raw value parses to a Handle.
    if (rec.generation != static_cast<uint16_t>(h.generation())) {
        return out;
    }
    if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
        return out;
    }

    out.isValid            = true;
    out.handle             = h;
    out.meshHandleBits     = rec.meshHandleBits;
    out.materialHandleBits = rec.materialHandleBits;
    out.lodLevel           = rec.lodLevel;
    out.pipelineId         = rec.pipelineId;
    out.drawPacketIndex    = rec.drawPacketIndex;
    out.pathReasonCode     = rec.pathReasonCode;
    out.gameObjectId       = rec.gameObjectId;
    return out;
}
```

- [ ] **Step 13: Add `objectid_buffer=on|off` token to the frame banner**

Existing (verbatim, body of `frameBannerTick`):

```cpp
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu visible=0 packets=0 views=1\n",
        (unsigned long long)f, (unsigned long long)active);
```

Replace with:

```cpp
    const char* oidTok = IsObjectIdBufferEnabled() ? "on" : "off";
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu visible=0 packets=0 views=1 objectid_buffer=%s\n",
        (unsigned long long)f, (unsigned long long)active, oidTok);
```

- [ ] **Step 14: Full relink (combined substrate API)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green. The record table sees writes on every M1
register/destroy call; `lookupAtPixel` is linked; substrate self-test
(Task 10) is the first caller.

- [ ] **Step 15: Tier1 smoke env-OFF**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. The `[STATIC_PROP_REGISTRY v1]` and
`[RENDER_WORLD v1] objects=N` counts MUST match M1 HEAD baseline (the
table is internal; no banner schema change). Frame banner shows
`objectid_buffer=off`. `lookupAtPixel` exists but is unreferenced;
the function is dormant.

- [ ] **Step 16: Atomic combined commit + Phase A close (C2 fix)**

```bash
git add RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp GameOS/gameos/gos_postprocess.h
git commit -m "$(cat <<'EOF'
feat(renderworld): Phase A substrate API -- combined atomic commit (M1.5 Task 5, C2 fix)

Single atomic commit per plan-review C2 fix (no dangling
lookupAtPixel declaration mid-stream):
- RenderObjectRecord + LookupResult declarations in RenderWorld.h
- s_objectRecords always-on table + population hooks (M1 decision)
- upsertStaticProp / adoptStaticPropRecipe populate;
  destroy retires (clears alive flag, bumps generation)
- lookupAtPixel synchronous glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)
  of attachment-2; generation + alive validation against s_objectRecords
- Frame banner gains objectid_buffer=on|off token
- getSceneFBO/getSceneObjectIdTex accessors on gosPostProcess

Memory: ~85 KB peak at tier1 mc2_24. No banner schema change to
[STATIC_PROP_REGISTRY v1]; tier1 5/5 env-OFF byte-identical to
M1 HEAD. Postprocess access via getGosPostProcess() accessor
(NOT a global g_pp -- that symbol does not exist; m2 fix).

PHASE A CLOSE: substrate dormant. env-OFF tier1 5/5 byte-identical
to M1 HEAD. env-ON: attachment-2 exists, is cleared per frame, but
no shader writes yet -- every readback returns 0. Phase B lands the
producer.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 4,7,9

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase B -- Shader write + producer rename

**Phase B goal:** static-prop fragment shader emits `Handle::raw()`
to attachment-2 when env-ON. PerDrawEntry struct gets the
`_pad0 -> objectIdRaw` rename on both sides. Producer fills the slot
with the live handle.

**Phase B gate (must pass before Phase C starts):**
1. Env-OFF: tier1 5/5 BYTE-IDENTICAL pixels vs M1 HEAD.
2. Env-ON: tier1 5/5 PASS, banner `objectid_buffer=on`, FPS delta
   bounded by spec budget (Section 10: <=0.5ms p99). No GL errors.
3. After the rename, `grep -n "_pad0" GameOS/gameos/gos_static_prop_batcher.h shaders/static_prop.frag` returns ZERO hits.

### Task 6: Registry accessor + RenderWorld handle helper (NEW per C1 fix)

**C1 plan-review fix:** clean three-owner split for the handle
encoding chain.

```
Registry owns:    typeID -> recipeIndex
RenderWorld owns: recipeIndex -> RenderObjectHandle bits
Batcher owns:     writing objectIdRaw into PerDrawEntry
```

This task adds the two missing helpers BEFORE the producer (Task 7)
reaches for them, so the producer site is a clean three-line call.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_registry.h`
- Modify: `GameOS/gameos/gos_static_prop_registry.cpp`
- Modify: `RenderWorld/RenderWorld.h`
- Modify: `RenderWorld/RenderWorld.cpp`

- [ ] **Step 1: Add `getRecipeIndexForType()` declaration in registry header**

In `GameOS/gameos/gos_static_prop_registry.h`, adjacent to existing
recipe accessors:

```cpp
// M1.5 C1 fix: typeID -> recipeIndex side-map accessor. Returns -1
// for unknown typeID (or after invalidate()).
int32_t getRecipeIndexForType(uint32_t typeID);
```

- [ ] **Step 2: Add the typeID -> recipeIndex side-map in registry .cpp**

In `GameOS/gameos/gos_static_prop_registry.cpp`:

```cpp
// M1.5 C1 fix: typeID -> recipeIndex side-map. Populated by
// registerRecipe(); set to -1 on invalidate(). Lookup returns -1
// if typeID is unknown.
static std::unordered_map<uint32_t, int32_t> s_typeIDToRecipeIndex;
```

In `registerRecipe()` (at the end, after the recipe slot is established):

```cpp
// M1.5 C1: maintain side-map for batcher's objectIdRaw producer.
s_typeIDToRecipeIndex[recipe.owningTypeID] = recipeIndex;
```

In `invalidate()` (when the recipe is tombstoned):

```cpp
// M1.5 C1: tombstone the side-map entry too.
s_typeIDToRecipeIndex[typeID] = -1;
```

Implement the accessor:

```cpp
int32_t getRecipeIndexForType(uint32_t typeID) {
    auto it = s_typeIDToRecipeIndex.find(typeID);
    if (it == s_typeIDToRecipeIndex.end()) return -1;
    return it->second;
}
```

- [ ] **Step 3: Add `objectIdRawForStaticPropRecipe()` declaration in RenderWorld.h**

In `RenderWorld/RenderWorld.h` next to `IsObjectIdBufferEnabled()`:

```cpp
// M1.5 C1 fix: centralize Handle encoding. Returns 0 for invalid
// recipeIndex (< 0). The producer in gos_static_prop_batcher.cpp
// calls this with the result of GpuStaticPropRegistry::getRecipeIndexForType().
uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex);
```

- [ ] **Step 4: Implement `objectIdRawForStaticPropRecipe()` in RenderWorld.cpp**

Implementation EXACT (uses the M1 recipeIndex -> handle.index()
translation already established in the table):

```cpp
uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex) {
    if (recipeIndex < 0) return 0u;
    return RenderCore::RenderObjectHandle::make(
        static_cast<uint32_t>(recipeIndex) & 0x000FFFFFu,
        1u
    ).raw();
}
```

- [ ] **Step 5: Build (no callers yet beyond Task 7's pending producer call)**

```powershell
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build green.

- [ ] **Step 6: Tier1 smoke env-OFF (no behavior change)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. The side-map populates on registration but
no consumer reads it yet; behavior is byte-identical.

- [ ] **Step 7: Commit**

```bash
git add GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp RenderWorld/RenderWorld.h RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): registry typeID->recipeIndex + RenderWorld handle helper (M1.5 Task 6, C1 fix)

Clean three-owner split per plan-review C1:
- Registry: getRecipeIndexForType(typeID) backed by side-map
- RenderWorld: objectIdRawForStaticPropRecipe(recipeIndex)
- Batcher (Task 7): writes objectIdRaw into PerDrawEntry

No producer wired yet; that lands atomically with the rename in Task 7.
env-OFF tier1 5/5 byte-identical.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 5

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 7: Atomic rename + producer wire + GLSL mirror (C3 fix)

**C3 plan-review fix:** original Tasks 8+9+10 merged into ONE atomic
commit. No "RED commit even if broken" allowance. The single commit ships:
- C++ struct rename `_pad0` -> `objectIdRaw` (with static_asserts)
- GLSL struct rename (every shader file mirroring PerDrawEntry)
- Producer write (calling `getRecipeIndexForType` +
  `objectIdRawForStaticPropRecipe` from Task 6)
- Shader read site update if applicable

Substeps retained for clarity but commit boundary is atomic.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`
- Modify: `shaders/static_prop.frag`
- Audit + modify: any other shader file mirroring `PerDrawEntry`

- [ ] **Step 1: Re-grep current struct shape**

```bash
grep -n "_pad0\|PerDrawEntry" GameOS/gameos/gos_static_prop_batcher.h
```

Expected: struct at `:46-55`, `static_assert` at `:63`. Confirm `_pad1`
stays at `:54` / `:64`.

- [ ] **Step 2: Rename field + offset assert**

Existing (verbatim, `:46-65`):

```cpp
struct PerDrawEntry {
    int32_t packetID;          //  0 - index into s_packets[]
    int32_t materialFlags;     //  4 - 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t maxLocalVertexID;  //  8 - type.vertexCount - 1
    int32_t texArrayLayer;     // 12 - group-relative layer in s_texArrayOff/On
    float   uvScaleX;          // 16 - 1.0f for Stage A
    float   uvScaleY;          // 20 - 1.0f for Stage A
    int32_t _pad0;             // 24 - std430 alignment + size = 32
    int32_t _pad1;             // 28
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, packetID)         ==  0, "packetID offset");
static_assert(offsetof(PerDrawEntry, materialFlags)    ==  4, "materialFlags offset");
static_assert(offsetof(PerDrawEntry, maxLocalVertexID) ==  8, "maxLocalVertexID offset");
static_assert(offsetof(PerDrawEntry, texArrayLayer)    == 12, "texArrayLayer offset");
static_assert(offsetof(PerDrawEntry, uvScaleX)         == 16, "uvScaleX offset");
static_assert(offsetof(PerDrawEntry, uvScaleY)         == 20, "uvScaleY offset");
static_assert(offsetof(PerDrawEntry, _pad0)            == 24, "_pad0 offset");
static_assert(offsetof(PerDrawEntry, _pad1)            == 28, "_pad1 offset");
```

Replace with:

```cpp
struct PerDrawEntry {
    int32_t packetID;          //  0 - index into s_packets[]
    int32_t materialFlags;     //  4 - 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t maxLocalVertexID;  //  8 - type.vertexCount - 1
    int32_t texArrayLayer;     // 12 - group-relative layer in s_texArrayOff/On
    float   uvScaleX;          // 16 - 1.0f for Stage A
    float   uvScaleY;          // 20 - 1.0f for Stage A
    int32_t objectIdRaw;       // 24 - M1.5: handle.raw() when MC2_OBJECT_ID_BUFFER=1, else 0
    int32_t _pad1;             // 28 - std430 alignment + size = 32
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, packetID)         ==  0, "packetID offset");
static_assert(offsetof(PerDrawEntry, materialFlags)    ==  4, "materialFlags offset");
static_assert(offsetof(PerDrawEntry, maxLocalVertexID) ==  8, "maxLocalVertexID offset");
static_assert(offsetof(PerDrawEntry, texArrayLayer)    == 12, "texArrayLayer offset");
static_assert(offsetof(PerDrawEntry, uvScaleX)         == 16, "uvScaleX offset");
static_assert(offsetof(PerDrawEntry, uvScaleY)         == 20, "uvScaleY offset");
static_assert(offsetof(PerDrawEntry, objectIdRaw)      == 24, "objectIdRaw offset");
static_assert(offsetof(PerDrawEntry, _pad1)            == 28, "_pad1 offset");
```

- [ ] **Step 3: Audit -- which shader files mirror PerDrawEntry?**

```bash
grep -rn "PerDrawEntry\|_pad0" shaders/
```

Expected: at minimum `shaders/static_prop.frag:37-46`. If a coalesce
vertex shader OR a shadow-coalesce variant also mirrors the struct,
list it for the same rename. Spec section 5 ("any coalesce shader
file that mirrors the struct").

- [ ] **Step 4: Edit `shaders/static_prop.frag`**

Existing (verbatim, `:37-46`):

```glsl
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   _pad0;
    int   _pad1;
};
```

Replace with:

```glsl
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;   // M1.5: handle.raw() (read into uint at use site)
    int   _pad1;
};
```

- [ ] **Step 5: If other shaders mirror the struct, apply the same rename**

For each file from Step 3's grep, perform the same `_pad0 ->
objectIdRaw` rename. Track in commit message.

- [ ] **Step 6: Verify rename hard-grep**

```bash
grep -rn "_pad0" shaders/ GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
```

Expected: ZERO hits in shaders/ and in the header. The producer .cpp
must NOT still have `_pad0` after Step 9 below.

- [ ] **Step 7: Locate the PerDrawEntry write site**

```bash
grep -n "PerDrawEntry\|entry\._pad0\|entries\[" GameOS/gameos/gos_static_prop_batcher.cpp | head -30
```

Identify the function (likely a per-type populate inside
`finalizeGeometry()` or the per-frame `flush()` setup) where each
`PerDrawEntry` is assembled.

- [ ] **Step 8: Include the env-flag accessor + registry helpers**

At the top of `gos_static_prop_batcher.cpp` (with other includes):

```cpp
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe
// (gos_static_prop_registry.h is already included by the existing producer; confirm via grep)
```

- [ ] **Step 9: Fill `objectIdRaw` per entry via the C1 three-owner chain**

At the PerDrawEntry aggregate-init site in `gos_static_prop_batcher.cpp`
(grep-confirmed around `:2007-2040`), set:

```cpp
// M1.5 C1 fix: three-owner chain.
//   1. Registry resolves typeID -> recipeIndex (-1 on miss).
//   2. RenderWorld encodes recipeIndex -> handle bits (0 if invalid).
//   3. Batcher writes objectIdRaw.
const int32_t recipeIndex =
    GpuStaticPropRegistry::getRecipeIndexForType(entry.owningTypeID);

entry.objectIdRaw =
    static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(recipeIndex));
```

Env-OFF behavior: the helper still returns nonzero bits, but the
shader does NOT have `#define MC2_OBJECT_ID_BUFFER 1` (Task 8 controls
the macro), so the read site is preprocessed away. Writing nonzero
bits into the renamed slot when env-OFF costs one int store per draw
and is invisible to the shader -- this is acceptable (cheaper than a
branch). If a future profile shows it matters, gate with
`IsObjectIdBufferEnabled()` then.

- [ ] **Step 10: Full relink (atomic rename + producer wire)**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: build GREEN. The struct rename + producer fill land
together with the GLSL mirror; no RED-state commit per C3.

- [ ] **Step 11: Tier1 smoke env-OFF (byte-identical)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. The shader does not yet declare
`layout(location=2) out uint v_objectId` (Task 8). The producer
writes the slot but no shader reads it. Pixels unchanged.

- [ ] **Step 12: Shader-tree redeploy reminder**

Per CLAUDE.md `shader_exe_deploy_lockstep.md`: any slice touching a
shader MUST redeploy the shader tree, not just mc2.exe. This is a
follow-on to whatever standard deploy step the executor performs at
slice close (see Task 13).

- [ ] **Step 13: Atomic combined commit (C3 fix)**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp shaders/static_prop.frag
git commit -m "$(cat <<'EOF'
refactor(static-prop): atomic _pad0 -> objectIdRaw rename + producer wire (M1.5 Task 7, C3 fix)

Single atomic commit per plan-review C3 fix (no RED-build commit):
- C++ PerDrawEntry._pad0 -> objectIdRaw rename + offset static_assert
- GLSL static_prop.frag (and any other shader mirroring the struct)
  rename
- Producer fills entry.objectIdRaw via the C1 three-owner chain:
  GpuStaticPropRegistry::getRecipeIndexForType(typeID) ->
  RenderWorld::objectIdRawForStaticPropRecipe(recipe)

Struct stays 32 B; offset 24 preserved; substitutive (one pad
becomes content; no size change). No shader consumer of the slot
yet; Task 8 adds the layout(location=2) out emit + read.

env-OFF tier1 5/5 byte-identical (shader has no v_objectId out
because MC2_OBJECT_ID_BUFFER macro is not defined; the producer's
nonzero writes are invisible).

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 5, appendix D M3

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 8: Static-prop fragment shader emits `v_objectId` at `layout(location=2)`

**Files:**
- Modify: `shaders/static_prop.frag`
- Modify: C++-side `makeProgram()` call for the static-prop fragment shader (location TBV by grep)

- [ ] **Step 1: Locate the makeProgram() call site**

```bash
grep -rn "static_prop\.frag\|makeProgram.*static_prop\|MC2_COALESCE" GameOS/gameos/ mclib/ | head -20
```

Identify the C++ TU that builds the static-prop fragment program.
Typical pattern: a `makeProgram("static_prop.vert", "static_prop.frag", prefix)`
or similar call. The `prefix` string is where `#define` flags are
injected.

- [ ] **Step 2: Inject the GLSL macro at the C++ prefix site (M2 plan-review fix)**

Per plan-review M2 fix: in `gos_static_prop_batcher.cpp` (around
`:504-572`) the existing prefixes are `static const char*` literals
named `kShaderPrefixLegacy` and `kShaderPrefixCoalesce`. Both must
become runtime `std::string` builders so the env flag can append
`#define MC2_OBJECT_ID_BUFFER 1` conditionally. Pass `.c_str()` to
the existing `makeProgram()` call path.

Existing (verbatim shape; grep-confirm at write time):

```cpp
static const char* kShaderPrefixLegacy   = "#version 430\n";
static const char* kShaderPrefixCoalesce =
    "#version 430\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "#define MC2_COALESCE 1\n";
```

Replace at the point where each program is built:

```cpp
// M1.5 M2 fix: BOTH legacy and coalesce prefixes are runtime builders
// (GLSL preprocessor does not inherit C++ build flags;
// memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
std::string legacyPrefix = "#version 430\n";
if (RenderWorld::IsObjectIdBufferEnabled()) {
    legacyPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
}

std::string coalescePrefix =
    "#version 430\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "#define MC2_COALESCE 1\n";
if (RenderWorld::IsObjectIdBufferEnabled()) {
    coalescePrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
}

// Pass legacyPrefix.c_str() / coalescePrefix.c_str() to the existing
// makeProgram() call sites in place of the prior literal pointers.
```

Confirm `RenderWorld::IsObjectIdBufferEnabled()` is reachable from
this TU (it's declared in `RenderWorld/RenderWorld.h`; add the include
if missing).

- [ ] **Step 3: Add `out uint v_objectId` at BOTH legacy + coalesce sites in `shaders/static_prop.frag` (M2/m3 plan-review fix)**

Per plan-review M2/m3: `static_prop.frag` has TWO `layout(location=0) out`
declarations -- one in the legacy branch (~line 36 per review grep)
and one in the coalesce branch (~line 61). BOTH need the sibling
`layout(location=2) out uint v_objectId;` under
`#ifdef MC2_OBJECT_ID_BUFFER`. Grep-confirm sites at write time:

```bash
grep -n "layout(location = 0) out\|layout(location=0) out" shaders/static_prop.frag
```

At EACH site, the existing shape (verbatim, sample for one site):

```glsl
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;
```

Replace with:

```glsl
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5: per-pixel object handle. Emitted to GL_COLOR_ATTACHMENT2
// (R32UI). Spec section 5; struct field renamed in Task 7.
layout(location = 2) out uint v_objectId;
#endif
```

This applies to BOTH the legacy site AND the coalesce site. Skipping
either one creates a link-time fragment-output mismatch when the macro
is defined (one variant declares the out, the other does not).

- [ ] **Step 4: Add non-coalesce-path uniform for the handle (legacy path)**

The non-coalesce path uploads per-draw uniforms (`u_packetID`,
`u_materialFlags`, etc.). Add `u_objectIdRaw` next to them, gated by
the macro (so env-OFF the shader does not declare the uniform at
all):

Existing (verbatim, `:52-56`):

```glsl
#else
uniform sampler2D u_tex;
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
uniform int       u_maxLocalVertexID;
uniform int       u_packetID;
#endif
```

Replace with:

```glsl
#else
uniform sampler2D u_tex;
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
uniform int       u_maxLocalVertexID;
uniform int       u_packetID;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5 non-coalesce path: handle bits uploaded as int (uniform-uint
// crash trap, memory/uniform_uint_crash.md), cast to uint in body.
uniform int       u_objectIdRaw;
#endif
#endif
```

- [ ] **Step 5: Write `v_objectId` in `main()`**

At the END of `main()` (after `FragColor` and `GBuffer1` have been
set; just before the closing brace):

Existing (verbatim, the end of `main()`):

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
```

Replace with:

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M1.5: emit handle.raw() to attachment-2. Alpha-tested fragments
    // that discard() above skip this write naturally. Coalesce path
    // sources from PerDrawData.entries[]; legacy path from u_objectIdRaw.
#ifdef MC2_COALESCE
    v_objectId = uint(perDraw_.entries[v_drawID + uint(u_drawIDBase)].objectIdRaw);
#else
    v_objectId = uint(u_objectIdRaw);
#endif
#endif
}
```

NOTE: the early-return debug-mode branches (modes 1-8) all `return`
before reaching the post-`FragColor` lines; they will NOT emit
`v_objectId`. In M1.5 this is acceptable -- debug modes are not the
production path and not the canary path. Documented in the commit.

- [ ] **Step 6: Non-coalesce producer upload (only if non-coalesce path is live)**

```bash
grep -n "glProgramUniform1i.*u_packetID\|glUniform1i.*u_packetID" GameOS/gameos/gos_static_prop_batcher.cpp mclib/
```

Identify the per-draw uniform upload site for the non-coalesce path.
Add an explicit-program upload for `u_objectIdRaw` adjacent to it,
gated by `IsObjectIdBufferEnabled()`. Use `glProgramUniform1i` (NOT
`glUniform1i`) per the explicit-program-upload trap
(`memory/glprogramuniform_vs_gluniform_explicit_program_trap.md`):

```cpp
if (RenderWorld::IsObjectIdBufferEnabled()) {
    const GLint loc = glGetUniformLocation(programGl, "u_objectIdRaw");
    if (loc >= 0) {
        glProgramUniform1i(programGl, loc, static_cast<GLint>(handleForDraw.raw()));
    }
}
```

If the coalesce path is the ONLY live path in the build (env var
`MC2_COALESCE=1` is the production setting), this step is a no-op
and we leave the non-coalesce branch alone. Confirm at write time.

- [ ] **Step 7: Full relink + shader redeploy**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Deploy shaders + exe per CLAUDE.md `shader_exe_deploy_lockstep.md`
(per-file `cp -f` + `diff -q`).

- [ ] **Step 8: Tier1 smoke env-OFF (byte-identical)**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS, byte-identical. The shader compiles WITHOUT
the `#define MC2_OBJECT_ID_BUFFER 1` prefix; `out uint v_objectId`
does not exist in the linked program; emission code is preprocessed
away.

- [ ] **Step 9: Tier1 smoke env-ON (functional gate)**

```bash
MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Pixel output (FragColor / GBuffer1) is
visually identical to env-OFF (we only added attachment-2 writes,
which are not displayed). Banner shows `objectid_buffer=on`. FPS
delta within spec budget (Section 10: <=0.5ms p99).

If a regression appears -- e.g. terrain dropped, or a mission that
worked env-OFF goes black env-ON -- it most likely indicates either
(a) Phase A's per-frame `glClearBufferuiv` is failing on the target
driver, or (b) the shader-link added an unintended state change.
Investigate immediately; do not promote.

- [ ] **Step 10: Commit**

```bash
git add shaders/static_prop.frag GameOS/gameos/gos_static_prop_batcher.cpp \
        $(grep -rl "makeProgram.*static_prop" GameOS mclib | head -3)
git commit -m "$(cat <<'EOF'
feat(static-prop): emit Handle.raw() to attachment-2 (M1.5 Task 8)

shaders/static_prop.frag gains layout(location=2) out uint v_objectId
under #ifdef MC2_OBJECT_ID_BUFFER at BOTH legacy and coalesce
output sites (M2 plan-review fix). Coalesce path reads from
perDraw_.entries[].objectIdRaw; legacy path from u_objectIdRaw
(uploaded via glProgramUniform1i per explicit-program-upload trap).
BOTH legacy + coalesce shader-program prefixes are runtime
std::string builders (M2 plan-review fix); each appends
#define MC2_OBJECT_ID_BUFFER 1 when IsObjectIdBufferEnabled().

env-OFF tier1 5/5 byte-identical; env-ON tier1 5/5 PASS with
attachment-2 receiving per-pixel handle.raw() values. Debug-mode
early-return branches do NOT emit v_objectId; that is M1.5-acceptable
(debug modes are not production / canary).

PHASE B CLOSE.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 5,6

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase C -- Validation gates

**Phase C goal:** prove the substrate works on target hardware.
Two in-binary self-tests (OBJECT_ID_SELFTEST canary, substrate
self-test) and one user-driven visual canary.

**Phase C gate (must pass before Phase D starts):** all three gates
green. The canary failure modes are documented STOP conditions for
M2+ promotion.

### Task 9: OBJECT_ID_SELFTEST canary (spec M2 fix)

**Files:**
- Modify: `RenderWorld/RenderWorld.cpp`
- Modify: program-startup / RenderWorld::init wiring

This is the load-bearing runtime evidence (M2 fix in spec Appendix D)
that the integer-MRT clear-then-write-then-read path works on the
target AMD 7900 XTX driver.

- [ ] **Step 1: Add the canary function**

In `RenderWorld/RenderWorld.cpp`, in the namespace block:

```cpp
// M1.5 M2 fix: OBJECT_ID_SELFTEST runtime canary. Mandatory before
// promoting MC2_OBJECT_ID_BUFFER=1 default behavior. Exercises the
// full MRT path against the live driver:
//   1. clear attachment-2 to 0
//   2. draw a single textured quad with a known synthetic handle H
//   3. glReadPixels the quad center; expect H.raw()
//   4. glReadPixels a nearby off-quad pixel; expect 0
//
// Gated by MC2_OBJECT_ID_BUFFER_SELFTEST=1; tier1 perf runs do NOT
// enable this (avoids startup hitch).
//
// Prints [OBJECT_ID_SELFTEST v1] PASS/FAIL one-shot. FAIL is a STOP:
// it indicates the AMD driver's integer-MRT path is misbehaving in
// the configured FBO shape.
void RunObjectIdSelfTest();
```

Implementation skeleton (executor fills the draw with a minimal
single-triangle setup -- a small immediate-mode-equivalent program
that writes `H.raw()` to one pixel via a temporary throwaway shader):

```cpp
void RunObjectIdSelfTest() {
    if (!envFlag("MC2_OBJECT_ID_BUFFER_SELFTEST")) return;
    if (!IsObjectIdBufferEnabled()) {
        std::fprintf(stderr,
            "[OBJECT_ID_SELFTEST v1] SKIPPED reason=env_disabled\n");
        return;
    }
    // ... bind sceneFBO_, setSceneDrawBuffers(MainSceneMRT),
    //     clear attachment-2 to 0, draw 1px quad with synthetic
    //     handle bits, glReadPixels (expect bits), read nearby
    //     pixel (expect 0). Implementation is mechanical; the
    //     test logic is the load-bearing piece -- the executor
    //     authors a minimal shader pair locally inside this
    //     function and tears them down at exit.
    //
    // PASS conditions (all four):
    //   a) read at quad-center pixel == kSyntheticHandle.raw()
    //   b) read at nearby off-quad pixel == 0
    //   c) glGetError() returns GL_NO_ERROR throughout
    //   d) no GL_DEBUG_SEVERITY_HIGH callback fires
    const bool pass = /* all four conditions */ true;
    std::fprintf(stderr,
        "[OBJECT_ID_SELFTEST v1] %s\n", pass ? "PASS" : "FAIL");
}
```

- [ ] **Step 2: Wire the canary call**

In `RenderWorld::init()`, AFTER the existing init banner:

```cpp
RunObjectIdSelfTest();
```

The canary only runs when both `MC2_OBJECT_ID_BUFFER=1` AND
`MC2_OBJECT_ID_BUFFER_SELFTEST=1` are set. Default smoke runs neither.

- [ ] **Step 3: Full relink**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

- [ ] **Step 4: Run the canary on target hardware (7900 XTX)**

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_OBJECT_ID_BUFFER_SELFTEST=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_01
```

Expected: `[OBJECT_ID_SELFTEST v1] PASS` in the run log.

If FAIL: STOP. The AMD driver is not honoring the integer-MRT
clear-then-write-then-read path as the GL spec states. Do NOT
promote any later slice that depends on attachment-2 until the
failure mode is understood (likely: a setSceneDrawBuffers ordering
bug, or an unexpected write-mask interaction). Open a sub-investigation
slice.

- [ ] **Step 5: Commit**

```bash
git add RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): OBJECT_ID_SELFTEST runtime canary (M1.5 Task 9)

Gated by MC2_OBJECT_ID_BUFFER_SELFTEST=1. Validates clear/write/read
of attachment-2 against the live driver: known-handle pixel reads
back as handle.raw(); nearby off-quad pixel reads back as 0. PASS
is the load-bearing M2-fix evidence replacing the previously
uncited AMD claim. FAIL is a STOP for promoting any later slice
that depends on attachment-2.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 12, appendix D M2

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 10: Substrate self-test (m5 hardening; exercises REAL producer path per C1)

**Files:**
- Modify: `RenderWorld/RenderWorld.cpp`

In-binary unit test of the record table + lookupAtPixel. Verifies the
generation-bump-on-destroy and the m5 hardening (after destroy, alive
flag clears).

- [ ] **Step 1: Add the self-test function**

In `RenderWorld.cpp`:

```cpp
// M1.5 substrate self-test (m5 hardening). Gated by
// MC2_RENDER_WORLD_SELFTEST=1. Exercises the REAL producer path per
// plan-review C1 fix (not just the record table in isolation):
//   1. Register a synthetic prop with a known typeID + recipe -> handle h1.
//   2. Call GpuStaticPropRegistry::getRecipeIndexForType(knownTypeID)
//      -> expect recipeIndex matching h1's recipe.
//   3. Call RenderWorld::objectIdRawForStaticPropRecipe(recipeIndex)
//      -> expect bits == h1.raw().
//   4. Decode bits to a Handle h1' and look up s_objectRecords[h1'.index()]
//      -> assert match (alive, generation, gameObjectId).
//   5. destroy(h1).
//   6. Assert s_objectRecords[h1.index()].alive == false (m5).
//   7. Re-register at the same index -> handle h2.
//   8. Assert h2.generation() > h1.generation() (slot recycle).
//   9. Lookup with stale h1 returns invalid (generation mismatch).
//
// GPU readback path is exercised by OBJECT_ID_SELFTEST (Task 9);
// this self-test is pure CPU-side but it walks the FULL three-owner
// chain (registry -> RenderWorld helper -> record table) so a
// regression in any owner surfaces here.
//
// Output: [RENDER_WORLD_SELFTEST v1] PASS/FAIL.
void RunRenderWorldSelfTest();
```

Implementation calls `upsertStaticProp` with a minimal synthetic
StaticPropDesc (empty batch is OK; the record table is populated
even if the legacy backend rejects the batch -- ensure populateRecord
fires before the legacy delegate). The synthetic flow tests record
state, not GPU.

- [ ] **Step 2: Wire and run**

Call from `RenderWorld::init()` AFTER `RunObjectIdSelfTest()`:

```cpp
RunRenderWorldSelfTest();
```

- [ ] **Step 3: Full relink + run**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
MC2_RENDER_WORLD_SELFTEST=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_01
```

Expected: `[RENDER_WORLD_SELFTEST v1] PASS`.

- [ ] **Step 4: Commit**

```bash
git add RenderWorld/RenderWorld.cpp
git commit -m "$(cat <<'EOF'
feat(renderworld): substrate self-test exercising real producer path (M1.5 Task 10)

Gated by MC2_RENDER_WORLD_SELFTEST=1. Walks the FULL three-owner
chain per plan-review C1 fix: register synthetic prop -> registry
getRecipeIndexForType -> RenderWorld objectIdRawForStaticPropRecipe ->
decode bits -> look up s_objectRecords; then destroy/re-register/
stale-lookup cycle. Asserts (a) alive flag clears on destroy
(m5 hardening), (b) generation bumps on re-register, (c) stale
handle returns invalid via generation check. Pure CPU-side test;
no GPU readback (Task 9 covers GPU).

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 12 (m5)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

### Task 11: Three-tier visual + stress canary (plan-review M4 fix)

**Files:** none modified; this is a smoke-only validation step.

Per plan-review M4 fix: replace the prior single-mission canary lean
with an explicit three-tier protocol.

```
M1.5 visual canary (primary):
  run mc2_03 with MC2_OBJECT_ID_BUFFER=1 and MC2_OBJECT_ID_BUFFER_CANARY=1
  sweep cursor over visible buildings and trees
  confirm [RENDER_WORLD_INSPECT v1] lines resolve nonzero handles

M1.5 stress/perf (secondary):
  run mc2_24 with MC2_OBJECT_ID_BUFFER=1
  verify tier1 pass and <= budget frame-time delta

M1.5 mixed check (optional):
  run mc2_10 as fallback if mc2_03 lacks tree+building mix
```

- [ ] **Step 1: Capture pre-canary baseline (env-OFF tier1 5/5) for FPS diff**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
mv tests/smoke/artifacts/<latest> /tmp/m1_5_off_baseline
```

- [ ] **Step 2: Run tier1 env-ON 5/5**

```bash
MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: 5/5 PASS. Capture FPS / frame-time stats from the artifact
directory.

- [ ] **Step 3: Primary visual canary -- mc2_03 with MC2_OBJECT_ID_BUFFER_CANARY=1**

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_OBJECT_ID_BUFFER_CANARY=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 60 --kill-existing --keep-logs --missions mc2_03
```

User moves the cursor over visible buildings and trees; confirms the
emitted `[RENDER_WORLD_INSPECT v1]` lines resolve to nonzero handles.
This is the substantive functional gate.

- [ ] **Step 4: Secondary stress/perf canary -- mc2_24**

```bash
MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_24
```

mc2_24 is the heaviest static-prop mission (2641 props). Verify
tier1 PASS and `<=` budget frame-time delta vs env-OFF baseline
(spec section 10: `<=0.5ms` p99).

- [ ] **Step 5: Optional mixed-mix fallback -- mc2_10**

If mc2_03 does not provide an adequate tree + building mix for the
user's visual confirmation, fall back to mc2_10:

```bash
MC2_OBJECT_ID_BUFFER=1 MC2_OBJECT_ID_BUFFER_CANARY=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 60 --kill-existing --keep-logs --missions mc2_10
```

- [ ] **Step 6: Document FPS delta**

Compare env-OFF vs env-ON p99 frame time per mission. A budget
overage is NOT a blocker for substrate ship, but it IS a blocker
for promoting `MC2_OBJECT_ID_BUFFER=1` to default.

- [ ] **Step 7: Commit (artifacts only; no code change)**

```bash
git commit --allow-empty -m "$(cat <<'EOF'
test(renderworld): three-tier canary -- visual + stress + mixed (M1.5 Task 11)

Plan-review M4 three-tier protocol:
  primary visual:   mc2_03 + MC2_OBJECT_ID_BUFFER_CANARY=1 (cursor sweep)
  stress/perf:      mc2_24 (heaviest 2641 props)
  optional mixed:   mc2_10 (fallback if mc2_03 lacks tree+building mix)

env-ON tier1 5/5 PASS. FPS delta documented in <commit message body>.
Primary canary confirmed [RENDER_WORLD_INSPECT v1] resolves to expected
buildings/trees on mc2_03.

PHASE C CLOSE.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 12

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Phase D -- Closure

### Task 12: Greybeard META-FIX vs PATCH ruling (m5 plan-review fix: no pre-judged verdict)

**This task does NOT modify code.** It records the greybeard ruling.

Subject: `setSceneDrawBuffers()` helper centralization (the C1
mitigation). The subagent must rule fresh; this plan does NOT
prescribe an expected outcome.

- [ ] **Step 1: Dispatch a fresh greybeard subagent**

Per CLAUDE.md "Meta-fix discipline (load-bearing)". Dispatch prompt
(verbatim, must include "run the greybeard skill"):

```
run the greybeard skill. Target: setSceneDrawBuffers() helper
introduced in RenderWorld Slice M1.5, GameOS/gameos/gos_postprocess.cpp.
Source spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md
section 3. Question: is this centralized helper a META-FIX (the
upstream change retiring the "MRT state drift across multi-pass"
bug class adversarial review C1 found) or a PATCH (justified) with
a named follow-up? Rule fresh; the plan does not prescribe a verdict.
```

- [ ] **Step 2: Record the ruling verbatim**

If the greybeard rules **META-FIX**, record the ruling verbatim
(quoted block) in the M1.5 closing commit body.

If the greybeard rules **PATCH (justified)**, record that ruling
verbatim with the named follow-up debt/slice. Either ruling is
acceptable; a missing or unsupported ruling is a slice gate failure.

Sample shape for either verdict (substitute the subagent's actual
text):

> **Greybeard ruling (subagent output, verbatim):**
> [paste exactly what the subagent returned -- META-FIX or PATCH,
> with the subagent's stated reasoning + any named follow-up.]

### Task 13: Document slice in worktree CLAUDE.md (m6 plan-review fix: compressed to M1 style)

**Files:**
- Modify: `.claude/worktrees/nifty-mendeleev/CLAUDE.md`

- [ ] **Step 1: Locate insertion point**

```bash
grep -n "## Active campaigns" .claude/worktrees/nifty-mendeleev/CLAUDE.md
grep -n "RenderWorld Slice M1" .claude/worktrees/nifty-mendeleev/CLAUDE.md
```

Insert IMMEDIATELY AFTER the existing M1 bullet.

- [ ] **Step 2: Add bullet (~10 lines, M1-style; m6 plan-review fix)**

```markdown
- **RenderWorld Slice M1.5** (SHIPPED <date>): object-ID buffer
  substrate. `GL_R32UI` attachment-2 on `sceneFBO_` gated on
  `MC2_OBJECT_ID_BUFFER=1`; centralized `setSceneDrawBuffers()`
  helper retires the MRT-drift bug class adversarial review C1
  flagged. `RenderObjectRecord` table (always populated, ~85 KB
  peak at tier1 mc2_24) + synchronous `lookupAtPixel(x,y)`.
  Static-prop frag emits `Handle::raw()` at `layout(location=2)`
  under `#ifdef MC2_OBJECT_ID_BUFFER`; `PerDrawEntry._pad0` renamed
  to `objectIdRaw` (substitutive; struct stays 32 B). Producer wires
  via a clean three-owner split (registry `getRecipeIndexForType`
  -> `RenderWorld::objectIdRawForStaticPropRecipe` -> batcher).
  Debug-mode pixels return `Handle::invalid()` from lookupAtPixel by
  design (early-return skips emit). Shader macro is restart-required.
  Env-OFF byte-identical to M1 HEAD; env-ON tier1 5/5 +
  OBJECT_ID_SELFTEST PASS on 7900 XTX. Picking wiring deferred to
  M1.6. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`.
  Plan: `docs/superpowers/plans/2026-05-23-renderworld-slice-m1-5-objectid-buffer-plan.md`.
```

- [ ] **Step 3: Commit + close M1.5**

```bash
git add .claude/worktrees/nifty-mendeleev/CLAUDE.md
git commit -m "$(cat <<'EOF'
docs(renderworld): mark Slice M1.5 shipped in CLAUDE.md (M1.5 Task 13)

C1 helper centralization retires MRT-drift bug class (greybeard
ruling recorded in Task 12 commit). Substrate: attachment-2 R32UI,
RenderObjectRecord table, lookupAtPixel, static-prop emit, clean
three-owner registry/RenderWorld/batcher chain. env-OFF byte-identical;
env-ON tier1 5/5 PASS + OBJECT_ID_SELFTEST PASS on 7900 XTX. Picking
wiring deferred to M1.6.

Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md
Plan: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-plan.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Goal-backward verification (slice success proof)

Each item must hold AFTER Task 13 commits.

1. **tier1 5/5 PASS env-OFF default:** byte-identical pixels vs M1 HEAD; `objects=N` matches; `objectid_buffer=off`.
2. **tier1 5/5 PASS env-ON:** banner `objectid_buffer=on`; FPS delta documented; no GL errors.
3. **OBJECT_ID_SELFTEST PASS on 7900 XTX:** with both env vars set; runtime evidence replaces undocumented AMD claim.
4. **Substrate self-test PASS:** record table generation bump on destroy; alive flag clears (m5).
5. **setSceneDrawBuffers helper invariant:** `grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp` shows three helper-body hits ONLY (per M3 plan-review fix: SingleColor 1-entry, MainSceneMRT 3-entry, MainSceneMRT 2-entry fallback).
6. **layout(location=2) uniqueness:** `grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/` matches only `static_prop.frag`.
7. **PerDrawEntry rename complete:** zero `_pad0` hits in `gos_static_prop_batcher.h` or `shaders/static_prop.frag`.
8. **`s_objectRecords` always-populated invariant:** record table writes fire on every upsert/adoption regardless of env flag.
9. **Greybeard META-FIX ruling recorded.**
10. **CLAUDE.md "Active campaigns" updated.**

---

## Risks and mitigations

### R1. MRT state drift across multi-pass (resolved by C1 META-FIX)

**Risk:** raw `glDrawBuffers(...)` calls at any scene-FBO site silently
drop attachment-2 from the active write mask between attachment-create
and static-prop draw. The adversarial review documented FIVE such
sites at HEAD; without the C1 fix, the per-frame `beginScene` rebind
would erase attachment-2 every frame.

**Mitigation:** the `setSceneDrawBuffers()` file-scope helper is the
ONLY allowed `glDrawBuffers` call against `sceneFBO_`. Phase A Task 3
Step 7 hard-grep-gates this; every Phase B+ task includes a re-grep
in its verify step. A future slice that adds a 6th site MUST route
through the helper.

### R2. AMD integer-MRT silent slow path

**Risk:** AMD's driver may treat integer-format MRT attachments as a
slow path even when the GL spec says it should be free. Spec section
10 budget is <=0.5ms p99 frame-time delta.

**Mitigation:** Task 11 captures the FPS delta on the target hardware
(7900 XTX). The OBJECT_ID_SELFTEST canary (Task 9) catches the
hard-failure mode where the driver silently corrupts the readback.
Greater than 20% frame-time regression at p99 BLOCKS promotion of
`MC2_OBJECT_ID_BUFFER=1` to default behavior. Substrate ships either
way (env-OFF default); only the default promotion is gated.

### R3. PerDrawEntry layout drift C++ vs GLSL

**Risk:** Task 7 (atomic C++/GLSL rename + producer wire) lands in a
single commit. A future change that adds fields to one side without
the other corrupts the std430 stride per
`memory/cpp_glsl_ubo_struct_lockstep.md`.

**Mitigation:** the `static_assert(sizeof(PerDrawEntry) == 32)` and
the per-offset asserts in `gos_static_prop_batcher.h` are the
compile-time safety net. Phase B Task 8 Steps 8/9 are the runtime
gates (tier1 5/5 env-OFF + env-ON). The rename itself is substitutive
(size + offset unchanged); the failure mode is symbolic, not binary.
C3 plan-review fix: rename + producer + GLSL mirror land atomically in
ONE commit -- no RED-build commit window.

### R4. lookupAtPixel synchronous stall

**Risk:** synchronous `glReadPixels` of a single integer pixel stalls
the GPU for ~1-5ms typical. Calling per-frame would tank performance.

**Mitigation:** `lookupAtPixel` is a debug / click-rate API ONLY.
Spec section 7 documents this explicitly. M1.6's picking integration
will call it at most ~10/sec (human click rate). PBO async readback
is documented as future work in spec section 1 non-goals; not in M1.5.

### R5. `s_objectRecords` memory pressure

**Risk:** always-populated table consumes per-mission memory.

**Mitigation:** worst case at tier1 mc2_24 is ~85 KB (~32 bytes per
record * ~2641 records). M1 decision: trivial cost; documented in
spec section 9 ("Runtime overhead when OFF"). If a future workload
introduces > 100K props per mission, table can be env-gated as a
follow-up slice -- but spec section 9 explicitly rationalizes against
env-gating now (one branch-not-taken vs distributed env checks at
every reader).

### R6. Shader hot-reload bypassing macro flag

**Risk:** runtime shader hot-reload may bypass `IsObjectIdBufferEnabled()`
caching and pick up the GLSL macro flag at recompile time, producing
a shader-program / FBO mismatch (program declares `out` at location=2
but the env-OFF FBO has no attachment).

**Mitigation:** the env-flag accessor caches at FIRST CALL. The
`makeProgram()` prefix is computed at program-LOAD time. Toggling the
env var at runtime has NO effect on the linked program (the cache
returns the original value; the prefix was already baked in).
Hot-reload, if it re-runs `makeProgram()`, will see the SAME cached
value and produce the SAME shader. Toggling the flag requires a
process restart. Documented in O3 + the M1.5 CLAUDE.md bullet.

---

## Rollback strategy per phase

- **Phase A (Tasks 1-5):** all modifications are gated on
  `IsObjectIdBufferEnabled()` returning true (which it does not by
  default). Revert by `git revert` of each task commit, in reverse.
  Task 5 is a single atomic commit (C2 fix); revert it whole.
  No game-side state touched.
- **Phase B (Tasks 6-8):** Task 7 is atomic (rename + producer + GLSL
  mirror in one commit per C3 fix); Task 6 (registry/RenderWorld
  helpers per C1) can be reverted independently only AFTER Task 7 is
  reverted (Task 7 calls into Task 6's helpers). Phase B revert order:
  8, then 7, then 6.
- **Phase C (Tasks 9-11):** self-tests are env-gated; reverting drops
  the canary but does NOT regress runtime behavior.
- **Phase D (Tasks 12-13):** docs / ruling only; revertable in
  isolation.

**Full M1.5 rollback:** `git revert` Tasks 13..1 (in reverse), OR
`git reset --hard <pre-M1.5-sha>`. All slice changes are confined to
the listed files.

---

## Build + smoke gate per phase (canonical commands)

**Build (every task that compiles code):**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Skip the `rm` line only for header-only edits with no static-state
implications.

**Smoke (every phase boundary; every Phase B task):**

```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

**Smoke env-ON (Phase B Task 8, Phase C Tasks 9-11):**

```bash
MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## Out of scope for M1.5 (explicit)

Per spec section 1 non-goals + scope narrowing 2026-05-23:

- Picking integration / `code/missiongui.cpp` modifications (M1.6)
- Mech / terrain / VFX / overlay attachment-2 writers (M2-M5)
- Async PBO readback infrastructure (future)
- Drag-rectangle selection (future)
- In-engine HUD scene-inspector overlay (future)
- Vulkan port of the buffer (future; spec Q9 documents shape preserve)
- `PipelineId` / `DrawPacket` / `pathReasonCode` real values (sentinels in M1.5; future slices)
- Glob migration of all scene-FBO textures to `glTexStorage2D` (separate modernization slice; m4 decision)
- Always-populated record table env-gating (one extra branch; documented overhead acceptable; would be additive split per M1 decision)

---

## Vulkan-prep restatement (load-bearing per CLAUDE.md)

Every M1.5 type and call was checked against the "is this expressible
in Vulkan?" test:

- `GL_R32UI` MRT attachment -> `VK_FORMAT_R32_UINT` color attachment.
- `out uint v_objectId` -> SPIR-V `OpTypeInt 32 0` fragment output.
- `glReadPixels(GL_RED_INTEGER, GL_UNSIGNED_INT)` -> `vkCmdCopyImageToBuffer` + fence-wait (Vulkan needs explicit staging + fence; M1.5 GL path is the synchronous shortcut).
- `RenderObjectRecord` / `LookupResult` -> plain C++ POD; backend-independent.
- `setSceneDrawBuffers()` helper -> in Vulkan the equivalent is `vkCmdBeginRenderingKHR` (dynamic rendering) attachment list; the central helper concept survives the port.

The Vulkan equivalence is documented; the M1.5 GL implementation is
the substrate; the port is future work outside this slice.

---

## Self-review against spec (writing-plans skill mandate)

- Spec section 3 (FBO architecture + setSceneDrawBuffers): Tasks 2-4. Covered.
- Spec section 4 (RenderObjectRecord): Task 5 (combined substrate API per C2). Covered.
- Spec section 5 (fragment shader contract + PerDrawEntry rename): Tasks 6-8. Covered.
- Spec section 6 (background-pass behavior): Task 8 ensures only `static_prop.frag` declares location=2; Phase D verification grep enforces.
- Spec section 7 (debug API): Task 5 (lookupAtPixel implementation). Covered.
- Spec section 8 (picking lifecycle): DEFERRED to M1.6 per spec scope narrowing; M1.5 plan explicitly skips `missiongui.cpp` modifications.
- Spec section 9 (env gating + always-populated table): Tasks 1, 5. Covered.
- Spec section 10 (perf budget): Task 11. FPS delta captured; budget enforced at default-promotion gate.
- Spec section 11 (forbidden behaviors): no async PBO, no separate ID pass, no SSBO for inspection table. Plan honors all.
- Spec section 12 (validation gates): Tasks 9-11. Covered. Tier1, three-tier canary (M4), substrate self-test, OBJECT_ID_SELFTEST canary all wired.
- Spec section 13 (M2 extension path): documented in spec; M1.5 substrate is shaped so M2 mech extension is three-lines per shader.
- Spec section 14 Q resolutions: Q7 (glTexImage2D) honored Task 4; Q8 (PerDrawEntry rename) honored Task 7.

**No placeholders.** Every task has explicit files, exact code, exact
commands. Per the writing-plans skill self-review checklist, no
`TBD`, no "implement later", no "add appropriate error handling."

**Type consistency:** `RenderObjectHandle` consumed in Tasks 5, 6, 7.
`RenderObjectRecord` + `LookupResult` introduced, table populated, and
`lookupAtPixel` implemented in the single atomic Task 5 commit (C2 fix).
`setSceneDrawBuffers` introduced Task 2, consumed Tasks 3, 4 (with the
M3 objectIdAttachmentReady parameter).
`getRecipeIndexForType` + `objectIdRawForStaticPropRecipe` introduced
Task 6 (C1 fix), consumed Task 7 (producer) and Task 10 (substrate
self-test).
`IsObjectIdBufferEnabled` introduced Task 1, consumed Tasks 2, 4, 5,
7, 8, 9, 10.

**Spec ambiguities flagged for user (Open items section):**
- O1: env-flag accessor location and shape (lean: `RenderWorld/RenderWorld.h`).
- O2: visual canary mission (lean: existing tier1 mc2_24).
- O3: shader-recompile-on-env-change semantics (lean: restart required; document).

---

## Pre-execution / must-pass before merge gates

Every M1.5 merge candidate MUST satisfy ALL ten gates below. Failing
any one is a slice failure; do not merge until restored.

1. **Firewall script `scripts/check-include-firewall.sh` exits 0.**

   ```bash
   sh scripts/check-include-firewall.sh
   ```

   Expected: exit 0. M1.5 adds no new game-side dependencies; the
   M1 firewall promise is preserved.

2. **tier1 5/5 PASS env-OFF default.**

   ```bash
   py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
   ```

   Expected: 5/5. Banner `objectid_buffer=off`.

3. **tier1 5/5 PASS env-ON (`MC2_OBJECT_ID_BUFFER=1`).**

   ```bash
   MC2_OBJECT_ID_BUFFER=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
   ```

   Expected: 5/5. Banner `objectid_buffer=on`. FPS delta documented;
   substrate ships even if > 0.5ms p99 (only default-promotion is
   gated).

4. **`[STATIC_PROP_REGISTRY v1]` counts unchanged vs M1 HEAD env either way.**

   ```bash
   grep -h "STATIC_PROP_REGISTRY v1" tests/smoke/artifacts/<latest>/*.log
   ```

   Expected: M1 baseline counts preserved (mc2_01=997, mc2_03=2552,
   mc2_10=2611, mc2_17=1521, mc2_24=2641). M1.5 does NOT touch the
   registry surface.

5. **`[RENDER_WORLD v1] objects=N` matches M1 HEAD pattern env either way.**

   The `objects=` field still sources from `legacy::getStaticPropActiveCount()`
   (the M1 m4 fix). M1.5 adds the `objectid_buffer=` token to the same
   banner line; the existing field is unchanged.

6. **OBJECT_ID_SELFTEST canary PASS on 7900 XTX.**

   ```bash
   MC2_OBJECT_ID_BUFFER=1 MC2_OBJECT_ID_BUFFER_SELFTEST=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_01
   ```

   Expected: `[OBJECT_ID_SELFTEST v1] PASS` in the artifact log.

7. **Substrate self-test (m5) PASS.**

   ```bash
   MC2_RENDER_WORLD_SELFTEST=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --missions mc2_01
   ```

   Expected: `[RENDER_WORLD_SELFTEST v1] PASS`.

8. **setSceneDrawBuffers helper invariant.**

   ```bash
   grep -n "glDrawBuffers" GameOS/gameos/gos_postprocess.cpp
   ```

   Expected: THREE hits ONLY, all inside the `setSceneDrawBuffers()`
   body (1-entry SingleColor, 3-entry MainSceneMRT env-ON, 2-entry
   MainSceneMRT fallback after M3 plan-review fix). ANY hit outside
   the helper body is a slice failure (C1 invariant broken).

9. **Static-prop frag has `layout(location=2) out uint v_objectId` ONLY when MC2_OBJECT_ID_BUFFER=1 macro defined.**

   ```bash
   grep -rE 'layout\s*\(\s*location\s*=\s*2\s*\)\s*out' shaders/
   ```

   Expected: ONE hit only -- inside `shaders/static_prop.frag` under
   `#ifdef MC2_OBJECT_ID_BUFFER`. Any other shader file declaring
   location=2 output is a slice failure (cross-shader collision).

10. **Greybeard PATCH-or-META-FIX ruling recorded.**

    Task 12's ruling is in the M1.5 closing commit message body (or
    in the CLAUDE.md "Active campaigns" bullet, Task 13). Either form
    is acceptable; missing it is a slice failure.

If a gate cannot be satisfied, the executor MUST file a deviation
note in the M1.5 closing commit and the gate MUST be addressed by a
follow-up patch BEFORE the slice is declared shipped. Skipping a gate
silently is the same class of error the pre-execution review pass
exists to catch.

End of plan.
