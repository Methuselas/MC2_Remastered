// tools/timing_math_harness/timing_math_harness.cpp
// SUBSYSTEM-HARNESS-ARC / GAMEOS-TIMING-MATH-HARNESS-1
//
// Tests the REAL GameOS time math (GameOS/gameos/utils/timing_math.h, the header
// timing.cpp::ticks2ms and gameos.cpp::gos_GetElapsedTime now delegate to)
// game-free — no platform clock, no window, no Environment. Guards the
// ticks->ms conversion and the frame-elapsed MaxTimeDelta clamp, neither of
// which a 30s steady tier1 smoke exercises (no stutter, no debugger pause).
//
// Build (standalone, no build64/ cache, no deps):
//   cmake -S tools/timing_math_harness -B build64-timing -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-timing --config RelWithDebInfo --target timing_math_harness

#include "contract_harness.h"
#include "timing_math.h"

using namespace contract_harness;
namespace tmath = timing_math;

// ---- ticks_to_ms ----------------------------------------------------------

static bool test_ticks_freq_zero_is_identity(TestCtx& t) {
    // freq==0 => "ticks already in ms" (non-Windows path), passed through.
    CH_CHECK(t, tmath::ticks_to_ms(0, 0) == 0);
    CH_CHECK(t, tmath::ticks_to_ms(1234, 0) == 1234);
    return t.failures == 0;
}

static bool test_ticks_windows_formula(TestCtx& t) {
    // 10 MHz counter: 10,000,000 ticks == 1000 ms.
    CH_CHECK(t, tmath::ticks_to_ms(10000000ull, 10000000ull) == 1000);
    // Half a second.
    CH_CHECK(t, tmath::ticks_to_ms(5000000ull, 10000000ull) == 500);
    // Exact historical formula (ticks*1000)/freq, truncating.
    CH_CHECK(t, tmath::ticks_to_ms(3, 1000000ull) == 0);          // 3us -> 0ms (trunc)
    CH_CHECK(t, tmath::ticks_to_ms(1500, 1000ull) == 1500);       // freq=1000 -> *1000/1000
    return t.failures == 0;
}

static bool test_ticks_no_overflow_large(TestCtx& t) {
    // ~1 hour at a 10 MHz counter: 3.6e10 ticks. *1000 stays within uint64.
    const uint64_t freq = 10000000ull;
    const uint64_t ticks = 36000ull * freq;                    // 36000 s
    CH_CHECK(t, tmath::ticks_to_ms(ticks, freq) == 36000000ull);  // 36,000,000 ms
    return t.failures == 0;
}

// ---- advance_elapsed_clamp ------------------------------------------------

static bool test_clamp_first_call_seeds(TestCtx& t) {
    // prev < 0 => seed to now, no clamp regardless of maxDelta.
    CH_CHECK(t, tmath::advance_elapsed_clamp(-1.0, 42.0, 1.0) == 42.0);
    CH_CHECK(t, tmath::advance_elapsed_clamp(-1.0, 42.0, 0.0) == 42.0);
    return t.failures == 0;
}

static bool test_clamp_small_gap_passes_through(TestCtx& t) {
    // now-prev (0.5) <= maxDelta (1.0) => return now unchanged.
    CH_CHECK(t, tmath::advance_elapsed_clamp(10.0, 10.5, 1.0) == 10.5);
    return t.failures == 0;
}

static bool test_clamp_large_gap_caps_catchup(TestCtx& t) {
    // now-prev (5.0) > maxDelta (1.0) => prev + maxDelta, NOT now.
    CH_CHECK(t, tmath::advance_elapsed_clamp(10.0, 15.0, 1.0) == 11.0);
    return t.failures == 0;
}

static bool test_clamp_disabled_when_maxdelta_nonpositive(TestCtx& t) {
    // maxDelta <= 0 disables clamping => return now even on a huge gap.
    CH_CHECK(t, tmath::advance_elapsed_clamp(10.0, 999.0, 0.0) == 999.0);
    CH_CHECK(t, tmath::advance_elapsed_clamp(10.0, 999.0, -1.0) == 999.0);
    return t.failures == 0;
}

static bool test_clamp_monotonic_never_backward(TestCtx& t) {
    // A stutter sequence: prev advances by at most maxDelta per call, never back.
    double prev = -1.0;
    const double maxDelta = 1.0;
    const double samples[] = {100.0, 100.5, 108.0 /*5s+ stall*/, 108.2, 120.0};
    double last = -1e9;
    for (double now : samples) {
        prev = tmath::advance_elapsed_clamp(prev, now, maxDelta);
        CH_CHECK(t, prev >= last);                  // monotonic non-decreasing
        CH_CHECK(t, prev <= now + 1e-9);            // never ahead of real time
        last = prev;
    }
    // After the big stall, the clamped value lags real time (caught up by <= maxDelta).
    CH_CHECK(t, last < 120.0);
    return t.failures == 0;
}

static bool test_demo_intentional_fail(TestCtx& t) {
    CH_CHECK(t, tmath::ticks_to_ms(10000000ull, 10000000ull) == 9999);  // real 1000
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("timing_math_harness");
    h.add("ticks_freq_zero_is_identity",        test_ticks_freq_zero_is_identity);
    h.add("ticks_windows_formula",              test_ticks_windows_formula);
    h.add("ticks_no_overflow_large",            test_ticks_no_overflow_large);
    h.add("clamp_first_call_seeds",             test_clamp_first_call_seeds);
    h.add("clamp_small_gap_passes_through",     test_clamp_small_gap_passes_through);
    h.add("clamp_large_gap_caps_catchup",       test_clamp_large_gap_caps_catchup);
    h.add("clamp_disabled_when_maxdelta_nonpositive", test_clamp_disabled_when_maxdelta_nonpositive);
    h.add("clamp_monotonic_never_backward",     test_clamp_monotonic_never_backward);
    h.add("demo_intentional_fail",              test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
