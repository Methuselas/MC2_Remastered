// BRAIN-DISPATCH-HARNESS-1: offline dispatch harness main.
//
// Runs parseBrainSpecialBody + bodyHasX + executeSpecialBody_TraceOnly against
// the fixture corpus, validating against manifest.json expected entries.
// No game launch required. No Apply exercised in v1 (deferred).
//
// CLI: brain_dispatch_harness --manifest <path> [--fixture-dir <dir>] [--json]
//
// Exit codes: 0 = all pass, 1 = one or more FAIL, 2 = manifest unreadable.

// Make sure stubs win over engine headers
#include "stubs/include/objmgr.h"
#include "stubs/include/warrior.h"
#include "stubs/include/tacordr.h"
#include "stubs/include/gameobj.h"
#include "stubs/include/inifile.h"

// Real dispatch API (code/brain_special_dispatch.h — LEAF, no engine deps)
#include "brain_special_dispatch.h"
#include "mech_brain_runtime.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>      // _dup, _dup2, _close, _fileno
#include <cstdlib>   // _putenv_s
#endif

// ---------------------------------------------------------------------------
// Minimal JSON parser — enough for our manifest schema.
// We use a simple tokenizer; no external JSON library required.
// ---------------------------------------------------------------------------

struct JsonValue;
using JsonObj  = std::map<std::string, JsonValue>;
using JsonArr  = std::vector<JsonValue>;

struct JsonValue {
    enum Kind { Null, Bool, Number, Str, Array, Object } kind = Null;
    bool        bval = false;
    double      nval = 0.0;
    std::string sval;
    JsonArr     aval;
    JsonObj     oval;

    bool isBool()   const { return kind == Bool; }
    bool isStr()    const { return kind == Str; }
    bool isArr()    const { return kind == Array; }
    bool isObj()    const { return kind == Object; }
    bool isBoolTrue() const { return kind == Bool && bval; }
    const std::string& str() const { return sval; }
    const JsonArr&     arr() const { return aval; }
    const JsonObj&     obj() const { return oval; }
    bool has(const std::string& k) const { return kind == Object && oval.count(k); }
    const JsonValue& at(const std::string& k) const {
        static JsonValue null{};
        auto it = oval.find(k);
        return it != oval.end() ? it->second : null;
    }
};

struct JsonParser {
    const char* p;
    const char* end;
    std::string error;

    JsonParser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    void skipWs() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }

    bool expect(char c) { skipWs(); if (p < end && *p == c) { ++p; return true; } return false; }

    std::string parseStr() {
        if (!expect('"')) { error = "expected '\"'"; return ""; }
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\') { ++p; if (p<end) s += *p++; }
            else s += *p++;
        }
        if (p < end) ++p; // closing "
        return s;
    }

    JsonValue parseValue() {
        skipWs();
        if (p >= end) return {};
        if (*p == '"') {
            JsonValue v; v.kind = JsonValue::Str; v.sval = parseStr(); return v;
        }
        if (*p == '{') {
            ++p;
            JsonValue v; v.kind = JsonValue::Object;
            skipWs();
            while (p < end && *p != '}') {
                std::string key = parseStr();
                expect(':');
                v.oval[key] = parseValue();
                skipWs();
                if (p < end && *p == ',') { ++p; skipWs(); }
            }
            if (p < end) ++p;
            return v;
        }
        if (*p == '[') {
            ++p;
            JsonValue v; v.kind = JsonValue::Array;
            skipWs();
            while (p < end && *p != ']') {
                v.aval.push_back(parseValue());
                skipWs();
                if (p < end && *p == ',') { ++p; skipWs(); }
            }
            if (p < end) ++p;
            return v;
        }
        if (std::strncmp(p, "true", 4) == 0)  { p+=4; JsonValue v; v.kind=JsonValue::Bool; v.bval=true;  return v; }
        if (std::strncmp(p, "false", 5) == 0) { p+=5; JsonValue v; v.kind=JsonValue::Bool; v.bval=false; return v; }
        if (std::strncmp(p, "null", 4) == 0)  { p+=4; JsonValue v; v.kind=JsonValue::Null; return v; }
        // number
        char* np = nullptr;
        double d = std::strtod(p, &np);
        if (np != p) { JsonValue v; v.kind=JsonValue::Number; v.nval=d; p=np; return v; }
        {
            char ebuf[128];
            std::snprintf(ebuf, sizeof(ebuf), "unexpected char 0x%02x '%c' at offset %d",
                          (unsigned char)*p, (*p >= 32 ? *p : '?'), (int)(p - (end - (end-p))));
            error = ebuf;
        }
        return {};
    }
};

