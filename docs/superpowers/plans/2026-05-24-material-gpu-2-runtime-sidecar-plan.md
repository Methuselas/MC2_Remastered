# MaterialGpu-2: Runtime Sidecar Upload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a static-prop-only `MaterialGpu` runtime table from existing `texArrayLayer` values, upload it as a mission-lifetime SSBO at binding 5, bind it in `flush()`, and validate that every emitted draw slot's `albedoTex` matches the legacy `texArrayLayer` — all gated on `MC2_MATERIAL_GPU=1`, zero shader changes.

**Architecture:** All new state lives in `gos_static_prop_batcher.cpp`'s anonymous namespace. The sidecar runs inside `finalizeGeometry()`'s existing `{}` block, after `s_sortedPacketOrder` is populated, iterating it directly so `s_packetMaterialIdx.size() == s_sortedPacketOrder.size()` — matching the emitted `PerDrawEntry` count slot-for-slot. `flush()` binds the SSBO at slot 5 when the gate is active and the buffer exists. `onMapUnload()` tears it down alongside the other per-map SSBOs. `gameosmain.cpp` emits a one-line startup banner.

**Tech Stack:** C++17 (`try_emplace`), OpenGL SSBO (`glBufferData`/`glBindBufferBase`), `std::vector<uint32_t>`, `std::vector<RenderCore::MaterialGpu>`, `std::unordered_map<int32_t, uint32_t>`.

---

## Worktree context

All paths are relative to the worktree root:
```
A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\
```

Build command (ALWAYS `--config RelWithDebInfo` — Release crashes with `GL_INVALID_ENUM`):
```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Deploy command (copy changed binaries only — NEVER `cp -r`):
```powershell
Copy-Item -Force "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe" "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
```

Smoke gate (canonical — verbatim, do not modify):
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Gate-ON smoke (set env var before calling — PowerShell inherits it to mc2.exe subprocess):
```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

Gate-ON logs land in:
```
A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts\<timestamp>\
```

---

## Packet-alignment invariant (read before implementing Task 3)

`gos_static_prop_batcher.cpp` has two packet-count concepts:

| Symbol | Size | Meaning |
|---|---|---|
| `s_packets.size()` | N (all packets) | Every registered packet, including those with `layerForPacket[i] == -1` (texture unavailable, skipped at draw time) |
| `s_sortedPacketOrder.size()` | M ≤ N | Only packets with `layerForPacket[i] >= 0`, in alpha-sorted draw order |

`PerDrawEntry entries` (the existing SSBO at binding 4) has `M` entries. The fragment shader reads `entries[gl_DrawID + base]` where `gl_DrawID` is the slot index in `s_sortedPacketOrder`.

**`s_packetMaterialIdx` MUST have `M` entries** so that in v3 the shader can index `materials[materialIdx]` using the same `gl_DrawID`. Do NOT iterate `layerForPacket[0..N-1]` — that produces `N` entries and breaks the slot mapping. Iterate `s_sortedPacketOrder` directly; every entry there already has `layerForPacket >= 0`.

---

## File structure

| File | Change type | What changes |
|---|---|---|
| `GameOS/gameos/gos_static_prop_batcher.cpp` | EDIT (bulk) | New include + gate + state; sidecar in `finalizeGeometry()`; bind in `flush()`; teardown in `onMapUnload()` |
| `GameOS/gameos/gameosmain.cpp` | EDIT (small) | One-line startup log after `[INSTR v1]` banner |
| `docs/tier1_env_vars.md` | EDIT (small) | New section + SSBO binding registry |

`RenderCore/MaterialGpu.h`, `shaders/include/material_gpu.hglsl`, `shaders/static_prop.frag`, `shaders/static_prop.vert` — **NOT changed.**

---

## Task 1: Binding-5 conflict pre-flight and stderr sink verification

**Files:** none changed (verification only)

- [ ] **Step 1: Verify smoke logs capture stderr**

Every v2 gate depends on grepping `[MATERIAL_GPU v1]` lines from smoke logs. All `[MATERIAL_GPU v1]` output uses `std::fputs(buf, stderr)`. If the smoke harness only captures stdout, every log-grep gate will false-fail.

