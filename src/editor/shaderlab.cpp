#include "editor/shaderlab.hpp"

#include "engine/audio.hpp"
#include "engine/timeline.hpp"
#include "framework/core/log.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ns {
namespace {

const char* fontGetter(void* data, int index) {
  auto* fonts = static_cast<std::vector<std::string>*>(data);
  if (!fonts || index < 0 || index >= (int)fonts->size()) return nullptr;
  return (*fonts)[(size_t)index].empty() ? "None" : (*fonts)[(size_t)index].c_str();
}

double wallNowLab() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    const size_t e = s.find('\n', p);
    std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
    if (e == std::string::npos) break;
    p = e + 1;
  }
  if (out.empty()) out.push_back("");
  return out;
}

std::string glslFloat(float v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.6g", (double)v);
  return b;
}

} // namespace

const std::vector<ShaderLab::Preset>& ShaderLab::presets() {
  static const std::vector<Preset> p = {
      {"CRT / Terminal", "CRT", "Phosphor scanlines, flicker, glow and film noise", 0},
      {"Neon", "Text", "Late-night signage with bloom and chromatic edges", 1},
      {"Glitch", "Glitch", "RGB separation, tearing and digital corruption", 2},
      {"Hologram", "Hologram", "Transparent projection with scan bands", 3},
      {"Chrome", "Chrome", "Reflective metallic typography", 4},
      {"Fire", "Fire", "Hot noise, embers and a dissolving edge", 5},
      {"Plasma", "Plasma", "Classic flowing demoscene plasma", 6},
      {"Electric", "Audio Reactive", "Beat-driven electric outline", 7},
      {"Matrix", "Classic Demoscene", "Falling data energy behind the letters", 8},
      {"ASCII", "Classic Demoscene", "Posterized terminal blocks", 9},
      {"Wireframe", "Abstract", "Glowing contour treatment", 10},
      {"Particle Text", "Particles", "Spark-like mask breakup", 11},
  };
  return p;
}

ShaderLab::~ShaderLab() { shutdown(); }

void ShaderLab::init(const Wiring& wiring) {
  w_ = wiring;
  initialized_ = true;
  previewFile_ = w_.shaderDir + "/.shaderlab_preview.frag";
  textBuf_.assign(text_.begin(), text_.end());
  textBuf_.push_back(0);
  textBuf_.resize(kTextCap + 1, 0);
  scanFonts();
  scanTextures();
  rebuildSourceFromPreset();
  compile();
  std::snprintf(saveNameBuf_, sizeof(saveNameBuf_), "%s", shaderName().c_str());
}

void ShaderLab::shutdown() {
  if (!initialized_) return;
  program_.reset();
  preview_.destroy();
  selectedFont_.fontTex.destroy();
  fillTexture_.destroy();
  std::error_code ec;
  if (!previewFile_.empty()) std::filesystem::remove(previewFile_, ec);
  initialized_ = false;
}

void ShaderLab::scanFonts() {
  fonts_.clear();
  fonts_.push_back("Engine default");
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(w_.assetDir) / "fonts";
  if (std::filesystem::is_directory(dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (ec || !e.is_regular_file()) continue;
      std::string ext = e.path().extension().string();
      for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (ext == ".ttf" || ext == ".otf") fonts_.push_back(e.path().string());
    }
  }
  std::sort(fonts_.begin() + 1, fonts_.end());
  fontIndex_ = 0;
}

void ShaderLab::scanTextures() {
  texturePaths_.clear();
  texturePaths_.push_back(std::string());  // None / solid-color fill
  std::vector<std::filesystem::path> roots;
  roots.emplace_back(std::filesystem::path(w_.assetDir) / "textures");
  roots.emplace_back(std::filesystem::path(w_.dataDir) / "textures");
  std::error_code ec;
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root, ec)) { ec.clear(); continue; }
    for (const auto& e : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) { ec.clear(); break; }
      if (!e.is_regular_file(ec)) { ec.clear(); continue; }
      std::string ext = e.path().extension().string();
      for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
        texturePaths_.push_back(std::filesystem::absolute(e.path()).string());
    }
  }
  std::sort(texturePaths_.begin() + 1, texturePaths_.end());
  texturePaths_.erase(std::unique(texturePaths_.begin() + 1, texturePaths_.end()),
                      texturePaths_.end());
  textureIndex_ = 0;
}

