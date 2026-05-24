# MaterialGpu-3 Shader Sampling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `static_prop.frag` to read albedo texture layer from the MaterialGpu material table instead of the legacy `PerDrawEntry.texArrayLayer`, controlled by two env gates, with zero visual delta.

**Architecture:** `_pad1` at offset 28 of `PerDrawEntry` is repurposed as `materialIdx` (same 4-byte slot, no stride change). A new env gate `MC2_MATERIAL_GPU_SAMPLE=1` (alongside the existing `MC2_MATERIAL_GPU=1`) gates a `u_materialGpuSample` uniform that switches the fragment shader between legacy `texArrayLayer` and `materials[materialIdx].albedoTex`. The sidecar built in MaterialGpu-2 already maps each draw slot to the correct table index; v3 writes it into PerDrawEntry and exposes it to the shader.

**Tech Stack:** C++17, OpenGL 4.3 (GLSL), cmake, RelWithDebInfo build config. No new libraries. Existing `RenderCore/MaterialGpu.h` + `shaders/include/material_gpu.hglsl` unchanged.

---

## File Map

| File | Change |
|---|---|
| `GameOS/gameos/gos_static_prop_batcher.h` | EDIT — rename `_pad1` → `uint32_t materialIdx`, replace `_pad1` static_assert |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | EDIT — 4 sites: state vars; finalizeGeometry reset+fill+valid+compare; ProgramLocs+resolve; flush sampleOn+uniform+log |
| `shaders/static_prop.frag` | EDIT — 4 sites: GLSL struct `uint materialIdx`; MaterialTable SSBO+include; SSBO read; effectiveLayer switch |
| `GameOS/gameos/gameosmain.cpp` | EDIT — extend `[MATERIAL_GPU v1]` banner with `sample=%d` |
| `docs/tier1_env_vars.md` | EDIT — `MC2_MATERIAL_GPU_SAMPLE` bullet; binding-5 registry update |
| `tools/shader_reflect/*.json` | EDIT — targeted update: `static_prop.frag` golden only |

---

## Reference constants (use exactly as shown)

```
Source root:  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/
Build dir:    A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/
Deploy dir:   A:/Games/mc2-opengl/mc2-win64-v0.4/
CMake:        C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe
Build cfg:    RelWithDebInfo   ← NEVER Release (crashes with GL_INVALID_ENUM)
```

**Canonical build command:**
```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo
```

**Canonical deploy command (after build):**
```powershell
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
(diff exit 0 = identical, which is a failure; expect nonzero = files differ, meaning deploy updated the binary.)

**Canonical smoke command (copy-paste verbatim, never modify):**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## Task 1: Rename `_pad1` → `materialIdx` in C++ header

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`

### Context

`PerDrawEntry` is defined at `gos_static_prop_batcher.h` lines 46–64. The field `int32_t _pad1` at offset 28 becomes `uint32_t materialIdx`. The struct is 32 bytes; no stride changes.

Current state:
```cpp
int32_t _pad1;             // 28 — std430 alignment + size = 32
```
```cpp
static_assert(offsetof(PerDrawEntry, _pad1) == 28, "_pad1 offset");
```

- [ ] **Step 1.1: Rename the field and update the comment**

In `GameOS/gameos/gos_static_prop_batcher.h`, find the `PerDrawEntry` struct. Replace the `_pad1` line.

The exact text to find (copy from file, one space between `int32_t` and `objectIdRaw`):
```cpp
    int32_t objectIdRaw;       // 24 — M1.5: handle.raw() when MC2_OBJECT_ID_BUFFER=1, else 0
    int32_t _pad1;             // 28 — std430 alignment + size = 32
```

Replace with (double space after `int32_t` to maintain comment alignment with `uint32_t`):
```cpp
    int32_t  objectIdRaw;      // 24 — M1.5: handle.raw() when MC2_OBJECT_ID_BUFFER=1, else 0
    uint32_t materialIdx;      // 28 — MaterialGpu-3: index into s_materialGpuTable
                               //      (was _pad1; filled from s_packetMaterialIdx[slot]
                               //       when MC2_MATERIAL_GPU=1 and sidecar valid, else 0u)
```

- [ ] **Step 1.2: Replace the `_pad1` static_assert with `materialIdx` asserts**

Find the line:
```cpp
static_assert(offsetof(PerDrawEntry, _pad1)            == 28, "_pad1 offset");
```

Replace with:
```cpp
static_assert(offsetof(PerDrawEntry, materialIdx)      == 28, "materialIdx offset");
```

Leave all other `static_assert` lines (offsets 0–24 and the `sizeof` assert) unchanged.

