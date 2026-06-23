// BT2018-BOX-DECAL-1 — screen-space AABB decal volume, fragment stage.
//
// Clean-room implementation of the standard published screen-space box-decal
// technique. Authored from the algorithm for MC2 (forward+MRT, reversed-Z,
// ZERO_TO_ONE); NOT derived from any third-party shader source.
//
// Steps: reconstruct the scene surface WORLD position from the depth COPY (sampling
// the copy, never the live bound depth attachment), discard if outside the world
// AABB, normal-reject steep faces, project a procedural decal pattern down the box
// up-axis, and alpha-composite into scene COLOR0 only (COLOR1 normal / COLOR2
// objectId are excluded from the draw-buffer list by the caller, so they're untouched).
//
// v1 ships a PROCEDURAL pattern (no decal texture, no gameplay producers yet) — the
// goal is to prove the projection + drape under reversed-Z. Texture + crater/footprint
// wiring + surface-class masking are explicit follow-up slices.
// No #version here — makeProgram() prefixes "#version 430\n".

uniform sampler2D u_sceneDepthTex;    // DEPTH COPY (feedback-safe), unit 0
uniform sampler2D u_sceneNormalTex;   // GBuffer1 world normal (n*0.5+0.5), unit 1
uniform mat4  u_inverseViewProj;      // NDC -> world (reconstruct frame, Y-up); GL_FALSE
uniform vec3  u_boxCenter;
uniform vec3  u_boxHalf;
uniform vec3  u_decalUpAxis;          // world up in the reconstruct frame; v1 = (0,1,0)
uniform float u_normalRejectCos;      // discard if dot(N,up) < this; <= -1.5 disables reject
uniform float u_decalStrength;        // 0 = no-op
uniform vec2  u_screenSize;

out vec4 FragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_screenSize;

    float depth = texture(u_sceneDepthTex, uv).r;   // far=0, near=1, sky~0 (reversed-Z)
    if (depth <= 0.0001) discard;                    // sky / cleared

    // Reversed-Z + ZERO_TO_ONE: window depth IS ndc z (no *2-1 on z). Proven path
    // (matches ssao.frag): world = invViewProj * vec4(ndc.xy, depth, 1); persp divide.
    vec4 w = u_inverseViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 worldPos = w.xyz / w.w;

    // World AABB clip: |local| <= 1 inside the box.
    vec3 local = (worldPos - u_boxCenter) / u_boxHalf;
    if (any(greaterThan(abs(local), vec3(1.0)))) discard;

    vec3 up = normalize(u_decalUpAxis);
    if (u_normalRejectCos > -1.5) {
        vec3 N = normalize(texture(u_sceneNormalTex, uv).xyz * 2.0 - 1.0);  // world-space
        if (dot(N, up) < u_normalRejectCos) discard;                       // skip steep/wall faces
    }

    // 2D decal coordinate = box-local projected onto the plane orthogonal to up.
    // v1 supports a cardinal up axis only (axis-aligned box).
    vec3 a = abs(up);
    vec2 d = (a.y > 0.5) ? local.xz : ((a.z > 0.5) ? local.xy : local.yz);

    // Procedural rings + edge fade: warps visibly over terrain bumps => proves the
    // screen-space projection (a coplanar mesh decal could not drape like this).
    float r    = length(d);
    float band = fract(r * 4.0);
    float ring = smoothstep(0.45, 0.50, band) * smoothstep(0.95, 0.90, band);
    vec3  col  = mix(vec3(0.10, 0.65, 1.0), vec3(1.0, 0.80, 0.12), step(0.5, band));
    float edgeFade = 1.0 - smoothstep(0.75, 1.0, max(abs(d.x), abs(d.y)));
    float alpha = u_decalStrength * edgeFade * (0.30 + 0.70 * ring);
    if (alpha <= 0.001) discard;

    FragColor = vec4(col, alpha);
}
