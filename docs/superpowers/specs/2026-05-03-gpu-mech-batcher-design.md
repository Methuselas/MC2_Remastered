# GPU Mech Batcher — Design Spec
**Date:** 2026-05-03  
**Status:** Approved for plan-write  
**Worktree:** nifty-mendeleev  
**Author:** brainstorm session + advisor review

---

## 0. Scope and Context

This spec covers `GpuMechBatcher`: a standalone SSBO-driven GPU draw path for MC2's rigid-node mechs. It replaces the per-mech CPU vertex submit (`mechShape->Render(true)`) with instanced GPU draws while keeping the CPU transform pipeline (`TransformMultiShape`) running unchanged through Slice A and B1.

**Out of scope:** CPU transform retirement (Slice B2), GPU skinning (Track D), GV / infantry / building actors, Carver5O/Magic/MCO/Wolfman/MC2X mod content, shadow submission.

---

## 1. Architecture Overview

### Three-slice structure

| Slice | Description | CPU transform | CPU shadow | Visual parity gate |
|---|---|---|---|---|
| **A** | GPU draw substrate: SSBO instance records, bone matrices, per-node shapeToWorld, parity harness | Runs unchanged | Runs unchanged | MC2_MECH_GPU_PARITY=1 zero mismatches |
| **B1** | GPU lighting: wire `lightDataIndex` → `calc_light()` in mech.vert; audit `LightsData[32]` capacity | Runs unchanged | Runs unchanged | Parity extended to lighting output |
| **B2** | CPU transform retirement: minimal hierarchy update path, geometry no longer submitted CPU-side | Retired | Runs unchanged | — |

Each slice ships independently with its own parity gate. Slice A can be flipped off via `g_useGpuMechs` killswitch (matching the `g_useGpuObjects` pattern from `gos_static_prop_batcher.h:62`).

### Standalone batcher

`GpuMechBatcher` is an independent singleton, NOT a subclass of `GpuStaticPropBatcher`. It inherits conventions (ring-buffered SSBOs, std430, static_asserts, parity contract, texture-slot indirection) but differs in enough structure (bone matrices, `mechType × LOD` registration, per-node bone index range) that forcing a shared base class would produce dead weight.

---

## 2. Data Model

### Registration unit: `mechType × LOD`

The stable geometry key is `(mechType->mechShape[lod], lod)`, NOT a live per-instance `mechShape*`. `Mech3DAppearance::recalcBounds()` (mech3d.cpp:2299–2345) can swap `mechShape` to a different LOD pointer mid-session, invalidating any cached instance pointer. Type-level geometry (built from `mechType->mechShape[lod]`) is stable across swaps.

```cpp
struct GpuMechTypeLodRecord {
    uint32_t firstBoneIndex;   // vertex-local bone index namespace base (normally 0 per type/LOD)
    uint32_t numBones;         // count of TG_Shape nodes (u8 assertion: <= 255)
    uint32_t firstPacket;      // into shared packet table
    uint32_t packetCount;
    uint32_t vertexCount;
    const TG_TypeShape* source;
};
```

**u8 bone-count invariant:** `numBones <= 255` must hold for every registered `mechType × LOD`. Validated at registration via `MC2_MECH_NODE_TRACE=1` (see Section 7).

### Vertex format (skinning-ready ABI, immutable VBO)

7 attributes, std430-friendly stride, locked for Slice A and beyond:

| Location | Type | Semantic | Source |
|---|---|---|---|
| 0 | `vec3` | `a_position` | `TG_TypeVertex::pos` |
| 1 | `vec3` | `a_normal` | `TG_TypeVertex::normal` |
| 2 | `vec2` | `a_uv` | baked from `TG_UVData` at registration |
| 3 | `uvec4` | `a_boneIndices` | node bone slot(s); Slice A uses `.x` only |
| 4 | `vec4` | `a_boneWeights` | Slice A: `(1,0,0,0)` rigid; skinning deferred to Track D |
| 5 | `vec2` | `a_tangentOct` | `GL_SHORT / GL_TRUE` normalized; zero-fill for stock content |
| 6 | `uint` | `a_aRGBLight` | `TG_TypeVertex::aRGBLight` |

**No `a_materialSlot` attribute.** Material/packet selection is a CPU flush-grouping concern.

### Per-instance GPU record: `GpuMechInstance`

std430, 48 bytes (static_assert enforced):

