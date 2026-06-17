// gos_vegetation.cpp — GPU instanced vegetation card batcher.
//
// Two crossed screen-aligned quads per instance, rendered with
// glDrawElementsInstanced. Enabled by MC2_VEGETATION_CARDS env var.
//
// Vertex layout (static VBO, stride 20):
//   location 0: vec3(horizontal, vertical, yawOffset)  offset  0
//   location 1: vec2(uvX, uvY)                         offset 12
//
// Instance layout (instance VBO, stride 28 = sizeof(Instance)):
//   location 2: vec3(x,y,z)        offset  0, divisor 1
//   location 3: float yaw          offset 12, divisor 1
//   location 4: float scale        offset 16, divisor 1
//   location 5: uint atlasFrame    offset 20, divisor 1
//   location 6: float seed         offset 24, divisor 1

#include "gos_vegetation.h"
#include "../../mclib/render_contract.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// File-local state
// ---------------------------------------------------------------------------
namespace {

static GLuint       s_vao          = 0;
static GLuint       s_staticVbo    = 0;
static GLuint       s_ibo          = 0;
static GLuint       s_instanceVbo  = 0;
static GLuint       s_atlasTexId   = 0;
static glsl_program* s_prog        = nullptr;
static uint32_t     s_instanceCount = 0;
static bool         s_initialized  = false;

// Vertex for static crossed-quad geometry.
struct CardVert {
    float horizontal;
    float vertical;
    float yawOffset;
    float uvX;
    float uvY;
};
static_assert(sizeof(CardVert) == 20, "CardVert stride must be 20 bytes");

// 8 vertices: two crossed quads.
// Quad 1 (yawOffset = 0.0), Quad 2 (yawOffset = PI/2).
static const CardVert k_verts[8] = {
    // Quad 1
    {-0.5f, 0.0f, 0.0f,       0.0f, 0.0f},   // BL
    { 0.5f, 0.0f, 0.0f,       1.0f, 0.0f},   // BR
    {-0.5f, 1.0f, 0.0f,       0.0f, 1.0f},   // TL
    { 0.5f, 1.0f, 0.0f,       1.0f, 1.0f},   // TR
    // Quad 2
    {-0.5f, 0.0f, 1.5707963f, 0.0f, 0.0f},   // BL
    { 0.5f, 0.0f, 1.5707963f, 1.0f, 0.0f},   // BR
    {-0.5f, 1.0f, 1.5707963f, 0.0f, 1.0f},   // TL
    { 0.5f, 1.0f, 1.5707963f, 1.0f, 1.0f},   // TR
};

// 12 indices (2 quads x 2 tris x 3 indices).
static const uint16_t k_indices[12] = {
    0, 1, 2,  1, 3, 2,   // quad 1
    4, 5, 6,  5, 7, 6,   // quad 2
};

// Default atlas path; can be overridden via setAtlasPath() before init().
static const char* s_atlasPathOverride = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GosVegetation::isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = (getenv("MC2_VEGETATION_CARDS") != nullptr) ? 1 : 0;
    }
    return cached == 1;
}

uint32_t GosVegetation::instanceCount() {
    return s_instanceCount;
}

void GosVegetation::setAtlasPath(const char* path) {
    // Must be called before init(); the pointer must remain valid through init().
    s_atlasPathOverride = path;
}

void GosVegetation::init() {
    if (s_initialized) {
        return;
    }

    // --- Shader ---
    s_prog = glsl_program::makeProgram(
        "vegetation_card",
        "shaders/gos_vegetation_card.vert",
        "shaders/gos_vegetation_card.frag",
        "#version 430\n");
    if (!s_prog || !s_prog->is_valid()) {
        fprintf(stderr,
            "[VEG v1] event=init_warn msg=shader_load_failed "
            "vert=shaders/gos_vegetation_card.vert "
            "frag=shaders/gos_vegetation_card.frag\n");
        s_prog = nullptr;
    }

    // --- Static VBO (crossed quads) ---
    glGenBuffers(1, &s_staticVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_staticVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(k_verts)),
                 k_verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // --- IBO ---
    glGenBuffers(1, &s_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(k_indices)),
                 k_indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // --- Instance VBO (starts empty; populated by uploadInstances) ---
    glGenBuffers(1, &s_instanceVbo);

    // --- VAO ---
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    // Bind static vertex buffer and configure per-vertex attribs.
    glBindBuffer(GL_ARRAY_BUFFER, s_staticVbo);

    // attr 0: vec3(horizontal, vertical, yawOffset), offset 0, stride 20
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20,
                          reinterpret_cast<const void*>(0));

    // attr 1: vec2(uvX, uvY), offset 12, stride 20
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20,
                          reinterpret_cast<const void*>(12));

    // Bind instance VBO and configure per-instance attribs.
    // stride = sizeof(Instance) = 28
    constexpr GLsizei kInstStride = static_cast<GLsizei>(sizeof(GosVegetation::Instance));
    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVbo);

    // attr 2: vec3(x,y,z), offset 0, divisor 1
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(0));
    glVertexAttribDivisor(2, 1);

    // attr 3: float yaw, offset 12, divisor 1
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(12));
    glVertexAttribDivisor(3, 1);

    // attr 4: float scale, offset 16, divisor 1
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(16));
    glVertexAttribDivisor(4, 1);

    // attr 5: uint atlasFrame, offset 20, divisor 1
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, kInstStride,
                           reinterpret_cast<const void*>(20));
    glVertexAttribDivisor(5, 1);

    // attr 6: float seed, offset 24, divisor 1
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(24));
    glVertexAttribDivisor(6, 1);

    // Bind IBO into VAO state.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // --- Atlas texture ---
    const char* atlasPath = s_atlasPathOverride
                          ? s_atlasPathOverride
                          : "data/textures/vegetation_grass_card.png";

    // Allow env-var override at runtime too.
    const char* envPath = getenv("MC2_VEGETATION_ATLAS");
    if (envPath && envPath[0] != '\0') {
        atlasPath = envPath;
    }

    Texture atlasTex = load_texture_from_file(atlasPath);
    if (atlasTex.isValid()) {
        s_atlasTexId = atlasTex.id;
    } else {
        fprintf(stderr,
            "[VEG v1] event=atlas_load_failed path=%s "
            "(flush will no-op until uploadInstances + valid atlas)\n",
            atlasPath);
        s_atlasTexId = 0;
    }

    s_initialized = true;
}

