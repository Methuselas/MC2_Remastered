# Per-Shape Light-Pack Scored Selection — Design Spec

- **Status:** DRAFT — ready for adversarial review
- **Date:** 2026-05-21
- **Worktree:** `claude/nifty-mendeleev`
- **Slice ID:** (E') — extends (E) SpotLight_ real illumination; in (E)'s scope per greybeard ruling
- **Companion specs:**
  - [(E) SpotLight_ retirement](2026-05-20-spotlight-real-illumination-design.md) — shipped Stages 0-3; this spec finishes the user-visible promise
  - (F) terrain lighting saturation — separate, orthogonal slice (lighting MODEL not data flow)
- **Greybeard verdict:** META-FIX, in (E)'s scope. Deferring would be the canonical additive-slice anti-pattern.
- **All file:line citations grep-verified at write time against `.claude/worktrees/nifty-mendeleev/`.**

---

## 1. The bug class (T1.16 evidence)

(E) Stages 0-3 retire cone billboards and register real `TG_Light` instances via `addWorldLight`. Diagnostic T1.16 confirmed:

- Registration works: 4 bldg + 19 mech + 31 GV lights registered into `worldLights[]` at slots 4, 6, 15, 17 (bldg) and 18-67 (mech + GV). Zero pool overflows.
- Camera gate (`projectForLightingShadow` at [mclib/camera.cpp:1935](mclib/camera.cpp)) correctly admits in-camera-zone slots — slot=36 mech and GV cluster 45/56-59 are 100% active in summary windows.
- BUT user-visible: mechs/GVs in mc2_10's intro DON'T visibly illuminate, even when their slots are `active=1`.

Adversarial + greybeard parallel reviews converged on:

**`GatherLightsParameters` at [mclib/txmmgr.cpp:1561-1720](mclib/txmmgr.cpp) is FIFO with a 16-slot cap. No scoring.** It walks `s_listOfLights[0..1023]` linearly and breaks on `num_lights == MAX_HW_LIGHTS_IN_WORLD (16)`. World-level base lights (AMBIENT, INFINITE, TERRAIN, weapon-bolt POINTs) occupy slots 0..17. (E)'s POINT lights register LATER at slots 18-67, beyond where the FIFO walk truncates.

**Buildings glow** because their slots (4/6/15/17) are below the 16-cap → survive FIFO → reach shader → visible yellow.

**Mechs/GVs don't glow** because their slots (18-67) are above the 16-cap → truncated → never reach shader.

The bug class: **per-actor finite-range light truncated by first-N-active selection.** This applies not just to (E)'s SpotLight_ lights — any future per-actor dynamic light (muzzle flash, weapon impact, explosion glow) hits the same wall.

## 2. Three approaches

### (α) Scored per-shape selection (RECOMMENDED)

Replace `GatherLightsParameters`'s FIFO with a partitioned scored selection:

1. **Always-in partition** (cheap, world-global): AMBIENT, INFINITE, INFINITE_WITH_FALLOFF. These are scene-wide; include unconditionally up to a soft cap (typically 1-4 such lights total in stock content).
2. **Finite-range partition** (POINT, SPOT, TERRAIN): Score each by distance-to-actor against `farDistance`. Include nearest-K up to the remaining slot count.

**Selection function signature:**
```cpp
// In mclib/light_select.h (new TU)
namespace mc2_light_select {
    // Returns N actual indices into s_listOfLights[] selected for this actor.
    // The decoupling point: now per-shape scored selection; later a clustered
    // implementation swaps this function without changing the packer.
    size_t select_lights_for_actor(
        const Stuff::Vector3D& actorWorldPos,
        TG_LightPtr* worldLights, size_t numWorldLights,
        size_t maxOutCount,
        size_t* outIndices  // capacity >= maxOutCount
    );
}
```

The packer at `GatherLightsParameters` calls this once, gets indices, packs only those into `TG_HWLightsData`. Identical SSBO layout; identical shader; only the SELECTION logic changes.

**Cost (CPU):** O(num_world_lights × num_lit_shapes) per frame. With 1024 candidate slots × ~200 lit shapes per frame = 200K distance computations. At <100ns per float-subtract-and-compare, ~20µs total per frame. Below F3's 100µs CPU-projection budget. Negligible.

### (β) Flat bump 16 → 32 or 64

Increase `MAX_HW_LIGHTS_IN_WORLD` shader-side. Doesn't fix the bug class (still FIFO; still truncates at whatever the new N is); just raises the truncation ceiling. Costs:
- Per-vertex shader loop is N× more iterations — 4× cost increase at N=64
- SSBO size per shape grows linearly — `TG_HWLightsData` from ~1.8KB at N=16 to ~7.2KB at N=64. With ~200 lit shapes per mission: ~1.4GB total SSBO at N=64. NOT viable.

**Rejected.** Doesn't address the bug class, AND blows out memory/perf.

### (γ) Clustered forward+ / deferred lighting

Modern approach used by Unreal, Frostbite, idTech. Decompose screen into 3D clusters; compute shader builds per-cluster light list; fragment shader iterates only the cluster's lights. Scales to thousands of lights with bounded per-pixel cost.

**Out of scope for now.** MC2's scene scale (50-100 lights) doesn't justify the architectural complexity (compute pass, per-tile data, fragment shader rewrite, full pipeline rework). The (F) lighting MODEL rework may revisit this when the broader question of MC2's lighting becomes the topic. Currently filed as a future milestone.

**The selection function in (α) is the future-extension hook for (γ):** when MC2 eventually goes clustered, replace `select_lights_for_actor()` with a per-cluster light-list lookup; the packer downstream stays unchanged. Decoupling selection from packing is what makes the future-clustered transition cheap.

## 3. Greybeard ruling (verbatim from parallel subagent review)

**1. Subsystem pin.** `GatherLightsParameters` in [mclib/txmmgr.cpp:1561-1720](mclib/txmmgr.cpp) — the per-shape pack that copies `TG_Shape::s_listOfLights` into the per-shape `TG_HWLightsData` SSBO entry. The selection is first-N-active, NOT best-N-by-relevance. No distance scoring, no type-priority, no spatial cull.

**2. Symptom vs cause.** Symptom: shader gets no contribution from (E) lights for mech/GV bodies. Cause: pack policy is first-active-16, ignoring spatial relevance of POINT/SPOT lights to the actor being packed.

**3. The meta-fix.** Replace `GatherLightsParameters`'s first-N-active selection with relevance-scored selection for finite-range lights. Partition: AMBIENT+INFINITE always-in, finite-range filled by distance-to-actor.

**4. Substitutive test.** Per-frame probe over the per-shape pack output (`TG_HWLightsData` after `GatherLightsParameters`) shows `point_lights_packed >= 1` for mech/GV shapes with (E) registrations within range. Visual: mc2_10 intro shows visible illumination contribution from spotlights.

**5. Verdict.** `META-FIX` — `GatherLightsParameters` selection becomes relevance-scored. Blast radius: one function + one parameter threaded through `CacheGpuLightData`. Bug class retired: "per-actor finite-range light truncated by first-N-active selection." In (E)'s scope because (E) cannot claim "real illumination" while its registered lights are systematically dropped before the shader.

## 4. Implementation shape

### 4.1 New TU: `mclib/light_select.{h,cpp}`

~50 LOC. Exposes `mc2_light_select::select_lights_for_actor()` per §2 (α) signature. Internally:

1. Walk `worldLights[0..numLights-1]`.
2. For each AMBIENT/INFINITE/INFINITEWITHFALLOFF: push index into `out[0..K1-1]` slots; track K1 used. Cap K1 at a small number (e.g. 4 — far more than stock content uses; cheap safety).
3. For each POINT/SPOT/TERRAIN with `active==true`: compute `distSq = (light->position - actorWorldPos).MagnitudeSquared()`; compute `farDistSq = light->farDistance * light->farDistance`; if `distSq <= farDistSq`, push (index, distSq) into a candidate vector.
4. Partial sort candidate vector by `distSq` ascending, take the nearest `maxOutCount - K1` indices, append to `out`.
5. Return total count `K1 + numFiniteRangeSelected`.

Pure C++; no GL; no engine state. Vulkan-friendly by absence-of-coupling.

### 4.2 `GatherLightsParameters` callsite

In [mclib/txmmgr.cpp:1561](mclib/txmmgr.cpp), the FIFO loop is replaced with:
1. Call `select_lights_for_actor(actorWorldPos, s_listOfLights, ...)` → indices array.
2. Loop over indices, packing each into `TG_HWLightsData` (the existing per-element copy code stays unchanged).

### 4.3 Thread `actorWorldPos` through `CacheGpuLightData`

`CacheGpuLightData` and its leaf-level `EmitBakedGpuLightData` need actor position. The call sites (grep-verify):
- `mclib/bdactor.cpp:1880` (`mc2CacheOrBakeStaticGpuLight`) — building case; pass `BldgAppearance::position`
- `mclib/bdactor.cpp:1887` (same function alt branch)
- mech-side equivalents in `mclib/mech3d.cpp` (Mech3DAppearance::position)
- GV-side equivalents in `mclib/gvactor.cpp` (GVAppearance::position)

~20 LOC of plumbing through the 4 callsites.

### 4.4 Backward compat / pre-existing callers

If `CacheGpuLightData` has callers I haven't identified (terrain bake, weapon-bolt-cache, etc.), they need either:
- A default `actorWorldPos = origin` overload (preserves existing behavior for non-actor cases), OR
- Update at the time of audit if they're meaningful (e.g. weapon-bolt should pass the bolt's world position)

