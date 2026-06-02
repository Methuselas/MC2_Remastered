# MC2 Asset Viewer Stage 2 — Lit PBR Material Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Asset Viewer's **Materials** sidebar entry live — render a metallic/roughness PBR material on a tangent-correct UV sphere with an orbit camera and one rotatable directional light, fed by four manually-assignable texture slots (baseColor / normal / ORM / emissive) plus a "Load .fit material" button.

**Architecture:** Add a new `MaterialPreviewPBR : PreviewSurface` alongside `TexturePreview2D`. It owns an offscreen FBO, the orbit camera + light state, the four slot textures, and a `MaterialRenderBackend`. The app keeps depending only on the `PreviewSurface` seam. Rendering goes through a `MaterialRenderBackend` interface so the shader backend is swappable.

**Backend decision (Task 0 spike — DONE 2026-06-01): Backend B (viewer-local Cook-Torrance).** See "Spike outcome" below. The preview is therefore **labeled approximate** in the UI and README, per the spec.

**Tech Stack:** C++17, SDL2 + GLEW + OpenGL (core 3.3+), Dear ImGui, `stb_image` (`3rdparty/stb/stb_image.h`) for slot decode. Fixture-based smoke harness (`mc2_asset_viewer --smoke*`), no external test framework.

---

## Spike outcome (Task 0 — completed, do not re-run)

The spike (`.claude/spike_pbr_compile.py`, throwaway) attempted standalone bring-up of MC2's real shader with no Mission. Findings:

1. **RenderCore init is NOT a swamp.** MC2's real `static_prop.{vert,frag}` shader compiles standalone (engine-style `#include` splice + prefix `#version 430\n#define MC2_USE_VIEW_UNIFORMS 1`, validated clean by `glslangValidator` in desktop-GL semantic mode, exit 0). `RenderWorld::init()` is mission-free and `shader_builder.cpp` pulls no game/`stuff`/`mclib` headers.
2. **But MC2 has no standalone ORM material shader to be faithful to.** The "real" shader is `static_prop`, which: adds a *sun-only* Schlick specular lobe **on top of baked vertex lighting** (`v_argb.rgb`); does **no** tangent-space normal mapping (no tangent attribute, uses interpolated `v_normal`); does **no** ORM/normal texture sampling (`normalTex`/`metallicRoughnessTex` are `kMaterialTexAbsent`; roughness/metallic are scalar factors); reads albedo from a `sampler2DArray` layer index, not a free baseColor 2D texture; and is tightly coupled to the static-prop batcher's `Instances`/`PerType`/`Colors`/`LightsData` SSBO contract and a Stuff→GL space swap.

**Conclusion:** Driving the real shader would replicate the entire batcher buffer contract and *still* not show normal or ORM maps. Backend A cannot deliver the spec's "material ball." **Backend B** (a small self-contained Cook-Torrance shader the viewer compiles itself, over the four ORM slots with tangent-space normals) is the only path that delivers the deliverable. Backend B → **"approximate" label is required** (spec lines 48-52, 109-111).

`MaterialRenderBackend.h` is still defined as an interface so a future Backend A can slot in, but only `LocalPbrMaterialBackend` (Backend B) is implemented in this slice.

---

## File structure

All new files under `tools/asset_viewer/` unless noted. Each added `.cpp` is appended to `ASSET_VIEWER_SOURCES` in `tools/asset_viewer/CMakeLists.txt`.

| File | Responsibility |
|---|---|
| `SphereMesh.{h,cpp}` | Generate a UV sphere: interleaved position(3) / normal(3) / tangent(4, w=handedness) / uv(2). Owns its VBO/EBO/VAO. |
| `MaterialRenderBackend.h` | Pure-virtual interface: `init()`, `setMaterial(const MaterialSlotTextures&)`, `render(const RenderInputs&)`. |
| `LocalPbrMaterialBackend.{h,cpp}` | Backend B. Compiles a self-contained Cook-Torrance GLSL program; binds the 4 slot textures + camera + light + ambient; draws the sphere. No RenderCore. |
| `MaterialTextureLoader.{h,cpp}` | Slot-aware decode (`stb_image`) + GL upload. Owns per-slot internalformat: baseColor/emissive → `GL_SRGB8_ALPHA8`, normal/ORM → `GL_RGBA8`. Returns a GL texture id (not an `ImTextureID`). |
| `MaterialSlots.{h,cpp}` | ImGui UI for the 4 slots; each slot uses the stage-1 `FileBrowser` Win32 picker to assign a file; uploads via `MaterialTextureLoader`. |
| `MaterialPreviewPBR.{h,cpp}` | The `PreviewSurface`. Owns slot textures, orbit camera + light state, the `LocalPbrMaterialBackend`, and the offscreen FBO. `draw()` renders into the FBO (GL-state contained) and shows it via `ImGui::Image`. Renders the persistent "approximate" label. |
| `FitMaterialLoader.{h,cpp}` | Minimal read-only FIT parser: one `Material{}` block → slot paths + `shader`/`ormPacking`/`alphaMode`. Sequenced LAST. |

Modified files:
- `tools/asset_viewer/AssetTypeSidebar.{h,cpp}` — add `Materials` to the type enum and make it selectable + return the active type.
- `tools/asset_viewer/AssetViewerApp.{h,cpp}` — own a `MaterialPreviewPBR` and dispatch to it when the active type is `Materials`; add smoke entrypoints.
- `tools/asset_viewer/CMakeLists.txt` — add the new sources; add `3rdparty/stb` to includes.
- `tools/asset_viewer/README.md` — document Materials mode + the "approximate preview" note.

---

## Conventions used across tasks (read once)

- **GL texture id type:** raw `GLuint`. To show in ImGui: `ImGui::Image((ImTextureID)(intptr_t)glId, size)` (matches `UiEditorImageCache.cpp:355`).
- **Smoke harness pattern:** the exe parses `argv[1]` for a `--smoke*` flag and calls a static method on `AssetViewerApp` that sets up a headless SDL/GL context, runs assertions, prints `[smoke] PASS` / `[smoke] FAIL: <reason>`, and returns `0` / `1` (matches `main.cpp:17-20`, `AssetViewerApp.cpp:54-93`). New checks are added as new `--smoke-*` subcommands so each is runnable in isolation.
- **GL context version (review fix — BLOCKER 3):** the stage-1 code requests a GL **3.0** core context (`main.cpp:31-32`, `AssetViewerApp.cpp:65-66`), but the PBR shaders below use `#version 330 core`. Before any of these tasks, bump BOTH the app (`main.cpp`) and EVERY new `--smoke-*` SDL setup to request **3.3** core: `SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);` (major stays 3). A strict driver rejects `#version 330` on a literal 3.0 context. Do not copy the `MINOR_VERSION, 0` line verbatim when "reusing the runSmoke setup" — use 3.
- **Fixtures:** generated into `tests/fixtures/asset_viewer/` by `tests/fixtures/asset_viewer/make_fixture.py` (extended per task). PNG, written with a deterministic generator.
- **Color space:** baseColor/emissive textures are uploaded as `GL_SRGB8_ALPHA8` (GL linearizes on sample). normal/ORM are `GL_RGBA8` (linear). Lighting is computed in linear space; the fragment shader applies gamma encoding (`pow(color, 1/2.2)`) on output because the FBO color attachment is plain `GL_RGBA8`, not sRGB.

---

## Task 1: SphereMesh with tangents

**Files:**
- Create: `tools/asset_viewer/SphereMesh.h`
- Create: `tools/asset_viewer/SphereMesh.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt` (add `SphereMesh.cpp`)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` (add `--smoke-sphere`)
- Modify: `tools/asset_viewer/AssetViewerApp.cpp` (smoke body)

- [ ] **Step 1: Write the SphereMesh header**

```cpp
// tools/asset_viewer/SphereMesh.h
#pragma once
#include <vector>
#include <cstdint>

// Interleaved vertex: position(3), normal(3), tangent(4 = xyz + handedness w), uv(2) = 12 floats.
struct SphereVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;   // tangent.xyz + handedness (+1 / -1)
    float u, v;
};

class SphereMesh {
public:
    // Generates a UV sphere of `radius` with `stacks` latitude bands and
    // `slices` longitude segments. CPU buffers only; call uploadGL() once a
    // GL context is current to create VAO/VBO/EBO.
    void generate(float radius, int stacks, int slices);
    void uploadGL();          // creates VAO/VBO/EBO from the CPU buffers
    void draw() const;        // glBindVertexArray + glDrawElements(GL_TRIANGLES)
    void destroyGL();         // delete VAO/VBO/EBO

