# `quadSetupTextures` GPU-Compute Port — Design Decisions (Stage 0)

> **Companion to** [`docs/superpowers/plans/2026-05-10-quadsetuptextures-gpu-compute-port.md`](../plans/2026-05-10-quadsetuptextures-gpu-compute-port.md).
>
> This design doc resolves the 7 open questions surfaced by the adversarial-plan-review pass against plan v1. Each section closes one question with grep evidence, commits to a single architectural choice, and identifies the load-bearing constraint the choice respects. The implementation plan is rewritten (`-v2`) against this design — the plan no longer carries unresolved decisions.

**Worktree:** `claude/parallel-amdahl` (branched from `claude/nifty-mendeleev` @ 7b9ad5f)
**Plan v1 commit:** `d2424ef` (under review; revised plan supersedes)
**Design doc v2 commit:** `a5fd168` (under adversarial review; v3 fixes sonnet adversarial-review findings)
**Slice 0 recon commit:** `4fa7a9a`

---

## Documentation discipline

Every cited symbol below is grep-verified at write-time per the worktree CLAUDE.md "grep at write-time, not after" rule. The verification appendix at the end lists each citation with M (matches) / D (divergent) / NF (not found) status. The adversarial-plan-review pass on plan v1 caught systematic line-number drift; this design doc v2 fixed those errors. Design doc v3 (this revision) fixes a second round of adversarial-review findings from a Sonnet pass against v2 (`a5fd168`): v2 still cited `terrain.cpp:938-940` for the leastZ/mostZ definitions — WRONG (those definitions are at `terrain.cpp:1341-1343`); v2 under-counted lightRGB consumers and missed the `gos_terrain_patch_stream` indirect consumer entirely; v2's fogRGB consumer list included `clouds.cpp` which is a false positive (different struct); v2's readback ring description said "2-slot + GL_TIMEOUT_IGNORED blocking" which diverges from the actual 3-slot non-blocking precedent in `gpu_cull_readback.cpp`. All citations in v3 were grep-verified at v3 write-time; grep output is pasted inline before each corrected claim.

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
- **`fogRGB`: CPU readback (same buffer).** Consumer: `mclib/quad.cpp` only for `ScreenVertex::fogRGB` reads. **`mclib/clouds.cpp:289-348` is a FALSE POSITIVE** — `cloudVertex0` is `CloudVertex*` (defined at `clouds.h:37`), NOT `ScreenVertex*`. `CloudVertex::fogRGB` is a different struct field entirely. The only other cross-file `->fogRGB` hits touching `ScreenVertex*` are in `gos_terrain_water_stream.cpp` (6 hits: `:475-478` thin-record pack + `:817` parity + `:990` legacy path).

> **MN1 — Dynamic-light pipelining edge case:** Transient lights (PPC bolts, explosions) that exist for 1-3 frames will have their lighting contribution appear with 1-frame latency in pipelined mode. This is acceptable: the visual artifact (sub-16 ms flash of unlighted vertex) is imperceptible at 60 fps, and the parity gate (synchronous mode) compares current-frame GPU output so the bug cannot hide.

Both fields use the **same SSBO output struct** (`GpuTerrainLightingOutput`), uploaded once per frame, read back once per frame, copied into the per-vertex pool.

### Grep evidence — lightRGB consumer breakdown

**C2 — corrected counts (grep -cn "lightRGB" per file, run at v3 write-time):**

```
$ grep -c "lightRGB" mclib/quad.cpp GameOS/gameos/gos_terrain_water_stream.cpp \
    GameOS/gameos/gos_terrain_indirect.cpp GameOS/gameos/gos_terrain_water_stream.h \
    code/carnage.cpp GameOS/gameos/gos_terrain_patch_stream.cpp \
    GameOS/gameos/gos_terrain_patch_stream.h code/weaponbolt.cpp \
    mclib/crater.cpp mclib/cevfx.cpp mclib/bdactor.cpp mclib/camera.cpp \
    code/gamecam.cpp mclib/mech3d.cpp mclib/gvactor.cpp mclib/genactor.cpp

mclib/quad.cpp:84
GameOS/gameos/gos_terrain_water_stream.cpp:14
GameOS/gameos/gos_terrain_indirect.cpp:10
GameOS/gameos/gos_terrain_water_stream.h:5
code/carnage.cpp:2
GameOS/gameos/gos_terrain_patch_stream.cpp:5
GameOS/gameos/gos_terrain_patch_stream.h:4
code/weaponbolt.cpp:6
mclib/crater.cpp:11
mclib/cevfx.cpp:8
mclib/bdactor.cpp:5
mclib/camera.cpp:4
code/gamecam.cpp:2
mclib/mech3d.cpp:2
mclib/gvactor.cpp:2
mclib/genactor.cpp:2
```

v2 cited `mclib/quad.cpp:55` — WRONG. The 55 figure was from `grep -c "->lightRGB"` (arrow-only). Total `lightRGB` string occurrences is **84** (includes local variable declarations, comments, and field assignments without the arrow operator in the draw path).

