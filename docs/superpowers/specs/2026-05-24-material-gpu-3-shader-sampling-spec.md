# MaterialGpu-3: First Shader Sampling Switch

**Date:** 2026-05-24 (rev 2 — reviewer fixes applied)
**Status:** Approved for planning
**Slice:** MaterialGpu-3 (first shader-consumer slice)
**Predecessor:** MaterialGpu-2 (runtime table + SSBO upload, shipped 2026-05-24)
**Successor:** MaterialGpu-4 (normal map / PBR fields, gated on art-pipeline delivery)

---

## 1. Goal

Wire `static_prop.frag` to read albedo texture layer from
`materials[materialIdx].albedoTex` instead of the legacy `PerDrawEntry.texArrayLayer`,
when both env gates are active. Retain `texArrayLayer` as the runtime fallback.
Zero pixel delta required.

**Canonical scope sentence:**

> MaterialGpu-3 is the first shader-consumer slice. It changes the shader-visible
> `PerDrawEntry` layout (replacing `_pad1` with `materialIdx` at the same offset),
> declares the MaterialGpu SSBO in `static_prop.frag`, and wires a runtime uniform
> switch between the material table and legacy `texArrayLayer`. Stride is unchanged.
> No visual change. No texture binding behavior changed.

---

## 2. Two env gates

```
MC2_MATERIAL_GPU=1          (v2 gate — upload, bind, validate)
MC2_MATERIAL_GPU_SAMPLE=1   (v3 gate — shader samples via materials[materialIdx].albedoTex)
```

**Sampling is active only when ALL four conditions are true:**

```cpp
sampleOn = s_materialGpuEnabled       // MC2_MATERIAL_GPU=1
        && s_materialGpuSampleEnabled  // MC2_MATERIAL_GPU_SAMPLE=1
        && s_materialGpuSsbo != 0      // SSBO actually uploaded
        && s_materialGpuSidecarValid;  // sidecar size matches emitted count (see §5.1)
```

**Gate-off behavior:**
- `MC2_MATERIAL_GPU_SAMPLE=1` alone (upload gate unset): `sampleOn = false`. SSBO is not
  bound. Shader uses `texArrayLayer`. The sample env var is silently inert.
- `MC2_MATERIAL_GPU=1` alone (sample gate unset): `sampleOn = false`. MaterialTable is
  declared in the shader and SSBO is bound at slot 5, but `u_materialGpuSample=0`.
  Shader uses `texArrayLayer`. This is the v2-mode smoke configuration.
- Sidecar mismatch: `s_materialGpuSidecarValid = false` → `sampleOn = false` regardless
  of env gates. Sampling is forced off for the whole pass. Legacy path is the fallback,
  not wrong-material-0.

**New batcher gate constants** (add alongside `s_materialGpuEnabled`):

```cpp
static const bool s_materialGpuSampleEnabled =
    (getenv("MC2_MATERIAL_GPU_SAMPLE") != nullptr);
// Tracks whether finalizeGeometry() produced a correctly-sized sidecar.
// Recomputed each finalizeGeometry() call. Initialized false (no table yet).
static bool s_materialGpuSidecarValid = false;
```

---

## 3. Scope

### In scope

- `GameOS/gameos/gos_static_prop_batcher.h` — rename `_pad1` → `materialIdx`, update static_asserts
- `GameOS/gameos/gos_static_prop_batcher.cpp` — `s_materialGpuSampleEnabled` + `s_materialGpuSidecarValid`; fill `materialIdx` in `finalizeGeometry()` + sidecar guard; `ProgramLocs::materialGpuSample`; resolve + set `u_materialGpuSample` uniform; `event=sample_mode` log; `event=entry_material_idx` compare log
- `shaders/static_prop.frag` — `_pad1` → `uint materialIdx` in GLSL struct; extend `#ifdef MC2_COALESCE` block with `material_gpu.hglsl` include, MaterialTable SSBO, `u_materialGpuSample`; `effectiveLayer` switch
- `GameOS/gameos/gameosmain.cpp` — extend `[MATERIAL_GPU v1]` startup banner with `sample=%d`
- `docs/tier1_env_vars.md` — document `MC2_MATERIAL_GPU_SAMPLE`
- `tools/shader_reflect/` — targeted update of `static_prop.frag` golden only (see §10)

