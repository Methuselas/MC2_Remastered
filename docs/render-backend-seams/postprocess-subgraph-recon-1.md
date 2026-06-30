# POSTPROCESS-SUBGRAPH-1-RECON-1

Map the `endScene()` sub-stage chain into a declarable subgraph for the frame-graph executor.

Branch: `claude/animated-prop-cook-recon-1`  
Date: 2026-06-29  
Live file refs: `GameOS/gameos/gos_postprocess.cpp` (4866 lines), `RenderCore/frame_executor.h` (117 lines), `RenderCore/RenderPassContract.h` (385 lines), `RenderCore/RenderResourceRegistry.h` (86 lines).

---

## TL;DR

`endScene()` contains **11 declared sub-stages** (plus 2 compute-only substrate passes) in strict order. Five are already executor-owned (`ExecutorIslandId`). The composite pass is the sole FBO-0 binding — it is the subgraph's output edge. Six intermediates (HZB pyramid levels, SSAO RT, sceneDepthCopyTex) are `GLuint` members with no `RenderResourceId` — they must be added before a subgraph can accurately declare reads/writes. The lowest-churn path is to extend `ExecutorIslandId` with per-subpass ids (not a parallel type), reusing the `IslandContract` shape already proven. Recommend declaring all 5 already-owned sub-stages plus composite (6 rows) in slice 1, then folding in the remaining 5 in a follow-up; new `RenderResourceId` stubs (append-only) are needed for the 3 ungated intermediates before slice 2.

---

## Q1 — Full ordered sub-stage inventory

Call order is from `endScene()` lines 2402–2669. Each line citation is the **call site** inside `endScene()`.

| # | Name | `run*()` fn | Call-site line | Gate / WillRun condition | FBO target | Executor-owned? |
|---|---|---|---|---|---|---|
| 1 | **HzbReduce** | `runHzbReduce()` | 2421 | `hzbEnabled_` (MC2_HZB_BUILD, default **OFF**) | `hzbFBO_` (dedicated per-mip FBO) | No |
| 2 | **HzbProbe** | `runHzbProbe()` | 2422 | `hzbProbeEnabled_` (MC2_HZB_PROBE, default **OFF**; requires HZB_BUILD) | read-only CPU readback, no draw | No |
| 3 | **ClusterDepthPyramid** | `cluster_depth_pyramid::Run()` | 2429 | MC2_CLUSTER_DEPTH_PYRAMID, default **OFF** | compute; no FBO | No |
| 4 | **LightgridBuild** | `lightgrid_build::Run()` | 2437 | MC2_LIGHTGRID_BUILD, default **OFF** | compute; no FBO | No |
| 5 | **PostprocessComputeBlur** | `postprocess_blur::Run()` | 2447 | MC2_POSTPROCESS_COMPUTE_BLUR, default **OFF** | compute on copy tex | No |
| 6 | **ScreenShadow** | `runScreenShadow()` | 2451 | `screenShadowEnabled_ && sceneHasTerrain_ && shadowsEnabled_`; `screenShadowEnabled_` default **ON** | `sceneFBO_` (SingleColor) | No (ScreenShadow excluded from ISLAND-2 — note in frame_executor.h:19) |
| 7 | **CloudShadow** | `runCloudShadow()` | 2458 | `enableCloudShadow_ && sceneHasTerrain_`; MC2_CLOUD_SHADOW default **OFF** | `sceneFBO_` (SingleColor) | **Yes** — `ExecutorIslandId::CloudShadow` (ISLAND-3) |
| 8 | **Shoreline** | `runShoreline()` | 2464 | `shorelineEnabled_ && sceneHasTerrain_`; `shorelineEnabled_` default **ON** | `sceneFBO_` (SingleColor) | **Yes** — `ExecutorIslandId::Shoreline` (ISLAND-3) |
| 9 | **SSAO** | `runSSAO()` | 2468 | `ssaoEnabled_ && sceneHasTerrain_`; MC2_SSAO default **OFF** | `ssaoFBO_` then `sceneFBO_` (SingleColor apply) | No |
| 10 | **BoxDecals** | `drawBoxDecals()` | 2472 | `projectedDecalsEnabled_ && sceneHasTerrain_`; MC2_PROJECTED_DECALS default **OFF** | `sceneFBO_` (SingleColor) | No |
| 11 | **EdgeFog** | `runEdgeFog()` | 2477 | `edgeFogEnabled_ && mapHalfExtent_ > 0 && sceneHasTerrain_`; default **ON** | `sceneFBO_` (SingleColor) | **Yes** — `ExecutorIslandId::EdgeFog` (ISLAND-2) |
| 12 | **FogOob** | `runFogOob()` | 2484 | `fogOobEnabled_ && sceneHasTerrain_`; default **ON** | `sceneFBO_` (SingleColor) | **Yes** — `ExecutorIslandId::FogOob` (ISLAND-2) |
| 13 | **Composite** | inline in `endScene()` | 2489 (`glBindFramebuffer(0)`) | program valid + `initialized_`; always runs | **FBO 0** (backbuffer) | **Yes (outer)** — `ExecutorIslandId::PostProcess` (ISLAND-1) |
| 14 | **ShadowDebugOverlay** | `drawShadowDebugOverlay()` | 2653 | `showShadowDebug_`; default **OFF** | FBO 0 (already bound) | No |