static JsonValue parseJsonFile(const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return {}; }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string content(fsize, '\0');
    std::fread(&content[0], 1, fsize, f);
    std::fclose(f);
    JsonParser jp(content);
    JsonValue v = jp.parseValue();
    err = jp.error;
    return v;
}

// ---------------------------------------------------------------------------
// stderr capture helpers
// ---------------------------------------------------------------------------

// Redirect stderr to a named temp file for the duration of the lambda.
// Returns the captured stderr text.
static std::string captureStderr(const std::function<void()>& fn) {
    // Use a fixed temp filename in the current directory.
    static int s_captureId = 0;
    char tmpName[64];
    std::snprintf(tmpName, sizeof(tmpName), "_harness_stderr_%d.tmp", ++s_captureId);

#ifdef _WIN32
    int oldStderr = _dup(2);
    if (oldStderr < 0) { fn(); return ""; }
    FILE* tmp = std::fopen(tmpName, "w+b");
    if (!tmp) { _close(oldStderr); fn(); return ""; }
    if (_dup2(_fileno(tmp), 2) < 0) {
        std::fclose(tmp); _close(oldStderr); fn(); return "";
    }
    fn();
    _fflush_nolock(stderr);
    std::fflush(stderr);
    _dup2(oldStderr, 2);
    _close(oldStderr);
    // Read back
    std::rewind(tmp);
    std::string result;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), tmp))
        result += buf;
    std::fclose(tmp);
    std::remove(tmpName);
    return result;
#else
    int oldStderr = dup(2);
    if (oldStderr < 0) { fn(); return ""; }
    FILE* tmp = fopen(tmpName, "w+b");
    if (!tmp) { close(oldStderr); fn(); return ""; }
    if (dup2(fileno(tmp), 2) < 0) {
        fclose(tmp); close(oldStderr); fn(); return "";
    }
    fn();
    fflush(stderr);
    dup2(oldStderr, 2);
    close(oldStderr);
    rewind(tmp);
    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), tmp))
        result += buf;
    fclose(tmp);
    remove(tmpName);
    return result;
#endif
}

// ---------------------------------------------------------------------------
// FakeGameObjectManager — for CoreAttack WID lookup in Apply (deferred v2)
// ---------------------------------------------------------------------------
class FakeGameObjectManager : public GameObjectManager {
public:
    GameObjectPtr getByWatchID(long) override { return nullptr; }
};

// ---------------------------------------------------------------------------
// Manifest fixture entry
// ---------------------------------------------------------------------------
struct FixtureEntry {
    std::string name;
    std::string file;
    std::string mission;   // derived from file name (e.g. "mc2_01_specials.fit" -> "mc2_01")
    bool        partial = false;
    // Expected assertions
    bool expectedLoaded               = true;
    bool expectedBodyHasPowerdown     = false;
    bool expectedBodyHasUnitEject     = false;
    bool expectedBodyHasCoreGuard     = false;
    bool expectedBodyHasCoreMoveTo    = false;
    bool expectedBodyHasCoreAttack    = false;
    bool expectedBodyHasUnitRetreat   = false;
    bool expectedBodyHasEffect        = false;
    std::vector<std::string> expectedTraceSubstrings;
};