### Out of scope

- No change to `RenderCore/MaterialGpu.h` or `shaders/include/material_gpu.hglsl`
- No change to `PerDrawEntry` size (still 32 bytes)
- No change to normal map, PBR scalar fields, or emissive path (MaterialGpu-4+)
- No change to the legacy non-coalesce path (`#else` branch of `MC2_COALESCE`)
- No texture atlas / bindless upgrade
- No `materialFlags` migration (ALPHA_TEST_BIT still read from `PerDrawEntry.materialFlags`)

---

## 4. `PerDrawEntry` change

Replace `_pad1` at offset 28 with `materialIdx`. Struct size, stride, and all other
field offsets are unchanged.

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
                                //       when MC2_MATERIAL_GPU=1 and sidecar valid, else 0u)
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, materialIdx) == 28, "materialIdx offset");
```

Remove the old `static_assert(offsetof(PerDrawEntry, _pad1) == 28)` line.
All other static_asserts (offsets 0-24) are unchanged.

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
    uint  materialIdx;    // was _pad1; uint matches uint32_t at std430 offset 28
};
```

`int` and `uint` are both 4-byte aligned in std430. Changing `int _pad1` to
`uint materialIdx` does not affect the stride or any other field's offset.

---

## 5. `finalizeGeometry()` — fill `materialIdx` and sidecar validation

### 5.1 Sidecar valid flag

At the start of the `finalizeGeometry()` sidecar block (inside `if (s_materialGpuEnabled)`),
after the sidecar loop completes and before the PerDrawEntry entries loop, set:

```cpp
// C1 fix: record whether the sidecar is aligned with the emitted draw count.
// This flag gates sampling in flush(). If the sizes diverge, sampling is
// disabled for the whole pass — legacy texArrayLayer is the fallback, not
// a wrong-material-0 read.
s_materialGpuSidecarValid =
    s_materialGpuEnabled &&
    (s_packetMaterialIdx.size() == s_sortedPacketOrder.size());

if (!s_materialGpuSidecarValid && s_materialGpuEnabled) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[MATERIAL_GPU v1] ERROR materialIdx sidecar size mismatch"
                  " emitted=%zu sidecar=%zu sample_forced=0\n",
                  s_sortedPacketOrder.size(), s_packetMaterialIdx.size());
    std::fputs(buf, stderr);
}
```

When gate is OFF (`!s_materialGpuEnabled`): `s_materialGpuSidecarValid = false`.
This ensures `sampleOn` in flush() is always false when the upload gate is off.

### 5.2 Fill `materialIdx` in the PerDrawEntry entries loop

Inside the existing entries-build loop (where `PerDrawEntry e{}` is constructed),
add after the other field assignments:

```cpp
// MaterialGpu-3: fill materialIdx from v2 sidecar.
// Only when gate is ON and sidecar is valid (sizes match).
// Fall back to 0u otherwise — safe because sampleOn will be false
// if the sidecar is invalid, so materials[] is never accessed.
if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
    e.materialIdx = s_packetMaterialIdx[slotIndex];  // bounds safe: valid iff size match
} else {
    e.materialIdx = 0u;
}
```

### 5.3 Entry materialIdx compare log (m1 fix)

After the entries loop (outside it), when gate is ON and sidecar is valid, emit
a compare log to prove `PerDrawEntry.materialIdx` was filled correctly:

