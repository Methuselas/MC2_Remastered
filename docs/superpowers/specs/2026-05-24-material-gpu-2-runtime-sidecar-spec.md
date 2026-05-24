# MaterialGpu-2: Runtime Sidecar Upload

**Date:** 2026-05-24
**Status:** Approved for planning
**Slice:** MaterialGpu-2 (runtime table + SSBO upload only — no shader sampling)
**Predecessor:** MaterialGpu-1 (reflect/contract fixture, shipped 2026-05-24)
**Successor:** MaterialGpu-3 (first shader sampling switch, gated on pixel-parity proof)

---

## 1. Goal

Build a static-prop-only `MaterialGpu` runtime table from existing `texArrayLayer`
values, upload it as a mission-lifetime SSBO at binding 5, and validate that every
packet's `materialIndex → MaterialGpu.albedoTex` exactly matches the legacy
`texArrayLayer` path.

No `PerDrawEntry` layout change. No shader change. No visual change. No texture
sampling change. Binding 5 is live in GL state but has no shader consumer in v2.

**Canonical scope sentence:**

> MaterialGpu v2 wires a static-prop-only MaterialGpu runtime table and SSBO upload
> beside the existing material path. It assigns and validates material indices but
> does not change shader sampling or visual output.

---

## 2. Scope

### In scope

- `gos_static_prop_batcher.cpp` — new file-scope state + `finalizeGeometry()` +
  `flush()` + `onMapUnload()` additions, all gated on `MC2_MATERIAL_GPU`
- `gameosmain.cpp` — startup log line (`[MATERIAL_GPU v1] enabled=0/1 binding=5`)
- `docs/tier1_env_vars.md` — document `MC2_MATERIAL_GPU`

### Out of scope

- No `PerDrawEntry` field added (`materialIdx` deferred to MaterialGpu-3)
- No `static_prop.frag` / `static_prop.vert` change
- No GLSL mirror change (`shaders/include/material_gpu.hglsl` untouched)
- No `scripts/check-material-gpu-mirror.sh` change
- No texture binding behavior changed
- No draw order or visual output changed
- No material cook pipeline

---

## 3. Env gate

```
MC2_MATERIAL_GPU=1   → full sidecar active (build, upload, bind, compare)
MC2_MATERIAL_GPU     unset (default) → startup log only; zero new code runs
```

**Gate controls the entire sidecar.** When `MC2_MATERIAL_GPU` is unset, no table is
built, no buffer is created or uploaded, no SSBO is bound, no compare runs. Default
env-OFF behavior is as close to HEAD as possible.

Gate implementation — file-scope constant at the top of the anonymous namespace in
`gos_static_prop_batcher.cpp`, consistent with existing env-gate patterns in that
file (Pattern A):

```cpp
static const bool s_materialGpuEnabled =
    (getenv("MC2_MATERIAL_GPU") != nullptr);
```

Startup log (gameosmain.cpp, alongside existing env-var reports):

```cpp
// Alongside MC2_GPU_OBJECTS etc.
OutputDebugString("[MATERIAL_GPU v1] enabled=%d binding=5\n",
                  (int)s_materialGpuEnabled);
```

---

## 4. New file-scope state

Add to the anonymous namespace of `gos_static_prop_batcher.cpp`, alongside the
existing `s_perDrawSsbo` / `s_perTypeSsbo` declarations:

```cpp
// MaterialGpu-2 sidecar — active only when MC2_MATERIAL_GPU=1.
// No shader consumer until MaterialGpu-3.
static std::vector<uint32_t>                s_packetMaterialIdx;   // per-packet
static std::vector<RenderCore::MaterialGpu> s_materialGpuTable;    // deduplicated
static GLuint                               s_materialGpuSsbo = 0;
```

`s_packetMaterialIdx[i]` maps packet index `i` (same ordering as the `entries[]`
loop in `finalizeGeometry()`) to a `MaterialGpu` table index.

`s_materialGpuTable` is deduplicated by `texArrayLayer` value: two packets sharing
the same `texArrayLayer` share one `MaterialGpu` entry. For v2, this deduplication
is the only source of table entries — no cook pipeline, no manifest.

`s_materialGpuSsbo` is 0 until `finalizeGeometry()` uploads it, and 0 again after
`onMapUnload()` deletes it.

---

## 5. Default material record

When inserting a new entry for `texArrayLayer = layer`:

```cpp
RenderCore::MaterialGpu m = {};
m.albedoTex            = static_cast<uint32_t>(layer);
m.normalTex            = RenderCore::kMaterialTexAbsent;   // 0xFFFFFFFFu
m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
m.emissiveTex          = RenderCore::kMaterialTexAbsent;
m.flags                = 0;
m.baseColorFactor      = 1.0f;   // neutral: full brightness, no tint
m.metallicFactor       = 0.0f;
m.roughnessFactor      = 0.0f;
```

