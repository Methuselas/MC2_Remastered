# Unified-projection F1 Stage A shipped -- 2026-05-22

Atomic single commit: `59fae27` on `claude/nifty-mendeleev`.

## Scope

37 files changed, +1882 / -1510 lines.

| Category | Count | Detail |
|---|---|---|
| Shader migrations | 16 modified | 13 vert + 1 geom + 1 tese + 2 comp |
| Shader deletions | 3 | ssao.frag, ssao_apply.frag, ssao_blur.frag |
| CPU producer + GOS API + cache | 1 | gameos_graphics.cpp (gos_SetWorldToClipGL + retire gos_SetTerrainMVP) |
| CPU bind site renames | 5 files | gameos_graphics.cpp + 4 batcher/bridge files (10 glGetUniformLocation + 2 setMat4Direct) |
| CPU producer call site | 1 | gamecam.cpp (collapse inline AW to single accessor call) |
| Header decl retire | 1 | gameos.hpp (gos_SetTerrainMVP + gos_SetWorldToClipGLProbeUBO retired) |
| Diagnostic retirement | 4 | gameosmain.cpp (probe helpers), mission.cpp (reset hook), CMakeLists.txt (option), code/unified_proj_basis_test.cpp (deleted) |
| Infra | 4 | run_smoke.py env assert, CLAUDE.md known issue, scripts/check-unified-projection-retirement.{sh,allowlist} |
| Spec + doc | 1 | F1 plan added to tree |

## Gate evidence

### Pre-ship (probe build with MC2_TERRAIN_INDIRECT=0)

| Gate | Result |
|---|---|
| Phase 0 Task 3 CPU basis-vector | max_delta = 0.0 |
| Phase 1+2 tier1 5/5 A-pre | 5/5 PASS, behind_new_only/compared <= 0.054%, nonfinite_new_only = 0 |
| Phase 2 user-driven mc2_10 wolfman canary | 1.4B compared verts, R-clipw ratio = 0/1.4B = 0.000 |

### Post-ship (default env, no probe)

| Gate | Result |
|---|---|
| Tier1 5/5 default env (Stage A shipped) | 5/5 PASS, 137-142 fps, +0 destroys, no GL errors |
| scripts/check-unified-projection-retirement.sh | PASS first run, no allowlist exceptions needed beyond shadow/HUD/MLR |
| /mc2-amd-shader-review skill | PASS (4 minor doc-staleness items, all fixed before final commit) |
| Build (clean, --config RelWithDebInfo) | PASS (warnings only: pre-existing C4267 + LNK4199 delay-load) |

## Architecture (canonical end-state)

```
Camera::worldToClipGL()     (mclib/camera.cpp)
        |
        v
gos_SetWorldToClipGL(mat)   (GameOS/gameos/gameos_graphics.cpp)
   - column-major Stuff -> row-major M repackage
   - writes terrain_mvp_ cache
        |
        +--------> 15+ cache readers via gos_GetTerrainMVPMat4()
        |              (CullUBO, mech-batcher, static-prop-batcher,
        |               particle-bridge, etc. -- inherit transparently)
        |
        v
10 CPU bind sites + 2 setMat4Direct calls
        |
        v
16 shaders: gl_Position = u_worldToClipGL * vec4(world, 1.0)
   - direct emit, no abs(clip.w), no screen-rhw round-trip,
     no terrainViewport, no bare mvp
```

R-clipw polarity correction is folded into `kAxisSwapMC2toGL` literal so in-front MC2 vertices produce `clip.w > 0`, passing the hardware clip-volume test under direct emit.

## Out of F1 (deferred per spec §8)

- Camera::projectZ body + 8 wrappers (consume separate `worldToClip` member; own retirement arcs)
- CPU pz gate in mclib/quad.cpp (red-band class blocker per `projectz_overlay_findings`)
- MLR (mitigation (c) -- dev-override regression accepted; runtime guard warns on MC2_DISABLE_GOSFX=0)
- TGL class-statics (0us per F3; cleanup if surfaces)
- Editor convergence (separate worktree)

## Corrections during execution (durable learnings)

Plan v1.1 was correct in architecture but two sub-systems needed structural correction during execution:

1. **AMD TES uniform propagation** (Task 7b/7c, durable memory `amd_tes_uniform_propagation_unreliable.md`): `glUniformMatrix4fv` and std140 UBO `glBindBufferBase` do NOT reliably propagate to tessellation evaluation shaders on AMD RDNA3. SSBO writes DO reach TES (proven by atomicAdd counter accumulation). A-pre probe used SSBO stash; Stage A reverted to plain `uniform mat4` because production has a single TES program plus explicit-program upload on the 10 CPU bind sites -- uniforms reach reliably under that configuration (verified by tier1 5/5 default-env pass).

2. **R-clipw polarity** (Task 7e/7f/7g, addendum-rclipw-polarity.md): MC2's `cameraToClip(FORWARD_AXIS=2, col=3) = +1.0f` (mclib/camera.cpp:1943) combined with the camera's `-z_eye` forward convention produces `clip.w = z_eye < 0` for in-front vertices. Legacy rendering survived via shader-side `abs(clip.w)`. F1's clean direct emit cannot tolerate negative clip.w (hardware clip-volume test rejects pre-perspective-divide). Fix: localized scalar negation of the worldToClipGL product, folded into `kAxisSwapMC2toGL` literal. NDC is invariant: `(-xyz)/(-w) = xyz/w`. The fix is scoped -- `cameraToClip` is untouched, so CPU `projectZ`, the 8 wrappers, and MLR (mitigation (c)) all see unchanged behavior.

## Side effects observed

User reports during A-pre canary work (not yet measured rigorously, captured here for downstream investigation):

- Water rendering during camera pans appears improved (less z-fight on surface visibility at pan boundaries). Cause unclear -- possibly the parallel A-pre matrix upload synchronized state that wasn't synced in legacy.
- Skybox rendering returned to light-blue (was previously darker).
- Out-of-bounds areas (where the fog/extra-terrain should be) returned to black (was white in some prior states).

None of these are F1 design intent; they appear to be incidental benefits of the new producer's clean state. Recommend monitoring post-ship to confirm durability.

## Follow-ups

- Stage A retired the probe + diagnostic SSBO infrastructure entirely. Future probe-based validation can re-introduce a fresh SSBO at the next free std430 binding (current free starts at 25; 23 and 24 are now retired).
- Plan-stage decision on which arc retires the 8 CPU projection wrappers next.
- Track whether MLR mitigation (c) needs revisiting if dev-override users complain.
- Worktree CLAUDE.md known-issue entry can be removed when MLR retirement Slices 1-5 ship.
