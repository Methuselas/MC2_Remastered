# GPU Mech Batcher — Implementation Plan (Slice A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `mechShape->Render(true)` per-mech CPU vertex submit with SSBO-driven instanced GPU draws via a new standalone `GpuMechBatcher`, while keeping CPU transform (`TransformMultiShape`) and CPU shadow submission unchanged.

**Architecture:** Per-map registration builds an immutable shared VBO/IBO from `Mech3DAppearanceType × LOD` type-level geometry (stable across LOD swaps). Per-frame `submitActor()` captures live per-actor bone matrices and texture handles into a pending list. `flush()` builds a bucket-sorted draw list keyed by `(typeLodIdx, globalPacketIdx, resolvedTexHandle, materialFlags)`, writes ring-buffered SSBOs in bucket order, and issues one `glDrawElementsInstancedBaseVertex` per bucket.

**⚠️ Advisor scope-change note — parity gate weakened:** The design spec (Section 6) calls for dual-FBO `MC2_MECH_GPU_PARITY=1` automated zero-mismatch parity as the Slice A gate. This plan intentionally defers dual-FBO parity to Slice A+ and substitutes operator visual observation plus `[MECHBATCHER v1] event=summary fallback_total=0` as the Slice A gate. **Advisor explicit sign-off required** on this scope reduction before execution. If the gate must remain automated zero-mismatch, add dual-FBO parity tasks (estimated +3 tasks) before executing this plan.

**Tech Stack:** OpenGL 4.3 core, GLSL 430, GL_ARB_buffer_storage (persistent mapping), `glsl_program::makeProgram()`, Stuff LinearMatrix4D, TG_TypeShape/TG_MultiShape/TG_ShapeRec, mech3d.cpp Mech3DAppearanceType.

**Spec:** `docs/superpowers/specs/2026-05-03-gpu-mech-batcher-design.md`

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Create | `GameOS/gameos/gos_mech_batcher.h` | All public types: `GpuMechVertex` (48B), `GpuMechInstance` (48B), `GpuMechBone`, `GpuMechTypeLodRecord`, `GpuMechPacket`, `GpuMechSubmitDesc`, `GpuMechFallbackReason`, `GpuMechBatcher` class |
| Create | `GameOS/gameos/gos_mech_batcher.cpp` | Full singleton impl: registration, ring SSBOs, submitActor, flush |
| Create | `GameOS/gameos/gos_mech_killswitch.h` | `g_useGpuMechs` extern + `gos_GetGLTextureId` forward (mirrors `gos_static_prop_killswitch.h`) |
| Create | `shaders/mech.vert` | Vertex shader: bone transform, D3D projection chain, aRGBLight decode, varyings to FS |
| Create | `shaders/mech.frag` | Fragment shader: texture sample + highlight/fog blend |
| Modify | `mclib/mech3d.cpp` | Wire `submitActor()` in `Mech3DAppearance::render()` gated on `g_useGpuMechs`; add eligible/fallback accounting |
| Modify | `mclib/txmmgr.cpp` | Wire `GpuMechBatcher::instance().flush()` after the static-prop flush at line ~1483 (inside `renderLists()`); same pattern as `GpuStaticPropBatcher::instance().flush()` |
| Modify | `code/mission.cpp` | `onMapLoad()` at line 1644, `finalizeGeometry()` at line 3042, `onMapUnload()` at line 3171 |
| Modify | `GameOS/gameos/gos_object_parity.cpp` | Consume `GpuMechBatcher::getAllowed/DisallowedLateRegEventCount()` in `ParityFrameTick()` |

---

## Task 1: Header — all types and class declaration

**Files:**
- Create: `GameOS/gameos/gos_mech_batcher.h`
- Create: `GameOS/gameos/gos_mech_killswitch.h`

- [ ] **Step 1.1: Write `gos_mech_killswitch.h`**

```cpp
// GameOS/gameos/gos_mech_killswitch.h
#pragma once
#include <cstdint>

// Runtime toggle for the GPU mech renderer.
// Default: enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M (wire in gameosmain.cpp hotkey handler).
extern bool g_useGpuMechs;

// Resolve a gosTextureHandle to the underlying raw GL texture name.
// Returns 0 if handle is INVALID_TEXTURE_ID or gosTexture is gone.
// Implemented in gameos_graphics.cpp (same as gos_static_prop_killswitch.h).
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Terrain projection chain uniforms — same as gos_static_prop_killswitch.h.
const float* gos_GetTerrainViewportVec4();
const float* gos_GetProj2ScreenMat4();
```

- [ ] **Step 1.2: Write `gos_mech_batcher.h`**

```cpp
// GameOS/gameos/gos_mech_batcher.h
#pragma once

#include <cstdint>
#include <cstddef>    // offsetof
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"
#include "mech3d.h"

// CPU-side vertex layout — packed 48 bytes for VBO storage.
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert ATTRIBUTE LAYOUT IN LOCKSTEP.
struct GpuMechVertex {
    float    position[3];    // 12B — a_position  (loc 0, GL_FLOAT)
    float    normal[3];      // 12B — a_normal     (loc 1, GL_FLOAT)
    float    uv[2];          //  8B — a_uv         (loc 2, GL_FLOAT)
    uint8_t  boneIndices[4]; //  4B — a_boneIndices (loc 3, GL_UNSIGNED_BYTE, IPointer -> uvec4)
    uint8_t  boneWeights[4]; //  4B — a_boneWeights (loc 4, GL_UNSIGNED_BYTE, GL_TRUE -> vec4 [0,1])
    int16_t  tangentOct[2];  //  4B — a_tangentOct  (loc 5, GL_SHORT, GL_TRUE -> vec2 [-1,1])
    uint32_t aRGBLight;      //  4B — a_aRGBLight   (loc 6, GL_UNSIGNED_INT, IPointer -> uint)
};                           // 48B total
static_assert(sizeof(GpuMechVertex) == 48, "GpuMechVertex must be 48 bytes");
static_assert(offsetof(GpuMechVertex, position)    ==  0, "position offset");
static_assert(offsetof(GpuMechVertex, normal)      == 12, "normal offset");
static_assert(offsetof(GpuMechVertex, uv)          == 24, "uv offset");
static_assert(offsetof(GpuMechVertex, boneIndices) == 32, "boneIndices offset");
static_assert(offsetof(GpuMechVertex, boneWeights) == 36, "boneWeights offset");
static_assert(offsetof(GpuMechVertex, tangentOct)  == 40, "tangentOct offset");
static_assert(offsetof(GpuMechVertex, aRGBLight)   == 44, "aRGBLight offset");

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

// Bone matrix: 4 explicit rows to avoid GLSL column-major confusion.
// Upload rows as row0..row3; GLSL mat4(row0,row1,row2,row3) fills COLUMNS from
// these, making boneT the transpose of the Stuff LinearMatrix4D —
// boneT * vec4(pos,1) == row-vector math. Named boneT in shader; do NOT remove.
struct GpuMechBone {
    float row0[4], row1[4], row2[4], row3[4];
};
static_assert(sizeof(GpuMechBone) == 64, "GpuMechBone must be 64 bytes");

// Per mechType×LOD registration record (CPU-side only).
struct GpuMechTypeLodRecord {
    uint32_t firstBoneIndex;   // vertex-local bone index namespace base (normally 0)
    uint32_t numBones;         // count of SHAPE_NODE children; must be <= 255
    uint32_t firstPacket;      // into s_packets table
    uint32_t packetCount;
    uint32_t vertexCount;      // total triangle-soup vertices across all nodes
    const TG_TypeShape* sourceNode0; // for late-reg logging
};

// Per-node per-texture-group draw record (CPU-side only, not uploaded).
struct GpuMechPacket {
    uint32_t firstIndex;         // index into shared IBO (in indices, not bytes)
    uint32_t indexCount;
    int32_t  baseVertex;         // signed; passed to glDrawElementsInstancedBaseVertex
    uint32_t textureSlot;        // index into owning TG_TypeShape::listOfTextures.
                                 // Slot 0 is per-actor (paint scheme); resolved from
                                 // GpuMechSubmitDesc::slot0TexHandle at submitActor time.
                                 // Slots 1+ are type-stable; resolved from owningTypeShape.
    uint32_t materialFlags;      // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeLodRecord;
    uint32_t nodeLocalIndex;     // which bone index within the type (index into listOfShapes)
    const TG_TypeShape* owningTypeShape; // type-level shape for slots 1+ texture resolution
};

// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;      // live per-instance shape (shapeToWorld only)
    const Mech3DAppearanceType* mechType;       // stable type pointer
    int                         currentLOD;
    uint32_t                    slot0TexHandle; // gos handle for texture slot 0 (per-actor paint
                                               // scheme; mech3d.cpp:2369 localTextureHandle).
                                               // TG_TypeShape::listOfTextures is a shared type-
                                               // level cache mutated by TransformMultiShape across
                                               // all actors — do NOT read it for slot 0.
    uint32_t                    lightDataIndex; // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
};

// Fallback accounting reasons (used in MC2_MECH_BATCHER_STATS output).
enum class GpuMechFallbackReason : uint8_t {
    UnregisteredType  = 0,
    U8BoneOverflow    = 1,
    RingOverflow      = 2,
    TglGpuUnsupported = 3,
    ShaderInitFailure = 4,
};

// Ring depth — must match STATIC_PROP_RING_FRAMES to share fence semantics
// with gos_object_parity.cpp. Cross-checked by static_assert in .cpp.
constexpr uint32_t MECH_RING_FRAMES = 3u;

class GpuMechBatcher {
public:
    static GpuMechBatcher& instance();

    void onMapLoad();
    void onMapUnload();

    // Register one Mech3DAppearanceType × LOD. Idempotent.
    // Reads mechType->mechShape[lod] (TG_TypeMultiShape*, stable type-level pointer).
    void registerTypeLod(const Mech3DAppearanceType* mechType, int lod);

    // Upload immutable VBO/IBO after all registerTypeLod() calls.
    void finalizeGeometry();

    // Caller-side accounting (called BEFORE any registration check).
    void recordEligibleActor();
    void recordCpuFallback(GpuMechFallbackReason reason);

    // Per-frame submission. Returns false on any failure; caller MUST
    // CPU-fallback (mechShape->Render(true)) this frame when false.
    [[nodiscard]] bool submitActor(const GpuMechSubmitDesc& desc);

    // Post-renderLists() draw flush.
    void flush();
    void flushShadow();  // no-op in Slice A/B1/B2; reserved for future shadow-offload slice

    bool wasLastFailureLateRegistration() const;

    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();
};
```

