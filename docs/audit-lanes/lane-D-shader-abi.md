# Lane D — Shader ABI Audit: SSBO/UBO Bindings, Struct Mirrors, Reflection Goldens

**Audit branch:** `claude/trackv-architecture-integrity-opus-1`  
**Nifty HEAD:** 29aebfe5  
**Date:** 2026-06-01  
**Auditor:** TRACKV-AUDIT-SHADER-ABI-BINDINGS (Sonnet lane)

---

## 1. Binding Map

All bindings are `GL_SHADER_STORAGE_BUFFER` unless noted. Passes that co-execute in the same frame are flagged.

### A. Global / frame-persistent bindings (all render passes)

| Slot | Target | Symbol | Value | File (C++ source) | Pass |
|------|--------|--------|-------|-------------------|------|
| 1 | UBO | `SCENE_DATA_ATTACHMENT_SLOT` | 1 | `gameos.hpp:2833`, `scene.hglsl:4` | All gos_tex/lighted shaders |
| 3 | UBO | `kViewUniformsBinding` | 3 | `RenderCore/ViewUniforms.h:38`, `view_uniforms.hglsl:23` | All view-space passes |
| 20 | SSBO | `LIGHT_DATA_SSBO_BINDING` | 20 | `gameos.hpp:2832`, `lighting.hglsl:15` | All lit passes |

### B. GPU Cull compute passes (C1a / C1b / C2)

| Slot | Target | Symbol | Value | Notes |
|------|--------|--------|-------|-------|
| 8 | SSBO | `RecordsBuf` | 8 | Input: GpuActorRecord array + header |
| 9 | SSBO | `DEBUG_SSBO_BINDING` (C1a) / `VisibleIds` (C1b) | 9 | **Same slot, different shader compile paths via `#ifdef GPU_CULL_C1B_INDIRECT`** — OK |
| 10 | SSBO | `BUCKET_COUNTS_BINDING` | 10 | — |
| 11 | SSBO | `BUCKET_CAPS_BINDING` (C1b input) / `INDIRECT_CMD_BINDING` (patch output) | 11 | **Dual-use: different shader programs, sequential — OK** |
| 12 | SSBO | `ACTOR_VIS_BINDING` | 12 | — |
| 13 | SSBO | `BLOCK_VIS_BINDING` | 13 | — |
| 14 | SSBO | `READBACK_SSBO_BINDING` | 14 | `gpu_cull_readback.h:18` — **COLLISION CANDIDATE — see Finding D-01** |
| 2 | UBO | `CULL_UBO_BINDING` | 2 | `gpu_cull_compute.cpp:49` — UBO target, not SSBO — **see Finding D-02** |
| 7 | SSBO | `CmdToBucket` | literal 7 | `gpu_cull_compute.cpp:1084` |
| 15 | SSBO | `PermutationBuf` | literal 15 | `gpu_cull_patch.comp` |
| 16 | SSBO | `BASE_INSTANCE_SSBO_BINDING` | 16 | `gos_static_prop_batcher.cpp:270` |

### C. Static prop batcher (draw pass)

| Slot | SSBO | Usage |
|------|------|-------|
| 0 | Instances / coalesce instances | Instance data |
| 1 | Colors | Per-instance color |
| 2 | PerType | StaticPropTypeDesc — **SSBO** |
| 3 | ParityOut | Object parity debug |
| 4 | PerDrawData | Per-draw uniforms |
| 5 | MaterialTable | MaterialGpu array |

### D. Mech batcher (draw pass)

| Slot | SSBO | Usage |
|------|------|-------|
| 0 | InstanceBuffer (GpuMechInstance) | |
| 1 | BoneBuffer (GpuMechBone) | |
| 2 | s_mechMaterialSsbo (MaterialGpu) | `gos_mech_batcher.cpp:1690` — **no binding=2 SSBO layout declared in mech.vert/mech.frag; see Finding D-03** |

### E. Terrain passes

