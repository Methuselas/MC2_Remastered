# RECON — FRAME-GRAPH-EXECUTOR-STAGING-RECON-1

Read-only input planning the **next 3 modeling stages** after the resource-DAG validator
(`RenderCore/frame_graph_validate.h`), the ambient ledger + runtime guard
(`RenderCore/ambient_contract.h` + `mclib/render_contract.cpp`), and the pass/resource tables
(`RenderCore/RenderPassContract.h`, `RenderResourceRegistry.{h,cpp}`). Still a **CONTRACT**
(declaration + validation), NOT an executor. This doc covers FBO modeling, texture-read modeling,
and path-variable terrain — the three gaps between "we declare resources" and a safe dry-run.

> All `file:line` below are against nifty HEAD at recon time and DRIFT CHEAPLY. Re-grep
> (`repo_query.py slice-preflight`) before coding any slice derived from this doc.

Reuses, does not re-derive: `frame-graph-recon-1.md` §2 (ambient ledger), §3 (path-variable),
§6 (executor blockers); `opengl-correctness-ledger-1.md` (NVIDIA 2D_ARRAY residual).

---

## STAGE A — FBO LEDGER  (FRAME-GRAPH-FBO-LEDGER-1)

### What exists

Every FBO is an anonymous `GLuint` member of `GosRenderer`/`gosPostProcess`. Inventory:

| GLuint var | Created (gen) | Attachments | Bound at (key sites) |
|---|---|---|---|
| `sceneFBO_` | `gos_postprocess.cpp:853` | COLOR0=`sceneColorTex_` (:864), DEPTH_STENCIL (:902), COLOR1=`sceneNormalTex_` (:913), COLOR2=objectId (:929, optional) | endScene scene bind :1338/:1407; all post sub-passes :1976/2035/2182/2236/2289/2345; restore-after-shadow :3502 |
| `shadowFBO_` (static) | `:3384` | DEPTH=`shadowDepthTex_` (:3399), COLOR0=`shadowDummyColorTex_` (:3408) | :3426/:3452/:3481 |
| `dynShadowFBO_` | `:3711` | dynamic shadow depth | :3750; `gameos_graphics.cpp:6502` via `getDynamicShadowFBO()` |
| `dynShadowArrayFBO_` | `:3813` | one CSM array layer (depth) | :3955 (CSM path, `MC2_SHADOW_CSM`) |
| `dynamicFullMapFbo_` | `:3864` | full dynamic shadow map | :3951 |
| `waterReflFBO_` | `:1010` | COLOR0=`waterReflColorTex_` (:1012), DEPTH=`waterReflDepthTex_` (:1013) | gated `MC2_WATER_REFLECTION_RT`; built `gamecam.cpp:522` |
| `ssaoFBO_` | `:975` | COLOR0=`ssaoColorTex_` (:977) | :1946 |
| `hzbFBO_` | `:1077` | re-attaches dst HZB mip level per-call (:1496) | :1488 |
| default (0) | n/a (window) | backbuffer | many `glBindFramebuffer(...,0)`; composite target before present :2469 |
| terrain-indirect transient | n/a (saves/restores `prevFBO`) | borrows caller FBO | `gos_terrain_indirect.cpp:3797/3805/3822` |

Accessors already exist: `getSceneFBO()` (gos_postprocess.h:63), `getWaterReflectionFBO()` (:89),
`getShadowFBO()` (:101), `getDynamicShadowFBO()` (:140). `gameos_graphics.cpp:6254/6502` bind
shadow FBOs through these getters. `csmSavedFBO_` (:432) and the terrain-indirect `prevFBO`
save/restore pattern prove the engine already treats "the currently-bound FBO" as ambient state
it must stash and restore — exactly the hazard a ledger formalizes.

### Logical-id mapping

