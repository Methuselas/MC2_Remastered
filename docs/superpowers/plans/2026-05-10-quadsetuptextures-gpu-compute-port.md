# `quadSetupTextures` GPU-Compute Port — Implementation Plan v2

> **Design doc (authoritative):** [`docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md`](../specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md) (commit `ac7c492`)
>
> **Revision history:** Plan v1 (`d2424ef`) failed adversarial review with 3 CRITICAL + 7 MAJOR findings (systematic line-number drift, fictional API names, unresolved sync/consumer/lifecycle choices). The design doc resolved all 7 open questions through 3 rounds of adversarial review (`63015e2` → `a5fd168` → `f1a37a3` → `ac7c492`). This plan v2 references the design doc for every architectural decision; no unresolved choices remain here.
>
> **For agentic workers:** Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement task-by-task. Steps use `- [ ]` syntax for tracking.

**Goal:** Replace the dominant CPU cost inside `Terrain::geometry quadSetupTextures` with two compute-shader passes feeding the existing per-vertex CPU state via SSBO, retiring ~6 ms/frame from the CPU floor.

**Recon foundation:** Slice 0 cost-split (mc2_10 wolfman, frame-3000 steady state):

| Bucket | µs/frame | % of setup_total |
|---|---:|---:|
| setup_total | 11,871 | 100% |
| **lighting** | **5,177** | **43.6%** (Phase 1 target) |
| **water_vert_proj** | **911** | **7.7%** (Phase 2 target) |
| visibility_check | 890 | 7.5% |
| recipe_cache | 656 | 5.5% |
| residual | 3,767 | 31.7% |

**Architecture:** Per design doc Q2 — CPU readback via 3-slot non-blocking persistent-mapped SSBO ring. GPU writes `lightRGB`/`fogRGB` into the compute output SSBO; staging ring copies into `vertices[i]->lightRGB`/`fogRGB` per-frame via non-blocking `tryConsume` (1-frame pipelined latency). Consumers (`gos_terrain_water_stream`, `gos_terrain_indirect`, `gos_terrain_patch_stream` indirect) inherit that 1-frame latency.

---

## Out of scope

- Threading / parallel-for — GPU compute supplants threading for this work.
- Recipe cache / cache-resident port — already optimized via Shape C (`memory/patchstream_shape_c.md`), combined <1 ms/frame.
- Visibility check port — overlaps with Track C compute cull infrastructure; separate slice.
- `if (!Terrain::terrainTextures2)` legacy branch — cold path; orthogonal cleanup slice.

---

## Architectural references (read before opening Phase 1)

1. **Design doc** `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md` (commit `ac7c492`) — authoritative for all Q1–Q7 decisions. Read this first.
2. **`memory/water_ssbo_pattern.md`** — canonical "static recipe + per-frame thin record" pattern; `mission_init` + `PackAndDispatch` implement this shape.
3. **`memory/substrate_coalesce_sync_point_lesson.md`** — why `glGetBufferSubData` / `glMapBufferRange(GL_MAP_READ_BIT)` is banned on the hot path. The 3-slot ring from Q2 is the answer.
4. **`memory/cpp_glsl_ubo_struct_lockstep.md`** — extending C++ SSBO structs without matching GLSL declaration corrupts per-element stride; the `static_assert` guards in Stage 1 enforce this.
5. **`memory/mc2_argb_packing.md`** — `lightRGB`/`fogRGB` are BGRA in memory; compute shader must pack accordingly.
6. **`memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`** — parity gate is not optional; visual smoke passes while SSBO math is wrong.

---

## Load-bearing constraints

- **GL 4.3 single-context** (`docs/superpowers/specs/2026-05-08-job-system-parallel-for-scope.md` Q5). All GL calls stay on the render thread. Compute dispatches in `PackAndDispatch` happen inside `Terrain::geometry` at `terrain.cpp:1783` zone, before `mcTextureManager->renderLists()` flush.
- **No `glMapBufferRange(GL_MAP_READ_BIT)` on the hot path** (per `memory/substrate_coalesce_sync_point_lesson.md`). The 3-slot persistent-mapped staging ring (design doc Q2, modeled on `gpu_cull_readback.cpp`) is the only compliant readback pattern.
- **`vertices[i]->lightRGB` / `->fogRGB` consumers** (design doc Q2 consumer table): `mclib/quad.cpp` (direct), `GameOS/gameos/gos_terrain_water_stream.cpp` (CPU mirror), `GameOS/gameos/gos_terrain_indirect.cpp` (CPU mirror at `:1459`), `GameOS/gameos/gos_terrain_patch_stream.cpp` (indirect via `quad.cpp:2133-2136` lightRGBc lambda). All inherit 1-frame pipelined latency when Phase 1 is active.
- **`calcThisFrame & 1` dedupe** (design doc Q3): the CPU lighting block gates on `!(vertices[0]->calcThisFrame & 1)` at `quad.cpp:~1276`; the compute shader writes all slots `[0, realVerticesMapSide²)` unconditionally; the parity comparator filters by `calcThisFrame & 1`.
- **`realVerticesMapSide` has no const cap** (design doc Q3): SSBO sizes are allocated dynamically at `mission_init` with `numVertices = realVerticesMapSide * realVerticesMapSide`; no compile-time fixed array.
- **Per-mission lifecycle hooks required** (design doc Q5): process-lifetime init/shutdown does NOT work here. Call `mission_init` alongside `gpu_cull::compute_init()` at `code/mission.cpp:2788`; call `mission_shutdown` from `Terrain::destroy` at `mclib/terrain.cpp:703`.