- [ ] **Step 1.3: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines. (There may be warnings about `uint32_t` ↔ `int32_t` if any callsite reads `_pad1`; check. There should be none since `_pad1` was never read.)

- [ ] **Step 1.4: Commit**

```powershell
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add GameOS/gameos/gos_static_prop_batcher.h
git commit -m "feat(material-gpu-3): rename PerDrawEntry._pad1 to materialIdx (uint32_t)"
```

---

## Task 2: Add batcher state variables

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (state variable section ~line 268)

### Context

The existing gate constant is at lines 268–270:
```cpp
static const bool s_materialGpuEnabled =
    (getenv("MC2_MATERIAL_GPU") != nullptr);
```

We add two new variables immediately after it.

- [ ] **Step 2.1: Add `s_materialGpuSampleEnabled` and `s_materialGpuSidecarValid`**

In `GameOS/gameos/gos_static_prop_batcher.cpp`, find the block:

```cpp
static const bool s_materialGpuEnabled =
    (getenv("MC2_MATERIAL_GPU") != nullptr);
static std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
```

Insert between those two lines:

```cpp
static const bool s_materialGpuEnabled =
    (getenv("MC2_MATERIAL_GPU") != nullptr);
static const bool s_materialGpuSampleEnabled =
    (getenv("MC2_MATERIAL_GPU_SAMPLE") != nullptr);
// Tracks whether finalizeGeometry() produced a correctly-sized sidecar.
// Reset to false at the start of every finalizeGeometry() call; set to true
// only after the sidecar loop completes with size == emitted count.
// Initialized false: no table built yet.
static bool s_materialGpuSidecarValid = false;
static std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
```

- [ ] **Step 2.2: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines.

- [ ] **Step 2.3: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-3): add s_materialGpuSampleEnabled and s_materialGpuSidecarValid"
```

---

## Task 3: finalizeGeometry() — sidecar valid flag + materialIdx fill + entry compare log

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (finalizeGeometry, ~lines 2053–2210)

### Context

Three separate edit sites inside `finalizeGeometry()`:

**Site A** — TOP of the `if (s_materialGpuEnabled)` block (~line 2053).
Current:
```cpp
        if (s_materialGpuEnabled) {
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();
```

**Site B** — BOTTOM of the `if (s_materialGpuEnabled)` block, after the `event=compare` log (~line 2155), before the closing `} // end s_materialGpuEnabled` brace.
Current:
```cpp
            }  // end compare for loop
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                std::fputs(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)
```

**Site C** — Inside the entries-build `for` loop (~lines 2160–2200), inside the `if (layerForPacket[globalPktIdx] >= 0)` block, after the `e.objectIdRaw = ...` assignment.

The entries loop is OUTSIDE the `if (s_materialGpuEnabled)` block. It runs unconditionally.

**Site D** — After the entries-build `for` loop (~line 2200), before `glGenBuffers(1, &s_perDrawSsbo)` (~line 2201).

- [ ] **Step 3.1: Site A — reset sidecar valid flag at top of if(s_materialGpuEnabled) block**

Find:
```cpp
        if (s_materialGpuEnabled) {
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();
```

Replace with:
```cpp
        if (s_materialGpuEnabled) {
            s_materialGpuSidecarValid = false;  // MAJ-1: reset before sidecar loop
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();
```

- [ ] **Step 3.2: Site B — set sidecar valid flag + error log at bottom of if(s_materialGpuEnabled) block**

Find the end of the `if (s_materialGpuEnabled)` block. It contains the `event=compare` log immediately before the closing brace. Find:

```cpp
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                std::fputs(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)
```

Replace with:

```cpp
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                std::fputs(buf, stderr);
            }

            // C1 fix: record whether the sidecar is aligned with the emitted draw count.
            // This flag gates sampling in flush(). If sizes diverge, sampling is
            // disabled for the whole pass — legacy texArrayLayer is the fallback.
            s_materialGpuSidecarValid =
                (s_packetMaterialIdx.size() == s_sortedPacketOrder.size());

            if (!s_materialGpuSidecarValid) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v1] ERROR materialIdx sidecar size mismatch"
                              " emitted=%zu sidecar=%zu sample_forced=0\n",
                              s_sortedPacketOrder.size(), s_packetMaterialIdx.size());
                std::fputs(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)
```

- [ ] **Step 3.3: Site C — fill materialIdx in entries-build loop**

**Note:** The `if (layerForPacket[globalPktIdx] >= 0)` guard here is always true for every entry in `s_sortedPacketOrder`. The skip-filter at line 2036 ensures only packets with `layerForPacket >= 0` are added to `s_sortedPacketOrder`. So every entry in the entries loop has a valid layer. The guard is legacy protective code, not a real branch. Place the `materialIdx` fill INSIDE this guard block (before `entries[i] = e;`).

