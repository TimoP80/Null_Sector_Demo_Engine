// ---------------------------------------------------------------------------
// Model support - 3D mesh nodes.
//
// ObjImporter     minimal OBJ loader (v/vt/vn/f, per-face smoothing, tangent
//                 generation for normal mapping) - no third-party dependency.
// ModelRenderer   forward-lit pass: draws MeshData with the lit.vert/lit.frag
//                 PBR-ish material model, driven by the engine camera + the
//                 scene graph's light nodes (up to 4), into a depth-enabled
//                 target that the DemoApp composites over the HDR scene.
//
// Assimp-based glTF/GLB/FBX/DAE import plugs into the same Model data
// structure (the interface is the importer, not the renderer).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/framebuffer.hpp"
#include "engine/mesh.hpp"
#include "engine/shader.hpp"
#include "framework/core/value.hpp"
#include "app/shadermanager.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ns {

struct Material {
  std::array<float, 4> baseColor{1, 1, 1, 1};
  float metallic = 0.0f;
  float roughness = 0.5f;
  float ao = 1.0f;
  std::array<float, 3> emission{0, 0, 0};
  float opacity = 1.0f;
  std::string albedoMap;   // optional texture path ("" = none)
  std::string normalMap;

  static Material fromJson(const Value& v);
  Value toJson() const;
};

// NOTE: named MeshPrimitive so it cannot collide with the scene graph's
// MeshData payload (framework/scene/scenegraph.hpp) - both live in ns::.
struct MeshPrimitive {
  std::vector<float> positions;   // xyz
  std::vector<float> normals;     // xyz
  std::vector<float> uvs;         // uv
  std::vector<float> tangents;    // xyz + handedness (w)
  std::vector<uint32_t> indices;
  Material material;
  Mesh mesh{::gl::TRIANGLES};
  int indexCount = 0;
  bool uploaded = false;

  void upload();       // build VAO/VBOs
  void draw() const;   // draw with the material bound
};

struct Model {
  std::string name;
  std::vector<MeshPrimitive> meshes;
};

/** minimal OBJ importer (v / vt / vn / f with v, v/vt, v//vn, v/vt/vn) */
class ObjImporter {
public:
  static bool load(const std::string& path, Model& out);
};

struct LightUniforms {
  std::array<std::array<float, 4>, 4> pos;    // xyz (+pad)
  std::array<std::array<float, 4>, 4> color;  // rgb + intensity
  std::array<int, 4> type;                    // 0 dir, 1 point, 2 spot
  std::array<float, 4> range;
  std::array<float, 4> angle;
  int count = 0;

  void clear() { count = 0; }
  void addDir(const std::array<float, 3>& dir, const std::array<float, 3>& color, float intensity);
  void addPoint(const std::array<float, 3>& pos, const std::array<float, 3>& color, float intensity, float range);
  void addSpot(const std::array<float, 3>& pos, const std::array<float, 3>& dir, const std::array<float, 3>& color,
               float intensity, float range, float angleDeg);
};

/** forward-lit 3D renderer for the scene graph's mesh nodes */
class ModelRenderer {
public:
  /** compile the lit program; throws on failure */
  void init(ShaderManager& sm);

  /** set the texture handles used by materials that reference albedo/normal
   *  maps (the DemoApp wires them from the asset manager; 0 = none) */
  void setTextures(unsigned albedo, unsigned normal) {
    albedoTex_ = albedo;
    normalTex_ = normal;
  }

  /** render a model into the CURRENT target with depth (the caller binds a
   *  color+depth FBO and enables depth testing). overrideMat: a per-node
   *  material from the director's material table (nullptr = use the model's
   *  own embedded material). */
  void drawModel(const Model& model, const float* modelMatrix, const LightUniforms& lights,
                 float ambient = 0.15f, const Material* overrideMat = nullptr);

  void drawMesh(const MeshPrimitive& m, const float* modelMatrix, const LightUniforms& lights,
                float ambient, const Material* overrideMat);

private:
  ProgramRef prog_;
  unsigned albedoTex_ = 0;
  unsigned normalTex_ = 0;
};

}  // namespace ns