---

## SSBO struct layout (committed — design doc Q6)

```cpp
// gos_terrain_lighting.h — Phase 1 reads bits 0-3; Phase 2 reads bits 4-7.
#define GPU_VERT_SHADOW          0x00000001u  // pVertex->shadow != 0
#define GPU_VERT_CALCFRAME_LIGHT 0x00000002u  // calcThisFrame & 1
#define GPU_VERT_BASE_COLOR_LIT  0x00000004u  // BaseVertexColor != 0
#define GPU_VERT_RAIN_DAMPEN     0x00000008u  // rainLightLevel < 1.0f
#define GPU_VERT_WATER           0x00000010u  // pVertex->water & 1
#define GPU_VERT_WATER_ANIM_NEG  0x00000020u  // water & 128
#define GPU_VERT_WATER_ANIM_POS  0x00000040u  // water & 64
#define GPU_VERT_CALCFRAME_WATER 0x00000080u  // calcThisFrame & 2

struct alignas(16) GpuTerrainVertexInput {  // 32 B std430
    float xy[2];         // 8 B @ offset 0
    float elevation;     // 4 B @ offset 8
    float _pad0;         // 4 B @ offset 12 (pads normal to 16-byte boundary)
    float normal[3];     // 12 B @ offset 16
    uint32_t flags;      // 4 B @ offset 28
};
// static_assert(sizeof(GpuTerrainVertexInput) == 32, "must be 32 B std430");

struct alignas(16) GpuTerrainLight {  // 32 B std430
    float position[3];   // 12 B @ offset 0
    uint32_t lightType;  // 4 B @ offset 12
    float color[3];      // 12 B @ offset 16
    float falloffParam;  // 4 B @ offset 28
};

struct alignas(4) GpuTerrainLightingOutput {  // 8 B std430
    uint32_t lightRGB;   // packed BGRA per memory/mc2_argb_packing.md
    uint32_t fogRGB;
};
```

Phase 1 ships the full 8-bit flag layout including Phase 2's bits 4-7 (design doc Q6). Phase 2 reads them without struct churn.

---

## File structure (Phase 1)

| File | Action |
|---|---|
| `GameOS/gameos/gos_terrain_lighting.h` | Create — public API: `mission_init`, `mission_shutdown`, `BeginFrame`, `PackAndDispatch`, `CopyResultsToVertexPool`, `IsEnabled`, `IsParityCheckEnabled` |
| `GameOS/gameos/gos_terrain_lighting.cpp` | Create — 3-slot ring lifecycle, `tl_*` compute helpers, `mission_init`/`mission_shutdown`, per-frame trio, parity comparator |
| `shaders/gos_terrain_lighting.comp` | Create — GLSL 4.3, `local_size_x = 64`, bindings 0/1/2 (vertex input / light input / output) |
| `shaders/include/terrain_lighting_shared.hglsl` | Create — matching GLSL struct declarations (lockstep per `memory/cpp_glsl_ubo_struct_lockstep.md`) |
| `mclib/terrain.cpp` | Modify — wire `mission_init` at `terrain.cpp:595` area, `mission_shutdown` at `terrain.cpp:703`, per-frame trio before setupTextures loop at `terrain.cpp:1783` |
| `code/mission.cpp` | Modify — call `gos_terrain_lighting::mission_init()` at `mission.cpp:2788` alongside `gpu_cull::compute_init()` |
| `mclib/quad.cpp` | Modify (Stage 3) — gate lighting block `1266-1891` behind `s_lightingGpuAuth` |
| `CMakeLists.txt` | Modify — add `gos_terrain_lighting.cpp` to GameOS sources |
| `scripts/run_smoke.py` | Modify — add `MC2_TERRAIN_LIGHTING_GPU`, `MC2_TERRAIN_LIGHTING_PARITY`, `MC2_TERRAIN_LIGHTING_TRACE` to env allowlist (after line 256 area, alongside existing `MC2_TERRAIN_INDIRECT_*` entries) |

Phase 2 adds: `gos_terrain_water_proj.{h,cpp}`, `shaders/gos_terrain_water_proj.comp`. Reuses Phase 1's `GpuTerrainVertexInput` SSBO (water flags already in bits 4-7). Phase 2 design doc is separate, written after Phase 1 ships.

---

## Killswitches + env vars

| Var | Purpose | Default |
|---|---|---|
| `MC2_TERRAIN_LIGHTING_GPU` | Phase 1 main gate | off until Stage 5 flip; default-on after |
| `MC2_TERRAIN_LIGHTING_PARITY` | Phase 1 parity check (both paths run + compare) | off |
| `MC2_TERRAIN_LIGHTING_TRACE` | Per-frame `[TERRAIN_LIGHTING_GPU v1] event=dispatch ...` print | off |
| `MC2_TERRAIN_WATER_PROJ_GPU` | Phase 2 main gate | off until Phase 2 Stage 5 |
| `MC2_TERRAIN_WATER_PROJ_PARITY` | Phase 2 parity | off |
| `MC2_TERRAIN_COST_SPLIT` | Already exists; retains existing semantics | off |

---

## Phase 1 — Lighting GPU Compute Port

