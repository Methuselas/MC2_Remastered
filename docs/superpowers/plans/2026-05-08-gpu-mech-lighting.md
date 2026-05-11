# GPU Mech Lighting Implementation Plan (Track D Slice B1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Slice A's flat `baseLight = vec3(1.0)` in `mech.vert` with VS-side per-vertex `calc_light()` from `lighting.hglsl`, gated by a separate `MC2_GPU_MECH_LIGHTING` killswitch, mirroring `static_prop.vert`'s Stage 2.C.2 pattern.

**Architecture:** Per-actor `cachedGpuLightIndex_` is refreshed in `Mech3DAppearance::update()` via `mechShape->CacheGpuLightData()` (mirrors bdactor.cpp:2314). At submit, that index flows through `GpuMechSubmitDesc::lightDataIndex` → `inst.lightDataIndex` SSBO field. In `mech.vert`, when `u_lightingMode != 0`, calc_light reads `LightsData[inst.lightDataIndex]` (UBO slot 0, bound by mcTextureManager) with worldNormal + worldMC2 + ambient floor 0.35, and writes lit RGB into `v_litColor`.

**Tech Stack:** OpenGL 4.3 core, GLSL 430, `lighting.hglsl::calc_light(int, vec3, vec3, vec3)`, `LightsData` UBO at `LIGHT_DATA_ATTACHMENT_SLOT=0`, `TG_MultiShape::CacheGpuLightData` / `getCachedGpuLightIndex`.

**Spec:** [docs/superpowers/specs/2026-05-08-gpu-mech-lighting-design.md](../specs/2026-05-08-gpu-mech-lighting-design.md)

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | Add `extern bool g_useGpuMechLighting` decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | Define `g_useGpuMechLighting`, cache `s_loc_u_lightingMode`, set uniform in flush, MC2_MECH_LIGHT_TRACE diag |
| Modify | `shaders/mech.vert` | `#include lighting.hglsl`, add `u_lightingMode`, conditional calc_light branch with ambient floor |
| Modify | `shaders/mech.frag` | Add debug mode 9 (lighting-only viz) |
| Modify | `mclib/mech3d.cpp` | Call `mechShape->CacheGpuLightData()` in update; read `getCachedGpuLightIndex()` in render |

No new files. All edits surgical against committed Slice A code.

---

## Task 1: Killswitch declaration + definition

**Files:**
- Modify: `GameOS/gameos/gos_mech_killswitch.h`
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 1.1: Add extern decl to killswitch header**

In `GameOS/gameos/gos_mech_killswitch.h`, after the existing `extern bool g_useGpuMechs;` (line 8), add a second extern:

```cpp
// Slice B1: gates VS-side calc_light() in mech.vert. Independent of
// g_useGpuMechs so an operator can keep GPU mech rendering on while
// flipping lighting off if a B1 regression surfaces. No effect when
// g_useGpuMechs is off (the entire batcher path skips).
extern bool g_useGpuMechLighting;
```

- [ ] **Step 1.2: Add definition in gos_mech_batcher.cpp**

In `GameOS/gameos/gos_mech_batcher.cpp`, immediately after `bool g_useGpuMechs = (getenv(...) != nullptr);` (line 25), add:

```cpp
// Slice B1: enables calc_light() in mech.vert. Requires g_useGpuMechs=true
// to take effect (the calc_light branch is inside the GPU mech draw path).
bool g_useGpuMechLighting = (getenv("MC2_GPU_MECH_LIGHTING") != nullptr);
```

- [ ] **Step 1.3: Build clean**

```
/mc2-build
```

Expected: clean build. No new uses yet — this is a pure declaration step.

- [ ] **Step 1.4: Commit**

```bash
git add GameOS/gameos/gos_mech_killswitch.h GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-b1): add g_useGpuMechLighting killswitch (MC2_GPU_MECH_LIGHTING env)"
```

---

## Task 2: Cache the `u_lightingMode` uniform location

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 2.1: Add the static GLint slot**

In `GameOS/gameos/gos_mech_batcher.cpp`, immediately after `static GLint s_loc_u_debugMode       = -1;` (line 43), add:

```cpp
static GLint s_loc_u_lightingMode    = -1;
```

- [ ] **Step 2.2: Resolve the location at link time**