Run a quick gate-OFF smoke on one mission:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 10 --kill-existing --keep-logs
```

Then grep the most recent log for any existing `fprintf(stderr)` output. The `[UNIFIED_PROJ v1]` warning is a reliable stderr canary that emits early on most runs — check for it, or for any other bracketed log tag known to use fprintf(stderr):
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "INSTR v1|UNIFIED_PROJ|RENDER_WORLD"
```

**If stderr lines appear in the log:** `std::fputs(buf, stderr)` is the correct log path. Proceed as written.

**If stderr lines do NOT appear in the log (only stdout):** Replace every `std::fputs(buf, stderr)` in Tasks 3, 4, 5, and 6 with `puts(buf)` to match the `[INSTR v1]` banner pattern (`puts(_cbbuf)`) before implementing any task. Report this substitution in your execution summary.

- [ ] **Step 2: Search all shader files for binding 5**

`grep` may not exist in vanilla PowerShell. Use PowerShell-native search:
```powershell
Get-ChildItem shaders -Recurse -Include *.frag,*.vert,*.comp,*.tesc,*.tese |
  Select-String -Pattern "binding\s*=\s*5"
```

Expected output — exactly one match:
```
shaders\fixtures\material_gpu_contract.frag:14:layout(std430, binding = 5) readonly buffer MaterialTable {
```

If any production shader (not in `shaders\fixtures\`) appears: **STOP. Do not proceed.** Report the conflict — binding 5 is reserved for MaterialGpu and a collision must be resolved before v2 ships.

If only the fixture line appears: safe to proceed.

---

## Task 2: Add include, gate, and state to batcher

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

### Context for this file

The file is ~4200 lines. The first few lines are `#include` directives; near line 260 the anonymous-namespace state variables are declared (`s_perDrawSsbo`, `s_perTypeSsbo`, etc.).

- [ ] **Step 1: Add the MaterialGpu and `<cstdio>` includes**

In `GameOS/gameos/gos_static_prop_batcher.cpp`, check whether `<cstdio>` is already directly included (scan the existing `#include` block near line 1-25). It is present (`#include <cstdio>` already exists) — do NOT add a duplicate. If for any reason it is absent, add it.

After line 3:
```cpp
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe
```

Add directly below it:
```cpp
#include "../../RenderCore/MaterialGpu.h"   // MaterialGpu-2: sidecar upload
```

The full include block (lines 1-3 + new line 4) becomes:
```cpp
#include "gos_static_prop_batcher.h"
#include "gos_static_prop_registry.h"    // M1.5: getRecipeIndexForType
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled + objectIdRawForStaticPropRecipe
#include "../../RenderCore/MaterialGpu.h"   // MaterialGpu-2: sidecar upload
```

- [ ] **Step 2: Add gate and state variables**

Find the line declaring `s_perDrawSsbo` (currently around line 260):
```cpp
GLuint s_perDrawSsbo               = 0;  // PerDrawEntry per type, sorted (binding 4)
```

After that line, add the gate constant and three new state variables. Add them as a clearly labelled block:
```cpp
// MaterialGpu-2 sidecar — active only when MC2_MATERIAL_GPU=1.
// No shader consumer until MaterialGpu-3.
// s_packetMaterialIdx[i] maps draw slot i (= s_sortedPacketOrder[i] position)
// to its entry in s_materialGpuTable.
// Size invariant: s_packetMaterialIdx.size() == s_sortedPacketOrder.size().
static const bool s_materialGpuEnabled =
    (getenv("MC2_MATERIAL_GPU") != nullptr);
static std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
static std::vector<RenderCore::MaterialGpu> s_materialGpuTable;   // deduplicated
static GLuint                               s_materialGpuSsbo = 0;
```

- [ ] **Step 3: Build to verify compile**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Expected: build succeeds with no errors related to `MaterialGpu.h` or the new state variables.

If you get `Cannot open include file: '../../RenderCore/MaterialGpu.h'`: verify the file exists at `RenderCore/MaterialGpu.h` in the worktree root. The batcher lives at `GameOS/gameos/gos_static_prop_batcher.cpp`, so the relative path `../../RenderCore/MaterialGpu.h` is correct.

- [ ] **Step 4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-2): add MaterialGpu.h include, gate, and state variables"
```

---

## Task 3: `finalizeGeometry()` sidecar

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

### Where to insert

Locate this block in `finalizeGeometry()` (around line 1988-2020 after Task 2's edits shift line numbers slightly):

```cpp
    s_sortedPacketOrder.clear();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
    {
        // 2026-05-11 packet-skip rule: only enroll packets whose texture-
        // ...
        for (uint8_t group = 0; group <= 1; ++group) {
            for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
                // ...
                for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                    const uint32_t globalPktIdx = type.firstPacket + pIdx;
                    if (layerForPacket[globalPktIdx] < 0) continue;
                    s_sortedPacketOrder.push_back(globalPktIdx);
                }
            }
            if (group == 0u) {
                s_alphaOffCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
            } else {
                s_alphaOnCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size())
                                  - s_alphaOffCmdCount;
            }
        }
        // <<< INSERT SIDECAR HERE, before the next line >>>
        std::vector<PerDrawEntry> entries(s_sortedPacketOrder.size());