### Stage 0: Design committed (no executor work required here)

All Stage 0 architectural decisions are resolved in the design doc (commit `ac7c492`). The executor does not need to re-run Stage 0 recon. Key committed answers:

- **Consumer strategy** → CPU readback via 3-slot non-blocking ring (design doc Q2)
- **SSBO struct layout** → committed in Q6 above; shipped full 8-bit flag pack
- **Lifecycle hooks** → `mission_init`/`mission_shutdown` + per-frame trio (design doc Q5)
- **Sync pattern** → `gpu_cull_readback.cpp` 3-slot non-blocking `tryConsume` (design doc Q2)
- **Parity API** → `Parity_CompareFrame(quadList, numberQuads, mappedOutput)` (design doc Q3)
- **Perf gate** → two-stage empirical (design doc Q7); see Stage 1 and Stage 3 gates below

### Stage 1: Input SSBO + compute scaffold + 3-slot ring (output unused)

**Files:** Create `gos_terrain_lighting.{h,cpp}`, `shaders/gos_terrain_lighting.comp`, `shaders/include/terrain_lighting_shared.hglsl`; modify `mclib/terrain.cpp`, `code/mission.cpp`, `CMakeLists.txt`, `scripts/run_smoke.py`.

- [ ] **Step 1: Create `shaders/include/terrain_lighting_shared.hglsl`.**

GLSL side of the lockstep structs (matching the C++ layout in `gos_terrain_lighting.h`):

```glsl
#ifndef TERRAIN_LIGHTING_SHARED_HGLSL
#define TERRAIN_LIGHTING_SHARED_HGLSL
// Keep in lockstep with gos_terrain_lighting.h — any extension here must
// extend the C++ struct too (memory/cpp_glsl_ubo_struct_lockstep.md).

struct GpuTerrainVertexInput { vec2 xy; float elevation; float _pad0; vec3 normal; uint flags; };
struct GpuTerrainLight       { vec3 position; uint lightType; vec3 color; float falloffParam; };
struct GpuTerrainLightingOutput { uint lightRGB; uint fogRGB; };

#define TG_LIGHT_POINT_GPU   1u
#define TG_LIGHT_SPOT_GPU    2u
#define TG_LIGHT_TERRAIN_GPU 3u

// Flag bits (bits 0-3 Phase 1, bits 4-7 Phase 2 — design doc Q6)
#define GPU_VERT_SHADOW          0x00000001u
#define GPU_VERT_CALCFRAME_LIGHT 0x00000002u
#define GPU_VERT_BASE_COLOR_LIT  0x00000004u
#define GPU_VERT_RAIN_DAMPEN     0x00000008u
#define GPU_VERT_WATER           0x00000010u
#define GPU_VERT_WATER_ANIM_NEG  0x00000020u
#define GPU_VERT_WATER_ANIM_POS  0x00000040u
#define GPU_VERT_CALCFRAME_WATER 0x00000080u
#endif
```

- [ ] **Step 2: Create `gos_terrain_lighting.h` with committed public API (design doc Q5).**

```cpp
#pragma once
#include <cstdint>

class TerrainQuad; // forward-decl; full def in mclib/quad.h:59

namespace gos_terrain_lighting {

// --- Committed SSBO struct layout (design doc Q6) ---
struct alignas(16) GpuTerrainVertexInput  { float xy[2]; float elevation; float _pad0;
                                            float normal[3]; uint32_t flags; };
struct alignas(16) GpuTerrainLight        { float position[3]; uint32_t lightType;
                                            float color[3]; float falloffParam; };
struct alignas(4)  GpuTerrainLightingOutput { uint32_t lightRGB; uint32_t fogRGB; };

static_assert(sizeof(GpuTerrainVertexInput)   == 32, "32 B std430");
static_assert(sizeof(GpuTerrainLight)         == 32, "32 B std430");
static_assert(sizeof(GpuTerrainLightingOutput) ==  8,  "8 B std430");

// --- Lifecycle (design doc Q5) ---
// Per-mission: allocate SSBOs sized to numVertices = realVerticesMapSide²,
//   compile shader on first call, reset ring.
// Call from code/mission.cpp:2788 alongside gpu_cull::compute_init().
void mission_init(uint32_t numVertices, uint32_t maxLights);

// Per-mission teardown: zero CPU state; keep GL allocs for reuse.
// Call from Terrain::destroy (terrain.cpp:703).
void mission_shutdown();

void BeginFrame();                                    // per-frame: advance ring index
void PackAndDispatch();                               // per-frame: pack input SSBO, dispatch, barrier
void CopyResultsToVertexPool(TerrainQuad* quadList,
                             int numberQuads);        // per-frame: tryConsume ring, copy into vertices[i]

bool IsEnabled();
bool IsParityCheckEnabled();

// Parity API (design doc Q3)
// Comparator walks quadList, filters by calcThisFrame & 1, indexes outputs[vertexNum].
void Parity_CompareFrame(TerrainQuad* quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput);

} // namespace gos_terrain_lighting
```

- [ ] **Step 3: Create `gos_terrain_lighting.cpp` — `tl_*` compile helpers + lifecycle.**

Per design doc Q1: copy the `gpu_cull_compute.cpp:145-231` private-static compile pattern into this module as `tl_compile_compute_shader` / `tl_link_compute_program` / `tl_build_compute_program_from_file`. Prefix `tl_` (terrain_lighting) avoids ODR collisions since helpers are `static`. Do NOT create a shared header — factoring is deferred until a third compute module appears (design doc Q1 rationale).

