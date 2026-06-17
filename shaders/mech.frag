// shaders/mech.frag — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".

// render_contract.hglsl uses PREC macro for portability; the includer must
// define it. Same convention as static_prop.frag (memory: PREC defaults to
// highp in this engine).
#define PREC highp

#include <include/render_contract.hglsl>

// MECH-SPECULAR-V1: camera position for view direction. Included only on the
// ViewUniforms variant (MC2_USE_VIEW_UNIFORMS injected by gos_mech_batcher.cpp
// when MC2_MECH_VIEWUNIFORMS=1). Without this variant u_cameraWorldPos is
// unavailable and specular cannot be computed; the default/objectid variants
// are byte-identical (all 6 specular uniforms are inert at strength 0).
#ifdef MC2_USE_VIEW_UNIFORMS
#include <include/view_uniforms.hglsl>
#endif
#include <include/pbr_common.hglsl>

// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   GBuffer2:       rc_gbuffer2_objectIdU32  // M2.5 (#ifdef MC2_OBJECT_ID_BUFFER)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true

in vec2 v_uv;
in vec4 v_litColor;
in vec4 v_highlightColor;
in vec4 v_fogRGB;  // .rgb = engine fog color, .a = per-actor haze factor (B2)
in vec3 v_normal;
// MECH-SPECULAR-V1: world position + sun direction (GL space) from mech.vert.
// Declared inside the MC2_USE_VIEW_UNIFORMS guard to match vert declarations;
// only the viewuniforms shader variant defines these varyings.
#ifdef MC2_USE_VIEW_UNIFORMS
in vec3       v_worldPos;
flat in vec3  v_mechSunDirGL;
flat in int   v_mechSunFound;
#endif

uniform sampler2D u_tex;
// Slice C2: PBR surface-detail material (Metal061B).
// u_pbrNormalTex bound to unit 1, u_pbrOrmTex to unit 2.
// Both sampled only when u_standardLitEnabled != 0 (MC2_STANDARD_LIT_V1=1).
uniform sampler2D u_pbrNormalTex;   // unit 1: NormalGL (RGB8, linear)
uniform sampler2D u_pbrOrmTex;      // unit 2: packed ORM (R=AO G=Rough B=Metal, linear)
uniform int   u_standardLitEnabled; // 0=Blinn-Phong passthrough, 1=GGX StandardLit
uniform float u_pbrTileScale;       // UV tile scale for detail material (default 4.0)
uniform int u_materialFlags;  // bit 0: ALPHA_TEST
// Slice B2: u_fogValue retained for backward compat / parity with
// static_prop convention but no longer drives the mix — per-actor
// haze is in v_fogRGB.a now. The CPU side stops uploading the
// uniform (location -1 = silent no-op for glUniform1f) but keeping
// the declaration leaves room for a global haze override later.
uniform float u_fogValue;
// Slice A debug: 0=normal, 1=solid magenta, 2=texture only, 3=light only,
// 4=normal-as-color. Wired through MC2_MECH_FRAG_DEBUG env var on the C++
// side via a uniform write at flush time.
uniform int u_debugMode;
// MECH-AMBIENT-1: gated hemisphere ambient fill strength. 0.0 = OFF (no-op,
// byte-identical to legacy). The C++ side uploads 0.0 when MC2_MECH_AMBIENT_V1
// is off, so the term below vanishes. Sky/ground hemisphere colors are fixed
// conservative constants (no new uniforms/material data).
uniform float u_mechAmbientV1Strength;
// MECH-SPECULAR-V1: conservative Blinn specular sheen, gated by
// u_mechSpecularV1Strength. 0.0 = OFF (byte-identical). Only effective when
// MC2_USE_VIEW_UNIFORMS is defined (viewuniforms shader variant); the remaining
// variants have no camera position so strength is always uploaded as 0 by the
// C++ side. Glass/cockpit heuristic uses dark-pixel (luminance + maxChannel)
// classification — no hue/blue detection. Roughness parameters set Blinn
// exponent via shininess = 2/a^2 - 2 (energy-conserving-ish). Additive white
// highlight (not albedo-tinted) — reads as a metal glint, conservative.
#ifdef MC2_USE_VIEW_UNIFORMS
uniform float u_mechSpecularV1Strength;   // 0 = OFF; C++ uploads 0 when gate off
uniform float u_mechMetalRoughness;       // base surface roughness (default 0.85)
uniform float u_mechGlassRoughness;       // glass/cockpit roughness (default 0.25)
uniform float u_mechGlassLumaThresh;      // glass if luma < this (default 0.12)
uniform float u_mechGlassMaxChanThresh;   // AND max(rgb) < this (default 0.18)
uniform int   u_mechSpecDebugMask;        // 1 = visualize cockpit mask in green/grey
#endif

layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M2.5: per-pixel mech ObjectID. Emitted to GL_COLOR_ATTACHMENT2
// (R32_UINT; M1.5 substrate). `flat in` matches mech.vert's
// `flat out uint v_objectIdRaw`. Alpha-tested fragments that
// discard() at line 56 skip this write naturally -- the attachment-2
// pixel retains the clear value (0 = Handle::invalid()), correctly
// classified as background under lookupAtPixel.
flat in uint v_objectIdRaw;
layout(location=2) out uint v_objectId;
#endif

const int ALPHA_TEST_BIT = 1;

void main() {
    // textureLod(.., 0.0) instead of texture(): AMD RX 7900 XTX
    // strict-fails auto-LOD sampling on mech paint-scheme textures even
    // with MIN_FILTER=GL_LINEAR — hardware appears to access mip 1+
    // (which is undefined / empty) and returns black. Constant UV (zero
    // derivatives) clamps LOD to base level and works; varying UV
    // doesn't. textureLod forces level 0 explicitly. Diagnosed
    // 2026-05-08 via debug-mode 7 vs 8 contrast.
    // Static_prop.frag escapes this trap because its textures have
    // a complete mip pyramid by the time it samples; mech textures
    // load with the paint-scheme cycle that doesn't always leave all
    // mip levels populated when the GPU mech batcher's flush runs.
    vec4 tex_color = textureLod(u_tex, v_uv, 0.0);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    vec4 c = tex_color * v_litColor;
    c.rgb += v_highlightColor.rgb * v_highlightColor.a;
    // MECH-AMBIENT-1 (gated): conservative hemisphere ambient FILL. Lifts
    // shadowed/under-lit surfaces for readability without touching the lit
    // model, PBR, materials, or team-baked albedo. Keyed on the world normal's
    // up-component (worldMC2 y = up). Multiplied by albedo so it tints with the
    // mech's own colour. u_mechAmbientV1Strength == 0.0 -> exact no-op
    // (byte-identical default path). Added before fog so it hazes consistently.
    {
        vec3  Nw    = normalize(v_normal);
        float upAmt = 0.5 + 0.5 * Nw.y;                       // 0 = down, 1 = up
        vec3  hemi  = mix(vec3(0.20, 0.18, 0.16),             // ground (warm/dark)
                          vec3(0.55, 0.60, 0.70), upAmt);     // sky (cool)
        c.rgb += tex_color.rgb * hemi * u_mechAmbientV1Strength;
    }
    // MECH-SPECULAR-V1 (gated): conservative Blinn specular metal sheen + optional
    // glass/cockpit heuristic (dark-pixel classification). Added AFTER the ambient
    // term so the specular contribution is not swallowed by ambient, and BEFORE
    // the fog mix so specular fogs correctly with the rest of the surface. Placed
    // BEFORE the debug-mode if-chain so explicit debug modes 1-9 still overwrite c.
    //
    // Byte-identical when gate off: u_mechSpecularV1Strength == 0.0 short-circuits
    // the entire block; all 6 uniforms are inert. Only effective on the
    // MC2_USE_VIEW_UNIFORMS variant (u_cameraWorldPos available); other variants
    // have no ViewUniformsBlock and the C++ side always uploads strength 0.
    //
    // Space invariant: v_normal, v_mechSunDirGL, and u_cameraWorldPos are all
    // GL world space. v_mechSunDirGL was Stuff->GL-swapped in mech.vert alongside
    // the position/normal swap. No in-frag space conversion required or performed.
#ifdef MC2_USE_VIEW_UNIFORMS
    if (u_mechSpecularV1Strength > 0.0 && v_mechSunFound != 0) {
        // Glass/cockpit heuristic: classify DARK pixels only (no hue/blue detection).
        // A pixel is "glass" if both luminance AND max channel are below threshold.
        float luma    = dot(tex_color.rgb, vec3(0.299, 0.587, 0.114));
        float maxChan = max(max(tex_color.r, tex_color.g), tex_color.b);
        bool  isGlass = (luma < u_mechGlassLumaThresh) && (maxChan < u_mechGlassMaxChanThresh);

        if (u_mechSpecDebugMask != 0) {
            // Cockpit-mask visualization: green = glass-classified, grey = metal.
            // Placed inside the strength > 0 block so it has no effect when the
            // gate is off. Debug modes 1-9 (below) will still override this if
            // u_debugMode != 0 — the debug-mode chain runs after this block.
            c = vec4(isGlass ? vec3(0.0, 1.0, 0.0) : vec3(0.15), 1.0);
        } else {
            // Blinn specular. N, V, L all in GL world space.
            // roughness -> Blinn power: shininess = 2/a^2 - 2 (Phong<->GGX mapping).
            // Clamped to [1, 2048] so extreme roughness gives a broad diffuse-like
            // lobe rather than a degenerate or negative exponent.
            float rough     = isGlass ? u_mechGlassRoughness : u_mechMetalRoughness;
            float specMul   = isGlass ? 1.6 : 1.0;  // glass cockpit slightly hotter (modest)
            vec3  N = normalize(v_normal);                           // GL space (swapped in vert)
            vec3  V = normalize(u_cameraWorldPos.xyz - v_worldPos);  // GL space
            // v_mechSunDirGL is the NEGATED surface->sun direction (lighting.hglsl
            // convention: light_dir.xyz = -(surface->sun)); negate to recover L.
            vec3  L = normalize(-v_mechSunDirGL);                    // surface->sun, GL space
            vec3  H = normalize(L + V);
            float NdotL     = max(dot(N, L), 0.0);
            float NdotH     = max(dot(N, H), 0.0);
            float a         = max(rough, 0.04);
            float shininess = clamp(2.0 / (a * a) - 2.0, 1.0, 2048.0);
            // NdotL gate avoids backface specular (physically correct: no highlight
            // when surface faces away from the light).
            float spec = pow(NdotH, shininess) * NdotL;
            // Additive white highlight — not albedo-tinted — reads as a metal glint.
            c.rgb += spec * u_mechSpecularV1Strength * specMul;
        }
    }
#endif  // MC2_USE_VIEW_UNIFORMS

    // GBuffer normal; updated to PBR detail normal when StandardLit active.
    vec3 N_gbuf = normalize(v_normal);

#ifdef MC2_USE_VIEW_UNIFORMS
    // Slice C2/C3/D: StandardLit GGX PBR surface-detail layer.
    // Gated by u_standardLitEnabled (MC2_STANDARD_LIT_V1=1) and sun presence.
    // Replaces the Blinn-Phong c.rgb with Cook-Torrance GGX result.
    if (u_standardLitEnabled != 0 && v_mechSunFound != 0) {
        vec2 pbrUV  = v_uv * u_pbrTileScale;
        vec3 orm    = texture(u_pbrOrmTex, pbrUV).rgb;
        float ao        = orm.r;
        float roughness = orm.g;
        float metallic  = orm.b;

        // Derivative TBN normal map (Slice C3). Falls back to vertex N on
        // degenerate screen-space derivatives (silhouette pixels etc.).
        vec3 N_pbr = applyPbrNormal(N_gbuf, v_worldPos, v_uv,
                                    u_pbrNormalTex, u_pbrTileScale, 1.0);
        N_gbuf = N_pbr;

        // v_mechSunDirGL = -(surface->sun); negate to get L = surface->sun.
        vec3 V = normalize(u_cameraWorldPos.xyz - v_worldPos);
        vec3 L = normalize(-v_mechSunDirGL);

        // sRGB decode before PBR lighting (gamma-correct path).
        vec3 albedo = pow(tex_color.rgb, vec3(2.2));

        // Approximate light/ambient colors for v0 (no sun-color uniform yet).
        // Tuned for typical MC2 daylight; tunable via MC2_PBR_TILE_SCALE env.
        vec3 lightColor   = vec3(2.0, 1.9, 1.7);
        vec3 ambientColor = vec3(0.4, 0.45, 0.5);

        StandardLitInput si;
        si.albedo       = albedo;
        si.N            = N_pbr;
        si.V            = V;
        si.L            = L;
        si.lightColor   = lightColor;
        si.ambientColor = ambientColor;
        si.roughness    = roughness;
        si.metallic     = metallic;
        si.ao           = ao;

        vec3 pbrLit = StandardLit(si);
        pbrLit += v_highlightColor.rgb * v_highlightColor.a;
        c = vec4(pbrLit, tex_color.a);

        // PBR debug modes (overridden by standard modes 1-9 below if also set).
        if      (u_debugMode == 10) c = vec4(roughness, roughness, roughness, 1.0);
        else if (u_debugMode == 11) c = vec4(metallic,  metallic,  metallic,  1.0);
        else if (u_debugMode == 12) c = vec4(N_pbr * 0.5 + 0.5, 1.0);
    }
#endif  // MC2_USE_VIEW_UNIFORMS

    // Slice B2: per-actor haze. v_fogRGB.a=0 → clear, =1 → fully fogged.
    c.rgb  = mix(c.rgb, v_fogRGB.rgb, v_fogRGB.a);

    // Debug overrides (MC2_MECH_FRAG_DEBUG=N).
    if      (u_debugMode == 1) c = vec4(1.0, 0.0, 1.0, 1.0);                    // solid magenta
    else if (u_debugMode == 2) c = vec4(tex_color.rgb, 1.0);                    // texture only
    else if (u_debugMode == 3) c = vec4(v_litColor.rgb, 1.0);                   // light only
    else if (u_debugMode == 4) c = vec4(normalize(v_normal) * 0.5 + 0.5, 1.0);  // normal-as-color
    else if (u_debugMode == 5) c = vec4(fract(v_uv.x), fract(v_uv.y), 0.0, 1.0); // UV visualizer
    else if (u_debugMode == 6) c = vec4(tex_color.aaa, 1.0);                    // texture alpha as grey
    else if (u_debugMode == 7) c = vec4(texture(u_tex, vec2(0.5, 0.5)).rgb, 1.0); // sample at hardcoded UV
    else if (u_debugMode == 8) c = vec4(textureLod(u_tex, v_uv, 0.0).rgb, 1.0);  // explicit LOD 0 sample
    else if (u_debugMode == 9) c = vec4(v_litColor.rgb / max(textureLod(u_tex, v_uv, 0.0).rgb, vec3(0.001)), 1.0); // lighting / texture (texture-inversion overlay)

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(N_gbuf);
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-pixel RenderObjectHandle.raw(). Debug-mode pixels
    // (u_debugMode 1..9 at lines 65-73) DO NOT discard; they still emit
    // a valid handle, which is the correct substrate behavior (a
    // lookupAtPixel on a debug-color pixel returns the actor's handle).
    v_objectId = v_objectIdRaw;
#endif
}
