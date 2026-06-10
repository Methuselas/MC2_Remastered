# Terrain 8z Fast-Path Dropout Recon

**Purpose:** Capture H2 findings before Phase 8z terrain legacy deletion. This document explains every known reason the fully-armed chunk/water/quadsetup fast path can fall back to legacy `setupTextures()`, and which findings block or constrain 8z.

---

## Section 1: Executive Summary

`mc2TerrainLodChunkEnabled()` (mclib/terrain.cpp:135, decl terrain.h:80, default ON, opt-out `MC2_TERRAIN_LOD_CHUNK=0`) is **NOT** the `setupTextures` skip gate.

The real fast-path skip is a 6-condition conjunction (the `fullyArmed` predicate, mclib/terrain.cpp:3653-3661):

```
IsFrameSolidArmed && IsFrameOverlayArmed && IsFrameMineArmed
    && WaterFastPathOwnsArmedDraw && terrainTextures2!=NULL
    && !drawTerrainGrid && NarrowEnabled
```

combined with `s_armedSkipOn` (`MC2_QUADSETUP_ARMED_SKIP`, default ON) => `skipSetup` (terrain.cpp:3661).

Any false condition re-enables the full `setupTextures` quad loop (~40K quads via `Terrain::geometry()` per-quad loop; `setupTextures()` defined mclib/quad.cpp:684, sole caller mclib/terrain.cpp:3676).

H2 found real 8z blockers (see Sections 3-5).

---

## Section 2: Fast-Path Gate

### 2.1 Outer armed-skip wrapper (terrain.cpp:3649-3661)

```cpp
static const bool s_armedSkipOn = []() {
    const char* v = getenv("MC2_QUADSETUP_ARMED_SKIP");
    ...
}();  // terrain.cpp:3649

const bool fullyArmed =
    gos_terrain_indirect::IsFrameSolidArmed()    &&   // terrain.cpp:3654
    gos_terrain_indirect::IsFrameOverlayArmed()  &&   // terrain.cpp:3655
    gos_terrain_indirect::IsFrameMineArmed()     &&   // terrain.cpp:3656
    gos_terrain_indirect::WaterFastPathOwnsArmedDraw() &&  // terrain.cpp:3657
    (Terrain::terrainTextures2 != NULL)          &&   // terrain.cpp:3658
    !drawTerrainGrid                             &&   // terrain.cpp:3659
    WaterStream::NarrowEnabled();                     // terrain.cpp:3660
const bool skipSetup = s_armedSkipOn && fullyArmed;  // terrain.cpp:3661
```

### 2.2 Inner terrain solid/overlay gate (quad.cpp:739-745)

```cpp
static const bool s_legacyForce =
    (getenv("MC2_SETUPTEXTURES_LEGACY_FORCE") != nullptr);  // quad.cpp:740-741
const bool legacyTerrainNeeded =
    s_legacyForce ||
    !(gos_terrain_indirect::IsFrameSolidArmed() &&
      gos_terrain_indirect::IsFrameOverlayArmed());  // quad.cpp:742-745
```
Kill-switch: `MC2_SETUPTEXTURES_LEGACY_FORCE=1`.

### 2.3 Inner water gate (quad.cpp:725)

```cpp
const bool legacyWaterDraw = !gos_terrain_indirect::WaterFastPathOwnsArmedDraw();
```

### 2.4 WaterFastPathOwnsArmedDraw() decomposition (terrain.cpp:2458-2497)

| Gate | Expression | Notes |
|------|-----------|-------|
| g1   | `s_fastPath` | `MC2_RENDER_WATER_FASTPATH` env OR `gpu_driven::IsWaterEnabled()` (terrain.cpp:2460-2462) |
| g2   | intro-legacy gate | `s_introLegacy ? IsFrameSolidArmed() : true`; restored by `MC2_MISSION_INTRO_LEGACY_RENDER=1` (terrain.cpp:2471-2475) |
| g3   | `WaterStream::IsReady()` | terrain.cpp:2476 |
| g4   | `WaterStream::GetRecipeCount() > 0` | terrain.cpp:2477 |
| g5   | `Terrain::terrainTextures2 != nullptr` | terrain.cpp:2478 |