void ShaderLab::selectTexture(int index) {
  if (index < 0 || index >= (int)texturePaths_.size()) return;
  fillTexture_.destroy();
  textureIndex_ = index;
  if (!texturePaths_[(size_t)index].empty()) {
    if (!loadPngAsset(texturePaths_[(size_t)index], fillTexture_)) {
      Log::warn("SHADERLAB", "texture load failed; using solid text fill");
      textureIndex_ = 0;
    }
  }
}

void ShaderLab::selectFont(int index) {
  if (index < 0 || index >= (int)fonts_.size()) return;
  selectedFont_.fontTex.destroy();
  customFontLoaded_ = false;
  fontIndex_ = index;
  if (fontIndex_ > 0) {
    selectedFont_ = buildTrueTypeFontAtlas(fonts_[(size_t)fontIndex_]);
    customFontLoaded_ = selectedFont_.fontTex.tex != 0;
    if (!customFontLoaded_) {
      fontIndex_ = 0;
      Log::warn("SHADERLAB", "font load failed; using engine default font");
    }
  }
}

const Assets* ShaderLab::activeFont() const {
  return customFontLoaded_ ? &selectedFont_ : w_.assets;
}

void ShaderLab::rebuildTextBuffer() {
  textBuf_.assign(text_.begin(), text_.end());
  textBuf_.push_back(0);
  textBuf_.resize(kTextCap + 1, 0);
}

std::string ShaderLab::sanitizeName(const std::string& in) const {
  std::string out;
  for (char c : in) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out += c;
  }
  return out.empty() ? "effect" : out;
}

std::string ShaderLab::shaderName() const {
  if (presetIndex_ >= 0 && presetIndex_ < (int)presets().size())
    return "shaderlab_" + sanitizeName(presets()[(size_t)presetIndex_].name);
  return "shaderlab_custom";
}

