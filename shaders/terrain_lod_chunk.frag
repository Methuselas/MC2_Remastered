// Phase 5: LOD-band debug visualization.
// u_lodStep ∈ {1,2,4,5,10,20} corresponding to LOD levels 0-5.
// Fine (green) -> coarse (dark red). Elevation modulates brightness.
// Phase 6: skirts are darkened (50%) for debug visibility when u_skirtDepth > 0.
// Phase 7.5: u_forceColor=1 enables neon palette — unmistakable proof chunk renderer is active.

in vec3 v_worldPos;
uniform int   u_lodStep;
uniform float u_skirtDepth;  // Phase 6: >0 when drawing a skirt strip
uniform int   u_forceColor;  // Phase 7.5: 1 = neon debug palette; 0 = colormap

// Phase 10 (Step 1a): production base color from the merged colormap atlas
// (tex1 in legacy gos_terrain.frag). Same atlas-UV reconstruction as the
// legacy useAtlasColormap path: atlas-absolute UV from world position.
uniform sampler2D u_colormap;
uniform float u_atlasTopLeftX;            // = Terrain::mapTopLeft3d.x
uniform float u_atlasTopLeftY;            // = Terrain::mapTopLeft3d.y
uniform float u_atlasOneOverWorldUnits;   // = Terrain::oneOverWorldUnitsMapSide
uniform vec4  terrainLightDir;            // Phase 10 Step 1b: sun dir (same uniform as legacy)

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 GBuffer1;   // shadow-handled flat-up (terrain MRT composite)

void main() {
    // Phase 7.5 debug: neon LOD-band palette when u_forceColor=1 (launch_lod_*color.bat).
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
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Production: sample the colormap atlas. Atlas UV in [0,1] across the map.
    // Skirt verts share the edge surface vertex's worldPos.xy, so they sample
    // the same color as the adjacent surface -> seamless production skirts (no
    // debug darken). (Step 1b will add normals-from-height lighting + GBuffer1.)
    vec2 uv;
    uv.x = (v_worldPos.x - u_atlasTopLeftX) * u_atlasOneOverWorldUnits;
    uv.y = (u_atlasTopLeftY - v_worldPos.y) * u_atlasOneOverWorldUnits;
    vec3 base = texture(u_colormap, uv).rgb;

    // Step 1b: geometric normal from world-pos screen derivatives (faceted per
    // triangle; gives relief lighting without sampling the height SSBO in the
    // frag). Terrain world up = +Z (elevation); flip to keep N up-facing.
    vec3 N = normalize(cross(dFdx(v_worldPos), dFdy(v_worldPos)));
    if (N.z < 0.0) N = -N;
    float NdotL       = dot(N, terrainLightDir.xyz);
    float diffuse     = clamp(NdotL, 0.02, 1.0);
    float normalLight = mix(0.35, 1.20, diffuse);   // same band as legacy gos_terrain.frag:771

    fragColor = vec4(base * normalLight, 1.0);
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);           // rc_gbuffer1_shadowHandled_flatUp
}
