//#version 400 (version provided by material prefix)

layout(triangles, equal_spacing, ccw) in;

in vec4 tcs_Color[];
in vec2 tcs_Texcoord[];
in float tcs_TerrainType[];
in vec3 tcs_WorldPos[];
in vec3 tcs_WorldNorm[];

out vec4 Color;
out vec2 Texcoord;
out float TerrainType;
out vec3 WorldNorm;
out vec3 WorldPos;
out float UndisplacedDepth;
flat out uint RecordIdx;  // matches gos_terrain_thin.vert; legacy chain has no thin-record
                          // context, so emit a constant 0u.  Frag's cement-atlas branch
                          // is gated on useCementAtlas != 0 which the legacy bridge never
                          // sets, so the SSBO read is dead on the legacy path.

uniform vec4 tessDisplace;      // x=phongAlpha, y=displaceScale
uniform vec4 tessDebug;         // x debug mode; -2 = screen-space projection probe
uniform mat4 terrainMVP;        // axisSwap * worldToClip (clip-space transform)
uniform vec4 terrainViewport;   // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;               // projection_ : screen pixels -> NDC

// Textures for displacement sampling (shared with fragment shader)
uniform sampler2D tex1;         // colormap (for material classification)
uniform sampler2D matNormal0;   // rock normal+disp
uniform sampler2D matNormal1;   // grass normal+disp
uniform sampler2D matNormal2;   // dirt normal+disp
uniform sampler2D matNormal3;   // concrete normal+disp
uniform vec4 detailNormalTiling; // .x = base tiling multiplier

// F1 Stage A-pre parity probe SSBO (spec §5.1).
// Per codex P1: guard SSBO declaration behind the same macro as the probe
// block, so default-build is byte-identical to pre-F1 (no SSBO binding,
// no dead-uniform-decl noise).
//
// Counter layout (uint32 each, offsets 0-7):
//   [0] = max_delta_comparable (bit-cast float, atomicMax)
//   [1] = count_compared
//   [2] = count_behind_old_only
//   [3] = count_behind_new_only
//   [4] = count_nonfinite_old_only (hazard: in-front but NaN/Inf)
//   [5] = count_behind_both
//   [6] = count_nonfinite_both
//   [7] = count_nonfinite_new_only
// Cleared at mission start; accumulated across run.
//
// Matrix transport (offsets 8-23, floats):
//   u_worldToClipGL: 16 floats in row-major order (same convention as
//   terrainMVP -- C++ stores rows, GPU reads cols, transpose is correct).
//   Written from C++ via glBufferSubData at offset 32 bytes (8 * sizeof(uint)).
//   AMD driver does not reliably propagate std140 UBO bindings or
//   glUniformMatrix4fv to tessellation stages; SSBO reads are reliable
//   (proven: atomicAdd to this same SSBO accumulates correctly per frame).
#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
layout(std430, binding = 23) buffer DebugSSBO {
    uint  debugSSBO_counters[8];
    float debugSSBO_matrix[16];  // CPU M[i*4+j] = Stuff(row i, col j); GL_FALSE semantics apply
};
// Read the probe matrix from SSBO as a mat4.
// F1 Task 7d fix: CPU writes M[i*4+j] = Stuff(row i, col j) (row-major repackage of
// column-major Stuff). Legacy glUniformMatrix4fv(loc, 1, GL_FALSE, M) reads 4 consecutive
// floats as one column, giving GLSL terrainMVP = transpose(Stuff). This SSBO reconstruct
// must do the same: read 4 consecutive floats per column so m = transpose(Stuff) = terrainMVP.
// Prior strided read produced m = Stuff (untransposed), inflating w by ~3000x.
// See observations doc for full layout trace.
mat4 ssbo_readWorldToClipGL() {
    // F1 Task 7d fix: read 4 consecutive floats per column, matching
    // glUniformMatrix4fv(loc, 1, GL_FALSE, M) interpretation in legacy
    // upload. C++ stored M[i*4+j] = matrix(row i, col j) (row-major
    // repackage of column-major Stuff). GL_FALSE on that buffer treats
    // 4 consecutive floats as one column, giving GLSL mat4 = transpose
    // of the original. SSBO reconstruct must do the same to match
    // legacy `terrainMVP` semantics. See observations doc for the
    // full layout trace.
    vec4 c0 = vec4(debugSSBO_matrix[0],  debugSSBO_matrix[1],  debugSSBO_matrix[2],  debugSSBO_matrix[3]);
    vec4 c1 = vec4(debugSSBO_matrix[4],  debugSSBO_matrix[5],  debugSSBO_matrix[6],  debugSSBO_matrix[7]);
    vec4 c2 = vec4(debugSSBO_matrix[8],  debugSSBO_matrix[9],  debugSSBO_matrix[10], debugSSBO_matrix[11]);
    vec4 c3 = vec4(debugSSBO_matrix[12], debugSSBO_matrix[13], debugSSBO_matrix[14], debugSSBO_matrix[15]);
    return mat4(c0, c1, c2, c3);
}
#endif // MC2_UNIFIED_PROJECTION_PARITY_PROBE

