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
// idiom). Three independent counter tables. We deliberately allocate the
// tables on the heap and NEVER destroy them — this avoids the well-known
// static-dtor ordering trap where one TU's static dtor calls into another
// TU whose statics have already been destroyed (and would crash the
// process during atexit, suppressing the [SMOKE v1] summary). Leaking the
// tables at process exit is the correct trade.
using CounterMap = std::map<std::string, unsigned long long>;
struct CounterTables {
    CounterMap spawn;
    CounterMap draw;
    CounterMap mlr_enqueue;
    std::mutex m;
};

bool g_enabled_initialized = false;
bool g_enabled_value = false;
bool g_atexit_registered = false;
CounterTables* g_tables = nullptr;
std::mutex g_init_mutex;

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

void atexit_dump() {
    if (g_enabled_value && g_tables) {
        dump_and_reset("atexit");
    }
}

void initialize_locked() {
    if (g_enabled_initialized) return;
    const char* v = std::getenv("MC2_FX_TRACE");
    g_enabled_value = (v && v[0] == '1');
    g_tables = new CounterTables();  // intentionally leaked
    if (!g_atexit_registered) {
        std::atexit(&atexit_dump);
        g_atexit_registered = true;
    }
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: fx_trace\n");
        std::fprintf(stderr, "[FX_TRACE v1] counters: spawn/draw/mlr_enqueue\n");
    }
}

void ensure_initialized() {
    if (g_enabled_initialized) return;
    std::lock_guard<std::mutex> lock(g_init_mutex);
    initialize_locked();
}

} // namespace

bool is_enabled() {
    if (!g_enabled_initialized) ensure_initialized();
    return g_enabled_value;
}

void record_spawn(const char* name) {
    if (!g_tables) ensure_initialized();
    if (!name) name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->spawn[name];
}

void record_draw(const char* name) {
    if (!g_tables) ensure_initialized();
    if (!name) name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->draw[name];
}

void record_mlr_enqueue(const char* leaf_name) {
    if (!g_tables) ensure_initialized();
    if (!leaf_name) leaf_name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->mlr_enqueue[leaf_name];
}

void dump_and_reset(const char* reason) {
    if (!g_tables) return;
    std::lock_guard<std::mutex> lock(g_tables->m);
    std::fprintf(stderr, "[FX_TRACE v1] === dump (%s) ===\n",
                 reason ? reason : "?");
    dump_table("spawn", g_tables->spawn);
    dump_table("draw", g_tables->draw);
    dump_table("mlr_enqueue", g_tables->mlr_enqueue);
    std::fprintf(stderr, "[FX_TRACE v1] === end dump ===\n");
    g_tables->spawn.clear();
    g_tables->draw.clear();
    g_tables->mlr_enqueue.clear();
}

} // namespace fx_trace
} // namespace mc2