3-slot ring matching `gpu_cull_readback.cpp:40` (`RING_FRAMES = 3u`, `GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` on staging buffers — design doc Q2):

```cpp
static constexpr uint32_t RING_FRAMES = 3u;  // matches gpu_cull_readback.cpp:40

// GPU-side SSBO: compute writes here (no persistent map needed)
static GLuint s_computeOutputSsbo = 0;
// 3-slot staging ring: persistent-mapped BAR, CPU reads (design doc Q2 Q+A)
static GLuint   s_stagingRing[RING_FRAMES] = {};
static void*    s_stagingMapped[RING_FRAMES] = {};
static GLsync   s_stagingFence[RING_FRAMES] = {};
static uint32_t s_currentSlot = 0;
```

`tryConsume` follows the T1/T2/T3 non-blocking pattern (design doc Q2): T1 = N-1 slot fence signaled, T2 = N-2 signaled (emit `terrain_light_fallback_n2` counter), T3 = skip update (emit `terrain_light_fallback_conservative` counter). **`glClientWaitSync` timeout is ALWAYS 0 on the hot path** — `GL_TIMEOUT_IGNORED` only in the parity mode's synchronous stall and in `mission_shutdown` teardown.

Log at lifecycle boundaries (not per-frame) per CLAUDE.md Debug Instrumentation Rule:
```
[TERRAIN_LIGHTING_GPU v1] event=mission_init verts=N lights=M
[TERRAIN_LIGHTING_GPU v1] event=shader_load_fail       // sticky-disable on failure
[TERRAIN_LIGHTING_GPU v1] event=mission_shutdown
```
Per-frame dispatch details under `MC2_TERRAIN_LIGHTING_TRACE=1` only.

- [ ] **Step 4: Create `shaders/gos_terrain_lighting.comp` skeleton.**

```glsl
// No #version — prepended as "#version 430\n" by tl_build_compute_program_from_file
//   per CLAUDE.md Critical Rules.
#extension GL_GOOGLE_include_directive : enable
#include "include/terrain_lighting_shared.hglsl"

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer VertexInputs  { GpuTerrainVertexInput vertices[]; };
layout(std430, binding = 1) readonly buffer LightInputs   { GpuTerrainLight lights[]; };
layout(std430, binding = 2) writeonly buffer LightOutputs { GpuTerrainLightingOutput outputs[]; };

layout(location = 0) uniform uint  u_numVertices;
layout(location = 1) uniform uint  u_numLights;
layout(location = 2) uniform vec3  u_sunLightDir;
layout(location = 3) uniform vec3  u_baseVertexColor;
layout(location = 4) uniform float u_rainLightLevel;
layout(location = 5) uniform uint  u_lighteningLevel;
layout(location = 6) uniform float u_fogStart;
layout(location = 7) uniform float u_fogFull;

void main() {
    uint vn = gl_GlobalInvocationID.x;
    if (vn >= u_numVertices) return;
    // Lighting math mirrors quad.cpp:1266-1891 (per-vertex lighting block).
    // TG_Light::GetFalloff formula: tgl.h:261-275. Two-arg: (float length, float &falloff).
    // GetFalloff called at quad.cpp:1347/1500/1654/1808 (per-vertex sites).
    // Pack lightRGB/fogRGB BGRA per memory/mc2_argb_packing.md.
    outputs[vn].lightRGB = 0u;  // placeholder until Stage 2 math iteration
    outputs[vn].fogRGB   = 0u;
}
```

The shader body is deliberately skeletal here — Stage 2 (parity gate) is the gate that proves math correctness. Parity mismatches in Stage 2 are the feedback loop for iterating the math.

- [ ] **Step 5: Wire per-mission lifecycle in `code/mission.cpp` and `mclib/terrain.cpp`.**

In `code/mission.cpp:2788` (alongside `gpu_cull::compute_init()`):
```cpp
// CRITICAL: at mission.cpp:2788, land->getNumVertices() returns 0 — numberVertices
// is not set until Terrain::update() runs each frame (terrain.cpp:893). Use the
// map-stable dense bound instead. Terrain::realVerticesMapSide (terrain.h:135,
// static long) is set during land->init() at mission.cpp:2222 — before line 2788.
gos_terrain_lighting::mission_init(
    (uint32_t)(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
    64u);                               // maxLights — tune after diagnostic
```

In `mclib/terrain.cpp:703` (`Terrain::destroy`):
```cpp
gos_terrain_lighting::mission_shutdown();
```

- [ ] **Step 6: Wire per-frame trio in `mclib/terrain.cpp:1783` zone (before setupTextures loop).**

Per design doc Q5 call-site spec — insert immediately after `gos_terrain_indirect::ComputePreflight()` at `terrain.cpp:1788`:

```cpp
// Phase 1: terrain lighting GPU compute — design doc Q5 wiring
gos_terrain_lighting::BeginFrame();
gos_terrain_lighting::PackAndDispatch();
gos_terrain_lighting::CopyResultsToVertexPool(quadList, numberQuads);
```

`CopyResultsToVertexPool` runs `tryConsume` (non-blocking T1/T2/T3). T3 (stale) retains prior values silently. At Stage 1, `CopyResultsToVertexPool` is a no-op stub (output unused) — the CPU body still runs.