    const std::vector<SphereVertex>& vertices() const { return verts_; }
    const std::vector<uint32_t>&     indices()  const { return idx_;   }

private:
    std::vector<SphereVertex> verts_;
    std::vector<uint32_t>     idx_;
    unsigned vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
};
```

- [ ] **Step 2: Write the failing smoke check (geometry + tangent fixtures), wire `--smoke-sphere`**

Add to `tools/asset_viewer/AssetViewerApp.h` (public):

```cpp
    static int runSmokeSphere();   // validates SphereMesh geometry + tangent basis
```

Add to `tools/asset_viewer/main.cpp` smoke dispatch (next to the existing `--smoke` branch):

```cpp
    if (argc >= 2 && strcmp(argv[1], "--smoke-sphere") == 0)
        return AssetViewerApp::runSmokeSphere();
```

Add the implementation to `tools/asset_viewer/AssetViewerApp.cpp` (no GL context needed — CPU-only checks):

```cpp
#include "SphereMesh.h"
#include <cmath>
#include <cstdio>

int AssetViewerApp::runSmokeSphere() {
    SphereMesh m;
    m.generate(1.0f, 32, 64);
    const auto& v = m.vertices();
    const auto& idx = m.indices();
    if (v.empty() || idx.empty()) { printf("[smoke] FAIL: empty mesh\n"); return 1; }
    if (idx.size() % 3 != 0)      { printf("[smoke] FAIL: index count not triangulated\n"); return 1; }

    for (const auto& sv : v) {
        // position is on the unit sphere
        float pr = std::sqrt(sv.px*sv.px + sv.py*sv.py + sv.pz*sv.pz);
        if (std::fabs(pr - 1.0f) > 1e-3f) { printf("[smoke] FAIL: vertex off sphere (r=%f)\n", pr); return 1; }
        // normal is unit length and equals the position direction (unit sphere)
        float nl = std::sqrt(sv.nx*sv.nx + sv.ny*sv.ny + sv.nz*sv.nz);
        if (std::fabs(nl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit normal\n"); return 1; }
        // tangent is unit length, perpendicular to the normal, handedness is +/-1
        float tl = std::sqrt(sv.tx*sv.tx + sv.ty*sv.ty + sv.tz*sv.tz);
        if (std::fabs(tl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit tangent\n"); return 1; }
        float ndott = sv.nx*sv.tx + sv.ny*sv.ty + sv.nz*sv.tz;
        if (std::fabs(ndott) > 1e-2f) { printf("[smoke] FAIL: tangent not perpendicular to normal (%f)\n", ndott); return 1; }
        if (std::fabs(std::fabs(sv.tw) - 1.0f) > 1e-3f) { printf("[smoke] FAIL: handedness not +/-1\n"); return 1; }
    }
    printf("[smoke] PASS sphere verts=%zu tris=%zu\n", v.size(), idx.size()/3);
    return 0;
}
```

- [ ] **Step 3: Run the check to verify it fails**

Run (from the build dir, after configuring): `mc2_asset_viewer --smoke-sphere`
Expected: link/build failure (SphereMesh not implemented) — implement next.

- [ ] **Step 4: Implement SphereMesh.cpp**

```cpp
// tools/asset_viewer/SphereMesh.cpp
#include "SphereMesh.h"
#include <GL/glew.h>
#include <cmath>

void SphereMesh::generate(float radius, int stacks, int slices) {
    verts_.clear();
    idx_.clear();
    const float PI = 3.14159265358979323846f;
    for (int i = 0; i <= stacks; ++i) {
        float phi = PI * (float)i / (float)stacks;          // 0..PI (lat)
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= slices; ++j) {
            float theta = 2.0f * PI * (float)j / (float)slices;  // 0..2PI (lon)
            float st = std::sin(theta), ct = std::cos(theta);

            SphereVertex sv{};
            // unit normal/position
            float nx = sp * ct, ny = cp, nz = sp * st;
            sv.px = radius * nx; sv.py = radius * ny; sv.pz = radius * nz;
            sv.nx = nx; sv.ny = ny; sv.nz = nz;
            // uv: u from longitude, v from latitude
            sv.u = (float)j / (float)slices;
            sv.v = (float)i / (float)stacks;
            // tangent = d(position)/d(theta), normalized. dP/dtheta = (-sp*st, 0, sp*ct).
            float tx = -sp * st, ty = 0.0f, tz = sp * ct;
            float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
            if (tl < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; tl = 1.0f; }  // poles
            sv.tx = tx/tl; sv.ty = ty/tl; sv.tz = tz/tl;
            // handedness: sign of dot(cross(N,T), B) where B = dP/dphi.
            // For this parameterization right-handed TBN gives +1.
            sv.tw = 1.0f;
            verts_.push_back(sv);
        }
    }
    int cols = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = i * cols + j;
            uint32_t b = (i + 1) * cols + j;
            idx_.push_back(a);   idx_.push_back(b);   idx_.push_back(a + 1);
            idx_.push_back(a + 1); idx_.push_back(b); idx_.push_back(b + 1);
        }
    }
}

void SphereMesh::uploadGL() {
    indexCount_ = (int)idx_.size();
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(SphereVertex), verts_.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_.size()*sizeof(uint32_t), idx_.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(SphereVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)(10*sizeof(float)));
    glBindVertexArray(0);
}

void SphereMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void SphereMesh::destroyGL() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    ebo_ = vbo_ = vao_ = 0;
}
```

Add `SphereMesh.cpp` to `ASSET_VIEWER_SOURCES` in `tools/asset_viewer/CMakeLists.txt`.

- [ ] **Step 5: Run the check to verify it passes**

Run: `mc2_asset_viewer --smoke-sphere`
Expected: `[smoke] PASS sphere verts=... tris=...`

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/SphereMesh.h tools/asset_viewer/SphereMesh.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): UV sphere mesh with tangents + geometry smoke"
```

---

## Task 2: MaterialRenderBackend interface + LocalPbrMaterialBackend (Backend B)

**Files:**
- Create: `tools/asset_viewer/MaterialRenderBackend.h`
- Create: `tools/asset_viewer/LocalPbrMaterialBackend.h`
- Create: `tools/asset_viewer/LocalPbrMaterialBackend.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt` (add `LocalPbrMaterialBackend.cpp`)

- [ ] **Step 1: Write the backend interface**

```cpp
// tools/asset_viewer/MaterialRenderBackend.h
#pragma once
#include <cstdint>

class SphereMesh;

// The four PBR slot textures as raw GL texture ids (0 = unassigned).
struct MaterialSlotTextures {
    uint32_t baseColor = 0;   // sRGB-uploaded
    uint32_t normal    = 0;   // linear; 0 => treat as flat (0,0,1)
    uint32_t orm       = 0;   // linear; R=AO, G=Roughness, B=Metallic; 0 => AO=1, rough=0.5, metal=0
    uint32_t emissive  = 0;   // sRGB; 0 => no emission
};

struct RenderInputs {
    const SphereMesh* mesh = nullptr;
    float viewProj[16];       // column-major MVP-less: this is proj*view (model is identity)
    float cameraPosWorld[3];
    float lightDirWorld[3];   // direction the light travels (surface->light = -lightDir)
    float lightColor[3];
    float ambient[3];
    int   fboWidth = 0;
    int   fboHeight = 0;
};

class MaterialRenderBackend {
public:
    virtual ~MaterialRenderBackend() = default;
    virtual bool init() = 0;                                  // compile/link program; false on failure
    virtual void setMaterial(const MaterialSlotTextures& slots) = 0;
    virtual void render(const RenderInputs& in) = 0;          // assumes target FBO already bound
    virtual void shutdown() = 0;
    virtual const char* name() const = 0;                     // e.g. "LocalPBR (approximate)"
    virtual bool isApproximate() const = 0;                   // Backend B => true
};
```

- [ ] **Step 2: Write the Backend B header**

```cpp
// tools/asset_viewer/LocalPbrMaterialBackend.h
#pragma once
#include "MaterialRenderBackend.h"

class LocalPbrMaterialBackend : public MaterialRenderBackend {
public:
    bool init() override;
    void setMaterial(const MaterialSlotTextures& slots) override;
    void render(const RenderInputs& in) override;
    void shutdown() override;
    const char* name() const override { return "LocalPBR (approximate)"; }
    bool isApproximate() const override { return true; }

private:
    unsigned program_ = 0;
    MaterialSlotTextures slots_{};
    // cached uniform locations
    int locViewProj_ = -1, locCamPos_ = -1, locLightDir_ = -1, locLightColor_ = -1, locAmbient_ = -1;
    int locHasNormal_ = -1, locHasOrm_ = -1, locHasEmissive_ = -1;
    int locBaseColor_ = -1, locNormalTex_ = -1, locOrmTex_ = -1, locEmissiveTex_ = -1;
};
```