**Notes on compute-only sub-stages (3–5):** These have no FBO at all. HzbReduce writes into `hzbFBO_`; ClusterDepthPyramid, LightgridBuild, and PostprocessComputeBlur are compute dispatches. They consume `sceneDepthTex_` and produce internal images that have no `RenderResourceId`.

---

## Q2 — Reads/writes per subpass + intermediate RTs needing logical IDs

### Per-subpass read/write table

| Sub-stage | Reads (texture / unit) | Writes (target / attachment) | Notes |
|---|---|---|---|
| HzbReduce | `sceneDepthTex_` (unit 0, level 0 seed), then `hzbLevelTex_[level-1]` (unit 0, reduction) | `hzbLevelTex_[level]` via `hzbFBO_` | Multi-pass: N mip reductions |
| HzbProbe | `hzbLevelTex_[L]` (CPU readback) | none (diagnostic) | — |
| ClusterDepthPyramid | `sceneDepthTex_` (inferred; substrate only) | internal tile image (no RenderResourceId) | Inert: no consumer |
| LightgridBuild | depth pyramid tile image (inferred) | internal light-bin grid (no RenderResourceId) | Inert: no consumer |
| PostprocessComputeBlur | `sceneColorCopyTex_` (via `getSceneColorCopyTexture()`) | internal blur output (no RenderResourceId) | Inert: no consumer |
| ScreenShadow | `sceneDepthTex_` (unit 0), `sceneNormalTex_` (unit 1), `shadowDepthTex_` (unit 2, static), `dynShadowDepthTex_`/`dynShadowArrayTex_` (unit 3), `dynamicFullMapTex_` (unit 4, CSM only) | `sceneFBO_` COLOR0 (multiplicative darken) | Uses units 0–4; **NOT executor-owned** — left off ISLAND-2 because it does not restore `glActiveTexture(GL_TEXTURE0)` cleanly (frame_executor.h:19) |
| CloudShadow | `sceneDepthTex_` (unit 0) | `sceneFBO_` COLOR0 (multiplicative darken) | — |
| Shoreline | `sceneDepthTex_` (unit 0), `sceneNormalTex_` (unit 1) | `sceneFBO_` COLOR0 (multiply brighten at shoreline) | — |
| SSAO (pass 1) | `sceneDepthTex_` (unit 0), `sceneNormalTex_` (unit 1) | `ssaoColorTex_` via `ssaoFBO_` (half-res AO) | Two-pass internally |
| SSAO (pass 2 / apply) | `ssaoColorTex_` (unit 0) | `sceneFBO_` COLOR0 (multiply AO) | Reads `ssaoColorTex_` — intermediate not a RenderResourceId |
| BoxDecals | `sceneDepthCopyTex_` (unit 0), `sceneNormalTex_` (unit 1) | `sceneFBO_` COLOR0 (alpha blend decal) | Reads COPY of depth (feedback-safe). `sceneDepthCopyTex_` not a RenderResourceId |
| EdgeFog | `sceneDepthTex_` (unit 0) | `sceneFBO_` COLOR0 (alpha blend fog) | — |
| FogOob | `sceneDepthTex_` (unit 0) | `sceneFBO_` COLOR0 (alpha blend OOB fog) | — |
| Composite | `sceneColorTex_` (unit 0), `sceneObjectIdTex_` (unit 2, optional) | **FBO 0** COLOR0 (overwrite backbuffer) | FBO-0 transition here |
| ShadowDebugOverlay | `shadowDepthTex_` or `dynShadowArrayTex_` (unit 0) | FBO 0 COLOR0 (overlay) | Debug only; uses 2D_ARRAY on CSM path |

