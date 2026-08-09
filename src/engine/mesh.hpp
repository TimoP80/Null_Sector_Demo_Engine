// ---------------------------------------------------------------------------
// Vertex buffer / VAO helper (port of src/engine/mesh.ts).
// Shaders declare attributes with layout(location = N).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include <cstdint>
#include <vector>

namespace ns {

class Mesh {
public:
  unsigned vao = 0;
  unsigned vbos[4] = {0, 0, 0, 0};
  unsigned ibo = 0;
  int indexCount = 0;
  unsigned indexType = 0;
  unsigned mode = 0;

  explicit Mesh(unsigned mode) : mode(mode) {
    ::glGenVertexArrays(1, &vao);
    indexType = ::gl::UNSIGNED_SHORT;
  }
  ~Mesh() { destroy(); }
  Mesh(const Mesh&) = delete;
  Mesh& operator=(const Mesh&) = delete;
  Mesh(Mesh&& o) noexcept { *this = std::move(o); }
  Mesh& operator=(Mesh&& o) noexcept {
    if (this != &o) {
      destroy();
      vao = o.vao; for (int i = 0; i < 4; i++) vbos[i] = o.vbos[i];
      ibo = o.ibo; indexCount = o.indexCount; indexType = o.indexType; mode = o.mode;
      o.vao = 0; for (int i = 0; i < 4; i++) o.vbos[i] = 0; o.ibo = 0; o.indexCount = 0;
    }
    return *this;
  }

  void destroy() {
    for (int i = 0; i < 4; i++) if (vbos[i]) ::glDeleteBuffers(1, &vbos[i]);
    if (ibo) ::glDeleteBuffers(1, &ibo);
    if (vao) ::glDeleteVertexArrays(1, &vao);
    for (int i = 0; i < 4; i++) vbos[i] = 0;
    ibo = 0; vao = 0; indexCount = 0;
  }

  /** upload float data into the VBO for an attribute location. stride/offset
   *  are in floats (interleaved layouts). */
  void setBuffer(int location, const float* data, int count, int size, unsigned usage, int stride = 0, int offset = 0) {
    ::glBindVertexArray(vao);
    if (!vbos[location]) ::glGenBuffers(1, &vbos[location]);
    ::glBindBuffer(::gl::ARRAY_BUFFER, vbos[location]);
    ::glBufferData(::gl::ARRAY_BUFFER, (GLsizeiptr)(count * sizeof(float)), data, usage);
    ::glEnableVertexAttribArray((unsigned)location);
    ::glVertexAttribPointer((unsigned)location, size, ::gl::FLOAT, GL_FALSE, stride * 4, (const void*)(intptr_t)(offset * 4));
    ::glBindVertexArray(0);
  }

  void setIndices(const uint16_t* data, int count) {
    ::glBindVertexArray(vao);
    if (!ibo) ::glGenBuffers(1, &ibo);
    ::glBindBuffer(::gl::ELEMENT_ARRAY_BUFFER, ibo);
    ::glBufferData(::gl::ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * sizeof(uint16_t)), data, ::gl::STATIC_DRAW);
    indexCount = count;
    indexType = ::gl::UNSIGNED_SHORT;
    ::glBindVertexArray(0);
  }

  void draw(int count) const {
    ::glBindVertexArray(vao);
    if (ibo) {
      ::glDrawElements(mode, count, indexType, nullptr);
    } else {
      ::glDrawArrays(mode, 0, count);
    }
  }
};

/** fullscreen triangle covering clip space (3 verts, positions + uvs) */
inline Mesh fullscreenTriangle() {
  Mesh m(::gl::TRIANGLES);
  const float pos[6] = {-1, -1, 3, -1, -1, 3};
  const float uv[6] = {0, 0, 2, 0, 0, 2};
  m.setBuffer(0, pos, 6, 2, ::gl::STATIC_DRAW);
  m.setBuffer(1, uv, 6, 2, ::gl::STATIC_DRAW);
  return m;
}

/** dynamic point cloud: location 0 = vec4 seedA, location 1 = vec4 seedB */
inline Mesh pointCloudMesh(int count) {
  Mesh m(::gl::POINTS);
  const std::vector<float> a((size_t)count * 4, 0.0f);
  const std::vector<float> b((size_t)count * 4, 0.0f);
  m.setBuffer(0, a.data(), count * 4, 4, ::gl::DYNAMIC_DRAW);
  m.setBuffer(1, b.data(), count * 4, 4, ::gl::DYNAMIC_DRAW);
  return m;
}

}  // namespace ns