- [ ] **Step 3: Write the failing program-compile smoke (`--smoke-backend`)**

Add to `AssetViewerApp.h` (public): `static int runSmokeBackend();`
Add to `main.cpp`: `if (argc >= 2 && strcmp(argv[1], "--smoke-backend") == 0) return AssetViewerApp::runSmokeBackend();`
Add to `AssetViewerApp.cpp` (this one NEEDS a GL context — reuse the headless SDL/GL setup pattern from the existing `runSmoke`, then):

```cpp
#include "LocalPbrMaterialBackend.h"
// ... inside runSmokeBackend(), after a core-profile GL context is current and glewInit() succeeded:
    LocalPbrMaterialBackend b;
    bool ok = b.init();
    GLenum e = glGetError();
    if (!ok)            { printf("[smoke] FAIL: backend init/compile failed\n"); return 1; }
    if (e != GL_NO_ERROR){ printf("[smoke] FAIL: glGetError 0x%x after init\n", e); return 1; }
    b.shutdown();
    printf("[smoke] PASS backend=%s approximate=%d\n", b.name(), (int)b.isApproximate());
    return 0;
```

Run: `mc2_asset_viewer --smoke-backend` → Expected: build/link failure (backend not implemented).

- [ ] **Step 4: Implement LocalPbrMaterialBackend.cpp**

```cpp
// tools/asset_viewer/LocalPbrMaterialBackend.cpp
#include "LocalPbrMaterialBackend.h"
#include "SphereMesh.h"
#include <GL/glew.h>
#include <cstdio>

static const char* kVert = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec4 a_tangent;   // xyz + handedness
layout(location=3) in vec2 a_uv;
uniform mat4 u_viewProj;                 // model is identity
out vec3 v_worldPos;
out vec3 v_normal;
out vec3 v_tangent;
out vec3 v_bitangent;
out vec2 v_uv;
void main() {
    v_worldPos = a_pos;
    v_normal   = normalize(a_normal);
    v_tangent  = normalize(a_tangent.xyz);
    v_bitangent = normalize(cross(v_normal, v_tangent) * a_tangent.w);
    v_uv = a_uv;
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
}
)GLSL";

static const char* kFrag = R"GLSL(
#version 330 core
in vec3 v_worldPos;
in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_bitangent;
in vec2 v_uv;
out vec4 fragColor;

uniform vec3 u_cameraPos;
uniform vec3 u_lightDir;     // travel direction; surface->light L = -u_lightDir
uniform vec3 u_lightColor;
uniform vec3 u_ambient;

uniform sampler2D u_baseColor;   // sRGB texture -> linear on sample
uniform sampler2D u_normalTex;   // linear
uniform sampler2D u_ormTex;      // linear R=AO G=Rough B=Metal
uniform sampler2D u_emissiveTex; // sRGB

uniform int u_hasNormal;
uniform int u_hasOrm;
uniform int u_hasEmissive;

const float PI = 3.14159265359;

float D_GGX(float NdotH, float a) {
    float a2 = a*a;
    float d = (NdotH*NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
float G_SchlickGGX(float NdotV, float k) { return NdotV / (NdotV * (1.0 - k) + k); }
float G_Smith(float NdotV, float NdotL, float rough) {
    float k = (rough + 1.0); k = (k*k) / 8.0;
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}
vec3 F_Schlick(float cosT, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - cosT, 5.0); }

void main() {
    vec3 albedo = texture(u_baseColor, v_uv).rgb;   // linear (sRGB internalformat)

    float ao = 1.0, rough = 0.5, metal = 0.0;
    if (u_hasOrm != 0) {
        vec3 orm = texture(u_ormTex, v_uv).rgb;
        ao = orm.r; rough = clamp(orm.g, 0.04, 1.0); metal = orm.b;
    }

    vec3 N = normalize(v_normal);
    if (u_hasNormal != 0) {
        vec3 tn = texture(u_normalTex, v_uv).xyz * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(v_tangent), normalize(v_bitangent), N);
        N = normalize(TBN * tn);
    }

    vec3 V = normalize(u_cameraPos - v_worldPos);
    vec3 L = normalize(-u_lightDir);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);
    float D = D_GGX(NdotH, rough*rough);
    float G = G_Smith(NdotV, NdotL, rough);
    vec3  F = F_Schlick(VdotH, F0);
    vec3 spec = (D * G) * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metal);
    vec3 diffuse = kd * albedo / PI;

    vec3 color = (diffuse + spec) * u_lightColor * NdotL;
    color += u_ambient * albedo * ao;                  // cheap ambient term
    if (u_hasEmissive != 0) color += texture(u_emissiveTex, v_uv).rgb;

    color = color / (color + vec3(1.0));               // Reinhard tonemap
    color = pow(color, vec3(1.0/2.2));                 // gamma encode (FBO is RGBA8)
    fragColor = vec4(color, 1.0);
}
)GLSL";

static unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
               fprintf(stderr, "[LocalPBR] shader compile error: %s\n", log); glDeleteShader(s); return 0; }
    return s;
}

bool LocalPbrMaterialBackend::init() {
    unsigned vs = compile(GL_VERTEX_SHADER, kVert);
    unsigned fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs); glAttachShader(program_, fs);
    glLinkProgram(program_);
    int ok = 0; glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) { char log[2048]; glGetProgramInfoLog(program_, sizeof log, nullptr, log);
               fprintf(stderr, "[LocalPBR] link error: %s\n", log); return false; }

    locViewProj_   = glGetUniformLocation(program_, "u_viewProj");
    locCamPos_     = glGetUniformLocation(program_, "u_cameraPos");
    locLightDir_   = glGetUniformLocation(program_, "u_lightDir");
    locLightColor_ = glGetUniformLocation(program_, "u_lightColor");
    locAmbient_    = glGetUniformLocation(program_, "u_ambient");
    locHasNormal_  = glGetUniformLocation(program_, "u_hasNormal");
    locHasOrm_     = glGetUniformLocation(program_, "u_hasOrm");
    locHasEmissive_= glGetUniformLocation(program_, "u_hasEmissive");
    locBaseColor_  = glGetUniformLocation(program_, "u_baseColor");
    locNormalTex_  = glGetUniformLocation(program_, "u_normalTex");
    locOrmTex_     = glGetUniformLocation(program_, "u_ormTex");
    locEmissiveTex_= glGetUniformLocation(program_, "u_emissiveTex");
    return true;
}

void LocalPbrMaterialBackend::setMaterial(const MaterialSlotTextures& slots) { slots_ = slots; }

void LocalPbrMaterialBackend::render(const RenderInputs& in) {
    if (!program_ || !in.mesh) return;
    glUseProgram(program_);
    glUniformMatrix4fv(locViewProj_, 1, GL_FALSE, in.viewProj);
    glUniform3fv(locCamPos_, 1, in.cameraPosWorld);
    glUniform3fv(locLightDir_, 1, in.lightDirWorld);
    glUniform3fv(locLightColor_, 1, in.lightColor);
    glUniform3fv(locAmbient_, 1, in.ambient);

    auto bind = [&](int unit, uint32_t tex, int loc) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(loc, unit);
    };
    // baseColor must exist for a meaningful preview; if 0, bind 0 (samples black).
    bind(0, slots_.baseColor, locBaseColor_);
    bind(1, slots_.normal,    locNormalTex_);
    bind(2, slots_.orm,       locOrmTex_);
    bind(3, slots_.emissive,  locEmissiveTex_);
    glUniform1i(locHasNormal_,   slots_.normal   ? 1 : 0);
    glUniform1i(locHasOrm_,      slots_.orm      ? 1 : 0);
    glUniform1i(locHasEmissive_, slots_.emissive ? 1 : 0);

    in.mesh->draw();
    glUseProgram(0);
}

void LocalPbrMaterialBackend::shutdown() {
    if (program_) glDeleteProgram(program_);
    program_ = 0;
}
```

Add `LocalPbrMaterialBackend.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 5: Run the check to verify it passes**

Run: `mc2_asset_viewer --smoke-backend`
Expected: `[smoke] PASS backend=LocalPBR (approximate) approximate=1`

- [ ] **Step 6: Commit**

```bash
git add tools/asset_viewer/MaterialRenderBackend.h \
        tools/asset_viewer/LocalPbrMaterialBackend.h tools/asset_viewer/LocalPbrMaterialBackend.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): local Cook-Torrance PBR backend (Backend B) + compile smoke"
```

---

## Task 3: MaterialTextureLoader (slot-aware upload)

**Files:**
- Create: `tools/asset_viewer/MaterialTextureLoader.h`
- Create: `tools/asset_viewer/MaterialTextureLoader.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt` (add source + `3rdparty/stb` include)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` (`--smoke-texload`)
- Modify: `tests/fixtures/asset_viewer/make_fixture.py` (emit material fixtures)

