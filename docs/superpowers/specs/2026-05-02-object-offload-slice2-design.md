# Object Offload — Slice 2 (GPU Vertex Lighting) — Design

Date: 2026-05-02
Worktree: `nifty-mendeleev`
Branch: `claude/nifty-mendeleev`
Author: ThranduilsRing + Claude (Opus 4.7, 1M context)
Brainstorm: [`brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)
Recon Zero: [`explorations/2026-05-02-object-offload-slice2-recon-zero.md`](../explorations/2026-05-02-object-offload-slice2-recon-zero.md)
Slice 1 design: [`specs/2026-05-02-object-offload-slice1-design.md`](2026-05-02-object-offload-slice1-design.md)
Arc: object offload, slice 2 of a 2-slice arc.
Status: **Stages 2.A, 2.B, 2.C COMPLETE behind `MC2_GPU_OBJECTS=1` flag (2026-05-02).** Slice 2 PR-ready checkpoint; Stages 2.D (parity) + 2.E (pinned-camera diff) pending. Step 0 adversarial review applied 2026-05-02 — line citations re-grep'd against current source, fictional cross-references corrected, internal contradictions resolved, architectural decisions locked (see Step 0 Sign-Off Log below). DO NOT redesign during execution.

**Landed commits (in chronological order):**
- `cdcdb7d` — Stage 2.A: substrate edits (no behavior change)
- `bd1bd25` — Stage 2.B: eligibility hoist + late-reg recovery wiring (defensive flag-set; falls through to legacy CPU Render — see late-reg correction in handoff prompt)
- `ad96c1f` — Stage 2.C.1: GLSL kernel + UBO schema lockstep + render-time gather + `TG_Shape::init()` static-state lifecycle fix
- `eb2a837` — Stage 2.C.2: flip static_prop draw to GPU lighting (per-vertex aRGBLight, per-type hot-color SSBO, calc_light invocation)

**Tier1 5/5 PASS in three configs (unset / `MC2_GPU_OBJECTS=1` / `+MC2_OBJBATCHER_TRACE=1`), +0 destroys delta on every mission.**

**Tracy delta carry-forward (advisor 2026-05-02):** smoke-camera Tracy on
mc2_01 default position showed **~15.7% `appearanceUpdate` reduction**,
above the 10% surface-to-user floor (see line 187/188 below) but below the
17% target. The recon's 17-21% prediction was at a building-heavy camera
with 759 actors/frame; smoke runs at default camera with ~4 actors visible.
**Pinned-camera Tracy at the recon-equivalent camera is required for
apples-to-apples validation** and lives at Stage 2.E's harness work.

**Stage 2.D pre-conditions (advisor 2026-05-02):** the two known
unregistered types from slice 1 spec lines 489-490 still hit late-reg
every frame (~3957 events/mission in mc2_01). They render correctly via
legacy CPU Render() but pollute parity sampling. Resolve via allowlist
add or registration-site fix BEFORE 2.D, OR Stage 2.D's parity harness
must explicitly exclude legacy-CPU-fallback actors. Better instrumentation
shipped post-2.C to make this practical (see handoff prompt
"Late-reg type identification").

### Step 0 Sign-Off Log (2026-05-02)

User-confirmed decisions from adversarial review, locked before Stage 2.A edits:

1. **calc_light() signature change is explicit Stage 2.C deliverable**: `calc_light(int lights_index, vec3 normal, vec3 vertex_world_pos, vec3 base_light)` — adding `vertex_world_pos` is unavoidable for POINT/SPOT/INFINITEWITHFALLOFF distance/falloff. All callers updated lockstep.
2. **`GatherGpuObjectLightDataOnly()` is per-actor**: hoisted OUT of the per-leaf loop in `submitMultiShape` (call site between ~694 and ~696, BEFORE the `for (int i = 0; i < n; ++i)` at 698). Returned index broadcast into each leaf's per-instance struct inside the loop body.
3. **Eligibility hoist lives INSIDE the existing cull gate**: at every site (`BldgAppearance::update`, `TreeAppearance::update`, `GenericAppearance::update`), the `g_useGpuObjects`/positions-only branch lives inside the existing `if (inView || g_useGpuStaticProps)` gate. The shadow-shape companion calls (`bldgShadowShape->TransformMultiShape`, `treeShadowShape->TransformMultiShape`) are NOT touched by slice 2.
4. **Late-registration "≤ 2 events per mission" claim removed**: was sourced from a fictional slice-1 line-86 reference. Replaced with empirical bounding via the new `late_register_recovery_skips` Gate F counter — observed during smoke, widened if higher.
5. **`needsFullBakeNextFrame` is a NEW `bool` on each appearance class**: NOT a packed bit into a fictional `appearanceFlags` byte. The spec previously implied an existing `appearanceFlags` aggregator; grep returned zero hits, so the spec adds an explicit new `bool needsFullBakeNextFrame` member to `BldgAppearance`, `TreeAppearance`, `GenericAppearance`.

6. **Stage 2.A must not change GPU-visible buffer layouts** (regression-discovered 2026-05-02). The legacy `addRenderShape` path (`bShadersDrawPathEnabled` block at `tgl.cpp:2522`) is active in stock and uploads `TG_HWLightsData` to the `LightsData` UBO declared in `lighting.hglsl:25-28` as `ObjectLights light[32]`. The C++ struct size and the GLSL `ObjectLights` size MUST match byte-for-byte at every stage boundary. Extending `TG_HWLightsData` without the lockstep GLSL change broke per-element stride for `light[i]` with `i>0` and crashed mc2_24 specifically (the airbase has many distinct light setups). **Falloff fields (`closeDistance`/`farDistance`/`oneOverDistance`) belong to Stage 2.C, where the C++ struct, the GLSL `ObjectLights` struct, and the new `calc_light()` reader all change in one commit.** Stage 2.A's `TG_HWLightsData` must remain bit-identical to the pre-slice-2 layout. The same rule applies to any other GPU-visible struct (`GpuStaticPropInstance` is allowed because `_pad0`→`lightDataIndex` is a name-only rename at offset 76; the size and layout do not change).

## Slice scope (single sentence)

Replace the per-vertex CPU lighting bake (the lighting subset of the per-vertex loop body inside `TG_Shape::MultiTransformShape`; Tracy zone `vlight` opens at `mclib/tgl.cpp:1726`, the per-vertex `for` loop spans `1728-2272`, with transform code in the early portion of the loop body and lighting/hot-color/highlight code from roughly line 1820 onward) with GPU vertex-shader lighting for the static-prop populations already wired to slice 1's batcher (buildings + trees + generics under `MC2_GPU_OBJECTS=1`), keeping a reduced CPU pass for screen-space positions, shadow-projection input, and `PerPolySelect` hit-test.

## What slice 2 explicitly does NOT do

- Move CPU `MultiTransformShape` entirely to GPU. Positions, shadow projection, `numVisibleFaces`/`listOfVisibleFaces`, and per-shape state (lightsOut, isWindow, isSpotlight) continue to flow through the reduced CPU pass.
- Touch the legacy CPU shadow path. `bldgShape->RenderShadows()` and analogues continue to consume `listOfShadowTVertices[]`. Shadows for static props are slice-3-or-beyond work (out of arc).
- Touch animated mover populations (Mech3DAppearance, GVAppearance). Out of arc.
- Implement per-face additive lighting on GPU. Per-face kernel is dead code in stock (`useFaceLighting=false` permanently — see Recon Section 9 Correction B). Slice 2 retires the per-face listOfTriangles writes WITHOUT introducing a GPU equivalent because there is no visible feature to preserve.
- Bypass any cull infrastructure. `inView`, `canBeSeen`, `objBlockInfo.active`, `objVertexActive` remain authoritative.
- Default-on flip. Slice 2 ships behind `MC2_GPU_OBJECTS=1` (default off) and stays flagged until Stage 1.E pinned-camera screenshot diff harness clears.

## Problem statement

Per Recon Zero Section 2 (Tracy data, 2026-05-02):
- `appearanceUpdate` for slice-2-scoped populations: ~1.94 ms/frame (bldg 813 µs + tree 1122 µs + generic 3 µs at the recon's camera/mission).
- Per-leaf shape time: 632 ns avg, with `vlight` (per-vertex transform + lighting) at 52.6% and `flight` (per-face lighting + queue emit) at 24.5%.
- Hierarchy/SetTextureHandle overhead: 0.99 ms/frame, NOT addressable by slice 2.

**Recoverable target**: ~330-407 µs/frame slice-2-scoped, **~17-21% `appearanceUpdate` reduction**. Honest framing per brainstorm Q1(a4).

## Architecture

### Seam: 2-b partial offload

Slice 2 splits `TG_Shape::MultiTransformShape` into two CPU functions:

1. **`MultiTransformShape_PositionsOnly`** (NEW) — the reduced CPU pass. Runs for GPU-eligible populations under `g_useGpuObjects=1`. Writes (line numbers verified against tgl.cpp 2026-05-02):
   - `listOfVertices[j].x/y/z/rhw/frgb` (screen-space positions for `PerPolySelect` + legacy CPU fallback)
   - `listOfShadowTVertices[]` (shadow projections for legacy CPU shadow path)
   - `numVisibleFaces`, `listOfVisibleFaces[]` (backface cull bookkeeping; the populating loop is at `tgl.cpp:2284-2288` inside the per-face flight scope)
   - `lastTurnTransformed = turn`
   - **Pool allocations for `listOfColors`, `listOfTriangles`, `listOfVisibleShadows` MUST remain non-null** even though their CONTENTS may be stale. `tglpp.cpp:14-21` (PerPolySelect) early-outs unless ALL of `listOfVertices`, `listOfColors`, `listOfShadowTVertices`, `listOfTriangles`, `listOfVisibleFaces`, `listOfVisibleShadows`, `lastTurnTransformed != (turn-1)` are non-null/satisfied. A literal "strip" without guarding allocation can cause silent PerPolySelect early-outs.

   Skips (the lighting/queue subset of the loop bodies; semantic targets, exact micro-ranges to be confirmed at edit-time against current tgl.cpp):
   - per-vertex lighting kernel — the lighting/hot-color subset of the `vlight` per-vertex loop body, roughly from line ~1820 (first hot-color/lighting branch after parallel-transform setup) through line 2246 (just before the `aRGBHighlight` block). Transform-only code earlier in the loop body MUST stay.
   - `aRGBHighlight` additive at `tgl.cpp:2247-2270` (the `if (aRGBHighlight)` block)
   - per-face flight scope at `tgl.cpp:2274-2515` — but KEEPS the backface cull at 2284-2288 because it populates `listOfVisibleFaces`
   - `listOfTriangles[j].aRGBLight[i]` write at `tgl.cpp:2450` and `listOfTriangles[j].fRGBLight[i]` write at `tgl.cpp:2471` — dead code in stock; removed (note: the surrounding code includes `if (greenSpec > 255)` clamps at 2424 and `if (gFinal > 255)` clamps at 2445; do not confuse those with the writes)
   - `addTriangle` queue calls at `tgl.cpp:2492, 2496, 2505, 2509` — dead-on-Renderer-3
   - `addRenderShape` block at `tgl.cpp:2522-2555` (the `bShadersDrawPathEnabled` `if` head opens at 2522, the `addRenderShape` call inside is at 2553, the closing brace is at 2555) — gated to legacy path only

2. **`MultiTransformShape`** (UNCHANGED) — full lighting bake. Continues to run for non-GPU-eligible cases: legacy fallback path, late-registration recovery, mover populations.

### GPU vertex lighting

Finishes the half-built kernel in `shaders/include/lighting.hglsl`:

- `get_base_light()` (lines 32-115) is already complete. No change.
- `calc_light()` (lines 119-137) is currently a 2-of-6-light-types stub with hardcoded `light[0]=directional`, `light[1]=ambient`, and an early-return `if (numLights.x == 0) return vec3(1)` at line ~127-128. Slice 2 ports the remaining 4 types: INFINITEWITHFALLOFF, POINT, SPOT, TERRAIN. Switch on `light_dir[i].w` (the type field — already populated by `GatherLightsParameters` at `txmmgr.cpp:974` via `lights->lightDir[num_lights][3] = (float)type;`). **The `numLights.x == 0` early-return MUST be preserved** (other shader translation units include this header and may not be on the slice 2 path).
- **`calc_light()` signature change is an explicit Stage 2.C deliverable**: current signature is `vec3 calc_light(in int lights_index, in vec3 normal, in vec3 base_light)` (3 params). Slice 2 changes it to `vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light)` (4 params). The new `vertex_world_pos` is required for POINT/SPOT/INFINITEWITHFALLOFF distance/falloff computation. Every existing caller of `calc_light()` (currently only the stub paths in `lighting.hglsl` itself) MUST be updated in lockstep; greps for `calc_light(` pre-edit and post-edit must return identical caller counts (only the signatures differ).
- **C++/GLSL name divergence at the struct boundary**: the C++ struct is `TG_HWLightsData` (`tgl.h:284-298`) with `lightDir[16][4]` (camelCase). The GLSL struct is `ObjectLights` (`lighting.hglsl:18-23`) with `vec4 light_dir[MAX_LIGHTS_IN_WORLD]` (snake_case). Layouts match byte-for-byte; only the names differ. Spec text uses the GLSL form (`light_dir[i].w`) when discussing shader logic and the C++ form (`lightDir[N][3]`) when discussing CPU-side population.
- `GetFalloff` math (linear interpolation per Section 9 Item 1; the existing `TG_Light::GetFalloff` at `tgl.h:261-275` is `falloff = (farDistance - length) * oneOverDistance;`) is added as a GLSL helper. Three new per-light fields in the SSBO: `closeDistance`, `farDistance`, `oneOverDistance`. Pack into `light_color.w` and a new vec4 slot per light, OR extend `TG_HWLightsData` schema (small). The source fields `TG_Light::closeDistance/farDistance/oneOverDistance` exist at `tgl.h:193-195`.
- `ENABLE_VERTEX_LIGHTING` is set to `1` in `lighting.hglsl:3`.

The slice 2 vertex shader (extends `shaders/static_prop.vert`; current file has no `calc_light()` invocation, pulls per-instance argb from `colors_` SSBO — switching to "consume lit ARGB from VS output" is a one-line varying rewire) calls `calc_light()` per vertex with the per-instance `lightDataIndex` and per-vertex `aRGBLight` tag (slice 1 zero-padded the slot; slice 2 writes it at registerType time per the Per-vertex VBO usage table above).

### Side-effect-free light-data gather

New helper `TG_Shape::GatherGpuObjectLightDataOnly()` (Recon Section 9 Item 5):

```cpp
uint32_t TG_Shape::GatherGpuObjectLightDataOnly() {
    GatherLightsParameters(&lightData_);
    return mcTextureManager->addLightDataStructure(&lightData_);
}
```

Declared in `mclib/tgl.h` near `MultiTransformShape` (line 852). Defined in `mclib/tgl.cpp` immediately after `MultiTransformShape`. **Per-actor** (NOT per-leaf) call: invoked **ONCE per multi-shape, hoisted OUTSIDE the per-leaf submit loop in `submitMultiShape`**. Concrete placement: `submitMultiShape` spans `gos_static_prop_batcher.cpp:641-737` and contains **TWO `for (int i = 0; i < n; ++i) {` loops with identical signatures**:
- The **first** at line 667 is the registration-check loop (early-out path; if any leaf type is unregistered, sets late-reg flag + returns false).
- The **second** at line 698 is the per-leaf submit loop where each leaf's per-instance struct is written.

The `GatherGpuObjectLightDataOnly()` call goes BEFORE the **second** loop (between approximately line 694 and line 696, after the registration-check loop has cleared and the eligibility filter has run). The returned `lightDataIndex` is then broadcast into each leaf's per-instance struct INSIDE the second loop's body. (Recon Section 9 Item 5 confirmed all leaves of one multi-shape see identical `lightData_`, which is why per-actor is correct.)

**DO NOT** place this call inside either `for` loop body — placing it inside the registration-check loop at line 667+ would gather lights for unregistered actors that get rejected, and placing it inside the submit loop at line 698+ would make it per-leaf and incur N-fold redundant `GatherLightsParameters` calls per multi-shape per frame. The call sits in the gap BETWEEN the two loops, after registration-check passes.

### Eligibility hoist

New method `GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(const TG_MultiShape* multi) const` per Recon Section 9 Item 4. Mirrors slice 1's render-time per-child gates EXCEPT the late-registration case.

**Critical placement rule (load-bearing — preserves cull invariant)**: the eligibility branch lives **INSIDE** the existing `if (inView || g_useGpuStaticProps)` cull gate at each call site, NOT before it. The legacy `TransformMultiShape` call is already inside this gate at:

- `mclib/bdactor.cpp:2200` — `bldgShape->TransformMultiShape` inside `if (inView || g_useGpuStaticProps)` opening at 2191
- `mclib/bdactor.cpp:4313` — `treeShape->TransformMultiShape` inside `if (inView || g_useGpuStaticProps)` opening at 4300
- `mclib/genactor.cpp:1189` — `genShape->TransformMultiShape` inside `if (inView || g_useGpuStaticProps)` opening at 1185

Each site also has a parallel shadow-shape call (`bldgShadowShape->TransformMultiShape` at `bdactor.cpp:2206`, `treeShadowShape->TransformMultiShape` at `bdactor.cpp:4321`). **Slice 2 leaves these shadow-shape calls untouched** — shadow path is out of arc.

The branch (logical structure; concrete C++ uses the local `bldgShape`/`treeShape`/`genShape` variable per site):

```cpp
// Existing cull gate — DO NOT lift the eligibility branch out of this gate.
if (inView || g_useGpuStaticProps) {
    if (g_useGpuObjects &&
        !needsFullBakeNextFrame &&  // NEW bool member; late-reg recovery clears eligibility for one frame
        GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape)) {
        bldgShape->TransformMultiShape_PositionsOnly(&xlatPosition, &rot);
    } else {
        bldgShape->TransformMultiShape(&xlatPosition, &rot);
        needsFullBakeNextFrame = false;  // recovery complete
    }

    // Shadow-shape call below is UNCHANGED (out of arc):
    // bldgShadowShape->TransformMultiShape(...);  // continues to run regardless of eligibility
}
```

`needsFullBakeNextFrame` is a NEW `bool` member added to `BldgAppearance`, `TreeAppearance`, `GenericAppearance` (see Stage 2.A files). It is NOT packed into a fictional `appearanceFlags` byte — those classes have no such aggregator field; they use individual `bool` members.

### Late-registration recovery

When `submitMultiShape` hits the unregistered-type branch at `gos_static_prop_batcher.cpp:674-693` (the full branch — `if (s_typeIndex.find(ts) == s_typeIndex.end())` opens at 674; the inner `if (count == 0)` print-allowed block runs 683-692; `++count; return false;` tail at ~692-693):

- Set per-actor flag `needsFullBakeNextFrame = true`. **The flag-set MUST dominate the `return false;` at the tail of the branch** — place it before the inner `if (count == 0)` print block (or on every path that exits via `return false;`), NOT inside the print block.
- Skip render for that actor this frame. Increment new F-gate counter `late_register_recovery_skips` (separate from `cpu_fallback_by_pop` to keep fallback ratio clean).

Next frame's update sees the flag, takes the `else` branch (full `TransformMultiShape`), clears the flag. Frame N+2 onward, normal eligibility hoist applies. **Visual impact**: one frame of "actor not rendered" per late-registered actor (typically artillery/bomber spawns; slice 1 documents two known types pending allowlist at slice 1 spec lines 489-490, but does NOT establish a per-mission cap). The acceptable rate is bounded empirically via `late_register_recovery_skips` observed during smoke; if observed rate exceeds expectations (>5 per mission), the eligibility-hoist coverage has a gap and the count must be investigated rather than treated as cosmetic.

### Per-instance SSBO additions

| Field | Type | Where | Purpose |
|---|---|---|---|
| `lightDataIndex` | uint32_t | repurpose `_pad0` in `GpuStaticPropInstance` (gos_static_prop_batcher.h:13-32) | Index into `LightsData[32]` UBO from `addLightDataStructure` |

No struct growth (slot already exists).

### Per-vertex VBO usage

Existing slice 1 vertex layout (`gos_static_prop_batcher.cpp:451-461`, stride `kVertexStride = 40`):

| Offset | Bytes | Field | VS attrib (current) |
|---|---|---|---|
| 0  | 12 | position.xyz (vec3) | `layout(location = 0)` |
| 12 | 12 | normal.xyz (vec3 — used by GPU lighting kernel) | `layout(location = 1)` |
| 24 | 8  | uv (vec2) | `layout(location = 2)` |
| 32 | 4  | localVertexID (uint) | `layout(location = 3) in uint a_localVertexID` |
| 36 | 4  | **zero-pad slot** | (none — currently unbound) |

**Slice 2 uses offset 36 for per-vertex `aRGBLight`** (the per-type vertex hot-color tag from `theShape->listOfTypeVertices[j].aRGBLight`). Add `layout(location = 4) in uint a_aRGBLight;` (or `vec4` with `GL_UNSIGNED_BYTE` `GL_TRUE` normalization) to `static_prop.vert`. **Storage encoding**: write the raw DWORD via `memcpy(vert+36, &aRGBLight, 4)` parallel to the rest of the staging writes in `registerType`. On little-endian x86 the bytes land as B,G,R,A in memory, which is what `lighting.hglsl:get_base_light()` already expects (it decodes `b | (g<<8) | (r<<16) | (a<<24)` from `GL_UNSIGNED_BYTE`/`GL_TRUE` swizzled `xyz`). Per `memory/mc2_argb_packing.md` — do NOT write the bytes in any other order.

No vertex stride growth.

### Per-type SSBO additions

New per-type SSBO carries the three hot-color fields used by `get_base_light()`:
- `hotPinkRGB` (vec3 padded to 16 B)
- `hotYellowRGB` (vec3 padded to 16 B)
- `hotGreenRGB` (vec3 padded to 16 B)

Total: 48 bytes per type × ~50 types = ~2.4 KB. Uploaded once at `finalizeGeometry()` time. New SSBO binding slot.

### `addRenderShape` double-draw avoidance

At `mclib/tgl.cpp:2522` (the `bShadersDrawPathEnabled` `if` head; the block closes at line 2555 with the `addRenderShape` call itself at line 2553), add the eligibility negation:

Current:
```cpp
if (bShadersDrawPathEnabled && !isSpotlight && !isWindow && ...)
```

Becomes:
```cpp
if (bShadersDrawPathEnabled && !eligibleForGpuObjects(this) && !isSpotlight && !isWindow && ...)
```

`eligibleForGpuObjects(TG_Shape*)` is a new free function or batcher method that checks `g_useGpuObjects && this->myType` is in the registered set. Defensive depth: R1 mutual exclusion already guarantees static-prop populations don't reach this branch when GPU path is on, but the gate prevents double-draw if R1 is ever loosened.

The second `addLightDataStructure`/`addRenderShape` site at `tgl.cpp:2817` is in `TG_Shape::Render` (legacy CPU draw path). For GPU-eligible populations, `Render` is not called (slice 1 batcher replaces it). No additional gating required.

### Render order / state restore (CRITICAL — inherited from slice 1)

- `batcher.flush()` runs AFTER `mcTextureManager->renderLists()` per `memory/render_order_post_renderlists_hook.md`. No change.
- Bridge state save/restore per slice 1 spec lines 178-200. No change.
- VAO 0 trap fix per `memory/projectz_overlay_findings.md`. No change.

## Shaders

### `shaders/include/lighting.hglsl` — extended

- Set `#define ENABLE_VERTEX_LIGHTING 1` (currently `0` at `lighting.hglsl:3`).
- **Signature change** (locked at Step 0 sign-off): replace 3-param `vec3 calc_light(in int lights_index, in vec3 normal, in vec3 base_light)` with 4-param `vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light)`. `vertex_world_pos` is required for falloff/spot computation. Update every caller in lockstep.
- Replace `calc_light()` (lines 119-137) body with full 6-type dispatch. Preserve the existing `if (numLights.x == 0) return vec3(1)` early-return. Pseudocode:

```glsl
vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light) {
    ObjectLights ld = light[lights_index];
    if (ld.numLights.x == 0) return vec3(1);  // PRESERVE — header is included by other shaders
    vec3 final = base_light;
    vec3 ambient = vec3(0);
    for (int i = 0; i < ld.numLights.x && i < MAX_LIGHTS_IN_WORLD; i++) {
        int type = int(ld.light_dir[i].w);
        vec3 color = ld.light_color[i].xyz;
        if (type == TG_LIGHT_AMBIENT) {
            ambient += color;
        } else if (type == TG_LIGHT_INFINITE) {
            float n_dot_l = clamp(dot(normal, -ld.light_dir[i].xyz), 0.0, 1.0);
            final += n_dot_l * color;
        } else if (type == TG_LIGHT_INFINITEWITHFALLOFF) {
            // length(vertex_world_pos - ld.light_pos[i])  // light_pos needs schema add
            // Apply linear falloff per GetFalloff math.
            // dot(normal, lightDir).
            ...
        } else if (type == TG_LIGHT_POINT) {
            // similar; vertex-to-light direction, falloff.
            ...
        } else if (type == TG_LIGHT_SPOT) {
            // cone test on s_spotDir, falloff.
            ...
        } else if (type == TG_LIGHT_TERRAIN) {
            // pre-baked specular contribution via per-vertex SSBO field — see schema below
            ...
        }
    }
    return final + ambient;
}
```

`vertex_world_pos` is the world-space vertex position — VS computes it from per-vertex shape-local position × per-instance shapeToWorld matrix.

`TG_LIGHT_TERRAIN` is special: CPU path pre-bakes its contribution into `listOfColors[].redSpec/.greenSpec/.blueSpec` only when `useShadows` is true. **For slice 2, the simplest port is to keep the CPU pre-bake** (`MultiTransformShape_PositionsOnly` retains the `case TG_LIGHT_TERRAIN:` branch in `MultiTransformShape` at `tgl.cpp:2063-2096`; the `redSpec/greenSpec/blueSpec` writes specifically are at `tgl.cpp:2084-2086`. These writes feed `listOfColors`, which is consumed by the legacy CPU fallback path). The GPU kernel can either ignore TG_LIGHT_TERRAIN (and accept slight specular under-count for GPU population) or sample the per-vertex specular contribution from a new SSBO. **Default**: ignore on GPU side; surface to spec-review for visual delta judgment.

### Per-instance schema additions

Per-instance VS reads `lightDataIndex` from the existing `_pad0` slot (now repurposed). Per-vertex VS reads `aRGBLight` from offset-36 of the per-vertex VBO.

### AMD invariants

Inherited from slice 1 (line 217-232). No additions.

## Killswitch / env gating

```
MC2_GPU_OBJECTS=1                 → g_useGpuObjects = true (default false)  [slice 1]
MC2_OBJBATCHER_TRACE=1            → [OBJBATCHER v1] per-frame prints  [slice 1]
MC2_OBJECT_PARITY_CHECK=1         → enables P3 dual-emit + P1 sampled bytewise (slice 2 NEW)
```

`MC2_OBJECT_RECON_TRACY=1` is the recon-instrumentation flag from commit `c4c4e96` (Recon Zero deliverable). Stays in tree, gated off by default. Useful for re-validating perf claims after slice 2 lands.

## Migration stages

### Stage 2.A — Substrate edits (no behavior change under `MC2_GPU_OBJECTS=0`)

Files:
- `mclib/tgl.h`: declare `MultiTransformShape_PositionsOnly` and `GatherGpuObjectLightDataOnly`. (Existing `MultiTransformShape` declaration is at `tgl.h:852`.)
- `mclib/tgl.cpp`: define both. `_PositionsOnly` is a copy-and-strip of `MultiTransformShape`. Implementation rule: walk `MultiTransformShape` from the top (`tgl.cpp:1657`) and KEEP all transform code (the `vlight` Tracy zone setup at 1726, the per-vertex `for` loop opening at 1728, parallel-transform code in the early portion of the loop body, screen-space writes to `listOfVertices[j].x/y/z/rhw/frgb`); STRIP the lighting/hot-color/highlight subset of the loop body (roughly line ~1820 through 2246 — the per-vertex lighting computation and color packing); KEEP the loop's closing brace at `tgl.cpp:2272`; STRIP the `aRGBHighlight` block at `tgl.cpp:2247-2270`; KEEP the per-face flight scope's backface-cull-only population of `listOfVisibleFaces` at `tgl.cpp:2284-2288`; STRIP the rest of the per-face flight scope at `tgl.cpp:2274-2515` (per-face lighting computation, `listOfTriangles[j].aRGBLight[i]` write at 2450, `listOfTriangles[j].fRGBLight[i]` write at 2471, the four `addTriangle` calls at 2492/2496/2505/2509); STRIP the `addRenderShape` block at `tgl.cpp:2522-2555`. **The exact micro-boundaries inside the per-vertex loop body are to be re-derived against current `tgl.cpp` at edit-time** (the line numbers above are accurate as of 2026-05-02 grep, but the `_PositionsOnly` strip should be done semantically — keep transform, strip lighting — rather than by literal line range, to be robust against further drift). Pool-allocation invariant: `_PositionsOnly` MUST leave `listOfColors`, `listOfTriangles`, `listOfVisibleShadows` pointers non-null (PerPolySelect at `tglpp.cpp:14-21` early-outs unless ALL pointers are non-null), even if their contents are stale.
- `mclib/tgl.cpp` line 2522: add `!eligibleForGpuObjects(this)` to the `bShadersDrawPathEnabled` condition (the `if` head). Add `eligibleForGpuObjects(TG_Shape*)` as a free function or batcher static.
- `GameOS/gameos/gos_static_prop_batcher.h`: declare `isMultiShapeEligibleForGpuObjects`. Add `lightDataIndex` to `GpuStaticPropInstance` (repurpose `_pad0` at offset 76; struct currently spans `gos_static_prop_batcher.h:13-21`). Update the `static_assert` at `gos_static_prop_batcher.h:30` (offsetof name and description string).
- `GameOS/gameos/gos_static_prop_batcher.cpp`: define `isMultiShapeEligibleForGpuObjects`. Wire late-registration setter at the unregistered-type branch `gos_static_prop_batcher.cpp:674-693`: set `needsFullBakeNextFrame=true` on the actor (via the appearance pointer) and increment `late_register_recovery_skips`, BEFORE the `return false;` tail at ~692 (NOT inside the inner `if (count == 0)` print block).
- `mclib/bdactor.h`, `mclib/bdactor.cpp`: add a NEW `bool needsFullBakeNextFrame;` member to `BldgAppearance` and `TreeAppearance`. Initialize false in the constructor. (Note: these classes do NOT have an `appearanceFlags` aggregator — grep confirmed they use individual `bool` members like `isReversed`, `forceLightsOut`, etc. Add a new `bool` next to those.)
- `mclib/genactor.h`, `mclib/genactor.cpp`: same — new `bool needsFullBakeNextFrame;` on `GenericAppearance`.

**No call sites are switched to the new path yet.** All actors still go through full `TransformMultiShape`.

**Gate**: tier1 5/5 PASS in two configs (unset, `MC2_GPU_OBJECTS=1`). +0 destroys delta. Tracy zones unchanged.

### Stage 2.B — Wire eligibility hoist + positions-only into update path

Files:
- `mclib/bdactor.cpp` `BldgAppearance::update` (function start at line 1957; existing `bldgShape->TransformMultiShape` call at line 2200 inside `if (inView || g_useGpuStaticProps)` opening at 2191) and `TreeAppearance::update` (function start at line 4213; existing `treeShape->TransformMultiShape` call at line 4313 inside `if (inView || g_useGpuStaticProps)` opening at 4300): wrap the existing call in the eligibility branch from the "Eligibility hoist" architecture section above. **The branch lives INSIDE the existing cull gate.** The shadow-shape companion calls (`bldgShadowShape->TransformMultiShape` at `bdactor.cpp:2206`, `treeShadowShape->TransformMultiShape` at `bdactor.cpp:4321`) are NOT touched.
- `mclib/genactor.cpp` `GenericAppearance::update` (function start at line 1049; existing `genShape->TransformMultiShape` call at line 1189 inside `if (inView || g_useGpuStaticProps)` opening at 1185): same pattern.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` (function spans 641-737): set `needsFullBakeNextFrame=true` on the late-registration branch (the `if (s_typeIndex.find(ts) == s_typeIndex.end())` block at lines 674-693), increment `late_register_recovery_skips`. The flag-set must dominate the `return false;` at the tail.

**Visual behavior at this stage**: with `MC2_GPU_OBJECTS=1`, eligible static-prop actors run positions-only at update-time. Their `.argb` is stale or zero. **Slice 1's batcher continues to read `listOfVertices[j].argb` and memcpy it into the per-instance color SSBO** — so it will draw with stale colors. This is intentional: at this stage, the colors are wrong but the kernel split is verified.

**Stage 2.B gate (intentionally narrow)**: Stage 2.B may be visually wrong under `MC2_GPU_OBJECTS=1`. **Do NOT evaluate visual parity, screenshot quality, or color correctness at this stage.** Stage 2.C completes the picture. Gate ONLY on:
- No crash / no hang in tier1 5/5 PASS (configs: unset / `MC2_GPU_OBJECTS=1` / `MC2_GPU_OBJECTS=1 + MC2_OBJBATCHER_TRACE=1`).
- +0 destroys delta in every mission per `memory/feedback_pool_peak_compare_same_mission.md`.
- TGL pool peak unchanged (Gate E proxy).
- No cull-cascade or lifecycle regression (the safety claim of slice 1+2 — never touch the cull path).
- F-gate counters healthy: `gpu_drawn_instances > 0` for every static-prop population, `late_register_recovery_skips ≤ 2` per mission for artillery/bomber-bearing missions.
- Tracy `appearanceUpdate` zone may move slightly (positions-only is a strict subset of the full kernel); render zone neutral. Don't gate on Tracy magnitude at 2.B.

**Anti-pattern to avoid**: someone "fixing" the intentional temporary visual break by undoing positions-only or by re-introducing color writes. The colors come back on at Stage 2.C when GPU lighting comes online; at Stage 2.B they're SUPPOSED to be wrong.

**Note**: this stage will produce visibly wrong colors for the GPU population. Mark in PR description. Stage 2.C completes the picture; Stage 2.B is intentionally a partial-state for clear bisection.

### Stage 2.C — Wire GPU vertex lighting (the meat)

Files:
- `shaders/include/lighting.hglsl`: set `ENABLE_VERTEX_LIGHTING 1`, **change `calc_light()` signature from 3-param to 4-param adding `vertex_world_pos` (locked at Step 0 sign-off)**, finish 6-type dispatch, add `GetFalloff` GLSL helper, preserve the `numLights.x == 0` early-return. Update all callers (currently only the stub paths inside `lighting.hglsl`) in lockstep — pre-edit and post-edit grep counts for `calc_light(` must match.
- `shaders/static_prop.vert`: per-vertex `calc_light()` invocation with the new 4-param signature; compute `vertex_world_pos = (shapeToWorld * vec4(position, 1)).xyz`; add `layout(location = 4) in uint a_aRGBLight;` (or `vec4` with `GL_UNSIGNED_BYTE`/`GL_TRUE`); output lit ARGB as varying. (Existing file currently has no `calc_light()` invocation; pulls per-instance argb from `colors_` SSBO. Extending it is feasible — no need for a new `static_prop_lit.vert`.)
- `shaders/static_prop.frag`: consume lit ARGB from VS varying instead of from per-instance color SSBO (one-line varying rewire).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` (641-737): **per-actor (NOT per-leaf) call to `GatherGpuObjectLightDataOnly()` hoisted between the TWO per-leaf `for (int i = 0; i < n; ++i)` loops** (the first at line 667 is the registration-check / early-out loop; the second at line 698 is the submit loop). The call site sits between approximately line 694 and 696 — AFTER the registration-check loop has cleared and BEFORE the submit loop begins. The returned `lightDataIndex` is broadcast into each leaf's per-instance struct INSIDE the second loop's body. Stop memcpying `listOfVertices[j].argb` into the per-instance color SSBO (since GPU lights it now). **DO NOT** place the gather call inside either loop body (registration loop at 667+ → wasted work on rejected actors; submit loop at 698+ → per-leaf, N-fold redundant).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `registerType` (~408-497): add per-vertex `aRGBLight` write at offset 36 (currently zero-padded — see Per-vertex VBO usage table). **Storage encoding: write the raw DWORD via `memcpy(vert+36, &aRGBLight, 4)` parallel to the rest of the staging writes.** On little-endian x86 the bytes land as B,G,R,A in memory, matching `lighting.hglsl:get_base_light()`'s existing `b | (g<<8) | (r<<16) | (a<<24)` decode from `GL_UNSIGNED_BYTE`/`GL_TRUE`. Source value: `typeShape->listOfTypeVertices[localVertIdx].aRGBLight`.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `finalizeGeometry` (510-558, runs once at map load): build per-type SSBO with hot-color fields (`hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB` — names match the `in vec3` parameters of `get_base_light()` at `lighting.hglsl:35`). Bind at draw time.
- `mclib/tgl.h`: extend `TG_HWLightsData` (currently at `tgl.h:284-298` with `lightDir[16][4]`, `lightColor[16][4]`, `numLights_`, `pad[3]`) with `closeDistance`/`farDistance`/`oneOverDistance` per-light fields (or pack into existing `vec4` w-components — small schema). `mclib/txmmgr.cpp:938-1005` `GatherLightsParameters` populates them from `s_listOfLights[i]->closeDistance` etc. (source fields exist at `tgl.h:193-195`).
- New SSBO binding for per-type hot-color buffer.

**Visual behavior at this stage**: with `MC2_GPU_OBJECTS=1`, GPU population renders with GPU-computed lighting. Should be visually equivalent to legacy CPU path (modulo per-face additive, which is dead in stock).

**Gate**: tier1 5/5 PASS, render zone Tracy delta ≥0% (substrate didn't slow down — slice 2's perf gate is on the **update** zone, not render), `appearanceUpdate` Tracy zone shows ~17-21% reduction with `MC2_GPU_OBJECTS=1`. Visual canary at fixed camera shows no regression. F-gate counters healthy.

### Stage 2.D — Parity instrumentation

Files:
- `GameOS/gameos/gos_static_prop_batcher.cpp`: `MC2_OBJECT_PARITY_CHECK=1` env gate. P3 dual-emit at first frame: run BOTH legacy `TransformMultiShape` and `_PositionsOnly` for all actors, bytewise-compare `listOfTriangles[j].aRGBLight[i]` (CPU) against GPU output.
- P1 sampled bytewise in steady state: 1 actor per type per frame, round-robin, compare GPU output (read back via PBO async, 1-frame stale OK) against CPU `MultiTransformShape` recomputation.
- Mismatch logging: `[OBJECT_PARITY v1] event=lighting_mismatch actor=X tri=Y corner=Z cpu=ARGB gpu=ARGB`. ULP tolerance ±2 LSB per channel.
- 600-frame summary line: counts of compared/passed/mismatched.

**Compare-target caveat (load-bearing)**: We compare at triangle-corner granularity (`listOfTriangles[].aRGBLight[i]`) — the value `TG_Shape::Render` actually emits — even though slice 2's GPU output is per-vertex-lit. This is intentional belt-and-suspenders. **Because `useFaceLighting=false` permanently in stock** (Recon Section 9 Correction B), the triangle-corner color is expected to equal the per-vertex-lit color modulo alpha-byte and packing. **Any mismatch here indicates packing, fog/highlight, terrain-light, or shader-math divergence — NOT missing per-face lighting.** A reviewer or future implementer who sees this Stage 2.D compare and asks "why corner-granularity if slice 2 is per-vertex?" gets the same answer: in stock, the corner-granularity compare degenerates to per-vertex equivalence + alpha-byte handling, but the comparison shape is more general (catches per-face additive divergence on hypothetical mod content where `useFaceLighting=true`, even though slice 2 doesn't claim parity in that case).

**Gate**: zero mismatches across tier1 stock missions with `MC2_OBJECT_PARITY_CHECK=1`. If mismatches exceed threshold, surface to user.

**Merge policy (slice 2 deliberately splits)**:

- **Stages 2.A-2.C may merge behind `MC2_GPU_OBJECTS=1` flag** if their respective gates pass (no crash, +0 destroys, pool peak unchanged, render-zone Tracy neutral, Stage 2.C visual canary clean + ≥17% appearanceUpdate reduction).
- **Stage 2.D parity is NOT a merge blocker for the slice 2 PR.** It IS a hard pre-condition for either: (a) declaring slice 2 "validated" / done, OR (b) any default-on flip consideration.
- **Stage 2.E pinned-camera screenshot diff** is also a pre-default-on blocker, separately.

This split is deliberate. Stage 2.D requires PBO async-readback infrastructure that may not exist in tree (see hand-off prompt Step 4); blocking the perf slice behind tooling-build risks stalling the actual win. The flag-merge-then-validate-then-default-on cadence mirrors slice 1's pattern.

**Note**: the P3 dual-emit at mission start adds a one-frame-of-startup cost; this is acceptable. P1 runs at steady state cost (~one extra `MultiTransformShape` per frame ≈ 1 µs/frame).

### Stage 2.E — Pinned-camera screenshot diff (separate PR, gates default-on)

Same harness as slice 1's Stage 1.E. If slice 1's Stage 1.E hasn't landed yet, this stage builds it; if it has, slice 2 reuses it.

Files:
- `tests/smoke/object_visual_diff.py`: deterministic camera-pin + screenshot capture + tolerance-based diff against baseline reference.
- Baseline captured with `MC2_GPU_OBJECTS=0`.
- Diff captured with `MC2_GPU_OBJECTS=1`.
- Threshold: ≤0.5% pixels diffed by ≤2 LSB.

**Gate**: pixel-diff under threshold. Required for default-on flip; NOT for flagged merge.

Slice 2 PR may merge before this exists. Default-on flip cannot.

## Test plan / gate ladder

Inherits slice 1's gate ladder. Slice 2 specific additions:

### A. Visual canary (slice 2)
- Side-by-side at fixed camera on `mc2_01` airbase region. Pass: no visible regression.

### B. Tracy delta on UPDATE zone
- Target: `appearanceUpdate` Tracy zone reduction ≥17% with `MC2_GPU_OBJECTS=1`.
- Threshold (lower bound): if reduction is <10%, surface to user; spec's perf claim was wrong.
- Render zone Tracy: no regression (slice 2 preserves slice 1's render path).

### C. Parity (Stage 2.D)
- `MC2_OBJECT_PARITY_CHECK=1` zero mismatches across tier1 stock.

### D. tier1 5/5 PASS triple
- Configs: unset / `MC2_GPU_OBJECTS=1` / `MC2_GPU_OBJECTS=1 + MC2_OBJECT_PARITY_CHECK=1`.
- +0 destroys delta on every mission.

### E. TGL pool peak ≤ pre-slice peak
- Slice 2 calls fewer pool allocations (positions-only pre-allocs via the same path; doesn't change consumption).

### F. F-gate counters (slice 2 additions)
- `late_register_recovery_skips` should be O(2) per mission with artillery/bomber spawns. Higher counts indicate eligibility-hoist gaps.
- Per-population GPU-drawn counts unchanged from slice 1.

### Triggers (slice will not merge behind flag)
- Visual canary regression.
- Tracy delta on update zone < 10% (or surface to user with explicit acceptance).
- Parity mismatches > 0.
- Destroys delta != 0.
- Pool peak rises.
- `late_register_recovery_skips` rate > expected (>5 per mission would indicate a real eligibility gap).

## Risks and open questions

### R1. CPU/GPU FP divergence on lit ARGB

Mitigated by P3 dual-emit + P1 sampled bytewise with ULP tolerance ±2 LSB per channel. If real divergence exceeds threshold, the spec needs to either tighten ULP rules or accept the divergence as ground truth (slice 2's GPU output becomes the new authoritative answer).

### R2. TG_LIGHT_TERRAIN handling on GPU

CPU path pre-bakes terrain-light specular into `listOfColors[]`. GPU kernel currently doesn't read this. **Default**: ignore TG_LIGHT_TERRAIN on GPU side; under-count specular by the small terrain-light contribution. Surface visual delta judgment to user before final default-on flip.

### R3. Late-registration recovery flicker

One frame of "actor not rendered" per late-registered actor (typically artillery/bomber spawns; slice 1 spec lines 489-490 document two known types pending allowlist). The rate is **NOT** bounded by a static per-mission cap — slice 1 establishes no such cap. Acceptability is bounded empirically via the new `late_register_recovery_skips` Gate F counter observed during smoke. If observed rate is low (≤2 per mission across tier1), the visible impact is acceptable — 1-frame flickers at mission start. If observed rate is higher (>5), the eligibility-hoist coverage has a gap and the count must be investigated rather than treated as cosmetic.

### R4. Mod compatibility (`useFaceLighting=true`)

A mod that flips `useFaceLighting` to true would silently differ from CPU on slice 2 (GPU doesn't compute per-face additive). Per `feedback_offload_scope_stock_only.md`, mod renderer breakage is the mod's problem.

### R5. Per-vertex `aRGBLight` data not in slice 1's vertex VBO

Slice 1 zero-padded offset 36 (`gos_static_prop_batcher.cpp:461`). Slice 2's Stage 2.A vertex VBO write changes this. Type registration MUST be fully redone for slice 2 to pick up the new vertex data — but `finalizeGeometry()` is called once at map load. **Spec clarification**: a type registered against slice 1's pre-aRGBLight VBO will not have correct vertex data when slice 2 wants to read it. Two paths: (a) slice 2 spec mandates that all types are re-registered from scratch when `MC2_GPU_OBJECTS=1` is detected at startup (clean but adds startup cost); (b) slice 2 is intrinsically a fresh-binary requirement (any binary with slice 2 code unconditionally writes aRGBLight at registration time, even if `MC2_GPU_OBJECTS=0`). **Default**: (b) — the registration cost is negligible; the tight coupling between binary-and-data-format is the simpler invariant.

## Files

### New
- (none — uses existing slice 1 batcher and existing lighting.hglsl)
- New SSBO binding for per-type hot-color buffer (in batcher.cpp).
- New env-gated parity (`MC2_OBJECT_PARITY_CHECK`).

### Modified
- `mclib/tgl.h`: declare `MultiTransformShape_PositionsOnly`, `GatherGpuObjectLightDataOnly` (Stage 2.A). Extend `TG_HWLightsData` with falloff fields **(Stage 2.C ONLY — must ship in lockstep with the matching `lighting.hglsl ObjectLights` extension and `calc_light()` 4-param rewrite; doing it earlier breaks the C++/GLSL UBO stride and corrupts the legacy shader path, regression-discovered 2026-05-02)**.
- `mclib/tgl.cpp`: define both new functions. Add `eligibleForGpuObjects` helper. Add `addRenderShape` gate at line 2522 (Stage 2.A).
- `mclib/txmmgr.cpp`: extend `GatherLightsParameters` to populate falloff fields **(Stage 2.C ONLY — see TG_HWLightsData note above; do not add lightFalloff writes in Stage 2.A)**.
- `mclib/bdactor.cpp` `BldgAppearance::update` (1957) + `TreeAppearance::update` (4213): eligibility branch INSIDE existing cull gate.
- `mclib/genactor.cpp` `GenericAppearance::update` (1049): same.
- `mclib/bdactor.h`, `mclib/genactor.h`: NEW `bool needsFullBakeNextFrame;` member on each appearance class (NOT a packed bit into a fictional `appearanceFlags` byte — those classes have no such aggregator).
- `GameOS/gameos/gos_static_prop_batcher.{h,cpp}`: add `isMultiShapeEligibleForGpuObjects`, `lightDataIndex` field, late-registration recovery setter, parity check infrastructure (Stage 2.D).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `registerType`: write per-vertex `aRGBLight` at offset 36.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `finalizeGeometry`: build per-type hot-color SSBO.
- `shaders/include/lighting.hglsl`: ENABLE_VERTEX_LIGHTING=1, finish calc_light().
- `shaders/static_prop.vert`: invoke calc_light().
- `shaders/static_prop.frag`: consume VS-produced lit ARGB.

### Touched but not behavior-changed
- `code/objmgr.cpp:2187, 2196`: PerPolySelect call sites — work because positions are still in `listOfVertices[].x/.y` via the reduced CPU pass.
- All cull-bypass sites (slice 1 R1 invariant): unchanged.

## Reference docs

- Brainstorm: [`brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)
- Recon Zero: [`explorations/2026-05-02-object-offload-slice2-recon-zero.md`](../explorations/2026-05-02-object-offload-slice2-recon-zero.md) (especially Section 9)
- Slice 1 design: [`specs/2026-05-02-object-offload-slice1-design.md`](2026-05-02-object-offload-slice1-design.md)
- Memory:
  - `memory/cull_gates_are_load_bearing.md` ⭐
  - `memory/tgl_pool_exhaustion_is_silent.md` ⭐
  - `memory/mc2_texture_handle_is_live.md`
  - `memory/static_prop_projection.md`
  - `memory/gpu_direct_renderer_bringup_checklist.md`
  - `memory/render_order_post_renderlists_hook.md`
  - `memory/feedback_offload_scope_stock_only.md`
  - `memory/feedback_smoke_no_canary.md`
  - `memory/feedback_pool_peak_compare_same_mission.md`
  - `memory/feedback_subagent_no_cmake_configure.md`
- Skill: `.claude/skills/adversarial-plan-review.md` — slice 2 spec MUST go through this before plan write per worktree CLAUDE.md "Review Discipline."
