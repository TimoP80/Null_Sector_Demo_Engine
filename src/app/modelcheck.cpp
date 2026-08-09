#include "app/modelcheck.hpp"
#include "app/appassets.hpp"
#include "app/model.hpp"
#include "app/shadermanager.hpp"
#include "engine/framebuffer.hpp"
#include "engine/math.hpp"
#include "engine/ubo.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ns {

namespace {

constexpr int kCheckSize = 256;

/** deterministic unit cube OBJ (v/vt/vn/f) - exercises indexed faces, UVs,
 *  normals, and the tangent generation path in the importer. */
std::string cubeObj() {
  return
      "# ns_check generated cube\n"
      "v -1 -1 -1\n  v  1 -1 -1\n  v  1  1 -1\n  v -1  1 -1\n"
      "v -1 -1  1\n  v  1 -1  1\n  v  1  1  1\n  v -1  1  1\n"
      "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
      "vn 0 0 -1\nvn 0 0 1\nvn 0 -1 0\nvn 0 1 0\nvn -1 0 0\nvn 1 0 0\n"
      // -z
      "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n"
      // +z
      "f 5/1/2 8/4/2 7/3/2\nf 5/1/2 7/3/2 6/2/2\n"
      // -y
      "f 1/1/3 5/2/3 6/3/3\nf 1/1/3 6/3/3 2/4/3\n"
      // +y
      "f 4/1/4 3/2/4 7/3/4\nf 4/1/4 7/3/4 8/4/4\n"
      // -x
      "f 1/1/5 4/2/5 8/3/5\nf 1/1/5 8/3/5 5/4/5\n"
      // +x
      "f 2/1/6 6/2/6 7/3/6\nf 2/1/6 7/3/6 3/4/6\n";
}

/** Small embedded GLB fixture: one triangle with POSITION + UNSIGNED_SHORT
 * indices. It exercises the binary header/chunks, JSON accessors, indices,
 * mesh upload, and the existing lit renderer without adding a repository asset. */
std::vector<uint8_t> triangleGlb() {
  std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":44}],"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
  while (json.size() & 3u) json.push_back(' ');
  std::vector<uint8_t> bin(44, 0);
  const float pos[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
  const uint16_t idx[] = {0, 1, 2};
  std::memcpy(bin.data(), pos, sizeof(pos));
  std::memcpy(bin.data() + 36, idx, sizeof(idx));
  std::vector<uint8_t> out;
  const auto u32 = [&out](uint32_t v) {
    out.push_back((uint8_t)(v & 255)); out.push_back((uint8_t)((v >> 8) & 255));
    out.push_back((uint8_t)((v >> 16) & 255)); out.push_back((uint8_t)((v >> 24) & 255));
  };
  u32(0x46546C67u); u32(2); u32((uint32_t)(12 + 8 + json.size() + 8 + bin.size()));
  u32((uint32_t)json.size()); u32(0x4E4F534Au);
  out.insert(out.end(), json.begin(), json.end());
  u32((uint32_t)bin.size()); u32(0x004E4942u);
  out.insert(out.end(), bin.begin(), bin.end());
  return out;
}

/** bounding sphere of a model, so the check camera always frames it */
void modelBounds(const Model& m, V3& center, float& radius) {
  V3 mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f};
  for (const auto& mesh : m.meshes) {
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
      mn[0] = std::min(mn[0], mesh.positions[i]);
      mn[1] = std::min(mn[1], mesh.positions[i + 1]);
      mn[2] = std::min(mn[2], mesh.positions[i + 2]);
      mx[0] = std::max(mx[0], mesh.positions[i]);
      mx[1] = std::max(mx[1], mesh.positions[i + 1]);
      mx[2] = std::max(mx[2], mesh.positions[i + 2]);
    }
  }
  center = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
  // radius over ALL meshes (a multi-mesh model must frame everything, not
  // just the first primitive - otherwise the camera clips geometry and the
  // readback false-fails)
  radius = 0.0f;
  for (const auto& mesh : m.meshes) {
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
      const float dx = mesh.positions[i] - center[0];
      const float dy = mesh.positions[i + 1] - center[1];
      const float dz = mesh.positions[i + 2] - center[2];
      radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
  }
}

