// HDRI-SKY-1: fullscreen triangle using gl_VertexID (no VBO required).
// Caller draws GL_TRIANGLES with count=3, no vertex array data bound.

out vec2 vNdc;

void main()
{
    // Standard gl_VertexID fullscreen triangle covering NDC [-1,1]^2.
    // VertexID  ndc.x  ndc.y
    //    0       -1      -1
    //    1        3      -1
    //    2       -1       3
    vec2 ndc = vec2(
        (gl_VertexID == 1) ?  3.0 : -1.0,
        (gl_VertexID == 2) ?  3.0 : -1.0
    );
    vNdc = ndc;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