Plan-phase will catch these via grep + signature audit.

### 4.5 Total scope

~150 LOC across:
- 1 new TU (`mclib/light_select.{h,cpp}`)
- 1 modified function (`GatherLightsParameters`)
- 1 new parameter threaded through `CacheGpuLightData` + its 4-5 callers
- No shader change
- No SSBO layout change
- No GL state change

## 5. Vulkan-prep audit

Per [memory/vulkan_prep_explicit_device_discipline.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\vulkan_prep_explicit_device_discipline.md), check each requirement:

| Requirement | This slice |
|---|---|
| Explicit device-mediated binding (no `vb.bind()`) | Not applicable — no new GL bindings |
| No implicit cross-call GL state | Not applicable — no new GL state |
| std430 lockstep | Not applicable — SSBO layout unchanged |
| `[0,1]` depth | Not applicable — no depth |
| Enqueue/flush patterns | Not applicable — selection runs at `CacheGpuLightData` time, before existing enqueue |
| No full RHI ahead of need | Compliant — no abstraction layer added; pure C++ refactor |

**Vulkan-ready by absence.** The selection function is pure CPU code with no GPU resource coupling. The downstream packer's GL behavior is unchanged.

## 6. Future-extension hooks

The slice is **explicitly designed to be the swap-point for clustered lighting later**:

1. **`select_lights_for_actor()` is a clean interface.** Its inputs (actor position, world light list) are exactly what a per-tile cluster lookup would also consume. Replacing the function body with a cluster-lookup at some future date doesn't perturb the caller.
2. **No new global state.** The selection function is stateless; no per-frame accumulation state to migrate.
3. **No shader change.** The fragment/vertex shader doesn't know about selection policy. A clustered approach would replace the data flow into `TG_HWLightsData`, not the SSBO layout.
4. **Document the swap-point in code.** Comment block at `select_lights_for_actor()` declaration:
   ```cpp
   // SCOPE: per-shape light selection for the current data-driven pack pipeline.
   // FUTURE: when scene scale grows to require clustered/forward+ lighting,
   // replace this function body with a per-tile cluster lookup. The packer
   // downstream stays unchanged; only the source of indices changes.
   ```

This is the "modernize the shim when you touch it" pattern from worktree CLAUDE.md change-discipline rule, applied to a new shim that doesn't exist yet but will.

## 7. Adversarial considerations

Potential failure modes to surface during plan-phase + adversarial-plan-review:

- **A1. Per-shape scoring is too eager.** If the selection function is called for every leaf shape in a deep mech hierarchy, the cost multiplies. Need to confirm whether `CacheGpuLightData` is called per-leaf (≈ hundreds per mech) or per-root-shape (1 per mech). The latter is fine; the former needs caching.
- **A2. Light position is stale for moving mechs.** The selection runs against `light->position`, which is set per-frame by our (E) in-place update. If frame ordering is wrong (selection runs before `Mech3DAppearance::UpdateGeometry` sets position), we score against last-frame position. Tolerable for a 1-frame lag visually; worth confirming the order is correct.
- **A3. Stable sort matters or not?** If two lights have equal `distSq`, the selection result should be deterministic to avoid frame-to-frame flicker. Use `std::stable_sort` or break ties by world-pool slot index.
- **A4. Saving the existing CPU-fast-path.** `tgl.cpp:1968` `TransformMultiShape` also reads `s_listOfLights[]` for CPU vertex lighting. Whether it uses the SAME selection (truncated by best-N) or sees the full list is unclear. Need plan-phase audit.
- **A5. AMBIENT+INFINITE always-in might starve POINT slots.** Stock content may have 2-4 AMBIENT/INFINITE lights, leaving 12-14 slots for POINT/SPOT. Plenty. But if a future mod has 20 AMBIENT lights, the partition explodes. Add a soft cap (`K1 ≤ 4` or similar; remaining always-in spillover into finite-range partition).
- **A6. Negative-coord-axis projection trap.** The Stuff axis swizzle (Vector3D vs Point3D, with x/y/z swap convention) means `light->position` is in MC2 coords and `actorWorldPos` must be in the same coord system. Need to confirm both `BldgAppearance::position` / `Mech3DAppearance::position` / `GVAppearance::position` are MC2 world-space (un-swizzled), not Stuff-space.