// Task 7c: diagnostic SSBO at binding 24 -- GPU matrix readback + sample vertex.
// Layout: 16 floats matrix + 4*4 floats sample data = 32 floats = 128 bytes.
#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
layout(std430, binding = 24) buffer DiagSSBO {
    float diagMatrix[16];          // TES-visible u_worldToClipGL, row-major
    float diagSampleWorld[4];      // worldPos used for sample vertex
    float diagSampleNewClip[4];    // newClip = ssbo_readWorldToClipGL() * worldPos
    float diagSampleLegacyClip[4]; // clip    = terrainMVP * worldPos (legacy)
    float diagSampleLegacyGlPos[4];// legacyGlPosition.xyzw
};
#endif // MC2_UNIFIED_PROJECTION_PARITY_PROBE

#include <include/terrain_common.hglsl>
#include <include/terrain_depth_bias.hglsl>  // single-source TERRAIN/WATER_DEPTH_FUDGE

void main()
{
    vec3 bary = gl_TessCoord;
    RecordIdx = 0u;  // legacy chain has no thin-record context; set BEFORE early-outs.

    if (tessDebug.x < -2.5) {
        Color = vec4(1.0);
        Texcoord = vec2(0.0);
        TerrainType = 0.0;
        WorldNorm = vec3(0.0, 0.0, 1.0);
        WorldPos = vec3(0.0);
        UndisplacedDepth = 0.0;
        vec2 p = bary.x * vec2(-0.85, -0.85)
               + bary.y * vec2( 0.85, -0.85)
               + bary.z * vec2( 0.00,  0.85);
        gl_Position = vec4(p, 0.0, 1.0);
        return;
    }

    // Barycentric interpolation of all attributes
    vec3 worldPos = bary.x * tcs_WorldPos[0]
                  + bary.y * tcs_WorldPos[1]
                  + bary.z * tcs_WorldPos[2];

    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    Color = bary.x * tcs_Color[0]
          + bary.y * tcs_Color[1]
          + bary.z * tcs_Color[2];

    Texcoord = bary.x * tcs_Texcoord[0]
             + bary.y * tcs_Texcoord[1]
             + bary.z * tcs_Texcoord[2];

    TerrainType = bary.x * tcs_TerrainType[0]
                + bary.y * tcs_TerrainType[1]
                + bary.z * tcs_TerrainType[2];

    if (tessDebug.x < -1.5) {
        WorldNorm = worldNorm;
        WorldPos = worldPos;
        UndisplacedDepth = 0.0;
        gl_Position = bary.x * gl_in[0].gl_Position
                    + bary.y * gl_in[1].gl_Position
                    + bary.z * gl_in[2].gl_Position;
        return;
    }

    // Match overlay depth against the same world-space terrain surface that TES displaces.
    vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                             + bary.y * tcs_WorldPos[1]
                             + bary.z * tcs_WorldPos[2];
    vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
    // glClipControl(ZERO_TO_ONE) makes NDC z native [0, 1]; no remap needed.
    UndisplacedDepth = uclip.z / uclip.w;

    // --- Phong tessellation smoothing ---
    float alpha = tessDisplace.x;  // phongAlpha
    if (alpha > 0.0) {
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, alpha);
    }

    // --- Texture-based displacement along normal (dirt only) ---
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, Texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);
        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = Texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }

    WorldNorm = worldNorm;
    WorldPos = worldPos;

    // --- Projection of DISPLACED position (visual rendering) ---
    // Begin computeLegacyGlPosition equivalent -- EXACT existing block:
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Match legacy CPU emit's TERRAIN_DEPTH_FUDGE=0.002 (mclib/quad.cpp:1707)
    // so decals/GpuStaticProps/water-on-terrain at coincident depth win the
    // GL_LEQUAL tie. Precedent: gos_terrain_water_fast.vert:350.
    // Doubled from 0.001->0.002 after glClipControl(ZERO_TO_ONE) adoption
    // (commit 4c8f9a4) -- the old fudge was tuned under [-1,1]->[0,1] remap
    // where the visible NDC range was halved; under native [0,1] z-fighting
    // headroom near shoreline shrank and the staircase regressed.
    screen.z = clip.z * rhw + TERRAIN_DEPTH_FUDGE;  // single-sourced; see terrain_depth_bias.hglsl
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    vec4 legacyGlPosition = vec4(ndc.xyz * absW, absW);
    // End computeLegacyGlPosition equivalent.

