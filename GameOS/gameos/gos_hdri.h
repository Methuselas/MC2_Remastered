#ifndef GAMEOS_GOS_HDRI_H
#define GAMEOS_GOS_HDRI_H

#include <GL/glew.h>

// Load an OpenEXR (.exr) equirectangular image as a GL_RGBA16F 2D texture.
// Returns the GL texture name on success, or 0 on any failure
// (file missing, decode error, GL upload error).
//
// Caller owns the returned texture and must glDeleteTextures() it.
//
// Failure is logged to stderr using the project's [HDRI_SKY v1]
// schema via std::fprintf(stderr, ...). No printf, no PAUSE macros.
//
// Sampler state is configured on the returned texture:
//   GL_TEXTURE_WRAP_S/T = GL_CLAMP_TO_EDGE
//   GL_TEXTURE_MIN_FILTER = GL_LINEAR (no mips)
//   GL_TEXTURE_MAG_FILTER = GL_LINEAR
//
// HDRI-SKY Item 2 (sun scan): if outSunAzGL / outSunValid are non-null, the
// loader scans the decoded equirect (before freeing the pixel buffer) for the
// dominant above-horizon light source and writes its GL-equirect azimuth in
// radians (the same atan(worldDir.z, worldDir.x) convention used by
// hdri_skybox.frag) to *outSunAzGL, setting *outSunValid=true. On any failure
// or empty scan *outSunValid is set false and *outSunAzGL left untouched. The
// scan is read-only on GL state and adds no GL calls.
GLuint loadHdriTexture(const char* path,
                       float* outSunAzGL = nullptr,
                       bool*  outSunValid = nullptr);

#endif  // GAMEOS_GOS_HDRI_H