## 8. Open questions

Resolve in plan-phase or surface to user for sign-off:

- **OQ1.** What's the `K1` cap for the always-in partition? Recommendation: 4 (sufficient for stock content's typical 1 AMBIENT + 1 INFINITE = 2 base lights; gives 2 slots of safety; leaves 12 for finite-range).
- **OQ2.** Does the per-shape filter run per-leaf or per-root for mechs? Plan-phase grep required.
- **OQ3.** Frame ordering of `select_lights_for_actor` vs `light->SetPosition` in our (E) in-place update — need to confirm selection sees current-frame positions. If not, simplest fix is to call `CacheGpuLightData` after position updates in the per-frame loop.
- **OQ4.** Does `tgl.cpp:1968` CPU vertex lighting need the same selection treatment, or is it dead/legacy enough to leave as FIFO?
- **OQ5.** For weapon-bolt lights or other non-actor TG_Light users, what `actorWorldPos` to use? Recommendation: pass `light->position` itself or origin; their per-shape effect is incidental.

## 9. Cross-references

- Spec (E): [docs/superpowers/specs/2026-05-20-spotlight-real-illumination-design.md](2026-05-20-spotlight-real-illumination-design.md)
- Plan (E): [docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md](../plans/2026-05-20-spotlight-real-illumination-plan.md)
- (F) lighting MODEL rework brief: [memory/terrain_lighting_compute_kernel_saturation.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\terrain_lighting_compute_kernel_saturation.md)
- Vulkan-prep discipline: [memory/vulkan_prep_explicit_device_discipline.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\vulkan_prep_explicit_device_discipline.md)
- Greybeard skill: [.claude/skills/greybeard.md](../../.claude/skills/greybeard.md)
- Adversarial review skill: [.claude/skills/adversarial-plan-review.md](../../.claude/skills/adversarial-plan-review.md)
- T1.16 diagnostic artifact: `tests/smoke/artifacts/2026-05-21T06-48-58/mc2_10.log`
- C++/GLSL UBO lockstep rule: [memory/cpp_glsl_ubo_struct_lockstep.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cpp_glsl_ubo_struct_lockstep.md) (not triggered here — SSBO layout unchanged — but worth checking if anyone tries to extend `TG_HWLightsData`)