```cpp
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  // resolved mechType × currentLOD GPU record index
    uint32_t baseBoneOffset;      // into per-frame bone SSBO
    uint32_t lightDataIndex;      // into LightsData UBO (Slice B1)
    uint32_t renderFlags;         // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    float    aRGBHighlight[4];    // passed to FS via varying (highlight color)
    float    fogRGB[4];           // passed to FS via varying (fog color)
};
// Layout: 16 (4 × uint32) + 16 (vec4) + 16 (vec4) = 48 bytes.
static_assert(sizeof(GpuMechInstance) == 48, "GpuMechInstance must match std430 struct");
```

### Bone matrix: `GpuMechBone`

Explicit row storage to avoid column-major confusion:

```cpp
struct GpuMechBone {
    float row0[4], row1[4], row2[4], row3[4];  // row-major Stuff Matrix4D rows
};
```

Upload Stuff rows as `row0..row3`. In GLSL, `mat4(b.row0, b.row1, b.row2, b.row3)` constructs a matrix whose *columns* are those rows — effectively the transpose needed for GLSL column-vector multiplication. Named `boneT` in the shader to make this intentional; do not "fix" it.

### Draw packet: `GpuMechPacket`

CPU-side only, not uploaded:

```cpp
struct GpuMechPacket {
    uint32_t firstIndex;      // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;      // into shared VBO
    uint32_t textureSlot;     // static material slot index; resolved to live gosTextureHandle at flush
    uint32_t materialFlags;   // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeLodRecord;
};
```

Packets are keyed by static geometry/material slot at registration. Live `gosTextureHandle` resolved at flush time only (MC2 mutates handles per frame — `memory/mc2_texture_handle_is_live.md`).

### Per-actor submit descriptor: `GpuMechSubmitDesc`

Carries all live per-actor context the batcher needs at submit/flush time:

```cpp
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;        // live per-instance shape (shapeToWorld source)
    const Mech3DAppearanceType* mechType;         // stable type pointer for packet/bone lookup
    int                         currentLOD;
    uint32_t                    lightDataIndex;   // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;      // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
    // Texture/paint context: live texture slot indices per packet, resolved at flush.
    // The batcher reads mechType->mechShape[currentLOD]->listOfTypeShapes[i]->listOfTextures
    // at flush time; no cached handles stored in desc.
};
```

### Fallback reason enum: `GpuMechFallbackReason`

```cpp
enum class GpuMechFallbackReason : uint8_t {
    UnregisteredType   = 0,
    U8BoneOverflow     = 1,
    RingOverflow       = 2,
    TglGpuUnsupported  = 3,
    ShaderInitFailure  = 4,
};
```

### SSBO ring layout

Matches `STATIC_PROP_RING_FRAMES = 3` from `gos_static_prop_batcher.h:56`. Defines a private `constexpr uint32_t MECH_RING_FRAMES = 3u;` with a `static_assert` cross-check against `STATIC_PROP_RING_FRAMES` (Option A from the batcher header).

Per-ring frame, double-buffered:
- **Instance SSBO** (binding 0): `GpuMechInstance[]`, compacted at flush
- **Bone SSBO** (binding 1): `GpuMechBone[]`, one entry per node per submitted actor

---

## 3. Registration Flow

`GpuMechBatcher::onMapLoad()` iterates eligible actor types and calls `registerTypeLod(mechType, lod)` for each `Mech3DAppearanceType* × LOD` combination.

**Registration inputs:**
- `mechType->mechShape[lod]` — `TG_TypeMultiShape*`, type-level geometry (stable; declared at mech3d.h:109)
- The multishape's per-node geometry for this LOD

**Registration produces:**
- VBO append: positions, normals, UVs (from `TG_UVData`), bone indices (node index within this type), bone weights (`(1,0,0,0)` for Slice A), tangent oct (zero-fill for stock), aRGBLight
- IBO append: triangle indices
- `GpuMechPacket` entries: one per distinct `(mesh range, textureSlot, materialFlags)` subset
- `GpuMechTypeLodRecord` entry with bone slot range

**`.tglgpu` sidecar:** When present, registration reads the sidecar for tangent + eventual skinning attributes. Header format (20 bytes, locked):

```
magic[8] ("TGLGPU\0\0") + version u16 + headerSize u16 + flags u32 + sourceHash u32
```

Full payload schema deferred to Track D. Absence of `.tglgpu` is the normal stock synthesis path (`tglgpu_absent`), not a fallback. `tglgpu_unsupported` is reserved for future diagnostic use when a sidecar exists but cannot be consumed.

**`finalizeGeometry()`** called at end of registration: uploads the immutable shared VBO/IBO.

**Late registration:** Not supported in Slice A. Any type not registered by `finalizeGeometry()` triggers CPU fallback for that actor every frame. The batcher emits `[MECHBATCHER v1] event=late_register` for unregistered submits.