### **Intermediate RTs needing new RenderResourceId values**

These are allocated `GLuint` textures inside `gosPostProcess` that are READ or written by sub-stages but have no `RenderResourceId` — they cannot be declared in a complete subgraph without new ids:

| Internal member | Kind | Format | Consumer sub-stage | Notes |
|---|---|---|---|---|
| **`hzbLevelTex_[0..N]`** | Texture2D array (N up to ~12) | R32F | HzbProbe (CPU), future occlusion cullers | **HIGH**: per-level textures are persistent across frames; they survive mission reloads. A subgraph that declares HzbReduce must reference at least a logical "HzbPyramid" id. Could model as a single `RenderResourceId::HzbPyramid` (the mip chain as one logical unit). |
| **`ssaoColorTex_`** | Texture2D (half-res) | R8 or RGBA8 | SSAO apply pass | Produced and consumed within the same endScene; not persistent across frames. One `RenderResourceId::SsaoOcclusion` id suffices. |
| **`sceneDepthCopyTex_`** | Texture2D | same as sceneDepthTex_ | BoxDecals | **HIGH**: allocated lazily by `copySceneDepthForParticles()` (also used by VFX soft-particles outside endScene). Already shared with the VFX pass — this means it IS read across endScene boundaries (VFX read it before endScene; BoxDecals inside). It is NOT persistent frame-to-frame but is persistent within a frame beyond endScene. Needs `RenderResourceId::SceneDepthCopy`. |
| **`sceneColorCopyTex_`** | Texture2D | RGBA16F | PostprocessComputeBlur (substrate only) | Not yet a consumer; substrate. Still needs an id if PostprocessComputeBlur is ever declared. `RenderResourceId::SceneColorCopy` |

The **other** sub-stage intermediates (ClusterDepthPyramid tile image, LightgridBuild grid) are inert substrates with no consumers — they can remain un-typed until a consumer is wired.

---

## Q3 — The composite boundary (FBO 0 transition)

`endScene()` line **2489**: `glBindFramebuffer(GL_FRAMEBUFFER, 0)` is the ONLY site inside endScene that binds FBO 0. All sub-stages 1–12 either use `hzbFBO_`, `ssaoFBO_`, or `sceneFBO_`. Sub-stages 13 (Composite) and 14 (ShadowDebugOverlay) draw to FBO 0.

This is the subgraph's **output edge**: `sceneFBO_.COLOR0 → FBO0.COLOR0` via the fullscreen-quad composite blit. The `IslandContract` for `ExecutorIslandId::PostProcess` already asserts `postRequiresDefaultFbo=true` (frame_executor.h:57), confirming this boundary is known.

**Confirmed**: executor-island-recon-1 conclusion that "composite binds FBO 0 mid-endScene" is accurate and unambiguous — it is the first and only FBO-0 bind in the chain (line 2489).

---

## Q4 — Conditional / config-variable subpasses

| Sub-stage | Gate kind | Default | Frame-skippable? |
|---|---|---|---|
| HzbReduce | env `MC2_HZB_BUILD` | **OFF** | Yes (entire sub-stage no-ops) |
| HzbProbe | env `MC2_HZB_PROBE` + requires HZB_BUILD | **OFF** | Yes |
| ClusterDepthPyramid | env `MC2_CLUSTER_DEPTH_PYRAMID` | **OFF** | Yes |
| LightgridBuild | env `MC2_LIGHTGRID_BUILD` | **OFF** | Yes |
| PostprocessComputeBlur | env `MC2_POSTPROCESS_COMPUTE_BLUR` | **OFF** | Yes |
| ScreenShadow | `screenShadowEnabled_` (member) + `sceneHasTerrain_` | **ON** (in-mission) | Skipped in menus (no terrain). `shadowsEnabled_` also gates. |
| CloudShadow | env `MC2_CLOUD_SHADOW` + `sceneHasTerrain_` | **OFF** | Yes |
| Shoreline | `shorelineEnabled_` (member) + `sceneHasTerrain_` | **ON** (in-mission) | Skipped in menus |
| SSAO | env `MC2_SSAO` + `sceneHasTerrain_` | **OFF** | Yes |
| BoxDecals | env `MC2_PROJECTED_DECALS` + `sceneHasTerrain_` | **OFF** | Yes |
| EdgeFog | `edgeFogEnabled_` (member) + `mapHalfExtent_ > 0` + `sceneHasTerrain_` | **ON** (in-mission) | Skipped in menus |
| FogOob | `fogOobEnabled_` + `sceneHasTerrain_` | **ON** (in-mission) | Skipped in menus |
| Composite | always (only gate: `initialized_` + prog valid) | **always** | No |
| ShadowDebugOverlay | `showShadowDebug_` (member) | **OFF** | Yes |

