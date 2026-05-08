// GameOS/gameos/gos_mech_batcher.cpp — GPU mech batcher, Slice A.
#include "gos_mech_batcher.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "gameos.hpp"                 // gos_InvalidateRenderStateCache
#include "utils/shader_builder.h"
#include "../../mclib/txmmgr.h"       // mcTextureManager->get_gosTextureHandle (live resolve)
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>

// MECH_RING_FRAMES must equal STATIC_PROP_RING_FRAMES so the parity SSBO
// ring and the mech fence ring share the same depth.
static_assert(MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES,
              "MECH_RING_FRAMES must match STATIC_PROP_RING_FRAMES");

// Enabled by env-var MC2_GPU_MECHS=1 at process start. Slice A is opt-in;
// runtime hotkey toggle (RAlt+M) is deferred — wire alongside the default-on
// flip if it becomes useful.
bool g_useGpuMechs = (getenv("MC2_GPU_MECHS") != nullptr);

// Slice B1: enables calc_light() in mech.vert. Requires g_useGpuMechs=true
// to take effect (the calc_light branch is inside the GPU mech draw path).
bool g_useGpuMechLighting = (getenv("MC2_GPU_MECH_LIGHTING") != nullptr);

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------
static bool   s_programLoadTried  = false;
static bool   s_programLoadFailed = false;
static GLuint s_mechProgram       = 0;
static glsl_program* s_mechProgramObj = nullptr;

// Cached uniform locations (set at program link time).
static GLint s_loc_u_instanceBase    = -1;
static GLint s_loc_u_materialFlags   = -1;
static GLint s_loc_terrainMVP     = -1;
static GLint s_loc_u_terrainViewport = -1;
static GLint s_loc_u_mvp             = -1;
static GLint s_loc_u_tex             = -1;
static GLint s_loc_u_fogValue        = -1;
static GLint s_loc_u_debugMode       = -1;
static GLint s_loc_u_lightingMode    = -1;

// Geometry (immutable after finalizeGeometry).
static GLuint s_sharedVao = 0;
static GLuint s_sharedVbo = 0;
static GLuint s_sharedIbo = 0;
static GLuint s_sampler   = 0;    // session-lifetime; GL_REPEAT / LINEAR
static bool   s_geometryFinalized = false;

// Staging buffers (cleared after finalizeGeometry).
static std::vector<uint8_t>   s_stagingVbo;
static std::vector<uint32_t>  s_stagingIbo;

// Type registration.
static std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
static std::vector<GpuMechPacket>        s_packets;

// Key: (Mech3DAppearanceType*, lod) -> s_typeLodRecords index
struct TypeLodKey {
    const Mech3DAppearanceType* type;
    int lod;
    bool operator==(const TypeLodKey& o) const { return type == o.type && lod == o.lod; }
};
struct TypeLodKeyHash {
    size_t operator()(const TypeLodKey& k) const {
        return std::hash<const void*>()(k.type) ^ (std::hash<int>()(k.lod) * 2654435761u);
    }
};
static std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;

// Per-frame ring SSBOs.
static GLuint   s_instanceSsbo    = 0;
static GLuint   s_boneSsbo        = 0;
static void*    s_instanceMap     = nullptr;
static void*    s_boneMap         = nullptr;
static size_t   s_instanceCapacity = 0;  // per ring slot (in GpuMechInstance units)
static size_t   s_boneCapacity     = 0;  // per ring slot (in GpuMechBone units)
static uint32_t s_frameSlot        = 0;
static GLsync   s_fence[MECH_RING_FRAMES] = {};

static constexpr size_t kInitialInstancesPerFrame = 512;
static constexpr size_t kInitialBonesPerFrame     = 8192;

// Per-frame pending submit list.
struct PendingSubmit {
    GpuMechSubmitDesc        desc;
    std::vector<GpuMechBone> bones;          // staged from listOfShapes[i].shapeToWorld
    std::vector<uint32_t>    packetTexHandles; // per-packet live gosHandle captured at submit
    uint32_t                 typeLodIdx;
};
static std::vector<PendingSubmit> s_pendingSubmits;

// Counters.
static bool     s_mechBatcherTrace     = false;
static bool     s_mechBatcherTraceInit = false;
static bool     s_mechLightTrace       = false;
static bool     s_mechLightTraceInit   = false;
static uint32_t s_lightCacheFullFrames = 0;  // monotonic; emitted on first overflow per frame
static uint32_t s_eligibleActorsThisFrame = 0;
static uint32_t s_fallbacksThisFrame[5]   = {};  // indexed by GpuMechFallbackReason
static uint64_t s_allowedLateRegEvents    = 0;
static uint64_t s_disallowedLateRegEvents = 0;
static bool     s_lastFailWasLateReg      = false;

