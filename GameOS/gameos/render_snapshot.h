// GameOS/gameos/render_snapshot.h
//
// Per-frame RenderSnapshot: immutable frame-lifetime view of render
// state populated by ExtractRenderSnapshot() from engine layers.
//
// Extraction v1 (2026-05-24): static-prop snapshot populated alongside
// v0 mech and light records. Arena 1 MiB per slot (2 MiB ping-pong total).
// v1.1 (2026-05-25): texArrayLayer/materialIdx wired; material cache counters added.
// Ping-pong: two module-static RenderFrameArena instances alternate each frame;
// no per-frame heap allocation. Snapshot is valid only for the current frame.

#pragma once

#include "../RenderCore/Handle.h"
#include <cstdint>
#include <cstdlib>
#include <memory>

// Frame arena management: persistent allocator with per-frame reset.
// Two module-static instances live in render_snapshot.cpp; ExtractRenderSnapshot()
// flips between them each call. After the flip the idle arena is reset and
// used for the new frame. Span pointers in RenderSnapshot are valid only for
// the frame in which they were produced — do not hold them past the call site.
class RenderFrameArena {
public:
    RenderFrameArena() = default;
    ~RenderFrameArena() = default;

    // Allocate n elements of type T from the current arena.
    // Returns nullptr on overflow (snapshot->arenaOverflow set by caller).
    template <typename T>
    T* alloc(size_t count) {
        size_t bytes = count * sizeof(T);
        if (m_used + bytes > kArenaBytes) {
            return nullptr;  // overflow
        }
        T* ptr = reinterpret_cast<T*>(m_buffer.get() + m_used);
        m_used += bytes;
        return ptr;
    }

    // Reset for next frame; caller alternates between two instances.
    void reset() {
        m_used = 0;
    }

    size_t bytesUsed() const { return m_used; }

private:
    static constexpr size_t kArenaBytes = 1024u * 1024u; // 1 MiB per slot; 2 MiB total ping-pong
    std::unique_ptr<uint8_t[]> m_buffer{new uint8_t[kArenaBytes]};
    size_t m_used = 0;
};

// --- v0: mech extraction record ---
struct ExtractedMech {
    // Placeholder for v0 fields; implementation deferred.
    // Expected fields: handle, typeId, transform, etc.
};

// --- v0: light record ---
struct LightRecord {
    // Placeholder for v0 fields; implementation deferred.
    // Expected fields: position, color, falloff, etc.
};

// --- v1: static-prop snapshot record ---
// Extraction v1: static-prop snapshot record.
// Fields marked "sentinel v1" are typed for v1.1/v2 but not authoritative here.
// Sentinel values mean "not yet extracted" — callers must not treat them as valid data.
struct ExtractedStaticProp {
    // --- identity (authoritative v1) ---
    RenderCore::RenderObjectHandle rwHandle;      // validated alive+generation by fillStaticPropSlots
    int32_t            recipeIndex;   // == rwHandle.index() by construction
    uint32_t           typeId;        // from GpuStaticPropInstance.typeID

    // --- transform (authoritative v1) ---
    // Row-major matrix from GpuStaticPropInstance.modelMatrix.
    // Translation is at [3],[7],[11] with Stuff-space axis swap:
    //   worldCenterX = -worldMatrix[3]   (MC2 east)
    //   worldCenterY =  worldMatrix[11]  (MC2 north)
    //   worldCenterZ =  worldMatrix[7]   (MC2 elevation)
    float worldMatrix[16];
    float worldCenterX;
    float worldCenterY;
    float worldCenterZ;
    // RecipeRange::extentRadius from previous frame's markVisible(); 0.0f if never visible.
    float boundingRadius;

    // --- material / lighting (sentinel v1) ---
    uint32_t materialIdx;    // sentinel 0xFFFFFFFFu — MaterialGpu wiring is v1.1
    int32_t  texArrayLayer;  // sentinel -1
    // RecipeRange::lightDataIndex; 0xFFFFFFFFu if getter failed.
    uint32_t lightDataIndex;

    // --- cull / AABB (sentinel v1) ---
    bool hasCullRecord; // false — no CPU GpuActorRecord accessor exists yet
    // AABB fields deferred until CPU mirror accessor is added
};

static_assert(sizeof(ExtractedStaticProp) <= 160,
    "ExtractedStaticProp exceeded 160-byte budget; adjust arena sizing");

// Simple span wrapper for array views.
template <typename T>
struct Span {
    T* data = nullptr;
    size_t count = 0;

    Span() = default;
    Span(T* d, size_t c) : data(d), count(c) {}

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    T* begin() { return data; }
    T* end() { return data + count; }
    const T* begin() const { return data; }
    const T* end() const { return data + count; }
};

// Per-frame render snapshot: immutable view of extracted engine state.
struct RenderSnapshot {
    // Frame identity
    uint64_t frameIndex = 0;

    // --- v0: mech + light records ---
    Span<ExtractedMech> mechs;
    Span<LightRecord> lights;

    // --- v1: static-prop snapshot ---
    Span<ExtractedStaticProp> staticProps;           // populated by ExtractRenderSnapshot v1
    uint32_t staticPropValidationFail = 0;           // hard gate: must be 0
    uint32_t staticPropSentinelMat    = 0;           // v1: expected == staticProps.size(); v1.1: < size when MC2_MATERIAL_GPU=1
    uint32_t staticPropSentinelCull   = 0;           // expected == staticProps.size() in v1

    // v1.1 material/texture wiring counters
    uint32_t staticPropTexWired       = 0;  // texArrayLayer != -1
    uint32_t staticPropTexSentinel    = 0;  // texArrayLayer == -1 (no valid primary)
    uint32_t staticPropMatWired       = 0;  // materialIdx != 0xFFFFFFFF (MC2_MATERIAL_GPU=1 only)
    uint32_t staticPropMatSentinel    = 0;  // materialIdx == 0xFFFFFFFF
    uint32_t staticPropPrimaryAlphaOn = 0;  // primary came from alpha-on fallback
    uint32_t staticPropMultiPacket    = 0;  // type has > 1 packet

    // Non-owning pointer to the current frame's ping-pong arena.
    // Owned by module statics in render_snapshot.cpp; valid for this frame only.
    // Do NOT hold this pointer past the frame — the arena is reset on the next call.
    RenderFrameArena* arena = nullptr;
    bool arenaOverflow = false;
};

// Extraction entry point — implemented in render_snapshot.cpp.
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot backed by a frame-lifetime arena (snap.arena).
RenderSnapshot ExtractRenderSnapshot();
