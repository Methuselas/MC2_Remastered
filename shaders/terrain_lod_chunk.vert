layout(location = 0) in ivec2 localOffset;  // (localX, localY) from patch VBO

uniform int   u_blockOriginX;
uniform int   u_blockOriginY;
uniform int   u_mapSide;
uniform float u_halfMap;
uniform mat4  u_worldToClipGL;

layout(binding = 23, std430) readonly buffer TerrainHeightBuf {
    float heights[];
};

out vec3 v_worldPos;

void main() {
    int mapX = u_blockOriginX + localOffset.x;
    int mapY = u_blockOriginY + localOffset.y;
    mapX = clamp(mapX, 0, u_mapSide - 1);
    mapY = clamp(mapY, 0, u_mapSide - 1);
    float h = heights[mapX + mapY * u_mapSide];

    float worldX = float(mapX) * 128.0 - u_halfMap;
    float worldY = u_halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    gl_Position = u_worldToClipGL * vec4(worldX, worldY, h, 1.0);
}
