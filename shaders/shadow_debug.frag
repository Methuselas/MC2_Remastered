//#version 420 (version provided by prefix)

// Item 1 P5: the dynamic shadow map becomes a GL_TEXTURE_2D_ARRAY under CSM.
// A sampler2D cannot read an array, so compile an array-sampler + layer-select
// variant when MC2_SHADOW_CSM is defined. Static-map debug (mode 0) still uses
// the 2D path; the C++ side picks which program/path applies.
#ifdef MC2_SHADOW_CSM
uniform sampler2DArray shadowDebugArray;
uniform int shadowDebugLayer;
uniform int shadowDebugUseArray;   // 1 = sample the CSM array layer; 0 = the 2D map
#endif
uniform sampler2D shadowDebugMap;

in vec2 TexCoord;
out vec4 FragColor;

void main()
{
    float d;
#ifdef MC2_SHADOW_CSM
    if (shadowDebugUseArray == 1)
        d = texture(shadowDebugArray, vec3(TexCoord, float(shadowDebugLayer))).r;
    else
        d = texture(shadowDebugMap, TexCoord).r;
#else
    d = texture(shadowDebugMap, TexCoord).r;
#endif

    // Magenta = depth 1.0 (cleared, nothing written)
    if (d >= 0.999) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    // Red = depth 0.0 (near plane clipping)
    if (d <= 0.001) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Grayscale ramp for normal depth values
    float v = pow(d, 0.5);
    FragColor = vec4(v, v, v, 1.0);
}
