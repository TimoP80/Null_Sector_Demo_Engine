#include "app/poststack.hpp"
#include "engine/camera.hpp"
#include "framework/vfs/vfs.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ns {

// ---------------------------------------------------------------------------
// per-pass implementations
// ---------------------------------------------------------------------------
namespace {

struct SimplePass : PostPass {  // single shader: src (uTex) -> dst
  ProgramRef prog;
  std::string fragFile;
  explicit SimplePass(std::string frag) : fragFile(std::move(frag)) {}
  bool init(ShaderManager& sm) override {
    prog = sm.get("fullscreen.vert", fragFile);
    return prog.ok();
  }
  void render(unsigned src, unsigned dst, int w, int h, const PostCtx& ctx) override {
    (void)dst;
    prog.use();
    prog.set2f("uRes", (float)w, (float)h);   // every pass shader divides uv by uRes
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, src);
    prog.set1i("uTex", 0);
    onBind();
    if (ctx.r) ctx.r->fsTriangle.draw(3);  // binds the fs VAO (core profile)
  }
  virtual void onBind() {}
};

// --- vignette -----------------------------------------------------------------
struct VignettePass : SimplePass {
  float amount = 0.4f, curve = 2.0f;
  VignettePass() : SimplePass("post_vignette.frag") {}
  void onBind() override {
    prog.set1f("uAmount", params.get("amount").asFloat(amount));
    prog.set1f("uCurve", params.get("curve").asFloat(curve));
  }
};

// --- film grain ----------------------------------------------------------------
struct GrainPass : SimplePass {
  GrainPass() : SimplePass("post_grain.frag") {}
  void onBind() override {
    prog.set1f("uAmount", params.get("amount").asFloat(0.18f));
  }
  void render(unsigned src, unsigned dst, int w, int h, const PostCtx& ctx) override {
    prog.use();
    prog.set2f("uRes", (float)w, (float)h);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, src);
    prog.set1i("uTex", 0);
    prog.set1f("uTime", ctx.time);
    prog.set1f("uAmount", params.get("amount").asFloat(0.18f));
    if (ctx.r) ctx.r->fsTriangle.draw(3);

  }
};

// --- scanlines ------------------------------------------------------------------
struct ScanlinesPass : SimplePass {
  ScanlinesPass() : SimplePass("post_scanlines.frag") {}
  void onBind() override {
    prog.set1f("uAmount", params.get("amount").asFloat(0.3f));
  }
};

// --- pixelation -------------------------------------------------------------------
struct PixelatePass : SimplePass {
  PixelatePass() : SimplePass("post_pixelate.frag") {}
  void onBind() override {
    prog.set1f("uPixels", params.get("pixels").asFloat(4.0f));
  }
};

// --- chromatic aberration ----------------------------------------------------------
struct ChromaticPass : SimplePass {
  ChromaticPass() : SimplePass("post_chromatic.frag") {}
  void onBind() override {
    prog.set1f("uAmount", params.get("amount").asFloat(0.0f));
  }
};

// --- color grade --------------------------------------------------------------------
struct GradePass : SimplePass {
  GradePass() : SimplePass("post_grade.frag") {}
  void onBind() override {
    prog.set1f("uSaturation", params.get("saturation").asFloat(1.0f));
    prog.set1f("uContrast", params.get("contrast").asFloat(1.0f));
    prog.set1f("uExposure", params.get("exposure").asFloat(1.0f));
    prog.set3f("uTintA", 1, 1, 1);
    prog.set3f("uTintB", 1, 1, 1);
    prog.set1f("uTonemap", params.get("tonemap").asBool(false) ? 1.0f : 0.0f);
  }
};

// --- fog -----------------------------------------------------------------------------
struct FogPass : SimplePass {
  FogPass() : SimplePass("post_fog.frag") {}
  void onBind() override {
    prog.set1f("uDensity", params.get("density").asFloat(0.35f));
    prog.set1f("uStart", params.get("start").asFloat(0.55f));
    prog.set3f("uColor", 0.15f, 0.2f, 0.4f);
  }
};

// --- fxaa ----------------------------------------------------------------------------
struct FxaaPass : SimplePass {
  FxaaPass() : SimplePass("fxaa.frag") {}
  void onBind() override {}
};

// --- dof --------------------------------------------------------------------------------
struct DofPass : PostPass {
  ProgramRef prog;
  bool init(ShaderManager& sm) override {
    prog = sm.get("fullscreen.vert", "dof.frag");
    return prog.ok();
  }
  void render(unsigned src, unsigned dst, int w, int h, const PostCtx& ctx) override {
    prog.use();
    prog.set2f("uRes", (float)w, (float)h);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, src);
    prog.set1i("uTex", 0);
    prog.set1i("uDepth", 0);  // depth is packed in the same texture's alpha
    prog.set1f("uFocus", params.get("focus").asFloat(ctx.camera ? ctx.camera->dofFocus : 8.0f));
    prog.set1f("uAperture", params.get("aperture").asFloat(ctx.camera ? ctx.camera->dofAperture : 0.0f));
    if (ctx.r) ctx.r->fsTriangle.draw(3);
  }
};

