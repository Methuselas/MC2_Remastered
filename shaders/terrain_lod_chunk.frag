in vec3 v_worldPos;
out vec4 fragColor;

void main() {
    // Debug: elevation-mapped color.
    // z range roughly -200..3000 for MC2 maps; normalize to [0,1].
    float t = clamp((v_worldPos.z + 200.0) / 3200.0, 0.0, 1.0);
    fragColor = vec4(t * 0.2 + 0.1, t * 0.6 + 0.2, t * 0.1 + 0.1, 1.0);
}
