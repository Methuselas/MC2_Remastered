// tools/objmgr_contract_harness/objmgr_contract_harness.cpp
// OBJMGR-CONTRACT-HARNESS-1 — contract harness for the GameObjectManager watch
// list bounds/index policy.
//
// Links ONLY code/objmgr_watch_policy.h (header-only, pure integer policy) +
// the contract_harness framework. It does NOT compile objmgr.cpp, open a window,
// or load mission data — the linker is the proof it stays game-free. Every test
// forces an exact corrupt/exhaustion/boundary edge case that tier1 smoke cannot
// reach, exercising the SAME functions production calls (no duplicated math).
//
// Replaces the throwaway, logic-duplicating ASan micro-test used to prove
// WATCHID-LOAD-GUARD-1: that test copied the guard; this one shares it.

#include "contract_harness.h"
#include "objmgr_watch_policy.h"

#include <cstdint>
#include <vector>

using namespace contract_harness;
namespace wp = mc2watch;

// watchList[] / objList[] are sized maxObjects+1; valid indices [0, maxObjects].
static constexpr long MAXOBJ = 7;

// --- setWatchID assign policy ------------------------------------------------

static bool test_assign_at_max_succeeds(TestCtx& t) {
    // Assigning the last in-bounds slot (index == maxObjects) must be allowed,
    // and the cursor then lands at maxObjects+1 where the next assign is refused.
    CH_CHECK(t, wp::canAssignWatchId((unsigned long)MAXOBJ, MAXOBJ) == true);
    CH_CHECK(t, wp::canAssignWatchId((unsigned long)MAXOBJ + 1, MAXOBJ) == false);
    // Simulated assign loop, writing into a real array (ASan would trap an OOB).
    std::vector<int> watchList(MAXOBJ + 1, 0);
    unsigned long next = 1;
    int assigned = 0;
    while (wp::canAssignWatchId(next, MAXOBJ)) {
        watchList[next] = (int)next;   // production-side write, must stay in-bounds
        ++next; ++assigned;
    }
    CH_CHECK(t, next == (unsigned long)MAXOBJ + 1);
    CH_CHECK(t, assigned == MAXOBJ);   // slots 1..maxObjects
    return true;
}

static bool test_assign_beyond_max_fails_safely(TestCtx& t) {
    // Exhausted space: predicate false, so production leaves the object
    // un-watchable and never writes watchList[next] (which would be OOB).
    CH_CHECK(t, wp::canAssignWatchId((unsigned long)MAXOBJ + 1, MAXOBJ) == false);
    CH_CHECK(t, wp::canAssignWatchId(1000000ul, MAXOBJ) == false);
    return true;
}

// --- getByWatchID resolve policy ---------------------------------------------

static bool test_resolve_rejects_invalid_ids(TestCtx& t) {
    const unsigned long next = 5;   // live ids are [1,4]
    CH_CHECK(t, wp::isResolvableWatchId(0, next) == false);          // reserved NULL slot
    CH_CHECK(t, wp::isResolvableWatchId(next, next) == false);       // == next: unassigned
    CH_CHECK(t, wp::isResolvableWatchId(next + 1, next) == false);   // > next: too high
    CH_CHECK(t, wp::isResolvableWatchId(1000000ul, next) == false);  // far OOB
    CH_CHECK(t, wp::isResolvableWatchId((unsigned long)-1, next) == false); // wrapped negative
    return true;
}

static bool test_resolve_accepts_live_ids(TestCtx& t) {
    const unsigned long next = 5;
    for (unsigned long id = 1; id < next; ++id)
        CH_CHECK(t, wp::isResolvableWatchId(id, next) == true);
    return true;
}

// --- Save loop-limit clamp ---------------------------------------------------

