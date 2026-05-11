# `quadSetupTextures` GPU-Compute Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the dominant CPU cost inside `Terrain::geometry quadSetupTextures` (5.18 ms/frame lighting + 0.91 ms/frame water-vertex projection at mc2_10 wolfman zoom) with two compute-shader passes feeding the existing per-vertex CPU state via SSBO, retiring ~6 ms/frame from the CPU floor.

**Architecture:** Per-vertex terrain lighting and water-elevation vertex projection are computed once per frame on the GPU into a dense SSBO indexed by `vertexNum`. The CPU `setupTextures()` body, instead of computing `lightRGB` / `wx/wy/wz/ww/clipInfo` inline, reads from the SSBO via a small CPU-side mirror that staging-copies the SSBO at end-of-frame (or before the consumer `TerrainQuad::drawWater()` runs). Parity gates compare CPU and GPU outputs per-vertex during soak; killswitches restore the legacy path. The pattern mirrors `water_ssbo_pattern.md`, `patchstream_shape_c.md`, and `indirect_terrain_solid_endpoint.md` — three already-shipped CPU→GPU offload slices in this codebase.

**Tech Stack:** GLSL 4.3 compute shaders (single shared GL context per `Q5` of `2026-05-08-job-system-parallel-for-scope.md`), SSBO uploads via existing `gos_terrain_*` infrastructure, MSVC2022 RelWithDebInfo build, tier1 smoke runner with mc2_10 as the perf canary.

**Recon foundation:** Slice 0 cost-split (this worktree, frame-3000 steady state, mc2_10 wolfman):

| Bucket | µs/frame | % of setup_total |
|---|---:|---:|
| setup_total | 11,871 | 100% |
| **lighting** | **5,177** | **43.6%** (Phase 1 target) |
| **water_vert_proj** | **911** | **7.7%** (Phase 2 target) |
| visibility_check | 890 | 7.5% |
| recipe_cache | 656 | 5.5% |
| cache_resident | 269 | 2.3% |
| detail_overlay | 201 | 1.7% |
| residual (distributed dispatch overhead) | 3,767 | 31.7% |

Phase 1 + Phase 2 combined cut: ~6.09 ms/frame at heavy state. After port the per-call setupTextures cost also drops because the gate-test + per-vertex setup that wraps the lighting and water bodies retires (estimated additional 1-2 ms cut from the residual).

---

## Out of scope