- [ ] **Step 7: Add env vars to smoke runner allowlist.**

In `scripts/run_smoke.py` after line 256 (alongside `MC2_TERRAIN_INDIRECT_*` entries):
```python
"MC2_TERRAIN_LIGHTING_GPU",
"MC2_TERRAIN_LIGHTING_PARITY",
"MC2_TERRAIN_LIGHTING_TRACE",
```

- [ ] **Step 8: Add `gos_terrain_lighting.cpp` to `CMakeLists.txt` GameOS sources.**

- [ ] **Step 9: Build + tier1 smoke (env unset — no behavior change).**

```
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: tier1 5/5 PASS + menu canary PASS. No `[TERRAIN_LIGHTING_GPU v1]` lines unless `MC2_TERRAIN_LIGHTING_GPU=1`. No GL errors.

- [ ] **Step 10: Run with `MC2_TERRAIN_LIGHTING_GPU=1` on mc2_10 (dispatch smoke).**

```
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_TRACE=1 ^
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 60 --kill-existing --keep-logs
```

Expected: PASS. Legacy CPU lighting still runs (output unused). Verify `[TERRAIN_LIGHTING_GPU v1] event=dispatch` lines appear. If absent, dispatch not wired. If `event=shader_load_fail`, fix compile error.

**Stage 1 dispatch gate (design doc Q7):** Measure Tracy zone `Terrain::TerrainLightingDispatch` (NEW — add inside `PackAndDispatch`). Target: **≤500 µs/frame** at mc2_10 wolfman steady state. If ≥1 ms, the input SSBO pack loop is too expensive — consider persistent-mapping the input SSBO too.

Record the Stage 1 baseline in the commit message (e.g. `dispatch_overhead = N µs/frame`).

- [ ] **Step 11: Commit Stage 1.**

```
feat(terrain_lighting): Phase 1 Stage 1 — SSBO scaffold + 3-slot ring
  (design doc Q1-Q6, plan v2 — d2424ef adversarial review fixed)

- gos_terrain_lighting.{h,cpp}: mission_init/shutdown + 3-slot non-blocking
  ring (RING_FRAMES=3, matches gpu_cull_readback.cpp:40 precedent).
- tl_compile_compute_shader/tl_link_compute_program/tl_build_compute_program_from_file
  copied privately from gpu_cull_compute.cpp:145-231 (design doc Q1).
- Compute shader shaders/gos_terrain_lighting.comp: local_size_x=64,
  binding 0/1/2, skeletal math (Stage 2 iterates to parity).
- Per-frame trio wired at terrain.cpp:1788 (after ComputePreflight);
  mission_init at mission.cpp:2788 (alongside gpu_cull::compute_init).
- CopyResultsToVertexPool is no-op stub at Stage 1 (output unused).
- Stage 1 dispatch overhead: N µs/frame (≤500 µs gate: PASS/FAIL).
- Tier1 5/5 PASS + menu canary PASS (env unset).
```

---

### Stage 2: Parity gate

**Files:** Implement shader math + parity comparator. CPU body remains authoritative.

- [ ] **Step 1: Implement `Parity_CompareFrame` per design doc Q3 API.**

```cpp
void Parity_CompareFrame(TerrainQuad* quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput) {
    // Walk quadList (same source CPU lighting walks); filter by calcThisFrame & 1.
    // Index output SSBO at outputs[v->vertexNum].
    // Skip if v == nullptr, v->vertexNum < 0, or !(v->calcThisFrame & 1).
    // Throttle prints to 16/frame; 600-frame summary (matches
    //   gos_terrain_indirect.cpp:276-291 pattern).
    // Per-mismatch:
    //   [TERRAIN_LIGHTING_PARITY v1] event=mismatch frame=%d vertex=%d
    //     field=lightRGB legacy=0x%08X gpu=0x%08X
    // 600-frame summary:
    //   [TERRAIN_LIGHTING_PARITY v1] event=summary frames=%lld
    //     verts_checked=%lld total_mismatches=%lld
}
```

In parity mode, `CopyResultsToVertexPool` uses `glClientWaitSync(GL_TIMEOUT_IGNORED)` on the current-frame fence (synchronous stall — acceptable only in parity mode; production never hits this path). Call `Parity_CompareFrame` AFTER the `quadSetupTextures` for-loop completes (`terrain.cpp:~1835` area, after the loop, before `CostSplit_RollFrame`).

- [ ] **Step 2: Iterate shader math until parity is clean.**

For each mismatch class: diff the `legacy=` vs `gpu=` bit patterns. Consult `quad.cpp:1266-1891` (lighting block) for the CPU formula. Common failure modes: wrong sRGB convention, wrong BGRA pack order (re-read `memory/mc2_argb_packing.md`), fp precision drift. The `TG_Light::GetFalloff` API is two-arg `(float length, float &falloff)` at `tgl.h:261-275`.

Run:
```
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1 ^
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 90 --kill-existing --keep-logs
```

Iterate until `total_mismatches=0`.

- [ ] **Step 3: Extended parity clean — all tier1 missions.**

```
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1 ^
  py -3 scripts/run_smoke.py --tier tier1 --duration 90 --kill-existing --keep-logs
