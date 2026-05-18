# Substrate `s_cpuVisibleCount` Write-Side Accumulation — Design

> Substitutive fix for the pinned ~1.82ms dead count-loop in
> `gpu_cull::substrate_flushUpload`. Branch `claude/gpu-driven-rendering`,
> worktree `.claude/worktrees/gpu-driven-rendering`. RE-GREP every cited
> file:line at implementation time (symbols stable, lines drift). No emoji,
> no wall-clock language.

## Problem

Two probe-first slices (`75638fd` outer 6-probe, `0c83ff5` intra-flush
3-zone) decisively pinned the long-standing ~2.06ms `GameLogic.Mission.Update`
SELF attribution gap. Root cause (capture: clean default env, wolfman zoom,
2639 frames, sigma 89us):

`SubFlush.HeaderAndCountLoop` = **1.82ms, 99.71%** of `substrate_flushUpload`,
100% self. `SubFrameBegin.RingWait` **absent** (ring never under-runs ->
not a GPU stall; no ring/coalesce coupling). The cost is the count-loop in
`substrate_flushUpload` (`GameOS/gameos/gpu_cull_substrate.cpp`, currently
`:251-258` inside the `SubFlush.HeaderAndCountLoop` zone) iterating
`recs[i].prevVisibilityBit` over `s_perFrameCount` records read back through
the `GL_MAP_COHERENT | GL_MAP_PERSISTENT` SSBO mapping — sequential CPU
reads from write-combined/uncached device memory (the classic 50-100x
slowdown) — to compute `s_cpuVisibleCount`, consumed ONLY by
`gpu_cull::parity_flushSummary()`, which early-outs when `parity_isEnabled()`
is false (default). ~15% of frame of dead readback for a counter nothing
reads at default env. Full arc:
`docs/superpowers/plans/progress/2026-05-17-mission-update-selftime-probe-RESULT.md`.

## Goal

Eliminate the flush-side readback/count-loop by accumulating
`s_cpuVisibleCount` at record-write time, where the record is hot in CPU
cache (the `const GpuActorRecord& rec` parameter, before the `memcpy` into
the mapped buffer). True substitutive elimination: the cost vanishes
unconditionally (not gated behind a debug flag), the named Tracy zone
collapses, and the parity numbers are bit-identical because the same
predicate is evaluated on the same data.

## Verified load-bearing facts (grep-confirmed)

- TWO producers write into `recs[0..s_perFrameCount)` and `++s_perFrameCount`:
  `substrate_submitDynamicActor` (`gpu_cull_substrate.cpp:209`, write block
  `:226-232`) and `substrate_appendStaticPropRecord` (`:303`, write block
  `:322-330`, plus an in-place `hdr->recordCount = s_perFrameCount` patch at
  `:334-337` because it runs AFTER flush). The flush count-loop counts
  across BOTH. Write-side accumulation MUST increment in both or it
  under-counts static props.
- The two producers legitimately DIVERGE: different overflow-log text
  (`event=substrate_overflow` vs `event=static_prop_overflow`, the latter
  carrying the load-bearing 2026-05-11 latch-bug comment) and the
  static-prop in-place header patch + distinct `SUBSTRATE_TRACE`. These
  must NOT be unified.
- `s_perFrameCount = 0` reset sites: `:107`, `:169` (init/reinit), and
  `substrate_frameBegin` (`:199`, the per-frame reset). `s_cpuVisibleCount`
  (`:50`, getter `:82`) must reset in lockstep at the same sites.
- Equivalence: each producer does `memcpy(dest, &rec, sizeof(GpuActorRecord))`,
  so `rec.prevVisibilityBit` on the cached parameter at submit == the
  `recs[i].prevVisibilityBit` the old loop read from the WC mapping at
  flush. Identical predicate, identical data, different (cached) source.

## Design

### Shared lockstep write helper

Add a file-static helper that owns the single responsibility "write one
record into the current slot array and maintain the two lockstep per-frame
counters". Precondition (documented, caller-enforced): the caller has
already passed its overflow guard and `s_mappedPtr != nullptr`.

