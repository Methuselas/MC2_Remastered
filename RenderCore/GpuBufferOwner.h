#pragma once
// RenderCore/GpuBufferOwner.h
// GPU-BUFFER-OWNER-SKELETON-1: first slice of the GPU resource-ownership arc.
//
// The RenderResourceRegistry is DESCRIPTIVE ONLY -- a registered RenderResourceId
// is a name tag, not an owner; the raw GLuint handle is still created/destroyed
// by file-static code. This header adds the OWNER record type: a plain value that
// pairs one GL buffer handle's IDENTITY with its logical id + lifetime + debug
// name. It is the foundation that later slices will use to encapsulate one raw
// GLuint global at a time.
//
// GL-FREE BY CONSTRUCTION. This header includes no GL and calls no GL. It does
// NOT glGenBuffers / glDeleteBuffers / bind -- it only records ownership identity.
// The raw GL handle is stored as a plain uint32_t (the GLuint value), never
// touched. A thin .cpp doing the actual create/destroy/bind is a LATER slice.
//
// Boundaries (matches gpu-buffer-wrapper-design-1.md non-goals, scoped down):
//   - NOT an allocator / sub-allocator. One owner == one GL buffer object.
//   - NO aliasing, NO ring/fence logic, NO Vulkan abstraction.
//   - Plain trivially-copyable record. No RAII, no GL lifetime here.
#include <cstdint>

#include "RenderResourceRegistry.h"  // RenderResourceId, RenderResourceLifetime

namespace RenderCore {

// Ownership identity record for ONE GL buffer object. Plain value type.
//   id        -- the logical RenderResourceId this buffer is registered under.
//   lifetime  -- how long the buffer stays valid (Persistent / Mission / ...).
//   debugName -- string-literal label (KHR_debug / residency key); never heap.
//   glName    -- the raw GLuint handle value (0 == unallocated). Stored, not used.
struct GpuBufferOwner {
    RenderResourceId       id        = RenderResourceId::Unknown;
    RenderResourceLifetime lifetime  = RenderResourceLifetime::Unset;
    const char*            debugName = nullptr;
    uint32_t               glName    = 0;  // GLuint value; 0 = unallocated. Identity only.

    // A fully-formed owner: a real GL handle and a non-null debug label.
    // (Identity invariant only -- says nothing about GL validity, which this
    // GL-free record cannot and must not check.)
    bool valid() const {
        return glName != 0 && debugName != nullptr;
    }
};

} // namespace RenderCore