---

## 4. Per-Frame Pipeline

### Submit phase (inside `*Appearance::render`)

For each visible mech actor:

1. `GpuMechBatcher::recordEligibleActor(pop)` — caller-side accounting before any gating.
2. Look up `typeLodRecord` for `(mechType, currentLOD)`.
3. If not registered → `recordCpuFallback(pop)`, fall through to CPU path.
4. Hoist `lightDataIndex = TG_Shape::GatherGpuObjectLightDataOnly(...)` once per actor (Slice B1; Slice A uses `lightDataIndex = 0`).
5. For each node in `mechShape->listOfShapes`:
   - Copy `shapeToWorld` matrix from `TG_ShapeRec::shapeToWorld` (populated by `TransformMultiShape` earlier in the frame).
   - Stage one `GpuMechBone` record.
6. Stage one `GpuMechInstance` record with `baseBoneOffset`, `lightDataIndex`, `renderFlags`, `aRGBHighlight`, `fogRGB`.
7. Append pending record to submit list.

**Cull gate**: preserve the existing mech visibility/lifecycle gate exactly. Do NOT couple to `g_useGpuStaticProps`. The submit path runs only when the actor would have CPU-rendered.

**Shadow submission**: unchanged in Slice A. `mechShadowShape->RenderShadows()` still runs CPU-side for all slices until an explicit shadow offload slice.

### Flush phase (post `renderLists()`)

Flush MUST be called after `renderLists()` — legacy object/terrain functions queue; `renderLists` flushes. GPU draws placed inside the legacy hook get overwritten (`memory/render_order_post_renderlists_hook.md`).

Flush sequence:

1. Select ring-buffer frame slot (fence sync on oldest slot).
2. **Bucket** pending submit list by draw key: `(typeLodRecordIndex, packetIndex, resolvedTextureHandle, materialFlags)`. Resolve `gosTextureHandle` per actor × packet at this step only.
3. **Write compacted** `GpuMechInstance[]` to current ring-buffer instance SSBO in bucket order.
4. **Write** staged `GpuMechBone[]` to bone SSBO.
5. Set explicit GL state:
   - `glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL)`
   - `glDepthMask(GL_TRUE)`
   - `glDisable(GL_BLEND)` (or match material blend state)
   - Bind session-lifetime sampler: `GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR`
6. For each bucket:
   - Bind `GL_TEXTURE_2D` with resolved live handle.
   - `glUniform1ui(u_instanceBase_loc, bucket.firstInstanceInSSBO)`
   - `glUniform1ui(u_materialFlags_loc, packet.materialFlags)`
   - `glDrawElementsInstanced(GL_TRIANGLES, packet.indexCount, GL_UNSIGNED_INT, firstIndex, instanceCount)`
7. Restore sampler binding.

**`u_instanceBase` and multidraw:** Because Slice A uses a per-draw `u_instanceBase` uniform, each compacted draw group requires a separate draw call. A future `glMultiDrawElementsIndirect` path would need either `gl_BaseInstance` (`ARB_shader_draw_parameters`, not guaranteed on all AMD 7900 XTX drivers at GL 4.3) or a draw-command-indexed indirection table.

---

## 5. GPU Shader Design

### `mech.vert`

**Vertex attributes** — locations 0–6 (skinning-ready, locked ABI):

```glsl
layout(location=0) in vec3  a_position;
layout(location=1) in vec3  a_normal;
layout(location=2) in vec2  a_uv;
layout(location=3) in uvec4 a_boneIndices;   // Slice A: .x only
layout(location=4) in vec4  a_boneWeights;   // Slice A: (1,0,0,0) rigid
layout(location=5) in vec2  a_tangentOct;    // zero-fill for stock
layout(location=6) in uint  a_aRGBLight;
```

**SSBO bindings:**

```glsl
layout(std430, binding=0) readonly buffer InstanceBuffer { GpuMechInstance instances[]; };
layout(std430, binding=1) readonly buffer BoneBuffer     { GpuMechBone     bones[];     };
```

**Per-draw uniforms:**

```glsl
uniform uint u_instanceBase;
uniform uint u_materialFlags;
uniform mat4 u_vp;             // camera VP, uploaded GL_FALSE (row-major)
```

**Transform:**

```glsl
uint idx = u_instanceBase + uint(gl_InstanceID);
GpuMechInstance inst = instances[idx];
GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
// GLSL fills mat4 columns from constructor args; columns = Stuff rows = correct transform.
// Named boneT: intentional transpose — do NOT remove.
vec4 worldPos = boneT * vec4(a_position, 1.0);
vec3 worldNormal = normalize(mat3(b.row0.xyz, b.row1.xyz, b.row2.xyz) * a_normal);
gl_Position = u_vp * worldPos;
```