```

Target: zero mismatches across 5 missions × 90 s ≈ 27,000 frames × ~62,500 verts ≈ ~1.7B field comparisons.

- [ ] **Step 4: Commit Stage 2.**

```
feat(terrain_lighting): Phase 1 Stage 2 — parity gate clean

- Parity_CompareFrame walks quadList + filters calcThisFrame&1 (design doc Q3).
- [TERRAIN_LIGHTING_PARITY v1] schema: 16/frame throttle + 600-frame summary
  (mirrors gos_terrain_indirect.cpp:276-291).
- tier1 5/5 × 90s: zero mismatches across ~1.7B field comparisons.
- Legacy CPU lighting still authoritative; GPU output discarded after parity.
```

**Stop condition:** If parity won't go to zero after 3 iteration rounds, STOP and surface the divergence pattern to user. Do NOT proceed to Stage 3 with non-zero parity.

---

### Stage 3: Consumer switch (GPU becomes authoritative)

**Files:** Gate CPU lighting block in `mclib/quad.cpp:1266-1891`. Wire `CopyResultsToVertexPool` non-stub.

- [ ] **Step 1: Implement `CopyResultsToVertexPool` non-blocking ring consumer (design doc Q2).**

```cpp
void CopyResultsToVertexPool(TerrainQuad* quadList, int numberQuads) {
    // T1: check N-1 slot fence with timeout=0
    // T2: check N-2 slot fence with timeout=0 (emit terrain_light_fallback_n2++)
    // T3: skip (emit terrain_light_fallback_conservative++)
    // On T1 or T2: copy stagingMapped[readSlot] into vertices[i]->lightRGB/fogRGB
    //   via quadList walk (same shape as Parity_CompareFrame — walk quads,
    //   filter vertexNum >= 0, write directly).
    // Production: glClientWaitSync timeout=0 ONLY. Never GL_TIMEOUT_IGNORED here.
}
```

BAR budget note (design doc Q2 MN4): staging ring is ~1.5 MB BAR (3 × 500 KB at 62,500 vertices × 8 B). This is the largest persistent-mapped allocation in the engine but well within Resizable BAR limits on RX 7900 XTX. Monitor `glMapBufferRange` latency via Tracy to confirm no fallback to system RAM.

- [ ] **Step 2: Gate CPU lighting block in `mclib/quad.cpp:1266-1891` (design doc Q5).**

```cpp
// Gate: authoritative when GPU enabled AND parity not forcing dual-run.
const bool s_lightingGpuAuth = gos_terrain_lighting::IsEnabled()
                                && !gos_terrain_lighting::IsParityCheckEnabled();
{
    CostSplitLightingScope _csLight;
    if (!s_lightingGpuAuth && terrainHandle != 0xffffffff) {
        // ... existing CPU lighting body (quad.cpp:1268-1889) unchanged ...
    }
    // s_lightingGpuAuth path: bracket runs; body skipped; ring already populated.
}
// close CostSplitLightingScope (1A-alt Slice 0)  — quad.cpp:1891
```

The `CostSplitLightingScope` bracket STAYS — it becomes retirement telemetry. Post-flip it must read ~0 µs.

- [ ] **Step 3: Build, deploy, run mc2_10 under authoritative gate.**

```
MC2_TERRAIN_LIGHTING_GPU=1 ^
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 90 --kill-existing --keep-logs
```

Expected: PASS. Visual: identical to legacy (Stage 2 proved bit-equality up to pipelined latency — 1-frame lag is imperceptible). `lighting_ns_per_frame` cost-split bucket must drop to ~0 µs.

- [ ] **Step 4: Stage 3 perf gate (design doc Q7) — AUTHORITATIVE commit gate.**

Measure with `MC2_TERRAIN_COST_SPLIT=1 MC2_TERRAIN_LIGHTING_GPU=1` at mc2_10 wolfman steady state:

**Gate 1 (retirement):** `lighting_ns_per_frame` ≤ **50 µs/frame**. Proves CPU body retired.

**Gate 2 (net Tracy cut):** Tracy zone `Terrain::geometry quadSetupTextures` mean ≥ **3.0 ms** cut vs pre-Phase-1 baseline (pre-Phase-1: 11.9 ms (11,871 µs) setup_total from Slice 0 commit `4fa7a9a` → post-Phase-1 target: ≤8.9 ms setup_total). Below 3.0 ms cut → STOP and surface to user.

Record both measurements in the commit message.

- [ ] **Step 5: Re-run parity under authoritative gate — tier1 5/5.**

```
MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1 ^
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
```

Both paths run; parity must still be zero.

- [ ] **Step 6: Commit Stage 3.**

```
feat(terrain_lighting): Phase 1 Stage 3 — GPU path authoritative

- CopyResultsToVertexPool: 3-slot T1/T2/T3 non-blocking tryConsume
  (design doc Q2; timeout=0 always on hot path, GL_TIMEOUT_IGNORED parity-only).
- CPU lighting block quad.cpp:1266-1891 gated off under s_lightingGpuAuth.
- CostSplitLightingScope bracket retained as retirement telemetry.
- Stage 3 perf gate: lighting_ns_per_frame = N µs (≤50 gate: PASS/FAIL),
  quadSetupTextures Tracy cut = N ms (≥3.0 ms gate: PASS/FAIL).
