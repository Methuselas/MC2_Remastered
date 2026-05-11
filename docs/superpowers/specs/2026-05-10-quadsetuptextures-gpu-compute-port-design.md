# `quadSetupTextures` GPU-Compute Port — Design Decisions (Stage 0)

> **Companion to** [`docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md`](../plans/2026-05-10-quadsetuptextures-gpu-compute-port.md).
>
> This design doc resolves the 7 open questions surfaced by the adversarial-plan-review pass against plan v1. Each section closes one question with grep evidence, commits to a single architectural choice, and identifies the load-bearing constraint the choice respects. The implementation plan is rewritten (`-v2`) against this design — the plan no longer carries unresolved decisions.

**Worktree:** `claude/parallel-amdahl` (branched from `claude/nifty-mendeleev` @ 7b9ad5f)
**Plan v1 commit:** `d2424ef` (under review; revised plan supersedes)
**Slice 0 recon commit:** `4fa7a9a`

---

## Documentation discipline

Every cited symbol below is grep-verified at write-time per the worktree CLAUDE.md "grep at write-time, not after" rule. The verification appendix at the end lists each citation with M (matches) / D (divergent) / NF (not found) status. The adversarial-plan-review pass on plan v1 caught systematic line-number drift; this design doc cites against the **current** tree state (post-Slice-0 commit `4fa7a9a`).

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

- **`lightRGB`: CPU readback.** All 55 consumers live inside `mclib/quad.cpp` (grep: 55 hits, zero hits outside the file in non-`.codex_tmp_isolate` paths). But the readback is **per-quad inline read into a stack variable**, not a whole-buffer copy — the existing `gVertex[i].argb = vertices[i]->lightRGB` pattern reads through the per-vertex pointer; preserving `vertices[i]->lightRGB` as the canonical CPU-side mirror requires writing the SSBO back to the CPU vertex pool ONCE per frame.
- **`fogRGB`: CPU readback (same buffer).** Consumer split: `mclib/quad.cpp` (most), plus **`mclib/clouds.cpp:289-348` (6 sites)** — cross-file. Refactoring clouds.cpp to read SSBO directly is out-of-scope blast radius; CPU readback is the surgical choice.

Both fields use the **same SSBO output struct** (`GpuTerrainLightingOutput`), uploaded once per frame, read back once per frame, copied into the per-vertex pool.

### Grep evidence

- `grep -c '->lightRGB' mclib/quad.cpp` → 55 hits, all writes inside the lighting block (672-1891) OR reads inside `TerrainQuad::draw` and `TerrainQuad::drawWater`.
- `grep '->lightRGB' mclib/ code/ GameOS/ --include='*.{cpp,h}' -r` → returned 55 hits in `mclib/quad.cpp` and 0 hits in any other live tree file (`.codex_tmp_isolate/` matches are not live code).
- `grep '->fogRGB' mclib/clouds.cpp` → 6 hits at lines 289, 298, 307, 330, 339, 348 — all reads inside cloud render path. Cross-file consumer confirmed.

### Sync-stall avoidance — the load-bearing detail

Per `memory/substrate_coalesce_sync_point_lesson.md`: **"grep `glGetBufferSubData|glReadPixels|glMapBuffer.*GL_MAP_READ_BIT` in the on-only path. Any hit on the hot frame path is suspect."**

The plan v1 codeblock had `glMapBufferRange(..., GL_MAP_READ_BIT)` exactly. That's a stall.

**Resolution: persistent-mapped pipelined SSBO with 2-frame latency.**

Pattern:

```cpp
// At init:
glGenBuffers(2, s_outputSsboRing);              // 2 ring buffers
for (int i = 0; i < 2; ++i) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_outputSsboRing[i]);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, mapSize, nullptr,
                    GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
    s_mappedRing[i] = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, mapSize,
                                       GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT |
                                       GL_MAP_COHERENT_BIT);
}
// Per frame:
int writeSlot = frameIndex % 2;
int readSlot  = (frameIndex - 1 + 2) % 2;
// Bind writeSlot SSBO for compute output; consume readSlot SSBO from CPU.
// First frame: readSlot has uninitialized data — Phase 1 Stage 1 emits a fence,
// CPU consumer waits OR uses CPU-legacy-output for frame 0 only.
```

