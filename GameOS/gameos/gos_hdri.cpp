#include "gos_hdri.h"

#include <cstdio>
#include <cstdlib>  // for free()

// tinyexr brings in miniz transitively. Define implementation macros
// in EXACTLY this one TU. miniz.c is compiled separately (registered
// in CMake at T7).
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

GLuint loadHdriTexture(const char* path)
{
    if (!path || !*path) {
        std::fprintf(stderr, "[HDRI_SKY v1] enabled=0 reason=path_empty\n");
        return 0;
    }

    // tinyexr LoadEXR signature:
    //   int LoadEXR(float** out_rgba, int* width, int* height,
    //               const char* filename, const char** err);
    // On success returns TINYEXR_SUCCESS (0), out_rgba is a newly-allocated
    // float[w*h*4] in RGBA order (always 4 channels, regardless of source).
    // Caller must free(out_rgba) and FreeEXRErrorMessage(err) if err != null.
    float* pixels = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    int rc = LoadEXR(&pixels, &w, &h, path, &err);
    if (rc != TINYEXR_SUCCESS) {
        std::fprintf(stderr,
            "[HDRI_SKY v1] enabled=0 reason=load_failed path=%s rc=%d err=%s\n",
            path, rc, err ? err : "(null)");
        if (err) FreeEXRErrorMessage(err);
        if (pixels) free(pixels);  // defensive — should be null on failure
        return 0;
    }

    // Drain any stale GL errors from a previous caller so the post-upload
    // check below blames only this function.
    while (glGetError() != GL_NO_ERROR) { /* drain */ }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        std::fprintf(stderr,
            "[HDRI_SKY v1] enabled=0 reason=glGenTextures_failed path=%s\n", path);
        free(pixels);
        return 0;
    }

    GLint prevBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBinding);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                 GL_RGBA, GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, prevBinding);

    free(pixels);  // tinyexr uses plain malloc; free with free() not delete

    const GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        std::fprintf(stderr,
            "[HDRI_SKY v1] enabled=0 reason=gl_error path=%s err=0x%x\n",
            path, glErr);
        glDeleteTextures(1, &tex);
        return 0;
    }

    std::fprintf(stderr,
        "[HDRI_SKY v1] loaded path=%s w=%d h=%d format=RGBA16F tex=%u\n",
        path, w, h, tex);
    return tex;
}
