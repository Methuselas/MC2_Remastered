// EDGE-FOG-4: cloud bank at the map boundary.
//
// XY boundary position uses ray-height-plane intersection — camera-distance-independent.
// This eliminates the "creeping in" artifact: the fog boundary stays fixed in world
// space as the camera scrolls, because we never read the depth for XY.
//
// Handles void pixels (rawDepth < 0.0001) by projecting the camera ray to the fog
// height plane. This extends the fog PAST the map boundary into the OOB void area.
//
// Height fade: pixels with actual world Z above the cloud bank top (u_fogHeight) get
// no fog. Geometry Z from depth; void pixels treated as being at u_fogHeight.
//
// Blend pattern: GL_SRC_ALPHA blend, outputs (fogColor, alpha). Only reads depthTex;
// sceneColorTex_ is never sampled — no read/write feedback loop (same as fog_oob).
//
// Gate:  MC2_EDGE_FOG      (default ON, "0" to disable)
// Tune:  MC2_EDGE_FOG_START   — world units inside boundary where fog begins (default 50)
//        MC2_EDGE_FOG_HEIGHT  — cloud bank top in world Z                   (default 50)
//        MC2_EDGE_FOG_MAX     — peak opacity 0..1                          (default 0.92)

in vec2 TexCoord;
layout(location = 0) out vec4 outFog;

uniform sampler2D depthTex;
uniform mat4      invViewProj;
uniform vec3      u_fogColor;
uniform float     u_halfExtent;
uniform float     u_fogStart;
uniform float     u_fogHeight;
uniform float     u_fogMax;
uniform float     u_waterElevation;  // sea-level world Z — skip fog at/below water surface

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    vec2 ndc = TexCoord * 2.0 - 1.0;

    // Camera ray in world space: from near plane (depth=1) to far plane (depth=0).
    // Computed unconditionally — used for ray-plane XY regardless of pixel type.
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 wNear = pNear.xyz / pNear.w;
    vec3 wFar  = pFar.xyz / pFar.w;

    // Actual geometry world Z for height fade.
    // Void pixels (no geometry): treat as being at the fog height plane.
    float geoZ = u_fogHeight;
    if (rawDepth >= 0.0001) {
        vec4 wp = invViewProj * vec4(ndc, rawDepth, 1.0);
        geoZ = wp.z / wp.w;
    }

    // Water surface sits at sea level — don't let the cloud bank overwrite it.
    // A 2 WU margin covers wave displacement so the edge doesn't flicker.
    if (geoZ <= u_waterElevation + 2.0) { outFog = vec4(0.0); return; }

    // Suppress fog for pixels above the cloud bank top.
    // smoothstep(a, b, x) with a > b → 1 at x <= b, 0 at x >= a.
    float heightFade = smoothstep(u_fogHeight + 20.0, u_fogHeight, geoZ);
    if (heightFade <= 0.0) { outFog = vec4(0.0); return; }

    // Intersect camera ray with the fog height plane for XY.
    // Key property: this result is independent of rawDepth (no depth-fudge contamination),
    // so the fog boundary does not shift as the camera scrolls.
    float dz = wFar.z - wNear.z;
    if (dz >= -0.001) { outFog = vec4(0.0); return; }  // looking up/horizontal: no fog
    float t = (u_fogHeight - wNear.z) / dz;
    if (t < 0.0 || t > 1.0) { outFog = vec4(0.0); return; }
    vec2 planeXY = wNear.xy + t * (wFar.xy - wNear.xy);

    // Signed distance from map boundary (positive = inside, negative = outside).
    float distFromEdge = u_halfExtent - max(abs(planeXY.x), abs(planeXY.y));

    // Cloud bank fill:
    //   inside boundary → ramp from 0 (at u_fogStart inside) to 1 (at edge)
    //   outside boundary → solid 1 (inside cloud bank floor, no fade-out)
    float innerRamp   = smoothstep(u_fogStart, 0.0, distFromEdge);
    float outsideFill = step(0.0, -distFromEdge);

    float fogFactor = clamp(max(innerRamp, outsideFill) * heightFade * u_fogMax, 0.0, 1.0);
    outFog = vec4(u_fogColor, fogFactor);
}
