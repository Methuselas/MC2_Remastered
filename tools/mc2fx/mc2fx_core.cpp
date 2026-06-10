// tools/mc2fx/mc2fx_core.cpp
//
// Implementation of the mc2fx reusable load/dump/save core. The heavy engine
// headers live here so mc2fx_core.h stays dependency-light.

#define _CRT_SECURE_NO_WARNINGS
#include "mc2fx_core.h"

#include <cstdio>
#include <cstring>

#include "gosfxheaders.hpp"          // pulls gosfx.hpp + Stuff + MLR
#include "particles/spec_library.h"
#include "mlr/mlr.hpp"
#include "mlr/mlrtexturepool.hpp"
#include "mlr/gosimagepool.hpp"
#include <windows.h>

// Heap installer provided by mc2fx_stubs.cpp.
extern void InstallMc2fxHeaps();

namespace mc2fx {

namespace {

bool g_engineInited = false;

std::string jsonEscape(const char* s)
{
    std::string r;
    if (!s) return r;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { r.push_back('\\'); r.push_back(c); }
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else r.push_back(c);
    }
    return r;
}

// SEH probe helper — MUST be its own function (no C++ unwinding objects in
// scope) per MSVC C2712. Returns 0 on success (bytes via *outBytes), else the
// SEH exception code; *faultAddr set on fault.
unsigned trySaveSpecInner(gosFX::Effect::Specification* spec,
                          Stuff::DynamicMemoryStream* s,
                          size_t* outBytes, void** faultAddr)
{
    __try {
        spec->Save(s);
        *outBytes = (size_t)s->GetBytesUsed();
        return 0;
    } __except (*faultAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

unsigned trySaveSpec(gosFX::Effect::Specification* spec, size_t* outBytes, void** faultAddr)
{
    // Stream allocated/freed OUTSIDE the SEH frame so trySaveSpecInner has no
    // destructible automatics (MSVC C2712). Leak on fault (diagnostic-only).
    // Born-large per-spec: see saveBlob note — base WriteBytes never grows.
    Stuff::DynamicMemoryStream* s = new Stuff::DynamicMemoryStream(static_cast<DWORD>(4u * 1024u * 1024u));
    unsigned code = trySaveSpecInner(spec, s, outBytes, faultAddr);
    if (code == 0) delete s;
    return code;
}

}  // namespace

void initEngineHeadless()
{
    if (g_engineInited) return;
    g_engineInited = true;

    // --- engine bring-up (headless) ---
    InstallMc2fxHeaps();
    // Stuff master class registry MUST init first; the gosFX spec factory
    // dispatch reads its registry.
    Stuff::InitializeClasses();
    // MLR limits mirror the engine's gameos init; not load-critical.
    MidLevelRenderer::InitializeClasses(64u * 1024u, 16u * 1024u, 1024u, 4096u, true);
    // MLRState::Load resolves texture NAMES through MLRTexturePool::Instance.
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    MidLevelRenderer::MLRTexturePool::Instance =
        new MidLevelRenderer::MLRTexturePool(
            new MidLevelRenderer::TGAFilePool("data\\tgl\\128\\"));
    gos_PopCurrentHeap();

    gosFX::InitializeClasses();
}

mc2::particles::SpecLibrary* loadBlob(const unsigned char* bytes, size_t n)
{
    if (!bytes || n == 0) return nullptr;
    gos_PushCurrentHeap(gosFX::Heap);
    // const_cast: MemoryStream takes a non-const void* but only reads on Load.
    Stuff::MemoryStream stream(const_cast<unsigned char*>(bytes),
                               static_cast<DWORD>(n));
    mc2::particles::SpecLibrary* lib = mc2::particles::SpecLibrary::Instance();
    lib->Load(&stream);
    gos_PopCurrentHeap();
    return lib;
}

std::string dumpCatalogJson(mc2::particles::SpecLibrary* lib)
{
    std::string out;
    if (!lib) return out;
    unsigned count = lib->Count();
    out.reserve(count * 64 + 128);
    char line[256];
    std::snprintf(line, sizeof line, "{\n  \"count\": %u,\n  \"effects\": [\n", count);
    out += line;
    for (unsigned i = 0; i < count; ++i) {
        gosFX::Effect::Specification* spec = lib->At(i);
        const char* name = (spec && spec->m_name) ? static_cast<const char*>(spec->m_name) : "";
        unsigned classID = spec ? static_cast<unsigned>(spec->GetClassID()) : 0u;
        unsigned effectID = spec ? spec->m_effectID : 0u;
        std::snprintf(line, sizeof line,
            "    { \"index\": %u, \"effectID\": %u, \"classID\": %u, \"name\": \"%s\" }%s\n",
            i, effectID, classID, jsonEscape(name).c_str(), (i + 1 < count) ? "," : "");
        out += line;
    }
    out += "  ]\n}\n";
    return out;
}

bool saveBlob(mc2::particles::SpecLibrary* lib, std::vector<unsigned char>& out,
              size_t reserveHint)
{
    if (!lib) return false;
    // ASan ground truth (memorystream.cpp:275): the Save path writes through the
    // virtual MemoryStream::WriteBytes(const void*, size_t). DynamicMemoryStream's
    // would-be growing override is declared WriteBytes(const void*, DWORD); on x64
    // DWORD(32) != size_t(64), so it does NOT override the size_t virtual and is
    // never dispatched on this path. The base writer does a raw Mem_Copy +
    // AdvancePointer with NO reallocation -> a buffer born too small overruns the
    // heap (the non-deterministic __fastfail). Fix tool-side by pre-sizing the
    // stream large enough to hold the whole blob so the base writer never overruns.
    gos_PushCurrentHeap(gosFX::Heap);
    if (std::getenv("MC2FX_SAVE_TRACE")) {
        // Per-spec probe: localize which effect's Save faults, with SEH so a
        // fault reports the address instead of killing the process.
        for (unsigned i = 0; i < lib->Count(); ++i) {
            std::fprintf(stderr, "[save] spec %u/%u cls=%u ...", i, lib->Count(),
                         (unsigned)lib->At(i)->GetClassID());
            std::fflush(stderr);
            size_t nb = 0; void* addr = nullptr;
            unsigned code = trySaveSpec(lib->At(i), &nb, &addr);
            if (code == 0) std::fprintf(stderr, " ok (%zu B)\n", nb);
            else std::fprintf(stderr, " FAULT code=0x%08X at %p\n", code, addr);
            std::fflush(stderr);
        }
    }
    // Born-large: base WriteBytes never grows the buffer (see note above).
    DWORD reserve = reserveHint ? static_cast<DWORD>(reserveHint) : (8u * 1024u * 1024u);
    Stuff::DynamicMemoryStream stream(reserve);
    stream.Rewind();  // streamSize stays = reserve (capacity); pos back to start
    lib->Save(&stream);
    size_t used = stream.GetBytesUsed();
    bool ok = false;
    if (used > 0) {
        stream.Rewind();
        const unsigned char* base = static_cast<const unsigned char*>(stream.GetPointer());
        out.assign(base, base + used);
        ok = true;
    }
    gos_PopCurrentHeap();
    return ok;
}

}  // namespace mc2fx
