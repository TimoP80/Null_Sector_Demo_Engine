#include "app/model.hpp"
#include "framework/vfs/vfs.hpp"
#include "engine/math.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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

namespace {

struct GlbAccessor {
  int view = -1;
  size_t offset = 0;
  size_t count = 0;
  int componentType = 0;
  int components = 1;
  bool normalized = false;
  size_t stride = 0;
};

int glbComponentSize(int type) {
  switch (type) {
    case 5120: case 5121: return 1;  // BYTE / UNSIGNED_BYTE
    case 5122: case 5123: return 2;  // SHORT / UNSIGNED_SHORT
    case 5125: case 5126: return 4;  // UNSIGNED_INT / FLOAT
    default: return 0;
  }
}

int glbComponentCount(const std::string& type) {
  if (type == "SCALAR") return 1;
  if (type == "VEC2") return 2;
  if (type == "VEC3") return 3;
  if (type == "VEC4") return 4;
  if (type == "MAT2") return 4;
  if (type == "MAT3") return 9;
  if (type == "MAT4") return 16;
  return 0;
}

bool glbRange(size_t offset, size_t size, size_t total) {
  return offset <= total && size <= total - offset;
}

bool glbAccessor(const Value& root, const std::vector<uint8_t>& bin, int index,
                 GlbAccessor& out, std::string& err) {
  const Value& accessors = root.get("accessors");
  const Value& views = root.get("bufferViews");
  const Value& a = accessors.atIndex(index < 0 ? (size_t)-1 : (size_t)index);
  if (!a.isObj()) { err = "accessor index out of range"; return false; }
  out.view = a.get("bufferView").asInt(-1);
  out.offset = (size_t)std::max(0, a.get("byteOffset").asInt(0));
  out.count = (size_t)std::max(0, a.get("count").asInt(0));
  out.componentType = a.get("componentType").asInt(0);
  out.components = glbComponentCount(a.get("type").asStr());
  out.normalized = a.get("normalized").asBool(false);
  const int componentSize = glbComponentSize(out.componentType);
  if (out.view < 0 || out.components == 0 || componentSize == 0 ||
      out.count == 0) {
    err = "unsupported or empty accessor";
    return false;
  }
  const Value& v = views.atIndex((size_t)out.view);
  if (!v.isObj()) { err = "bufferView index out of range"; return false; }
  const size_t viewOffset = (size_t)std::max(0, v.get("byteOffset").asInt(0));
  const size_t viewLength = (size_t)std::max(0, v.get("byteLength").asInt(0));
  out.stride = (size_t)std::max(0, v.get("byteStride").asInt(0));
  const size_t elementSize = (size_t)componentSize * (size_t)out.components;
  if (out.stride == 0) out.stride = elementSize;
  if (out.stride < elementSize || viewOffset > bin.size() ||
      viewLength > bin.size() - viewOffset ||
      out.count > 0 && (out.count - 1) > (SIZE_MAX - out.offset - elementSize) / out.stride ||
      out.offset + (out.count - 1) * out.stride + elementSize > viewLength) {
    err = "accessor exceeds binary bufferView";
    return false;
  }
  out.offset += viewOffset;
  return true;
}

float glbComponent(const uint8_t* p, int type, bool normalized) {
  switch (type) {
    case 5126: {
      float v = 0; std::memcpy(&v, p, sizeof(v)); return v;
    }
    case 5120: {
      int8_t v = 0; std::memcpy(&v, p, sizeof(v));
      return normalized ? std::max(-1.0f, (float)v / 127.0f) : (float)v;
    }
    case 5121: {
      uint8_t v = *p; return normalized ? (float)v / 255.0f : (float)v;
    }
    case 5122: {
      int16_t v = 0; std::memcpy(&v, p, sizeof(v));
      return normalized ? std::max(-1.0f, (float)v / 32767.0f) : (float)v;
    }
    case 5123: {
      uint16_t v = 0; std::memcpy(&v, p, sizeof(v));
      return normalized ? (float)v / 65535.0f : (float)v;
    }
    case 5125: {
      uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); return (float)v;
    }
    default: return 0.0f;
  }
}

bool glbFloats(const Value& root, const std::vector<uint8_t>& bin, int index,
               int wanted, std::vector<float>& out, std::string& err) {
  GlbAccessor a;
  if (!glbAccessor(root, bin, index, a, err) || a.components < wanted) return false;
  const int bytes = glbComponentSize(a.componentType);
  out.resize(a.count * (size_t)wanted);
  for (size_t i = 0; i < a.count; ++i) {
    const uint8_t* base = bin.data() + a.offset + i * a.stride;
    for (int c = 0; c < wanted; ++c)
      out[i * (size_t)wanted + (size_t)c] = glbComponent(base + c * bytes,
                                                           a.componentType,
                                                           a.normalized);
  }
  return true;
}