// ---------------------------------------------------------------------------
// Shader load
// ---------------------------------------------------------------------------
static void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    s_programLoadTried = true;

    s_mechProgramObj = glsl_program::makeProgram(
        "mech", "shaders/mech.vert", "shaders/mech.frag", "#version 430\n");

    if (!s_mechProgramObj || !s_mechProgramObj->is_valid()) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=shader_fail — GPU mech path disabled\n");
        s_mechProgramObj    = nullptr;
        s_mechProgram       = 0;
        s_programLoadFailed = true;
        return;
    }
    s_mechProgram = s_mechProgramObj->shp_;

    auto loc = [&](const char* name) {
        return glGetUniformLocation(s_mechProgram, name);
    };
    s_loc_u_instanceBase    = loc("u_instanceBase");
    s_loc_u_materialFlags   = loc("u_materialFlags");
    s_loc_terrainMVP     = loc("terrainMVP");
    s_loc_u_terrainViewport = loc("u_terrainViewport");
    s_loc_u_mvp             = loc("u_mvp");
    s_loc_u_tex             = loc("u_tex");
    s_loc_u_fogValue        = loc("u_fogValue");
    s_loc_u_debugMode       = loc("u_debugMode");
    s_loc_u_lightingMode    = loc("u_lightingMode");

    std::fprintf(stderr, "[MECHBATCHER v1] event=shader_ok prog=%u\n", s_mechProgram);
}

// ---------------------------------------------------------------------------
// Ring SSBO management
// ---------------------------------------------------------------------------
static void ensureRingCapacity(size_t neededInstances, size_t neededBones) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededBones     > s_boneCapacity;
    if (!needGrow) return;

    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);     s_boneSsbo     = 0; s_boneMap     = nullptr; }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : kInitialInstancesPerFrame);
    s_boneCapacity = std::max(neededBones,
        s_boneCapacity ? s_boneCapacity * 2 : kInitialBonesPerFrame);

    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)),
        nullptr, flags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_instanceCapacity * sizeof(GpuMechInstance)), flags);

    glGenBuffers(1, &s_boneSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_boneSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)),
        nullptr, flags);
    s_boneMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
        (GLsizeiptr)(MECH_RING_FRAMES * s_boneCapacity * sizeof(GpuMechBone)), flags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_boneMap) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=persistent_map_fail\n");
    }
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=ring_alloc instances=%zu bones=%zu\n",
        s_instanceCapacity, s_boneCapacity);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GpuMechBatcher& GpuMechBatcher::instance() {
    static GpuMechBatcher batcher;
    return batcher;
}

void GpuMechBatcher::onMapLoad() {
    s_typeLodRecords.clear();
    s_packets.clear();
    s_typeLodIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_geometryFinalized = false;
    s_programLoadTried  = false;
    s_programLoadFailed = false;
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_load\n");
}

void GpuMechBatcher::onMapUnload() {
    for (uint32_t i = 0; i < MECH_RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_sharedVao)    { glDeleteVertexArrays(1, &s_sharedVao);    s_sharedVao    = 0; }
    if (s_sharedVbo)    { glDeleteBuffers(1, &s_sharedVbo);         s_sharedVbo    = 0; }
    if (s_sharedIbo)    { glDeleteBuffers(1, &s_sharedIbo);         s_sharedIbo    = 0; }
    if (s_sampler)      { glDeleteSamplers(1, &s_sampler);          s_sampler      = 0; }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo);      s_instanceSsbo = 0; s_instanceMap = nullptr; }
    if (s_boneSsbo)     { glDeleteBuffers(1, &s_boneSsbo);          s_boneSsbo     = 0; s_boneMap     = nullptr; }
    s_instanceCapacity  = 0;
    s_boneCapacity      = 0;
    s_geometryFinalized = false;
    s_pendingSubmits.clear();
    std::fprintf(stderr, "[MECHBATCHER v1] event=map_unload\n");
}

bool GpuMechBatcher::wasLastFailureLateRegistration() const { return s_lastFailWasLateReg; }
uint64_t GpuMechBatcher::getAllowedLateRegEventCount()      { return s_allowedLateRegEvents; }
uint64_t GpuMechBatcher::getDisallowedLateRegEventCount()  { return s_disallowedLateRegEvents; }

void GpuMechBatcher::recordEligibleActor()                         { ++s_eligibleActorsThisFrame; }
void GpuMechBatcher::recordCpuFallback(GpuMechFallbackReason r)   { ++s_fallbacksThisFrame[(int)r]; }
void GpuMechBatcher::flushShadow() {}  // no-op in Slice A/B1/B2