Find the entries-build loop. Inside it, after `e.objectIdRaw = ...` (the M1.5 line):

```cpp
                e.objectIdRaw =
                    static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(m1_5_recipeIndex));
            }
            entries[i] = e;
```

Replace with:

```cpp
                e.objectIdRaw =
                    static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(m1_5_recipeIndex));

                // MaterialGpu-3: fill materialIdx from v2 sidecar.
                // Guard: sidecar valid means s_packetMaterialIdx.size() == entries.size(),
                // so index i is in bounds.
                if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
                    e.materialIdx = s_packetMaterialIdx[i];
                } else {
                    e.materialIdx = 0u;
                }
            }
            entries[i] = e;
```

- [ ] **Step 3.4: Site D — entry materialIdx compare log after entries loop**

Find the line immediately after the entries-build for loop closes and before `glGenBuffers(1, &s_perDrawSsbo)`:

```cpp
        }
        glGenBuffers(1, &s_perDrawSsbo);
```

(The `}` above closes the entries-build for loop. It may have adjacent code — look for the `glGenBuffers` line.)

Insert between the `}` (for loop end) and `glGenBuffers`:

```cpp
        }

        // m1: verify PerDrawEntry.materialIdx matches s_packetMaterialIdx[slot].
        // Catches assignment mistakes before shader sampling begins.
        if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
            int entryMismatches = 0;
            const uint32_t emittedCount2 = static_cast<uint32_t>(s_sortedPacketOrder.size());
            for (uint32_t i = 0; i < emittedCount2; ++i) {
                if (entries[i].materialIdx != s_packetMaterialIdx[i]) {
                    ++entryMismatches;
                }
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v1] event=entry_material_idx emitted=%u mismatches=%d\n",
                          emittedCount2, entryMismatches);
            std::fputs(buf, stderr);
        }

        glGenBuffers(1, &s_perDrawSsbo);
```

Note: `emittedCount` was declared inside the `if (s_materialGpuEnabled)` block (a different scope that ended above). There is no shadowing — `emittedCount2` is just a clear local name for the compare log loop counter. You may also name it `emittedCount` since the prior one is out of scope.

- [ ] **Step 3.5: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines.

- [ ] **Step 3.6: Quick smoke to verify gate-OFF behavior still works**

Deploy:
```powershell
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

Run smoke (no env vars — gates OFF):
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0, 5/5 PASS. Check one mission log for `[MATERIAL_GPU v1] event=compare`: should NOT appear (gate is OFF).

- [ ] **Step 3.7: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-3): fill materialIdx in PerDrawEntry + sidecar valid flag"
```

---

## Task 4: ProgramLocs + loadProgramsIfNeeded() uniform resolution

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (ProgramLocs struct ~line 381; loadProgramsIfNeeded ~line 610)

### Context

`ProgramLocs` struct is at ~lines 381–394. The coalesce-only section currently ends with `GLint texArr = -1;`.

`loadProgramsIfNeeded()` resolves coalesce uniforms at ~lines 605–626. The last resolve currently is `s_locsCoalesce.texArr`.

- [ ] **Step 4.1: Add `materialGpuSample` to ProgramLocs**

Find the ProgramLocs struct:
```cpp
    // Coalesce-only.
    GLint drawIDBase       = -1;
    GLint texArr           = -1;
};
```

Replace with:
```cpp
    // Coalesce-only.
    GLint drawIDBase          = -1;
    GLint texArr              = -1;
    GLint materialGpuSample   = -1;   // MaterialGpu-3: u_materialGpuSample
};
```

- [ ] **Step 4.2: Resolve uniform in loadProgramsIfNeeded() + M3 error log**

Find the coalesce uniform resolution block. Look for the line:
```cpp
            s_locsCoalesce.texArr            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
```

Add immediately after it (still inside the same `if (coalesceObj && coalesceObj->is_valid())` block, before the `if (s_locsCoalesce.texArr >= 0)` sampler bind):

```cpp
            s_locsCoalesce.texArr            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
            s_locsCoalesce.materialGpuSample = glGetUniformLocation(s_staticPropProgramCoalesce, "u_materialGpuSample");

            // M3 fix: if both gates are ON and the uniform is absent, log an error.
            // This can only happen if the shader wasn't recompiled with v3 changes.
            // The if(loc >= 0) guard in flush() already handles -1 safely (no-op),
            // but the error makes the failure observable without a debugger.
            if (s_materialGpuEnabled && s_materialGpuSampleEnabled &&
                s_locsCoalesce.materialGpuSample < 0) {
                std::fputs("[MATERIAL_GPU v1] ERROR uniform_missing name=u_materialGpuSample\n",
                           stderr);
            }
```

