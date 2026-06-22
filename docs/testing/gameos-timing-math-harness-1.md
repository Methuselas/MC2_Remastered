# GAMEOS-TIMING-MATH-HARNESS-1

**Type:** C++ standalone harness over a header-only pure-math extraction. No GL,
no platform clock, no engine.
**Scope:** behavior-preserving extraction + harness. Timing only (case-fold is a
separate slice).

## Extraction

New header `GameOS/gameos/utils/timing_math.h` (pure, `<stdint.h>` only):

- `ticks_to_ms(ticks, freq)` — the Windows `(ticks*1000)/freq` conversion, with
  `freq==0` meaning "ticks already in ms" (non-Windows pass-through).
- `advance_elapsed_clamp(prev, now, maxDelta)` — the `gos_GetElapsedTime`
  frame-elapsed clamp (seed on first call, cap catch-up to `maxDelta`, disable
  when `maxDelta <= 0`).

Production now delegates to these exact functions:

- `GameOS/gameos/utils/timing.cpp` — `ticks2ms()` Windows branch (1 line).
- `GameOS/gameos/gameos.cpp` — `gos_GetElapsedTime()` clamp body (the `#else`,
  i.e. **non-Windows-compiled**, branch).

### Exact equivalence (verified)

`ticks2ms` Windows branch is **byte-identical for all production-reachable
inputs**, with one deliberate, unreachable-case deviation:

- Original: `ticks = (ticks * 1000) / Frequency.QuadPart`. `ticks` is `uint64_t`;
  `Frequency.QuadPart` is `LONGLONG` (signed int64) which converts to `uint64_t`
  for the division. `ticks * 1000` is `uint64_t` (wrapping).
- New: `ticks_to_ms(ticks, (uint64_t)Frequency.QuadPart)` → `(ticks * 1000) / freq`
  with `freq` `uint64_t`. Same widths, same signedness, same `*1000` overflow
  wrap, same truncating division. For any `freq > 0` the result is identical.
- **Deviation, freq == 0 only:** the original divides by zero (undefined
  behavior / crash); the new code returns `ticks` unchanged. On Windows
  `QueryPerformanceFrequency` is always non-zero, so this case is unreachable in
  production — the guard replaces UB with a defined value and also lets the
  non-Windows "ticks are already ms" path reuse the same function (freq 0 =
  pass-through). It is therefore **not** an unconditional byte-identical change;
  it is byte-identical wherever the original was defined.

The clamp extraction is byte-identical (it is pure control flow with no integer
conversions), and lives only in the non-Windows branch.

## Why these are safe

- The `ticks_to_ms` Windows change computes the identical historical formula —
  verified by `ticks_windows_formula`.
- The clamp lives entirely in the `#else` (non-`PLATFORM_WINDOWS`) branch, so it
  is **not compiled into the Windows shipping build at all** — the Windows
  runtime is provably unchanged.

## Why smoke is a bad oracle

A 30 s steady tier1 smoke never stutters, never pauses under a debugger, and
runs at a stable cadence, so it never drives the `MaxTimeDelta` catch-up path or
a degenerate tick conversion. The clamp's correctness (monotonic, never
backward, capped catch-up after a multi-second stall) is only observable on a
fault path smoke does not produce.

## Tests (game-free)

`ticks_freq_zero_is_identity`, `ticks_windows_formula`, `ticks_no_overflow_large`,
`clamp_first_call_seeds`, `clamp_small_gap_passes_through`,
`clamp_large_gap_caps_catchup`, `clamp_disabled_when_maxdelta_nonpositive`,
`clamp_monotonic_never_backward`. Plus `demo_intentional_fail` (via `--test`) to
prove the exit-code path bites.

## Run

```
cmake -S tools/timing_math_harness -B build64-timing -G "Visual Studio 17 2022" -A x64
cmake --build build64-timing --config RelWithDebInfo --target timing_math_harness
build64-timing/RelWithDebInfo/timing_math_harness.exe
# or via the canonical helper:
py -3 tools/build_contract_harnesses.py --run --only timing_math_harness
```

## Pre-merge gate

`gameos.cpp` / `timing.cpp` are key_source. A tier1 smoke (5/5) is the required
pre-merge integration gate — NOT run by this harness slice. The Windows runtime
behavior is provably identical (see above), so smoke is a regression backstop,
not a correctness dependency.
