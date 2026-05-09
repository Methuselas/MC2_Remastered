# Track D Slice C — GPU Mech Cull + Weighted Skinning

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice B (in soak; B1+B2+B+ functionally complete)

## Goal

Two independent surgical adds:
- **C1 — GPU mech cull (render-only):** skip submitActor for mechs the GPU frustum/visibility shader marks invisible. Mirrors the static-prop `gpu_cull_compute.cpp` integration but layered RENDER-side only.
- **C2 — Weighted multi-bone skinning:** shader-only ~10 line addition to `mech.vert` that reads all 4 boneIndices/boneWeights and computes a weighted bone matrix. Stock data passes byte-identical (`weights = 255,0,0,0` collapses to single-bone behavior); unblocks imported meshes.

## ⚠️ Critical invariant (load-bearing)

**Offscreen mechs MUST continue running their AI:** patrolling, attacking, taking damage, calling for help, responding to commands. Per worktree `CLAUDE.md` "Load-Bearing Cull Infrastructure" — the existing `inView`/`canBeSeen`/`objBlockInfo`/`objVertexActive` chain gates BOTH render AND update(). C1 adds an additional render-side gate ONLY. It must not interfere with `update()`, `objmgr` iteration, AI tick, or lifecycle (`setExists(false)`).

The existing `MC2_GPU_CULL_LIFECYCLE` env (mech3d.cpp:28) is the lifecycle-gating opt-in; it stays exactly as-is. C1's killswitch (`MC2_GPU_MECH_CULL`) is independent and only suppresses GPU-batcher submitActor calls.

## Architecture

### C1 — Mech GPU cull (render-only)

```
Mech3DAppearance::render()
    └─ if (mechShouldRender)                      [existing CPU cull pre-gate, untouched]
         if (visible)                              [existing CPU visible flag, untouched]
            recordEligibleActor()                  [existing batcher counter]
            // C1 render-skip gate (new)
            if (MC2_GPU_MECH_CULL && gpu_cull::readback_isActorVisibleLagged(actorHandle_) == false)
                return;     ← skip submitActor; CPU update/AI already ran this frame
            ...submitActor block...

ObjectManager::update() / AI / lifecycle  [unchanged: never gated by C1]
TG_MultiShape::TransformMultiShape()      [unchanged: cull-aware bone walk
                                           still driven by existing inView/etc.]
```

**Why render-only is safe:** The GPU cull readback is one frame stale (lagged). If we used it to gate `update()` we'd skip an entire frame of AI tick for actors the GPU just-marked-invisible — they'd freeze for one frame. By gating only the render submit, the actor's bones / position / AI tick run normally; we just don't draw what the GPU says was invisible last frame.

**Failure modes considered:**
- Stale readback says "invisible" but actor is actually visible this frame → one frame of missing render (imperceptible at 60Hz, recovered next frame).
- Readback unavailable / disabled → fail-open: we render normally (existing behavior preserved).
- Actor newly spawned → no readback record yet → `readback_isActorVisibleLagged` returns true (fail-open per existing semantics).

### C2 — Weighted multi-bone skinning

`mech.vert` currently:
```glsl
GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
```

After C2, gated on `u_skinningMode`:
```glsl
mat4 boneT;
if (u_skinningMode != 0) {
    boneT = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        float w = a_boneWeights[i];
        if (w > 0.0) {
            GpuMechBone bn = bones[a_boneIndices[i] + inst.baseBoneOffset];
            boneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
        }
    }
} else {
    GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
    boneT = mat4(b.row0, b.row1, b.row2, b.row3);
}
```

Stock data Slice A registration writes `boneWeights = (255, 0, 0, 0)` (= float 1.0, 0, 0, 0 normalized). The weighted branch evaluates `1.0 * mat4(bone[idx0]) + 0 * ... = mat4(bone[idx0])` — same as the rigid branch. Output byte-identical. Verified at the smoke gate by switch-toggle byte-compare.

Imported meshes (Track D Assimp pipeline) can ship multi-bone weights directly into the existing 48B vertex format; no engine change beyond what C2 already did.

## Killswitches

```
MC2_GPU_MECHS=1               # Slice A: GPU mech batcher path
MC2_GPU_MECH_LIGHTING=1       # Slice B1: VS-side calc_light
MC2_GPU_MECH_CULL=1           # Slice C1: render-side cull skip (NEW)
MC2_GPU_MECH_SKIN=1           # Slice C2: weighted multi-bone skinning (NEW)
```

C1 + C2 each independent; C1 requires `MC2_GPU_MECHS=1` to take effect (gate is inside the GPU-batcher path); C2 same. C1 is independent of `MC2_GPU_CULL_LIFECYCLE` (lifecycle gate is a separate, pre-existing concern).

## Verification gate (the user's "more verification" theme)

Slice B's verification gap was operator-visual smoke didn't cover gameplay-with-AI/damage/shutdown scenarios. C1 has a stronger version of that risk because it touches cull infra.

### Required verification before C1+C2 ships

1. **Tier1 5/5 PASS** at `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1`, +0 destroys, no GL errors.
2. **C2 byte-compare smoke:** identical screenshot at `MC2_GPU_MECH_SKIN=0` and `MC2_GPU_MECH_SKIN=1` on stock data — proves weighted-skin is a no-op for rigid input.
3. **Offscreen-AI verification:**
   - With `MC2_GPU_MECH_CULL=1`, run a mission where enemies spawn out of frustum (e.g. mc2_03 or mc2_24 has off-screen enemy patrols).
   - Verify enemies engage / patrol / damage when they come back into view — they should be in their expected position based on continuous offscreen AI tick, not in their spawn position.
   - Verify in-mission damage to offscreen mechs persists when they re-enter view.
   - Verify shutdown command on offscreen mech still executes (mech is in shutdown state when it comes back).
4. **CPU pre-cull preservation:** confirm existing `inView`/`canBeSeen` cull is unmodified. Read `mclib/objmgr.cpp` and the mech3d.cpp:2451 `mechShouldRender` logic — they must be byte-identical to pre-C1.
5. **`MC2_DESTROY_TRACE=1` clean:** no spurious `[DESTROY v1]` events. Cull-driven destruction would surface here; absence is the safety check.
6. **Adversarial review** post-implementation, with explicit scrutiny on:
   - Whether C1 cascades into `update()` anywhere.
   - Whether the readback API has hidden side effects on actor lifecycle.
   - Byte-identity of C2 stock-data output.

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | Two new externs |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | Two new env-var globals; `u_skinningMode` uniform plumbing |
| Modify | `mclib/mech3d.cpp` | C1 render-skip gate inside `Mech3DAppearance::render()` |
| Modify | `shaders/mech.vert` | `u_skinningMode` uniform; weighted bone branch |

No new files. No changes to `update()`, `recalcBounds()`, `objmgr.cpp`, or anything in the lifecycle/cull-load-bearing chain.

## Out of scope

- **C3 GPU bone-hierarchy compute:** explicitly deferred. Marginal CPU win for substantial complexity.
- **Cull-aware mech update gating:** that's `MC2_GPU_CULL_LIFECYCLE`'s job (separate, pre-existing, default off).
- **GPU LOD selection:** future slice; CPU `recalcBounds` is fine.
- **Animation pose interpolation on GPU:** state machine is awkward to lift to GPU.
- **B-suite default-on flip:** still gated on user verification of Slice B per `track_d_slice_b1_shipped.md`.