**Gate-kind taxonomy for the declarative model:**
- `GateKind::EnvAlways` — resolved once at `init()`, never changes (HZB, SSAO, etc.)
- `GateKind::FrameLatch` — `sceneHasTerrain_` latch: false in menus, true in missions (set by `markTerrainDrawn()`)
- `GateKind::EnvAndLatch` — combination (CloudShadow, Shoreline, ScreenShadow, EdgeFog, FogOob)
- `GateKind::Always` — Composite (unconditional)
- `GateKind::DebugOverride` — ShadowDebugOverlay

---

## Q5 — Declarative model proposal

### Decision: extend `ExecutorIslandId` vs a new `PostProcessSubpassId`

**Recommendation: extend `ExecutorIslandId`** (lower churn).

Rationale:
1. The 5 already-owned sub-stages already use `ExecutorIslandId` values; `executorOwnBeginSub` / `executorOwnEndSub` dispatch on this enum.
2. The `IslandContract` struct (frame_executor.h:38–47) already has all the right fields for subpass declaration (reads/writes expressed as booleans, gate semantics via `warnIfNoTerrainLatch`, FBO assertions).
3. A parallel `PostProcessSubpassId` would require a second dispatch table and second loop — pure overhead for no semantic gain at this modeling stage.
4. The only downside is that `ExecutorIslandId` entries grow beyond the 5 currently executor-validated ones; non-validated sub-stages get `ownedByExecutor=false` in their rows (see proposed struct extension below).

### `PostProcessSubpass` table proposal — `RenderCore/postprocess_subgraph.h`

