layout(location = 0) in ivec2 localOffset;  // (localX, localY) grid offset from block origin (stride-baked)
layout(location = 1) in int   isSkirtFlag;  // Phase 6: 0=surface vertex, 1=skirt bottom vertex

uniform int   u_blockOriginX;
uniform int   u_blockOriginY;
uniform int   u_mapSide;
uniform float u_halfMap;
uniform float u_skirtDepth;   // Phase 6: world-unit depth to pull skirt verts downward
uniform mat4  u_worldToClipGL;

// TERRAIN-DEPTH-BIAS-OWNERSHIP-1: terrain opaque writes TRUE depth now (0). The
// old -0.002 (applied 2x here = net -0.004 to match the thin VS+FS path) recessed
// distant terrain by a distance-growing world band and let objects poke through.
// Lockstep with shaders/include/terrain_depth_bias.hglsl (TERRAIN_DEPTH_FUDGE=0).
// Kept as a local const (this VS does not include the header).
const float TERRAIN_DEPTH_FUDGE = 0.0;

// Phase 10.4 edge stitching: this block's quad extent + per-edge COARSER-neighbor
// stride. u_edgeStitch packs 4 bytes: N=bits0-7, S=8-15, W=16-23, E=24-31; each
// byte is the coarser neighbour's vertex stride on that edge, or 0 = no stitch.
uniform int   u_quadCountX;
uniform int   u_quadCountY;
uniform int   u_edgeStitch;

layout(binding = 23, std430) readonly buffer TerrainHeightBuf {
    float heights[];
};
// Step 5b: per-vertex terrainType (concrete selection). Interpolated to the frag
// so cement/terrain boundary patches blend (legacy smoothstep(2,3,TerrainType)).
layout(binding = 24, std430) readonly buffer TerrainTypeBuf {
    float terrainTypes[];
};
// TERRAIN-VISUAL-HEIGHT-SAMPLE-1: 4x VISUAL heightfield (render-only geometry
// displacement). V*V row-major, V=(mapSide-1)*4+1. Read ONLY in the displaced
// branch below; binding unbound / u_visualDisplace==0 -> never sampled.
layout(binding = 26, std430) readonly buffer TerrainVisualHeightBuf {
    float heightsFine[];
};
uniform int u_visualDisplace;  // 0=off (byte-identical); 1=LOD0 corner-pinned 4x-fine displace;
                                // 2=coarser-band displace at EXISTING (unsubdivided) vertex density
uniform int u_visualSide;      // V = (mapSide-1)*4+1
// TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: far-band fade knob. 1.0 = full bake displacement
// (default); 0.0 = coarse heightfield only (mode-2 verts fall back to `h`, matching
// the byte-identical coarse path's Z). Only affects u_visualDisplace==2 verts.
uniform float u_visualDisplaceFar;
// TERRAIN-LOD-GEOMORPH-1: binding-26 layout extension. When u_geomorphMips==1 the
// buffer holds [fine V*V | mip(2) | mip(4) | mip(5) | mip(10) | mip(20)], each mip
// u_mapSide*u_mapSide row-major: MAX of the fine bake over the +/- stride/2
// coarse-cell footprint at EVERY coarse vertex (full resolution, so stitch-stride
// lookups at any index are well-defined). Coarse-band INTERIOR verts read their
// OWN stride's mip so decimation can no longer drop ridge maxima (silhouette fix);
// block-PERIMETER verts keep the S2 fine-bake/stitch path verbatim, so every seam
// agrees with every neighbour band by construction (seam ownership: perimeter
// never mips, never morphs).
uniform int   u_geomorphMips;   // 1 = mips resident in binding 26
uniform int   u_lodStep;        // this block's stride (1,2,4,5,10,20); shared with frag
// TERRAIN-LOD-GEOMORPH-1 rung a: per-block geomorph factor m [0,1]. Interior
// verts lerp own-band mip height -> parent-band (next-coarser) surface as the
// block approaches its demotion threshold, so the band switch lands on geometry
// that already matches (temporal slide instead of a one-frame snap). m is
// constant per block; perimeter verts never morph, so shared edges agree
// between blocks regardless of each side's m (seam-ownership ruling).
uniform float u_morphFactor;
float mipH(int cx, int cy, int stride) {
    cx = clamp(cx, 0, u_mapSide - 1);
    cy = clamp(cy, 0, u_mapSide - 1);
    int lvl = (stride == 2) ? 0 : (stride == 4) ? 1 : (stride == 5) ? 2
            : (stride == 10) ? 3 : 4;   // 20 -> 4
    int base = u_visualSide * u_visualSide + lvl * u_mapSide * u_mapSide;
    return heightsFine[base + cx + cy * u_mapSide];
}
// Parent-band surface at coarse vertex (cx,cy): bilinear over the parent
// stride-P lattice (P is NOT always a multiple of the own stride — 4->5 — so a
// full bilinear is required, not a lattice point-sample). At parent lattice
// points this equals the parent's own vertex height exactly; between them it
// lies on the parent's bilinear patch (differs from the parent's triangulated
// surface only by the diagonal split — bounded, zero at parent verts).
float parentBandH(int cx, int cy, int P) {
    int x0 = (cx / P) * P;
    int y0 = (cy / P) * P;
    float tx = float(cx - x0) / float(P);
    float ty = float(cy - y0) / float(P);
    float h00 = mipH(x0,     y0,     P);
    float h10 = mipH(x0 + P, y0,     P);
    float h01 = mipH(x0,     y0 + P, P);
    float h11 = mipH(x0 + P, y0 + P, P);
    return mix(mix(h00, h10, tx), mix(h01, h11, tx), ty);
}
out vec3  v_worldPos;
out float v_terrainType;