```

The closing `}` of the `group=0..1` for-loop is the anchor. Insert the sidecar block in the blank line between that closing `}` and `std::vector<PerDrawEntry> entries(...)`. The sidecar stays **inside** the outer `{ }` scope that started with `s_sortedPacketOrder.clear()` — it can see `layerForPacket` (declared earlier in `finalizeGeometry()`) and `s_sortedPacketOrder` (just populated above).

- [ ] **Step 1: Insert the sidecar block**

```cpp
        // --- MaterialGpu-2 sidecar (MC2_MATERIAL_GPU=1 only) ---
        // Runs after s_sortedPacketOrder is populated (skip-filtered, layer >= 0 only).
        // Iterates s_sortedPacketOrder in draw-slot order so that:
        //   s_packetMaterialIdx.size() == s_sortedPacketOrder.size() == PerDrawEntry count
        // This is the v3 invariant: draw slot i reads materials[s_packetMaterialIdx[i]].
        if (s_materialGpuEnabled) {
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();

            std::unordered_map<int32_t, uint32_t> layerToMaterialIdx;
            const uint32_t emittedCount =
                static_cast<uint32_t>(s_sortedPacketOrder.size());

            // Iterate in draw-slot order (same as PerDrawEntry build below).
            // All entries in s_sortedPacketOrder have layerForPacket >= 0 (skip rule above).
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const int32_t  layer        = layerForPacket[globalPktIdx]; // >= 0 guaranteed

                auto [it, inserted] = layerToMaterialIdx.try_emplace(
                    layer, static_cast<uint32_t>(s_materialGpuTable.size()));
                if (inserted) {
                    RenderCore::MaterialGpu m = {};
                    m.albedoTex            = static_cast<uint32_t>(layer);
                    m.normalTex            = RenderCore::kMaterialTexAbsent;
                    m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
                    m.emissiveTex          = RenderCore::kMaterialTexAbsent;
                    m.flags                = 0;
                    m.baseColorFactor      = 1.0f;   // neutral: full brightness
                    m.metallicFactor       = 0.0f;
                    m.roughnessFactor      = 0.0f;
                    s_materialGpuTable.push_back(m);
                }
                s_packetMaterialIdx.push_back(it->second);
            }

            // --- Upload ---
            // GL_STATIC_DRAW: table is mission/map lifetime, not per-frame.
            // Guard byteSize > 0: some drivers misbehave on a zero-byte glBufferData.
            // When no static props exist, s_materialGpuSsbo stays 0 and flush() skips the bind.
            const size_t byteSize =
                s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);

            if (byteSize > 0) {
                // Idempotent: delete any buffer from a prior finalizeGeometry call.
                // finalizeGeometry() should be once-per-map, but defensive cleanup
                // prevents leaks if it is ever called again (partial reinit, hot-reload, etc.).
                if (s_materialGpuSsbo != 0) {
                    glDeleteBuffers(1, &s_materialGpuSsbo);
                    s_materialGpuSsbo = 0;
                }
                while (glGetError() != GL_NO_ERROR) {}  // drain stale BEFORE operation
                glGenBuffers(1, &s_materialGpuSsbo);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_materialGpuSsbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(byteSize),
                             s_materialGpuTable.data(),
                             GL_STATIC_DRAW);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                const GLenum uploadErr = glGetError();  // sample THIS operation AFTER
                if (uploadErr != GL_NO_ERROR) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[MATERIAL_GPU v1] GL ERROR after upload: 0x%x\n",
                                  uploadErr);
                    std::fputs(buf, stderr);
                }
            }
            // s_materialGpuSsbo remains 0 when byteSize == 0.

            // --- Log upload ---
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] event=table_upload"
                              " materials=%zu bytes=%zu emitted=%u\n",
                              s_materialGpuTable.size(), byteSize, emittedCount);
                std::fputs(buf, stderr);
            }

            // --- Debug compare ---
            // For each draw slot i: materials[s_packetMaterialIdx[i]].albedoTex
            // must equal layerForPacket[s_sortedPacketOrder[i]].
            // mismatches > 0 means the sidecar is wrong; execution continues for log collection.
            int mismatches = 0;
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const uint32_t idx          = s_packetMaterialIdx[i];
                const uint32_t albedo       = s_materialGpuTable[idx].albedoTex;
                const uint32_t expected     =
                    static_cast<uint32_t>(layerForPacket[globalPktIdx]);
                if (albedo != expected) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[MATERIAL_GPU v1] MISMATCH slot=%u pkt=%u"
                                  " materialIdx=%u albedoTex=%u expected=%u\n",
                                  i, globalPktIdx, idx, albedo, expected);
                    std::fputs(buf, stderr);
                    ++mismatches;
                }
            }
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                std::fputs(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)
```

- [ ] **Step 2: Build to verify compile**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Expected: build succeeds with no errors.

Common errors:
- `error C2440: 'initializing': cannot convert from...` — make sure `auto [it, inserted]` is C++17 (check `cxx_std_17` is set in CMake). If the project is not C++17, report this as a blocker.
- `'try_emplace': is not a member of 'std::unordered_map'` — same root cause.

- [ ] **Step 3: Deploy and run gate-ON smoke on one mission to verify upload log**

Deploy:
```powershell
Copy-Item -Force "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe" "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
```

Run a single-mission smoke with the gate active:
```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

