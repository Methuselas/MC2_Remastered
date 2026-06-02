// HostServices/DefaultHostServices.h
// ENGINE-HOST-SERVICES-0 — std-library-only default implementations of the
// host-service contract. Dependency-free (no GameOS, no GL, no Mission) so the
// seam is instantiable in a headless/test context and so every caller has a
// working fallback before it wires its own host. Production hosts override as
// needed (e.g. a game IClock over gos_GetElapsedTime, an IFileSystem over
// FastFile).
//
// Behavior lives in DefaultHostServices.cpp.
#pragma once

#include "HostServices.h"

namespace mc2host {

// Reads from the real filesystem via std::ifstream. exists() via std::ifstream
// open probe. Takes caller-resolved paths.
class DiskFileSystem final : public IFileSystem {
public:
    bool exists(const char* path) const override;
    bool read(const char* path, std::vector<uint8_t>& out) const override;
};

// Writes one line per call to stderr: "[LEVEL] category: msg".
class StderrLogger final : public ILogger {
public:
    void log(Level level, const char* category, const char* msg) override;
};

// Monotonic seconds since construction, via std::chrono::steady_clock.
class ChronoClock final : public IClock {
public:
    ChronoClock();
    double elapsedSeconds() const override;
private:
    std::int64_t startNanos_;  // steady_clock epoch nanos at construction
};

// Reads process environment via std::getenv. flag(): absent -> dflt; "0",
// "false", "off", "no" (any case) -> false; anything else present -> true.
class EnvConfig final : public IConfig {
public:
    bool        flag(const char* key, bool dflt) const override;
    const char* str (const char* key, const char* dflt) const override;
};

// Process-lifetime bundle of the defaults above (Meyers singletons). Returns
// the same pointers on every call — safe to copy the bundle by value.
HostServices defaultHostServices();

} // namespace mc2host