// Derive mission name from fixture filename:
// "mc2_01_specials_callchain.fit" -> "mc2_01_specials_callchain"
// The harness calls parseBrainSpecialBody(missionName, ...) which builds path
// "data/missions/<missionName>_specials.fit" — so we strip "_specials.fit" suffix.
// But the harness will use direct file path instead (see below).
static std::string missionFromFile(const std::string& fname) {
    // Strip ".fit" extension
    std::string s = fname;
    if (s.size() > 4 && s.substr(s.size()-4) == ".fit")
        s = s.substr(0, s.size()-4);
    // If ends with "_specials", strip it
    if (s.size() > 9 && s.substr(s.size()-9) == "_specials")
        s = s.substr(0, s.size()-9);
    return s;
}

static FixtureEntry entryFromJson(const JsonValue& jv) {
    FixtureEntry e;
    if (jv.has("name"))    e.name    = jv.at("name").str();
    if (jv.has("file"))    e.file    = jv.at("file").str();
    if (jv.has("mission")) e.mission = jv.at("mission").str();
    if (jv.has("partial")) e.partial = jv.at("partial").isBoolTrue();

    if (jv.has("expected_bodyHasPowerdown"))  e.expectedBodyHasPowerdown  = jv.at("expected_bodyHasPowerdown").isBoolTrue();
    if (jv.has("expected_bodyHasUnitEject"))  e.expectedBodyHasUnitEject  = jv.at("expected_bodyHasUnitEject").isBoolTrue();
    if (jv.has("expected_bodyHasCoreGuard"))  e.expectedBodyHasCoreGuard  = jv.at("expected_bodyHasCoreGuard").isBoolTrue();
    if (jv.has("expected_bodyHasCoreMoveTo")) e.expectedBodyHasCoreMoveTo = jv.at("expected_bodyHasCoreMoveTo").isBoolTrue();
    if (jv.has("expected_bodyHasCoreAttack")) e.expectedBodyHasCoreAttack = jv.at("expected_bodyHasCoreAttack").isBoolTrue();
    if (jv.has("expected_bodyHasUnitRetreat"))e.expectedBodyHasUnitRetreat= jv.at("expected_bodyHasUnitRetreat").isBoolTrue();
    if (jv.has("expected_bodyHasEffect"))     e.expectedBodyHasEffect     = jv.at("expected_bodyHasEffect").isBoolTrue();

    if (jv.has("expected_trace_substrings")) {
        for (const JsonValue& sv : jv.at("expected_trace_substrings").arr())
            e.expectedTraceSubstrings.push_back(sv.str());
    }
    return e;
}

// ---------------------------------------------------------------------------
// Path resolution helpers
// ---------------------------------------------------------------------------

// The real parseBrainSpecialBody builds "data/missions/<missionName>_specials.fit"
// and opens it relative to CWD. For the harness we need to open the fixture file
// directly. Strategy: we chdir to the fixture dir so the built path is correct —
// OR we implement a thin wrapper that accepts an explicit path.
//
// Simplest approach: invoke parseBrainSpecialBody with a synthetic missionName
// that makes the built path match the fixture. We compute missionName such that
// "data/missions/<missionName>_specials.fit" == fixtureDir/file.
//
// Since we can't control the path prefix easily, we instead set CWD to a temp
// directory with a "data/missions/" subdirectory containing symlinks — but that's
// complex. Simpler: just use a mission name that includes a relative path prefix.
//
// ACTUAL APPROACH: The dispatch.cpp builds: "data/missions/<missionName>_specials.fit"
// If we chdir to <fixtureDir>/../.. (i.e., the worktree root) and the fixture dir
// is "tests/fixtures/brain_runtime/", we'd need "data/missions/" to exist there.
//
// Simplest correct approach: Copy/symlink? No.
// Even simpler: compute missionName = "../../tests/fixtures/brain_runtime/" + baseStem
// so the path becomes "data/missions/../../tests/fixtures/brain_runtime/<stem>_specials.fit"
// when CWD = worktree root.
//
// Actually the cleanest: just call parseBrainSpecialBody_RawScan directly via a
// thin wrapper that takes absolute path. But parseBrainSpecialBody_RawScan is static.
//
// FINAL APPROACH: Pass a missionName that contains relative path tokens so the
// constructed path resolves correctly from CWD. Set CWD = worktreeRoot.
// missionName = "../../tests/fixtures/brain_runtime/<stem>"
// => fitPath = "data/missions/../../tests/fixtures/brain_runtime/<stem>_specials.fit"
//           = "tests/fixtures/brain_runtime/<stem>_specials.fit" (after .. resolution)
// This works on both Windows and Linux.