| Slot | SSBO | Pass | File |
|------|------|------|------|
| 0 | QuadRecordBuf (tesc) / VertexInputs (lighting comp) / Recipe (solid/water comp) / CardCloud (cardcloud comp) | terrain tesc / lighting / gpu_driven / cardcloud | multiple |
| 1 | RecipeBuf (thin vert) / LightInputs (lighting) / Lighting (solid/water comp) | multiple | multiple |
| 2 | ThinRecordBuf (thin vert/frag, lighting output, HandleLut in solid comp, WaterWindow) | multiple | multiple |
| 3 | ThinOut write (solid/water comp) | gpu_driven_terrain_solid/water | — |
| 5 | WaterRecipeBuf (water_fast vert/mask_water vert/gameos 2744) | water passes | — |
| 6 | WaterThinBuf (water_fast vert) / Header (solid/water comp) / kWaterThinSsboBinding | water passes | `gos_terrain_water_stream.h:109` |
| 7 | PerCmdBuf (water_fast_mdi) / Canary (solid comp) / CmdToBucket (cull_patch) | multiple | — |
| 8 | CmdBuf (solid comp) / RecordsBuf (cull, cull_rollup) | multi | — |
| 9 | SolidWin (solid comp) / DebugOut/VisibleIds (cull) | multi | — |
| 17 | SolidMaskBuf | terrain mask solid | `gameos_graphics.cpp:3652` |
| 18 | WaterMaskBuf | terrain mask water | `gameos_graphics.cpp:3803` |
| 19 | RecipeBuf (mask solid) | terrain mask solid | `gameos_graphics.cpp:3653` |
| 20 | SurfaceVertexBuf | terrain surface | `gameos_graphics.cpp:3099` |
| 21 | SurfaceIndexBuf | terrain surface | `gameos_graphics.cpp:3131` |
| 22 | SurfaceTileBuf | terrain surface | `gameos_graphics.cpp:3108` |

### F. Particle pass

| Slot | SSBO | Value | Notes |
|------|------|-------|-------|
| 14 | GpuParticle SSBO | literal 14 | `gos_particle_bridge.cpp:521` — **COLLISION CANDIDATE — see Finding D-01** |

---

## 2. ABI Mirror Map

| GPU Struct | Shader File | C++ Header | Size Check | offsetof Pins |
|-----------|-------------|------------|------------|---------------|
| GpuActorRecord | gpu_cull.comp (struct mirrors comment) | gpu_cull_record.h | `static_assert(== 64)` | 8 fields pinned |
| GpuActorRecordHeader | gpu_cull.comp | gpu_cull_record.h | `static_assert(== 16)` | yes |
| GpuParticle | particles.hglsl | mclib/particles/spec.h | `static_assert(== 64)` | 8 fields pinned |
| GpuMechInstance | mech.vert (inline struct) | gos_mech_batcher.h | `static_assert(== 64)` | 8 fields pinned |
| GpuMechBone | mech.vert (inline struct) | gos_mech_batcher.h | `static_assert(== 64)` | — |
| GpuTerrainVertexInput | gos_terrain_lighting.comp | gos_terrain_lighting.h | `static_assert(== 32)` | — |
| GpuTerrainLight | gos_terrain_lighting.comp | gos_terrain_lighting.h | `static_assert(== 48)` | — |
| GpuTerrainLightingOutput | gpu_driven_terrain_solid/water.comp | gos_terrain_lighting.h | `static_assert(== 8)` | — |
| MaterialGpu | material_gpu.hglsl (32 B std430) | RenderCore/MaterialGpu.h (baseline passed) | baseline PASSED | — |
| TerrainQuadRecord | gos_terrain.tesc | gos_terrain_patch_stream.h | `static_assert(== 192)` | — |
| TerrainQuadRecipe | gpu_driven_terrain_solid.comp | gos_terrain_patch_stream.h | `static_assert(== 144)` | — |
| TerrainQuadThinRecord | gos_terrain.vert/frag | gos_terrain_patch_stream.h | `static_assert(== 96)`, offsetof(clipPos)==32 | — |
| WaterRecipe | gpu_driven_water.comp | gos_terrain_water_stream.h | `static_assert(== 64)` | — |
| WaterThinRecord | gos_terrain_water_fast*.vert | gos_terrain_water_stream.h | `static_assert(== 48)` | — |
| GpuDrivenBucketHeader | gpu_driven_cmd_patch/solid/water.comp | (inline, 16 B std430) | comment-only | — |
| DrawArraysIndirectCommand | gpu_driven_cmd_patch/solid.comp | (GL spec, 16 B) | comment-only | — |
| TerrainSurfaceVertex | gos_terrain_surface.vert (binding 20) | gos_terrain_surface_schema.h | `static_assert(== 32)` | offsetof(px)==0, offsetof(nx)==16 |
| TerrainSurfaceTile | gos_terrain_surface.vert (binding 22) | gos_terrain_surface_schema.h | `static_assert(== 32)` | offsetof(wp0)==0, offsetof(minU)==16 |
| TerrainSurfaceAdjacency | (future, not consumed by VS yet) | gos_terrain_surface_schema.h | `static_assert(== 16)` | — |
| ViewUniforms | view_uniforms.hglsl (144 B std140) | RenderCore/ViewUniforms.h | comment "144 B" | — |

