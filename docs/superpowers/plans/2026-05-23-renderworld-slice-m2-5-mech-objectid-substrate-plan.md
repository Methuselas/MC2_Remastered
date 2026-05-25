# RenderWorld Slice M2.5 -- Mech ObjectID Substrate Implementation Plan

**PLAN STATUS: READY FOR EXECUTE -- external-review fixes applied (C1+M1+M2+M3+m1+m2+m3)**

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

---

## External review fixes applied

External greybeard review verdict: EXECUTE WITH FIXES. All 7 findings applied verbatim:

1. **C1 (CRITICAL):** Moved `extern "C" uint64_t consumeAndResetMlrMechDraws();` declaration from inside `GpuMechBatcher::onMapUnload()` body to file scope in `gos_mech_batcher.cpp` (top-of-file alongside other engine includes). Definition stays at file scope in `mclib/mech3d.cpp`. Added `#include <cstdint>` to `mclib/mech3d.cpp` (grep-confirmed absent; `uint64_t` not currently used in that TU). `GameOS/gameos/gos_mech_batcher.cpp` already pulls `uint64_t` transitively (8 existing uses); plan documents the include site. T5 Step 4 + commit message updated to reflect file-scope declaration + body-only call shape.
2. **M1 (MAJOR):** Normalized split summary log shape as the OFFICIAL output everywhere. Removed all wording suggesting the two counters might share one banner line. Validation gates in T7 require BOTH adjacent lines (mlr first, then gpu_mech_id). Split justified explicitly: the two emitters live in different TUs (`gos_mech_batcher.cpp` for `event=mech_id_summary`; `gos_mech_batcher.cpp` also for `event=mlr_mech_summary` after consuming the MLR counter via cross-TU getter into `mclib/mech3d.cpp`).
3. **M2 (MAJOR):** Added new shader-output uniqueness gate after T4: `Select-String` for `layout(location=2) out` across `shaders/*.frag` must return EXACTLY two files (`static_prop.frag` + `mech.frag`). Added companion `flat` qualifier gate for `mech.vert`/`mech.frag` integer varying.
4. **M3 (MAJOR):** T2 Step 2 now explicitly grep-checks for `#include <string>` in `gos_mech_batcher.cpp` before adding, and adds it if absent. (`mechPrefix` is `std::string`.)
5. **m1 (MINOR):** File-structure note no longer claims `RenderWorld/RenderWorld.h` is modified. Forward declaration stays in `RenderWorld.cpp` next to `RunGameplayPickSelfTest`. No public header change.
6. **m2 (MINOR):** Verified `event=shader_ok` at `gos_mech_batcher.cpp:274` is ALWAYS-ON (emitted inside `loadProgramsIfNeeded()` after a successful link; NOT trace-gated). **Choice: option (a) is unnecessary** -- the gate works as written. Plan documents the verified line number so executor can re-grep.
7. **m3 (MINOR):** Gate 5 wording verified substrate-only: Shift+click on a mech must NOT produce a `[STATIC_PROP_PICK v1] hit` line (mover-first short-circuit + M2.6 owns mech pick). Wording already correct; rechecked and tightened.

**Goal:** Close the M2 chain: emit `Mech3DAppearance::mechRenderHandle.raw()` to `GL_COLOR_ATTACHMENT2` via `mech.frag` under `#ifdef MC2_OBJECT_ID_BUFFER`, plus measurable writer / MLR-fallback observability counters and a synthetic mech-ID self-test.

---

## Adversarial findings applied (CONDITIONAL-PASS revision)

Review: `docs/superpowers/reviews/2026-05-23-renderworld-slice-m2-5-plan-adversarial.md` (2026-05-23). All 4 findings landed below.

1. **MAJOR (T3 Step 5 missing third reset site at `gos_mech_batcher.cpp:1365`)** — RESOLVED by **dropping `s_gpuMechIdWritesThisFrame` entirely**. Grep against the plan confirmed the per-frame counter had NO consumer (only `s_gpuMechIdWritesThisMission` is read by the `event=mech_id_summary` emit). Reviewer-preferred option (b): "delete `s_gpuMechIdWritesThisFrame` entirely -- `s_gpuMechIdWritesThisMission` is the only counter actually consumed by an emit, so the per-frame variable is dead state. Option (b) is preferred (simpler, removes the bug class)." T3 Step 3 / Step 4 / Step 5 / Step 6 revised: per-mission counter is incremented directly in the per-instance loop; the per-frame reset sites are now untouched by M2.5 (no per-frame counter to maintain).
2. **MINOR m1 (line-348 mislabel as `endFrame()`)** — RESOLVED. T3 Step 5 prose now correctly identifies the three `s_eligibleActorsThisFrame = 0` sites: line 348 (`onMapLoad()` per-mission), line 992 (`flush()` early-out), line 1365 (`flush()` normal end). Since the per-frame counter is dropped (above), the misnamed reset prose is moot, but the citation is corrected for accuracy.
3. **MINOR m2 (invented `mclib_` prefix)** — RESOLVED. Renamed `mclib_consumeAndResetMlrMechDraws` (original) -> `consumeAndResetMlrMechDraws` (revised; lowerCamel, no subsystem prefix; mirrors the closest existing `extern "C"` convention `elfHash` in `mclib/fst_hash.cpp:15`).
4. **MINOR m3 (dead-code fallback for `Handle::bits`)** — RESOLVED. `RenderCore/Handle.h:30` confirms `struct Handle { uint32_t bits = 0; ... }` -- `bits` is public. T6 Step 3 now uses `RenderCore::RenderObjectHandle::make(idx, gen)` unconditionally; the "if private" fallback prose is deleted.

PLAN STATUS line at end of file updated accordingly.

**Architecture:** Per-instance SSBO field (`objectIdRaw`) added to `GpuMechInstance` (48B -> 64B, three trailing pad uints for explicit std430), threaded through `GpuMechBatcher::flush()` from a new `GpuMechSubmitDesc::objectIdRaw` carrier. `mech.vert` reads it and forwards as `flat out uint v_objectIdRaw`; `mech.frag` declares `layout(location=2) out uint v_objectId` and emits the bits, all macro-gated. GLSL prefix injection mirrors the shipped `gos_static_prop_batcher.cpp:510-521` pattern. Submit-site assignment is unconditional per Q3. Two new always-on per-mission counters (`gpu_mech_id_writes`, `mlr_mech_draws`) and a synthetic `RunMechObjectIdSelfTest()` wired into `RenderWorld::init()` complete the substrate observability.

**Tech Stack:** C++14, OpenGL 4.5 (GL_ARB_shader_storage_buffer_object std430), Windows/MSVC, CMake 3.x, PowerShell smoke runner.

---

## Plan-stage blocker resolutions

### B1 -- Citation drift verified

Every citation in the spec was re-grepped at plan-write time. One small drift fixed inline:

| Spec citation | Verified value | Status |
|---|---|---|
| `gos_mech_batcher.h:35-51` `GpuMechInstance` 48B + asserts | lines 33-51 (header comment + struct + asserts) | OK (header comment + struct band) |
| `gos_mech_batcher.h:88-106` `GpuMechSubmitDesc` | lines 87-106 | OK |
| `gos_mech_batcher.h:119` `MECH_RING_FRAMES = 3u` | line 119 | EXACT |
| `gos_mech_batcher.cpp:218-233` `loadProgramsIfNeeded` | lines 218-233 (header) + 218-275 (body) | EXACT band; ends line 275 |
| `gos_mech_batcher.cpp:1093-1104` per-instance fill loop | lines 1093-1103 (inner) wrapped in bucket loop 1082-1106 | OK (spec band correct) |
| `gos_mech_batcher.cpp:1109-1111` SSBO binding 0 range | lines 1109-1111 | EXACT |
| `gos_mech_batcher.cpp:1337-1340` `[MECHBATCHER v1] event=summary` | lines 1336-1340 | OK |
| `gos_mech_batcher.cpp:1336` summary GATED ON `s_mechBatcherTrace` | line 1329 `if (s_mechBatcherTrace)` block enclosing 1336-1348 | **GATED** -- see Citation drift §below |
| `mech.vert` GLSL struct lines 30-37 | lines 30-37 (SSBO bind 38-40 follows) | EXACT |
| `mech.vert` end-of-main varying assigns 165-173 | lines 165-173 | EXACT |
| `mech.frag` location=0/1 outs lines 36-37 | lines 36-37 | EXACT |
| `mech.frag` end-of-main 75-77 | lines 75-77 (file ends at line 77) | EXACT |
| `mech3d.cpp:2549-2586` submit site | lines 2549-2586 (body 2538-2601 surrounding gate) | EXACT |
| `mech3d.cpp:2608` MLR fallback `mechShape->Render(true)` | line 2608 | EXACT |
| `mech3d.h:478` `mechRenderHandle` | line 478 | EXACT |
| `mech3d.h:487-489` `getRenderWorldHandle()` accessor | lines 487-489 | EXACT |
| `static_prop.frag:56-60` legacy `u_objectIdRaw` | lines 56-60 | EXACT |
| `static_prop.frag:68-72` `layout(location=2) out uint` | lines 68-72 | EXACT |
| `static_prop.frag:174-181` body emit under macro | lines 174-181 | OK (spec said 174-183) |
| `gos_static_prop_batcher.cpp:510-521` prefix injection | lines 510-521 | EXACT |
| `gos_mech_batcher.cpp` does NOT include `RenderWorld.h` | grep returns 0 hits | CONFIRMED (spec correct) |
| `RenderWorld.h:85` `IsObjectIdBufferEnabled` | line 85 | EXACT |
| `RenderWorld.h:116-120` `RenderObjectKind` | lines 116-120 | EXACT |
| `RenderWorld.cpp:40,365` `RunGameplayPickSelfTest` | line 40 (decl), 365 (call) | EXACT |
| `RenderWorld.cpp:250` `runSubstrateSelfTest` | line 250 (def), 361 (call) | EXACT |

### Citation drift fixes (load-bearing)

1. **`[MECHBATCHER v1] event=summary` is GATED.** Spec §6 + Q4 + Q6 amendment 2 imply the counters live on the existing summary line. Code reality: that line at `gos_mech_batcher.cpp:1336-1340` is wrapped in `if (s_mechBatcherTrace)` at line 1329, where `s_mechBatcherTrace = (getenv("MC2_MECH_BATCHER_STATS") != nullptr)`. Counters mandated by Q4 (`gpu_mech_id_writes`) and Q6 amendment 2 (`mlr_mech_draws`) are required to be **always-on per-mission** so the M2.6 decision rule has live data. **Plan choice (external-review fix M1: split-line is OFFICIAL):** emit TWO new always-on stderr lines from `GpuMechBatcher::onMapUnload()` (per-mission lifecycle hook, already present), one per counter. The two emitters live in different TUs: GPU writes are counted in `gos_mech_batcher.cpp` itself; MLR draws are counted in `mclib/mech3d.cpp` and consumed via an `extern "C"` getter (`consumeAndResetMlrMechDraws()`) at the unload hook. Split-line shape is sanctioned by spec Q6 amendment 2 "split across two log lines if MLR draws live in a different TU". The two lines emit adjacent (mlr first, then gpu_mech_id) so log-scrapers see them together. Single-banner shape is explicitly NOT supported in M2.5; do not collapse to one line.

    Always-on `event=shader_ok prog=N` exists at `gos_mech_batcher.cpp:274` (verified during external review m2): emitted inside `loadProgramsIfNeeded()` after the successful link, NOT trace-gated. T7 Gate 2 can require its presence directly.