- [ ] **Step 4.3: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines. Note: `u_materialGpuSample` doesn't exist in the shader yet — `glGetUniformLocation` will return `-1` at runtime. That is expected and safe.

- [ ] **Step 4.4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-3): add ProgramLocs.materialGpuSample, resolve in loadProgramsIfNeeded"
```

---

## Task 5: flush() — sampleOn, uniform upload, event=sample_mode log

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (flush coalesce path, ~lines 3490–3530)

### Context

Inside `flush()`, in the coalesce branch, after the SSBO bind block for slot 5:
```cpp
        if (s_materialGpuEnabled && s_materialGpuSsbo != 0) {
            // ... glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, s_materialGpuSsbo) ...
        }
```
...and before the per-group draw calls (the `if (s_alphaOffCmdCount > 0u)` block), add:
1. Compute `sampleOn` (5 conditions)
2. Upload `u_materialGpuSample` uniform
3. Emit `event=sample_mode` log

- [ ] **Step 5.1: Add sampleOn + uniform upload + event=sample_mode log**

Find the closing brace of the slot-5 SSBO bind block. The landmark comment on the NEXT line is (copy verbatim including `, so`):
```cpp
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

        // 2026-05-11 per-packet rework: each indirect cmd is per-PACKET, so
```

Insert between `}` (SSBO bind close) and the `// 2026-05-11` comment:

```cpp
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

        // MaterialGpu-3: compute sampleOn once per flush.
        // Five conditions — all must be true for shader sampling to occur.
        // Including loc >= 0 as condition 5 prevents misleading "enabled=1 loc=-1" logs.
        const bool sampleOn = s_materialGpuEnabled
                           && s_materialGpuSampleEnabled
                           && s_materialGpuSsbo != 0
                           && s_materialGpuSidecarValid
                           && s_locsCoalesce.materialGpuSample >= 0;

        if (s_locsCoalesce.materialGpuSample >= 0) {
            glUniform1i(s_locsCoalesce.materialGpuSample, sampleOn ? 1 : 0);
        }
        // If loc == -1: M3 error already logged at loadProgramsIfNeeded(); no-op here.

        // M2: required diagnostic log — once per flush, so gate interaction is observable.
        // reason cascade mirrors the sampleOn condition order exactly.
        {
            const char* reason = "ok";
            if (!s_materialGpuEnabled)                    reason = "upload_env_off";
            else if (!s_materialGpuSampleEnabled)          reason = "sample_env_off";
            else if (s_materialGpuSsbo == 0)               reason = "no_ssbo";
            else if (!s_materialGpuSidecarValid)           reason = "sidecar_invalid";
            else if (s_locsCoalesce.materialGpuSample < 0) reason = "uniform_missing";
            // else: reason == "ok" → sampleOn is true (all 5 conditions met)

            char buf[96];
            if (sampleOn) {
                // sampleOn=true guarantees loc >= 0, so loc is always valid here.
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

        // 2026-05-11 per-packet rework: each indirect cmd is per-PACKET, so
```

- [ ] **Step 5.2: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines.

- [ ] **Step 5.3: Verify event=sample_mode log with upload-only gate**

Deploy:
```powershell
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

Run smoke with upload gate only:
```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 15 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU
```

After smoke completes, find the latest artifact directory and check the log:
```powershell
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($latest.FullName)\mc2_01\stderr.log" | Select-String "event=sample_mode" | Select-Object -First 5
```

Expected output: lines like `[MATERIAL_GPU v1] event=sample_mode enabled=0 reason=sample_env_off`
(Upload gate ON, sample gate OFF → reason=sample_env_off.)

- [ ] **Step 5.4: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(material-gpu-3): flush() sampleOn + u_materialGpuSample upload + event=sample_mode log"
```

---

## Task 6: gameosmain.cpp startup banner extension

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (~lines 861–867)

### Context

Current banner block:
```cpp
        {
            const bool matGpuOn = (getenv("MC2_MATERIAL_GPU") != nullptr);
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v1] enabled=%d binding=5\n",
                          (int)matGpuOn);
            std::fputs(buf, stderr);
        }
```

- [ ] **Step 6.1: Extend banner with `sample=%d`**

Replace the banner block above with:

```cpp
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

`char buf[64]`: max output `[MATERIAL_GPU v1] enabled=1 sample=1 binding=5\n` = 47 chars + NUL. Fits in 64.

- [ ] **Step 6.2: Build — verify no errors**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines.

- [ ] **Step 6.3: Commit**

```powershell
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(material-gpu-3): extend MATERIAL_GPU startup banner with sample=%d"
```

---

## Task 7: GLSL shader changes in static_prop.frag

**Files:**
- Modify: `shaders/static_prop.frag`

### Context

All changes are within the `#ifdef MC2_COALESCE` block. The `#else` (legacy uniform) path is untouched. Four edit sites:

- **Site A**: PerDrawEntry GLSL struct — `int _pad1` → `uint materialIdx`
- **Site B**: After `uniform sampler2DArray u_texArr;` — add MaterialTable SSBO + include + uniform
- **Site C**: Per-draw SSBO read block (~line 90–103) — add `materialIdx` read
- **Site D**: Texture sample (~line 105) — replace with `effectiveLayer` switch + sample

Current state of the coalesce block (lines 32–50):
```glsl
#ifdef MC2_COALESCE
uniform int u_drawIDBase;
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;
    int   _pad1;
};
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
uniform sampler2DArray u_texArr;
#else
```

Current per-draw read block (~lines 89–105):
```glsl
#ifdef MC2_COALESCE
    int materialFlags    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int packetID         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].packetID;
    int maxLocalVertexID = perDraw_.entries[v_drawID + uint(u_drawIDBase)].maxLocalVertexID;
    int texArrayLayer    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    ...
    float uvScaleX = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;

    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(texArrayLayer)));
```

- [ ] **Step 7.1: Site A — rename `_pad1` → `materialIdx` in GLSL struct**

In `shaders/static_prop.frag`, find:
```glsl
    int   objectIdRaw;   // M1.5: handle.raw() (read into uint at use site)
    int   _pad1;
```

Replace with:
```glsl
    int   objectIdRaw;   // M1.5: handle.raw() (read into uint at use site)
    uint  materialIdx;   // MaterialGpu-3: index into MaterialTable.materials[]
                         // (was _pad1; uint matches uint32_t at std430 offset 28)
```

`int` and `uint` are both 4-byte at std430. No stride change.

- [ ] **Step 7.2: Site B — add MaterialTable SSBO + include + uniform**

Find:
```glsl
uniform sampler2DArray u_texArr;
#else
```

Replace with:
```glsl
uniform sampler2DArray u_texArr;

// MaterialGpu-3: material table at binding 5.
// RENDER CONTRACT: static_prop.frag coalesce declares MaterialTable at binding 5
//   after MaterialGpu-3. Binding 5 may be unbound when u_materialGpuSample=0;
//   shader MUST NOT access materialTable_.materials[] in that case (enforced by the uniform branch below).
// Always declared in the coalesce variant so the reflection surface is stable.
// Instance name materialTable_ follows the usage pattern documented in material_gpu.hglsl.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table

#else
```

**Critical:** The block uses instance name `materialTable_` (matching `material_gpu.hglsl` line 16: `} materialTable_;`). All access MUST use `materialTable_.materials[i]` — NOT bare `materials[i]`. Do not omit the instance name.

- [ ] **Step 7.3: Site C — add materialIdx read to per-draw SSBO block**

Find the per-draw read block. Look for the `uvScaleY` read line:
```glsl
    float uvScaleY = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
```

Add immediately after it:
```glsl
    float uvScaleY = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;
```

- [ ] **Step 7.4: Site D — replace texture sample with effectiveLayer switch**

Find:
```glsl
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(texArrayLayer)));
```

Replace with:
```glsl
    // MaterialGpu-3: runtime switch between legacy layer and material table.
    // u_materialGpuSample is a pass-wide (not per-fragment) uniform —
    // the branch collapses to a single predicate on AMD hardware.
    // materialTable_.materials[] is only accessed when u_materialGpuSample != 0,
    // enforcing the render contract above (binding 5 must be set when sampling).
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materialTable_.materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            // kMatTexAbsent = 0xFFFFFFFFu (defined in material_gpu.hglsl).
            // v2 mismatches=0 strongly predicts this guard is never triggered
            // for well-formed static-prop packets. Retained defensively.
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
```