---

## 3. Findings

### FINDING D-01 — P1: SSBO binding=14 shared between gpu_cull READBACK and particle billboard

**Severity:** P1 (arch violation — no GL-UB because passes are sequential, but there is no documented unbind contract and one side does not restore the slot)

**Evidence:**
- `gpu_cull_readback.h:18` — `constexpr uint32_t READBACK_SSBO_BINDING = 14u`
- `gpu_cull_compute.cpp:953` — `glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 14, ...)`
- `gpu_cull_compute.cpp:1187` — `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0u)` — **cull unbinds to 0 after compute**
- `gos_particle_bridge.cpp:56,521` — `GpuParticle SSBO at binding=14`; `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, s_ssbo)` — **particle bridge does NOT unbind binding=14 after the draw loop**

**Risk:** GL SSBO bindings are context-global. If the particle draw runs first and binding=14 still points to the GpuParticle SSBO when gpu_cull compute runs, the readback SSBO write lands in the particle buffer (corrupt particles, readback returns garbage). If gpu_cull runs first and its unbind-to-0 fires before the particle bind, the particle draw reads an empty SSBO (all particles at origin). The hazard is ordering-dependent. Currently gpu_cull appears to run at the cull/pre-render phase and particles at post-solid, so the ordering likely saves it — but there is no `static_assert`, no binding-restore, and no comment documenting the ordering invariant.

**Recommended slice:** S — add `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0)` at the end of `gos_particle_bridge_flush` (after the draw loop, before the texture restore block), and add a comment documenting the shared-slot contract. Optionally rename READBACK_SSBO_BINDING to avoid collision with particle slot 14.

**SPECULATIVE:** whether the current call ordering is guaranteed is not confirmed from static analysis alone.

---

### FINDING D-02 — P2: CULL_UBO_BINDING=2 numeric collision with SSBO binding=2 (different GL target — not GL-UB, but confusing)

**Severity:** P2 (documentation/clarity gap; different GL buffer targets are independent namespaces, so no actual collision)

**Evidence:**
- `gpu_cull_compute.cpp:49` — `constexpr uint32_t CULL_UBO_BINDING = 2u` → `glBindBufferBase(GL_UNIFORM_BUFFER, 2, s_frustumUbo)` (lines 440, 932)
- `static_prop.vert:57` — `layout(std430, binding = 2) readonly buffer PerType` → bound via `gos_static_prop_batcher.cpp:4389` `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo)`
- `gos_terrain_mask_solid.vert:33`, `gos_terrain_mask_water.vert:41` — `layout(std430, binding = 2) readonly buffer LightingBuf`
- `gos_terrain_patch_stream.cpp:1433` — `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_thinRecordBuf)`

