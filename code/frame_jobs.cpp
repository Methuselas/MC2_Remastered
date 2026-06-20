// code/frame_jobs.cpp
#include "frame_jobs.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

// FRAME-JOBS-2D: thread-local flag — set true on workers during touchWorkerPrepass.
thread_local bool g_isFrameJobsWorker = false;

namespace {

struct Chunk { int begin, end; };

struct Pool {
    // Set by dispatch before notify; read-only during execution.
    std::vector<Chunk>            chunks;
    std::function<void(int,int)>  fn;
    std::atomic<int>              nextChunk{0};
    int                           totalChunks{0};

    // Epoch: incremented per dispatch; workers distinguish old vs new work.
    std::mutex                    mu;
    std::condition_variable       cvWork;
    std::condition_variable       cvDone;
    int                           epoch{0};
    std::vector<int>              workerEpoch; // per-worker last-seen epoch, under mu
    std::atomic<int>              workersDone{0};
    int                           numWorkers{0};
    bool                          quit{false};

    std::vector<std::thread>      threads;
};

Pool* g_pool      = nullptr;
bool  g_enabled   = false;
bool  g_touchEnabled = false;
int   g_batch     = 64;
bool  g_trace     = false;

std::atomic<int> g_chunksThisFrame{0};
std::atomic<int> g_workersCount{0};
bool             g_serialFallback = false;

void workerFn(Pool* pool, int workerId) {
    while (true) {
        // Wait for a new epoch or quit signal.
        {
            std::unique_lock<std::mutex> lk(pool->mu);
            pool->cvWork.wait(lk, [&] {
                return pool->quit || pool->workerEpoch[workerId] != pool->epoch;
            });
            if (pool->quit) return;
            pool->workerEpoch[workerId] = pool->epoch;
        }

        // Steal and execute chunks.
        while (true) {
            int idx = pool->nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (idx >= pool->totalChunks) break;
            pool->fn(pool->chunks[idx].begin, pool->chunks[idx].end);
            g_chunksThisFrame.fetch_add(1, std::memory_order_relaxed);
        }

        // Signal done. Workers that stole zero chunks still signal.
        int done = pool->workersDone.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == pool->numWorkers) {
            std::lock_guard<std::mutex> lk(pool->mu);
            pool->cvDone.notify_one();
        }
    }
}

} // namespace

void frameJobsInit() {
    const char* envEnabled = std::getenv("MC2_FRAME_JOBS");
    g_enabled = envEnabled && std::atoi(envEnabled) != 0;

    if (const char* e = std::getenv("MC2_FRAME_JOBS_BATCH"))
        g_batch = std::max(1, std::atoi(e));
    g_trace = std::getenv("MC2_FRAME_JOBS_TRACE") &&
              std::atoi(std::getenv("MC2_FRAME_JOBS_TRACE")) != 0;

    const char* touchEnv = getenv("MC2_FRAME_JOBS_TOUCH");
    g_touchEnabled = g_enabled && touchEnv && touchEnv[0] == '1';

    if (!g_enabled) return;

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    int nw = (hw > 1) ? (hw - 1) : 1;
    if (const char* e = std::getenv("MC2_FRAME_JOBS_WORKERS"))
        nw = std::atoi(e);
    const int hwMax = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    nw = std::max(1, std::min(hwMax, nw));

    if (nw <= 1) { g_enabled = false; return; } // serial fallback

    g_pool = new Pool();
    g_pool->numWorkers = nw;
    g_pool->workerEpoch.assign(nw, -1);
    for (int i = 0; i < nw; ++i)
        g_pool->threads.emplace_back(workerFn, g_pool, i);

    g_workersCount.store(nw, std::memory_order_relaxed);

    if (g_touchEnabled && g_trace) {
        printf("FRAME_JOBS_TOUCH: enabled workers=%d batch=%d\n", nw, g_batch);
    }
}

void frameJobsShutdown() {
    if (!g_pool) return;
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->quit = true;
    }
    g_pool->cvWork.notify_all();
    for (auto& t : g_pool->threads) t.join();
    delete g_pool;
    g_pool = nullptr;
    g_enabled = false;
}

void parallelForRange(int count, int batchSize,
                      const std::function<void(int, int)>& fn) {
    if (count <= 0) return;

    if (!g_enabled || !g_pool) {
        fn(0, count);
        g_serialFallback = true;
        return;
    }

    g_pool->chunks.clear();
    for (int i = 0; i < count; i += batchSize)
        g_pool->chunks.push_back({i, std::min(i + batchSize, count)});
    g_pool->fn           = fn;
    g_pool->totalChunks  = static_cast<int>(g_pool->chunks.size());
    g_pool->nextChunk.store(0, std::memory_order_relaxed);

    // Reset done counter and increment epoch atomically under the mutex.
    // This prevents a prior-epoch worker's fetch_add from racing the reset.
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->workersDone.store(0, std::memory_order_relaxed);
        ++g_pool->epoch;
    }
    g_pool->cvWork.notify_all();

    // Main thread steals chunks too.
    while (true) {
        int idx = g_pool->nextChunk.fetch_add(1, std::memory_order_relaxed);
        if (idx >= g_pool->totalChunks) break;
        fn(g_pool->chunks[idx].begin, g_pool->chunks[idx].end);
        g_chunksThisFrame.fetch_add(1, std::memory_order_relaxed);
    }

    // Wait for all workers to exhaust their steal loops.
    {
        std::unique_lock<std::mutex> lk(g_pool->mu);
        g_pool->cvDone.wait(lk, [pool = g_pool] {
            return pool->workersDone.load(std::memory_order_acquire)
                   >= pool->numWorkers;
        });
    }
}

bool frameJobsEnabled()      { return g_enabled;      }
bool frameJobsTouchEnabled() { return g_touchEnabled; }
int  frameJobsBatch()        { return g_batch;        }
bool frameJobsTrace()        { return g_trace;        }

FrameJobsFrameStats frameJobsGetFrameStats() {
    return {
        g_chunksThisFrame.load(std::memory_order_relaxed),
        g_workersCount.load(std::memory_order_relaxed),
        g_serialFallback
    };
}

void frameJobsResetFrameStats() {
    g_chunksThisFrame.store(0, std::memory_order_relaxed);
    g_serialFallback = false;
}
