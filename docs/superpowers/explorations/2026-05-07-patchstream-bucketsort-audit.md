# PatchStream.BucketSort Sort-Key Audit

Date: 2026-05-07
Scope: audit-only (no implementation)

## TL;DR

The proposed optimization — "sort PatchStream buckets by state-class so
ApplyRenderStates can short-circuit on consecutive equal states" — does
**not match the call graph**. PatchStream's per-bucket draw loop never
calls `applyRenderStates()`. The 417 `ApplyRenderStates ×417 = ~256 µs`
zone is driven by `gos_RenderIndexedArray()` in
`gameos_graphics.cpp:4003` (DrawIndexedTris.Lighted) and siblings,
which `mclib/txmmgr.cpp` renderLists invokes per shape submission.
PatchStream's contribution to the 417 is **zero**.

There is also no state-equality short-circuit in `applyRenderStates`
to fast-path back to. Every call pays the full ~600 ns.

The audit therefore reframes the optimization, and returns no
implementation.

## What BucketSort actually sorts by

`gos_terrain_patch_stream.cpp:948-960` —

```cpp
struct BucketSortEntry { DWORD gosHandle; uint32_t stagingIdx; };
BucketSortEntry sortBuf[kPatchStreamMaxBuckets];
{
ZoneScopedN("PatchStream.BucketSort");
for (uint32_t i = 0; i < s_stagingCount; ++i) {
    sortBuf[i] = { tex_resolve(s_staging[i].textureIndex), i };
}
std::sort(sortBuf, sortBuf + s_stagingCount,
    [](const BucketSortEntry& a, const BucketSortEntry& b) {
        if (a.gosHandle != b.gosHandle) return a.gosHandle < b.gosHandle;
        return a.stagingIdx < b.stagingIdx;
    });
}
```

Sort key is **single-dimensional: resolved gosTextureHandle** with a
stagingIdx tie-break for stability. The intent is documented in the
preceding comment (`gos_terrain_patch_stream.cpp:943-947`): grouping
same-texture staging buckets so the consolidate step (lines 962-994)
can merge them into one contiguous ring range and one draw call.

It does NOT sort by program / sampler / blend / depth / materialFlags
because PatchStream is a single shader-program endpoint with a single
material (`gos_terrain_bridge_getMaterial()` at line 1003) and
homogeneous pipeline state across its buckets. Splitting the key by
those dimensions would add zero useful runs — they're already constant
within the entire PatchStream draw range.

## Why the sort is irrelevant to ApplyRenderStates ×417

The PatchStream bucket draw loop (`gos_terrain_patch_stream.cpp:1054+`)
issues raw `glDrawArrays` per bucket inside
`gos_terrain_bridge_drawSingleBucket`. It bypasses
`applyRenderStates` entirely; the bridge sets terrain GL state
once before the loop and reuses it. Grep evidence: the only mentions
of `applyRenderStates` in `gos_terrain_patch_stream.cpp` (lines 1300,
1410) are comments noting the bypass.

The 417 `ApplyRenderStates` calls/frame come from `gos_RenderIndexedArray`
in `GameOS/gameos/gameos_graphics.cpp` (lines 4003, 4035) and the
unbatched `drawTris/drawQuads/drawLines/drawPoints` paths (lines 3105,
3128, 3164, 3175, 3191, 3202, 3235, 3250). Caller volume: txmmgr's
`renderLists()` issues 20+ `gos_RenderIndexedArray()` sites in
`mclib/txmmgr.cpp` — one per shape submission for the legacy CPU mech /
gv / static-shape path.

PatchStream's slice of the 417: **0**. Verified by code inspection,
not by Tracy split.

## ApplyRenderStates has no short-circuit