| FBO GLuint | Logical RenderResourceId (target) | Status |
|---|---|---|
| `sceneFBO_` COLOR0 | `MainColor` | id EXISTS |
| `sceneFBO_` DEPTH_STENCIL | `MainDepth` | id EXISTS |
| `sceneFBO_` COLOR1 (normals) | — | **NEW id needed** (`MainNormal` / GBuffer1) |
| `sceneFBO_` COLOR2 (objectId) | — | **NEW id needed** (`ObjectId`, optional MRT) |
| `shadowFBO_` DEPTH | `ShadowStaticMap` | id EXISTS |
| `dynShadowFBO_` DEPTH | `ShadowDynamicMap` | id EXISTS |
| `dynShadowArrayFBO_` / `dynamicFullMapFbo_` | `ShadowDynamicMap` (CSM variant) | id EXISTS (CSM layer = sub-resource, unmodeled) |
| `waterReflFBO_` COLOR/DEPTH | `WaterReflectionColor` / `WaterReflectionDepth` | ids EXIST |
| `ssaoFBO_` COLOR0 | — | **NEW id** (`SsaoColor`) or model as post-internal (out of graph scope) |
| `hzbFBO_` | — | **NEW id** (`HzbPyramid`) or post-internal |
| default (0) | — | **NEW id** (`Backbuffer`) — present target |
| EditorPreview | not found in this TU | **none today** — editor uses GPU path; recon-only if it appears |

So: the executor-relevant targets (scene color/depth, both shadow maps, water reflection) ALL
already have a `RenderResourceId`. The gaps are: `MainNormal`(GBuffer1), `ObjectId`, `Backbuffer`,
and optionally `SsaoColor`/`HzbPyramid` (post-internal scratch — arguably out of the graph).

### `glName` population — the missing wiring

`RenderResourceDesc::glName` (RenderResourceRegistry.h:55) is declared "debug only" and **is never
populated at runtime**: grep shows `registerOrUpdateRenderResource()` has ZERO production callers
— only `tests/unit/test_rendercore.cpp` writes, and `GuiRuntime/GraphicsOptionsWindow.cpp:2128`
reads. The registry is an empty array (`s_registry[kSlots] = {}`, RenderResourceRegistry.cpp:9) in
a live game. So `getRenderResourceCount()` returns 0 at runtime today.

### Runtime probe feasibility — HIGH, and the sampler already exists

`noteRenderPass()` ALREADY samples the live draw FBO:
`glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo)` (render_contract.cpp:682) and prints
`[RENDER_PASS v1] ... fbo=<GLuint> ...` per pass (:692). The raw per-pass bound-FBO is therefore
ALREADY captured — it is just an opaque integer, unmapped to a logical id.

Mirror-the-ambient-guard design is directly buildable:
1. **Populate the registry**: at FBO creation (the `glGen*` sites above), call
   `registerOrUpdateRenderResource()` setting `glName = <fbo GLuint>` and `valid=true`. One call per
   FBO; ~9 call sites. (FBO GLuint != texture GLuint — add an `fboGlName` field or a tiny FBO-side
   table; `glName` currently conflates "the texture name".) **Decision point**: model the FBO as its
   own thing or keep glName = the color-attachment texture? Cleanest is a parallel `kFboLedger[]`
   mapping FBO-GLuint → expected `RenderResourceId` writes[], because one FBO writes MULTIPLE
   resources (sceneFBO_ → MainColor+MainDepth+MainNormal).
2. **Per-pass expected target**: each `kRenderPassContracts[]` row's `writes[]` already declares the
   logical targets. Add a derived "expected FBO" = the FBO whose ledger-declared writes[] superset
   the pass writes[].
3. **Runtime cross-check**: in `noteRenderPass`, look up live `fbo` in the FBO ledger → logical
   writes → compare to the pass's declared writes[]. Count mismatches; default-OFF diagnostic
   (`MC2_FBO_PROBE`), promote to fatal later. EXACTLY the `compareAmbient` shape (pure compare +
   runtime sample), already proven at 0 divergence for ambient.

### Verdict — **BUILDABLE NOW** (two-slice)

- **Slice A1 (buildable)**: add `kFboLedger[]` (FBO-GLuint → writes[] logical ids) + populate it at
  the ~9 `glGen*` sites via `registerOrUpdateRenderResource` (or a dedicated FBO register fn). Pure
  additive; no behavior change. Add a unit test that the ledger covers every pass row's writes[].
- **Slice A2 (buildable, after A1)**: extend `noteRenderPass` to map live `fbo` → logical writes and
  `compareFbo()` against the declared pass writes[]; default-OFF, diagnostic counter. Mirrors the
  ambient runtime guard 1:1.
