# Unified-Projection F1 Plan Addendum: A-pre Transport Pivot to UBO

**Date:** 2026-05-22
**Status:** AUTHORITATIVE for Tasks 4-6 + 8 + 10-14. Plan v1.1 body retained for context; deltas here take precedence.
**Supersedes:** per-program flat-uniform upload approach described in plan v1.1 §4-5 and spec v2.8 §2.1.1 + §5.1.
**Spec postscript:** `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`

**Transport evolution (three attempts):**
1. std140 UBO at binding=0 (`glBindBufferBase` + `glUniformBlockBinding`): AMD driver does not propagate UBO bindings to tessellation stages. `glGetUniformfv` readback returned zeros; probe showed `compared=0`.
2. Flat uniform `glUniformMatrix4fv(loc, 1, GL_FALSE, data)` after `apply()`: `glGetUniformfv` readback confirmed non-zero matrix in program, no GL error, `curProg` matched `cacheProg`. Yet probe still showed `compared=0`. AMD driver stores the uniform in program-level CPU cache but does not propagate to TES stage memory during execution.
3. Probe SSBO at binding=23 (FINAL, WORKING): Extended the existing 8-counter SSBO to hold 16 additional floats at byte offset 32. Written per-frame via `glBufferSubData` + re-issued `glBindBufferBase`. TES reads via `ssbo_readWorldToClipGL()` helper function. Proven working: `compared > 0` at every frame; transport confirmed.

---

## 1. Trigger

Task 7 probe smoke: `compared = 0` across all frames. `behind_new_only` accumulated 3.5M+ per 60 frames, proving the probe SSBO block executes and `newClip = u_worldToClipGL * vec4(world,1)` runs -- but `newClip.w = 0` for every vertex. Root cause: `u_worldToClipGL` is the zero matrix in the running tessellation program.

Per-program upload (`gos_SetWorldToClipGLProbeOnly(terrainLocs_.program, ...)`) reached ONE specific terrain program. Terrain shaders are built per-material-variant via `getMaterialVariation()`. The tessellator runs DIFFERENT material-variant programs that never received the upload. CPU bit-identity was proven in Task 3 (max_delta=0.0); the break was purely in the binding-transport layer.

---

## 2. What changed in A-pre transport

**Old:** `gos_SetWorldToClipGLProbeOnly(GLuint program, const Stuff::Matrix4D& mat)` -- per-program flat-uniform upload via `glProgramUniformMatrix4fv(program, loc, ...)`. Fragile: caller must know the exact running program; misses any variant not explicitly targeted.

**New (final):** `gos_SetWorldToClipGLProbeUBO(const Stuff::Matrix4D& mat)` -- no program argument. Repackages column-major Stuff to row-major. Writes the 16-float matrix into the probe SSBO at binding=23 (byte offset 32, after the 8 uint32 counters) via `glBufferSubData` + `glBindBufferBase`. Also stores in `gosRenderer::probeWorldToClipGL_[]` for a flat-uniform fallback path (no-op when shader loc=-1). Does NOT write `terrain_mvp_` cache -- A-pre observation-only contract preserved.

**New function:** `unifiedProj_probeSetMatrix(const float* M16)` in `gameosmain.cpp` (non-static, extern-declared in `gameos_graphics.cpp` under `#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE`). Writes `M16` into SSBO at `kProbeSSBOMatrixOffset = 32`, then re-issues `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, s_unifiedProjProbeSSBO)`.

**SSBO buffer size:** expanded from 32 bytes (8 uint32 counters) to 96 bytes (8 uint32 + 16 float).

**Retired symbols:** `gos_SetWorldToClipGLProbeOnly`, `gos_GetTerrainTeseProgram` (free function + `gosRenderer::getTerrainTeseProgram()` method). Removed from `gameos.hpp`, `gameos_graphics.cpp`, and `gamecam.cpp`.

---

## 3. GLSL probe declaration

`gos_terrain.tese` A-pre state: `uniform mat4 u_worldToClipGL` is NOT declared. The probe SSBO at binding=23 is extended:

```glsl
layout(std430, binding = 23) buffer DebugSSBO {
    uint  debugSSBO_counters[8];
    float debugSSBO_matrix[16];  // u_worldToClipGL row-major; see C++ kProbeSSBOMatrixOffset
};
mat4 ssbo_readWorldToClipGL() { /* transpose row-major storage to GLSL col-major */ }
```

The `ssbo_readWorldToClipGL()` helper reads `debugSSBO_matrix[row*4+col]` and assembles a column-major GLSL `mat4` (transposing the C++ row-major storage). This matches the convention used for `terrainMVP` (`GL_FALSE` upload of row-major array).