2. **`mech.frag` is 77 lines total.** Spec §4.4.2 shows the body write inserted after `GBuffer1 = ...` at line 76. File ends at line 77 with `}`. Body inserts go BEFORE the closing brace, not after line 76 unconditionally -- verified verbatim in Task 4 Step 3.

3. **`gos_mech_batcher.h` already includes `mech3d.h`.** Line 11. The new `objectIdRaw` field in `GpuMechSubmitDesc` is plain `uint32_t`; no new include needed in the header. The .cpp gains one new include (`RenderWorld/RenderWorld.h`) for `IsObjectIdBufferEnabled()` -- unpoliced per Q5.

### B2 -- Discard path on `mech.frag` alpha-test confirmed

`mech.frag:55-57` `if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) { discard; }`. Spec §4.4.2 correctly notes that discarded fragments skip the `v_objectId = ...` write naturally -- the attachment-2 pixel retains the clear value (0 = `Handle::invalid()`). No special-casing needed in the body write.

### B3 -- Debug-mode override pixels

`mech.frag:65-73` (debug modes 1-9) can replace `c` with arbitrary colors, but they DO NOT discard. The body write at the end runs regardless. M2.5 substrate is correct in env-ON: those debug pixels still carry the actor's handle. Acceptable: debug-mode pixels are a developer-only path, and emitting a valid handle for them is the correct substrate behavior (lookupAtPixel returns the right mech).

### B4 -- `getRenderWorldHandle()` accessor scope at submit site

`mech3d.cpp:2549-2586` runs inside a `Mech3DAppearance` member function (verified by surrounding context at line 2509 entering the per-mech render loop; `mechShape`, `hazeFactor`, `localTextureHandle` are `this->` members). The M2 accessor `getRenderWorldHandle()` is public on `Mech3DAppearance` (`mech3d.h:487-489`) and inline -- direct call is correct; no friend/static_cast/adapter call.

---

## File structure

**Modified files:**

- `GameOS/gameos/gos_mech_batcher.h` -- add `uint32_t objectIdRaw` to `GpuMechSubmitDesc` (line 106 area); grow `GpuMechInstance` from 48B to 64B with one new `objectIdRaw` field + three `_padN` slots; extend `static_assert` chain.
- `GameOS/gameos/gos_mech_batcher.cpp` -- add `#include "../../RenderWorld/RenderWorld.h"`; gate `loadProgramsIfNeeded` shader prefix on `IsObjectIdBufferEnabled()` (mirrors `gos_static_prop_batcher.cpp:510-521`); fill `inst.objectIdRaw` in per-instance loop; add per-mission `s_gpuMechIdWritesThisMission` counter (per-frame counter dropped per adversarial M1 -- it had no consumer); emit always-on `[MECHBATCHER v1] event=mech_id_summary` line from `onMapUnload()`.
- `shaders/mech.vert` -- grow GLSL `GpuMechInstance` struct in lockstep (lines 30-37); add gated `flat out uint v_objectIdRaw` declaration; add gated `v_objectIdRaw = inst.objectIdRaw;` write at end of `main()`.
- `shaders/mech.frag` -- add gated `flat in uint v_objectIdRaw;` + `layout(location=2) out uint v_objectId;`; add gated `v_objectId = v_objectIdRaw;` body write.
- `mclib/mech3d.cpp` -- one unconditional assignment at the M2 submit site `desc.objectIdRaw = getRenderWorldHandle().raw();` (Q3).
- `mclib/mech3d.cpp` (MLR fallback) -- increment `s_mlrMechDrawsThisMission` counter at `mechShape->Render(true)` site (line 2608). Counter is declared file-scope; an `extern "C" uint64_t consumeAndResetMlrMechDraws()` getter at file scope is consumed by `GpuMechBatcher::onMapUnload()` (different TU). **The split-line shape is OFFICIAL** (per external-review fix M1): two separate stderr lines emit at mission unload, mlr first then gpu_mech_id, justified by the two counters living in different TUs. Add `#include <cstdint>` to this TU (grep-verified absent; `uint64_t` not currently used here).
- `RenderWorld/RenderWorld.cpp` -- add `RunMechObjectIdSelfTest()` (Q1); add forward declaration adjacent to existing `RunGameplayPickSelfTest` decl at line 40 (`.cpp`-local; mirrors M2-pre pattern); wire into `init()` after `RunGameplayPickSelfTest()`. **No public header change** -- `RenderWorld/RenderWorld.h` is NOT modified (external-review fix m1).
- `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md` -- add M2.5 SHIPPED to Active campaigns; add MLR-fallback note to Known issues (verbatim Q6 amendment 1 paragraph).

**Created files:** none.

**Per Q5:** no firewall-script edit, no allowlist line. `GameOS/` is outside `SCOPE_DIRS` at `scripts/check-include-firewall.sh:22`.

---

## Critical inline rules (encoded throughout steps)

These rules from `CLAUDE.md` are load-bearing for M2.5; restated here so each Task step inherits them:

- **Build:** ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`.
- **Full relink discipline:** Task 1 (struct layout change 48B -> 64B) is a class-layout change touching multiple TUs. Per `memory/feedback_class_layout_change_needs_clean_first.md`: delete `mc2.exe` AND `build64/RelWithDebInfo/CMakeFiles` before `cmake --build`. Without this, the running exe may hold the OLD 48B stride while the freshly-loaded shader expects the NEW 64B layout (spec §11 "Hot-reload-without-relink trap"; symptom: wrong-mech meshes).
- **Shaders deploy in lockstep with exe** (`memory/shader_exe_deploy_lockstep.md`). The smoke runner reads shaders from the worktree source tree directly (`shaders/...` relative path), so the only deploy site that matters for M2.5 smoke validation IS the worktree shader edit; no separate `cp` to deploy. Verify per-task by checking that smoke `[MECHBATCHER v1] event=shader_ok prog=N` appears in the artifacts log after env-ON runs.
- **GLSL macros do NOT inherit C++ build flags** (`memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`). The macro `MC2_OBJECT_ID_BUFFER` MUST be injected into the C++-side `makeProgram()` prefix; Task 2 implements this.
- **`flat` qualifier on integer varyings is MANDATORY.** GL spec FORBIDS linear interpolation of integer varyings. Missing `flat` produces a link error -> `[MECHBATCHER v1] event=shader_fail` -> GPU mech path goes silently dormant. Verify per Task 4 Step 7 by grepping for `event=shader_ok` in env-ON artifacts.
- **`uniform uint` crash trap** (`memory/uniform_uint_crash.md`). M2.5 declares NO new uniforms (the ObjectID rides the SSBO); the trap does not apply.
- **`glProgramUniform*` vs `glUniform*` trap.** Not applicable: M2.5 uses SSBO writes, not uniform uploads.
- **Smoke command verbatim** (worktree path): `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs`

---

## Task 1: Grow `GpuMechInstance` + `GpuMechSubmitDesc` structs (lockstep with shader)

**This task is one atomic commit.** Per spec §11 "Lockstep edit risk": the C++ struct change in this commit MUST land together with the GLSL struct change in Task 4 step 1 + the macro injection in Task 2. Steps 1-7 below land the C++ side; do NOT commit until Task 2 (prefix injection) and Task 4 (shader struct + writes) are also staged. The single commit at the end of Task 4 encompasses Tasks 1-4 to keep struct + shader in lockstep.

**Files:**

- Modify: `GameOS/gameos/gos_mech_batcher.h` -- grow both structs

**Verification:**

- [ ] **Step 1: Re-grep the existing struct band before editing**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.h" -Pattern "struct alignas\(16\) GpuMechInstance" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.h" -Pattern "sizeof\(GpuMechInstance\) == 48" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.h" -Pattern "struct GpuMechSubmitDesc" | Select-Object LineNumber, Line
```

Expected:
- `struct alignas(16) GpuMechInstance` at line 35.
- `sizeof(GpuMechInstance) == 48` static_assert at line 44.
- `struct GpuMechSubmitDesc` at line 88.

If any line drifts more than +/- 5 lines, locate the symbol with a wider grep and adjust the `Existing:` blocks below.

- [ ] **Step 2: Grow `GpuMechInstance` from 48B to 64B**

**Existing (`GameOS/gameos/gos_mech_batcher.h` lines 33-51):**

```cpp
// Per-instance GPU record — std430, 48 bytes.
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert IN LOCKSTEP.
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  // resolved Mech3DAppearanceType × LOD record index
    uint32_t baseBoneOffset;      // index into per-frame bone SSBO for this actor's nodes
    uint32_t lightDataIndex;      // into LightsData UBO (Slice B1; 0 in Slice A)
    uint32_t renderFlags;         // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    float    aRGBHighlight[4];    // forwarded to FS as v_highlightColor
    float    fogRGB[4];           // forwarded to FS as v_fogRGB
};
// Layout: 16 (4 × uint32) + 16 (vec4) + 16 (vec4) = 48 bytes.
static_assert(sizeof(GpuMechInstance) == 48,
              "GpuMechInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuMechInstance, typeLodRecordIndex) ==  0);
static_assert(offsetof(GpuMechInstance, baseBoneOffset)     ==  4);
static_assert(offsetof(GpuMechInstance, lightDataIndex)     ==  8);
static_assert(offsetof(GpuMechInstance, renderFlags)        == 12);
static_assert(offsetof(GpuMechInstance, aRGBHighlight)      == 16);
static_assert(offsetof(GpuMechInstance, fogRGB)             == 32);
```

**Replace with:**

```cpp
// Per-instance GPU record — std430, 64 bytes (M2.5: was 48 bytes).
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert IN LOCKSTEP.
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  // resolved Mech3DAppearanceType × LOD record index
    uint32_t baseBoneOffset;      // index into per-frame bone SSBO for this actor's nodes
    uint32_t lightDataIndex;      // into LightsData UBO (Slice B1; 0 in Slice A)
    uint32_t renderFlags;         // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    float    aRGBHighlight[4];    // forwarded to FS as v_highlightColor
    float    fogRGB[4];           // forwarded to FS as v_fogRGB
    // M2.5: RenderObjectHandle.raw() emitted by mech.frag as
    //   layout(location=2) out uint v_objectId
    // under #ifdef MC2_OBJECT_ID_BUFFER. 0 = Handle::invalid()
    // (clear-value match -- background read by lookupAtPixel).
    uint32_t objectIdRaw;         // 48
    // Per Q2 resolved: generic _padN names; only the consumed slot is
    // named. Future slices (M3 terrain chunk, M4 VFX) rename in place.
    uint32_t _pad1;               // 52
    uint32_t _pad2;               // 56
    uint32_t _pad3;               // 60
};
// Layout: 16 (4 × uint32) + 16 (vec4) + 16 (vec4) + 16 (uint32 + 3*pad) = 64 bytes.
static_assert(sizeof(GpuMechInstance) == 64,
              "GpuMechInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuMechInstance, typeLodRecordIndex) ==  0);
static_assert(offsetof(GpuMechInstance, baseBoneOffset)     ==  4);
static_assert(offsetof(GpuMechInstance, lightDataIndex)     ==  8);
static_assert(offsetof(GpuMechInstance, renderFlags)        == 12);
static_assert(offsetof(GpuMechInstance, aRGBHighlight)      == 16);
static_assert(offsetof(GpuMechInstance, fogRGB)             == 32);
static_assert(offsetof(GpuMechInstance, objectIdRaw)        == 48);
```