- [ ] **Step 7.5: Build — verify shader compiles (no glsl compilation errors)**

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | Select-String -Pattern "error C|error:" | Select-Object -First 20
```

Expected: 0 error lines. GLSL errors surface at runtime (shader compilation on first frame), not cmake build time.

- [ ] **Step 7.6: Deploy and run smoke — gates OFF (regression check)**

**Shaders are loaded from disk at runtime — BOTH the binary and the shader file must be deployed.**

```powershell
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/static_prop.frag" "A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/static_prop.frag"
```

Verify deploy succeeded:
```powershell
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/static_prop.frag" "A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/static_prop.frag"
```
Expected: `diff -q` reports files differ (diff output present) — confirming the new shader was written. If `diff -q` says "identical," the source file and deploy are byte-for-byte the same, which means the deploy path already had the same version (OK if no prior shader was there). If deploy fails silently, the running game will use the old shader and G4 will show `reason=uniform_missing` — see WARNING below.

> **WARNING:** If `event=sample_mode enabled=0 reason=uniform_missing` appears after deploying both gates ON, the most likely cause is that the shader file was not deployed (old shader on disk). Check that the deployed `static_prop.frag` contains `u_materialGpuSample`. Do NOT chase this as a C++ bug.

Run smoke with no env vars:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0, 5/5 PASS. No GL errors in logs. The shader is unchanged behaviorally when both gates are OFF (u_materialGpuSample defaults to 0 → effectiveLayer = texArrayLayer).

- [ ] **Step 7.7: Smoke — both gates ON (first sampling test)**

(The shader was already deployed in Step 7.6 — no additional deploy needed here.)

```powershell
$env:MC2_MATERIAL_GPU = "1"; $env:MC2_MATERIAL_GPU_SAMPLE = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01,mc2_24 --duration 30 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU; Remove-Item Env:\MC2_MATERIAL_GPU_SAMPLE
```

Find the latest artifact directory and inspect logs:
```powershell
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($latest.FullName)\mc2_01\stderr.log" | Select-String "event=sample_mode|ERROR|GL ERROR" | Select-Object -First 20
```

Expected:
- `event=sample_mode enabled=1 loc=N` (N >= 0) — sampling active
- `event=entry_material_idx emitted=M mismatches=0` — materialIdx fill correct
- No `ERROR` lines
- No `GL ERROR` lines

If you see `event=sample_mode enabled=0 reason=uniform_missing`: the shader change in 7.2–7.4 wasn't picked up (check that the shader file was actually modified and the deploy path used the new binary). If `loc` is non-negative but you see GL errors, check the binding.

- [ ] **Step 7.8: Commit**

```powershell
git add shaders/static_prop.frag
git commit -m "feat(material-gpu-3): static_prop.frag shader consumer — MaterialTable SSBO + effectiveLayer switch"
```

---

## Task 8: docs/tier1_env_vars.md updates

**Files:**
- Modify: `docs/tier1_env_vars.md`

### Context

Current state in the file:
```markdown
## MaterialGpu sidecar (MaterialGpu-2)

- `MC2_MATERIAL_GPU=1` — ...
```
And the binding registry:
```markdown
| 5 | MaterialGpu | PROVISIONAL — v2 binds + restores, no shader consumer; v3 makes load-bearing |
```

- [ ] **Step 8.1: Add MC2_MATERIAL_GPU_SAMPLE bullet**

Find the `MC2_MATERIAL_GPU=1` bullet line in the `## MaterialGpu sidecar` section. Add a new bullet immediately after it:

```markdown
- `MC2_MATERIAL_GPU_SAMPLE=1` — enable MaterialGpu shader sampling (MaterialGpu-3).
  Requires `MC2_MATERIAL_GPU=1` to have any effect (both required for `u_materialGpuSample=1`).
  Default OFF. When both active: `static_prop.frag` reads `materials[materialIdx].albedoTex`
  instead of `PerDrawEntry.texArrayLayer`. Expected result: zero pixel delta (same layer index).
  Log: `event=sample_mode enabled=1 loc=N` (once per flush). Diagnostic reason codes:
  `upload_env_off | sample_env_off | no_ssbo | sidecar_invalid | uniform_missing`.
```

- [ ] **Step 8.2: Update binding-5 registry entry**

Find the binding-5 row:
```markdown
| 5 | MaterialGpu | PROVISIONAL — v2 binds + restores, no shader consumer; v3 makes load-bearing |
```

Replace with:
```markdown
| 5 | MaterialGpu | ACTIVE (v3+) — always declared in static_prop.frag coalesce variant; SSBO bound when MC2_MATERIAL_GPU=1, unbound otherwise. Shader accesses only when u_materialGpuSample=1. |
```

- [ ] **Step 8.3: Commit**

```powershell
git add docs/tier1_env_vars.md
git commit -m "docs(material-gpu-3): add MC2_MATERIAL_GPU_SAMPLE + update binding-5 registry"
```

---

## Task 9: Reflection golden update (targeted: static_prop.frag only)

**Files:**
- Modify: `tools/shader_reflect/*.json` (static_prop.frag variants only)

