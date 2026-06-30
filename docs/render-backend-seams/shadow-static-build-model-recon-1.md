# SHADOW-STATIC-BUILD-MODEL-RECON-1
## Declaratively model the once-per-mission static-shadow-build ordering exception

**Status:** RECON ONLY — no code changed.
**Branch:** `claude/nifty-mendeleev`
**Date:** 2026-06-29

---

## TL;DR and Recommendation

The once-per-mission static-shadow-build fires **lazily inside `renderLists()`**
(txmmgr.cpp:2577–2601), guarded by `!gos_StaticLightMatrixBuilt()`. On that one
frame it calls `gos_BeginShadowPrePass()` → `gosRenderer::beginShadowPrePass()`
→ `noteRenderPass(ShadowCaster)`. This note lands **after** the MechOpaque preamble
note already recorded at txmmgr.cpp:2362, so the dry-run sees sequenceIdx(Mech) <
sequenceIdx(Shadow) but Shadow is declared slot 0 in `kFramePassOrder` — apparent
out-of-order.

The current fix (render_contract.cpp:762–786) detects this at frame-boundary time
with a runtime order-comparison and marks MechOpaque `knownEarlyDrawSite`. **This
heuristic would suppress a genuine steady-state Mech-before-Shadow bug on the
static-build frame**, because the mark fires whenever `seqIdx(Mech) < seqIdx(Shadow)`
regardless of whether the shadow event was a one-shot build or a normal per-frame
shadow pass.

**Recommendation: Option C** — do not call `noteRenderPass` inside the one-shot
static-build path. The static build is a once-per-mission bake, not a per-frame
shadow pass; excluding it from the per-frame dry-run trace is the most honest model
and requires the smallest code change. Remove the runtime heuristic block in
`dryrunFrameBoundary()` (render_contract.cpp:762–786) in the same commit.

---

## Q1: Event Characterization — What, When, and Why Late

### What the static-shadow-build is

`renderLists()` (txmmgr.cpp:2577–2601) contains a one-shot guard:

```
if (gos_IsTerrainTessellationActive() && !gos_StaticLightMatrixBuilt()
    && Terrain::mapData) {
    ...
    gos_BeginShadowPrePass(true);
    Terrain::mapData->renderStaticTerrainShadowFullMap(...);
    gos_EndShadowPrePass();
    ...
}
```

`gos_StaticLightMatrixBuilt()` is a process-scoped latch that is cleared by
`gos_ResetStaticShadowPriming()` at mission load (code/mission.cpp:3924,
code/saveload.cpp:1163, editor/EditorData.cpp:465). The build runs exactly once
per mission, on the **first frame that reaches renderLists()** after mission load.

### When it fires (mission load? lazy?)

**Lazily on the first rendered frame of each mission.** It is not triggered at
mission-load time; it fires mid-`renderLists()` on frame 1, inside the normal
submission loop. After that frame `gos_MarkStaticLightMatrixBuilt()` sets the latch
and the block is skipped every subsequent frame.

### Why its `noteRenderPass(ShadowCaster)` lands after MechOpaque's

`noteRenderPass(PassIdentity::OpaqueObject)` is called at the top of `renderLists()`
(txmmgr.cpp:2362), **before any object submission**. `gos_BeginShadowPrePass()`
(and thus its `noteRenderPass(PassIdentity::ShadowCaster)`) is called later, at
txmmgr.cpp:2599, inside the one-shot build block deep inside the same function.

So on the build frame the dry-run sees:
- sequenceIdx 0 → `RenderPassId::MechOpaque` (from the preamble note :2362)
- sequenceIdx 1 → `RenderPassId::Shadow` (from gos_BeginShadowPrePass :6274, called at txmmgr.cpp:2599)

`kFramePassOrder` declares Shadow=slot0, MechOpaque=slot1, so walking the declared
order, MechOpaque has sequenceIdx 0 which is **less than** the preceding Shadow's
sequenceIdx 1 → apparent out-of-order.

### Normal-frame ordering

