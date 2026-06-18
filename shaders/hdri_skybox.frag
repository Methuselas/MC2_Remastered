// HDRI-SKY-1: equirect sky background.
// Direction reconstruction: inverse projection of NDC -> view dir,
// then inverse view rotation (translation excluded) -> world dir.
// Sample equirectangular HDRI. Linear output. No tonemap, no exposure.

in vec2 vNdc;

layout(location = 0) out vec4 FragColor;

uniform mat4 invProj;
uniform mat3 invViewRot;
uniform sampler2D u_hdri;

// HDRI-SKY Item 2 sub-flags (all default to the byte-identical baseline):
//   uvDebug == 0  -> normal equirect sample (default).
//   uvDebug != 0  -> emit worldDir*0.5+0.5 as RGB (orientation diagnostic).
uniform int   uvDebug;   // default 0 (set from MC2_HDRI_SKY_UV_DEBUG)
//   skyYaw        -> radians to rotate the sampled direction about GL-up (Y)
//                    before the equirect lookup. 0.0 => byte-identical baseline.
uniform float skyYaw;    // default 0.0 (sun-sync / az-offset)
//   frameFix != 0 -> SCENE-FRAME reconstruction is active. The real fix lives
//                    C++-side (code/gamecam.cpp): when MC2_HDRI_SKY_FRAME_FIX is
//                    set the sky is fed eye->worldToViewGL() (= kAxisSwapMC2toGL *
//                    worldToCameraMatrix), the SAME world->view rotation the scene
//                    rasterizes terrain/props/mechs with, instead of the raw
//                    worldToCameraGL() (no swap). invViewRot then reconstructs the
//                    ray in the scene's GL frame, so elevation stays on worldDir.y
//                    and azimuth matches the cast shadows. No in-shader swizzle is
//                    applied here -- the proven engine matrix carries the swap.
//                    This uniform is informational only (kept for the UV-debug log);
//                    the equirect math below is identical for frameFix 0 and != 0.
uniform int   frameFix;  // set from MC2_HDRI_SKY_FRAME_FIX (now LOAD-BEARING)

// HDRI-SKY frame fix (direct-basis path). When frameFix != 0 the C++ side
// (gos_postprocess.cpp / SkyRenderAdapter) feeds the camera's WORLD-space basis
// (raw MC2 frame: x=east, y=north, z=elevation) instead of view+proj matrices,
// and the ray is reconstructed WITHOUT any matrix inversion:
//   worldDir = normalize(camFwd + camRight*vNdc.x*tHX + camUp*vNdc.y*tHY)
// then equirect is taken in the raw MC2 frame (azimuth = atan2(y,x),
// elevation on z). This sidesteps the kPixelHomogToGLNDC X/Y-flip that the
// inverse-matrix reconstruction baked into the ray. These uniforms are unused
// (and left at 0) on the default matrix path.
uniform vec3  camFwd;    // world forward  (MC2 frame)  [frameFix==1, legacy]
uniform vec3  camRight;  // world screen-right (MC2 frame) [frameFix==1, legacy]
uniform vec3  camUp;     // world screen-up (MC2 frame)  [frameFix==1, legacy]
uniform float tHX;       // tan(halfFOV_horizontal)       [frameFix==1, legacy]
uniform float tHY;       // tan(halfFOV_vertical)         [frameFix==1, legacy]

// HDRI-SKY frame fix (frameFix==2, one-proven-matrix path). C++ feeds the
// inverse of worldToClipGL (the EXACT matrix the GPU rasterizes terrain with,
// = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL). Unprojecting NDC
// through it yields a ray in the RAW MC2 world frame (x=east, y=north,
// z=elevation, Z-UP). No camera basis, no FOV, no handedness toggle.
// Column-major, uploaded verbatim like invProj, consumed as M*v.
uniform mat4  invWorldToClipGL;

const float PI = 3.14159265358979323846;