- [ ] **Step 1: Write the header**

```cpp
// tools/asset_viewer/MaterialTextureLoader.h
#pragma once
#include <cstdint>
#include <string>

enum class MaterialSlotKind { BaseColor, Normal, Orm, Emissive };

// Decodes an image file (PNG/JPG/BMP/TGA via stb_image) and uploads it to a GL
// texture with a SLOT-AWARE internalformat:
//   BaseColor/Emissive -> GL_SRGB8_ALPHA8 (GL linearizes on sample)
//   Normal/Orm         -> GL_RGBA8        (linear)
// Returns the GL texture id, or 0 on failure (errorOut set). The caller owns
// the texture and must glDeleteTextures it.
uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut);
```

- [ ] **Step 2: Write the failing smoke (`--smoke-texload`)**

Extend `tests/fixtures/asset_viewer/make_fixture.py` to also write `mat_base.png` (a 2x2 sRGB-ish color) and `mat_orm.png` (a 2x2 linear gray) into `tests/fixtures/asset_viewer/`.

Add to `AssetViewerApp.h`: `static int runSmokeTexLoad(const char* fixtureDir);`
Add to `main.cpp`: `if (argc >= 2 && strcmp(argv[1], "--smoke-texload") == 0) return AssetViewerApp::runSmokeTexLoad(argc >= 3 ? argv[2] : ".");`
Add to `AssetViewerApp.cpp` (needs GL context — reuse headless setup; then):

```cpp
#include "MaterialTextureLoader.h"
// inside runSmokeTexLoad, with a GL context current:
    std::string base = std::string(fixtureDir) + "/mat_base.png";
    std::string orm  = std::string(fixtureDir) + "/mat_orm.png";
    std::string err;
    uint32_t tb = MaterialTextureLoader_Load(base, MaterialSlotKind::BaseColor, &err);
    if (!tb) { printf("[smoke] FAIL: baseColor load: %s\n", err.c_str()); return 1; }
    GLint fmt = 0;
    glBindTexture(GL_TEXTURE_2D, tb);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    if (fmt != GL_SRGB8_ALPHA8) { printf("[smoke] FAIL: baseColor not sRGB (0x%x)\n", fmt); return 1; }
    uint32_t to = MaterialTextureLoader_Load(orm, MaterialSlotKind::Orm, &err);
    if (!to) { printf("[smoke] FAIL: orm load: %s\n", err.c_str()); return 1; }
    glBindTexture(GL_TEXTURE_2D, to);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    if (fmt != GL_RGBA8) { printf("[smoke] FAIL: orm not linear RGBA8 (0x%x)\n", fmt); return 1; }
    glDeleteTextures(1, &tb); glDeleteTextures(1, &to);
    printf("[smoke] PASS texload sRGB/linear internalformats correct\n");
    return 0;
```

Run: `mc2_asset_viewer --smoke-texload tests/fixtures/asset_viewer` → Expected: build failure (loader missing).

- [ ] **Step 3: Implement MaterialTextureLoader.cpp**

```cpp
// tools/asset_viewer/MaterialTextureLoader.cpp
#include "MaterialTextureLoader.h"
#include <GL/glew.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"     // 3rdparty/stb on the include path

uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut) {
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);  // force RGBA
    if (!px) { if (errorOut) *errorOut = std::string("decode failed: ") + stbi_failure_reason(); return 0; }

    GLint internal = (kind == MaterialSlotKind::BaseColor || kind == MaterialSlotKind::Emissive)
                     ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(px);
    return tex;
}
```

In `tools/asset_viewer/CMakeLists.txt`: add `MaterialTextureLoader.cpp` to `ASSET_VIEWER_SOURCES`, and add `${CMAKE_SOURCE_DIR}/3rdparty/stb` (adjust relative root as the file already does for other includes) to `target_include_directories(mc2_asset_viewer ...)`.

- [ ] **Step 4: Run the check to verify it passes**

Run: `mc2_asset_viewer --smoke-texload tests/fixtures/asset_viewer`
Expected: `[smoke] PASS texload sRGB/linear internalformats correct`

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/MaterialTextureLoader.h tools/asset_viewer/MaterialTextureLoader.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp \
        tests/fixtures/asset_viewer/make_fixture.py tests/fixtures/asset_viewer/mat_base.png \
        tests/fixtures/asset_viewer/mat_orm.png
git commit -m "feat(asset-viewer): slot-aware PBR texture loader (sRGB base/emissive, linear normal/orm)"
```

---

## Task 4: MaterialPreviewPBR (FBO render + GL-state containment + approximate label)

**Files:**
- Create: `tools/asset_viewer/MaterialPreviewPBR.h`
- Create: `tools/asset_viewer/MaterialPreviewPBR.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// tools/asset_viewer/MaterialPreviewPBR.h
#pragma once
#include "PreviewSurface.h"
#include "MaterialRenderBackend.h"
#include "LocalPbrMaterialBackend.h"
#include "SphereMesh.h"
#include <memory>
#include <string>

class MaterialPreviewPBR : public PreviewSurface {
public:
    MaterialPreviewPBR();
    ~MaterialPreviewPBR() override;

    void setSource(const std::string& path) override;          // FIT path (Task 8); no-op for now
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Material (PBR)"; }

    // Slot assignment (called by MaterialSlots UI in Task 5). Takes ownership of the GL texture.
    void setSlotTexture(MaterialSlotKind kind, uint32_t glTex);

    // Camera/light controls exposed so the UI (Task 5) can drive them.
    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
    float* lightDir()   { return lightDir_; }

private:
    void ensureGL(int w, int h);     // lazy backend/mesh init + (re)create FBO on size change
    void destroyFbo();
    void buildViewProj(int w, int h, float out[16], float camPosOut[3]) const;

    std::unique_ptr<MaterialRenderBackend> backend_;
    SphereMesh mesh_;
    MaterialSlotTextures slots_{};
    bool glReady_ = false;
    bool backendOk_ = false;     // review fix MAJOR 6: backend_->init() succeeded
    bool fboComplete_ = false;   // review fix MAJOR 6: last FBO build was complete

    unsigned fbo_ = 0, colorTex_ = 0, depthRbo_ = 0;
    int fboW_ = 0, fboH_ = 0;

    float yaw_ = 0.6f, pitch_ = 0.3f, dist_ = 3.0f;
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };
    std::string fitPath_;
};
```

(`MaterialSlotKind` comes from `MaterialTextureLoader.h`; include it in the header.)

- [ ] **Step 2: Write the failing offscreen-render smoke (`--smoke-render`)**

Add to `AssetViewerApp.h`: `static int runSmokeRender(const char* fixtureDir);`
Add to `main.cpp`: `if (argc >= 2 && strcmp(argv[1], "--smoke-render") == 0) return AssetViewerApp::runSmokeRender(argc >= 3 ? argv[2] : ".");`

Implement in `AssetViewerApp.cpp` (GL context current). Per the spec's verification section, assert non-black center + clean `glGetError()` (no brittle pixel goldens):

```cpp
#include "MaterialPreviewPBR.h"
#include "MaterialTextureLoader.h"
// runSmokeRender body (after GL context + glewInit):
    const int W = 256, H = 256;
    // Build an FBO directly here mirroring MaterialPreviewPBR's, render a flat base material.
    MaterialPreviewPBR preview;
    std::string err;
    uint32_t base = MaterialTextureLoader_Load(std::string(fixtureDir) + "/mat_base.png",
                                               MaterialSlotKind::BaseColor, &err);
    if (!base) { printf("[smoke] FAIL: base load %s\n", err.c_str()); return 1; }
    preview.setSlotTexture(MaterialSlotKind::BaseColor, base);

    // Review fix MAJOR 5: do NOT call preview.draw() here — draw() ends with
    // ImGui::Image/TextColored and this smoke has no ImGui context/frame, so it
    // would dereference an uninitialized GImGui and crash. renderToPixels() is
    // ImGui-free: it lazily builds the FBO + renders + reads back.
    // The renderToPixels test hook (declared in the header below) returns false
    // if backend init or FBO completeness failed (review fix MAJOR 6), so a
    // failed shader compile is caught here rather than passing on a non-black clear.
    std::vector<uint8_t> rgba;
    if (!preview.renderToPixels(W, H, rgba)) { printf("[smoke] FAIL: renderToPixels (init/FBO)\n"); return 1; }
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) { printf("[smoke] FAIL: glGetError 0x%x\n", e); return 1; }
    // Review fix MAJOR 6: the FBO clears to (0.10,0.11,0.13) -> a broken shader
    // would still be non-black. Assert the sphere actually drew: the center
    // region must be meaningfully BRIGHTER than the corner (background) region.
    auto regionAvg = [&](int x0, int y0)->long {
        long s = 0; for (int y=y0; y<y0+16; ++y) for (int x=x0; x<x0+16; ++x) {
            const uint8_t* p = &rgba[(y*W + x)*4]; s += p[0]+p[1]+p[2]; } return s/256;
    };
    long center = regionAvg(W/2 - 8, H/2 - 8);
    long corner = regionAvg(2, 2);
    if (center == 0)            { printf("[smoke] FAIL: center all black\n"); return 1; }
    if (center <= corner + 24)  { printf("[smoke] FAIL: sphere not distinct from background (c=%ld bg=%ld)\n", center, corner); return 1; }
    printf("[smoke] PASS render: sphere distinct (c=%ld bg=%ld), glGetError clean\n", center, corner);
    return 0;