```cpp
// RenderCore/postprocess_subgraph.h
//
// POSTPROCESS-SUBGRAPH-1: declarative sub-stage inventory for the PostProcess
// pass. Mirrors the kRenderPassContracts static-array + sentinel + static_assert
// pattern. GL-free; no game-side includes. Offline-testable.
//
// Sub-stage ids are added as ExecutorIslandId values (extends frame_executor.h).
// The 5 already-owned ids keep their existing rows in kExecutorIslands[].
// Sub-stages NOT yet executor-owned have ownedByExecutor=false.

#pragma once
#include <cstdint>
#include "RenderResourceRegistry.h"
#include "frame_executor.h"   // ExecutorIslandId

namespace RenderCore { namespace framegraph {

// Extend ExecutorIslandId (append before Count):
//   HzbReduce       -- new
//   HzbProbe        -- new (diagnostic, no draw)
//   ClusterDepth    -- new (compute substrate)
//   LightgridBuild  -- new (compute substrate)
//   ComputeBlur     -- new (compute substrate)
//   ScreenShadow    -- new (NOT owned: texture-unit leak; see frame_executor.h:19)
//   Ssao            -- new (two-pass internally)
//   BoxDecals       -- new
//   Composite       -- existing PostProcess (ISLAND-1, already owned)
//   ShadowDebugOverlay -- new (debug)
// (EdgeFog, FogOob, Shoreline, CloudShadow already in ExecutorIslandId)

enum class GateKind : uint8_t {
    Always      = 0,  // unconditional (Composite)
    EnvAlways,        // env-var resolved once at init; never changes per-frame
    FrameLatch,       // sceneHasTerrain_ latch only
    EnvAndLatch,      // env gate + sceneHasTerrain_ latch
    DebugOverride,    // debug member flag
};

struct PostProcessSubpass {
    ExecutorIslandId id;
    const char*      name;
    // Resources read. RenderResourceId::Unknown terminates.
    RenderResourceId reads[6]  = {};
    // Resources written. RenderResourceId::Unknown terminates.
    RenderResourceId writes[4] = {};
    GateKind         gateKind;
    const char*      gateEnv;          // env var name (nullptr if none / FrameLatch only)
    bool             defaultOn;        // true = enabled by default when gate is env+present
    bool             bindsFbo0;        // true = explicitly binds FBO 0 (composite boundary)
    bool             ownedByExecutor;  // true = validated by executorOwnBegin/End
    const char*      notes;
};

// static_assert: kPostProcessSubpasses length matches a derived expected count.
// (Omitted here; fill in when header is implemented.)

static constexpr PostProcessSubpass kPostProcessSubpasses[] = {
    {
        ExecutorIslandId::HzbReduce,      // needs new enum value
        "HzbReduce",
        /* reads  */ { RenderResourceId::MainDepth },
        /* writes */ { /* RenderResourceId::HzbPyramid (NEW) */ },
        GateKind::EnvAlways,  "MC2_HZB_BUILD",     /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Builds reverse-Z Hi-Z pyramid from sceneDepthTex_; no consumers yet."
    },
    {
        ExecutorIslandId::HzbProbe,       // needs new enum value
        "HzbProbe",
        /* reads  */ { /* RenderResourceId::HzbPyramid (NEW) */ },
        /* writes */ {},
        GateKind::EnvAlways,  "MC2_HZB_PROBE",     /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "CPU readback diagnostic; no draw, no FBO."
    },
    {
        ExecutorIslandId::ClusterDepth,   // needs new enum value
        "ClusterDepthPyramid",
        /* reads  */ { RenderResourceId::MainDepth },
        /* writes */ {},                 // internal compute image, no RenderResourceId
        GateKind::EnvAlways, "MC2_CLUSTER_DEPTH_PYRAMID", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Compute; inert substrate. Internal tile image has no RenderResourceId yet."
    },
    {
        ExecutorIslandId::LightgridBuild, // needs new enum value
        "LightgridBuild",
        /* reads  */ {},                 // depends on ClusterDepth tile image
        /* writes */ {},                 // internal light-bin grid
        GateKind::EnvAlways, "MC2_LIGHTGRID_BUILD", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Compute; inert substrate. Requires ClusterDepthPyramid output."
    },
    {
        ExecutorIslandId::ComputeBlur,    // needs new enum value
        "PostprocessComputeBlur",
        /* reads  */ { /* RenderResourceId::SceneColorCopy (NEW) */ },
        /* writes */ {},                 // internal blur output
        GateKind::EnvAlways, "MC2_POSTPROCESS_COMPUTE_BLUR", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Compute; inert substrate."
    },
    {
        ExecutorIslandId::ScreenShadow,   // needs new enum value
        "ScreenShadow",
        /* reads  */ { RenderResourceId::MainDepth, RenderResourceId::MainColor /*via normal*/,
                       RenderResourceId::ShadowStaticMap, RenderResourceId::ShadowDynamicMap },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::EnvAndLatch, nullptr, /*defaultOn*/ true,
        /*bindsFbo0*/ false, /*owned*/ false,
        "NOT executor-owned: uses tex units 0-4 incl. GL_TEXTURE_2D_ARRAY; "
        "does not restore glActiveTexture(GL_TEXTURE0). See frame_executor.h:19."
    },
    {
        ExecutorIslandId::CloudShadow,    // EXISTING ISLAND-3
        "CloudShadow",
        /* reads  */ { RenderResourceId::MainDepth },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::EnvAndLatch, "MC2_CLOUD_SHADOW", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ true,
        "Multiplicative cloud darkening over all non-sky pixels."
    },
    {
        ExecutorIslandId::Shoreline,      // EXISTING ISLAND-3
        "Shoreline",
        /* reads  */ { RenderResourceId::MainDepth, /* sceneNormalTex_ = MainColor MRT1, no id */ },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::FrameLatch, nullptr, /*defaultOn*/ true,
        /*bindsFbo0*/ false, /*owned*/ true,
        "Multiplicative foam brightening at water-terrain boundary."
    },
    {
        ExecutorIslandId::Ssao,           // needs new enum value
        "Ssao",
        /* reads  */ { RenderResourceId::MainDepth, /* sceneNormalTex_ no id */,
                       /* ssaoColorTex_ = RenderResourceId::SsaoOcclusion (NEW, internal) */ },
        /* writes */ { RenderResourceId::MainColor,
                       /* RenderResourceId::SsaoOcclusion (NEW) */ },
        GateKind::EnvAndLatch, "MC2_SSAO", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Two-pass: AO->ssaoFBO_, apply->sceneFBO_. ssaoColorTex_ is an unregistered intermediate."
    },
    {
        ExecutorIslandId::BoxDecals,      // needs new enum value
        "BoxDecals",
        /* reads  */ { RenderResourceId::MainDepth /* copy — SceneDepthCopy (NEW) */,
                       /* sceneNormalTex_ no id */ },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::EnvAndLatch, "MC2_PROJECTED_DECALS", /*defaultOn*/ false,
        /*bindsFbo0*/ false, /*owned*/ false,
        "Reads sceneDepthCopyTex_ (VFX-shared depth copy). "
        "SceneDepthCopy crosses endScene boundary — shared with VFX pass."
    },
    {
        ExecutorIslandId::EdgeFog,        // EXISTING ISLAND-2
        "EdgeFog",
        /* reads  */ { RenderResourceId::MainDepth },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::EnvAndLatch, "MC2_EDGE_FOG", /*defaultOn*/ true,
        /*bindsFbo0*/ false, /*owned*/ true,
        "Alpha-blends edge fog into sceneFBO_; stays on sceneFBO_."
    },
    {
        ExecutorIslandId::FogOob,         // EXISTING ISLAND-2
        "FogOob",
        /* reads  */ { RenderResourceId::MainDepth },
        /* writes */ { RenderResourceId::MainColor },
        GateKind::EnvAndLatch, "MC2_OOB_FOG", /*defaultOn*/ true,
        /*bindsFbo0*/ false, /*owned*/ true,
        "Alpha-blends OOB ground fog into sceneFBO_; stays on sceneFBO_."
    },
    {
        ExecutorIslandId::PostProcess,    // EXISTING ISLAND-1 (composite)
        "Composite",
        /* reads  */ { RenderResourceId::MainColor, RenderResourceId::Backbuffer },
        /* writes */ { RenderResourceId::Backbuffer },
        GateKind::Always, nullptr, /*defaultOn*/ true,
        /*bindsFbo0*/ true, /*owned*/ true,
        "SUBGRAPH OUTPUT EDGE. Binds FBO 0 (line 2489), blits sceneFBO_.COLOR0 -> backbuffer. "
        "FXAA, exposure, viewmode, LOWLIGHT all done here."
    },
    {
        ExecutorIslandId::ShadowDebugOverlay, // needs new enum value
        "ShadowDebugOverlay",
        /* reads  */ { RenderResourceId::ShadowStaticMap, RenderResourceId::ShadowDynamicMap },
        /* writes */ { RenderResourceId::Backbuffer },
        GateKind::DebugOverride, nullptr, /*defaultOn*/ false,
        /*bindsFbo0*/ false,  // FBO 0 already bound by Composite above
        /*owned*/ false,
        "Debug overlay; draws on top of composite on FBO 0."
    },
};

// Offline self-consistency test shape (see Q5 offline test):
// For each subpass in order:
//   1. Every read[] must be either (a) produced by an earlier subpass's writes[],
//      or (b) listed as an external input (MainColor/MainDepth/ShadowMaps produced
//      by the upstream frame-graph passes).
//   2. Composite (bindsFbo0=true) must be the last non-debug subpass.
//   3. ShadowDebugOverlay (if present) must follow Composite.
//   4. No subpass with gateKind==Always may be ownedByExecutor=false.

}} // namespace RenderCore::framegraph
```