// ---------------------------------------------------------------------------
// Registration (Task 4)
// ---------------------------------------------------------------------------
void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType* mechType, int lod) {
    if (!mechType) return;
    const TypeLodKey key{mechType, lod};
    if (s_typeLodIndex.count(key)) return;  // idempotent
    if (s_geometryFinalized) {
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=late_register type=%p lod=%d\n",
            (void*)mechType, lod);
        ++s_disallowedLateRegEvents;
        return;
    }

    TG_TypeMultiShape* typeMulti = mechType->mechShape[lod];
    if (!typeMulti) return;

    const int numNodes = typeMulti->GetNumShapes();
    if (numNodes == 0 || numNodes > 255) {
        if (numNodes > 255) {
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=u8_bone_overflow type=%p lod=%d numNodes=%d\n",
                (void*)mechType, lod, numNodes);
        }
        return;
    }

    static const bool s_nodeTrace = (getenv("MC2_MECH_NODE_TRACE") != nullptr);
    if (s_nodeTrace) {
        std::fprintf(stderr, "[MECHREG v1] event=register type=%p lod=%d numBones=%d\n",
                     (void*)mechType, lod, numNodes);
    }

    const uint32_t typeLodIdx = (uint32_t)s_typeLodRecords.size();
    s_typeLodIndex[key] = typeLodIdx;

    GpuMechTypeLodRecord rec{};
    rec.firstBoneIndex = 0;
    rec.numBones       = (uint32_t)numNodes;
    rec.firstPacket    = (uint32_t)s_packets.size();
    rec.packetCount    = 0;
    rec.vertexCount    = 0;
    rec.sourceNode0    = nullptr;

    for (int nodeIdx = 0; nodeIdx < numNodes; ++nodeIdx) {
        TG_TypeNodePtr tnode = typeMulti->GetTypeNode(nodeIdx);
        if (!tnode || tnode->GetNodeType() != SHAPE_NODE) continue;
        TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(tnode);
        if (nodeIdx == 0) rec.sourceNode0 = typeShape;

        // Skip spotlight leaves: TG_Shape::isSpotlight is set on the
        // per-instance shape from "SpotLight_*" node name prefix at
        // tgl.cpp:259/475. The CPU mech path's per-leaf rendering
        // path skips spotlights based on per-actor lightsOut state;
        // Slice A doesn't carry per-actor lightsOut, so skipping
        // spotlight geometry entirely is the safe move. Mirrors what
        // gos_static_prop_batcher.cpp does via its renderFlags
        // (bit 2: isSpotlight). Slice B+ can re-enable with a
        // per-actor lightsOut/spotlight flag.
        const char* nodeName = tnode->getNodeId();
        if (nodeName && S_strnicmp(nodeName, "SpotLight_", 10) == 0) {
            if (s_nodeTrace) {
                std::fprintf(stderr,
                    "[MECHREG v1] event=skip_spotlight type=%p lod=%d nodeIdx=%d name=%s\n",
                    (void*)mechType, lod, nodeIdx, nodeName);
            }
            continue;
        }

        if (!typeShape->numTypeTriangles || !typeShape->listOfTypeTriangles ||
            !typeShape->listOfTypeVertices) continue;

        const int32_t baseVertex = (int32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex));

        // Group triangles by localTextureHandle (same as static prop batcher).
        const uint32_t numTris = typeShape->numTypeTriangles;
        uint32_t runStart = 0;
        while (runStart < numTris) {
            const DWORD runTexSlot =
                typeShape->listOfTypeTriangles[runStart].localTextureHandle;
            uint32_t runEnd = runStart;
            while (runEnd < numTris &&
                   typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTexSlot)
                ++runEnd;

            const uint32_t packetFirstIndex = (uint32_t)s_stagingIbo.size();

            for (uint32_t t = runStart; t < runEnd; ++t) {
                const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
                const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
                const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

                for (int c = 0; c < 3; ++c) {
                    const TG_TypeVertex& src =
                        typeShape->listOfTypeVertices[tri.Vertices[c]];

                    GpuMechVertex vert{};
                    std::memcpy(vert.position, &src.position.x, 12);
                    std::memcpy(vert.normal,   &src.normal.x,   12);
                    vert.uv[0] = cornerU[c];
                    vert.uv[1] = cornerV[c];
                    // boneIndices: .x = nodeIdx (rigid, Slice A), .yzw = 0
                    vert.boneIndices[0] = (uint8_t)(nodeIdx & 0xFF);
                    vert.boneIndices[1] = 0;
                    vert.boneIndices[2] = 0;
                    vert.boneIndices[3] = 0;
                    // boneWeights: .x = 255 (= 1.0 normalized), .yzw = 0
                    vert.boneWeights[0] = 255;
                    vert.boneWeights[1] = 0;
                    vert.boneWeights[2] = 0;
                    vert.boneWeights[3] = 0;
                    // tangentOct: zero-fill for stock (no .tglgpu sidecar)
                    vert.tangentOct[0] = 0;
                    vert.tangentOct[1] = 0;
                    vert.aRGBLight = src.aRGBLight;

                    s_stagingVbo.insert(s_stagingVbo.end(),
                        reinterpret_cast<uint8_t*>(&vert),
                        reinterpret_cast<uint8_t*>(&vert) + sizeof(GpuMechVertex));

                    // Write index LOCAL to this packet's baseVertex.
                    // glDrawElementsInstancedBaseVertex adds pkt.baseVertex at draw time,
                    // so IBO must contain (globalVertex - baseVertex), not globalVertex.
                    const uint32_t localIdx =
                        (uint32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex) - 1u)
                        - (uint32_t)baseVertex;
                    s_stagingIbo.push_back(localIdx);
                    ++rec.vertexCount;
                }
            }

            // Derive ALPHA_TEST_BIT from the texture slot's textureAlpha flag.
            uint32_t matFlags = 0;
            if (runTexSlot < (DWORD)typeShape->numTextures &&
                typeShape->listOfTextures[runTexSlot].textureAlpha) {
                matFlags = 1u;  // ALPHA_TEST_BIT (matches mech.frag ALPHA_TEST_BIT constant)
            }

            GpuMechPacket pkt{};
            pkt.firstIndex          = packetFirstIndex;
            pkt.indexCount          = (runEnd - runStart) * 3;
            pkt.baseVertex          = baseVertex;
            pkt.textureSlot         = (uint32_t)runTexSlot;
            pkt.materialFlags       = matFlags;
            pkt.owningTypeLodRecord = typeLodIdx;
            pkt.nodeLocalIndex      = (uint32_t)nodeIdx;
            pkt.owningTypeShape     = typeShape;
            s_packets.push_back(pkt);
            ++rec.packetCount;

            runStart = runEnd;
        }
    }

    s_typeLodRecords.push_back(rec);
}

void GpuMechBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;
    loadProgramsIfNeeded();

    // Bail cleanly if shader failed: geometry upload skipped.
    // submit() fast-rejects on s_geometryFinalized==false.
    if (s_programLoadFailed) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_skip reason=shader_fail\n");
        return;
    }

    if (s_stagingVbo.empty()) {
        std::fprintf(stderr, "[MECHBATCHER v1] event=finalize_empty — no types registered\n");
        s_geometryFinalized = true;
        return;
    }

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    (GLsizeiptr)s_stagingVbo.size(),
                    s_stagingVbo.data(), 0);  // immutable, GPU-only

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(), 0);

    // Vertex attribute setup — 48-byte GpuMechVertex, 7 attributes.
    auto enableF = [](GLuint loc, GLint sz, GLenum type, GLboolean norm,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, sz, type, norm, stride, (void*)offset);
    };
    auto enableI = [](GLuint loc, GLint sz, GLenum type,
                      GLsizei stride, size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, sz, type, stride, (void*)offset);
    };

    const GLsizei S = (GLsizei)sizeof(GpuMechVertex);
    enableF(0, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, position));
    enableF(1, 3, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, normal));
    enableF(2, 2, GL_FLOAT,          GL_FALSE, S, offsetof(GpuMechVertex, uv));
    enableI(3, 4, GL_UNSIGNED_BYTE,            S, offsetof(GpuMechVertex, boneIndices));
    enableF(4, 4, GL_UNSIGNED_BYTE,  GL_TRUE,  S, offsetof(GpuMechVertex, boneWeights));
    enableF(5, 2, GL_SHORT,          GL_TRUE,  S, offsetof(GpuMechVertex, tangentOct));
    enableI(6, 1, GL_UNSIGNED_INT,             S, offsetof(GpuMechVertex, aRGBLight));

    glBindVertexArray(0);

    // Session-lifetime sampler: GL_REPEAT / GL_LINEAR.
    // GL_LINEAR (not GL_LINEAR_MIPMAP_LINEAR) because mech textures may not
    // have mipmaps generated — sampling a mipmap chain that doesn't exist
    // is undefined behavior on AMD, often black. Slice A+ can revisit if
    // mech textures get mipmaps from the upscaler pipeline.
    glGenSamplers(1, &s_sampler);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T,     GL_REPEAT);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    s_geometryFinalized = true;
    std::fprintf(stderr,
        "[MECHBATCHER v1] event=finalize_ok types=%zu packets=%zu\n",
        s_typeLodRecords.size(), s_packets.size());
}