### Context

The shader reflection CI compiles each shader variant and compares the reflected uniform/SSBO layout against a golden JSON. After the GLSL changes in Task 7, the `static_prop.frag` golden is stale and needs updating. **Only `static_prop.frag` goldens may change.** If any other golden changes, stop and investigate.

Expected changes to the `static_prop.frag` golden:
1. New SSBO at binding 5, block name `MaterialTable`, `array_stride=32`
2. `MaterialGpu` struct members: `albedoTex` (offset 0), `normalTex` (4), `metallicRoughnessTex` (8), `emissiveTex` (12), `flags` (16), `baseColorFactor` (20), `metallicFactor` (24), `roughnessFactor` (28)
3. New uniform `u_materialGpuSample`, type `int`
4. `PerDrawEntry` member at offset 28: name changes from `_pad1` to `materialIdx`, type changes from `int` to `uint`
5. No `CONTRACT_VIOLATION`

- [ ] **Step 9.1: Run the golden update**

```powershell
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
py -3 tools/shader_reflect/reflect.py --update
```

Expected: exits with code 0 (or a nonzero that indicates "goldens updated, re-run to pass"). Wait for completion.

- [ ] **Step 9.2: Check which goldens changed — ONLY static_prop.frag variants should appear**

```powershell
git diff --name-only tools/shader_reflect/
```

Expected: only files matching `*static_prop*` appear. If ANY other golden file appears, STOP. Do not commit. Investigate what shader change caused the unexpected drift (the change in Task 7 should only affect static_prop.frag since material_gpu.hglsl is not currently included by any other shader).

- [ ] **Step 9.3: Inspect static_prop.frag diff for expected changes**

```powershell
git diff tools/shader_reflect/
```

Verify the diff contains:
- A new entry for `MaterialTable` SSBO at binding 5
- A new entry for `u_materialGpuSample` uniform
- The `materialIdx` field (was `_pad1`) at offset 28

If the diff looks correct, proceed.

- [ ] **Step 9.4: Run reflect.py to confirm it exits 0**

```powershell
py -3 tools/shader_reflect/reflect.py
```

Expected: exit 0, no `CONTRACT_VIOLATION` lines in output.

- [ ] **Step 9.5: Commit the updated goldens**

```powershell
git add tools/shader_reflect/
git commit -m "chore(material-gpu-3): update static_prop.frag shader reflection golden"
```

---

## Task 10: Full verification — all v3 gates

**Files:** None modified in this task. This is a verification-only task.

### Required gates (all must pass)

| Gate | Test condition | Expected result |
|---|---|---|
| G1: All gates OFF | No MC2_* env vars | Tier1 5/5, exit 0; startup: `enabled=0 sample=0` |
| G2: Upload only | `MC2_MATERIAL_GPU=1` | Tier1 5/5, exit 0; `event=sample_mode enabled=0 reason=sample_env_off` each flush |
| G3: Sample only | `MC2_MATERIAL_GPU_SAMPLE=1` (upload unset) | Tier1 5/5, exit 0; `event=sample_mode enabled=0 reason=upload_env_off` |
| G4: Both gates ON | `MC2_MATERIAL_GPU=1 MC2_MATERIAL_GPU_SAMPLE=1` | Tier1 5/5, exit 0; `event=sample_mode enabled=1 loc=N` (N >= 0) |
| G5: No GL errors | Both gates ON | No `GL ERROR` lines in logs |
| G6: No sidecar mismatch | Both gates ON | No `ERROR materialIdx sidecar size mismatch` in logs |
| G7: Sampling active | Both gates ON | `event=sample_mode enabled=1` lines per flush |
| G8: materialIdx correct | Both gates ON | `event=entry_material_idx emitted=M mismatches=0` for all missions |
| G9: Pixel parity | Both gates ON | 5/5 PASS + no GL ERROR (screenshot diff = 0 if harness captures; otherwise PASS is accepted) |
| G10: Reflection golden | After golden update | `reflect.py` exit 0, no CONTRACT_VIOLATION, only static_prop.frag changed |

- [ ] **Step 10.1: Deploy latest build + shader**

**Both the binary and shader must be deployed.** Shaders load from disk at runtime.

```powershell
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/static_prop.frag" "A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/static_prop.frag"
```

Verify the deployed shader has the `u_materialGpuSample` uniform (confirms Task 7 changes are live):
```powershell
Select-String "u_materialGpuSample" "A:/Games/mc2-opengl/mc2-win64-v0.4/shaders/static_prop.frag"
```
Expected: one match line. If no match — the wrong shader was deployed; stop and investigate before proceeding.

