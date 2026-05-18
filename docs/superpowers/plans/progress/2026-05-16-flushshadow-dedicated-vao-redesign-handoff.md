# flushShadow Dedicated-VAO Redesign - Handoff

Date: 2026-05-16
Branch: claude/gpu-driven-rendering
Status: HEAD `da11f63` is STABLE. Dynamic GPU-batcher shadow path is
DEFAULT-OFF (opt-in `MC2_SHADOW_ENABLE=1`). This doc is the contract to
finish Phase 1 correctly.

## Where Phase 1 stands

Done + committed, reviewed, stable:
- Task 1 shaders + loader (`96dcf48`): `shadow_mech.vert`,
  `shadow_static_prop.vert`, `shadow_instanced.frag`, program
  registration. Spec+quality PASS. Loads clean (`[SHADOW_MECH] prog=..`,
  `[SHADOW_STATIC_PROP] prog=..`).
- Task 2 frustum-fit `buildDynamicLightMatrix` (`4a0e70d`+`143f7b8`):
  clipToWorld unproject + Stuff->MC2 swizzle, fixed elevation bounds +
  map-bounds clamp, pow2 anti-shimmer, [0,1] z-row + up-hint guard
  preserved. Spec+quality PASS.
- Task 3 `GpuStaticPropBatcher::flushShadow` non-indirect full-range
  draw (`c214d93`). Task 4 `GpuMechBatcher::flushShadow` previous-fenced
  ring slot (`f224bc7`+`53de050`). Both spec+quality PASS as written.
- Task 5 region (concurrent session, uncommitted `mclib/txmmgr.cpp`
  WIP): builds the fit matrix, `gos_BeginDynamicShadowPass`, calls both
  `flushShadow`, `gos_EndDynamicShadowPass`, BEFORE
  `gpu_cull::compute_dispatch()`.
- Crash-cascade defensive work kept (`18f847e` finalized guards +
  bounds checks; `c313a32` explicit IBO bind; `da11f63` default-off
  gate + save/restore bracket + `MC2_SHADOW_DIAG` probe).

Static-shadow path (`0c421d1`, [0,1] ZERO_TO_ONE) is untouched and
correct - OUT OF SCOPE.

## The architecture defect (root cause of the crash cascade)

`flushShadow` (both batchers) reuses the live batchers' `s_sharedVao`.
`GL_ELEMENT_ARRAY_BUFFER` binding is **VAO state**. Any element-buffer
bind/restore on the shared VAO corrupts the element binding the main
indirect `flush()` indexed draws depend on -> the driver dereferences
`firstIndex*sizeof(uint32_t)` as a client pointer (crash addresses
0x2A18 = 2694*4, 0x17AC = 1515*4). Patching bind/restore order only
relocated the corruption (flushShadow crash -> invisible props/mechs ->
main flush() crash). `MC2_SHADOW_DISABLE` bisect proved flushShadow is
the sole cause. Full reasoning:
`memory/element_array_buffer_is_vao_state_new_draw_paths_own_their_vao.md`.

## The fix (next slice) - dedicated shadow VAO

`flushShadow` must NEVER touch `s_sharedVao`. Create a private VAO per
batcher (`s_shadowVao`), configured ONCE after `finalizeGeometry()`:

1. Grep `finalizeGeometry()` in each batcher for the exact vertex-attrib
   setup on `s_sharedVao` (the `glVertexAttribPointer`/format calls,
   stride, offsets, types) and the `s_sharedVbo`/`s_sharedIbo` glGen.
2. Create `s_shadowVao`; bind it; bind `s_sharedVbo` as its array
   buffer; replicate ONLY the attribs the shadow `.vert` consumes
   (static-prop: loc0 position; mech: loc0 position, loc3 boneIndices,
   loc4 boneWeights - confirm against `shadow_mech.vert`/
   `shadow_static_prop.vert`); bind `s_sharedIbo` as its element buffer.
   This makes the IBO VAO-resident on the PRIVATE VAO. Recreate it in
   `finalizeGeometry` / after `finalizePending` (mech) and delete it in
   `onMapUnload` alongside `s_sharedVao`.
3. `flushShadow`: `glBindVertexArray(s_shadowVao)` ONLY. NO
   `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...)` (it is now VAO-resident
   and isolated). Bind SSBO ranges, set uniforms, draw.
4. Restore minimal GLOBAL (non-VAO) state only: program -> prevProgram,
   SSBO binding points 0(/1) -> prevSsbo (these are not VAO state and
   compute_dispatch/flush rely on them). Do NOT touch
   `GL_ELEMENT_ARRAY_BUFFER` or `s_sharedVao` at all. `glBindVertexArray`
   back to 0 (or prevVao) is safe since the private VAO is isolated.
5. Remove the now-unnecessary `s_sharedIbo` explicit binds and the
   element-buffer save/restore added during the cascade (they exist
   only because of the shared-VAO mistake). Keep the finalized guard +
   bounds checks (correct defense) and `MC2_SHADOW_DIAG` (probe).
6. Flip the gate back to default-ON once verified (remove the
   `MC2_SHADOW_ENABLE` opt-in, or invert to a kill-switch).

## Verification (mandatory before default-on)

- Build RelWithDebInfo full relink; deploy v0.4.
- `scripts/quick_shot.py` style screenshot (override exe path to
  `mc2-win64-v0.4`) on a NO-INTRO mission - `mc2_24` confirmed
  intro-free and is the chosen visual-verify mission. Confirm:
  props/buildings + mechs + terrain all render (no invisible-caster
  regression) AND a dynamic shadow is visibly cast by mechs/buildings.
- Crash repro missions: `mc2_01` (was frame-2 0x2A18) and `mc2_03`
  (was frame-1823) must run 1000+ frames, zero `Crash Report`.
- tier1 gate (`run_smoke.py --tier tier1 --duration 30`).
- `MC2_SHADOW_DIAG=1` one run: `elemBind` non-zero & stable across
  frames (the original tell), instance counts > 0.
- Mandated adversarial review of the diff (catastrophic-axis;
  spec+quality, grep-closed) before flipping default-on.

## Remaining Phase 1 tail (after the redesign verifies)

- Task 6: replace/fold `MC2_SHADOW_DIAG` into the planned
  `[SHADOWFIT v1]` env-gated parity probe (spec
  `2026-05-16-gpu-driven-dynamic-sun-shadow-design.md` rev 3).
- Task 7: user/screenshot visual sign-off; file the Phase 2
  light-volume caster-cull follow-up in
  `docs/superpowers/VPL-RETIREMENT-DEFERRED.md` (off-screen-caster
  low-sun gap, do NOT touch the `inView` cull cascade).

## Coordination

`mclib/txmmgr.cpp` (the Task-5 region) is the concurrent session's
uncommitted WIP - it owns that file's commit. The redesign above is
entirely within the two batcher .cpp/.h; no txmmgr change needed.
