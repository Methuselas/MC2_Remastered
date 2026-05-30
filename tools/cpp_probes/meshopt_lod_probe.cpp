// meshopt_lod_probe (MESHOPT-LOD-PROBE-1) — OFFLINE host tool.
//
// Self-contained, deterministic: no file input, no randomness. Generates a
// procedural tessellated grid plane in code, runs meshopt_simplify to produce
// a level-1 LOD, runs meshopt_buildMeshlets to count clusters, and emits a
// manifest-compatible JSON (a `lods` array + a `probe` stats block) to stdout
// (and to --out <path> if given).
//
// Scope: OFFLINE tooling only. Links ONLY the vendored meshoptimizer static
// lib. No runtime LOD behavior, no renderer/DrawPacket/material coupling.
//
// Procedural mesh generation (deterministic):
//   - N = 33 vertices per side -> 33x33 = 1089 vertices.
//   - Grid spans the unit square [0,1]^2 in X/Z; vertex (i,j) at
//       x = i/(N-1), z = j/(N-1).
//   - Height field y = 0.15 * sin(x * 2pi * 3) * cos(z * 2pi * 3). The smooth
//     ridged surface gives meshopt_simplify meaningful error to minimize, so
//     the collapse is non-trivial.
//   - Two triangles per quad cell: (N-1)*(N-1)*2 = 32*32*2 = 2048 triangles,
//     6144 indices. Winding (v00,v10,v11) (v00,v11,v01).

#include <meshoptimizer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int   kN          = 33;     // vertices per side
constexpr float kTargetRatio = 0.25f; // level-1 target index fraction
constexpr float kTargetError = 0.01f; // meshopt_simplify target_error
constexpr float kPi          = 3.14159265358979323846f;

// Count distinct vertex indices referenced by an index buffer.
size_t uniqueVertexCount(const std::vector<unsigned int>& indices, size_t vertexCount) {
    std::vector<uint8_t> used(vertexCount, 0);
    size_t count = 0;
    for (unsigned int idx : indices) {
        if (idx < vertexCount && !used[idx]) {
            used[idx] = 1;
            ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    const char* outPath = nullptr;
    for (int a = 1; a < argc; ++a) {
        if (std::strcmp(argv[a], "--out") == 0 && a + 1 < argc) {
            outPath = argv[++a];
        }
    }

    // --- Procedural mesh -----------------------------------------------------
    std::vector<float> positions; // xyz interleaved
    positions.reserve(static_cast<size_t>(kN) * kN * 3);
    for (int j = 0; j < kN; ++j) {
        for (int i = 0; i < kN; ++i) {
            const float x = static_cast<float>(i) / (kN - 1);
            const float z = static_cast<float>(j) / (kN - 1);
            const float y = 0.15f * std::sin(x * 2.0f * kPi * 3.0f) *
                            std::cos(z * 2.0f * kPi * 3.0f);
            positions.push_back(x);
            positions.push_back(y);
            positions.push_back(z);
        }
    }
    const size_t inputVertices = static_cast<size_t>(kN) * kN;

    std::vector<unsigned int> indices;
    indices.reserve(static_cast<size_t>(kN - 1) * (kN - 1) * 6);
    for (int j = 0; j < kN - 1; ++j) {
        for (int i = 0; i < kN - 1; ++i) {
            const unsigned int v00 = static_cast<unsigned int>(j * kN + i);
            const unsigned int v10 = v00 + 1;
            const unsigned int v01 = v00 + kN;
            const unsigned int v11 = v01 + 1;
            indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
            indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
        }
    }
    const size_t inputIndices = indices.size();
    const size_t inputTriangles = inputIndices / 3;

    // --- Simplify (level 1) --------------------------------------------------
    const size_t targetIndexCount =
        static_cast<size_t>(kTargetRatio * static_cast<float>(inputIndices));
    std::vector<unsigned int> simplified(inputIndices);
    float resultError = 0.0f;
    const size_t outIndexCount = meshopt_simplify(
        simplified.data(), indices.data(), inputIndices,
        positions.data(), inputVertices, sizeof(float) * 3,
        targetIndexCount, kTargetError, /*options=*/0, &resultError);
    simplified.resize(outIndexCount);

    const size_t outputIndices   = outIndexCount;
    const size_t outputTriangles = outputIndices / 3;
    const size_t outputVertices  = uniqueVertexCount(simplified, inputVertices);

    // --- Meshlets (cluster count) -------------------------------------------
    const size_t kMaxVertices  = 64;
    const size_t kMaxTriangles = 124;
    const float  kConeWeight   = 0.0f;
    const size_t maxMeshlets =
        meshopt_buildMeshletsBound(inputIndices, kMaxVertices, kMaxTriangles);
    std::vector<meshopt_Meshlet>  meshlets(maxMeshlets);
    std::vector<unsigned int>     meshletVertices(maxMeshlets * kMaxVertices);
    std::vector<unsigned char>    meshletTriangles(maxMeshlets * kMaxTriangles * 3);
    const size_t clusterCount = meshopt_buildMeshlets(
        meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
        indices.data(), inputIndices, positions.data(), inputVertices,
        sizeof(float) * 3, kMaxVertices, kMaxTriangles, kConeWeight);

    // --- Emit JSON -----------------------------------------------------------
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"lods\": [\n"
        "    { \"level\": 0, \"vertexCount\": %zu, \"triangleCount\": %zu, \"error\": 0.0 },\n"
        "    { \"level\": 1, \"vertexCount\": %zu, \"triangleCount\": %zu, \"error\": %.8f }\n"
        "  ],\n"
        "  \"probe\": {\n"
        "    \"inputVertices\": %zu,\n"
        "    \"inputIndices\": %zu,\n"
        "    \"outputVertices\": %zu,\n"
        "    \"outputIndices\": %zu,\n"
        "    \"targetRatio\": %.4f,\n"
        "    \"resultError\": %.8f,\n"
        "    \"clusterCount\": %zu\n"
        "  }\n"
        "}\n",
        inputVertices, inputTriangles,
        outputVertices, outputTriangles, resultError,
        inputVertices, inputIndices, outputVertices, outputIndices,
        kTargetRatio, resultError, clusterCount);

    if (n < 0 || n >= static_cast<int>(sizeof(buf))) {
        std::fprintf(stderr, "meshopt_lod_probe: JSON buffer overflow\n");
        return 1;
    }

    std::fputs(buf, stdout);

    if (outPath) {
        FILE* f = std::fopen(outPath, "wb");
        if (!f) {
            std::fprintf(stderr, "meshopt_lod_probe: cannot open --out %s\n", outPath);
            return 1;
        }
        std::fwrite(buf, 1, static_cast<size_t>(n), f);
        std::fclose(f);
    }

    return 0;
}