```cpp
// m1: verify PerDrawEntry.materialIdx matches s_packetMaterialIdx[slot].
// Catches assignment mistakes before shader sampling begins.
if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
    int entryMismatches = 0;
    const uint32_t emittedCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
    for (uint32_t i = 0; i < emittedCount; ++i) {
        if (entries[i].materialIdx != s_packetMaterialIdx[i]) {
            ++entryMismatches;
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "[MATERIAL_GPU v1] event=entry_material_idx emitted=%u mismatches=%d\n",
                  emittedCount, entryMismatches);
    std::fputs(buf, stderr);
}
```

This check runs inside `finalizeGeometry()` after `entries` has been built and before
the SSBO upload, so the comparison is valid. `entries` must be in scope at this point
(it is — it's a local `std::vector<PerDrawEntry>` in the same `{ }` scope).

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
    GLint materialGpuSample   = -1;   // MaterialGpu-3: u_materialGpuSample
};
```

### 6.2 Resolve in `loadProgramsIfNeeded()`

After the existing coalesce uniform lookups (alongside `drawIDBase`, `texArr`):

```cpp
s_locsCoalesce.materialGpuSample =
    glGetUniformLocation(s_staticPropProgramCoalesce, "u_materialGpuSample");

// M3 fix: missing uniform when both gates are ON is a validation error.
// The uniform must be present in the coalesce program after v3 ships.
// If it is absent (loc == -1) and both gates are ON, log an error.
// The if-guard in flush() already handles the -1 case safely (no-op),
// but the error log makes the failure observable without a debugger.
if (s_materialGpuEnabled && s_materialGpuSampleEnabled &&
    s_locsCoalesce.materialGpuSample < 0) {
    std::fputs("[MATERIAL_GPU v1] ERROR uniform_missing name=u_materialGpuSample\n",
               stderr);
}
```

A missing location with both gates OFF is silently fine — the non-coalesce program
and pre-v3 builds don't have the uniform.

### 6.3 Set in `flush()` — once per flush, before draw calls

At the same site where `fogValue` and `debugAddrMode` are set for the coalesce program:

```cpp
// MaterialGpu-3: compute sampleOn once per flush.
const bool sampleOn = s_materialGpuEnabled
                   && s_materialGpuSampleEnabled
                   && s_materialGpuSsbo != 0
                   && s_materialGpuSidecarValid;

if (s_locsCoalesce.materialGpuSample >= 0) {
    glUniform1i(s_locsCoalesce.materialGpuSample, sampleOn ? 1 : 0);
} else if (s_materialGpuEnabled && s_materialGpuSampleEnabled) {
    // Uniform absent when both gates are ON — sampling silently stays off.
    // Error was already logged at loadProgramsIfNeeded() time; don't flood.
}
```

Do NOT call `glGetUniformLocation` inside `flush()`.

### 6.4 Required `event=sample_mode` log (M2 fix)

**This log is required** (not optional). Emit once per `flush()` call, before the
draw loop, in the coalesce path:

```cpp
// M2: required diagnostic log — emitted once per flush so gate interaction
// is always observable without a debugger. One line per flush, not per draw.
{
    const char* reason = "ok";
    if (!s_materialGpuEnabled)       reason = "upload_env_off";
    else if (!s_materialGpuSampleEnabled) reason = "sample_env_off";
    else if (s_materialGpuSsbo == 0) reason = "no_ssbo";
    else if (!s_materialGpuSidecarValid) reason = "sidecar_invalid";
    else if (s_locsCoalesce.materialGpuSample < 0) reason = "uniform_missing";

    char buf[96];
    if (sampleOn) {
        std::snprintf(buf, sizeof(buf),
                      "[MATERIAL_GPU v1] event=sample_mode enabled=1 loc=%d\n",
                      s_locsCoalesce.materialGpuSample);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "[MATERIAL_GPU v1] event=sample_mode enabled=0 reason=%s\n",
                      reason);
    }
    std::fputs(buf, stderr);
}
```

This is the key diagnostic for debugging "SAMPLE=1 but no change." The `loc=N` on the
enabled path proves the uniform was found and set. The `reason=` on the disabled path
pinpoints exactly which condition blocked sampling.

**Note:** This emits once per `flush()` call — which is once per frame. For a 30-second
smoke run at 30-60 FPS, this produces ~1000-2000 log lines. This is acceptable for a
diagnostic slice. If log volume becomes a concern, add a `static int s_sampleLogCount`
cap (e.g. first 5 flushes only) — but leave the cap out of v3 to keep debugging clean.

---

## 7. Shader changes (`shaders/static_prop.frag`)

All changes are within the `#ifdef MC2_COALESCE` block. The `#else` (legacy uniform)
path is untouched.

