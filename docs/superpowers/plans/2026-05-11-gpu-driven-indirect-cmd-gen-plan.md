# GPU-Driven Indirect Command Generation — Implementation Plan (Phase C)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Companion design:** [`docs/superpowers/specs/2026-05-11-gpu-driven-indirect-cmd-gen-design.md`](../specs/2026-05-11-gpu-driven-indirect-cmd-gen-design.md) — Stage 0 v4 at commit `40037e8`. This plan implements the design as written. Every architectural decision is locked there; this plan is mechanical execution.

> **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/` on branch `claude/gpu-driven-rendering`. Forked from `claude/nifty-mendeleev` @ `5667023`.

**Goal:** Eliminate ~2.2 ms/frame of CPU work in the combined Tracy zones `render textureManagerRenderLists` (1.35 ms) + `render water` (814 µs) + `render objects` (533 µs) by moving the per-frame CPU thin-record pack loops onto GPU compute. Frame is CPU-bound by ~15 ms at wolfman-mc2_10; savings translate to frame time.

**Architecture:** Per-bucket compute shaders read mission-static recipe SSBOs + Phase 1's lighting SSBO directly + per-frame quadList window, write thin-record SSBO + indirect-cmd SSBO + atomic visible-count. Beta two-dispatch pattern (cull/pack → patch-writes-cmd) ending with `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` before MDI. CPU path: arm + dispatch + barrier + MDI. ~30 µs CPU per bucket; replaces 600 µs+ per-bucket CPU pack loops.

**Tech stack:** OpenGL 4.3 single-context (`GL_COMMAND_BARRIER_BIT` required); std430 SSBOs; Tracy CPU+GPU zones; tier1 5/5 smoke regression gate; `superpowers:adversarial-plan-review` for each Stage's pre-land verification.

**Per-stage gate ladder (from design doc):**
A. Visual canary at fixed seed/camera (legacy/fast side-by-side).
B. Tracy delta target per zone (Z2 ≥80% for water Stage 1; Z1 ≥1.0 ms collectively for Stages 2+3).
C. `MC2_GPU_DRIVEN_PARITY=1` + `MC2_TERRAIN_LIGHTING_PARITY=1` byte-equal across stock tier1.
D. Tier1 5/5 PASS triple: unset / `<BUCKET>=1` / `<BUCKET>=1 PARITY=1`. +0 destroys delta per mission.

---

## Stage 0.5 — Prerequisites (Phase 1 API + smoke-runner registration)

These land before Stage 1 (either as a separate commit ahead of Stage 1 or as the first commits in Stage 1's PR). Both are tiny mechanical changes.

### Task 0.5.A — Publish `gos_terrain_lighting::GetOutputSsbo()` accessor

**Files:**
- Modify: `GameOS/gameos/gos_terrain_lighting.h` — add public declaration
- Modify: `GameOS/gameos/gos_terrain_lighting.cpp` — add one-line implementation

- [ ] **Step 1: Add public declaration to header**

Append to the `namespace gos_terrain_lighting { ... }` block in `gos_terrain_lighting.h` (after the existing parity API at `:99`):

```cpp
// Returns the GL buffer name of the per-vertex lighting output SSBO
// (lightRGB/fogRGB) written by the per-frame compute dispatch. Phase C
// compute shaders bind this at their input slot 1 to read lighting bytes
// directly, eliminating the CPU-mirror bounce that legacy pack loops
// require. Returns 0 if Phase 1 is disabled or not yet initialized;
// glBindBufferBase with buffer 0 unbinds the slot (well-defined per GL spec).
GLuint GetOutputSsbo();
```

- [ ] **Step 2: Add implementation to .cpp**

In `gos_terrain_lighting.cpp`, immediately after the existing `IsParityCheckEnabled()` definition at `:85-88`:

```cpp
GLuint GetOutputSsbo() {
    return s_computeOutputSsbo;
}
```

`s_computeOutputSsbo` is the existing file-static declared at `:137`.

- [ ] **Step 3: Verify build**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/build64
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build . --config RelWithDebInfo --clean-first
```

Expected: build succeeds, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_lighting.h GameOS/gameos/gos_terrain_lighting.cpp
git commit -m "feat(terrain_lighting): publish GetOutputSsbo() accessor for Phase C compute consumers"
```

### Task 0.5.B — Register Phase C env vars in `run_smoke.py`

**Files:**
- Modify: `scripts/run_smoke.py` env-var passthrough list (around `:259-284`)

- [ ] **Step 1: Add Phase C env vars to the passthrough tuple**

Locate the existing tuple at `scripts/run_smoke.py:259-284` (currently holds `MC2_TERRAIN_LIGHTING_PARITY`, `MC2_GPU_CULL*` family, etc.). Append:

```python
                            "MC2_GPU_DRIVEN",
                            "MC2_GPU_DRIVEN_WATER",
                            "MC2_GPU_DRIVEN_TERRAIN_SOLID",
                            "MC2_GPU_DRIVEN_OVERLAY",
                            "MC2_GPU_DRIVEN_PARITY",
                            "MC2_GPU_DRIVEN_TRACE",
