/***************************************************************
* FILENAME: AssetThumbnailCache.cpp
* DESCRIPTION: See AssetThumbnailCache.h.
* DATE: 2026-06-10
****************************************************************/

#include "stdafx.h"

#include "AssetThumbnailCache.h"

#include "EditorObjectMgr.h"
#include "paths.h"           // artPath (extern char artPath[])

// stb_image -- file-static instance to avoid ODR conflicts with
// burnin_jpeg_decode.cpp (which has its own STB_IMAGE_IMPLEMENTATION) and
// MapGeneratorDialog.cpp (same pattern: STB_IMAGE_STATIC + IMPLEMENTATION).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../3rdparty/stb/stb_image.h"

#include <GL/glew.h>

#include <unordered_map>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Internal cache
// ---------------------------------------------------------------------------
// Sentinel stored for IDs that have no thumbnail, so we never retry.
static constexpr AssetThumbnailCache::TexHandle NO_THUMB = static_cast<AssetThumbnailCache::TexHandle>(-1);

static std::unordered_map<int, AssetThumbnailCache::TexHandle> s_cache;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static AssetThumbnailCache::TexHandle loadThumbnail(int objectID)
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return 0;

    // ID validity guard: getTGAFileName uses getGroup/getIndexInGroup on the
    // raw packed ID; protect against obviously-bad values.
    const char* tgaBase = mgr->getTGAFileName(objectID);
    if (!tgaBase || !tgaBase[0])
        return 0;
    if (std::strcmp(tgaBase, "NONE") == 0 || std::strcmp(tgaBase, "0") == 0)
        return 0;

    // Build full path: artPath + tgaBase + ".tga"
    // artPath is "data\art\" (or similar, read from ini at startup).
    char path[512];
    std::snprintf(path, sizeof(path), "%s%s.tga", artPath, tgaBase);

    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(path, &w, &h, &n, 4 /*force RGBA*/);
    if (!px)
        return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(px);

    return static_cast<AssetThumbnailCache::TexHandle>(tex);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
AssetThumbnailCache::TexHandle AssetThumbnailCache::get(int objectID)
{
    auto it = s_cache.find(objectID);
    if (it != s_cache.end())
    {
        const TexHandle h = it->second;
        return (h == NO_THUMB) ? 0 : h;
    }

    const TexHandle h = loadThumbnail(objectID);
    // Store 0 as NO_THUMB sentinel so we never retry a missing file.
    s_cache[objectID] = (h == 0) ? NO_THUMB : h;
    return h;
}

void AssetThumbnailCache::clear()
{
    for (auto& kv : s_cache)
    {
        if (kv.second != NO_THUMB && kv.second != 0)
        {
            GLuint tex = static_cast<GLuint>(kv.second);
            glDeleteTextures(1, &tex);
        }
    }
    s_cache.clear();
}