**Risk:** GL separates UBO (`GL_UNIFORM_BUFFER`) and SSBO (`GL_SHADER_STORAGE_BUFFER`) binding point namespaces, so there is no corruption risk. However, when reading the binding map it is easy to confuse slot 2 UBO with the multiple slot 2 SSBOs. Document clearly in the binding registry.

**Recommended slice:** XS — add a comment in `gpu_cull_compute.cpp` at the CULL_UBO_BINDING definition explaining it is a UBO-namespace slot, independent of SSBO namespace slot 2.

---

### FINDING D-03 — P1: mech.vert/mech.frag binding=2 (s_mechMaterialSsbo) has no GLSL layout declaration; relies on implicit binding

**Severity:** P1 (arch violation — missing explicit layout qualifier for mech MaterialTable SSBO)

**Evidence:**
- `gos_mech_batcher.cpp:1690` — `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_mechMaterialSsbo)` with comment "Upload table to SSBO at binding 2"
- `shaders/mech.vert` — no `layout(std430, binding = 2)` buffer declaration; only bindings 0 (InstanceBuffer) and 1 (BoneBuffer) are declared in the vertex shader
- `shaders/mech.frag` — no `layout(std430, binding = 2)` buffer declaration either; the frag uses `u_materialFlags` (a uniform int) but not an SSBO-indexed MaterialGpu table
- `include/material_gpu.hglsl` — only a comment: `//   layout(std430, binding = N) readonly buffer MaterialTable {` — no actual binding, by design
- `tools/shader_reflect/expected/shaders__mech.vert__default.json` and `mech.frag__default.json` — golden does **not** list a binding=2 SSBO

**Risk:** The C++ side binds s_mechMaterialSsbo at slot 2, but no shader currently reads it. The MaterialGpu Mech-2 feature is planned but not landed. The SSBO is uploaded and bound but the shader does not consume it — this is wasted GPU bandwidth and a correctness risk when the consuming shader is added (the binding may be forgotten or mapped to wrong slot at that time).

**Mitigating:** `u_materialIdx` is stored in GpuMechInstance at offset 52 (static_assert pinned) and is passed to the shader as a per-instance field. The actual MaterialGpu lookup is deferred to a future phase.

**Recommended slice:** M — either (a) add a comment in `mech.vert` and `gos_mech_batcher.cpp` documenting that binding=2 is a reserved-but-unused slot pending Mech-2, or (b) defer the SSBO upload+bind until the consuming shader is added. Add a TODO golden entry.

---

### FINDING D-04 — P1: gpu_cull.comp C2_READBACK variant (with `GPU_CULL_C2_READBACK` define) has no reflection golden

**Severity:** P1 (missing golden coverage for an active shader variant that adds binding=14 SSBO)

**Evidence:**
- `shaders/gpu_cull.comp:142-147` — `#ifdef GPU_CULL_C2_READBACK` adds `layout(std430, binding = READBACK_SSBO_BINDING) coherent buffer ReadbackBuf`
- `tools/shader_reflect/expected/shaders__gpu_cull.comp__default.json` — exists (covers C1a default), but no `shaders__gpu_cull.comp__c2_readback.json`
- `gpu_cull_compute.cpp:275` — `GPU_CULL_C2_READBACK` is injected at runtime as a preamble define during shader compilation

**Risk:** If the struct layout of ReadbackBuf changes, reflect.py will not catch it because no golden covers this variant. The C2 path is the production readback path used in normal gameplay.

**Recommended slice:** S — add `shaders__gpu_cull.comp__c2_readback.json` golden by running `reflect.py --defines GPU_CULL_C2_READBACK=1`. Also add C1b variant golden (`GPU_CULL_C1B_INDIRECT`).

---

### FINDING D-05 — P2: Three compute shaders have no reflection golden

**Severity:** P2 (coverage gap; no ABI change risk today, but new field additions will not be caught)

