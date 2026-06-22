// tools/ibl_registry_contract_harness/ibl_registry_contract_harness.cpp
// SUBSYSTEM-HARNESS-ARC / IBL-REGISTRY-CONTRACT-HARNESS-1
//
// Registry-integrity harness for the IBL lighting tables. Links ONLY the real
// header-only constexpr registries (RenderCore/IblHdriRegistry.h +
// IblShRegistry.h) — no .cpp, no GL, no game, no 3rdparty. It tests the actual
// production tables (no duplication), guarding the dangerous silent class:
//
//   kSkyNumberHdriMap names a set "qwantani_night", a typo says "qwantani_nite",
//   lookup falls back to "default" with no error, and a 30s smoke still passes.
//
// Asset existence is handled separately from registry integrity (the HDRIs are
// too large for normal Git tracking and may be deployed out-of-band):
//   * default suite: hdri_assets_inventory — INFORMATIONAL, reports missing
//     assets to stderr but PASSES if registry names are valid.
//   * strict:        hdri_assets_exist_strict — runnable via --test, and also
//     run in the default suite when MC2_IBL_ASSET_STRICT=1. FAILS on any absent
//     referenced asset. Lets a clean checkout stay green while still catching
//     drift on demand.
//
// Repo root: compile-time MC2_REPO_ROOT (set by CMake), env override
// MC2_CONTRACT_REPO_ROOT. See docs/testing/lighting-staticprop-harness-recon-1.md.

#include "contract_harness.h"

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "RenderCore/IblHdriRegistry.h"
#include "RenderCore/IblShRegistry.h"

namespace fs = std::filesystem;
using namespace contract_harness;
namespace rc = RenderCore;

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

static fs::path repoRoot() {
    if (const char* e = std::getenv("MC2_CONTRACT_REPO_ROOT"); e && *e)
        return fs::path(e);
    return fs::path(MC2_REPO_ROOT);
}

static bool assetStrictEnv() {
    const char* e = std::getenv("MC2_IBL_ASSET_STRICT");
    return e && e[0] == '1';
}

// An HDRI is "present" if the source .exr OR its cooked .ktx2 sidecar exists.
static bool hdriAssetPresent(const fs::path& root, const std::string& exrPath, std::string& foundAs) {
    std::error_code ec;
    if (fs::is_regular_file(root / exrPath, ec)) { foundAs = exrPath; return true; }
    std::string ktx = exrPath;
    const std::string ext = ".exr";
    if (ktx.size() >= ext.size() && ktx.compare(ktx.size() - ext.size(), ext.size(), ext) == 0)
        ktx.replace(ktx.size() - ext.size(), ext.size(), ".ktx2");
    else
        ktx += ".ktx2";
    if (fs::is_regular_file(root / ktx, ec)) { foundAs = ktx; return true; }
    return false;
}

// ---- registry-integrity tests (pure, no filesystem) ------------------------

static bool test_index0_is_default(TestCtx& t) {
    CH_CHECK(t, rc::kIblHdriSetCount > 0);
    CH_CHECK(t, rc::kIblHdriSets[0].name != nullptr &&
                std::string(rc::kIblHdriSets[0].name) == "default");
    CH_CHECK(t, rc::kIblShSetCount > 0);
    CH_CHECK(t, rc::kIblShSets[0].name != nullptr &&
                std::string(rc::kIblShSets[0].name) == "default");
    return t.failures == 0;
}

// THE typo guard: every kSkyNumberHdriMap entry's set name must resolve.
static bool test_every_skymap_name_resolves(TestCtx& t) {
    for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i) {
        const auto& e = rc::kSkyNumberHdriMap[i];
        if (rc::findHdriSetByName(e.hdriSetName) == nullptr)
            t.fail(std::string("sky ") + std::to_string(e.skyNumber) +
                   " names unresolved HDRI set '" +
                   (e.hdriSetName ? e.hdriSetName : "(null)") + "'");
    }
    return t.failures == 0;
}

// Every sky number 1..21 has an explicit map entry (catches a dropped sky;
// an intentional default would be an explicit entry naming "default").
static bool test_all_sky_1_to_21_mapped(TestCtx& t) {
    for (int sky = 1; sky <= 21; ++sky) {
        bool found = false;
        for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i)
            if (rc::kSkyNumberHdriMap[i].skyNumber == sky) { found = true; break; }
        if (!found) t.fail("sky number " + std::to_string(sky) + " has no map entry");
    }
    return t.failures == 0;
}

