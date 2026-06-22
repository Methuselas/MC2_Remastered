// tools/mech_texname_harness/mech_texname_harness.cpp
// SUBSYSTEM-HARNESS-ARC / GLB-TEXNAME-DERIVE-HARNESS-1
//
// Tests the REAL GLB->MC2 texture-name rules (mclib/mech_texname_derive.h, the
// header assimp_importer.cpp now delegates to) game-free — no Assimp, no GL.
// Guards the "derived atlas name is wrong" magenta/black-mech class: dir/ext
// strip, sanitize, 256-clamp off-by-one, the "a_" alpha-cutout prefix, and the
// dropped-mesh (_dmg/_explode/blip/indc/uix) filter. Import is gated off so tier1
// never exercises this; the failure is visual, not a crash — only a unit test
// reliably catches it.
//
// Build (standalone):
//   cmake -S tools/mech_texname_harness -B build64-texname -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-texname --config RelWithDebInfo --target mech_texname_harness

#include "contract_harness.h"
#include "mech_texname_derive.h"

#include <string>

using namespace contract_harness;
namespace mt = mech_texname;

static bool test_strip_path_and_ext(TestCtx& t) {
    CH_CHECK(t, mt::deriveName("textures/MadCat_Body.png", false) == "madcat_body.tga");
    CH_CHECK(t, mt::deriveName("C:\\art\\Foo.PNG", false) == "foo.tga");      // backslash + ext + case
    CH_CHECK(t, mt::deriveName("plain", false) == "plain.tga");               // no dir, no ext
    return t.failures == 0;
}

static bool test_sanitize_nonalnum_to_underscore(TestCtx& t) {
    CH_CHECK(t, mt::deriveName("mad cat#1", false) == "mad_cat_1.tga");       // space + # -> _
    CH_CHECK(t, mt::deriveName("a-b_c9", false) == "a-b_c9.tga");             // - _ digits kept
    return t.failures == 0;
}

static bool test_empty_stem_returns_empty(TestCtx& t) {
    CH_CHECK(t, mt::deriveName("", false).empty());
    CH_CHECK(t, mt::deriveName("dir/.png", false).empty());                  // only an extension
    return t.failures == 0;
}

static bool test_alpha_prefix(TestCtx& t) {
    CH_CHECK(t, mt::deriveName("leaf", true) == "a_leaf.tga");               // wantAlpha -> a_
    CH_CHECK(t, mt::deriveName("leaf", false) == "leaf.tga");                // no prefix when off
    // Idempotent: a name already starting "a_" is not double-prefixed.
    CH_CHECK(t, mt::deriveName("a_card", true) == "a_card.tga");
    return t.failures == 0;
}

static bool test_clamp_fits_256(TestCtx& t) {
    std::string longstem(400, 'x');
    std::string out = mt::deriveName(longstem, false);
    CH_CHECK(t, out.size() + 1 <= 256);                  // stem+".tga"+NUL fits textureName[256]
    CH_CHECK(t, out.size() >= 4 && out.substr(out.size() - 4) == ".tga");
    // With alpha, still fits after the a_ prefix.
    std::string outA = mt::deriveName(longstem, true);
    CH_CHECK(t, outA.size() + 1 <= 256);
    CH_CHECK(t, outA.compare(0, 2, "a_") == 0);
    return t.failures == 0;
}

static bool test_dropped_mesh_filter(TestCtx& t) {
    for (const char* n : {"Mech_dmg", "torso_explode", "BodyBlip", "armINDC", "thing_uix"})
        CH_CHECK(t, mt::isDroppedMeshName(n));           // case-insensitive substring drops
    for (const char* n : {"Marauder_Body", "left_arm", "torso"})
        CH_CHECK(t, !mt::isDroppedMeshName(n));          // intact parts kept
    CH_CHECK(t, !mt::isDroppedMeshName(nullptr));
    return t.failures == 0;
}

static bool test_embedded_index_not_resolved_here(TestCtx& t) {
    // deriveName receives the ALREADY-resolved src; a literal "*0" sanitizes to
    // "_0" (the *N resolution stays in the importer, by design).
    CH_CHECK(t, mt::deriveName("*0", false) == "_0.tga");
    return t.failures == 0;
}

static bool test_demo_intentional_fail(TestCtx& t) {
    CH_CHECK(t, mt::deriveName("body", false) == "WRONG.tga");  // intentionally wrong
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("mech_texname_harness");
    h.add("strip_path_and_ext",            test_strip_path_and_ext);
    h.add("sanitize_nonalnum_to_underscore", test_sanitize_nonalnum_to_underscore);
    h.add("empty_stem_returns_empty",      test_empty_stem_returns_empty);
    h.add("alpha_prefix",                  test_alpha_prefix);
    h.add("clamp_fits_256",                test_clamp_fits_256);
    h.add("dropped_mesh_filter",           test_dropped_mesh_filter);
    h.add("embedded_index_not_resolved_here", test_embedded_index_not_resolved_here);
    h.add("demo_intentional_fail",         test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
