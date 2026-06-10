// tools/mc2fx/main.cpp
//
// mc2fx — standalone gosFX effect-blob (.fx) inspector / round-trip tool.
//
// SLICE 1 (this file): `dump` only.
//   mc2fx dump <mc2.fx> [out.json]
//   - Reads the .fx blob bytes (loose file, CRT fopen — mc2.fx loads loose in
//     the engine: code/mechcmd2.cpp:1672-1686, no FastFile/pak involved).
//   - Wraps bytes in a Stuff::MemoryStream and runs the live gosFX loader
//     (SpecLibrary::Load -> Effect::Specification::Create factory dispatch).
//   - Emits shallow JSON: { version, count, effects:[{index,effectID,name,classID}] }.
//
// This proves: the curated link is game-free, the loader runs headless (no GL,
// no device — MLRState::Load is metadata-only), and the spec catalog round-trips
// to a readable form. SLICE 2 will add per-curve depth + a `build` (Save) path
// that re-emits the blob via the already-complete EffectLibrary::Save API.
//
// Init order (load path requirements, per gosfx-modder-dropin recon):
//   1. install a heap-aware allocator (slim gos_* stubs, CRT-backed)
//   2. MidLevelRenderer::InitializeClasses(...)  -> constructs MLRTexturePool::Instance
//   3. gosFX::InitializeClasses()                -> registers spec factories + gosFX::Heap
//   4. push gosFX::Heap, Load, walk, pop.

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gosfxheaders.hpp"          // pulls gosfx.hpp + Stuff + MLR
#include "particles/spec_library.h"
#include "mlr/mlr.hpp"
#include "mlr/mlrtexturepool.hpp"    // MLRTexturePool::Instance (texture-name resolve)
#include "mlr/gosimagepool.hpp"      // TGAFilePool (GOSImagePool subclass)

// Forward decl for the heap installer provided by mc2fx_stubs.cpp.
extern void InstallMc2fxHeaps();

namespace {

bool readFileBytes(const char* path, std::vector<unsigned char>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Minimal JSON string escaper (names are ASCII identifiers in practice).
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

int cmdDump(const char* inPath, const char* outPath)
{
    std::vector<unsigned char> bytes;
    if (!readFileBytes(inPath, bytes)) {
        std::fprintf(stderr, "mc2fx: cannot read '%s'\n", inPath);
        return 2;
    }

    // --- engine bring-up (headless) ---
    InstallMc2fxHeaps();
    // Stuff master class registry MUST init first (engine: logmain.cpp:465,
    // mechcmd2.cpp:1647). Without it RegisteredClass::FindClassData — which the
    // gosFX spec factory dispatch uses — reads an uninitialized registry and
    // segfaults inside SpecLibrary::Load.
    Stuff::InitializeClasses();
    // MLR limits mirror the engine's gameos init; values are not load-critical
    // (Load only touches MLRState metadata), just need a constructed pool.
    MidLevelRenderer::InitializeClasses(64u * 1024u, 16u * 1024u, 1024u, 4096u, true);
    // MLRState::Load resolves texture NAMES through MLRTexturePool::Instance,
    // which the engine creates in txmmgr.cpp:553 (renderer init we don't run).
    // Build it headless: a TGAFilePool (GOSImagePool subclass) + the pool, under
    // MLR::Heap. No GL — GOSImage ctors hit the no-op mcTextureManager stub. The
    // path string only seeds the (unused) texture base dir.
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    MidLevelRenderer::MLRTexturePool::Instance =
        new MidLevelRenderer::MLRTexturePool(
            new MidLevelRenderer::TGAFilePool("data\\tgl\\128\\"));
    gos_PopCurrentHeap();

    gosFX::InitializeClasses();

    // --- load ---
    gos_PushCurrentHeap(gosFX::Heap);
    Stuff::MemoryStream stream(bytes.data(), static_cast<int>(bytes.size()));
    mc2::particles::SpecLibrary* lib = mc2::particles::SpecLibrary::Instance();
    lib->Load(&stream);
    unsigned count = lib->Count();

    // --- emit JSON ---
    std::string out;
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
    gos_PopCurrentHeap();

    if (outPath) {
        FILE* of = std::fopen(outPath, "wb");
        if (!of) { std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3; }
        std::fwrite(out.data(), 1, out.size(), of);
        std::fclose(of);
        std::fprintf(stderr, "mc2fx: dumped %u effects -> %s\n", count, outPath);
    } else {
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}

void usage()
{
    std::fprintf(stderr,
        "mc2fx — gosFX effect blob inspector\n"
        "  mc2fx dump <mc2.fx> [out.json]   dump effect catalog to JSON (stdout if no out)\n");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "dump") {
        return cmdDump(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    usage();
    return 1;
}