- [ ] **Step 3: Grow `GpuMechSubmitDesc` with one new field**

**Existing (`GameOS/gameos/gos_mech_batcher.h` lines 87-106):**

```cpp
// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;      // live per-instance shape (shapeToWorld only)
    const Mech3DAppearanceType* mechType;       // stable type pointer
    int                         currentLOD;
    uint32_t                    slot0TexHandle; // mcTextureManager SLOT INDEX (NOT a gos handle)
                                               // for texture slot 0 (per-actor paint scheme;
                                               // localTextureHandle from Mech3DAppearance, set
                                               // by mcTextureManager->loadTexture). Resolved to
                                               // a live gos handle at flush time via
                                               // mcTextureManager->get_gosTextureHandle(slot)
                                               // per memory/mc2_texture_handle_is_live.md.
                                               // TG_TypeShape::listOfTextures is a shared type-
                                               // level cache mutated by TransformMultiShape across
                                               // all actors — do NOT read it for slot 0.
    uint32_t                    lightDataIndex; // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
};
```

**Replace with:**

```cpp
// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;      // live per-instance shape (shapeToWorld only)
    const Mech3DAppearanceType* mechType;       // stable type pointer
    int                         currentLOD;
    uint32_t                    slot0TexHandle; // mcTextureManager SLOT INDEX (NOT a gos handle)
                                               // for texture slot 0 (per-actor paint scheme;
                                               // localTextureHandle from Mech3DAppearance, set
                                               // by mcTextureManager->loadTexture). Resolved to
                                               // a live gos handle at flush time via
                                               // mcTextureManager->get_gosTextureHandle(slot)
                                               // per memory/mc2_texture_handle_is_live.md.
                                               // TG_TypeShape::listOfTextures is a shared type-
                                               // level cache mutated by TransformMultiShape across
                                               // all actors — do NOT read it for slot 0.
    uint32_t                    lightDataIndex; // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
    // M2.5: RenderObjectHandle.raw() for this actor's mech handle
    // (M2 storage). 0 = Handle::invalid() = no ObjectID write at this
    // pixel (treated identically to legacy-path fallback). CPU-side
    // carrier is UNCONDITIONAL per Q3; the consumer is GLSL-macro-gated.
    // Source: mech3d.cpp submit site reads
    //   appearance.getRenderWorldHandle().raw()  [mech3d.h:487].
    uint32_t                    objectIdRaw;
};
```

- [ ] **Step 4: STAGE -- do NOT build/commit yet**

Tasks 1-4 must commit together (struct + macro injection + SSBO fill + shader struct + shader writes form one atomic lockstep change). Proceed to Task 2 without an intervening commit. The build + smoke + commit gates fire at the end of Task 4.

---

## Task 2: Wire GLSL macro prefix injection in `gos_mech_batcher.cpp`

**Files:**

- Modify: `GameOS/gameos/gos_mech_batcher.cpp` -- add `#include`, gate prefix on `IsObjectIdBufferEnabled()`.

- [ ] **Step 1: Re-grep the existing shader load band**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "static void loadProgramsIfNeeded" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern '"mech", "shaders/mech.vert"' | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "#include" | Select-Object -First 20 LineNumber, Line
```

Expected: `loadProgramsIfNeeded` at line 218; `makeProgram` call at line 222; first 20 includes show no `RenderWorld/RenderWorld.h`. If `RenderWorld.h` already appears, STOP -- the spec recon discrepancy has been resolved by another slice and the add below is a duplicate.

- [ ] **Step 2: Add `#include "../../RenderWorld/RenderWorld.h"` (and `<string>` if absent) to `gos_mech_batcher.cpp`**

External-review fix M3: T2 Step 3 below converts the shader prefix to `std::string`. Grep first:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "#include <string>" | Select-Object LineNumber, Line
```

If this returns zero hits, add `#include <string>` to the system-includes band at the top of the TU. If it already appears, skip.

Locate the existing `#include` block at the top of `gos_mech_batcher.cpp`. Add the new RenderWorld include adjacent to other engine-side includes (the .cpp already includes `gos_mech_batcher.h`, `tgl.h`, and friends; add the RenderWorld include in the same engine block, NOT inside the system-include block).

**Existing top-of-file include band (snippet):**

```cpp
#include "gos_mech_batcher.h"
// ... (other includes) ...
```

**Replace by ADDING (immediately after `#include "gos_mech_batcher.h"`):**

```cpp
#include "gos_mech_batcher.h"
// M2.5: IsObjectIdBufferEnabled() drives the GLSL prefix that gates the
// mech.frag layout(location=2) write. Mirrors the include shipped by M1.5
// at gos_static_prop_batcher.cpp:3. GameOS/ is outside the firewall
// SCOPE_DIRS (scripts/check-include-firewall.sh:22) so this include is
// not policed by the firewall script; reviewer-discipline gate only.
// RenderWorld/RenderWorld.h is the PUBLIC header (no legacy/* reach).
#include "../../RenderWorld/RenderWorld.h"
```

If the precise location of `#include "gos_mech_batcher.h"` has drifted, place the new include immediately after the LAST engine-relative include in the top-of-file block (any `#include "../..."`).

- [ ] **Step 3: Gate the shader prefix on `IsObjectIdBufferEnabled()`**

**Existing (`GameOS/gameos/gos_mech_batcher.cpp` lines 218-233):**

```cpp
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", "#version 430\n");

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;
```

**Replace with (mirrors `gos_static_prop_batcher.cpp:510-521`):**

```cpp
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    // M2.5: GLSL preprocessor does NOT inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    // Build the prefix as a std::string and append the
    // MC2_OBJECT_ID_BUFFER macro definition when the env gate is on,
    // mirroring gos_static_prop_batcher.cpp:510-521.
    std::string mechPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        mechPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", mechPrefix.c_str());

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;
```

- [ ] **Step 4: STAGE -- do NOT build/commit yet**

Continue to Task 3.

---

## Task 3: Fill `desc.objectIdRaw` at submit site + thread `gpu_mech_id_writes` counter

**Files:**

- Modify: `mclib/mech3d.cpp` -- submit-site assignment (Q3 unconditional).
- Modify: `GameOS/gameos/gos_mech_batcher.cpp` -- per-instance fill + counter + `onMapUnload` emit.

- [ ] **Step 1: Re-grep submit-site line numbers**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\mclib\mech3d.cpp" -Pattern "GpuMechSubmitDesc desc\{\}" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\mclib\mech3d.cpp" -Pattern "submitActor\(desc\)" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\mclib\mech3d.cpp" -Pattern "desc\.fogARGB        = \(uint32_t\)hazeByte" | Select-Object LineNumber, Line
```

Expected: `GpuMechSubmitDesc desc{}` at line 2549; `submitActor(desc)` at line 2586; `desc.fogARGB` at line 2584. If any line has drifted >+/-5, locate by symbol and update the `Existing:` band below.

- [ ] **Step 2: Insert `desc.objectIdRaw` assignment immediately before `submitActor`**

**Existing (`mclib/mech3d.cpp` lines 2582-2586):**

```cpp
				const float hazeClamped = (hazeFactor < 0.0f) ? 0.0f : (hazeFactor > 1.0f ? 1.0f : hazeFactor);
				const uint8_t hazeByte  = (uint8_t)(hazeClamped * 255.0f + 0.5f);
				desc.fogARGB        = (uint32_t)hazeByte << 24;

				gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
```

**Replace with:**

```cpp
				const float hazeClamped = (hazeFactor < 0.0f) ? 0.0f : (hazeFactor > 1.0f ? 1.0f : hazeFactor);
				const uint8_t hazeByte  = (uint8_t)(hazeClamped * 255.0f + 0.5f);
				desc.fogARGB        = (uint32_t)hazeByte << 24;

				// M2.5 (Q3 unconditional): forward the M2-stored RenderWorld handle
				// to the GPU. M2 stored the handle on Mech3DAppearance::mechRenderHandle
				// via GameAdapters::Mech::registerMech. M2.5 emits the bits to
				// attachment-2 via mech.frag under MC2_OBJECT_ID_BUFFER.
				//
				// CPU fill is UNCONDITIONAL per spec Q3: env-OFF still pays the
				// load+store (< 10 ns per instance) so instance data shape stays
				// stable; the env gate lives at the GLSL macro level. Realistic
				// cost at mc2_24 (46 mechs): < 1 us per frame.
				//
				// Handle::invalid().raw() == 0 by definition, so any pre-register
				// frame or actor that missed registration writes 0 -- correctly
				// classified as "background" by RenderWorld::lookupAtPixel.
				desc.objectIdRaw    = getRenderWorldHandle().raw();

				gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
```

- [ ] **Step 3: Add per-frame + per-mission counters in `gos_mech_batcher.cpp`**

Grep for the existing per-frame counter band to anchor the insertion:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "static uint32_t s_eligibleActorsThisFrame" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "static uint32_t s_fallbacksThisFrame" | Select-Object LineNumber, Line
```

Expected: `s_eligibleActorsThisFrame` at line 187; `s_fallbacksThisFrame[5]` at line 188.

**Existing (`GameOS/gameos/gos_mech_batcher.cpp` lines 187-188):**

```cpp
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason
```

**Replace with:**

```cpp
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason

// M2.5 (Q4): always-on per-mission counter of per-instance fills whose
// objectIdRaw was non-zero. Incremented in flush()'s per-instance loop;
// emitted on the per-mission [MECHBATCHER v1] event=mech_id_summary
// line from onMapUnload(); reset in onMapLoad() and on emit.
//
// Per adversarial M1: the per-frame intermediate (s_gpuMechIdWritesThisFrame)
// was dropped -- it had NO consumer (only the per-mission counter is read
// by the emit). Direct per-mission accumulation removes three would-be
// reset sites (onMapLoad, flush early-out, flush normal end at line 1365)
// from M2.5's edit surface.
//
// Counter is ALWAYS-ON (NOT env-gated). When env-OFF the handle bits are
// still written into inst.objectIdRaw (Q3 unconditional CPU fill), so
// the counter tracks writer volume regardless of env state.
static uint64_t s_gpuMechIdWritesThisMission = 0;
```

- [ ] **Step 4: Fill `inst.objectIdRaw` + increment counter in the per-instance loop**

