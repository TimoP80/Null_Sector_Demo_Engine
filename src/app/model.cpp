#include "app/model.hpp"
#include "engine/math.hpp"
#include "framework/core/log.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ns {

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------
Material Material::fromJson(const Value& v) {
  Material m;
  float f[4];
  if (v.get("baseColor").toFloats(f, 4) == 4) m.baseColor = {f[0], f[1], f[2], f[3]};
  else if (v.get("color").toFloats(f, 3) == 3) m.baseColor = {f[0], f[1], f[2], 1.0f};
  m.metallic = (float)v.get("metallic").asNum(m.metallic);
  m.roughness = (float)v.get("roughness").asNum(m.roughness);
  m.ao = (float)v.get("ao").asNum(m.ao);
  if (v.get("emission").toFloats(f, 3) == 3) m.emission = {f[0], f[1], f[2]};
  m.opacity = (float)v.get("opacity").asNum(m.opacity);
  m.albedoMap = v.get("albedoMap").asStr();
  m.normalMap = v.get("normalMap").asStr();
  return m;
}

Value Material::toJson() const {
  Value o = Value::object();
  Value bc = Value::array();
  for (float v : baseColor) bc.push(Value((double)v));
  o.set("baseColor") = std::move(bc);
  o.set("metallic") = Value((double)metallic);
  o.set("roughness") = Value((double)roughness);
  o.set("ao") = Value((double)ao);
  Value em = Value::array();
  for (float v : emission) em.push(Value((double)v));
  o.set("emission") = std::move(em);
  o.set("opacity") = Value((double)opacity);
  if (!albedoMap.empty()) o.set("albedoMap") = Value(albedoMap);
  if (!normalMap.empty()) o.set("normalMap") = Value(normalMap);
  return o;
}

// ---------------------------------------------------------------------------
// MeshPrimitive
// ---------------------------------------------------------------------------
void MeshPrimitive::upload() {
  const int vcount = (int)positions.size() / 3;
  if (vcount == 0) return;
  mesh.setBuffer(0, positions.data(), vcount * 3, 3, ::gl::STATIC_DRAW);
  if (!normals.empty()) mesh.setBuffer(1, normals.data(), vcount * 3, 3, ::gl::STATIC_DRAW);
  if (!uvs.empty()) mesh.setBuffer(2, uvs.data(), vcount * 2, 2, ::gl::STATIC_DRAW);
  if (!tangents.empty()) mesh.setBuffer(3, tangents.data(), vcount * 4, 4, ::gl::STATIC_DRAW);
  // indices: use uint32 upload path through the raw loader (Mesh uses uint16
  // by default; index counts can exceed 65535)
  if (!indices.empty()) {
    ::glBindVertexArray(mesh.vao);
    if (!mesh.ibo) ::glGenBuffers(1, &mesh.ibo);
    ::glBindBuffer(::gl::ELEMENT_ARRAY_BUFFER, mesh.ibo);
    ::glBufferData(::gl::ELEMENT_ARRAY_BUFFER,
                   (GLsizeiptr)(indices.size() * sizeof(uint32_t)), indices.data(), ::gl::STATIC_DRAW);
    mesh.indexCount = (int)indices.size();
    mesh.indexType = ::gl::UNSIGNED_INT;
    ::glBindVertexArray(0);
  }
  indexCount = mesh.indexCount;
  uploaded = true;
}

void MeshPrimitive::draw() const {
  ::glBindVertexArray(mesh.vao);
  if (mesh.ibo) {
    ::glDrawElements(mesh.mode, mesh.indexCount, mesh.indexType, nullptr);
  } else {
    ::glDrawArrays(mesh.mode, 0, (int)positions.size() / 3);
  }
}

