# `quadSetupTextures` GPU-Compute Port — Design Decisions (Stage 0)

> **Companion to** [`docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md`](../plans/2026-05-10-quadsetuptextures-gpu-compute-port.md).
>
> This design doc resolves the 7 open questions surfaced by the adversarial-plan-review pass against plan v1. Each section closes one question with grep evidence, commits to a single architectural choice, and identifies the load-bearing constraint the choice respects. The implementation plan is rewritten (`-v2`) against this design — the plan no longer carries unresolved decisions.

**Worktree:** `claude/parallel-amdahl` (branched from `claude/nifty-mendeleev` @ 7b9ad5f)
**Plan v1 commit:** `d2424ef` (under review; revised plan supersedes)
**Slice 0 recon commit:** `4fa7a9a`

---

## Documentation discipline

Every cited symbol below is grep-verified at write-time per the worktree CLAUDE.md "grep at write-time, not after" rule. The verification appendix at the end lists each citation with M (matches) / D (divergent) / NF (not found) status. The adversarial-plan-review pass on plan v1 caught systematic line-number drift; this design doc v2 fixes those errors against the **current** tree state (parallel-amdahl worktree, post-Slice-0 commit `4fa7a9a`). All citations in v2 were grep-verified at write-time; no item is marked M on memory alone.

---

## Open question 1 — Compile helper: factor shared or copy pattern?

### Decision

**Copy the pattern into `gos_terrain_lighting.cpp` (and the future Phase 2 `gos_terrain_water_proj.cpp`)** without factoring a shared header in this slice. The factoring is a separate cleanup slice if/when a third compute-shader module appears.

### Grep evidence

`GameOS/gameos/gpu_cull_compute.cpp:145-229` defines the private static trio:

```cpp
static GLuint compile_compute_shader(const char** strings, int count);     // 145-168
static GLuint link_compute_program(GLuint shader);                          // 170-193
static GLuint build_compute_program_from_file(                              // 197-229
    const char* fname,
    const std::string* preambles, int nPreambles,
    const char* debugName);
```

All three are file-private (`static`). The high-level `build_compute_program_from_file` prepends `"#version 430\n"` as the first string per worktree CLAUDE.md Critical Rules ("Shader #version: Never in shader files. Pass `#version 430\n` as prefix to `makeProgram()`").

The pattern is ~85 lines of straightforward GL boilerplate.

### Why copy not factor

1. **Single-existing-consumer rule.** `gpu_cull_compute.cpp` is currently the only consumer. Factoring a shared header to support a second consumer is premature — `YAGNI` says wait for the third.
2. **No struct/state shared.** The compile helper has no per-module state; copying ~85 lines duplicates work but no semantics. Bug-fix divergence risk is low because the GL API the helpers call is stable across versions.
3. **Audit surface.** A shared header introduces an inter-module dependency that the adversarial-plan-review pass would have to retrace whenever either module touches GL initialization. Local copies keep the audit scope tight.
4. **House-style precedent.** `gos_terrain_water_stream.cpp`, `gos_terrain_patch_stream.cpp`, `gos_terrain_indirect.cpp` — none cross-share GL helpers; each module owns its own program lifecycle.

### Implication for the plan

Phase 1 Stage 1 Step 3 changes from a fictional `gos_make_compute_program(...)` call to:

```cpp
// gos_terrain_lighting.cpp (private file-scope helpers — pattern from gpu_cull_compute.cpp:145-229)
static GLuint tl_compile_compute_shader(const char** strings, int count) { ... }
static GLuint tl_link_compute_program(GLuint shader) { ... }
static GLuint tl_build_compute_program_from_file(const char* fname,
                                                 const std::string* preambles,
                                                 int nPreambles,
                                                 const char* debugName) { ... }
```

Prefix `tl_` (terrain_lighting) avoids ODR conflicts since the helpers are `static`.

---

## Open question 2 — Consumer strategy: SSBO-direct refactor or CPU readback?

### Decision

**Split by field:**

- **`lightRGB`: CPU readback.** Consumers span `mclib/quad.cpp` (55 `->lightRGB` hits — writes in the lighting block `1266-1891` plus reads in `draw`/`drawWater`) AND several GPU-direct renderer files that read the CPU mirror per-frame (see consumer breakdown below). Preserving `vertices[i]->lightRGB` as the canonical CPU-side mirror requires writing the SSBO back to the CPU vertex pool ONCE per frame after compute dispatch. GPU-direct renderers (`gos_terrain_water_stream`, `gos_terrain_indirect`) read the CPU mirror, meaning they inherit the 1-frame pipelined latency: frame N+1 GPU draws use lighting computed at frame N. This is visually invisible (~16 ms at 60 fps) and matches the latency that the `quadSetupTextures` CPU path already accepts from its per-frame dedupe gate.
- **`fogRGB`: CPU readback (same buffer).** Consumer split: `mclib/quad.cpp` (most), plus **`mclib/clouds.cpp:289-348` (6 sites)** — cross-file. Refactoring clouds.cpp to read SSBO directly is out-of-scope blast radius; CPU readback is the surgical choice.

