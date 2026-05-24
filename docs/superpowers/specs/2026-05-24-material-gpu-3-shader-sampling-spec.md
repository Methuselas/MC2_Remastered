# MaterialGpu-3: First Shader Sampling Switch

**Date:** 2026-05-24 (rev 1)
**Status:** Approved for planning
**Slice:** MaterialGpu-3 (first shader consumption of the MaterialGpu table)
**Predecessor:** MaterialGpu-2 (runtime table + SSBO upload, shipped 2026-05-24)
**Successor:** MaterialGpu-4 (normal map / PBR fields, gated on art-pipeline delivery)

---

## 1. Goal

Wire `static_prop.frag` to read albedo texture layer from
`materials[materialIdx].albedoTex` instead of the legacy `PerDrawEntry.texArrayLayer`,
when both env gates are active. Retain `texArrayLayer` as the runtime fallback.
Zero pixel delta required.

**Canonical scope sentence:**

> MaterialGpu-3 adds a shader-visible `materialIdx` field to `PerDrawEntry`, declares
> the MaterialGpu SSBO in `static_prop.frag`, and wires a runtime uniform switch that
> selects between the material table and legacy `texArrayLayer`. No visual change.
> No texture binding behavior changed.

---

## 2. Two env gates

```
MC2_MATERIAL_GPU=1          (v2 gate — upload, bind, validate)
MC2_MATERIAL_GPU_SAMPLE=1   (v3 gate — shader samples via materials[materialIdx].albedoTex)
```

**Sampling is active only when BOTH gates are set:**

```cpp
sampleOn = s_materialGpuEnabled &&
           s_materialGpuSampleEnabled &&
           s_materialGpuSsbo != 0;
```

If `MC2_MATERIAL_GPU_SAMPLE=1` is set but `MC2_MATERIAL_GPU` is unset:
`sampleOn = false` and the shader uses `texArrayLayer`. The SSBO is not bound.

New batcher gate constant (alongside `s_materialGpuEnabled`):

```cpp
static const bool s_materialGpuSampleEnabled =
    (getenv("MC2_MATERIAL_GPU_SAMPLE") != nullptr);
```

---

## 3. Scope

### In scope

- `GameOS/gameos/gos_static_prop_batcher.h` — rename `_pad1` → `materialIdx`, update static_asserts
- `GameOS/gameos/gos_static_prop_batcher.cpp` — fill `materialIdx` in `finalizeGeometry()`; add `ProgramLocs::materialGpuSample`; resolve and set `u_materialGpuSample` uniform
- `shaders/static_prop.frag` — rename `_pad1` → `uint materialIdx` in PerDrawEntry GLSL struct; add MaterialTable SSBO + `u_materialGpuSample` under `#ifdef MC2_COALESCE`; switch sampling
- `GameOS/gameos/gameosmain.cpp` — extend `[MATERIAL_GPU v1]` startup banner with `sample=%d`
- `docs/tier1_env_vars.md` — document `MC2_MATERIAL_GPU_SAMPLE`
- `tools/shader_reflect/` — update `static_prop.frag` golden (new SSBO binding + uniform)

### Out of scope

- No change to `RenderCore/MaterialGpu.h` or `shaders/include/material_gpu.hglsl`
- No change to `PerDrawEntry` size (still 32 bytes)
- No change to normal map, PBR scalar fields, or emissive path (MaterialGpu-4+)
- No change to the legacy non-coalesce path (`#else` branch of `MC2_COALESCE`)
- No texture atlas / bindless upgrade
- No `materialFlags` migration (ALPHA_TEST_BIT still read from `PerDrawEntry.materialFlags`)

---

## 4. `PerDrawEntry` change

Replace `_pad1` at offset 28 with `materialIdx`. Struct size and stride unchanged.

### C++ (`gos_static_prop_batcher.h`)

```cpp
struct PerDrawEntry {
    int32_t  packetID;          //  0 — index into s_packets[]
    int32_t  materialFlags;     //  4 — 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t  maxLocalVertexID;  //  8 — type.vertexCount - 1
    int32_t  texArrayLayer;     // 12 — group-relative layer in s_texArrayOff/On
    float    uvScaleX;          // 16 — 1.0f for Stage A
    float    uvScaleY;          // 20 — 1.0f for Stage A
    int32_t  objectIdRaw;       // 24 — M1.5: handle.raw() when MC2_OBJECT_ID_BUFFER=1, else 0
    uint32_t materialIdx;       // 28 — MaterialGpu-3: index into s_materialGpuTable
                                //      (was _pad1; filled from s_packetMaterialIdx[slot]
                                //       when MC2_MATERIAL_GPU=1, else 0u)
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, materialIdx) == 28, "materialIdx offset");
```

