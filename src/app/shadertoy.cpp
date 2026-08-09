#include "app/appassets.hpp"
#include "app/shadertoy.hpp"
#include "app/shadertoyparse.hpp"
#include "engine/paths.hpp"
#include "engine/renderer.hpp"
#include "framework/core/log.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ns {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {
std::string readTextFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string dataDir() {
  return resolveRuntimeDir("NULLSECTOR_DATA_DIR", NULLSECTOR_DATA_DIR, "data");
}
}  // namespace

unsigned ShadertoyFX::compileStage(unsigned type, const std::string& src, const std::string& label) {
  const unsigned sh = ::glCreateShader(type);
  const char* c = src.c_str();
  ::glShaderSource(sh, 1, &c, nullptr);
  ::glCompileShader(sh);
  int status = 0;
  ::glGetShaderiv(sh, ::gl::COMPILE_STATUS, &status);
  if (!status) {
    char log[8192];
    int len = 0;
    ::glGetShaderInfoLog(sh, sizeof(log), &len, log);
    ::glDeleteShader(sh);
    throw std::runtime_error("Shadertoy compile error (" + label + "):\n" + std::string(log, len > 0 ? len : 0));
  }
  return sh;
}

unsigned ShadertoyFX::linkProgram(const std::vector<unsigned>& stages, const std::string& label) {
  const unsigned prog = ::glCreateProgram();
  for (unsigned s : stages) ::glAttachShader(prog, s);
  ::glLinkProgram(prog);
  int status = 0;
  ::glGetProgramiv(prog, ::gl::LINK_STATUS, &status);
  if (!status) {
    char log[8192];
    int len = 0;
    ::glGetProgramInfoLog(prog, sizeof(log), &len, log);
    ::glDeleteProgram(prog);
    throw std::runtime_error("Shadertoy link error (" + label + "):\n" + std::string(log, len > 0 ? len : 0));
  }
  const unsigned blockIdx = ::glGetUniformBlockIndex(prog, "NullBlock");
  if (blockIdx != ::gl::INVALID_INDEX) ::glUniformBlockBinding(prog, blockIdx, 0);
  return prog;
}

// ---------------------------------------------------------------------------
// source parsing: split on `// pass: <name>` marker lines. The marker rule
// is strict (a marker must be the first comment on a whitespace-prefixed
// line, and its name is a single token), so prose that merely MENTIONS
// "// pass:" - e.g. the header comment "(no `// pass:` markers = image
// pass)" or nested "//   // pass: common" descriptions - never splits the
// file, and a lone false marker can no longer truncate or misname a pass.
// ---------------------------------------------------------------------------
bool ShadertoyFX::parseSource() {
  passes_.clear();
  std::string full = readTextFile(dataDir() + "/shadertoy/" + file_);
  // an absolute file_ (a shader dropped in the editor from OUTSIDE
  // data/shadertoy, e.g. the exe dir) reads directly instead of
  // concatenating onto the shadertoy dir
  if (full.empty() && std::filesystem::exists(file_)) full = readTextFile(file_);
  if (full.empty()) {
    Log::error("SHADERTOY", "cannot read shadertoy file: data/shadertoy/" + file_);
    return false;
  }

  renderScale_ = scaleOverride_ > 0.0f ? scaleOverride_ : extractShadertoyRenderScale(full);
  if (renderScale_ < 1.0f) {
    Log::info("SHADERTOY", file_ + ": buffer renderScale " + std::to_string(renderScale_));
  }

  std::vector<ShadertoyPass> sp = splitShadertoyPasses(full);
  for (auto& p : sp) passes_.push_back(Pass{std::move(p.name), std::move(p.src)});
  return true;
}