**Evidence — missing goldens:**
- `shaders/cardcloud_sim.comp` (binding=0 CardCloudSimBuf, std430)
- `shaders/gos_terrain_lighting.comp` (bindings 0/1/2 compute pass)
- `shaders/hzb_reduce.frag` (sampler2D uSrc)
- `shaders/ssao.frag` / `shaders/ssao_apply.frag` (sampler-only, lower priority)

**Risk:** Struct changes in `CardCloudSimBuf` or `GpuTerrainLightingOutput` (the latter is a 8-byte struct consumed by 3 compute passes) will go undetected by CI if reflect goldens are not present.

**Recommended slice:** S — run reflect.py on the three compute shaders and commit the resulting goldens.

---

### FINDING D-06 — P3: view_uniforms_contract.frag uses std430 (SSBO) to mirror a std140 UBO — layout mismatch in the fixture itself

**Severity:** P3 (fixture-only, does not affect runtime; but the fixture verifies wrong layout qualifier)

**Evidence:**
- `shaders/include/view_uniforms.hglsl:23` — `layout(binding = 3, std140) uniform ViewUniformsBlock`
- `shaders/fixtures/view_uniforms_contract.frag:13` — `layout(std430, binding = 3) readonly buffer ViewUniformsBlock` — uses `std430` + SSBO
- Comment in fixture: "Binding 3 is fixture-only — not a runtime allocation."

**Risk:** `std430 == std140` for the ViewUniforms struct (mat4 + mat4 + vec4 = all 16B-aligned), so the byte offsets match in practice. However, the fixture validates `std430` member offsets rather than `std140` member offsets. For this specific struct there is no observable difference, but the fixture is semantically wrong and sets a bad precedent for future ViewUniforms fields.

**Recommended slice:** XS/defer — document the equivalence in a comment in the fixture. Note it in the reflect golden.

---

### FINDING D-07 — P0-SPECULATIVE: object-id R32UI attachment uses correct `usampler2D` — PASSED

**Evidence:**
- `shaders/postprocess.frag:21` — `uniform usampler2D u_objectIdTex;` — correctly typed unsigned sampler
- `gos_postprocess.cpp:583` — `glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, ...)` — internal format is unsigned integer
- `gos_postprocess.cpp:1895` — `glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_)` on unit 2
- `gos_postprocess.cpp:1853` — `setInt("u_objectIdTex", 2)` — correct unit assignment
- `shaders/mech.frag:85` — `layout(location=2) out uint v_objectId` — correct uint output to R32UI attachment

**Verdict:** No issue. The R32UI path correctly uses `usampler2D`, integer attachment, and unsigned output. PASSED.

---

### FINDING D-08 — P2: GpuDrivenBucketHeader C++ mirror has no static_assert size pin

**Severity:** P2 (no static_assert protecting the 16 B std430 struct shared across solid/water/cmd_patch shaders)

**Evidence:**
- `gpu_driven_cmd_patch.comp:29`, `gpu_driven_terrain_solid.comp:100`, `gpu_driven_water.comp:98` — all define 16B struct with trailing `uint pad_` field
- No `static_assert(sizeof(GpuDrivenBucketHeader) == 16)` found in any C++ header

**Risk:** If a field is added to the bucket header struct on the C++ side without updating all three compute shaders, the struct stride breaks silently.

**Recommended slice:** XS — add a C++ `struct GpuDrivenBucketHeader { uint32_t count; uint32_t firstIndex; uint32_t baseInstance; uint32_t pad_; };` with `static_assert(sizeof == 16)` in `gos_terrain_indirect.h` or a shared header.

---

### FINDING D-09 — P3: shader_reflect/expected/ contains no goldens for ssao.frag, ssao_apply.frag, hzb_reduce.frag, shadow_screen.frag C2-variant

**Severity:** P3 (low risk; sampler-only shaders, no SSBO layout to drift)

**Evidence:** These files are present in `shaders/` but have no corresponding `.json` in `tools/shader_reflect/expected/`. `hzb_reduce.frag` and `shadow_screen.frag` are pure-sampler; risk is low.