```

Add the test hook to the header in this step:

```cpp
    // Test hook: render the sphere with current slots into an internal FBO and read it back.
    bool renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut);
```

Run: `mc2_asset_viewer --smoke-render tests/fixtures/asset_viewer` → Expected: build failure (not implemented).

- [ ] **Step 3: Implement MaterialPreviewPBR.cpp**

```cpp
// tools/asset_viewer/MaterialPreviewPBR.cpp
#include "MaterialPreviewPBR.h"
#include "imgui.h"
#include <GL/glew.h>
#include <cmath>
#include <vector>

static void perspective(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i=0;i<16;i++) m[i]=0;
    m[0]=f/aspect; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.0f; m[14]=(2*zf*zn)/(zn-zf);
}
static void lookAt(const float eye[3], const float ctr[3], const float up[3], float m[16]) {
    float f[3]={ctr[0]-eye[0],ctr[1]-eye[1],ctr[2]-eye[2]};
    float fl=std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); f[0]/=fl;f[1]/=fl;f[2]/=fl;
    float s[3]={f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    float sl=std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); s[0]/=sl;s[1]/=sl;s[2]/=sl;
    float u[3]={s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0;m[7]=0;m[11]=0;m[15]=1;
}
static void mul4(const float a[16], const float b[16], float o[16]) {  // o = a*b (column-major)
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
    }
}

MaterialPreviewPBR::MaterialPreviewPBR() : backend_(std::make_unique<LocalPbrMaterialBackend>()) {}
MaterialPreviewPBR::~MaterialPreviewPBR() {
    if (slots_.baseColor) glDeleteTextures(1, &slots_.baseColor);
    if (slots_.normal)    glDeleteTextures(1, &slots_.normal);
    if (slots_.orm)       glDeleteTextures(1, &slots_.orm);
    if (slots_.emissive)  glDeleteTextures(1, &slots_.emissive);
    destroyFbo();
    if (glReady_) { mesh_.destroyGL(); backend_->shutdown(); }
}

void MaterialPreviewPBR::setSource(const std::string& path) { fitPath_ = path; /* Task 8 */ }

void MaterialPreviewPBR::setSlotTexture(MaterialSlotKind kind, uint32_t glTex) {
    uint32_t* dst = nullptr;
    switch (kind) {
      case MaterialSlotKind::BaseColor: dst = &slots_.baseColor; break;
      case MaterialSlotKind::Normal:    dst = &slots_.normal;    break;
      case MaterialSlotKind::Orm:       dst = &slots_.orm;       break;
      case MaterialSlotKind::Emissive:  dst = &slots_.emissive;  break;
    }
    if (dst) { if (*dst) glDeleteTextures(1, dst); *dst = glTex; }
}

void MaterialPreviewPBR::ensureGL(int w, int h) {
    if (!glReady_) {
        backendOk_ = backend_->init();                 // review fix MAJOR 6: capture result
        mesh_.generate(1.0f, 48, 96); mesh_.uploadGL();
        glReady_ = true;
    }
    if (w != fboW_ || h != fboH_ || !fbo_) {
        destroyFbo();
        fboW_ = w; fboH_ = h;
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glGenTextures(1, &colorTex_);
        glBindTexture(GL_TEXTURE_2D, colorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
        glGenRenderbuffers(1, &depthRbo_);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRbo_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo_);
        fboComplete_ = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        if (!fboComplete_) fprintf(stderr, "[MaterialPreviewPBR] FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void MaterialPreviewPBR::destroyFbo() {
    if (depthRbo_) glDeleteRenderbuffers(1, &depthRbo_);
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (fbo_)      glDeleteFramebuffers(1, &fbo_);
    fbo_ = colorTex_ = depthRbo_ = 0;
}

void MaterialPreviewPBR::buildViewProj(int w, int h, float out[16], float camPosOut[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_), cy = std::cos(yaw_), sy = std::sin(yaw_);
    float eye[3] = { dist_*cp*sy, dist_*sp, dist_*cp*cy };
    float ctr[3] = {0,0,0}, up[3] = {0,1,0};
    float view[16], proj[16];
    lookAt(eye, ctr, up, view);
    perspective(0.8f, (float)w/(float)h, 0.05f, 50.0f, proj);
    mul4(proj, view, out);
    camPosOut[0]=eye[0]; camPosOut[1]=eye[1]; camPosOut[2]=eye[2];
}

// Shared core: render into fbo_ with full GL-state containment.
static void renderContained(MaterialRenderBackend* backend, SphereMesh* mesh,
                            const MaterialSlotTextures& /*slots already set*/,
                            unsigned fbo, int w, int h,
                            const float viewProj[16], const float camPos[3], const float lightDir[3]) {
    // save state
    GLint prevFbo = 0, prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST), wasCull = glIsEnabled(GL_CULL_FACE), wasBlend = glIsEnabled(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    RenderInputs in{};
    in.mesh = mesh;
    for (int i=0;i<16;i++) in.viewProj[i] = viewProj[i];
    for (int i=0;i<3;i++) { in.cameraPosWorld[i]=camPos[i]; in.lightDirWorld[i]=lightDir[i]; }
    in.lightColor[0]=in.lightColor[1]=in.lightColor[2]=3.0f;
    in.ambient[0]=in.ambient[1]=in.ambient[2]=0.15f;
    in.fboWidth=w; in.fboHeight=h;
    backend->render(in);

    // restore state (don't leak into ImGui) — review fix MAJOR 4.
    // The backend bound textures on units 0..3 and left glUseProgram(0). ImGui's
    // GL3 backend re-binds its own program/blend/scissor each frame, but it
    // assumes the ACTIVE texture unit is 0 and does not unbind our 2D textures.
    // So explicitly: unbind units 3..0 and leave unit 0 active.
    for (int u = 3; u >= 0; --u) { glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, 0); }
    // (glActiveTexture loop ends on GL_TEXTURE0, the unit ImGui expects.)
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (!wasDepth) glDisable(GL_DEPTH_TEST); else glDepthFunc(GL_LESS);
    if (!wasCull)  glDisable(GL_CULL_FACE); else glCullFace(GL_BACK);
    if (wasBlend)  glEnable(GL_BLEND);
}

void MaterialPreviewPBR::draw(const ImVec2& availableSize) {
    int w = (int)availableSize.x, h = (int)availableSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) {   // review fix MAJOR 6: surface init failure instead of a misleading blank
        ImGui::TextColored(ImVec4(1.0f,0.4f,0.3f,1.0f),
            "Material preview unavailable (shader compile or FBO init failed). See stderr.");
        return;
    }
    backend_->setMaterial(slots_);
    float vp[16], cam[3];
    buildViewProj(w, h, vp, cam);
    renderContained(backend_.get(), &mesh_, slots_, fbo_, w, h, vp, cam, lightDir_);

    ImGui::Image((ImTextureID)(intptr_t)colorTex_, ImVec2((float)w, (float)h), ImVec2(0,1), ImVec2(1,0));
    if (backend_->isApproximate())
        ImGui::TextColored(ImVec4(1.0f,0.7f,0.2f,1.0f),
            "Preview mode: Local PBR approximation, not exact MC2 shader.");
}

