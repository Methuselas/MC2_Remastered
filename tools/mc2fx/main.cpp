// tools/mc2fx/main.cpp
//
// mc2fx — standalone gosFX effect-blob (.fx) inspector / round-trip tool.
//
// Thin arg dispatcher over mc2fx_core (initEngineHeadless/loadBlob/
// dumpCatalogJson/saveBlob). Subcommands:
//   mc2fx dump    <mc2.fx> [out.json]   dump effect catalog to JSON
//   mc2fx rebuild <in.fx>  <out.fx>     Load -> Save round-trip + byte-compare
//
// `rebuild` is the Save-fidelity gate: it re-emits the blob via SpecLibrary::
// Save and byte-compares against the input, reporting the first divergence.

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mc2fx_core.h"
#include "particles/spec_library.h"   // SpecLibrary (for the returned handle)

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

bool writeFileBytes(const char* path, const unsigned char* data, size_t n)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    size_t put = std::fwrite(data, 1, n, f);
    std::fclose(f);
    return put == n;
}

int cmdDump(const char* inPath, const char* outPath)
{
    std::vector<unsigned char> bytes;
    if (!readFileBytes(inPath, bytes)) {
        std::fprintf(stderr, "mc2fx: cannot read '%s'\n", inPath);
        return 2;
    }
    mc2fx::initEngineHeadless();
    mc2::particles::SpecLibrary* lib = mc2fx::loadBlob(bytes.data(), bytes.size());
    if (!lib) { std::fprintf(stderr, "mc2fx: load failed\n"); return 4; }
    std::string out = mc2fx::dumpCatalogJson(lib);

    if (outPath) {
        if (!writeFileBytes(outPath, reinterpret_cast<const unsigned char*>(out.data()), out.size())) {
            std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
        }
        std::fprintf(stderr, "mc2fx: dumped %u effects -> %s\n", lib->Count(), outPath);
    } else {
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}

int cmdRebuild(const char* inPath, const char* outPath)
{
    std::vector<unsigned char> inBytes;
    if (!readFileBytes(inPath, inBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read '%s'\n", inPath);
        return 2;
    }
    mc2fx::initEngineHeadless();
    mc2::particles::SpecLibrary* lib = mc2fx::loadBlob(inBytes.data(), inBytes.size());
    if (!lib) { std::fprintf(stderr, "mc2fx: load failed\n"); return 4; }

    std::vector<unsigned char> outBytes;
    // Pre-size the Save stream from the input: the engine's Save writer never
    // grows its buffer (size_t/DWORD virtual-override mismatch), so the stream
    // must be born big enough. 4x input + 1MiB headroom covers any re-emit growth.
    size_t reserveHint = inBytes.size() * 4 + (1u << 20);
    if (!mc2fx::saveBlob(lib, outBytes, reserveHint)) {
        std::fprintf(stderr, "mc2fx: save failed (no terminating size found)\n");
        return 5;
    }
    if (!writeFileBytes(outPath, outBytes.data(), outBytes.size())) {
        std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
    }

    // --- byte-compare ---
    std::fprintf(stderr, "mc2fx rebuild: in=%zu bytes  out=%zu bytes\n",
                 inBytes.size(), outBytes.size());
    size_t n = inBytes.size() < outBytes.size() ? inBytes.size() : outBytes.size();
    size_t firstDiff = (size_t)-1;
    for (size_t i = 0; i < n; ++i) {
        if (inBytes[i] != outBytes[i]) { firstDiff = i; break; }
    }
    if (firstDiff == (size_t)-1 && inBytes.size() == outBytes.size()) {
        std::fprintf(stderr, "mc2fx rebuild: IDENTICAL (byte-exact round-trip)\n");
        std::printf("IDENTICAL\n");
        return 0;
    }
    if (firstDiff == (size_t)-1) {
        // common prefix matches but lengths differ
        firstDiff = n;
    }
    std::fprintf(stderr, "mc2fx rebuild: DIFFER at offset %zu\n", firstDiff);
    auto hexDump = [](const char* label, const std::vector<unsigned char>& b, size_t off) {
        std::fprintf(stderr, "  %s:", label);
        for (size_t i = off; i < off + 16 && i < b.size(); ++i)
            std::fprintf(stderr, " %02X", b[i]);
        std::fprintf(stderr, "\n");
    };
    size_t ctx = firstDiff >= 4 ? firstDiff - 4 : 0;
    hexDump("in ", inBytes, ctx);
    hexDump("out", outBytes, ctx);
    std::printf("DIFFER@%zu in=%zu out=%zu\n", firstDiff, inBytes.size(), outBytes.size());
    return 0;
}

void usage()
{
    std::fprintf(stderr,
        "mc2fx — gosFX effect blob inspector\n"
        "  mc2fx dump    <mc2.fx> [out.json]   dump effect catalog to JSON (stdout if no out)\n"
        "  mc2fx rebuild <in.fx>  <out.fx>     Load->Save round-trip + byte-compare\n");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "dump") {
        return cmdDump(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    if (cmd == "rebuild") {
        if (argc < 4) { usage(); return 1; }
        return cmdRebuild(argv[2], argv[3]);
    }
    usage();
    return 1;
}
