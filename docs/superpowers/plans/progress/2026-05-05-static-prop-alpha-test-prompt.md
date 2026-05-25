# Prompt — Static-prop GPU path: chainlink fence renders solid instead of alpha-tested

**Date:** 2026-05-05
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Predecessor session:** Slice 2 GPU draw axis-swap fix (2026-05-04 → 05); buildings/trees now render at correct positions. Two latent visual gaps surfaced once geometry was visible — this prompt covers the alpha-test gap.

---

## Symptom

In `mc2_06` (and other missions with chainlink fences), fence assets that should render as a sparse mesh of wire (transparent gaps between strands) instead render as a **solid opaque rectangle** in the GPU static-prop path. Other alpha-tested props (trees) appear to render correctly. Verify against legacy CPU path by toggling RAlt+0 (`g_useGpuStaticProps` killswitch); CPU path should show the fence with proper transparency.

## Background

- Legacy MC2 uses `gos_State_AlphaTest` (not alpha-blend) for static props. See `mclib/tgl.cpp:2891`:
  ```cpp
  gos_SetRenderState( gos_State_AlphaTest, theShape->alphaTestOn);
  ```
- The GPU offload path mirrors this with a per-packet flag. See `GameOS/gameos/gos_static_prop_batcher.cpp:601`:
  ```cpp
  pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
  ```
- The fragment shader does the discard at `shaders/static_prop.frag:51-53`:
  ```glsl
  if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
      discard;
  }
  ```
- The render contract at `static_prop.frag:18-19` declares `blend=Opaque`. The flush() explicitly does `glDisable(GL_BLEND)` at `gos_static_prop_batcher.cpp:1355`. **No alpha blending is ever applied — alpha test is the only transparency mechanism on this path, matching legacy.**

## Hypotheses (in order of likelihood)

### (A) `typeShape->alphaTestOn` is false at type-registration time for fence shapes
The packet's `materialFlags` is set ONCE during `registerType()` from `typeShape->alphaTestOn`. If `alphaTestOn` is set later (e.g. by a per-mission init), the GPU packet has stale `materialFlags=0` for the rest of the run and the shader's discard never fires.

**To verify:**
- Find the fence's `TG_TypeShape` (likely tagged with a recognizable type name — search for "fence" / "chainlink" in `objects/` data or grep the `s_types[].sourceName` if such a field exists).
- At `gos_static_prop_batcher.cpp:601`, log `typeShape->alphaTestOn` and the resolved `materialFlags` for the offending typeID.
- Cross-check against `tgl.cpp:606` (`alphaTestOn = false;` default in TG_TypeShape ctor) and grep for where it's later set to `true` — ensure that path runs before the type registers with the GPU batcher.

**Fix sketch if this is the cause:**
- Either delay GPU type registration until first draw (when `alphaTestOn` is guaranteed final), or
- Re-resolve `materialFlags` at draw time the same way `gosTextureHandle` is re-resolved at `gos_static_prop_batcher.cpp:1538-1542`. This would require carrying `typeShape*` (or its index) into the per-packet record, which is already there as `pkt.owningTypeID` → `s_types[id].source`. Per-draw cost: one bool read.

### (B) Texture alpha channel isn't being uploaded
GL_RGB-only texture upload would deliver `tex_color.a == 1.0` to the shader, making the discard condition unreachable.

**To verify:**
- Capture in RenderDoc: examine the bound texture for a fence draw. Does it have an alpha channel? Is alpha varying?
- Or: temporarily replace the discard condition with `if (... && tex_color.a < 1.0)` — if pixels start vanishing on fences, alpha IS varying. If still solid, alpha is uniformly 1.0 (channel missing).

**Fix sketch:** find the texture upload site for static-prop textures (likely `mc2_GosTextures` / `gos_LoadTexture` path); confirm RGBA is being requested for `alphaTestOn` shapes.

### (C) Threshold mismatch
Legacy `gos_State_AlphaTest` may use a threshold other than 0.5 (e.g. > 0 = "any non-zero alpha passes," or 0.75/255 ≈ 0.5 with a specific reference). With anti-aliased / mipmap-filtered fence textures, soft alpha edges might consistently fall above 0.5 and render solid.

**To verify:** find where `gos_State_AlphaTest` is implemented in `GameOS/gameos/gameos_graphics.cpp` and confirm the comparison/threshold. If it's `> 0` rather than `>= 0.5`, the GPU path is rejecting more pixels than legacy.

**Fix sketch:** match legacy threshold exactly. Probably `tex_color.a < (1.0/255.0)` (i.e. only zero alpha discards) or some other low value.

## Out of scope

- Adding alpha *blending* to the GPU path. Legacy doesn't blend static props; we don't need to either.
- Sorting / multi-pass split. Alpha-test is order-independent and writes depth normally.
- Per-vertex alpha. The shader already reads `tex_color.a` from the texture; per-vertex alpha would be a separate, larger feature.

## Verification recipe

1. **Identify a clean fence test mission.** The `mc2_06` airstrip area shows fences. Check the camera position memory or just run mc2_06 and pan toward the airstrip border.
2. **Build/deploy** per [worktree CLAUDE.md](../../../../CLAUDE.md) (`/mc2-build-deploy`). Force full link if you flipped any global initializer (`memory/msvc_incremental_link_silent_staleness.md`).
3. **Visual diff:**
   - Launch with `g_useGpuStaticProps = true` (default) — fence should now show wire-mesh transparency.
   - Toggle RAlt+0 to force CPU path — verify fence looks **the same** (or visibly close — slight differences in lighting/normal computation are tolerable; transparency mesh pattern is the comparison axis).
4. **Smoke gate:** `py -3 scripts/run_smoke.py --tier tier1 --kill-existing` — exit 0.

## Files to read first

1. `mclib/tgl.cpp:2880-2950` — legacy alpha-test render setup.
2. `mclib/tgl.h:570-680` — TG_TypeShape::alphaTestOn declaration and setter.
3. `GameOS/gameos/gos_static_prop_batcher.cpp:540-610` — registerType packet generation including materialFlags.
4. `GameOS/gameos/gos_static_prop_batcher.cpp:1531-1552` — per-packet draw including materialFlags upload.
5. `shaders/static_prop.frag` — fragment shader, the alpha-test discard logic.
6. `GameOS/gameos/gameos_graphics.cpp` — search for `gos_State_AlphaTest` to find legacy threshold semantics.

## Communication style

User wants: direct, no catastrophizing, one fix at a time. Run the diagnostic (probably just a printf in `registerType()` printing `typeShape->alphaTestOn` per type, to settle hypothesis A) before changing render code. Do not pile on shader edits without measurement first — the predecessor session burned hours on guess-fixes for a coordinate bug that ground truth would have settled in 5 minutes.

This is a **slice-3-class follow-up** — narrow scope, single visual feature, one source of truth (legacy CPU path). Should ship in one session.