- [ ] **Step 1.3: Add `friend class GpuMechBatcher` to `TG_TypeShape` and `TG_MultiShape`**

`GpuMechBatcher` needs access to `protected` members in both classes —
specifically `TG_TypeShape::numTypeTriangles/listOfTypeTriangles/listOfTypeVertices/numTextures/listOfTextures`
and `TG_MultiShape::listOfShapes`. Both classes already have `friend class GpuStaticPropBatcher;`
in their friend lists; mirror that pattern.

First, confirm the existing friend list positions:

```bash
grep -n "friend class GpuStaticPropBatcher\|friend class" mclib/tgl.h | head -10
grep -n "friend class GpuStaticPropBatcher\|friend class" mclib/msl.h | head -10
```

In `mclib/tgl.h` — find the line `friend class GpuStaticPropBatcher;` inside `TG_TypeShape`
and add directly below it:

```cpp
    friend class GpuMechBatcher;
```

In `mclib/msl.h` — find the line `friend class GpuStaticPropBatcher;` inside `TG_MultiShape`
and add directly below it:

```cpp
    friend class GpuMechBatcher;
```

Verify both edits:

```bash
grep -n "GpuMechBatcher\|GpuStaticPropBatcher" mclib/tgl.h mclib/msl.h
```

Expected: one GpuMechBatcher line adjacent to each GpuStaticPropBatcher line.

- [ ] **Step 1.4: Verify static_asserts are consistent**

```bash
grep -n "static_assert\|GpuMechVertex\|GpuMechInstance\|GpuMechBone" \
    GameOS/gameos/gos_mech_batcher.h
```

Expected: All three structs have `static_assert(sizeof(...) == ...)` and offset checks.

- [ ] **Step 1.5: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.h GameOS/gameos/gos_mech_killswitch.h \
        mclib/tgl.h mclib/msl.h
git commit -m "feat: add GPU mech batcher header + friend declarations in TG_TypeShape/TG_MultiShape"
```

---

## Task 2: Shaders — `mech.vert` and `mech.frag`

**Files:**
- Create: `shaders/mech.vert`
- Create: `shaders/mech.frag`

- [ ] **Step 2.1: Write `shaders/mech.vert`**

```glsl
// shaders/mech.vert — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".
//
// Bone transform convention: GpuMechBone stores Stuff LinearMatrix4D rows.
// mat4(row0..row3) fills GLSL COLUMNS from those args => boneT is the
// transpose of the original Stuff matrix. boneT * v == row-vec v * M_original.
// Do NOT "fix" the transpose — it is intentional and load-bearing.

#include <include/lighting.hglsl>

// Vertex attributes — 48-byte skinning-ready ABI, locked for Slice A+.
// Storage types: see GpuMechVertex in gos_mech_batcher.h.
layout(location=0) in vec3  a_position;
layout(location=1) in vec3  a_normal;
layout(location=2) in vec2  a_uv;
layout(location=3) in uvec4 a_boneIndices;   // GL_UNSIGNED_BYTE via IPointer; Slice A: .x only
layout(location=4) in vec4  a_boneWeights;   // GL_UNSIGNED_BYTE normalized; Slice A: (1,0,0,0)
layout(location=5) in vec2  a_tangentOct;    // GL_SHORT normalized; zero-fill for stock
layout(location=6) in uint  a_aRGBLight;     // TG_TypeVertex::aRGBLight, BGRA packed

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

// Per-frame bone SSBO (binding 1).
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(std430, binding=1) readonly buffer BoneBuffer {
    GpuMechBone bones[];
};

// 'uniform uint' crashes this engine's shader compiler — use int + cast.
uniform int  u_instanceBase;
uniform int  u_materialFlags;
uniform mat4 u_worldToClip;      // upload GL_TRUE (Stuff Matrix4D col-major -> GLSL transpose)
uniform vec4 u_terrainViewport;  // (vmx, vmy, vax, vay) for D3D->GL projection chain
uniform mat4 u_mvp;              // px->NDC (upload GL_TRUE)

// Varyings — FS does NOT read the SSBO; all per-instance data forwarded here.
out vec2 v_uv;
out vec4 v_litColor;
out vec4 v_highlightColor;
out vec3 v_fogRGB;
out vec3 v_normal;   // world-space for GBuffer1

void main() {
    uint instIdx = uint(u_instanceBase) + uint(gl_InstanceID);
    GpuMechInstance inst = instances[instIdx];

    // Bone transform: boneT is the transpose of the Stuff LinearMatrix4D.
    GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
    mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
    vec4 worldPos = boneT * vec4(a_position, 1.0);

    // World-space normal via 3x3 rotation block of boneT.
    vec3 worldNormal = normalize(mat3(boneT) * a_normal);

    // D3D pixel-homogeneous projection chain (identical to static_prop.vert).
    vec4 clip4 = u_worldToClip * worldPos;
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc   = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

    if (clip4.w < 0.1) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }

    // Slice A: decode a_aRGBLight (BGRA packed uint) as base vertex color.
    // Slice B1: replace with calc_light() using inst.lightDataIndex.
    vec3 baseLight;
    baseLight.x = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;  // r
    baseLight.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;  // g
    baseLight.z = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;  // b
    baseLight = clamp(baseLight + inst.aRGBHighlight.rgb, 0.0, 1.0);

    v_uv             = a_uv;
    v_litColor       = vec4(baseLight, 1.0);
    v_highlightColor = inst.aRGBHighlight;
    v_fogRGB         = inst.fogRGB.rgb;
    v_normal         = worldNormal;
}
```

- [ ] **Step 2.2: Write `shaders/mech.frag`**

```glsl
// shaders/mech.frag — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true

in vec2 v_uv;
in vec4 v_litColor;
in vec4 v_highlightColor;
in vec3 v_fogRGB;
in vec3 v_normal;

