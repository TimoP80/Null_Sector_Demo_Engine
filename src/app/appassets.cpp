#include "app/appassets.hpp"
#include "engine/paths.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"

#include <fstream>
#include <sstream>

// declarations only - the implementation lives in engine/assets.cpp
#include "stb_image.h"

namespace ns {

std::string AppAssets::dataDir() {
  return resolveRuntimeDir("NULLSECTOR_DATA_DIR", NULLSECTOR_DATA_DIR, "data");
}
std::string AppAssets::shaderDir() {
  return resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders");
}

void AppAssets::init(AssetManager& am) {
  // texture: stb_image PNG/JPEG -> Texture (RGBA8, linear)
  am.registerKind(
      "texture",
      [](const std::string& path) -> void* {
        return loadTexture(path);
      },
      [](void* h) { delete static_cast<Texture*>(h); },
      [](const std::string& path, void*& h) {
        Texture* fresh = loadTexture(path);
        if (!fresh) return false;
        delete static_cast<Texture*>(h);
        h = fresh;
        return true;
      });

  // model: OBJ -> Model
  am.registerKind(
      "model",
      [](const std::string& path) -> void* { return loadModel(path); },
      [](void* h) { delete static_cast<Model*>(h); },
      [](const std::string& path, void*& h) {
        Model* fresh = loadModel(path);
        if (!fresh) return false;
        delete static_cast<Model*>(h);
        h = fresh;
        return true;
      });

  // material: JSON -> Material
  am.registerKind(
      "material",
      [](const std::string& path) -> void* { return loadMaterial(path); },
      [](void* h) { delete static_cast<Material*>(h); },
      [](const std::string& path, void*& h) {
        Material* fresh = loadMaterial(path);
        if (!fresh) return false;
        delete static_cast<Material*>(h);
        h = fresh;
        return true;
      });

  // scene: JSON -> SceneGraph
  am.registerKind(
      "scene",
      [](const std::string& path) -> void* { return loadScene(path); },
      [](void* h) { delete static_cast<SceneGraph*>(h); });

  // timeline: JSON -> TimelineEditor
  am.registerKind(
      "timeline",
      [](const std::string& path) -> void* { return loadTimeline(path); },
      [](void* h) { delete static_cast<TimelineEditor*>(h); });

  // script: text -> ScriptEngine (no GL state; version tracks reloads)
  am.registerKind(
      "script",
      [](const std::string& path) -> void* { return loadScript(path); },
      [](void* h) { delete static_cast<ScriptEngine*>(h); });
}

Texture* AppAssets::loadTexture(const std::string& path) {
  int w = 0, h = 0, comp = 0;
  unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (!px) {
    Log::error("ASSET", "texture load failed: " + path + " (" + (stbi_failure_reason() ? stbi_failure_reason() : "?") + ")");
    return nullptr;
  }
  auto tex = new Texture();
  *tex = Texture::fromRGBA(w, h, px, {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, true});
  stbi_image_free(px);
  Log::info("ASSET", "texture '" + path + "' (" + std::to_string(w) + "x" + std::to_string(h) + ")");
  return tex;
}

Model* AppAssets::loadModel(const std::string& path) {
  auto m = new Model();
  if (!ObjImporter::load(path, *m)) {
    delete m;
    return nullptr;
  }
  return m;
}

Material* AppAssets::loadMaterial(const std::string& path) {
  try {
    const Value v = Json::parseFile(path);
    auto m = new Material();
    *m = Material::fromJson(v);
    return m;
  } catch (const std::exception& e) {
    Log::error("ASSET", "material load failed: " + path + " (" + e.what() + ")");
    return nullptr;
  }
}

SceneGraph* AppAssets::loadScene(const std::string& path) {
  try {
    const Value v = Json::parseFile(path);
    auto g = new SceneGraph();
    g->fromJson(v);
    return g;
  } catch (const std::exception& e) {
    Log::error("ASSET", "scene load failed: " + path + " (" + e.what() + ")");
    return nullptr;
  }
}

TimelineEditor* AppAssets::loadTimeline(const std::string& path) {
  try {
    const Value v = Json::parseFile(path);
    auto t = new TimelineEditor();
    t->fromJson(v);
    return t;
  } catch (const std::exception& e) {
    Log::error("ASSET", "timeline load failed: " + path + " (" + e.what() + ")");
    return nullptr;
  }
}

ScriptEngine* AppAssets::loadScript(const std::string& path) {
  auto s = new ScriptEngine();
  if (!s->load(path)) {
    delete s;
    return nullptr;
  }
  return s;
}

}  // namespace ns
