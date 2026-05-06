#include "gpu_cull_record.h"
#include <cstdio>
#include <cstddef>
#include <cstring>

namespace gpu_cull {

int gpu_cull_record_selftest() {
    int failures = 0;

    // Verify each field offset matches the std430 spec by reading via raw pointer.
    GpuActorRecord r{};
    const unsigned char* base = reinterpret_cast<const unsigned char*>(&r);

    struct { const char* name; size_t expected; size_t actual; } checks[] = {
        { "worldCenter",       0,  offsetof(GpuActorRecord, worldCenter)       },
        { "boundingRadius",   12,  offsetof(GpuActorRecord, boundingRadius)    },
        { "worldAabbMin",     16,  offsetof(GpuActorRecord, worldAabbMin)      },
        { "category",         28,  offsetof(GpuActorRecord, category)          },
        { "worldAabbMax",     32,  offsetof(GpuActorRecord, worldAabbMax)      },
        { "flags",            44,  offsetof(GpuActorRecord, flags)             },
        { "actorId",          48,  offsetof(GpuActorRecord, actorId)           },
        { "prevVisibilityBit",52,  offsetof(GpuActorRecord, prevVisibilityBit) },
        { "consumerFlags",    56,  offsetof(GpuActorRecord, consumerFlags)     },
    };
    for (auto& c : checks) {
        bool ok = (c.expected == c.actual);
        if (!ok) failures++;
        printf("[GPU_CULL v1] event=selftest_%s case=record_offsets field=%s expected=%zu got=%zu\n",
               ok ? "pass" : "fail", c.name, c.expected, c.actual);
        fflush(stdout);
    }

    // Size check
    {
        bool ok = (sizeof(GpuActorRecord) == 64);
        if (!ok) failures++;
        printf("[GPU_CULL v1] event=selftest_%s case=record_size expected=64 got=%zu\n",
               ok ? "pass" : "fail", sizeof(GpuActorRecord));
        fflush(stdout);
    }
    // Header size check
    {
        bool ok = (sizeof(GpuActorRecordHeader) == 16);
        if (!ok) failures++;
        printf("[GPU_CULL v1] event=selftest_%s case=header_size expected=16 got=%zu\n",
               ok ? "pass" : "fail", sizeof(GpuActorRecordHeader));
        fflush(stdout);
    }
    return failures;
}

} // namespace gpu_cull
