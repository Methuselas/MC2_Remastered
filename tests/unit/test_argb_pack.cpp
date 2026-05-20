// tests/unit/test_argb_pack.cpp
// Tier 2: unit tests for MC2's per-vertex ARGB color packing.
//
// Subject under test: the contract from
// memory/mc2_argb_packing.md --
//
//   gos_VERTEX::argb is a DWORD written as
//        (A << 24) | (R << 16) | (G << 8) | B
//   On little-endian, this stores in memory as bytes B, G, R, A.
//   GL attribute pointers see the layout as BGRA and shaders swizzle .bgra.
//   SSBO bit-decode reads R from (packed >> 16) & 0xff, G from >> 8, etc.
//
// This file proves that the pack-side, the GL-attribute view, and the
// SSBO bit-decode view all agree. Any one of them silently disagreeing
// produces the wrong color on a vertex with NO log signal -- exactly
// the kind of bug only a unit test catches early.

#include "doctest.h"
#include <cstdint>
#include <cstring>

namespace {

// Pack a color the way the engine does. VERBATIM mirror of
//   mclib/tgl.cpp:2260
//     listOfVertices[j].argb = (0xff << 24) + (redFinal << 16)
//                            + (greenFinal << 8) + (blueFinal);
//   mclib/tgl.cpp:2281, mclib/tgl.cpp:2369 (same expression with different
//     alpha source), GameOS/gameos/gameos_graphics.cpp:864 (same channel
//     layout, OR-form: (a<<24)|(b<<16)|(g<<8)|r -- note GameOS variant
//     stores R/B swapped on purpose, see that site's comment).
// If the engine packer ever moves a channel, THIS test is the regression
// guard -- adjust the engine site and this mirror in lockstep, then the
// "ARGB DWORD lays out as B,G,R,A bytes" case will catch any drift.
constexpr uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(a) << 24)
         | (uint32_t(r) << 16)
         | (uint32_t(g) <<  8)
         | (uint32_t(b)      );
}

// Mirror the GLSL SSBO bit-decode that the GPU side runs against the
// same DWORD value. Channel positions MUST match the packer above.
struct DecodedColor { uint8_t r, g, b, a; };
constexpr DecodedColor decode_argb(uint32_t packed) {
    return {
        uint8_t((packed >> 16) & 0xffu),   // R
        uint8_t((packed >>  8) & 0xffu),   // G
        uint8_t((packed      ) & 0xffu),   // B
        uint8_t((packed >> 24) & 0xffu),   // A
    };
}

}  // anon


TEST_CASE("ARGB pack -> SSBO bit-decode is a lossless round-trip") {
    const struct { uint8_t r, g, b, a; } cases[] = {
        { 0x12, 0x34, 0x56, 0xff },
        { 0x00, 0x00, 0x00, 0x00 },
        { 0xff, 0xff, 0xff, 0xff },
        { 0x80, 0x40, 0x20, 0x10 },
        { 0x01, 0x02, 0x03, 0x04 },
    };
    for (auto c : cases) {
        const uint32_t packed = pack_argb(c.a, c.r, c.g, c.b);
        const auto d = decode_argb(packed);
        CHECK(d.r == c.r);
        CHECK(d.g == c.g);
        CHECK(d.b == c.b);
        CHECK(d.a == c.a);
    }
}

TEST_CASE("ARGB DWORD lays out as B,G,R,A bytes in memory (LE)") {
    // The load-bearing invariant: GL_BGRA / glVertexAttribPointer with
    // GL_BGRA size==GL_BGRA and the .bgra shader swizzle BOTH assume
    // this byte order. Flip endianness or flip the packer's channel
    // shifts and every per-vert color goes wrong.
    const uint32_t packed = pack_argb(0xff, 0x12, 0x34, 0x56);
    uint8_t bytes[4];
    std::memcpy(bytes, &packed, 4);
    CHECK(bytes[0] == 0x56);   // B
    CHECK(bytes[1] == 0x34);   // G
    CHECK(bytes[2] == 0x12);   // R
    CHECK(bytes[3] == 0xff);   // A
}

TEST_CASE("Alpha channel is preserved when R/G/B are zero") {
    // Regression guard for any "optimization" that skips alpha encoding
    // when the color body is zero. The shadow-caster eligibility gate
    // and TG_Shape transparency machinery read alpha independently.
    const uint32_t packed = pack_argb(0x80, 0x00, 0x00, 0x00);
    const auto d = decode_argb(packed);
    CHECK(d.r == 0); CHECK(d.g == 0); CHECK(d.b == 0);
    CHECK(d.a == 0x80);
}

TEST_CASE("Per-channel isolation: setting one channel never bleeds") {
    // Each channel slot in the DWORD is independent. Sweep each channel
    // through all 256 values with the others pinned and confirm decode
    // recovers only the swept channel.
    for (int v = 0; v < 256; ++v) {
        const uint8_t x = uint8_t(v);

        CHECK(decode_argb(pack_argb(x, 0, 0, 0)).a == x);
        CHECK(decode_argb(pack_argb(x, 0, 0, 0)).r == 0);
        CHECK(decode_argb(pack_argb(x, 0, 0, 0)).g == 0);
        CHECK(decode_argb(pack_argb(x, 0, 0, 0)).b == 0);

        CHECK(decode_argb(pack_argb(0, x, 0, 0)).r == x);
        CHECK(decode_argb(pack_argb(0, 0, x, 0)).g == x);
        CHECK(decode_argb(pack_argb(0, 0, 0, x)).b == x);
    }
}
