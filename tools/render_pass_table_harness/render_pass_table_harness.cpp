// tools/render_pass_table_harness/render_pass_table_harness.cpp
// SUBSYSTEM-HARNESS-ARC / RENDER-PASS-TABLE-HARNESS-1
//
// Static-data integrity harness for the descriptive render-pass contract table.
// Links NOTHING — it #includes the real header-only constexpr tables
// (RenderCore/RenderPassContract.h + RenderResourceRegistry.h, both "no GL, no
// game-side includes") and asserts invariants the header itself flags as
// unguarded: its static_assert checks array LENGTH only, not field-value
// staleness (RenderPassContract.h:130-131). This is the cheapest arc tier (no
// .cpp, no GL, no 3rdparty) — same as ibl_registry_contract_harness.
//
// Distinct from render_state_contract_harness (which links render_contract.cpp
// and tests the RUNTIME scope stack + CONTRACT-3 ordering audit). This harness
// tests the STATIC tables only; no runtime, no overlap.
//
// Build (standalone):
//   cmake -S tools/render_pass_table_harness -B build64-rptable -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-rptable --config RelWithDebInfo --target render_pass_table_harness

#include "contract_harness.h"

#include <set>
#include <string>

#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"

using namespace contract_harness;
namespace rc = RenderCore;

static const auto& kPasses = rc::kRenderPassContracts;
static constexpr int kPassCount = rc::kRenderPassContractCount;

static uint32_t rid(rc::RenderResourceId r) { return (uint32_t)r; }
static constexpr uint32_t kResUnknown = (uint32_t)rc::RenderResourceId::Unknown; // 0
static constexpr uint32_t kResCount   = (uint32_t)rc::RenderResourceId::Count;   // one past last

// Resources legitimately read but produced OUTSIDE the pass table (uploaded at
// load, owned by another subsystem). Empty today — every read resource has an
// in-table writer. A future externally-produced read must be added here
// EXPLICITLY (never a silent closure pass).
static const std::set<uint32_t> kExternallyProducedReads = {
    // e.g. (uint32_t)rc::RenderResourceId::TerrainHeightTexture, when a pass
    // begins sampling it and it is uploaded outside any pass's writes[].
};

// ---- tests -----------------------------------------------------------------

// Runtime mirror of the header's static_assert (cheap, documents the invariant).
static bool test_count_matches_enum(TestCtx& t) {
    CH_CHECK(t, kPassCount == (int)rc::kRenderPassIdCount);
    return t.failures == 0;
}

static bool test_unique_pass_ids(TestCtx& t) {
    std::set<uint32_t> seen;
    for (int i = 0; i < kPassCount; ++i) {
        uint32_t id = (uint32_t)kPasses[i].id;
        if (!seen.insert(id).second)
            t.fail("duplicate pass id at row " + std::to_string(i) + ": " + std::to_string(id));
    }
    return t.failures == 0;
}

// Table row order must match the enum: row i is id (i+1) (ids start at 1).
static bool test_id_by_value_matches_table_index(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) {
        uint32_t id = (uint32_t)kPasses[i].id;
        CH_CHECK(t, id == (uint32_t)(i + 1));
        if (id != (uint32_t)(i + 1))
            t.fail("row " + std::to_string(i) + " id=" + std::to_string(id) +
                   " expected " + std::to_string(i + 1) + " (row/enum order drift)");
    }
    // Every non-sentinel enum value 1..kRenderPassIdCount is present exactly once.
    for (uint32_t want = 1; want <= rc::kRenderPassIdCount; ++want) {
        int n = 0;
        for (int i = 0; i < kPassCount; ++i) if ((uint32_t)kPasses[i].id == want) ++n;
        if (n != 1) t.fail("enum value " + std::to_string(want) + " mapped " +
                           std::to_string(n) + " times (expected 1)");
    }
    return t.failures == 0;
}

static bool test_pass_names_nonempty_unique(TestCtx& t) {
    std::set<std::string> seen;
    for (int i = 0; i < kPassCount; ++i) {
        const char* nm = kPasses[i].name;
        if (!nm || nm[0] == '\0') { t.fail("row " + std::to_string(i) + " has empty name"); continue; }
        if (!seen.insert(nm).second) t.fail(std::string("duplicate pass name: ") + nm);
    }
    return t.failures == 0;
}

