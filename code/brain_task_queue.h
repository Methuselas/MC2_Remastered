#pragma once

// BrainTaskQueue is mission-runtime ephemeral state. MC2 campaign/checkpoint saves do not
// resume arbitrary mid-mission brain queues, so the queue is intentionally not serialized.
// If true mid-mission save/load support is confirmed later, persistence must be designed
// as a separate compatibility-aware slice.

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdio>

struct BrainTaskEntry {
    uint8_t  priority_tier;       // 0=Logistics..6=Background
    uint32_t insertion_frame_ms;  // (uint32_t)(scenarioTime*1000+0.5) at push time — NOT render frame
    uint32_t stable_seq_id;       // per-queue monotone counter incremented on push
    int32_t  warrior_id;          // vehicleWID — stable numeric, NOT pointer-derived
    // payload placeholder (opaque, no real task types this slice):
    uint8_t  _reserved[8];        // zeroed; real payload added in BRAIN-RUNTIME-1
};

class BrainTaskQueue {
public:
    static constexpr int MAX_BRAIN_TASKS = 8;

    // push: returns false if full (soft-fail, no crash)
    bool push(uint8_t tier, uint32_t frame_ms, int32_t warrior_id, const uint8_t* payload8 = nullptr) {
        if (count_ >= MAX_BRAIN_TASKS)
            return false;
        BrainTaskEntry& e = entries_[count_++];
        e.priority_tier      = tier;
        e.insertion_frame_ms = frame_ms;
        e.stable_seq_id      = seq_++;
        e.warrior_id         = warrior_id;
        std::memset(e._reserved, 0, sizeof(e._reserved));
        if (payload8)
            std::memcpy(e._reserved, payload8, sizeof(e._reserved));
        return true;
    }

    // drain: pops one task in deterministic order, returns true if task was popped.
    // In this slice: discards the task (no tacOrder write — stub).
    bool drain() {
        if (count_ == 0)
            return false;
        // Sort ascending = highest-priority first at index 0.
        std::sort(entries_, entries_ + count_, [](const BrainTaskEntry& a, const BrainTaskEntry& b) {
            if (a.priority_tier      != b.priority_tier)      return a.priority_tier      < b.priority_tier;
            if (a.insertion_frame_ms != b.insertion_frame_ms) return a.insertion_frame_ms < b.insertion_frame_ms;
            if (a.stable_seq_id      != b.stable_seq_id)      return a.stable_seq_id      < b.stable_seq_id;
            return a.warrior_id < b.warrior_id;
        });
        // Pop index 0 (highest priority): shift remaining entries down.
        for (int i = 1; i < count_; ++i)
            entries_[i - 1] = entries_[i];
        --count_;
        return true;
    }

    bool isEmpty() const    { return count_ == 0; }
    int  numPending() const { return count_; }

    void reset() {
        count_ = 0;
        seq_   = 0;
    }

    // debug dump (MC2_BRAIN_TASKQ_TRACE gate, cheap)
    void dumpTrace(int32_t warrior_id_for_log) const {
        std::fprintf(stderr, "BRAIN_TASKQ: wid=%d pending=%d\n", warrior_id_for_log, count_);
        for (int i = 0; i < count_; ++i) {
            const BrainTaskEntry& e = entries_[i];
            std::fprintf(stderr, "  [%d] tier=%u frame_ms=%u seq=%u wid=%d\n",
                i, (unsigned)e.priority_tier, e.insertion_frame_ms, e.stable_seq_id, e.warrior_id);
        }
    }

private:
    BrainTaskEntry entries_[MAX_BRAIN_TASKS];
    int            count_ = 0;
    uint32_t       seq_   = 0;  // per-queue monotone insert counter
};
