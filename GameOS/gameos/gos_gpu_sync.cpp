#include "gos_gpu_sync.h"

#include <GL/glew.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {

// The audited edge -> barrier-bits table. ONLY edges used in this slice are
// mapped (v1, no overgeneralization). Add a new edge here -- in one place --
// when a new compute->draw / clear->shader / coherent-write->read path appears.
GLbitfield barrierBitsFor(GpuProducer p, GpuConsumer c) {
    // A CPU write through a persistent GL_MAP_COHERENT_BIT mapping is made
    // visible without an explicit flush, but ordering it BEFORE a subsequent
    // server-side read still requires GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. Add
    // the SSBO bit when the consumer reads the buffer as shader storage.
    if (p == GpuProducer::CpuCoherentWrite) {
        if (c == GpuConsumer::BufferCopy)
            return GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
        if (c == GpuConsumer::InstancedDraw ||
            c == GpuConsumer::MultiDrawIndirect ||
            c == GpuConsumer::ShaderStorageRead)
            return GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT;
    }
    // A buffer cleared via glClear* then read/atomic'd by a compute shader: the
    // clear is a buffer-update; the compute access is shader-storage.
    if (p == GpuProducer::ClearBuffer && c == GpuConsumer::ComputeShader)
        return GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
    // A compute dispatch that wrote an indirect command buffer, consumed by an
    // indirect draw: command-barrier orders the args; SSBO bit covers same-buffer
    // shader reads.
    if (p == GpuProducer::ComputeShader && c == GpuConsumer::MultiDrawIndirect)
        return GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT;
    // A compute dispatch that wrote an SSBO, consumed by a later shader stage.
    if (p == GpuProducer::ComputeShader &&
        (c == GpuConsumer::ShaderStorageRead || c == GpuConsumer::ComputeShader))
        return GL_SHADER_STORAGE_BARRIER_BIT;
    return 0;  // unmapped edge
}

bool traceEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_GPU_SYNC_TRACE");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

}  // namespace

int gpuSsboOffsetAlignment() {
    static GLint s_align = 0;
    if (s_align == 0) {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &s_align);
        if (s_align < 16) s_align = 256;  // sane fallback (NVIDIA = 256, AMD ~32)
        std::fprintf(stderr, "[GPU_ALIGN] SSBO_OFFSET_ALIGNMENT=%d\n", (int)s_align);
    }
    return (int)s_align;
}

void gpuBindSsboRange(unsigned int index, unsigned int buffer,
                      long long offset, long long size, const char* tag) {
    const long long a = (long long)gpuSsboOffsetAlignment();
    if (a > 0 && (offset % a) != 0) {
        static std::set<std::string> s_seen;  // once per tag, no per-frame spam
        const std::string key = tag ? tag : "(none)";
        if (s_seen.insert(key).second) {
            std::fprintf(stderr,
                "[GPU_ALIGN] MISALIGNED tag=%s offset=%lld align=%lld "
                "(remainder=%lld) -- glBindBufferRange will fail on NVIDIA\n",
                key.c_str(), offset, a, offset % a);
            std::fflush(stderr);
        }
    }
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, (GLuint)index, (GLuint)buffer,
                      (GLintptr)offset, (GLsizeiptr)size);
}

void gpuSyncBarrier(GpuProducer producer, GpuConsumer consumer, const char* tag) {
    GLbitfield bits = barrierBitsFor(producer, consumer);
    if (bits == 0) {
        // Unmapped edge == a contract miswire. Be loud and fail safe (never
        // silently skip a barrier -- that is exactly the bug class this helper
        // exists to kill).
        std::fprintf(stderr,
            "[GPU_SYNC] WARNING unmapped edge tag=%s producer=%d consumer=%d "
            "-> GL_ALL_BARRIER_BITS (add the edge to gos_gpu_sync.cpp)\n",
            tag ? tag : "(none)", (int)producer, (int)consumer);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        return;
    }
    glMemoryBarrier(bits);
    if (traceEnabled()) {
        static std::set<std::string> s_seen;  // once-per-tag, no per-frame spam
        const std::string key = tag ? tag : "(none)";
        if (s_seen.insert(key).second) {
            std::fprintf(stderr,
                "[GPU_SYNC] tag=%s producer=%d consumer=%d bits=0x%x\n",
                key.c_str(), (int)producer, (int)consumer, (unsigned)bits);
        }
    }
}
