// HostServices/HostServices.h
// ENGINE-HOST-SERVICES-0 — the caller-agnostic host contract.
//
// These are the ONLY things the render/asset-preview engine layer should
// require from whatever drives it (mc2.exe, mc2_asset_viewer.exe, the mission
// editor, a future Blender bridge, a headless smoke/test harness). The host
// supplies a current GL context plus this small bundle of services; the engine
// asks for filesystem / logging / clock / config through these interfaces
// instead of reaching for Mission, GameObjectManager, or process globals.
//
// CONTRACT-ONLY. No behavior lives here. No GameOS, no GL, no game headers —
// this directory is in the include-firewall scope (scripts/check-include-
// firewall.sh) and must stay free of <GL/glew.h>, Stuff/, mission.h, objmgr.h,
// etc. Default implementations live in DefaultHostServices.{h,cpp}; production
// hosts (game / editor / viewer) provide their own.
//
// Design ref: docs/engine-standalone-seams.md (Slice 1).
#pragma once

#include <cstdint>
#include <vector>

namespace mc2host {

// Filesystem / asset resolver. Paths are caller-resolved absolute (or cwd-
// relative) paths; the engine does NOT depend on the global FastFile registry.
// A host MAY back this with FastFile, raw disk, a memory image, or a Blender
// bridge — the engine cannot tell.
struct IFileSystem {
    virtual bool exists(const char* path) const = 0;
    // Reads the whole file into `out` (cleared first). Returns false if the
    // file is missing or unreadable; `out` is left empty in that case.
    virtual bool read(const char* path, std::vector<uint8_t>& out) const = 0;
    virtual ~IFileSystem() = default;
};

// Logging sink. Replaces scattered fprintf(stderr,...) / SPEW / gosASSERT in
// the engine layer with an injectable callback the host owns.
struct ILogger {
    enum class Level { Trace, Info, Warn, Error };
    virtual void log(Level level, const char* category, const char* msg) = 0;
    virtual ~ILogger() = default;
};

// Monotonic time source for frame ticks / dt. A game host wraps
// gos_GetElapsedTime; a tool host may use a std clock. Engine never reaches a
// global clock.
struct IClock {
    virtual double elapsedSeconds() const = 0;
    virtual ~IClock() = default;
};

// Runtime config / env gates. Replaces direct getenv("MC2_*") reads inside the
// engine with a host-owned, read-once-at-init lookup.
struct IConfig {
    virtual bool        flag(const char* key, bool dflt) const = 0;
    virtual const char* str (const char* key, const char* dflt) const = 0;
    virtual ~IConfig() = default;
};

// Description of the GL context the host already made current. Plain data; no
// GL types so this header stays firewall-clean.
struct GLContextInfo {
    int  glMajor     = 0;
    int  glMinor     = 0;
    bool coreProfile = false;
    bool hasBPTC     = false;  // GLEW_ARB_texture_compression_bptc — needed for BC7
};

// The bundle handed to engine init. Non-owning pointers; the host owns the
// lifetimes and guarantees they outlive the engine.
struct HostServices {
    IFileSystem* files  = nullptr;
    ILogger*     log    = nullptr;
    IClock*      clock  = nullptr;
    IConfig*     config = nullptr;
};

} // namespace mc2host
