// ---------------------------------------------------------------------------
// AppAssets - registers the GL-backed asset kinds into an AssetManager:
//
//   texture   PNG/JPEG -> Texture* (stb_image)
//   model     OBJ -> Model* (ObjImporter; Assimp glTF/FBX plugs in here)
//   material  JSON -> Material*
//   scene     JSON -> SceneGraph
//   timeline  JSON -> TimelineEditor
//   script    text -> ScriptEngine
//   shader    (ShaderManager owns programs; the kind tracks the source file
//              so live reload recompiles)
//
// Handles are opaque void*; each loader returns a heap object the FreeFn
// deletes. Reload functions swap the object in place (textures re-upload,
// models re-import, materials re-parse) and bump the version.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/resources/assetmanager.hpp"
#include "app/model.hpp"
#include "app/shadermanager.hpp"
#include "framework/scene/scenegraph.hpp"
#include "framework/script/scriptengine.hpp"
#include "framework/timeline/timelineeditor.hpp"

#include <string>

namespace ns {

class AppAssets {
public:
  /** register all kinds; shader dir is needed for shader sources */
  static void init(AssetManager& am);

  // --- loaders (public for direct use) --------------------------------------
  static Texture* loadTexture(const std::string& path);
  static Model* loadModel(const std::string& path);
  static Material* loadMaterial(const std::string& path);
  static SceneGraph* loadScene(const std::string& path);
  static TimelineEditor* loadTimeline(const std::string& path);
  static ScriptEngine* loadScript(const std::string& path);

  /** data dir (data/...) with the standard runtime resolution */
  static std::string dataDir();
  static std::string shaderDir();
};

}  // namespace ns