In the smoke log for mc2_01, grep for upload lines:
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "MATERIAL_GPU"
```

Expected lines (order may vary):
```
[MATERIAL_GPU v1] event=table_upload materials=N bytes=B emitted=M
[MATERIAL_GPU v1] event=compare emitted=M mismatches=0
```

Where `N > 0` (mc2_01 has static props). `mismatches=0` is required — if non-zero, something is wrong with the dedup loop; do not proceed, investigate.

- [ ] **Step 4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-2): add finalizeGeometry sidecar (table build + upload + compare)"
```

---

## Task 4: `flush()` bind at slot 5

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

### Where to insert

Locate this line in `GpuStaticPropBatcher::flush()` (around line 3365 after previous tasks' edits):
```cpp
        // 11.7.f — bind slot 4 (PerDraw SSBO).
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_perDrawSsbo);
```

Insert the sidecar bind **immediately after** that line:

- [ ] **Step 1: Insert the slot-5 bind block after the slot-4 bind**

```cpp
        // MaterialGpu-2: bind slot 5 (MaterialGpu SSBO) when gate active and buffer exists.
        // No production shader declares binding=5 in v2; the bind is a driver-error-free no-op
        // from the shader perspective. v3 will add the shader consumer.
        if (s_materialGpuEnabled && s_materialGpuSsbo != 0) {
            while (glGetError() != GL_NO_ERROR) {}  // drain stale BEFORE
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, s_materialGpuSsbo);
            const GLenum bindErr = glGetError();    // sample AFTER
            if (bindErr != GL_NO_ERROR) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] GL ERROR after bind: 0x%x\n", bindErr);
                std::fputs(buf, stderr);
            }
        }
```

- [ ] **Step 2: Build**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Expected: build succeeds.

- [ ] **Step 3: Deploy and verify no GL ERROR at bind**

```powershell
Copy-Item -Force "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe" "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

Grep logs for GL errors:
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "GL ERROR"
```

Expected: no output (zero matches). Any `GL ERROR` line is a failure — investigate before committing.

- [ ] **Step 4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-2): bind MaterialGpu SSBO at slot 5 in flush()"
```

---

## Task 5: `onMapUnload()` teardown

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

### Where to insert

Locate the teardown block at the end of `GpuStaticPropBatcher::onMapUnload()` (around lines 1100-1107 after earlier edits):
```cpp
    if (s_perDrawSsbo)      { glDeleteBuffers(1,  &s_perDrawSsbo);      s_perDrawSsbo      = 0; }
    if (s_permutationSsbo)  { glDeleteBuffers(1,  &s_permutationSsbo);  s_permutationSsbo  = 0; }
    if (s_cmdToBucketSsbo)  { glDeleteBuffers(1,  &s_cmdToBucketSsbo);  s_cmdToBucketSsbo  = 0; }
    s_sortedPacketOrder.clear();
    s_sortedPacketOrder.shrink_to_fit();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
}
```

Insert the MaterialGpu teardown **after `s_cmdToBucketSsbo = 0;`** and **before `s_sortedPacketOrder.clear()`**:

- [ ] **Step 1: Insert the teardown block**

```cpp
    // MaterialGpu-2 teardown: runs regardless of gate state (defensive).
    // When gate is OFF, s_materialGpuSsbo == 0 and the if-block is skipped entirely.
    if (s_materialGpuSsbo != 0) {
        const size_t byteSize =
            s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[MATERIAL_GPU v1] event=unload materials=%zu bytes=%zu\n",
                      s_materialGpuTable.size(), byteSize);
        std::fputs(buf, stderr);
        glDeleteBuffers(1, &s_materialGpuSsbo);
        s_materialGpuSsbo = 0;
    }
    s_packetMaterialIdx.clear();
    s_materialGpuTable.clear();