// Required descriptive strings must be present (killSwitchEnv/notes are nullable).
static bool test_required_strings_present(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) {
        const auto& p = kPasses[i];
        const std::string where = std::string("'") + (p.name ? p.name : "?") + "'";
        if (!p.ownerSubsystem || !p.ownerSubsystem[0]) t.fail(where + " ownerSubsystem empty");
        if (!p.inspectorSectionId || !p.inspectorSectionId[0]) t.fail(where + " inspectorSectionId empty");
    }
    return t.failures == 0;
}

static bool resourcesInRange(TestCtx& t, const rc::RenderResourceId arr[4], const char* which, const char* pass) {
    for (int j = 0; j < 4; ++j) {
        uint32_t r = rid(arr[j]);
        if (r == kResUnknown) continue;
        if (r >= kResCount)
            t.fail(std::string(pass) + " " + which + "[" + std::to_string(j) + "] id=" +
                   std::to_string(r) + " out of range [1," + std::to_string(kResCount) + ")");
    }
    return t.failures == 0;
}

static bool test_read_resource_ids_in_range(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) resourcesInRange(t, kPasses[i].reads, "reads", kPasses[i].name);
    return t.failures == 0;
}

static bool test_write_resource_ids_in_range(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) resourcesInRange(t, kPasses[i].writes, "writes", kPasses[i].name);
    return t.failures == 0;
}

// Once the Unknown(0) terminator appears, all later slots must be Unknown too
// (a non-Unknown after Unknown is a silent truncation — consumers stop at the
// first Unknown and miss it).
static bool noMidTerminator(TestCtx& t, const rc::RenderResourceId arr[4], const char* which, const char* pass) {
    bool seenTerm = false;
    for (int j = 0; j < 4; ++j) {
        bool isUnknown = rid(arr[j]) == kResUnknown;
        if (isUnknown) seenTerm = true;
        else if (seenTerm)
            t.fail(std::string(pass) + " " + which + " has a non-Unknown entry after the "
                   "terminator at slot " + std::to_string(j) + " (silent truncation)");
    }
    return t.failures == 0;
}

static bool test_no_mid_array_terminator_in_reads(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) noMidTerminator(t, kPasses[i].reads, "reads", kPasses[i].name);
    return t.failures == 0;
}

static bool test_no_mid_array_terminator_in_writes(TestCtx& t) {
    for (int i = 0; i < kPassCount; ++i) noMidTerminator(t, kPasses[i].writes, "writes", kPasses[i].name);
    return t.failures == 0;
}

// Collect the set of resources written by ANY pass.
static std::set<uint32_t> collectWrittenResources() {
    std::set<uint32_t> w;
    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].writes[j]);
            if (r != kResUnknown) w.insert(r);
        }
    return w;
}

// Every resource a pass READS must be WRITTEN by some pass, unless explicitly
// allowlisted as externally-produced.
static bool test_every_read_has_static_writer(TestCtx& t) {
    const std::set<uint32_t> written = collectWrittenResources();
    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].reads[j]);
            if (r == kResUnknown) continue;
            if (written.count(r) || kExternallyProducedReads.count(r)) continue;
            t.fail(std::string(kPasses[i].name) + " reads resource " + std::to_string(r) +
                   " that NO pass writes (and it is not allowlisted external)");
        }
    return t.failures == 0;
}

// CONTRACT-3 closure: the documented cross-pass dependency (ShadowDynamicMap,
// read by opaque passes and written by Shadow) must have a producer. This is the
// static form of render_state_harness's dynamic "missing writer" test.
static bool test_producer_consumer_closure_for_contract3(TestCtx& t) {
    const std::set<uint32_t> written = collectWrittenResources();
    const uint32_t shadowDyn = (uint32_t)rc::RenderResourceId::ShadowDynamicMap;
    // It is actually read by some pass...
    bool readSomewhere = false;
    for (int i = 0; i < kPassCount && !readSomewhere; ++i)
        for (int j = 0; j < 4; ++j)
            if (rid(kPasses[i].reads[j]) == shadowDyn) { readSomewhere = true; break; }
    if (readSomewhere)
        CH_CHECK(t, written.count(shadowDyn) > 0);  // ...so it must have a producer
    // General closure: no read resource lacks a writer (mirrors #8, asserted as
    // the CONTRACT-3 invariant over the whole table).
    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].reads[j]);
            if (r == kResUnknown || written.count(r) || kExternallyProducedReads.count(r)) continue;
            t.fail("CONTRACT-3 closure broken: resource " + std::to_string(r) +
                   " is read but never written");
        }
    return t.failures == 0;
}