// ---------------------------------------------------------------------------
// lights
// ---------------------------------------------------------------------------
void LightUniforms::addDir(const std::array<float, 3>& dir, const std::array<float, 3>& c, float intensity) {
  if (count >= 4) return;
  pos[(size_t)count] = {dir[0], dir[1], dir[2], 0};
  color[(size_t)count] = {c[0], c[1], c[2], intensity};
  type[(size_t)count] = 0;
  range[(size_t)count] = 0;
  angle[(size_t)count] = 0;
  count++;
}
void LightUniforms::addPoint(const std::array<float, 3>& p, const std::array<float, 3>& c, float intensity, float range) {
  if (count >= 4) return;
  pos[(size_t)count] = {p[0], p[1], p[2], 1};
  color[(size_t)count] = {c[0], c[1], c[2], intensity};
  type[(size_t)count] = 1;
  this->range[(size_t)count] = range;
  angle[(size_t)count] = 0;
  count++;
}
void LightUniforms::addSpot(const std::array<float, 3>& p, const std::array<float, 3>& dir, const std::array<float, 3>& c,
                            float intensity, float range, float angleDeg) {
  if (count >= 4) return;
  pos[(size_t)count] = {p[0], p[1], p[2], 1};
  color[(size_t)count] = {c[0], c[1], c[2], intensity};
  type[(size_t)count] = 2;
  this->range[(size_t)count] = range;
  angle[(size_t)count] = std::cos(angleDeg * 3.14159265f / 180.0f);
  count++;
}

// ---------------------------------------------------------------------------
// OBJ importer
// ---------------------------------------------------------------------------
namespace {
struct ObjFace {
  std::array<int, 3> v{0, 0, 0};    // 1-based
  std::array<int, 3> vt{0, 0, 0};
  std::array<int, 3> vn{0, 0, 0};
};
}  // namespace