**Lighting (Slice B1):**

```glsl
// Slice A: base_light from a_aRGBLight decode, no calc_light().
// Slice B1: replace with calc_light(int(inst.lightDataIndex), worldNormal, worldPos.xyz, baseLight)
```

**Varyings to FS:**

```glsl
out vec2 v_uv;
out vec4 v_litColor;      // base light * texture modulate
out vec4 v_highlightColor; // inst.aRGBHighlight decoded
out vec3 v_fogRGB;         // inst.fogRGB decoded
```

FS does NOT read the instance SSBO directly — all per-instance data is forwarded via varyings.

### `mech.frag`

Samples `sampler2D u_tex` at `v_uv`, applies `v_highlightColor` and `v_fogRGB` blend, optionally alpha-tests via `u_materialFlags & ALPHA_TEST_BIT`. Structurally identical to `static_prop.frag` with the fog/highlight varyings substituted in.

**No `#version` directive.** Prefix injected by `makeProgram()` (matching project convention from CLAUDE.md).

---

## 6. Parity Validation

**Dual-FBO design:** CPU and GPU outputs must exist in separate buffers for XOR comparison.

- **CPU reference FBO** (`fbo_mech_parity_cpu`): CPU `mechShape->Render(true)` draws here when `MC2_MECH_GPU_PARITY=1`.
- **GPU candidate FBO** (`fbo_mech_parity_gpu`): `GpuMechBatcher::flush()` draws here.
- **Comparison compute pass**: XORs 32×32 tile hashes from both attachments post-frame. Mismatches → `[MECHBATCHER v1] event=parity_fail tile=(%d,%d) cpu=0x%08x gpu=0x%08x`.

**Parity scope by slice:**

| Slice | Gate |
|---|---|
| Slice A | Draw substrate: position, bone transform, base lit/hot color, texture, fog, highlight |
| Slice B1 | Extends gate to `calc_light()` output — per-actor `lightDataIndex`, `LightsData` UBO read, lit result matches CPU shading |

**Late-reg event counters** — same contract as `GpuStaticPropBatcher`:
- `GpuMechBatcher::getAllowedLateRegEventCount()` — allowlisted late-reg events (e.g., skybox equivalents)
- `GpuMechBatcher::getDisallowedLateRegEventCount()` — non-zero is a STOP signal

Both consumed by `gos_object_parity::ParityFrameTick()`.

---

## 7. Measurement Tasks

Five env-gated loggers, all following the `[SUBSYS v1]` schema convention from CLAUDE.md:

| Env-var | What it measures | Required before |
|---|---|---|
| `MC2_MECH_LOD_TRACE=1` | Per-actor LOD swap frequency in `recalcBounds()` (mech3d.cpp:2299–2345); `[MECHLOD] actor=%p old=%d new=%d frame=%u` | Slice A: quantify swap rate to inform LOD-change invalidation budget |
| `MC2_MECH_NODE_TRACE=1` | Per-`mechType×LOD` node count at registration; validates `numBones <= 255` u8 assertion | Slice A registration |
| `MC2_MECH_GPU_PARITY=1` | Dual-FBO pixel parity; zero mismatches = visual correctness gate | Slice A completion; extended to B1 |
| `MC2_MECH_LIGHT_TRACE=1` | B1 capacity audit: peak `LightsData[32]` slots used, mech-contributed slots, dedup hit rate, near-overflow/overflow frames | Slice B1 gate |
| `MC2_MECH_BATCHER_STATS=1` | `[MECHBATCHER v1] event=summary` per-frame: registered_types, submitted_actors, fallbacks_by_reason, compacted_draws, bone_ssbo_bytes | Slice A→B1 transition |

**CPU fallback criterion:** Zero unexpected fallbacks at Slice A completion. All fallbacks counted by reason:

| Reason | Expected at Slice A | Notes |
|---|---|---|
| `unregistered_type` | 0 | Non-zero is a registration-walk bug |
| `u8_bone_overflow` | 0 | Non-zero means a mech type exceeds 255 nodes |
| `ring_overflow` | 0 | Non-zero means SSBO ring undersized |
| `tglgpu_unsupported` | 0 | Should not appear in stock validation |
| `shader_init_failure` | 0 | Non-zero is a compile/link error |
| `tglgpu_absent` | informational | Normal stock synthesis path — NOT a fallback |

---

## 8. Interface Contract