- [ ] **Step 10.1b: Prerequisite — verify coalesce path is active for tier1 missions**

G2/G3/G4 depend on `event=sample_mode` lines appearing in the log. These lines are only emitted in the coalesce branch of `flush()`. If coalesce is NOT active (e.g., driver doesn't support `GL_ARB_shader_draw_parameters`), no `event=sample_mode` lines will appear regardless of gate state, and the smoke gates would be silently vacuous. Verify coalesce is active before proceeding.

Run a quick smoke with upload gate to check:
```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 15 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($latest.FullName)\mc2_01\stderr.log" | Select-String "event=sample_mode|coalesce|COALESCE" | Select-Object -First 5
```

Expected: at least one `event=sample_mode` line. If no `event=sample_mode` lines appear at all but smoke exits 0, check for `[COALESCE v1] event=ready` or `[GPUPROPS-DIAG] static_prop_coalesce program=N` (N > 0) to confirm the coalesce program loaded. If coalesce is not active, the `event=sample_mode` checks in G2/G3/G4 below are vacuous — escalate rather than accepting a false PASS.

- [ ] **Step 10.2: G1 — All gates OFF**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0. Check one mission log:
```powershell
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($latest.FullName)\mc2_01\stderr.log" | Select-String "MATERIAL_GPU v1" | Select-Object -First 3
```
Expected: `[MATERIAL_GPU v1] enabled=0 sample=0 binding=5` at startup. No `event=sample_mode` lines (coalesce may not be active for all missions in legacy path — if present, should show `enabled=0 reason=upload_env_off`).

- [ ] **Step 10.3: G2 — Upload only**

```powershell
$env:MC2_MATERIAL_GPU = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU
```

Expected: exit 0. Check logs:
```powershell
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($latest.FullName)\mc2_01\stderr.log" | Select-String "event=sample_mode" | Select-Object -First 3
```
Expected: `event=sample_mode enabled=0 reason=sample_env_off`

- [ ] **Step 10.4: G3 — Sample only (upload gate unset)**

```powershell
$env:MC2_MATERIAL_GPU_SAMPLE = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU_SAMPLE
```

Expected: exit 0. Logs should show `event=sample_mode enabled=0 reason=upload_env_off`.

- [ ] **Step 10.5: G4–G9 — Both gates ON (full 5/5)**

```powershell
$env:MC2_MATERIAL_GPU = "1"; $env:MC2_MATERIAL_GPU_SAMPLE = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:\MC2_MATERIAL_GPU; Remove-Item Env:\MC2_MATERIAL_GPU_SAMPLE
```

Expected: exit 0, 5/5 PASS.

Inspect logs for all missions:
```powershell
$latest = Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
foreach ($m in @("mc2_01","mc2_03","mc2_10","mc2_17","mc2_24")) {
    Write-Host "=== $m ==="; 
    Get-Content "$($latest.FullName)\$m\stderr.log" | Select-String "event=sample_mode|event=entry_material_idx|GL ERROR|ERROR " | Select-Object -First 5
}
```

All 5 missions must show:
- `event=sample_mode enabled=1 loc=N` (N >= 0) — G4/G5/G7
- `event=entry_material_idx emitted=M mismatches=0` — G8
- No `GL ERROR` — G5
- No `ERROR materialIdx sidecar size mismatch` — G6

- [ ] **Step 10.6: G10 — Reflection golden**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tools\shader_reflect\reflect.py
```

Expected: exit 0, no `CONTRACT_VIOLATION` output.

- [ ] **Step 10.7: Record gate results and commit verification note**

```powershell
git commit --allow-empty -m "chore(material-gpu-3): all 10 v3 gates verified PASS"
```

(Use `--allow-empty` since this is a marker commit with no file changes. Only commit this if all gates actually passed.)

---

## Verification summary

All 10 gates must PASS before declaring MaterialGpu-3 complete:

```
G1  All gates OFF             ✓ Tier1 5/5, startup enabled=0 sample=0
G2  Upload only               ✓ Tier1 5/5, reason=sample_env_off
G3  Sample only               ✓ Tier1 5/5, reason=upload_env_off
G4  Both gates ON             ✓ Tier1 5/5
G5  No GL errors              ✓ No GL ERROR in both-gates logs
G6  No sidecar mismatch       ✓ No ERROR materialIdx sidecar in logs
G7  event=sample_mode enabled=1  ✓ loc=N (N >= 0) each flush
G8  materialIdx mismatches=0  ✓ All missions
G9  Pixel parity              ✓ 5/5 PASS + no GL ERROR
G10 Reflection golden         ✓ reflect.py exit 0, no CONTRACT_VIOLATION
```