- **Threading / parallel-for on `quadSetupTextures`.** This is the original dispatch prompt's framing, retired by Phase B's architectural review (see this worktree's prior session log). GPU compute supplants threading for this work. The cost-split data confirmed it: the per-call CPU work is dominated by per-vertex lighting math which is the textbook GPU-compute shape, and threading the wrapper while the body remains CPU would have hit noise floor.
- **Recipe cache / cache-resident port.** Already optimized via Shape C cache (memory: `patchstream_shape_c.md`). Combined cost <1 ms/frame.
- **Visibility check port.** ~0.9 ms/frame. Real cost but per-call work is small (~64 ns); GPU port would be a frustum-cull compute that overlaps with Track C compute cull infrastructure — out of scope here.
- **The `if (!Terrain::terrainTextures2)` legacy branch.** Cold-path in production; removed by an orthogonal cleanup slice if at all.

## Required sub-skills

- `superpowers:using-git-worktrees` — already established (this worktree is `parallel-amdahl`).
- `superpowers:writing-plans` (this skill).
- `adversarial-plan-review` — mandatory final gate before executor session opens (this slice introduces SSBO schemas, new shader files, retires load-bearing CPU code paths, and crosses CPU↔GPU memory model boundaries — all triggers per `.claude/skills/adversarial-plan-review.md`).

## Architectural references (read in order before opening Phase 1)

1. **`memory/water_ssbo_pattern.md`** — the canonical "static recipe + per-frame thin record + single draw post-renderLists" pattern. Phase 2 (water projection port) is structurally identical to renderWater Stage 2 but writes per-vertex projection state instead of submitting draw commands.
2. **`memory/patchstream_shape_c.md`** — Shape C cache-read pattern. Phase 1's SSBO is conceptually similar: per-map-vertex stable data computed once-per-frame on GPU, consumed by CPU readers.
3. **`memory/indirect_terrain_solid_endpoint.md`** — PR1 architecture. Establishes the dense `vertexNum`-indexed SSBO convention and the parity-gate pattern this slice extends.
4. **`memory/mc3_modernization_philosophy.md`** — "GPU/modern by default; at every fork choose GPU." Architectural priors that justify this slice over the threading alternative.
5. **`docs/architecture.md` — coordinate spaces section.** The `wx/wy/wz/ww/clipInfo` semantics that Phase 2 writes must match the CPU `eye->projectForTerrainAdmission()` output exactly.
6. **`memory/cpp_glsl_ubo_struct_lockstep.md`** — load-bearing lesson for any new SSBO/UBO this slice adds.
7. **Existing GLSL compute shader, if any** in `shaders/` — establish house style for compute-shader files.

## Load-bearing constraints

- **GL 4.3 single-context constraint** (memory `2026-05-08-job-system-parallel-for-scope.md` Q5). All GL submission stays on the render thread. Compute dispatches happen before `mcTextureManager->renderLists()` flush so the output SSBO is ready for any CPU consumers that read it back.
- **`TerrainQuad::drawWater` consumes `vertices[i]->wx/wy/wz/ww`** (quad.cpp:2905-2908). Phase 2's SSBO write must complete and be readable before drawWater runs. Either (a) drawWater reads from the SSBO directly (preferred — eliminates the readback), or (b) we glMapBuffer-readback the SSBO into the CPU vertex pool after dispatch but before drawWater. **Spec must commit to (a) or (b) in Stage 0 of Phase 2.**
- **Per-vertex `lightRGB` is consumed by `addTerrainTriangles` and friends** at multiple sites in quad.cpp:880-910 and inside `addTerrainTriangles` itself. Phase 1's lightRGB must be written into the CPU `vertices[i]->lightRGB` field (or those consumers refactored to read from the SSBO). **Stage 1 of Phase 1 verifies this consumer set.**
- **Camera-relative coordinate convention** (`memory/static_prop_projection.md`, MEMORY.md Coordinate Spaces): the compute shader's projection chain must match the CPU `eye->projectForTerrainAdmission` chain exactly (D3D pixel-homogeneous, abs(clip.w) load-bearing per `memory/clip_w_sign_trap.md`). Parity gate per-component-bit-equal is the gate condition.
- **Existing cost-split buckets** (`lighting`, `water_vert_proj`) become **retirement telemetry**: after default-on flip, both buckets must read ~0 µs/frame (the bracket runs but the inner CPU code is gated off behind the killswitch). If post-flip telemetry shows non-zero, the gate didn't catch all sites.

## File structure (Phase 1)

| File | Action | Responsibility |
|---|---|---|
| `mclib/quad.cpp` | Modify | Gate the existing lighting block (lines ~1200-1830) behind `s_terrainLightingGpuEnabled`. When enabled, skip the CPU body and let the SSBO writes from Phase 1's compute shader satisfy downstream consumers. Keep the `CostSplitLightingScope` bracket — it now measures the retirement (should read ~0 µs). |
| `GameOS/gameos/gos_terrain_lighting.h` | Create | Public API: `BuildLightingSSBO()`, `DispatchLightingCompute()`, `MapLightingResult()`, `IsLightingGpuEnabled()`, parity getters. |
| `GameOS/gameos/gos_terrain_lighting.cpp` | Create | Implementation. SSBO lifecycle (alloc, resize on map load, free on destroy). Compute shader program load. Dispatch wiring. Parity comparator (CPU vs GPU per-vertex lightRGB). Env reader for `MC2_TERRAIN_LIGHTING_GPU` + `MC2_TERRAIN_LIGHTING_PARITY`. |
| `shaders/gos_terrain_lighting.comp` | Create | GLSL 4.3 compute shader. `local_size_x = 64`. Input SSBOs (per-vertex state, per-light state, fog/env uniforms). Output SSBO (packed DWORD lightRGB per vertex). |
| `shaders/include/terrain_lighting_shared.hglsl` | Create | Shared struct definitions (PerVertexInput, PerLight, FogParams) to keep CPU↔GPU struct layouts lockstep per `memory/cpp_glsl_ubo_struct_lockstep.md`. |
| `mclib/terrain.cpp` | Modify | Wire `BuildLightingSSBO()` once at mission init; wire `DispatchLightingCompute()` once per frame before the quadSetupTextures loop. |
| `GameOS/gameos/gos_terrain_indirect.h` | Modify | Add env-var reader stubs for `IsLightingGpuEnabled()` if we want to share infrastructure, or keep separate in `gos_terrain_lighting.h`. Decide in Stage 1. |
| `scripts/run_smoke.py` | Modify | Add `MC2_TERRAIN_LIGHTING_GPU`, `MC2_TERRAIN_LIGHTING_PARITY` to the Popen env allowlist (line ~234 area). |
| `tests/smoke/README.md` | Modify | Document the new env vars + the parity-check grep pattern. |

Phase 2 (water projection) adds an analogous set: `gos_terrain_water_proj.{h,cpp}`, `shaders/gos_terrain_water_proj.comp`. Phase 2 reuses the per-vertex input SSBO from Phase 1 (vx/vy/elevation already there) and writes a distinct output SSBO (wx/wy/wz/ww/clipInfo).

---

## Phase 1 — Lighting GPU Compute Port

### Stage 0: Verify consumer set + commit to architectural decisions

**Files:** `mclib/quad.cpp`, `mclib/terrain.cpp`, `GameOS/gameos/gameos_graphics.cpp`, anywhere that reads `vertices[i]->lightRGB` or `vertices[i]->fogRGB`.

- [ ] **Step 1: Grep all readers of `lightRGB` and `fogRGB`.**

```bash
grep -rn '->lightRGB\b' mclib/ code/ GameOS/ --include='*.cpp' --include='*.h'
grep -rn '->fogRGB\b'  mclib/ code/ GameOS/ --include='*.cpp' --include='*.h'
```

Expected: a finite list of consumer sites (probably 5-20 each). Document them in a spec note inline.

- [ ] **Step 2: Decide consumer strategy — readback to CPU vertex pool, or refactor consumers to read SSBO.**

Decision criterion:
- If consumers are all in 1-2 functions reachable from `Terrain::render` → refactor those consumers to read SSBO directly. Skip CPU readback entirely.
- If consumers are scattered across 10+ sites → preserve `vertices[i]->lightRGB` semantics; readback SSBO into CPU vertex pool once per frame after dispatch.

Document the decision in `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md` as the spec sibling to this plan.

- [ ] **Step 3: Grep the per-light data source.**

```bash
grep -n 'getNumTerrainLights\|getTerrainLight\b' mclib/ GameOS/ --include='*.cpp' --include='*.h' -r
```

Identify: how many lights typically exist per frame (mc2_10 wolfman captures should show this; alternatively, add a per-frame `numTerrainLights` print under `MC2_TERRAIN_LIGHTING_GPU_TRACE=1` for one mission run). The compute shader's per-light SSBO needs to size for the worst case.

- [ ] **Step 4: Spec the PerLight SSBO struct.**

Sketch the C++ side:

```cpp
// gos_terrain_lighting.h
struct alignas(16) GpuTerrainLight {
    float position[3];     // 12 B
    uint32_t lightType;    // 4 B — TG_LIGHT_POINT, TG_LIGHT_SPOT, TG_LIGHT_TERRAIN, etc.
    float color[3];        // 12 B (aRGB unpacked to floats 0-1)
    float falloffParam;    // 4 B — single packed param; per-type semantics TBD by GetFalloff()
    // total: 32 B per light — std430 aligned
};
```

And the matching GLSL struct in `shaders/include/terrain_lighting_shared.hglsl`. **Both must be defined in the same header included by both sides** per `memory/cpp_glsl_ubo_struct_lockstep.md`.

- [ ] **Step 5: Spec the PerVertexInput SSBO struct.**

```cpp
struct alignas(16) GpuTerrainVertexInput {
    float vx, vy;            // 8 B
    float elevation;         // 4 B
    float _pad0;             // 4 B
    float normal[3];         // 12 B
    uint32_t flags;          // 4 B — shadow bit, water bit, water&64, water&128, calcThisFrame bits
    // total: 32 B per vertex
};
```

Output struct:

```cpp
struct alignas(4) GpuTerrainLightingOutput {
    uint32_t lightRGB;       // packed BGRA per memory: mc2_argb_packing.md
    uint32_t fogRGB;         // packed BGRA + fog factor
    // total: 8 B per vertex
};
```

For 250K-vertex maps (worst case wolfman): 32 B × 250K = 8 MB input, 8 B × 250K = 2 MB output. Well within SSBO budget.

- [ ] **Step 6: Commit Stage 0 design notes.**

```bash
git add docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md
git commit -m "spec: design notes for terrain lighting GPU compute port (Stage 0)"
```

### Stage 1: Land input SSBO + compute shader (no consumer wiring)

**Files:**
- Create: `GameOS/gameos/gos_terrain_lighting.h`
- Create: `GameOS/gameos/gos_terrain_lighting.cpp`
- Create: `shaders/include/terrain_lighting_shared.hglsl`
- Create: `shaders/gos_terrain_lighting.comp`
- Modify: `mclib/terrain.cpp` (mission-init + per-frame dispatch wiring, output unused)
- Modify: `CMakeLists.txt` (add new GameOS source)

- [ ] **Step 1: Create the shared struct header.**

`shaders/include/terrain_lighting_shared.hglsl`:

```glsl
#ifndef TERRAIN_LIGHTING_SHARED_HGLSL
#define TERRAIN_LIGHTING_SHARED_HGLSL

struct GpuTerrainVertexInput {
    vec2 xy;
    float elevation;
    float _pad0;
    vec3 normal;
    uint flags;
};

struct GpuTerrainLight {
    vec3 position;
    uint lightType;
    vec3 color;
    float falloffParam;
};

struct GpuTerrainLightingOutput {
    uint lightRGB;
    uint fogRGB;
};

#define TG_LIGHT_POINT_GPU   1u
#define TG_LIGHT_SPOT_GPU    2u
#define TG_LIGHT_TERRAIN_GPU 3u

#endif
```

C++ side same struct in `gos_terrain_lighting.h`:

```cpp
#pragma once
#include <cstdint>

namespace gos_terrain_lighting {

struct alignas(16) GpuTerrainVertexInput {
    float xy[2];
    float elevation;
    float _pad0;
    float normal[3];
    uint32_t flags;
};
static_assert(sizeof(GpuTerrainVertexInput) == 32, "vertex input must be 32 B std430");

struct alignas(16) GpuTerrainLight {
    float position[3];
    uint32_t lightType;
    float color[3];
    float falloffParam;
};
static_assert(sizeof(GpuTerrainLight) == 32, "light must be 32 B std430");

struct alignas(4) GpuTerrainLightingOutput {
    uint32_t lightRGB;
    uint32_t fogRGB;
};
static_assert(sizeof(GpuTerrainLightingOutput) == 8, "output must be 8 B std430");

// ... rest of API
}
```

The `static_assert`s guard the lockstep invariant.

- [ ] **Step 2: Create the compute shader skeleton.**

`shaders/gos_terrain_lighting.comp`:

```glsl
// Compiled with "#version 430\n" prefix per CLAUDE.md Critical Rules.
#extension GL_GOOGLE_include_directive : enable
#include "include/terrain_lighting_shared.hglsl"

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer VertexInputs {
    GpuTerrainVertexInput vertices[];
};
layout(std430, binding = 1) readonly buffer LightInputs {
    GpuTerrainLight lights[];
};
layout(std430, binding = 2) writeonly buffer LightingOutputs {
    GpuTerrainLightingOutput outputs[];
};

layout(location = 0) uniform vec3 u_sunLightDir;
layout(location = 1) uniform vec3 u_baseVertexColor;
layout(location = 2) uniform float u_rainLightLevel;
layout(location = 3) uniform uint u_lighteningLevel;
layout(location = 4) uniform float u_fogStart;
layout(location = 5) uniform float u_fogFull;
layout(location = 6) uniform uint u_numVertices;
layout(location = 7) uniform uint u_numLights;

void main() {
    uint vn = gl_GlobalInvocationID.x;
    if (vn >= u_numVertices) return;

    GpuTerrainVertexInput v = vertices[vn];
    // ... per-vertex lighting math mirroring quad.cpp:1200-1771
    // First two lights: sun + ambient baked into the lightR/G/B-from-intensity LUT
    // (mirror eye->getLightRed/Green/Blue).
    // Remaining lights: iterate via lights[] SSBO, accumulate spec*.

    uint packedLight = 0u;
    uint packedFog = 0u;
    // ... pack to BGRA per memory: mc2_argb_packing.md

    outputs[vn].lightRGB = packedLight;
    outputs[vn].fogRGB = packedFog;
}
```

The shader body's per-vertex math must mirror `quad.cpp:1200-1771` (vertex 0..3 lighting blocks) exactly. Don't write it fully in this step — Stage 2 (parity) is the gate that proves the math is right.

- [ ] **Step 3: Implement `gos_terrain_lighting.cpp` skeleton.**

```cpp
#include "gos_terrain_lighting.h"
#include "gos_makeprogram.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>

namespace gos_terrain_lighting {
namespace {
    bool s_enabled = false;
    bool s_parity  = false;
    GLuint s_program = 0;
    GLuint s_vertexInputSsbo = 0;
    GLuint s_lightInputSsbo  = 0;
    GLuint s_outputSsbo      = 0;
    uint32_t s_capacityVertices = 0;
    uint32_t s_capacityLights   = 0;
}

bool IsEnabled() {
    static const bool cached = (getenv("MC2_TERRAIN_LIGHTING_GPU") != nullptr
                                && getenv("MC2_TERRAIN_LIGHTING_GPU")[0] == '1');
    return cached;
}
bool IsParityCheckEnabled() {
    static const bool cached = (getenv("MC2_TERRAIN_LIGHTING_PARITY") != nullptr
                                && getenv("MC2_TERRAIN_LIGHTING_PARITY")[0] == '1');
    return cached;
}

void Init(uint32_t numVertices, uint32_t maxLights) {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    s_capacityVertices = numVertices;
    s_capacityLights   = maxLights;
    glGenBuffers(1, &s_vertexInputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vertexInputSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, numVertices * sizeof(GpuTerrainVertexInput),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &s_lightInputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightInputSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maxLights * sizeof(GpuTerrainLight),
                 nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &s_outputSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_outputSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, numVertices * sizeof(GpuTerrainLightingOutput),
                 nullptr, GL_DYNAMIC_COPY);

    // Compile shader. makeProgram lives in gos_makeprogram.h per CLAUDE.md
    // Critical Rules — pass "#version 430\n" as prefix.
    s_program = gos_make_compute_program("shaders/gos_terrain_lighting.comp",
                                          "#version 430\n");
    if (s_program == 0) {
        fprintf(stderr, "[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail\n");
        fflush(stderr);
        // Disable; legacy CPU path remains live.
        // Sticky disable matches gos_terrain_indirect::ForceDisableArmingForProcess.
    }
}

void Shutdown() {
    if (s_program) { glDeleteProgram(s_program); s_program = 0; }
    if (s_vertexInputSsbo) { glDeleteBuffers(1, &s_vertexInputSsbo); s_vertexInputSsbo = 0; }
    if (s_lightInputSsbo)  { glDeleteBuffers(1, &s_lightInputSsbo);  s_lightInputSsbo  = 0; }
    if (s_outputSsbo)      { glDeleteBuffers(1, &s_outputSsbo);      s_outputSsbo      = 0; }
}

// ... BuildLightingSSBO, Dispatch, MapResult — Stage 2/3
}  // namespace
```

- [ ] **Step 4: Wire dispatch into terrain.cpp (output unused for now).**

In `terrain.cpp` around line 1788 (where `ComputePreflight()` is called):

```cpp
// 1A-alt Phase 1 Stage 1 — dispatch lighting compute; output unused at this stage,
// just proves the shader loads + dispatches without crashing.
gos_terrain_indirect::ComputePreflight();
gos_terrain_lighting::BuildAndDispatch();
```

`BuildAndDispatch` packs current per-vertex state into the input SSBO, packs lights, glDispatchCompute(ceil(N/64), 1, 1), barrier.

- [ ] **Step 5: Add new env vars to the smoke runner allowlist.**

`scripts/run_smoke.py` line ~234 (the existing allowlist):

```python
                            "MC2_TERRAIN_LIGHTING_GPU",
                            "MC2_TERRAIN_LIGHTING_PARITY",
```

- [ ] **Step 6: Build + tier1 smoke (env unset → no behavior change).**

```bash
cd <worktree>
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: tier1 5/5 PASS + menu canary PASS. No `[TERRAIN_LIGHTING_GPU v1]` lines unless `MC2_TERRAIN_LIGHTING_GPU=1`. No GL errors.

- [ ] **Step 7: Run with `MC2_TERRAIN_LIGHTING_GPU=1` (output still unused) on mc2_10.**

```bash
MC2_TERRAIN_LIGHTING_GPU=1 py -3 scripts/run_smoke.py --mission mc2_10 --duration 60 --kill-existing --keep-logs
```

Expected: PASS. The lighting compute dispatches every frame but output is discarded; legacy CPU lighting still runs. No visual diff because we haven't switched the consumer yet.

Watch for `[TERRAIN_LIGHTING_GPU v1] event=dispatch frame=N verts=V lights=L elapsed_us=X` — if absent, dispatch isn't wired. If X is 0 or huge, profile.

- [ ] **Step 8: Commit Stage 1.**

```bash
git add GameOS/gameos/gos_terrain_lighting.h GameOS/gameos/gos_terrain_lighting.cpp \
        shaders/gos_terrain_lighting.comp shaders/include/terrain_lighting_shared.hglsl \
        mclib/terrain.cpp scripts/run_smoke.py CMakeLists.txt
git commit -m "feat(terrain_lighting): Phase 1 Stage 1 — SSBO + compute shader scaffold

- New GameOS/gameos/gos_terrain_lighting.{h,cpp} owns SSBOs + program.
- Compute shader at shaders/gos_terrain_lighting.comp dispatches per-vertex
  but output is unused at this stage; legacy CPU lighting still runs.
- Env gate MC2_TERRAIN_LIGHTING_GPU=1 enables dispatch.
- Tier1 5/5 PASS + menu canary PASS with env unset.
- mc2_10 60s smoke PASS with env set; [TERRAIN_LIGHTING_GPU v1] event=dispatch
  prints emit per frame."
```

### Stage 2: Parity gate

**Files:** Modify `mclib/quad.cpp` to add a parity comparator wrapped around the existing CPU lighting block. The CPU result becomes the reference; GPU SSBO output is compared per-vertex per-frame.

- [ ] **Step 1: Add `Parity_CompareAfterDispatch()` API.**

```cpp
// gos_terrain_lighting.h
void Parity_CompareAfterDispatch(const VertexPtr* vertexArray, uint32_t numVertices);
```

Implementation in `gos_terrain_lighting.cpp`: glMapBufferRange the output SSBO read-only, walk each vertex, compare against `vertexArray[i]->lightRGB`. On mismatch, throttle to 16 prints/frame:

```cpp
fprintf(stderr,
        "[TERRAIN_LIGHTING_PARITY v1] event=mismatch frame=%d vertex=%d "
        "field=lightRGB legacy=0x%08X gpu=0x%08X\n",
        frame, vn, legacy, gpu);
```

End-of-frame summary every 600 frames:

```cpp
fprintf(stderr,
        "[TERRAIN_LIGHTING_PARITY v1] event=summary frames=%lld "
        "verts_checked=%lld total_mismatches=%lld\n",
        frames, verts, mismatches);
```

This mirrors the existing `TERRAIN_INDIRECT_PARITY v1` schema in `gos_terrain_indirect.cpp:242-248`.

- [ ] **Step 2: Wire parity-after-dispatch in terrain.cpp.**

```cpp
gos_terrain_lighting::BuildAndDispatch();
if (gos_terrain_lighting::IsParityCheckEnabled()) {
    // Note: the CPU lighting writes vertices[i]->lightRGB inside setupTextures().
    // To compare, parity must run AFTER the quadSetupTextures loop completes.
    // Hoist the call to terrain.cpp:1820 area (after the for loop, before
    // CostSplit_RollFrame).
}
```

The placement matters: GPU dispatch happens BEFORE the quadSetupTextures loop runs, CPU writes happen DURING the loop, parity comparison must happen AFTER the loop completes.

- [ ] **Step 3: Build, deploy, run mc2_10 with `MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1`.**

```bash
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1 \
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 90 --kill-existing --keep-logs
```

Expected output: `[TERRAIN_LIGHTING_PARITY v1] event=summary ... total_mismatches=N` — N will likely be NON-ZERO on first try (the shader body in Stage 1 was sketched, not perfect). Each mismatch print pinpoints the diverging vertex.

- [ ] **Step 4: Iterate shader math until parity is clean.**

For each mismatch class:
- Identify which uniform / SSBO field is wrong (the print includes legacy and GPU values — diff the bits).
- Common failure modes (from `memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`): wrong sRGB convention, wrong matrix mul order, missed clamp, fp16/fp32 precision drift.
- Re-run until summary shows `total_mismatches=0` across 5 missions × 90s = ~27,000 frames × 250K verts = ~6.75B field comparisons.

- [ ] **Step 5: Commit Stage 2 once parity is clean.**

```bash
git commit -m "feat(terrain_lighting): Phase 1 Stage 2 — parity gate clean

- Per-vertex lightRGB comparator wraps the GPU output vs CPU vertices[i]->lightRGB.
- tier1 5/5 + mc2_10 90s soak: zero mismatches across ~6.75B field comparisons.
- Schema matches TERRAIN_INDIRECT_PARITY v1 — [TERRAIN_LIGHTING_PARITY v1]
  event=mismatch lines + 600-frame event=summary cadence.
- Legacy CPU lighting still authoritative; GPU output discarded after parity."
```

### Stage 3: Consumer switch (GPU output becomes authoritative; CPU still runs as parity reference)

**Files:** Modify `mclib/quad.cpp` to gate the CPU lighting block. Modify the post-loop wiring to copy SSBO output into `vertices[i]->lightRGB` BEFORE drawWater runs.

- [ ] **Step 1: Add consumer-readback step after dispatch.**

If Stage 0 decided strategy (b) (CPU readback):

```cpp
// gos_terrain_lighting.cpp
void MapAndCopyToVertices(VertexPtr* vertexArray, uint32_t numVertices) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_outputSsbo);
    auto* mapped = (const GpuTerrainLightingOutput*)glMapBufferRange(
        GL_SHADER_STORAGE_BUFFER, 0, numVertices * sizeof(GpuTerrainLightingOutput),
        GL_MAP_READ_BIT);
    for (uint32_t i = 0; i < numVertices; ++i) {
        vertexArray[i]->lightRGB = mapped[i].lightRGB;
        vertexArray[i]->fogRGB   = mapped[i].fogRGB;
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
}
```

**Hazard:** glMapBufferRange + GL_MAP_READ_BIT after a compute write is a sync stall — see `memory/substrate_coalesce_sync_point_lesson.md` (the renderWater Stage 2 stall lesson). Mitigation: use a persistent-mapped pipelined buffer with N-frame latency, OR readback into a pinned host buffer via glGetBufferSubData on a previous frame's result. Spec must commit to a non-stalling pattern.

If Stage 0 decided strategy (a) (SSBO consumer refactor):

Refactor `addTerrainTriangles`, `pz_emit_terrain_tris`, drawWater, etc., to read `lightRGB` via a thin accessor that pulls from the SSBO when `IsEnabled()` is true.

- [ ] **Step 2: Gate the CPU lighting block in `quad.cpp:1200-1830`.**

```cpp
// 1A-alt Phase 1 Stage 3 — gate off legacy CPU lighting block when GPU path
// is authoritative. Parity check (when on) still requires legacy to run — so
// the gate is gpu_authoritative = IsEnabled() && !IsParityCheckEnabled().
const bool s_lightingGpuAuth = gos_terrain_lighting::IsEnabled()
                                && !gos_terrain_lighting::IsParityCheckEnabled();
{
    CostSplitLightingScope _csLight;
    if (s_lightingGpuAuth) {
        // Skip — GPU authoritative.
    } else if (terrainHandle != 0xffffffff) {
        // ... existing CPU lighting body ...
    }
}
```

- [ ] **Step 3: Build, deploy, run mc2_10 with `MC2_TERRAIN_LIGHTING_GPU=1` (no parity flag — gate is now authoritative).**

```bash
MC2_TERRAIN_LIGHTING_GPU=1 \
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 90 --kill-existing --keep-logs
```

Expected: PASS. Visual: identical to legacy (Stage 2 proved bit-equality). Cost-split summary `lighting_ns_per_frame` should drop to ~0 µs (the bracket runs but the body is skipped).

- [ ] **Step 4: Re-run with parity on, confirm zero mismatches under default-on gate path.**

```bash
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
```

Both paths run, parity must still be zero across all 5 tier1 missions.

- [ ] **Step 5: Commit Stage 3.**

```bash
git commit -m "feat(terrain_lighting): Phase 1 Stage 3 — GPU path authoritative