// frameFix selector:
//   0 -> legacy matrix path (invProj + invViewRot), byte-identical baseline.
//   1 -> legacy camera-basis path (camFwd/camRight/camUp), Stuff Y-up frame.
//   2 -> one-proven-matrix path: unproject through inverse(worldToClipGL),
//        ray in raw MC2 Z-up frame.
void main()
{
    vec3 worldDir;
    bool zUp = false;   // true => worldDir is in raw MC2 Z-up frame (frameFix==2)
    if (frameFix == 2) {
        // One-proven-matrix unprojection. REVERSE-Z is live on this engine
        // (glClipControl ZERO_TO_ONE, near=1, far=0, GL_GEQUAL). Unproject two
        // NDC depths and take the ray near->far.
        vec4 pn = invWorldToClipGL * vec4(vNdc, 1.0, 1.0);   // near (reverse-Z near=1)
        vec4 pf = invWorldToClipGL * vec4(vNdc, 0.0, 1.0);   // far  (reverse-Z far=0)
        vec3 worldNear = pn.xyz / pn.w;
        vec3 worldFar  = pf.xyz / pf.w;
        // Ray points from the camera outward (near -> far). If a future build
        // flips the clip convention, swap to normalize(worldNear - worldFar).
        worldDir = normalize(worldFar - worldNear);          // raw MC2 frame, Z-up
        zUp = true;
    } else if (frameFix == 1) {
        // Legacy direct-basis reconstruction in the Stuff Y-up world frame.
        worldDir = normalize(camFwd
                             + camRight * (vNdc.x * tHX)
                             + camUp    * (vNdc.y * tHY));
    } else {
        // Default (byte-identical) path: NDC -> view direction -> world dir.
        vec4 clip = vec4(vNdc, 1.0, 1.0);
        vec3 viewDir = (invProj * clip).xyz;
        worldDir = normalize(invViewRot * viewDir);
    }

    // HDRI-SKY frame fix (root cause): worldDir is reconstructed via invViewRot =
    // transpose(upper-3x3 of the view matrix passed from code/gamecam.cpp). The
    // scene rasterizes through worldToClipGL() = kAxisSwapMC2toGL *
    // worldToCameraMatrix * cameraToClipGL, so its world->view rotation INCLUDES
    // kAxisSwapMC2toGL. Feeding the sky the raw worldToCameraGL() (NO swap) left
    // the sky ray a handedness/azimuth mismatch vs the pixels behind it.
    //
    // FIX IS C++-SIDE, NOT HERE: when MC2_HDRI_SKY_FRAME_FIX is set, gamecam.cpp
    // feeds eye->worldToViewGL() (the swap-included view) instead. That matrix's
    // upper-3x3 is orthonormal (orthonormal R_cam * signed-permutation swap), so
    // its transpose is a valid inverse-rotation for the ray. The swap is applied
    // by the proven engine matrix, not by an in-shader swizzle.
    //
    // No azimuth negation is added here: in the resulting GL frame the equirect
    // azimuth atan(worldDir.z, worldDir.x) equals atan2(MC2.north, -MC2.east),
    // which is EXACTLY the convention the sun-sync code uses
    // (sunAzGL = atan2(ly, -lx), gos_postprocess.cpp). Adding a shader-side -x
    // would double-correct. The prior in-shader vec3(-x, z, y) swap is REMOVED:
    // worldDir was already Y-up (elevation on y), so z->y shoved a horizontal axis
    // into the elevation slot (the "green to NE" regression).

    // Phase 1 orientation diagnostic: show the reconstructed direction as
    // color. If this gradient is itself absent on-screen the draw is being
    // occluded/clipped/masked, not mis-oriented.
    // When frameFix is also active the gradient shows the post-swap direction:
    // green (Y) should track elevation, horizon is where worldDir.y ~ 0.
    if (uvDebug != 0) {
        FragColor = vec4(worldDir * 0.5 + 0.5, 1.0);
        return;
    }

    // Phase 2 sun-sync: rotate worldDir about the UP axis by skyYaw so the baked
    // sun lines up with the mission sun. Gated to a no-op (cos=1, sin=0) when
    // skyYaw == 0.0 so the default launch is byte-identical to the legacy sample.
    if (skyYaw != 0.0) {
        float s = sin(skyYaw);
        float c = cos(skyYaw);
        if (zUp) {
            // Raw MC2 Z-up frame (frameFix==2): rotate about +Z (up).
            // x' = x*c - y*s ; y' = x*s + y*c.
            worldDir = vec3(worldDir.x * c - worldDir.y * s,
                            worldDir.x * s + worldDir.y * c,
                            worldDir.z);
        } else {
            // Y-up frame (legacy frameFix 0/1): rotate about +Y (up).
            // x' = x*c - z*s ; z' = x*s + z*c.
            worldDir = vec3(worldDir.x * c - worldDir.z * s,
                            worldDir.y,
                            worldDir.x * s + worldDir.z * c);
        }
    }

    // Equirect mapping.
    // invWorldToClipGL inverts kPixelHomogToGLNDC which negates the Z component,
    // so worldDir.z is negative for "up" (GL NDC Y+ maps to world -Z via inversion).
    // TinyEXR row 0 (sky zenith) lands at GL v=0; asin(-1)/PI+0.5 == 0.0, so
    // we do NOT flip v here — the negated z already reads the correct row.
    vec2 uv;
    if (zUp) {
        // Raw MC2 Z-up equirect (frameFix==2, one-proven-matrix path).
        // azimuth   = atan2(north, east) = atan2(worldDir.y, worldDir.x)
        // elevation = asin(worldDir.z)   — worldDir.z < 0 when looking up (see above)
        uv = vec2(
            atan(worldDir.y, worldDir.x) / (2.0 * PI) + 0.5,
            asin(clamp(worldDir.z, -1.0, 1.0)) / PI + 0.5
        );
    } else {
        // Y-up equirect (legacy frameFix 0 baseline + frameFix 1 camera-basis,
        // both in a Y-up world frame): azimuth = atan2(z, x), elevation on y.
        uv = vec2(
            atan(worldDir.z, worldDir.x) / (2.0 * PI) + 0.5,
            1.0 - (asin(clamp(worldDir.y, -1.0, 1.0)) / PI + 0.5)
        );
    }

    FragColor = vec4(texture(u_hdri, uv).rgb, 1.0);
}