Returns `g1 && g2 && g3 && g4 && g5` (terrain.cpp:2497).

### 2.5 IsFrameSolidArmed() (gos_terrain_indirect.cpp:2383-2384)

```cpp
bool IsFrameSolidArmed() {
    return s_frameSolidArmed && !s_processArmingDisabled;
}
```

### 2.6 ComputePreflight() arming failure conditions (gos_terrain_indirect.cpp:2455+)

Checked in order:
1. `s_processArmingDisabled` — hard GL fail already tripped (2476); `ForceDisableArmingForProcess()` set it (2396-2401).
2. `!IsEnabled()` — `MC2_TERRAIN_INDIRECT=0` (2477; `IsEnabled()` at 74-88).
3. `!IsDenseRecipeReady()` — recipe not yet built (2486; `IsDenseRecipeReady()` at 1354).
4. `!ResourcesReady()` — GL buffer alloc failed or `g_atlasGLTex==0` (2495; `ResourcesReady()` at 1762).
5. `InMissionTransition()` — permanent no-op stub returning false (2504; stub at 1855).
6. `!ShouldArmGpuTerrain(solidEnabled, hasTileNodeIds, hasColormapAtlas)` — non-colormap map (2528; predicate at gos_terrain_arm_logic.h:42-45).
7. `zero_thin` / `zero_cmd` — zero thin records packed or zero draw commands (2558/2565).

---

## Section 3: Fallback Trigger Table

| ID | Trigger | File:line | Condition | Transient or steady-state | 8z affected? | Required action |
|----|---------|-----------|-----------|--------------------------|-------------|-----------------|
| T1 | `MC2_TERRAIN_INDIRECT=0` | gos_terrain_indirect.cpp:74-88 | `IsEnabled()==false` | steady | YES | document as dead after 8z; kill-switch semantics decision required |
| T2 | Dense recipe / atlas not ready | gos_terrain_indirect.cpp:2486-2495, 1354, 1762-1838 | `!IsDenseRecipeReady()` or `!ResourcesReady()` (`g_atlasGLTex==0`) | transient (first N frames after load) | YES | ensure chunk path renders frame 1 or explicitly accept brief black on load |
| T3 | Legacy non-colormap map (no tile nodes AND no atlas) | gos_terrain_arm_logic.h:42-45, gos_terrain_indirect.cpp:2528-2531 | `ShouldArmGpuTerrain(solidEnabled,false,false)==false` | steady (mission-lifetime) | YES | audit shipped/modded maps; atlas cook or defined fallback BEFORE deletion |
| T4 | `MC2_GPU_DRIVEN=0` | gpu_driven_common.cpp:30-36 | `IsGlobalEnabled()==false` | steady | YES | kill-switch semantics decision |
| T5 | `MC2_GPU_DRIVEN_TERRAIN_SOLID=0` | gpu_driven_common.cpp:63-69 | `IsTerrainSolidEnabled()==false` | steady | YES | kill-switch semantics decision |
| T6 | `MC2_SETUPTEXTURES_LEGACY_FORCE=1` | quad.cpp:739-741 | `s_legacyForce==true` | steady | YES | becomes dead after 8z |
| T7 | Solid not armed / overlay-only | quad.cpp:742-745 | `!IsFrameSolidArmed()\|\|!IsFrameOverlayArmed()` | transient/steady per-frame | YES | covered by arming preconditions |
| T8 | `MC2_GPU_DRIVEN_OVERLAY=0` | gos_terrain_indirect.cpp:224-232, 262 | `IsOverlayEnabled()==false` | steady | YES | decal fallback (M2d) needs answer before deletion |
| T9 | `MC2_GPU_DRIVEN_WATER=0` | gpu_driven_common.cpp:54-61, terrain.cpp:2462 | `IsWaterEnabled()==false` and no `MC2_RENDER_WATER_FASTPATH` | steady | YES | water legacy path removal |
| T10 | WaterStream not ready | terrain.cpp:2476-2477 | `!WaterStream::IsReady()` or `GetRecipeCount()==0` | transient (until `primeMissionTerrainCache`) | YES | warmup answer required |
| T11 | `terrainTextures2==NULL` | terrain.cpp:2478 | legacy non-colormap map | steady (old-format maps) | YES | tied to T3 |
| T12 | `MC2_MISSION_INTRO_LEGACY_RENDER` + solid not armed | terrain.cpp:2471-2475 | `s_introLegacy && !IsFrameSolidArmed()` | transient (intro pan) | YES | opt-in; warmup answer required |
| T13 | `drawTerrainGrid` editor mode | terrain.cpp:3659, terrain.cpp:258 | `drawTerrainGrid==true` | steady (editor session) | YES | 8z-B editor quarantine MANDATORY |
| T14 | `NarrowEnabled()==false` (`MC2_WATER_UPLOAD_NARROW=0`) | terrain.cpp:3660, gos_terrain_water_stream.cpp:81-88 | `!NarrowEnabled()` | steady | partial (blocks skip opt only) | kill-switch semantics decision |
| T15 | `MC2_QUADSETUP_ARMED_SKIP=0` | terrain.cpp:3649-3661 | `s_armedSkipOn==false` | steady | partial | kill-switch semantics decision |
| T16 | `s_processArmingDisabled` (hard GL fail) | gos_terrain_indirect.cpp:1659, 2396-2401, 2476 | `processArmingDisabled==true` | steady once tripped | YES | define post-8z hard-fail behavior (must be loud; currently silent fallback) |
| T17 | Mine enqueue suppression (LOD chunk) | quad.cpp:990-995 | `mc2TerrainLodChunkEnabled()\|\|IsFrameMineArmed()` | steady (chunk ON) | YES | deletion must not re-enable mine path |
| T18 | Grid not divisible by `verticesBlockSide`, chunk OFF | terrain.cpp:501-504 | `!chunkEnabled && (realVerticesMapSide-1)%verticesBlockSide!=0` -> `STOP()` | fatal | YES | only chunk mode allows partial-edge blocks |
| T19 | `DrawIndirect()==false` -> `ForceDisableArmingForProcess` | gos_terrain_indirect.cpp:2396-2401 | permanent fallback | steady once tripped | YES | define post-8z behavior (must be loud) |
| T20 | `MC2_TERRAIN_LOD_CHUNK=0` | terrain.cpp:135-142 | `mc2TerrainLodChunkEnabled()==false` | steady | YES | this opt-out becomes impossible after 8z; kill-switch semantics decision |