// Build a shader with the selected text laid out as explicit glyph rectangles.
// This keeps the runtime lightweight: exported shaders only need the normal
// engine font atlas and a handful of uniforms, not a second text renderer.
std::string ShaderLab::makeSource(int style) const {
  const Assets* a = activeFont();
  const FontMetrics fm = a ? a->fontMetrics : FontMetrics{};
  const float refH = 540.0f;
  const float h = std::max(8, fontSize_) / refH;
  const float w = h * (float)fm.cellW / std::max(1, fm.cellH);
  const float spacing = letterSpacing_ / refH;
  const float lineH = h * std::max(0.6f, lineSpacing_);
  std::vector<std::string> lines = splitLines(text_);
  if (wrap_) {
    const int maxChars = std::max(1, (int)(wrapWidth_ / std::max(0.001f, w + spacing)));
    std::vector<std::string> wrapped;
    for (const std::string& line : lines) {
      if ((int)line.size() <= maxChars) { wrapped.push_back(line); continue; }
      for (size_t p = 0; p < line.size(); p += (size_t)maxChars)
        wrapped.push_back(line.substr(p, (size_t)maxChars));
    }
    lines = std::move(wrapped);
  }
  float maxWidth = 0;
  for (const std::string& line : lines)
    maxWidth = std::max(maxWidth, (float)line.size() * (w + spacing));
  const float totalH = lineH * (float)lines.size();
  const float top = verticalAlign_ == 0 ? 0.08f
                     : verticalAlign_ == 2 ? 0.92f - totalH
                                           : 0.5f + totalH * 0.5f;

  std::ostringstream s;
  s << "#version 300 es\n"
       "// Shader Lab generated typography shader. Edit freely.\n"
       "// @param glow float 0.0 8.0 2.0\n"
       "// @param distortion float 0.0 1.0 0.12\n"
       "// @param scanlines float 0.0 1.0 0.35\n"
       "uniform vec2 uResolution;\n"
       "uniform float uTime;\n"
       "uniform float uDeltaTime;\n"
       "uniform float uProgress;\n"
       "uniform float uBPM;\n"
       "uniform float uBeat;\n"
       "uniform float uBar;\n"
       "uniform float uBeatPhase;\n"
       "uniform float uAudioLevel;\n"
       "uniform float uBass;\n"
       "uniform float uMid;\n"
       "uniform float uTreble;\n"
       "uniform float uKick;\n"
       "uniform float uSnare;\n"
       "uniform sampler2D uText;\n"
       "uniform sampler2D uFillTexture;\n"
       "uniform float uTextureEnabled;\n"
       "uniform float uTextureMix;\n"
       "uniform float uTextureScale;\n"
       "uniform float uTextureScroll;\n"
       "uniform vec2 uMouse;\n"
       "uniform vec4 uColor;\n"
       "uniform vec4 uColor2;\n"
       "uniform float uIntensity;\n"
       "uniform float uSpeed;\n"
       "uniform float uScale;\n"
       "uniform float uGlow;\n"
       "uniform float uDistortion;\n"
       "uniform float uScanlines;\n"
       "uniform vec2 uTextPos;\n"
       "uniform float uTextScale;\n"
       "uniform float uTextRotation;\n"
       "uniform float uTextOpacity;\n"
       "in vec2 vUV;\n"
       "out vec4 fragColor;\n\n"
       "float hash12(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
       "float noise2(vec2 p) { vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f); float a=hash12(i), b=hash12(i+vec2(1,0)), c=hash12(i+vec2(0,1)), d=hash12(i+vec2(1,1)); return mix(mix(a,b,f.x),mix(c,d,f.x),f.y); }\n"
       "vec3 hsv(float h, float sat, float val) { vec3 k=vec3(1.0,2.0/3.0,1.0/3.0); vec3 p=abs(fract(vec3(h)+k)*6.0-3.0); return val*mix(k.xxx,clamp(p-1.0,0.0,1.0),sat); }\n"
       "float glyph(vec2 p, vec2 origin, vec2 size, int code) { vec2 q=(p-origin)/size; if(any(lessThan(q,vec2(0))) || any(greaterThan(q,vec2(1)))) return 0.0; vec2 cell=vec2(float(code % 16),float(code / 16)); q.y=1.0-q.y; return texture(uText,(cell+q)/vec2(16.0,8.0)).a; }\n"
       "float textMask(vec2 uv) {\n"
       "  vec2 p=uv; float c=cos(-uTextRotation), d=sin(-uTextRotation); p=(mat2(c,-d,d,c)*(p-vec2(0.5))/max(0.001,uTextScale))+vec2(0.5)-uTextPos; float m=0.0;\n";

  for (size_t li = 0; li < lines.size(); li++) {
    const std::string& line = lines[li];
    const float lineWidth = (float)line.size() * (w + spacing);
    float x = horizontalAlign_ == 0 ? 0.06f : horizontalAlign_ == 2 ? 0.94f - lineWidth
                                                                     : 0.5f - lineWidth * 0.5f;
    const float y = top - (float)(li + 1) * lineH;
    for (size_t ci = 0; ci < line.size(); ci++) {
      const unsigned char code = (unsigned char)line[ci];
      if (code < 32 || code > 127) continue;
      s << "  m=max(m,glyph(p,vec2(" << glslFloat(x + (float)ci * (w + spacing))
        << "," << glslFloat(y) << "),vec2(" << glslFloat(w) << "," << glslFloat(h)
        << ")," << (int)code << "));\n";
    }
  }
  s << "  return m*uTextOpacity; }\n\n"
       "void main() {\n"
       "  vec2 uv=vUV;\n"
       "  float scroll=" << (scrolling_ ? "fract(uTime*uSpeed*0.08)" : "0.0") << ";\n"
       "  vec2 q=uv+vec2(scroll,0.0);\n"
       "  float mask=textMask(q);\n"
       "  float e=0.0;\n"
       "  e=max(e,textMask(q+vec2(0.002,0))); e=max(e,textMask(q-vec2(0.002,0)));\n"
       "  e=max(e,textMask(q+vec2(0,0.003))); e=max(e,textMask(q-vec2(0,0.003)));\n"
       "  float edge=max(0.0,e-mask);\n"
       "  float bass=uBass, kick=uKick, energy=uAudioLevel;\n"
       "  vec3 bg=vec3(0.004,0.008,0.018)+0.018*hsv(0.60+uBar*0.025,0.75,1.0);\n"
       "  bg += 0.035*vec3(noise2(q*18.0+uTime*uSpeed*0.15));\n"
       "  vec3 col=uColor.rgb;\n"
       "  vec3 textureFill=uColor.rgb;\n"
       "  if (uTextureEnabled > 0.5) { textureFill=texture(uFillTexture,fract(q*uTextureScale+vec2(uTime*uTextureScroll,0.0))).rgb; }\n";
  switch (style) {
    case 0: s << "  col=vec3(0.18,1.0,0.42)*(0.7+mask*1.4); bg*=0.5; col*=0.85+0.15*sin(uTime*9.0);\n"; break;
    case 1: s << "  col=mix(uColor.rgb,uColor2.rgb,0.25+0.25*sin(uTime*uSpeed)); col*=1.0+mask*uGlow+kick*0.8;\n"; break;
    case 2: s << "  float tear=(hash12(vec2(floor(q.y*18.0),floor(uTime*12.0)))-0.5)*uDistortion; mask=max(mask,textMask(q+vec2(tear,0))); col=vec3(mask,mask*0.35,mask*1.3)+vec3(kick,0.0,kick);\n"; break;
    case 3: s << "  float scan=0.5+0.5*sin(q.y*180.0-uTime*5.0); col=mix(uColor.rgb,uColor2.rgb,scan*0.45); col*=0.75+mask*1.5;\n"; break;
    case 4: s << "  float chrome=0.5+0.5*sin(q.x*13.0+q.y*8.0+uTime*0.35); col=mix(vec3(0.35,0.42,0.58),vec3(1.0),chrome)+uColor.rgb*0.22; col*=0.8+mask*1.5;\n"; break;
    case 5: s << "  float fire=noise2(q*7.0+vec2(0,-uTime*uSpeed*0.8)); col=mix(vec3(0.35,0.01,0),vec3(1.0,0.62,0.04),fire); col+=vec3(1,0.1,0)*kick; mask*=smoothstep(0.18,0.65,fire+mask);\n"; break;
    case 6: s << "  float pl=0.5+0.5*sin(q.x*12.0+sin(q.y*8.0+uTime*uSpeed)*2.0); col=mix(uColor.rgb,uColor2.rgb,pl); col*=0.7+mask*1.8;\n"; break;
    case 7: s << "  float arc=pow(max(0.0,1.0-abs(noise2(q*24.0+uTime*3.0)-0.5)*2.0),6.0); col=uColor.rgb*(1.0+kick*2.0)+uColor2.rgb*arc*2.0; mask=max(mask,edge*2.0);\n"; break;
    case 8: s << "  float rain=step(0.78,fract(q.y*22.0+uTime*uSpeed+hash12(vec2(floor(q.x*34.0),0)))); bg+=vec3(0.01,0.22,0.06)*rain; col=vec3(0.2,1.0,0.35)*(0.7+mask*1.5);\n"; break;
    case 9: s << "  float cell=step(0.55,fract(q.x*70.0))*step(0.55,fract(q.y*42.0)); col=mix(uColor.rgb,vec3(0.9),cell)*mask*1.8;\n"; break;
    case 10: s << "  col=uColor.rgb*(edge*3.0+mask*0.12); bg*=0.65;\n"; break;
    default: s << "  float sparks=step(0.86,noise2(q*90.0-uTime*uSpeed)); col=uColor.rgb*(mask+sparks*2.0)+uColor2.rgb*edge*2.0; mask=max(mask,sparks*0.35);\n"; break;
  }
  s << "  col=mix(col,col*(0.35+0.65*textureFill),uTextureEnabled*uTextureMix);\n"
       "  col += uColor2.rgb*edge*uGlow*1.8;\n"
       "  col *= 0.85+0.18*energy+0.22*kick;\n"
       "  float lines=0.92+0.08*sin(uv.y*uResolution.y*0.55); col*=mix(1.0,lines,uScanlines);\n"
       "  col += vec3(0.02,0.04,0.08)*smoothstep(0.75,0.1,length(uv-0.5));\n"
       "  fragColor=vec4(bg+col*max(mask,edge*0.25),1.0);\n"
       "}\n";
  return s.str();
}

