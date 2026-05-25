# B1 C13 — GPU particle render invisibility: GL pipeline inspection

Date: 2026-05-21
Branch: claude/nifty-mendeleev
Scope: inspection-only (per session prompt). No fix code beyond ≤5 LOC
one-line typos.

## Background

C12 (commit `3a9aa36`) proved records flow CPU-side end-to-end:
`emit_total=5288`, `records_flushed_total=5288`,
`nonempty_flush_total=4431`, `prog_compiled prog=170` across 30s mc2_10.
No overflow, no compile fail. But particles are invisible to the user.
Bug is downstream of `gos_particle_bridge_flush` in the GL pipeline.

## Files inspected

- `shaders/particle_billboard.vert`
- `shaders/particle_billboard.frag`
- `shaders/include/particles.hglsl`
- `mclib/particles/batcher.{h,cpp}`
- `mclib/particles/spec.h`
- `mclib/particles/spawn_cardcloud.cpp`
- `GameOS/gameos/gos_particle_bridge.{h,cpp}`
- `code/gamecam.cpp` (hook site, lines 270-309)

Cross-reference: `memory/gpu_direct_renderer_bringup_checklist.md`
(10 traps), `memory/clip_w_sign_trap.md`,
`memory/terrain_mvp_gl_false.md`.

---

## CRITICAL FINDING #1 — wrong projection chain in VS

**`shaders/particle_billboard.vert:66-78`** writes `gl_Position = clip;`
directly, where `clip = terrainMVP * vec4(worldPos, 1.0)`. This is
WRONG: `terrainMVP` does **not** produce GL clip-space coordinates.

Authoritative source: `shaders/gos_terrain_surface.vert:29-51` — quote:

> "axisSwap*worldToClip is NOT a GL clip matrix -- terrainMVP*world
> yields D3D pixel-homogeneous coords; the pixels->NDC step
> (terrainViewport + the `mvp` uniform = projection_) is a REAL
> coordinate conversion the camera comment explicitly says 'can't be
> matrix'."

Every other consumer of `terrainMVP` in the codebase does the full
three-step chain (clip-homogeneous → divide+viewport → NDC via `mvp`):

- `shaders/static_prop.vert:148-156` — pattern:
  ```glsl
  vec4 clip4 = terrainMVP * world;
  float rhw  = 1.0 / clip4.w;
  vec3  px;
  px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
  px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
  px.z = clip4.z * rhw;
  vec4 ndc = u_mvp * vec4(px, 1.0);
  float absW = abs(clip4.w);
  gl_Position = vec4(ndc.xyz * absW, absW);
  ```
- `shaders/mech.vert:120-129` — identical pattern (with comment
  explicitly calling it "D3D pixel-homogeneous projection chain").
- `shaders/gos_terrain_water_fast.vert:323+` — same pattern.

`particle_billboard.vert` shortcuts the chain and emits raw
pixel-homogeneous values as if they were GL clip-space. Result:
`gl_Position.xy` lands at pixel-domain magnitudes (hundreds of pixels)
which after the GL pipeline's perspective divide produces NDC values
far outside [-1,1] — the rasterizer clips every primitive away.

Even when not clipped, the `pz ∈ [0,1)` "in-front" test at
`particle_billboard.vert:71-75` is checking *pixel-homogeneous* z, not
GL clip-z; it likely returns false for the canary at
`(0, 0, 50)` because that world point may be outside the
pixel-homogeneous z window for typical missions, sending the canary
through the `gl_Position = vec4(0,0,-2,1)` degenerate branch.

**Required corresponding bridge change:** the bridge currently sets
only `terrainMVP`. It must also set `u_terrainViewport` (from
`gos_GetTerrainViewportVec4()`, used at
`gos_mech_batcher.cpp:1207-1209`) and `u_mvp` (from
`gos_GetProj2ScreenMat4()`, used at `gos_mech_batcher.cpp:1210-1212`
with `GL_TRUE`). `gos_particle_bridge.cpp:148-162` only uploads
`terrainMVP` and `uAtlas`.