**Existing (`GameOS/gameos/gos_mech_batcher.cpp` lines 1093-1104):**

```cpp
        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            instDst[instHead++]     = inst;
        }
```

**Replace with:**

```cpp
        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            // M2.5 (Q3 unconditional): carry the RenderWorld handle through
            // to the SSBO. Env-OFF: GLSL macro gates out the FS write, so
            // the value is never read by the GPU.
            inst.objectIdRaw        = d.objectIdRaw;
            // M2.5 (Q4): count non-zero writes for per-mission observability.
            // Direct per-mission accumulation (no per-frame intermediate;
            // per adversarial M1 the per-frame counter had no consumer).
            if (d.objectIdRaw != 0u) {
                ++s_gpuMechIdWritesThisMission;
            }
            instDst[instHead++]     = inst;
        }
```

- [ ] **Step 5: NO-OP (per adversarial M1: per-frame counter dropped)**

Originally this step added a per-frame counter reset alongside `s_eligibleActorsThisFrame = 0;` at every reset site. Adversarial review M1 identified THREE such sites (not two as the original prose claimed) -- and that the per-frame counter had no consumer.

Grep-confirmed reset sites in `gos_mech_batcher.cpp` (for the record; not edited by M2.5):
- line 348: `onMapLoad()` per-mission baseline (NOT `endFrame()` as plan originally said)
- line 992: `flush()` early-out path (gates: `!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed || s_pendingSubmits.empty()`)
- line 1365: `flush()` normal end-of-flush reset

**Resolution:** the per-frame counter is dropped. The per-mission counter (`s_gpuMechIdWritesThisMission`) is incremented directly in T3 Step 4 and reset in T3 Step 6 (`onMapLoad` zeroes it; `onMapUnload` emits + zeroes it). No edits are made to any of the three `s_eligibleActorsThisFrame = 0;` sites. Skip to Step 6.

- [ ] **Step 6: Reset the per-mission counter in `onMapLoad()` and emit on `onMapUnload()`**

Grep for the two map-lifecycle hooks:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "void GpuMechBatcher::onMapLoad" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp" -Pattern "void GpuMechBatcher::onMapUnload" | Select-Object LineNumber, Line
```

Locate each function definition. At the TOP of `onMapLoad()` (after any existing per-mission resets), add:

```cpp
    // M2.5 (Q4): per-mission writer counter; emitted on onMapUnload.
    s_gpuMechIdWritesThisMission = 0;
```

At the TOP of `onMapUnload()` (BEFORE any teardown that might depend on the counter being already-cleared), add:

```cpp
    // M2.5 (Q4 + Q6 amendment 2): always-on per-mission writer summary.
    // Surfaces gpu_mech_id_writes to the M2.6 readiness decision rule.
    // Always-on (NOT env-gated): M2.6 needs this signal regardless of
    // MC2_OBJECT_ID_BUFFER state to size the MLR-vs-GPU split.
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=%llu\n",
        (unsigned long long)s_gpuMechIdWritesThisMission);
    s_gpuMechIdWritesThisMission = 0;
```

If `onMapLoad` / `onMapUnload` do not exist with those exact names (they should -- they are declared at `gos_mech_batcher.h:125-126`), STOP and report; do not invent alternative hooks.

- [ ] **Step 7: STAGE -- do NOT build/commit yet**

Continue to Task 4 (shader + commit).

---

## Task 4: Shader-side struct grow + body writes + atomic commit (Tasks 1+2+3+4)

**Files:**

- Modify: `shaders/mech.vert` -- GLSL struct grow + gated forward varying + gated body write.
- Modify: `shaders/mech.frag` -- gated `flat in` + `layout(location=2) out uint` + gated body write.

- [ ] **Step 1: Grow GLSL `GpuMechInstance` struct in lockstep**

**Existing (`shaders/mech.vert` lines 30-40):**

```glsl
// Per-instance SSBO (binding 0).
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};
```

**Replace with:**

```glsl
// Per-instance SSBO (binding 0).
// M2.5: 64 bytes (was 48). MUST mirror C++ GpuMechInstance at
// GameOS/gameos/gos_mech_batcher.h:35. Trailing _padN slots keep the
// std430 layout explicit so M3+ field adds (terrain chunk, VFX) take
// named slots rather than silently consuming pad bytes.
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
    uint  objectIdRaw;
    uint  _pad1;
    uint  _pad2;
    uint  _pad3;
};
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};
```

- [ ] **Step 2: Add gated `flat out uint v_objectIdRaw` after existing `out` declarations**

Grep to anchor the insertion point (file may drift):

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\shaders\mech.vert" -Pattern "out vec3 v_normal" | Select-Object LineNumber, Line
```

Expected: `out vec3 v_normal;` at line 76.

**Existing (`shaders/mech.vert` lines 76-77):**

```glsl
out vec3 v_normal;   // world-space for GBuffer1

void main() {
```

**Replace with:**

```glsl
out vec3 v_normal;   // world-space for GBuffer1
// M2.5: forward per-instance ObjectID to FS for the
// layout(location=2) out uint emission under
// #ifdef MC2_OBJECT_ID_BUFFER. `flat` qualifier MANDATORY: GL spec
// FORBIDS linear interpolation of integer varyings; without `flat`
// the program fails to link and [MECHBATCHER v1] event=shader_fail
// fires -> GPU mech path goes silently dormant.
#ifdef MC2_OBJECT_ID_BUFFER
flat out uint v_objectIdRaw;
#endif

void main() {
```

- [ ] **Step 3: Add gated body write at end of `main()` in `mech.vert`**

Grep to anchor the end-of-main band:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\shaders\mech.vert" -Pattern "v_normal         = worldNormal;" | Select-Object LineNumber, Line
```

Expected: line 173 (immediately before closing `}` at line 174).

**Existing (`shaders/mech.vert` lines 172-174):**

```glsl
    v_fogRGB         = vec4(g_scene.fogColor.rgb, inst.fogRGB.a);
    v_normal         = worldNormal;
}
```

**Replace with:**

```glsl
    v_fogRGB         = vec4(g_scene.fogColor.rgb, inst.fogRGB.a);
    v_normal         = worldNormal;
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: forward per-instance ObjectID through the existing SSBO read;
    // no extra memory traffic. Driver handles `flat` carry-through as one
    // register write per provoking vertex.
    v_objectIdRaw    = inst.objectIdRaw;
#endif
}
```

- [ ] **Step 4: Add gated `flat in` + `layout(location=2) out` to `mech.frag`**

**Existing (`shaders/mech.frag` lines 36-37):**

```glsl
layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
```

**Replace with:**

```glsl
layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M2.5: per-pixel mech ObjectID. Emitted to GL_COLOR_ATTACHMENT2
// (R32_UINT; M1.5 substrate). `flat in` matches mech.vert's
// `flat out uint v_objectIdRaw`. Alpha-tested fragments that
// discard() at line 56 skip this write naturally -- the attachment-2
// pixel retains the clear value (0 = Handle::invalid()), correctly
// classified as background under lookupAtPixel.
flat in uint v_objectIdRaw;
layout(location=2) out uint v_objectId;
#endif
```

- [ ] **Step 5: Add gated body write at end of `main()` in `mech.frag`**

**Existing (`shaders/mech.frag` lines 74-77):**

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
```

**Replace with:**

```glsl
    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-pixel RenderObjectHandle.raw(). Debug-mode pixels
    // (u_debugMode 1..9 at lines 65-73) DO NOT discard; they still emit
    // a valid handle, which is the correct substrate behavior (a
    // lookupAtPixel on a debug-color pixel returns the actor's handle).
    v_objectId = v_objectIdRaw;
#endif
}
```

- [ ] **Step 6: Render-contract comment refresh (defense-in-depth)**

The `[RENDER_CONTRACT]` block at `mech.frag:11-15` documents the pass's outputs. Update to include GBuffer2 under the macro gate:

**Existing (`shaders/mech.frag` lines 11-15):**

```glsl
// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true
```

**Replace with:**

```glsl
// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   GBuffer2:       rc_gbuffer2_objectIdU32  // M2.5 (#ifdef MC2_OBJECT_ID_BUFFER)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true
```

- [ ] **Step 7: FULL-RELINK build (struct layout change)**

Per CLAUDE.md "Full relink before deploy when load-bearing functions change" + spec §11 "Hot-reload-without-relink trap": deleting `mc2.exe` is insufficient when struct layout changes. Delete CMakeFiles to force a clean recompile of every TU that includes `gos_mech_batcher.h` (compute the right .obj files matters less than guaranteeing the recompile).

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\CMakeFiles -Recurse -Force -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 20
```

Expected: build succeeds. `static_assert(sizeof(GpuMechInstance) == 64)` passes. All offset asserts pass. No linker errors. If the build fails on `RenderWorld::IsObjectIdBufferEnabled` not declared, verify the Task 2 Step 2 include landed in `gos_mech_batcher.cpp`.

- [ ] **Step 8: Tier1 smoke env-OFF (zero pixel delta gate)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS. Banner shows M2 fields unchanged. `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` appears at end of each mission (N may be >0 because Q3 CPU fill is unconditional; this is correct). NO `[MECHBATCHER v1] event=shader_fail` lines. NO pixel-output regression (the GLSL macro is undefined env-OFF; the new `out uint v_objectId` declaration is absent from the linked program).

If `event=shader_fail` appears: the most likely cause is a missing `flat` qualifier on the integer varying, which would only fail under env-ON anyway -- env-OFF failure indicates a typo in the unconditional GLSL struct change. Re-verify Task 4 Step 1.

- [ ] **Step 9: Tier1 smoke env-ON (substrate active)**

```powershell
$env:MC2_OBJECT_ID_BUFFER = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_OBJECT_ID_BUFFER = $null
```

Expected: tier1 5/5 PASS. `[MECHBATCHER v1] event=shader_ok prog=N` appears (mandatory; absence = silent shader_fail trap). `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` shows N > 0 on missions with mechs. No frame-time regression beyond M1.5 baseline.

If env-ON shows `event=shader_fail` while env-OFF shows `event=shader_ok`: the `flat` qualifier is missing on either the vert `out` or the frag `in`. Re-verify Task 4 Steps 2 and 4.

- [ ] **Step 9.5: Shader-output uniqueness allowlist gate (external-review fix M2)**

M1.5's gate expected ONLY `static_prop.frag` to declare `layout(location=2) out`. M2.5 intentionally ADDS `mech.frag` -- the allowlist must now show exactly two files:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\*.frag" `
  -Pattern "layout\s*\(\s*location\s*=\s*2\s*\)\s*out"
```

Expected matches: EXACTLY two files:
- `shaders/static_prop.frag`
- `shaders/mech.frag`

If any third file appears, STOP and report. No other shader is expected to write attachment 2 in M2.5.

Companion `flat` qualifier check (the `flat` qualifier is MANDATORY on integer varyings; without it the program fails to link silently):

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\mech.vert","A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\mech.frag" `
  -Pattern "flat .*uint .*objectId"