void ShaderLab::rebuildSourceFromPreset() {
  if (presetIndex_ < 0 || presetIndex_ >= (int)presets().size()) return;
  selectedPreset_ = presets()[(size_t)presetIndex_].name;
  const std::string src = makeSource(presets()[(size_t)presetIndex_].style);
  sourceBuf_.assign(src.begin(), src.end());
  sourceBuf_.push_back(0);
  sourceBuf_.resize(kSourceCap + 1, 0);
  sourceDirty_ = true;
  compileQueued_ = true;
  parseMetadata();
}

void ShaderLab::parseMetadata() {
  params_.clear();
  const std::string src = sourceBuf_.empty() ? std::string() : sourceBuf_.data();
  std::istringstream in(src);
  std::string line;
  while (std::getline(in, line)) {
    const size_t p = line.find("// @param ");
    if (p == std::string::npos) continue;
    std::istringstream ls(line.substr(p + 10));
    Param x;
    ls >> x.name >> x.type;
    if (x.type == "float" || x.type == "int") {
      ls >> x.min >> x.max >> x.value;
      if (x.max <= x.min) x.max = x.min + 1;
      x.defaultValue = x.value;
    } else if (x.type == "color") {
      std::string hex; ls >> hex;
      if (hex.size() == 7 && hex[0] == '#') {
        auto h = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10; };
        x.color[0] = (h(hex[1]) * 16 + h(hex[2])) / 255.0f;
        x.color[1] = (h(hex[3]) * 16 + h(hex[4])) / 255.0f;
        x.color[2] = (h(hex[5]) * 16 + h(hex[6])) / 255.0f;
        x.color[3] = 1;
        for (int i = 0; i < 4; ++i) x.defaultColor[i] = x.color[i];
      }
    } else continue;
    params_.push_back(x);
  }
}