uniform sampler2D u_tex;
uniform int u_materialFlags;  // bit 0: ALPHA_TEST
uniform float u_fogValue;     // 1.0 = clear (per static_prop convention)

layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;

const int ALPHA_TEST_BIT = 1;

void main() {
    vec4 tex_color = texture(u_tex, v_uv);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    vec4 c = tex_color * v_litColor;
    c.rgb += v_highlightColor.rgb * v_highlightColor.a;
    c.rgb  = mix(v_fogRGB, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
```

- [ ] **Step 2.3: Verify no `#version` directive crept in**

```bash
grep -n "#version" shaders/mech.vert shaders/mech.frag
```

Expected: no output.

- [ ] **Step 2.4: Commit**

```bash
git add shaders/mech.vert shaders/mech.frag
git commit -m "feat: add mech.vert and mech.frag shaders (Slice A, 48B ABI)"
```

---

## Task 3: Batcher skeleton — singleton, killswitch, shader load, stubs

**Files:**
- Create: `GameOS/gameos/gos_mech_batcher.cpp` (skeleton + stubs; grows in later tasks)

- [ ] **Step 3.1: Write skeleton `gos_mech_batcher.cpp`**

```cpp
// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.
#include "gos_mech_batcher.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "glsl_program.h"
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>

// MECH_RING_FRAMES must equal STATIC_PROP_RING_FRAMES so the parity SSBO
// ring and the mech fence ring share the same depth.
static_assert(MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "MECH_RING_FRAMES must match STATIC_PROP_RING_FRAMES");

// Enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M in gameosmain.cpp hotkey handler.
bool g_useGpuMechs = (getenv("MC2_GPU_MECHS") != nullptr);

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
static bool   s_programLoadTried  = false;
static bool   s_programLoadFailed = false;
static GLuint s_mechProgram       = 0;
static glsl_program* s_mechProgramObj = nullptr;

// Cached uniform locations (set at program link time).
static GLint s_loc_u_instanceBase    = -1;
static GLint s_loc_u_materialFlags   = -1;
static GLint s_loc_u_worldToClip     = -1;
static GLint s_loc_u_terrainViewport = -1;
static GLint s_loc_u_mvp             = -1;
static GLint s_loc_u_tex             = -1;
static GLint s_loc_u_fogValue        = -1;

// Geometry (immutable after finalizeGeometry).
static GLuint s_sharedVao = 0;
static GLuint s_sharedVbo = 0;
static GLuint s_sharedIbo = 0;
static GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
static bool   s_geometryFinalized = false;

// Staging buffers (cleared after finalizeGeometry).
static std::vector<uint8_t>   s_stagingVbo;
static std::vector<uint32_t>  s_stagingIbo;

// Type registration.
static std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
static std::vector<GpuMechPacket>        s_packets;

// Key: (Mech3DAppearanceType*, lod) -> s_typeLodRecords index
struct TypeLodKey {
    const Mech3DAppearanceType* type;
    int lod;
    bool operator==(const TypeLodKey& o) const { return type == o.type && lod == o.lod; }
};
struct TypeLodKeyHash {
    size_t operator()(const TypeLodKey& k) const {
        return std::hash<const void*>()(k.type) ^ (std::hash<int>()(k.lod) * 2654435761u);
    }
};
static std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;

// Per-frame ring SSBOs.
static GLuint   s_instanceSsbo    = 0;
static GLuint   s_boneSsbo        = 0;
static void*    s_instanceMap     = nullptr;
static void*    s_boneMap         = nullptr;
static size_t   s_instanceCapacity = 0;  // per ring slot (in GpuMechInstance units)
static size_t   s_boneCapacity     = 0;  // per ring slot (in GpuMechBone units)
static uint32_t s_frameSlot        = 0;
static GLsync   s_fence[MECH_RING_FRAMES] = {};

static constexpr size_t kInitialInstancesPerFrame = 512;
static constexpr size_t kInitialBonesPerFrame     = 8192;

// Per-frame pending submit list.
struct PendingSubmit {
    GpuMechSubmitDesc        desc;
    std::vector<GpuMechBone> bones;          // staged from listOfShapes[i].shapeToWorld
    std::vector<uint32_t>    packetTexHandles; // per-packet live gosHandle captured at submit
    uint32_t                 typeLodIdx;
};
static std::vector<PendingSubmit> s_pendingSubmits;

// Counters.
static bool     s_mechBatcherTrace     = false;
static bool     s_mechBatcherTraceInit = false;
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason
static uint64_t s_allowedLateRegEvents    = 0;
static uint64_t s_disallowedLateRegEvents = 0;
static bool     s_lastFailWasLateReg      = false;

// ---------------------------------------------------------------------------
// Shader load
// ---------------------------------------------------------------------------
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

    auto loc = [&](const char* name) {
        return glGetUniformLocation(s_mechProgram, name);
    };
    s_loc_u_instanceBase    = loc("u_instanceBase");
    s_loc_u_materialFlags   = loc("u_materialFlags");
    s_loc_u_worldToClip     = loc("u_worldToClip");
    s_loc_u_terrainViewport = loc("u_terrainViewport");
    s_loc_u_mvp             = loc("u_mvp");
    s_loc_u_tex             = loc("u_tex");
    s_loc_u_fogValue        = loc("u_fogValue");

    std::fprintf(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management (Task 5 replaces this stub)
// ---------------------------------------------------------------------------
static void ensureRingCapacity(size_t, size_t) {}  // STUB — Task 5

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GpuMechBatcher& GpuMechBatcher::instance() {
    static GpuMechBatcher batcher;
    return batcher;
}

void GpuMechBatcher::onMapLoad() {
    s_typeLodRecords.clear();
    s_packets.clear();
    s_typeLodIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_geometryFinalized = false;
    s_programLoadTried  = false;
    s_programLoadFailed = false;
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_load\n");
}

void GpuMechBatcher::onMapUnload() {
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_sharedVao)    { glDeleteVertexArrays(1, &s_sharedVao);    s_sharedVao    = 0; }
    if (s_sharedVbo)    { glDeleteBuffers(1, &s_sharedVbo);         s_sharedVbo    = 0; }
    if (s_sharedIbo)    { glDeleteBuffers(1, &s_sharedIbo);         s_sharedIbo    = 0; }
    if (s_sampler)      { glDeleteSamplers(1, &s_sampler);          s_sampler      = 0; }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo);      s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);          s_boneSsbo     = 0; s_boneMap     = nullptr; }
    s_instanceCapacity  = 0;
    s_boneCapacity      = 0;
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[(int)r]; }
void GpuMechBatcher::flushShadow() {}  // no-op in Slice A/B1/B2

// ---------------------------------------------------------------------------
// Stubs — replaced in Tasks 4, 6, 7. Present so the project links cleanly.
// ---------------------------------------------------------------------------
void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType*, int) {}
void GpuMechBatcher::finalizeGeometry() { s_geometryFinalized = true; }
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc&) { return false; }
void GpuMechBatcher::flush() {}
```

- [ ] **Step 3.2: Wire into CMakeLists.txt**

```bash
grep -n "gos_static_prop_batcher" \
    A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CMakeLists.txt
```

Add `GameOS/gameos/gos_mech_batcher.cpp` to the same source list.

- [ ] **Step 3.3: Build — must link cleanly (stubs provide all symbols)**

```
/mc2-build
```

Expected: clean build. No undefined symbols.

- [ ] **Step 3.4: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp CMakeLists.txt
git commit -m "feat: add GpuMechBatcher skeleton (singleton, stubs, shader load, MC2_GPU_MECHS)"
```

---

## Task 4: Registration — `registerTypeLod()` + `finalizeGeometry()`

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp` (replace stubs)

- [ ] **Step 4.1: Replace `registerTypeLod()` stub**

Remove the stub and add:

```cpp
void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType* mechType, int lod) {
    if (!mechType) return;
    const TypeLodKey key{mechType, lod};
    if (s_typeLodIndex.count(key)) return;  // idempotent
    if (s_geometryFinalized) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=late_register type=%p lod=%d\n",
            (void*)mechType, lod);
        ++s_disallowedLateRegEvents;
        return;
    }

    TG_TypeMultiShape* typeMulti = mechType->mechShape[lod];
    if (!typeMulti) return;

    const int numNodes = typeMulti->GetNumShapes();
    if (numNodes == 0 || numNodes > 255) {
        if (numNodes > 255) {
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=u8_bone_overflow type=%p lod=%d numNodes=%d\n",
                (void*)mechType, lod, numNodes);
        }
        return;
    }

    static const bool s_nodeTrace = (getenv("MC2_MECH_NODE_TRACE") != nullptr);
    if (s_nodeTrace) {
        std::fprintf(stderr, "[MECHREG v1] event=register type=%p lod=%d numBones=%d\n",
                     (void*)mechType, lod, numNodes);
    }

    const uint32_t typeLodIdx = (uint32_t)s_typeLodRecords.size();
    s_typeLodIndex[key] = typeLodIdx;

    GpuMechTypeLodRecord rec{};
    rec.firstBoneIndex = 0;
    rec.numBones       = (uint32_t)numNodes;
    rec.firstPacket    = (uint32_t)s_packets.size();
    rec.packetCount    = 0;
    rec.vertexCount    = 0;
    rec.sourceNode0    = nullptr;

    for (int nodeIdx = 0; nodeIdx < numNodes; ++nodeIdx) {
        TG_TypeNodePtr tnode = typeMulti->GetTypeNode(nodeIdx);
        if (!tnode || tnode->GetNodeType() != SHAPE_NODE) continue;
        TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(tnode);
        if (nodeIdx == 0) rec.sourceNode0 = typeShape;

        if (!typeShape->numTypeTriangles || !typeShape->listOfTypeTriangles ||
            !typeShape->listOfTypeVertices) continue;

        const int32_t baseVertex = (int32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex));

        // Group triangles by localTextureHandle (same as static prop batcher).
        const uint32_t numTris = typeShape->numTypeTriangles;
        uint32_t runStart = 0;
        while (runStart < numTris) {
            const DWORD runTexSlot =
                typeShape->listOfTypeTriangles[runStart].localTextureHandle;
            uint32_t runEnd = runStart;
            while (runEnd < numTris &&
                   typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTexSlot)
                ++runEnd;

            const uint32_t packetFirstIndex = (uint32_t)s_stagingIbo.size();

            for (uint32_t t = runStart; t < runEnd; ++t) {
                const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
                const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
                const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

                for (int c = 0; c < 3; ++c) {
                    const TG_TypeVertex& src =
                        typeShape->listOfTypeVertices[tri.Vertices[c]];

                    GpuMechVertex vert{};
                    std::memcpy(vert.position, &src.position.x, 12);
                    std::memcpy(vert.normal,   &src.normal.x,   12);
                    vert.uv[0] = cornerU[c];
                    vert.uv[1] = cornerV[c];
                    // boneIndices: .x = nodeIdx (rigid, Slice A), .yzw = 0
                    vert.boneIndices[0] = (uint8_t)(nodeIdx & 0xFF);
                    vert.boneIndices[1] = 0;
                    vert.boneIndices[2] = 0;
                    vert.boneIndices[3] = 0;
                    // boneWeights: .x = 255 (= 1.0 normalized), .yzw = 0
                    vert.boneWeights[0] = 255;
                    vert.boneWeights[1] = 0;
                    vert.boneWeights[2] = 0;
                    vert.boneWeights[3] = 0;
                    // tangentOct: zero-fill for stock (no .tglgpu sidecar)
                    vert.tangentOct[0] = 0;
                    vert.tangentOct[1] = 0;
                    vert.aRGBLight = src.aRGBLight;

                    s_stagingVbo.insert(s_stagingVbo.end(),
                        reinterpret_cast<uint8_t*>(&vert),
                        reinterpret_cast<uint8_t*>(&vert) + sizeof(GpuMechVertex));

                    // Write index LOCAL to this packet's baseVertex.
                    // glDrawElementsInstancedBaseVertex adds pkt.baseVertex at draw time,
                    // so IBO must contain (globalVertex - baseVertex), not globalVertex.
                    // Using s_stagingIbo.size() (the global IBO slot) here would be wrong
                    // for all packets after the first because baseVertex != 0.
                    const uint32_t localIdx =
                        (uint32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex) - 1u)
                        - (uint32_t)baseVertex;
                    s_stagingIbo.push_back(localIdx);
                    ++rec.vertexCount;
                }
            }

            // Derive ALPHA_TEST_BIT from the texture slot's textureAlpha flag.
            // TG_TinyTexture::textureAlpha (tgl.h:334) is set by SetTextureAlpha()
            // and reflects whether the texture has meaningful alpha for discard.
            uint32_t matFlags = 0;
            if (runTexSlot < (DWORD)typeShape->numTextures &&
                typeShape->listOfTextures[runTexSlot].textureAlpha) {
                matFlags = 1u;  // ALPHA_TEST_BIT (matches mech.frag ALPHA_TEST_BIT constant)
            }

            GpuMechPacket pkt{};
            pkt.firstIndex          = packetFirstIndex;
            pkt.indexCount          = (runEnd - runStart) * 3;
            pkt.baseVertex          = baseVertex;
            pkt.textureSlot         = (uint32_t)runTexSlot;
            pkt.materialFlags       = matFlags;
            pkt.owningTypeLodRecord = typeLodIdx;
            pkt.nodeLocalIndex      = (uint32_t)nodeIdx;
            pkt.owningTypeShape     = typeShape;
            s_packets.push_back(pkt);
            ++rec.packetCount;

            runStart = runEnd;
        }
    }

    s_typeLodRecords.push_back(rec);
}
```

- [ ] **Step 4.2: Replace `finalizeGeometry()` stub**

```cpp
void GpuMechBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;
    loadProgramsIfNeeded();

    // Bail cleanly if shader failed: geometry upload skipped.
    // submit() fast-rejects on s_geometryFinalized==false.
    if (s_programLoadFailed) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_skip reason=shader_fail\n");
        return;
    }

    if (s_stagingVbo.empty()) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_empty — no types registered\n");
        s_geometryFinalized = true;
        return;
    }

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    (GLsizeiptr)s_stagingVbo.size(),
                    s_stagingVbo.data(), 0);  // immutable, GPU-only

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(), 0);

    // Vertex attribute setup — 48-byte GpuMechVertex, 7 attributes.
    // enableF uses glVertexAttribPointer; enableI uses glVertexAttribIPointer.
    auto enableF = [](GLuint loc, GLint sz, GLenum type, GLboolean norm,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, sz, type, norm, stride, (void*)offset);
    };
    auto enableI = [](GLuint loc, GLint sz, GLenum type,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, sz, type, stride, (void*)offset);
    };

    const GLsizei S = (GLsizei)sizeof(GpuMechVertex);
    enableF(0, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, position));
    enableF(1, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, normal));
    enableF(2, 2, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, uv));
    enableI(3, 4, GL_UNSIGNED_BYTE,            S, offsetof(GpuMechVertex, boneIndices));
    enableF(4, 4, GL_UNSIGNED_BYTE,  GL_TRUE,  S, offsetof(GpuMechVertex, boneWeights));
    enableF(5, 2, GL_SHORT,          GL_TRUE,  S, offsetof(GpuMechVertex, tangentOct));
    enableI(6, 1, GL_UNSIGNED_INT,             S, offsetof(GpuMechVertex, aRGBLight));

    glBindVertexArray(0);

    // Session-lifetime sampler: GL_REPEAT / LINEAR_MIPMAP_LINEAR.
    // Prevents inheriting CLAMP_TO_EDGE / NEAREST from terrain patch stream.
    glGenSamplers(1, &s_sampler);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    s_geometryFinalized = true;
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=finalize_ok types=%zu packets=%zu\n",
        s_typeLodRecords.size(), s_packets.size());
}
```

- [ ] **Step 4.3: Build**

```
/mc2-build
```

Expected: clean build. `submitActor` and `flush` stubs still active.

- [ ] **Step 4.4: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat: add registerTypeLod + finalizeGeometry to GpuMechBatcher (48B ABI)"
```

---

## Task 5: Ring SSBO management

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 5.1: Replace `ensureRingCapacity()` stub**

```cpp
static void ensureRingCapacity(size_t neededInstances, size_t neededBones) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededBones     > s_boneCapacity;
    if (!needGrow) return;

    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);     s_boneSsbo     = 0; s_boneMap     = nullptr; }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : kInitialInstancesPerFrame);
    s_boneCapacity = std::max(neededBones,
        s_boneCapacity ? s_boneCapacity * 2 : kInitialBonesPerFrame);

    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)),
        nullptr, flags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)), flags);

    glGenBuffers(1, &s_boneSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_boneSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)),
        nullptr, flags);
    s_boneMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)), flags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_boneMap) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=persistent_map_fail\n");
    }
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=ring_alloc instances=%zu bones=%zu\n",
        s_instanceCapacity, s_boneCapacity);
}
```

- [ ] **Step 5.2: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat: add ring SSBO management to GpuMechBatcher"
```