```

- [ ] **Step 2: Verify with a passthrough dry-run**

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 5 --fail-fast
```

(env vars are unset; smoke should run identically to pre-change. Goal here is no regression from the script edit, not actual Phase C testing.)

- [ ] **Step 3: Commit**

```bash
git add scripts/run_smoke.py
git commit -m "smoke: register MC2_GPU_DRIVEN* env vars in run_smoke.py passthrough"
```

---

## Stage 1 — Water (precedent proof, Z2 anchor)

Stage 1 ships the canonical Phase C pattern on the smallest bucket. Anchor: drop `GameCamera::render waterFastPath` zone (`gamecam.cpp:255-256`) by ≥80% at wolfman-mc2_01 (water-heavy mission).

### Task 1.1 — `GpuDrivenBucketHeader` struct + shared infrastructure

**Files:**
- Create: `GameOS/gameos/gpu_driven_common.h` — shared types and helpers
- Create: `GameOS/gameos/gpu_driven_common.cpp` — common impl

- [ ] **Step 1: Define the shared header struct**

Create `gpu_driven_common.h`:

```cpp
#pragma once
#include "gos_graphics.h"   // for GLuint, etc.

namespace gpu_driven {

// 16-byte std430-aligned per-bucket header. `visibleCount` is an atomicAdd
// target; _pad slots reserved for future per-bucket telemetry. See design
// doc "SSBO schemas" section.
struct GpuDrivenBucketHeader {
    uint32_t visibleCount;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(GpuDrivenBucketHeader) == 16,
              "GpuDrivenBucketHeader must be 16 B");

// Shared kill-switch helpers (cached at process start; same pattern as
// gos_terrain_lighting::IsEnabled / IsParityCheckEnabled).
bool IsGlobalEnabled();   // MC2_GPU_DRIVEN unset OR != "0"
bool IsParityEnabled();   // MC2_GPU_DRIVEN_PARITY == "1"
bool IsTraceEnabled();    // MC2_GPU_DRIVEN_TRACE == "1"
bool IsWaterEnabled();    // MC2_GPU_DRIVEN_WATER unset OR != "0", AND IsGlobalEnabled()
bool IsTerrainSolidEnabled();  // MC2_GPU_DRIVEN_TERRAIN_SOLID — similar
bool IsOverlayEnabled();       // MC2_GPU_DRIVEN_OVERLAY — similar

}  // namespace gpu_driven
```

- [ ] **Step 2: Implement env-var caches in .cpp**

Create `gpu_driven_common.cpp` mirroring the `static const bool` cache pattern from `gos_terrain_lighting.cpp:85-87`. One getter per env var. Default behavior: `MC2_GPU_DRIVEN` defaults to ON (unset = enabled); per-bucket flags default to ON when global is on; PARITY and TRACE default OFF.

- [ ] **Step 3: Wire into CMakeLists.txt**

Add `gpu_driven_common.cpp` to the gameos library sources list.

- [ ] **Step 4: Build + commit**

```bash
git add GameOS/gameos/gpu_driven_common.h GameOS/gameos/gpu_driven_common.cpp CMakeLists.txt
git commit -m "feat(gpu_driven): add common header + env-var caches"
```

### Task 1.2 — Water compute shader (`gpu_driven_water.comp`)

**Files:**
- Create: `shaders/gpu_driven_water.comp`

- [ ] **Step 1: Write the shader source**