```

Expected: one `flat out` in `mech.vert`, one `flat in` in `mech.frag`. (Both inside `#ifdef MC2_OBJECT_ID_BUFFER` blocks.)

- [ ] **Step 10: COMMIT (atomic lockstep: Tasks 1+2+3+4)**

Stage all the artifacts touched by Tasks 1-4 together:

```powershell
git add A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.h A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp A:\Games\mc2-opengl-src\shaders\mech.vert A:\Games\mc2-opengl-src\shaders\mech.frag A:\Games\mc2-opengl-src\mclib\mech3d.cpp
```

Then commit:

```bash
git commit -m "$(cat <<'EOF'
feat(renderworld): M2.5 mech ObjectID substrate -- per-instance SSBO field + frag write

Slice M2.5 (lockstep T1+T2+T3+T4): emits Mech3DAppearance::mechRenderHandle.raw()
to GL_COLOR_ATTACHMENT2 via mech.frag under #ifdef MC2_OBJECT_ID_BUFFER.
Closes the M2 chain ("M2 stored the handle; M2.5 emits it to the GPU").

Struct grow (atomic lockstep):
- GpuMechInstance: 48B -> 64B (uint32_t objectIdRaw + 3 generic _padN slots).
  static_assert chain updated for size + new offset.
- GpuMechSubmitDesc: gains uint32_t objectIdRaw carrier.
- shaders/mech.vert: GLSL GpuMechInstance struct mirrors C++ exactly.

GLSL prefix injection mirrors gos_static_prop_batcher.cpp:510-521.
gos_mech_batcher.cpp gains one include (RenderWorld/RenderWorld.h) for
IsObjectIdBufferEnabled() -- unpoliced per Q5 (GameOS/ outside firewall
SCOPE_DIRS at scripts/check-include-firewall.sh:22).

CPU fill unconditional per Q3: desc.objectIdRaw = getRenderWorldHandle().raw()
fires regardless of env. Realistic cost < 1 us per frame at mc2_24 (46 mechs).

flat-qualified integer varying mandatory: GL spec forbids linear interp on
integer varyings; missing flat -> event=shader_fail -> GPU mech path dormant.

Observability (Q4 always-on): new [MECHBATCHER v1] event=mech_id_summary
gpu_mech_id_writes=N line emitted from GpuMechBatcher::onMapUnload() per
mission. Always-on (NOT env-gated) so the M2.6 readiness decision has live
data regardless of MC2_OBJECT_ID_BUFFER state. Companion mlr_mech_draws
counter lands in T5.

Lockstep edit risk (spec §11): struct + shader land in ONE commit because
hot-reloading shader after C++ struct grow without full relink reads wrong
fields (symptom: wrong-mech meshes). CMakeFiles deleted before build to
force full recompile of every TU including gos_mech_batcher.h.

Tier1 5/5 PASS env-OFF (zero pixel delta) and env-ON (event=shader_ok,
gpu_mech_id_writes > 0 on mech missions). No frame-time regression.

Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md
Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-plan.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: MLR-fallback `mlr_mech_draws` counter (Q6 amendment 2)

**Files:**

- Modify: `mclib/mech3d.cpp` -- counter + emit at MLR fallback site (line 2608) + mission lifecycle hook.

The MLR draw site lives in `mclib/mech3d.cpp` at line 2608 (`mechShape->Render(true)`). The counter is always-on (NOT env-gated) and surfaces per-mission via a split `[MECHBATCHER v1] event=mlr_mech_summary` log line. **Per external-review fix M1: split-line is the OFFICIAL and only shape.** The two counters live in different TUs (MLR draws in `mclib/mech3d.cpp`; GPU writes in `GameOS/gameos/gos_mech_batcher.cpp`), so each emits its own stderr line. Spec Q6 amendment 2 sanctions split-line for exactly this case ("split across two log lines if MLR draws live in a different TU"). Do not collapse to a single banner line in M2.5.

- [ ] **Step 1: Re-grep MLR fallback site**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\mclib\mech3d.cpp" -Pattern "mechShape->Render\(true\)" | Select-Object LineNumber, Line
```

Expected: line 2608 (within the `if (!gpuMechSubmitted && !mechGpuCullSkip)` block at 2607-2609).

- [ ] **Step 2: Add file-scope counter to `mclib/mech3d.cpp`**