```

After insertion the bottom of `onMapUnload()` looks like:
```cpp
    if (s_perDrawSsbo)      { glDeleteBuffers(1,  &s_perDrawSsbo);      s_perDrawSsbo      = 0; }
    if (s_permutationSsbo)  { glDeleteBuffers(1,  &s_permutationSsbo);  s_permutationSsbo  = 0; }
    if (s_cmdToBucketSsbo)  { glDeleteBuffers(1,  &s_cmdToBucketSsbo);  s_cmdToBucketSsbo  = 0; }
    // MaterialGpu-2 teardown
    if (s_materialGpuSsbo != 0) {
        const size_t byteSize =
            s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[MATERIAL_GPU v1] event=unload materials=%zu bytes=%zu\n",
                      s_materialGpuTable.size(), byteSize);
        std::fputs(buf, stderr);
        glDeleteBuffers(1, &s_materialGpuSsbo);
        s_materialGpuSsbo = 0;
    }
    s_packetMaterialIdx.clear();
    s_materialGpuTable.clear();
    s_sortedPacketOrder.clear();
    s_sortedPacketOrder.shrink_to_fit();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
}
```

- [ ] **Step 2: Build**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Expected: build succeeds.

- [ ] **Step 3: Deploy and verify unload log**

Deploy, then run a two-mission smoke so at least one mission-reload occurs:
```powershell
Copy-Item -Force "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe" "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01,mc2_03 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

Grep for unload:
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "event=unload"
```

Expected: at least one `[MATERIAL_GPU v1] event=unload materials=N bytes=B` line.

- [ ] **Step 4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-2): add onMapUnload teardown for MaterialGpu SSBO"
```

---