Remove the `_pad1` static_assert and add the `materialIdx` one. All other static_asserts unchanged.

### GLSL (`static_prop.frag`, inside `#ifdef MC2_COALESCE`)

```glsl
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;
    uint  materialIdx;    // was _pad1; uint matches uint32_t std430 stride
};
```

`int` and `uint` are both 4-byte aligned in std430 — the change does not affect stride.

---

## 5. `finalizeGeometry()` — fill `materialIdx`

Inside the existing entries-build loop (where `PerDrawEntry e{}` is constructed),
add after the other field assignments:

```cpp
// MaterialGpu-3: fill materialIdx from v2 sidecar.
// Guard size invariant: s_packetMaterialIdx.size() == s_sortedPacketOrder.size()
// (enforced by the finalizeGeometry sidecar that runs earlier in the same scope).
// If the sidecar is disabled (gate OFF) or the sizes diverge, fall back to 0u.
if (s_materialGpuEnabled) {
    if (slotIndex < s_packetMaterialIdx.size()) {
        e.materialIdx = s_packetMaterialIdx[slotIndex];
    } else {
        // Size mismatch: v2 sidecar not aligned with emitted count.
        // Log once per finalizeGeometry call (not per-slot, to avoid log flood).
        e.materialIdx = 0u;
        // (mismatch log emitted once outside the loop — see §5.1)
    }
} else {
    e.materialIdx = 0u;
}
```

### 5.1 Mismatch guard

After the entries loop (outside it), log and clamp:

```cpp
if (s_materialGpuEnabled &&
    s_packetMaterialIdx.size() != s_sortedPacketOrder.size()) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[MATERIAL_GPU v1] ERROR materialIdx sidecar size mismatch"
                  " emitted=%zu sidecar=%zu\n",
                  s_sortedPacketOrder.size(), s_packetMaterialIdx.size());
    std::fputs(buf, stderr);
}
```

This prevents a bad v2 sidecar from silently producing wrong material indices.
`materialIdx = 0u` is safe as a fallback (index 0 in the material table points to a valid
entry when the table is non-empty, and the SSBO is not accessed when the sampling gate is OFF).

---

## 6. Uniform location — `u_materialGpuSample`

### 6.1 Add to `ProgramLocs`

```cpp
struct ProgramLocs {
    // Shared
    GLint terrainMVP       = -1;
    GLint mvp              = -1;
    GLint fogValue         = -1;
    GLint debugAddrMode    = -1;
    // Legacy-only
    GLint maxLocalVertexID = -1;
    GLint materialFlags    = -1;
    GLint packetID         = -1;
    // Coalesce-only
    GLint drawIDBase          = -1;
    GLint texArr              = -1;
    GLint materialGpuSample   = -1;   // NEW: u_materialGpuSample for MaterialGpu-3
};
```

### 6.2 Resolve in `loadProgramsIfNeeded()`

After the existing coalesce-program uniform lookups (alongside `drawIDBase`, `texArr`):

```cpp
s_locsCoalesce.materialGpuSample =
    glGetUniformLocation(s_staticPropProgramCoalesce, "u_materialGpuSample");
```

