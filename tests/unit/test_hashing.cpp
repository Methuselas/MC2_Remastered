// tests/unit/test_hashing.cpp
// Tier 2: unit tests for the FastFileFind hash path.
//
// Subject under test: `elfHash` (standard Unix ELF hash) and the path-
// normalization step that runs before it in `FastFileFind::find` /
// `File::open`.
//
// Why this exists: the slash-direction trap
// (memory/fst_forward_slash_invariant.md) is the canonical case where a
// unit test would have collapsed a multi-day investigation into 20 lines.
// `elfHash` of "art/foo.tga" != `elfHash` of "art\\foo.tga"; the engine
// normalizes to forward slash before hashing, but only if every caller
// routes through the same entry-point. That invariant is unit-testable
// without an OpenGL context.
//
// SKETCH STATUS: structurally complete. To activate, factor
// `elfHash` and the path normalizer into a leaf TU (e.g.
// mclib/fst_hash.cpp + mclib/fst_hash.h with no transitive includes)
// and add it to mc2_tests' target_sources. Until then, these tests
// reference the symbol by `extern "C"` -- they will link-fail until the
// extraction lands, which is the right pressure.

#include "doctest.h"
#include <cstdint>
#include <string>

// Leaf TU surface from mclib/fst_hash.{h,cpp}. The header carries no
// transitive engine includes; the .cpp is linked into the mc2_tests
// target directly (see tests/unit/CMakeLists.txt target_sources).
#include "fst_hash.h"

TEST_CASE("elfHash empty string is zero") {
    CHECK(elfHash("") == 0u);
}

TEST_CASE("elfHash is deterministic and order-sensitive") {
    const auto a = elfHash("art/objects/mech_atlas.tga");
    const auto b = elfHash("art/objects/mech_atlas.tga");
    const auto c = elfHash("art/objects/mech_atlas.TGA");
    CHECK(a == b);
    CHECK(a != c);          // case-sensitive (engine lowercases earlier)
}

TEST_CASE("FST key normalization collapses backslashes to forward") {
    // The load-bearing invariant from memory/fst_forward_slash_invariant.md.
    // Any caller that hashes a backslash path WITHOUT first running the
    // normalizer gets a silent miss.
    char buf[256];
    fst_normalize_key(buf, "art\\objects\\mech_atlas.tga");
    CHECK(std::string(buf) == "art/objects/mech_atlas.tga");

    // Mixed separators -> all forward.
    fst_normalize_key(buf, "art\\objects/mech_atlas.tga");
    CHECK(std::string(buf) == "art/objects/mech_atlas.tga");

    // Already-normalized input is a no-op.
    fst_normalize_key(buf, "art/objects/mech_atlas.tga");
    CHECK(std::string(buf) == "art/objects/mech_atlas.tga");
}

TEST_CASE("Normalized-then-hashed keys are slash-direction-invariant") {
    // The composite property the engine relies on:
    //   hash(normalize(P_with_backslash)) == hash(normalize(P_with_forward))
    char a[256], b[256];
    fst_normalize_key(a, "art\\objects\\mech_atlas.tga");
    fst_normalize_key(b, "art/objects/mech_atlas.tga");
    CHECK(elfHash(a) == elfHash(b));
}

// TODO when factored: add a known-collision pair test (two different
// strings that produce the same elfHash, to assert the hash table's
// secondary-key comparison path is exercised in CI).
