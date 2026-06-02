// tools/asset_viewer/SphereMesh.h
#pragma once
#include <vector>
#include <cstdint>

// Interleaved vertex: position(3), normal(3), tangent(4 = xyz + handedness w), uv(2) = 12 floats.
struct SphereVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;   // tangent.xyz + handedness (+1 / -1)
    float u, v;
};

class SphereMesh {
public:
    // Generates a UV sphere of `radius` with `stacks` latitude bands and
    // `slices` longitude segments. CPU buffers only; call uploadGL() once a
    // GL context is current to create VAO/VBO/EBO.
    void generate(float radius, int stacks, int slices);
    void uploadGL();          // creates VAO/VBO/EBO from the CPU buffers
    void draw() const;        // glBindVertexArray + glDrawElements(GL_TRIANGLES)
    void destroyGL();         // delete VAO/VBO/EBO

    const std::vector<SphereVertex>& vertices() const { return verts_; }
    const std::vector<uint32_t>&     indices()  const { return idx_;   }

private:
    std::vector<SphereVertex> verts_;
    std::vector<uint32_t>     idx_;
    unsigned vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
};