static bool test_out_of_range_returns_default(TestCtx& t) {
    const rc::IblHdriSet& d = rc::kIblHdriSets[0];
    for (int sky : {0, 22, -1, INT_MAX, INT_MIN, 1000}) {
        const rc::IblHdriSet& got = rc::lookupHdriForSkyNumber(sky);
        CH_CHECK(t, &got == &d);
    }
    return t.failures == 0;
}

static bool test_no_duplicate_hdri_set_names(TestCtx& t) {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string n = rc::kIblHdriSets[i].name ? rc::kIblHdriSets[i].name : "(null)";
        if (!seen.insert(n).second)
            t.fail("duplicate HDRI set name (shadows lookup-by-name): " + n);
    }
    return t.failures == 0;
}

static bool test_no_duplicate_sh_set_names(TestCtx& t) {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblShSetCount; ++i) {
        std::string n = rc::kIblShSets[i].name ? rc::kIblShSets[i].name : "(null)";
        if (!seen.insert(n).second)
            t.fail("duplicate SH set name: " + n);
    }
    return t.failures == 0;
}

// Every non-null mission->SH mapping must name a set that exists.
static bool test_sh_registry_names_resolve_if_referenced(TestCtx& t) {
    int checked = 0;
    for (size_t i = 0; i < rc::kMissionShMapCount; ++i) {
        const auto& e = rc::kMissionShMap[i];
        if (e.missionName == nullptr && e.shSetName == nullptr) continue;  // placeholder
        ++checked;
        if (rc::findShSetByName(e.shSetName) == nullptr)
            t.fail(std::string("mission '") + (e.missionName ? e.missionName : "(null)") +
                   "' names unresolved SH set '" + (e.shSetName ? e.shSetName : "(null)") + "'");
    }
    std::fprintf(stderr, "    SH mission mappings checked: %d\n", checked);
    return t.failures == 0;
}

// ---- asset existence (separated from integrity) ----------------------------

static void collectHdriMissing(const fs::path& root, std::vector<std::string>& missing) {
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs;
        if (!hdriAssetPresent(root, rc::kIblHdriSets[i].exrPath, foundAs))
            missing.push_back(std::string(rc::kIblHdriSets[i].name) + " -> " +
                              rc::kIblHdriSets[i].exrPath);
    }
}

// Default: INFORMATIONAL inventory — reports missing assets, never fails.
static bool test_hdri_assets_inventory(TestCtx& t) {
    (void)t;
    std::vector<std::string> missing;
    collectHdriMissing(repoRoot(), missing);
    std::fprintf(stderr, "    HDRI assets: %zu sets, %zu missing (informational)\n",
                 rc::kIblHdriSetCount, missing.size());
    for (const auto& m : missing)
        std::fprintf(stderr, "      MISSING %s\n", m.c_str());
    if (!missing.empty())
        std::fprintf(stderr, "    (set MC2_IBL_ASSET_STRICT=1 or run --test "
                             "hdri_assets_exist_strict to make this fail)\n");
    return true;  // informational only
}

// Strict: FAIL on any absent referenced asset. Default-excluded unless
// MC2_IBL_ASSET_STRICT=1; always runnable via --test.
static bool test_hdri_assets_exist_strict(TestCtx& t) {
    std::vector<std::string> missing;
    collectHdriMissing(repoRoot(), missing);
    for (const auto& m : missing)
        t.fail("referenced HDRI asset absent (.exr and .ktx2): " + m);
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("ibl_registry_contract_harness");
    // registry integrity (always default)
    h.add("index0_is_default",                    test_index0_is_default);
    h.add("every_skymap_name_resolves",           test_every_skymap_name_resolves);
    h.add("all_sky_1_to_21_mapped",               test_all_sky_1_to_21_mapped);
    h.add("out_of_range_returns_default",         test_out_of_range_returns_default);
    h.add("no_duplicate_hdri_set_names",          test_no_duplicate_hdri_set_names);
    h.add("no_duplicate_sh_set_names",            test_no_duplicate_sh_set_names);
    h.add("sh_registry_names_resolve_if_referenced", test_sh_registry_names_resolve_if_referenced);
    // asset existence (separated)
    h.add("hdri_assets_inventory",                test_hdri_assets_inventory);
    // strict runs in the default suite only when MC2_IBL_ASSET_STRICT=1
    h.add("hdri_assets_exist_strict",             test_hdri_assets_exist_strict,
          /*inDefault=*/assetStrictEnv());
    return h.run(argc, argv);
}