bool ShaderLab::compile() {
  if (!initialized_ || sourceBuf_.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(previewFile_).parent_path(), ec);
  std::ofstream out(previewFile_, std::ios::binary | std::ios::trunc);
  if (!out) {
    compileStatus_ = "source write failed";
    compileError_ = "Cannot write " + previewFile_;
    Log::error("SHADERLAB", compileError_);
    return false;
  }
  out.write(sourceBuf_.data(), (std::streamsize)std::strlen(sourceBuf_.data()));
  out.close();
  try {
    auto next = std::make_unique<Shader>("fullscreen.vert", previewFile_);
    program_ = std::move(next);
    compileError_.clear();
    compileStatus_ = "Shader OK";
    lastCompiledSource_ = sourceBuf_.data();
    Log::info("SHADERLAB", "compiled typography preview");
    // Compiling validates the source but does not save it to disk. Keeping the
    // dirty flag here makes accidental loss visible and lets Save/Revert work
    // like a normal authoring tool.
    compileQueued_ = false;
    parseMetadata();
    return true;
  } catch (const std::exception& e) {
    compileStatus_ = "Compile error";
    compileError_ = e.what();
    compileQueued_ = false;
    Log::error("SHADERLAB", compileError_);
    return false;
  }
}

bool ShaderLab::ensurePreview(int width, int height) {
  width = std::max(160, std::min(width, 1920));
  height = std::max(90, std::min(height, 1080));
  if (preview_.w == width && preview_.h == height) return true;
  preview_ = FrameTarget::color(width, height, ::gl::RGBA8, ::gl::RGBA,
                                ::gl::UNSIGNED_BYTE,
                                {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false});
  previewW_ = width;
  previewH_ = height;
  return preview_.fbo != 0;
}

void ShaderLab::bindUniforms(float time, float dt) {
  if (!program_) return;
  program_->set2f("uResolution", (float)previewW_, (float)previewH_);
  program_->set1f("uTime", time);
  program_->set1f("uDeltaTime", dt);
  program_->set1f("uProgress", progress_);
  const float bpm = w_.timeline && w_.timeline->beatSec() > 0 ? 60.0f / w_.timeline->beatSec() : 120.0f;
  const TimelineState* ts = w_.timeline ? &w_.timeline->s : nullptr;
  program_->set1f("uBPM", bpm);
  program_->set1f("uBeat", ts ? ts->beat : time * bpm / 60.0f);
  program_->set1f("uBar", ts ? ts->bar : time * bpm / 240.0f);
  program_->set1f("uBeatPhase", ts ? ts->beatPhase : 0.0f);
  const float bass = w_.audio ? w_.audio->react.bass.load() : 0.0f;
  const float mid = w_.audio ? w_.audio->react.mid.load() : 0.0f;
  const float treble = w_.audio ? w_.audio->react.treble.load() : 0.0f;
  const float energy = w_.audio ? w_.audio->react.energy.load() : 0.0f;
  const float kick = w_.audio ? w_.audio->react.kick.load() : 0.0f;
  program_->set1f("uAudioLevel", energy);
  program_->set1f("uBass", bass);
  program_->set1f("uMid", mid);
  program_->set1f("uTreble", treble);
  program_->set1f("uKick", kick);
  program_->set1f("uSnare", w_.audio ? w_.audio->react.onset.load() : 0.0f);
  program_->set2f("uMouse", 0, 0);
  program_->set4f("uColor", color_[0], color_[1], color_[2], color_[3]);
  program_->set4f("uColor2", color2_[0], color2_[1], color2_[2], color2_[3]);
  program_->set1f("uIntensity", intensity_);
  program_->set1f("uSpeed", speed_);
  program_->set1f("uScale", scale_);
  program_->set1f("uProgress", progress_);
  program_->set2f("uTextPos", textPos_[0], textPos_[1]);
  program_->set1f("uTextScale", textScale_);
  program_->set1f("uTextRotation", textRotation_ * 3.14159265f / 180.0f);
  program_->set1f("uTextOpacity", textOpacity_);
  program_->set1f("uTextureEnabled", fillTexture_.tex ? 1.0f : 0.0f);
  program_->set1f("uTextureMix", textureMix_);
  program_->set1f("uTextureScale", textureScale_);
  program_->set1f("uTextureScroll", textureScroll_);
  if (fillTexture_.tex) {
    fillTexture_.bind(1);
    program_->set1i("uFillTexture", 1);
  }
  for (const Param& p : params_) {
    if (p.type == "float" || p.type == "int") program_->set1f(p.name.c_str(), p.value);
    else if (p.type == "color") program_->set4f(p.name.c_str(), p.color[0], p.color[1], p.color[2], p.color[3]);
  }
}