static bool test_save_loop_limit_never_exceeds_capacity(TestCtx& t) {
    // nextWatchID can legitimately reach maxObjects+1; the scan must still stop
    // at maxObjects (last valid index of the maxObjects+1-sized arrays).
    for (unsigned long next = 0; next <= (unsigned long)MAXOBJ + 4; ++next) {
        long hi = wp::clampSaveLoopLimit(next, MAXOBJ);
        CH_CHECK(t, hi <= MAXOBJ);
        long expect = ((long)next < MAXOBJ) ? (long)next : MAXOBJ;  // min(next,maxObj)
        CH_CHECK(t, hi == expect);
    }
    return true;
}

// --- CopyFrom nextWatchId clamp ----------------------------------------------

static bool test_copyfrom_clamps_corrupt_next(TestCtx& t) {
    const long cap = MAXOBJ + 1;
    CH_CHECK(t, wp::clampSavedNextWatchId(-1, MAXOBJ) == 1ul);            // negative -> empty
    CH_CHECK(t, wp::clampSavedNextWatchId(-99999, MAXOBJ) == 1ul);
    CH_CHECK(t, wp::clampSavedNextWatchId(cap + 1, MAXOBJ) == (unsigned long)cap); // over -> cap
    CH_CHECK(t, wp::clampSavedNextWatchId(0x7fffffff, MAXOBJ) == (unsigned long)cap);
    // In-range values (incl. the boundary cap) pass through unchanged -> no diag.
    CH_CHECK(t, wp::clampSavedNextWatchId(0, MAXOBJ) == 0ul);
    CH_CHECK(t, wp::clampSavedNextWatchId(3, MAXOBJ) == 3ul);
    CH_CHECK(t, wp::clampSavedNextWatchId(cap, MAXOBJ) == (unsigned long)cap);
    return true;
}

// --- Load watchSave index validation -----------------------------------------

static bool test_load_rejects_out_of_range_index(TestCtx& t) {
    CH_CHECK(t, wp::isValidWatchSaveIndex(-1, MAXOBJ) == false);
    CH_CHECK(t, wp::isValidWatchSaveIndex((int32_t)(MAXOBJ + 1), MAXOBJ) == false);
    CH_CHECK(t, wp::isValidWatchSaveIndex(INT32_MAX, MAXOBJ) == false);
    CH_CHECK(t, wp::isValidWatchSaveIndex(INT32_MIN, MAXOBJ) == false);
    return true;
}

static bool test_load_allows_boundary_and_valid_index(TestCtx& t) {
    CH_CHECK(t, wp::isValidWatchSaveIndex(0, MAXOBJ) == true);
    CH_CHECK(t, wp::isValidWatchSaveIndex((int32_t)MAXOBJ, MAXOBJ) == true);  // boundary
    CH_CHECK(t, wp::isValidWatchSaveIndex(3, MAXOBJ) == true);
    return true;
}

static bool test_load_restore_never_derefs_oob(TestCtx& t) {
    // End-to-end of the Load restore loop using the SHARED predicate, over a real
    // objList with deliberately corrupt watchSave. Mirrors the (now retired)
    // WATCHID-LOAD-GUARD-1 ASan micro-test, but via production policy code.
    std::vector<int> objList(MAXOBJ + 1);          // sized maxObjects+1
    for (long i = 0; i <= MAXOBJ; ++i) objList[i] = 0x1000 + (int)i;
    const int32_t watchSave[] = { -1, 0, (int32_t)MAXOBJ, (int32_t)(MAXOBJ + 1),
                                  INT32_MAX, INT32_MIN, 3 };
    std::vector<int> watchList(sizeof(watchSave) / sizeof(watchSave[0]), -777);
    for (size_t j = 0; j < watchList.size(); ++j) {
        const int32_t idx = watchSave[j];
        if (!wp::isValidWatchSaveIndex(idx, MAXOBJ)) {
            watchList[j] = 0;                       // production: invalidate slot
            continue;                               // and never read objList[idx]
        }
        watchList[j] = objList[idx];                // safe: idx in [0,maxObjects]
    }
    // Corrupt entries (j=0,3,4,5) invalidated; valid (j=1,2,6) resolved.
    CH_CHECK(t, watchList[0] == 0);
    CH_CHECK(t, watchList[3] == 0);
    CH_CHECK(t, watchList[4] == 0);
    CH_CHECK(t, watchList[5] == 0);
    CH_CHECK(t, watchList[1] == objList[0]);
    CH_CHECK(t, watchList[2] == objList[MAXOBJ]);   // boundary resolves
    CH_CHECK(t, watchList[6] == objList[3]);
    return true;
}

