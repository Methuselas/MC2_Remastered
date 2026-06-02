// tools/asset_viewer/SphereMesh.cpp
#include "SphereMesh.h"
#include <GL/glew.h>
#include <cmath>

void SphereMesh::generate(float radius, int stacks, int slices) {
    verts_.clear();
    idx_.clear();
    const float PI = 3.14159265358979323846f;
    for (int i = 0; i <= stacks; ++i) {
        float phi = PI * (float)i / (float)stacks;          // 0..PI (lat)
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= slices; ++j) {
            float theta = 2.0f * PI * (float)j / (float)slices;  // 0..2PI (lon)
            float st = std::sin(theta), ct = std::cos(theta);

            SphereVertex sv{};
            // unit normal/position
            float nx = sp * ct, ny = cp, nz = sp * st;
            sv.px = radius * nx; sv.py = radius * ny; sv.pz = radius * nz;
            sv.nx = nx; sv.ny = ny; sv.nz = nz;
            // uv: u from longitude, v from latitude
            sv.u = (float)j / (float)slices;
            sv.v = (float)i / (float)stacks;
            // tangent = d(position)/d(theta), normalized. dP/dtheta = (-sp*st, 0, sp*ct).
            float tx = -sp * st, ty = 0.0f, tz = sp * ct;
            float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
            if (tl < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; tl = 1.0f; }  // poles
            sv.tx = tx/tl; sv.ty = ty/tl; sv.tz = tz/tl;
            // handedness: sign of dot(cross(N,T), B) where B = dP/dphi.
            // For this parameterization right-handed TBN gives +1.
            sv.tw = 1.0f;
            verts_.push_back(sv);
        }
    }
    int cols = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = i * cols + j;
            uint32_t b = (i + 1) * cols + j;
            idx_.push_back(a);   idx_.push_back(b);   idx_.push_back(a + 1);
            idx_.push_back(a + 1); idx_.push_back(b); idx_.push_back(b + 1);
        }
    }
}

void SphereMesh::uploadGL() {
    indexCount_ = (int)idx_.size();
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(SphereVertex), verts_.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_.size()*sizeof(uint32_t), idx_.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(SphereVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)(10*sizeof(float)));
    glBindVertexArray(0);
}

void SphereMesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void SphereMesh::destroyGL() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    ebo_ = vbo_ = vao_ = 0;
}
