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

    // MATERIAL-M0: roughness default PINNED to 1.0 to match the authoritative
    // producer (MaterialGpu record default, gos_static_prop_batcher.cpp
    // m.roughnessFactor) and the static_prop.frag fallback. All three sites
    // must agree. See docs/material-m0-contract.md.
    float ao = 1.0, rough = 1.0, metal = 0.0;
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