## Task 6: `gameosmain.cpp` startup log

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp`

### Where to insert

Locate the `[INSTR v1]` banner in `gameosmain.cpp` (around line 826-854). The banner ends with:
```cpp
        puts(_cbbuf);
        crashbundle_append(_cbbuf);

        // [UNIFIED_PROJ v1] Warn when MC2_DISABLE_GOSFX=0 ...
        {
```

Insert the MaterialGpu startup log **after `crashbundle_append(_cbbuf);`** and **before the `[UNIFIED_PROJ v1]` block**:

- [ ] **Step 1: Insert the startup log block**

```cpp
        // [MATERIAL_GPU v1] startup banner — separate from [INSTR v1] to keep
        // that buffer size stable. Duplicates the getenv check because
        // s_materialGpuEnabled is a private file-scope static in the batcher
        // (not accessible cross-TU — Option A from MaterialGpu-2 spec §3).
        {
            const bool matGpuOn = (getenv("MC2_MATERIAL_GPU") != nullptr);
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v1] enabled=%d binding=5\n",
                          (int)matGpuOn);
            std::fputs(buf, stderr);
        }
```

After insertion the area looks like:
```cpp
        puts(_cbbuf);
        crashbundle_append(_cbbuf);

        // [MATERIAL_GPU v1] startup banner
        {
            const bool matGpuOn = (getenv("MC2_MATERIAL_GPU") != nullptr);
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v1] enabled=%d binding=5\n",
                          (int)matGpuOn);
            std::fputs(buf, stderr);
        }

        // [UNIFIED_PROJ v1] Warn when MC2_DISABLE_GOSFX=0 dev-override is active
```

- [ ] **Step 2: Build**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64" --config RelWithDebInfo
```

Expected: build succeeds.

- [ ] **Step 3: Deploy and verify gate-ON banner**

```powershell
Copy-Item -Force "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\RelWithDebInfo\mc2.exe" "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

Grep startup banner:
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "MATERIAL_GPU v1.*enabled="
```

Expected gate-ON:
```
[MATERIAL_GPU v1] enabled=1 binding=5
```

- [ ] **Step 4: Verify gate-OFF banner (no table_upload lines)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs
```

Check logs (gate OFF — no `MC2_MATERIAL_GPU` set):
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "MATERIAL_GPU"
```

Expected gate-OFF:
```
[MATERIAL_GPU v1] enabled=0 binding=5
```

And NO `table_upload` or `event=compare` lines. If those appear with gate OFF, the `if (s_materialGpuEnabled)` guard is broken.

- [ ] **Step 5: Commit**

```powershell
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(material-gpu-2): add [MATERIAL_GPU v1] startup banner to gameosmain.cpp"
```

---

## Task 7: Document `MC2_MATERIAL_GPU` and SSBO binding registry

**Files:**
- Modify: `docs/tier1_env_vars.md`

- [ ] **Step 1: Add MaterialGpu section**

Open `docs/tier1_env_vars.md`. After the last section (`## Firewall / no-raw-GL / VFX-no-objectId`), append:

```markdown

## MaterialGpu sidecar (MaterialGpu-2)

- `MC2_MATERIAL_GPU=1` — enable static-prop MaterialGpu sidecar: builds table from `texArrayLayer`, uploads mission-lifetime SSBO at binding 5, binds in `flush()`, compares `albedoTex` vs legacy layer. Default OFF. Log prefix: `[MATERIAL_GPU v1]`. Emits: `event=table_upload materials=N bytes=B emitted=M`, `event=compare emitted=M mismatches=0`, `event=unload materials=N bytes=B`. No visual change. No shader consumer until MaterialGpu-3.

## SSBO binding registry (static_prop pass)

| Binding | Owner | Status |
|---|---|---|
| 0 | Instances | active |
| 1 | Colors (legacy) | active |
| 2 | PerType | active |
| 3 | Parity (debug) | active (`MC2_OBJECT_PARITY_CHECK=1`) |
| 4 | PerDraw | active (coalesce path) |
| 5 | MaterialGpu | PROVISIONAL — v2 binds, no shader consumer; v3 makes load-bearing |
```

