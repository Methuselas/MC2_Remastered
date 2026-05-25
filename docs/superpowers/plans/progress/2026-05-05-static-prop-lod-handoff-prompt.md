# Prompt — Static-prop GPU path: buildings render with legacy/low-LOD textures instead of upscaled overrides

**Date:** 2026-05-05
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Predecessor session:** Slice 2 GPU draw axis-swap fix (2026-05-04 → 05); buildings/trees now render at correct positions. Two latent visual gaps surfaced once geometry was visible — this prompt covers the texture-fallback gap.

---

## Symptom

Buildings rendered through the GPU static-prop offload path show **stock/low-LOD textures**, not the 4x upscaled textures shipped under `data/art_4x_gpu/` and `data/tgl_4x_gpu/` (per `memory/MEMORY.md` "Texture upscaling" section). Toggling RAlt+0 to force the legacy CPU path produces the upscaled textures. Same exe, same mission, same texture files on disk — only the offload path is affected.

This is the **documented LOD handoff bug** flagged in `memory/mech_paint_and_mipmap_system.md` ("LOD handoff bug unfixed"), now reproducible with visible geometry.

## Background

- MC2 `TG_TypeShape::listOfTextures[]` carries texture *slots*, not handles. Each slot's `gosTextureHandle` is **mutated per-frame per-actor** by `TransformMultiShape::SetTextureHandle` (called from `mech3d.cpp:4170`, `gvactor.cpp:2702`, `tgl.cpp:1321`, etc.). The mutation selects the appropriate handle for distance/LOD/paint variation — **including the upscale-override file's handle**, when present.
- The GPU offload path was deliberately designed to **bypass `TransformMultiShape`** — that's the whole point of "object offload" (move expensive per-actor CPU work onto the GPU). See `memory/MEMORY.md` "BldgAppearance LOD swap is unsafe for animated buildings" and the slice 2 architecture docs.
- The flush() resolves the texture handle at draw time:
  ```cpp
  // gos_static_prop_batcher.cpp:1538-1542
  uint32_t gosHandle = 0;
  const TG_TypeShape* src = type.source;
  if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
      gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
  }
  ```
  But `listOfTextures[].gosTextureHandle` is only **last-set** by whatever `TransformMultiShape` happened to run most recently — for actors fully owned by the GPU path, that's never. The handle stays at registration-time default (typically the stock-FST handle).
- Confirmed by reading the comment at `gos_static_prop_batcher.cpp:1533-1537`:
  > "MC2 mutates `TG_TypeShape::listOfTextures[slot].gosTextureHandle` each frame in `TransformMultiShape` (msl.cpp:1321 via `SetTextureHandle`), so capturing the handle at registration time gives stale (usually zero) reads."
  
  Resolving at draw time picks up the current value — but the current value is never updated for offloaded actors.

## Hypotheses (in order of likelihood)

### (A) GPU offload skips TMS, so handle is never updated to upscale-override
**To verify:**
- Add a printf in flush() at the texture resolution site logging `gosHandle` for a known fence/building type, once per run.
- Add a printf at `TG_Shape::SetTextureHandle` (or whatever sets `listOfTextures[i].gosTextureHandle`) gated on the same type — confirm it's NOT being called for offloaded actors during normal play.
- Cross-check against the upscale-override system: `mclib/file.cpp` (or wherever `File::open` lives) should report which path was used for a given TGA. If the upscale `.tga` exists on disk under `data/art_4x_gpu/`, MC2 should hit it. The handle MC2 hands back from that load is what should reach the SSBO — but only if SOMETHING calls `SetTextureHandle` for the offloaded actor.

### (B) The upscale-override system pre-resolves at load time and the GPU path needs to query that resolved handle differently
- Read `pack_mat_normal.py` and the upscale loader (search for "art_4x_gpu" callers in C++) to confirm whether upscaled textures register a *new* gosHandle vs. mutating the stock handle in place.
- If they register new handles and a lookup table maps `(stock_handle → upscale_handle)`, the GPU path could just resolve through that table at draw time without reviving TMS.

## Fix paths (write the recon **before** committing to one)