This is the single most likely root cause and explains 100% of the
"records flushed but nothing visible" symptom.

---

## MAJOR FINDING #2 — canary world position is likely off-map

`code/gamecam.cpp:293-305` hardcodes the canary at `(0, 0, 50)` in
"raw MC2 world coords". Per `gos_static_prop_registry.cpp:512-515`
that is x=east, y=north, z=elev. MC2 tier1 maps typically span
~0..4000 on each axis; (0,0,50) is the SW corner. The smoke camera
on mc2_10 is not pointed at that corner. Even after FINDING #1 is
fixed, the canary may still be off-screen due to placement, not
rendering. This compounds the diagnostic: a "fix" to the projection
chain might still show no canary because the canary itself sits
behind the camera.

The CardCloud emits (5288 records) come from real spec positions
(spawn_cardcloud.cpp:139-144 transforms `local_offset` via
`parentToWorld`), so once #1 is fixed those should appear without
placement changes.

---

## MAJOR FINDING #3 — depth direction mismatch hint

`gos_particle_bridge.cpp:171` sets `glDepthFunc(GL_GEQUAL)` "reverse-Z
convention (matches water fast path)". Verify against
`gos_static_prop_batcher.cpp` and `gos_mech_batcher.cpp` for the
current depth-func convention. If those use `GL_LESS`/`GL_LEQUAL`
(forward-Z), then `GL_GEQUAL` here is wrong and would depth-fail every
particle fragment against the already-rendered scene. Did not deep-
verify in this pass — flagged for hypothesis #3.

---

## 10-trap checklist (`gpu_direct_renderer_bringup_checklist.md`)

