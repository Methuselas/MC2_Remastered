// tools/mc2fx/mc2fx_core.h
//
// Reusable gosFX effect-blob load/dump/save core. main.cpp (dump/rebuild) and
// the later previewer both link this for the SAME headless load path.
//
// Dependency-light: forward-declares the gosFX/SpecLibrary types; the .cpp
// includes the heavy engine headers.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mc2 { namespace particles { class SpecLibrary; } }

namespace mc2fx {

// One-time headless engine bring-up (Stuff -> MLR -> MLRTexturePool -> gosFX).
// Idempotent: safe to call more than once; only the first call does work.
void initEngineHeadless();

// Load a .fx blob into the SpecLibrary singleton. initEngineHeadless() must
// have run. Returns the loaded singleton (or nullptr on failure). The singleton
// requires an empty library (SpecLibrary::Load Verify(!m_effects.GetLength())),
// so this can only be called once per process.
mc2::particles::SpecLibrary* loadBlob(const unsigned char* bytes, size_t n);

// Shallow effect-catalog JSON (index/effectID/classID/name) for a loaded lib.
std::string dumpCatalogJson(mc2::particles::SpecLibrary* lib);

// Re-emit the loaded library to a byte blob via SpecLibrary::Save. Returns true
// on success; `out` receives exactly the bytes the Save path wrote.
//
// `reserveHint` pre-sizes the backing stream buffer. This is LOAD-BEARING, not an
// optimization: the engine's MemoryStream::WriteBytes(const void*, size_t) virtual
// (the one the Save path actually dispatches to) does NOT grow the buffer — the
// would-be growing override DynamicMemoryStream::WriteBytes is declared with a
// DWORD arg, so on x64 (DWORD != size_t) it never overrides the size_t virtual and
// is dead on this path. So the stream must be born large enough to hold the whole
// blob or the base writer overruns the heap. Pass >= expected output size (e.g.
// 4x the input). 0 keeps the legacy zero-size buffer (will overrun for real blobs).
bool saveBlob(mc2::particles::SpecLibrary* lib, std::vector<unsigned char>& out,
              size_t reserveHint = 0);

}  // namespace mc2fx