bool MaterialPreviewPBR::renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut) {
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) return false;   // review fix MAJOR 6
    backend_->setMaterial(slots_);
    float vp[16], cam[3];
    buildViewProj(w, h, vp, cam);
    renderContained(backend_.get(), &mesh_, slots_, fbo_, w, h, vp, cam, lightDir_);
    rgbaOut.resize((size_t)w*h*4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
```

Add `MaterialPreviewPBR.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 4: Run the check to verify it passes**

Run: `mc2_asset_viewer --smoke-render tests/fixtures/asset_viewer`
Expected: `[smoke] PASS render non-black center, glGetError clean`

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/MaterialPreviewPBR.h tools/asset_viewer/MaterialPreviewPBR.cpp \
        tools/asset_viewer/CMakeLists.txt tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "feat(asset-viewer): MaterialPreviewPBR FBO surface w/ GL-state containment + approximate label"
```

---

## Task 5: MaterialSlots UI (manual slot assignment)

**Files:**
- Create: `tools/asset_viewer/MaterialSlots.h`
- Create: `tools/asset_viewer/MaterialSlots.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// tools/asset_viewer/MaterialSlots.h
#pragma once
#include "MaterialTextureLoader.h"
#include <string>

class MaterialPreviewPBR;

// Renders four slot rows (BaseColor / Normal / ORM / Emissive). Each row shows
// the assigned filename + a "Browse..." button that opens the Win32 picker
// (same path FileBrowser uses), loads via MaterialTextureLoader (slot-aware),
// and pushes the GL texture into the preview. Also renders light + camera controls.
class MaterialSlots {
public:
    void draw(MaterialPreviewPBR& preview);

private:
    void slotRow(const char* label, MaterialSlotKind kind, MaterialPreviewPBR& preview);
    std::string paths_[4];     // indexed by (int)MaterialSlotKind
    std::string errors_[4];
};
```

- [ ] **Step 2: Expose a public file picker (review fix — BLOCKER 1)**

The existing picker is `static bool PickTextureFileWin32(std::string& outPath)` — file-local in `tools/asset_viewer/FileBrowser.cpp` (two overloads: real Win32 at `:26`, non-Win32 stub at `:60`), NOT a member and NOT in the header. Do NOT call `PickTextureFileWin32()` directly and do NOT change its signature. Instead add a public wrapper that returns the path (or `""` on cancel), keeping the picker logic in one TU.

Add to `tools/asset_viewer/FileBrowser.h` (public section):

```cpp
    // Opens the native file picker; returns the chosen path, or "" if cancelled.
    static std::string PickFile();
```

Add to `tools/asset_viewer/FileBrowser.cpp` (after the existing `PickTextureFileWin32` definitions so it can call them):

```cpp
std::string FileBrowser::PickFile() {
    std::string out;
    return PickTextureFileWin32(out) ? out : std::string();
}
```

- [ ] **Step 3: Implement MaterialSlots.cpp**

Call `FileBrowser::PickFile()` (the wrapper from Step 2) — NOT `PickTextureFileWin32()`.

```cpp
// tools/asset_viewer/MaterialSlots.cpp
#include "MaterialSlots.h"
#include "MaterialPreviewPBR.h"
#include "FileBrowser.h"     // for FileBrowser::PickFile()
#include "imgui.h"

void MaterialSlots::slotRow(const char* label, MaterialSlotKind kind, MaterialPreviewPBR& preview) {
    int i = (int)kind;
    ImGui::PushID(label);
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(paths_[i].empty() ? "(none)" : paths_[i].c_str());
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string picked = FileBrowser::PickFile();
        if (!picked.empty()) {
            std::string err;
            uint32_t tex = MaterialTextureLoader_Load(picked, kind, &err);
            if (tex) { paths_[i] = picked; errors_[i].clear(); preview.setSlotTexture(kind, tex); }
            else     { errors_[i] = err; }
        }
    }
    if (!errors_[i].empty()) ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "  %s", errors_[i].c_str());
    ImGui::PopID();
}

void MaterialSlots::draw(MaterialPreviewPBR& preview) {
    ImGui::SeparatorText("Material slots");
    slotRow("Base Color", MaterialSlotKind::BaseColor, preview);
    slotRow("Normal",     MaterialSlotKind::Normal,    preview);
    slotRow("ORM",        MaterialSlotKind::Orm,       preview);
    slotRow("Emissive",   MaterialSlotKind::Emissive,  preview);

    ImGui::SeparatorText("View");
    ImGui::SliderFloat("Orbit yaw",   &preview.orbitYaw(),   -3.14159f, 3.14159f);
    ImGui::SliderFloat("Orbit pitch", &preview.orbitPitch(), -1.5f, 1.5f);
    ImGui::SliderFloat("Zoom",        &preview.zoom(),        1.2f, 8.0f);
    ImGui::SeparatorText("Light");
    ImGui::SliderFloat3("Light dir", preview.lightDir(), -1.0f, 1.0f);
}
```

Add `MaterialSlots.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 4: Build to verify it compiles (UI has no headless smoke)**

Run: build the target (e.g. `cmake --build <build-dir> --target mc2_asset_viewer`).
Expected: clean compile/link. (UI interaction is validated in Task 6 manual run.)

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/MaterialSlots.h tools/asset_viewer/MaterialSlots.cpp \
        tools/asset_viewer/FileBrowser.h tools/asset_viewer/FileBrowser.cpp \
        tools/asset_viewer/CMakeLists.txt
git commit -m "feat(asset-viewer): material slot pickers + camera/light controls UI"
```

---

## Task 6: Wire Materials mode into the app (MVP)

**Files:**
- Modify: `tools/asset_viewer/AssetTypeSidebar.h`
- Modify: `tools/asset_viewer/AssetTypeSidebar.cpp`
- Modify: `tools/asset_viewer/AssetViewerApp.h`
- Modify: `tools/asset_viewer/AssetViewerApp.cpp`
- Modify: `tools/asset_viewer/README.md`

- [ ] **Step 1: Make Materials selectable in the sidebar**

Edit `tools/asset_viewer/AssetTypeSidebar.h`:

```cpp
enum class AssetType { Textures, Materials };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return active_; }
private:
    AssetType active_ = AssetType::Textures;
};
```

Edit `tools/asset_viewer/AssetTypeSidebar.cpp` `draw()` so `Textures` and `Materials` are enabled `ImGui::Selectable`s that set `active_`, the rest stay disabled:

```cpp
void AssetTypeSidebar::draw() {
    if (ImGui::Selectable("Textures",  active_ == AssetType::Textures))  active_ = AssetType::Textures;
    if (ImGui::Selectable("Materials", active_ == AssetType::Materials)) active_ = AssetType::Materials;
    ImGui::BeginDisabled();
    ImGui::Selectable("Static Props", false);
    ImGui::Selectable("Mechs", false);
    ImGui::EndDisabled();
}
```

- [ ] **Step 2: Dispatch to MaterialPreviewPBR in the app**

Edit `tools/asset_viewer/AssetViewerApp.h`: add members + include:

```cpp
#include "MaterialPreviewPBR.h"
#include "MaterialSlots.h"
// ... private:
    MaterialPreviewPBR materialSurface_;
    MaterialSlots      materialSlots_;
