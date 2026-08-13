// ---------------------------------------------------------------------------
// Shader Lab - demoscene typography-focused shader authoring workspace.
// The lab uses the engine's existing Shader/Texture/GL path for its preview;
// it does not introduce a second renderer. Preset shaders remain ordinary
// .frag assets and expose the same built-in uniforms used by runtime effects.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/assets.hpp"
#include "engine/framebuffer.hpp"
#include "engine/renderer.hpp"
#include "engine/shader.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ns {
class AudioEngine;
class Timeline;
}

namespace ns {

class ShaderLab {
public:
  struct Wiring {
    Renderer* renderer = nullptr;
    Assets* assets = nullptr;
    AudioEngine* audio = nullptr;
    Timeline* timeline = nullptr;
    std::string shaderDir;
    std::string assetDir;
    std::string dataDir;
  };

  ShaderLab() = default;
  ~ShaderLab();
  ShaderLab(const ShaderLab&) = delete;
  ShaderLab& operator=(const ShaderLab&) = delete;

  void init(const Wiring& wiring);
  void shutdown();
  void render(float time, float dt, int width, int height);
  void draw(float time);

  bool visible() const { return visible_; }
  void setVisible(bool v) { visible_ = v; }
  /** true once the user pressed Insert into Timeline; the editor consumes it
   *  after draw() and performs the document operation at a safe point. */
  bool takeInsertRequest();
  /** save the current source using the Shader Lab asset-name field; returns
   *  true when Ctrl+S was handled by the lab */
  bool saveCurrentAsset();
  std::string exportedShaderPath() const { return exportedPath_; }
  /** selected font asset used by the generated glyph layout, or empty for
   *  the engine default atlas; exported timeline commands use this path so
   *  playback renders the same font as the lab preview */
  std::string selectedFontPath() const;
  /** selected fill texture as a production-relative virtual path, or empty */
  std::string selectedTexturePath() const;
  float textureMix() const { return textureMix_; }
  float textureScale() const { return textureScale_; }
  float textureScroll() const { return textureScroll_; }
  std::string timelineSnippet() const;

private:
  struct Param {
    std::string name;
    std::string type;
    float min = 0, max = 1, value = 0, defaultValue = 0;
    float color[4] = {1, 1, 1, 1};
    float defaultColor[4] = {1, 1, 1, 1};
  };
  struct Preset { const char* name; const char* category; const char* description; int style; };

  Wiring w_;
  bool initialized_ = false;
  bool visible_ = false;
  bool insertRequest_ = false;
  bool sourceDirty_ = false;
  bool compileQueued_ = false;
  double lastEdit_ = 0;
  float previewTime_ = 0;
  float previewDt_ = 1.0f / 60.0f;
  int previewW_ = 960, previewH_ = 540;

  FrameTarget preview_;
  std::unique_ptr<Shader> program_;
  std::string previewFile_;
  std::string exportedPath_;
  std::string compileError_;
  std::string compileStatus_ = "not compiled";
  std::string savedSource_;
  std::string lastCompiledSource_;
  char saveNameBuf_[128] = "shaderlab_neon";
  std::string selectedPreset_ = "Neon";
  int presetIndex_ = 1;
  int fontIndex_ = 0;
  std::vector<std::string> fonts_;
  Assets selectedFont_;
  bool customFontLoaded_ = false;
  int textureIndex_ = 0;
  std::vector<std::string> texturePaths_;
  Texture fillTexture_;
  float textureMix_ = 1.0f;
  float textureScale_ = 1.0f;
  float textureScroll_ = 0.0f;

  std::string text_ = "NULL SECTOR\nDIGITAL HORIZON";
  std::vector<char> textBuf_;
  std::vector<char> sourceBuf_;
  static constexpr int kTextCap = 4096;
  static constexpr int kSourceCap = 131072;
  int fontSize_ = 104;
  float letterSpacing_ = 0.0f;
  float lineSpacing_ = 1.15f;
  int horizontalAlign_ = 1; // left, center, right
  int verticalAlign_ = 1;   // top, center, bottom
  float textPos_[2] = {0, 0};
  float textScale_ = 1.0f;
  float textRotation_ = 0.0f;
  float textOpacity_ = 1.0f;
  bool scrolling_ = false;
  bool wrap_ = false;
  float wrapWidth_ = 0.86f;
  float color_[4] = {0.05f, 0.95f, 0.85f, 1.0f};
  float color2_[4] = {0.35f, 0.15f, 1.0f, 1.0f};
  float intensity_ = 1.0f;
  float speed_ = 1.0f;
  float scale_ = 1.0f;
  float progress_ = 0.0f;
  std::vector<Param> params_;

  static const std::vector<Preset>& presets();
  void scanFonts();
  void scanTextures();
  void selectFont(int index);
  void selectTexture(int index);
  const Assets* activeFont() const;
  void rebuildTextBuffer();
  void rebuildSourceFromPreset();
  void parseMetadata();
  bool compile();
  bool ensurePreview(int w, int h);
  void bindUniforms(float time, float dt);
  std::string makeSource(int style) const;
  std::string shaderName() const;
  std::string sanitizeName(const std::string& in) const;
  bool saveAsset();
  void markEdited();
  void drawPreview(float time);
  void drawTextControls();
  void drawParameterControls();
  void drawSourceEditor();
  void restoreSource(const std::string& source, const char* status);
};

} // namespace ns