---

## Section 4: Surprising Findings

**Finding 1 — MC2_TERRAIN_ACTIVE_AB resurrects makeLists under chunk ON**

`MC2_TERRAIN_ACTIVE_AB` forces `makeLists` even with chunk ON (terrain.cpp:1523):

```cpp
if (!mc2TerrainLodChunkEnabled() || getenv("MC2_TERRAIN_ACTIVE_AB"))
    Terrain::mapData->makeLists(...);  // terrain.cpp:1523-1526
```

This reactivates the legacy vertex/quad list build that `slimReduce` + `TerrainQuad::draw` consume. 8z deletion **silently breaks** this A/B diagnostic; it was not previously documented as a deletion casualty. The env must be retired or converted before 8z.

**Finding 2 — `s_resourcesAllocated` latched true on GL allocation failure**

`ResourcesReady()` sets `s_resourcesAllocated = true` at lines 1789 and 1820, but line 1774 guards `if (s_resourcesAllocated) return false` on the next call. If `glGenBuffers` returns 0 (driver failure), the buffers are invalid yet `s_resourcesAllocated` is set — `ResourcesReady()` returns false permanently, the GPU path never arms, and there is a silent permanent fallback to `setupTextures` with no throttled log after the first warning. After 8z this becomes a **silent black-terrain failure** with no operator recovery path.

**Finding 3 — InMissionTransition() is a permanent no-op stub**

`InMissionTransition()` is defined as `static inline bool InMissionTransition() { return false; }` at gos_terrain_indirect.cpp:1855. The hook is present in `ComputePreflight()` at line 2504 but never fires. Hot-reload or mid-game map-swap would be ungated. Undocumented preflight gap. Not a current blocker but worth noting before any live-reload work.

