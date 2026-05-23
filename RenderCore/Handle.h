// RenderCore/Handle.h
//
// Slice M1 (route-only): type-safe opaque handle.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 3 (handle model), 20/12 bit split (Q3.1 RESOLVED 2026-05-22).
//
// Invariants (load-bearing):
//   - Holder MUST NOT interpret `index` or `generation`.
//   - Handle::invalid() is the ONLY sentinel; do not overload with -1 / 0.
//   - Generation MUST be bumped when a slot is recycled.
//   - Equality is bitwise; trivially hashable by uint32_t pun.
//
// Phase 1 documentary: M1 does not yet recycle slots (recipe path is
// mission-lifetime). Generation defaults to 1 on first construction so
// that the canonical invalid() (index=0, generation=0) cannot collide
// with the first legitimate live handle.

#pragma once

#include <cstdint>

namespace RenderCore {

// M4 fix (adversarial review pass 2 2026-05-22): explicit uint32_t
// shift/mask, NOT bitfields. C++ bitfield layout is implementation-defined;
// future Vulkan / cross-compiler ports need the bit ordering to be
// well-defined. Wire encoding: bits[19:0] = index, bits[31:20] = generation.
template <typename Tag>
struct Handle {
    uint32_t bits = 0;  // [19:0] index, [31:20] generation

    static constexpr Handle make(uint32_t index, uint32_t generation) noexcept {
        Handle h;
        h.bits = (generation << 20) | (index & 0xFFFFFu);
        return h;
    }

    [[nodiscard]] constexpr uint32_t index() const noexcept {
        return bits & 0xFFFFFu;
    }
    [[nodiscard]] constexpr uint32_t generation() const noexcept {
        return bits >> 20;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return bits != 0;
    }

    [[nodiscard]] static constexpr Handle invalid() noexcept {
        return Handle{0u};
    }

    [[nodiscard]] constexpr uint32_t raw() const noexcept {
        return bits;
    }

    constexpr bool operator==(Handle o) const noexcept { return bits == o.bits; }
    constexpr bool operator!=(Handle o) const noexcept { return bits != o.bits; }
};

// Tag types — empty structs purely for nominal typing of Handle<>.
struct RenderObjectTag {};
struct ViewTag        {};
struct MeshTag        {};
struct MaterialTag    {};
struct TextureTag     {};

using RenderObjectHandle = Handle<RenderObjectTag>;
using ViewHandle         = Handle<ViewTag>;
using MeshHandle         = Handle<MeshTag>;
using MaterialHandle     = Handle<MaterialTag>;
using TextureHandle      = Handle<TextureTag>;

// Compile-time invariants.
static_assert(sizeof(RenderObjectHandle) == sizeof(uint32_t),
              "Handle must be 32 bits wide for hot-path passing.");

} // namespace RenderCore
