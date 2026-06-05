// Unit tests for ObjectRecentRing (editor/object_recent_ring.h) — the MRU list
// behind the object companion panel's "Recent" strip.
//
// Dependency-free; compile and run standalone:
//   cl /EHsc /std:c++17 /I.. object_recent_ring_test.cpp && object_recent_ring_test.exe
//   (or)  g++ -std=c++17 -I.. object_recent_ring_test.cpp -o t && ./t

#include "../object_recent_ring.h"
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } \
} while (0)

int main() {
    // empty
    {
        ObjectRecentRing r;
        CHECK(r.items().empty());
    }

    // push appends most-recent-first
    {
        ObjectRecentRing r;
        r.push(1, 10);
        r.push(2, 20);
        CHECK(r.items().size() == 2);
        CHECK(r.items()[0].group == 2 && r.items()[0].indexInGroup == 20);  // MRU first
        CHECK(r.items()[1].group == 1 && r.items()[1].indexInGroup == 10);
    }

    // dedupe: re-pushing an existing entry moves it to front, no duplicate
    {
        ObjectRecentRing r;
        r.push(1, 10);
        r.push(2, 20);
        r.push(1, 10);                       // re-select first
        CHECK(r.items().size() == 2);        // not 3
        CHECK(r.items()[0].group == 1 && r.items()[0].indexInGroup == 10);
        CHECK(r.items()[1].group == 2 && r.items()[1].indexInGroup == 20);
    }

    // (group,index) identity: same group different index are distinct entries
    {
        ObjectRecentRing r;
        r.push(3, 1);
        r.push(3, 2);
        CHECK(r.items().size() == 2);
    }

    // cap at kCap, oldest dropped, order preserved
    {
        ObjectRecentRing r;
        for (int i = 0; i < ObjectRecentRing::kCap + 3; ++i)
            r.push(0, i);                    // 0..kCap+2
        CHECK((int)r.items().size() == ObjectRecentRing::kCap);
        CHECK(r.items()[0].indexInGroup == ObjectRecentRing::kCap + 2);   // newest
        CHECK(r.items()[ObjectRecentRing::kCap - 1].indexInGroup == 3);   // oldest kept (0,1,2 dropped)
    }

    // clear
    {
        ObjectRecentRing r;
        r.push(1, 1);
        r.clear();
        CHECK(r.items().empty());
    }

    if (g_failures == 0) { std::printf("object_recent_ring_test: ALL PASS\n"); return 0; }
    std::printf("object_recent_ring_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
