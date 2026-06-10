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

// Rich per-effect JSON (`dump --full`): decodes the editable base curves
// (m_lifeSpan, m_minimumChildSeed, m_maximumChildSeed) plus the common billboard
// subclass curves (Singleton/Card colors+scale, Card halfHeight/aspectRatio/index,
// ParticleCloud family particlesPerSecond/pLifeSpan/p-colors/startingSpeed). Each
// curve emits its type + stored values. Unknown subclasses get a decoded:false
// marker; never crashes on an unknown type.
std::string dumpFullJson(mc2::particles::SpecLibrary* lib);

// ---- build / patch ----------------------------------------------------------
// One field edit parsed from a patch.json. `curveType` is "constant" (only type
// supported in slice 1). `value` is the scalar to store.
struct PatchEdit {
    std::string effect;     // effect name to match (m_name)
    std::string field;      // member name, e.g. "m_lifeSpan", "m_scale"
    std::string curveType;  // "constant"
    double      value;
};

// Parse a patch.json (limited schema, see main.cpp/usage) into a flat edit list.
// Returns false + fills `err` on a parse error. Tiny hand parser; no nlohmann.
bool parsePatchJson(const std::string& text, std::vector<PatchEdit>& edits,
                    std::string& err);

// Apply edits to a loaded library in place. For each edit, locate the named curve
// member on the matched effect and overwrite its stored constant value. Appends a
// human-readable line per edit (applied / not-found / unsupported) to `report`.
// Returns the number of edits actually applied.
unsigned applyPatch(mc2::particles::SpecLibrary* lib,
                    const std::vector<PatchEdit>& edits, std::string& report);

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