Immediately after `s_loc_u_debugMode       = loc("u_debugMode");` (line 134), add:

```cpp
s_loc_u_lightingMode    = loc("u_lightingMode");
```

- [ ] **Step 2.3: Build clean**

```
/mc2-build
```

Expected: clean build. The uniform doesn't exist in the shader yet, so `loc("u_lightingMode")` returns -1; `glUniform1i(-1, ...)` is a silent no-op. Defensive.

- [ ] **Step 2.4: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-b1): cache s_loc_u_lightingMode uniform location"
```

---

## Task 3: Wire `u_lightingMode` upload at flush time

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 3.1: Add the uniform upload after u_fogValue**

In `GameOS/gameos/gos_mech_batcher.cpp`, immediately after `glUniform1f(s_loc_u_fogValue, 1.0f);` (line 732), add:

```cpp
// Slice B1: lighting mode 0 = Slice A flat-white passthrough,
// 1 = calc_light() per-vertex. Set per-flush from killswitch.
if (s_loc_u_lightingMode >= 0)
    glUniform1i(s_loc_u_lightingMode, g_useGpuMechLighting ? 1 : 0);
```

- [ ] **Step 3.2: Build clean**

```
/mc2-build
```

Expected: clean build. Still no shader-side use; uniform value lands silently.

- [ ] **Step 3.3: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-b1): upload u_lightingMode uniform per flush from g_useGpuMechLighting"
```

---

## Task 4: Add `u_lightingMode` uniform + lighting include to mech.vert

**Files:**
- Modify: `shaders/mech.vert`

- [ ] **Step 4.1: Promote the existing `#include <include/lighting.hglsl>` to active use**