The non-`albedoTex` fields are placeholders. They carry no semantic meaning in v2
because the shader does not sample them. They will be replaced by real values from
the material cook pipeline in a future slice.

`baseColorFactor = 1.0f` is the neutral identity value (full brightness). Storing
`0xFFFFFFFFu` into a `float` field would produce a quiet NaN — do not do this.

---

## 6. `finalizeGeometry()` additions

All additions are guarded by `if (!s_materialGpuEnabled) return; // early-out` at
the top of the new block, inserted **after** the existing `texArrayLayer` population
loop (i.e., after `layerForPacket[]` is fully built, before the `glBufferData` calls
for `s_perDrawSsbo`).

```
// --- MaterialGpu-2 sidecar ---
if (s_materialGpuEnabled) {

    s_packetMaterialIdx.clear();
    s_materialGpuTable.clear();

    // Build the map: texArrayLayer -> materialIdx.
    // layerForPacket[i] was already computed by the existing loop above.
    std::unordered_map<int32_t, uint32_t> layerToMaterialIdx;
    const int packetCount = (int)layerForPacket.size();

    for (int i = 0; i < packetCount; ++i) {
        int32_t layer = layerForPacket[i];
        // try_emplace: inserts only if key absent (C++17).
        auto [it, inserted] = layerToMaterialIdx.try_emplace(
            layer, (uint32_t)s_materialGpuTable.size());
        if (inserted) {
            RenderCore::MaterialGpu m = {};
            m.albedoTex            = (uint32_t)layer;
            m.normalTex            = RenderCore::kMaterialTexAbsent;
            m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
            m.emissiveTex          = RenderCore::kMaterialTexAbsent;
            m.flags                = 0;
            m.baseColorFactor      = 1.0f;
            m.metallicFactor       = 0.0f;
            m.roughnessFactor      = 0.0f;
            s_materialGpuTable.push_back(m);
        }
        s_packetMaterialIdx.push_back(it->second);
    }

    // Upload — mission/map lifetime, not per-frame.
    // GL_STATIC_DRAW: table is built once per map, freed on unload.
    const size_t byteSize =
        s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);
    glGenBuffers(1, &s_materialGpuSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_materialGpuSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)byteSize,
                 s_materialGpuTable.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // GL error check (see §9).
    // GL error check — drain then sample, matching batcher pattern.
    while (glGetError() != GL_NO_ERROR) {}
    const GLenum uploadErr = glGetError();
    if (uploadErr != GL_NO_ERROR)
        OutputDebugString("[MATERIAL_GPU v1] GL ERROR after upload: 0x%x\n", uploadErr);

    // Log upload (also logs when packetCount == 0).
    OutputDebugString("[MATERIAL_GPU v1] event=table_upload materials=%zu"
                      " bytes=%zu packets=%d\n",
                      s_materialGpuTable.size(), byteSize, packetCount);

    // Debug compare: every packet's albedoTex must match its texArrayLayer.
    int mismatches = 0;
    for (int i = 0; i < packetCount; ++i) {
        uint32_t idx      = s_packetMaterialIdx[i];
        uint32_t albedo   = s_materialGpuTable[idx].albedoTex;
        uint32_t expected = (uint32_t)layerForPacket[i];
        if (albedo != expected) {
            OutputDebugString("[MATERIAL_GPU v1] MISMATCH packet=%d"
                              " materialIdx=%u albedoTex=%u expected=%u\n",
                              i, idx, albedo, expected);
            ++mismatches;
        }
    }
    OutputDebugString("[MATERIAL_GPU v1] event=compare packets=%d"
                      " mismatches=%d\n", packetCount, mismatches);
}
```

Mismatches are logged as warnings — not `assert()`, not crash. The v2 gate requires
`mismatches=0` as a validation condition, but the code continues executing to
preserve smoke-log collection even on unexpected failures.

---

## 7. `flush()` additions

Bind the SSBO at binding 5 when the gate is active and the buffer exists. Add
alongside the existing slot-2 bind at `glBindBufferBase(..., 2, s_perTypeSsbo)`:

```cpp
if (s_materialGpuEnabled && s_materialGpuSsbo != 0) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, s_materialGpuSsbo);
    while (glGetError() != GL_NO_ERROR) {}
    const GLenum bindErr = glGetError();
    if (bindErr != GL_NO_ERROR)
        OutputDebugString("[MATERIAL_GPU v1] GL ERROR after bind: 0x%x\n", bindErr);
}
```

No shader declares binding 5 in v2 — the GL bind is a no-op from the shader's
perspective. This proves the bind path compiles and runs without driver error.

---

## 8. `onMapUnload()` additions

Add alongside the existing per-map SSBO teardowns:

```cpp
if (s_materialGpuSsbo != 0) {
    const size_t byteSize =
        s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);
    OutputDebugString("[MATERIAL_GPU v1] event=unload materials=%zu bytes=%zu\n",
                      s_materialGpuTable.size(), byteSize);
    glDeleteBuffers(1, &s_materialGpuSsbo);
    s_materialGpuSsbo = 0;
}
s_packetMaterialIdx.clear();
s_materialGpuTable.clear();
```