---

## Task 6: `submitActor()` — bone staging + texture capture

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 6.1: Confirm `SetTextureHandle` writes the type-level descriptor**

```bash
grep -n "SetTextureHandle\|gosTextureHandle" mclib/msl.cpp | head -30
```

Expected: `SetTextureHandle` (msl.cpp:1321, called from `TransformMultiShape`) updates `TG_TypeShape::listOfTextures[slot].gosTextureHandle` — the type-level shared descriptor, NOT a per-instance `TG_Shape` field. `TG_Shape` has no `listOfTextures` member (verified at tgl.h:373; the struct holds `node`, `localShapeToWorld`, `shapeToWorld`, not textures). Reading from `pkt.owningTypeShape->listOfTextures` at submit time is per-actor correct because `mech3d.cpp:2369` calls `mechShape->SetTextureHandle(0, localTextureHandle)` before the GPU submit block runs in the same `render()` call.

- [ ] **Step 6.2: Replace `submitActor()` stub**

```cpp
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return false;
    if (!desc.mechShape || !desc.mechType) return false;

    const TypeLodKey key{desc.mechType, desc.currentLOD};
    auto it = s_typeLodIndex.find(key);
    if (it == s_typeLodIndex.end()) {
        s_lastFailWasLateReg = true;
        ++s_disallowedLateRegEvents;
        return false;
    }
    const uint32_t typeLodIdx = it->second;
    const GpuMechTypeLodRecord& rec = s_typeLodRecords[typeLodIdx];

    PendingSubmit ps;
    ps.desc       = desc;
    ps.typeLodIdx = typeLodIdx;
    ps.bones.reserve(rec.numBones);

    // Stage bone matrices from live shapeToWorld (set by TransformMultiShape).
    // listOfShapes[i].shapeToWorld is a Stuff::LinearMatrix4D with entries[12]
    // stored column-major: entries[(col<<2)+row], 3 explicit cols + implicit col3=[0,0,0,1].
    // Row k extraction: [entries[k], entries[4+k], entries[8+k], w] where w=1 for row3 only.
    const int numShapes = desc.mechShape->GetNumShapes();
    for (int i = 0; i < numShapes && i < (int)rec.numBones; ++i) {
        const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
        const float* e = (const float*)sr.shapeToWorld.entries;
        GpuMechBone bone;
        bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
        bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
        bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
        bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
        ps.bones.push_back(bone);
    }
    while ((int)ps.bones.size() < (int)rec.numBones) {
        GpuMechBone id{};
        id.row0[0]=1.f; id.row1[1]=1.f; id.row2[2]=1.f; id.row3[3]=1.f;
        ps.bones.push_back(id);
    }

    // Capture live per-actor texture handle for each packet.
    //
    // SLOT 0 is per-actor (paint scheme / team color):
    //   mech3d.cpp:2369 calls SetTextureHandle(0, localTextureHandle) per actor BEFORE
    //   this submitActor runs. However TG_TypeShape::listOfTextures is a shared type-level
    //   cache mutated by TransformMultiShape for ALL actors during update() — the last
    //   actor through TransformMultiShape overwrites it, so it reflects the wrong actor
    //   by render time. Use desc.slot0TexHandle (the raw gos handle passed by the caller)
    //   directly for slot 0. This is the only per-actor-varying slot.
    //
    // SLOTS 1+ are type-stable (engine glow, damage decals, etc.):
    //   TransformMultiShape sets these from stable type data; they do not vary by actor.
    //   Reading from owningTypeShape is correct for slots 1+.
    ps.packetTexHandles.resize(rec.packetCount, 0);
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            // Per-actor handle carried explicitly through the submit desc.
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.owningTypeShape && pkt.owningTypeShape->listOfTextures &&
                   pkt.textureSlot < (uint32_t)pkt.owningTypeShape->numTextures) {
            // Slots 1+ are type-stable; read from the shared type descriptor.
            ps.packetTexHandles[p] =
                pkt.owningTypeShape->listOfTextures[pkt.textureSlot].gosTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));
    return true;
}
```

