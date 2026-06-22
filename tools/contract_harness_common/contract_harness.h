// tools/contract_harness_common/contract_harness.h
// SUBSYSTEM-HARNESS-ARC / HARNESS-TEMPLATE-1
//
// Header-only shared infrastructure for MC2 subsystem "contract" harnesses:
// small standalone executables that link production code where practical and
// force exact edge cases WITHOUT launching the game.
//
// Design constraints (see docs/testing/subsystem-harness-arc-1.md):
//   * C++17 standard library only. No external deps. No GL, no gameos.
//   * Header-only so every harness CMake just adds one include dir.
//   * Uniform CLI: --list / --test <name> / --json / --seed <n>.
//   * Exit code 0 == all selected tests PASS, nonzero == failure.
//
// STDOUT/STDERR CONTRACT: the framework owns stdout (the report / JSON). Tests
// MUST write any human diagnostics to stderr (std::fprintf(stderr, ...)); writing
// diagnostics to stdout corrupts --json output.
//
// Usage (a harness .cpp):
//   #include "contract_harness.h"
//   using namespace contract_harness;
//   static bool test_foo(TestCtx& t) { CH_CHECK(t, 1 + 1 == 2); return true; }
//   int main(int argc, char** argv) {
//       Harness h("my_harness");
//       h.add("foo", test_foo);
//       return h.run(argc, argv);
//   }

#ifndef MC2_CONTRACT_HARNESS_H
#define MC2_CONTRACT_HARNESS_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace contract_harness {

// Per-test context. A test fails by either returning false or recording a
// failure via CH_CHECK / t.fail(). Randomized tests should seed off t.seed.
struct TestCtx {
    std::uint64_t seed = 0;
    std::string   firstFailure;     // first failure message, for the report
    int           failures = 0;

    void fail(const std::string& msg) {
        ++failures;
        if (firstFailure.empty()) firstFailure = msg;
    }
};

// CH_CHECK(ctx, cond): record a failure (with file:line) if cond is false.
// Does NOT early-return; the test keeps running so multiple checks report.
#define CH_CHECK(ctx, cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            (ctx).fail(std::string(__FILE__ ":") + std::to_string(__LINE__) + \
                       " CH_CHECK(" #cond ")");                               \
        }                                                                     \
    } while (0)

using TestFn = std::function<bool(TestCtx&)>;

struct TestCase {
    std::string name;
    TestFn      fn;
    bool        inDefault = true;   // run when no --test is given (the green suite)
};

struct TestResult {
    std::string name;
    bool        passed = false;
    long        ms = 0;
    std::string message;            // failure detail, empty on pass
};

class Harness {
public:
    explicit Harness(std::string name) : name_(std::move(name)) {}

    // inDefault=false registers a test that runs ONLY when named via --test
    // (e.g. an intentional-failure demo). It still appears in --list.
    void add(const std::string& testName, TestFn fn, bool inDefault = true) {
        tests_.push_back({testName, std::move(fn), inDefault});
    }

    // Parse argv, run selected tests, emit report, return exit code.
    int run(int argc, char** argv) {
        bool        json = false;
        bool        list = false;
        std::string only;
        std::uint64_t seed = 0;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--list")  list = true;
            else if (a == "--json") json = true;
            else if (a == "--test" && i + 1 < argc) only = argv[++i];
            else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
            else if (a == "-h" || a == "--help") { usage(); return 2; }
            else { std::fprintf(stderr, "ERROR: unknown arg '%s'\n", a.c_str()); usage(); return 2; }
        }

        if (list) { emitList(json); return 0; }

        std::vector<TestResult> results;
        bool anyRan = false;
        for (const auto& tc : tests_) {
            if (only.empty()) {
                if (!tc.inDefault) continue;   // skip demo-only tests in the green suite
            } else if (tc.name != only) {
                continue;
            }
            anyRan = true;
            results.push_back(runOne(tc, seed));
        }

        if (!only.empty() && !anyRan) {
            std::fprintf(stderr, "ERROR: no test named '%s'\n", only.c_str());
            return 2;
        }

        return emitResults(results, json);
    }

private:
    static long nowMs() {
        using namespace std::chrono;
        return (long)duration_cast<milliseconds>(
                   steady_clock::now().time_since_epoch()).count();
    }

    TestResult runOne(const TestCase& tc, std::uint64_t seed) {
        TestCtx ctx;
        ctx.seed = seed;
        long t0 = nowMs();
        bool ret = false;
        try {
            ret = tc.fn(ctx);
        } catch (const std::exception& e) {
            ctx.fail(std::string("exception: ") + e.what());
        } catch (...) {
            ctx.fail("exception: non-std");
        }
        long t1 = nowMs();
        TestResult r;
        r.name = tc.name;
        r.ms = (t1 - t0) < 0 ? 0 : (t1 - t0);
        r.passed = ret && ctx.failures == 0;
        if (!r.passed) {
            r.message = ctx.firstFailure.empty()
                ? (ret ? "test returned false (no detail)" : "test returned false")
                : ctx.firstFailure;
        }
        return r;
    }

    void usage() const {
        std::fprintf(stderr,
            "usage: %s [--list] [--test <name>] [--json] [--seed <n>]\n",
            name_.c_str());
    }

    void emitList(bool json) const {
        if (json) {
            std::printf("{\"harness\":\"%s\",\"tests\":[", name_.c_str());
            for (size_t i = 0; i < tests_.size(); ++i)
                std::printf("%s\"%s\"", i ? "," : "", tests_[i].name.c_str());
            std::printf("]}\n");
        } else {
            std::printf("%s tests:\n", name_.c_str());
            for (const auto& tc : tests_) std::printf("  %s\n", tc.name.c_str());
        }
    }

    int emitResults(const std::vector<TestResult>& results, bool json) const {
        int    fails = 0;
        long   total = 0;
        for (const auto& r : results) { if (!r.passed) ++fails; total += r.ms; }
        const char* status = fails ? "FAIL" : "PASS";

        if (json) {
            std::printf("{\"harness\":\"%s\",\"tests\":[", name_.c_str());
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                std::printf("%s{\"name\":\"%s\",\"status\":\"%s\",\"ms\":%ld",
                            i ? "," : "", r.name.c_str(),
                            r.passed ? "PASS" : "FAIL", r.ms);
                if (!r.passed)
                    std::printf(",\"message\":\"%s\"", jsonEscape(r.message).c_str());
                std::printf("}");
            }
            std::printf("],\"status\":\"%s\",\"elapsed_ms\":%ld}\n", status, total);
        } else {
            for (const auto& r : results) {
                std::printf("  [%s] %s (%ld ms)%s%s\n",
                            r.passed ? "PASS" : "FAIL", r.name.c_str(), r.ms,
                            r.passed ? "" : " - ",
                            r.passed ? "" : r.message.c_str());
            }
            std::printf("%s: %s (%zu test%s, %d failure%s, %ld ms)\n",
                        name_.c_str(), status, results.size(),
                        results.size() == 1 ? "" : "s", fails,
                        fails == 1 ? "" : "s", total);
        }
        return fails ? 1 : 0;
    }

    static std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    std::string            name_;
    std::vector<TestCase>  tests_;
};

}  // namespace contract_harness

#endif  // MC2_CONTRACT_HARNESS_H