`gameos_graphics.cpp:2836-2965`. The body unconditionally executes
`glDisable(GL_CULL_FACE)` / `glDepthMask` / `glDepthFunc` / `glBlendFunc`
and binds 3 texture units + sets sampler params, regardless of whether
`renderStates_[k] == curStates_[k]` for any k. The trailing
`curStates_[k] = renderStates_[k]` writes are bookkeeping for
external code (e.g. `selectBasicRenderMaterial(curStates_)`), not a
guard. The comment at `gameos_graphics.cpp:5448` ("applyRenderStates()
may skip if curStates_ == renderStates_, so bypass it") describes
**intent** that is not actually implemented.

So a hypothetical sort that produced runs of identical state would not,
today, win anything from `applyRenderStates`. The win would have to come
from **adding** the state-equality early-out to `applyRenderStates`
itself.

## Distinct-state-transition estimate (informational)

Without instrumenting per-call state vectors, I can only bound: txmmgr's
renderLists alternates a small set of (alpha mode, zwrite, filter, address,
texture handle) tuples. The texture changes nearly every call (every shape
has its own `gos_State_Texture`). The non-texture portion changes a
handful of times (open/close transparency batches, terrain flag, water
flag). A reasonable estimate: **~30-60 distinct
non-texture-only state transitions** across the 417 calls — the rest
differ only in texture handle.

That suggests the right optimization is a tiered short-circuit:

- **Cheap path (no glXxx state changes, just the texture rebind)**:
  if every non-texture state matches `curStates_`, skip the
  cull/depth/blend/sampler GL calls; only run the 3-unit texture bind
  loop. Texture binds are ~50-150 ns each; the rest is the bulk.
- **Full path**: as today.

Without measurement of the curStates vs renderStates miss rate I won't
quote a savings number, but the floor case ("texture changes every
call, nothing else does") would skip ~5 glXxx calls per ApplyRenderStates,
roughly the cheapest 200-300 ns of the ~600 ns budget. Upper bound:
~80-120 µs deletable.

## Decision: design-note, not implementation

The original framing — "swap BucketSort comparator" — does not apply.
The right slice is: **add a state-equality early-out inside
`applyRenderStates()`**. That is a one-function change in
`gameos_graphics.cpp:2836` and does not require touching PatchStream
sort or any consumer.

Sketch (NOT implemented here; needs a separate slice):

```cpp
void gosRenderer::applyRenderStates() {
    ZoneScopedN("ApplyRenderStates");

    // Detect non-texture-state equality — texture is rebound separately
    // because mclib reassigns gos_State_Texture every shape.
    bool nonTexEqual = true;
    for (int k = 0; k < gos_State_LAST; ++k) {
        if (k == gos_State_Texture || k == gos_State_Texture2 ||
            k == gos_State_Texture3) continue;
        if (renderStates_[k] != curStates_[k]) { nonTexEqual = false; break; }
    }

    if (!nonTexEqual) {
        // existing body ... cull, depth, blend, alpha, filter, address
    }

    // texture bind loop unchanged — runs every call
    // ...
}
```

Risks to surface in the implementing slice's adversarial review:
- Some callers (`gos_ForceApplyRenderStates`, line 5446) already work
  around the lack of a short-circuit by zeroing curStates first. That
  callsite is correct under the new code (curStates differs → full
  body runs), but the contract should be documented.
- The auto-reset states (`gos_State_Terrain`, `gos_State_Water` at
  lines 2958-2961) write `curStates_[k] = renderStates_[k]` then clear
  `renderStates_[k]`. Under the early-out, these still need to run
  every call so the auto-reset happens. Move them outside the guard.
- Sampler state and filter/address are still tied to the bound
  texture; if the early-out skips the sampler-param call but the
  texture bind path runs, the texture inherits whatever sampler state
  was last set. Acceptable today (sampler matches the prior state by
  definition of the early-out), but fragile under future refactors.

## Confirmation that BucketSort's current key is correct

For PatchStream's purpose (consolidate same-texture staging into one
draw range), texture-handle is the only useful key. State-class sort
within PatchStream would not help and could only hurt by introducing
extra bucket boundaries when the same texture appears under
nominally-different bucket entries from the staging side.

Draw-order invariants preserved by the current sort: same-texture
runs are stable on stagingIdx (deterministic frame-to-frame output for
parity); cross-texture order is determined by handle value, not source
order, but that doesn't affect terrain rendering (no overlapping z,
no transparency between terrain quads at this stage).

## Outcome

- No code change.
- This file is the design note. The follow-up implementation slice is
  the `applyRenderStates` early-out in `gameos_graphics.cpp:2836`,
  not a BucketSort change.