- gpu_authoritative = IsEnabled() && !IsParityCheckEnabled().
- Legacy CPU lighting block at quad.cpp:1200-1830 gates off when authoritative.
- lighting_ns_per_frame cost-split bucket reads ~0 µs under gate.
- tier1 5/5 + mc2_10 PASS under gate; parity (when on) zero mismatches.
- Killswitch MC2_TERRAIN_LIGHTING_GPU=0 restores legacy."
```

### Stage 4: Soak

- [ ] **Step 1: Set up 7-day soak window per Track B precedent.**

The soak window is the user's responsibility — they need to run with `MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1` across their normal gameplay sessions and not see any visual regressions or parity mismatches. Document the soak schedule in `memory/terrain_lighting_gpu_soak.md`.

- [ ] **Step 2: Define soak abort criteria.**

- Any visual regression vs legacy (screenshots optional but recommended).
- Any non-zero parity mismatches.
- Any GL errors (`[GL_ERROR v1]` lines).
- Any FPS regression vs Stage 3 baseline (cost-split should show the lighting bucket retired, so FPS should match or improve).

If any abort condition fires: revert to Stage 2 (parity-only mode), bisect.

### Stage 5: Default-on flip

- [ ] **Step 1: Flip `IsEnabled()` semantics to default-on per Track B precedent.**

```cpp
// gos_terrain_lighting.cpp
bool IsEnabled() {
    // Default ON post Stage 5 flip. Explicit "0" still opts out for bisection;
    // any other value (including unset) opts in.
    static const bool cached = [] {
        const char* env = getenv("MC2_TERRAIN_LIGHTING_GPU");
        return (env == nullptr) || (env[0] != '0');
    }();
    return cached;
}
```

- [ ] **Step 2: Build, deploy, tier1 5/5 + menu canary + mc2_10 90s smoke clean.**

- [ ] **Step 3: Commit Stage 5 + update `memory/terrain_lighting_gpu_soak.md` with flip date.**

### Stage 6: CPU code demotion

Per CLAUDE.md "Debug Instrumentation Rule" — keep the legacy CPU lighting code in the tree gated off, do not delete. The bracket and the CostSplitLightingScope remain as retirement telemetry (must read ~0 µs in production builds).

- [ ] **Step 1: Add a sanity assertion: lighting_ns_per_frame should be < 50 µs in production.**

If it isn't, some code path is still hitting the CPU body — investigate.

---

## Phase 2 — Water Vertex Projection GPU Compute Port

Outline only — full spec deferred until Phase 1 ships and the architectural patterns are proven on lighting.

**Goal:** Retire the ~0.91 ms/frame water-vertex projection block at `quad.cpp:887-1198`.

**Structural notes:**
- Reuses Phase 1's per-vertex input SSBO (vx/vy/elevation already there). Adds water-flag-bit reading.
- Output: separate SSBO of `wx/wy/wz/ww/clipInfo + calcThisFrame` per water-eligible vertex.
- Consumer: `TerrainQuad::drawWater` at quad.cpp:2905-2908 reads from the SSBO directly (preferred — eliminates CPU readback) OR readback into vertex pool (fallback per Phase 1 Stage 0 strategy choice).
- Reductions (`leastZ`/`mostZ`/`leastW`/`mostW`/`leastWY`/`mostWY`): GPU atomic-min/max into a small reduction buffer, OR CPU reduce-pass over the output SSBO (small N — water-eligible vertices only).
- Parity: same shape as Phase 1.
- Killswitch: `MC2_TERRAIN_WATER_PROJ_GPU=1`.
- Stages 0-6 mirror Phase 1 structure exactly.

**Pre-Phase-2 architectural decision (deferred from Phase 1):** Whether to refactor `TerrainQuad::drawWater` to read from SSBO. If yes, Phase 2 fully retires the per-vertex shared-write race surface (D3 from Slice 0 Phase 0 verification appendix) by removing the CPU writes entirely. If no, Phase 2 saves the projection cost but retains the shared-vertex write surface for any future threading work.

---

## Killswitches + env vars

| Var | Purpose | Default |
|---|---|---|
| `MC2_TERRAIN_LIGHTING_GPU` | Phase 1 main gate | off until Stage 5 flip; default-on after |
| `MC2_TERRAIN_LIGHTING_PARITY` | Phase 1 parity check (forces both paths to run + compare) | off |
| `MC2_TERRAIN_LIGHTING_GPU_TRACE` | Phase 1 per-frame `[TERRAIN_LIGHTING_GPU v1] event=dispatch ...` print | off |
| `MC2_TERRAIN_WATER_PROJ_GPU` | Phase 2 main gate | off until Phase 2 Stage 5 |
| `MC2_TERRAIN_WATER_PROJ_PARITY` | Phase 2 parity | off |
| `MC2_TERRAIN_COST_SPLIT` | already exists; retains existing semantics | off |

## Parity / Soak gates

- **Phase 1 Stage 2:** zero parity mismatches across tier1 5/5 + Carver5O + Magic canary, ~6.75B field-comparisons.
- **Phase 1 Stage 3:** `lighting_ns_per_frame` cost-split bucket reads <50 µs under `MC2_TERRAIN_LIGHTING_GPU=1`.
- **Phase 1 Stage 4 soak:** 7-day window. Zero parity mismatches under `_PARITY=1`. No GL errors. No visual regressions.
- **Phase 1 perf gate:** Tracy zone `Terrain::geometry quadSetupTextures` at mc2_10 wolfman — pre-Phase-1 11.3 ms mean → post-Phase-1 target ≤6.5 ms mean (4.8 ms cut from lighting retirement, less than the 5.18 ms bucket because some dispatch overhead remains).

## Stop conditions

- **Phase 1 Stage 2 parity won't go to zero after 3 iteration rounds → STOP.** Either the CPU math is non-reproducible (e.g. depends on uninitialized state) or the GPU math has a subtle convention mismatch. Surface to user with the divergence pattern. Do NOT proceed to Stage 3 with non-zero parity.
- **Phase 1 Stage 3 GL_INVALID_OPERATION on dispatch → STOP, bisect.** Likely SSBO binding mismatch or shader resource declaration mismatch.
- **Phase 1 Stage 5 default-on flip causes any tier1 mission to FAIL → STOP, revert flip to Stage 2 mode.**
- **Phase 1 perf gate misses (cut < 3 ms at mc2_10 wolfman) → STOP, surface to user.** Likely the consumer-readback strategy from Stage 0 chose poorly; revisit (refactor consumers to read SSBO directly).

## Adversarial review gate (mandatory)

Before opening Phase 1 Stage 1: run `adversarial-plan-review` skill against this plan. This slice qualifies under the skill's "Mandatory" trigger list:
- Adds two new SSBO schemas (vertex input, light input, lighting output, plus Phase 2 water-projection output).
- Adds two new compute shader programs.
- Retires the per-vertex CPU lighting and water-projection paths (load-bearing for `vertices[i]->lightRGB` consumers + `drawWater` `wx/wy/wz/ww` consumers).
- Crosses CPU↔GPU memory model boundary; introduces sync-stall hazards per `memory/substrate_coalesce_sync_point_lesson.md`.
- Perf gate ≥30% on the target Tracy zone.

The skill dispatch prompt MUST include "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

## Exit criteria

- Phase 1 Stages 0-6 complete; default-on flip survives the soak window.
- `memory/terrain_lighting_gpu.md` (or similar) captures the canonical GPU compute port pattern for the codebase, for Phase 2 and future slices to reuse.
- The Slice 0 recon cost-split buckets (`lighting`, `water_vert_proj`) retain their place in the summary line as retirement telemetry (must read ~0 µs in production).
- Phase 2 spec follow-up, gated on Phase 1 patterns proving out.

## Open questions for the spec session

1. **Consumer strategy** (Stage 0 Step 2): readback or refactor? The data backing this decision is in the grep of `->lightRGB`/`->fogRGB` consumers — that grep must run first.
2. **Per-light SSBO size**: max lights per frame at mc2_10 wolfman. Needs one diagnostic-mission capture.
3. **Sync-stall avoidance**: persistent-mapped pipelined buffer, fence-and-wait, or skip-frame latency? The renderWater Stage 2 sync-fix memory file is the load-bearing reference.
4. **Phase 2 drawWater refactor**: in scope or deferred? If in scope, Phase 2 also retires D3 (shared-vertex write race surface).

These open questions are resolved in the spec design doc sibling (Stage 0 of Phase 1).