- [ ] **Step 6.3: Build**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 6.4: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat: add submitActor with bone staging and per-actor texture capture"
```

---

## Task 7: `flush()` — bucket-sorted compaction + draw loop

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 7.1: Replace `flush()` stub**

```cpp
void GpuMechBatcher::flush() {
    if (!s_mechBatcherTraceInit) {
        s_mechBatcherTrace     = (getenv("MC2_MECH_BATCHER_STATS") != nullptr);
        s_mechBatcherTraceInit = true;
    }

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed ||
        s_pendingSubmits.empty()) {
        s_pendingSubmits.clear();
        s_eligibleActorsThisFrame = 0;
        std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
        return;
    }

    // Step 1: Count total bones (one block per actor; independent of packet count).
    size_t totalBones = 0;
    for (const auto& ps : s_pendingSubmits) totalBones += ps.bones.size();

    // Step 2: Build draw buckets.
    // Key: (typeLodIdx, globalPacketIdx, texHandle, materialFlags)
    // Each actor × packet produces one entry in the matching bucket.
    // Actors with different textures for the same packet end up in different buckets
    // (separate draw calls) — correctly handles per-actor paint schemes.
    struct BucketKey {
        uint32_t typeLodIdx;
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        bool operator<(const BucketKey& o) const {
            if (typeLodIdx      != o.typeLodIdx)      return typeLodIdx      < o.typeLodIdx;
            if (globalPacketIdx != o.globalPacketIdx) return globalPacketIdx < o.globalPacketIdx;
            if (texHandle       != o.texHandle)       return texHandle       < o.texHandle;
            return materialFlags < o.materialFlags;
        }
    };

    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]

    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        const PendingSubmit& ps = s_pendingSubmits[si];
        const GpuMechTypeLodRecord& rec = s_typeLodRecords[ps.typeLodIdx];
        for (uint32_t p = 0; p < rec.packetCount; ++p) {
            const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
            BucketKey key;
            key.typeLodIdx       = ps.typeLodIdx;
            key.globalPacketIdx  = rec.firstPacket + p;
            key.texHandle        = ps.packetTexHandles[p];
            key.materialFlags    = pkt.materialFlags;
            buckets[key].push_back(si);
        }
    }

    // Total instances = sum over all buckets (each actor appears once per packet).
    size_t totalInstances = 0;
    for (const auto& kv : buckets) totalInstances += kv.second.size();

    ensureRingCapacity(totalInstances, totalBones);
    if (!s_instanceMap || !s_boneMap) {
        s_pendingSubmits.clear();
        return;
    }

    // Step 3: Advance ring slot and wait for oldest fence.
    s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    GpuMechInstance* instDst = (GpuMechInstance*)s_instanceMap + s_frameSlot * s_instanceCapacity;
    GpuMechBone*     boneDst = (GpuMechBone*)    s_boneMap     + s_frameSlot * s_boneCapacity;

    // Step 4: Write bone SSBO once per actor; record each actor's boneBase offset.
    std::vector<uint32_t> actorBoneBase(s_pendingSubmits.size());
    uint32_t boneHead = 0;
    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        actorBoneBase[si] = boneHead;
        for (const auto& b : s_pendingSubmits[si].bones)
            boneDst[boneHead++] = b;
    }

    // Step 5: Write instance SSBO in bucket order; collect draw calls.
    // Each actor may appear in multiple buckets (once per packet of its type).
    struct DrawCall {
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        uint32_t instanceBase;
        uint32_t instanceCount;
    };
    std::vector<DrawCall> drawCalls;
    drawCalls.reserve(buckets.size());

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t instHead = 0;
    for (const auto& kv : buckets) {
        const BucketKey& key              = kv.first;
        const std::vector<uint32_t>& subs = kv.second;

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = instHead;
        dc.instanceCount   = (uint32_t)subs.size();

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
        drawCalls.push_back(dc);
    }

    // Step 6: Bind SSBOs (whole per-frame slices; shader indexes via u_instanceBase).
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
        (GLintptr) (s_frameSlot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (GLsizeiptr)(totalInstances * sizeof(GpuMechInstance)));
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_boneSsbo,
        (GLintptr) (s_frameSlot * s_boneCapacity * sizeof(GpuMechBone)),
        (GLsizeiptr)(totalBones * sizeof(GpuMechBone)));

    glUseProgram(s_mechProgram);
    glBindVertexArray(s_sharedVao);

    // Save prior state; set explicit mech-render state.
    GLint     prevDepthFunc; glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    GLboolean prevBlend;     glGetBooleanv(GL_BLEND,            &prevBlend);
    GLboolean prevCull;      glGetBooleanv(GL_CULL_FACE,        &prevCull);
    GLint     prevCullMode;  glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Bind sampler to unit 0: prevents CLAMP_TO_EDGE / NEAREST inheritance.
    GLint prevSampler = 0;
    glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &prevSampler);
    glBindSampler(0, s_sampler);

    // Static uniforms.
    glUniform1i(s_loc_u_tex,      0);
    glUniform1f(s_loc_u_fogValue, 1.0f);

    // Projection uniforms (same sources as static_prop batcher).
    const float* wtc = (const float*)&TG_Shape::s_worldToClip.entries[0];
    if (s_loc_u_worldToClip >= 0)
        glUniformMatrix4fv(s_loc_u_worldToClip, 1, GL_TRUE, wtc);
    const float* vp = gos_GetTerrainViewportVec4();
    if (s_loc_u_terrainViewport >= 0 && vp)
        glUniform4fv(s_loc_u_terrainViewport, 1, vp);
    const float* mm = gos_GetProj2ScreenMat4();
    if (s_loc_u_mvp >= 0 && mm)
        glUniformMatrix4fv(s_loc_u_mvp, 1, GL_TRUE, mm);

    // Step 7: Issue one draw call per bucket.
    uint32_t drawnCalls = 0;
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        if (s_loc_u_instanceBase >= 0)
            glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
        if (s_loc_u_materialFlags >= 0)
            glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);

        const uint32_t glTexId = gos_GetGLTextureId(dc.texHandle);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glTexId);

        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++drawnCalls;
    }

    // Restore state.
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevCull)  { glEnable(GL_CULL_FACE); glCullFace((GLenum)prevCullMode); }
    else             glDisable(GL_CULL_FACE);
    glBindSampler(0, (GLuint)prevSampler);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // Per-reason stats output (MC2_MECH_BATCHER_STATS=1).
    if (s_mechBatcherTrace) {
        static const char* const kFallbackNames[] = {
            "UnregisteredType", "U8BoneOverflow", "RingOverflow",
            "TglGpuUnsupported", "ShaderInitFailure"
        };
        uint32_t fallbackTotal = 0;
        for (int i = 0; i < 5; ++i) fallbackTotal += s_fallbacksThisFrame[i];
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=summary eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u fallback_total=%u\n",
            s_eligibleActorsThisFrame, s_pendingSubmits.size(),
            buckets.size(), drawnCalls, fallbackTotal);
        for (int i = 0; i < 5; ++i) {
            if (s_fallbacksThisFrame[i] > 0) {
                std::fprintf(stderr,
                    "[MECHBATCHER v1] event=fallback reason=%s count=%u\n",
                    kFallbackNames[i], s_fallbacksThisFrame[i]);
            }
        }
    }

    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
}
```

- [ ] **Step 7.2: Build and link**

```
/mc2-build
```

Expected: clean build. All methods defined — no undefined symbols.

- [ ] **Step 7.3: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat: add flush() with bucket-sorted draw loop and sampler ownership"
```