> **MN1 — Dynamic-light pipelining edge case:** Transient lights (PPC bolts, explosions) that exist for 1-3 frames will have their lighting contribution appear with 1-frame latency in pipelined mode. This is acceptable: the visual artifact (sub-16 ms flash of unlighted vertex) is imperceptible at 60 fps, and the parity gate (synchronous mode) compares current-frame GPU output so the bug cannot hide.

Both fields use the **same SSBO output struct** (`GpuTerrainLightingOutput`), uploaded once per frame, read back once per frame, copied into the per-vertex pool.

### Grep evidence — lightRGB consumer breakdown

`grep -rn '->lightRGB' mclib/ code/ GameOS/ --include='*.cpp' --include='*.h'` (excluding `.codex_tmp_isolate/` isolation paths), run at v2 write-time:

| File | `->lightRGB` hits | Nature |
|---|---|---|
| `mclib/quad.cpp` | 55 | Mix: 8 writes in lighting block (`:1322/1373/1476/1527/1630/1681/1784/1835`); rest are reads in `draw`/`drawWater` |
| `GameOS/gameos/gos_terrain_water_stream.cpp` | 8 | CPU mirror reads — pack thin record (`:460-463`), parity check (`:809, :811`), draw path (`:929, :967`) |
| `GameOS/gameos/gos_terrain_indirect.cpp` | 2 | CPU mirror reads — draw path (`:1408 comment`, `:1459`) |
| `GameOS/gameos/gos_terrain_water_stream.h` | 1 | Comment in table header (`:44`) — not a real consumer |
| `code/carnage.cpp` | 1 | `ExplosionType::lightRGB` field on a different struct (`:687`) — not the per-vertex terrain mirror |

**All GPU-direct renderer consumers (`gos_terrain_water_stream`, `gos_terrain_indirect`) read `vertices[c]->lightRGB` from the CPU pool.** They inherit the 1-frame pipelined latency when the compute port is active. The `carnage.cpp` hit is a different struct's field (explosion type definition, not a terrain vertex read).

- `grep '->fogRGB' mclib/clouds.cpp` → 6 hits at lines 289, 298, 307, 330, 339, 348 — all reads inside cloud render path. Cross-file consumer confirmed.

### Sync-stall avoidance — the load-bearing detail

Per `memory/substrate_coalesce_sync_point_lesson.md`: **"grep `glGetBufferSubData|glReadPixels|glMapBuffer.*GL_MAP_READ_BIT` in the on-only path. Any hit on the hot frame path is suspect."**

The plan v1 codeblock had `glMapBufferRange(..., GL_MAP_READ_BIT)` exactly. That's a stall.

**Resolution: GPU SSBO + persistent-mapped staging copy ring with glFenceSync pipelining.**

This is the pattern proven in `gpu_cull_readback.cpp` (the only existing READ-path persistent-mapped buffer in the codebase). Two key observations from grepping the tree:

1. **All existing persistent-mapped buffers except `gpu_cull_readback.cpp:230-235` are WRITE-only.** `gos_static_prop_batcher.cpp:641`, `gos_terrain_patch_stream.cpp:269/294`, `gpu_cull_substrate.cpp:115` all use `GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`.

2. **`gpu_cull_readback.cpp:230-235` IS the precedent** for a CPU-readable persistent-mapped buffer. It uses `GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` on a staging copy buffer (separate from the GPU-side SSBO), with `glCopyBufferSubData` from VRAM to the BAR staging buffer, and `glFenceSync`/`glClientWaitSync` (proven pattern: `gpu_cull_readback.cpp:469`, `gpu_cull_substrate.cpp:266`, `gos_terrain_patch_stream.cpp:1511`) to synchronize.

**Pattern (adopted from gpu_cull_readback precedent):**

```cpp
// At init: GPU-side SSBO (compute writes), CPU-visible staging ring (CPU reads)
glGenBuffers(1, &s_computeSsbo);             // GPU SSBO — compute writes here
glBufferStorage(..., 0, nullptr,
    GL_DYNAMIC_STORAGE_BIT);                 // no persistent map needed — write-only from shader

glGenBuffers(2, s_stagingRing);              // 2-slot staging ring for CPU readback
for (int i = 0; i < 2; ++i) {
    // READ+WRITE+PERSISTENT+COHERENT — matches gpu_cull_readback.cpp:230-235
    glBufferStorage(..., nullptr,
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
        GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    s_stagingMapped[i] = glMapBufferRange(...,
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
        GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
}

// Per frame:
int writeSlot = frameIndex % 2;   // GPU writes into computeSsbo; then glCopyBufferSubData → stagingRing[writeSlot]
int readSlot  = 1 - writeSlot;    // CPU reads stagingRing[readSlot] (1 frame stale)
// After glCopyBufferSubData: glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) for writeSlot.
// Before reading stagingRing[readSlot]: glClientWaitSync(s_fence[readSlot], ...) — already complete at N-1 frames.
```

The `GL_MAP_WRITE_BIT` on the staging buffer matches `gpu_cull_readback.cpp`'s pattern exactly (write+read; write-only persistent maps have proven to work; read-only persistent is untested and AMD driver behavior is unknown without this extra write flag). The `glFenceSync`/`glClientWaitSync` idiom is proven in 4 existing files (substrate, patch_stream, indirect, readback).