```glsl
// shaders/gpu_driven_water.comp — Phase C Stage 1
// Cull/pack pass. Reads water recipe + Phase 1 lighting + quad-window;
// writes water thin-record + atomicAdd into bucket header's visibleCount.
// Per-cmd uniforms live in slot 5; the cmd patch shader writes cmd.count
// in a separate single-invocation dispatch after this one.
//
// #version 430 prefix is added by host-side build_compute_program_from_file.

layout(local_size_x = 64) in;

struct WaterRecipe {
    // [layout matches existing g_recipeBuffer struct in
    //  gos_terrain_water_stream.cpp — Stage 1 plan-write task 1.2.2 below
    //  will grep the exact layout and paste it here verbatim.]
};

struct WaterThinRecord {
    // [layout matches existing WaterThinRecord at
    //  gos_terrain_water_stream.cpp:453 — verbatim from grep at task time.]
};

struct GpuDrivenBucketHeader { uint visibleCount; uint pad0; uint pad1; uint pad2; };

// Phase 1 lighting output (read-only). Layout matches GpuTerrainLightingOutput
// at gos_terrain_lighting.cpp/h. Bind at slot 1.
struct GpuTerrainLightingOutput {
    // [from gos_terrain_lighting.h — paste verbatim at task time]
};

layout(std430, binding = 0) readonly  buffer Recipes      { WaterRecipe          recipes[]; };
layout(std430, binding = 1) readonly  buffer Lighting     { GpuTerrainLightingOutput lighting[]; };
layout(std430, binding = 2) readonly  buffer QuadWindow   { uint                 windowIdx[]; };  // per-frame: recipe indices in this frame's quadList window
layout(std430, binding = 3) writeonly buffer Thin         { WaterThinRecord      thin[]; };
layout(std430, binding = 6) coherent  buffer Header       { GpuDrivenBucketHeader hdr; };

layout(std140, binding = 0) uniform CameraUbo {
    mat4 terrainMVP;
    vec4 cameraPos;
    // ... grep against existing CPU upload sites for parity layout ...
};

layout(std140, binding = 1) uniform WaterParamsUbo {
    float cloudOffsetX;
    float cloudOffsetY;
    float sprayOffsetX;
    float sprayOffsetY;
    // ... etc., grep from existing water-fast-path uniforms ...
};

uniform uint u_windowCount;
uniform uint u_maxThinRecords;

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= u_windowCount) return;

    uint recipeIdx = windowIdx[id];
    WaterRecipe r = recipes[recipeIdx];

    // [Per-corner projectZ check — matches the CPU pack skip set at
    //  gos_terrain_water_stream.cpp:475-480 verbatim. Pre-cull on
    //  CPU rule per memory/water_ssbo_pattern.md constraint #3 is
    //  satisfied because compute shader uses the SAME formula.]
    vec4 c0 = terrainMVP * vec4(r.vx0, r.vy0, r.elev0, 1.0);
    // ... etc for all 4 corners
    uint pzValidBits = /* compute per-tri pzValid from corner clips */;
    if (pzValidBits == 0u) return;  // both tris cull

    // Per-corner lightRGB / fogRGB from Phase 1 SSBO (FRESH same-frame
    // after Phase 1's post-dispatch barrier — see design doc Phase 1
    // same-frame barrier ordering subsection).
    uvec4 lrgb = uvec4(
        lighting[r.vn0].lightRGB,
        lighting[r.vn1].lightRGB,
        lighting[r.vn2].lightRGB,
        lighting[r.vn3].lightRGB
    );
    uvec4 frgb = uvec4(
        lighting[r.vn0].fogRGB,
        lighting[r.vn1].fogRGB,
        lighting[r.vn2].fogRGB,
        lighting[r.vn3].fogRGB
    );

    // Atomic slot allocation.
    uint outSlot = atomicAdd(hdr.visibleCount, 1u);
    if (outSlot >= u_maxThinRecords) return;

    // Pack thin record. Layout BYTE-IDENTICAL to legacy CPU pack at
    // gos_terrain_water_stream.cpp:453+ — parity check enforces.
    WaterThinRecord tr;
    tr.recipeIdx = recipeIdx;
    tr.flags     = pzValidBits;
    tr.lightRGB0 = lrgb.x; tr.lightRGB1 = lrgb.y;
    tr.lightRGB2 = lrgb.z; tr.lightRGB3 = lrgb.w;
    tr.fogRGB0   = frgb.x; tr.fogRGB1   = frgb.y;
    tr.fogRGB2   = frgb.z; tr.fogRGB3   = frgb.w;
    // ... terrainTypeToMaterial low-byte patch per water_ssbo_pattern.md
    //     "Thin-record byte parity" section ...
    thin[outSlot] = tr;
}
```

The `[grep at task time]` placeholders are deliberate — the plan executor MUST grep the actual current struct layouts and paste them verbatim before compiling. The design doc's `cpp_glsl_ubo_struct_lockstep.md` rule applies.

- [ ] **Step 2: Verify shader compiles standalone**

