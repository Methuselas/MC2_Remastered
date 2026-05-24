#include "debug_renderer.h"

#include <cstdlib>   // getenv
#include <cstdio>    // fprintf, stderr
#include <cstddef>   // offsetof
#include <vector>
#include <cmath>     // sinf, cosf

#include <GL/glew.h>

#include "utils/shader_builder.h"  // glsl_program::makeProgram

// Forward-declare the scene viewproj getter (defined in gameos_graphics.cpp).
// Upload with the same transpose convention as the existing terrain MVP upload sites.
extern const float* gos_GetTerrainMVPMat4();

namespace DebugRenderer {

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct DbgVert { float x, y, z, r, g, b, a; };

static constexpr int kVertCapacity = 65536;

// ---------------------------------------------------------------------------
// Env helper (default OFF -- absent or "0" = disabled)
// MUST be defined before the static bools that call it.
// ---------------------------------------------------------------------------

static bool envFlagDefaultOff(const char* name) {
    const char* v = getenv(name);
    return v && !(v[0] == '0' && v[1] == '\0');
}

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static bool              s_enabled       = envFlagDefaultOff("MC2_DEBUG_RENDERER");
static bool              s_testCanary    = envFlagDefaultOff("MC2_DEBUG_RENDERER_TEST");
static bool              s_initialized   = false;
static bool              s_capWarnedOnce = false;

static GLuint            s_vao = 0;
static GLuint            s_vbo = 0;
static glsl_program*     s_program = nullptr;
static GLuint            s_programId = 0;  // cached for glProgramUniformMatrix4fv
static GLint             s_mvpLoc = -1;

static std::vector<DbgVert> s_verts;

// ---------------------------------------------------------------------------
// Internal: init on first flush
// ---------------------------------------------------------------------------

static void init_once() {
    if (s_initialized) return;
    s_initialized = true;

    fprintf(stderr, "[DEBUGDRAW v1] event=init enabled=1\n");

    // Load shader (no #version in files; prefix injected here per project rule)
    s_program = glsl_program::makeProgram(
        "debug_prim",
        "shaders/debug_prim.vert",
        "shaders/debug_prim.frag",
        "#version 430\n"
    );
    if (!s_program) {
        fprintf(stderr, "[DEBUGDRAW v1] event=shader_fail -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // Cache program ID for explicit-program uniform upload.
    // We read shp_ directly (public field, same as gos_static_prop_batcher.cpp:539 and
    // gameos_graphics.cpp:2094) instead of going through apply() -- apply() flushes dirty
    // matrix uniforms with GL_TRUE (shader_builder.cpp:781), wrong for our GL_FALSE MVP.
    s_programId = s_program->shp_;

    s_mvpLoc = glGetUniformLocation(s_programId, "uMVP");
    if (s_mvpLoc < 0) {
        fprintf(stderr, "[DEBUGDRAW v1] event=uniform_missing name=uMVP -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // VAO + VBO
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    // Allocate buffer at max capacity once
    glBufferData(GL_ARRAY_BUFFER,
                 kVertCapacity * (GLsizeiptr)sizeof(DbgVert),
                 nullptr, GL_DYNAMIC_DRAW);

    // layout(location=0) aPosition: vec3 at offset 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, x));

    // layout(location=1) aColor: vec4 at offset 12
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, r));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    s_verts.reserve(4096);
}

// ---------------------------------------------------------------------------
// Color unpack helper
// ---------------------------------------------------------------------------

static DbgVert makeVert(float x, float y, float z, uint32_t rgba) {
    DbgVert v;
    v.x = x; v.y = y; v.z = z;
    v.r = ((rgba >> 24) & 0xFF) / 255.0f;
    v.g = ((rgba >> 16) & 0xFF) / 255.0f;
    v.b = ((rgba >>  8) & 0xFF) / 255.0f;
    v.a = ((rgba >>  0) & 0xFF) / 255.0f;
    return v;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba) {
    if (!s_enabled) return;
    s_verts.push_back(makeVert(a.x, a.y, a.z, rgba));
    s_verts.push_back(makeVert(b.x, b.y, b.z, rgba));
}

void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba) {
    if (!s_enabled) return;
    // 8 corners: bit0=X, bit1=Y, bit2=Z
    float cx[8], cy[8], cz[8];
    for (int i = 0; i < 8; ++i) {
        cx[i] = (i & 1) ? mx.x : mn.x;
        cy[i] = (i & 2) ? mx.y : mn.y;
        cz[i] = (i & 4) ? mx.z : mn.z;
    }
    // 12 edges: explicit list, no __builtin_popcount (MSVC portability)
    static constexpr int kEdges[12][2] = {
        {0,1}, {0,2}, {0,4},
        {1,3}, {1,5},
        {2,3}, {2,6},
        {3,7},
        {4,5}, {4,6},
        {5,7},
        {6,7},
    };
    for (const auto& e : kEdges) {
        int a = e[0], b = e[1];
        s_verts.push_back(makeVert(cx[a], cy[a], cz[a], rgba));
        s_verts.push_back(makeVert(cx[b], cy[b], cz[b], rgba));
    }
}

void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba) {
    if (!s_enabled) return;
    if (segments < 3)   segments = 3;
    if (segments > 256) segments = 256;
    const float step = 6.2831853f / (float)segments;  // 2*PI
    for (int i = 0; i < segments; ++i) {
        float a0 = step * i;
        float a1 = step * (i + 1);
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a0),
            center.y,
            center.z + radius * sinf(a0), rgba));
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a1),
            center.y,
            center.z + radius * sinf(a1), rgba));
    }
}

void flushWorldPrims() {
    // full implementation in Task 4
    s_verts.clear();   // prevent unbounded growth until Task 4 lands
}

void flushScreenPrims() {
    // reserved for future crisp HUD/debug-text pass
}

} // namespace DebugRenderer