- **Risk**: LOW. Additive, default-OFF, no reorder. Two real traps: (a) `hzbFBO_` re-attaches its
  target every call (:1496) — model as "dynamic, skip" not a fixed target; (b) the terrain-indirect
  transient FBO borrows the caller's binding (:3797) — it has no stable logical id, declare it
  "inherits caller target" and skip. CSM array-layer sub-targets are a sub-resource the flat id
  enum can't express — note as a known modeling limitation, do not block on it.

---

## STAGE B — TEXTURE-UNIT / RESOURCE-READ LEDGER  (FRAME-GRAPH-TEXTURE-LATCH-RECON)

### What exists

Texture binding is per-draw and mostly NOT guarded. The binding pattern is
`glActiveTexture(GL_TEXTUREn) + glBindTexture(target, name)` scattered across batchers and post
passes. The ONE existing guard is `GlScopedTextureUnit` (RAII), applied to exactly three live sites:
overlay/decal/decalStatic in `gameos_graphics.cpp:9398/9516/9589` (per opengl-correctness-ledger-1.md
:58). `gos_InvalidateRenderStateCache()` forces legacy `gos_SetRenderState` re-apply but
**explicitly does NOT track texture units** (frame-graph-recon-1.md §2, "texture-unit latches" row;
ledger-1 :138). So most passes inherit whatever unit bindings the prior pass left.

Which passes bind their own vs inherit: shadow-map consumers (StaticProp/Mech/Terrain/Veg all
read `ShadowDynamicMap`) bind the shadow unit themselves each pass; the terrain-indirect path
captures+restores the unit-4 sampler2DArray (`gameos_graphics.cpp:3888-3897/:4044-4074`,
GLSTATE-TEXTURE-ARRAY-RESTORE-1, PROVEN_COVERED). Post-composite unit-0/2 are guard-wrapped. The
HAZARD class is everything else: a pass that samples a texture it assumes a prior pass left bound.

### The documented NVIDIA 2D_ARRAY residual leak — characterized

Two distinct items in `opengl-correctness-ledger-1.md`:
- **GLSTATE-SHADOWDEBUG-2DARRAY-1** (:60, DEFERRED_LOW_RISK): the postprocess shadow-debug overlay
  binds a `GL_TEXTURE_2D_ARRAY` on unit0 and never rebinds 0 (`gos_postprocess.cpp:2632-2645`). Gated
  `showShadowDebug_` (default-off, ImGui-only) → not live. Fix is a one-liner `glBindTexture(target,0)`
  at :2645 IF ever default-on.
- **Residual 2D_ARRAY-target leak class** (:154, OPEN/NEEDS_VENDOR_TEST): the general hazard that a
  `GL_TEXTURE_2D_ARRAY` binding bleeds into a later array consumer. `GlScopedTextureUnit` does NOT
  cover array targets (:151 "outside GlScopedTextureUnit scope → SKIP"). No AMD repro; needs a
  RenderDoc NVIDIA capture to confirm a live bleed. This is the canonical "pass works because a
  prior pass left a texture bound" ghost the texture ledger would model.

### Runtime cross-check of reads[] vs bound units — the gap

`kRenderPassContracts[].reads[]` lists logical `RenderResourceId`s (ShadowDynamicMap, MainDepth,
WaterReflection*). To cross-check at runtime you would, per pass, query each texture unit
(`glGetIntegeri_v(GL_TEXTURE_BINDING_2D, unit)` / `_2D_ARRAY`) and map the bound GLuint back to a
logical id — but there is **no GLuint→logical-id reverse map for textures** (same gap as Stage A's
FBO map, and worse: textures live in `mcTextureManager` + per-FBO attachments + shadow maps, many
owners). And `reads[]` does not record WHICH unit a resource is expected on, so even with a reverse
map you cannot say "ShadowDynamicMap must be on unit N". The binding-slot occupancy doc
(`sampler-unit-occupancy.{md,json}`, `binding-slot-occupancy.md`) is the per-pass unit inventory that
WOULD seed the "expected unit" column, but it is multiplexed per-pass (binding-slots are reused
across passes, per the GPU-BINDING-SLOTS-LOCKSTEP-1 finding) — there is no single flat unit→resource
map.

### Verdict — **RECON-ONLY** (with one small buildable carve-out)

Almost entirely recon-only, because:
1. No texture GLuint→logical-id reverse map exists (and textures have many owners, unlike the ~9 FBOs).
2. `reads[]` lacks an expected-unit column; binding slots are multiplexed per-pass so there is no flat
   truth to compare against.