Use the existing `build_compute_program_from_file` pattern from `gpu_cull_compute.cpp:145-229` (copy locally per Phase 1 design doc Q1's "copy not factor" decision).

### Task 1.3 — Shared cmd patch shader (`gpu_driven_cmd_patch.comp`)

**Files:**
- Create: `shaders/gpu_driven_cmd_patch.comp`

- [ ] **Step 1: Write the shader** (~12 lines)

```glsl
// Single-invocation dispatch. Reads BucketHeader.visibleCount and writes
// IndirectCmd[0..N-1].count. Verts-per-element is a uniform.

layout(local_size_x = 1) in;

struct GpuDrivenBucketHeader { uint visibleCount; uint pad0; uint pad1; uint pad2; };
struct DrawArraysIndirectCommand { uint count; uint instanceCount; uint first; uint baseInstance; };

layout(std430, binding = 0) readonly buffer Header { GpuDrivenBucketHeader hdr; };
layout(std430, binding = 1) writeonly buffer Cmd { DrawArraysIndirectCommand cmds[]; };

uniform uint u_vertsPerElement;   // 6 for terrain quads, 6 for water quads
uniform uint u_cmdCount;          // 1 for SOLID/OVERLAY, 2 for water (base + detail/spray)

void main() {
    uint vis = hdr.visibleCount;
    for (uint i = 0u; i < u_cmdCount; ++i) {
        cmds[i].count         = vis * u_vertsPerElement;
        cmds[i].instanceCount = 1u;
        cmds[i].first         = 0u;
        cmds[i].baseInstance  = 0u;
    }
}
```

- [ ] **Step 2: Commit shaders together with task 1.2**

```bash
git add shaders/gpu_driven_water.comp shaders/gpu_driven_cmd_patch.comp
git commit -m "feat(gpu_driven): water + shared cmd-patch compute shaders"
```

### Task 1.4 — Host-side water compute dispatch

**Files:**
- Modify: `GameOS/gameos/gos_terrain_water_stream.h` — declare new public API
- Modify: `GameOS/gameos/gos_terrain_water_stream.cpp` — add dispatch path

- [ ] **Step 1: Add new public API declarations to header**

```cpp
namespace WaterStream {
    // Existing: BeginFrameNarrow, NarrowEnabled, UploadAndBindThinRecords, ...

    // Phase C Stage 1.
    // Dispatches GPU compute that writes thin-record SSBO + indirect-cmd
    // SSBO + atomic visible-count. Returns true on dispatch success;
    // false if killswitch off or resources not ready. Always issues the
    // GL_COMMAND_BARRIER_BIT before return. Called from the
    // renderWaterFastPath bridge instead of UploadAndBindThinRecords
    // when MC2_GPU_DRIVEN_WATER is on.
    bool ComputeDispatchAndBindThinRecords();

    // Accessor used by the bridge to bind the indirect-cmd buffer for MDI.
    GLuint GetIndirectCmdBuffer();
    GLuint GetPerCmdSsbo();  // for the gl_DrawID-indexed per-cmd uniforms
}
```

- [ ] **Step 2: Implement `ComputeDispatchAndBindThinRecords()`**

Pseudocode (real impl follows the Beta two-dispatch shape from design doc):

```cpp
bool ComputeDispatchAndBindThinRecords() {
    ZoneScopedN("WaterStream::ComputeDispatchAndBindThinRecords");

    if (!gpu_driven::IsWaterEnabled()) return false;
    if (g_recipeBuffer == 0)           return false;   // no recipe yet
    if (g_waterComputeProgram == 0) {
        // Lazy-build compute programs on first call.
        g_waterComputeProgram   = build_compute_program_from_file(
            "gpu_driven_water.comp", nullptr, 0, "gpu_driven_water");
        g_cmdPatchComputeProgram = build_compute_program_from_file(
            "gpu_driven_cmd_patch.comp", nullptr, 0, "gpu_driven_cmd_patch");
        if (!g_waterComputeProgram || !g_cmdPatchComputeProgram) return false;
    }

    // Build per-frame QuadWindow SSBO from current quadList (CPU-side; one
    // pass to extract recipe indices that match the existing skip set).
    const uint32_t windowCount = BuildQuadWindowSSBO();
    if (windowCount == 0) {
        // Issue count=0 MDI by leaving header.visibleCount=0 and dispatching.
        // Or skip the dispatch entirely; both are acceptable. Skip is cheaper.
        return false;
    }

    // Zero the header (visibleCount = 0) via glClearNamedBufferSubData.
    GLuint zero = 0u;
    glClearNamedBufferSubData(g_waterBucketHeaderSsbo, GL_R32UI, 0,
                              sizeof(GpuDrivenBucketHeader),
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);

    // Bind compute SSBOs and UBOs per design doc per-program binding table.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1,
                     gos_terrain_lighting::GetOutputSsbo());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_quadWindowSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g_thinRecordSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, g_perCmdSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_waterBucketHeaderSsbo);

    glUseProgram(g_waterComputeProgram);
    SetCameraUbo();        // existing helper or new — grep parity
    SetWaterParamsUbo();   // new — populate cloudOffset, sprayOffset, etc.
    glUniform1ui(glGetUniformLocation(g_waterComputeProgram, "u_windowCount"),
                 windowCount);
    glUniform1ui(glGetUniformLocation(g_waterComputeProgram, "u_maxThinRecords"),
                 kMaxWaterThinRecords);

    const uint32_t groups = (windowCount + 63u) / 64u;
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Patch dispatch: write cmd.count.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_waterBucketHeaderSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_indirectCmdBuffer);
    glUseProgram(g_cmdPatchComputeProgram);
    glUniform1ui(glGetUniformLocation(g_cmdPatchComputeProgram, "u_vertsPerElement"), 6u);
    glUniform1ui(glGetUniformLocation(g_cmdPatchComputeProgram, "u_cmdCount"),        2u);  // base + detail
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    glUseProgram(0);
    return true;
}
```

- [ ] **Step 3: Build, fix compile errors, repeat until clean**

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_water_stream.h GameOS/gameos/gos_terrain_water_stream.cpp
git commit -m "feat(gpu_driven): Stage 1 water host-side compute dispatch path"
```

### Task 1.5 — Modify water fast-path bridge for 2-cmd MDI

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp` water fast-path bridge around `:2197-2259` to issue `glMultiDrawArraysIndirect` with `drawcount=2` when armed.

- [ ] **Step 1: Add WaterPerCmd struct definition near the bridge**

In `gameos_graphics.cpp` near `:2197`:

```cpp
struct WaterPerCmd {
    uint32_t textureSlot;
    uint32_t isWater;
    uint32_t detailMode;
    float    uvScale;
    float    uvOffsetX;
    float    uvOffsetY;
    uint32_t _pad0;
    uint32_t _pad1;
};
static_assert(sizeof(WaterPerCmd) == 32, "");
```

- [ ] **Step 2: Build the per-cmd SSBO once per frame on CPU**

In the bridge, before the MDI call: populate `g_perCmdSsbo` with two `WaterPerCmd` entries (base + detail). Upload via `glBufferSubData`.

- [ ] **Step 3: Replace 2× `glDrawArrays` with `glMultiDrawArraysIndirect`**

Replace `:2215` and `:2242` with:

```cpp
// Bind both textures to a TEXTURE_2D_ARRAY (or keep 2 separate units if the
// FS reads `texture(tex0 or tex1)` keyed on per-cmd state — grep the existing
// FS for the texture-binding pattern).
glBindBuffer(GL_DRAW_INDIRECT_BUFFER, WaterStream::GetIndirectCmdBuffer());
glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, /*drawcount=*/ 2, /*stride=*/ 0);
glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
```

The VS+FS must be updated (or a new pair created) that reads `WaterPerCmd[gl_DrawID]` via a `flat varying uint cmdId = gl_DrawID;` rather than per-uniform `isWater`/`detailMode`/`uvScale`/`uvOffset`. This is a small VS+FS edit — keep the existing water-fast-path shaders and add a `gl_DrawID`-aware variant.

- [ ] **Step 4: Hook the bridge to use the new path when armed**

The water-fast-path entry function checks `WaterStream::IsGpuDrivenArmed()` (set by `ComputeDispatchAndBindThinRecords` returning true). If armed: issue MDI. Else: fall back to existing 2× `glDrawArrays`.

- [ ] **Step 5: Build, fix, commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp shaders/gos_terrain_water_fast.vert shaders/gos_tex_vertex.frag
git commit -m "feat(gpu_driven): Stage 1 water bridge — 2-cmd MDI with WaterPerCmd"
```

### Task 1.6 — Water parity check

**Files:**
- Modify: `gos_terrain_water_stream.cpp` — add `ComputeDispatchParity_Check()` that runs both paths and byte-compares.

- [ ] **Step 1: Implement parity-mode dual-run + byte compare**

When `MC2_GPU_DRIVEN_PARITY=1` AND `MC2_TERRAIN_LIGHTING_PARITY=1`:
- Run legacy `UploadAndBindThinRecords()` into a parity SSBO.
- Run `ComputeDispatchAndBindThinRecords()` into the live thin-record SSBO.
- Read back both via fenced ring (NOT `glGetBufferSubData` on hot path per `substrate_coalesce_sync_point_lesson.md`) and byte-compare 600 frames late.
- Emit `[GPU_DRIVEN_WATER_PARITY v1] event=summary frames=N quads_checked=Q total_mismatches=K` every 600 frames.
- Field-level mismatch printer throttled to 16 prints/frame.

Mirror the renderWater Stage 3 parity infrastructure shape (see `gos_terrain_water_stream.cpp` existing parity code).

- [ ] **Step 2: Commit**

```bash
git add GameOS/gameos/gos_terrain_water_stream.cpp
git commit -m "feat(gpu_driven_water): parity check infrastructure"
```

### Task 1.7 — Stage 1 verification gates

- [ ] **Step 1: Build + deploy**

```bash
sh A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/.claude/skills/mc2-build-deploy.md  # follow skill manually
```

- [ ] **Step 2: Visual canary**

Run tier1 mc2_01 (water-heavy) with `MC2_GPU_DRIVEN_WATER=0` (legacy) and `MC2_GPU_DRIVEN_WATER=1` (Stage 1). Screenshot diff at fixed seed/camera. Expect pixel-identical.

- [ ] **Step 3: Parity gate**

```bash
MC2_GPU_DRIVEN_PARITY=1 MC2_TERRAIN_LIGHTING_PARITY=1 MC2_GPU_DRIVEN_WATER=1 \
  py -3 scripts/run_smoke.py --tier tier1 --duration 30
```

Expect `total_mismatches=0` across all 5 tier1 missions.

- [ ] **Step 4: Tracy delta gate**

Capture Tracy on wolfman-mc2_01. `GameCamera::render waterFastPath` zone should drop ≥80% (target: ~150 µs from ~814 µs).

- [ ] **Step 5: Tier1 5/5 PASS triple**

Run smoke with: unset / `MC2_GPU_DRIVEN_WATER=1` / `MC2_GPU_DRIVEN_WATER=1 MC2_GPU_DRIVEN_PARITY=1 MC2_TERRAIN_LIGHTING_PARITY=1`. All three pass + 0 destroys delta per mission.

- [ ] **Step 6: Stage 1 sign-off commit**

```bash
git tag stage-1-water-gates-pass
```

---

## Stage 2 — Terrain SOLID (Z1 anchor, largest CPU saver)

Stage 2 is the largest single CPU saver — targets ≥1.0 ms drop on `GameCamera::render textureManagerRenderLists` at wolfman-mc2_10. Reuses Stage 1's `gpu_driven_cmd_patch.comp` and `GpuDrivenBucketHeader`.

### Task 2.1 — Split `ComputePreflight()` into preflight + `ComputeDispatch()` (CRITICAL per design v4)

**Files:**
- Modify: `GameOS/gameos/gos_terrain_indirect.h` — add `ComputeDispatch()` declaration
- Modify: `GameOS/gameos/gos_terrain_indirect.cpp` — split body
- Modify: `mclib/terrain.cpp` — insert `ComputeDispatch()` call at `:1800`

- [ ] **Step 1: Add new public API symbol**

In `gos_terrain_indirect.h`:

```cpp
// v4 split: ComputePreflight() does arming gates only. ComputeDispatch()
// runs after Phase 1's PackAndDispatch at terrain.cpp:1798 so it can read
// gos_terrain_lighting::GetOutputSsbo() with same-frame data.
void ComputeDispatch();
```

- [ ] **Step 2: Move dispatch body**

Currently `ComputePreflight()` at `gos_terrain_indirect.cpp:1605-1637` returns bool and (under v4 GPU-driven) was going to issue the compute dispatch. Split:
- `ComputePreflight()` — keep gates (`IsDenseRecipeReady`, `ResourcesReady`, etc.); set `s_frameSolidArmed` based on gate results; upload `QuadWindowSSBO`; do NOT dispatch compute.
- `ComputeDispatch()` — new function; if `!s_frameSolidArmed || !gpu_driven::IsTerrainSolidEnabled()` return; else issue the Beta two-dispatch sequence (mirrors water Stage 1.4 host-side code).

- [ ] **Step 3: Insert `ComputeDispatch()` call in terrain.cpp**

In `mclib/terrain.cpp`, insert at line 1800 (between `:1799` `CopyResultsToVertexPool` and `:1805` `WaterStream::BeginFrameNarrow`):

```cpp
// Phase C: SOLID compute dispatch. MUST be AFTER PackAndDispatch above
// so Phase 1's post-dispatch barrier has published the lighting SSBO.
gos_terrain_indirect::ComputeDispatch();
```

- [ ] **Step 4: Build, run tier1 with `MC2_GPU_DRIVEN_TERRAIN_SOLID=0` — must be byte-identical to pre-Stage-2 (no behavioral change yet, just split + hook)**

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_terrain_indirect.h GameOS/gameos/gos_terrain_indirect.cpp mclib/terrain.cpp
git commit -m "refactor(gpu_driven_solid): split ComputePreflight into arming+dispatch hooks"
```

### Task 2.2 — SOLID compute shader (`gpu_driven_terrain_solid.comp`)

**Files:**
- Create: `shaders/gpu_driven_terrain_solid.comp`

- [ ] **Step 1: Write shader**

Mirror Stage 1's `gpu_driven_water.comp` shape. Differences:
- Recipe struct = `TerrainQuadRecipe` (grep from `gos_terrain_indirect.cpp` at task time — declared near `:1247-1253` per design doc citations)
- Thin record = `TerrainQuadThinRecord` (grep from `gos_terrain_indirect.cpp` for the existing layout)
- 1 indirect cmd, not 2 (no per-cmd variation)
- Texture-slot field per `mc2_texture_handle_is_live.md` — write slot index, not GL handle
- Cement-atlas index field per `indirect_terrain_solid_endpoint.md` cement-multi-sampler precedent

- [ ] **Step 2: Compile-test standalone, then commit**

```bash
git add shaders/gpu_driven_terrain_solid.comp
git commit -m "feat(gpu_driven_solid): SOLID cull/pack compute shader"
```

### Task 2.3 — SOLID host-side `ComputeDispatch()` body

**Files:**
- Modify: `gos_terrain_indirect.cpp` — fill in `ComputeDispatch()` body

- [ ] **Step 1: Implement the Beta two-dispatch sequence**

Pattern matches Stage 1's water `ComputeDispatchAndBindThinRecords` verbatim:
1. Zero `g_solidBucketHeaderSsbo.visibleCount`.
2. Bind compute SSBOs at slots 0-6 per design doc table.
3. Bind cmd-patch shader at the next step.
4. `glDispatchCompute(groups, 1, 1)` for the SOLID cull/pack.
5. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)`.
6. Switch to `g_cmdPatchComputeProgram`, bind header+cmd, set `u_vertsPerElement=6, u_cmdCount=1`, `glDispatchCompute(1, 1, 1)`.
7. `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)`.

`PackThinRecordsForFrame()` and `BuildIndirectCommands()` become **no-ops when `MC2_GPU_DRIVEN_TERRAIN_SOLID=1`** (early-return at function top; existing function bodies retained for legacy fallback per "demote-don't-delete" rule).

- [ ] **Step 2: Ring-slot binding**

Per design doc "Ring-slot persistence" subsection: bind `g_thinRecordSSBO` via `glBindBufferRange` at the per-frame ring slot, both for the compute write AND for the consumer MDI bind. The existing ring-slot logic at `:1300` and `gameos_graphics.cpp:2458-2464` stays unchanged.

- [ ] **Step 3: Build + commit**

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "feat(gpu_driven_solid): host-side compute dispatch path"
```