### 7.1 Extend the existing `#ifdef MC2_COALESCE` block

Add the following immediately after the existing `uniform sampler2DArray u_texArr;`
line, still inside the `#ifdef MC2_COALESCE` / `#else` block (before `#endif`):

```glsl
// MaterialGpu-3: material table at binding 5.
// Only accessed when u_materialGpuSample != 0 (both MC2_MATERIAL_GPU and
// MC2_MATERIAL_GPU_SAMPLE set, SSBO uploaded, sidecar valid).
// Always declared in the coalesce variant so the reflection surface is stable.
// RENDER CONTRACT: static_prop.frag coalesce declares MaterialTable at binding 5
//   after MaterialGpu-3. Binding 5 may be unbound when u_materialGpuSample=0;
//   shader MUST NOT access materials[] in that case. This is enforced by the
//   runtime uniform branch below.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
};
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table
```

The full `#ifdef MC2_COALESCE` block structure after the edit:
```glsl
#ifdef MC2_COALESCE
    // ... existing PerDrawEntry struct (with uint materialIdx at offset 28) ...
    // ... existing layout(std430, binding = 4) readonly buffer PerDrawData ...
    // ... existing uniform sampler2DArray u_texArr ...
    #include <include/material_gpu.hglsl>
    layout(std430, binding = 5) readonly buffer MaterialTable {
        MaterialGpu materials[];
    };
    uniform int u_materialGpuSample;
#else
    // ... existing legacy uniforms unchanged ...
#endif
```

`#include <include/material_gpu.hglsl>` matches the existing include style
(`#include <include/render_contract.hglsl>` is already on line 9).

### 7.2 Read `materialIdx` from PerDrawEntry

In the existing per-draw SSBO read block (alongside `texArrayLayer`, `uvScaleX`, etc.),
add one new line:

```glsl
    uint  materialIdx  = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;  // NEW
```

Full read block with the addition:
```glsl
    int   materialFlags    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int   packetID         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].packetID;
    int   maxLocalVertexID = perDraw_.entries[v_drawID + uint(u_drawIDBase)].maxLocalVertexID;
    int   texArrayLayer    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    float uvScaleX         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx      = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;
```

(`objectIdRaw` is read separately under `#ifdef MC2_OBJECT_ID_BUFFER` — leave that untouched.)

### 7.3 Effective layer selection

Replace the existing texture sample:
```glsl
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(texArrayLayer)));
```

With:
```glsl
    // MaterialGpu-3: runtime switch between legacy layer and material table.
    // u_materialGpuSample is a pass-wide (not per-fragment) uniform —
    // the branch collapses to a single predicate on AMD hardware.
    // materials[] is only accessed when u_materialGpuSample != 0,
    // enforcing the render contract above (binding 5 must be set when sampling).
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            // kMatTexAbsent = 0xFFFFFFFFu (defined in material_gpu.hglsl).
            // v2 mismatches=0 strongly predicts this guard is never true for
            // emitted static-prop packets. Retained defensively: if it fires,
            // texArrayLayer is used as fallback (no missing-texture corruption).
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
```

---

## 8. `gameosmain.cpp` startup banner

Extend the existing `[MATERIAL_GPU v1]` banner to include `sample=%d`:

```cpp
// [MATERIAL_GPU v1] startup banner — add sample gate alongside enabled.
// Both checks duplicate the env test (s_materialGpuEnabled/SampleEnabled are
// private to the batcher TU — Option A from MaterialGpu-2 spec §3).
{
    const bool matGpuOn    = (getenv("MC2_MATERIAL_GPU")        != nullptr);
    const bool matSampleOn = (getenv("MC2_MATERIAL_GPU_SAMPLE") != nullptr);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "[MATERIAL_GPU v1] enabled=%d sample=%d binding=5\n",
                  (int)matGpuOn, (int)matSampleOn);
    std::fputs(buf, stderr);
}
```

`char buf[64]`: max output `[MATERIAL_GPU v1] enabled=1 sample=1 binding=5\n` = 44 chars + NUL.

---

## 9. `docs/tier1_env_vars.md` additions

In the existing `## MaterialGpu sidecar (MaterialGpu-2)` section, add after the
`MC2_MATERIAL_GPU=1` bullet:

```markdown
- `MC2_MATERIAL_GPU_SAMPLE=1` — enable MaterialGpu shader sampling (MaterialGpu-3).
  Requires `MC2_MATERIAL_GPU=1` to have any effect (both required for `u_materialGpuSample=1`).
  Default OFF. When both active: `static_prop.frag` reads `materials[materialIdx].albedoTex`
  instead of `PerDrawEntry.texArrayLayer`. Expected result: zero pixel delta (same layer index).
  Log: `event=sample_mode enabled=1 loc=N` (once per flush). Diagnostic reason codes:
  `upload_env_off | sample_env_off | no_ssbo | sidecar_invalid | uniform_missing`.
```

Also update the static_prop SSBO binding registry to reflect the new runtime contract:

```markdown
| 5 | MaterialGpu | ACTIVE (v3+) — always declared in static_prop.frag coalesce variant;
|   |             | SSBO bound when MC2_MATERIAL_GPU=1, unbound otherwise.
|   |             | Shader accesses only when u_materialGpuSample=1. |
```

---

## 10. Reflection golden update — targeted, `static_prop.frag` only

**Only the `static_prop.frag` golden may change.** If `reflect.py --update` would
modify any other golden, stop and investigate — that is unexpected drift.

Expected changes to `static_prop.frag` golden:

1. New SSBO at binding 5, block name `MaterialTable`, `array_stride=32`
2. `MaterialGpu` struct members: `albedoTex` (offset 0), `normalTex` (4),
   `metallicRoughnessTex` (8), `emissiveTex` (12), `flags` (16),
   `baseColorFactor` (20), `metallicFactor` (24), `roughnessFactor` (28)
3. New uniform `u_materialGpuSample`, type `int`
4. `PerDrawEntry` member at offset 28: name changes from `_pad1` to `materialIdx`,
   type changes from `int` to `uint` (same stride)
5. No CONTRACT_VIOLATION

**Targeted update procedure:**

```powershell
# 1. Run update
py -3 tools/shader_reflect/reflect.py --update

# 2. Check which goldens changed — ONLY static_prop.frag variants should appear
git diff --name-only tools/shader_reflect/

# 3. Inspect the static_prop.frag diff for the expected changes above
git diff tools/shader_reflect/

# 4. If any non-static_prop golden changed: STOP. Do not commit. Investigate.
# 5. If only static_prop goldens changed and they match expectations: commit.
```

---

## 11. Log format