### Path 1 — Run a stripped TMS purely for the SetTextureHandle side effect
- Lowest disruption. Loop over offloaded actors before flush(); call a TMS variant that ONLY touches `listOfTextures[i].gosTextureHandle`, skipping the expensive transform/lighting bake.
- Risk: TMS has lots of side effects beyond handle update (per `memory/cull_gates_are_load_bearing.md`, `memory/tgl_pool_exhaustion_is_silent.md`). A "stripped" TMS is a maintenance trap unless tightly scoped.
- Cost: per-frame O(actors) CPU work — reintroduces some of what slice 2 offloaded.

### Path 2 — Replicate the LOD/handle-selection logic in submit() (or per-flush)
- Cleanest separation. Read the handle-selection rules once (likely in `tgl.cpp` SetTextureHandle), implement them as a small pure function, call it at submit time and stash the resolved handle directly in the SSBO instance record.
- Per-instance SSBO struct grows by a uint (or expand `firstColorOffset` field if there's bit headroom). Coordinate with `cpp_glsl_ubo_struct_lockstep.md` — any C++ struct change MUST mirror in `static_prop.vert` lockstep, or per-element stride breaks for `arr[i>0]`.
- Cost: one-time recon of SetTextureHandle's rules, then runtime is a per-submit-only resolve (cheap).

### Path 3 — Sidestep: have the upscale override register its handle in the same slot as the stock texture at load time, so the post-registration `gosTextureHandle` is already the upscale handle
- If the upscale loader can be made to write into `listOfTextures[i].gosTextureHandle` at game startup (after texture registration but before any rendering), the GPU path would naturally pick it up via its existing draw-time resolve — no TMS needed.
- Risk: if the upscale handle is ALSO used by the legacy CPU path, mutating in place might break it. Verify CPU path still works after this change.

## Out of scope

- Distance-based LOD swap (separate problem; GPU path uses LOD0 always per `memory/bldg_animation_lod_swap_unsafe.md` for animated buildings).
- Mech upscaling (mechs go through a different render path; mech3d.cpp).
- Mipmap generation / filter selection (`gosHint_MipmapFilter0` is its own opt-in axis, see `memory/mech_paint_and_mipmap_system.md`).

## Verification recipe

1. **Pick a test building** with visible texture detail and a confirmed upscale override on disk. The `data/art_4x_gpu/` folder should make it obvious which TGA path is the override.
2. **Side-by-side comparison:**
   - GPU path (default): screenshot the building.
   - CPU path (RAlt+0): screenshot the same building from same angle.
   - Pixel-level diff or visual comparison should make the texture difference obvious.
   - After the fix: GPU path should match CPU path's texture quality.
3. **Smoke gate:** `py -3 scripts/run_smoke.py --tier tier1 --kill-existing` — exit 0.
4. **Memory churn check:** if Path 1 is taken, watch Tracy/`drawPass` numbers. If they regress significantly, reconsider — the whole point of slice 2 was to NOT do per-actor CPU work.

## Files to read first

1. `memory/mech_paint_and_mipmap_system.md` — origin of the LOD handoff bug.
2. `memory/MEMORY.md` "Texture upscaling" + "MC2 file override system" sections.
3. `memory/mc2_texture_handle_is_live.md` — the per-frame handle mutation rule.
4. `mclib/tgl.cpp` — search for `SetTextureHandle` and the handle-selection logic.
5. `mclib/msl.cpp:1321` — TransformMultiShape's SetTextureHandle call site.
6. `GameOS/gameos/gos_static_prop_batcher.cpp:1531-1552` — current draw-time resolve.
7. `GameOS/gameos/gos_static_prop_batcher.cpp:540-610` — packet generation including textureSlot capture.

## Communication style

User wants: direct, no catastrophizing, one fix at a time. **Do recon first** — write a brainstorm and identify the actual handoff mechanism before picking Path 1 / 2 / 3. The fix is structural, not a one-liner, and getting the design wrong here (e.g. resurrecting too much TMS) reintroduces the perf cost slice 2 was paid to eliminate. Adversarial-plan-review is the default stance for this work per worktree CLAUDE.md "Review Discipline".

Ship together with [the alpha-test prompt](2026-05-05-static-prop-alpha-test-prompt.md) only if the fixes happen to share files — otherwise treat as independent work items.