#ifdef MC2_UNIFIED_PROJECTION_PARITY_PROBE
    vec4 newClip = ssbo_readWorldToClipGL() * vec4(worldPos, 1.0);

    const float epsilon = 1e-4;
    bool oldBehind = (legacyGlPosition.w <= epsilon);
    bool newBehind = (newClip.w <= epsilon);

    vec3 oldNDC = oldBehind ? vec3(0.0) : (legacyGlPosition.xyz / legacyGlPosition.w);
    vec3 newNDC = newBehind ? vec3(0.0) : (newClip.xyz       / newClip.w);

    // Hazard = in-front AND has NaN/Inf NDC. Isolates math hazard from
    // behind-camera classification (spec gemini #4 fix).
    bool oldHazard = !oldBehind && (any(isnan(oldNDC)) || any(isinf(oldNDC)));
    bool newHazard = !newBehind && (any(isnan(newNDC)) || any(isinf(newNDC)));
    bool comparable = !oldBehind && !newBehind && !oldHazard && !newHazard;

    if (comparable) {
        float d = max(abs(oldNDC.x - newNDC.x),
                  max(abs(oldNDC.y - newNDC.y),
                      abs(oldNDC.z - newNDC.z)));
        // Un-nest bitcast inside atomicMax per AMD GLSL compiler quirk
        // (spec gemini #3). Cast to local uint first.
        uint dBits = floatBitsToUint(d);
        atomicMax(debugSSBO_counters[0], dBits);
        atomicAdd(debugSSBO_counters[1], 1u);
    } else {
        if (oldBehind && newBehind) {
            atomicAdd(debugSSBO_counters[5], 1u);  // count_behind_both
        } else if (oldBehind) {
            atomicAdd(debugSSBO_counters[2], 1u);  // count_behind_old_only
        } else if (newBehind) {
            atomicAdd(debugSSBO_counters[3], 1u);  // count_behind_new_only
        } else {
            if (oldHazard && newHazard) {
                atomicAdd(debugSSBO_counters[6], 1u);
            } else if (oldHazard) {
                atomicAdd(debugSSBO_counters[4], 1u);
            } else /* newHazard */ {
                atomicAdd(debugSSBO_counters[7], 1u);
            }
        }
    }

    // Task 7c: diagnostic snapshot -- exactly one TES invocation per frame writes
    // the matrix the TES actually sees + a sample vertex for CPU comparison.
    // Guard: first vertex of first primitive (gl_PrimitiveID==0, bary corner).
    if (gl_PrimitiveID == 0 && bary.x > 0.999) {
        mat4 m = ssbo_readWorldToClipGL();
        // Store row-major so CPU readback index = [row*4+col].
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                diagMatrix[r*4+c] = m[c][r];
        diagSampleWorld[0] = worldPos.x;    diagSampleWorld[1] = worldPos.y;
        diagSampleWorld[2] = worldPos.z;    diagSampleWorld[3] = 1.0;
        diagSampleNewClip[0] = newClip.x;   diagSampleNewClip[1] = newClip.y;
        diagSampleNewClip[2] = newClip.z;   diagSampleNewClip[3] = newClip.w;
        diagSampleLegacyClip[0] = clip.x;   diagSampleLegacyClip[1] = clip.y;
        diagSampleLegacyClip[2] = clip.z;   diagSampleLegacyClip[3] = clip.w;
        diagSampleLegacyGlPos[0] = legacyGlPosition.x;
        diagSampleLegacyGlPos[1] = legacyGlPosition.y;
        diagSampleLegacyGlPos[2] = legacyGlPosition.z;
        diagSampleLegacyGlPos[3] = legacyGlPosition.w;
    }
#endif

    // CRITICAL: BOTH branches output legacy. Stage A-pre changes ZERO
    // production behavior. A build without
    // MC2_UNIFIED_PROJECTION_PARITY_PROBE behaves byte-identical to pre-F1.
    gl_Position = legacyGlPosition;

    // Seam expansion removed. All worldPos.xy variants changed the surface gradient
    // at edge triangles (shifting shadow coords, fwidth derivatives, fwConcrete AA),
    // producing diamond outlines on flat concrete. Clip-space-only expansion changed
    // triangle screen size, altering fwidth results in the fragment shader — same
    // artifact class. Residual sub-pixel flat-terrain seams are acceptable given the
    // trade-off; tessellated geometry already eliminates the majority of the original
    // D3D7 gaps.
}