// --- bloom (extract -> blur x levels -> additive compose) --------------------------------
struct BloomPass : PostPass {
  ProgramRef extract, blur, compose;
  std::vector<FrameTarget> levels;    // 3 blurred levels (half-res chain)
  std::vector<FrameTarget> scratch;   // blur ping-pong
  float threshold = 1.0f, intensity = 1.0f;
  int levelsCount = 3;

  bool init(ShaderManager& sm) override {
    extract = sm.get("fullscreen.vert", "bloom_extract.frag");
    blur = sm.get("fullscreen.vert", "bloom_blur.frag");
    compose = sm.get("fullscreen.vert", "post_bloom_compose.frag");
    return extract.ok() && blur.ok() && compose.ok();
  }
  void resize(int w, int h) override {
    const TextureOpts opts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
    levels.clear();
    scratch.clear();
    int lw = w / 2, lh = h / 2;
    for (int i = 0; i < levelsCount; i++) {
      levels.emplace_back(FrameTarget::color(std::max(2, lw), std::max(2, lh), ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts));
      scratch.emplace_back(FrameTarget::color(std::max(2, lw), std::max(2, lh), ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts));
      lw /= 2; lh /= 2;
    }
  }
  void render(unsigned src, unsigned dst, int w, int h, const PostCtx& ctx) override {
    threshold = params.get("threshold").asFloat(threshold);
    intensity = params.get("intensity").asFloat(intensity) * (0.6f + ctx.bloomMul * 0.8f);
    // the caller (PostStack::process) has bound the ping-pong destination FBO;
    // the internal extract/blur passes rebind their own half-res levels, so
    // remember it and restore it before the compose draw below - otherwise the
    // composed output lands in the last blur level and the ping-pong target
    // (what the next pass / present reads) is never written
    GLint dstFbo = 0;
    ::glGetIntegerv(::gl::FRAMEBUFFER_BINDING, &dstFbo);

    // extract (half-res)
    levels[0].bind();
    ::glClearColor(0, 0, 0, 1);
    ::glClear(::gl::COLOR_BUFFER_BIT);      extract.use();
      extract.set2f("uRes", (float)levels[0].w, (float)levels[0].h);
      ::glActiveTexture(::gl::TEXTURE0);
      ::glBindTexture(::gl::TEXTURE_2D, src);
      extract.set1i("uTex", 0);
      extract.set1f("uThreshold", threshold);
      extract.set1f("uIntensity", 1.0f);
      if (ctx.r) ctx.r->fsTriangle.draw(3);

    // blur each level (2 separable passes)
    for (int i = 0; i < levelsCount; i++) {
      FrameTarget& lv = levels[(size_t)i];
      FrameTarget& sc = scratch[(size_t)i];
      blur.use();
      blur.set2f("uRes", (float)lv.w, (float)lv.h);
      ::glActiveTexture(::gl::TEXTURE0);
      ::glBindTexture(::gl::TEXTURE_2D, lv.colorTex());
      blur.set1i("uTex", 0);
      blur.set1f("uRadius", 1.6f + i * 0.7f);
      sc.bind();
      ::glUniform2f(blur.loc("uDir"), 1.0f, 0.0f);
      if (ctx.r) ctx.r->fsTriangle.draw(3);
      lv.bind();
      ::glActiveTexture(::gl::TEXTURE0);
      ::glBindTexture(::gl::TEXTURE_2D, sc.colorTex());
      blur.set1i("uTex", 0);
      ::glUniform2f(blur.loc("uDir"), 0.0f, 1.0f);
      if (ctx.r) ctx.r->fsTriangle.draw(3);
    }

    // additive compose into dst (restore the caller's target FBO + viewport)
    ::glBindFramebuffer(::gl::FRAMEBUFFER, (unsigned)dstFbo);  // 0 = default FB is legal
    ::glViewport(0, 0, w, h);
    compose.use();
    compose.set2f("uRes", (float)w, (float)h);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, src);
    compose.set1i("uBase", 0);
    ::glActiveTexture(::gl::TEXTURE1);
    ::glBindTexture(::gl::TEXTURE_2D, levels[0].colorTex());
    compose.set1i("uBloom0", 1);
    ::glActiveTexture(::gl::TEXTURE2);
    ::glBindTexture(::gl::TEXTURE_2D, levels[1].colorTex());
    compose.set1i("uBloom1", 2);
    ::glActiveTexture(::gl::TEXTURE3);
    ::glBindTexture(::gl::TEXTURE_2D, levels[2].colorTex());
    compose.set1i("uBloom2", 3);
    compose.set1f("uIntensity", intensity);
    if (ctx.r) ctx.r->fsTriangle.draw(3);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// PostStack
// ---------------------------------------------------------------------------
PostStack::~PostStack() { clear(); }

void PostStack::clear() {
  passes_.clear();
  pp_.clear();
}

void PostStack::setDefaultParam(const std::string& pass, const std::string& key, double v) {
  for (auto& p : passes_) {
    if (p->name == pass && p->params.get(key).isNull()) {
      p->params.set(key) = Value(v);
    }
  }
}

bool PostStack::loadPreset(const Value& preset, ShaderManager& sm) {
  clear();
  const Value& passes = preset.get("passes");
  if (!passes.isArr()) {
    Log::error("POST", "post preset: missing 'passes' array");
    return false;
  }
  for (const auto& p : passes.asArr()) {
    const std::string name = p.get("name").asStr();
    std::unique_ptr<PostPass> pass;
    if (name == "vignette") pass = std::make_unique<VignettePass>();
    else if (name == "grain") pass = std::make_unique<GrainPass>();
    else if (name == "scanlines") pass = std::make_unique<ScanlinesPass>();
    else if (name == "pixelate") pass = std::make_unique<PixelatePass>();
    else if (name == "chromatic") pass = std::make_unique<ChromaticPass>();
    else if (name == "grade") pass = std::make_unique<GradePass>();
    else if (name == "fog") pass = std::make_unique<FogPass>();
    else if (name == "fxaa") pass = std::make_unique<FxaaPass>();
    else if (name == "dof") pass = std::make_unique<DofPass>();
    else if (name == "bloom") pass = std::make_unique<BloomPass>();
    else if (name == "copy") pass = std::make_unique<SimplePass>("post_copy.frag");
    else {
      Log::error("POST", "unknown post pass '" + name + "' (vignette/grain/scanlines/pixelate/chromatic/grade/fog/crt/fxaa/dof/bloom/copy)");
      return false;
    }
    pass->name = name;
    pass->params = p;
    if (!pass->init(sm)) {
      Log::error("POST", "pass '" + name + "' failed to compile");
      return false;
    }
    passes_.push_back(std::move(pass));
  }
  // internal programs
  copyProg_ = sm.get("fullscreen.vert", "post_copy.frag").state;
  presentProg_ = sm.get("fullscreen.vert", "post_present.frag").state;
  return true;
}

bool PostStack::loadPresetFile(const std::string& path, ShaderManager& sm) {
  try {
    // virtual path (data/post/...): read through the runtime VFS, fall back
    // to a direct file read for absolute editor paths
    std::string text = runtimeFS().readText(path);
    if (text.empty()) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(path, ec) && !ec) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
          std::ostringstream ss;
          ss << f.rdbuf();
          text = ss.str();
        }
      }
    }
    if (text.empty()) throw JsonError("cannot open preset: " + path);
    return loadPreset(Json::parseText(text), sm);
  } catch (const std::exception& e) {
    Log::error("POST", "preset load failed: " + std::string(e.what()));
    return false;
  }
}