On every non-build frame the one-shot guard at txmmgr.cpp:2577 is skipped
(latch is set). `noteRenderPass(ShadowCaster)` is **never called** on normal frames
from this path — the Shadow slot goes entirely unobserved. The frame_pass_trace.h
comment (line 12–15) documents this: "Shadow … VFX, VegetationCards are INVISIBLE
today (no callsite)." An unobserved slot is classified `unobservedCount++`, not a
divergence. So on normal frames Shadow=unobserved, MechOpaque=fired first → no
out-of-order event. The apparent ordering problem exists **only** on the static-build
frame, because that is the only frame on which `noteRenderPass(ShadowCaster)` fires
at all via the `beginShadowPrePass` callsite.

---

## Q2: Is Conflation the Root?

Yes. `PassIdentity::ShadowCaster` → `RenderPassId::Shadow` (render_contract.cpp:561).
There is a **single callsite** that produces `noteRenderPass(ShadowCaster)`:
`gosRenderer::beginShadowPrePass()` (gameos_graphics.cpp:6278). That function is
called **both** for the once-per-mission static build (txmmgr.cpp:2599 via
`gos_BeginShadowPrePass(true)`) and for any per-frame dynamic shadow invocation.

Both collapse to the same `RenderPassId::Shadow` slot in the trace — the trace
cannot distinguish a "this is the one-shot static bake" call from a "this is a
normal per-frame shadow" call. `recordPassFired` ignores duplicates (first-fire
wins), so even if both fired in the same frame only one sequenceIdx would land.

The cleanest model is therefore to **suppress the static-build call from the
dry-run trace entirely** (Option C) rather than trying to add a separate occurrence
id or a special slot.

---

## Q3/Q4: Option Comparison

### Option A — Declare a `knownLate` exception on the Shadow pass contract

Add a `bool isLateOnStaticBuildFrame` field to `RenderPassContract` (or
`FramePassEntry`), set it for `RenderPassId::Shadow`, and consult it in
`dryRunCompare` to suppress Shadow's late-order position.

**Problem:** Shadow is declared **first** in `kFramePassOrder` (slot 0) because it
logically produces `ShadowDynamicMap` consumed by everything. Suppressing its
sequencing via a declared flag doesn't fix the underlying confusion — it would
suppress the Shadow slot's ordering check regardless of whether the event was a
one-time bake. A genuine future "dynamic shadow fired too late" bug would also be
suppressed if the same mechanism were consulted naively.