### `sceneNormalTex_` gap

`sceneNormalTex_` (GBuffer1, COLOR_ATTACHMENT1 of sceneFBO_) is read by ScreenShadow, Shoreline, SSAO, and BoxDecals but has **no `RenderResourceId`**. The resource registry currently has `MainColor` (COLOR0) and `MainDepth` (depth-stencil) but nothing for the normal/GBuffer1 attachment. This is an additional id that must be added before those subpasses can be fully declared. Proposed: `RenderResourceId::MainNormal` (append before `Count`).

---

## Q6 — Scope/risk + slicing recommendation

### New `RenderResourceId` values needed (all append-before-Count, safe)

| Proposed id | Used by | Frame-persistent? |
|---|---|---|
| `MainNormal` | ScreenShadow, Shoreline, SSAO, BoxDecals reads | No (per-frame GBuffer) |
| `HzbPyramid` | HzbReduce (write), HzbProbe (read) | **Yes** (survives mission reload) |
| `SsaoOcclusion` | SSAO pass1 (write), SSAO apply (read) | No (within endScene only) |
| `SceneDepthCopy` | BoxDecals read; also VFX pass (cross-boundary!) | No (per-frame copy) |
| `SceneColorCopy` | PostprocessComputeBlur | No (within endScene only) |