- tier1 5/5 PASS; parity zero mismatches under PARITY=1.
- Killswitch MC2_TERRAIN_LIGHTING_GPU=0 (or unset before Stage 5) restores legacy.
```

**Stop conditions for Stage 3:**
- `GL_INVALID_OPERATION` on dispatch → STOP, bisect (likely SSBO binding or shader resource mismatch).
- Perf gate misses (cut < 3 ms) → STOP, surface to user (revisit consumer strategy).
- Any tier1 mission FAIL → STOP, bisect.

---

### Stage 4: Soak

- [ ] **Step 1: 7-day soak per Track B precedent (`memory/track_b_widen_static_prop_registry.md`).**

Run with `MC2_TERRAIN_LIGHTING_GPU=1 MC2_TERRAIN_LIGHTING_PARITY=1` across normal gameplay sessions. Document soak start date and abort criteria in `memory/terrain_lighting_gpu_soak.md`.

**Soak abort criteria:**
- Any visual regression vs legacy.
- Any non-zero parity mismatches.
- Any `[GL_ERROR v1]` lines.
- Any FPS regression vs Stage 3 baseline (lighting bucket should remain ~0 µs).

If any criterion fires: revert `s_lightingGpuAuth` to parity-only mode (Stage 2 equivalent), bisect.

---

### Stage 5: Default-on flip

- [ ] **Step 1: Flip `IsEnabled()` semantics to default-on.**

```cpp
bool IsEnabled() {
    // Default ON post Stage 5. Explicit "0" opts out; absent or non-"0" opts in.
    static const bool cached = [] {
        const char* env = getenv("MC2_TERRAIN_LIGHTING_GPU");
        return (env == nullptr) || (env[0] != '0');
    }();
    return cached;
}
```

- [ ] **Step 2: Build, deploy, tier1 5/5 + menu canary + mc2_10 90s smoke clean under default-on.**

- [ ] **Step 3: Commit Stage 5 + update soak memory file.**

---

### Stage 6: CPU code demotion

Per CLAUDE.md Debug Instrumentation Rule: **keep** the legacy CPU lighting code in the tree, gated off. Do NOT delete. The `CostSplitLightingScope` bracket and the gated CPU body remain as retirement telemetry.

- [ ] **Step 1: Add production-mode assertion.**

```cpp
// At end of mission run (or triggered by MC2_TERRAIN_COST_SPLIT summary):
// lighting_ns_per_frame < 50 µs required; if not, log:
// [TERRAIN_LIGHTING_GPU v1] event=retirement_leak lighting_ns=N
```

This catches any code path accidentally running the CPU body in production.

- [ ] **Step 2: Commit Stage 6.**

---

## Phase 2 — Water Vertex Projection GPU Compute Port

Outline only — full spec and design doc written after Phase 1 ships.

**Goal:** Retire the ~0.91 ms/frame water-vertex projection block (`quad.cpp:946-1260`, `CostSplitWaterVertProjScope`).

**Key Phase 2 architectural prerequisite (design doc Q4):** The `leastZ`/`mostZ`/`leastW`/`mostW`/`leastWY`/`mostWY` reduction is a cross-cutting concern. Definitions at `mclib/terrain.cpp:1341-1343`; per-frame reset at `terrain.cpp:1382-1384` inside `Terrain::geometry()`; writers at `quad.cpp:1008/1075/1142/1209` (water block) and `terrain.cpp:1549-1552` / `terrain.cpp:1696-1715` (non-water and legacy fallback); consumer at `terrain.cpp:1832 eye->setInverseProject(mostZ, leastW, yzRange, ywRange)`. Phase 2 design doc must commit to option (a) joint port, (b) water-only + parallel CPU, or (c) deferral. Phase 2 CANNOT ignore this.

**Structural notes:**
- Reuses Phase 1's `GpuTerrainVertexInput` SSBO — water flags already in bits 4-7 (design doc Q6).
- New file: `gos_terrain_water_proj.{h,cpp}`, `shaders/gos_terrain_water_proj.comp`.
- Same compile-helper copy pattern (`tl_*` → `twp_*` prefix).
- Same 3-slot non-blocking ring readback pattern.
- Parity: `Parity_CompareFrame` walks `quad.cpp` water block output (`wx/wy/wz/ww/clipInfo`).
- Killswitch: `MC2_TERRAIN_WATER_PROJ_GPU=1`.
- Stages 0-6 mirror Phase 1 structure exactly.
- Phase 2 `drawWater` call site is `mclib/terrain.cpp:1140`; GPU results must be populated before that call.

---

## Perf / soak gates (summary)

| Gate | Criterion | Measurement |
|---|---|---|
| Stage 1 dispatch overhead | ≤500 µs/frame | Tracy zone `Terrain::TerrainLightingDispatch` (NEW) at mc2_10 wolfman |
| Stage 2 parity | zero mismatches | tier1 5/5 × 90 s ≈ 1.7B field comparisons |
| Stage 3 retirement | `lighting_ns_per_frame` ≤50 µs/frame | `MC2_TERRAIN_COST_SPLIT=1` summary |
| Stage 3 net Tracy cut | ≥3.0 ms vs 11.9 ms pre-Phase-1 baseline (`4fa7a9a` Slice 0 setup_total) | Tracy `Terrain::geometry quadSetupTextures` mean |
| Stage 4 soak | 7 days, no abort criteria fired | User's gameplay sessions |

---

## Stop conditions

- **Stage 2 parity won't clear after 3 iterations → STOP.** Either CPU math is non-reproducible or GPU has a convention mismatch. Surface divergence pattern to user.
- **Stage 3 `GL_INVALID_OPERATION` on dispatch → STOP, bisect.** Likely SSBO binding or shader resource declaration mismatch.
- **Stage 3 perf gate misses (cut < 3 ms) → STOP, surface to user.** Revisit whether consumer strategy should shift to SSBO-direct (eliminates readback overhead).
- **Stage 5 default-on causes any tier1 mission FAIL → STOP, revert to Stage 2 mode.**

---

## Partial-landing hazard (load-bearing — do not land without the paired gate)

Stage 3 consumer switch and the CPU-block gate in `quad.cpp:1266-1891` MUST land in the same commit. A Stage 3 commit that ships the `CopyResultsToVertexPool` wiring without the `s_lightingGpuAuth` gate produces a frame where GPU output AND CPU output both write `vertices[i]->lightRGB` — the CPU write wins (it runs second) and the GPU work is invisible.

---

## Adversarial review gate

This plan v2 was written after the adversarial review of v1 (`d2424ef`) was resolved by design doc `ac7c492`. The plan references design doc Q sections for every decision. Before opening Stage 1 executor session, confirm:
1. Design doc commit `ac7c492` is HEAD of the spec file.
2. No new SSBO fields have been added to `gos_terrain_indirect.cpp` or `gpu_cull_readback.cpp` since this plan was written (would change the precedent patterns).

---

## Verification appendix

New citations introduced in plan v2 (not already in design doc verification appendix). All grepped at plan v2 write-time.

| Citation | Grep result |
|---|---|
| `mclib/quad.cpp:946` `CostSplitWaterVertProjScope _csWvp;` | `grep -n "CostSplitWaterVertProjScope" mclib/quad.cpp` → `:154` (struct def), `:946` (open), `:1260` (close comment) |
| `mclib/quad.cpp:1266` `CostSplitLightingScope _csLight;` | `grep -n "CostSplitLightingScope" mclib/quad.cpp` → `:168` (struct def), `:1266` (open), `:1891` (close comment) |
| `mclib/quad.cpp:670` `void TerrainQuad::setupTextures (void)` | confirmed via design doc v3 appendix (M) |
| `mclib/quad.cpp:2977` `void TerrainQuad::drawWater (void)` | `grep -n "TerrainQuad::drawWater" mclib/quad.cpp` → `:2977` |
| `mclib/terrain.cpp:1783` `ZoneScopedN("Terrain::geometry quadSetupTextures")` | `grep -n "quadSetupTextures" mclib/terrain.cpp` → `:1783` |
| `mclib/terrain.cpp:1788` `gos_terrain_indirect::ComputePreflight()` | `grep -n "ComputePreflight" mclib/terrain.cpp` → `:1788` |
| `mclib/terrain.cpp:1140` `currentQuad->drawWater()` | `grep -n "drawWater" mclib/terrain.cpp` → `:1140` |
| `code/mission.cpp:2788` `gpu_cull::compute_init()` | `grep -n "compute_init" code/mission.cpp` → `:2788` — confirmed |
| `mclib/terrain.cpp:703` `Terrain::destroy` | confirmed via design doc v3 appendix (M) |
| `mclib/terrain.cpp:595` `Terrain::primeMissionTerrainCache` | confirmed via design doc v3 appendix (M) |
| `gos_terrain_indirect.cpp:276-291` parity throttle + `ParityFrameTick` | `grep -n "parity\|s_parityMismatch" GameOS/gameos/gos_terrain_indirect.cpp` → `:265-291` confirmed |
| `gos_terrain_indirect.cpp:279` `[TERRAIN_INDIRECT_PARITY v1] event=mismatch` schema | `grep -n "TERRAIN_INDIRECT_PARITY" GameOS/gameos/gos_terrain_indirect.cpp` → `:279` confirmed |
| `scripts/run_smoke.py:256` env allowlist area (after `MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK`) | `grep -n "MC2_TERRAIN_INDIRECT\|allowlist" scripts/run_smoke.py` → `:255 MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK`, `:256 MC2_TERRAIN_COST_SPLIT` — new entries go after `:256` |
| `mclib/terrain.cpp:1341-1343` leastZ/mostZ definitions | confirmed via design doc v3 appendix (M) |
| `mclib/terrain.cpp:1382-1384` per-frame reset | confirmed via design doc v3 appendix (M) |
| `mclib/terrain.cpp:1549-1552` non-water reduction writers | confirmed via design doc v3 appendix (M) |
| `mclib/terrain.cpp:1696-1715` legacy fallback reduction writers | confirmed via design doc v3 appendix (M) |
| `mclib/terrain.cpp:1832` `eye->setInverseProject` | confirmed via design doc v3 appendix (M) |
| `quad.cpp:490-495` extern leastZ/etc. | confirmed via design doc v3 appendix (M) |
| `gpu_cull_readback.cpp:40` `RING_FRAMES = 3u` | confirmed via design doc v3 appendix (M) |
| `GameOS/gameos/gpu_cull_compute.cpp:145-231` compile-helper trio | confirmed via design doc v3 appendix (M) |
| `gos_static_prop_batcher.cpp:641` write-only persistent map | `grep -n "GL_MAP_WRITE_BIT\|MAP_PERSISTENT" GameOS/gameos/gos_static_prop_batcher.cpp` → `:641` confirmed |