void ShaderLab::render(float time, float dt, int width, int height) {
  if (!initialized_ || !visible_) return;
  previewTime_ = time;
  previewDt_ = dt > 0 ? dt : 1.0f / 60.0f;
  if (compileQueued_ && wallNowLab() - lastEdit_ > 0.25) compile();
  if (!ensurePreview(width, height) || !program_) return;
  preview_.bind();
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glClearColor(0.002f, 0.004f, 0.012f, 1.0f);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  program_->use();
  bindUniforms(time, dt);
  const Assets* a = activeFont();
  if (a && a->fontTex.tex) {
    a->fontTex.bind(0);
    program_->set1i("uText", 0);
  }
  w_.renderer->fsTriangle.draw(3);
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
}

void ShaderLab::markEdited() {
  sourceDirty_ = true;
  compileQueued_ = true;
  lastEdit_ = wallNowLab();
}

bool ShaderLab::saveAsset() {
  if (!initialized_ || sourceBuf_.empty()) return false;
  const std::string clean = sanitizeName(saveNameBuf_);
  const std::string path = w_.shaderDir + "/" + clean + ".frag";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(sourceBuf_.data(), (std::streamsize)std::strlen(sourceBuf_.data()));
  out.close();
  exportedPath_ = path;
  savedSource_ = sourceBuf_.data();
  sourceDirty_ = false;
  compileStatus_ = "Saved " + std::filesystem::path(path).filename().string();
  return true;
}

void ShaderLab::restoreSource(const std::string& source, const char* status) {
  if (source.empty()) return;
  sourceBuf_.assign(source.begin(), source.end());
  sourceBuf_.push_back(0);
  sourceBuf_.resize(kSourceCap + 1, 0);
  selectedPreset_ = "Custom";
  presetIndex_ = -1;
  parseMetadata();
  sourceDirty_ = true;
  compileQueued_ = true;
  lastEdit_ = wallNowLab();
  compileStatus_ = status ? status : "restored";
  compileError_.clear();
}

std::string ShaderLab::selectedTexturePath() const {
  if (textureIndex_ <= 0 || textureIndex_ >= (int)texturePaths_.size()) return std::string();
  const std::filesystem::path selected = std::filesystem::absolute(texturePaths_[(size_t)textureIndex_]);
  const std::filesystem::path assetRoot = std::filesystem::absolute(w_.assetDir);
  const std::filesystem::path dataRoot = std::filesystem::absolute(w_.dataDir);
  std::error_code ec;
  const std::filesystem::path assetRel = std::filesystem::relative(selected, assetRoot, ec);
  const std::string assetRelText = assetRel.generic_string();
  if (!ec && !assetRel.empty() && assetRelText != ".." &&
      assetRelText.rfind("../", 0) != 0)
    return (std::filesystem::path("assets") / assetRel).generic_string();
  ec.clear();
  const std::filesystem::path dataRel = std::filesystem::relative(selected, dataRoot, ec);
  const std::string dataRelText = dataRel.generic_string();
  if (!ec && !dataRel.empty() && dataRelText != ".." &&
      dataRelText.rfind("../", 0) != 0)
    return (std::filesystem::path("data") / dataRel).generic_string();
  return selected.generic_string();
}

std::string ShaderLab::selectedFontPath() const {
  if (fontIndex_ <= 0 || fontIndex_ >= (int)fonts_.size()) return std::string();
  return fonts_[(size_t)fontIndex_];
}

std::string ShaderLab::timelineSnippet() const {
  if (exportedPath_.empty()) return std::string();
  const std::string file = std::filesystem::path(exportedPath_).filename().string();
  std::ostringstream s;
  s << "at 0 { shader " << file << " { useFont true";
  const std::string font = selectedFontPath();
  if (!font.empty())
    s << "; font assets/fonts/" << std::filesystem::path(font).filename().string();
  const std::string texture = selectedTexturePath();
  if (!texture.empty()) {
    s << "; texture " << texture
      << "; textureMix " << glslFloat(textureMix_)
      << "; textureScale " << glslFloat(textureScale_)
      << "; textureScroll " << glslFloat(textureScroll_);
  }
  s << " } }\n";
  s << "// Shader Lab preset: " << selectedPreset_ << "\n";
  s << "// text is baked into the generated glyph layout; the font atlas is selected above.\n";
  return s.str();
}