/** fill the shared NullBlock UBO with a camera framed on the model */
void bindCheckCamera(SharedBlock& shared, const V3& center, float radius) {
  const float dist = std::max(radius * 2.5f, 0.5f);
  const Mat4 view = mat4FromBasis({1, 0, 0}, {0, 1, 0}, {0, 0, -1},
                                  {center[0], center[1], center[2] + dist});
  const Mat4 proj = mat4Perspective(1.0f, 1.0f, 0.05f, 400.0f);
  shared.data.fill(0);
  shared.data[OFF_UCAMPOS + 0] = center[0];
  shared.data[OFF_UCAMPOS + 1] = center[1];
  shared.data[OFF_UCAMPOS + 2] = center[2] + dist;
  for (int i = 0; i < 16; i++) {
    shared.data[OFF_UVIEW + i] = view[(size_t)i];
    shared.data[OFF_UPROJ + i] = proj[(size_t)i];
  }
  shared.commit();
}

/** draw a model into an offscreen color+depth target and verify the center
 *  pixel changed from the clear color - proves geometry import + upload +
 *  depth + lit shader + UBO + material override all actually ran. */
bool drawAndReadback(ModelRenderer& mr, const Model& model, const Material& mat,
                     SharedBlock& shared) {
  V3 center{0, 0, 0};
  float radius = 1.5f;
  modelBounds(model, center, radius);
  bindCheckCamera(shared, center, radius);

  LightUniforms lights;
  lights.addDir({0, 0, 1}, {1, 1, 1}, 1.2f);

  Mat4 modelMat{};
  modelMat[0] = modelMat[5] = modelMat[10] = modelMat[15] = 1.0f;

  FrameTarget target = FrameTarget::colorDepth(kCheckSize, kCheckSize);
  target.bind();
  if (::glCheckFramebufferStatus(::gl::FRAMEBUFFER) != ::gl::FRAMEBUFFER_COMPLETE) {
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
    Log::error("MODEL", "check framebuffer incomplete");
    return false;
  }
  // clear to pure green; the pass condition is DISTANCE from the clear color,
  // so no material color can false-fail (and a pixel that never drew stays
  // exactly at the clear color)
  ::glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  ::glClear(::gl::COLOR_BUFFER_BIT | ::gl::DEPTH_BUFFER_BIT);
  ::glEnable(::gl::DEPTH_TEST);
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::CULL_FACE);
  mr.drawModel(model, modelMat.data(), lights, 0.35f, &mat);
  ::glDisable(::gl::DEPTH_TEST);

  // sample center + 4 quadrant offsets; ANY pixel off the clear color proves
  // geometry drew (robust to edge-on / degenerate rasterization)
  const int off = kCheckSize / 8;
  const int samples[10] = {
      kCheckSize / 2,         kCheckSize / 2,
      kCheckSize / 2 + off,   kCheckSize / 2 + off,
      kCheckSize / 2 - off,   kCheckSize / 2 + off,
      kCheckSize / 2 + off,   kCheckSize / 2 - off,
      kCheckSize / 2 - off,   kCheckSize / 2 - off,
  };
  bool drew = false;
  for (size_t i = 0; i < 10; i += 2) {
    unsigned char px[4] = {0, 0, 0, 0};
    ::glReadPixels(samples[i], samples[i + 1], 1, 1, ::gl::RGBA, ::gl::UNSIGNED_BYTE, px);
    const int dr = (int)px[0] - 0;
    const int dg = (int)px[1] - 255;
    const int db = (int)px[2] - 0;
    if (dr * dr + dg * dg + db * db > 120 * 120) { drew = true; break; }
  }
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  if (!drew) Log::error("MODEL", "readback: all sampled pixels still the clear green - nothing drew");
  return drew;
}

}  // namespace

