// GameOS/gameos/gos_materials.cpp
//
// Material profile registry implementation.
// See gos_materials.h for full contract / texture semantic documentation.
//
// Texture semantic used by this table: MaterialTextureSemantic::RawGlId
//   normalTex / metallicRoughnessTex store raw GL texture object IDs.
//   This table is HOMOGENEOUS in RawGlId semantic (unlike the per-actor mech
//   table at binding 2 which is TextureManagerSlot).
//   Slice C (mech.frag sampling) must bind u_normalMap / u_ormMap explicitly
//   using the raw GL id from this table, NOT via mcTextureManager.
//
// Profiles registered:
//   index 0 : "default"  -- passthrough; no textures; flat metallic=0 rough=0.85
//   index 1 : "metal061b"-- Metal061B_1K normal + packed ORM (R=AO G=rough B=metal)
//                           (only when MC2_MECH_SURFACE_MATERIAL != "0"/"none")
//

#include "gos_materials.h"

#include "../../RenderCore/MaterialGpu.h"
#include "utils/Image.h"
#include <GL/glew.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

namespace {

// MC2_MATERIAL_GPU defaults ON (mirrors gos_mech_batcher.cpp / gos_static_prop_batcher.cpp).
// Set MC2_MATERIAL_GPU=0 to disable all table upload and bind.
const bool s_gpuEnabled = []() {
    const char* v = getenv("MC2_MATERIAL_GPU");
    return v == nullptr || (v[0] != '0');
}();

struct ProfileEntry {
    std::string    name;
    RenderCore::MaterialGpu gpu;      // uploaded to SSBO
    GLuint         ownedNormal = 0;   // raw GL tex owned by this entry; 0 = none
    GLuint         ownedOrm    = 0;   // raw GL tex owned by this entry; 0 = none
};

std::vector<ProfileEntry>                s_profiles;
std::unordered_map<std::string, uint32_t> s_nameToIndex;
GLuint                                    s_ssbo = 0;
bool                                      s_initialized = false;

// ---------------------------------------------------------------------------
// Helper: upload profile table to SSBO at binding 5.
// ---------------------------------------------------------------------------
static void uploadSsbo() {
    if (!s_gpuEnabled) return;
    if (s_profiles.empty()) return;

    std::vector<RenderCore::MaterialGpu> table;
    table.reserve(s_profiles.size());
    for (const auto& p : s_profiles)
        table.push_back(p.gpu);

    if (s_ssbo == 0) glGenBuffers(1, &s_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(table.size() * sizeof(RenderCore::MaterialGpu)),
                 table.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    std::fprintf(stderr,
        "[GOS_MATERIALS] uploaded profile table: %u entries, ssbo=%u\n",
        static_cast<uint32_t>(table.size()), s_ssbo);
}

// ---------------------------------------------------------------------------
// Helper: load a single-channel or RGB PNG as GL_RGB8 (linear).
// Returns GL texture object, or 0 on failure.
// w_out / h_out filled on success.
// ---------------------------------------------------------------------------
static GLuint loadLinearPng(const char* path, int& w_out, int& h_out) {
    Image img;
    if (!img.loadPNG(path)) {
        std::fprintf(stderr, "[GOS_MATERIALS] WARN: could not load PNG: %s\n", path);
        return 0;
    }
    w_out = img.getWidth();
    h_out = img.getHeight();
    FORMAT fmt = img.getFormat();

    // Determine GL internal/source formats.
    GLenum glInternal, glFormat, glType;
    switch (fmt) {
    case FORMAT_I8:   // single channel -> replicate to R channel, upload as GL_RED
        glInternal = GL_R8;
        glFormat   = GL_RED;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_IA8:  // two channels (R, A) -> upload as RG
        glInternal = GL_RG8;
        glFormat   = GL_RG;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_RGB8:
        glInternal = GL_RGB8;
        glFormat   = GL_RGB;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_RGBA8:
        glInternal = GL_RGBA8;
        glFormat   = GL_RGBA;
        glType     = GL_UNSIGNED_BYTE;
        break;
    default:
        std::fprintf(stderr,
            "[GOS_MATERIALS] WARN: unsupported PNG format %d for: %s\n", (int)fmt, path);
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, glInternal,
                 w_out, h_out, 0,
                 glFormat, glType, img.getPixels());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::fprintf(stderr, "[GOS_MATERIALS] loaded linear PNG %s -> tex=%u (%dx%d fmt=%d)\n",
        path, tex, w_out, h_out, (int)fmt);
    return tex;
}

// ---------------------------------------------------------------------------
// Helper: build packed ORM texture (GL_RGB8, linear).
//   R = AO       (no AO source -> fill 255)
//   G = Roughness (from roughPath, any channel)
//   B = Metallic  (from metalPath, any channel)
// Both source images must be the same size.
// Returns GL texture object, or 0 on any failure.
// ---------------------------------------------------------------------------
static GLuint buildPackedOrm(const char* roughPath, const char* metalPath,
                              int expectedW, int expectedH) {
    // Load roughness channel.
    Image roughImg;
    if (!roughImg.loadPNG(roughPath)) {
        std::fprintf(stderr, "[GOS_MATERIALS] WARN: could not load roughness PNG: %s\n", roughPath);
        return 0;
    }
    // Load metalness channel.
    Image metalImg;
    if (!metalImg.loadPNG(metalPath)) {
        std::fprintf(stderr, "[GOS_MATERIALS] WARN: could not load metalness PNG: %s\n", metalPath);
        return 0;
    }

    const int rw = roughImg.getWidth();
    const int rh = roughImg.getHeight();
    const int mw = metalImg.getWidth();
    const int mh = metalImg.getHeight();

    if (rw != mw || rh != mh) {
        std::fprintf(stderr,
            "[GOS_MATERIALS] WARN: roughness (%dx%d) and metalness (%dx%d) size mismatch\n",
            rw, rh, mw, mh);
        return 0;
    }

    const int w = rw;
    const int h = rh;
    if (expectedW > 0 && (w != expectedW || h != expectedH)) {
        // Size mismatch vs normal map -- log but continue (shader uses same UV).
        std::fprintf(stderr,
            "[GOS_MATERIALS] WARN: ORM (%dx%d) differs from normal (%dx%d) -- continuing\n",
            w, h, expectedW, expectedH);
    }

    // Determine bytes per pixel for each source.
    const int rBpp = getBytesPerPixel(roughImg.getFormat());
    const int mBpp = getBytesPerPixel(metalImg.getFormat());
    if (rBpp == 0 || mBpp == 0) {
        std::fprintf(stderr, "[GOS_MATERIALS] WARN: zero-bpp format in ORM sources\n");
        return 0;
    }

    const unsigned char* rPixels = roughImg.getPixels();
    const unsigned char* mPixels = metalImg.getPixels();

    // Pack into RGB: R=255(AO), G=roughness(channel0), B=metalness(channel0).
    std::vector<uint8_t> packed(static_cast<size_t>(w * h * 3));
    for (int i = 0; i < w * h; ++i) {
        packed[i * 3 + 0] = 255u;                       // R = AO (no source, fill 1.0)
        packed[i * 3 + 1] = rPixels[i * rBpp + 0];      // G = Roughness (channel 0)
        packed[i * 3 + 2] = mPixels[i * mBpp + 0];      // B = Metallic  (channel 0)
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, packed.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::fprintf(stderr,
        "[GOS_MATERIALS] packed ORM tex=%u (%dx%d) rough=%s metal=%s\n",
        tex, w, h, roughPath, metalPath);
    return tex;
}

// ---------------------------------------------------------------------------
// Register "default" profile at index 0.
// Passthrough: no textures; flat scalars only.
// ---------------------------------------------------------------------------
static void registerDefault() {
    ProfileEntry e;
    e.name = "default";

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    g.normalTex            = RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;
    g.flags                = 0u;
    g.baseColorFactor      = 1.0f;
    g.metallicFactor       = 0.0f;
    g.roughnessFactor      = 0.85f;

    s_nameToIndex["default"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// Register "metal061b" profile.
// Source files (1K-PNG set from ambientCG Metal061B download):
//   NormalGL  -> normalTex  (GL_RGB8, linear)
//   Roughness + Metalness -> metallicRoughnessTex  (packed ORM, GL_RGB8, linear)
// ---------------------------------------------------------------------------
static void registerMetal061b() {
    static const char* kNormal = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_NormalGL.png";
    static const char* kRough  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Roughness.png";
    static const char* kMetal  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Metalness.png";

    ProfileEntry e;
    e.name = "metal061b";

    // Load normal map (linear -- NOT sRGB).
    int nw = 0, nh = 0;
    e.ownedNormal = loadLinearPng(kNormal, nw, nh);

    // Build packed ORM texture.
    e.ownedOrm = buildPackedOrm(kRough, kMetal, nw, nh);

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    // Store raw GL texture IDs (RawGlId semantic -- see header).
    // Slice C (mech.frag) must bind these as raw GL textures, NOT via mcTextureManager.
    g.normalTex            = e.ownedNormal  != 0
                                 ? e.ownedNormal
                                 : RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = e.ownedOrm != 0
                                 ? e.ownedOrm
                                 : RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;

    // Only set map flags when textures loaded successfully.
    g.flags = 0u;
    if (e.ownedNormal != 0)
        g.flags |= RenderCore::MaterialFlags::kNormalMap;
    if (e.ownedOrm != 0)
        g.flags |= RenderCore::MaterialFlags::kMetallicRoughness;

    g.baseColorFactor = 1.0f;
    g.metallicFactor  = 1.0f;
    g.roughnessFactor = 1.0f;

    s_nameToIndex["metal061b"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));

    std::fprintf(stderr,
        "[GOS_MATERIALS] registered metal061b: normalTex=%u ormTex=%u flags=0x%x\n",
        g.normalTex, g.metallicRoughnessTex, g.flags);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace gos_materials {

void init() {
    if (s_initialized) return;
    s_initialized = true;

    if (!s_gpuEnabled) {
        std::fprintf(stderr,
            "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");
        return;
    }

    // Always register index-0 default.
    registerDefault();

    // Gate: MC2_MECH_SURFACE_MATERIAL controls which profiles to load.
    // null/unset or "metal061b" -> load metal061b.
    // "0" or "none" -> no extra profiles.
    const char* matEnv = getenv("MC2_MECH_SURFACE_MATERIAL");
    const bool loadMetal061b =
        (matEnv == nullptr) ||
        (matEnv[0] == '\0') ||
        (strcmp(matEnv, "metal061b") == 0);

    if (loadMetal061b) {
        registerMetal061b();
    } else {
        std::fprintf(stderr,
            "[GOS_MATERIALS] MC2_MECH_SURFACE_MATERIAL=%s: metal061b skipped\n",
            matEnv ? matEnv : "(null)");
    }

    uploadSsbo();

    std::fprintf(stderr,
        "[GOS_MATERIALS] init complete: %u profiles, ssbo=%u\n",
        static_cast<uint32_t>(s_profiles.size()), s_ssbo);
}

void shutdown() {
    // Delete all owned GL textures.
    for (auto& p : s_profiles) {
        if (p.ownedNormal) {
            glDeleteTextures(1, &p.ownedNormal);
            p.ownedNormal = 0;
        }
        if (p.ownedOrm) {
            glDeleteTextures(1, &p.ownedOrm);
            p.ownedOrm = 0;
        }
    }
    s_profiles.clear();
    s_nameToIndex.clear();

    if (s_ssbo) {
        glDeleteBuffers(1, &s_ssbo);
        s_ssbo = 0;
    }

    s_initialized = false;
    std::fprintf(stderr, "[GOS_MATERIALS] shutdown complete\n");
}

uint32_t getProfileIndex(const char* name) {
    if (!name) return 0u;
    auto it = s_nameToIndex.find(name);
    if (it == s_nameToIndex.end()) return 0u;
    return it->second;
}

uint32_t profileCount() {
    return static_cast<uint32_t>(s_profiles.size());
}

void bindMaterialTable() {
    if (!s_gpuEnabled) return;
    if (s_ssbo == 0) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, s_ssbo);
}

} // namespace gos_materials