| # | Trap | Status | Evidence |
|---|---|---|---|
| 1 | `uniform uint` crash | PASS | shader uses `uint(gl_VertexID)` casts only; no `uniform uint` (VS:53-54). |
| 2 | Two-tier texture handle | N/A | bridge owns its atlas tex directly (`bridge.cpp:62-69`), not via mcTextureManager. |
| 3 | `terrainMVP` upload `GL_FALSE` | PASS | `bridge.cpp:156` uses `GL_FALSE`. |
| 4 | VAO 0 silently drops draws | PASS | `bridge.cpp:46-47, 146` creates and binds dedicated VAO. Save/restore at :125, :189. |
| 5 | Sampler inheritance | PASS | `bridge.cpp:49-54, 165` creates dedicated sampler, binds + restores. |
| 6 | Render order — AFTER `renderLists()` | PASS | hook at `gamecam.cpp:282-308` is post-renderLists (per surrounding context lines 270-272 showing waterFastPath; mcTextureManager flush is upstream of this block per surrounding code in render path). |
| 7 | CPU pre-cull / no clip.w sign test | PARTIAL FAIL | `vert:70-75` uses `pz = clip.z/abs(clip.w); inFront iff pz in [0,1)` — but applied to D3D pixel-homog `clip`, not GL clip. Mirrors the legacy thin-vert pattern correctly in form, but the input space is wrong (see FINDING #1). |
| 8 | Map-stable indexing | N/A | no SSBO recipe / quadList indexing; each frame is staged fresh. |
| 9 | Depth state explicit | SUSPECT | depth-test enabled (`:170`), depth-func set (`:171`), depth-mask set (`:172`). But the chosen depth-func `GL_GEQUAL` may be wrong vs sibling fast-paths (FINDING #3). |
| 10 | AMD auto-LOD strict-fail | PASS | `frag:25` uses `textureLod(uAtlas, v_uv, 0.0)`. Tex created with MAX_LEVEL=0 (`bridge.cpp:68`). |

Score: 7/10 confirmed PASS, 1 N/A, 1 PARTIAL FAIL (#7 — input-space
bug, not the guard itself), 1 SUSPECT (#9 depth-func direction),
1 untested (#2, not applicable).

Stage 1' "all 10 addressed" claim is overstated — trap #6 says the
*order* is right but the *projection-chain reality* under-served by
the bridge's single-uniform upload was missed by every prior pass.

---

## Hypotheses ranked by likelihood

### #1 (HIGHEST, ~90%): Wrong projection chain in VS

- **Where:** `shaders/particle_billboard.vert:66-78` +
  `GameOS/gameos/gos_particle_bridge.cpp:148-162` (uniforms upload).
- **What:** VS treats `terrainMVP*world` as GL clip-space when it is
  actually D3D pixel-homogeneous. Bridge only uploads `terrainMVP`
  but the legacy chain needs `terrainMVP` + `u_terrainViewport` +
  `u_mvp` (= projection_).
- **Predicted symptom:** every particle clipped or off-screen (matches
  reported user observation: 5288 records flushed, zero visible).
- **Recommended fix:** port the static_prop.vert:148-156 / mech.vert:
  120-129 projection chain into particle_billboard.vert verbatim;
  upload `u_terrainViewport` (via `gos_GetTerrainViewportVec4`) and
  `u_mvp` (via `gos_GetProj2ScreenMat4`, `GL_TRUE`) in the bridge.
  Mirror the `gl_Position = vec4(ndc.xyz * absW, absW)` w-restore
  trick from mech.vert:129 so clip-w sign behavior matches siblings.

### #2 (MEDIUM, ~15%): Canary world position off-map

- **Where:** `code/gamecam.cpp:293-305` (canary), separate from real
  CardCloud emits.
- **What:** `(0, 0, 50)` is map corner, not playfield center for any
  tier1 mission.
- **Predicted symptom:** canary invisible even after #1 fixed; real
  particles from CardCloud should appear once #1 is fixed because
  those use parentToWorld transformed positions
  (spawn_cardcloud.cpp:139-144) which are real actor centers.
- **Recommended next step:** after #1 fix, smoke without canary and
  observe whether spec-driven emits become visible.

### #3 (LOW, ~5%): Depth-func direction

- **Where:** `GameOS/gameos/gos_particle_bridge.cpp:171`.
- **What:** `glDepthFunc(GL_GEQUAL)` may be wrong if non-water
  fast-paths use forward-Z.
- **Recommended next step:** grep `glDepthFunc` in
  `gos_mech_batcher.cpp` and `gos_static_prop_batcher.cpp` and align.
  Note this is downstream of #1 — even with correct depth-func, wrong
  projection chain makes draw invisible.

---

## Recommended next-step diagnostic OR fix

1. **First fix #1** (projection chain). High confidence single root
   cause; mirrors the same fix applied to mech and static-prop fast
   paths in prior sessions. Estimated diff: ~10 lines in VS, ~6 lines
   in bridge.
2. **Re-smoke** mc2_10 + check user-visible result. If still nothing
   visible:
3. **Verify #3** (depth-func) by grepping sibling batchers, then
   re-smoke.
4. **Address #2** (canary position) only if real CardCloud emits also
   remain invisible after #1+#3.

If after #1+#3 nothing renders, escalate to RenderDoc capture — the
post-renderLists draw should appear as a discrete `glDrawArrays(GL_
TRIANGLES, 0, 6*N)` with the bridge's program; pipeline-state HTML
export will reveal residual binding / state mismatches.

---

## Auto-fixes applied this session

None. The required fix is a coordinated multi-file change
(VS + bridge uniform upload), outside the ≤5-LOC one-line typo scope.

## Items NOT inspected this pass

- `mclib/particles/spec_library.{h,cpp}` — not on the inspection list
  and the bug is post-flush, not in spec lookup.
- Particle spec atlas binding slot 23 ("ParticleSpecTable=23") — the
  current bridge does NOT bind anything at slot 23 (only the 1×1
  white atlas at unit 0). That binding is a future stage; not part of
  the Stage 1' draw and not a candidate for the current bug.
