layout(location = 0) in ivec2 localOffset;  // (localX, localY) grid offset from block origin (stride-baked)
layout(location = 1) in int   isSkirtFlag;  // Phase 6: 0=surface vertex, 1=skirt bottom vertex

uniform int   u_blockOriginX;
uniform int   u_blockOriginY;
uniform int   u_mapSide;
uniform float u_halfMap;
uniform float u_skirtDepth;   // Phase 6: world-unit depth to pull skirt verts downward
uniform mat4  u_worldToClipGL;

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

out vec3  v_worldPos;
out float v_terrainType;

float sampleH(int mx, int my) {
    mx = clamp(mx, 0, u_mapSide - 1);
    my = clamp(my, 0, u_mapSide - 1);
    return heights[mx + my * u_mapSide];
}

void main() {
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

    // Phase 6: skirt bottom verts are pulled below the (possibly stitched) surface.
    h -= float(isSkirtFlag) * u_skirtDepth;

    float worldX = float(mapX) * 128.0 - u_halfMap;
    float worldY = u_halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);
    v_terrainType = terrainTypes[mapX + mapY * u_mapSide];  // interpolated to frag

    gl_Position = u_worldToClipGL * vec4(worldX, worldY, h, 1.0);
}