- [ ] **Step 2: Commit**

```powershell
git add docs/tier1_env_vars.md
git commit -m "docs(material-gpu-2): document MC2_MATERIAL_GPU env var and SSBO binding 5 reservation"
```

---

## Task 8: Full verification — all 8 required v2 gates

This task has no code changes. It runs all 8 gates from spec §11 and confirms every one passes.

**Files:** none changed

- [ ] **Gate 1: Tier1 5/5 with gate OFF**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit code 0, `PASS 5/5`. No `table_upload` lines in any log. No `GL ERROR` lines attributable to MaterialGpu.

- [ ] **Gate 2+3: `materials > 0` and upload log present with gate ON**

```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_MATERIAL_GPU
```

Grep all logs:
```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "MATERIAL_GPU"
```

Expected: at least one mission reports `event=table_upload materials=N bytes=B emitted=M` with `N > 0` (missions with static props). Missions with no static props may report `materials=0 emitted=0` — that is correct behavior, not a failure.

- [ ] **Gate 4: Reflection gate (already satisfied — informational only)**

MaterialGpu-1 shipped with `array_stride=32` and all 8 field offsets verified. Confirm the golden still passes:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tools\shader_reflect\reflect.py
```

Expected: exit 0, no CONTRACT_VIOLATION.

- [ ] **Gate 5: Compare passes (`mismatches=0`)**

From the gate-ON logs (captured in Gate 2+3 above):
```powershell
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "event=compare"
```

Expected: all lines show `mismatches=0`. Any non-zero mismatch count is a failure.

- [ ] **Gate 6: No GL errors**

From the gate-ON logs:
```powershell
Get-ChildItem $logDir -Filter "*.log" -Recurse | Select-String "GL ERROR"
```

Expected: no output. Any `[MATERIAL_GPU v1] GL ERROR` line is a failure.

- [ ] **Gate 7: No binding-5 conflict (already verified in Task 1 — re-confirm)**

```powershell
Get-ChildItem shaders -Recurse -Include *.frag,*.vert,*.comp,*.tesc,*.tese |
  Select-String -Pattern "binding\s*=\s*5"
```

Expected: only `shaders\fixtures\material_gpu_contract.frag`.

- [ ] **Gate 8: No pixel delta**

Since binding 5 has no shader consumer in v2, there must be zero visual change between gate OFF and gate ON.

If the smoke framework captures per-mission screenshots automatically: diff them and require zero pixel delta.

If the smoke harness does not capture screenshots for this path: `PASS 5/5` on the gate-ON tier1 run (already obtained in Gate 2+3) plus no `GL ERROR` lines and no rendering anomaly observed during the run is sufficient evidence. Document the pass criterion used in the execution summary.

- [ ] **Final: Record gate results**

Do not create a git tag. Record all 8 gate results (PASS/FAIL with evidence) in the execution summary returned to the maintainer. The maintainer decides whether to tag or merge.

---

## Quick log reference

```
Gate OFF startup:
  [MATERIAL_GPU v1] enabled=0 binding=5

Gate ON startup:
  [MATERIAL_GPU v1] enabled=1 binding=5

Per mission (gate ON, static props present):
  [MATERIAL_GPU v1] event=table_upload materials=N bytes=B emitted=M
  [MATERIAL_GPU v1] event=compare emitted=M mismatches=0
  [MATERIAL_GPU v1] event=unload materials=N bytes=B    ← on mission end

On failure:
  [MATERIAL_GPU v1] GL ERROR after upload: 0x<hex>
  [MATERIAL_GPU v1] GL ERROR after bind: 0x<hex>
  [MATERIAL_GPU v1] MISMATCH slot=I pkt=P materialIdx=U albedoTex=A expected=E
```

Gate-ON pure-terrain mission (no static props):
```
[MATERIAL_GPU v1] event=table_upload materials=0 bytes=0 emitted=0
[MATERIAL_GPU v1] event=compare emitted=0 mismatches=0
```
(No upload, no bind, no unload — `s_materialGpuSsbo` stays 0.)
