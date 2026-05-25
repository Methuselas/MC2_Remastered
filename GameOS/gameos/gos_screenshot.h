// gos_screenshot.h - Shared TGA capture helper.
// Reads the currently-bound framebuffer via glReadPixels and writes an
// uncompressed 24-bit TGA. Caller is responsible for binding the desired FBO
// before calling.
#pragma once

namespace gos { namespace screenshot {

// Writes an uncompressed BGR TGA from the currently-bound framebuffer at
// (w, h) viewport size. Returns true on success, false on fopen failure.
// Logs to stderr on both success and failure paths (prefix "VALIDATE:" is
// preserved for backwards compatibility with existing log-grep tooling).
bool writeTGA(const char* path, int w, int h);

}}  // namespace gos::screenshot
