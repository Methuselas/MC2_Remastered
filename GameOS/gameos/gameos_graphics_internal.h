#ifndef GAMEOS_GRAPHICS_INTERNAL_H
#define GAMEOS_GRAPHICS_INTERNAL_H

// GAMEOS-GRAPHICS-SPLIT-1: shared internals for the gameos_graphics_*.cpp
// extraction TUs. Rules (see docs/splits/gameos-graphics-split-1-plan.md on
// nifty): class definitions moved here must be VERBATIM (every field — the
// classes were TU-internal, so any drift is an ODR bug); gosRenderer and gVAO
// stay in the main TU, extraction TUs reach the renderer only through the
// shim functions declared at the bottom (defined in gameos_graphics.cpp).

#include "gameos.hpp"
#include "gos_font.h"

#include <cstdint>

class gosRenderer;

////////////////////////////////////////////////////////////////////////////////
// gosFont — definition moved verbatim from gameos_graphics.cpp (SPLIT-1
// slice 1). Implementation lives in gameos_graphics_font.cpp.
class gosFont {
        friend class gosRenderer;
    public:
        static gosFont* load(const char* fontFile);

        int getMaxCharWidth() const { return gi_.max_advance_; }
        int getMaxCharHeight() const { return gi_.font_line_skip_; }
        int getFontAscent() const { return gi_.font_ascent_; }

        int getCharWidth(int c) const;
        void getCharUV(int c, uint32_t* u, uint32_t* v) const;
        int getCharAdvance(int c) const;
        const gosGlyphMetrics& getGlyphMetrics(int c) const;
        const gosGlyphInfo& getGlyphInfo() const { return gi_; }


        DWORD getTextureId() const { return tex_id_; }
        const char* getName() const { return font_name_; }
        const char* getId() const { return font_id_; }

        uint32_t getRefCount() { return ref_count_; }
        uint32_t addRef() { return ++ref_count_; }
        uint32_t decRef() { gosASSERT(ref_count_>0); return --ref_count_; }

    private:
        static uint32_t destroy(gosFont* font);
        gosFont():font_name_(0), font_id_(0), tex_id_(0), ref_count_(1) {};
        ~gosFont();

        char* font_name_;
        char* font_id_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
        uint32_t ref_count_;
};

// ── Renderer shims (defined in gameos_graphics.cpp) ─────────────────────────
// The gosRenderer class definition stays private to the main TU; extraction
// TUs use these instead of member calls.

// .bmp+.glyph fallback: create the font page gosTexture, createHardwareTexture
// (STOPs on failure, matching the old inline behavior) and register it with
// the renderer. Returns the gos texture handle.
DWORD gosRendererAddFontBmpTexture(const char* textureName);

// gosFont::~gosFont: release the font page texture.
void gosRendererDeleteTexture(DWORD texId);

#endif // GAMEOS_GRAPHICS_INTERNAL_H