// ---------------------------------------------------------------------------
// submitActor (Task 6) — bone staging + texture capture
// ---------------------------------------------------------------------------
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return false;
    if (!desc.mechShape || !desc.mechType) return false;

    const TypeLodKey key{desc.mechType, desc.currentLOD};
    auto it = s_typeLodIndex.find(key);
    if (it == s_typeLodIndex.end()) {
        s_lastFailWasLateReg = true;
        ++s_disallowedLateRegEvents;
        return false;
    }
    const uint32_t typeLodIdx = it->second;
    const GpuMechTypeLodRecord& rec = s_typeLodRecords[typeLodIdx];

    PendingSubmit ps;
    ps.desc       = desc;
    ps.typeLodIdx = typeLodIdx;
    ps.bones.reserve(rec.numBones);

    // Stage bone matrices from live shapeToWorld (set by TransformMultiShape).
    // listOfShapes[i].shapeToWorld is a Stuff::LinearMatrix4D with entries[12]
    // stored column-major: entries[(col<<2)+row], 3 explicit cols + implicit col3=[0,0,0,1].
    // Row k extraction: [entries[k], entries[4+k], entries[8+k], w] where w=1 for row3 only.
    const int numShapes = desc.mechShape->GetNumShapes();
    for (int i = 0; i < numShapes && i < (int)rec.numBones; ++i) {
        const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
        const float* e = (const float*)sr.shapeToWorld.entries;
        GpuMechBone bone;
        bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
        bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
        bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
        bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
        ps.bones.push_back(bone);
    }
    while ((int)ps.bones.size() < (int)rec.numBones) {
        GpuMechBone id{};
        id.row0[0]=1.f; id.row1[1]=1.f; id.row2[2]=1.f; id.row3[3]=1.f;
        ps.bones.push_back(id);
    }

    // Capture live per-actor texture handle for each packet.
    //
    // SLOT 0 is per-actor (paint scheme / team color). TG_TypeShape::listOfTextures
    // is a shared type-level cache mutated by TransformMultiShape — by render time
    // it reflects the LAST actor through, not the current one. Use desc.slot0TexHandle
    // (the raw gos handle passed by the caller) directly for slot 0.
    //
    // SLOTS 1+ are type-stable; reading from owningTypeShape is correct.
    ps.packetTexHandles.resize(rec.packetCount, 0);
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.owningTypeShape && pkt.owningTypeShape->listOfTextures &&
                   pkt.textureSlot < (uint32_t)pkt.owningTypeShape->numTextures) {
            ps.packetTexHandles[p] =
                pkt.owningTypeShape->listOfTextures[pkt.textureSlot].gosTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));
    return true;
}