The 1-frame latency means: CPU lighting computed at frame N is consumed by CPU readers at frame N+1. The frame-1-late-by-one-frame visual is invisible in practice (~16 ms at 60 fps) but the parity gate must compare current-frame GPU output against CURRENT-frame CPU output — so during parity-on mode, we keep the writeSlot synchronous (wait on fence) and accept the stall just for the parity window. Production runs with parity off use the pipelined non-stalling pattern.

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
- **`realVerticesMapSide²` is bounded.** mc2_10 wolfman has `mapData.realVerticesMapSide` at the wolfman-extended value (~250 per side → 62,500 vertices). Even worst-case modded maps fit in ~1 MB of output SSBO. Sparse-vs-dense dispatch is not a perf concern at this scale.
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
- SSBO size: `realVerticesMapSide² * sizeof(GpuTerrainLightingOutput)` = max ~62500 × 8 B = 500 KB. Fits comfortably.
- Compute shader dispatch count: `ceil(numVertices / 64)` workgroups, where `numVertices = realVerticesMapSide²`. The shader-side body branches on `if (vn >= u_numVertices) return;` to bound the upper edge.

---

## Open question 4 — Multi-source `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction

### Decision

**Phase 1 does NOT touch the reduction state (it's water-projection-block only, not lighting-block). Phase 2 must port BOTH writers jointly OR commit to keeping the CPU reduction live in parallel with the GPU output.**

### Grep evidence

- `quad.cpp:1341-1343`: file-scope globals `leastZ`, `mostZ`, `leastW`, `mostW`, `leastWY`, `mostWY` declared.
- `quad.cpp:1382-1384`: reset to `1.0/-1.0/0.0` each frame (probably called from a per-frame init somewhere — grep `extern float leastZ` for callers).
- `quad.cpp:1006-1213`: 4 writers, all inside the water-projection block (one per vertex of the quad). Writes only when `screenPos` is computed (i.e. when `vertices[i]->calcThisFrame & 2 == 0` AND `clipped1 || clipped2`).
- `terrain.cpp:1549-1552`: writers in the non-water terrain projection loop (block at terrain.cpp ~1421 — D1 fast-path vertex-project loop per `vertex_project_loop_d1_asymptotic.md` memory).
- `terrain.cpp:1698-1715`: writers in the legacy fallback projection loop.
- Consumer: `terrain.cpp:1832 eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` — singular consumption point.

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

**Add three lifecycle hooks**, mirroring `gos_terrain_indirect`'s existing pattern:

```cpp
// gos_terrain_lighting.h
namespace gos_terrain_lighting {

void Init(uint32_t maxVertices, uint32_t maxLights);  // process-init: alloc SSBOs, compile shader
void Shutdown();                                       // process-teardown: free GL

void OnMissionLoad(uint32_t numVertices);              // per-mission: resize SSBO if needed,
                                                       //   reset frame counter, clear ring buffers
void OnMissionUnload();                                // per-mission: zero out CPU state,
                                                       //   keep GL allocations

void BeginFrame();                                     // per-frame: advance ring index,
                                                       //   reset accumulators
void PackAndDispatch();                                // per-frame: pack input SSBO,
                                                       //   bind program, glDispatchCompute,
                                                       //   memory barrier, ring advance
void CopyResultsToVertexPool(TerrainQuadPtr quadList,
                             int numberQuads);         // per-frame: read prev-frame SSBO,
                                                       //   memcpy into vertices[i]->lightRGB/fogRGB

bool IsEnabled();
bool IsParityCheckEnabled();
}
```

### Grep evidence

The `gos_terrain_indirect` precedent at `GameOS/gameos/gos_terrain_indirect.h:228-247` (per Slice-0 read pass) has the same shape: `Reset*VBO()` + `Build*VBO()` + per-mission chokepoints `MarkMineDirty()` + `RebuildMineStaticVBOIfDirty()`. The pattern is established.

Memory `water_ssbo_pattern.md` documents the canonical "static recipe + per-frame thin record" model, which is structurally what `OnMissionLoad` + `PackAndDispatch` implement.

### Call-site wiring (specified, not deferred)

- `Init` → `gos_RendererInit` (or wherever GL extensions are initialized first). One call per process.
- `Shutdown` → `gos_RendererShutdown`. One call per process.
- `OnMissionLoad` → `Terrain::primeMissionTerrainCache` (the existing canonical "mission terrain ready" hook used by indirect-terrain dense recipe build).
- `OnMissionUnload` → `Terrain::destroy`.
- `BeginFrame` + `PackAndDispatch` → `terrain.cpp:1788` area, before the `quadSetupTextures` for-loop.
- `CopyResultsToVertexPool` → SAME spot, AFTER `PackAndDispatch`. Reads the readSlot ring buffer (1 frame stale during pipelined mode) and writes into `vertices[i]->lightRGB`/`fogRGB`. Cheap (62,500 vertex × 8 bytes memcpy = ~500 KB/frame).

### Implication for the plan

Plan v2 Phase 1 Stage 1 ships all hooks even when no consumer is wired yet — `OnMissionLoad` allocs, `Shutdown` frees, etc. The plan's File-structure table in v1 missed `OnMissionLoad`/`OnMissionUnload`; plan v2 adds them.

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

- `TG_LIGHT_POINT_GPU` (1): falloffParam = falloff start distance. `GetFalloff` formula: `clamp(1.0 - (length - falloffParam) / (falloffEnd - falloffParam), 0, 1)`. **Open: TG_Light::GetFalloff signature** — needs sub-grep to confirm one-parameter vs multi-parameter falloff. **Plan v2 Stage 0 Step 4** documents this grep.
- `TG_LIGHT_SPOT_GPU` (2): falloffParam = cone-angle cosine. Same falloff formula plus cone-cosine factor.
- `TG_LIGHT_TERRAIN_GPU` (3): falloffParam = terrain-attenuation scalar. Per memory `camera.cpp:723` (light getter).

### Grep evidence

Per `mclib/quad.cpp:1390-1411` (the lighting block per-vertex 0 inner loop in current state):

```cpp
TG_LightPtr thisLight = eye->getTerrainLight(i);
if (thisLight) {
    if ((thisLight->lightType == TG_LIGHT_POINT) ||
        (thisLight->lightType == TG_LIGHT_SPOT) ||
        (thisLight->lightType == TG_LIGHT_TERRAIN)) {
        ...
        float length = vertexToLight.GetApproximateLength();
        float falloff = 1.0f;
        if (thisLight->GetFalloff(length, falloff)) { ... }
    }
}
```

`TG_Light::GetFalloff(length, &falloff)` is the actual API. Returns bool (whether the vertex is within falloff range), writes falloff factor into the output param. Plan v2 Stage 0 Step 4 will grep `TG_Light::GetFalloff` definition to extract the per-type math for shader reproduction.

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
| 5 — Per-mission lifecycle | `Init` / `Shutdown` + `OnMissionLoad` / `OnMissionUnload` + per-frame trio | File-structure table extended; call-site wiring spec'd to existing chokepoints |
| 6 — Flag-bit layout | Full 8-bit layout committed up-front; Phase 1 pack includes Phase 2 bits | Plan v2 Stage 0 Step 5 cites this doc's layout verbatim |
| 7 — Perf gate | Two-stage: dispatch-overhead ≤500 µs + retirement-residual ≤50 µs + net cut ≥3.0 ms | Plan v2 Stage 1 + Stage 3 each carry a measurement requirement, not speculation |

---

## Verification appendix

Each citation grep-verified at write-time (Stage 0 of Phase 2 per user's "2 then 1" decision). Status: M (matches), D (divergent), NF (not found).

| Citation | Status | Note |
|---|---|---|
| `gpu_cull_compute.cpp:145-229` compile helper trio | M | grep confirmed at file:line; read in Slice 0 |
| `mclib/quad.cpp:1341-1343` leastZ etc. globals | M | grep confirmed `extern float leastZ; ...` at 1341 |
| `mclib/quad.cpp:1382-1384` per-frame reset | M | grep confirmed |
| `mclib/quad.cpp:1006-1213` water-block reduction writers | M | grep confirmed (4 writers + reductions inline) |
| `mclib/terrain.cpp:1549-1552` non-water reduction writers | M | grep confirmed (4 writers inline) |
| `mclib/terrain.cpp:1698-1715` legacy fallback reduction writers | M | grep confirmed (4 writers inline) |
| `mclib/terrain.cpp:1832` `eye->setInverseProject(mostZ, leastW, ...)` consumer | D | adversarial review claimed :1832; not directly grep'd this round. Plan v2 should re-grep at write-time. Treat as approximate. |
| `mclib/quad.cpp` `lightRGB` consumer count = 55 in-file | M | grep -c confirmed |
| `mclib/clouds.cpp:289-348` fogRGB consumers | M | grep confirmed 6 hits at 289/298/307/330/339/348 |
| `mclib/mapdata.cpp:1114` `vertexNum = -1` | M | confirmed (cited in MEMORY.md Coordinate Spaces) |
| `mclib/mapdata.cpp:1119` `vertexNum = topLeftX + topLeftY * realVerticesMapSide` | M | confirmed |
| Current `setupTextures` body lives at `quad.cpp:672-1892` (function-close); Setup scope at :672; WaterVertProj :946-1260; Lighting :1266-1891 | M | grep confirmed via `CostSplitSetupTotalScope` / `CostSplitLightingScope` / `CostSplitWaterVertProjScope` symbol search |
| `TG_Light::GetFalloff(length, &falloff)` API shape | D | not grep'd at write-time of this doc; plan v2 Stage 0 Step 4 grep is the closure. |
| `Terrain::primeMissionTerrainCache` chokepoint | D | cited via memory `indirect_terrain_solid_endpoint.md` plan; not directly grep'd this round. Plan v2 Stage 1 Step 4 (mission-init wire) re-verifies. |
| `gos_RendererInit` / `gos_RendererShutdown` chokepoints | D | cited via house-style precedent; not grep'd. Plan v2 verifies. |
| `memory/substrate_coalesce_sync_point_lesson.md` "any hit on hot frame path is suspect" | M | quoted from auto-memory index entry |
| `memory/water_ssbo_pattern.md` "static recipe + per-frame thin record" | M | quoted from auto-memory index entry |

**Divergent (D) items are explicitly flagged for plan v2 to re-grep at write-time, per the documentation discipline rule. The design commits to the named chokepoints; plan v2 cannot ship them as fictional content.**

---

## What the design does NOT commit to

These remain open and are the only legitimate carve-outs:

1. **Phase 2's choice of (a) joint port / (b) parallel CPU / (c) deferral.** Belongs to a separate Phase 2 design doc after Phase 1 ships.
2. **`TG_Light::GetFalloff` exact per-type math.** Plan v2 Stage 0 Step 4 closes this with a definition grep.
3. **`Terrain::primeMissionTerrainCache` and `gos_RendererInit/Shutdown` exact signatures.** Plan v2 verification responsibility.

Everything else in plan v1 that was deferred is now committed.

---

## Sign-off

This design doc resolves the 7 open questions surfaced by the plan v1 adversarial review (commit `d2424ef`). Plan v2 should cite this doc for every architectural decision it makes; the plan no longer carries unresolved choices. The adversarial review on plan v2 should be evaluating execution-readiness, not architectural decisions.

**Next: adversarial re-review of THIS design doc** before plan v2 is written, to catch any new fictional content this doc introduced. Then plan v2.