bool glbIndices(const Value& root, const std::vector<uint8_t>& bin, int index,
                std::vector<uint32_t>& out, std::string& err) {
  GlbAccessor a;
  if (!glbAccessor(root, bin, index, a, err) || a.components != 1) return false;
  const int bytes = glbComponentSize(a.componentType);
  if (a.componentType != 5121 && a.componentType != 5123 && a.componentType != 5125) {
    err = "indices must use an unsigned integer component type";
    return false;
  }
  out.resize(a.count);
  for (size_t i = 0; i < a.count; ++i)
    out[i] = (uint32_t)glbComponent(bin.data() + a.offset + i * a.stride,
                                     a.componentType, false);
  (void)bytes;
  return true;
}

struct GlbMat {
  float m[16];
};

GlbMat glbIdentity() {
  GlbMat r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r;
}

GlbMat glbMul(const GlbMat& a, const GlbMat& b) {
  GlbMat r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
  return r;
}

GlbMat glbNodeMatrix(const Value& node) {
  const Value& matrix = node.get("matrix");
  if (matrix.isArr() && matrix.size() >= 16) {
    GlbMat r{};
    for (int i = 0; i < 16; ++i) r.m[i] = matrix.atIndex((size_t)i).asFloat(i % 5 == 0 ? 1.0f : 0.0f);
    return r;
  }
  GlbMat r = glbIdentity();
  const Value& t = node.get("translation");
  if (t.isArr() && t.size() >= 3) {
    r.m[12] = t.atIndex(0).asFloat(); r.m[13] = t.atIndex(1).asFloat(); r.m[14] = t.atIndex(2).asFloat();
  }
  Q4 q{0, 0, 0, 1};
  const Value& rv = node.get("rotation");
  if (rv.isArr() && rv.size() >= 4)
    q = {rv.atIndex(0).asFloat(), rv.atIndex(1).asFloat(), rv.atIndex(2).asFloat(), rv.atIndex(3).asFloat()};
  const Mat3 qm = mat3FromQuat(q);
  GlbMat rot = glbIdentity();
  for (int i = 0; i < 9; ++i) rot.m[(i / 3) * 4 + (i % 3)] = qm[i];
  const Value& sv = node.get("scale");
  V3 scale{1, 1, 1};
  if (sv.isArr() && sv.size() >= 3) scale = {sv.atIndex(0).asFloat(1), sv.atIndex(1).asFloat(1), sv.atIndex(2).asFloat(1)};
  GlbMat scl = glbIdentity(); scl.m[0] = scale[0]; scl.m[5] = scale[1]; scl.m[10] = scale[2];
  return glbMul(glbMul(r, rot), scl);
}

void glbTransform(MeshPrimitive& mesh, const GlbMat& m) {
  for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
    const float x = mesh.positions[i], y = mesh.positions[i + 1], z = mesh.positions[i + 2];
    mesh.positions[i] = m.m[0] * x + m.m[4] * y + m.m[8] * z + m.m[12];
    mesh.positions[i + 1] = m.m[1] * x + m.m[5] * y + m.m[9] * z + m.m[13];
    mesh.positions[i + 2] = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
  }
  for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3) {
    V3 n{m.m[0] * mesh.normals[i] + m.m[4] * mesh.normals[i + 1] + m.m[8] * mesh.normals[i + 2],
         m.m[1] * mesh.normals[i] + m.m[5] * mesh.normals[i + 1] + m.m[9] * mesh.normals[i + 2],
         m.m[2] * mesh.normals[i] + m.m[6] * mesh.normals[i + 1] + m.m[10] * mesh.normals[i + 2]};
    n = vNorm(n);
    mesh.normals[i] = n[0]; mesh.normals[i + 1] = n[1]; mesh.normals[i + 2] = n[2];
  }
}

Material glbMaterial(const Value& root, int index) {
  Material m;
  const Value& mats = root.get("materials");
  const Value& src = mats.atIndex(index < 0 ? (size_t)-1 : (size_t)index);
  if (!src.isObj()) return m;
  const Value& pbr = src.get("pbrMetallicRoughness");
  float f[4];
  if (pbr.get("baseColorFactor").toFloats(f, 4) == 4) m.baseColor = {f[0], f[1], f[2], f[3]};
  m.metallic = pbr.get("metallicFactor").asFloat(m.metallic);
  m.roughness = pbr.get("roughnessFactor").asFloat(m.roughness);
  if (src.get("emissiveFactor").toFloats(f, 3) == 3) m.emission = {f[0], f[1], f[2]};
  m.opacity = m.baseColor[3];
  return m;
}

}  // namespace