```

**Review fix — BLOCKER 2:** the real `drawUi()` (`AssetViewerApp.cpp:23-50`) is a three-`BeginChild` layout (sidebar / browser / inspector). It never calls `surface_.draw()` directly — drawing happens inside `inspector_.draw(surface_)` → `TextureInspectorPanel.cpp:20` calls `surface.draw(ImGui::GetContentRegionAvail())`. There is no `previewSize` variable. Wire the Materials branch into the existing structure, dispatching inside the inspector child. Replace the browser child's selection handling and the inspector child body in `drawUi()` as follows:

```cpp
    // browser child (existing): only feed the texture surface in Textures mode.
    ImGui::BeginChild("browser", ImVec2(browserW, 0), true);
    browser_.draw();
    if (browser_.hasSelection()) {
        std::string sel = browser_.takeSelection();
        if (sidebar_.active() == AssetType::Textures) surface_.setSource(sel);
        // Materials mode: slots are assigned via MaterialSlots' own picker, not the browser.
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // inspector child (existing): dispatch on the active asset type.
    ImGui::BeginChild("inspector", ImVec2(0, 0), true);
    switch (sidebar_.active()) {
      case AssetType::Textures:
        inspector_.draw(surface_);                                  // calls surface_.draw(GetContentRegionAvail())
        break;
      case AssetType::Materials:
        materialSlots_.draw(materialSurface_);                      // slot pickers + light/camera
        materialSurface_.draw(ImGui::GetContentRegionAvail());      // lit sphere + approximate label
        break;
    }
    ImGui::EndChild();
```

- [ ] **Step 3: Document Materials mode + approximate note in README**

Append to `tools/asset_viewer/README.md`:

```markdown
## Materials (Stage 2)

Select **Materials** in the sidebar to preview a PBR material on a lit sphere.
Assign up to four slots via **Browse...**: Base Color (sRGB), Normal (linear),
ORM (linear; R=AO, G=Roughness, B=Metallic), Emissive (sRGB). Use the View and
Light controls to orbit, zoom, and rotate the directional light.

> **Preview mode: Local PBR approximation, not exact MC2 shader.** The viewer
> renders with a self-contained Cook-Torrance shader (Backend B). MC2 has no
> standalone ORM material shader to mirror, so this preview is approximate and
> must not be treated as pixel-exact to in-game rendering.
```

- [ ] **Step 4: Build + manual MVP run**

Run: build `mc2_asset_viewer`, launch it, select **Materials**, Browse a base-color PNG into the Base Color slot.
Expected: a lit sphere appears, orbit/zoom/light sliders respond, and the orange "Preview mode: Local PBR approximation…" line is visible.

- [ ] **Step 5: Commit**

```bash
git add tools/asset_viewer/AssetTypeSidebar.h tools/asset_viewer/AssetTypeSidebar.cpp \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/README.md
git commit -m "feat(asset-viewer): wire Materials mode -> lit PBR sphere (MVP shippable)"
```

---

## Task 7: Smoke — tangent validation + render regression

**Files:**
- Modify: `tests/fixtures/asset_viewer/make_fixture.py` (flat-blue normal + directional normal)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` (`--smoke-tangent`)

This task adds the spec's required tangent checks (flat-blue normal == no normal within tolerance; a directional normal perturbs shading as predicted; seam does not explode) using broad-tolerance image compares, not exact goldens.

- [ ] **Step 1: Generate normal-map fixtures**

Extend `tests/fixtures/asset_viewer/make_fixture.py` to write:
- `nrm_flat.png` — every pixel `(128,128,255,255)` (tangent-space +Z).
- `nrm_tilt_u.png` — every pixel `(192,128,255,255)` (tilted toward +U).

- [ ] **Step 2: Write the failing tangent smoke (`--smoke-tangent`)**

Add to `AssetViewerApp.h`: `static int runSmokeTangent(const char* fixtureDir);`
Add to `main.cpp`: `if (argc >= 2 && strcmp(argv[1], "--smoke-tangent") == 0) return AssetViewerApp::runSmokeTangent(argc >= 3 ? argv[2] : ".");`

Implement in `AssetViewerApp.cpp` (GL context current). Compares three renders of a fixed camera/light:

```cpp
// helper: mean abs per-channel diff over the whole image
static double meanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0; size_t n = std::min(a.size(), b.size());
    for (size_t i=0;i<n;i++) s += std::abs((int)a[i]-(int)b[i]);
    return n ? s/n : 1e9;
}
int AssetViewerApp::runSmokeTangent(const char* dir) {
    const int W=256,H=256;
    auto load=[&](const char* f, MaterialSlotKind k){ std::string e; return MaterialTextureLoader_Load(std::string(dir)+"/"+f,k,&e); };
    uint32_t base = load("mat_base.png", MaterialSlotKind::BaseColor);
    if (!base) { printf("[smoke] FAIL: base load\n"); return 1; }

    MaterialPreviewPBR p;
    // fixed deterministic view/light for comparability
    p.orbitYaw()=0.0f; p.orbitPitch()=0.0f; p.zoom()=3.0f;
    p.lightDir()[0]=-0.5f; p.lightDir()[1]=0.0f; p.lightDir()[2]=-0.5f;

    std::vector<uint8_t> noNormal, flatNormal, tiltNormal;
    p.setSlotTexture(MaterialSlotKind::BaseColor, base);
    p.renderToPixels(W,H,noNormal);

    uint32_t base2 = load("mat_base.png", MaterialSlotKind::BaseColor); p.setSlotTexture(MaterialSlotKind::BaseColor, base2);
    p.setSlotTexture(MaterialSlotKind::Normal, load("nrm_flat.png", MaterialSlotKind::Normal));
    p.renderToPixels(W,H,flatNormal);

    p.setSlotTexture(MaterialSlotKind::Normal, load("nrm_tilt_u.png", MaterialSlotKind::Normal));
    p.renderToPixels(W,H,tiltNormal);

    double dFlat = meanDiff(noNormal, flatNormal);     // expect SMALL
    double dTilt = meanDiff(noNormal, tiltNormal);     // expect LARGER than dFlat
    if (dFlat > 6.0) { printf("[smoke] FAIL: flat-blue normal differs from no-normal (mean=%.2f)\n", dFlat); return 1; }
    if (dTilt < dFlat + 2.0) { printf("[smoke] FAIL: tilted normal did not perturb shading (flat=%.2f tilt=%.2f)\n", dFlat, dTilt); return 1; }

    // seam check: no all-black vertical band (tangent discontinuity blow-up)
    // sample the column at u-wrap (x = W-1) center rows; ensure not fully black.
    long col=0; for (int y=H/4;y<3*H/4;y++){ const uint8_t* px=&flatNormal[(y*W+(W-1))*4]; col+=px[0]+px[1]+px[2]; }
    if (col==0) { printf("[smoke] FAIL: seam column all black (tangent discontinuity)\n"); return 1; }

    printf("[smoke] PASS tangent flat=%.2f tilt=%.2f seam-ok\n", dFlat, dTilt);
    return 0;
}
```

Run: `mc2_asset_viewer --smoke-tangent tests/fixtures/asset_viewer` → Expected: build failure until fixtures + method exist; then run.

- [ ] **Step 3: Run to verify it passes**

Run: `mc2_asset_viewer --smoke-tangent tests/fixtures/asset_viewer`
Expected: `[smoke] PASS tangent flat=... tilt=... seam-ok`

If `dFlat` exceeds tolerance, the sphere's tangent handedness (`SphereVertex.tw`) or the TBN bitangent sign in the shader is wrong — fix in `SphereMesh::generate` / the vertex shader before proceeding.

- [ ] **Step 4: Commit**

```bash
git add tests/fixtures/asset_viewer/make_fixture.py tests/fixtures/asset_viewer/nrm_flat.png \
        tests/fixtures/asset_viewer/nrm_tilt_u.png tools/asset_viewer/AssetViewerApp.h \
        tools/asset_viewer/AssetViewerApp.cpp tools/asset_viewer/main.cpp
git commit -m "test(asset-viewer): tangent validation smoke (flat==none, tilt perturbs, no seam blow-up)"
```

---

## Task 8: FitMaterialLoader (LAST — "Load .fit material")

**Files:**
- Create: `tools/asset_viewer/FitMaterialLoader.h`
- Create: `tools/asset_viewer/FitMaterialLoader.cpp`
- Modify: `tools/asset_viewer/CMakeLists.txt`
- Modify: `tools/asset_viewer/MaterialSlots.cpp` (add the "Load .fit material" button)
- Modify: `tests/fixtures/asset_viewer/` (add a tiny `sample.fit`)
- Modify: `tools/asset_viewer/AssetViewerApp.{h,cpp}` + `main.cpp` (`--smoke-fit`)

- [ ] **Step 1: Write the header**

```cpp
// tools/asset_viewer/FitMaterialLoader.h
#pragma once
#include <string>

// Minimal, read-only parser for ONE Material{} block inside a .fit file.
// NOT a general FIT parser; does not commit any format decision.
struct FitMaterial {
    std::string baseColor;
    std::string normal;
    std::string orm;
    std::string emissive;
    std::string shader;       // informational
    std::string ormPacking;   // e.g. "RAO_GRough_BMetal"
    std::string alphaMode;    // informational
    bool found = false;
};

// Parses the first Material{} block. Paths are returned as-written (caller
// resolves relative to the .fit's directory). Returns found=false if none.
FitMaterial FitMaterialLoader_Parse(const std::string& fitPath, std::string* errorOut);
```

- [ ] **Step 2: Add the failing parser smoke + fixture**

Create `tests/fixtures/asset_viewer/sample.fit` containing exactly:

```
Material {
    baseColor = "mat_base.png"
    normal    = "nrm_flat.png"
    orm       = "mat_orm.png"
    emissive  = ""
    shader    = "pbr.orm"
    ormPacking = "RAO_GRough_BMetal"
    alphaMode  = "opaque"
}
```

Add to `AssetViewerApp.h`: `static int runSmokeFit(const char* fixtureDir);`
Add to `main.cpp`: `if (argc >= 2 && strcmp(argv[1], "--smoke-fit") == 0) return AssetViewerApp::runSmokeFit(argc >= 3 ? argv[2] : ".");`
Implement in `AssetViewerApp.cpp` (no GL needed):

```cpp
#include "FitMaterialLoader.h"
int AssetViewerApp::runSmokeFit(const char* dir) {
    std::string err;
    FitMaterial m = FitMaterialLoader_Parse(std::string(dir) + "/sample.fit", &err);
    if (!m.found)                       { printf("[smoke] FAIL: no Material block (%s)\n", err.c_str()); return 1; }
    if (m.baseColor != "mat_base.png")  { printf("[smoke] FAIL: baseColor='%s'\n", m.baseColor.c_str()); return 1; }
    if (m.normal    != "nrm_flat.png")  { printf("[smoke] FAIL: normal='%s'\n", m.normal.c_str()); return 1; }
    if (m.orm       != "mat_orm.png")   { printf("[smoke] FAIL: orm='%s'\n", m.orm.c_str()); return 1; }
    if (m.ormPacking != "RAO_GRough_BMetal") { printf("[smoke] FAIL: ormPacking='%s'\n", m.ormPacking.c_str()); return 1; }
    printf("[smoke] PASS fit parse base/normal/orm/packing\n");
    return 0;
}
```

Run: `mc2_asset_viewer --smoke-fit tests/fixtures/asset_viewer` → Expected: build failure until implemented.

- [ ] **Step 3: Implement FitMaterialLoader.cpp**

```cpp
// tools/asset_viewer/FitMaterialLoader.cpp
#include "FitMaterialLoader.h"
#include <fstream>
#include <sstream>
#include <cctype>

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}
static std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

FitMaterial FitMaterialLoader_Parse(const std::string& fitPath, std::string* errorOut) {
    FitMaterial m;
    std::ifstream f(fitPath);
    if (!f) { if (errorOut) *errorOut = "cannot open " + fitPath; return m; }
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    size_t mp = text.find("Material");
    if (mp == std::string::npos) { if (errorOut) *errorOut = "no Material keyword"; return m; }
    size_t ob = text.find('{', mp);
    size_t cb = (ob == std::string::npos) ? std::string::npos : text.find('}', ob);
    if (ob == std::string::npos || cb == std::string::npos) { if (errorOut) *errorOut = "no { } block"; return m; }

    std::string body = text.substr(ob + 1, cb - ob - 1);
    std::istringstream bs(body);
    std::string line;
    auto assign = [&](const std::string& key, const std::string& val) {
        if      (key == "baseColor")  m.baseColor  = unquote(val);
        else if (key == "normal")     m.normal     = unquote(val);
        else if (key == "orm")        m.orm        = unquote(val);
        else if (key == "emissive")   m.emissive   = unquote(val);
        else if (key == "shader")     m.shader     = unquote(val);
        else if (key == "ormPacking") m.ormPacking = unquote(val);
        else if (key == "alphaMode")  m.alphaMode  = unquote(val);
    };
    while (std::getline(bs, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        assign(trim(line.substr(0, eq)), line.substr(eq + 1));
    }
    m.found = true;
    return m;
}
```

Add `FitMaterialLoader.cpp` to `ASSET_VIEWER_SOURCES`.

- [ ] **Step 4: Run the parser smoke to verify it passes**

Run: `mc2_asset_viewer --smoke-fit tests/fixtures/asset_viewer`
Expected: `[smoke] PASS fit parse base/normal/orm/packing`

- [ ] **Step 5: Wire the "Load .fit material" button into the slots UI**

In `tools/asset_viewer/MaterialSlots.cpp`, at the top of `draw()`, add the button. It opens the picker, parses the FIT, resolves each non-empty path relative to the .fit's directory, loads via `MaterialTextureLoader`, and pushes into the preview:

```cpp
#include "FitMaterialLoader.h"
#include <filesystem>
// ... inside MaterialSlots::draw(), before "Material slots":
    if (ImGui::Button("Load .fit material")) {
        std::string fit = FileBrowser::PickFile();   // picker; user selects a .fit (review fix BLOCKER 1)
        if (!fit.empty()) {
            std::string err;
            FitMaterial fm = FitMaterialLoader_Parse(fit, &err);
            if (fm.found) {
                std::filesystem::path base = std::filesystem::path(fit).parent_path();
                auto loadInto = [&](const std::string& rel, MaterialSlotKind k, int idx) {
                    if (rel.empty()) return;
                    std::string full = (base / rel).string();
                    std::string e; uint32_t t = MaterialTextureLoader_Load(full, k, &e);
                    if (t) { paths_[idx] = full; errors_[idx].clear(); preview.setSlotTexture(k, t); }
                    else   { errors_[idx] = e; }
                };
                loadInto(fm.baseColor, MaterialSlotKind::BaseColor, 0);
                loadInto(fm.normal,    MaterialSlotKind::Normal,    1);
                loadInto(fm.orm,       MaterialSlotKind::Orm,       2);
                loadInto(fm.emissive,  MaterialSlotKind::Emissive,  3);
            }
        }
    }
```

- [ ] **Step 6: Build + manual check, then commit**

Run: build `mc2_asset_viewer`, launch, Materials → "Load .fit material", pick `tests/fixtures/asset_viewer/sample.fit`.
Expected: the four slots populate and the sphere updates.

```bash
git add tools/asset_viewer/FitMaterialLoader.h tools/asset_viewer/FitMaterialLoader.cpp \
        tools/asset_viewer/MaterialSlots.cpp tools/asset_viewer/CMakeLists.txt \
        tools/asset_viewer/AssetViewerApp.h tools/asset_viewer/AssetViewerApp.cpp \
        tools/asset_viewer/main.cpp tests/fixtures/asset_viewer/sample.fit
git commit -m "feat(asset-viewer): minimal FIT material loader + Load .fit button"
```

---

## Final verification (run all smokes)

- [ ] Run each, expect `[smoke] PASS`:

```bash
mc2_asset_viewer --smoke-sphere
mc2_asset_viewer --smoke-backend
mc2_asset_viewer --smoke-texload tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-render  tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-tangent tests/fixtures/asset_viewer
mc2_asset_viewer --smoke-fit     tests/fixtures/asset_viewer
mc2_asset_viewer --smoke         tests/fixtures/asset_viewer   # existing stage-1 texture smoke still passes
```

- [ ] Manual MVP: launch, Materials mode, assign a base color, see the lit sphere + approximate label, orbit/zoom/light respond, "Load .fit material" populates slots.

- [ ] Delete the throwaway spike artifacts: `.claude/spike_pbr_compile.py`, `.claude/spike_spliced_vert.glsl`, `.claude/spike_spliced_frag.glsl`.

---

## Self-review against spec

- **Spike (spec Task 0):** done; outcome recorded; Backend B chosen with evidence; approximate label required → implemented in `MaterialPreviewPBR::draw` (Task 4) + README (Task 6). ✓
- **UV sphere with tangents (spec line 24, 74):** Task 1. ✓
- **Slot assignment baseColor/normal/orm/emissive (spec line 25):** Tasks 3, 5. ✓
- **Load .fit material button + minimal FIT parser, sequenced LAST (spec line 25, 73, 201):** Task 8. ✓
- **Orbit camera + zoom + one rotatable directional light + ambient (spec line 26):** Tasks 4, 5. ✓
- **ORM packing R=AO,G=Rough,B=Metal (spec line 27, 91):** shader in Task 2 + ORM smoke. ✓
- **`MaterialRenderBackend` interface, impl hidden behind it (spec line 67):** Task 2. ✓
- **`MaterialPreviewPBR : PreviewSurface`, FBO→ImGui::Image (spec line 70):** Task 4. ✓
- **Slot-aware texture upload, NOT blind `UiEditorImageCache_Get` (spec lines 71, 94-111):** Task 3 (own decode+upload via stb, per-slot internalformat). ✓
- **FBO / GL-state ownership (spec lines 113-123):** Task 4 `renderContained` saves/restores FBO+viewport+depth/cull/blend, recreates FBO on size change, checks completeness, deletes all GL resources in dtor. ✓
- **Tangent validation: flat-blue==no-normal, directional perturbs, no seam explosion (spec lines 126-135):** Task 7. ✓
- **Smoke: non-black center + clean glGetError, broad-tolerance flat-normal, no brittle goldens (spec lines 137-147):** Tasks 4, 7. ✓
- **UI: Materials selectable + slot pickers + light control + lit sphere (spec lines 149-154):** Tasks 5, 6. ✓
- **Reuses FileBrowser/Win32 picker + PreviewSurface seam (spec lines 156-160):** Tasks 5, 6. ✓
- **Deferred items (IBL, KTX2/BC7, multi-material, general FIT) (spec lines 162-169):** not implemented — correct. ✓
- **Credit @Methuselas on commits touching his code (spec lines 206-209):** Task 3 reuses the decode concept but writes its own loader; if any commit ends up touching `UiEditorImageCache`, add the `Co-authored-by`/credit line. Note carried here. ⚠ (no current task edits his files)

Deviation from spec note: spec's preferred Task-3 implementation said "reuse the existing decode code where possible." This plan decodes with `stb_image` directly rather than threading through `UiEditorImageCache`'s private decode path, because that path uploads with a fixed display format and does not expose raw pixels for slot-aware re-upload. This satisfies the spec's hard requirement (own the GL upload + per-slot internalformat; do not blindly use `UiEditorImageCache_Get`). `stb_image.h` is already vendored at `3rdparty/stb`.
