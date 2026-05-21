//==========================================================================//
// File:    fx_trace.cpp                                                     //
// Contents: Neutral fx invocation counter implementation.                   //
//           Plan v6 §1.                                                     //
//===========================================================================//

#include "fx_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace mc2 {
namespace fx_trace {

namespace {

// Latch the env value once at first query (per debug_instrumentation_rule.md
// idiom; same as other tier-1 instrumentation env vars).
bool g_enabled_initialized = false;
bool g_enabled_value = false;
std::mutex g_init_mutex;

void initialize_enabled_flag() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_enabled_initialized) return;
    const char* v = std::getenv("MC2_FX_TRACE");
    g_enabled_value = (v && v[0] == '1');
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: fx_trace\n");
        std::fprintf(stderr, "[FX_TRACE v1] counters: spawn/draw/mlr_enqueue\n");
    }
}

// Three independent counter tables keyed by name. We dedupe on string
// content, not pointer (callers may pass spec-name buffers that get freed).
using CounterMap = std::map<std::string, unsigned long long>;
struct CounterTables {
    CounterMap spawn;
    CounterMap draw;
    CounterMap mlr_enqueue;
    std::mutex m;
};

CounterTables& tables() {
    static CounterTables t;
    return t;
}

void dump_table(const char* label, const CounterMap& tbl) {
    if (tbl.empty()) {
        std::fprintf(stderr, "[FX_TRACE v1] %s: (empty)\n", label);
        return;
    }
    unsigned long long total = 0;
    for (const auto& kv : tbl) total += kv.second;
    std::fprintf(stderr, "[FX_TRACE v1] %s total=%llu unique=%zu\n",
                 label, total, tbl.size());
    for (const auto& kv : tbl) {
        std::fprintf(stderr, "[FX_TRACE v1] %s name=%s count=%llu\n",
                     label, kv.first.c_str(), kv.second);
    }
}

struct AtexitDumper {
    ~AtexitDumper() {
        if (g_enabled_value) {
            dump_and_reset("atexit");
        }
    }
};
AtexitDumper g_atexit_dumper;

} // namespace

bool is_enabled() {
    if (!g_enabled_initialized) initialize_enabled_flag();
    return g_enabled_value;
}

void record_spawn(const char* name) {
    if (!name) name = "(null)";
    auto& t = tables();
    std::lock_guard<std::mutex> lock(t.m);
    ++t.spawn[name];
}

void record_draw(const char* name) {
    if (!name) name = "(null)";
    auto& t = tables();
    std::lock_guard<std::mutex> lock(t.m);
    ++t.draw[name];
}

void record_mlr_enqueue(const char* leaf_name) {
    if (!leaf_name) leaf_name = "(null)";
    auto& t = tables();
    std::lock_guard<std::mutex> lock(t.m);
    ++t.mlr_enqueue[leaf_name];
}

void dump_and_reset(const char* reason) {
    auto& t = tables();
    std::lock_guard<std::mutex> lock(t.m);
    std::fprintf(stderr, "[FX_TRACE v1] === dump (%s) ===\n",
                 reason ? reason : "?");
    dump_table("spawn", t.spawn);
    dump_table("draw", t.draw);
    dump_table("mlr_enqueue", t.mlr_enqueue);
    std::fprintf(stderr, "[FX_TRACE v1] === end dump ===\n");
    t.spawn.clear();
    t.draw.clear();
    t.mlr_enqueue.clear();
}

} // namespace fx_trace
} // namespace mc2