bool GlbImporter::load(const std::string& path, Model& out) {
  std::vector<uint8_t> bytes = runtimeFS().read(path);
  if (bytes.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path, std::ios::binary);
      if (f) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
  }
  if (bytes.empty()) {
    Log::error("MODEL", "cannot open GLB: " + path);
    return false;
  }
  return loadBytes(bytes, out, path);
}

bool GlbImporter::loadBytes(const std::vector<uint8_t>& bytes, Model& out,
                            const std::string& label) {
  if (bytes.size() < 12) { Log::error("MODEL", "GLB header truncated: " + label); return false; }
  auto u32 = [&](size_t off) -> uint32_t {
    uint32_t v = 0; std::memcpy(&v, bytes.data() + off, sizeof(v)); return v;
  };
  if (u32(0) != 0x46546C67u || u32(4) != 2u) {
    Log::error("MODEL", "unsupported GLB header: " + label); return false;
  }
  const size_t total = u32(8);
  if (total < 12 || total > bytes.size()) {
    Log::error("MODEL", "invalid GLB length: " + label); return false;
  }
  std::string json;
  std::vector<uint8_t> bin;
  size_t off = 12;
  while (off + 8 <= total) {
    const size_t len = u32(off);
    const uint32_t type = u32(off + 4);
    off += 8;
    if (!glbRange(off, len, total)) { Log::error("MODEL", "GLB chunk exceeds file: " + label); return false; }
    if (type == 0x4E4F534Au) json.assign((const char*)bytes.data() + off, len);
    else if (type == 0x004E4942u) bin.assign(bytes.begin() + (ptrdiff_t)off, bytes.begin() + (ptrdiff_t)(off + len));
    off += len;
  }
  while (!json.empty() && (json.back() == '\0' || json.back() == ' ' || json.back() == '\n' || json.back() == '\r' || json.back() == '\t')) json.pop_back();
  if (json.empty() || bin.empty()) { Log::error("MODEL", "GLB needs JSON and BIN chunks: " + label); return false; }

  try {
    const Value root = Json::parseText(json);
    const Value& meshes = root.get("meshes");
    if (!meshes.isArr() || meshes.size() == 0) throw std::runtime_error("GLB has no meshes");
    out.meshes.clear();
    out.name = std::filesystem::path(label).stem().string();
    const auto emitMesh = [&](int meshIndex, const GlbMat& world) {
      if (meshIndex < 0 || (size_t)meshIndex >= meshes.size()) return;
      const Value& meshDef = meshes.atIndex((size_t)meshIndex);
      const Value& primitives = meshDef.get("primitives");
      if (!primitives.isArr()) return;
      for (const auto& primitive : primitives.asArr()) {
        if (!primitive.isObj()) continue;
        const int mode = primitive.get("mode").asInt(4);
        if (mode != 4 && mode != 5 && mode != 6) {
          Log::warn("MODEL", "GLB primitive mode is not triangles/strip/fan; skipped");
          continue;
        }
        const Value& attrs = primitive.get("attributes");
        const int posIndex = attrs.get("POSITION").asInt(-1);
        std::string err;
        GlbAccessor posInfo;
        if (posIndex < 0 || !glbAccessor(root, bin, posIndex, posInfo, err) || posInfo.components != 3) {
          Log::warn("MODEL", "GLB primitive has no valid POSITION: " + err);
          continue;
        }
        MeshPrimitive m;
        if (!glbFloats(root, bin, posIndex, 3, m.positions, err)) { Log::warn("MODEL", err); continue; }
        const int normalIndex = attrs.get("NORMAL").asInt(-1);
        const int uvIndex = attrs.get("TEXCOORD_0").asInt(-1);
        if (normalIndex >= 0) glbFloats(root, bin, normalIndex, 3, m.normals, err);
        if (uvIndex >= 0) glbFloats(root, bin, uvIndex, 2, m.uvs, err);
        const size_t vcount = m.positions.size() / 3;
        if (m.normals.size() != vcount * 3) m.normals.assign(vcount * 3, 0.0f);
        if (m.uvs.size() != vcount * 2) m.uvs.assign(vcount * 2, 0.0f);
        std::vector<uint32_t> src;
        const int indexAccessor = primitive.get("indices").asInt(-1);
        if (indexAccessor >= 0 && !glbIndices(root, bin, indexAccessor, src, err)) { Log::warn("MODEL", err); continue; }
        if (src.empty()) { src.resize(vcount); for (size_t i = 0; i < vcount; ++i) src[i] = (uint32_t)i; }
        if (mode == 4) m.indices = std::move(src);
        else if (mode == 6) {
          for (size_t i = 1; i + 1 < src.size(); ++i) { m.indices.push_back(src[0]); m.indices.push_back(src[i]); m.indices.push_back(src[i + 1]); }
        } else {
          for (size_t i = 2; i < src.size(); ++i) {
            const uint32_t a = src[i - 2], b = src[i - 1], c = src[i];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { m.indices.insert(m.indices.end(), {b, a, c}); }
            else { m.indices.insert(m.indices.end(), {a, b, c}); }
          }
        }
        if (m.indices.empty() || std::any_of(m.indices.begin(), m.indices.end(),
                                              [vcount](uint32_t i) { return (size_t)i >= vcount; })) {
          Log::warn("MODEL", "GLB primitive index is outside POSITION accessor");
          continue;
        }
        const int materialIndex = primitive.get("material").asInt(-1);
        m.material = glbMaterial(root, materialIndex);
        glbTransform(m, world);
        m.upload();
        if (m.indexCount > 0) out.meshes.push_back(std::move(m));
      }
    };

    const Value& nodes = root.get("nodes");
    std::function<void(int, const GlbMat&)> visit = [&](int nodeIndex, const GlbMat& parent) {
      const Value& node = nodes.atIndex(nodeIndex < 0 ? (size_t)-1 : (size_t)nodeIndex);
      if (!node.isObj()) return;
      const GlbMat world = glbMul(parent, glbNodeMatrix(node));
      const int meshIndex = node.get("mesh").asInt(-1);
      if (meshIndex >= 0) emitMesh(meshIndex, world);
      const Value& children = node.get("children");
      if (children.isArr()) for (const auto& child : children.asArr()) visit(child.asInt(-1), world);
    };
    const Value& scenes = root.get("scenes");
    const int sceneIndex = root.get("scene").asInt(0);
    bool visitedNodes = false;
    if (nodes.isArr() && scenes.isArr() && sceneIndex >= 0 && (size_t)sceneIndex < scenes.size()) {
      const Value& roots = scenes.atIndex((size_t)sceneIndex).get("nodes");
      if (roots.isArr()) {
        for (const auto& rootNode : roots.asArr()) { visit(rootNode.asInt(-1), glbIdentity()); visitedNodes = true; }
      }
    }
    if (!visitedNodes) for (size_t i = 0; i < meshes.size(); ++i) emitMesh((int)i, glbIdentity());
    if (out.meshes.empty()) throw std::runtime_error("GLB contains no renderable triangle primitives");
    size_t vertices = 0;
    for (const auto& m : out.meshes) vertices += m.positions.size() / 3;
    Log::info("MODEL", "loaded GLB '" + label + "' (" + std::to_string(out.meshes.size()) +
                         " primitives, " + std::to_string(vertices) + " verts)");
    return true;
  } catch (const std::exception& e) {
    Log::error("MODEL", "GLB parse failed: " + label + " (" + e.what() + ")");
    out.meshes.clear();
    return false;
  }
}

bool ObjImporter::load(const std::string& path, Model& out) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext) c = (char)std::tolower((unsigned char)c);
  if (ext == ".glb") return GlbImporter::load(path, out);
  std::string text = runtimeFS().readText(path);
  if (text.empty()) {
    // absolute editor path: direct read
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path);
      if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
      }
    }
  }
  if (text.empty()) {
    Log::error("MODEL", "cannot open model: " + path);
    return false;
  }
  return loadText(text, out, path);
}

bool ObjImporter::loadText(const std::string& text, Model& out,
                             const std::string& label) {
  std::istringstream in(text);
  std::vector<std::array<float, 3>> v, vn;
  std::vector<std::array<float, 2>> vt;
  std::vector<ObjFace> faces;

  std::string line;
  while (std::getline(in, line)) {
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
    Log::error("MODEL", "OBJ has no faces: " + label);
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
  out.name = std::filesystem::path(label).stem().string();
  out.meshes.push_back(std::move(m));
  Log::info("MODEL", "loaded '" + label + "' (" + std::to_string(vcount) + " verts, " +
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