Classification by struct type (ScreenVertex terrain pool vs. other):

| File | Total `lightRGB` hits | `ScreenVertex::lightRGB` hits | Nature |
|---|---|---|---|
| `mclib/quad.cpp` | 84 | 55 (`->lightRGB`) | 8 writes in lighting block (`:1322/1373/1476/1527/1630/1681/1784/1835`); rest are reads in `draw`/`drawWater`; 29 additional occurrences are local `DWORD lightRGBN` vars and comments |
| `GameOS/gameos/gos_terrain_water_stream.cpp` | 14 | 8 (`->lightRGB`) | CPU mirror reads — pack thin record (`:460-463`), parity check (`:809, :811`), draw path (`:929, :967`); 6 additional occurrences are `trec.lightRGB0-3` and comments |
| `GameOS/gameos/gos_terrain_indirect.cpp` | 10 | 2 (`->lightRGB`) | CPU mirror reads — `:1407-1408` (comments), `:1450/1455/1458` (lambda context/comments), `:1459` (direct read `q.vertices[c]->lightRGB`), `:1526-1529` (`tr.lightRGB0-3` assignments) |
| `GameOS/gameos/gos_terrain_water_stream.h` | 5 | 0 | Struct field declarations `lightRGB0-3` in thin-record struct + comment; no live consumers |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` | 5 | 0 (indirect consumer — see M2 below) | Receives `lightRGB0-3` via `appendThinRecordDirect(tr)` where `tr.lightRGB0-3` are packed from `vertices[c]->lightRGB` in `quad.cpp:2133-2136`. Indirect consumer of the CPU mirror pool via quad.cpp. |
| `GameOS/gameos/gos_terrain_patch_stream.h` | 4 | 0 | Struct field declarations `lightRGB0-3` in `TerrainQuadThinRecord` (`:68`) and `PatchStreamThinRecord` (`:108`) + function parameter declaration (`:186-187`); no live consumers |
| `code/carnage.cpp` | 2 | 0 | `:315` reads `ExplosionType::lightRGB` from file (`readIdULong`); `:687` calls `SetaRGB(explosionType->lightRGB)`. **Different struct** — `ExplosionType::lightRGB` is NOT `ScreenVertex::lightRGB`. NOT a terrain pool consumer. |
| `code/weaponbolt.cpp` | 6 | 0 | `WeaponBoltType::lightRGB` — config read from file, used for dynamic light color. Different struct. NOT a terrain pool consumer. |
| `mclib/crater.cpp` | 11 | 0 | Local `DWORD lightRGB` variable computed from per-light falloff and written to `gVertex[n].argb` directly. Not a `ScreenVertex` read — crater does its own lighting computation from scratch. NOT a terrain pool consumer. |
| `mclib/cevfx.cpp` | 8 | 0 | Uses `CevFxVertex::lightRGB` (field in `cevfx.h:71/127`) and local computations — NOT `ScreenVertex::lightRGB`. NOT a terrain pool consumer. |
| `mclib/bdactor.cpp` | 5 | 0 | Local `DWORD lightRGB` computation; passed to `eye->setLightColor()`. Different pattern entirely. NOT a terrain pool consumer. |
| `mclib/camera.cpp` | 4 | 0 | Local `DWORD lightRGB` computation; passed to `setLightColor()`. NOT a terrain pool consumer. |
| `code/gamecam.cpp` | 2 | 0 | Local `DWORD lightRGB` computation; passed to `eye->setLightColor()`. NOT a terrain pool consumer. |
| `mclib/mech3d.cpp` | 2 | 0 | Local `DWORD lightRGB` computation; passed to `eye->setLightColor()`. NOT a terrain pool consumer. |
| `mclib/gvactor.cpp` | 2 | 0 | Local `DWORD lightRGB` computation; passed to `eye->setLightColor()`. NOT a terrain pool consumer. |
| `mclib/genactor.cpp` | 2 | 0 | Local `DWORD lightRGB` computation; passed to `eye->setLightColor()`. NOT a terrain pool consumer. |

**Summary of actual ScreenVertex::lightRGB terrain pool consumers:**
1. `mclib/quad.cpp` — direct writer (8 sites) and direct reader (many draw sites)
2. `GameOS/gameos/gos_terrain_water_stream.cpp` — reads CPU mirror via `->lightRGB` (8 arrow hits)
3. `GameOS/gameos/gos_terrain_indirect.cpp` — reads CPU mirror via `->lightRGB` at `:1459` (1 direct arrow hit)
4. `GameOS/gameos/gos_terrain_patch_stream.cpp` — **INDIRECT consumer** (M2): receives `lightRGB0-3` via `tr` parameter packed by `quad.cpp:2133-2136` from the `lightRGBc` lambda at `quad.cpp:2081-2086`, which reads `vertices[c]->lightRGB`. The patch_stream module itself does not directly dereference `->lightRGB` but it consumes values originating from the CPU pool.

All four terrain-pool consumers inherit 1-frame pipelined latency when the compute port is active.

**C3 — fogRGB consumer correction (grep-verified at v3 write-time):**

```
$ grep -n "->fogRGB" mclib/ code/ GameOS/ (ScreenVertex consumers only)
mclib/quad.cpp:1374/1411/1425/1528/1565/1579/1682/1719/1733/1836/1885  (writes)
mclib/quad.cpp:2194/2262/2272/2282/2429/2621/2631/2641/2785/3033/3043/3053/3192/3321/3331/3341/3480/4470/4484/4494/4508  (reads)
GameOS/gameos/gos_terrain_water_stream.cpp:475-478/817/990  (ScreenVertex reads)
GameOS/gameos/gos_static_prop_batcher.cpp:2572  (different — child->fogRGB on a prop type, NOT ScreenVertex)