// ---------------------------------------------------------------------------
// GLSL wrapper
// ---------------------------------------------------------------------------
std::string ShadertoyFX::wrap(const std::string& passName, const std::string& src) {
  const bool hasMain = src.find("mainImage") != std::string::npos;
  std::ostringstream out;
  out << "#version 330 core\n"
      << "// Shadertoy importer pass: " << passName << "\n"
      << "#define texture2D texture\n"
      << "#define texture2DLodEXT textureLod\n"
      << "#define texture2DProj textureProj\n"
      << "#define iGlobalTime iTime\n"
      << "uniform float iTime;\n"
      << "uniform float iTimeDelta;\n"
      << "uniform int   iFrame;\n"
      << "uniform vec3  iResolution;\n"
      << "uniform vec4  iMouse;\n"
      << "uniform vec4  iDate;\n"
      << "uniform vec4  iChannelTime;\n"
      << "uniform vec4  iChannelResolution[4];\n"
      << "uniform sampler2D iChannel0;\n"
      << "uniform sampler2D iChannel1;\n"
      << "uniform sampler2D iChannel2;\n"
      << "uniform sampler2D iChannel3;\n"
      << "out vec4 fragColor;\n"
      << "\n"
      << src << "\n";
  if (hasMain) {
    out << "\nvoid main() {\n"
        << "  mainImage(fragColor, gl_FragCoord.xy);\n"
        << "}\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// compile
// ---------------------------------------------------------------------------
bool ShadertoyFX::compilePasses(EffectContext&) {
  destroyPrograms();
  const std::string sd = resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders");
  const std::string vs = readTextFile(sd + "/fullscreen.vert");
  if (vs.empty()) {
    Log::error("SHADERTOY", "fullscreen.vert not found - cannot build passes");
    return false;
  }
  vert_ = compileStage(::gl::VERTEX_SHADER, vs, "fullscreen.vert");

  // passthrough program for the live-scene snapshot (see render()).
  const char* copyFrag =
      "#version 330 core\n"
      "in vec2 vUV;\n"
      "uniform sampler2D uTex;\n"
      "out vec4 fragColor;\n"
      "void main() { fragColor = texture(uTex, vUV); }\n";
  try {
    const unsigned cfs = compileStage(::gl::FRAGMENT_SHADER, copyFrag, "scene copy");
    copyProg_ = linkProgram({vert_, cfs}, "scene copy");
    ::glDeleteShader(cfs);
  } catch (const std::exception& e) {
    Log::error("SHADERTOY", "scene copy program failed:\n" + std::string(e.what()));
    return false;
  }

  // `// pass: common` is a header-only pass: its source is prepended to
  // every other pass so shared helpers (hash21/noise/warp/...) are in scope
  // for buffer and image passes. Previously it was dropped entirely, so any
  // multi-pass file whose passes used the common helpers failed to compile.
  std::string commonSrc;
  for (const auto& p : passes_)
    if (p.name == "common") { commonSrc = p.src; break; }

  for (auto& p : passes_) {
    if (p.name == "common") continue;  // header-only pass: no program of its own
    const std::string body = commonSrc.empty() ? p.src : commonSrc + "\n" + p.src;
    const std::string fragSrc = wrap(p.name, body);
    try {
      const unsigned fs = compileStage(::gl::FRAGMENT_SHADER, fragSrc, file_ + "[" + p.name + "]");
      p.prog = linkProgram({vert_, fs}, file_ + "[" + p.name + "]");
      ::glDeleteShader(fs);
    } catch (const std::exception& e) {
      Log::error("SHADERTOY", std::string("pass '") + p.name + "' failed:\n" + e.what());
      return false;
    }
  }
  return true;
}

void ShadertoyFX::destroyPrograms() {
  for (auto& p : passes_) {
    if (p.prog) { ::glDeleteProgram(p.prog); p.prog = 0; }
  }
  if (copyProg_) { ::glDeleteProgram(copyProg_); copyProg_ = 0; }
  if (vert_) { ::glDeleteShader(vert_); vert_ = 0; }
}

void ShadertoyFX::init(EffectContext& ctx) {
  if (!parseSource()) throw std::runtime_error("shadertoy parse failed: " + file_);

  // order passes: common first, then buffer_a..d, image last. COPY, do not
  // move: moving out of passes_ while iterating it leaves moved-from entries
  // (empty names) that the catch-all loop below would re-move into the
  // ordered list as phantom empty passes ("no program defined" link errors).
  std::vector<Pass> ordered;
  ordered.reserve(passes_.size());
  for (const auto& p : passes_) if (p.name == "common") ordered.push_back(p);
  for (const auto& p : passes_) if (p.name.rfind("buffer_", 0) == 0) ordered.push_back(p);
  for (const auto& p : passes_) if (p.name == "image") ordered.push_back(p);
  for (const auto& p : passes_) if (p.name != "common" && p.name.rfind("buffer_", 0) != 0 && p.name != "image")
    ordered.push_back(p);
  passes_ = std::move(ordered);

  // guarantee an image pass: buffers-only files get a trivial one that
  // samples the last buffer through iChannel0 (a single-pass file with no
  // markers already IS the image pass)
  bool hasImage = false;
  for (const auto& p : passes_) if (p.name == "image") hasImage = true;
  if (!hasImage) {
    Pass synth;
    synth.name = "image";
    synth.src =
        "// synthetic image pass (no image tab in source)\n"
        "void mainImage(out vec4 o, in vec2 p) { o = texture(iChannel0, p / iResolution.xy); }\n";
    passes_.push_back(std::move(synth));
  }

  // external texture for iChannel0 of the first pass: a missing file is a
  // loud error (placeholder fallback), and a present file is actually
  // decoded - silently binding a blank 2x2 for either case hid both.
  if (!texPath_.empty()) {
    const std::string tp = dataDir() + "/textures/" + texPath_;
    Texture* loaded = nullptr;
    std::error_code ec;
    if (!std::filesystem::exists(tp, ec)) {
      Log::error("SHADERTOY", "iChannel0 texture not found: " + tp + " - using 2x2 placeholder");
    } else {
      loaded = AppAssets::loadTexture(tp);  // logs its own stbi reason on failure
    }
    if (loaded) {
      extTex_ = std::move(*loaded);
      delete loaded;  // moved-from: nothing to free
      Log::info("SHADERTOY", "iChannel0 texture: " + tp);
    } else {
      extTex_ = Texture::blank(2, 2, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE,
                               {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false});
    }
  }
  // black 1x1 fallback for unbound channels
  const unsigned char black[4] = {0, 0, 0, 255};
  blackTex_ = [&]() {
    Texture t = Texture::fromRGBA(1, 1, black, {::gl::NEAREST, ::gl::NEAREST, ::gl::CLAMP_TO_EDGE, false});
    const unsigned id = t.tex;
    t.tex = 0;  // keep the id, drop RAII
    return id;
  }();

  resize(ctx);
  frame_ = 0;
  lastTime_ = -1e9f;
  if (!compilePasses(ctx)) throw std::runtime_error("shadertoy compile failed: " + file_);

  // GPU timing: PerfTimer creates + warms its query ring on first beginFrame()
  perf_.setLabel(file_);
}

void ShadertoyFX::bufferSize(const EffectContext& ctx, int& w, int& h) const {
  // output (image pass + scene snapshot) stays at the caller's full render
  // resolution; the BUFFER passes are the expensive part, and renderScale
  // shrinks only those targets. clamp to a minimum of 2px per side.
  const int outW = ctx.hdr ? ctx.hdr->w : ctx.r->resW;
  const int outH = ctx.hdr ? ctx.hdr->h : ctx.r->resH;
  w = fixedW_ > 0 ? fixedW_ : std::max(2, (int)(outW * renderScale_));
  h = fixedH_ > 0 ? fixedH_ : std::max(2, (int)(outH * renderScale_));
}

void ShadertoyFX::resize(EffectContext& ctx) {
  const int outW = ctx.hdr ? ctx.hdr->w : ctx.r->resW;
  const int outH = ctx.hdr ? ctx.hdr->h : ctx.r->resH;
  int bufW = 0, bufH = 0;
  bufferSize(ctx, bufW, bufH);
  const TextureOpts opts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  buffers_.clear();
  sceneSnap_ = FrameTarget::color(outW, outH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts);
  for (int i = 0; i < 4; i++) {
    buffers_.emplace_back(FrameTarget::color(bufW, bufH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts));
  }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void ShadertoyFX::bindUniforms(EffectContext& ctx, Pass& pass, float passTime,
                               const std::vector<unsigned>& channelTexs, int w, int h) {
  ::glUseProgram(pass.prog);
  const int locTime = ::glGetUniformLocation(pass.prog, "iTime");
  if (locTime >= 0) ::glUniform1f(locTime, passTime);
  const int locDelta = ::glGetUniformLocation(pass.prog, "iTimeDelta");
  if (locDelta >= 0) ::glUniform1f(locDelta, ctx.dt);
  const int locFrame = ::glGetUniformLocation(pass.prog, "iFrame");
  if (locFrame >= 0) ::glUniform1i(locFrame, frame_);
  const int locRes = ::glGetUniformLocation(pass.prog, "iResolution");
  if (locRes >= 0) ::glUniform3f(locRes, (float)w, (float)h, 1.0f);
  const int locMouse = ::glGetUniformLocation(pass.prog, "iMouse");
  if (locMouse >= 0) ::glUniform4f(locMouse, mouse[0], mouse[1], mouse[2], mouse[3]);

  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  // extra data-driven uniforms (audio feeds + script-animated floats)
  for (const auto& kv : uniforms) {
    const int l = ::glGetUniformLocation(pass.prog, kv.first.c_str());
    if (l >= 0) ::glUniform1f(l, kv.second);
  }

  const int locDate = ::glGetUniformLocation(pass.prog, "iDate");
  if (locDate >= 0) {
    ::glUniform4f(locDate, (float)(tm.tm_year + 1900), (float)(tm.tm_mon + 1), (float)tm.tm_mday,
                  (float)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec));
  }
  const int locChTime = ::glGetUniformLocation(pass.prog, "iChannelTime");
  if (locChTime >= 0) {
    const float t0 = passStart_[0] > 0 ? ctx.time - passStart_[0] : 0;
    const float t1 = passStart_[1] > 0 ? ctx.time - passStart_[1] : 0;
    const float t2 = passStart_[2] > 0 ? ctx.time - passStart_[2] : 0;
    const float t3 = passStart_[3] > 0 ? ctx.time - passStart_[3] : 0;
    ::glUniform4f(locChTime, t0, t1, t2, t3);
  }
  // channels + resolutions
  const char* chNames[4] = {"iChannel0", "iChannel1", "iChannel2", "iChannel3"};
  const char* chRes[4] = {"iChannelResolution[0]", "iChannelResolution[1]", "iChannelResolution[2]", "iChannelResolution[3]"};
  for (int i = 0; i < 4; i++) {
    unsigned tex = channelTexs[(size_t)i];
    if (!tex) tex = blackTex_;
    const int loc = ::glGetUniformLocation(pass.prog, chNames[i]);
    if (loc >= 0) {
      ::glUniform1i(loc, i);
      ::glActiveTexture(::gl::TEXTURE0 + i);
      ::glBindTexture(::gl::TEXTURE_2D, tex);
    }
    const int locr = ::glGetUniformLocation(pass.prog, chRes[i]);
    if (locr >= 0) {
      int tw = 1, th = 1;
      for (const auto& b : buffers_) {
        if (b.colorTex() == tex) { tw = b.w; th = b.h; break; }
      }
      if (tex == extTex_.tex) { tw = extTex_.w ? extTex_.w : 2; th = extTex_.h ? extTex_.h : 2; }
      if (tex == sceneSnap_.colorTex()) { tw = sceneSnap_.w; th = sceneSnap_.h; }
      ::glUniform4f(locr, (float)tw, (float)th, 1.0f, 1.0f);
    }
  }
}

void ShadertoyFX::render(EffectContext& ctx) {
  if (passes_.empty() || !passes_.back().prog) return;

  // loop/seek detection: reset the per-pass iTime origins
  if (ctx.time < lastTime_ - 0.5f) {
    passStart_ = {0, 0, 0, 0};
  }
  lastTime_ = ctx.time;
  frame_++;

  // buffer passes render into the scaled targets (iResolution matches the
  // buffer size so fragCoord-based rays stay consistent); the image pass
  // draws into the caller's full-res target, so it gets the output size.
  int bufW = 0, bufH = 0;
  bufferSize(ctx, bufW, bufH);
  const int outW = ctx.hdr ? ctx.hdr->w : ctx.r->resW;
  const int outH = ctx.hdr ? ctx.hdr->h : ctx.r->resH;

  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);

  // GPU timing: PerfTimer collects any samples the GPU has finished and
  // stamps the start of this frame's passes (GL_TIMESTAMP ring - see
  // perftimer.hpp). GL_TIMESTAMP (not TIME_ELAPSED) because several effects
  // render in one frame and GL allows only one active elapsed query.
  perf_.beginFrame();

  // determine the buffer pass order + output targets
  std::vector<Pass*> bufferPasses;
  Pass* imagePass = nullptr;
  for (auto& p : passes_) {
    if (p.name.rfind("buffer_", 0) == 0) bufferPasses.push_back(&p);
    if (p.name == "image") imagePass = &p;
  }

  // snapshot the live scene BEFORE any pass writes into the target: the
  // image pass samples iChannel1 = "the live scene" and buffer A's fallback
  // reads it too - sampling the very target you are drawing into is a
  // feedback loop (undefined; renders as a solid colour on most drivers).
  // Copy once up front, sample the copy. Only when something actually reads
  // the scene: single-pass files whose shader never samples iChannel1 (the
  // wrapper's unused samplers are optimized out, so the uniform location is
  // -1) skip the copy entirely.
  const bool imageUsesScene = imagePass &&
                              ::glGetUniformLocation(imagePass->prog, "iChannel1") >= 0;
  const unsigned sceneTex = (ctx.hdr && ctx.hdr->colorTex() && copyProg_ && sceneSnap_.colorTex() &&
                             (!bufferPasses.empty() || imageUsesScene))
                                ? copyScene(ctx)
                                : 0;

  // render buffers A..D
  unsigned prevBufferTex = extTex_.tex;  // buffer A sees the external texture
  if (!prevBufferTex) prevBufferTex = sceneTex;
  int bufIdx = 0;
  for (Pass* p : bufferPasses) {
    if (bufIdx >= 4) break;
    if (passStart_[(size_t)bufIdx] <= 0) passStart_[(size_t)bufIdx] = ctx.time;
    buffers_[(size_t)bufIdx].bind();
    ::glClearColor(0, 0, 0, 1);
    ::glClear(::gl::COLOR_BUFFER_BIT);
    std::vector<unsigned> chs(4, 0);
    chs[0] = prevBufferTex;
    bindUniforms(ctx, *p, ctx.time - passStart_[(size_t)bufIdx], chs, bufW, bufH);
    ctx.r->fsTriangle.draw(3);
    prevBufferTex = buffers_[(size_t)bufIdx].colorTex();
    bufIdx++;
  }

  // image pass: channel0 = last buffer (or ext tex), channel1 = live scene.
  // The composite MUST land in the caller's scene target (ctx.hdr) - the
  // buffer loop above left the last buffer bound, so rebind it first or the
  // frame overwrites the buffer and the screen stays black.
  if (imagePass) {
    if (ctx.hdr) ctx.hdr->bind();
    std::vector<unsigned> chs(4, 0);
    chs[0] = bufIdx > 0 ? buffers_[(size_t)bufIdx - 1].colorTex() : (extTex_.tex ? extTex_.tex : 0);
    chs[1] = sceneTex;
    bindUniforms(ctx, *imagePass, ctx.time, chs, outW, outH);
    ctx.r->fsTriangle.draw(3);
  }

  perf_.endFrame();

  // periodic note so authors can see what the renderScale option is worth
  // live, e.g. "[SHADERTOY] tunnel_warp.glsl: 1.10 ms/frame GPU (renderScale
  // 0.50, buffers 800x450)"
  if (perf_.logDue()) {
    Log::info("SHADERTOY", perf_.logLine() + " (renderScale " + fmtMs(renderScale_) +
               ", buffers " + std::to_string(bufferWidth()) + "x" +
               std::to_string(bufferHeight()) + ")");
  }

  // unbind the scene texture unit (10) if the post pipeline binds it later
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, 0);
}

unsigned ShadertoyFX::copyScene(EffectContext& ctx) {
  sceneSnap_.bind();
  ::glDisable(::gl::BLEND);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, ctx.hdr->colorTex());
  ::glUseProgram(copyProg_);
  ::glUniform1i(::glGetUniformLocation(copyProg_, "uTex"), 0);
  ctx.r->fsTriangle.draw(3);
  return sceneSnap_.colorTex();
}

bool ShadertoyFX::reload(EffectContext& ctx) {
  if (!parseSource()) return false;
  resize(ctx);  // renderScale may have changed: realloc the buffer targets
  return compilePasses(ctx);
}

}  // namespace ns