bool ObjImporter::load(const std::string& path, Model& out) {
  std::ifstream f(path);
  if (!f) {
    Log::error("MODEL", "cannot open OBJ: " + path);
    return false;
  }
  std::vector<std::array<float, 3>> v, vn;
  std::vector<std::array<float, 2>> vt;
  std::vector<ObjFace> faces;

  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::string tag;
    is >> tag;
    if (tag == "v") {
      std::array<float, 3> p{};
      is >> p[0] >> p[1] >> p[2];
      v.push_back(p);
    } else if (tag == "vn") {
      std::array<float, 3> p{};
      is >> p[0] >> p[1] >> p[2];
      vn.push_back(p);
    } else if (tag == "vt") {
      std::array<float, 2> p{};
      is >> p[0] >> p[1];
      vt.push_back(p);
    } else if (tag == "f") {
      ObjFace face;
      int fi = 0;
      std::string tok;
      while (is >> tok && fi < 3) {
        int a = 0, b = 0, c = 0;
        char sep1 = 0, sep2 = 0;
        const int n = std::sscanf(tok.c_str(), "%d%c%d%c%d", &a, &sep1, &b, &sep2, &c);
        if (n >= 1) face.v[(size_t)fi] = a;
        if (n >= 3) face.vt[(size_t)fi] = b;
        if (n >= 5) face.vn[(size_t)fi] = c;
        fi++;
      }
      if (fi == 3) faces.push_back(face);
      // triangulate quads/n-gons beyond the first 3 verts (simple fan)
      while (is >> tok) {
        ObjFace extra = face;
        extra.v[0] = face.v[0];
        extra.v[1] = face.v[(size_t)fi - 1];
        int a = 0, b = 0, c = 0;
        char s1 = 0, s2 = 0;
        const int n = std::sscanf(tok.c_str(), "%d%c%d%c%d", &a, &s1, &b, &s2, &c);
        if (n >= 1) extra.v[2] = a;
        if (n >= 3) extra.vt[2] = b;
        if (n >= 5) extra.vn[2] = c;
        extra.vt[0] = face.vt[0];
        extra.vn[0] = face.vn[0];
        extra.vt[1] = face.vt[(size_t)fi - 1];
        extra.vn[1] = face.vn[(size_t)fi - 1];
        faces.push_back(extra);
        fi++;
      }
    }
  }
  if (faces.empty() || v.empty()) {
    Log::error("MODEL", "OBJ has no faces: " + path);
    return false;
  }

  // de-index into a single mesh with generated normals + tangents
  MeshPrimitive m;
  const auto vIdx = [&](int i) { return (i > 0 ? i - 1 : (int)v.size() - 1); };
  const auto vtIdx = [&](int i) { return (i > 0 ? i - 1 : (int)vt.size() - 1); };
  const auto vnIdx = [&](int i) { return (i > 0 ? i - 1 : (int)vn.size() - 1); };

  struct VKey {
    int v, vt, vn;
    bool operator<(const VKey& o) const {
      if (v != o.v) return v < o.v;
      if (vt != o.vt) return vt < o.vt;
      return vn < o.vn;
    }
  };
  std::map<VKey, uint32_t> vmap;

  const auto addVertex = [&](const ObjFace& fc, int corner) -> uint32_t {
    const VKey key{fc.v[(size_t)corner], fc.vt[(size_t)corner], fc.vn[(size_t)corner]};
    auto it = vmap.find(key);
    if (it != vmap.end()) return it->second;
    const std::array<float, 3>& p = v[vIdx(key.v)];
    m.positions.push_back(p[0]); m.positions.push_back(p[1]); m.positions.push_back(p[2]);
    if (key.vt > 0 && !vt.empty()) {
      const std::array<float, 2>& uv = vt[vtIdx(key.vt)];
      m.uvs.push_back(uv[0]); m.uvs.push_back(uv[1]);
    } else {
      m.uvs.push_back(0); m.uvs.push_back(0);
    }
    if (key.vn > 0 && !vn.empty()) {
      const std::array<float, 3>& n = vn[vnIdx(key.vn)];
      m.normals.push_back(n[0]); m.normals.push_back(n[1]); m.normals.push_back(n[2]);
    } else {
      m.normals.push_back(0); m.normals.push_back(0); m.normals.push_back(0);
    }
    const uint32_t idx = (uint32_t)(m.positions.size() / 3 - 1);
    vmap[key] = idx;
    return idx;
  };

  for (const auto& fc : faces) {
    const uint32_t i0 = addVertex(fc, 0);
    const uint32_t i1 = addVertex(fc, 1);
    const uint32_t i2 = addVertex(fc, 2);
    m.indices.push_back(i0);
    m.indices.push_back(i1);
    m.indices.push_back(i2);

    // face normal (for vertices without vn)
    const std::array<float, 3>& p0 = v[vIdx(fc.v[0])];
    const std::array<float, 3>& p1 = v[vIdx(fc.v[1])];
    const std::array<float, 3>& p2 = v[vIdx(fc.v[2])];
    std::array<float, 3> e1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    std::array<float, 3> e2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    std::array<float, 3> n{vCross(e1, e2)};
    const float l = vLen(n);
    if (l > 1e-8f) { n[0] /= l; n[1] /= l; n[2] /= l; }
    if (m.normals[i0 * 3] == 0 && m.normals[i0 * 3 + 1] == 0 && m.normals[i0 * 3 + 2] == 0) {
      m.normals[i0 * 3] = n[0]; m.normals[i0 * 3 + 1] = n[1]; m.normals[i0 * 3 + 2] = n[2];
    }
    if (m.normals[i1 * 3] == 0 && m.normals[i1 * 3 + 1] == 0 && m.normals[i1 * 3 + 2] == 0) {
      m.normals[i1 * 3] = n[0]; m.normals[i1 * 3 + 1] = n[1]; m.normals[i1 * 3 + 2] = n[2];
    }
    if (m.normals[i2 * 3] == 0 && m.normals[i2 * 3 + 1] == 0 && m.normals[i2 * 3 + 2] == 0) {
      m.normals[i2 * 3] = n[0]; m.normals[i2 * 3 + 1] = n[1]; m.normals[i2 * 3 + 2] = n[2];
    }
  }

  // tangent generation (per-vertex accumulate, standard Mikkelsen-ish);
  // tangents are vec4 per vertex, so the buffer is (N/3)*4 floats
  m.tangents.assign(m.positions.size() / 3 * 4, 0.0f);
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
    const float* pa = &m.positions[a * 3];
    const float* pb = &m.positions[b * 3];
    const float* pc = &m.positions[c * 3];
    const float* ua = &m.uvs[a * 2];
    const float* ub = &m.uvs[b * 2];
    const float* uc = &m.uvs[c * 2];
    const float e1x = pb[0] - pa[0], e1y = pb[1] - pa[1], e1z = pb[2] - pa[2];
    const float e2x = pc[0] - pa[0], e2y = pc[1] - pa[1], e2z = pc[2] - pa[2];
    const float du1 = ub[0] - ua[0], dv1 = ub[1] - ua[1];
    const float du2 = uc[0] - ua[0], dv2 = uc[1] - ua[1];
    float r = du1 * dv2 - du2 * dv1;
    if (std::fabs(r) < 1e-8f) r = 1e-8f;
    r = 1.0f / r;
    const float tx = (e1x * dv2 - e2x * dv1) * r;
    const float ty = (e1y * dv2 - e2y * dv1) * r;
    const float tz = (e1z * dv2 - e2z * dv1) * r;
    for (uint32_t k : {a, b, c}) {
      m.tangents[k * 4 + 0] += tx;
      m.tangents[k * 4 + 1] += ty;
      m.tangents[k * 4 + 2] += tz;
    }
  }
  const int vcount = (int)m.positions.size() / 3;
  for (int i = 0; i < vcount; i++) {
    std::array<float, 3> t{m.tangents[i * 4], m.tangents[i * 4 + 1], m.tangents[i * 4 + 2]};
    std::array<float, 3> n{m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]};
    std::array<float, 3> b = vCross(n, t);
    const float l = vLen(b);
    if (l > 1e-8f) { b[0] /= l; b[1] /= l; b[2] /= l; }
    m.tangents[i * 4 + 3] = vDot(vCross(t, n), b) < 0 ? -1.0f : 1.0f;
    const float tl = vLen(t);
    if (tl > 1e-8f) {
      m.tangents[i * 4] /= tl;
      m.tangents[i * 4 + 1] /= tl;
      m.tangents[i * 4 + 2] /= tl;
    }
  }

  m.upload();
  out.name = std::filesystem::path(path).stem().string();
  out.meshes.push_back(std::move(m));
  Log::info("MODEL", "loaded '" + path + "' (" + std::to_string(vcount) + " verts, " +
                         std::to_string(faces.size()) + " faces)");
  return true;
}