Teardown runs regardless of gate state (guards against any partial-init edge cases),
but in practice `s_materialGpuSsbo` is 0 when the gate is off.

---

## 9. GL error checking

The batcher uses an inline drain-then-sample pattern: drain all pending errors with
`while (glGetError() != GL_NO_ERROR) {}`, then sample once and log if non-zero.
Follow this exact pattern at both call sites (already shown in §6 and §7 code).
Do not add new GL-error infrastructure in v2 — use what exists.

**Gate for v2 verification:** both call sites must be error-free in a smoke run with
`MC2_MATERIAL_GPU=1`.

---

## 10. Binding 5 registry

Binding 5 is reserved for the `MaterialGpu` SSBO. This registration must be
documented in:

- `docs/tier1_env_vars.md` — gate documentation + binding registry
- The comment block in `gos_static_prop_batcher.cpp` above the SSBO declarations

```
Binding  Owner                     Status
-------  ------------------------  -----------------------------------------
0        Instances                 active
1        Colors (legacy)           active
2        PerType                   active
3        Parity (debug)            active (MC2_OBJECT_PARITY_CHECK=1)
4        PerDraw                   active (coalesce path)
5        MaterialGpu               PROVISIONAL — v2 binds, no shader consumer
                                   v3 makes this load-bearing
...
```

**v3 handoff:** MaterialGpu-3 will add `materialIdx` to `PerDrawEntry`, update the
GLSL mirror, and add a `materials[]` SSBO declaration to `static_prop.frag`. At that
point binding 5 becomes shader-load-bearing and the provisional label is removed.

---

## 11. Required v2 gates

All must pass before declaring MaterialGpu-2 complete:

| Gate | Verification |
|---|---|
| **Tier1 5/5, gate OFF** | `py -3 run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs` with `MC2_MATERIAL_GPU` unset |
| **`materials > 0` on missions with static props** | `[MATERIAL_GPU v1] event=table_upload materials=N` with N > 0 in tier1 logs |
| **Upload log present** | `[MATERIAL_GPU v1] event=table_upload` line exists in gate-ON smoke log |
| **Reflection gate** | Already satisfied by MaterialGpu-1 fixture (`array_stride=32`, all 8 offsets correct) |
| **Compare passes** | `[MATERIAL_GPU v1] event=compare mismatches=0` in gate-ON smoke log |
| **No GL errors** | No GL error output at upload or bind call sites in gate-ON smoke run |
| **No pixel delta** | Visual smoke with `MC2_MATERIAL_GPU=1`: no visual change vs gate-OFF |

Tier1 smoke gate command (canonical):
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## 12. Log summary

```
[MATERIAL_GPU v1] enabled=1 binding=5        ← startup, gate ON
[MATERIAL_GPU v1] enabled=0                  ← startup, gate OFF (no further output)

[MATERIAL_GPU v1] event=table_upload materials=N bytes=B packets=P
[MATERIAL_GPU v1] event=compare packets=P mismatches=0
[MATERIAL_GPU v1] MISMATCH packet=I materialIdx=U albedoTex=A expected=E  ← if any
[MATERIAL_GPU v1] event=unload materials=N bytes=B
```

When env ON but no static props (e.g., pure terrain mission):
```
[MATERIAL_GPU v1] event=table_upload materials=0 bytes=0 packets=0
[MATERIAL_GPU v1] event=compare packets=0 mismatches=0
```

No special branch needed — the loop runs over zero elements and logs naturally.

---

## 13. Files changed

| File | Change |
|---|---|
| `GameOS/gameos/gos_static_prop_batcher.cpp` | EDIT — new state + finalizeGeometry + flush + onMapUnload additions |
| `GameOS/gameos/gameosmain.cpp` | EDIT — startup log line |
| `docs/tier1_env_vars.md` | EDIT — document `MC2_MATERIAL_GPU` + binding 5 reservation |

`RenderCore/MaterialGpu.h` — **NOT changed.** Header comment "NOT WIRED" removed
only when the first shader consumer is added (MaterialGpu-3).

`shaders/include/material_gpu.hglsl`, `shaders/static_prop.frag`,
`shaders/static_prop.vert` — **NOT changed.**

---

## 14. v3 handoff

MaterialGpu-3 is the first shader behavior switch:

```
MaterialGpu v3:
  add materialIdx to PerDrawEntry (C++ + GLSL mirror)
  update static_prop.frag to read materials[materialIdx].albedoTex
  retain texArrayLayer as legacy fallback
  A/B compare under separate env gate
  pixel-parity required (zero visual delta vs v2)
```

v2 failure → table construction / upload / binding / lifecycle is wrong.
v3 failure → shader-visible `PerDrawEntry` field / GLSL layout / shader sampling.
That separation is the whole point.