---

## Task 8: Measurement env-vars

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`
- Modify: `mclib/mech3d.cpp` (LOD trace only)

- [ ] **Step 8.1: Verify `MC2_MECH_NODE_TRACE` is present (already wired in Task 4)**

```bash
grep -n "MC2_MECH_NODE_TRACE\|MECHREG v1" GameOS/gameos/gos_mech_batcher.cpp
```

Expected: both the env-var check and `[MECHREG v1] event=register` log line.

- [ ] **Step 8.2: Verify `MC2_MECH_BATCHER_STATS` is present (already wired in Task 7)**

```bash
grep -n "MC2_MECH_BATCHER_STATS\|MECHBATCHER v1" GameOS/gameos/gos_mech_batcher.cpp
```

Expected: env-var check and `[MECHBATCHER v1] event=summary` line.

- [ ] **Step 8.3: Add `MC2_MECH_LOD_TRACE` to `mech3d.cpp` `recalcBounds()`**

First, read the LOD swap block to find the actual variable names:

```bash
sed -n '2290,2350p' mclib/mech3d.cpp
```

Add at the top of `mech3d.cpp` (after includes):

```cpp
static const bool s_mechLodTrace = (getenv("MC2_MECH_LOD_TRACE") != nullptr);
```

Inside `recalcBounds()`, just before the LOD swap conditional (adapt variable names to match the actual code shown by the sed output above):

```cpp
if (s_mechLodTrace) {
    std::fprintf(stderr, "[MECHLOD v1] event=lod_swap actor=%p old_lod=%d new_lod=%d\n",
                 (void*)this, currentLOD, newLOD);  // use actual variable names from code
}
```

- [ ] **Step 8.4: Note on `MC2_MECH_LIGHT_TRACE`**

`MC2_MECH_LIGHT_TRACE` (for the Slice B1 `LightsData[32]` capacity audit) is **intentionally deferred to Slice B1**. It is not implemented in Slice A. The spec Section 7 measurement gate for Slice B1 lighting will add it at that time.

- [ ] **Step 8.5: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp mclib/mech3d.cpp
git commit -m "feat: wire MC2_MECH_NODE_TRACE, MC2_MECH_BATCHER_STATS, MC2_MECH_LOD_TRACE"
```

---

## Task 9: Mission lifecycle hookup

**Files:**
- Modify: `code/mission.cpp`

- [ ] **Step 9.1: Add the three hookup points**

Add `#include "GameOS/gameos/gos_mech_batcher.h"` near the top of `code/mission.cpp`, grouped with other GameOS includes.

Find and add immediately AFTER the existing `GpuStaticPropBatcher::instance().onMapLoad()` call (line 1644):

```cpp
GpuMechBatcher::instance().onMapLoad();
```

Find and add immediately AFTER `GpuStaticPropBatcher::instance().finalizeGeometry()` (line 3042):

```cpp
GpuMechBatcher::instance().finalizeGeometry();
```

Find and add immediately AFTER `GpuStaticPropBatcher::instance().onMapUnload()` (line 3171):

```cpp
GpuMechBatcher::instance().onMapUnload();
```

- [ ] **Step 9.2: Build**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 9.3: Commit**

```bash
git add code/mission.cpp
git commit -m "feat: wire GpuMechBatcher lifecycle into mission.cpp map load/unload"
```

---

## Task 10: `mech3d.cpp` submit wire

**Files:**
- Modify: `mclib/mech3d.cpp`

This is the primary gameplay-visible change. Read the actual render function before editing anything.

- [ ] **Step 10.1: Read the render function and cull gate**

```bash
sed -n '2366,2450p' mclib/mech3d.cpp
```

The key block is around line 2406: `mechShape->Render(true)`. Note the EXACT condition that gates entry into the visible render block (e.g., `if (inView && visible)` or similar). **Do not introduce a reference to `g_useGpuStaticProps` — that flag is for static props, not mechs. Preserve the existing gate byte-for-byte.**

- [ ] **Step 10.2: Add includes at top of `mech3d.cpp`**