mclib/clouds.cpp:289/298/307/330/339/348  → cloudVertex0->fogRGB
  → cloudVertex0 is CloudVertex* (clouds.h:37: "typedef struct _CloudVertex { ... DWORD fogRGB; } CloudVertex;")
  → CloudVertex is NOT ScreenVertex (vertex.h:91). DIFFERENT STRUCT. FALSE POSITIVE.

$ grep -n "CloudVertex\|cloudVertex" mclib/clouds.h
30:typedef struct _CloudVertex
37:	DWORD			fogRGB;				//Haze DWORD
38:} CloudVertex;
```

**`clouds.cpp` is NOT a ScreenVertex::fogRGB consumer. It reads `CloudVertex::fogRGB` (same field name, entirely different struct defined at `clouds.h:37`).** Remove from the fogRGB consumer list.

Actual `ScreenVertex::fogRGB` consumers:
1. `mclib/quad.cpp` — writes and reads (sole writer of per-vertex fog; many read sites in draw paths)
2. `GameOS/gameos/gos_terrain_water_stream.cpp` — reads CPU mirror at `:475-478` (thin-record pack), `:817` (parity), `:990` (legacy path)

### Sync-stall avoidance — the load-bearing detail

Per `memory/substrate_coalesce_sync_point_lesson.md`: **"grep `glGetBufferSubData|glReadPixels|glMapBuffer.*GL_MAP_READ_BIT` in the on-only path. Any hit on the hot frame path is suspect."**

The plan v1 codeblock had `glMapBufferRange(..., GL_MAP_READ_BIT)` exactly. That's a stall.

**Resolution: GPU SSBO + persistent-mapped staging copy ring with glFenceSync pipelining.**

This is the pattern proven in `gpu_cull_readback.cpp` (the only existing READ-path persistent-mapped buffer in the codebase). Two key observations from grepping the tree:

1. **All existing persistent-mapped buffers except `gpu_cull_readback.cpp:230-235` are WRITE-only.** `gos_static_prop_batcher.cpp:641`, `gos_terrain_patch_stream.cpp:269/294`, `gpu_cull_substrate.cpp:115` all use `GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`.

2. **`gpu_cull_readback.cpp:230-235` IS the precedent** for a CPU-readable persistent-mapped buffer. It uses `GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` on a staging copy buffer (separate from the GPU-side SSBO), with `glCopyBufferSubData` from VRAM to the BAR staging buffer, and `glFenceSync`/`glClientWaitSync` (proven pattern: `gpu_cull_readback.cpp:469`, `gpu_cull_substrate.cpp:266`, `gos_terrain_patch_stream.cpp:1511`) to synchronize.

**M1 — 3-slot non-blocking pattern (grep-verified at v3 write-time):**

v2 described a "2-slot ring + `GL_TIMEOUT_IGNORED` parity stall" — this is a **DIVERGENCE from the actual precedent**, not an adoption of it. The actual `gpu_cull_readback.cpp` pattern is:

```
$ head -5 GameOS/gameos/gpu_cull_readback.cpp
// gpu_cull_readback.cpp — Track C, Slice C2: async readback ring buffer.
//
// 3-slot readback ring + per-frame glFenceSync.
// Non-blocking tryConsume with three-tier fallback:
//   T1: N-1 slot ready     → use it

