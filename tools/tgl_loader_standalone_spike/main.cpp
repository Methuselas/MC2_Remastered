// tools/tgl_loader_standalone_spike/main.cpp
//
// NS3 game-free TGL/ASE loader LINK-PROOF spike.
//
// Loads a real .tgl via TG_TypeMultiShape::LoadBinaryCopy and prints
// geometry (vertices / UVs / per-triangle texture names) WITHOUT linking
// any code/ game TU or GameOS gameos.cpp. The link itself is the proof of
// game-freeness; this driver proves the loaded data is real.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "heap.h"
#include "tgl.h"
#include "msl.h"

// TG_Shape::tglHeap is a static member the loader allocates all geometry
// from; it must be non-null before LoadBinaryCopy. We own it here.
extern UserHeapPtr userHeapForTgl; // defined in stubs.cpp (sets TG_Shape::tglHeap)

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        std::printf("usage: %s <path-to.tgl>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    // Install the TGL heap (non-GOS backing). stubs.cpp provides the heap
    // object and assigns TG_Shape::tglHeap.
    extern void InstallTglHeap();
    InstallTglHeap();

    std::printf("[spike] loading: %s\n", path);

    TG_TypeMultiShape ms;
    long rc = ms.LoadBinaryCopy(path);
    if (rc != 0)
    {
        std::printf("[spike] LoadBinaryCopy FAILED rc=%ld\n", rc);
        return 1;
    }

    long numShapes   = ms.GetNumShapes();
    long numTextures = ms.GetNumTextures();
    std::printf("[spike] LOAD OK: shapes=%ld textures=%ld\n", numShapes, numTextures);

    // Texture table.
    std::printf("[spike] texture names:\n");
    for (long t = 0; t < numTextures; ++t)
    {
        char buf[256] = {0};
        ms.GetTextureName((DWORD)t, buf, (long)sizeof(buf));
        std::printf("    [%ld] %s\n", t, buf);
    }

    if (numShapes <= 0)
    {
        std::printf("[spike] no shapes to inspect\n");
        return 0;
    }

    // Inspect shape 0. GetTypeNode returns a TG_TypeNode*; the renderable
    // geometry lives on TG_TypeShape (a TG_TypeNode subclass). Walk shapes
    // until we find one that is a real TG_TypeShape with vertices.
    TG_TypeShape* shape = nullptr;
    long shapeIdx = -1;
    for (long i = 0; i < numShapes; ++i)
    {
        TG_TypeNodePtr node = ms.GetTypeNode(i);
        if (!node) continue;
        // dynamic_cast is safe: TG_TypeNode has virtuals (it's polymorphic).
        TG_TypeShape* s = dynamic_cast<TG_TypeShape*>(node);
        if (s && s->GetNumTypeVertices() > 0)
        {
            shape = s;
            shapeIdx = i;
            break;
        }
    }

    if (!shape)
    {
        std::printf("[spike] no TG_TypeShape with geometry found among %ld nodes\n", numShapes);
        return 0;
    }

    int  numV = shape->GetNumTypeVertices();
    long numT = shape->GetNumTypeTriangles();
    const TG_TypeVertex*   verts = shape->GetTypeVertices();
    const TG_TypeTriangle* tris  = shape->GetTypeTriangles();

    std::printf("[spike] shape[%ld]: vertices=%d triangles=%ld\n", shapeIdx, numV, numT);

    // First 3 vertices: position + normal.
    int showV = numV < 3 ? numV : 3;
    for (int v = 0; v < showV; ++v)
    {
        const TG_TypeVertex& tv = verts[v];
        std::printf("    v[%d] pos=(% .4f,% .4f,% .4f) nrm=(% .4f,% .4f,% .4f) aRGB=0x%08lX\n",
            v,
            tv.position.x, tv.position.y, tv.position.z,
            tv.normal.x,   tv.normal.y,   tv.normal.z,
            (unsigned long)tv.aRGBLight);
    }

    // First 3 triangles: vertex indices, UVs (per-triangle), and the
    // resolved per-triangle texture name (localTextureHandle -> name).
    int showT = (int)(numT < 3 ? numT : 3);
    for (int f = 0; f < showT; ++f)
    {
        const TG_TypeTriangle& tt = tris[f];
        char texbuf[256] = {0};
        ms.GetTextureName(tt.localTextureHandle, texbuf, (long)sizeof(texbuf));
        std::printf("    tri[%d] idx=(%lu,%lu,%lu) texHandle=%lu tex=\"%s\"\n",
            f,
            (unsigned long)tt.Vertices[0],
            (unsigned long)tt.Vertices[1],
            (unsigned long)tt.Vertices[2],
            (unsigned long)tt.localTextureHandle,
            texbuf);
        std::printf("            uv0=(% .4f,% .4f) uv1=(% .4f,% .4f) uv2=(% .4f,% .4f)\n",
            tt.uvdata.u0, tt.uvdata.v0,
            tt.uvdata.u1, tt.uvdata.v1,
            tt.uvdata.u2, tt.uvdata.v2);
    }

    std::printf("[spike] SUCCESS — geometry loaded game-free.\n");
    return 0;
}
