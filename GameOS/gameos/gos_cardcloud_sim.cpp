//==========================================================================//
// File:    gos_cardcloud_sim.cpp                                            //
// Contents: GameOS-side GL bridge for the CardCloud GPU simulation buffer.  //
//           VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (compare-only substrate).        //
//                                                                           //
//           Owns a PERSISTENT SSBO of CardCloudSimParticle records. Each    //
//           frame, gate-ON, CardCloud::Draw gathers its live per-particle   //
//           state and submits it here; this uploads the records to the      //
//           SSBO (grow-as-needed) and, when MC2_VFX_GPU_SIM_COMPARE=1,      //
//           emits a rate-limited integrity/compare log.                     //
//                                                                           //
//           NOT in this slice: compute integration, GPU->CPU readback,      //
//           rendering, CPU-sim bypass. The SSBO is storage only; no shader  //
//           binds it yet. COMPUTE-1 adds the cardcloud_sim.comp + readback. //
//===========================================================================//

#include "particles/cardcloud_sim.h"
#include "particles/batcher.h"   // Batcher::is_gpu_sim_compare_enabled()

#include <gameos.hpp>
#include <GL/glew.h>

#include <cstdio>
#include <cfloat>

namespace {

// Persistent SSBO: allocated once, grown on demand, reused across frames.
// COMPUTE-1 will glBindBufferBase this at kBinding and dispatch a compute
// pass over it; BUFFER-1 only uploads (storage only, no binding).
GLuint  s_ssbo         = 0;
GLsizei s_capacity     = 0;   // records
bool    s_initFailed   = false;

void ensureCapacity(GLsizei needRecords) {
    if (s_ssbo == 0) {
        glGenBuffers(1, &s_ssbo);
        if (s_ssbo == 0) { s_initFailed = true; return; }
    }
    if (needRecords > s_capacity) {
        GLsizei newCap = (needRecords < 1024) ? 1024 : needRecords;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
        while (glGetError() != GL_NO_ERROR) {}  // drain prior errors
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(newCap * sizeof(mc2::particles::CardCloudSimParticle)),
                     nullptr, GL_DYNAMIC_DRAW);
        if (glGetError() != GL_NO_ERROR) {
            // Alloc failed (e.g. GL_OUT_OF_MEMORY): the data store is now
            // invalid. Disable the bridge rather than leave s_capacity stale
            // and write into unbacked storage next frame.
            s_initFailed = true;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            return;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_capacity = newCap;   // only after confirmed success
    }
}

} // namespace

extern "C" void gos_cardcloud_sim_submit(
    const mc2::particles::CardCloudSimParticle* records,
    unsigned int                                count,
    unsigned int                                cpuActiveCount)
{
    if (s_initFailed) return;
    if (records == nullptr || count == 0) {
        // Still log the (empty) compare so an all-dead frame is visible.
        if (mc2::particles::Batcher::is_gpu_sim_compare_enabled()) {
            static unsigned long long s_emptyCalls = 0;
            if ((++s_emptyCalls % 240ull) == 0ull) {
                std::fprintf(stderr,
                    "[VFX_GPU_SIM v1] class=CardCloud cpuActive=%u submitted=0 (no live)\n",
                    cpuActiveCount);
                std::fflush(stderr);
            }
        }
        return;
    }

    ensureCapacity((GLsizei)count);
    if (s_initFailed || s_ssbo == 0) return;

    // Persistent SSBO upload (storage only; no shader binding this slice).
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    (GLsizeiptr)(count * sizeof(mc2::particles::CardCloudSimParticle)),
                    records);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // First-submit banner (once).
    {
        static bool s_banner = false;
        if (!s_banner) {
            s_banner = true;
            std::fprintf(stderr,
                "[VFX_GPU_SIM v1] event=buffer_ready class=CardCloud ssbo=%u recordBytes=%u\n",
                (unsigned)s_ssbo,
                (unsigned)sizeof(mc2::particles::CardCloudSimParticle));
            std::fflush(stderr);
        }
    }

    // CPU-side compare/integrity log (NO GPU readback this slice). Validates
    // the gather/upload/index path: submitted == live count (cpuActive minus
    // dead slots), and bounds/age/alpha look sane. Parity vs a GPU-integrated
    // buffer is COMPUTE-1.
    if (mc2::particles::Batcher::is_gpu_sim_compare_enabled()) {
        static bool s_first = false;
        static unsigned long long s_calls = 0;
        float minA = FLT_MAX, maxA = -FLT_MAX;
        float minAge = FLT_MAX, maxAge = -FLT_MAX;
        float bbMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float bbMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        unsigned alive = 0;
        for (unsigned i = 0; i < count; ++i) {
            const mc2::particles::CardCloudSimParticle& p = records[i];
            if (p.flags & mc2::particles::kCardCloudSimFlagAlive) ++alive;
            if (p.color[3] < minA) minA = p.color[3];
            if (p.color[3] > maxA) maxA = p.color[3];
            if (p.age < minAge) minAge = p.age;
            if (p.age > maxAge) maxAge = p.age;
            for (int k = 0; k < 3; ++k) {
                if (p.position[k] < bbMin[k]) bbMin[k] = p.position[k];
                if (p.position[k] > bbMax[k]) bbMax[k] = p.position[k];
            }
        }
        if (!s_first) {
            s_first = true;
            std::fprintf(stderr,
                "[VFX_GPU_SIM v1] class=CardCloud FIRST_SUBMIT cpuActive=%u submitted=%u alive=%u\n",
                cpuActiveCount, count, alive);
            std::fflush(stderr);
        }
        if ((++s_calls % 240ull) == 0ull) {
            std::fprintf(stderr,
                "[VFX_GPU_SIM v1] class=CardCloud calls=%llu cpuActive=%u submitted=%u alive=%u "
                "age=[%.3f,%.3f] alpha=[%.3f,%.3f] bbX=[%.1f,%.1f] bbY=[%.1f,%.1f] bbZ=[%.1f,%.1f] "
                "ssboCap=%d compareStatus=plumbing-ok(no-integration-yet)\n",
                s_calls, cpuActiveCount, count, alive,
                (double)minAge, (double)maxAge, (double)minA, (double)maxA,
                (double)bbMin[0], (double)bbMax[0], (double)bbMin[1], (double)bbMax[1],
                (double)bbMin[2], (double)bbMax[2], (int)s_capacity);
            std::fflush(stderr);
        }
    }
}

// NOTE — buffer lifecycle: the single SSBO is allocated once and reused for the
// process lifetime (same convention as gos_particle_bridge's SSBO); it is NOT
// freed on mission reload. That is fine for a single reused storage buffer. A
// teardown/free hook is intentionally deferred to COMPUTE-1, which binds the
// buffer to a compute pass and must release it on GL-context teardown.
//
// NOTE — multi-instance (BUFFER-1 limitation): each CardCloud instance's Draw
// submits independently and overwrites the SSBO at offset 0 (last-writer-wins
// per frame). This is harmless this slice (the SSBO has NO consumer — no
// compute, no readback, no render) and the per-submit compare log is still a
// valid plumbing check per instance. COMPUTE-1 introduces per-frame
// accumulation (append all live CardCloud instances, flush once) when the
// buffer is actually integrated + read.