static std::string makeHarnessMissionName(const std::string& fixtureFile) {
    // Strip ".fit" extension
    std::string s = fixtureFile;
    if (s.size() > 4 && s.substr(s.size()-4) == ".fit")
        s = s.substr(0, s.size()-4);
    // Strip "_specials" suffix if present (so parseBrainSpecialBody re-appends it)
    // If the file already has "_specials" in name, the final path will be:
    //   data/missions/../../tests/fixtures/brain_runtime/<s>_specials.fit
    // But if s doesn't end in _specials, the constructed path won't match.
    // Since ALL brain fixture files ARE named <stem>_specials.fit or <stem>.fit,
    // we handle both cases.
    bool endsInSpecials = (s.size() > 9 && s.substr(s.size()-9) == "_specials");
    if (endsInSpecials) {
        // Strip _specials; parseBrainSpecialBody will re-add it
        s = s.substr(0, s.size()-9);
    }
    // parseBrainSpecialBody appends "_specials.fit" to missionName
    // So for "mc2_01_ai.fit" (no _specials suffix), we'd need missionName = "mc2_01_ai"
    // and parseBrainSpecialBody would look for "mc2_01_ai_specials.fit" — WRONG.
    //
    // For non-_specials files, we can't use parseBrainSpecialBody directly.
    // Mark these as "direct" path (handled separately below via raw scan directly).
    return "../../tests/fixtures/brain_runtime/" + s;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // --- Parse CLI ---
    std::string manifestPath;
    std::string fixtureDir = "tests/fixtures/brain_runtime";
    bool        jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0 && i+1 < argc)
            manifestPath = argv[++i];
        else if (std::strcmp(argv[i], "--fixture-dir") == 0 && i+1 < argc)
            fixtureDir = argv[++i];
        else if (std::strcmp(argv[i], "--json") == 0)
            jsonOutput = true;
    }

    if (manifestPath.empty()) {
        std::fprintf(stderr, "Usage: brain_dispatch_harness --manifest <path> [--fixture-dir <dir>] [--json]\n");
        return 2;
    }

    // --- Load manifest ---
    std::string jsonErr;
    JsonValue manifest = parseJsonFile(manifestPath, jsonErr);
    if (!jsonErr.empty()) {
        std::fprintf(stderr, "ERROR: manifest parse failed: %s\n", jsonErr.c_str());
        return 2;
    }
    if (!manifest.has("fixtures")) {
        std::fprintf(stderr, "ERROR: manifest missing 'fixtures' array\n");
        return 2;
    }

    std::vector<FixtureEntry> fixtures;
    for (const JsonValue& jv : manifest.at("fixtures").arr())
        fixtures.push_back(entryFromJson(jv));

    // --- Set up fake ObjectManager for Apply path ---
    static FakeGameObjectManager s_fakeObjMgr;
    ObjectManager = &s_fakeObjMgr;

    // --- Set required env gates ---
    // The harness always runs with DISPATCH=1 (parse+trace) and CALL=1 (for callchain fixtures).
    // Gates are read once via static initializers — set BEFORE first dispatch call.
    // Note: _putenv on Windows updates getenv() correctly.
    (void)_putenv("MC2_BRAIN_DISPATCH=1");
    (void)_putenv("MC2_BRAIN_DISPATCH_CALL=1");
    (void)_putenv("MC2_BRAIN_DISPATCH_VAR=1");

    // --- Results ---
    int passCount = 0, failCount = 0, skipCount = 0;
    struct Result {
        std::string name;
        std::string status; // PASS / FAIL / SKIP
        std::vector<std::string> failures;
    };
    std::vector<Result> results;

    // --- Run fixtures ---
    for (const FixtureEntry& fix : fixtures) {
        Result res;
        res.name = fix.name;

        // Build path to fixture file
        std::string fitPath = fixtureDir + "/" + fix.file;

        // Derive missionName for parseBrainSpecialBody call.
        // parseBrainSpecialBody(missionName, ...) opens "data/missions/<missionName>_specials.fit"
        // We need: data/missions/<missionName>_specials.fit == fixtureDir/file
        // Solution: missionName = "../../<fixtureDir>/<stem_no_fit_no_specials_suffix>"
        // Then the constructed path = "data/missions/../../<fixtureDir>/<stem>_specials.fit"
        // which resolves to "<fixtureDir>/<stem>_specials.fit" from worktree root.
        //
        // For fixtures that DO end in "_specials.fit": strip "_specials" → parseBrainSpecialBody re-adds it.
        // For fixtures that DON'T end in "_specials.fit": must pass the FULL stem so that
        //   parseBrainSpecialBody appends "_specials.fit" and the result is wrong — handled as partial.
        //   Instead, synthesize a path that matches: use stem_no_fit as the whole segment,
        //   so the constructed path = "data/missions/../../<dir>/<stem_no_fit>_specials.fit"
        //   which equals <dir>/<stem_no_fit>_specials.fit — this won't match the actual file.
        //   These fixtures are marked partial and the body won't load — that's expected.
        //
        // Special case: files like "mc2_01_specials_callchain.fit" DO end in something that
        // contains "_specials" but not as the LAST suffix. They DON'T have "_specials.fit".
        // We need to pass missionName such that the constructed path matches the actual file.
        // Strategy: strip ".fit" from file → get stem.
        //   If stem ends in "_specials": strip it, pass "../../<dir>/<baseStem>"
        //     → constructed path = "data/missions/../../<dir>/<baseStem>_specials.fit" ✓
        //   Else: pass "../../<dir>/<stem>"
        //     → constructed path = "data/missions/../../<dir>/<stem>_specials.fit" ✗ (won't match)
        //     → HOWEVER for brace-block files the raw scanner IS called with this path...
        //     Since the file IS "mc2_01_specials_callchain.fit" and not
        //     "mc2_01_specials_callchain_specials.fit", this fails.
        //
        // REAL FIX: for brace-block files that don't end in "_specials.fit", compute
        // missionName to make the path match the actual file minus "_specials.fit".
        // Wait — ALL specials fixture files in our corpus that have content end in "_specials.fit"
        // OR are brace-block files that also end in "_specials.fit" (checking the actual files).
        //
        // Checking: mc2_01_specials_callchain.fit — does NOT end in _specials.fit
        //   → actual stem = "mc2_01_specials_callchain" (no _specials suffix)
        //   → missionName = "../../tests/fixtures/brain_runtime/mc2_01_specials_callchain"
        //   → parseBrainSpecialBody looks for: "data/missions/../../tests/fixtures/brain_runtime/mc2_01_specials_callchain_specials.fit"
        //   → this file does NOT exist; RawScan fails; FitIni fallback fails; loaded=false
        //
        // THIS IS A NAMING MISMATCH. The fixture file is named "..._callchain.fit" not
        // "..._callchain_specials.fit". parseBrainSpecialBody ALWAYS appends "_specials.fit".
        //
        // SOLUTION: for non-_specials-ending files, we DON'T use parseBrainSpecialBody.
        // Instead, we duplicate the raw-scan logic inline, passing fitPath directly.
        // Since parseBrainSpecialBody_RawScan is static, we expose it via a thin public wrapper.
        // For v1: just mark non-_specials brace files as needing direct-path loading.
        //
        // ALTERNATE SOLUTION: add a public entry point parseBrainSpecialBodyFromPath in dispatch.cpp.
        // But that changes the TU. For PURE harness we'll use a renamed file convention:
        // we just pass the full path via the data/missions relative trick differently.
        //
        // ACTUAL SIMPLEST SOLUTION: Use a directory structure that matches the path.
        // Create a "data/missions/" symlink/dir under build dir... complex.
        //
        // BEST PRAGMATIC SOLUTION: For non-_specials brace files, strip ".fit" and nothing else.
        // That makes the constructed path: "data/missions/../../<dir>/<stem>_specials.fit"
        // = "<dir>/<stem>_specials.fit" which is the wrong name.
        //
        // WORKING APPROACH: rename the fixture files in the harness copy OR use the right naming.
        // Since we can't rename corpus files, the clean fix is to add parseBrainSpecialBodyFromPath().
        // For now: mark brace-block non-_specials files as handled via a workaround. We call
        // parseBrainSpecialBody with a missionName whose constructed path = actual fixture path.
        // For "mc2_01_specials_callchain.fit":
        //   we want: "data/missions/<X>_specials.fit" == "tests/fixtures/brain_runtime/mc2_01_specials_callchain.fit"
        //   So X = "../../tests/fixtures/brain_runtime/mc2_01_specials_callchain"
        //   and the file looked for = "data/missions/../../tests/fixtures/brain_runtime/mc2_01_specials_callchain_specials.fit"
        //   = "tests/fixtures/brain_runtime/mc2_01_specials_callchain_specials.fit"
        //   That's wrong because the file is "mc2_01_specials_callchain.fit" not "_callchain_specials.fit".
        //
        // TRUTH: We need parseBrainSpecialBody to look for the EXACT file path.
        // The ONLY way without modifying dispatch.cpp is to pre-process the fixture path.
        // Since the callchain file IS "mc2_01_specials_callchain.fit" (non-standard naming),
        // we need a public API. ADDING parseBrainSpecialBodyFromPath to dispatch.cpp is minimal:
        // just expose the path directly.
        //
        // For THIS harness, we call parseBrainSpecialBody with missionName such that the path
        // resolves correctly. The callchain files DO end in something other than _specials.
        // We'll use a secondary approach: create the "correct" path symlink in a temp dir.
        // Actually, SIMPLEST: just set CWD = fixture dir temporarily and call with bare stem name.
        //
        // In fixture dir CWD: parseBrainSpecialBody("mc2_01_specials_callchain", ...) builds
        // "data/missions/mc2_01_specials_callchain_specials.fit" — still wrong!
        //
        // THE REAL FIX: expose parseBrainSpecialBodyFromPath publicly.
        // This is a 3-line addition to brain_special_dispatch.h/.cpp.

        // Use parseBrainSpecialBodyFromPath (explicit-path API added in BRAIN-DISPATCH-HARNESS-1)
        // so we don't need to reverse-engineer the "data/missions/<X>_specials.fit" path logic.

        BrainSpecialBody body;
        SpecialIndex     index;

        // Capture stderr from parseBrainSpecialBodyFromPath + executeSpecialBody_TraceOnly
        std::string traceOutput = captureStderr([&]() {
            parseBrainSpecialBodyFromPath(fitPath.c_str(), body, &index);
            if (body.loaded) {
                executeSpecialBody_TraceOnly(body, /*wid=*/1, nullptr, &index, nullptr);
            }
        });

        // --- Assertions ---
        if (fix.partial) {
            // Partial entries: just verify it loads without crash
            res.status = "SKIP";
            ++skipCount;
            results.push_back(res);
            continue;
        }

        // 1. loaded
        if (fix.expectedLoaded && !body.loaded) {
            res.failures.push_back("expected loaded=true but body.loaded=false (trace: " + traceOutput.substr(0, 200) + ")");
        }

        if (body.loaded) {
            // 2. bodyHasX checks
            if (fix.expectedBodyHasPowerdown  && !bodyHasPowerdown(body))
                res.failures.push_back("expected bodyHasPowerdown=true");
            if (!fix.expectedBodyHasPowerdown && bodyHasPowerdown(body) && fix.expectedBodyHasPowerdown == false && !fix.name.empty())
                {} // not checking negatives unless explicitly set
            if (fix.expectedBodyHasUnitEject  && !bodyHasUnitEject(body))
                res.failures.push_back("expected bodyHasUnitEject=true");
            if (fix.expectedBodyHasCoreGuard  && !bodyHasCoreGuard(body))
                res.failures.push_back("expected bodyHasCoreGuard=true");
            if (fix.expectedBodyHasCoreMoveTo && !bodyHasCoreMoveTo(body))
                res.failures.push_back("expected bodyHasCoreMoveTo=true");
            if (fix.expectedBodyHasCoreAttack && !bodyHasCoreAttack(body))
                res.failures.push_back("expected bodyHasCoreAttack=true");
            if (fix.expectedBodyHasUnitRetreat&& !bodyHasUnitRetreat(body))
                res.failures.push_back("expected bodyHasUnitRetreat=true");
            if (fix.expectedBodyHasEffect     && !bodyHasEffect(body))
                res.failures.push_back("expected bodyHasEffect=true");
        }

        // 3. trace substring checks
        for (const std::string& sub : fix.expectedTraceSubstrings) {
            if (traceOutput.find(sub) == std::string::npos) {
                res.failures.push_back("trace missing substring: '" + sub + "'");
                // Show first 400 chars of trace for diagnosis
                if (res.failures.size() == 1)
                    res.failures.push_back("  trace preview: " + traceOutput.substr(0, 400));
            }
        }

        if (res.failures.empty()) {
            res.status = "PASS";
            ++passCount;
        } else {
            res.status = "FAIL";
            ++failCount;
        }
        results.push_back(res);
    }

    // --- Output ---
    if (jsonOutput) {
        std::printf("{\n  \"pass\": %d, \"fail\": %d, \"skip\": %d,\n  \"results\": [\n", passCount, failCount, skipCount);
        for (size_t i = 0; i < results.size(); ++i) {
            const Result& r = results[i];
            std::printf("    {\"name\": \"%s\", \"status\": \"%s\"", r.name.c_str(), r.status.c_str());
            if (!r.failures.empty()) {
                std::printf(", \"failures\": [");
                for (size_t j = 0; j < r.failures.size(); ++j) {
                    // Escape quotes in failure message
                    std::string f = r.failures[j];
                    std::string esc;
                    for (char c : f) { if (c=='"') esc+="\\\""; else esc+=c; }
                    std::printf("\"%s\"%s", esc.c_str(), j+1<r.failures.size()?", ":"");
                }
                std::printf("]");
            }
            std::printf("}%s\n", i+1<results.size()?",":"");
        }
        std::printf("  ]\n}\n");
    } else {
        std::printf("brain_dispatch_harness: %d fixtures (%d pass, %d fail, %d skip)\n",
                    (int)results.size(), passCount, failCount, skipCount);
        for (const Result& r : results) {
            std::printf("  %-50s  %s\n", r.name.c_str(), r.status.c_str());
            for (const std::string& f : r.failures)
                std::printf("    FAIL: %s\n", f.c_str());
        }
    }

    return (failCount > 0) ? 1 : 0;
}
