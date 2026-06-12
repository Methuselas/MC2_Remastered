// Standalone correctness harness for ktxDecodeBc7ToRgba8 (COMPRESSION-BC7-CPUDECODE-1).
// Loads a BC7 .ktx2, decodes mip0 to RGBA8, writes a raw .rgba dump, and (if a
// ground-truth raw is provided) compares per-channel against it.
// Build: cl /EHsc /std:c++17 test_bc7_decode.cpp KtxLoader.cpp
// Run:   test_bc7_decode <in.ktx2> <out.rgba> [groundtruth.rgba]
#include "KtxLoader.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <in.ktx2> <out.rgba> [gt.rgba]\n", argv[0]); return 2; }

    RenderCore::KtxImage img;
    if (!RenderCore::ktxLoadRgba8(argv[1], img)) { std::fprintf(stderr, "ktxLoadRgba8 FAILED\n"); return 1; }
    std::printf("loaded: %dx%d vkFormat=%u compressed=%d mips=%d srgb=%d\n",
                img.width, img.height, img.vkFormat, (int)img.isCompressed, img.mipCount, (int)img.isSrgb);
    if (!img.isCompressed) { std::fprintf(stderr, "not BC7\n"); return 1; }

    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!RenderCore::ktxDecodeBc7ToRgba8(img, 0, rgba, &w, &h)) { std::fprintf(stderr, "decode FAILED\n"); return 1; }
    std::printf("decoded: %dx%d bytes=%zu\n", w, h, rgba.size());
    if (rgba.size() != (size_t)w * h * 4) { std::fprintf(stderr, "size mismatch\n"); return 1; }

    // Basic sanity: not all-zero, alpha present.
    size_t nz = 0; for (uint8_t b : rgba) if (b) ++nz;
    std::printf("nonzero bytes: %zu / %zu (%.1f%%)\n", nz, rgba.size(), 100.0 * nz / rgba.size());

    if (FILE* o = std::fopen(argv[2], "wb")) { std::fwrite(rgba.data(), 1, rgba.size(), o); std::fclose(o); }

    if (argc >= 4) {
        FILE* g = std::fopen(argv[3], "rb");
        if (!g) { std::fprintf(stderr, "cannot open gt %s\n", argv[3]); return 1; }
        std::vector<uint8_t> gt(rgba.size());
        size_t rd = std::fread(gt.data(), 1, gt.size(), g); std::fclose(g);
        std::printf("gt bytes read: %zu (expected %zu)\n", rd, gt.size());
        if (rd != gt.size()) { std::fprintf(stderr, "gt size mismatch\n"); return 1; }
        double sse = 0; int maxd = 0;
        for (size_t i = 0; i < rgba.size(); ++i) { int d = (int)rgba[i] - (int)gt[i]; sse += (double)d * d; if (std::abs(d) > maxd) maxd = std::abs(d); }
        double mse = sse / rgba.size();
        double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
        std::printf("vs ground-truth: MSE=%.4f PSNR=%.2fdB maxDelta=%d\n", mse, psnr, maxd);
        // Both decoders are exact BC7 -> identical output expected. Allow tiny slack.
        if (maxd > 1) { std::fprintf(stderr, "FAIL: maxDelta>%d\n", 1); return 1; }
        std::printf("PASS: decode matches ground-truth (maxDelta<=1)\n");
    }
    return 0;
}