```cpp
class GpuMechBatcher {
public:
    static GpuMechBatcher& instance();

    void onMapLoad();
    void onMapUnload();

    void registerTypeLod(const Mech3DAppearanceType* mechType, int lod);
    // Internally reads mechType->mechShape[lod] (TG_TypeMultiShape*, stable type-level pointer)
    void finalizeGeometry();

    void recordEligibleActor();
    void recordCpuFallback(GpuMechFallbackReason reason);

    // GpuMechSubmitDesc carries all live per-actor context needed at flush:
    //   mechShape (live TG_MultiShape*), mechType (Mech3DAppearanceType*), currentLOD,
    //   texture/paint context, lightDataIndex, renderFlags, highlightARGB, fogARGB.
    [[nodiscard]] bool submitActor(const GpuMechSubmitDesc& desc);

    void flush();
    void flushShadow();  // no-op in Slice A/B1/B2; reserved for a future shadow-offload slice

    bool wasLastFailureLateRegistration() const;

    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();
};
```

---

## 9. Constraints and Load-Bearing Rules

- **Stock install must remain playable** (`memory/stock_install_must_remain_playable.md`): missing `.tglgpu` sidecars degrade to zero-fill, never fail.
- **Texture handles mutate per-frame** (`memory/mc2_texture_handle_is_live.md`): resolve at flush only, never cache.
- **Flush after `renderLists()`** (`memory/render_order_post_renderlists_hook.md`): mech GPU draws placed inside legacy hook are overwritten.
- **Depth state ownership** (`memory/gpu_direct_depth_state_inheritance.md`): batcher explicitly sets `GL_DEPTH_TEST + GL_LEQUAL + GL_DEPTH_MASK_TRUE`; does not inherit prior state.
- **Blend state ownership** (`memory/blend_state_inheritance_in_post_process.md`): batcher calls `glDisable(GL_BLEND)` before draw; restores after.
- **Sampler state ownership** (`memory/sampler_state_inheritance_in_fast_paths.md`): batcher binds session-lifetime sampler before flush, restores after.
- **Cull gates load-bearing** (`memory/cull_gates_are_load_bearing.md`): preserve existing mech visibility gate; do not add `g_useGpuStaticProps` dependency.
- **LOD swap hazard** (mech3d.cpp:2299–2345): registration uses `mechType->mechShape[lod]` (type-level, stable), never live `mechShape*`.
- **`GpuStaticPropInstance` struct** (gos_static_prop_batcher.h:27–35): `GpuMechInstance` follows identical static_assert discipline with offsetof checks.
- **RING_FRAMES cross-check** (gos_static_prop_batcher.h:56): `MECH_RING_FRAMES = 3u` with static_assert against `STATIC_PROP_RING_FRAMES`.
- **No `#version` in shaders** (CLAUDE.md): prefix via `makeProgram()` only.
- **Deferred uniforms** (`memory/deferred_vs_direct_uniforms.md`): `setFloat/setInt` before `apply()`; `u_instanceBase` set via `glUniform1ui` after shader bind, not via deferred system.
- **Scope: stock missions only** (`memory/feedback_offload_scope_stock_only.md`): parity validates against tier1 stock only.

---

## Appendix: Files to Create / Modify

| Action | File | Notes |
|---|---|---|
| Create | `GameOS/gameos/gos_mech_batcher.h` | `GpuMechBatcher` class, `GpuMechInstance`, `GpuMechBone`, `GpuMechTypeLodRecord`, `GpuMechPacket`, `GpuMechSubmitDesc`, `GpuMechFallbackReason` |
| Create | `GameOS/gameos/gos_mech_batcher.cpp` | Singleton impl, ring-buffer management, registration, flush |
| Create | `shaders/mech.vert` | Rigid-node vertex shader, 7-attribute ABI, `u_instanceBase` pattern |
| Create | `shaders/mech.frag` | Fragment shader, varyings from VS, one bound `sampler2D` |
| Modify | `mclib/mech3d.cpp` | Wire `submitActor()` inside `Mech3DAppearance::render()` gated on `g_useGpuMechs`; add `recordEligibleActor/recordCpuFallback` calls |
| Modify | `code/mission.cpp` | Call `onMapLoad/onMapUnload/finalizeGeometry` at map lifecycle (mirrors `GpuStaticPropBatcher` hookup at mission.cpp:1644, 3042, 3171) |
| Modify | `GameOS/gameos/gos_object_parity.cpp` | Consume `GpuMechBatcher::getAllowedLateRegEventCount/getDisallowedLateRegEventCount` in `ParityFrameTick()` |