Locate the top of `mclib/mech3d.cpp` after the existing `#include` block (the file's namespace structure is C-style; find the first `static` or file-scope global definition and insert the counter adjacent).

Add (in the file-scope global band near the top of the .cpp -- pick a spot adjacent to any existing `static` global; if uncertain, place it immediately after the last `#include` directive):

```cpp
// M2.5 (Q6 amendment 2): always-on MLR mech draw counter. Incremented
// at the legacy mechShape->Render(true) fallback site (~line 2608) so
// the M2.6 readiness decision rule has live data on Path-B incidence.
// NOT env-gated: M2.6 must consult this number regardless of
// MC2_OBJECT_ID_BUFFER state. Emitted per-mission via
// Mech3DAppearance::endMission() hook.
static uint64_t s_mlrMechDrawsThisMission = 0;
```

If `Mech3DAppearance::endMission()` does not exist as a class hook, place the emit in the existing mission-end seam: grep for an existing mission-teardown function in `mech3d.cpp`:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\mclib\mech3d.cpp" -Pattern "Mech3DAppearance::destroy|endMission" | Select-Object -First 10 LineNumber, Line
```

If no mission-scoped hook exists in `mech3d.cpp`, emit on `Mech3DAppearance::destroy()` (per-actor teardown) by deferring the emit to a separately gated `s_emitOnNextDestroy` flag set from `GameAdapters::Mech::endMission()`. **Fallback:** emit instead from the existing `GpuMechBatcher::onMapUnload()` by reading the counter through an `extern uint64_t` declaration. Implementation choice deferred to executor based on grep result; both shapes satisfy "per-mission".

- [ ] **Step 3: Increment counter at the MLR fallback site**

**Existing (`mclib/mech3d.cpp` lines 2603-2609):**

```cpp
			// Slice C1: if GPU mech cull says invisible, skip BOTH the GPU
			// submit AND the CPU fallback. This is the whole point of the
			// cull — render nothing for this actor this frame. CPU update
			// (AI, position, animation, damage) has already run.
			if (!gpuMechSubmitted && !mechGpuCullSkip) {
				mechShape->Render(true);  // CPU path — unchanged
			}
```

**Replace with:**

```cpp
			// Slice C1: if GPU mech cull says invisible, skip BOTH the GPU
			// submit AND the CPU fallback. This is the whole point of the
			// cull — render nothing for this actor this frame. CPU update
			// (AI, position, animation, damage) has already run.
			if (!gpuMechSubmitted && !mechGpuCullSkip) {
				// M2.5 (Q6 amendment 2): count MLR/CPU-fallback draws so
				// the always-on per-mission mlr_mech_summary line reflects
				// Path-B incidence. M2.6 readiness decision consults this
				// value (spec §12 Q6 amendment 3).
				++s_mlrMechDrawsThisMission;
				mechShape->Render(true);  // CPU path — unchanged
			}
```

- [ ] **Step 4: Emit per-mission summary (extern "C" declaration at FILE SCOPE)**

The chosen emit site is `Mech3DAppearance::destroy()` is per-actor and wrong scope; the right scope is "per mission load/unload." `GpuMechBatcher::onMapUnload()` already fires once per mission. Cross-TU coupling is acceptable here because the counter is for observability only.

**External-review fix C1 (CRITICAL):** the `extern "C"` declaration MUST live at file scope, not inside a function body. Language-linkage declarations inside function bodies are non-portable. The DEFINITION (in `mclib/mech3d.cpp`) stays at file scope; the DECLARATION (in `gos_mech_batcher.cpp`) also lives at file scope, near the other top-of-file includes / forward decls.

**Step 4a: definition at file scope in `mclib/mech3d.cpp`** (adjacent to the counter added in Step 2):

```cpp
// M2.5: getter for the per-mission MLR draw count, callable from
// GpuMechBatcher::onMapUnload() in a different TU. Declaration in
// gos_mech_batcher.cpp is at file scope per external-review C1.
extern "C" uint64_t consumeAndResetMlrMechDraws() {
    const uint64_t v = s_mlrMechDrawsThisMission;
    s_mlrMechDrawsThisMission = 0;
    return v;
}
```

**Step 4b: file-scope declaration in `GameOS/gameos/gos_mech_batcher.cpp`** (place in the engine-include / forward-decl band near the top of the TU, after the includes; this is the SAME TU that already has the new `RenderWorld/RenderWorld.h` include from T2 Step 2):

```cpp
// M2.5 (external-review C1): file-scope forward declaration of the
// MLR-side per-mission counter getter, defined in mclib/mech3d.cpp.
// Language-linkage declarations must be at file scope -- not inside a
// function body. Avoids a new header.
extern "C" uint64_t consumeAndResetMlrMechDraws();
```

If `gos_mech_batcher.cpp` does not already pull `<cstdint>` (grep-verified at plan-write time: `uint64_t` already used 8 times in this TU, transitively visible), no new include is required here. If a future build error appears citing `uint64_t` as undeclared, add `#include <cstdint>` to this TU as well.

For `mclib/mech3d.cpp` (counter DEFINITION TU): `uint64_t` is NOT currently used in that file (grep-verified). Add `#include <cstdint>` to the system-includes band at the top of `mclib/mech3d.cpp` BEFORE adding the counter or getter:

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\mclib\mech3d.cpp" -Pattern "#include <cstdint>" | Select-Object LineNumber, Line
```

If zero hits, add `#include <cstdint>` adjacent to other system includes (`<stdio.h>` / `<stdint.h>` cluster, whichever shape the file uses).

**Step 4c: body-only call in `GpuMechBatcher::onMapUnload()`** (immediately before the M2.5 mech_id_summary emit added in Task 3 Step 6):

```cpp
    // M2.5 (Q6 amendment 2): consume the MLR-side per-mission counter
    // and emit on its own [MECHBATCHER v1] event=mlr_mech_summary line.
    // The split-line shape is OFFICIAL (external-review M1): the two
    // counters live in different TUs and MUST emit on two adjacent
    // lines (mlr first, then gpu_mech_id). Do not collapse to one line.
    // consumeAndResetMlrMechDraws is forward-declared at file scope
    // (external-review C1) -- declaration is NOT inside this function body.
    const uint64_t mlrDraws = consumeAndResetMlrMechDraws();
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=%llu\n",
        (unsigned long long)mlrDraws);
```

Place this BEFORE the `event=mech_id_summary` emit added in Task 3 Step 6, so the two lines appear adjacent in the log (mlr first, then gpu_mech_id).

- [ ] **Step 5: FULL-RELINK build**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: build succeeds. Linker resolves the cross-TU `consumeAndResetMlrMechDraws` reference.

- [ ] **Step 6: Tier1 smoke env-OFF + env-ON**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_OBJECT_ID_BUFFER = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_OBJECT_ID_BUFFER = $null
```

Expected for both runs: tier1 5/5 PASS. Each mission's stderr shows BOTH lines at teardown:

```
[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M
[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N
```

Record the `M` value (per Q6 amendment 3, the M2.6 readiness gate depends on whether `M > 0` on any tier1 mission).

- [ ] **Step 7: Commit**

```powershell
git add A:\Games\mc2-opengl-src\mclib\mech3d.cpp A:\Games\mc2-opengl-src\GameOS\gameos\gos_mech_batcher.cpp
```

```bash
git commit -m "$(cat <<'EOF'
feat(renderworld): M2.5 T5 -- mlr_mech_draws counter (Q6 amendment 2)

Always-on per-mission counter at the MLR/CPU-fallback site
(mclib/mech3d.cpp:2608 mechShape->Render(true)). Emitted on its own
[MECHBATCHER v1] event=mlr_mech_summary line (split-line shape is OFFICIAL
per external-review M1 -- the two counters live in different TUs and emit
on adjacent lines, mlr first then gpu_mech_id). Cross-TU coupling via
extern "C" getter consumeAndResetMlrMechDraws(): DEFINITION at file scope
in mclib/mech3d.cpp; DECLARATION at file scope in gos_mech_batcher.cpp
(external-review C1 -- language-linkage decls inside function bodies are
non-portable; only the call lives in onMapUnload()).
mclib/mech3d.cpp gains #include <cstdint> (uint64_t not previously used).

NOT env-gated: the M2.6 readiness decision rule (spec §12 Q6 amendment 3)
consults this value regardless of MC2_OBJECT_ID_BUFFER state. If tier1
ever shows mlr_mech_draws > 0, M2.6 must preserve mover-first legacy
fallback for those mechs and cannot claim full mech GPU-pick coverage.

Tier1 5/5 PASS env-OFF and env-ON. Both lines emit per-mission:
  [MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M
  [MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N

Spec Q6 amendment 2.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `[MECH_OBJECT_ID_SELFTEST v1]` self-test (Q1)

**Files:**

- Modify: `RenderWorld/RenderWorld.cpp` -- add `RunMechObjectIdSelfTest()` in anonymous namespace; wire into `init()` after `RunGameplayPickSelfTest()`.

Per Q1: separate `[MECH_OBJECT_ID_SELFTEST v1]` canary, NOT an extension of the M1.5 `[OBJECT_ID_SELFTEST v1]`. Mirror the shape of the existing `runSubstrateSelfTest()` at `RenderWorld.cpp:250` -- synthetic record manipulation, no GL state required. The self-test exercises the M2 mech-handle path: `registerMech` allocates a handle, `s_objectRecords[h.index()].kind == Mech`, `destroyMech` bumps generation, post-destroy `lookupAtPixel`-style lookup returns isValid=false.

- [ ] **Step 1: Re-grep self-test wiring band**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\RenderWorld\RenderWorld.cpp" -Pattern "RunGameplayPickSelfTest" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\RenderWorld\RenderWorld.cpp" -Pattern "void runSubstrateSelfTest" | Select-Object LineNumber, Line
Select-String -Path "A:\Games\mc2-opengl-src\RenderWorld\RenderWorld.cpp" -Pattern "void RunGameplayPickSelfTest" | Select-Object LineNumber, Line
```

Expected:
- `void RunGameplayPickSelfTest();` forward decl at line 40.
- `void runSubstrateSelfTest()` def at line 250.
- `RunGameplayPickSelfTest();` call inside `init()` at line 365.

- [ ] **Step 2: Add forward declaration alongside `RunGameplayPickSelfTest`**

**Existing (`RenderWorld/RenderWorld.cpp` line 40 area):**

```cpp
void RunGameplayPickSelfTest();
```

**Replace with:**

```cpp
void RunGameplayPickSelfTest();
// M2.5 (Q1): separate mech-substrate self-test. Mirrors the M2-pre
// precedent ([GAMEPLAY_PICK_SELFTEST v1] separate from
// [RENDER_WORLD_SELFTEST v1]). Validates that registerMech allocates
// a Mech-kind handle and destroyMech bumps the generation correctly.
void RunMechObjectIdSelfTest();
```

- [ ] **Step 3: Implement `RunMechObjectIdSelfTest()` in the anonymous namespace**

Locate the anonymous namespace closing brace (line 336 `} // namespace`) and add the new function BEFORE that brace. Place it after `runSubstrateSelfTest()` so the related self-tests cluster:

Add (immediately after the `runSubstrateSelfTest()` body ends and BEFORE `} // namespace` at line 336):

```cpp
// M2.5 (Q1): mech-substrate self-test. Synthetic-only -- no GL state,
// no real readback. Exercises the M2 registerMech / destroyMech /
// s_objectRecords path that M2.5 GPU writes feed. Gated by
// MC2_MECH_OBJECT_ID_SELFTEST=1; runs once at RenderWorld::init() after
// RunGameplayPickSelfTest().
//
// Why synthetic: real GPU-readback validation requires a live mission
// frame + a known mech-on-cursor pixel; that path is exercised
// per-frame by RunGameplayPickSelfTest's mech-side extension in M2.6.
// M2.5's job is to prove the substrate (handle allocation, record kind,
// generation roundtrip) is intact -- a pure-CPU self-test suffices.
//
// Result lines:
//   [MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=N handle=0xNN
//   [MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=N reason=<...>
//   [MECH_OBJECT_ID_SELFTEST v1] result=SKIPPED reason=env_disabled
//
// FAIL is a STOP: the mech substrate is broken; M2.6 pickup will be
// unreliable.
void RunMechObjectIdSelfTest() {
    if (!envFlag("MC2_MECH_OBJECT_ID_SELFTEST")) {
        std::fprintf(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=SKIPPED reason=env_disabled\n");
        return;
    }

    // Step 1: register a synthetic mech, validate handle + record.
    RenderWorld::RenderMechDesc desc;
    desc.mechTypeId   = 0u;
    desc.gameObjectId = 0xC0FFEEu;
    desc.debugCookie  = 0u;
    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);
    if (!h.isValid()) {
        std::fprintf(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=1 reason=registerMech_returned_invalid\n");
        return;
    }
    const uint32_t idx = h.index();
    const uint16_t gen = h.generation();

    // Step 2: validate record kind=Mech and alive=true.
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (idx >= s_objectRecords.size()) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=record_index_out_of_range_%u\n",
                (unsigned)idx);
            return;
        }
        const auto& rec = s_objectRecords[idx];
        if (rec.kind != RenderWorld::RenderObjectKind::Mech) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=wrong_kind_%u\n",
                (unsigned)rec.kind);
            RenderWorld::destroyMech(h);
            return;
        }
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) == 0u) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=alive_not_set_after_register\n");
            RenderWorld::destroyMech(h);
            return;
        }
        if (rec.generation != gen) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=generation_mismatch_record_%u_handle_%u\n",
                (unsigned)rec.generation, (unsigned)gen);
            RenderWorld::destroyMech(h);
            return;
        }
        if (rec.gameObjectId != 0xC0FFEEu) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=gameObjectId_lost\n");
            RenderWorld::destroyMech(h);
            return;
        }
    }

    // Step 3: handle round-trip. handle.raw() carries idx+gen; rebuilding
    // via the canonical production constructor MUST recover the same raw bits.
    // Per adversarial m3: RenderObjectHandle::make(idx, gen) is the
    // production path (RenderCore/Handle.h:32); use it unconditionally rather
    // than touching the `bits` field directly.
    const uint32_t raw = h.raw();
    RenderCore::RenderObjectHandle h2 = RenderCore::RenderObjectHandle::make(idx, gen);
    if (h2.raw() != raw) {
        std::fprintf(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=3 reason=roundtrip_raw_%08X_vs_%08X\n",
            (unsigned)h2.raw(), (unsigned)raw);
        RenderWorld::destroyMech(h);
        return;
    }

    // Step 4: destroyMech bumps generation and clears alive.
    RenderWorld::destroyMech(h);
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        const auto& rec = s_objectRecords[idx];
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) != 0u) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=4 reason=alive_set_after_destroy\n");
            return;
        }
        if (rec.generation != static_cast<uint16_t>(gen + 1u)) {
            std::fprintf(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=4 reason=generation_not_bumped_%u\n",
                (unsigned)rec.generation);
            return;
        }
    }

    std::fprintf(stderr,
        "[MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=%u handle=0x%08X\n",
        (unsigned)gen, (unsigned)raw);
}
```

- [ ] **Step 4: Wire `RunMechObjectIdSelfTest()` into `RenderWorld::init()` after `RunGameplayPickSelfTest()`**

**Existing (`RenderWorld/RenderWorld.cpp` lines 360-366):**

```cpp
    // M1.5 T10: substrate self-test (gated by MC2_RENDER_WORLD_SELFTEST=1).
    runSubstrateSelfTest();
    // M2-pre: gameplay-pick self-test (gated by MC2_GAMEPLAY_PICK_SELFTEST=1
    // + MC2_OBJECT_ID_BUFFER=1). Validates the extracted spine has not
    // diverged from M1.6 gate semantics. Mirrors substrate self-test shape.
    RunGameplayPickSelfTest();
}
```

**Replace with:**

```cpp
    // M1.5 T10: substrate self-test (gated by MC2_RENDER_WORLD_SELFTEST=1).
    runSubstrateSelfTest();
    // M2-pre: gameplay-pick self-test (gated by MC2_GAMEPLAY_PICK_SELFTEST=1
    // + MC2_OBJECT_ID_BUFFER=1). Validates the extracted spine has not
    // diverged from M1.6 gate semantics. Mirrors substrate self-test shape.
    RunGameplayPickSelfTest();
    // M2.5 (Q1): mech-substrate self-test (gated by
    // MC2_MECH_OBJECT_ID_SELFTEST=1). Validates registerMech / destroyMech
    // / record-table generation + kind plumbing. Synthetic; no GL state.
    RunMechObjectIdSelfTest();
}
```

- [ ] **Step 5: Build**

```powershell
Remove-Item A:\Games\mc2-opengl-src\build64\RelWithDebInfo\mc2.exe -ErrorAction SilentlyContinue
cmake --build A:\Games\mc2-opengl-src\build64 --config RelWithDebInfo --target mc2 -- /m 2>&1 | Select-Object -Last 15
```

Expected: build succeeds. Self-test uses `RenderObjectHandle::make(idx, gen)` (production constructor at `RenderCore/Handle.h:32`) unconditionally.

- [ ] **Step 6: Tier1 smoke env-ON with self-test gate**

```powershell
$env:MC2_OBJECT_ID_BUFFER = "1"
$env:MC2_MECH_OBJECT_ID_SELFTEST = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_MECH_OBJECT_ID_SELFTEST = $null
$env:MC2_OBJECT_ID_BUFFER = $null
```

Expected: tier1 5/5 PASS. Each mission shows ONE `[MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=1 handle=0x...` line at startup (the self-test runs once per `RenderWorld::init()`; since `init()` fires per mission load, the line appears 5 times across the tier1 run).

- [ ] **Step 7: Verify self-test FAIL detection (negative test)**

This step is OPTIONAL but recommended on first-execution; skip on re-runs. Temporarily edit the self-test to inject a deliberate failure (e.g. change `if (rec.kind != Mech)` to `if (rec.kind == Mech)`), rebuild, run env-ON, verify a `result=FAIL` line appears, then revert the edit.

- [ ] **Step 8: Commit**

```powershell
git add A:\Games\mc2-opengl-src\RenderWorld\RenderWorld.cpp
```

```bash
git commit -m "$(cat <<'EOF'
feat(renderworld): M2.5 T6 -- [MECH_OBJECT_ID_SELFTEST v1] synthetic self-test (Q1)

Adds RunMechObjectIdSelfTest() in RenderWorld.cpp, gated by
MC2_MECH_OBJECT_ID_SELFTEST=1, wired into RenderWorld::init() after
RunGameplayPickSelfTest(). Per Q1: SEPARATE canary (not an extension
of [OBJECT_ID_SELFTEST v1]), mirroring M2-pre's [GAMEPLAY_PICK_SELFTEST v1]
precedent. Different producer surfaces (M1.5 coalesce/legacy vs M2.5
GpuMechBatcher SSBO) deserve separate failure signals.

Self-test is synthetic (CPU-only, no GL state): exercises
registerMech -> record kind/alive/generation/gameObjectId validation
-> handle.raw() round-trip -> destroyMech generation bump. Real
GPU-readback validation lives in M2.6 (mech pickup wiring).

Log lines:
  [MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=N handle=0xNN
  [MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=N reason=<...>
  [MECH_OBJECT_ID_SELFTEST v1] result=SKIPPED reason=env_disabled

Tier1 5/5 PASS env-ON with MC2_MECH_OBJECT_ID_SELFTEST=1: result=PASS
emitted on each mission load.

Spec Q1.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Validation gates + CLAUDE.md update + commit docs

**Files:**

- No source changes; gate verification only.
- Modify: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md` -- Active campaigns + Known issues.

### Gate 1: Tier1 5/5 PASS env-OFF (zero pixel delta vs M2 HEAD)

- [ ] **Step 1: Run tier1 env-OFF**

```powershell
$env:MC2_OBJECT_ID_BUFFER = $null
$env:MC2_MECH_OBJECT_ID_SELFTEST = $null
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: tier1 5/5 PASS, exit 0. Pixel output identical to M2 HEAD (the commit before Task 4's atomic commit). Per spec §7 + §8: additive `objectIdRaw` field is zero-defaulted CPU-side; GLSL macro-gate ensures no shader-output addition. Frame-time unchanged.

The new `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` and `event=mlr_mech_summary mlr_mech_draws=M` lines appear at each mission unload (always-on per Q4 + Q6 amendment 2). `[MECH_OBJECT_ID_SELFTEST v1]` is SKIPPED env-disabled.

If gate fails: inspect `tests/smoke/artifacts/<latest>/<mission>.ring_trace.log`. Most likely regression cause is the unconditional CPU fill at submit site (Task 3 Step 2) -- verify `desc.objectIdRaw` is only ASSIGNED, never READ outside the SSBO fill loop (Q3 says the env gate lives at the GLSL macro, not at the CPU read).

### Gate 2: Tier1 5/5 PASS env-ON `MC2_OBJECT_ID_BUFFER=1` (substrate active)

- [ ] **Step 2: Run tier1 env-ON**

```powershell
$env:MC2_OBJECT_ID_BUFFER = "1"
$env:MC2_MECH_OBJECT_ID_SELFTEST = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_OBJECT_ID_BUFFER = $null
$env:MC2_MECH_OBJECT_ID_SELFTEST = $null
```

Expected: tier1 5/5 PASS, exit 0. Frame-time delta <= 0.5ms p99 vs env-OFF baseline (M1.5 budget; mech contribution should be << 1 us per spec §7).

- [ ] **Step 3: Verify substrate-active artifacts**

```powershell
$latestArtifactDir = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\ |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
Write-Host "Artifact dir: $latestArtifactDir"

# Mandatory: event=shader_ok (absence = silent shader_fail trap)
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "\[MECHBATCHER v1\] event=shader_ok" | Select-Object -First 5

# Mandatory: gpu_mech_id_writes > 0 on mech missions
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "event=mech_id_summary gpu_mech_id_writes=\d+" | Select-Object -First 10

# Mandatory: mlr_mech_summary line exists
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "event=mlr_mech_summary mlr_mech_draws=\d+" | Select-Object -First 10

# Mandatory: self-test PASS
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "\[MECH_OBJECT_ID_SELFTEST v1\] result=PASS" | Select-Object -First 10

# Forbidden: shader_fail
Select-String -Path "$latestArtifactDir\*.ring_trace.log" -Pattern "\[MECHBATCHER v1\] event=shader_fail" | Select-Object -First 5
```

Expected:
- `event=shader_ok` line(s) present.
- `event=mech_id_summary gpu_mech_id_writes=N` where N > 0 on at least one mission.
- `event=mlr_mech_summary mlr_mech_draws=M` line(s) present.
- `[MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 ...` present.
- `event=shader_fail` count = 0.

If `event=shader_fail` appears env-ON only: the `flat` qualifier is missing on the integer varying. Re-verify Task 4 Steps 2 + 4. If `gpu_mech_id_writes == 0` on every mission: the M2 mech adapter did not register handles before submit, OR Task 3 Step 2's assignment did not land. Cross-check `mech3d.cpp:2585` for the new `desc.objectIdRaw = getRenderWorldHandle().raw();` line.

### Gate 3: `mc2_03` canary -- writer count + MLR fallback magnitude

- [ ] **Step 4: Record mc2_03 counter values**

```powershell
$latestArtifactDir = Get-ChildItem A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\ |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName

$gpuWrites = Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "gpu_mech_id_writes=(\d+)" |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
$mlrDraws = Select-String -Path "$latestArtifactDir\mc2_03.ring_trace.log" -Pattern "mlr_mech_draws=(\d+)" |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
Write-Host "mc2_03 gpu_mech_id_writes: $gpuWrites"
Write-Host "mc2_03 mlr_mech_draws:     $mlrDraws"

if ([int]$gpuWrites -le 0) {
    Write-Host "GATE 3 FAIL: gpu_mech_id_writes=0 on mc2_03"
    exit 1
}
Write-Host "GATE 3 PASS: gpu_mech_id_writes=$gpuWrites mlr_mech_draws=$mlrDraws (record both in handoff)"
```

Expected: `gpu_mech_id_writes > 0` on mc2_03 (combat mission with multiple mechs).

**Record `mlr_mech_draws` value in the handoff.** Per spec Q6 amendment 3:
- If `mlr_mech_draws > 0` on any tier1 mission: M2.6 MUST preserve mover-first legacy fallback for MLR mechs; cannot claim full mech GPU-pick coverage.
- If `mlr_mech_draws == 0` across all 5 missions for ~3 ship cycles: gap is provably-rare-in-practice; M2.6 can ship without the conditional fallback warning.

### Gate 4: Firewall clean + shader-output uniqueness (external-review M2)

- [ ] **Step 5: Verify firewall**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: exit 0. `GameOS/` is outside SCOPE_DIRS so the new `RenderWorld/RenderWorld.h` include in `gos_mech_batcher.cpp` is not policed by the script (Q5). Reviewer-discipline gate: confirm visually that the include is to the PUBLIC `RenderWorld/RenderWorld.h` header (no `RenderWorld/legacy/*` reach), that the consumer uses only `IsObjectIdBufferEnabled()`, and that the engine -> engine direction holds.

- [ ] **Step 5.5: Re-run shader-output uniqueness gate (T4 Step 9.5 -- external-review M2)**

```powershell
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\*.frag" `
  -Pattern "layout\s*\(\s*location\s*=\s*2\s*\)\s*out"
Select-String -Path "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\mech.vert","A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\mech.frag" `
  -Pattern "flat .*uint .*objectId"
```

Expected (M2.5 allowlist):
- `layout(location=2) out` matches in EXACTLY two files: `shaders/static_prop.frag`, `shaders/mech.frag`.
- `flat .* uint .* objectId` matches once in `mech.vert` (as `flat out`) and once in `mech.frag` (as `flat in`).

Any third frag declaring attachment 2 = STOP and report (allowlist update required out of band). Missing `flat` qualifier = silent shader-link failure under env-ON (would surface as `event=shader_fail` in Gate 2; this static grep catches it before runtime).

### Gate 5: User-driven canary -- Shift+click on a mech (M2.5 substrate-only behavior)

- [ ] **Step 6: User-driven verification**

This gate is user-observable. Launch mc2_03 manually with both env vars:

```powershell
$env:MC2_OBJECT_ID_BUFFER = "1"
$env:MC2_STATIC_PROP_PICK = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_03 --duration 60 --kill-existing --keep-logs
$env:MC2_OBJECT_ID_BUFFER = $null
$env:MC2_STATIC_PROP_PICK = $null
```

User actions during the 60s window:
1. Shift+click on a static prop. Expected: `[STATIC_PROP_PICK v1] hit ...` log line (M1.6 behavior; M2.5 does NOT alter this). Proves no regression vs M1.6.
2. Shift+click on a mech. Expected: NO `[STATIC_PROP_PICK v1] hit ...` line (the M2-pre spine's mover-first gate short-circuits; mech pixels carry a Mech-kind handle, not a StaticProp-kind handle). Selection behavior MUST NOT change: mech click is still a no-op or falls through to mover-first legacy. M2.6 will add the `kind == Mech` branch; M2.5 ships substrate only (external-review m3 gate).

Post-run inspection (substrate-only proof):
- `[STATIC_PROP_PICK v1] hit` lines exist for static-prop clicks (proves the spine still works; no M1.6 regression).
- ZERO `[STATIC_PROP_PICK v1] hit` lines from mech clicks (substrate-only; mech pick is M2.6).
- `[MECH_OBJECT_ID_SELFTEST v1] result=PASS` exists (proves the substrate is inspectable).
- No selection behavior changes (legacy mover-first behavior preserved under Shift+click-on-mech).

### Gate 6: CLAUDE.md update

- [ ] **Step 7: Update Active campaigns + Known issues**

In `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md`, locate the M2 SHIPPED entry in Active campaigns and add the M2.5 entry after it.

**Add to Active campaigns (after the M2 SHIPPED bullet):**

```
- **RenderWorld Slice M2.5** (SHIPPED 2026-05-23): mech ObjectID substrate. Per-instance objectIdRaw field added to GpuMechInstance (48B -> 64B) + GpuMechSubmitDesc; mclib/mech3d.cpp submit site fills desc.objectIdRaw = getRenderWorldHandle().raw() unconditionally (Q3). GLSL prefix injection in gos_mech_batcher.cpp mirrors gos_static_prop_batcher.cpp:510-521 (#define MC2_OBJECT_ID_BUFFER 1 appended to "#version 430\n" when env-ON). shaders/mech.vert grows GpuMechInstance struct in lockstep; flat out uint v_objectIdRaw added; shaders/mech.frag adds gated flat in uint + layout(location=2) out uint v_objectId + body write. Observability (always-on per-mission): [MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N (Q4) emitted from GpuMechBatcher::onMapUnload(); companion event=mlr_mech_summary mlr_mech_draws=M (Q6 amendment 2) emitted on adjacent line via extern "C" cross-TU getter into mclib/mech3d.cpp. Synthetic self-test [MECH_OBJECT_ID_SELFTEST v1] (Q1) gated by MC2_MECH_OBJECT_ID_SELFTEST=1, wired into RenderWorld::init() after RunGameplayPickSelfTest(); validates registerMech / record kind+gen+alive / handle round-trip / destroyMech bump. Lockstep edit risk (spec §11): Task 1+2+3+4 land as ONE atomic commit because struct+shader hot-reload-without-relink reads wrong fields (symptom: wrong-mech meshes). CMakeFiles deleted before build to force full recompile of every TU including gos_mech_batcher.h. Firewall clean per Q5: new gos_mech_batcher.cpp -> RenderWorld/RenderWorld.h include is unpoliced (GameOS/ outside SCOPE_DIRS at scripts/check-include-firewall.sh:22); reviewer-discipline gate only. Tier1 5/5 PASS env-OFF (zero pixel delta vs M2 HEAD) AND env-ON MC2_OBJECT_ID_BUFFER=1 (event=shader_ok; gpu_mech_id_writes > 0 on mech missions; self-test PASS). mc2_03 canary: gpu_mech_id_writes=<RECORDED> mlr_mech_draws=<RECORDED>. Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-spec.md. Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-5-mech-objectid-substrate-plan.md. Next: M2.6 (mech pickup via tryGameplayPick branch on kind == Mech).
```

**Add to Known issues (current) -- verbatim per Q6 amendment 1 + 4:**

```
- MLR-rendered mechs do not write object IDs in M2.5. M2.6 pickup works only for GPU-batched mech pixels. If tier1 exercises MLR, pickup must fall back to legacy mover selection for those mechs and cannot claim full mech GPU-pick coverage. Tier1 mc2_03 baseline (M2.5 ship): mlr_mech_draws=<RECORDED>. M2.6 readiness decision rule documented in spec §12 Q6 amendment 3.
```

Replace `<RECORDED>` with the actual values captured in Gate 3.

- [ ] **Step 8: Commit docs**

```powershell
git add A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md
```

```bash
git commit -m "$(cat <<'EOF'
docs(renderworld): M2.5 SHIPPED -- update CLAUDE.md active campaigns + known issues

Marks RenderWorld Slice M2.5 (mech ObjectID substrate) as SHIPPED.
All 6 validation gates passed:
  - Gate 1: tier1 5/5 PASS env-OFF (zero pixel delta vs M2 HEAD).
  - Gate 2: tier1 5/5 PASS env-ON MC2_OBJECT_ID_BUFFER=1
            (event=shader_ok; gpu_mech_id_writes > 0; self-test PASS).
  - Gate 3: mc2_03 canary -- gpu_mech_id_writes / mlr_mech_draws recorded.
  - Gate 4: firewall clean (reviewer-discipline gate per Q5).
  - Gate 5: user-driven Shift+click canary -- substrate-only behavior
            preserved (mech-pick flip is M2.6 territory).
  - Gate 6: CLAUDE.md updated.

Known-issues bullet added verbatim per spec Q6 amendment 1+4:
MLR-rendered mechs do not write object IDs in M2.5; M2.6 readiness
decision rule (spec §12 Q6 amendment 3) consults tier1 mlr_mech_draws.

Next: M2.6 (mech pickup via tryGameplayPick spine branch on kind == Mech).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist

### Spec coverage

| Spec section | Covered by task |
|---|---|
| §1 (Purpose / closing the loop) | T3 (submit-site fill) + T4 (shader emit) |
| §2 (Relationship to M1.5 / M2 / M2-pre / M2.6) | Plan header + T4 commit message |
| §3 (Architecture: SSBO not uniform; no setSceneDrawBuffers extension) | T1 (struct) + T3 (per-instance fill) |
| §4.1.1 GpuMechSubmitDesc field | T1 Step 3 |
| §4.1.2 GpuMechInstance struct grow (48B->64B; pad fields named per Q2) | T1 Step 2 |
| §4.2.1 per-instance SSBO fill | T3 Step 4 |
| §4.2.2 GLSL prefix injection at program load | T2 Step 3 |
| §4.2.2 include add | T2 Step 2 |
| §4.3.1 mech.vert struct grow | T4 Step 1 |
| §4.3.2 mech.vert flat out uint | T4 Step 2 |
| §4.3.3 mech.vert body write | T4 Step 3 |
| §4.4.1 mech.frag flat in + layout(location=2) out | T4 Step 4 |
| §4.4.2 mech.frag body write | T4 Step 5 |
| §4.5 mech3d.cpp submit-site assignment | T3 Step 2 |
| §5 Gating (env var + GLSL macro discipline) | T2 Step 3 + T4 Steps 2/4 (macro gates) |
| §6 MLR fallback gap (counter + verbatim docs language) | T5 + T7 Step 7 (CLAUDE.md verbatim) |
| §7 Cost analysis (CPU + GPU budgets) | T7 Gate 2 (frame-time delta gate) |
| §8 Validation strategy (env-OFF + env-ON tier1 + self-test + user-driven) | T7 Gates 1+2+5+6 |
| §8 Render-contract registry update (GBuffer2 mech.frag) | T4 Step 6 |
| §8 Shader-output uniqueness grep | T7 Gate 4 (implicitly: firewall + grep diff) |
| §9 Firewall direction (Q5 reviewer-discipline; no script edit) | T7 Gate 4 |
| §10 META-FIX argument | Plan-header architecture line + T4 commit message |
| §11 Lockstep edit risk + hot-reload trap | T1 Step 4 (stage-not-commit) + T4 Step 7 (full relink) + T4 commit |
| §11 flat qualifier mandatory | T4 Steps 2 + 4 (verbatim flat in/out) |
| §11 Full relink discipline | T4 Step 7 (CMakeFiles delete) |
| §12 Q1 (separate self-test canary) | T6 |
| §12 Q2 (_pad1/_pad2/_pad3 names) | T1 Step 2 + T4 Step 1 |
| §12 Q3 (unconditional CPU fill) | T3 Step 2 |
| §12 Q4 (gpu_mech_id_writes always-on per-mission) | T3 Steps 3-6 |
| §12 Q5 (no firewall-script edit) | T2 Step 2 comment + T7 Gate 4 |
| §12 Q6 amendment 1 (verbatim docs language) | T7 Step 7 (CLAUDE.md verbatim) |
| §12 Q6 amendment 2 (mlr_mech_draws always-on counter) | T5 |
| §12 Q6 amendment 3 (M2.6 readiness decision rule) | T7 Gate 3 (record values) + Known issues line |
| §12 Q6 amendment 4 (CLAUDE.md known-issues at ship) | T7 Step 7 |

### Placeholder scan

- No "TBD" or "TODO" in the plan.
- No "implement later" or "similar to task N" shortcuts.
- All `Existing:` blocks show the actual current code (read at plan-write time and grep-verified per Citation drift section).
- All `Replace with:` blocks are complete, not partial diffs.
- Commit messages are specific (not placeholders).
- `<RECORDED>` placeholders in CLAUDE.md update text are intentional: filled in by Gate 3 measurements at execution time.

### Type consistency

- `RenderCore::RenderObjectHandle` used as the handle type throughout (consistent with M2 plan + M1.5 + M2-pre).
- `RenderWorld::IsObjectIdBufferEnabled()` -- exact spelling used in T2 Step 3.
- `RenderWorld::RenderMechDesc` -- exact spelling used in T6 Step 3 self-test (mirrors M2 plan T3 Step 2).
- `RenderWorld::RenderObjectKind::Mech` -- exact spelling used in T6 Step 3 (mirrors M2 plan T1 Step 3 enum decl).
- `RenderWorld::kRenderObjectFlagAlive` -- exact spelling used in T6 Step 3 (mirrors M1.5 substrate self-test usage at RenderWorld.cpp:266).
- `getRenderWorldHandle()` -- exact spelling (M2 accessor at mech3d.h:487-489); NOT `getRenderHandle()` or any abbreviation.
- `objectIdRaw` -- single canonical name on BOTH C++ side (`GpuMechSubmitDesc::objectIdRaw`, `GpuMechInstance::objectIdRaw`) AND GLSL side (`inst.objectIdRaw`, `v_objectIdRaw`). The frag `out` is `v_objectId` (matches M1.5 static_prop.frag:71 convention).
- `MC2_OBJECT_ID_BUFFER` (env var + GLSL macro) -- single canonical name; NOT `MC2_OBJ_ID_BUFFER` or any abbreviation.
- `MC2_MECH_OBJECT_ID_SELFTEST` -- single canonical name (matches the Q1-blessed log line `[MECH_OBJECT_ID_SELFTEST v1]`).
- `[MECHBATCHER v1] event=mech_id_summary` and `event=mlr_mech_summary` -- single canonical names; not "mech_id_writes_summary" or "mlr_draws_summary" or any variant.
- `[MECH_OBJECT_ID_SELFTEST v1]` -- single canonical log-prefix tag per Q1.

### Atomic-commit discipline

- Task 1 stages C++ struct grow; does NOT commit (stage-only Step 4).
- Task 2 stages GLSL macro injection; does NOT commit.
- Task 3 stages submit-site fill + per-instance fill + counter; does NOT commit.
- Task 4 stages shader struct + writes AND commits all of Tasks 1+2+3+4 together (Step 10). This is the lockstep requirement from spec §11.
- Task 5 commits MLR counter (additive on top of T4; does not retroactively change struct).
- Task 6 commits self-test (pure RenderWorld.cpp; independent).
- Task 7 commits docs (CLAUDE.md only).

Total commit count: 4. Each commit is independently revertable (T5 / T6 / T7 do not depend on each other; T4 is the load-bearing atomic lockstep).

---

PLAN STATUS: READY FOR EXECUTE -- external-review fixes applied (C1+M1+M2+M3+m1+m2+m3)

## Commit-message-ready summary of what the plan delivers

Slice M2.5 closes the M2 chain ("M2 stored the handle; M2.5 emits it to the GPU"). Five artifacts edited (gos_mech_batcher.h/.cpp, mech.vert, mech.frag, mech3d.cpp) + one self-test added (RenderWorld.cpp) + one docs update (CLAUDE.md). One atomic lockstep commit (T1+T2+T3+T4) + three independent follow-up commits (T5 MLR counter, T6 self-test, T7 docs). Substrate-only: M2.6 will flip the Shift+click trigger using the resulting Mech-handle pixels. All Q1-Q6 resolutions encoded verbatim; all four adversarial findings (MAJOR M1 firewall framing + 3 MINORs) applied. No firewall-script edit (Q5 reviewer-discipline gate). Two always-on per-mission counters (`gpu_mech_id_writes`, `mlr_mech_draws`) seed the M2.6 readiness decision rule (spec §12 Q6 amendment 3).