Stage A shaders (Tasks 10-14) will use a different transport as described in §6; the SSBO matrix slot is an A-pre-only mechanism for the probe.

---

## 4. Producer site

`code/gamecam.cpp` -- the Task 5 block with `gos_GetTerrainTeseProgram()` + `if (terrainProg != 0)` guard replaced with a single unconditional call:

```cpp
gos_SetTerrainMVP(M);
// F1 Stage A-pre Task 7b: parallel probe-only upload via UBO.
gos_SetWorldToClipGLProbeUBO(eye->worldToClipGL());
```

No guard needed: `gos_SetWorldToClipGLProbeUBO` handles the first-call buffer allocation internally; `glBufferSubData` on a bound UBO is safe from the first frame.

---

## 5. Updated A-pre gate (Task 8 amendment)

Smoke run: `MC2_TERRAIN_INDIRECT=0`, mission mc2_01, 30s. Build must have `MC2_UNIFIED_PROJECTION_PARITY_PROBE=ON` in cmake cache.

Pass criteria:
- `event=ssbo_init binding=23` present (SSBO wired)
- Multiple `tag=frame_N` snapshots
- `count_compared > 0` (THE breakthrough indicator -- proves SSBO matrix reaches running tess program)
- `count_nonfinite_new_only == 0`
- `count_nonfinite_both == 0`
- `count_behind_new_only` nonzero is EXPECTED and correct: legacy path uses `abs(clip.w)` making `oldBehind` always false; new path uses raw `newClip.w` so behind-camera vertices accumulate as `behind_new_only`. This is not a failure.

**`max_delta_comparable` will be large at A-pre (hundreds to thousands of NDC units) -- this is EXPECTED and correct.** The delta measures the pipeline difference between legacy projection (axisSwap + perspective divide + screen-space + `mvp` matrix) vs direct clip-space NDC from `u_worldToClipGL`. They converge to ~0 only at Stage A when the legacy multi-step path is replaced. Large delta does NOT indicate a bug at A-pre.

**A-pre verdict:** PASS if `ssbo_init` present + `compared > 0` + `nonfinite_new_only = 0` + `nonfinite_both = 0`.

Failure class: `compared = 0` with nonzero `behind_new_only` = SSBO matrix not reaching TES (matrix reads as zero, all `newClip.w = 0 <= epsilon`). Check: `MC2_TERRAIN_INDIRECT=0` set; SSBO binding re-issued before draw in `unifiedProj_probeSetMatrix`; CMake flag `MC2_UNIFIED_PROJECTION_PARITY_PROBE=ON` present in cache.

---

## 6. Stage A implications (Tasks 10-14)

14 vert + 3 compute/frag shader migrations (Tasks 10-12): replace every `terrainMVP` consumption with UBO block declaration + `u_worldToClipGL * vec4(world,1)` usage. Same matrix name; transport is UBO at `binding = 0`.

Stage A `gos_SetWorldToClipGL` (Task 14): writes BOTH the UBO (for shader reads, same `s_unifiedProjUBO` / `kUnifiedProjUBOBinding`) AND the `terrain_mvp_` cache (for 15+ CPU readers via `gos_GetTerrainMVPMat4()`). Cache write is what distinguishes Stage A from A-pre.

`gos_GetCurrentTerrainProgram()` plumbing from plan v1.1 is no longer needed. The UBO binding-point model eliminates all per-program upload chasing.

---

## 7. Why UBO beats the alternatives

Per-program flat-uniform upload (`glProgramUniformMatrix4fv`) requires the caller to enumerate every active GL program that needs the matrix. Terrain material variants are generated at draw time by `getMaterialVariation()` and their program handles are not exposed to the upload site. Chasing them would require either a program registry (new state) or re-uploading inside the draw loop (invasive). UBO at a fixed binding point is the correct abstraction: the driver propagates the binding to every program that declares the matching block, regardless of how many variants exist. This also aligns with Vulkan-prep discipline (explicit device-mediated binding via `glBindBufferBase` rather than per-program implicit state), and is the correct Stage A end-state -- no second pivot required when moving from A-pre to Stage A.

---

## 8. Promotion plan

This addendum is authoritative. Spec v2.8 sections §2.1.1 and §5.1 are SUPERSEDED for the A-pre transport description; a POSTSCRIPT block has been prepended to the spec pointing here. Plan v1.1 sections for Tasks 4-6 and 8 are retained for task-numbering context but the transport mechanism described here applies. Tasks 10-14 adopt UBO end-state as described in section 6 above.
