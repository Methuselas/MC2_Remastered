#ifndef ASSET_THUMBNAIL_CACHE_H
#define ASSET_THUMBNAIL_CACHE_H
/***************************************************************
* FILENAME: AssetThumbnailCache.h
* DESCRIPTION: Per-object-ID thumbnail cache for the Asset Browser.
*   Loads a TGA (via stb_image) from artPath + tgaName + ".tga" the first
*   time an object ID is queried, uploads it as a GL_RGBA texture, and
*   returns the GLuint handle as an ImTextureID.  Returns 0 for IDs whose
*   tgaName is "NONE", empty, or whose file is missing.  A sentinel value
*   (NO_THUMB) is stored so the lookup is never retried on a miss.
*   Call clear() when the catalog is reloaded (e.g. new map).
* DATE: 2026-06-10
****************************************************************/

#include <cstdint>

class AssetThumbnailCache
{
public:
    // Opaque handle returned to callers.  0 means "no thumbnail".
    using TexHandle = uint32_t;

    // Look up (or load) the thumbnail for this object catalog ID.
    // Returns 0 if the TGA is NONE/missing.
    static TexHandle get(int objectID);

    // Release all GL textures and clear the table.  Call on catalog reload.
    static void clear();
};

#endif // ASSET_THUMBNAIL_CACHE_H