// ---------------------------------------------------------------------------
// ModelRenderer
// ---------------------------------------------------------------------------
void ModelRenderer::init(ShaderManager& sm) {
  prog_ = sm.get("lit.vert", "lit.frag");
  if (!prog_.ok()) throw std::runtime_error("lit shader failed to compile");
}

void ModelRenderer::drawMesh(const MeshPrimitive& m, const float* modelMatrix, const LightUniforms& lights,
                             float ambient, const Material* overrideMat) {
  if (!m.uploaded || m.indexCount == 0) return;
  const Material& mat = overrideMat ? *overrideMat : m.material;
  prog_.use();
  prog_.setMat4("uModel", modelMatrix);
  prog_.set4f("uBaseColor", mat.baseColor[0], mat.baseColor[1], mat.baseColor[2], mat.baseColor[3]);
  prog_.set1f("uMetallic", mat.metallic);
  prog_.set1f("uRoughness", mat.roughness);
  prog_.set1f("uAO", mat.ao);
  prog_.set3f("uEmission", mat.emission[0], mat.emission[1], mat.emission[2]);
  prog_.set1f("uOpacity", mat.opacity);
  prog_.set1f("uAmbient", ambient);

  const bool hasAlbedo = !mat.albedoMap.empty() && albedoTex_ != 0;
  const bool hasNormal = !mat.normalMap.empty() && normalTex_ != 0;
  prog_.set1f("uHasAlbedo", hasAlbedo ? 1.0f : 0.0f);
  prog_.set1f("uHasNormal", hasNormal ? 1.0f : 0.0f);
  if (hasAlbedo) {
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, albedoTex_);
    prog_.set1i("uAlbedo", 0);
  }
  if (hasNormal) {
    ::glActiveTexture(::gl::TEXTURE1);
    ::glBindTexture(::gl::TEXTURE_2D, normalTex_);
    prog_.set1i("uNormalMap", 1);
  }

  prog_.set1i("uLightCount", lights.count);
  for (int i = 0; i < lights.count; i++) {
    char nm[32];
    std::snprintf(nm, sizeof(nm), "uLightPos[%d]", i);
    prog_.setVec4(nm, lights.pos[(size_t)i].data());
    std::snprintf(nm, sizeof(nm), "uLightColor[%d]", i);
    prog_.setVec4(nm, lights.color[(size_t)i].data());
    std::snprintf(nm, sizeof(nm), "uLightType[%d]", i);
    prog_.set1i(nm, lights.type[(size_t)i]);
    std::snprintf(nm, sizeof(nm), "uLightRange[%d]", i);
    prog_.set1f(nm, lights.range[(size_t)i]);
    std::snprintf(nm, sizeof(nm), "uLightAngle[%d]", i);
    prog_.set1f(nm, lights.angle[(size_t)i]);
  }
  m.draw();
}

void ModelRenderer::drawModel(const Model& model, const float* modelMatrix, const LightUniforms& lights,
                              float ambient, const Material* overrideMat) {
  for (const auto& mesh : model.meshes) {
    drawMesh(mesh, modelMatrix, lights, ambient, overrideMat);
  }
}

}  // namespace ns