All five are pure appends before `Count`. No renumbering. Safe by registry invariant.

### Slicing recommendation

**Slice 1 (low risk, modeling-only, 0 behavior change):** Declare the 6 subpasses that are either already executor-owned OR unconditional:

1. CloudShadow (owned)
2. Shoreline (owned)
3. EdgeFog (owned)
4. FogOob (owned)
5. PostProcess/Composite (owned)
6. ShadowDebugOverlay (not owned, debug)

This requires: extending `ExecutorIslandId` with `ShadowDebugOverlay` (1 new value), adding `RenderResourceId::MainNormal` (1 new id, needed for Shoreline reads declaration). The owned 4 sub-stages already have `IslandContract` rows; only the ShadowDebugOverlay row is new.

Effort: ~1 hour. Risk: none (header-only + static_assert).

**Slice 2 (medium risk, more new ids):** Declare the 5 compute/env-gated sub-stages plus ScreenShadow:

- HzbReduce, HzbProbe, ClusterDepthPyramid, LightgridBuild, PostprocessComputeBlur, ScreenShadow
- Requires: 6 new `ExecutorIslandId` values, `RenderResourceId::HzbPyramid` + `SceneColorCopy`
- ScreenShadow's declaration is informational-only (ownedByExecutor=false; texture-unit leak documented)

**Slice 3:** SSAO + BoxDecals (require `SsaoOcclusion`, `SceneDepthCopy`; `SceneDepthCopy` is cross-boundary with VFX — note carefully in the subgraph that its producer is the VFX pass, not endScene).

### HIGH findings

**HIGH — `sceneDepthCopyTex_` crosses endScene boundaries.** BoxDecals reads `sceneDepthCopyTex_` (line 1358), which is allocated and written by `copySceneDepthForParticles()` — the same copy consumed by the VFX soft-particles pass OUTSIDE endScene (before endScene is called). In the subgraph model, `SceneDepthCopy` must be declared as a read produced by the VFX pass (or a dedicated "CopyDepth" sub-stage), not by endScene itself. If modeled as a PostProcess-internal intermediate it will appear to have no producer.

**HIGH — ScreenShadow reads 5 texture units (0–4) and leaves no `glActiveTexture(GL_TEXTURE0)` restore**, which is why it was explicitly excluded from executor ownership (frame_executor.h:19). Any modeling of ScreenShadow as an executor-owned island requires fixing that cleanup first. The declaration can be added as `ownedByExecutor=false` immediately; ownership upgrade is a separate slice.

**HIGH — `sceneNormalTex_` is read by 4 sub-stages but has no `RenderResourceId`.** In the current `kRenderPassContracts` PostProcess row, `reads[]` lists only `MainColor / MainDepth / ShadowDynamicMap` (RenderPassContract.h:267) — the normal GBuffer input is silently omitted. The subgraph declaration must add `MainNormal` and update the top-level PostProcess contract row as well.

---

## File:line drift notice

The following citations are drift-prone (large active file):

- `endScene()` sub-stage call sites: lines 2421–2653 in `gos_postprocess.cpp` (4866 lines total)
- `ExecutorIslandId` enum: `frame_executor.h` lines 27–34
- `kExecutorIslands[]`: `frame_executor.h` lines 50–103
- `kRenderPassContracts[]` PostProcess row: `RenderPassContract.h` lines 258–277
- `RenderResourceId::Count`: `RenderResourceRegistry.h` line 26 (currently `Count = 15`)

Verify line numbers against live file before any code slice.
