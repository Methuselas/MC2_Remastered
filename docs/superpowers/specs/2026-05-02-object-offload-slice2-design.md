# Object Offload — Slice 2 (GPU Vertex Lighting) — Design

Date: 2026-05-02
Worktree: `nifty-mendeleev`
Branch: `claude/nifty-mendeleev`
Author: ThranduilsRing + Claude (Opus 4.7, 1M context)
Brainstorm: [`brainstorms/2026-05-02-object-offload-scope.md`](../brainstorms/2026-05-02-object-offload-scope.md)
Recon Zero: [`explorations/2026-05-02-object-offload-slice2-recon-zero.md`](../explorations/2026-05-02-object-offload-slice2-recon-zero.md)
Slice 1 design: [`specs/2026-05-02-object-offload-slice1-design.md`](2026-05-02-object-offload-slice1-design.md)
Arc: object offload, slice 2 of a 2-slice arc.
Status: **approved for implementation pending Step 0 adversarial implementation review** per worktree CLAUDE.md "Review Discipline." Step 0 of the hand-off prompt is the gating review; CRITICAL findings surface to user before any code edit, but the architecture is settled — DO NOT redesign during Step 0 unless review uncovers a blocker.

## Slice scope (single sentence)

Replace the per-vertex CPU lighting bake (the per-vertex loop body inside `TG_Shape::MultiTransformShape` at `mclib/tgl.cpp:1755-2249`) with GPU vertex-shader lighting for the static-prop populations already wired to slice 1's batcher (buildings + trees + generics under `MC2_GPU_OBJECTS=1`), keeping a reduced CPU pass for screen-space positions, shadow-projection input, and `PerPolySelect` hit-test.

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

1. **`MultiTransformShape_PositionsOnly`** (NEW) — the reduced CPU pass. Runs for GPU-eligible populations under `g_useGpuObjects=1`. Writes:
   - `listOfVertices[j].x/y/z/rhw/frgb` (screen-space positions for `PerPolySelect` + legacy CPU fallback)
   - `listOfShadowTVertices[]` (shadow projections for legacy CPU shadow path)
   - `numVisibleFaces`, `listOfVisibleFaces[]` (backface cull bookkeeping)
   - `lastTurnTransformed = turn`
   
   Skips:
   - per-vertex lighting kernel (lines 1755-2249) — `.argb` is left whatever-it-was-before
   - `aRGBHighlight` additive (lines 2227-2249)
   - per-face lighting (lines 2272-2447) — but KEEPS the backface cull at 2284-2288 because it populates `listOfVisibleFaces`
   - `listOfTriangles[].aRGBLight[i]` and `.fRGBLight[i]` writes (lines 2424, 2445) — dead code in stock; removed
   - `addTriangle` queue calls (lines 2466-2483) — dead-on-Renderer-3
   - `addRenderShape` block (lines 2517-2553) — gated to legacy path only

2. **`MultiTransformShape`** (UNCHANGED) — full lighting bake. Continues to run for non-GPU-eligible cases: legacy fallback path, late-registration recovery, mover populations.

### GPU vertex lighting

Finishes the half-built kernel in `shaders/include/lighting.hglsl`:

- `get_base_light()` (lines 32-115) is already complete. No change.
- `calc_light()` (lines 119-137) is currently a 2-of-6-light-types stub with hardcoded `light[0]=directional`, `light[1]=ambient`. Slice 2 ports the remaining 4 types: INFINITEWITHFALLOFF, POINT, SPOT, TERRAIN. Switch on `light_dir[i].w` (the type field — already populated by `GatherLightsParameters` at `txmmgr.cpp:974`).
- `GetFalloff` math (linear interpolation per Section 9 Item 1) is added as a GLSL helper. Three new per-light fields in the SSBO: `closeDistance`, `farDistance`, `oneOverDistance`. Pack into `light_color.w` and a new vec4 slot per light, OR extend `TG_HWLightsData` schema (small).
- `ENABLE_VERTEX_LIGHTING` is set to `1` in `lighting.hglsl:3`.

The slice 2 vertex shader (probably extends `shaders/static_prop.vert` or a new `static_prop_lit.vert`) calls `calc_light()` per vertex with the per-instance `lightDataIndex` and per-vertex `aRGBLight` tag (already in the slice 1 vertex VBO at offset 36 per Recon Section 4).

### Side-effect-free light-data gather

New helper `TG_Shape::GatherGpuObjectLightDataOnly()` (Recon Section 9 Item 5):