### Task 2.4 — SOLID parity check

**Files:**
- Modify: `gos_terrain_indirect.cpp` — add `ComputeDispatchParity_Check()` mirroring Stage 1.6's pattern.

- [ ] **Step 1: Dual-run + byte-compare under combined parity envs.**
- [ ] **Step 2: Field-level mismatch printer.**
- [ ] **Step 3: 600-frame summary.**
- [ ] **Step 4: Commit.**

### Task 2.5 — Stage 2 verification gates

- [ ] **Step 1: Visual canary** — tier1 mc2_10 (CPU-heavy) screenshot diff.
- [ ] **Step 2: Parity gate** — tier1 5/5 with `MC2_GPU_DRIVEN_TERRAIN_SOLID=1 MC2_GPU_DRIVEN_PARITY=1 MC2_TERRAIN_LIGHTING_PARITY=1`. Expect `mismatches=0`.
- [ ] **Step 3: Tracy delta gate** — wolfman-mc2_10: Z1 drops ≥1.0 ms from 1.35 ms. Combined Z1+Z2 ≤ 500 µs.
- [ ] **Step 4: Tier1 5/5 PASS triple.**
- [ ] **Step 5: Sign-off tag** `stage-2-solid-gates-pass`.

---

## Stage 3 — Terrain OVERLAY (CONDITIONAL — gated on Phase B)

