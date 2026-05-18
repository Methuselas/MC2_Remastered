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
slowdown) — to compute `s_cpuVisibleCount`. **Consumer identification
(advisor-corrected 2026-05-17, grep-verified):** `s_cpuVisibleCount` is
read via `substrate_getCpuVisibleCount()` (`gpu_cull_substrate.cpp:81-82`)
by exactly THREE callers — `gpu_cull_readback.cpp:358`, `:393` (a
`TracyPlot("GPU.VisibleCount.CPU")`, gated by `readback_isEnabled()`,
default-OFF) and `gpu_cull_compute.cpp:1197` (inside
`compute_emitParitySummary()`, C1a-mode only; the default C1b path returns
at `:1187` before reaching it). `gpu_cull::parity_flushSummary()`
(`gpu_cull_parity.cpp:53-63`) does **NOT** read this counter (it prints
`s_mismatches`/`s_totalChecked`/`s_flushCount` only) — an earlier draft
mis-named it as the consumer. Every real consumer is a default-OFF
diagnostic, which is why the cost is dead at default env. ~15% of frame of
dead readback for a counter no default-armed path reads. Full arc:
`docs/superpowers/plans/progress/2026-05-17-mission-update-selftime-probe-RESULT.md`.

## Goal

Eliminate the flush-side readback/count-loop by accumulating
`s_cpuVisibleCount` at record-write time, where the record is hot in CPU
cache (the `const GpuActorRecord& rec` parameter). True substitutive
elimination: the cost vanishes unconditionally (not gated behind a debug
flag) and the named Tracy zone collapses. **Semantics ruling
(advisor-confirmed): ADOPT ALL-RECORDS** — both producers increment via the
shared helper. The old flush-side loop ran *inside* `substrate_flushUpload`
*before* static-prop append, so it counted dynamic actors only; that was a
latent under-count masked because every real consumer is a default-OFF
diagnostic. Making both producers increment makes the code match the
intended all-records semantics already stated in the counter-semantics
contract below. The new accumulator value equals the old loop's value at
any consumer that reads pre-static-append, and is the (correct) larger
all-records value at consumers that read post-append — the only behavioral
delta lands in the default-OFF `TracyPlot`/C1a diagnostics, so
armed-default-regime risk is zero.

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
- Per-record equivalence: each producer does
  `memcpy(dest, &rec, sizeof(GpuActorRecord))`, so `rec.prevVisibilityBit`
  on the cached parameter == the `recs[i].prevVisibilityBit` that landed in
  the mapped buffer — identical predicate, identical data, different
  (cached) source. This guarantees per-record fidelity. The *aggregate*
  intentionally differs from the old loop: old = dynamic-only (pre-append
  placement); new = all-records (both producers) — the corrected
  all-records semantics, not a discrepancy. Equality is asserted only
  against an all-records legacy recompute over the FINAL population (see
  Compile-gated count parity).
- **Per-frame ordering (advisor-traced, RE-GREP):** `substrate_frameBegin`
  (`mission.cpp:~520`, resets counters) -> `ObjectManager->update` ->
  dynamic submits (`substrate_submitDynamicActor`) -> `substrate_flushUpload`
  (`objmgr.cpp:~2210`; old count-loop here, **dynamic-only** because static
  props are not yet appended) -> later in `renderLists()`:
  `GpuStaticPropRegistry::flush` -> `substrate_appendStaticPropRecord`
  (`txmmgr.cpp:~2060`, `s_perFrameCount` keeps growing) -> `compute_dispatch`
  (`txmmgr.cpp:~2075`) -> next-frame `readback` consume. **`s_cpuVisibleCount`
  is final only after the last producer (static-prop append) has run; no
  consumer may treat it as final before late static-prop appends.** The
  three real consumers read it lagged (readback consumes slot N-1/N-2;
  compute-C1a is non-default) — all observe the post-append value in the
  armed regime.
- **Threading precondition:** `substrate_writeRecord()` assumes substrate
  record production is externally serialized on the existing render/game
  submission path (the existing `++s_perFrameCount` already relies on this).
  It introduces NO atomics; it inherits, and must not weaken, the existing
  single-threaded-producer contract.

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
// increment is computed from the cache-hot `rec` parameter, never by
// reading back from the write-combined mapped buffer. It occurs AFTER the
// successful memcpy so the accumulator counts exactly records actually
// written into recs[0..s_perFrameCount). Replaces the old flush-side loop.
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
Keep the header field writes (`:261-264`). Note the deleted loop was
**dynamic-only** (it ran pre-static-append); under all-records the
accumulator is finalized only after the post-flush static-prop appends —
NOT at flush time. No consumer reads it inside flush (the `:82` getter's
three callers are all lagged/default-OFF — see Verified facts), so deleting
the in-flush loop does not strand any reader: every real consumer reads the
finalized post-append value in the regime where it is armed at all.

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

### Compile-gated count parity (proof aid only)