**Finding 4 — Water-armed-but-solid-not-armed intro frames run ~40K empty setupTextures calls**

When water is armed but solid is not yet armed (intro frames), the outer `skipSetup` is false because `IsFrameSolidArmed()` is false. `setupTextures()` runs the full ~40K quad loop; the water `(ii)` block is skipped (not the legacy water path), the terrain recipe block is skipped (solid not armed). The loop does near-nothing but still iterates. Undocumented intro-frame perf residual. Bank in Baseline A as known cost.

**Finding 5 — `drawTerrainGrid` is a global bool in the shared game binary, not compile-gated**

`drawTerrainGrid` is declared at terrain.cpp:258 as a plain global `bool`, not gated to an editor build. Any stray set in a release binary permanently suppresses the quad-skip optimization. 8z-B quarantine should prefer a compile-time editor gate over relying on the runtime global.

---

## Section 5: 8z-A Precondition Checklist

- [ ] Confirm no production or modded map hits T3 (non-colormap, no atlas -> never arms). If any exist: atlas cook BEFORE deletion, or a defined fallback.
- [ ] Define behavior for T16/T19 hard-GL-failure after deletion (currently silent fallback; must become a loud assert/log, or keep a minimal emergency draw path).
- [ ] Retire or convert `MC2_TERRAIN_ACTIVE_AB` (Finding 1) — it resurrects `makeLists` under chunk ON.
- [ ] Add loud assert/log at the `s_resourcesAllocated` GL-fail latch (Finding 2) BEFORE its `setupTextures` safety net is removed.
- [ ] Make 8z-B editor quarantine mandatory (T13) and prefer compile-gate/editor-build gate over the runtime `drawTerrainGrid` global (Finding 5).
- [ ] Ensure a first-N-frame terrain-draw answer exists if warmup fallback (T2/T10/T12) is removed (chunk path renders frame 1, or brief black on load is explicitly accepted and documented).

---

## Section 6: 8z Impact

Phase 8z is no longer just deleting `makeLists` / `geometry` / `slimReduce` / `TerrainQuad::draw`. It must **first** eliminate or explicitly replace the steady-state fallback dependencies (T1, T3, T4, T5, T8, T9, T11, T13, T16, T19, T20) and provide a warmup/first-frame answer for transient ones (T2, T10, T12). Partial-skip kill-switches (T14, T15) and the `MC2_TERRAIN_ACTIVE_AB` diagnostic must have defined semantics post-deletion before any legacy code is removed.

---

## Section 7: Stop Conditions

- **Continuous steady-state FASTPATH_DROP** (not warmup) => fast path not actually armed in production => **8z UNSAFE** until resolved.
- **Unknown/unclassified fallback branch** => undocumented legacy dependency => **8z BLOCKED**, escalate.
- **Production map without atlas (T3)** => atlas cook or defined fallback **REQUIRED** before deletion.
- **Editor `drawTerrainGrid` dependency (T13)** => editor quarantine **REQUIRED**.

---

## Section 8: Recommended Next Steps

1. **Implement default-off H2 transition log.** Add `[FASTPATH_DROP] frame=... reason=... quadSetupMs=... water=... chunk=... drawPass=... editor=... fallback=...` gated by `MC2_FASTPATH_DROP_LOG=1`. The reason enum maps to trigger IDs in Section 3.

2. **Run gameplay steady-state.** Confirm drops do not occur in steady-state (expect zero); any continuous drop is an 8z blocker.

3. **Audit shipped/modded maps for T3** (non-colormap / no atlas). One non-atlas map is sufficient to block deletion.

4. **Decide kill-switch semantics** (T1/T4/T5/T6/T14/T15/T20) after 8z — removed/dead or retained as no-ops.

5. **Only then proceed to 8z-A** production deletion; 8z-B editor disposition per T13.

---

*Source: H2 fast-path drop-site recon, 2026-06-09. Read-only; line numbers verified against nifty-mendeleev worktree at recon time — grep to confirm before relying on any citation.*