**Pre-flight check before starting Stage 3:**

```bash
# Has Phase B published an overlay recipe SSBO at this point?
grep -rn "overlay" A:/Games/mc2-opengl-src/.claude/worktrees/pre-bake-terrain/GameOS/gameos/gos_terrain_indirect.cpp | grep -i 'recipe\|ssbo'
```

If the grep yields zero new overlay-recipe symbols beyond what existed at baseline `5667023`, **Stage 3 falls out of v1**. Skip to Stage 4. OVERLAY remains scaffold-only; a follow-up slice picks it up when Phase B's recipe lands.

If Phase B HAS published an overlay recipe — proceed with the tasks below. Their shape mirrors Stage 2 but with the Phase B recipe as input.

### Task 3.1 — Pull Phase B's overlay recipe symbols

**Files:** depend on what Phase B published. Likely additions to `gos_terrain_indirect.{h,cpp}` for the recipe SSBO + flip `IsFrameOverlayArmed()` to true under `MC2_GPU_DRIVEN_OVERLAY=1`.

- [ ] **Step 1: Merge Phase B's commits or read across worktrees.**
- [ ] **Step 2: Grep-verify the recipe layout. Apply the design doc's "Phase B recipe-layout-frozen contract": Phase C compute shader's read paths must match field-for-field.**