The 1-frame latency means: GPU lighting written at frame N is readable by CPU at frame N+1. The frame-1-late-by-one-frame visual is invisible in practice (~16 ms at 60 fps) but the parity gate must compare current-frame GPU output against CURRENT-frame CPU output — so during parity-on mode, we call `glClientWaitSync(GL_TIMEOUT_IGNORED)` on the writeSlot fence before reading (synchronous stall, acceptable in parity-only mode). Production runs with parity off are fully pipelined.

Alternative: triple-buffered ring. Plan v2 picks 2 buffers (sufficient for 1-frame pipelining) unless empirical testing shows GPU≥CPU contention, in which case 3 buffers.

### Implication for the plan

- Phase 1 Stage 1 (input + compute scaffold): ships 2-buffer ring SSBO + persistent mapping.
- Phase 1 Stage 2 (parity): toggles synchronous fence-wait when `MC2_TERRAIN_LIGHTING_PARITY=1` so comparison is bit-accurate against current frame.
- Phase 1 Stage 3 (consumer flip): uses readSlot output (1-frame-old SSBO) to populate `vertices[i]->lightRGB` and `vertices[i]->fogRGB` in a tight loop AFTER compute dispatch, BEFORE the `quadSetupTextures` for-loop. Position matters: drawWater and quad.cpp consumers run later in the frame and need the CPU mirror populated.

---

## Open question 3 — `vertexNum == -1` and `calcThisFrame` dedupe semantics

### Decision

**Compute shader writes unconditionally for ALL slots `[0, realVerticesMapSide²)`. The parity comparator restricts to vertices the CPU actually wrote this frame, identified by `vertices[i]->calcThisFrame & 1` (lighting) or `& 2` (water projection). The CPU vertex pool's `vertexNum == -1` slots correspond to no `vertices[i]` pointer and are never compared.**

### Grep evidence

- `mclib/mapdata.cpp:1114`: `currentVertex->vertexNum = -1;` — the unmapped-vertex case (off-map quad corners).
- `mclib/mapdata.cpp:1119`: `currentVertex->vertexNum = topLeftX + topLeftY * realVerticesMapSide;` — the mapped case. **`vertexNum` is a flat dense index in `[0, realVerticesMapSide²)` for mapped vertices.**
- `quad.cpp` (post Slice-0):
  - Lighting block per-vertex gate: `if (!(vertices[0]->calcThisFrame & 1))` (line ~1276).
  - Water block per-vertex gate: `if (!(vertices[0]->calcThisFrame & 2))` (line ~961).
  - Set after compute: `vertices[i]->calcThisFrame |= 1;` (lighting), `vertices[i]->calcThisFrame |= 2;` (water).

### Why "all slots" not "live slots"

- **No central walk of live vertices.** The CPU codepath iterates the per-frame `quadList`, dereferences `quad->vertices[i]` (4 pointers per quad), and gates by `calcThisFrame` bits to dedupe corner-shared vertices. There's no flat "live vertex list" to drive a smaller compute dispatch.
- **`realVerticesMapSide` has no const cap.** It is a `static long` defined at `terrain.cpp:105` and assigned from map geometry at `terrain.cpp:315` (sqrt of post-comp vertex count) or `terrain.cpp:389` (explicit `verticesPerMapSide`). There is no compile-time upper bound. For stock mc2_10 wolfman, `realVerticesMapSide` ≈ 250 (62,500 vertices). For modded maps, it could be larger. The SSBO allocator in `mission_init` receives `numVertices = realVerticesMapSide * realVerticesMapSide` and must allocate that count dynamically, not a fixed cap. Budget at 62,500 × 32 B input + 62,500 × 8 B output ≈ 2.5 MB; this is fine for any realistically-sized map.
- **GPU work is cheaper than gather logic.** Computing per-vertex lighting for 62,500 vertices at GPU compute speed (~1 µs total at moderate light count) is faster than building a sparse vertex list on CPU and uploading it. The "wasted" computation for invisible/off-frame vertices is invisible.

### Parity comparator iteration

The comparator walks `quadList` (the same source the CPU lighting walks), dereferences `quad->vertices[i]`, reads `vertexNum`, and indexes the SSBO directly at `outputs[vertexNum]`. For vertices with `vertexNum == -1` (off-map) or with `calcThisFrame & 1 == 0` (not lit this frame on CPU side), the comparator skips. Only vertices the CPU body actually wrote are compared.

Schema:

```cpp
// gos_terrain_lighting.cpp
void Parity_CompareFrame(TerrainQuadPtr quadList, int numberQuads,
                         const GpuTerrainLightingOutput* mappedOutput) {
    for (int q = 0; q < numberQuads; ++q) {
        for (int i = 0; i < 4; ++i) {
            VertexPtr v = quadList[q].vertices[i];
            if (!v || v->vertexNum < 0) continue;
            if (!(v->calcThisFrame & 1)) continue;  // CPU didn't write this vertex
            uint32_t legacyLight = v->lightRGB;
            uint32_t gpuLight = mappedOutput[v->vertexNum].lightRGB;
            if (legacyLight != gpuLight) {
                Parity_PrintMismatch(/*...*/);
            }
        }
    }
}
```

### Implication for the plan