The terrain `drawSite` analogy is not a perfect fit here: terrain's draw-site is
**per-branch and config-variable**, so a table lookup at frame-boundary time is
the right model. The static-shadow build is not config-variable — it fires on
exactly one frame and disappears. A declared exception for it on the contract row
would be permanent ("Shadow can be late") rather than scoped ("Shadow can be late
ONLY on the one build frame").

**Verdict:** Declarative-but-imprecise. Does not shrink the masking window much vs
the current heuristic. Rejected.

### Option B — Model the static-shadow-build as a DISTINCT pass-occurrence

Add a new `RenderPassId::ShadowStaticBuild` slot (or a separate occurrence index)
that is exempt from ordering, leaving `RenderPassId::Shadow` for the per-frame
dynamic shadow.

**Problem:** The distinction doesn't exist at the call sites today. Both the
static build and any future per-frame shadow path call `gosRenderer::beginShadowPrePass()`,
which holds the single `noteRenderPass(ShadowCaster)` call. Adding a caller
distinction requires a new API or a flag parameter, and `kFramePassOrder` would need
a new slot — adding non-trivial schema drift. The `static_assert` enforcing that
`kFramePassOrderCount == kRenderPassIdCount` would also require a new `RenderPassId`
enum value.

**Verdict:** Most structurally honest long-term but highest complexity for a
one-time event. Overshoot. Not recommended for this recon.

### **Option C — Suppress at the source (recommended)**

Do not call `noteRenderPass` inside `gosRenderer::beginShadowPrePass()` when it
is executing the once-per-mission static build. Call it only for per-frame dynamic
shadow passes.

The distinction is already clear at the call site: the static build is always
invoked from the one-shot guard block in txmmgr.cpp (~:2577–2601) via
`gos_BeginShadowPrePass(true)` (the `true` = clearDepth = first-time bake). A
`bool isStaticBuild` parameter (or a separate `gos_BeginStaticShadowPrePass()`
entry point) can gate the `noteRenderPass` call.

Alternatively — and more minimally — the `noteRenderPass` call in
`beginShadowPrePass()` can be relocated to the **per-frame dynamic shadow codepath**
only, wherever that exists. Since Shadow is currently classified as `INVISIBLE`
(no normal-frame callsite), the simplest approach is:

1. **Remove** the `noteRenderPass(ShadowCaster)` call from
   `gosRenderer::beginShadowPrePass()` (gameos_graphics.cpp:6278).
2. Add `noteRenderPass(ShadowCaster)` at the future per-frame dynamic shadow
   draw site when that path is instrumented (or leave Shadow unobserved until
   then — the recon already documents this is the current state for normal frames).
3. **Remove** the runtime heuristic block (render_contract.cpp:762–786).

**Why this is correct:** the static-build is not part of the per-frame shadow
pass that `kFramePassOrder` models. It is a one-time terrain bake that happens to
reuse `beginShadowPrePass`/`endShadowPrePass` as its rendering vehicle. The dry-run
trace is a per-frame pass ordering record; a once-per-mission setup event does not
belong in it. Removing it from the trace is not "suppressing" an ordering violation
— it is correctly scoping the trace to per-frame observations.

**HIGH: The current heuristic marks MechOpaque `knownEarlyDrawSite` on any frame
where sequenceIdx(Mech) < sequenceIdx(Shadow). If a genuine steady-state
Mech-before-Shadow regression were introduced that happened to fire on the same
frame as a static-shadow-build event, the heuristic would suppress both — the real
bug would be invisible. Option C eliminates this masking window entirely because
the heuristic block is removed.**

### Option D — Tighten the heuristic (e.g. only on frame 1, or only when build flag set)

Keep the runtime order-comparison but add a guard: only fire if
`!gos_StaticLightMatrixBuilt()` was true at the start of this frame (i.e., we know
a static build just occurred).

**Problem:** This requires threading a "static build happened this frame" flag out
of `renderLists()` into `dryrunFrameBoundary()`, which is still a runtime heuristic,
just a tighter one. It does not remove the heuristic; it narrows it to build frames
only. Compared to Option C it provides no additional benefit: if you have access to
the build flag you can just as easily use it to skip the `noteRenderPass` call.

**Verdict:** Better than status quo but inferior to C. Not recommended.

---

## SLICE PROPOSAL (not built)

### Files to change

| File | Change |
|---|---|
| `GameOS/gameos/gameos_graphics.cpp` | Remove `noteRenderPass(ShadowCaster)` at :6278 from `beginShadowPrePass()`. Add a new internal call at the future per-frame dynamic-shadow entry point (or leave unobserved for now — Shadow was already INVISIBLE on normal frames). |
| `mclib/render_contract.cpp` | Remove the SHADOW-OBSERVE-2 heuristic block at :762–786 from `dryrunFrameBoundary()`. |
| `tests/unit/test_frame_graph.cpp` | Update the comment at :397 that lists Shadow as having no callsite — it now has no callsite at all (not just none on normal frames). |

### Exact edit — gameos_graphics.cpp

Remove lines 6273–6279 from `gosRenderer::beginShadowPrePass()`:

```cpp
// DELETE these two lines from beginShadowPrePass():
render_contract::assertPassContract(render_contract::PassIdentity::ShadowCaster,
                                    "gosRenderer::beginShadowPrePass");
render_contract::noteRenderPass(render_contract::PassIdentity::ShadowCaster,
                                "gosRenderer::beginShadowPrePass");
```

The `assertPassContract` call can stay (it validates GL state, not ordering); only
`noteRenderPass` needs removal. Or move `noteRenderPass` to a new
`gosRenderer::beginDynamicShadowPass()` entry point called only for per-frame
dynamic shadow rendering when that path is defined.

### Exact edit — render_contract.cpp

Remove the `SHADOW-OBSERVE-2` block (lines 762–786 inclusive):

```cpp
// DELETE the entire block:
// SHADOW-OBSERVE-2: Shadow is declared first in kFramePassOrder ...
{
    const int shadowSlot   = RenderCore::framegraph::declaredOrderIndex(...);
    const int mechSlot     = ...;
    if (shadowSlot >= 0 && mechSlot >= 0 &&
        g_dryrunTrace.entries[shadowSlot].fired &&
        g_dryrunTrace.entries[mechSlot].fired &&
        g_dryrunTrace.entries[mechSlot].sequenceIdx < ...) {
        RenderCore::framegraph::markEntryKnownEarly(...MechOpaque...);
    }
}
```

### How this removes the runtime heuristic

After the edit:
- `beginShadowPrePass()` no longer calls `noteRenderPass` → Shadow slot never fires
  in the trace → `entries[shadowSlot].fired == false` on every frame.
- `dryrunFrameBoundary()` does not examine Shadow/Mech sequence order.
- MechOpaque is never marked `knownEarlyDrawSite` by this block.
- `outOfOrder` stays 0 (Shadow unobserved → `unobservedCount++`, not `outOfOrderCount++`).

### Verification plan

1. **out_of_order stays 0 on the static-build frame** — smoke two missions including
   one that hits the build frame (any mission with tessellation active). Confirm
   `g_dryrunOutOfOrder` remains 0 after the latch fires on frame 1.

2. **out_of_order stays 0 on normal frames** — same smoke run, later frames.
   Shadow remains unobserved (as it was before SHADOW-OBSERVE-2), `unobservedCount`
   increments per-frame for the Shadow slot, `outOfOrderCount` stays 0.

3. **knownEarlySuppressed stays 0 for the Shadow/Mech block** — the SHADOW-OBSERVE-2
   heuristic block is gone; `knownEarlySuppressed` should only count LOD-chunk terrain
   events (DRYRUN-DRAWSITE-ORDER-1). Confirm the counter is non-zero only when
   LOD-chunk terrain draws (expected) and does not increase on the static-build frame.

4. **ambient mismatch stays 0** — `g_dryrunOutOfOrder == 0`, consistent with ambient.

5. **Smoke 2/2 pass** — run `--mission mc2_01 --mission mc2_24`, exit 0.

6. **Genuine Mech-before-Shadow is now detectable** — if a future code change caused
   `noteRenderPass(ShadowCaster)` to fire before `noteRenderPass(OpaqueObject)` on
   a normal frame, Shadow would land sequenceIdx 0 and MechOpaque sequenceIdx 1, and
   since Shadow is slot 0 and Mech is slot 1 the sequence would be monotonic —
   actually correct. If instead MechOpaque fired first (seqIdx 0) and Shadow fired
   second (seqIdx 1) on a NORMAL frame where Shadow IS instrumented, that would
   register as `outOfOrderCount++` (Shadow slot 0 would see seqIdx 1 > prevSeq 0 →
   fine, but MechOpaque slot 1 would see seqIdx 0 < prevSeq 1 → out-of-order).
   The heuristic removal means no blanket suppression — the trace is honest.

---

## Line citations (drift-prone — verify against live branch)

| Symbol / line | File | Notes |
|---|---|---|
| txmmgr.cpp:2362 | `mclib/txmmgr.cpp` | `noteRenderPass(OpaqueObject)` preamble (MechOpaque mapping) |
| txmmgr.cpp:2577–2601 | `mclib/txmmgr.cpp` | One-shot static-shadow-build guard + `gos_BeginShadowPrePass(true)` |
| gameos_graphics.cpp:6278 | `GameOS/gameos/gameos_graphics.cpp` | `noteRenderPass(ShadowCaster)` inside `beginShadowPrePass` |
| render_contract.cpp:561 | `mclib/render_contract.cpp` | `ShadowCaster → RenderPassId::Shadow` collapse |
| render_contract.cpp:762–786 | `mclib/render_contract.cpp` | SHADOW-OBSERVE-2 runtime heuristic block (to be deleted) |
| frame_pass_trace.h:44–49 | `RenderCore/frame_pass_trace.h` | `knownEarlyDrawSite` field definition |
| frame_pass_trace.h:141–148 | `RenderCore/frame_pass_trace.h` | `dryRunCompare` suppression branch |
| RenderPassContract.h:365 | `RenderCore/RenderPassContract.h` | `kFramePassOrder` — Shadow=slot 0 |
| gos_postprocess.cpp:3548 | `GameOS/gameos/gos_postprocess.cpp` | `gos_ResetStaticShadowPriming()` — clears latch per mission |
| mission.cpp:3924 | `code/mission.cpp` | Caller of `gos_ResetStaticShadowPriming()` at mission start |