Find where other GameOS headers are included and add:

```cpp
#include "GameOS/gameos/gos_mech_batcher.h"
#include "GameOS/gameos/gos_mech_killswitch.h"
```

- [ ] **Step 10.3: Find and add `registerTypeLod()` call at type-load time**

Find where `Mech3DAppearanceType` finishes setting up `mechShape[lod]` pointers:

```bash
grep -n "mechShape\[" mclib/mech3d.cpp | head -30
```

After the last `mechShape[lod] = ...` assignment for all LODs in the type-load path, add:

```cpp
// Register all loaded LODs with the GPU mech batcher (idempotent).
for (int lod = 0; lod < MAX_LODS; ++lod) {
    if (mechShape[lod]) {
        GpuMechBatcher::instance().registerTypeLod(this, lod);
    }
}
```

- [ ] **Step 10.4: Wire `submitActor()` in `Mech3DAppearance::render()`**

From the output of Step 10.1, identify the exact block containing `mechShape->Render(true)`. The replacement must:

1. Call `GpuMechBatcher::instance().recordEligibleActor()` BEFORE the registration check (so it counts actors that would have rendered, even if they skip for other reasons).
2. Try the GPU path if `g_useGpuMechs`.
3. Fall back to `mechShape->Render(true)` if GPU submit returns false.

Template — adapt variable names to match the actual code from Step 10.1. The highlight
computation below mirrors mech3d.cpp:2382-2402 exactly:
- `highLight` — local uint32_t, computed from teamId + homeTeamRelationship table
- `highlightColor` — `long` on ObjectAppearance base class (objectappearance.h:97)
- `flashColor` / `drawFlash` — `DWORD` / `bool` on Mech3DAppearance (mech3d.h:437-438)

```cpp
// GPU mech batcher accounting — before any early-outs.
GpuMechBatcher::instance().recordEligibleActor();

bool gpuSubmitted = false;
if (g_useGpuMechs) {
    // Replicate the highlight selection from the CPU path (mech3d.cpp:2382-2402).
    uint32_t gpuHighlightARGB = 0x007f7f7f;
    if ((teamId > -1) && (teamId < 8)) {
        static const uint32_t kHighLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
        gpuHighlightARGB = kHighLightTable[homeTeamRelationship];
    }
    if (!(selected & DRAW_COLORED) || duration > 0)
        gpuHighlightARGB = (uint32_t)highlightColor;
    if (drawFlash)
        gpuHighlightARGB = (uint32_t)flashColor;

    GpuMechSubmitDesc desc{};
    desc.mechShape      = mechShape;
    desc.mechType       = mechType;
    desc.currentLOD     = currentLOD;
    desc.slot0TexHandle = (uint32_t)localTextureHandle;  // per-actor paint scheme gos handle;
                                                         // must be captured here, not read back
                                                         // from TG_TypeShape (shared cache,
                                                         // stale after TransformMultiShape).
    desc.lightDataIndex = 0;           // Slice B1 wires this
    desc.renderFlags    = 0;
    desc.highlightARGB  = gpuHighlightARGB;
    desc.fogARGB        = 0;           // fog state not available in Slice A; Slice B2 wires this

    gpuSubmitted = GpuMechBatcher::instance().submitActor(desc);
    if (!gpuSubmitted) {
        GpuMechBatcher::instance().recordCpuFallback(
            GpuMechBatcher::instance().wasLastFailureLateRegistration()
                ? GpuMechFallbackReason::UnregisteredType
                : GpuMechFallbackReason::ShaderInitFailure);
    }
}

if (!gpuSubmitted) {
    mechShape->Render(true);  // CPU path — unchanged
}
```

- [ ] **Step 10.5: Wire `flush()` inside `renderLists()` in `mclib/txmmgr.cpp`**

The flush hook belongs in `mclib/txmmgr.cpp` inside `MC_TextureManager::renderLists()`,
immediately after the existing static-prop flush block. **Not** in `gameos_graphics.cpp`
(which never calls `renderLists()` directly) and **not** in `gamecam.cpp`.

```bash
grep -n "GpuStaticPropBatcher::instance.*flush\|g_useGpuStaticProps\|g_useGpuObjects" \
    mclib/txmmgr.cpp | head -10
```

Expected output (approximately line 1478-1484):
```
1478:    if (g_useGpuStaticProps || g_useGpuObjects) {
1479:        GpuStaticPropBatcher::instance().flush();
1480:    }
```

Add the mech batcher flush immediately after that block:

```cpp
if (g_useGpuMechs) {
    GpuMechBatcher::instance().flush();
}
```

Also add `#include "GameOS/gameos/gos_mech_batcher.h"` and
`#include "GameOS/gameos/gos_mech_killswitch.h"` near the top of `txmmgr.cpp`,
grouped with the existing static-prop batcher includes.

- [ ] **Step 10.6: Build**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 10.7: Commit**

```bash
git add mclib/mech3d.cpp mclib/txmmgr.cpp
git commit -m "feat: wire GpuMechBatcher submit in mech3d.cpp and flush in txmmgr.cpp renderLists()"
```

---

## Task 11: `gos_object_parity.cpp` counter hookup

**Files:**
- Modify: `GameOS/gameos/gos_object_parity.cpp`

- [ ] **Step 11.1: Add include**

```cpp
#include "gos_mech_batcher.h"
```

- [ ] **Step 11.2: Find `ParityFrameTick()` and add mech late-reg counters**

```bash
grep -n "ParityFrameTick\|getAllowedLateReg\|getDisallowedLateReg" \
    GameOS/gameos/gos_object_parity.cpp | head -20
```

Inside `ParityFrameTick()` (or the equivalent end-of-frame tick function), add after the existing static prop counter reads:

```cpp
uint64_t mechDisallowed = GpuMechBatcher::getDisallowedLateRegEventCount();
if (mechDisallowed > 0) {
    std::fprintf(stderr,
        "[OBJECT_PARITY v1] event=mech_late_reg_disallowed count=%llu\n",
        (unsigned long long)mechDisallowed);
}
```

- [ ] **Step 11.3: Build**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 11.4: Commit**

```bash
git add GameOS/gameos/gos_object_parity.cpp
git commit -m "feat: wire GpuMechBatcher late-reg counters into gos_object_parity"
```

---

## Task 12: Deploy, enable, smoke test

- [ ] **Step 12.1: Deploy**

```
/mc2-deploy
```

Expected: `mc2.exe` and shaders (`mech.vert`, `mech.frag`) deployed to `A:/Games/mc2-opengl/mc2-win64-v0.3/`.

- [ ] **Step 12.2: Verify shaders deployed**

```bash
ls -la "A:/Games/mc2-opengl/mc2-win64-v0.3/shaders/mech.vert" \
       "A:/Games/mc2-opengl/mc2-win64-v0.3/shaders/mech.frag"
```

Expected: both files present, modification time matches deploy.

- [ ] **Step 12.3: Smoke with traces — GPU mechs OFF (default)**

```bash
MC2_MECH_NODE_TRACE=1 MC2_MECH_BATCHER_STATS=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 20
```

Expected:
- `[MECHBATCHER v1] event=map_load` in log
- `[MECHBATCHER v1] event=finalize_ok` with nonzero types count
- `[MECHREG v1] event=register` lines for each mech type × LOD
- No crashes; visuals identical to baseline (`g_useGpuMechs=false` → CPU path)

- [ ] **Step 12.4: Enable GPU mechs and smoke**

```bash
MC2_GPU_MECHS=1 MC2_MECH_BATCHER_STATS=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 20
```

Expected:
- `[MECHBATCHER v1] event=summary` with `fallback_total=0` (or only `ShaderInitFailure` if shader compiles cleanly — check for `event=shader_ok`)
- Mechs appear and animate identically to CPU path
- No GL errors

- [ ] **Step 12.5: Diagnose fallbacks if any**

```bash
MC2_GPU_MECHS=1 MC2_MECH_BATCHER_STATS=1 MC2_MECH_NODE_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 20
```

Match fallback reason to fix:
- `UnregisteredType` — registration walk missed a type; `registerTypeLod` call site fires after `finalizeGeometry`
- `U8BoneOverflow` — a mech type has >255 nodes; inspect `[MECHREG v1]` lines
- `ShaderInitFailure` — see `[MECHBATCHER v1] event=shader_fail`; check shader compile errors