```cpp
// Writes one record into the current ring slot's array and maintains the
// two lockstep per-frame counters (s_perFrameCount, s_cpuVisibleCount).
// PRECONDITION: caller has passed the s_perFrameCount >= s_maxActors
// overflow guard and verified s_mappedPtr != nullptr. The cpuVisible
// increment is computed on the cache-hot `rec` BEFORE the memcpy into the
// write-combined mapped buffer, replacing the old flush-side readback loop.
static inline void substrate_writeRecord(const GpuActorRecord& rec) {
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    char* dest = static_cast<char*>(s_mappedPtr)
                 + slotOffset
                 + sizeof(GpuActorRecordHeader)
                 + s_perFrameCount * sizeof(GpuActorRecord);
    memcpy(dest, &rec, sizeof(GpuActorRecord));
    if (rec.prevVisibilityBit) ++s_cpuVisibleCount;
    ++s_perFrameCount;
}
```

Increment is "after successful write" (the memcpy), atomic with the
count bump, no early-return between — satisfying the reviewer's strengthened
invariant "count exactly the records actually written into
`recs[0..s_perFrameCount)`".

### Producer call sites

- `substrate_submitDynamicActor`: keep `:210-211` guards and the
  `:213-223` overflow-guard/log unchanged. Replace the write block
  (`:226-232`: slotOffset/dest/memcpy/`++s_perFrameCount`) with a single
  `substrate_writeRecord(rec);`.
- `substrate_appendStaticPropRecord`: keep `:304-305` guards and the
  `:307-320` overflow-guard/log unchanged. Replace the write block
  (`:322-330`) with `substrate_writeRecord(rec);`. KEEP the subsequent
  in-place `hdr->recordCount = s_perFrameCount` patch (`:334-337`) and the
  `SUBSTRATE_TRACE` (`:339-340`) exactly as-is — these are the legitimate
  divergence and stay caller-side.

### Flush-side: delete the count-loop, keep the header writes

In `substrate_flushUpload`, delete the inner count-loop block (currently
`:251-259`: the `{ uint32_t cpuVis = 0; ... s_cpuVisibleCount = cpuVis; }`).
Keep the header field writes (`:261-264`). `s_cpuVisibleCount` is already
correct by flush time (all submits/appends for the frame have run; it is
finalized exactly when `parity_flushSummary()` reads it via the `:82`
getter — unchanged consumer).

### Reset in lockstep

Add `s_cpuVisibleCount = 0;` immediately adjacent to each existing
`s_perFrameCount = 0;` at `:107`, `:169`, and `substrate_frameBegin`
(`:199`). The per-frame `substrate_frameBegin` reset is load-bearing; the
init/reinit ones are for lockstep safety.

### Tracy zone rename + demotion

The count-loop is gone, so the `SubFlush.HeaderAndCountLoop` zone name now
misleads. Rename it `SubFlush.HeaderWrite` (it wraps only the ~4 trivial
header field writes). The OUTER `GOM.substrateFlushUpload` zone (`75638fd`)
remains the real substitutive done-governor (must drop ~1.8ms). After the
proof capture, the now-sub-100ns inner `SubFlush.HeaderWrite` and
`SubFlush.FenceInsert` zones are removed (they would violate the 100ns
floor as permanent instruments); `GOM.substrateFlushUpload` and
`SubFrameBegin.RingWait` stay. This demotion is a distinct post-proof step
in the plan, not bundled with the behavior change.

### Compile-gated shadow parity (proof aid only)

For proof builds only, behind a default-OFF compile gate, recompute the
old count and compare:

```cpp
#if defined(MC2_SUBSTRATE_COUNT_PARITY)
    // PROOF-ONLY: recompute the legacy flush-side count and assert the
    // write-side accumulator matches. Compile-gated OFF by default; NOT a
    // runtime path in normal/shipping builds; removed after the proof
    // capture. Does not reintroduce a default-path additive cost.
    {
        const size_t slotOffset = s_frameSlot * s_slotBytes;
        const GpuActorRecord* recs = reinterpret_cast<const GpuActorRecord*>(
            static_cast<const char*>(s_mappedPtr) + slotOffset + sizeof(GpuActorRecordHeader));
        uint32_t legacy = 0;
        for (uint32_t i = 0; i < s_perFrameCount; ++i)
            if (recs[i].prevVisibilityBit) ++legacy;
        if (legacy != s_cpuVisibleCount)
            printf("[GPU_CULL v1] event=cpuvis_parity_mismatch legacy=%u accum=%u\n",
                   legacy, s_cpuVisibleCount), fflush(stdout);
    }
#endif
```