void PostStack::ensureTargets(int w, int h) {
  if ((int)pp_.size() != 2 || pp_[0].w != w || pp_[0].h != h) {
    const TextureOpts opts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
    pp_.clear();
    pp_.emplace_back(FrameTarget::color(w, h, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts));
    pp_.emplace_back(FrameTarget::color(w, h, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts));
    w_ = w;
    h_ = h;
    for (auto& p : passes_) p->resize(w, h);
  }
}

void PostStack::resize(int w, int h) { ensureTargets(w, h); }

unsigned PostStack::process(unsigned input, const PostCtx& ctx, int w, int h) {
  if (passes_.empty()) return input;
  ensureTargets(w, h);

  unsigned src = input;
  bool ping = false;
  for (auto& p : passes_) {
    unsigned dst = ping ? pp_[1].colorTex() : pp_[0].colorTex();
    // bind the destination FBO
    FrameTarget& target = ping ? pp_[1] : pp_[0];
    ::glBindFramebuffer(::gl::FRAMEBUFFER, target.fbo);
    ::glViewport(0, 0, target.w, target.h);
    ::glDisable(::gl::BLEND);
    ::glDisable(::gl::DEPTH_TEST);
    p->render(src, dst, target.w, target.h, ctx);
    src = dst;
    ping = !ping;
  }
  // last output lives in the last-written target
  lastOutput_ = ping ? pp_[0].colorTex() : pp_[1].colorTex();
  return lastOutput_;
}

void PostStack::present(unsigned finalTex, int viewW, int viewH) {
  if (!presentProg_ || !presentProg_->id) return;
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, viewW, viewH);
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glUseProgram(presentProg_->id);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, finalTex);
  ::glUniform1i(presentProg_->loc("uTex"), 0);
  ::glUniform1f(presentProg_->loc("uTonemap"), hasTonemap() ? 0.0f : 1.0f);
  ::glUniform2f(presentProg_->loc("uRes"), (float)viewW, (float)viewH);
  r_.fsTriangle.draw(3);
  lastOutput_ = finalTex;
}

std::vector<std::string> PostStack::chain() const {
  std::vector<std::string> out;
  for (const auto& p : passes_) out.push_back(p->name);
  return out;
}

bool PostStack::hasTonemap() const {
  for (const auto& p : passes_) {
    if (p->name == "grade" && p->params.get("tonemap").asBool(false)) return true;
  }
  return false;
}

}  // namespace ns