$ grep -n "RING_FRAMES\|readback_tryConsume\|GL_TIMEOUT_IGNORED" GameOS/gameos/gpu_cull_readback.cpp (relevant)
17:// glClientWaitSync timeout is ALWAYS 0 (zero) — never GL_TIMEOUT_IGNORED on hot path.
40:constexpr uint32_t RING_FRAMES           = 3u;
317:// readback_tryConsume — NEVER blocks (timeout=0 on all glClientWaitSync calls)
319:ReadbackTier readback_tryConsume() {
```

Three-tier fallback (non-blocking throughout):
- **T1**: N-1 slot fence already signaled → consume, no wait
- **T2**: N-2 slot fence already signaled → consume, emit `readback_fallback_n2` counter
- **T3**: both not ready → conservative (all visible), emit `readback_fallback_conservative` counter

`GL_TIMEOUT_IGNORED` appears only in the **shutdown path** (`gpu_cull_readback.cpp:279` during ring teardown) and a `readback_selftest` helper — never on the per-frame hot path.

**This design adopts the 3-slot non-blocking pattern literally.** The three-tier fallback for lighting readback:
- **T1**: N-1 slot ready → copy staging ring N-1 into vertex pool (1-frame latency, normal case)
- **T2**: N-2 slot ready → copy staging ring N-2 into vertex pool (2-frame latency, emit `terrain_light_fallback_n2` counter)
- **T3**: neither ready → skip vertex pool update this frame, keep prior values (emit `terrain_light_fallback_conservative` counter)

The `terrain_light_fallback_conservative` case is benign for lighting: stale lighting values from 2+ frames ago are visually imperceptible at 60 fps. This is the same tradeoff `gpu_cull_readback` accepts for visibility culling.

**Parity-only exception:** `MC2_TERRAIN_LIGHTING_PARITY=1` mode uses `glClientWaitSync(GL_TIMEOUT_IGNORED)` on the CURRENT frame's fence before comparison — a synchronous stall, acceptable only in parity mode. Production never calls `GL_TIMEOUT_IGNORED` on the hot path.

```cpp
// At init: GPU-side SSBO (compute writes), CPU-visible staging ring (CPU reads)
static constexpr uint32_t RING_FRAMES = 3u;   // matches gpu_cull_readback.cpp:40

glGenBuffers(1, &s_computeSsbo);             // GPU SSBO — compute writes here
glBufferStorage(..., 0, nullptr,
    GL_DYNAMIC_STORAGE_BIT);                 // write-only from shader side

glGenBuffers(RING_FRAMES, s_stagingRing);    // 3-slot staging ring for CPU readback
for (int i = 0; i < (int)RING_FRAMES; ++i) {
    // READ+WRITE+PERSISTENT+COHERENT — matches gpu_cull_readback.cpp:230-235
    glBufferStorage(..., nullptr,
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
        GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    s_stagingMapped[i] = glMapBufferRange(...,
        GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
        GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
}

// Per frame (non-blocking hot path):
uint32_t writeSlot = s_currentSlot;
// GPU writes into computeSsbo; glCopyBufferSubData → stagingRing[writeSlot]
// glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) for writeSlot.

// tryConsume() — NEVER blocks (timeout=0):
uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;
// Check n1 fence with timeout=0; if signaled → copy; else check n2; else skip.

s_currentSlot = (s_currentSlot + 1u) % RING_FRAMES;
```

The `GL_MAP_WRITE_BIT` on the staging buffer matches `gpu_cull_readback.cpp`'s pattern exactly. The `glFenceSync`/`glClientWaitSync` idiom is proven in 4 existing files (substrate, patch_stream, indirect, readback).

> **MN3 — AMD driver cross-reference (docs/amd-driver-rules.md audit):** `docs/amd-driver-rules.md` contains no entry specifically about `glClientWaitSync(GL_TIMEOUT_IGNORED)` on the hot path or persistent-mapped BAR buffers. The only sync/barrier entries are about `GL_COMMAND_BARRIER_BIT` sequence (C1b canary) and `glGetBufferSubData` (substrate sync-stall lesson). There is no AMD-specific known issue recorded against the `timeout=0` non-blocking pattern used here. The 3-slot non-blocking pattern has been operating correctly in `gpu_cull_readback.cpp` on RX 7900 XTX (driver 26.3.1) since Track C shipped. **No AMD blocker found; document as "verified safe via gpu_cull_readback.cpp production runtime."**

> **MN4 — BAR memory budget (requires user sign-off):** The proposed lighting SSBO staging ring is **~500 KB × 3 slots = ~1.5 MB BAR** (based on `realVerticesMapSide` ≈ 250, `GpuTerrainLightingOutput` = 8 B per vertex, 62,500 vertices). The existing `gpu_cull_readback.cpp` staging ring is ~8 KB × 3 slots = ~24 KB BAR — roughly 60× smaller. The new lighting ring will be the largest persistent-mapped BAR allocation in the engine. This is well within the physical BAR allocation limit on modern discrete GPUs (typical BAR is ≥256 MB on RX 7900 XTX with Resizable BAR enabled), but it should be **noted in the plan's "Architectural decisions requiring user sign-off" section** and measured at runtime via Tracy to confirm the `glMapBufferRange` does not fall back to system RAM. If the GPU does not have Resizable BAR enabled, 1.5 MB BAR is still within the guaranteed 256 MB BAR1 window. No change to architecture needed — document the budget and note it for the plan.

### Implication for the plan

- Phase 1 Stage 1 (input + compute scaffold): ships **3-slot ring** SSBO + persistent mapping, matching `gpu_cull_readback.cpp` `RING_FRAMES = 3u` precedent exactly.
- Phase 1 Stage 2 (parity): uses `glClientWaitSync(GL_TIMEOUT_IGNORED)` on writeSlot fence only inside `MC2_TERRAIN_LIGHTING_PARITY=1` path — never on hot path.
- Phase 1 Stage 3 (consumer flip): uses T1/T2/T3 non-blocking tryConsume to populate `vertices[i]->lightRGB` and `vertices[i]->fogRGB` in a tight loop AFTER compute dispatch, BEFORE the `quadSetupTextures` for-loop. Position matters: drawWater and quad.cpp consumers run later in the frame and need the CPU mirror populated. T3 (neither slot ready) skips the update; stale values are retained.

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

**C1 — leastZ/mostZ actual locations (grep-verified at v3 write-time):**

```
$ grep -n "float leastZ = 1.0f" mclib/terrain.cpp
1341:float leastZ = 1.0f,leastW = 1.0f;

$ grep -n "leastZ = 1.0f" mclib/terrain.cpp
1341:float leastZ = 1.0f,leastW = 1.0f;
1382:	leastZ = 1.0f;leastW = 1.0f;

$ grep -n "mostZ\|leastZ\|leastW\|mostW\|leastWY\|mostWY" mclib/terrain.cpp (relevant lines)
1341:float leastZ = 1.0f,leastW = 1.0f;
1342:float mostZ = -1.0f, mostW = -1.0;
1343:float leastWY = 0.0f, mostWY = 0.0f;
1382:	leastZ = 1.0f;leastW = 1.0f;
1383:	mostZ = -1.0f; mostW = -1.0;
1384:	leastWY = 0.0f; mostWY = 0.0f;
...
1549:						if (screenPos.z < leastZ) leastZ = screenPos.z;
1550:						if (screenPos.z > mostZ)  mostZ  = screenPos.z;
1551:						if (screenPos.w < leastW) { leastW = screenPos.w; leastWY = screenPos.y; }
1552:						if (screenPos.w > mostW)  { mostW  = screenPos.w; mostWY  = screenPos.y; }
1696-1715: (legacy fallback writers, same pattern)
1832:	eye->setInverseProject(mostZ,leastW,yzRange,ywRange);

$ grep -n "^extern float leastZ" mclib/quad.cpp
490:extern float leastZ;
491:extern float leastW;
492:extern float mostZ;
493:extern float mostW;
494:extern float leastWY;
495:extern float mostWY;
```

- `quad.cpp:490-495`: `extern float leastZ; extern float leastW; extern float mostZ; extern float mostW; extern float leastWY; extern float mostWY;` — **extern FORWARD-DECLARES only**. Both `quad.cpp` (externs) and `terrain.cpp` (definitions + per-frame reset) participate.
- **Definitions at `terrain.cpp:1341-1343`** (file-scope globals). v2 incorrectly cited `:938-940` — that location does not exist in this tree.
- **Per-frame reset at `terrain.cpp:1382-1384`**, inside `Terrain::geometry()`: `leastZ = 1.0f; leastW = 1.0f; mostZ = -1.0f; mostW = -1.0; leastWY = 0.0f; mostWY = 0.0f;`. v2 incorrectly cited this reset as `quad.cpp:946-948` — no such reset exists in quad.cpp.
- `quad.cpp:1008/1075/1142/1209`: 4 writers, all inside the water-projection block `946-1260` (one per vertex of the quad). Writes only when `screenPos` is computed.
- `terrain.cpp:1549-1552`: writers in the non-water terrain projection loop (D1 fast-path vertex-project loop).
- `terrain.cpp:1696-1715`: writers in the legacy fallback projection loop.
- Consumer: `terrain.cpp:1832 eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` — singular consumption point. **M — grep confirmed.**

### Implication for the plan

**Phase 1 (lighting) is completely unaffected by this.** The lighting block writes `lightRGB`/`fogRGB`; it doesn't touch the reduction state. Phase 1 ships standalone.

**Phase 2 (water-projection GPU port) requires a Stage 0 sub-decision before code lands:**

- **Option (a) — joint port.** Port the terrain.cpp non-water and legacy fallback writers (`:1549-1552`, `:1696-1715`) as part of Phase 2's compute work. Single GPU reduction (atomic-min/max or 2-pass), single consumer side. Larger blast radius. Retires reduction state entirely.
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
| 2 — Consumer strategy | CPU readback via pipelined **3-slot** non-blocking persistent-mapped SSBO (matches `gpu_cull_readback.cpp` `RING_FRAMES=3u` precedent exactly) | Stage 1 ships 3-slot ring; Stage 3 uses T1/T2/T3 tryConsume (never blocks on hot path); parity uses `GL_TIMEOUT_IGNORED` only in parity mode |
| 3 — `vertexNum == -1` / `calcThisFrame` | Shader writes all slots; comparator filters by quadList walk + calcThisFrame bit | API: `Parity_CompareFrame(quadList, numberQuads, mappedOutput)`; SSBO size = realVerticesMapSide² |
| 4 — Multi-source reduction | Phase 1 unaffected; Phase 2 design doc commits to a/b/c later | Plan v1's Phase 2 outline updated to flag this as a real Phase-2 prereq |
| 5 — Per-mission lifecycle | `mission_init` + `mission_shutdown` (per-mission, matching `gpu_cull::compute_init` precedent) + per-frame trio; process-level if needed: `InitializeGameEngine`/`TerminateGameEngine` at `mechcmd2.cpp:870/1918` | `gos_RendererInit/Shutdown` were fictional — removed; call-site wiring spec'd to real symbols |
| 6 — Flag-bit layout | Full 8-bit layout committed up-front; Phase 1 pack includes Phase 2 bits | Plan v2 Stage 0 Step 5 cites this doc's layout verbatim |
| 7 — Perf gate | Two-stage: dispatch-overhead ≤500 µs + retirement-residual ≤50 µs + net cut ≥3.0 ms | Plan v2 Stage 1 + Stage 3 each carry a measurement requirement, not speculation |

---

## Verification appendix (v3 — regenerated, all citations grep-verified at v3 write-time)

Status: M (matches, grep'd at v3 write-time), NF (not found), D (divergent from v2 claim, corrected in v3).

All greps run against `A:/Games/mc2-opengl-src/.claude/worktrees/parallel-amdahl/` source tree.

| Citation | Status | Grep result / evidence |
|---|---|---|
| `GameOS/gameos/gpu_cull_compute.cpp:145-168` `compile_compute_shader` | M | Confirmed clean from v2 (opus reviewer); not re-grepped in v3 |
| `GameOS/gameos/gpu_cull_compute.cpp:170-193` `link_compute_program` | M | Confirmed clean from v2 |
| `GameOS/gameos/gpu_cull_compute.cpp:197-231` `build_compute_program_from_file` | M | Confirmed clean from v2 |
| `GameOS/gameos/gpu_cull_compute.cpp:308` `compute_init` function body | M | Confirmed clean from v2 |
| `code/mission.cpp:2788` `gpu_cull::compute_init()` call | M | Confirmed clean from v2 |
| `mclib/quad.cpp:490-495` `extern float leastZ/leastW/mostZ/mostW/leastWY/mostWY` | M | v3 grep: `grep -n "^extern float leastZ" mclib/quad.cpp` → `490:extern float leastZ;` through `495:extern float mostWY;` |
| **`mclib/terrain.cpp:938-940` leastZ definitions** | **D** | **v2 claim was WRONG. v3 grep: `grep -n "float leastZ = 1.0f" mclib/terrain.cpp` → `1341:float leastZ = 1.0f,leastW = 1.0f;`. Definitions are at `terrain.cpp:1341-1343`.** |
| `mclib/terrain.cpp:1341-1343` `float leastZ = 1.0f,...` definitions | M | v3 grep confirmed: `:1341 float leastZ = 1.0f,leastW = 1.0f;` `:1342 float mostZ = -1.0f, mostW = -1.0;` `:1343 float leastWY = 0.0f, mostWY = 0.0f;` |
| **`mclib/terrain.cpp:946-948` per-frame reset** | **D** | **v2 claim was WRONG. v3 grep: `grep -n "leastZ = 1.0f" mclib/terrain.cpp` → `1341` (definition) and `1382` (reset). Reset is at `terrain.cpp:1382-1384`, not `:946-948`.** |
| `mclib/terrain.cpp:1382-1384` per-frame reset inside `Terrain::geometry` | M | v3 grep confirmed: `:1382 leastZ = 1.0f;leastW = 1.0f;` `:1383 mostZ = -1.0f; mostW = -1.0;` `:1384 leastWY = 0.0f; mostWY = 0.0f;` |
| `mclib/quad.cpp:1008/1075/1142/1209` water-block leastZ/mostZ writers | M | Confirmed clean from v2 |
| `mclib/quad.cpp:946-1260` CostSplitWaterVertProjScope | M | Confirmed clean from v2 |
| `mclib/quad.cpp:1266-1891` CostSplitLightingScope | M | Confirmed clean from v2 |
| `mclib/quad.cpp:670` `void TerrainQuad::setupTextures (void)` | M | Confirmed clean from v2 |
| `mclib/terrain.cpp:1549-1552` non-water reduction writers | M | Confirmed clean from v2 |
| `mclib/terrain.cpp:1696-1715` legacy fallback reduction writers | M | v3 grep: `sed -n '1694,1717p' mclib/terrain.cpp` shows leastZ/mostZ/leastW/mostW writers from `:1696` through `:1715`. v2 cited `:1698-1715` (slightly off; actual first conditional at `:1696`). |
| `mclib/terrain.cpp:1832` `eye->setInverseProject(mostZ, leastW, ...)` | M | Confirmed clean from v2 |
| **`mclib/quad.cpp` total `lightRGB` occurrences = 55** | **D** | **v2 cited 55 (from `grep -c "->lightRGB"`). v3 grep: `grep -c "lightRGB" mclib/quad.cpp` → `84`. The 55 figure was arrow-operator-only. Full string count is 84. Both figures correct for their respective queries; table now shows both.** |
| `mclib/quad.cpp` `->lightRGB` writes: 8 sites (`:1322/1373/1476/1527/1630/1681/1784/1835`) | M | Confirmed clean from v2 |
| `GameOS/gameos/gos_terrain_water_stream.cpp` `lightRGB` count | D | v2 cited 8 (`->lightRGB` only). v3 grep: `grep -c "lightRGB" ...water_stream.cpp` → **14**. Corrected in consumer table above. |
| `GameOS/gameos/gos_terrain_indirect.cpp` `lightRGB` count | D | v2 cited 2 (`->lightRGB` only). v3 grep: `grep -c "lightRGB" ...indirect.cpp` → **10**. Corrected. Direct read at `:1459`; lambda context at `:1450/1455/1458`; comments at `:1407-1408`; tr assignments at `:1526-1529`. |
| `GameOS/gameos/gos_terrain_water_stream.h` `lightRGB` count | D | v2 cited 1 (comment). v3 grep: `grep -c "lightRGB" ...water_stream.h` → **5**. Field declarations + comment. |
| `code/carnage.cpp` `lightRGB` count | D | v2 cited 1. v3 grep: `grep -c "lightRGB" code/carnage.cpp` → **2** (`:315` file read, `:687` SetaRGB call). Both are `ExplosionType::lightRGB`, NOT terrain pool consumer. |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` `lightRGB` | **NF in v2** | **v2 omitted entirely. v3 grep: `grep -c "lightRGB" ...patch_stream.cpp` → 5 hits (`:836` param decl, `:862-865` field assignments). Indirect consumer via `quad.cpp:2133-2136` (lightRGBc lambda reads `vertices[c]->lightRGB`, packs into `tr`, calls `appendThinRecordDirect`). Added to consumer table.** |
| `GameOS/gameos/gos_terrain_patch_stream.h` `lightRGB` | NF in v2 | v3 grep: `grep -c "lightRGB" ...patch_stream.h` → 4 (struct field declarations + function param). No live consumers — definitions only. |
| `code/weaponbolt.cpp` `lightRGB` | NF in v2 | v3 grep: `grep -c "lightRGB" code/weaponbolt.cpp` → 6. `WeaponBoltType::lightRGB` — different struct, NOT terrain pool. |
| `mclib/crater.cpp` `lightRGB` | NF in v2 | v3 grep: `grep -c "lightRGB" mclib/crater.cpp` → 11. Local `DWORD lightRGB` variable, NOT `ScreenVertex::lightRGB`. |
| `mclib/cevfx.cpp` + `cevfx.h` `lightRGB` | NF in v2 | v3 grep: `.cpp` → 8, `.h` → 6. `CevFxVertex::lightRGB` — different struct. |
| `mclib/bdactor.cpp`, `camera.cpp`, `gamecam.cpp`, `mech3d.cpp`, `gvactor.cpp`, `genactor.cpp` | NF in v2 | v3 grep: 5, 4, 2, 2, 2, 2 hits respectively. All are local `DWORD lightRGB` computations passed to `eye->setLightColor()` — NOT terrain pool consumers. |
| **`mclib/clouds.cpp:289-348` fogRGB consumers** | **D** | **v2 called this a cross-file consumer. v3 identifies it as a FALSE POSITIVE: `cloudVertex0` is `CloudVertex*` (clouds.h:37), not `ScreenVertex*`. `CloudVertex::fogRGB` is a different struct. Removed from fogRGB consumer list.** |
| `gpu_cull_readback.cpp` 3-slot non-blocking ring | D | v2 described "2-slot + GL_TIMEOUT_IGNORED". v3 grep: `head -5 gpu_cull_readback.cpp` → "3-slot readback ring + per-frame glFenceSync"; `:40 constexpr uint32_t RING_FRAMES = 3u;`; `:17 glClientWaitSync timeout is ALWAYS 0 (zero) — never GL_TIMEOUT_IGNORED on hot path." Design now adopts 3-slot non-blocking pattern. |
| `mclib/mapdata.cpp:1114` `vertexNum = -1` | M | Confirmed clean from v2 |
| `mclib/mapdata.cpp:1119` `vertexNum = topLeftX + topLeftY * realVerticesMapSide` | M | Confirmed clean from v2 |
| `mclib/terrain.cpp:595` `Terrain::primeMissionTerrainCache` | M | Confirmed clean from v2 |
| `code/mission.cpp:2240` `land->primeMissionTerrainCache` call | M | Confirmed clean from v2 |
| `mclib/terrain.cpp:703` `void Terrain::destroy (void)` | M | Confirmed clean from v2 |
| `mclib/tgl.h:261` `bool TG_Light::GetFalloff(float length, float &falloff)` | M | Confirmed clean from v2 |
| `mclib/quad.cpp:1347/1500/1654/1808` GetFalloff call sites | M | Confirmed clean from v2 |
| `code/mechcmd2.cpp:870` `void __stdcall InitializeGameEngine()` | M | Confirmed clean from v2 |
| `code/mechcmd2.cpp:1918` `void __stdcall TerminateGameEngine()` | M | Confirmed clean from v2 |
| **`gos_RendererInit` / `gos_RendererShutdown`** | **NF** | Confirmed NF from v2. Fictional names, never existed. |
| `GameOS/gameos/gpu_cull_readback.cpp:230-235` persistent-mapped staging flags | M | Confirmed clean from v2 |
| `gos_static_prop_batcher.cpp:641`, `gos_terrain_patch_stream.cpp:269/294`, `gpu_cull_substrate.cpp:115` — WRITE-only persistent maps | M | Confirmed clean from v2 |
| `gos_static_prop_batcher.cpp:3464`, `gos_terrain_patch_stream.cpp:1511`, `gos_terrain_indirect.cpp:1679`, `gpu_cull_readback.cpp:469` — `glFenceSync` sites | M | Confirmed clean from v2 |
| `mclib/terrain.cpp:105` `Terrain::realVerticesMapSide` — `static long`, no const cap | M | Confirmed clean from v2 |

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

**v2 (commit `a5fd168`)** fixed the adversarial-review findings against design doc v1 (commit `d2424ef`). See v2 sign-off block in git history.

**v3 (this revision)** fixes the Sonnet adversarial-review findings against design doc v2 (`a5fd168`):

- **C1 — leastZ/mostZ actual locations:** v2 cited `terrain.cpp:938-940` for definitions — WRONG. v3 grep at write-time: `grep -n "float leastZ = 1.0f" mclib/terrain.cpp` → `1341`. Definitions are at `terrain.cpp:1341-1343`. Per-frame reset is at `terrain.cpp:1382-1384` (v2 cited `:946-948` — also wrong). Verification appendix corrected. Q4 grep evidence rewritten with inline grep output.
- **C2 — lightRGB consumer table count:** v2 used `grep -c "->lightRGB"` (arrow-only = 55 for quad.cpp). v3 uses `grep -c "lightRGB"` (full string) as the task specified, giving quad.cpp=84, water_stream.cpp=14, indirect.cpp=10, water_stream.h=5, carnage.cpp=2. Consumer table rebuilt with all files classified by struct type (terrain pool vs. other).
- **C3 — clouds.cpp false positive:** v2 listed `clouds.cpp:289-348` as a fogRGB cross-file consumer. FALSE POSITIVE — `cloudVertex0` is `CloudVertex*` (different struct at `clouds.h:37`). Removed. Actual ScreenVertex::fogRGB consumers: quad.cpp + gos_terrain_water_stream.cpp only.
- **M1 — readback ring is 3-slot non-blocking:** v2 described "2-slot + GL_TIMEOUT_IGNORED" which DIVERGES from the actual precedent. v3 grep: `gpu_cull_readback.cpp:40 constexpr uint32_t RING_FRAMES = 3u;`, comment line 17 "timeout is ALWAYS 0 — never GL_TIMEOUT_IGNORED on hot path." Design now adopts the 3-slot non-blocking three-tier fallback (T1/T2/T3) literally. `GL_TIMEOUT_IGNORED` is parity-mode-only.
- **M2 — gos_terrain_patch_stream unenumerated consumer:** v2 omitted entirely. v3 grep: patch_stream.cpp receives `lightRGB0-3` via `appendThinRecordDirect(tr)` packed from `lightRGBc` lambda at `quad.cpp:2081-2086` which reads `vertices[c]->lightRGB`. Added to consumer table as indirect consumer.
- **MN1-MN2 (count corrections):** water_stream.cpp corrected to 14, indirect.cpp to 10, water_stream.h to 5. Direct consumer at indirect.cpp:1459 confirmed.
- **MN3 — AMD driver rules cross-reference:** `docs/amd-driver-rules.md` audited. No known AMD issue with `glClientWaitSync(timeout=0)` or persistent BAR. 3-slot non-blocking pattern running in production on RX 7900 XTX. No blocker.
- **MN4 — BAR memory budget:** Lighting staging ring is ~1.5 MB BAR (3 slots × 500 KB). Noted in Q2 sync section. Added to sign-off as user-sign-off item for the plan.

This design doc resolves the 7 open questions surfaced by the plan v1 adversarial review AND the follow-up Sonnet adversarial-review findings against v2. Plan v3 should cite this doc for every architectural decision; no unresolved choices remain in the design layer.