In `shaders/mech.vert` line 9, the include is already present (Slice A imported it but didn't use any function). No edit needed at the include line itself.

Confirm with:

```bash
grep -n "#include <include/lighting.hglsl>" shaders/mech.vert
```

Expected: line 9.

- [ ] **Step 4.2: Add the `u_lightingMode` uniform declaration**

In `shaders/mech.vert`, find the `uniform mat4 u_mvp;` line (around line 45). Immediately after that line, add:

```glsl
// Slice B1: 0 = Slice A passthrough (baseLight=vec3(1.0)),
// 1 = VS-side calc_light per-vertex. Driven by MC2_GPU_MECH_LIGHTING.
// 'uniform uint' crashes the engine's shader compile (see
// memory/uniform_uint_crash.md); use int.
uniform int u_lightingMode;
```

- [ ] **Step 4.3: Replace the flat-white baseLight with the conditional calc_light branch**

Find the existing block in `shaders/mech.vert` (around line 105):

```glsl
    vec3 baseLight = vec3(1.0);
    // Highlight is still added (selected mechs glow). Highlight alpha=0 by
    // default makes this a no-op for unselected actors.
    baseLight = clamp(baseLight + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
```

Replace the entire block (and the dead-code-reference comment that follows up to the `v_uv = a_uv;` line at ~line 114) with:

```glsl
    // Slice B1: per-vertex GPU lighting via calc_light from lighting.hglsl.
    // u_lightingMode=0 keeps Slice A's flat-white passthrough (used as a
    // fast bisect lever during soak); =1 enables calc_light. The
    // LightsData UBO at LIGHT_DATA_ATTACHMENT_SLOT=0 is bound once at
    // session start by MC_TextureManager (mclib/txmmgr.cpp:318); no
    // per-frame rebind needed. inst.lightDataIndex selects this actor's
    // ObjectLights entry, populated per-actor by
    // mechShape->CacheGpuLightData() in Mech3DAppearance::update().
    //
    // Ambient floor 0.35 prevents shadowed mechs from going pure black
    // (CPU mech path has implicit ambient via its lighting model).
    // Tunable post-soak.
    const float kAmbientFloor = 0.35;
    vec3 baseLight;
    if (u_lightingMode != 0) {
        vec3 base = vec3(kAmbientFloor);
        vec3 litRGB = calc_light(int(inst.lightDataIndex),
                                 worldNormal,
                                 worldMC2,
                                 base);
        baseLight = clamp(litRGB + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                          0.0, 1.0);
    } else {
        // Slice A passthrough: flat white + highlight.
        baseLight = clamp(vec3(1.0) + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                          0.0, 1.0);
    }
```

- [ ] **Step 4.4: Verify the rest of v_litColor assignment is unchanged**

```bash
grep -n "v_litColor       = vec4(baseLight, 1.0);" shaders/mech.vert
```

Expected: one match. If not present, restore that line directly after the block.

- [ ] **Step 4.5: Verify line endings + deploy shader**

```bash
unix2dos shaders/mech.vert
cp -f shaders/mech.vert A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/shaders/mech.vert
diff -q shaders/mech.vert A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/shaders/mech.vert
```

Expected: no output from `diff -q`.

- [ ] **Step 4.6: Build clean**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 4.7: Commit**

```bash
git add shaders/mech.vert
git commit -m "feat(slice-b1): add u_lightingMode + conditional calc_light branch in mech.vert"
```

---

## Task 5: Wire `mechShape->CacheGpuLightData()` in `Mech3DAppearance::update()`

**Files:**
- Modify: `mclib/mech3d.cpp`

- [ ] **Step 5.1: Read the existing update site**

```bash
sed -n '4320,4335p' mclib/mech3d.cpp
```

Confirm `updateGeometry();` is present at line 4327 and the surrounding code is the standard Mech3DAppearance::update body.

- [ ] **Step 5.2: Add the CacheGpuLightData call**

In `mclib/mech3d.cpp`, immediately after `updateGeometry();` (line 4327), add:

```cpp
            // Slice B1: refresh per-actor LightsData UBO slot index for the
            // GPU mech batcher path. Mirrors bdactor.cpp:2314 pattern.
            // No-op when killswitch is off; cheap (one hash lookup +
            // dedup-cache slot write) when on. msl.cpp:1828.
            if (g_useGpuMechs && mechShape) {
                mechShape->CacheGpuLightData();
            }
```

- [ ] **Step 5.3: Build clean**

```
/mc2-build
```

Expected: clean build. `g_useGpuMechs` is already in scope via the existing `#include "../GameOS/gameos/gos_mech_killswitch.h"`.

- [ ] **Step 5.4: Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat(slice-b1): refresh cachedGpuLightIndex_ in Mech3DAppearance::update"
```

---

## Task 6: Source `desc.lightDataIndex` from `getCachedGpuLightIndex()` in render

**Files:**
- Modify: `mclib/mech3d.cpp`

- [ ] **Step 6.1: Replace the hardcoded zero**

In `mclib/mech3d.cpp` line 2509, replace:

```cpp
				desc.lightDataIndex = 0;       // Slice B1 wires this
```

with:

```cpp
				// Slice B1: use the dedup-cache slot populated by
				// CacheGpuLightData() in update(). 0xFFFFFFFFu sentinel
				// means "not yet cached" — fall back to 0, which the
				// shader interprets as "use the engine's default ambient
				// slot" (visually equivalent to Slice A flat-white minus
				// the ambient term; safe).
				const uint32_t cachedLI = mechShape->getCachedGpuLightIndex();
				desc.lightDataIndex = (cachedLI == 0xFFFFFFFFu) ? 0u : cachedLI;
```

- [ ] **Step 6.2: Build clean**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 6.3: Commit**

```bash
git add mclib/mech3d.cpp
git commit -m "feat(slice-b1): wire desc.lightDataIndex from mechShape->getCachedGpuLightIndex"
```

---

## Task 7: Add MC2_MECH_LIGHT_TRACE capacity audit

**Files:**
- Modify: `GameOS/gameos/gos_mech_batcher.cpp`

- [ ] **Step 7.1: Add per-frame init flag near the top of the file**

In `GameOS/gameos/gos_mech_batcher.cpp`, find the existing block of `static bool s_mechBatcherTrace*` declarations (around line 87-88). Immediately after `static bool     s_mechBatcherTraceInit = false;`, add:

```cpp
static bool     s_mechLightTrace      = false;
static bool     s_mechLightTraceInit  = false;
static uint32_t s_lightCacheFullFrames = 0;  // monotonic; emitted on shutdown
```

- [ ] **Step 7.2: Probe over-cap submits in flush()**

In `GameOS/gameos/gos_mech_batcher.cpp` `flush()` function, immediately after the existing `if (!s_mechBatcherTraceInit) { ... }` block (around line 553), add:

```cpp
    if (!s_mechLightTraceInit) {
        s_mechLightTrace     = (getenv("MC2_MECH_LIGHT_TRACE") != nullptr);
        s_mechLightTraceInit = true;
    }
    if (s_mechLightTrace) {
        // LightsData UBO holds 32 ObjectLights entries (lighting.hglsl).
        // Any submit with lightDataIndex >= 32 reads OOB; AMD typically
        // returns zero → flat-black mech. Surface the event so soak ops
        // can raise the cap.
        bool overCap = false;
        for (const auto& ps : s_pendingSubmits) {
            if (ps.desc.lightDataIndex >= 32u) { overCap = true; break; }
        }
        if (overCap) {
            ++s_lightCacheFullFrames;
            std::fprintf(stderr,
                "[MECHLIGHT v1] event=cache_full frames=%u submitted=%zu\n",
                s_lightCacheFullFrames, s_pendingSubmits.size());
        }
    }
```

- [ ] **Step 7.3: Build clean**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 7.4: Commit**

```bash
git add GameOS/gameos/gos_mech_batcher.cpp
git commit -m "feat(slice-b1): add MC2_MECH_LIGHT_TRACE capacity audit (LightsData[32] OOB detector)"
```

---

## Task 8: Add debug mode 9 (lighting-only viz) to mech.frag

**Files:**
- Modify: `shaders/mech.frag`

- [ ] **Step 8.1: Add the new debug branch**

In `shaders/mech.frag`, immediately after the line:

```glsl
    else if (u_debugMode == 8) c = vec4(textureLod(u_tex, v_uv, 0.0).rgb, 1.0);  // explicit LOD 0 sample
```

(at line 66), add:

```glsl
    else if (u_debugMode == 9) c = vec4(v_litColor.rgb / max(textureLod(u_tex, v_uv, 0.0).rgb, vec3(0.001)), 1.0); // lighting-only (lit / texture)
```

- [ ] **Step 8.2: Verify line endings + deploy shader**

```bash
unix2dos shaders/mech.frag
cp -f shaders/mech.frag A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/shaders/mech.frag
diff -q shaders/mech.frag A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/shaders/mech.frag
```

Expected: no output from `diff -q`.

- [ ] **Step 8.3: Build clean**

```
/mc2-build
```

Expected: clean build.

- [ ] **Step 8.4: Commit**

```bash
git add shaders/mech.frag
git commit -m "feat(slice-b1): add debug mode 9 (lighting-only viz: v_litColor / texture)"
```

---

## Task 9: Deploy + smoke + operator visual canary

**Files:** none (validation only)

- [ ] **Step 9.1: Full clean build + deploy**

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 --clean-first
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe
```

Expected: clean build, deploy succeeds, `diff -q` no output.

- [ ] **Step 9.2: A — CPU baseline smoke (regression sentinel)**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, no GL errors. Mech CPU path unaffected.

- [ ] **Step 9.3: B — Slice A only (GPU rendering, flat-white lighting)**

```bash
MC2_GPU_MECHS=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, `[MECHBATCHER v1] event=summary fallback_total=0`. Mechs render textured but flat-lit (Slice A behavior preserved when MC2_GPU_MECH_LIGHTING is unset).

- [ ] **Step 9.4: C — B1 enabled (GPU rendering + GPU lighting)**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_MECH_BATCHER_STATS=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: PASS, +0 destroys, `fallback_total=0`. Mechs render textured AND lit — visually closer to CPU baseline than Slice B above.

- [ ] **Step 9.5: D — Capacity audit on tier1 sweep**

```bash
MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_MECH_LIGHT_TRACE=1 \
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe "A:/Games/mc2-opengl/mc2-win64-v0.3-gpuMech/mc2.exe"
```

Expected: tier1 5/5 PASS. NO `[MECHLIGHT v1] event=cache_full` events in any mission log.

If `event=cache_full` fires: this slice's gate is BLOCKED. Recipe to raise cap — bump `lightDataStructuresCapacity` in `mclib/txmmgr.cpp` and `MAX_LIGHTS_IN_WORLD` / `LightsData[32]` array size in `shaders/include/lighting.hglsl` in lockstep, per `memory/cpp_glsl_ubo_struct_lockstep.md`. Land that as a separate single-purpose commit before proceeding.

- [ ] **Step 9.6: Operator visual A/B/C canary**

Full-zoom-out and full-zoom-in pass on mc2_01 (Bushwhacker/Razorback variety) and mc2_24 (different mech mix). For each: alternate the three killswitch states (none, MC2_GPU_MECHS=1, MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1) and confirm B1 visually matches the CPU baseline within "GPU lit" tolerance — mechs in shadow are darker; mechs lit by powerplants/spotlights/sun show colored highlights consistent with the CPU path.

Specifically check:
- A daytime open-terrain shot (mc2_01 mission start area).
- A shadowed/canyon shot if mc2_24 has one.
- A cluster of mechs near a powerplant or destructible (for point-light contributions).

If a regression surfaces (e.g. mech goes black where CPU baseline shows lit, or color tint is wrong), flip MC2_GPU_MECH_LIGHTING=0 to retain GPU rendering with Slice A flat-white lighting and report findings.

- [ ] **Step 9.7: No commit step** — validation only. Move to Task 10 for the final memory commit.

---

## Task 10: Adversarial review + memory pin

**Files:**
- Create: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/track_d_slice_b1_shipped.md`
- Modify: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 10.1: Dispatch adversarial review**

Use the `superpowers:code-reviewer` subagent with the dispatch prompt requiring `adversarial-plan-review` skill (per worktree CLAUDE.md "Review Discipline" section). Specific scrutiny vectors:

- `cachedGpuLightIndex_` lifecycle — is the sentinel `0xFFFFFFFFu` actually possible at submit time given the unconditional `CacheGpuLightData` call in update? (If not, the fallback is dead but harmless.)
- LightsData UBO layout consistency between `TG_HWLightsData` C++ struct (`mclib/tgl.h:304`) and the GLSL `LightsData` UBO declaration (`shaders/include/lighting.hglsl:39`) — verify B1 didn't disturb either side.
- VS-side calc_light correctness — does mech.vert pass `worldMC2` (post-axis-swap) or `worldStuff` (pre-swap) as `vertex_world_pos`? It must match the frame the lights data is in. (Spec says worldMC2; verify.)
- `g_useGpuMechLighting` fail-closed when `g_useGpuMechs=false` — confirm calc_light isn't entered when the batcher path is bypassed entirely.

Address all CRITICAL/MAJOR findings inline before proceeding.

- [ ] **Step 10.2: Write the slice memory file**

Create `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/track_d_slice_b1_shipped.md`:

```markdown
---
name: Track D Slice B1 — GPU Mech Lighting shipped 2026-05-08
description: VS-side calc_light wired into mech.vert; opt-in via MC2_GPU_MECH_LIGHTING; mechs in MC2_GPU_MECHS=1 mode now correctly lit
type: project
---

VS-side calc_light landed on `claude/gpu-mech-batcher` worktree.
Opt-in via MC2_GPU_MECH_LIGHTING=1 (independent of MC2_GPU_MECHS;
both must be 1 for B1 to take effect). Default off; soak window
begins 2026-05-08.

## What shipped
- [exact commit range from git log]
- mech.vert: `u_lightingMode` uniform, conditional calc_light with
  ambient floor 0.35, mirroring static_prop.vert Stage 2.C.2.
- mech3d.cpp: CacheGpuLightData() in update(), getCachedGpuLightIndex()
  in render() → desc.lightDataIndex.
- gos_mech_batcher.cpp: g_useGpuMechLighting extern + per-flush
  uniform upload + MC2_MECH_LIGHT_TRACE capacity audit.
- mech.frag: debug mode 9 = v_litColor / texture (lighting-only viz).

## Slice B1 gate (passed)
- Tier1 5/5 PASS at MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1, +0 destroys.
- Operator visual A/B/C canary on mc2_01 + mc2_24: B1 matches CPU
  baseline within GPU-lit tolerance.
- [MECHBATCHER v1] event=summary fallback_total=0 across full smoke.
- No [MECHLIGHT v1] event=cache_full events.
- Adversarial review verdict: PASS or PASS-WITH-FINDINGS-ADDRESSED.

## Deferred to B+
- Shutdown/powerup mech lightsOut wire (real gameplay feature; B1
  doesn't render shutdown mechs as dark). Source signal likely from
  Mech3DAppearance::pilotState or status enum.
- Default-on flip — separate slice after 7-day soak.
- Dual-FBO parity (MC2_MECH_GPU_PARITY) — required before default-on
  per Slice A advisor sign-off.

## Soak begins
2026-05-08. Watch for `event=cache_full` triggers and any operator
report of "shadowed mechs go pure black" (would mean ambient floor
needs raising) or "mech glow under spotlight too saturated" (would
mean per-light contribution capping).
```

Replace `[exact commit range from git log]` with output from `git log --oneline f2e357d..HEAD` (or whatever the spec's first commit was).

- [ ] **Step 10.3: Add MEMORY.md index entry**

In `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`, immediately after the existing line:

```
- ⭐ [Track D Slice A — GPU Mech Batcher shipped 2026-05-08](track_d_slice_a_shipped.md) — opt-in via MC2_GPU_MECHS=1; nine bugs caught during bring-up incl. NEW finding: AMD auto-LOD strict-fail on mech paint textures requires `textureLod(.., 0.0)` not `texture()`; spotlight leaves skipped via "SpotLight_*" name detect
```

add:

```
- ⭐ [Track D Slice B1 — GPU Mech Lighting shipped 2026-05-08](track_d_slice_b1_shipped.md) — VS-side calc_light wired in mech.vert; opt-in via MC2_GPU_MECH_LIGHTING (independent of MC2_GPU_MECHS); ambient floor 0.35 prevents shadowed mechs from going pure black; soak begins; shutdown/powerup mech lightsOut deferred to B+
```

- [ ] **Step 10.4: Commit memory updates**

```bash
git add C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/track_d_slice_b1_shipped.md \
        C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md
```

(These are outside the worktree; commit in their own repo / leave uncommitted if it's a non-git directory. Verify with `git status` from those paths first.)

---

## Spec Coverage Check

| Spec section | Covered by task |
|---|---|
| Architecture data-flow diagram | Tasks 4, 5, 6 (shader + update + render wires) |
| u_lightingMode uniform + binding | Tasks 2, 3 (cache + upload) |
| `g_useGpuMechLighting` extern + env-var init | Task 1 |
| CacheGpuLightData call in update | Task 5 |
| getCachedGpuLightIndex source | Task 6 |
| Sentinel 0xFFFFFFFFu fallback | Task 6 |
| Ambient floor `kAmbientFloor = 0.35` | Task 4 |
| MC2_MECH_LIGHT_TRACE capacity audit | Task 7 |
| Debug mode 9 lighting-only viz | Task 8 |
| Three killswitch separation | Tasks 1, 3 (separate var + per-flush gate) |
| Slice B1 gate validation | Task 9 |
| Adversarial review | Task 10.1 |
| Memory pin + index | Task 10.2-10.4 |
| Out-of-scope: lightsOut, isWindow, dual-FBO parity | Documented in spec; no task |

## Type / Symbol Consistency

- `g_useGpuMechLighting` — declared Task 1.1, defined Task 1.2, read Task 3.1 ✓
- `s_loc_u_lightingMode` — declared Task 2.1, set Task 2.2, read Task 3.1 ✓
- `u_lightingMode` (GLSL) — declared Task 4.2, read Task 4.3 ✓
- `kAmbientFloor` — declared and used at Task 4.3 (single site) ✓
- `cachedGpuLightIndex_` / `getCachedGpuLightIndex()` — already exists in `mclib/msl.h:276,337`; read in Task 6.1 (no new C++ method) ✓
- `mechShape->CacheGpuLightData()` — already exists at `mclib/msl.cpp:1828`; called in Task 5.2 ✓
- `MC2_GPU_MECH_LIGHTING` env — defined Task 1.2 only ✓
- `MC2_MECH_LIGHT_TRACE` env — defined Task 7.2 only ✓
- `s_mechLightTrace`/`s_mechLightTraceInit`/`s_lightCacheFullFrames` — declared Task 7.1, read+written Task 7.2 ✓

## Placeholder Scan

- No "TBD", "implement later", or vague requirements.
- Every code step has the actual code.
- Every commit has its message.
- Test/validation tasks (9, 10) have their commands.
- Task 10 memory file template has one explicit `[exact commit range from git log]` placeholder — that's a runtime substitution at execution time, not a plan-level gap.

Plan ready for execution.