// ---------------------------------------------------------------------------
// flush (Task 7) — bucket-sorted compaction + draw loop
// ---------------------------------------------------------------------------
void GpuMechBatcher::flush() {
    if (!s_mechBatcherTraceInit) {
        s_mechBatcherTrace     = (getenv("MC2_MECH_BATCHER_STATS") != nullptr);
        s_mechBatcherTraceInit = true;
    }
    if (!s_mechLightTraceInit) {
        s_mechLightTrace     = (getenv("MC2_MECH_LIGHT_TRACE") != nullptr);
        s_mechLightTraceInit = true;
    }
    if (s_mechLightTrace) {
        // LightsData UBO holds 32 ObjectLights entries (lighting.hglsl).
        // Any submit with lightDataIndex >= 32 reads OOB; AMD typically
        // returns zero → flat-black mech. Surface the event so soak ops
        // can raise the cap. Recipe to fix when fired: bump
        // lightDataStructuresCapacity in mclib/txmmgr.cpp + the
        // LightsData[N] array in shaders/include/lighting.hglsl in
        // lockstep per memory/cpp_glsl_ubo_struct_lockstep.md.
        bool overCap = false;
        for (const auto& ps : s_pendingSubmits) {
            if (ps.desc.lightDataIndex >= 32u) { overCap = true; break; }
        }
        if (overCap) {
            ++s_lightCacheFullFrames;
            std::fprintf(stderr,
                "[MECHLIGHT v1] event=cache_full frames=%u submitted=%zu\n",
                s_lightCacheFullFrames, s_pendingSubmits.size());
        }
    }

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed ||
        s_pendingSubmits.empty()) {
        s_pendingSubmits.clear();
        s_eligibleActorsThisFrame = 0;
        std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
        return;
    }

    // Step 1: Count total bones (one block per actor).
    size_t totalBones = 0;
    for (const auto& ps : s_pendingSubmits) totalBones += ps.bones.size();

    // Step 2: Build draw buckets.
    // Key: (typeLodIdx, globalPacketIdx, texHandle, materialFlags).
    // Each actor × packet produces one entry in the matching bucket.
    // Different per-actor paint schemes for the same packet -> different buckets.
    struct BucketKey {
        uint32_t typeLodIdx;
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        bool operator<(const BucketKey& o) const {
            if (typeLodIdx      != o.typeLodIdx)      return typeLodIdx      < o.typeLodIdx;
            if (globalPacketIdx != o.globalPacketIdx) return globalPacketIdx < o.globalPacketIdx;
            if (texHandle       != o.texHandle)       return texHandle       < o.texHandle;
            return materialFlags < o.materialFlags;
        }
    };

    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]

    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        const PendingSubmit& ps = s_pendingSubmits[si];
        const GpuMechTypeLodRecord& rec = s_typeLodRecords[ps.typeLodIdx];
        for (uint32_t p = 0; p < rec.packetCount; ++p) {
            const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
            BucketKey key;
            key.typeLodIdx       = ps.typeLodIdx;
            key.globalPacketIdx  = rec.firstPacket + p;
            key.texHandle        = ps.packetTexHandles[p];
            key.materialFlags    = pkt.materialFlags;
            buckets[key].push_back(si);
        }
    }

    size_t totalInstances = 0;
    for (const auto& kv : buckets) totalInstances += kv.second.size();

    ensureRingCapacity(totalInstances, totalBones);
    if (!s_instanceMap || !s_boneMap) {
        s_pendingSubmits.clear();
        return;
    }

    // Step 3: Advance ring slot and wait for oldest fence.
    s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    GpuMechInstance* instDst = (GpuMechInstance*)s_instanceMap + s_frameSlot * s_instanceCapacity;
    GpuMechBone*     boneDst = (GpuMechBone*)    s_boneMap     + s_frameSlot * s_boneCapacity;

    // Step 4: Write bone SSBO once per actor; record each actor's boneBase offset.
    std::vector<uint32_t> actorBoneBase(s_pendingSubmits.size());
    uint32_t boneHead = 0;
    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        actorBoneBase[si] = boneHead;
        for (const auto& b : s_pendingSubmits[si].bones)
            boneDst[boneHead++] = b;
    }

    // Step 5: Write instance SSBO in bucket order; collect draw calls.
    struct DrawCall {
        uint32_t globalPacketIdx;
        uint32_t texHandle;
        uint32_t materialFlags;
        uint32_t instanceBase;
        uint32_t instanceCount;
    };
    std::vector<DrawCall> drawCalls;
    drawCalls.reserve(buckets.size());

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t instHead = 0;
    for (const auto& kv : buckets) {
        const BucketKey& key              = kv.first;
        const std::vector<uint32_t>& subs = kv.second;

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = instHead;
        dc.instanceCount   = (uint32_t)subs.size();

        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            instDst[instHead++]     = inst;
        }
        drawCalls.push_back(dc);
    }

    // Step 6: Bind SSBOs (whole per-frame slices; shader indexes via u_instanceBase).
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
        (GLintptr) (s_frameSlot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (GLsizeiptr)(totalInstances * sizeof(GpuMechInstance)));
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_boneSsbo,
        (GLintptr) (s_frameSlot * s_boneCapacity * sizeof(GpuMechBone)),
        (GLsizeiptr)(totalBones * sizeof(GpuMechBone)));

    // Save prior GL state. The mech flush bypasses applyRenderStates'
    // tracked slot set, so the engine's render-state cache becomes
    // out-of-sync after we mutate state directly. We save EVERYTHING the
    // static_prop batcher saves (mirror its pattern), restore at end, and
    // call gos_InvalidateRenderStateCache() to force the next
    // applyRenderStates to re-issue. Skipping invalidate has been observed
    // to make subsequent draws (water, mechs themselves on later frames)
    // disappear after a few frames as the cache thinks GL is in state X
    // while reality is state Y.
    GLint     prevDepthFunc;   glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    GLboolean prevDepthTest;   glGetBooleanv(GL_DEPTH_TEST,      &prevDepthTest);
    GLboolean prevDepthMask;   glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevBlend;       glGetBooleanv(GL_BLEND,            &prevBlend);
    GLboolean prevCull;        glGetBooleanv(GL_CULL_FACE,        &prevCull);
    GLint     prevCullMode;    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    GLint     prevProgram;     glGetIntegerv(GL_CURRENT_PROGRAM,  &prevProgram);
    GLint     prevVao;         glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLint     prevArrayBuf;    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    GLint     prevElemBuf;     glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    GLint     prevActiveTex;   glGetIntegerv(GL_ACTIVE_TEXTURE,   &prevActiveTex);
    GLint     prevSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &prevSampler);
    GLint     prevSsbo0     = 0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &prevSsbo0);
    GLint     prevSsbo1     = 0; glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &prevSsbo1);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTexUnit0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);

    glUseProgram(s_mechProgram);
    glBindVertexArray(s_sharedVao);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Bind our REPEAT/LINEAR sampler. Per
    // memory/sampler_state_inheritance_in_fast_paths.md: when a prior pass
    // (e.g. patch_stream) leaves a CLAMP_TO_EDGE sampler bound on unit 0,
    // mech UVs that fall outside [0,1] (legitimate for tiled mech body
    // textures) all clamp to the texture edge — which for many mech
    // textures is a black border, producing the all-black mech symptom.
    // Sampler-object state OVERRIDES the texture object's glTexParameter
    // values, so setting them on the texture object alone is insufficient.
    // Identified via debug=7 (hardcoded UV (0.5, 0.5) showed yellow paint
    // while debug=2 with v_uv showed black).
    glBindSampler(0, s_sampler);

    // Static uniforms.
    glUniform1i(s_loc_u_tex,      0);
    glUniform1f(s_loc_u_fogValue, 1.0f);
    // Slice B1: lighting mode 0 = Slice A flat-white passthrough,
    // 1 = calc_light() per-vertex. Set per-flush from killswitch.
    if (s_loc_u_lightingMode >= 0)
        glUniform1i(s_loc_u_lightingMode, g_useGpuMechLighting ? 1 : 0);
    if (s_mechBatcherTrace) {
        static int s_uniDiagPrinted = 0;
        if (s_uniDiagPrinted < 2) {
            ++s_uniDiagPrinted;
            GLint utexVal = -99;
            if (s_loc_u_tex >= 0)
                glGetUniformiv(s_mechProgram, s_loc_u_tex, &utexVal);
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=uni_probe loc_u_tex=%d u_tex_val=%d s_loc_terrainMVP=%d s_loc_u_mvp=%d s_loc_u_terrainViewport=%d\n",
                s_loc_u_tex, utexVal, s_loc_terrainMVP, s_loc_u_mvp, s_loc_u_terrainViewport);
        }
    }
    {
        // MC2_MECH_FRAG_DEBUG=N: 0=normal, 1=magenta, 2=texOnly, 3=lightOnly, 4=normal.
        const char* dbg = std::getenv("MC2_MECH_FRAG_DEBUG");
        const int dbgMode = dbg ? std::atoi(dbg) : 0;
        if (s_loc_u_debugMode >= 0)
            glUniform1i(s_loc_u_debugMode, dbgMode);
    }

    // Projection uniforms — match static_prop batcher and the
    // terrain_overlay.vert convention: terrainMVP = CPU-composed
    // axisSwap * worldToClip, row-major, uploaded with GL_FALSE.
    // Plan template said "TG_Shape::s_worldToClip with GL_TRUE" — that's
    // wrong; it skips the axis swap, mech ends up off-screen. Caught
    // 2026-05-08 by operator visual smoke (mechs invisible after PREC fix
    // re-enabled the GPU path).
    const float* terrainMVP = gos_GetTerrainMVPMat4();
    if (s_loc_terrainMVP >= 0 && terrainMVP)
        glUniformMatrix4fv(s_loc_terrainMVP, 1, GL_FALSE, terrainMVP);
    const float* vp = gos_GetTerrainViewportVec4();
    if (s_loc_u_terrainViewport >= 0 && vp)
        glUniform4fv(s_loc_u_terrainViewport, 1, vp);
    const float* mm = gos_GetProj2ScreenMat4();
    if (s_loc_u_mvp >= 0 && mm)
        glUniformMatrix4fv(s_loc_u_mvp, 1, GL_TRUE, mm);

    // Step 7: Issue one draw call per bucket.
    uint32_t drawnCalls = 0;
    static int s_texDiagPrinted = 0;
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        if (s_loc_u_instanceBase >= 0)
            glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
        if (s_loc_u_materialFlags >= 0)
            glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);

        // dc.texHandle is the mcTextureManager slot index, NOT a gos
        // handle (TG_TinyTexture::gosTextureHandle is a misnamed slot
        // index per memory/mc2_texture_handle_is_live.md). Resolve to the
        // live gos handle THIS FRAME, then to GL texture id.
        const DWORD liveGosHandle = (dc.texHandle != 0xFFFFFFFFu && mcTextureManager)
            ? mcTextureManager->get_gosTextureHandle(dc.texHandle)
            : 0u;
        const uint32_t glTexId = gos_GetGLTextureId((uint32_t)liveGosHandle);
        if (s_mechBatcherTrace && s_texDiagPrinted < 16) {
            ++s_texDiagPrinted;
            // Probe what the bound texture object actually contains.
            GLint tw = 0, th = 0, tfmt = 0, tMin = 0, tMag = 0;
            GLint sR = 0, sG = 0, sB = 0, sA = 0;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &th);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tfmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tMin);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &tMag);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sR);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sG);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sB);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sA);
            // Probe the actually-effective sampler state (sampler-object
            // overrides texture-object params silently per brainstorm #1).
            GLint boundSampler = -1;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &boundSampler);
            GLint sampMin = -1, sampMag = -1, sampWrapS = -1;
            if (boundSampler != 0) {
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MIN_FILTER, &sampMin);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MAG_FILTER, &sampMag);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_WRAP_S,     &sampWrapS);
            }
            std::fprintf(stderr,
                "[MECHBATCHER v1] event=tex_probe glTex=%u w=%d h=%d fmt=0x%x texMin=0x%x texMag=0x%x boundSampler=%d sampMin=0x%x sampMag=0x%x sampWrapS=0x%x\n",
                glTexId, tw, th, tfmt, tMin, tMag, boundSampler, sampMin, sampMag, sampWrapS);
            // Pixel-readback diagnostic — gated on its own env var
            // (NOT MC2_MECH_BATCHER_STATS) because glGetTexImage is a
            // synchronous GPU stall that pollutes any perf measurement.
            // Only enable when actively diagnosing texture-content bugs.
            static const bool s_pixelReadback = (getenv("MC2_MECH_TEX_READBACK") != nullptr);
            if (s_pixelReadback && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                std::vector<uint8_t> pixels((size_t)tw * th * 3, 0);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                int nonZero = 0;
                for (uint8_t b : pixels) if (b != 0) { nonZero = 1; break; }
                std::fprintf(stderr,
                    "[MECHBATCHER v1] event=tex_pixels glTex=%u bytes=%zu nonZero=%d first15="
                    "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
                    glTexId, pixels.size(), nonZero,
                    pixels[ 0], pixels[ 1], pixels[ 2],
                    pixels[ 3], pixels[ 4], pixels[ 5],
                    pixels[ 6], pixels[ 7], pixels[ 8],
                    pixels[ 9], pixels[10], pixels[11],
                    pixels[12], pixels[13], pixels[14]);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glTexId);
        // The actual texture-black fix in Slice A is mech.frag's
        // textureLod(u_tex, v_uv, 0.0) — see memory/amd_auto_lod_strict_fail.md.
        // Sampler-state inheritance is defended by the glBindSampler(0,
        // s_sampler) above (REPEAT/LINEAR), which OVERRIDES texture-object
        // params anyway. So no per-texture glTexParameteri here. Keeping the
        // texture object's persistent state untouched avoids leaking mech-
        // specific filter/wrap onto a gosTexture handle that another renderer
        // may want to sample with auto-LOD or CLAMP later.

        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++drawnCalls;
    }

    // Restore prior state in reverse order of save.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)prevSsbo0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)prevSsbo1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    glActiveTexture((GLenum)prevActiveTex);
    glBindSampler(0, (GLuint)prevSampler);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevCull)  { glEnable(GL_CULL_FACE); glCullFace((GLenum)prevCullMode); }
    else             glDisable(GL_CULL_FACE);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // RENDER_STATES v1: even with the explicit save/restore above, the
    // engine's applyRenderStates cache mirrors a separate (slot,value)
    // table — it does NOT re-read GL state. Without invalidate, a
    // subsequent applyRenderStates call early-outs on matching cached
    // values while the actual texture binding / sampler / depth bits we
    // touched go unrechecked. Mirrors gos_static_prop_batcher.cpp:1791.
    gos_InvalidateRenderStateCache();

    // Per-reason stats output (MC2_MECH_BATCHER_STATS=1).
    if (s_mechBatcherTrace) {
        static const char* const kFallbackNames[] = {
            "UnregisteredType", "U8BoneOverflow", "RingOverflow",
            "TglGpuUnsupported", "ShaderInitFailure"
        };
        uint32_t fallbackTotal = 0;
        for (int i = 0; i < 5; ++i) fallbackTotal += s_fallbacksThisFrame[i];
        std::fprintf(stderr,
            "[MECHBATCHER v1] event=summary eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u fallback_total=%u\n",
            s_eligibleActorsThisFrame, s_pendingSubmits.size(),
            buckets.size(), drawnCalls, fallbackTotal);
        for (int i = 0; i < 5; ++i) {
            if (s_fallbacksThisFrame[i] > 0) {
                std::fprintf(stderr,
                    "[MECHBATCHER v1] event=fallback reason=%s count=%u\n",
                    kFallbackNames[i], s_fallbacksThisFrame[i]);
            }
        }
    }

    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
}