float sampleH(int mx, int my) {
    mx = clamp(mx, 0, u_mapSide - 1);
    my = clamp(my, 0, u_mapSide - 1);
    return heights[mx + my * u_mapSide];
}

void main() {
    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: corner-pinned interior subdivision. When on,
    // this patch is a 4x-finer grid: localOffset is in FINE (1/4-coarse) units.
    // INTERIOR verts take the 4x visual height (binding 26). CHUNK-EDGE verts (and
    // skirts) stay on the COARSE line — coarse-interpolated between coarse verts at
    // the neighbour's stitch stride (else stride 1) — so stitch / skirt / LOD seams
    // are pixel-identical to the coarse path and no cracks form. Default OFF ->
    // falls through to the original coarse path below (byte-identical).
    // TERRAIN-LOD-GEOMORPH-1 FIX (latent S2 bug): this branch is the MODE-1
    // (LOD0) path — its localOffset is in FINE (1/4-coarse) units and its world
    // position math is fine-grid (fx*32). The original `!= 0` condition ALSO
    // caught mode 2, whose patches are COARSE-unit — every coarse-band block
    // rendered collapsed to 1/4 size at its NW corner (floating slabs) and the
    // mode-2 Z-swap block below was unreachable dead code. Caught by the
    // FORCE_LOD=4 pixel oracle; must be `== 1` so mode 2 falls through to the
    // coarse path + stitch + its own displacement block.
    if (u_visualDisplace == 1) {
        int qx4 = u_quadCountX * 4;
        int qy4 = u_quadCountY * 4;
        int lx  = localOffset.x;
        int ly  = localOffset.y;
        int fx  = clamp(u_blockOriginX * 4 + lx, 0, u_visualSide - 1);
        int fy  = clamp(u_blockOriginY * 4 + ly, 0, u_visualSide - 1);
        float hh;
        bool onEdge = (lx == 0 || lx == qx4 || ly == 0 || ly == qy4) || (isSkirtFlag != 0);
        if (onEdge) {
            float coarseAlong; bool alongX; int Sc;
            if      (ly == 0)   { coarseAlong = float(lx) * 0.25; alongX = true;  Sc = (u_edgeStitch)       & 0xFF; }
            else if (ly == qy4) { coarseAlong = float(lx) * 0.25; alongX = true;  Sc = (u_edgeStitch >> 8)  & 0xFF; }
            else if (lx == 0)   { coarseAlong = float(ly) * 0.25; alongX = false; Sc = (u_edgeStitch >> 16) & 0xFF; }
            else                { coarseAlong = float(ly) * 0.25; alongX = false; Sc = (u_edgeStitch >> 24) & 0xFF; }
            if (Sc < 1) Sc = 1;
            float c0 = floor(coarseAlong / float(Sc)) * float(Sc);
            float c1 = c0 + float(Sc);
            float tt = (coarseAlong - c0) / float(Sc);
            float h0, h1;
            if (alongX) {
                int fyC = u_blockOriginY + (ly == 0 ? 0 : u_quadCountY);
                h0 = sampleH(u_blockOriginX + int(c0), fyC);
                h1 = sampleH(u_blockOriginX + int(c1), fyC);
            } else {
                int fxC = u_blockOriginX + (lx == 0 ? 0 : u_quadCountX);
                h0 = sampleH(fxC, u_blockOriginY + int(c0));
                h1 = sampleH(fxC, u_blockOriginY + int(c1));
            }
            hh = mix(h0, h1, tt);
        } else {
            hh = heightsFine[fx + fy * u_visualSide];
        }
        hh -= float(isSkirtFlag) * u_skirtDepth;
        float wX = float(fx) * 32.0 - u_halfMap;
        float wY = u_halfMap - float(fy) * 32.0;
        int mtx = clamp(u_blockOriginX + (lx >> 2), 0, u_mapSide - 1);
        int mty = clamp(u_blockOriginY + (ly >> 2), 0, u_mapSide - 1);
        v_worldPos    = vec3(wX, wY, hh);
        v_terrainType = terrainTypes[mtx + mty * u_mapSide];
        vec4 clipD = u_worldToClipGL * vec4(wX, wY, hh, 1.0);
        clipD.z += 2.0 * TERRAIN_DEPTH_FUDGE * clipD.w;
        gl_Position = clipD;
        return;
    }

    int mapX = clamp(u_blockOriginX + localOffset.x, 0, u_mapSide - 1);
    int mapY = clamp(u_blockOriginY + localOffset.y, 0, u_mapSide - 1);
    float h = heights[mapX + mapY * u_mapSide];

    // Phase 10.4: stitch this surface vertex to a coarser neighbour's edge line.
    // The shared edge then samples the SAME (coarse) line from both sides, so the
    // fine intermediate vertices lie exactly on the coarse segment -> no T-junction
    // crack. Corners sit at offset 0 (coarse-aligned) so they are never moved.
    // Skirt verts (isSkirtFlag != 0) are left alone — they are the vertical seal.
    if (isSkirtFlag == 0 && u_edgeStitch != 0) {
        int  Sc = 0, along = 0;
        bool alongX = true;
        if      (localOffset.y == 0            && ((u_edgeStitch)       & 0xFF) > 0) { Sc = (u_edgeStitch)       & 0xFF; along = localOffset.x; alongX = true;  } // N
        else if (localOffset.y == u_quadCountY && ((u_edgeStitch >> 8)  & 0xFF) > 0) { Sc = (u_edgeStitch >> 8)  & 0xFF; along = localOffset.x; alongX = true;  } // S
        else if (localOffset.x == 0            && ((u_edgeStitch >> 16) & 0xFF) > 0) { Sc = (u_edgeStitch >> 16) & 0xFF; along = localOffset.y; alongX = false; } // W
        else if (localOffset.x == u_quadCountX && ((u_edgeStitch >> 24) & 0xFF) > 0) { Sc = (u_edgeStitch >> 24) & 0xFF; along = localOffset.y; alongX = false; } // E
        if (Sc > 0 && (along % Sc) != 0) {
            int   c0 = (along / Sc) * Sc;
            int   c1 = c0 + Sc;
            float t  = float(along - c0) / float(Sc);
            float h0, h1;
            if (alongX) {
                int fy = u_blockOriginY + localOffset.y;          // fixed row (N or S edge)
                h0 = sampleH(u_blockOriginX + c0, fy);
                h1 = sampleH(u_blockOriginX + c1, fy);
            } else {
                int fx = u_blockOriginX + localOffset.x;          // fixed col (W or E edge)
                h0 = sampleH(fx, u_blockOriginY + c0);
                h1 = sampleH(fx, u_blockOriginY + c1);
            }
            h = mix(h0, h1, t);
        }
    }

    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: coarser-band (LOD1+) displacement at the
    // SAME (unsubdivided) vertex density as the coarse path above -- no new verts.
    // Every coarse/LOD vertex (mapX,mapY) lands EXACTLY on a bake grid point
    // (fx=mapX*4, fy=mapY*4 are always integers, since the bake is a uniform 4x
    // subdivision of the SAME map grid) -- no interpolation needed, so this is a
    // pure Z-swap at the existing sample point, byte-stable wherever the fade is 0.
    // Stitch/skirt correctness: this branch runs AFTER the coarse stitch above, so
    // `h`/`mapX`/`mapY` are already the (possibly-stitched) coarse-line values;
    // stitched edge verts (T-junctions against a coarser neighbour) mix two coarse
    // bake samples exactly like the coarse-heightfield stitch does (h0/h1 from
    // sampleH -> here from heightsFine at the corresponding integer bake index),
    // so shared edges with ANY neighbour band (LOD0 fine-corner-pinned or another
    // coarse band) still agree by construction -- zero new crack risk introduced.
    if (u_visualDisplace == 2 && isSkirtFlag == 0) {
        float hv;
        int fxV, fyV;
        // Re-derive the same edge-stitch decision to pick between a single bake
        // sample (interior / non-stitched) and a bake-based mix (stitched edge),
        // mirroring the coarse block above exactly but reading heightsFine.
        int  Sc2 = 0, along2 = 0; bool alongX2 = true;
        if      (localOffset.y == 0            && ((u_edgeStitch)       & 0xFF) > 0) { Sc2 = (u_edgeStitch)       & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.y == u_quadCountY && ((u_edgeStitch >> 8)  & 0xFF) > 0) { Sc2 = (u_edgeStitch >> 8)  & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.x == 0            && ((u_edgeStitch >> 16) & 0xFF) > 0) { Sc2 = (u_edgeStitch >> 16) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        else if (localOffset.x == u_quadCountX && ((u_edgeStitch >> 24) & 0xFF) > 0) { Sc2 = (u_edgeStitch >> 24) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        if (Sc2 > 0 && (along2 % Sc2) != 0) {
            int c0 = (along2 / Sc2) * Sc2;
            int c1 = c0 + Sc2;
            float t2 = float(along2 - c0) / float(Sc2);
            float hv0, hv1;
            if (alongX2) {
                int fyC = u_blockOriginY + localOffset.y;
                fxV = clamp((u_blockOriginX + c0) * 4, 0, u_visualSide - 1);
                int fyVc = clamp(fyC * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxV + fyVc * u_visualSide];
                int fxV1 = clamp((u_blockOriginX + c1) * 4, 0, u_visualSide - 1);
                hv1 = heightsFine[fxV1 + fyVc * u_visualSide];
            } else {
                int fxC = u_blockOriginX + localOffset.x;
                int fxVc = clamp(fxC * 4, 0, u_visualSide - 1);
                int fyV0 = clamp((u_blockOriginY + c0) * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxVc + fyV0 * u_visualSide];
                int fyV1 = clamp((u_blockOriginY + c1) * 4, 0, u_visualSide - 1);
                hv1 = heightsFine[fxVc + fyV1 * u_visualSide];
            }
            hv = mix(hv0, hv1, t2);
        } else {
            fxV = clamp(mapX * 4, 0, u_visualSide - 1);
            fyV = clamp(mapY * 4, 0, u_visualSide - 1);
            hv = heightsFine[fxV + fyV * u_visualSide];
            // TERRAIN-LOD-GEOMORPH-1: block-INTERIOR verts of a coarse band read
            // their OWN max-mip level instead of the bake corner value, so the
            // stride-N lattice carries the peak of the footprint it decimated
            // away (silhouette-LOSS fix, recon sec 3/4b). Perimeter verts
            // (localOffset on the block edge — includes every stitched vert)
            // are EXCLUDED and keep the S2 sample above: both sides of every
            // seam read identical values, zero new crack risk. Skirts never
            // reach here (mode-2 branch already gates isSkirtFlag==0).
            bool onPerim = (localOffset.x == 0 || localOffset.x == u_quadCountX ||
                            localOffset.y == 0 || localOffset.y == u_quadCountY);
            if (u_geomorphMips == 1 && !onPerim && u_lodStep > 1) {
                hv = mipH(mapX, mapY, u_lodStep);
                // Rung a: slide toward the parent band's surface as the block
                // nears its demotion threshold (m=0 at band interior -> own
                // surface unchanged; m=1 -> parent surface, so the LOD switch
                // is geometry-continuous). Stride 20 has no parent (m forced 0
                // CPU-side; guard here anyway).
                if (u_morphFactor > 0.0 && u_lodStep < 20) {
                    int P = (u_lodStep == 2) ? 4 : (u_lodStep == 4) ? 5
                          : (u_lodStep == 5) ? 10 : 20;
                    hv = mix(hv, parentBandH(mapX, mapY, P),
                             clamp(u_morphFactor, 0.0, 1.0));
                }
            }
        }
        h = mix(h, hv, clamp(u_visualDisplaceFar, 0.0, 1.0));
    }

    // Phase 6: skirt bottom verts are pulled below the (possibly stitched) surface.
    h -= float(isSkirtFlag) * u_skirtDepth;

    float worldX = float(mapX) * 128.0 - u_halfMap;
    float worldY = u_halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);
    v_terrainType = terrainTypes[mapX + mapY * u_mapSide];  // interpolated to frag

    vec4 clip = u_worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;  // pre-divide -> net NDC -0.004, early-Z preserved
    gl_Position = clip;
}