3. The live hazard (2D_ARRAY bleed) is NVIDIA-only and unreproduced on dev AMD — you cannot validate
   a guard you cannot trigger.

**Eventual ledger shape** (the modeling proposal): per pass, a `samples[]` of
`{RenderResourceId, expectedUnit, target}` rows, plus a `boundUnitsRestored` bool. A runtime probe
would sample `GL_TEXTURE_BINDING_*` for the declared units and assert the expected logical resource
is bound, AND assert each pass restores the units it bound (the leak check). This requires (a) Stage A's
mapping work generalized to textures and (b) extending `reads[]` to `samples[]` with unit numbers
sourced from `sampler-unit-occupancy.json`.

**Small buildable carve-out**: a STATIC doc-check (no runtime) that every `reads[]` resource has at
least one declared producer earlier in `kFramePassOrder[]` OR is a sanctioned pre-seed (ShadowDynamic
frameBegin pre-seed, Water reflection temporal) — i.e. extend `frame_graph_validate.h`'s stale-resource
check to flag a read with no live binder. That is buildable now and is the cheapest down-payment on the
texture ledger.

---

## STAGE C — PATH-VARIABLE TERRAIN  (modeling proposal)

### The branches

Terrain solid draw is a 3-way branch in `mclib/txmmgr.cpp` (frame-graph-recon-1.md §3 confirmed
against current lines):

```
if (gos_terrain_indirect::IsFrameSolidArmed())          -> INDIRECT BRIDGE (DrawIndirect)   txmmgr.cpp:2999
else if (TerrainPatchStream::isReady() && !overflowed)  -> LOD-CHUNK / PatchStream::flush()  txmmgr.cpp:3016-3019
else                                                     -> LEGACY MLR gos_RenderIndexedArray  txmmgr.cpp:3089/3105
```

Gates/defaults:
- `IsFrameSolidArmed()` is **CAMERA-WINDOWED** — armed only under live in-game camera motion;
  deterministic capture/smoke does **NOT** arm it (:3023 "capture takes the legacy MLR path" comment).
- `g_useGpuStaticProps`/`g_useGpuObjects` gate the *object* phase (:2655/3138), not terrain solid.
- So: **capture/smoke exercises the LEGACY MLR branch** (`gos_RenderIndexedArray`, the
  `masterVertexNodes` loop :3089). In-game free-cam exercises INDIRECT. PatchStream is the middle
  state. This is the #1 capture/smoke vs in-game divergence (recon §3 "structural smell").

### What differs between branches

| Axis | Legacy MLR | LOD-chunk (PatchStream) | Indirect bridge |
|---|---|---|---|
| Draw call | `gos_RenderIndexedArray` per masterVertexNode | `TerrainPatchStream::flush()` | `glMultiDrawElementsIndirect` |
| MVP source | live `terrainMVP` (per-draw upload) | live MVP | **dispatch-MVP snapshot** + `g_viewContentEpoch` bump (gos_object_draw_mvp.h) |
| Writes | MainColor, MainDepth | MainColor, MainDepth | MainColor, MainDepth (same) |
| Reads | ShadowDynamicMap | ShadowDynamicMap | ShadowDynamicMap + recipe/thin SSBOs + cement atlas |
| Barrier | none (immediate) | none | **compute → GL_COMMAND_BARRIER → indirect** (mis-order = stale instances) |
| markTerrainDrawn latch | set | set | set (must — else 4 post passes bail) |
| colorMask re-assert | yes | yes | yes |
| Extra ambient | — | — | publishes dispatch-MVP/epoch BEFORE object phase |

The WRITES and the two latches (colorMask re-assert, markTerrainDrawn) are identical across branches —
that is what keeps the single `RenderPassId::Terrain` row honest TODAY for resource flow. What differs
is **MVP source** (live vs snapshot+epoch), **barrier requirement** (none vs COMMAND), and **reads**
(indirect adds SSBOs + cement atlas). Those three differences are invisible to the current single row.

### Verdict — **RECON-ONLY** (modeling proposal: sub-pass split)

The executor cannot treat "Terrain" as one pass. Proposed sub-pass split (declarative rows; the
imperative loop still branches internally — this is modeling, not dispatch):

