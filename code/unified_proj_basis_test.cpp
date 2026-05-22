// F1 Stage A-pre Task 3: CPU basis-vector smoke test (spec 2026-05-22 ss0.6).
// Called from GameCamera::render() at the first frame where worldToClip is
// guaranteed populated. Compares the GLSL-visible matrix orientation produced
// by Camera::worldToClipGL() against the legacy gamecam.cpp:165-187 upload
// product. One-off scaffold; deleted in Stage A.
//
// Gate: MC2_UNIFIED_PROJECTION_BASIS_TEST=1 env var.
// No emoji. RelWithDebInfo only.

#ifndef GAMECAM_H
#include "gamecam.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cmath>

// unifiedProj_runBasisTest
// Called exactly once per process (static-once guard lives in gamecam.cpp).
// eye is non-null, Camera::worldToClip is populated for this frame.
void unifiedProj_runBasisTest(Camera* eye)
{
    // --- Build legacy side ---
    // The gamecam.cpp:165-187 upload repackages worldToClip (column-major
    // Stuff bytes) into a row-major float[16] via WTC macro + axis swap,
    // then uploads with glUniformMatrix4fv(..., GL_FALSE, M).
    // Replicate that here to get the GLSL-visible matrix.
    const Stuff::Matrix4D& legacyWorldToClip = eye->getWorldToClip();
    {
        // Silence "unused variable" if the block below is compiled out.
        (void)legacyWorldToClip;
    }
    float legacyM_rowmajor[16];
    {
        const float* W = (const float*)&legacyWorldToClip;
        // WTC(r,c): element at row r, col c of a column-major matrix.
        #define WTC(r,c) W[(c)*4+(r)]
        float AW[4][4];
        for (int j = 0; j < 4; j++) {
            AW[0][j] = -WTC(0,j);
            AW[1][j] =  WTC(2,j);
            AW[2][j] =  WTC(1,j);
            AW[3][j] =  WTC(3,j);
        }
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                legacyM_rowmajor[i*4+j] = AW[i][j];
        #undef WTC
    }

    // --- Build new side ---
    // worldToClipGL() returns kAxisSwapMC2toGL * (worldToCameraMatrix * cameraToClip),
    // stored column-major. Unpack to row-major for comparison.
    Stuff::Matrix4D newM = eye->worldToClipGL();
    float newM_rowmajor[16];
    {
        const float* col = (const float*)&newM;
        #define WTC(r,c) col[(c)*4 + (r)]
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                newM_rowmajor[i*4 + j] = WTC(i, j);
        #undef WTC
    }

    // --- Element-wise comparison ---
    float maxElemDelta = 0.0f;
    for (int k = 0; k < 16; ++k) {
        float d = fabsf(newM_rowmajor[k] - legacyM_rowmajor[k]);
        if (d > maxElemDelta) maxElemDelta = d;
    }
    fprintf(stderr,
        "[UNIFIED_PROJ_BASIS] event=elementwise_compare max_delta=%.8f "
        "result=%s (threshold=1e-5)\n",
        maxElemDelta,
        (maxElemDelta < 1e-5f) ? "PASS" : "FAIL");
    fflush(stderr);

    // --- Per-point NDC comparison ---
    struct TestPoint { const char* name; Stuff::Vector3D world; };
    Stuff::Vector3D camOrig = eye->getCameraOrigin();
    TestPoint pts[7] = {
        { "camera_origin",    Stuff::Vector3D(camOrig.x,              camOrig.y,               camOrig.z)              },
        { "world_x_unit",     Stuff::Vector3D(camOrig.x + 1.0f,       camOrig.y,               camOrig.z)              },
        { "ground_y_unit",    Stuff::Vector3D(camOrig.x,               camOrig.y + 1.0f,        camOrig.z)              },
        { "elevation_z_unit", Stuff::Vector3D(camOrig.x,               camOrig.y,               camOrig.z + 1.0f)       },
        { "in_front_near",    Stuff::Vector3D(camOrig.x,               camOrig.y + 10.0f,       camOrig.z)              },
        { "in_front_far",     Stuff::Vector3D(camOrig.x,               camOrig.y + 1000.0f,     camOrig.z)              },
        { "behind_camera",    Stuff::Vector3D(camOrig.x,               camOrig.y - 10.0f,       camOrig.z)              },
    };

    // mul: row-major M16 * homogeneous point (vx,vy,vz,1) -> Vector4D result.
    // Row-major convention: result[i] = sum_j M16[j*4+i] * v[j]
    // (column index walks the row dimension of the matrix).
    auto mul = [](const float M16[16], float vx, float vy, float vz) -> Stuff::Vector4D {
        float r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float v[4] = { vx, vy, vz, 1.0f };
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i] += M16[j*4+i] * v[j];
        Stuff::Vector4D out;
        out.x = r[0]; out.y = r[1]; out.z = r[2]; out.w = r[3];
        return out;
    };

    const float wEpsilon = 1e-4f;
    for (int p = 0; p < 7; ++p) {
        Stuff::Vector4D oldClip = mul(legacyM_rowmajor,
            pts[p].world.x, pts[p].world.y, pts[p].world.z);
        Stuff::Vector4D newClip = mul(newM_rowmajor,
            pts[p].world.x, pts[p].world.y, pts[p].world.z);

        bool oldBehind = (oldClip.w <= wEpsilon);
        bool newBehind = (newClip.w <= wEpsilon);
        if (oldBehind || newBehind) {
            fprintf(stderr,
                "[UNIFIED_PROJ_BASIS] event=point name=%s status=behind_or_degenerate "
                "oldW=%.4f newW=%.4f\n",
                pts[p].name, oldClip.w, newClip.w);
            fflush(stderr);
            continue;
        }

        float oldNDC[3] = {
            oldClip.x / oldClip.w,
            oldClip.y / oldClip.w,
            oldClip.z / oldClip.w
        };
        float newNDC[3] = {
            newClip.x / newClip.w,
            newClip.y / newClip.w,
            newClip.z / newClip.w
        };

        float d = 0.0f;
        for (int k = 0; k < 3; ++k) {
            float dk = fabsf(oldNDC[k] - newNDC[k]);
            if (dk > d) d = dk;
        }
        fprintf(stderr,
            "[UNIFIED_PROJ_BASIS] event=point name=%s ndc_delta=%.8f "
            "result=%s (threshold=1e-3)\n",
            pts[p].name, d, (d < 1e-3f) ? "PASS" : "FAIL");
        fflush(stderr);
    }
}