void GosVegetation::shutdown() {
    if (s_vao)         { glDeleteVertexArrays(1, &s_vao);        s_vao = 0; }
    if (s_staticVbo)   { glDeleteBuffers(1, &s_staticVbo);       s_staticVbo = 0; }
    if (s_ibo)         { glDeleteBuffers(1, &s_ibo);              s_ibo = 0; }
    if (s_instanceVbo) { glDeleteBuffers(1, &s_instanceVbo);     s_instanceVbo = 0; }
    if (s_atlasTexId)  { glDeleteTextures(1, &s_atlasTexId);     s_atlasTexId = 0; }
    if (s_prog)        { glsl_program::deleteProgram("vegetation_card"); s_prog = nullptr; }
    s_instanceCount  = 0;
    s_initialized    = false;
}

void GosVegetation::uploadInstances(const Instance* instances, uint32_t count) {
    if (!s_initialized) {
        return;
    }
    s_instanceCount = count;
    if (count == 0 || instances == nullptr) {
        return;
    }

    const GLsizeiptr byteSize =
        static_cast<GLsizeiptr>(count) *
        static_cast<GLsizeiptr>(sizeof(Instance));

    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVbo);
    glBufferData(GL_ARRAY_BUFFER, byteSize, instances, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GosVegetation::flush(const float* worldToClipGL_4x4,
                          float camX, float camY, float camZ,
                          float lightDirX, float lightDirY,
                          float lightDirZ, float lightDirW,
                          float time) {
    if (!isEnabled()) {
        return;
    }
    if (s_instanceCount == 0 || s_atlasTexId == 0) {
        return;
    }
    if (!s_prog || !s_prog->is_valid()) {
        return;
    }

    // --- Explicit GL state for vegetation card pass ---
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);       // depth write OFF for v1 (cards transparent to each other)
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);     // both sides visible
    glDepthFunc(GL_GEQUAL);      // reverse-Z

    // [RENDER_CONTRACT:Pass=VegetationCards]
    render_contract::assertPassContract(
        render_contract::PassIdentity::VegetationCards,
        "GosVegetation::flush");

    // Bind program.
    s_prog->apply();

    const GLuint progId = s_prog->shp_;

    // Upload matrix uniform (explicit-program form per project rule).
    {
        const GLint loc = glGetUniformLocation(progId, "u_worldToClipGL");
        glProgramUniformMatrix4fv(progId, loc, 1, GL_FALSE, worldToClipGL_4x4);
    }

    // Upload vec3 camera pos.
    {
        const GLint loc = glGetUniformLocation(progId, "u_cameraPos");
        glProgramUniform3f(progId, loc, camX, camY, camZ);
    }

    // Upload vec4 terrain light direction.
    {
        const GLint loc = glGetUniformLocation(progId, "u_terrainLightDir");
        glProgramUniform4f(progId, loc, lightDirX, lightDirY, lightDirZ, lightDirW);
    }

    // Upload time.
    {
        const GLint loc = glGetUniformLocation(progId, "u_time");
        glProgramUniform1f(progId, loc, time);
    }

    // Bind atlas texture to unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlasTexId);
    {
        const GLint loc = glGetUniformLocation(progId, "u_atlas");
        glProgramUniform1i(progId, loc, 0);
    }

    // Draw.
    glBindVertexArray(s_vao);
    glDrawElementsInstanced(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT,
                            nullptr,
                            static_cast<GLsizei>(s_instanceCount));

    // --- Restore critical state ---
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    // glDepthFunc remains GL_GEQUAL (reverse-Z; matches rest of engine)

    glBindVertexArray(0);
}
