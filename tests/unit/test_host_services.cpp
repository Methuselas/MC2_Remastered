// tests/unit/test_host_services.cpp
// ENGINE-HOST-SERVICES-0: GL-free unit tests for the host-service contract
// types and their process-default implementations. No GameOS, no GL, no
// Mission — std-library only. Proves the seam is instantiable standalone.
#include "doctest.h"
#include "HostServices.h"
#include "DefaultHostServices.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace mc2host;

TEST_SUITE("HostServices") {

// ---------------------------------------------------------------------------
// Bundle wiring
// ---------------------------------------------------------------------------
TEST_CASE("defaultHostServices wires every slot to a non-null impl") {
    HostServices hs = defaultHostServices();
    REQUIRE(hs.files  != nullptr);
    REQUIRE(hs.log    != nullptr);
    REQUIRE(hs.clock  != nullptr);
    REQUIRE(hs.config != nullptr);
}

TEST_CASE("defaultHostServices returns stable process-lifetime pointers") {
    HostServices a = defaultHostServices();
    HostServices b = defaultHostServices();
    CHECK(a.files  == b.files);
    CHECK(a.log    == b.log);
    CHECK(a.clock  == b.clock);
    CHECK(a.config == b.config);
}

// ---------------------------------------------------------------------------
// IClock / ChronoClock
// ---------------------------------------------------------------------------
TEST_CASE("ChronoClock elapsedSeconds is non-negative and monotonic") {
    ChronoClock clk;
    double t0 = clk.elapsedSeconds();
    double t1 = clk.elapsedSeconds();
    CHECK(t0 >= 0.0);
    CHECK(t1 >= t0);
}

// ---------------------------------------------------------------------------
// IConfig / EnvConfig
// ---------------------------------------------------------------------------
TEST_CASE("EnvConfig returns default for an absent key") {
    EnvConfig cfg;
    const char* kAbsent = "MC2_HOSTSVC_DEFINITELY_UNSET_KEY_XYZ";
    CHECK(cfg.flag(kAbsent, true)  == true);
    CHECK(cfg.flag(kAbsent, false) == false);
    CHECK(std::string(cfg.str(kAbsent, "fallback")) == "fallback");
}

// ---------------------------------------------------------------------------
// IFileSystem / DiskFileSystem
// ---------------------------------------------------------------------------
TEST_CASE("DiskFileSystem reports absent path as non-existent") {
    DiskFileSystem fs;
    const char* kNoSuch = "this_path_does_not_exist_hostsvc_test.nope";
    CHECK(fs.exists(kNoSuch) == false);
    std::vector<uint8_t> out{0xAB};
    CHECK(fs.read(kNoSuch, out) == false);
}

TEST_CASE("DiskFileSystem round-trips a real file's bytes") {
    const char* kPath = "hostsvc_test_roundtrip.bin";
    const unsigned char payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};
    {
        std::ofstream f(kPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(payload), sizeof(payload));
    }
    DiskFileSystem fs;
    CHECK(fs.exists(kPath) == true);
    std::vector<uint8_t> out;
    REQUIRE(fs.read(kPath, out) == true);
    REQUIRE(out.size() == sizeof(payload));
    for (size_t i = 0; i < sizeof(payload); ++i) {
        CHECK(out[i] == payload[i]);
    }
    std::remove(kPath);
}

// ---------------------------------------------------------------------------
// ILogger / StderrLogger
// ---------------------------------------------------------------------------
TEST_CASE("StderrLogger accepts every level without throwing") {
    StderrLogger lg;
    lg.log(ILogger::Level::Trace, "TEST", "trace message");
    lg.log(ILogger::Level::Info,  "TEST", "info message");
    lg.log(ILogger::Level::Warn,  "TEST", "warn message");
    lg.log(ILogger::Level::Error, "TEST", "error message");
    CHECK(true);  // reached here = no throw / no crash
}

// ---------------------------------------------------------------------------
// GLContextInfo POD
// ---------------------------------------------------------------------------
TEST_CASE("GLContextInfo carries context capabilities as plain data") {
    GLContextInfo gl;
    gl.glMajor     = 4;
    gl.glMinor     = 3;
    gl.coreProfile = true;
    gl.hasBPTC     = true;
    CHECK(gl.glMajor == 4);
    CHECK(gl.glMinor == 3);
    CHECK(gl.coreProfile);
    CHECK(gl.hasBPTC);
}

// ---------------------------------------------------------------------------
// Polymorphism through the interface base pointers
// ---------------------------------------------------------------------------
TEST_CASE("services are usable through their interface base pointers") {
    DiskFileSystem disk;
    ChronoClock    clk;
    EnvConfig      env;
    StderrLogger   logger;

    IFileSystem* ifs  = &disk;
    IClock*      iclk = &clk;
    IConfig*     icfg = &env;
    ILogger*     ilog = &logger;

    CHECK(ifs->exists("still_no_such_file.nope") == false);
    CHECK(iclk->elapsedSeconds() >= 0.0);
    CHECK(icfg->flag("MC2_HOSTSVC_DEFINITELY_UNSET_KEY_XYZ", true) == true);
    ilog->log(ILogger::Level::Info, "TEST", "via base pointer");
    CHECK(true);
}

} // TEST_SUITE("HostServices")