Placed in `substrate_flushUpload` where the old loop was. This is a
proof aid, NOT lifecycle instrumentation (the debug-instrumentation
demote-not-delete rule does not apply): it is fully removed in the same
post-proof cleanup step as the inner-zone removal, because keeping it —
even compile-gated — would leave a latent reintroduction of the exact
readback loop this design exists to delete.

## Counter-semantics contract (adopted from the outside review, load-bearing)

> `s_cpuVisibleCount` is the visible count for records actually submitted
> into the substrate record array this frame. It is NOT a world-visible
> object count. The count is accumulated only through the shared substrate
> record write helper, after overflow rejection and after the record
> write. Both dynamic actors and static-prop appenders use this helper.
> When StaticDecorativeSet (`MC2_STATIC_DECOR_GPU=1`) is enabled, severed
> decoratives no longer submit substrate records and therefore
> intentionally disappear from `s_cpuVisibleCount`; decorative visibility
> is validated through StaticDecorativeSet parity and
> `decoratives_seen_in_objmgr_loop == 0`, NOT through substrate
> visible-count preservation. Accumulator parity is scoped to the active
> substrate producer set: before decorative severance, the old flush-loop
> count must equal the write-side accumulated count; after severance,
> parity must compare against a legacy count generated under the SAME
> feature configuration, or report decorative records separately.

This makes this design compose with the concurrent StaticDecorativeSet
design without clash (user decision: proceed independently with the
contract baked in). Performance wins must be attributed separately in any
shared proof: counter accumulation eliminates the substrate flush
count-loop cost; StaticDecorativeSet severance eliminates decorative
static-prop production/replay cost — adjacent but distinct.

Killswitch behavior: under `MC2_STATIC_DECOR_GPU=0` (legacy fallback) and
under bake-failure fallback (a failed decorative re-enters the legacy CPU
path), those records still flow through `substrate_writeRecord` and are
counted naturally — no special-casing needed; the helper is the single
choke point.

## Substitutive done-governor (proof criteria)

Per `feedback_offload_must_be_substitutive_not_additive`, DONE requires ALL:
1. Fresh clean capture (env per the RESULT doc protocol, wolfman zoom):
   `GOM.substrateFlushUpload` drops by ~1.8ms and `SubFlush.HeaderWrite`
   is ~ns (collapsed) — the named cost is GONE, not displaced.
2. Total-frame delta consistent with the ~1.8ms removal (no cost reappearing
   elsewhere — check `substrate_submitDynamicActor` /
   `substrate_appendStaticPropRecord` zones did not balloon; the added
   per-record branch is cache-hot and must be negligible).
3. `MC2_SUBSTRATE_COUNT_PARITY` proof build: zero
   `cpuvis_parity_mismatch` over a full mission.
4. Parity-enabled run (`parity_isEnabled()` true): `parity_flushSummary()`
   numbers unchanged vs. pre-fix baseline under the same producer set.
5. tier1 smoke 5/5 (no regression).

## Out of scope

- StaticDecorativeSet itself (concurrent session; this design only defines
  the contract so they compose).
- The static-prop in-place `hdr->recordCount` patch and the divergent
  overflow-log paths (kept verbatim — not unified).
- Any change to `parity_flushSummary()` / the `:82` getter / the consumer.
- The outer 6-probe and `SubFrameBegin.RingWait` instruments (permanent;
  untouched).

## Discipline

- Single file: `GameOS/gameos/gpu_cull_substrate.cpp`. Per-file staging
  only (`git add` by name); branch carries foreign uncommitted WIP — never
  `git add -A`; `git show <sha> --stat` must list only that file.
- Build `--config RelWithDebInfo`, full relink (`rm` changed `.obj` +
  `mc2.exe`; `Stop-Process -Name mc2` first if running). Deploy exe-only,
  per-file `Copy-Item -Force` + `Get-FileHash`. NEVER `cp -r`.
- Plan stages: behavior change (helper + producers + flush deletion +
  reset + shadow-parity + zone rename) as one reviewable slice; the
  post-proof demotion/removal of sub-100ns inner zones + shadow-parity as a
  distinct follow-up step gated on the proof capture.
- This design was advisor-validated (mc2-render-expert mechanism recon,
  mc2-render-perf-expert sizing) and outside-reviewed; the writing-plans
  output goes through the mandated adversarial-plan-review before exec.