// RENDER-PASS-DAG-CONTRACT-1: kFramePassOrder lists every real RenderPassId
// (1..kRenderPassIdCount) exactly once, with no duplicates and no None(0).
static bool test_frame_pass_order_complete(TestCtx& t) {
    CH_CHECK(t, rc::kFramePassOrderCount == (int)rc::kRenderPassIdCount);

    std::set<uint32_t> seen;
    for (int i = 0; i < rc::kFramePassOrderCount; ++i) {
        uint32_t id = (uint32_t)rc::kFramePassOrder[i];
        if (id == (uint32_t)rc::RenderPassId::None) {
            t.fail("kFramePassOrder slot " + std::to_string(i) + " is None (not a real pass)");
            continue;
        }
        if (id < 1 || id > rc::kRenderPassIdCount)
            t.fail("kFramePassOrder slot " + std::to_string(i) + " id=" + std::to_string(id) +
                   " out of range [1," + std::to_string(rc::kRenderPassIdCount) + "]");
        if (!seen.insert(id).second)
            t.fail("kFramePassOrder duplicate id " + std::to_string(id) +
                   " at slot " + std::to_string(i));
    }
    // Every real enum value appears in the order list.
    for (uint32_t want = 1; want <= rc::kRenderPassIdCount; ++want)
        if (!seen.count(want))
            t.fail("kFramePassOrder missing RenderPassId value " + std::to_string(want));
    return t.failures == 0;
}

// RENDER-PASS-DAG-CONTRACT-1: ordered-DAG read-before-write. Walking
// kFramePassOrder in execution order, every resource a pass reads[] must have
// been written by SOME earlier pass in the order (or be allowlisted external).
// This is the dynamic "every read has a prior writer" DAG invariant, distinct
// from the order-agnostic test_every_read_has_static_writer above.
static bool test_ordered_dag_read_before_write(TestCtx& t) {
    std::set<uint32_t> writtenSoFar = kExternallyProducedReads;
    for (int i = 0; i < rc::kFramePassOrderCount; ++i) {
        const rc::RenderPassContract* p = nullptr;
        for (int k = 0; k < kPassCount; ++k)
            if (kPasses[k].id == rc::kFramePassOrder[i]) { p = &kPasses[k]; break; }
        if (!p) {
            t.fail("kFramePassOrder slot " + std::to_string(i) +
                   " has no matching contract row");
            continue;
        }
        // Check this pass's reads against everything written by EARLIER passes.
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(p->reads[j]);
            if (r == kResUnknown) break;
            if (!writtenSoFar.count(r))
                t.fail(std::string(p->name) + " reads " + std::to_string(r) +
                       " but no earlier pass in kFramePassOrder writes it "
                       "(ordered-DAG read-before-write violation)");
        }
        // Now publish this pass's writes for downstream passes.
        for (int j = 0; j < 4; ++j) {
            uint32_t w = rid(p->writes[j]);
            if (w == kResUnknown) break;
            writtenSoFar.insert(w);
        }
    }
    return t.failures == 0;
}

// Demo failure (inDefault=false): proves the failure path via --test only.
static bool test_demo_intentional_fail(TestCtx& t) {
    CH_CHECK(t, kPassCount == 999);  // intentionally wrong
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("render_pass_table_harness");
    h.add("count_matches_enum",                       test_count_matches_enum);
    h.add("unique_pass_ids",                          test_unique_pass_ids);
    h.add("id_by_value_matches_table_index",          test_id_by_value_matches_table_index);
    h.add("pass_names_nonempty_unique",               test_pass_names_nonempty_unique);
    h.add("required_strings_present",                  test_required_strings_present);
    h.add("read_resource_ids_in_range",               test_read_resource_ids_in_range);
    h.add("write_resource_ids_in_range",              test_write_resource_ids_in_range);
    h.add("no_mid_array_terminator_in_reads",         test_no_mid_array_terminator_in_reads);
    h.add("no_mid_array_terminator_in_writes",        test_no_mid_array_terminator_in_writes);
    h.add("every_read_has_static_writer",             test_every_read_has_static_writer);
    h.add("producer_consumer_closure_for_contract3",  test_producer_consumer_closure_for_contract3);
    h.add("frame_pass_order_complete",                 test_frame_pass_order_complete);
    h.add("ordered_dag_read_before_write",             test_ordered_dag_read_before_write);
    h.add("demo_intentional_fail",                     test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
