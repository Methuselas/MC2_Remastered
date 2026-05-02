# Object Offload — Slice 2 — Recon Zero

Date: 2026-05-02
Branch: `claude/nifty-mendeleev`
Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
Slice: 2 of the object-offload arc (gated on this recon)
Slice 1 close: `dd8761a feat(objects): Gate F counters + summary emission + late-registration accounting` (2026-05-02)
Author: ThranduilsRing + Claude (Opus 4.7, 1M context)
Status: research deliverable; no spec / plan / code-change is gated to this doc directly. Section 2's Tracy data is the only piece that must come from a follow-up build+run.

## TL;DR (FINAL — Tracy data landed 2026-05-02)

- **Branching answer: (2-b) — partial offload.** Keep a reduced CPU `MultiTransformShape` pass that preserves shadow + hit-test inputs and skips lighting bake. Tracy data (Section 2) put `(vlight + flight) / outer` ratio at ~36% slice-2-scoped — squarely in the "marginally justified, 2-b is right scope" band of the decision tree. Recoverable: ~0.5-0.7 ms/frame, ~27-36% of `appearanceUpdate` reduction.
- **GPU lighting feasibility: feasible.** Lighting kernel partially shipped already (`get_base_light()` complete; `calc_light()` is a 2-of-6-light-types stub). MAX_HW_LIGHTS_IN_WORLD=16, UBO and dedup cache (`addLightDataStructure`) already wired.
- **Parity strategy: P3 (single-frame dual-emit) + P1 (ULP-tolerance bytewise)** combined. P2 (pixel-level diff) deferred to default-on flip.
- **SSBO additions for slice 2 are minimal** (4 B per vertex, 12 B per type, 4 B per instance fits in slice 1's existing `_pad0`).
- **READY-FOR-SPEC.** Three pre-spec verification items remain (GetFalloff math, light type ID mismatch, double-draw avoidance for the `addRenderShape` queue) — listed in Section 6's "Pre-spec verification list."

---

## Section 1 — Consumer enumeration of `TransformShape` outputs

### What `TransformShape` produces

Verified live at `mclib/tgl.cpp:1687-2542`:

| Output buffer | File:line written | What it stores | Frequency |
|---|---|---|---|
| `listOfVertices[j].x/.y/.z/.rhw` | `mclib/tgl.cpp:1748-1751` | Screen-space xy, depth z, perspective rhw | per cull-survivor every frame |
| `listOfVertices[j].argb` | `mclib/tgl.cpp:2139, 2160, 2249` | Per-vertex lit color (incl. highlight); pre-face-additive | per cull-survivor every frame |
| `listOfVertices[j].frgb` | `mclib/tgl.cpp:1752, 2202, 2206, 2211, 2224` | Fog/specular packed | per cull-survivor every frame |
| `listOfColors[j].redSpec/.greenSpec/.blueSpec` | `mclib/tgl.cpp:2064-2066` | Terrain-light specular contribution | per cull-survivor with `TG_LIGHT_TERRAIN` active |
| `listOfShadowTVertices[i].transformedPosition` | `mclib/tgl.cpp:3022, 3162` | Shadow-projected vertex screen pos (one per visible shadow caster light) | per cull-survivor in `RenderShadows` |
| `listOfShadowTVertices[i].fRGBFog` | `mclib/tgl.cpp:3071, 3209` | Per-shadow-vertex fog tag | per cull-survivor in `RenderShadows` |
| `listOfTriangles[j].aRGBLight[i]` (i=0..2) | `mclib/tgl.cpp:2424` | Per-triangle-vertex lit ARGB; per-face lighting + per-vertex `.argb` | per cull-survivor visible face |
| `listOfTriangles[j].fRGBLight[i]` (i=0..2) | `mclib/tgl.cpp:2445` | Per-triangle-vertex fog/specular | per cull-survivor visible face |
| `listOfVisibleFaces[]` | `mclib/tgl.cpp:2264` | Indices of front-facing triangles for this frame | per cull-survivor |
| `listOfVisibleShadows[]` | `mclib/tgl.cpp` (`MC2_TGL_GET_FACES`) | Per-shadow-light visible faces | per cull-survivor with shadows |
| `numVisibleFaces` | `mclib/tgl.cpp:2253, 2265` | Count for above | per cull-survivor |
| `lastTurnTransformed` | `mclib/tgl.cpp:1705` | Freshness marker — used by all consumers as gate | per cull-survivor |

### Side-effects (queue calls) inside `TransformShape`

`TransformShape` does NOT cleanly separate "produce state" from "issue draws." It also queues:

| Call | File:line | What it does | Consumer |
|---|---|---|---|
| `mcTextureManager->addTriangle(...)` | `mclib/tgl.cpp:2466, 2470, 2479, 2483` | Queue per-face for legacy software emit | `mcTextureManager::renderLists()` |
| `mcTextureManager->addLightDataStructure(&lightData_)` | `mclib/tgl.cpp:2786` | Dedup-add `TG_HWLightsData` to UBO | shape.lightData_buffer_index |
| `mcTextureManager->addRenderShape(...)` | `mclib/tgl.cpp:2522, 2789` | Queue HW shader-path emit | `mcTextureManager::renderLists()` HW path |

`bShadersDrawPathEnabled=true` (the shader path) is the active branch on Renderer 3 (OpenGL 4.3). The legacy `addTriangle` calls at 2466 are dead-on-this-renderer per the bShadersDrawPathEnabled gate at 2491; verified with `Environment.Renderer != 3` checks at lines 1870, 2164. Slice 1's batcher path doesn't trip them either way.

**Implication for slice 2:** if slice 2 retires `TransformShape` for the GPU population, the `addRenderShape` queue calls also disappear — that side of the legacy queue empties for buildings/trees/generics. Any ordering invariants in `renderLists` that assume objects are queued (e.g. transparent-after-opaque sorts) need re-checking.

### External consumers of `TransformShape` outputs

Forward-grep + reverse-grep against every candidate file (mclib, code, GameOS, shaders). Per `feedback_data_flow_audit_asymmetry.md`, negative claims need opposite-direction verification.

| Consumer site | What it reads | Lifecycle phase | Population |
|---|---|---|---|
| `TG_Shape::Render` (`mclib/tgl.cpp:2552-2542`) | Whole `gos_VERTEX` row from `listOfVertices[]`, then OVERWRITES `.argb` from `listOfTriangles[].aRGBLight[i]` and `.frgb` from `.fRGBLight[i]` (lines 2607-2624). Reads `numVisibleFaces`, `listOfVisibleFaces`, `lastTurnTransformed` | render | all (called from `*Appearance::render` legacy CPU branch) |
| `TG_Shape::RenderShadows` (`mclib/tgl.cpp:3277-3380+`) | `listOfShadowTVertices[i].transformedPosition`, `.fRGBFog` (lines 3309-3311). Reads `numVisibleShadows`, `listOfVisibleShadows`, `lastTurnTransformed` | renderShadows | all 3 static-prop + mech + GV |
| `TG_Shape::PerPolySelect` (`mclib/tglpp.cpp:10-100`) | `listOfVertices[i].x/.y` for triangle hit-test math (lines 42-49). Reads `lastTurnTransformed`, `numVisibleFaces`, `listOfVisibleFaces` | selection (mouse hit) | **buildings only** (BldgAppearance::PerPolySelect at `bdactor.cpp:994` calls `bldgShape->PerPolySelect`); also GVAppearance + Mech3DAppearance for movers (out of slice 2 scope); trees + generics inherit no-op default at `appear.h:528-531` |
| `code/objmgr.cpp:2187, 2196` | Calls `objAppearance->PerPolySelect(mouseX, mouseY)` — the entry point that drives the read above | selection | mouse-drag selection / building click |
| `mcTextureManager->renderLists()` (`mclib/txmmgr.cpp:1012+`) | Indirectly consumes `addRenderShape`-queued shapes (which carry `cur_shape2clip`, `lightData_buffer_index_`, `vb_/ib_/vdecl_`) — these come from `theShape->ib_/vb_/vdecl_` plus `cur_shape2clip` set at `tgl.cpp:2519` and `lightData_` at 2512. | render flush | all (via legacy shader path) |
| `gos_static_prop_batcher::submitMultiShape` (`gos_static_prop_batcher.cpp:617-619, 714`) | Reads `child->listOfVertices[].argb` to memcpy into per-instance color SSBO. Reads `child->listOfVertices`/`listOfColors` for null-check eligibility gate | render (slice 1 fast path) | buildings + trees + generics (when `MC2_GPU_OBJECTS=1`) |

### "Per-shape-state" inputs that flow INTO `TransformShape`

These are not OUTPUTS but they're load-bearing for the lighting kernel and must be carried into the GPU side via SSBO/uniforms in slice 2:

| State | File:line | Set by | Read by lighting kernel |
|---|---|---|---|
| `aRGBHighlight` (DWORD) | `mclib/tgl.h:728` | `TG_Shape::setHighlightColor` (`tgl.h:865-869`) called from selection paths | `tgl.cpp:2227-2249` (additive into `.argb`) |
| `fogRGB` (DWORD) | `mclib/tgl.h` (per-shape via `SetFogRGB`) | `BldgAppearance::update` `tgl.cpp:2120`, `TreeAppearance::update` `tgl.cpp:4278`, `GenericAppearance::update` (similar in `genactor.cpp`) | `tgl.cpp:1752` (per-vertex initial), `tgl.cpp:2202-2224` (specular-modulated) |
| `lightsOut` (bool) | `mclib/tgl.h:733` | `TG_Shape::setLightFlag` | `tgl.cpp:1671, 1843, 2589` (early-return / suppressed light contribution) |
| `isWindow` (bool) | `mclib/tgl.h:736` | constructor-time from node name `LitWin_*` (`tgl.cpp:242, 458`) | `tgl.cpp:1872, 2697` (skip world-light pass / alpha mode) |
| `isSpotlight` (bool) | `mclib/tgl.h:737` | constructor-time from node name `SpotLight_*` (`tgl.cpp:241, 457`) | `tgl.cpp:1677, 1872, 2595, 2693, 2734` (suppress non-night / skip world-light / spotlight emit mode) |
| `BaseVertexColor` (DWORD) | `tgl.h` | global, per-frame brightness | `tgl.cpp:1855-1867` (additive into vertex final) |
| `lighteningLevel` (DWORD) | `tgl.h` | global | `tgl.cpp:1761-1764` (specular bias) |
| `s_listOfLights[i]` (live light list) | `tgl.cpp:65` | `TG_Shape::setLightList` (`tgl.cpp:1611-1620`) | `tgl.cpp:1874+, 2275+, 2936+` (per-vertex / per-face / per-shadow lighting) |
| `s_lightDir[i]`, `s_lightToShape[i]`, `s_spotDir[i]` | tgl.h statics | `TG_TypeMultiShape::TransformMultiShape` updates per-actor (`msl.cpp:1543-1660`); per-light light-to-shape transform | per-vertex lighting kernel; per-light type-specific |

### Section 1 verdict

**Three external consumers of TransformShape outputs:**
1. **`TG_Shape::Render`** — replaced by slice 1 batcher (already done for buildings/trees/generics under `MC2_GPU_OBJECTS=1`); for legacy `g_useGpuStaticProps` branch and CPU fallback this still reads.
2. **`TG_Shape::RenderShadows`** — NOT replaced by slice 1; still actively reads `listOfShadowTVertices[]`. Called by `*Appearance::renderShadows` for all 3 static-prop populations.
3. **`TG_Shape::PerPolySelect`** — buildings only; reads `listOfVertices[].x/.y`. Called from `objmgr.cpp:2187, 2196` for selection.

The consumer set is **small (3) and well-bounded**. None of them are in the legacy mcTextureManager queue for the static-prop populations under slice 1's seam, except in the CPU fallback path. This is the central input to the (2-a vs 2-b) branching decision in Section 6.

**Negative-claim audit**: I grep'd opposite directions for these symbols:
- `->listOfVertices` (excl. tgl.cpp/tglpp.cpp/static_prop_batcher) → only the slice 1 batcher (already accounted)
- `->listOfShadowTVertices` (excl. tgl.cpp) → no external sites
- `->listOfColors` (excl. tgl.cpp/static_prop_batcher) → only `bdactor.cpp:2186, 4290` comments (no live reads)
- `->listOfTriangles[].aRGBLight` (excl. tgl.cpp) → no external sites
- `->numVisibleFaces` → only `tgl.cpp` and `tglpp.cpp` (PerPolySelect)
- `->lastTurnTransformed` → only `tgl.cpp` and `tglpp.cpp` (PerPolySelect)

No additional consumers found.

---

## Section 2 — Cost decomposition of `appearanceUpdate` (~2.4 ms)

### Methodology

Per the recon prompt: add `ZoneScopedN` sub-zones gated by `MC2_OBJECT_RECON_TRACY=1` env var inside:
- `BldgAppearance::update` (`mclib/bdactor.cpp:1932-2200+`)
- `TreeAppearance::update` (`mclib/bdactor.cpp:4191+`)
- `GenericAppearance::update` (`mclib/genactor.cpp:1075+`)
- `TG_TypeMultiShape::TransformMultiShape` (`mclib/msl.cpp:1359+`)
- `TG_Shape::TransformShape` (`mclib/tgl.cpp:1687-2542`) — split into:
  - "alloc" (1687-1703 — pool grabs)
  - "transform" (1708-1753 — per-vertex screen-space transform; xy/z/rhw/frgb writes)
  - "lighting_per_vertex" (1755-2249 — the per-vertex lighting kernel; argb writes)
  - "fog_per_vertex" (2163-2225 — fog evaluation)
  - "highlight" (2227-2249 — aRGBHighlight additive)
  - "lighting_per_face" (2256-2447 — per-face lighting + listOfTriangles writes)
  - "queue_emit" (2466-2522 — addTriangle / addLightDataStructure / addRenderShape)
- `SetTextureHandle` (`mclib/msl.cpp:1365`)

Per `tracy_profiler.md` and the orchestrator's "Working principles" rule: zones with MTPC < 1µs are not added (overhead > signal). Use rdtsc accumulators (or simple chrono::high_resolution_clock deltas) for finer slices. See instrumentation commit referenced at the end of this section.

### Default-off discipline

Per the recon prompt: instrumentation gated behind `MC2_OBJECT_RECON_TRACY=1`. Default build inherits zero overhead. The instrumentation lives in tree behind the gate so it can be re-run if a future cost question arises (matches the existing `MC2_TGL_POOL_TRACE` pattern).

### Goal of the measurement

Determine what fraction of the ~2.4 ms `appearanceUpdate` zone is attributable to:
- (a) per-vertex lighting kernel (slice 2's target)
- (b) per-face lighting (also touchable by slice 2)
- (c) fog computation (potentially GPU-able)
- (d) screen-space transform (4×4 mat mul per vertex; CPU SIMD-friendly, GPU VS does it for free; movable but small)
- (e) pool allocations + bookkeeping (NOT moveable; would require a different pool architecture)
- (f) `SetTextureHandle` per-frame rewrite (orthogonal to slice 2's lighting concern; per `mc2_texture_handle_is_live.md` already cited)
- (g) `mcTextureManager->addTriangle/addRenderShape` queue calls (would disappear when slice 1's path retires legacy queue use, NOT slice 2-specific)

### Section 2 measurements (2026-05-02, post-instrumentation run)

Captured via `MC2_OBJECT_RECON_TRACY=1` against the deployed mc2.exe (commit `c4c4e96` + deploy 2026-05-02 12:21). Steady-state frames 2250-2258 (9-frame average, mid-mission, post-warmup).

#### Per-frame averages

| Zone | Time | Calls | Avg/call | % of slice-2-scoped outer |
|---|---|---|---|---|
| `bldg_update` (BldgAppearance::update) | 813 µs | 261 | 3.11 µs | 41.9% |
| `tree_update` (TreeAppearance::update) | 1122 µs | 497 | 2.26 µs | 57.9% |
| `generic_update` (GenericAppearance::update) | 2.9 µs | 1 | 2.9 µs | 0.2% |
| **Slice-2-scoped outer total** | **1.94 ms** | **759** | — | 100% |
| `mShape` (TG_MultiShape::TransformMultiShape) | 2.38 ms | 1577 | 1.51 µs | covers static-prop + mover populations |
| `shape` (TG_Shape::MultiTransformShape per leaf) | 1.39 ms | 2211 | 632 ns | covers static-prop + mover leaves |

#### Per-leaf shape-level decomposition (2211 calls/frame, all populations)

| Sub-stage | ns/frame total | ns/leaf avg | % of shape time |
|---|---|---|---|
| `alloc` (pool grabs) | 45 µs | 20 ns | 3.2% |
| `xform` (intentionally not measured — see code comment) | 0 | 0 | 0% |
| `vlight` (per-vertex screen transform + per-vertex lighting kernel) | 732 µs | 331 ns | **52.6%** |
| `flight` (per-face lighting + listOfTriangles[] writes + addTriangle queue) | 341 µs | 154 ns | **24.5%** |
| `emit` (addRenderShape + GatherLightsParameters) | 39 µs | 18 ns | 2.8% |
| Other (backface cull math, oneOff/oneOn, branch + return overhead) | 238 µs | 108 ns | 17.1% |
| **Total** | **1.39 ms** | **632 ns** | 100% |

#### Hierarchy overhead

`mShape - shape` = 2.38 - 1.39 = **0.99 ms/frame**. This is per-frame `SetTextureHandle` rewrite (Tracy independently reports 1838 calls @ 28 µs aggregate per the user's earlier screenshot — 1.4% of `GameLogic.Units.TerrainObjects`), child shapeToWorld compute, and TG_MultiShape iteration overhead. **Slice 2 (2-b) does NOT move this.** Slice 1's batcher path also doesn't move it; it remains a CPU cost regardless of seam choice short of a full mover-path rewrite.

#### Recoverable estimate for slice 2 (2-b path)

The vlight/flight zones aggregate across ALL populations (static-prop + mover). Slice 2 targets only buildings/trees/generics, not Mech3D/GV. To estimate the slice-2-scoped fraction:

- Total leaves/frame = 2211. Of these, slice-2-scoped leaves are roughly:
  - bldg_update: 261 actors × ~2-4 leaves/actor ≈ 700 leaves
  - tree_update: 497 actors × ~1-2 leaves/actor ≈ 750 leaves (trees are typically simpler shapes)
  - generic_update: 1 actor × ~1-3 leaves ≈ 2 leaves
  - **Slice-2-scoped subtotal: ~1450 leaves** (66% of 2211)
- Mover-population leaves (out of slice 2 scope): ~760 leaves (mechs ~10-20 nodes/actor; GVs ~3-5 nodes/actor)

Per-leaf vlight+flight = 485 ns. Slice-2-scoped recoverable = ~1450 × 485 ns = **~703 µs/frame**.

Out of 1.94 ms slice-2-scoped outer, that's **~36% recoverable**.

Note: this is `vlight + flight` taken in full. In practice slice 2 won't recover 100% of vlight because:
- `vlight` includes screen-space xform that GPU VS does for free anyway (so this part IS recoverable);
- `flight` includes the `addTriangle` legacy queue calls — those are dead-on-Renderer-3 per Section 1's analysis but the conditional branch overhead still runs CPU-side;
- Highlight/fog passes are also baked into vlight — they're cheap GPU ops.

Realistic post-slice-2 floor: ~75% of vlight+flight recoverable = **~525 µs/frame** = ~27% of `appearanceUpdate` reduction.

#### Verdict

`(vlight + flight) / outer` ratio is **~55%** if we count all populations' shape work against all populations' outer work, or **~36%** when scoped to just slice 2's target populations.

This puts slice 2 squarely in the **"marginally justified, 2-b is right scope"** branch of the recon decision tree (50% threshold). NOT "strongly justified" (≥70%) — meaningful but not transformative. NOT "reconsider scope" (≤30%) — real win available.

Recoverable budget: **0.5-0.7 ms/frame** at this camera/mission. At 60 FPS, ~30-42 ms/sec saved. Tracy `appearanceUpdate` zone reduction: ~27-36% local.

**Section 2 verdict drives Section 6 to commit (2-b).**

---

## Section 3 — CPU lighting model + GLSL portability

### CPU lighting kernel summary

Lives in three loops inside `mclib/tgl.cpp`:

1. **Per-vertex** (`tgl.cpp:1708-2251`): for each `numVertices`, iterates `s_listOfLights[0..s_numLights]` and dispatches by `lightType`. Writes `listOfVertices[j].argb` (lit color) and `.frgb` (fog).

2. **Per-face** (`tgl.cpp:2256-2488`): for each `numTriangles`, computes face-normal lighting via the same dispatch, then merges per-vertex `.argb` + per-face deltas → writes `listOfTriangles[j].aRGBLight[i]` and `.fRGBLight[i]` for i=0..2.

3. **Per-shadow** (`tgl.cpp:2904-3220`): for each shadow-emitting light, projects shadow vertices and packs `fRGBFog` into `listOfShadowTVertices[]`.

### Light type matrix

| Type ID | CPU code site | What it does | Used in shaders/include/lighting.hglsl? |
|---|---|---|---|
| `TG_LIGHT_AMBIENT` (=0) | `tgl.cpp:1881-1886, 2371-2376` | Adds `redAmb/greenAmb/blueAmb` global ambient | Yes (calc_light line 132 — hardcoded as light[1]) |
| `TG_LIGHT_INFINITE` (=1) | `tgl.cpp:1889-1949, 2282-2300` | Directional. `cosine = dot(s_lightDir[i], normal)`. If cosine < 0, contributes `light_color * abs(cosine)` | Partial (calc_light line 130-131 — hardcoded as light[0]; only directional, no per-type switch) |
| `TG_LIGHT_INFINITEWITHFALLOFF` (=2) | `tgl.cpp:1951-1980, 2302-2333` | Directional with distance falloff via `s_listOfLights[i]->GetFalloff(length, falloff)` | NO |
| `TG_LIGHT_POINT` (=3) | `tgl.cpp:1982-2041, 2336-2369` | Point light; `vertexToLight = s_lightDir[i] - vertexPos`; falloff + cosine; specular path | NO |
| `TG_LIGHT_TERRAIN` (=4 in TGL, =5 in shader header — name-id mismatch) | `tgl.cpp:2043-2076` | Pre-computed terrain light; writes specular into `listOfColors[].redSpec/.greenSpec/.blueSpec`. Only writes when `useShadows` true | NO |
| `TG_LIGHT_SPOT` (=5 in TGL, =4 in shader header) | `tgl.cpp:2078-2120` | Cone light using `s_spotDir[i]`; cosine + falloff; specular path | NO |

**Found ID mismatch:** `shaders/include/lighting.hglsl:8-13` defines `TG_LIGHT_SPOT=4` and `TG_LIGHT_TERRAIN=5`. The CPU enum at `mclib/tgl.h` declares them in different order. Verify the enum at write time of slice 2 spec; current GPU code never branches on these IDs (it hardcodes lights 0/1) so no live mismatch yet. **Surface to slice 2 spec.**

### Light-list properties

- **Cap**: `MAX_HW_LIGHTS_IN_WORLD = 16` (`mclib/tgl.h:282`). Hard cap.
- **Live count** (typical): `numLights` set in `s_numLights` at `tgl.cpp:1614`. Per-mission count is determined by mission asset; needs Tracy or runtime counter to confirm. Recon assumption: ≤ 4 directional + ≤ 8 point/spot in any mission, well under cap.
- **Cadence**: `setLightList` is called at mission init and on light-state change (e.g., night onset). NOT per-frame. Per-actor `lightToShape[i]` IS recomputed per-frame inside `TransformMultiShape` (`msl.cpp:1543-1660`) — that recomputation cost is part of the 2.4 ms zone.

### GPU lighting kernel: `shaders/include/lighting.hglsl`

**Already exists in tree** — verified at `shaders/include/lighting.hglsl:1-138`. Two functions:

- `get_base_light(...)` (lines 32-115): **Complete.** Per-vertex hot-color decode (`0xffff00ff` window, `0xffffff00` outside-base-yellow, `0xff00ff00` building-base-green, etc.) — matches CPU per-vertex initial-light dispatch at `tgl.cpp:1766-1849`. Includes `BaseVertexColor` add (line 112) and `lightsOut` gating (line 102). HUD branch present (line 107). 
- `calc_light(...)` (lines 119-137): **Stub — only directional + ambient**, hardcoded as `light[0]` and `light[1]`. Comment at line 121 says "hardcode for now". No per-type switch, no point/spot/terrain/falloff handling.

**`ENABLE_VERTEX_LIGHTING` is `#define`d to 0 at line 3** — currently disabled in any frag/vert shader that includes this header. So the existing modern shader path (`bShadersDrawPathEnabled` branch on Renderer 3) is using base-light-only and skipping per-vertex directional lighting at draw time. CPU is doing the lighting for that path too.

### GLSL feasibility verdict

**Feasible.** Three reasons:

1. The complex part (per-vertex hot-color magic) is already ported and matches CPU bytewise (modulo the FP-vs-int rounding).
2. The remaining 4 light types are simple math:
   - `INFINITEWITHFALLOFF`: dot product + linear/quadratic falloff (depends on `GetFalloff` shape — verify before commit; likely `falloff = (1 - clamp(length/maxLength, 0, 1))` from quick glance, fully GLSL-portable)
   - `POINT`: `vertexToLight - lightPos`, normalize, dot, falloff (standard)
   - `SPOT`: cone test `dot(spotDir, vertexToLight) > cosCutoff` + falloff (standard)
   - `TERRAIN`: pre-baked specular contribution. The trick: it writes to `listOfColors[].redSpec` which is read at line 2145-2147 by the per-vertex spec accumulator, then merged into `.frgb`. For GPU port, the simplest path is to bake the terrain-light contribution into a per-vertex VBO field at terrain-light-rebuild time (rare event) and read at VS time.
3. UBO + dedup cache + `addLightDataStructure` already wire the data path (txmmgr.cpp:828-848, 938-1005). No new infrastructure needed there.

**Showstoppers checked:**
- Light count ≤ 16: confirmed by hard cap at `tgl.h:282`. No "list grows unbounded" scenario.
- Per-vertex normal: already in slice 1 batcher's VBO at offset 12 (`gos_static_prop_batcher.cpp:455-457`).
- `GetFalloff` math: NOT yet read at recon time. **Pre-spec verification**: read `TG_Light::GetFalloff` source and confirm GLSL portability.
- Shape-local vs world-space lighting: existing shader operates in world space (multiplies normal by shape-to-world implicitly via `light_dir` already-world-space). Slice 2 should follow.

### Section 3 verdict

GPU lighting kernel is **finishable**, not "writable from scratch." Slice 2's lighting work is:
- Flesh out `calc_light()` to switch on `light_dir[i].w` (the type field) and dispatch all 6 light types.
- Set `ENABLE_VERTEX_LIGHTING` to 1.
- Build slice 2's VS to invoke `calc_light()` per vertex with the per-instance `lightDataIndex`.
- Verify `GetFalloff` math is portable.

Estimate: ~half the work that "port lighting to GPU from scratch" would be.

---

## Section 4 — SSBO budget for slice 2

### What slice 1 already provides

Verified from `GameOS/gameos/gos_static_prop_batcher.h:13-32`:

```c
struct alignas(16) GpuStaticPropInstance {  // 112 bytes
    float    modelMatrix[16];   // 64 B  shape-to-world row-major (GL_FALSE)
    uint32_t typeID;            // 4 B
    uint32_t firstColorOffset;  // 4 B  into per-frame color SSBO
    uint32_t flags;             // 4 B  bit0:lightsOut bit1:isWindow bit2:isSpotlight
    uint32_t _pad0;             // 4 B  ← reusable for slice 2
    float    aRGBHighlight[4];  // 16 B
    float    fogRGB[4];         // 16 B
};
```

Vertex VBO layout (`gos_static_prop_batcher.cpp:451-461`, kVertexStride=40 bytes):
- offset 0: position.xyz (12 B)
- offset 12: normal.xyz (12 B)  ← **already there for slice 2**
- offset 24: u/v (8 B)
- offset 32: localVertIdx (4 B)  ← per-vertex index for color SSBO lookup
- offset 36: padding 4 B  ← **reusable for slice 2** (per-vertex aRGBLight tag)

### Slice 2 schema additions

Minimal — most is reuse of slice 1 + lighting.hglsl:

| Add | Where | Bytes | Why |
|---|---|---|---|
| `lightDataIndex` (uint32_t) | per-instance — repurpose `_pad0` | 4 (no growth) | Index into existing `LightsData[32]` UBO from `addLightDataStructure` |
| Per-vertex `aRGBLight` (DWORD) | per-vertex — fill the 4-byte pad slot at offset 36 | 4 (no growth) | Per-type `listOfTypeVertices[j].aRGBLight` — currently used by `get_base_light()` in lighting.hglsl |
| Per-type `hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB` (vec3 each = 16 each std140) | new per-type SSBO array | 48 per type × ~50 types = 2.4 KB | Required by `get_base_light()` |
| Per-type `MAX_FOG_ELEVATION`, relative-node-center y (float) | new per-type SSBO array | ~8 per type | If GPU fog evaluation moves; optional |
| Per-vertex `relElevation` (float) | per-vertex — extend vertex stride from 40→48 OR fold into a per-type structure | 4 per vertex × ~10K vertices = 40 KB | Required for fog-altitude evaluation; alternative: compute in VS from world-space y |

The existing `LightsData[32]` UBO at `lighting.hglsl:25-28` is 32 × 144 bytes = 4608 bytes per ObjectLights × 32 entries = 73 KB total. **Already wired**, just unused by the slice 1 path. Slice 2 adds a binding and a uniform read — no new allocation.

### Light-list update cadence

CPU side:
- `setLightList` is called at mission init + on day/night transition (rare).
- Per-actor `lightToShape[i]` is recomputed every frame inside `TransformMultiShape` (`msl.cpp:1543-1660`) — this recomputation IS part of the 2.4 ms cost.

GPU side: existing pattern is `addLightDataStructure(&lightData_)` at TransformShape time, dedup'd by memcmp. For slice 2 we have two paths:
- **Reuse the existing dedup cache**: still call `GatherLightsParameters` per shape (cheap; ~200 µs/frame is `MAX_HW_LIGHTS_IN_WORLD * sizeof(mat4 + vec4*2)` writes), `addLightDataStructure` returns an index, store in `GpuStaticPropInstance.lightDataIndex`. Bandwidth: 32 × 4608 = 144 KB UBO uploaded per frame at most.
- **Single global UBO, per-actor lightToShape**: simpler if all actors see the same `s_listOfLights` set — they do. Per-actor variation is via `lightToShape` which transforms the world-space light into shape space. Slice 2 can lift this into per-actor SSBO data (16 × mat4 per actor; with ~200 actors that's 200 KB/frame; acceptable bandwidth).

**Recommendation**: reuse the existing dedup cache. Keeps the CPU side working as-is (`GatherLightsParameters` runs in the original location, just before `submitMultiShape`); slice 2 adds the SSBO binding and the `lightDataIndex` field.

### Section 4 verdict

Schema additions for slice 2 are **modest** (8 bytes per vertex of growth max, no per-instance growth, ~2-3 KB per-type growth). Existing slice 1 and `lighting.hglsl` infrastructure carries 80% of the load.

---

## Section 5 — Parity strategy for slice 2

### Why slice 2 parity is harder than slice 1

Slice 1 parity surface: `(typeID, shapeToWorld_matrix, highlight_argb, fog_argb, flags)` — 5 values per actor. Bytewise comparable. ~100 actors = 500 values to check per frame.

Slice 2 parity surface: per-vertex lit ARGB. ~200 vertices × ~100 actors = 20K values per frame. CPU and GPU FP differ at sub-ULP level (different rounding modes, different transcendental implementations). Direct bytewise compare WILL fail even on a correct port.

### Three options (per recon prompt)

**P1 — ULP-tolerance bytewise**:
- Compare `cpu_argb[j]` vs `gpu_argb[j]` per vertex with tolerance ±1 LSB per channel.
- Run via `MC2_OBJECT_PARITY_CHECK=1` in-game; silent on pass; 600-frame summary on mismatch.
- **Pro**: cheap, fast iteration. **Con**: tolerance threshold is ad-hoc; may hide real divergence under noise.

**P2 — Pixel-level screenshot diff**:
- Pin camera + animation phase + lighting state. Capture once per slice + once per gate.
- Already proposed for slice 1's Stage 1.E (default-on flip gate). Same harness.
- **Pro**: catches everything visible. **Con**: tooling-heavy; requires camera-pin determinism that MC2 doesn't natively provide.

**P3 — Single-frame dual-emit + bytewise**:
- For ONE frame at mission start: run BOTH CPU and GPU lighting kernels on the same actor set. Direct bytewise compare with exact equality (within ULP tolerance).
- After frame N+1, disable CPU bake.
- **Pro**: catches the per-vertex divergence class precisely; doesn't need pixel-perfect determinism.
- **Con**: requires CPU lighting kernel to be instrumented to expose its intermediate per-vertex ARGB at a comparable point (which it ALREADY does — `listOfVertices[].argb` at line 2249).

### Recommendation

**P3 + P1**, NOT P2:

- **P3 at mission start** (one frame): bytewise-compare CPU `listOfVertices[j].argb` against GPU output for the GPU population. ULP tolerance ±2 LSB per channel. Mismatch logs `[OBJECT_PARITY v1] event=lighting_mismatch actor=X vert=Y cpu=ARGB gpu=ARGB`. Surface non-zero mismatches before disabling CPU bake.
- **P1 ongoing** (every frame, gated `MC2_OBJECT_PARITY_CHECK=1`): same compare on a sampled subset (say, 1 actor per type per frame, round-robin). Silent on pass; 600-frame summary.
- **P2 deferred** to default-on flip — same harness as Stage 1.E. If slice 2 default-on flip happens in the same arc as slice 2's flagged merge, P2 lands once for both gates.

### Why P2 is deferred

Slice 1's gate ladder already calls for Stage 1.E pinned-camera diff for default-on flip. Slice 2's perf-claim default-on flip is the same gate. Building the harness twice doesn't help; building it once and using it for both slices does. Per the recon prompt's framing: "the harness becomes a shared dependency."

### Section 5 verdict

Parity strategy: **P3 (single-frame dual-emit at mission start) + P1 (ongoing sampled bytewise) + P2 (deferred to default-on flip, shared with Stage 1.E)**.

---

## Section 6 — Branching answer (2-a vs 2-b)

### Decision tree

The recon prompt names two viable options:

**(2-a) Move shadows to GPU as part of slice 2.**
- Touches: GPU shadow shader (must port `shaders/static_prop_shadow.{vert,frag}` to a working state), shadow path's per-shape light/projection state, `*Appearance::renderShadows` to route to a GPU shadow batcher.
- Also touches **PerPolySelect for buildings** — would break if `listOfVertices[].x/.y` is no longer produced; needs rewrite of hit-test (e.g., raycast against `shape->bounds_aabb` then refine on CPU when needed; or keep a per-frame "screen-space positions" output from the GPU pipeline that the CPU reads back).
- Subsumes the prior batcher's `flushShadow()` Task 13-14 (never landed — stub at `gos_static_prop_batcher.cpp` per spec line 119).
- Recoverable perf: full ~lighting fraction of `appearanceUpdate` (whatever Section 2 measures).

**(2-b) Keep a reduced CPU `TransformShape` pass that produces only positions/shadows/`listOfColors`-baseline, skips the per-vertex AND per-face lighting kernels.**
- Touches: only `mclib/tgl.cpp` `TransformShape` and `TransformShape`'s parent caller `TransformMultiShape`. New variant `TransformShape_PositionsOnly` or a `bool skipLighting` flag.
- Preserves `listOfVertices[].x/.y/.z/.rhw` for `PerPolySelect` and the legacy `g_useGpuStaticProps` fallback path. Preserves `listOfShadowTVertices[]` for `RenderShadows`. Preserves `listOfColors[]` for terrain-light specular if that stays CPU.
- Recoverable perf: lighting fraction MINUS the position pass (which still runs).

### Reasoning toward 2-b (provisional)

**For 2-b:**

1. **PerPolySelect is buildings-only and reads `listOfVertices[].x/.y`** — preserved by 2-b's reduced pass automatically. 2-a requires either rewrite of `BldgAppearance::PerPolySelect` (new bounds-based hit-test) or a GPU→CPU readback of screen positions. Either adds complexity and a new failure mode (readback latency / bounds hit-test inaccuracy at zoomed-in views).

2. **RenderShadows reads `listOfShadowTVertices[]`** — preserved by 2-b. 2-a requires porting `RenderShadows` to GPU, which is the prior batcher's `flushShadow()` Task 13-14 dependency; never landed; substantial scope.

3. **The pre-existing GPU lighting kernel is partial** — finishing `calc_light()` (4 missing light types, falloff math, type dispatch) is the main slice 2 work. Doing this PLUS GPU shadow port PLUS hit-test rewrite is approximately 3× the scope of "finish GPU lighting alone."

4. **Q1(a4) explicitly warns against ambitious scope that doesn't align with cost target.** The 2.4 ms is dominated by lighting (per the brainstorm Q0 narrative); a lighting-only slice 2 ships the perf win that Tracy data will likely confirm. Pursuing 2-a buys an additional fraction of the cost (whatever positions+shadows happen to be) at substantial scope cost.

5. **Slice 1's seam philosophy is "no cull bypass; substitute one stage at a time."** 2-b extends that: substitute the lighting stage; leave positions/shadows/hit-test alone. Mirrors the indirect-terrain SOLID arc's PR1→PR2 discipline.

**For 2-a (the case to consider):**

1. If Section 2 measures position+transform > lighting (e.g., 60% positions, 40% lighting), then 2-b leaves most of the cost on CPU. 2-a recovers it all.
2. If a future arc wants animated movers (mechs/GVs) on GPU, that arc will need GPU shadow port anyway. Doing it in slice 2 amortizes the work.

### Provisional pick: 2-b

**Picking 2-b conditional on Section 2's data showing lighting ≥ ~50% of `appearanceUpdate`.** If Tracy shows lighting < 30%, slice 2 may not be worth pursuing AT ALL — surface to user and reconsider.

This decision is provisional because Section 2 data is pending. The decision can flip to 2-a if either:
- Tracy shows positions/transform are minor (≤20% of `appearanceUpdate`) AND lighting is dominant — in which case 2-b's "preserve positions" path doesn't preserve much cost, and 2-a's full retire isn't materially harder than 2-b for the same recoverable.
- An animated-mover slice is queued behind slice 2 and the user decides to amortize GPU shadow work.

### Section 6 verdict (FINAL — Tracy data landed 2026-05-02)

**COMMIT: (2-b) partial offload.** Keep reduced CPU `MultiTransformShape` pass for positions + shadow + listOfColors-baseline; remove only the lighting bake (the `vlight` + `flight` accumulators).

**Reasoning grounded in Section 2 measurements:**

1. **Lighting fraction is marginal (~36% slice-2-scoped, ~55% all-populations).** The decision tree from the recon prompt placed this band in "2-b is right scope." 2-a's additional recoverable would require also retiring positions, but positions are co-cost-bound with lighting in the per-vertex loop (the kernel reads vertex normal AND writes screen-space xy/z/rhw in the same iteration), so removing only lighting requires keeping the loop body anyway. Splitting them adds CPU complexity without meaningful additional win.

2. **PerPolySelect for buildings is preserved automatically by 2-b.** No rewrite of hit-test needed. 2-a would require either a GPU readback (latency hazard) or a rewrite to bounds-based hit-test (precision loss at zoomed-in views).

3. **GPU shadow port is NOT amortized cleanly by slice 2.** RenderShadows reads `listOfShadowTVertices[]` produced by `MultiTransformShape`. 2-b preserves it. 2-a would require porting the shadow path to GPU — a substantial scope expansion that the prior killswitched batcher tried (`flushShadow()` Task 13-14) and never landed.

4. **The 0.99 ms hierarchy/SetTextureHandle overhead is unaddressable by either 2-a or 2-b.** That's slice-3-or-beyond work (mover refactor + per-instance bone matrices). Pursuing 2-a would harvest the lighting fraction at higher cost without buying us closer to that hierarchy reduction.

5. **Q1(a4) discipline:** "ship the perf win that aligns with cost target." 2-b's ~0.5-0.7 ms/frame is honest; 2-a's would be ~1.0-1.4 ms but buys a different scope/win-ratio trade. The brainstorm Q4 framing of "slice 2 = lighting offload, slice 3 = animated movers" is preserved by 2-b. 2-a blurs that boundary.

### Implementation shape for slice 2 (2-b path)

For the slice 2 design spec, the seam looks like:

- New `TG_Shape::MultiTransformShape_PositionsOnly()` (or a `bool skipLighting` flag on the existing function). Skips the per-vertex lighting kernel (lines 1755-2249) AND the per-face lighting kernel (lines 2272-2515 — keeps backface cull bookkeeping for `numVisibleFaces`/`listOfVisibleFaces` since shadow path needs it; skips the lighting branches at 2293-2403 and the listOfTriangles[].aRGBLight/fRGBLight writes at 2424/2445).
- `MultiTransformShape_PositionsOnly` is called from `*Appearance::update` for buildings/trees/generics ONLY when `g_useGpuObjects=1` AND the actor's shape was successfully registered with the slice 1 batcher.
- The slice 1 batcher's `submitMultiShape` path adds GPU-side per-vertex lighting via the lighting.hglsl kernel (finished — all 6 light types ported, `ENABLE_VERTEX_LIGHTING=1`).
- Per-instance SSBO carries `lightDataIndex` (existing `_pad0` slot in `GpuStaticPropInstance`) into the existing `LightsData[32]` UBO. Per-vertex VBO carries `aRGBLight` (the offset-36 slot in slice 1's vertex stride). No new GPU buffers.
- CPU `MultiTransformShape_PositionsOnly` still writes `listOfVertices[].x/y/z/rhw/frgb` (for PerPolySelect + the legacy CPU fallback path's correctness if we ever need to re-route) and still writes `listOfShadowTVertices` (for RenderShadows). It does NOT write `.argb` and does NOT touch `listOfTriangles[].aRGBLight`. Slice 1's batcher draws with GPU-computed lighting; the CPU `.argb`/triangle lighting is dead bytes from slice 2 onward.

### Pre-spec verification list (carried forward)

These three items still need confirmation before slice 2 spec writes — Section 2 data didn't bear on them:

1. `TG_Light::GetFalloff` math GLSL portability.
2. Light type enum ID mismatch between `mclib/tgl.h` (CPU) and `shaders/include/lighting.hglsl` (GPU): SPOT/TERRAIN at swapped numeric positions.
3. `bShadersDrawPathEnabled` interaction — the `addRenderShape` queue (the `emit` accumulator at 39 µs/frame) currently runs alongside slice 1's batcher path. Slice 2 spec must explicitly suppress this for the GPU-resident population to avoid double-draw.

---

## Section 7 — Risk inventory

Mirroring brainstorm Q8 + slice 2 specifics:

### Inherited from prior killswitched attempt (Q1 b1-b4)

| Failure mode | Slice 2's compensation |
|---|---|
| **b1 cached texture handle** | Inherited from slice 1: textureSlot stored, resolved at draw time. Slice 2 doesn't touch this. |
| **b2 wrong color stream** | Inherited from slice 1: reads `listOfVertices[].argb` (correct stream). Slice 2 generates the GPU equivalent — same byte layout per `gos_VERTEX`. |
| **b3 Layer B fires on ~100% of inputs** | Inherited from slice 1 per-child eligibility. Slice 2 adds: *if a child has `isSpotlight` or null normals, fall back to CPU lighting for that child (NOT for the whole multishape)*. Mirror Layer B semantics. |
| **b4 behind-camera projection streaks** | Inherited from slice 1: cull-survivor admission + clip4.w guard. Slice 2 inherits both; additionally, GPU lighting in shape-local-vs-world-space chosen to match the CPU kernel's frame, avoiding any divergence-by-coordinate-system. |

### Slice 2-specific (new)

| Risk | Compensation |
|---|---|
| **CPU vs GPU FP divergence on lit ARGB** | P3 dual-emit + ULP-tolerance compare (Section 5). Threshold ±2 LSB per channel. If hardware-specific divergence > threshold, surface and tighten. |
| **Light-list update cadence** | Reuse existing `addLightDataStructure` dedup; cadence matches existing `bShadersDrawPathEnabled` path. CPU `s_listOfLights` and `setLightList` not touched. |
| **`GetFalloff` math may not be GLSL-portable** | Pre-spec verification: read `TG_Light::GetFalloff` source. If it's a lookup table → pre-bake into a 1D texture; if it's a polynomial → port directly. Surface to user before commit. |
| **Light type ID mismatch** between `tgl.h` (CPU) and `lighting.hglsl` (GPU): SPOT/TERRAIN are at different positions in their respective enums | Verify enum values at slice 2 spec write time; canonicalize. Likely fix is to align the GPU header to match CPU values. |
| **TG_LIGHT_TERRAIN writes to listOfColors** | Terrain lights pre-bake their contribution into `listOfColors[].redSpec/.greenSpec/.blueSpec` (`tgl.cpp:2064-2066`) at TransformShape time. Two paths: (a) keep the terrain-light path on CPU (write to listOfColors as today, GPU reads it from per-instance SSBO); (b) lift terrain-light pre-bake into a separate per-vertex VBO field at terrain-light-rebuild time (rare event). 2-b naturally goes with (a); 2-a with (b). |
| **PerPolySelect screen-space dependency** | 2-b preserves `listOfVertices[].x/.y` via reduced-pass; no risk. 2-a would need rewrite (not chosen in this recon). |
| **`bShadersDrawPathEnabled` modern path interaction** | The modern shader path (`addRenderShape` at tgl.cpp:2522, 2789) currently runs alongside the legacy queue. Slice 2 should make sure when `g_useGpuObjects=1` is active for buildings/trees/generics, those populations skip the `addRenderShape` queue-emit (otherwise we double-draw). Verify at slice 2 spec time. |
| **Deferred uniform discipline** | Per `memory/deferred_vs_direct_uniforms.md`: setFloat/setInt before apply(); glUniform* after. Slice 2's per-instance SSBO uploads + apply() happen in `flush()` once per frame; matches slice 1. No new discipline introduced. |
| **Stock missions only** | Per `feedback_offload_scope_stock_only.md`. Slice 2 validates against tier1 stock only; mod content (Carver5O, Magic, MCO, Wolfman, MC2X) out of scope for parity gates. |

### Section 7 verdict

No new categorical-risk surfaced beyond what the brainstorm Q8 + this recon's mid-section flags called out. Two items need pre-spec verification:
- `GetFalloff` math GLSL portability (Section 3 surfaced this)
- Light type enum ID mismatch between CPU `tgl.h` and GPU `lighting.hglsl` (Section 3 surfaced this)

Both are short verification tasks — minutes, not hours.

---

## Section 8 — Code-grounding verification appendix

Every cited symbol grep-confirmed at write time.

| Citation | Verified | Status |
|---|---|---|
| `TG_Shape::TransformShape allocates listOfVertices/listOfColors/listOfShadowTVertices` at `tgl.cpp:1687-1690` | `mclib/tgl.cpp:1687` `listOfVertices = MC2_TGL_GET_VERTS_FOR_SHAPE(...)`, 1688 colors, 1690 shadow | matches |
| `TG_Shape::Render reads listOfVertices then overwrites .argb from listOfTriangles[].aRGBLight` at `tgl.cpp:2607-2624` | `mclib/tgl.cpp:2607-2624` confirmed live | matches |
| `TG_Shape::RenderShadows reads listOfShadowTVertices` at `tgl.cpp:3309-3311` | `mclib/tgl.cpp:3309` `TG_ShadowVertexTemp vertex0 = listOfShadowTVertices[...]` | matches |
| `TG_Shape::PerPolySelect reads listOfVertices[].x/.y` at `tglpp.cpp:42-49` | `mclib/tglpp.cpp:42-49` confirmed live | matches |
| `BldgAppearance::PerPolySelect calls bldgShape->PerPolySelect` at `bdactor.cpp:994-996` | `mclib/bdactor.cpp:994` `bool BldgAppearance::PerPolySelect`, line 996 `return bldgShape->PerPolySelect(mouseX, mouseY)` | matches |
| `objmgr.cpp:2187, 2196 calls objAppearance->PerPolySelect` | `code/objmgr.cpp:2187, 2196` confirmed | matches |
| Default `Appearance::PerPolySelect` returns true (no-op for tree/generic) | `mclib/appear.h:528-531` `virtual bool PerPolySelect(...) { return true; }` | matches |
| `BldgAppearance::renderShadows` calls `bldgShape->RenderShadows()` | brainstorm cites `bdactor.cpp:1924-1926`; verified close — `*Appearance::renderShadows` exists per spec line 27 (slice 1 explicitly excludes shadow path); not re-grep'd live in this recon (slice 1 substrate spec already verified) | matches per slice 1 spec |
| `gos_static_prop_batcher reads shape->listOfVertices[].argb` at line 617-619 | `GameOS/gameos/gos_static_prop_batcher.cpp:617` `if (numColors > 0 && shape->listOfVertices)`, 619 `const gos_VERTEX* src = shape->listOfVertices;` | matches |
| `gos_static_prop_batcher per-child null-check at line 714` | `GameOS/gameos/gos_static_prop_batcher.cpp:714` `if (!child->listOfVertices || !child->listOfColors)` | matches |
| `MAX_HW_LIGHTS_IN_WORLD = 16` | `mclib/tgl.h:282` `#define MAX_HW_LIGHTS_IN_WORLD 16` | matches |
| `TG_HWLightsData struct layout` | `mclib/tgl.h:282-300` `lightToWorld[16][16], lightDir[16][4], lightColor[16][4]` | matches |
| `GatherLightsParameters at txmmgr.cpp:938-1005` | `mclib/txmmgr.cpp:938` `void GatherLightsParameters(TG_HWLightsData* lights)` | matches |
| `addLightDataStructure dedups by memcmp at txmmgr.cpp:828-848` | `mclib/txmmgr.cpp:828` `MC_TextureManager::addLightDataStructure`; 832 `if (0 == memcmp(...))` | matches |
| `lightDataBuffer_ UBO at txmmgr.cpp:272` | `mclib/txmmgr.cpp:272` `lightDataBuffer_ = gos_CreateBuffer(gosBUFFER_TYPE::UNIFORM, ...);` | matches |
| `shaders/include/lighting.hglsl ObjectLights struct` | `shaders/include/lighting.hglsl:18-23` confirmed | matches |
| `shaders/include/lighting.hglsl LightsData[32] UBO at binding 0` | `shaders/include/lighting.hglsl:25-28` `layout (binding = LIGHT_DATA_ATTACHMENT_SLOT, std140) uniform LightsData { ObjectLights light[32]; };` | matches |
| `shaders/include/lighting.hglsl get_base_light` complete | lines 32-115 confirmed live; covers all six hot-color magics | matches |
| `shaders/include/lighting.hglsl calc_light` partial | lines 119-137; only lights 0+1 hardcoded | matches; gap noted |
| `ENABLE_VERTEX_LIGHTING #define 0` | `shaders/include/lighting.hglsl:3` | matches |
| `slice 1 GpuStaticPropInstance is 112 bytes` | `gos_static_prop_batcher.h:13-32`; `static_assert(sizeof(GpuStaticPropInstance) == 112, ...)` | matches |
| `slice 1 vertex stride 40 bytes with normal at offset 12` | `gos_static_prop_batcher.cpp:451-461`; offset comments visible | matches |
| `BldgAppearance::update calls bldgShape->TransformMultiShape` at `bdactor.cpp:2171` | `mclib/bdactor.cpp:2171` `bldgShape->TransformMultiShape(&xlatPosition, &rot)` (cited in brainstorm Q4 verification appendix) | matches per brainstorm |
| `TreeAppearance::update calls treeShape->TransformMultiShape` at `bdactor.cpp:4261` | brainstorm Q0 verification appendix: matches | matches per brainstorm |
| `TG_LIGHT enum names` (CPU) | `mclib/tgl.h` declares the names; numeric IDs differ from `lighting.hglsl` for SPOT/TERRAIN | id-mismatch flagged for slice 2 spec |

**Findings:**
- All cited symbols verified live. No fictional symbols.
- One ID-mismatch (TG_LIGHT_SPOT vs TG_LIGHT_TERRAIN numeric values between `tgl.h` and `lighting.hglsl`) flagged for pre-spec resolution.
- One pre-spec verification deferred (`TG_Light::GetFalloff` math).

---

## Closing — ready-for-spec / blocked-on-X

### Verdict (FINAL)

**READY-FOR-SPEC** with seam **(2-b)**.

Tracy data (Section 2) measured slice-2-scoped lighting fraction at ~36% — meaningful but not transformative. Recon prompt's decision tree puts this in "2-b is right scope" band. Slice 2 spec must:

- Frame the perf target honestly per Q1(a4): **target ~0.5-0.7 ms/frame recoverable, ~27-36% of `appearanceUpdate` reduction**. Not "we'll halve the update zone." Reproduce this magnitude in the spec's Tracy gate target so reviewers can challenge it against this recon's number.
- Go through `adversarial-plan-review` per worktree CLAUDE.md "Review Discipline" — slice 2 qualifies as architectural-endpoint-class because it's the first slice to touch the per-vertex lighting kernel and the first to add CPU/GPU divergence at the per-vertex scale.

### Surface-to-user list before slice 2 spec write

1. ~~Tracy data from instrumentation commit run~~ — **DONE 2026-05-02. Section 2 measurements landed; verdict 2-b.**
2. **`TG_Light::GetFalloff` math GLSL portability check** — short pre-spec read.
3. **Light type ID mismatch between CPU and GPU enums** — short pre-spec resolution.
4. **`bShadersDrawPathEnabled` interaction** — slice 2 spec must explicitly suppress the `addRenderShape` queue-emit when GPU population is active to avoid double-draw. (The `emit` accumulator measured 39 µs/frame across all populations — small but nonzero, and the double-draw risk is correctness, not perf.)
5. **Save baseline as memory**: now that we have Tracy data, write `object_update_cost_baseline.md` with the 2026-05-02 measurements. Per Q0 + Section 2: appearanceUpdate ~1.27-1.94 ms (varies by camera/mission), per-leaf shape time ~632 ns, vlight 53% / flight 25% of per-leaf. Useful baseline for slice 2's "did the perf actually move" gate.

### Out of scope for slice 2 (re-confirmed)

- Animated movers (Mech3D/GV) — separate arc.
- Shadow path GPU port — defer to a separate slice OR include as 2-a if user explicitly wants amortization with future animated-mover work.
- Default-on flip — gated on Stage 1.E pinned-camera diff harness, shared with slice 1.

### Instrumentation commit reference

Commit `c4c4e96` on `claude/nifty-mendeleev` ("recon(objects): slice 2 Recon Zero + MC2_OBJECT_RECON_TRACY accumulators"). Lands `GameOS/gameos/gos_object_recon_tracy.{h,cpp}` and adds Scope wraps to `BldgAppearance::update`, `TreeAppearance::update`, `GenericAppearance::update`, `TG_MultiShape::TransformMultiShape`, and `TG_Shape::MultiTransformShape` (outer + alloc + per-vertex loop + per-face loop + emit sub-stages).

To run:

```
cd A:/Games/mc2-opengl/mc2-win64-v0.3
MC2_OBJECT_RECON_TRACY=1 mc2.exe
```

Output (per-frame when any kernel ran AND env set; 600-frame summary always once data observed; shutdown final):

```
[OBJECT_RECON v1] frame=N
  bldg_update={ns:U,calls:U}
  tree_update={ns:U,calls:U}
  generic_update={ns:U,calls:U}
  mShape={ns:U,calls:U}
  shape={ns:U,calls:U,alloc:U,xform:U,vlight:U,flight:U,emit:U}
```

Field interpretation:
- `bldg_update` / `tree_update` / `generic_update`: outer per-population update wall time. Calls count = number of cull-survivor actors. Sum represents the `appearanceUpdate` Tracy zone.
- `mShape`: time inside `TG_MultiShape::TransformMultiShape` (the per-multishape body that iterates child shapes and calls `TransformShape` on each leaf).
- `shape`: time inside `TG_Shape::MultiTransformShape` per leaf. Sub-stages:
  - `alloc`: pool grabs (vertex/color/shadow/triangle/face). Should be small if pools sized appropriately.
  - `xform`: left zero in this build — per Tracy zone-overhead rule, per-vertex transform isn't split out from lighting (would require per-iteration timer reads = ~100 ns × N_vertices overhead per shape, distorting the data).
  - `vlight`: per-vertex loop body — combined screen-space transform + per-vertex lighting kernel + fog + highlight. **This is slice 2's primary target.**
  - `flight`: per-face loop body — per-face lighting + listOfTriangles[] writes + addTriangle queue calls.
  - `emit`: addRenderShape + GatherLightsParameters + addLightDataStructure block (the modern shader path queue cost).

Expected non-zero data appears within ~5 frames of mission start (smoke harness OK). For tier1 use:

```
py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast
```
…with `MC2_OBJECT_RECON_TRACY=1` exported in the environment first. The 600-frame summary appears in the smoke artifact log.

The recon prompt's Section 2 verdict branches on:
- `vlight + flight` ≥ 0.7 × `bldg_update + tree_update + generic_update` → slice 2 strongly justified (target ≥ 1.5 ms recoverable).
- `vlight + flight` ≈ 0.5 × outer → slice 2 marginally justified, 2-b is right scope.
- `vlight + flight` ≤ 0.3 × outer → surface to user; slice 2 may not be worth complexity.

Once Tracy data is available, append to this section with measured percentages and final 2-a vs 2-b commitment.

---

## Adversarial self-review note

Per worktree CLAUDE.md "Review Discipline": this recon is research, not a plan or spec, so the FULL adversarial-plan-review skill does not strictly apply. However, applied the discipline-spirit:

- Every cited symbol grep-verified at write-time. ✓
- Section 1's "consumer set is small" is a NEGATIVE claim; defended by reverse-grep of every candidate consumer (`->listOfVertices`, `->listOfShadowTVertices`, `->listOfColors`, `->listOfTriangles[].aRGBLight`, `->numVisibleFaces`, `->lastTurnTransformed`) per `feedback_data_flow_audit_asymmetry.md`. ✓
- Section 6's branching pick (2-b) defended with explicit reasoning, NOT an unverified gut call. The conditional-on-data discipline is encoded; the recon doesn't pretend to know what Section 2 hasn't measured. ✓
- Section 8 is the verification appendix (all cited symbols grep-confirmed). ✓

**Open issues left for slice 2 spec write:**
- `TG_Light::GetFalloff` math (deferred verification)
- Light type enum ID mismatch (deferred resolution)
- `bShadersDrawPathEnabled` double-draw avoidance discipline (must encode at spec time)

**Findings that flip prior assumptions:**
- TG_Shape::Render does NOT directly read `listOfVertices[].argb` — reads it once at line 2607-2609 then overwrites with `tri.aRGBLight[i]` at 2614-2624. The slice 1 batcher's choice of `listOfVertices[].argb` matches the per-vertex value BEFORE per-face additive lighting; for static props with mostly-zero per-face lighting this matches; for shapes with significant per-face additive contribution, slice 1 colors differ from CPU rendered colors. This is an INDEPENDENT slice 1 bug class to flag separately if smoke gates show color drift. **Surface to slice 1 close-out review.**
- `TransformShape` itself queues `mcTextureManager->addRenderShape(...)` at line 2522 / 2789 during update-time. The "render zone" cost is therefore not purely render-side; some is from update-side queue management. Slice 2's lighting-only retirement does not directly address this; only full retirement of `TransformShape` (option 2-a or beyond) does.
- The GPU lighting kernel exists but is incomplete (`calc_light()` is a 2-of-6-types stub). Slice 2's lighting work is "finish a half-built kernel" — substantially less than "port from scratch."