ModelCheckResult checkModelPipeline() {
  ModelCheckResult r;
  const auto check = [&](bool ok, const std::string& label) {
    r.total++;
    if (ok) {
      r.ok++;
      Log::info("MODEL", "ok: " + label);
    } else {
      r.failed++;
      r.failedItems.push_back(label);
      Log::error("MODEL", "FAIL: " + label);
    }
  };

  try {
    ShaderManager sm;
    ModelRenderer mr;
    try {
      mr.init(sm);  // throws if lit.vert + lit.frag don't compile
      check(true, "lit shader compiles");
    } catch (const std::exception& e) {
      check(false, std::string("lit shader compiles: ") + e.what());
      return r;  // nothing else works without the program
    }

    SharedBlock shared;

    // 1. generated cube through the whole chain (temp file, removed after)
    const std::string tmp = (std::filesystem::temp_directory_path() / "ns_check_cube.obj").string();
    {
      std::ofstream f(tmp);
      f << cubeObj();
      if (!f.good()) {
        check(false, "cannot write temp OBJ: " + tmp);
        return r;
      }
    }
    Model cube;
    const bool cubeOk = ObjImporter::load(tmp, cube);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    const bool cubeUploaded = cubeOk && !cube.meshes.empty() && cube.meshes[0].uploaded &&
                              cube.meshes[0].indexCount > 0;
    check(cubeUploaded, "generated cube imports + uploads");
    if (!cubeUploaded) return r;

    Model glb;
    const bool glbOk = GlbImporter::loadBytes(triangleGlb(), glb);
    const bool glbUploaded = glbOk && glb.meshes.size() == 1 &&
                             glb.meshes[0].uploaded && glb.meshes[0].indexCount == 3;
    check(glbUploaded, "embedded GLB imports + uploads");

    Material bright;  // visible under the check light
    bright.baseColor = {0.2f, 0.6f, 1.0f, 1.0f};
    bright.emission = {0.1f, 0.1f, 0.3f};
    check(drawAndReadback(mr, cube, bright, shared), "generated cube draws (pixel readback)");
    if (glbUploaded)
      check(drawAndReadback(mr, glb, bright, shared), "embedded GLB draws (pixel readback)");

    // 2. shipped demo models (the ones demo.nsd actually uses)
    const std::string data = AppAssets::dataDir();
    for (const char* f : {"terrain.obj", "gem.obj"}) {
      const std::string path = data + "/models/" + f;
      if (!std::filesystem::exists(path)) {
        check(false, std::string("shipped model missing: ") + f);
        continue;
      }
      Model m;
      if (!ObjImporter::load(path, m) || m.meshes.empty()) {
        check(false, std::string("shipped model imports: ") + f);
        continue;
      }
      check(true, std::string("shipped model imports: ") + f);
      check(drawAndReadback(mr, m, bright, shared), std::string("shipped model draws: ") + f);
    }

    // 3. shipped materials parse + bind as per-node overrides
    for (const char* f : {"chrome.json", "neon.json"}) {
      const std::string path = data + "/materials/" + f;
      if (!std::filesystem::exists(path)) {
        check(false, std::string("shipped material missing: ") + f);
        continue;
      }
      try {
        const Material mat = Material::fromJson(Json::parseFile(path));
        check(true, std::string("material loads: ") + f);
        check(drawAndReadback(mr, cube, mat, shared), std::string("material override draws: ") + f);
      } catch (const std::exception& e) {
        check(false, std::string("material load: ") + f + " (" + e.what() + ")");
      }
    }
  } catch (const std::exception& e) {
    check(false, std::string("pipeline exception: ") + e.what());
  }
  return r;
}

}  // namespace ns