bool ShaderLab::takeInsertRequest() {
  const bool v = insertRequest_;
  insertRequest_ = false;
  return v;
}

bool ShaderLab::saveCurrentAsset() {
  if (!initialized_) return false;
  return saveAsset();
}

void ShaderLab::drawTextControls() {
  const char* align[] = {"Left", "Center", "Right"};
  const char* valign[] = {"Top", "Center", "Bottom"};
  if (ImGui::InputTextMultiline("Text", textBuf_.data(), textBuf_.size(), ImVec2(-1, 72))) {
    text_ = textBuf_.data();
    rebuildSourceFromPreset();
    markEdited();
  }
  if (!fonts_.empty() && ImGui::Combo("Font", &fontIndex_, fontGetter,
                                        &fonts_, (int)fonts_.size())) {
    selectFont(fontIndex_);
    rebuildSourceFromPreset();
    markEdited();
  }
  if (!texturePaths_.empty() && ImGui::Combo("Fill texture", &textureIndex_, fontGetter,
                                               &texturePaths_, (int)texturePaths_.size())) {
    selectTexture(textureIndex_);
    markEdited();
  }
  if (textureIndex_ > 0) {
    ImGui::SliderFloat("Texture mix", &textureMix_, 0.0f, 1.0f);
    ImGui::SliderFloat("Texture scale", &textureScale_, 0.1f, 8.0f);
    ImGui::SliderFloat("Texture motion", &textureScroll_, -2.0f, 2.0f);
  }
  if (ImGui::SliderInt("Font size", &fontSize_, 12, 300)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::SliderFloat("Letter spacing", &letterSpacing_, -20, 80)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::SliderFloat("Line spacing", &lineSpacing_, 0.6f, 2.5f)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::Combo("Horizontal", &horizontalAlign_, align, 3)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::Combo("Vertical", &verticalAlign_, valign, 3)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::DragFloat2("Position", textPos_, 0.005f, -0.5f, 0.5f)) markEdited();
  if (ImGui::SliderFloat("Scale", &textScale_, 0.2f, 3.0f)) markEdited();
  if (ImGui::SliderFloat("Rotation", &textRotation_, -180, 180)) markEdited();
  if (ImGui::SliderFloat("Opacity", &textOpacity_, 0, 1)) markEdited();
  if (ImGui::Checkbox("Scroll", &scrolling_)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::Checkbox("Wrap text", &wrap_)) { rebuildSourceFromPreset(); markEdited(); }
  if (wrap_ && ImGui::SliderFloat("Wrap width", &wrapWidth_, 0.25f, 1.0f)) { rebuildSourceFromPreset(); markEdited(); }
  if (ImGui::ColorEdit4("Color", color_)) markEdited();
  if (ImGui::ColorEdit4("Color 2", color2_)) markEdited();
  ImGui::SliderFloat("Intensity", &intensity_, 0, 8);
  ImGui::SliderFloat("Speed", &speed_, 0, 10);
  ImGui::SliderFloat("Progress", &progress_, 0, 1);
  if (ImGui::Button("Reset text layout")) {
    textPos_[0] = textPos_[1] = 0; textScale_ = 1; textRotation_ = 0; textOpacity_ = 1;
    markEdited();
  }
}

void ShaderLab::drawParameterControls() {
  ImGui::SeparatorText("GLSL metadata parameters");
  if (params_.empty()) {
    ImGui::TextDisabled("Add // @param name float min max default to the shader.");
    return;
  }
  for (Param& p : params_) {
    ImGui::PushID(p.name.c_str());
    if (p.type == "color") ImGui::ColorEdit4(p.name.c_str(), p.color);
    else if (p.type == "int") ImGui::SliderFloat(p.name.c_str(), &p.value, p.min, p.max, "%.0f");
    else ImGui::SliderFloat(p.name.c_str(), &p.value, p.min, p.max);
    ImGui::PopID();
  }
  if (ImGui::Button("Reset parameters")) {
    for (Param& p : params_) {
      p.value = p.defaultValue;
      for (int i = 0; i < 4; ++i) p.color[i] = p.defaultColor[i];
    }
  }
}