A return of `-1` means the uniform is absent or optimized away (e.g. non-coalesce build,
or the field was DCE'd). The `if (loc >= 0)` guard below handles this cleanly.

### 6.3 Set in `flush()` — once per flush, before draw calls

At the same site where `fogValue` and `debugAddrMode` are set for the coalesce program
(around line 3490-3497):

```cpp
// MaterialGpu-3: set sampling mode uniform once per flush.
// sampleOn is true only when both MC2_MATERIAL_GPU and MC2_MATERIAL_GPU_SAMPLE
// are set AND the SSBO is uploaded (s_materialGpuSsbo != 0).
const bool sampleOn = s_materialGpuEnabled &&
                      s_materialGpuSampleEnabled &&
                      s_materialGpuSsbo != 0;
if (s_locsCoalesce.materialGpuSample >= 0) {
    glUniform1i(s_locsCoalesce.materialGpuSample, sampleOn ? 1 : 0);
}
```

`sampleOn` is computed once and reused. Do NOT call `glGetUniformLocation` inside `flush()`.

---

## 7. Shader changes (`shaders/static_prop.frag`)

All changes are within the `#ifdef MC2_COALESCE` block. The `#else` (legacy uniform) path
is untouched.

### 7.1 New declarations (extend the existing `#ifdef MC2_COALESCE` block)

Add the following immediately after the existing `uniform sampler2DArray u_texArr;`
line, still inside the `#ifdef MC2_COALESCE` / `#else` block (before `#endif`):

```glsl
// MaterialGpu-3: material table at binding 5.
// Only accessed when u_materialGpuSample != 0 (both gates active).
// Declared here always so the reflection surface is stable across gate states.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
};
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table
```

The full `#ifdef MC2_COALESCE` block after the edit:
```glsl
#ifdef MC2_COALESCE
// ... existing PerDrawEntry struct (with materialIdx replacing _pad1) ...
// ... existing layout(std430, binding = 4) perDraw_ ...
// ... existing uniform sampler2DArray u_texArr ...
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
};
uniform int u_materialGpuSample;
#else
// ... existing legacy uniforms ...
#endif
```

The `#include <include/material_gpu.hglsl>` style matches the existing
`#include <include/render_contract.hglsl>` on line 9.

### 7.2 Read `materialIdx` from PerDrawEntry

In the existing SSBO-read block (alongside `texArrayLayer`, `uvScaleX`, etc.):

```glsl
#ifdef MC2_COALESCE
    int   materialFlags    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int   packetID         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].packetID;
    int   maxLocalVertexID = perDraw_.entries[v_drawID + uint(u_drawIDBase)].maxLocalVertexID;
    int   texArrayLayer    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    float uvScaleX         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx      = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;  // NEW
```

### 7.3 Effective layer selection

Replace the existing texture sample:
```glsl
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(texArrayLayer)));
```

With:
```glsl
    // MaterialGpu-3: runtime switch between legacy and material table.
    // u_materialGpuSample is a pass-wide uniform (not per-fragment divergent).
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            // kMatTexAbsent = 0xFFFFFFFFu (from material_gpu.hglsl).
            // v2 mismatches=0 strongly predicts this path is always taken for
            // emitted static-prop packets. The guard is defensive; if it fires,
            // the legacy layer is used as fallback (no black/missing textures).
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
```

`kMatTexAbsent` is the correct GLSL constant name (defined in `material_gpu.hglsl` line 26).

---

## 8. `gameosmain.cpp` startup banner

Extend the existing `[MATERIAL_GPU v1]` banner to include the sample gate:

Change:
```cpp
std::snprintf(buf, sizeof(buf),
              "[MATERIAL_GPU v1] enabled=%d binding=5\n",
              (int)matGpuOn);
```

To:
```cpp
const bool sampleOn = (getenv("MC2_MATERIAL_GPU_SAMPLE") != nullptr);
std::snprintf(buf, sizeof(buf),
              "[MATERIAL_GPU v1] enabled=%d sample=%d binding=5\n",
              (int)matGpuOn, (int)sampleOn);
```

Buffer was `char buf[64]`. Max output: `[MATERIAL_GPU v1] enabled=1 sample=1 binding=5\n` = 44 chars + NUL. Fits.

---

## 9. `docs/tier1_env_vars.md` — add `MC2_MATERIAL_GPU_SAMPLE`

In the existing `## MaterialGpu sidecar (MaterialGpu-2)` section, add:

```markdown
- `MC2_MATERIAL_GPU_SAMPLE=1` — enable MaterialGpu shader sampling (MaterialGpu-3).
  Requires `MC2_MATERIAL_GPU=1` to have any effect (both must be set for
  `u_materialGpuSample=1`). Default OFF. Log: `enabled=%d sample=%d` in startup banner.
  When both active: static_prop.frag reads `materials[materialIdx].albedoTex`
  instead of `PerDrawEntry.texArrayLayer`. Expected result: zero pixel delta.
```

---

## 10. Reflection golden update

Adding `MaterialTable` SSBO (binding 5) and `u_materialGpuSample` uniform to
`static_prop.frag` changes its reflection output. The existing golden will show
a DRIFT (not CONTRACT_VIOLATION). Update with:

```powershell
py -3 tools/shader_reflect/reflect.py --update
```

Verify the updated golden shows:
- `array_stride=32` for `MaterialTable` (matching `MaterialGpu` struct size)
- `u_materialGpuSample` uniform at type `int`
- No CONTRACT_VIOLATION

This is the last task before the final v3 gate run.

---

## 11. Log format

```
Startup (gate OFF/ON):
  [MATERIAL_GPU v1] enabled=0 sample=0 binding=5
  [MATERIAL_GPU v1] enabled=1 sample=0 binding=5   ← v2 mode: upload+bind, no sampling
  [MATERIAL_GPU v1] enabled=1 sample=1 binding=5   ← v3 mode: full

Per flush (if log added — optional, not required for v3):
  [MATERIAL_GPU v1] event=sample_mode enabled=0 reason=sample_env_off
  [MATERIAL_GPU v1] event=sample_mode enabled=1

Error:
  [MATERIAL_GPU v1] ERROR materialIdx sidecar size mismatch emitted=M sidecar=S
```

The `event=sample_mode` log is optional for v3 but recommended for debugging gate interaction.
If implemented, emit once per `flush()` call (not per draw).

---

## 12. Required v3 gates

All must pass before declaring MaterialGpu-3 complete:

| Gate | Verification |
|---|---|
| **Tier1 5/5, both gates OFF** | Smoke no MC2_* set, exit 0, 5/5 PASS |
| **Tier1 5/5, upload-only (`MC2_MATERIAL_GPU=1`, sample OFF)** | `u_materialGpuSample=0`, shader uses `texArrayLayer`. Exit 0, 5/5 PASS. Startup banner: `enabled=1 sample=0` |
| **Tier1 5/5, both gates ON** | `MC2_MATERIAL_GPU=1 MC2_MATERIAL_GPU_SAMPLE=1`. Exit 0, 5/5 PASS |
| **No GL errors (both-gates-ON run)** | No `GL ERROR` lines in logs |
| **No sidecar size mismatch** | No `ERROR materialIdx sidecar size mismatch` lines |
| **Pixel parity (upload-only vs both-gates-ON)** | v2 `mismatches=0` strongly predicts zero delta; v3 still requires it. If smoke captures screenshots: pixel diff = 0. If not: PASS 5/5 + no GL ERROR accepted as evidence. Document criterion used. |
| **Reflection golden passes** | `reflect.py` exit 0 after golden update, no CONTRACT_VIOLATION |
| **`MC2_MATERIAL_GPU_SAMPLE=1` alone (no upload gate)** | Verify `u_materialGpuSample=0` in this config (SSBO unbound → `sampleOn` forced false) |

---

## 13. Files changed

| File | Change |
|---|---|
| `GameOS/gameos/gos_static_prop_batcher.h` | EDIT — `_pad1` → `uint32_t materialIdx`, update static_assert |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | EDIT — `s_materialGpuSampleEnabled` gate; fill `materialIdx` in entries loop + mismatch guard; `ProgramLocs::materialGpuSample`; resolve + set uniform |
| `shaders/static_prop.frag` | EDIT — `_pad1` → `uint materialIdx` in GLSL struct; `#include material_gpu.hglsl`; MaterialTable SSBO; `uniform int u_materialGpuSample`; `effectiveLayer` switch |
| `GameOS/gameos/gameosmain.cpp` | EDIT — extend banner with `sample=%d` |
| `docs/tier1_env_vars.md` | EDIT — document `MC2_MATERIAL_GPU_SAMPLE` |
| `tools/shader_reflect/*.json` | EDIT — update `static_prop.frag` golden after `--update` |

`RenderCore/MaterialGpu.h`, `shaders/include/material_gpu.hglsl` — **NOT changed.**

---

## 14. v4 handoff

MaterialGpu-4 is the first PBR field switch:

```
MaterialGpu-4:
  wire normalTex, metallicRoughnessTex, emissiveTex fields
  requires art-pipeline delivery of normal/PBR maps for static props
  gated on MC2_MATERIAL_GPU_PBR (or successor gate)
  pixel-parity NOT required (PBR is a visual upgrade)
  predecessor: MaterialGpu-3 must ship and be pixel-stable
```

v3 failure → shader read path / uniform / PerDrawEntry field is wrong.
v4 failure → PBR map lookup / GLSL lighting equation is wrong.
That separation is the point.
