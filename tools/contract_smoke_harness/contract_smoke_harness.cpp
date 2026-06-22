// tools/contract_smoke_harness/contract_smoke_harness.cpp
// SUBSYSTEM-HARNESS-ARC / HARNESS-TEMPLATE-1 — trivial reference harness.
//
// This harness exists to prove the template infrastructure works end to end:
// CLI parsing, the test registry, PASS/FAIL exit codes, and JSON output. It
// links NO production code and exercises NO real subsystem. Real harnesses
// (objmgr_contract_harness, txmmgr_bounds_harness, ...) are modeled on it.
//
// Build (standalone, does not touch build64/):
//   cmake -S tools/contract_smoke_harness -B build64-contract -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-contract --config RelWithDebInfo --target contract_smoke_harness
//
// Run:
//   contract_smoke_harness --list
//   contract_smoke_harness --test pass         # exit 0
//   contract_smoke_harness --test fail         # exit 1
//   contract_smoke_harness --json --test pass

#include "contract_harness.h"

using namespace contract_harness;

// A test that always passes.
static bool test_pass(TestCtx& t) {
    CH_CHECK(t, 1 + 1 == 2);
    CH_CHECK(t, std::string("abc").size() == 3);
    return true;
}

// A test that always fails — exercises the failure / nonzero-exit path.
static bool test_fail(TestCtx& t) {
    CH_CHECK(t, 2 + 2 == 5);  // intentional failure
    return true;
}

// A test that consumes the --seed flag, demonstrating the randomness convention
// (deterministic given a fixed seed).
static bool test_seeded(TestCtx& t) {
    std::uint64_t x = t.seed * 2654435761ull + 1u;  // trivial deterministic mix
    CH_CHECK(t, (x & 1u) == 1u);                     // always odd by construction
    return true;
}

int main(int argc, char** argv) {
    Harness h("contract_smoke_harness");
    h.add("pass", test_pass);
    h.add("fail", test_fail, /*inDefault=*/false);  // demo-only: runnable via --test fail
    h.add("seeded", test_seeded);
    return h.run(argc, argv);
}