- Plan v1's `Parity_CompareAfterDispatch(const VertexPtr* vertexArray, uint32_t numVertices)` API is wrong — there's no flat `vertexArray`. Plan v2's API is `Parity_CompareFrame(TerrainQuadPtr quadList, int numberQuads, const GpuTerrainLightingOutput* mappedOutput)`.
- SSBO size: `realVerticesMapSide * realVerticesMapSide * sizeof(GpuTerrainLightingOutput)` = stock-mc2_10 ~62500 × 8 B = 500 KB. Dynamically allocated at `mission_init` (no const cap on realVerticesMapSide; see Q3 grep note above).
- Compute shader dispatch count: `ceil(numVertices / 64)` workgroups, where `numVertices = realVerticesMapSide²`. The shader-side body branches on `if (vn >= u_numVertices) return;` to bound the upper edge.

---

## Open question 4 — Multi-source `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction

### Decision

**Phase 1 does NOT touch the reduction state (it's water-projection-block only, not lighting-block). Phase 2 must port BOTH writers jointly OR commit to keeping the CPU reduction live in parallel with the GPU output.**

### Grep evidence

- `quad.cpp:490-495`: `extern float leastZ; extern float leastW; extern float mostZ; extern float mostW; extern float leastWY; extern float mostWY;` — extern declarations. **The definitions live in `terrain.cpp:938-940`** (file-scope globals `float leastZ = 1.0f, leastW = 1.0f; float mostZ = -1.0f, mostW = -1.0; float leastWY = 0.0f, mostWY = 0.0f;`).
- **Per-frame reset** is at `terrain.cpp:946-948`, inside `Terrain::geometry()`: `leastZ = 1.0f; leastW = 1.0f; mostZ = -1.0f; mostW = -1.0; leastWY = 0.0f; mostWY = 0.0f;`. There is no reset in `quad.cpp`.
- `quad.cpp:1008/1075/1142/1209`: 4 writers, all inside the water-projection block `946-1260` (one per vertex of the quad). Writes only when `screenPos` is computed.
- `terrain.cpp:1549-1552`: writers in the non-water terrain projection loop (D1 fast-path vertex-project loop).
- `terrain.cpp:1698-1715`: writers in the legacy fallback projection loop.
- Consumer: `terrain.cpp:1832 eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` — singular consumption point. **M — grep confirmed.**

### Implication for the plan

**Phase 1 (lighting) is completely unaffected by this.** The lighting block writes `lightRGB`/`fogRGB`; it doesn't touch the reduction state. Phase 1 ships standalone.

**Phase 2 (water-projection GPU port) requires a Stage 0 sub-decision before code lands:**

- **Option (a) — joint port.** Port the terrain.cpp non-water and legacy fallback writers (`:1549, :1698`) as part of Phase 2's compute work. Single GPU reduction (atomic-min/max or 2-pass), single consumer side. Larger blast radius. Retires reduction state entirely.
- **Option (b) — water-only port + parallel CPU.** Keep terrain.cpp CPU writers live (they're already inside `vertexProjectLoop` which is itself a candidate Phase 3+ slice). The CPU-side `leastZ` etc. remain live; GPU side does water-projection compute but DOES NOT reduce; CPU water-vertex writes continue for the reduction path only.
- **Option (c) — defer Phase 2 entirely.** If joint port is too wide and parallel CPU is too messy, ship Phase 1 and stop. Re-evaluate Phase 2 after `vertexProjectLoop` ports to GPU (which would naturally consume the terrain.cpp writers).

**Phase 2 design doc (separate, written after Phase 1 ships) commits to one of (a)/(b)/(c).** This design doc only commits Phase 2 to "the reduction is a real cross-cutting concern; Phase 2 cannot ignore it."

---

## Open question 5 — Per-mission lifecycle hook

### Decision

**Two tiers of lifecycle hooks**, matching the precedent established by `gpu_cull::compute_init()`:

**Tier 1 — Per-mission init/shutdown** (the `gpu_cull` precedent tier): The existing GPU-cull compute infrastructure calls `gpu_cull::compute_init()` from `mission.cpp:2788` (inside `Mission::init`), NOT from a process-level renderer init. This is the correct model for GPU infrastructure that allocates mission-sized buffers. We adopt the same pattern: a `per_mission_init` call from the same chokepoint, and teardown via the existing `Terrain::destroy` / `Mission::destroy` chain.

**Tier 2 — Process-level init/shutdown** (optional, for shader compilation): Shader compilation can be deferred to first per-mission init. If a separate process-level init is desired for pre-compiling the shader at startup, the real process-level chokepoints are `InitializeGameEngine` at `code/mechcmd2.cpp:870` and `TerminateGameEngine` at `code/mechcmd2.cpp:1918`. These are the `Environment.InitializeGameEngine`/`TerminateGameEngine` function-pointer callbacks (wired at `mechcmd2.cpp:2811/2813`).

> **`gos_RendererInit` and `gos_RendererShutdown` do not exist anywhere in the codebase.** Grep of `GameOS/gameos/`, `mclib/`, `code/` returns zero hits. These were fictional names in v1. They are replaced above by real symbols.

```cpp
// gos_terrain_lighting.h
namespace gos_terrain_lighting {

// Per-mission: alloc SSBOs (sized to mission's realVerticesMapSide²),
//   compile shader on first call, reset ring buffers.
// Call from Mission::init alongside gpu_cull::compute_init() at mission.cpp:2788.
void mission_init(uint32_t numVertices, uint32_t maxLights);

// Per-mission teardown: zero CPU state, keep GL allocations for reuse.
// Call from Terrain::destroy (terrain.cpp:703) or Mission::destroy chain.
void mission_shutdown();

void BeginFrame();                                     // per-frame: advance ring index
void PackAndDispatch();                                // per-frame: pack input SSBO,
                                                       //   glDispatchCompute, memory barrier
void CopyResultsToVertexPool(TerrainQuadPtr quadList,
                             int numberQuads);         // per-frame: fence-wait on readSlot,
                                                       //   copy staging ring → vertices[i]->lightRGB/fogRGB

bool IsEnabled();
bool IsParityCheckEnabled();
}
```

### Grep evidence

- `gpu_cull::compute_init()` at `GameOS/gameos/gpu_cull_compute.cpp:308` — the per-mission precedent symbol, called from `mission.cpp:2788`.
- `Mission::init` calls `gpu_cull::compute_init()` at `mission.cpp:2788`.
- `Terrain::primeMissionTerrainCache` at `terrain.cpp:595`, called from `mission.cpp:2240` — the canonical "mission terrain ready" hook; `mission_init` can be called at the same site or immediately after.
- `Terrain::destroy` at `terrain.cpp:703` — the per-mission teardown chokepoint for `mission_shutdown`.
- `InitializeGameEngine` at `code/mechcmd2.cpp:870` — real process-level init hook (Environment callback).
- `TerminateGameEngine` at `code/mechcmd2.cpp:1918` — real process-level teardown hook.

Memory `water_ssbo_pattern.md` documents the canonical "static recipe + per-frame thin record" model, which is structurally what `mission_init` + `PackAndDispatch` implement.

### Call-site wiring (specified, not deferred)

- `mission_init` → from `Mission::init` at `mission.cpp:2788` area, alongside `gpu_cull::compute_init()`.
- `mission_shutdown` → from `Terrain::destroy` (`terrain.cpp:703`).
- `BeginFrame` + `PackAndDispatch` → `terrain.cpp` geometry-loop area, before the `setupTextures` for-loop.
- `CopyResultsToVertexPool` → SAME spot, AFTER `PackAndDispatch`. Fence-waits on readSlot (already-complete at N-1 frames in pipelined mode) and writes into `vertices[i]->lightRGB`/`fogRGB`. Cheap (62,500 vertex × 8 bytes = ~500 KB/frame).

### Implication for the plan

Plan v2 Phase 1 Stage 1 ships all hooks even when no consumer is wired yet — `mission_init` allocs, `mission_shutdown` frees, etc. The plan's file-structure table in v1 missed per-mission teardown; plan v2 adds `mission_shutdown`.

---

## Open question 6 — Phase 2 SSBO flag-bit layout (forward-compat)

### Decision

**Encode all per-vertex flag bits Phase 2 will need into Phase 1's `GpuTerrainVertexInput.flags` field at first introduction.** This eliminates retroactive struct churn between Phase 1 and Phase 2.

### Flag-bit layout (committed)

```cpp
// gos_terrain_lighting.h
// Phase 1 reads bits 0-3 (lighting + shadow + frame-dedupe).
// Phase 2 reads bits 4-7 (water animation + projection dedupe).
// Bits 8-31 reserved for future slices.

#define GPU_VERT_SHADOW          0x00000001u  // vertices[i]->pVertex->shadow != 0
#define GPU_VERT_CALCFRAME_LIGHT 0x00000002u  // vertices[i]->calcThisFrame & 1 (CPU-side dedupe)
#define GPU_VERT_BASE_COLOR_LIT  0x00000004u  // BaseVertexColor != 0 (broadcast from uniform)
#define GPU_VERT_RAIN_DAMPEN     0x00000008u  // rainLightLevel < 1.0f (broadcast from uniform)

#define GPU_VERT_WATER           0x00000010u  // vertices[i]->pVertex->water & 1
#define GPU_VERT_WATER_ANIM_NEG  0x00000020u  // water & 128 (frameCos sign-flip)
#define GPU_VERT_WATER_ANIM_POS  0x00000040u  // water & 64  (frameCos pass-through)
#define GPU_VERT_CALCFRAME_WATER 0x00000080u  // vertices[i]->calcThisFrame & 2 (water dedupe)
```

Phase 1's compute shader reads bits 0-3 + 7 (the `CALCFRAME_LIGHT` bit informs the shader whether to bother writing — not load-bearing because the parity comparator filters, but useful for skipping work).

Phase 2's compute shader reads bits 4-7.

Phase 1 ships the FULL flag pack including bits 4-7 even though Phase 1 doesn't use them. Phase 2 reads them without changing the struct.

### Per-light SSBO struct (committed)

```cpp
struct alignas(16) GpuTerrainLight {
    float position[3];      // 12 B
    uint32_t lightType;     // 4 B
    float color[3];         // 12 B
    float falloffParam;     // 4 B
    // total: 32 B std430
};
```

`falloffParam` semantics depend on `lightType`:

- `TG_LIGHT_POINT_GPU` (1): falloffParam = `closeDistance`. `GetFalloff` formula (from `mclib/tgl.h:261-275`): returns `false` when `length >= farDistance`; returns `true` with `falloff = (farDistance - length) * oneOverDistance` otherwise; `falloff = 1.0f` when `length <= closeDistance`.
- `TG_LIGHT_SPOT_GPU` (2): same `GetFalloff` formula plus cone-cosine factor (per `mlrspotlight.cpp:216-232`).
- `TG_LIGHT_TERRAIN_GPU` (3): falloffParam = terrain-attenuation scalar. Per memory `camera.cpp:723` (light getter).

### Grep evidence

Per `mclib/quad.cpp` (the lighting block per-vertex 0 inner loop), `GetFalloff` is called at:
- `quad.cpp:1347` — vertex 0
- `quad.cpp:1500` — vertex 1
- `quad.cpp:1654` — vertex 2
- `quad.cpp:1808` — vertex 3

The signature is `bool TG_Light::GetFalloff(float length, float &falloff)` — defined at `mclib/tgl.h:261`. Returns `true` if the vertex is within falloff range, writes the falloff scalar into the output reference. Implementation: `falloff = (farDistance - length) * oneOverDistance` for intermediate distances, `1.0f` for `length <= closeDistance`, `false` (out of range) for `length >= farDistance`.

Per-type implementations in:
- `mclib/mlr/mlrpointlight.cpp:186` — calls `GetFalloff(length, falloff)`
- `mclib/mlr/mlrspotlight.cpp:232` — calls `GetFalloff(length, falloff)`
- `mclib/mlr/mlrinfinitelightwithfalloff.cpp:197` — calls `GetFalloff(length, falloff)`

All call sites confirmed by grep at v2 write-time. **This is NOT an open question** — the API is two-arg (length, &falloff), returns bool, math is in `tgl.h:261-275`.

### std430 layout verification — `GpuTerrainVertexInput` (MN3)

The input struct uses std430. Manual alignment check:

| Field | Type | Size | Align | Offset |
|---|---|---|---|---|
| `xy` | `vec2` | 8 B | 8 B | 0 |
| `elevation` | `float` | 4 B | 4 B | 8 |
| `_pad0` | `float` | 4 B | 4 B | 12 |
| `normal` | `vec3` | 12 B | 16 B | 16 (align to 16) |
| `flags` | `uint` | 4 B | 4 B | 28 |
| **total** | | **32 B** | | |

Stride = 32 B. Manual check passes: `vec3` at offset 16 satisfies 16-byte alignment; `uint flags` at offset 28 satisfies 4-byte alignment; struct total is 32 B (divisible by largest member alignment 16 B). The `_pad0` field is required to push `normal` to the 16-byte boundary.

### Implication for the plan

Plan v1's flag bits ("shadow bit, water bit, water&64, water&128, calcThisFrame bits") were hand-waved. Plan v2 cites this design doc's exact bit layout in Stage 0 Step 5.

---

## Open question 7 — Phase 1 perf gate: realistic target

### Decision

**Replace plan v1's "≤6.5 ms target (4.8 ms cut)" speculation with an empirically-anchored two-stage gate:**

- **Stage 1 measurement (output unused):** dispatch cost only. The compute pass runs every frame, output is discarded. Target: **dispatch overhead ≤500 µs/frame** at mc2_10 wolfman. If ≥1 ms, the input SSBO pack loop is too expensive and Stage 0 must reconsider (e.g. persistent-map the input SSBO too).
- **Stage 3 measurement (gate authoritative, CPU body retired):** target: **`lighting_ns_per_frame` cost-split bucket reads < 50 µs/frame.** This proves the gate retired the CPU work as intended. Net Tracy cut on `Terrain::geometry quadSetupTextures` mean = **(5.18 ms CPU lighting retired) − (Phase 1 dispatch + copy overhead) > 3.0 ms**. Below 3.0 ms cut → STOP per stop-condition.

### Why two-stage gates

Plan v1 conflated "lighting bucket value at Slice 0" with "wall-clock cut" and produced an unsubstantiated 4.8 ms figure. Two-stage gates separate "did the dispatch work" from "did the retirement work" — both must pass independently for Phase 1 to ship.

### Implication for the plan

Plan v2 Stage 1 ends with "measure Stage 1 dispatch overhead via Tracy zone `Terrain::TerrainLightingDispatch` (NEW); record baseline; gate on ≤500 µs." Plan v2 Stage 3 ends with "measure post-retirement cost-split `lighting_ns_per_frame` ≤50 µs AND Tracy `quadSetupTextures` cut ≥3.0 ms." Both targets are empirically anchored, not speculative.

---

## Summary of design commitments

| Question | Decision | Plan-v2 implication |
|---|---|---|
| 1 — Compile helper | Copy pattern privately into each new module | Stage 1 Step 3 uses `tl_compile_compute_shader` etc., local statics, no fictional API |
| 2 — Consumer strategy | CPU readback via pipelined 2-buffer persistent-mapped SSBO | Stage 1 ships ring buffers; Stage 3 reads readSlot into vertex pool; parity sync-waits |
| 3 — `vertexNum == -1` / `calcThisFrame` | Shader writes all slots; comparator filters by quadList walk + calcThisFrame bit | API: `Parity_CompareFrame(quadList, numberQuads, mappedOutput)`; SSBO size = realVerticesMapSide² |
| 4 — Multi-source reduction | Phase 1 unaffected; Phase 2 design doc commits to a/b/c later | Plan v1's Phase 2 outline updated to flag this as a real Phase-2 prereq |
| 5 — Per-mission lifecycle | `mission_init` + `mission_shutdown` (per-mission, matching `gpu_cull::compute_init` precedent) + per-frame trio; process-level if needed: `InitializeGameEngine`/`TerminateGameEngine` at `mechcmd2.cpp:870/1918` | `gos_RendererInit/Shutdown` were fictional — removed; call-site wiring spec'd to real symbols |
| 6 — Flag-bit layout | Full 8-bit layout committed up-front; Phase 1 pack includes Phase 2 bits | Plan v2 Stage 0 Step 5 cites this doc's layout verbatim |
| 7 — Perf gate | Two-stage: dispatch-overhead ≤500 µs + retirement-residual ≤50 µs + net cut ≥3.0 ms | Plan v2 Stage 1 + Stage 3 each carry a measurement requirement, not speculation |

---

## Verification appendix (v2 — regenerated, all citations grep-verified at v2 write-time)

Status: M (matches, grep'd at v2 write-time), NF (not found), D (divergent from v1 claim, corrected).

All greps run against `A:/Games/mc2-opengl-src/.claude/worktrees/parallel-amdahl/` source tree.

| Citation | Status | Grep result |
|---|---|---|
| `GameOS/gameos/gpu_cull_compute.cpp:145-168` `compile_compute_shader` | M | Read confirmed `static GLuint compile_compute_shader(const char** strings, int count)` at :145 |
| `GameOS/gameos/gpu_cull_compute.cpp:170-193` `link_compute_program` | M | Read confirmed `static GLuint link_compute_program(GLuint shader)` at :170 |
| `GameOS/gameos/gpu_cull_compute.cpp:197-231` `build_compute_program_from_file` | M | Read confirmed closes at :231 (v1 cited :145-229, slightly off — actual end :231) |
| `GameOS/gameos/gpu_cull_compute.cpp:308` `compute_init` function body | M | grep confirmed `bool compute_init()` at :308 |
| `code/mission.cpp:2788` `gpu_cull::compute_init()` call | M | grep confirmed |
| `mclib/quad.cpp:490-495` `extern float leastZ/leastW/mostZ/mostW/leastWY/mostWY` | M | Read confirmed (v1 wrongly cited :1341-1343 — actual location :490-495) |
| **`mclib/quad.cpp:1341-1343` leastZ extern declarations** | **D** | **v1 claim was wrong — actual location is :490-495 (see above)** |
| `mclib/terrain.cpp:938-940` `float leastZ = 1.0f,...` definitions | M | grep confirmed `float leastZ = 1.0f,leastW = 1.0f;` at :938 |
| `mclib/terrain.cpp:946-948` per-frame reset inside `Terrain::geometry` | M | Read confirmed `leastZ = 1.0f; leastW = 1.0f; ...` at :946-948 |
| **`mclib/quad.cpp:1382-1384` per-frame reset** | **NF** | **v1 claim was wrong — no reset in quad.cpp at all; reset is in terrain.cpp:946-948** |
| `mclib/quad.cpp:1008/1075/1142/1209` water-block leastZ/mostZ writers | M | grep confirmed 4 assignments inside setupTextures water block |
| `mclib/quad.cpp:946-1260` CostSplitWaterVertProjScope (water-projection block) | M | grep confirmed `CostSplitWaterVertProjScope _csWvp` at :946; close at :1260 |
| `mclib/quad.cpp:1266-1891` CostSplitLightingScope (lighting block) | M | grep confirmed `CostSplitLightingScope _csLight` at :1266; close at :1891 |
| `mclib/quad.cpp:670` `void TerrainQuad::setupTextures (void)` | M | grep confirmed (v1 cited "672-1892 function-close" — function body OPENS at :670; scope tracker at :672) |
| `mclib/terrain.cpp:1549-1552` non-water reduction writers | M | Read confirmed 4 writers at :1549-1552 |
| `mclib/terrain.cpp:1698-1715` legacy fallback reduction writers | M | Read confirmed 4 writers at :1698-1715 |
| `mclib/terrain.cpp:1832` `eye->setInverseProject(mostZ, leastW, ...)` | M | grep confirmed `eye->setInverseProject(mostZ,leastW,yzRange,ywRange)` at :1832 |
| `mclib/quad.cpp` `->lightRGB` hits = 55 | M | grep -c confirmed 55 hits |
| **`->lightRGB` zero hits outside quad.cpp** | **D** | **v1 claim was wrong — grep found 8 hits in `gos_terrain_water_stream.cpp`, 2 in `gos_terrain_indirect.cpp`, 1 in `carnage.cpp` (different struct), 1 in `gos_terrain_water_stream.h` (comment)** |
| `mclib/clouds.cpp:289-348` fogRGB consumers (6 hits) | M | grep confirmed 6 hits |
| `mclib/mapdata.cpp:1114` `vertexNum = -1` | M | confirmed |
| `mclib/mapdata.cpp:1119` `vertexNum = topLeftX + topLeftY * realVerticesMapSide` | M | confirmed |
| `mclib/terrain.cpp:595` `Terrain::primeMissionTerrainCache` definition | M | grep confirmed |
| `code/mission.cpp:2240` `land->primeMissionTerrainCache` call | M | grep confirmed |
| `mclib/terrain.cpp:703` `void Terrain::destroy (void)` | M | grep confirmed |
| `mclib/tgl.h:261` `bool TG_Light::GetFalloff(float length, float &falloff)` | M | grep confirmed; definition at :261-275 |
| `mclib/quad.cpp:1347/1500/1654/1808` GetFalloff call sites | M | grep confirmed all 4 |
| `code/mechcmd2.cpp:870` `void __stdcall InitializeGameEngine()` | M | grep confirmed |
| `code/mechcmd2.cpp:1918` `void __stdcall TerminateGameEngine()` | M | grep confirmed |
| **`gos_RendererInit` / `gos_RendererShutdown`** | **NF** | **v1 cited these as real hooks — grep of entire GameOS/ mclib/ code/ returns zero hits. Fictional. Replaced by `InitializeGameEngine`/`TerminateGameEngine`.** |
| `GameOS/gameos/gpu_cull_readback.cpp:230-235` `GL_MAP_READ_BIT|GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT` staging buffer | M | grep confirmed — this IS the precedent for persistent-READ mapping |
| `gos_static_prop_batcher.cpp:641`, `gos_terrain_patch_stream.cpp:269/294`, `gpu_cull_substrate.cpp:115` — WRITE-only persistent maps | M | grep confirmed — all existing persistent maps except readback are WRITE-only |
| `gos_static_prop_batcher.cpp:3464`, `gos_terrain_patch_stream.cpp:1511`, `gos_terrain_indirect.cpp:1679`, `gpu_cull_readback.cpp:469` — `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)` | M | grep confirmed — proven ring-fence pattern used in 4 files |
| `mclib/terrain.cpp:105` `Terrain::realVerticesMapSide` — `static long`, no const cap | M | grep confirmed `long Terrain::realVerticesMapSide = 0` at :105; assigned at :315 (sqrt of vertex count) and :389 (explicit verticesPerMapSide); no upper bound constant exists |
| `memory/substrate_coalesce_sync_point_lesson.md` sync-stall lesson | M | quoted from auto-memory index |
| `memory/water_ssbo_pattern.md` "static recipe + per-frame thin record" | M | quoted from auto-memory index |

---

## What the design does NOT commit to

These remain open and are the only legitimate carve-outs:

1. **Phase 2's choice of (a) joint port / (b) parallel CPU / (c) deferral.** Belongs to a separate Phase 2 design doc after Phase 1 ships.

Everything else in plan v1 that was deferred is now committed. Specifically closed in v2 (previously deferred):
- `TG_Light::GetFalloff` — confirmed two-arg API `(float length, float &falloff)` at `tgl.h:261`; math derived from definition at `:261-275`.
- `Terrain::primeMissionTerrainCache` — confirmed at `terrain.cpp:595`, called from `mission.cpp:2240`.
- `gos_RendererInit/Shutdown` — confirmed NF (fictional); replaced by `InitializeGameEngine`/`TerminateGameEngine` at `mechcmd2.cpp:870/1918` for process-level, `gpu_cull::compute_init` pattern (`mission.cpp:2788`) for per-mission.

---

## Sign-off

**v2 (this revision)** fixes the adversarial-review findings against design doc v1 (commit `63015e2`):
- **C1:** Verification appendix regenerated; `quad.cpp:1341-1343` → `quad.cpp:490-495`; `quad.cpp:1382-1384` → NF (reset is in `terrain.cpp:946-948`).
- **C2:** `gos_RendererInit`/`gos_RendererShutdown` removed (NF); Q5 lifecycle redesigned around `gpu_cull::compute_init()` precedent (`mission.cpp:2788`) and real `InitializeGameEngine`/`TerminateGameEngine` hooks.
- **M1:** lightRGB consumer table corrected; cross-file consumers in `gos_terrain_water_stream.cpp` (8 hits), `gos_terrain_indirect.cpp` (2 hits), `carnage.cpp` (different struct, 1 hit) documented; 1-frame latency note added (MN1).
- **M2:** Persistent-COHERENT-READ-only SSBO replaced by the `gpu_cull_readback.cpp` staging-copy + `glFenceSync`/`glClientWaitSync` pattern (the proven ring-fence precedent in the codebase).
- **MN2:** `TG_Light::GetFalloff` closed as non-open; 4 call sites in quad.cpp + definition in `tgl.h:261` confirmed.
- **MN3:** std430 layout math for `GpuTerrainVertexInput` added to Q6.
- **MN4:** `realVerticesMapSide` dynamic-sizing confirmed; no const cap.

This design doc resolves the 7 open questions surfaced by the plan v1 adversarial review (commit `d2424ef`). Plan v2 should cite this doc for every architectural decision it makes; the plan no longer carries unresolved choices. The adversarial review on plan v2 should be evaluating execution-readiness, not architectural decisions.