### Task 3.2 — OVERLAY compute shader (`gpu_driven_terrain_overlay.comp`)

**Files:**
- Create: `shaders/gpu_driven_terrain_overlay.comp`

- [ ] **Step 1: Mirror Stage 2's shader with the overlay recipe's specific fields.**
- [ ] **Step 2: Commit.**

### Task 3.3 — OVERLAY host-side dispatch + bridge

**Files:**
- Modify: `gos_terrain_indirect.cpp` — `OverlayComputeDispatch()` + flip `IsFrameOverlayArmed()`.
- Modify: `gameos_graphics.cpp` OVERLAY MDI bridge if not present.

- [ ] **Step 1: Mirror Stage 2 host-side pattern.**
- [ ] **Step 2: Commit.**

### Task 3.4 — Stage 3 verification gates

The OVERLAY path has NO legacy CPU baseline to parity against (`IsFrameOverlayArmed()` was unconditionally false). Gate ladder:

- [ ] **Step 1: Visual canary at fixed seed/camera** — `MC2_GPU_DRIVEN_OVERLAY=0` (no overlay drawn — current baseline) vs `MC2_GPU_DRIVEN_OVERLAY=1` (overlay drawn). Manual inspection.
- [ ] **Step 2: Tier1 5/5 PASS** with `MC2_GPU_DRIVEN_OVERLAY=0` (must match pre-Stage-3 baseline bit-for-bit) AND `MC2_GPU_DRIVEN_OVERLAY=1` (overlay drawn — may cause visual changes from baseline; that's expected).
- [ ] **Step 3: Sign-off tag** `stage-3-overlay-gates-pass`.

---

## Stage 4 — Soak window

7 days per Track B precedent. All shipped Phase C buckets soak with `_PARITY=1` running silently in nightly tier1 runs.

- [ ] **Day 1-2:** monitor nightly tier1 logs for any `[GPU_DRIVEN_*_PARITY v1] event=mismatch` lines. Zero tolerance — any mismatch reverts the offending bucket to parity-only mode.
- [ ] **Day 3-4:** spot-check wolfman zoom on mc2_10 and mc2_01 — Tracy zone deltas stable at design-target levels.
- [ ] **Day 5-6:** explicit camera-pan canaries (RTS-pan-into-fog-of-war stress) — no destroy delta vs pre-Stage-2 baseline.
- [ ] **Day 7:** soak sign-off summary written to memory file `memory/gpu_driven_indirect_cmds.md`. Includes per-bucket fps deltas and any anomalies.

If any bucket fails soak → revert to parity-only for that bucket; bisect; restart soak window for that bucket only. Other buckets may proceed independently.

---

## Stage 5 — Per-bucket default-on flips (rolling)

Each bucket flips independently. Order: water → SOLID → OVERLAY (if shipped). Each flip is a one-line code change.

### Task 5.1 — Flip water default-on

- [ ] **Step 1: Modify `gpu_driven_common.cpp::IsWaterEnabled()`** — change default from unset-equals-OFF to unset-equals-ON (literal `"0"` opts out).
- [ ] **Step 2: Run tier1 5/5** with env unset (should now be ON by default).
- [ ] **Step 3: Run tier1 5/5** with `MC2_GPU_DRIVEN_WATER=0` (must reproduce pre-Stage-1 behavior bit-for-bit).
- [ ] **Step 4: Commit.**

```bash
git commit -m "feat(gpu_driven_water): flip default-on"
```

### Task 5.2 — Flip SOLID default-on

(Mirror 5.1 with `IsTerrainSolidEnabled()`.)

### Task 5.3 — Flip OVERLAY default-on (if Stage 3 shipped)

(Mirror 5.1 with `IsOverlayEnabled()`.)

---

## Stage 6 — Demote legacy CPU paths

Per CLAUDE.md "demote-don't-delete" rule: gate legacy paths off, leave them in tree. **No code deletion in this stage** — a separate post-soak slice does deletion.

### Task 6.1 — Demote water legacy `UploadAndBindThinRecords` to silent

- [ ] **Step 1: `UploadAndBindThinRecords()` becomes a no-op when `gpu_driven::IsWaterEnabled()`** — already done in Stage 1; in Stage 6 just verify no surprise call sites still reach it.
- [ ] **Step 2: Grep `UploadAndBindThinRecords` for any remaining unconditional callers.** Confirm all go through the killswitch.

### Task 6.2 — Demote SOLID legacy `PackThinRecordsForFrame` + `BuildIndirectCommands`

(Same shape as 6.1.)

### Task 6.3 — Stage 6 sign-off commit

```bash
git commit -m "chore(gpu_driven): demote legacy CPU pack paths to gated-off"
git tag phase-c-v1-complete
```

---

## Memory file update (post-Stage-6)

Create `memory/gpu_driven_indirect_cmds.md`:

```markdown
---
name: GPU-driven indirect command generation — Phase C v1 shipped
description: Per-bucket compute shaders + Beta two-dispatch pattern + GL_COMMAND_BARRIER_BIT sync. Pattern for future per-frame CPU→GPU offloads.
type: project
---

(Captures: compute-shader pattern, per-bucket invalidation contract, sync-stall
avoidance, Phase 1 lighting SSBO direct-read win, post-Stage-6 perf deltas
per bucket at wolfman anchor.)
```

Append to `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` index under "Rendering / shaders" section.

---

## Stop conditions (per stage)

- Per-bucket parity diff non-zero after 3 iteration rounds → STOP, surface to user. Likely cause: compute math vs CPU math drift; or barrier ordering.
- Per-bucket Tracy delta < 200 µs → STOP that bucket, surface to user. Other buckets may still ship.
- Sync stall surfaces in profiling (Tracy GPU timeline shows CPU wait for GPU completion) → STOP, switch to non-blocking ring + skip-frame fallback per `gpu_cull_readback.cpp` precedent.
- Any tier1 mission FAIL under default-on flip → STOP, revert that bucket to parity-only, bisect.
- AMD driver compute-dispatch-before-MDI ordering bug surfaces → STOP, surface to user; may need explicit fence between dispatch and draw.

## Adversarial review checkpoints

Per CLAUDE.md "Review Discipline" — adversarial review runs at each of these points:

1. After Stage 0.5 + Stage 1 — first compute-shader bucket shipped; review for any pattern drift before Stage 2 scales it up.
2. After Stage 2 — largest CPU-saver bucket; review for the `ComputePreflight`/`ComputeDispatch` split correctness and the Phase 1 same-frame read.
3. After Stage 3 (if shipped) — Phase B integration.
4. Before any default-on flip in Stage 5 — soak data + parity history review.

Each adversarial-review dispatch MUST include the verbatim instruction "use the adversarial-plan-review skill in `.claude/skills/`".

---

## Self-review checklist (after writing this plan)

- [x] Every Stage in the design doc has a corresponding plan section.
- [x] Stage 0.5 prerequisites are explicit and small.
- [x] Each Stage has a verification gate ladder matching the design doc.
- [x] Killswitches use the exact env-var names from the design doc.
- [x] `ComputePreflight()`/`ComputeDispatch()` split (v4 CRITICAL fix) is the first task of Stage 2.
- [x] `scripts/run_smoke.py` env-var registration is in Stage 0.5.B (NOT buried in Stage 1).
- [x] Stage 3 conditionality on Phase B is the explicit pre-flight check at top of Stage 3.
- [x] No code deletion in v1 (per "demote-don't-delete" rule).
- [x] Memory file update is in the plan, not optional.
- [x] Adversarial review checkpoints called out.

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-11-gpu-driven-indirect-cmd-gen-plan.md`. Two execution options:

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.
**2. Inline Execution** — execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints for review.

Either approach should run adversarial review at each checkpoint per the discipline rule above. Stage 0.5 is mechanical and small; Stage 1 is the precedent-proof and benefits from a dispatched subagent + post-task review.