// --- MF3 generational handle policy ------------------------------------------

static bool test_generation_assign_sets_one(TestCtx& t) {
    // Fresh (never-assigned) slot is gen 0; first assign promotes it to 1.
    CH_CHECK(t, wp::nextGenerationOnAssign(0) == (uint16_t)1);
    // An already-assigned slot keeps its current generation on re-assign.
    CH_CHECK(t, wp::nextGenerationOnAssign(1) == (uint16_t)1);
    CH_CHECK(t, wp::nextGenerationOnAssign(42) == (uint16_t)42);
    return true;
}

static bool test_generation_bump_on_free(TestCtx& t) {
    CH_CHECK(t, wp::bumpGenerationOnFree(1) == (uint16_t)2);
    CH_CHECK(t, wp::bumpGenerationOnFree(0) == (uint16_t)1);
    // Wrap is acceptable (staleness-only): 65535 -> 0.
    CH_CHECK(t, wp::bumpGenerationOnFree((uint16_t)65535) == (uint16_t)0);
    return true;
}

static bool test_generation_stale_vs_fresh(TestCtx& t) {
    // Simulate alloc -> free -> realloc into the SAME slot, then validate handles.
    uint16_t gen = wp::nextGenerationOnAssign(0);   // first assign: gen 1
    const uint16_t oldHandleGen = gen;              // a handle captured while gen==1
    gen = wp::bumpGenerationOnFree(gen);            // slot freed: gen 2
    const uint16_t newHandleGen = wp::nextGenerationOnAssign(gen); // re-assign keeps gen 2
    CH_CHECK(t, newHandleGen == (uint16_t)2);
    // Stale handle (gen 1) must NOT match the slot's current generation.
    CH_CHECK(t, wp::generationMatches(gen, oldHandleGen) == false);
    // Fresh handle (gen 2) must match.
    CH_CHECK(t, wp::generationMatches(gen, newHandleGen) == true);
    return true;
}

// Demo-only (not in the default suite): proves the harness actually fails when a
// guard is wrong, so a green default run is meaningful.
static bool test_demo_intentional_fail(TestCtx& t) {
    CH_CHECK(t, wp::isValidWatchSaveIndex((int32_t)(MAXOBJ + 1), MAXOBJ) == true); // wrong on purpose
    return true;
}

int main(int argc, char** argv) {
    Harness h("objmgr_contract_harness");
    h.add("assign_at_max_succeeds",                 test_assign_at_max_succeeds);
    h.add("assign_beyond_max_fails_safely",         test_assign_beyond_max_fails_safely);
    h.add("resolve_rejects_invalid_ids",            test_resolve_rejects_invalid_ids);
    h.add("resolve_accepts_live_ids",               test_resolve_accepts_live_ids);
    h.add("save_loop_limit_never_exceeds_capacity", test_save_loop_limit_never_exceeds_capacity);
    h.add("copyfrom_clamps_corrupt_next",           test_copyfrom_clamps_corrupt_next);
    h.add("load_rejects_out_of_range_index",        test_load_rejects_out_of_range_index);
    h.add("load_allows_boundary_and_valid_index",   test_load_allows_boundary_and_valid_index);
    h.add("load_restore_never_derefs_oob",          test_load_restore_never_derefs_oob);
    h.add("generation_assign_sets_one",             test_generation_assign_sets_one);
    h.add("generation_bump_on_free",                test_generation_bump_on_free);
    h.add("generation_stale_vs_fresh",              test_generation_stale_vs_fresh);
    h.add("demo_intentional_fail",                  test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
