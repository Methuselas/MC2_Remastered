// Phase 5: LOD-band debug visualization.
// u_lodStep ∈ {1,2,4,5,10,20} corresponding to LOD levels 0-5.
// Fine (green) -> coarse (dark red). Elevation modulates brightness.
// Phase 6: skirts are darkened (50%) for debug visibility when u_skirtDepth > 0.
// Phase 7.5: u_forceColor=1 enables neon palette — unmistakable proof chunk renderer is active.

in vec3 v_worldPos;
uniform int   u_lodStep;
uniform float u_skirtDepth;  // Phase 6: >0 when drawing a skirt strip
uniform int   u_forceColor;  // Phase 7.5: 1 = neon debug palette; 0 = normal LOD bands
out vec4 fragColor;

void main() {
    // Phase 7.5: neon palette overrides all other logic when u_forceColor=1.
    // Colors are deliberately extreme so any terrain coverage is unmistakable.
    if (u_forceColor != 0) {
        vec3 fc;
        if      (u_lodStep == 1)  fc = vec3(0.0,  1.0,  0.0);   // LOD0 neon green
        else if (u_lodStep == 2)  fc = vec3(1.0,  1.0,  0.0);   // LOD1 yellow
        else if (u_lodStep == 4)  fc = vec3(1.0,  0.0,  1.0);   // LOD2 magenta
        else if (u_lodStep == 5)  fc = vec3(0.0,  1.0,  1.0);   // LOD3 cyan
        else if (u_lodStep == 10) fc = vec3(1.0,  0.0,  0.0);   // LOD4 red
        else                      fc = vec3(1.0,  1.0,  1.0);   // LOD5 white
        if (u_skirtDepth > 0.0)   fc = vec3(0.0,  0.0,  0.5);   // skirts dark blue
        fragColor = vec4(fc, 1.0);
        return;
    }

    vec3 lodColor;
    if      (u_lodStep == 1)  lodColor = vec3(0.0,  0.85, 0.15);  // LOD0 bright green
    else if (u_lodStep == 2)  lodColor = vec3(0.45, 0.85, 0.0);   // LOD1 yellow-green
    else if (u_lodStep == 4)  lodColor = vec3(0.85, 0.75, 0.0);   // LOD2 yellow
    else if (u_lodStep == 5)  lodColor = vec3(0.9,  0.45, 0.0);   // LOD3 orange
    else if (u_lodStep == 10) lodColor = vec3(0.9,  0.15, 0.0);   // LOD4 red
    else                      lodColor = vec3(0.6,  0.0,  0.1);   // LOD5 dark red

    // Subtle elevation modulation: normalize height to [0.6, 1.0].
    float t = clamp((v_worldPos.z + 200.0) / 3200.0, 0.0, 1.0);
    float bright = 0.6 + 0.4 * t;

    // Phase 6: darken skirt pixels 50% for debug visibility.
    if (u_skirtDepth > 0.0)
        fragColor = vec4(lodColor * bright * 0.5, 1.0);
    else
        fragColor = vec4(lodColor * bright, 1.0);
}