void ShaderLab::drawSourceEditor() {
  ImGui::SeparatorText("GLSL source");
  ImGui::TextDisabled("Built-ins: uResolution uTime uBPM uBeat uBar uBass uMid uTreble uKick uText");
  const std::string source = sourceBuf_.empty() ? std::string() : sourceBuf_.data();
  const int lineCount = std::max(1, (int)splitLines(source).size());
  ImGui::BeginChild("shaderlab_source_editor", ImVec2(0, 330), true);
  ImGui::BeginChild("shaderlab_line_numbers", ImVec2(42, 0), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  for (int line = 1; line <= lineCount; ++line) ImGui::TextDisabled("%4d", line);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("shaderlab_source_text", ImVec2(0, 0), false);
  if (ImGui::InputTextMultiline("##shaderlab_source", sourceBuf_.data(), sourceBuf_.size(),
                                ImVec2(-1, -1), ImGuiInputTextFlags_AllowTabInput)) {
    selectedPreset_ = "Custom";
    presetIndex_ = -1;
    markEdited();
    parseMetadata();
  }
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::SetNextItemWidth(220);
  ImGui::InputTextWithHint("##shaderlab_save_name", "shader asset name", saveNameBuf_,
                          sizeof(saveNameBuf_));
  ImGui::SameLine();
  if (ImGui::Button("Compile")) compile();
  ImGui::SameLine();
  if (ImGui::Button("Save Shader Asset")) saveAsset();
  ImGui::SameLine();
  if (ImGui::Button("Save As")) saveAsset();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save using the asset name field");
  ImGui::SameLine();
  if (ImGui::Button("Revert saved") && !savedSource_.empty())
    restoreSource(savedSource_, "reverted saved source");
  ImGui::SameLine();
  if (ImGui::Button("Revert compiled") && !lastCompiledSource_.empty())
    restoreSource(lastCompiledSource_, "reverted compiled source");
  ImGui::SameLine();
  ImGui::TextColored(compileError_.empty() ? ImVec4(0.35f, 1, 0.75f, 1) : ImVec4(1, 0.35f, 0.35f, 1),
                     "%s%s", sourceDirty_ ? "* " : "", compileStatus_.c_str());
  if (!compileError_.empty()) {
    ImGui::BeginChild("shaderlab_errors", ImVec2(0, 130), true);
    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Compile failed - previous valid preview is retained");
    ImGui::TextWrapped("%s", compileError_.c_str());
    ImGui::EndChild();
  }
}

void ShaderLab::drawPreview(float) {
  ImGui::SeparatorText("Live preview");
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float aspect = 16.0f / 9.0f;
  ImVec2 size(std::max(240.0f, avail.x), std::max(135.0f, avail.x / aspect));
  if (size.y > avail.y * 0.52f) size = ImVec2(std::max(240.0f, avail.y * aspect * 0.52f), std::max(135.0f, avail.y * 0.52f));
  if (preview_.colorTex()) {
    ImGui::Image((ImTextureID)(intptr_t)preview_.colorTex(), size, ImVec2(0, 1), ImVec2(1, 0));
  } else {
    ImGui::TextDisabled("Preview framebuffer is not ready");
  }
  ImGui::Text("Shader: %s   |   FPS follows editor transport   |   %s", selectedPreset_.c_str(), compileStatus_.c_str());
}

void ShaderLab::draw(float time) {
  if (!visible_ || !initialized_) return;
  ImGui::SetNextWindowSize(ImVec2(980, 760), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Shader Lab", &visible_, ImGuiWindowFlags_MenuBar)) {
    ImGui::End(); return;
  }
  if (ImGui::BeginMenuBar()) {
    ImGui::TextDisabled("DEMO EFFECT LABORATORY");
    ImGui::SameLine();
    ImGui::Text("%s%s", selectedPreset_.c_str(), sourceDirty_ ? " *" : "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current shader source");
    ImGui::EndMenuBar();
  }
  ImGui::BeginChild("shaderlab_left", ImVec2(290, 0), true);
  ImGui::SeparatorText("Presets");
  for (int i = 0; i < (int)presets().size(); i++) {
    const Preset& p = presets()[(size_t)i];
    if (ImGui::Selectable(p.name, presetIndex_ == i)) {
      presetIndex_ = i;
      selectedPreset_ = p.name;
      rebuildSourceFromPreset();
      compile();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s | %s", p.category, p.description);
  }
  ImGui::SeparatorText("Text & typography");
  drawTextControls();
  drawParameterControls();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("shaderlab_right", ImVec2(0, 0), false);
  drawPreview(time);
  drawSourceEditor();
  ImGui::EndChild();
  ImGui::End();
}

} // namespace ns