For proof builds only, behind a default-OFF compile gate, recompute the
all-records count and compare. **Placement is load-bearing
(advisor-corrected):** this MUST run at a point where `s_perFrameCount` is
FINAL — i.e. AFTER the post-flush static-prop appends (e.g. immediately
before `compute_dispatch`, the `txmmgr.cpp:~2075` site, RE-GREP). Placing
it at the old loop site inside `substrate_flushUpload` (pre-append) would
recompute a dynamic-only legacy value and compare it to the all-records
accumulator — guaranteeing a false `cpuvis_parity_mismatch` every frame and
producing a green-but-meaningless done-governor. The recompute must iterate
the SAME final `recs[0..s_perFrameCount)` the accumulator saw:

```cpp
#if defined(MC2_SUBSTRATE_COUNT_PARITY)
    // PROOF-ONLY: recompute the all-records legacy count over the FINAL
    // post-static-append population and assert the write-side accumulator
    // matches. Compile-gated OFF by default; NOT a runtime path in
    // normal/shipping builds; removed after the proof capture. MUST be
    // invoked after the last static-prop append for the frame (pre
    // compute_dispatch), NOT at the old in-flush loop site.
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

Exposed as a small file-static hook (e.g. `substrate_countParityCheck()`)
called from the post-append / pre-dispatch site so the recompute is not
re-buried inside `substrate_flushUpload`. This is a proof aid, NOT lifecycle
instrumentation (the debug-instrumentation demote-not-delete rule does not
apply): it is fully removed in the same post-proof cleanup step as the
inner-zone removal, because keeping it — even compile-gated — would leave a
latent reintroduction of the exact readback loop this design deletes.

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
1. **Structural disappearance (hard requirement, not a timing target):** on
   a clean comparable capture (env per the RESULT doc protocol, wolfman
   zoom), the flush-side count-loop is absent, `SubFlush.HeaderWrite` is
   header-only (~ns), and no equivalent cost reappears in the producer
   zones. `GOM.substrateFlushUpload` should drop by approximately the prior
   `SubFlush.HeaderAndCountLoop` cost — exact ms varies with record count
   and capture noise, so the *structural* disappearance + no-reappearance is
   the gate, not a specific millisecond figure.
2. Total-frame delta directionally consistent with the removal; the added
   per-record cache-hot branch in `substrate_writeRecord` must be negligible
   (confirm `substrate_submitDynamicActor` / `substrate_appendStaticPropRecord`
   zones did not balloon).
3. `MC2_SUBSTRATE_COUNT_PARITY` proof build, with the parity hook placed at
   the post-static-append / pre-`compute_dispatch` site (NOT the old loop
   site): zero `cpuvis_parity_mismatch` over a full mission. (Pre-append
   placement would false-alarm every frame — see Compile-gated count parity.)
4. **Real-consumer positive confirmation (replaces the prior, invalid
   "parity_flushSummary unchanged" criterion — that function does not read
   this counter, so a no-change assertion there proves nothing):** a run
   with `readback_isEnabled()` armed (and a compute-C1a-armed run) where
   `TracyPlot("GPU.VisibleCount.CPU")` and the C1a false-pos/neg counts are
   sane and now correctly include static-prop records — this is the only
   place the all-records behavioral delta is observable, so it needs a
   positive confirmation, not a no-change assertion.
5. tier1 smoke 5/5 (no regression).

## Out of scope

- StaticDecorativeSet itself (concurrent session; this design only defines
  the contract so they compose).
- The static-prop in-place `hdr->recordCount` patch and the divergent
  overflow-log paths (kept verbatim — not unified).
- Any change to the `:82` getter or the three real consumers
  (`gpu_cull_readback.cpp:358/393`, `gpu_cull_compute.cpp:1197`). Their
  *observed value* changes (dynamic-only -> all-records) by design, but
  their code is untouched. `parity_flushSummary()` is unrelated to this
  counter and is not touched.
- The outer 6-probe and `SubFrameBegin.RingWait` instruments (permanent;
  untouched).

## Discipline

- Behavior change is single-file: `GameOS/gameos/gpu_cull_substrate.cpp`
  (helper + producers + flush-loop deletion + lockstep reset + zone rename).
  The proof-only `MC2_SUBSTRATE_COUNT_PARITY` hook additionally adds ONE
  compile-gated call at the post-static-append / pre-`compute_dispatch` site
  (`txmmgr.cpp:~2075`, RE-GREP) — a second file, but ONLY inside the
  default-OFF `#if` and fully removed in the post-proof cleanup; it is never
  in a shipping/default build. Per-file staging only (`git add` by name);
  branch carries foreign uncommitted WIP — never `git add -A`;
  `git show <sha> --stat` must list only the intended file(s).
- Build `--config RelWithDebInfo`, full relink (`rm` changed `.obj` +
  `mc2.exe`; `Stop-Process -Name mc2` first if running). Deploy exe-only,
  per-file `Copy-Item -Force` + `Get-FileHash`. NEVER `cp -r`.
- Plan stages: behavior change (helper + producers + flush-loop deletion +
  lockstep reset + zone rename) plus the compile-gated count-parity hook as
  one reviewable slice; the post-proof demotion/removal of sub-100ns inner
  zones + full count-parity-hook removal as a distinct follow-up step gated
  on the proof capture.
- This design was advisor-validated three times (mc2-render-expert
  substrate mechanism recon; mc2-render-perf-expert intra-flush sizing;
  mc2-render-expert consumer-ordering + all-records semantics ruling) and
  twice outside-reviewed; the writing-plans output goes through the mandated
  adversarial-plan-review before exec.