```
Startup:
  [MATERIAL_GPU v1] enabled=0 sample=0 binding=5   ← gates OFF
  [MATERIAL_GPU v1] enabled=1 sample=0 binding=5   ← upload-only (v2 mode)
  [MATERIAL_GPU v1] enabled=1 sample=1 binding=5   ← both gates ON

Per finalizeGeometry() (gate ON, sidecar valid):
  [MATERIAL_GPU v1] event=table_upload materials=N bytes=B emitted=M
  [MATERIAL_GPU v1] event=compare emitted=M mismatches=0
  [MATERIAL_GPU v1] event=entry_material_idx emitted=M mismatches=0
  [MATERIAL_GPU v1] event=unload materials=N bytes=B   ← on mission end

Per flush() (coalesce path, once per flush):
  [MATERIAL_GPU v1] event=sample_mode enabled=1 loc=N
  [MATERIAL_GPU v1] event=sample_mode enabled=0 reason=<reason>

  Reason codes:
    upload_env_off    — MC2_MATERIAL_GPU not set
    sample_env_off    — MC2_MATERIAL_GPU_SAMPLE not set
    no_ssbo           — s_materialGpuSsbo == 0 (table not uploaded yet)
    sidecar_invalid   — s_packetMaterialIdx.size() != s_sortedPacketOrder.size()
    uniform_missing   — u_materialGpuSample location is -1

Errors:
  [MATERIAL_GPU v1] ERROR materialIdx sidecar size mismatch emitted=M sidecar=S sample_forced=0
  [MATERIAL_GPU v1] ERROR uniform_missing name=u_materialGpuSample
```

---

## 12. Required v3 gates

All must pass before declaring MaterialGpu-3 complete:

| Gate | Verification |
|---|---|
| **Tier1 5/5, all gates OFF** | No MC2_* set. Exit 0, 5/5 PASS. Startup: `enabled=0 sample=0` |
| **Tier1 5/5, upload-only** | `MC2_MATERIAL_GPU=1` only. Exit 0, 5/5 PASS. Startup: `enabled=1 sample=0`. Logs: `event=sample_mode enabled=0 reason=sample_env_off` each flush |
| **Tier1 5/5, sample-only (SAMPLE=1 alone)** | `MC2_MATERIAL_GPU_SAMPLE=1` only (upload gate unset). Exit 0, 5/5 PASS. Logs: `event=sample_mode enabled=0 reason=upload_env_off` |
| **Tier1 5/5, both gates ON** | `MC2_MATERIAL_GPU=1 MC2_MATERIAL_GPU_SAMPLE=1`. Exit 0, 5/5 PASS. Startup: `enabled=1 sample=1` |
| **No GL errors** | No `GL ERROR` lines in both-gates-ON logs |
| **No sidecar mismatch** | No `ERROR materialIdx sidecar size mismatch` in both-gates-ON logs |
| **`event=sample_mode enabled=1`** | Both-gates-ON logs show `enabled=1 loc=N` (N >= 0) each flush |
| **`event=entry_material_idx mismatches=0`** | All missions show `mismatches=0` in both-gates-ON logs |
| **Pixel parity** | v2 `mismatches=0` strongly predicts zero delta; shader read path is new so parity is still required. If harness captures screenshots: diff = 0. If not: PASS 5/5 + no GL ERROR accepted. Document criterion. |
| **Reflection golden passes** | `reflect.py` exit 0 after targeted golden update (§10), no CONTRACT_VIOLATION, no unexpected files changed |

---

## 13. Files changed

| File | Change |
|---|---|
| `GameOS/gameos/gos_static_prop_batcher.h` | EDIT — `_pad1` → `uint32_t materialIdx`; replace static_assert |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | EDIT — `s_materialGpuSampleEnabled`; `s_materialGpuSidecarValid`; fill `materialIdx`; sidecar mismatch guard; `ProgramLocs::materialGpuSample`; resolve + error-log uniform; set uniform in flush(); `event=sample_mode` log; `event=entry_material_idx` compare |
| `shaders/static_prop.frag` | EDIT — `uint materialIdx` in GLSL struct; MaterialTable SSBO + include + `u_materialGpuSample` under MC2_COALESCE; `effectiveLayer` switch |
| `GameOS/gameos/gameosmain.cpp` | EDIT — extend banner with `sample=%d` |
| `docs/tier1_env_vars.md` | EDIT — `MC2_MATERIAL_GPU_SAMPLE` bullet; binding-5 status update |
| `tools/shader_reflect/*.json` | EDIT — `static_prop.frag` golden only (see §10) |

`RenderCore/MaterialGpu.h`, `shaders/include/material_gpu.hglsl` — **NOT changed.**

---

## 14. v4 handoff

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