```cpp
uint32_t TG_Shape::GatherGpuObjectLightDataOnly() {
    GatherLightsParameters(&lightData_);
    return mcTextureManager->addLightDataStructure(&lightData_);
}
```

Declared in `mclib/tgl.h` near `MultiTransformShape` (line 852). Defined in `mclib/tgl.cpp` immediately after `MultiTransformShape`. **Per-actor** (not per-leaf) call: invoked once at the top of `GpuStaticPropBatcher::submitMultiShape`'s eligible-child loop (around `gos_static_prop_batcher.cpp:698`), the returned index broadcast into each leaf's per-instance struct.

### Eligibility hoist

New method `GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(const TG_MultiShape* multi) const` per Recon Section 9 Item 4. Mirrors slice 1's render-time per-child gates EXCEPT the late-registration case. Called from `BldgAppearance::update`, `TreeAppearance::update`, `GenericAppearance::update` BEFORE the `TransformMultiShape` call site. Branch:

```cpp
if (g_useGpuObjects &&
    !appearanceFlags_needsFullBakeNextFrame &&  // late-reg recovery clears eligibility for one frame
    GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape)) {
    bldgShape->TransformMultiShape_PositionsOnly(&xlatPosition, &rot);
} else {
    bldgShape->TransformMultiShape(&xlatPosition, &rot);
    appearanceFlags_needsFullBakeNextFrame = false;  // recovery complete
}
```

### Late-registration recovery

When `submitMultiShape` hits the unregistered-type branch at `gos_static_prop_batcher.cpp:683-693`:
- Set per-actor flag `appearanceFlags_needsFullBakeNextFrame = true`.
- Skip render for that actor this frame. Increment new F-gate counter `late_register_recovery_skips` (separate from `cpu_fallback_by_pop` to keep fallback ratio clean).

Next frame's update sees the flag, takes the `else` branch (full `TransformMultiShape`), clears the flag. Frame N+2 onward, normal eligibility hoist applies. **Maximum visual impact**: one frame of "actor not rendered" per artillery/bomber spawn (≤ 2 events per mission per slice 1 spec line 86).

### Per-instance SSBO additions

| Field | Type | Where | Purpose |
|---|---|---|---|
| `lightDataIndex` | uint32_t | repurpose `_pad0` in `GpuStaticPropInstance` (gos_static_prop_batcher.h:13-32) | Index into `LightsData[32]` UBO from `addLightDataStructure` |

No struct growth (slot already exists).

### Per-vertex VBO usage

Existing slice 1 vertex layout (gos_static_prop_batcher.cpp:451-461) already includes:
- offset 12: normal.xyz (used by GPU lighting kernel)
- offset 36: 4-byte pad slot — **slice 2 uses this for per-vertex `aRGBLight`** (the per-type vertex hot-color tag from `theShape->listOfTypeVertices[j].aRGBLight`).

No vertex stride growth.

### Per-type SSBO additions

New per-type SSBO carries the three hot-color fields used by `get_base_light()`:
- `hotPinkRGB` (vec3 padded to 16 B)
- `hotYellowRGB` (vec3 padded to 16 B)
- `hotGreenRGB` (vec3 padded to 16 B)

Total: 48 bytes per type × ~50 types = ~2.4 KB. Uploaded once at `finalizeGeometry()` time. New SSBO binding slot.

### `addRenderShape` double-draw avoidance

At `mclib/tgl.cpp:2522` (the `bShadersDrawPathEnabled` block), add the eligibility negation:

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

- Set `#define ENABLE_VERTEX_LIGHTING 1` (currently 0 at line 3).
- Replace `calc_light()` (lines 119-137) with full 6-type dispatch. Pseudocode:

```glsl
vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light) {
    ObjectLights ld = light[lights_index];
    if (ld.numLights.x == 0) return vec3(1);
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

`TG_LIGHT_TERRAIN` is special: CPU path pre-bakes its contribution into `listOfColors[].redSpec/.greenSpec/.blueSpec` only when `useShadows` is true. **For slice 2, the simplest port is to keep the CPU pre-bake** (`MultiTransformShape_PositionsOnly` retains the terrain-light branch at `tgl.cpp:2050-2076` since it writes `listOfColors`, which is consumed by the legacy CPU fallback path). The GPU kernel can either ignore TG_LIGHT_TERRAIN (and accept slight specular under-count for GPU population) or sample the per-vertex specular contribution from a new SSBO. **Default**: ignore on GPU side; surface to spec-review for visual delta judgment.

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
- `mclib/tgl.h`: declare `MultiTransformShape_PositionsOnly` and `GatherGpuObjectLightDataOnly`.
- `mclib/tgl.cpp`: define both. `_PositionsOnly` is a copy-and-strip of `MultiTransformShape` — keeps lines 1656-1753 + the per-face backface-cull at 2284-2288 + the listOfShadowTVertices population (which is in `RenderShadows`, separate); strips lines 1755-2249 (per-vertex lighting kernel) and lines 2293-2515 (per-face lighting + listOfTriangles writes + addTriangle queue calls + addRenderShape block).
- `mclib/tgl.cpp` line 2522: add `!eligibleForGpuObjects(this)` to the `bShadersDrawPathEnabled` condition. Add `eligibleForGpuObjects` as a free function or batcher static.
- `GameOS/gameos/gos_static_prop_batcher.h`: declare `isMultiShapeEligibleForGpuObjects`. Add `lightDataIndex` to `GpuStaticPropInstance` (repurpose `_pad0`). Update static_assert.
- `GameOS/gameos/gos_static_prop_batcher.cpp`: define `isMultiShapeEligibleForGpuObjects`. Add per-actor `appearanceFlags_needsFullBakeNextFrame` flag check + late-registration setter at line 683-693 branch.
- `mclib/bdactor.h`, `mclib/bdactor.cpp`: add `appearanceFlags_needsFullBakeNextFrame` 1-bit flag to BldgAppearance + TreeAppearance. Initialize false. (Or pack into existing `appearanceFlags`.)
- `mclib/genactor.h`, `mclib/genactor.cpp`: same for GenericAppearance.

**No call sites are switched to the new path yet.** All actors still go through full `TransformMultiShape`.

**Gate**: tier1 5/5 PASS in two configs (unset, `MC2_GPU_OBJECTS=1`). +0 destroys delta. Tracy zones unchanged.

### Stage 2.B — Wire eligibility hoist + positions-only into update path

Files:
- `mclib/bdactor.cpp` `BldgAppearance::update` (line 1957) and `TreeAppearance::update` (line 4209): replace the unconditional `TransformMultiShape` call with the eligibility branch above.
- `mclib/genactor.cpp` `GenericAppearance::update` (line 1049): same pattern.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape`: set `appearanceFlags_needsFullBakeNextFrame=true` on the late-registration branch at line 683-693, increment `late_register_recovery_skips`.

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
- `shaders/include/lighting.hglsl`: set `ENABLE_VERTEX_LIGHTING 1`, finish `calc_light()` 6-type dispatch, add `GetFalloff` GLSL helper.
- `shaders/static_prop.vert` (or new `static_prop_lit.vert`): per-vertex `calc_light()` invocation; output lit ARGB to fragment shader.
- `shaders/static_prop.frag`: consume lit ARGB from VS output instead of from per-instance color SSBO.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape`: at top of eligible-child loop (line ~698), call `multi->listOfShapes[0].node->GatherGpuObjectLightDataOnly()` (or per-actor equivalent), broadcast index into per-leaf `lightDataIndex`. Stop memcpying `listOfVertices[j].argb` (since GPU lights it).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `registerType`: add per-vertex `aRGBLight` write at offset 36 (currently zero-padded). Source from `typeShape->listOfTypeVertices[localVertIdx].aRGBLight`.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `finalizeGeometry`: build per-type SSBO with hot-color fields. Bind at draw time.
- `mclib/tgl.h`: extend `TG_HWLightsData` with `closeDistance`/`farDistance`/`oneOverDistance` per-light fields (or pack into existing `vec4` w-components). `mclib/txmmgr.cpp:938-1005` `GatherLightsParameters` populates them from `s_listOfLights[i]->closeDistance` etc.
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

One frame of "actor not rendered" per artillery/bomber spawn. Slice 1's late-register count caps at "two types per mission" per spec line 86. Worst case: 2 actors flicker for 1 frame each at mission start. Acceptable.

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
- `mclib/tgl.h`: declare `MultiTransformShape_PositionsOnly`, `GatherGpuObjectLightDataOnly`. Extend `TG_HWLightsData` with falloff fields.
- `mclib/tgl.cpp`: define both. Add `eligibleForGpuObjects` helper. Add `addRenderShape` gate at line 2522.
- `mclib/txmmgr.cpp`: extend `GatherLightsParameters` to populate falloff fields.
- `mclib/bdactor.cpp` `BldgAppearance::update` + `TreeAppearance::update`: eligibility branch.
- `mclib/genactor.cpp` `GenericAppearance::update`: same.
- `mclib/bdactor.h`, `mclib/genactor.h`: per-actor `appearanceFlags_needsFullBakeNextFrame` flag.
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