- [ ] **Step 12.6: Final commit**

```bash
git add .
git commit -m "feat: GPU mech batcher Slice A — smoke-validated, MC2_GPU_MECHS=1 to enable"
```

---

## Self-Review Checklist

**Spec coverage:**

| Spec section | Covered by task |
|---|---|
| Sec 1: Three-slice structure, killswitch | Task 3 (`g_useGpuMechs` via `MC2_GPU_MECHS`), Task 10 |
| Sec 2: GpuMechVertex (48B, 7 attribs, static_asserts) | Task 1 |
| Sec 2: GpuMechInstance (48B, static_asserts) | Task 1 |
| Sec 2: GpuMechBone (explicit rows, transpose rationale) | Task 1, Task 6 |
| Sec 2: GpuMechTypeLodRecord, GpuMechPacket | Task 1 |
| Sec 2: GpuMechSubmitDesc, GpuMechFallbackReason | Task 1 |
| Sec 2: MECH_RING_FRAMES cross-check | Task 3 (static_assert) |
| Sec 3: registerTypeLod (type×LOD key, 48B triangle-soup VBO) | Task 4 |
| Sec 3: finalizeGeometry (GL_UNSIGNED_BYTE VAO, sampler, shader-fail guard) | Task 4 |
| Sec 3: Late registration → CPU fallback | Task 4 (`disallowedLateRegEvents`) |
| Sec 4: submitActor (bone staging, per-actor tex capture) | Task 6 |
| Sec 4: flush (bucket sort, compact, `u_instanceBase`, `glDrawElementsInstancedBaseVertex`) | Task 7 |
| Sec 4: GL state ownership + sampler bind/restore | Task 7 |
| Sec 4: Flush after `renderLists()` | Task 10.5 |
| Sec 4: Cull gate independence (no `g_useGpuStaticProps`) | Task 10.4 |
| Sec 5: mech.vert (7-attr 48B ABI, bone transform, projection, aRGBLight) | Task 2 |
| Sec 5: mech.frag (tex + highlight/fog, GBuffer1) | Task 2 |
| Sec 6: Dual-FBO parity | **DEFERRED (ADVISOR SCOPE CHANGE)** — Slice A gate is `fallback_total=0` + operator visual; spec requires `MC2_MECH_GPU_PARITY=1` zero-mismatch. Advisor sign-off required. |
| Sec 7: MC2_MECH_LOD_TRACE | Task 8 |
| Sec 7: MC2_MECH_NODE_TRACE → `[MECHREG v1]` | Task 4 (wired in registerTypeLod) |
| Sec 7: MC2_MECH_BATCHER_STATS with per-reason fallback output | Task 7 |
| Sec 7: MC2_MECH_LIGHT_TRACE | **DEFERRED** — Slice B1 (LightsData audit) |
| Sec 8: `recordEligibleActor()` before any registration check | Task 10.4 |
| Sec 9: Late-reg counters in gos_object_parity | Task 11 |
| Appendix: mission.cpp lifecycle hookup | Task 9 |

**Placeholder scan:** No TBDs. Tasks 10.1 and 10.3 contain code-discovery substeps (read actual variable names, find actual LOD swap site) — these are required at execution time because the plan cannot grep non-existent symbols. They are instructions, not placeholders. Task 10.4 now contains the fully expanded highlight computation (no placeholder).

**Adversarial review findings — disposition (round 1 + round 2):**

Round 1 (prior session):
- C1 (`sr.shape` → should be `sr.node`): **RESOLVED** — Task 6 no longer accesses TG_ShapeRec for texture lookup at all.
- C2 (`TG_Shape::listOfTextures` nonexistent): **RESOLVED** — Task 6 reads slot 0 from `desc.slot0TexHandle` (per-actor, from mech3d.cpp `localTextureHandle`) and slots 1+ from `pkt.owningTypeShape->listOfTextures` (type-stable). TG_Shape is never accessed for texture data.
- C3 (`TG_TypeMultiShape::GetTypeNode(int)` missing): **FALSE ALARM** — exists at msl.h:166 as `GetTypeNode(long)`.
- C4 (flush hook in wrong file): **RESOLVED** — Task 10.5 targets `mclib/txmmgr.cpp:~1483` inside `renderLists()`.
- M4 (alpha-test materialFlags always 0): **RESOLVED** — Task 4 decodes `textureAlpha` at registration.
- Task 10.4 highlight ARGB placeholder: **RESOLVED** — full mech3d.cpp:2382-2402 conditionals in plan.

Round 2 (this session):
- C1 (NEW): `TG_TypeShape` protected members not accessible from `GpuMechBatcher` (tgl.h:566 protected block; friend list has `GpuStaticPropBatcher`, not `GpuMechBatcher`): **RESOLVED** — Task 1.3 adds `friend class GpuMechBatcher;` to TG_TypeShape in tgl.h.
- C2 (NEW): `TG_MultiShape::listOfShapes` is `protected:` (msl.h:259-262; friend list has `GpuStaticPropBatcher`, not `GpuMechBatcher`): **RESOLVED** — Task 1.3 adds `friend class GpuMechBatcher;` to TG_MultiShape in msl.h.
- M1 (texture cross-contamination): `TG_TypeShape::listOfTextures[0].gosTextureHandle` is a shared type-level cache; `TransformMultiShape` mutates it for all actors in update order, so reading it at submit time gives the last actor's handle, not the current actor's: **RESOLVED** — `GpuMechSubmitDesc::slot0TexHandle` carries the per-actor gos handle explicitly from mech3d.cpp `localTextureHandle`; Task 6 uses it for slot 0, type-shape for slots 1+ (which are type-stable).
- M2 (Step 6.1 rationale was factually wrong): **RESOLVED** — Step 6.1 now correctly describes `SetTextureHandle` as writing the type-level multi-shape descriptor and explains why the shared cache cannot be used for slot 0.

Round 3 (user review):
- CRITICAL (IBO double-offset): Task 4 wrote `iboIdx = s_stagingIbo.size()` (global IBO slot) then drew with `glDrawElementsInstancedBaseVertex(pkt.baseVertex)`, adding baseVertex twice — only the first packet draws to the right vertices: **RESOLVED** — Task 4 now computes `localIdx = (stagingVbo.size()/stride - 1) - baseVertex`, writing indices local to the packet's base. Subsequent packets yield indices 0,1,2,... relative to their baseVertex, which `glDrawElementsInstancedBaseVertex` maps correctly to global VBO positions.
- MAJOR (parity gate weakened vs spec): Spec Section 6 requires `MC2_MECH_GPU_PARITY=1` zero-mismatch dual-FBO gate for Slice A; plan defers to Slice A+: **FLAGGED FOR ADVISOR** — architecture description and spec-coverage table now carry an explicit ⚠️ scope-change note requiring advisor sign-off before execution.
- MINOR (`<cstddef>` missing for `offsetof`): **RESOLVED** — added to header include list.

**Type consistency:**
- `GpuMechVertex` defined Task 1, used in Task 4 staging and VAO ✓
- `GpuMechFallbackReason` defined Task 1, used in Tasks 6, 10 ✓
- `GpuMechSubmitDesc` defined Task 1 (now includes `slot0TexHandle`), populated Task 10, consumed Task 6 ✓
- `GpuMechTypeLodRecord::numBones` defined Task 1, populated Task 4, read Task 6 ✓
- `PendingSubmit::packetTexHandles` defined Task 3, populated Task 6, consumed Task 7 ✓
- `gos_GetGLTextureId()` declared Task 1, called Task 7 ✓
- `glDrawElementsInstancedBaseVertex` uses `pkt.baseVertex` (Task 7) which is set in Task 4; IBO indices are LOCAL to that base (Task 4 computes `localIdx = globalVertex - baseVertex`), so the hardware resolves `localIdx + baseVertex = globalVertex` correctly ✓
- No `sr.shape` reference remains in the plan ✓
- `desc.slot0TexHandle` defined in GpuMechSubmitDesc (Task 1), set in Task 10.4, read in Task 6 ✓
