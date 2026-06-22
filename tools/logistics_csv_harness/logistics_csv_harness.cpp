// tools/logistics_csv_harness/logistics_csv_harness.cpp
// SUBSYSTEM-HARNESS-ARC / LOGISTICS-CSV-TOKENIZER-HARNESS-1
//
// Exercises the REAL logistics CSV tokenizer (code/logistics_csv.h, the same
// header LogisticsComponent::extractString/Int/Float now delegate to) game-free.
// Regression-locks LOGISTICS-CSV-TOKENIZER-FIX-1: the past-NUL cursor walk on
// malformed/truncated mod CSV rows that smoke can't force (stock compbas.csv is
// well-formed). No game, no GL, links nothing.
//
// Build (standalone):
//   cmake -S tools/logistics_csv_harness -B build64-logcsv -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-logcsv --config RelWithDebInfo --target logistics_csv_harness

#include "contract_harness.h"
#include "logistics_csv.h"

#include <cstring>
#include <string>
#include <vector>

using namespace contract_harness;
namespace lc = logistics_csv;

// Helper: run a row through N field extracts, return the fields.
static std::vector<std::string> extractFields(const char* row, int n) {
    // Mutable copy with generous slack so a (buggy) past-end walk would read
    // initialized memory we control, not random heap — the test asserts the
    // cursor STAYS in-bounds, this just makes a failure observable not UB.
    std::vector<char> buf(strlen(row) + 64, '\0');
    memcpy(buf.data(), row, strlen(row));
    char* cur = buf.data();
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        char field[1024];
        lc::extractField(cur, field, 1024);
        out.emplace_back(field);
    }
    return out;
}

// Golden parity: a well-formed compbas-style row parses field-for-field exactly.
static bool test_wellformed_row_parity(TestCtx& t) {
    const char* row = "7,Weapon,Medium Laser,1,4.0,250,5";
    auto f = extractFields(row, 7);
    const char* want[] = {"7", "Weapon", "Medium Laser", "1", "4.0", "250", "5"};
    for (int i = 0; i < 7; ++i) CH_CHECK(t, f[(size_t)i] == want[i]);
    return t.failures == 0;
}

// Cursor must land ON the trailing NUL after the last field, and stay there.
static bool test_cursor_parks_on_nul_at_end(TestCtx& t) {
    char buf[] = "a,b,c";
    char* cur = buf;
    char field[8];
    lc::extractField(cur, field, 8);   // a
    lc::extractField(cur, field, 8);   // b
    lc::extractField(cur, field, 8);   // c -> ends on '\0'
    CH_CHECK(t, *cur == '\0');         // parked on the terminator
    CH_CHECK(t, cur == buf + 5);       // exactly at the NUL, not past it
    // Further extracts must stay parked and return empty (no past-NUL walk).
    int extra = lc::extractField(cur, field, 8);
    CH_CHECK(t, extra == 0 && field[0] == '\0');
    CH_CHECK(t, cur == buf + 5);       // did NOT advance past the NUL
    return t.failures == 0;
}

// THE bug: a truncated row read for MORE fields than it has must not walk the
// cursor past the buffer end (init() reads ~24 fields; a short mod row has few).
static bool test_truncated_row_no_past_nul_walk(TestCtx& t) {
    char buf[] = "7,Weapon,Laser";   // only 3 fields
    char* cur = buf;
    const char* end = buf + strlen(buf);   // the NUL
    char field[64];
    for (int i = 0; i < 24; ++i) {         // over-read like init()
        lc::extractField(cur, field, 64);
        CH_CHECK(t, cur <= end);            // never past the terminator
    }
    CH_CHECK(t, cur == end);                // parked exactly on the NUL
    return t.failures == 0;
}

// Empty fields return 0; Int/Float return -1 on empty (legacy contract).
static bool test_empty_fields(TestCtx& t) {
    char buf[] = "a,,b";
    char* cur = buf;
    char field[8];
    CH_CHECK(t, lc::extractField(cur, field, 8) == 1);  // "a"
    CH_CHECK(t, lc::extractField(cur, field, 8) == 0);  // empty
    CH_CHECK(t, std::string(field).empty());
    CH_CHECK(t, lc::extractField(cur, field, 8) == 1);  // "b"
    char b2[] = ",5";
    char* c2 = b2;
    CH_CHECK(t, lc::extractInt(c2) == -1);              // empty -> -1
    CH_CHECK(t, lc::extractInt(c2) == 5);               // "5"
    char b3[] = ",2.5";
    char* c3 = b3;
    CH_CHECK(t, lc::extractFloat(c3) == -1.0f);
    return t.failures == 0;
}

// No trailing delimiter on the last field: value correct, cursor on NUL.
static bool test_no_trailing_delimiter(TestCtx& t) {
    char buf[] = "alpha,42";
    char* cur = buf;
    char field[16];
    lc::extractField(cur, field, 16);              // alpha
    int n = lc::extractField(cur, field, 16);      // 42 (ends on NUL)
    CH_CHECK(t, n == 2 && std::string(field) == "42");
    CH_CHECK(t, *cur == '\0');
    return t.failures == 0;
}

// Over-long field vs a small buffer: copy is clamped to outCap-1, NUL-terminated,
// no overrun (legacy gosASSERT was a release no-op).
static bool test_overlong_field_clamped_no_overrun(TestCtx& t) {
    std::string big(300, 'X');
    big += ",tail";
    std::vector<char> buf(big.size() + 1, '\0');
    memcpy(buf.data(), big.c_str(), big.size());
    char* cur = buf.data();
    char small[16];
    // Canary after the buffer to detect an overrun past small[15].
    char canary = 0x7E;
    int n = lc::extractField(cur, small, 16);
    CH_CHECK(t, n == 15);                       // clamped to outCap-1
    CH_CHECK(t, small[15] == '\0');             // NUL-terminated at the bound
    CH_CHECK(t, strlen(small) == 15);
    CH_CHECK(t, canary == 0x7E);                // untouched
    return t.failures == 0;
}

// Demo failure (inDefault=false): proves the failure path via --test only.
static bool test_demo_intentional_fail(TestCtx& t) {
    char buf[] = "a,b";
    char* cur = buf;
    char field[8];
    lc::extractField(cur, field, 8);
    CH_CHECK(t, std::string(field) == "WRONG");  // intentionally wrong
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("logistics_csv_harness");
    h.add("wellformed_row_parity",              test_wellformed_row_parity);
    h.add("cursor_parks_on_nul_at_end",         test_cursor_parks_on_nul_at_end);
    h.add("truncated_row_no_past_nul_walk",     test_truncated_row_no_past_nul_walk);
    h.add("empty_fields",                       test_empty_fields);
    h.add("no_trailing_delimiter",              test_no_trailing_delimiter);
    h.add("overlong_field_clamped_no_overrun",  test_overlong_field_clamped_no_overrun);
    h.add("demo_intentional_fail",              test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