**Recommended slice:** defer — document the gap.

---

## 4. Reflection Coverage Summary

| Coverage area | Status |
|---|---|
| All main vertex/fragment shaders | COVERED (goldens present) |
| GPU cull compute (C1a default) | COVERED |
| GPU cull compute (C1b, C2_READBACK variants) | **MISSING** (Finding D-04) |
| gpu_driven_terrain_solid/water/cmd_patch | COVERED |
| particle_billboard | COVERED (binding=14 confirmed) |
| cardcloud_sim.comp | **MISSING** (Finding D-05) |
| gos_terrain_lighting.comp | **MISSING** (Finding D-05) |
| Fixture: material_gpu_contract | COVERED (baseline PASSED) |
| Fixture: view_uniforms_contract | COVERED (layout qualifier mismatch — Finding D-06, P3) |
| hzb_reduce, ssao, ssao_apply | **MISSING** (Finding D-09, P3) |

---

## 5. Collision Analysis Summary

No hard GL-ABI binding collision was found that causes corruption in currently-shipping code. All numeric collisions between passes are resolved by:
- Different GL buffer targets (UBO vs SSBO namespaces) — D-02
- Disjoint shader programs (C1a vs C1b compile variants) — binding 9/11
- Sequential pass ordering with one side unbinding — binding 14 (but the ordering is undocumented — D-01)

The one architectural gap (D-01) is a latent hazard: if particle_bridge_flush is called before gpu_cull's unbind fires (e.g., due to frame ordering change, future render graph reorder, or test harness), binding=14 points to the wrong buffer in a way the driver will not catch.

---

## 6. Answers to Lane Questions

1. **Unique/documented bindings?** Mostly yes. SSBO and UBO namespaces are independent. Symbolic constants used for global bindings (LIGHT=20, SCENE=1, VIEW=3, READBACK=14). Terrain/cull use mostly literal integers, which is a maintenance debt but not wrong. Some constants (CULL_UBO=2, INDIRECT_CMD=11/BUCKET_CAPS=11) are file-local and not in a shared registry.

2. **Collisions across co-executing passes?** No hard collision confirmed. Binding=14 (READBACK vs particle) is sequential-only safe — undocumented (Finding D-01, P1).

3. **Shader structs mirrored correctly in C++?** Yes for all structs with static_asserts: GpuActorRecord, GpuParticle, GpuMechInstance, GpuMechBone, GpuTerrainVertex/Light/LightingOutput, TerrainQuadRecord/Recipe/ThinRecord, WaterRecipe/ThinRecord, TerrainSurface*. GpuDrivenBucketHeader has no C++ static_assert (Finding D-08, P2).

4. **Reflection goldens updated?** Mostly yes. Gaps: gpu_cull C1b/C2 variants, cardcloud_sim.comp, gos_terrain_lighting.comp (Findings D-04/D-05, P1/P2).

5. **Integer samplers handled correctly?** Yes — `usampler2D` used correctly for GL_R32UI (Finding D-07, PASSED).

6. **Object-ID / R32UI paths using unsigned samplers?** Yes — `postprocess.frag` uses `usampler2D u_objectIdTex`, attachment is `GL_R32UI`, output is `out uint`. PASSED.

7. **Shader feature macros consistent with PipelineDesc/env gates?** `GPU_CULL_C2_READBACK` is injected at runtime from C++ (single source of truth via `gpu_cull_readback.h::READBACK_SSBO_BINDING`). `GPU_CULL_C1B_INDIRECT` follows same pattern. `MC2_OBJECT_ID_BUFFER` gates the mech.frag `v_objectId` output and gos_postprocess FBO setup consistently. No macro drift found.

8. **Shader interfaces bypassing reflection checks?** `gpu_cull.comp` C2/C1b variants bypass reflect.py because reflect.py only covers the default (no-define) variant. This is the main gap (Finding D-04).