- `RenderPassId::TerrainSolidLegacy` — live MVP, no barrier, the branch CAPTURE EXERCISES. Mark
  `capturePath=true`.
- `RenderPassId::TerrainLODChunk` — PatchStream live-MVP flush.
- `RenderPassId::TerrainIndirectBridge` — snapshot MVP + epoch publish + `barrierAfter=Command`,
  reads += recipe/thin SSBOs + cement atlas (needs NEW resource ids: `TerrainRecipeBuffer`,
  `TerrainThinBuffer`, `CementAtlas` — none exist today).

All three share: writes {MainColor,MainDepth}, produces terrain latch, re-asserts colorMask. Encode
that shared contract once (a "terrain family" trait) and let the three rows differ only on
MVP-source / barrier / reads. Add a declared `mutuallyExclusive` group {Legacy, LODChunk, Indirect}
with a runtime assertion that **exactly one** fires per frame (mirrors the existing parity counters
`legacy_solid_setup_quads` vs `indirect_overlay_packed_quads`). The `capturePath` flag makes the
capture/smoke divergence a DECLARED fact instead of a buried comment — a frame-graph that validates
only the indirect branch would then fail the "capture exercises Legacy" assertion at declare time.

**Not buildable as one slice** — needs 3 new RenderPassId rows (renumber-safe append) + 3 new
RenderResourceIds + a mutual-exclusion group concept. Sequence it AFTER Stage A (FBO ledger gives the
target-binding precedent the sub-pass rows reference) and AFTER the static stale-resource check from
Stage B's carve-out. Pure modeling; zero behavior change; the value is making the capture/in-game
divergence a tested invariant.

---

## EXECUTOR READINESS

Against the advisor's staging (guard ~30-35% → FBO ~35-45% → texture ~45-60% → path-variable
~60-70% → dry-run ~70-80%):

**Where we actually are: ~30-35% (ambient guard shipped, 0 divergence).** The resource DAG validator
+ ambient ledger + runtime ambient guard are done and proven. FBO is NOT started but is the cheapest
next stage and is **buildable now** (sampler already exists in `noteRenderPass`). Texture is the
hard, recon-only middle. Path-variable terrain is modeling-only. Nothing approaching a dry-run is
safe until at least FBO is mapped and the terrain split is declared.

**Ordered slice list to FRAME-GRAPH-EXECUTOR-DRYRUN-1:**

1. **A1 — FBO ledger table** (`kFboLedger[]` + populate at ~9 `glGen*` sites). Buildable. LOW risk.
2. **A2 — FBO runtime probe** (map live `GL_DRAW_FRAMEBUFFER_BINDING` → logical writes, compare to
   declared, default-OFF). Buildable. LOW risk. Lands us ~35-45%.
3. **B-carveout — static stale-read check** (extend `frame_graph_validate.h`: every `reads[]` has an
   earlier producer or is a sanctioned pre-seed). Buildable. LOW risk.
4. **C — terrain sub-pass split** (3 rows + 3 resource ids + mutual-exclusion group + `capturePath`
   flag + one-fires-per-frame assertion). Modeling-only, no behavior change. MED modeling effort.
   Lands us ~60-70%.
5. **B-full — texture/sample ledger** (`samples[]` with expectedUnit/target + texture GLuint→logical
   reverse map + restore-check). RECON-ONLY today; blocked on NVIDIA repro for the live hazard and on
   a texture reverse-map. Defer; revisit after a vendor capture.
6. **DRYRUN-1** — walk `kFramePassOrder[]` (with the terrain split), for each pass declare expected
   {FBO target, ambient state, resource reads/writes}, sample live GL, compare, count divergences.
   NO scheduling, NO reorder. This is the integration of stages A+ambient+C; it does NOT require the
   full texture ledger (B-full), only the B-carveout static check. Reachable after slices 1-4.

**Blunt summary:** FBO is buildable now (2 slices, low risk, sampler already present, all
executor-relevant target ids already exist — only need a GLuint→id map and ~9 register calls).
Texture is recon-only (no reverse map, multiplexed slots, NVIDIA-only unreproduced hazard) — ship
only the static stale-read carve-out now. Terrain path-variable is modeling-only but MANDATORY before
a dry-run, because capture exercises the legacy branch the single Terrain row under-models. DRYRUN-1
needs A + ambient + C, not B-full.
