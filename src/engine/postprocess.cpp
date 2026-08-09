#include "engine/postprocess.hpp"
#include "engine/gl.hpp"
#include "framework/core/log.hpp"
#include <algorithm>
#include <cstdint>

namespace ns {

PostFX::PostFX(Renderer& r) : r_(r) {
  const int w = r.resW, h = r.resH;
  dofProg_ = std::make_unique<Shader>("fullscreen.vert", "dof.frag");
  extractProg_ = std::make_unique<Shader>("fullscreen.vert", "bloom_extract.frag");
  blurProg_ = std::make_unique<Shader>("fullscreen.vert", "bloom_blur.frag");
  composeProg_ = std::make_unique<Shader>("fullscreen.vert", "compose.frag");
  flashProg_ = std::make_unique<Shader>("fullscreen.vert", "flash.frag");
  fxaaProg_ = std::make_unique<Shader>("fullscreen.vert", "fxaa.frag");

  const TextureOpts ldrOpts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  dofTarget_ = FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR});
  flashTarget_ = FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR});
  ldrA_ = FrameTarget::color(r.viewW, r.viewH, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE, ldrOpts);
  ldrB_ = FrameTarget::color(r.viewW, r.viewH, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE, ldrOpts);
  const unsigned char blk[4] = {0, 0, 0, 255};
  blackTex_ = Texture::fromRGBA(1, 1, blk);
  rebuildBloom();
}

int PostFX::fmtInternal() const { return ::gl::RGBA16F; }
int PostFX::fmtFormat() const { return ::gl::RGBA; }
int PostFX::fmtType() const { return ::gl::HALF_FLOAT; }

void PostFX::rebuildBloom() {
  for (auto& t : bloomLevels_) t.destroy();
  for (auto& t : scratchLevels_) t.destroy();
  bloomLevels_.clear();
  scratchLevels_.clear();
  int w = std::max(4, r_.resW / 2);
  int h = std::max(4, r_.resH / 2);
  int level = 0;
  while (w >= 8 && h >= 8 && level < 6) {
    bloomLevels_.push_back(FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR}));
    scratchLevels_.push_back(FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR}));
    w = std::max(4, w / 2);
    h = std::max(4, h / 2);
    level++;
  }
}

void PostFX::resize() {
  dofTarget_.destroy();
  flashTarget_.destroy();
  ldrA_.destroy();
  ldrB_.destroy();
  const int w = r_.resW, h = r_.resH;
  const TextureOpts ldrOpts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  dofTarget_ = FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR});
  flashTarget_ = FrameTarget::color(w, h, fmtInternal(), fmtFormat(), fmtType(), {::gl::LINEAR, ::gl::LINEAR});
  ldrA_ = FrameTarget::color(r_.viewW, r_.viewH, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE, ldrOpts);
  ldrB_ = FrameTarget::color(r_.viewW, r_.viewH, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE, ldrOpts);
  rebuildBloom();
}

void PostFX::process(unsigned hdrTex, Camera& camera, float motion, float time) {
  Renderer& r = r_;
  const Quality& q = r.quality;
  if (bloomLevels_.empty()) return;
  perf_.beginFrame();  // GPU timing: PerfTimer ring (see perftimer.hpp)

  const float bloomThresh = 0.75f - fx.bloom * 0.18f;
  const float bloomIntensity = 1.0f + (float)q.bloom * 0.15f + fx.bloom * 0.9f;

  // --- depth of field --------------------------------------------------------
  unsigned src = hdrTex;
  if (q.dof && camera.dofAperture > 0.001f) {
    dofTarget_.bind();
    dofProg_->use();
    dofProg_->set1i("uTex", 0);
    dofProg_->set1i("uDepth", 0);  // explicit, like the web port (not the unit-0 default)
    dofProg_->setVec2("uRes", (float)dofTarget_.w, (float)dofTarget_.h);
    dofProg_->set1f("uFocus", camera.dofFocus);
    dofProg_->set1f("uAperture", camera.dofAperture);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, hdrTex);
    r.fsTriangle.draw(3);
    src = dofTarget_.colorTex();
  }

  // --- landing verdict flash (pre-bloom, additive HDR) ------------------------
  const bool flashActive = fx.landing > 0 && fx.landingT > 0.001f;
  if (flashActive) {
    flashTarget_.bind();
    ::glDisable(::gl::BLEND);
    flashProg_->use();
    flashProg_->setVec2("uRes", (float)flashTarget_.w, (float)flashTarget_.h);
    flashProg_->set1f("uLanding", (float)fx.landing);
    flashProg_->set1f("uLandingT", fx.landingT);
    r.fsTriangle.draw(3);
  }
  const unsigned flashTex = flashActive ? flashTarget_.colorTex() : blackTex_.tex;

  // --- bloom -------------------------------------------------------------------
  FrameTarget& l0 = bloomLevels_[0];
  l0.bind();
  extractProg_->use();
  extractProg_->set1i("uTex", 0);
  extractProg_->set1i("uFlash", 1);
  extractProg_->setVec2("uRes", (float)l0.w, (float)l0.h);
  extractProg_->set1f("uThreshold", bloomThresh);
  extractProg_->set1f("uIntensity", bloomIntensity);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, src);
  ::glActiveTexture(::gl::TEXTURE1);
  ::glBindTexture(::gl::TEXTURE_2D, flashTex);
  r.fsTriangle.draw(3);

  for (size_t i = 0; i < bloomLevels_.size(); i++) {
    FrameTarget& cur = bloomLevels_[i];
    FrameTarget& scratch = scratchLevels_[i];
    scratch.bind();
    blurProg_->use();
    blurProg_->set1i("uTex", 0);
    blurProg_->setVec2("uRes", (float)cur.w, (float)cur.h);
    blurProg_->setVec2("uDir", 1.0f, 0.0f);
    blurProg_->set1f("uRadius", 1.0f);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, cur.colorTex());
    r.fsTriangle.draw(3);

    cur.bind();
    blurProg_->setVec2("uDir", 0.0f, 1.0f);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, scratch.colorTex());
    r.fsTriangle.draw(3);

    if (i < bloomLevels_.size() - 1) {
      FrameTarget& next = bloomLevels_[i + 1];
      next.bind();
      blurProg_->setVec2("uRes", (float)next.w, (float)next.h);
      blurProg_->setVec2("uDir", 1.0f, 1.0f);
      blurProg_->set1f("uRadius", 1.6f);
      ::glActiveTexture(::gl::TEXTURE0);
      ::glBindTexture(::gl::TEXTURE_2D, cur.colorTex());
      r.fsTriangle.draw(3);
    }
  }

  // --- composite ---------------------------------------------------------------
  FrameTarget& writeLdr = prevLdr_ ? ldrA_ : ldrB_;
  FrameTarget& prevFrame = prevLdr_ ? ldrB_ : ldrA_;
  writeLdr.bind();
  composeProg_->use();
  composeProg_->set1i("uTex", 0);
  composeProg_->set1i("uBloom0", 1);
  composeProg_->set1i("uBloom1", 2);
  composeProg_->set1i("uBloom2", 3);
  composeProg_->set1i("uBloom3", 4);
  composeProg_->set1i("uBloom4", 5);
  composeProg_->set1i("uBloom5", 6);
  composeProg_->set1i("uPrev", 7);
  composeProg_->set1i("uFlash", 8);
  composeProg_->setVec2("uRes", (float)writeLdr.w, (float)writeLdr.h);
  composeProg_->set1f("uTime", time);
  composeProg_->set1f("uMotion", motion);
  composeProg_->set1f("uGrain", 0.85f);
  composeProg_->set1f("uScan", 1.0f);
  composeProg_->set1f("uVignette", 0.55f);
  composeProg_->set1f("uCA", 1.0f);
  composeProg_->set1f("uDistort", 0.22f);
  composeProg_->set1f("uSaturation", 1.12f);
  composeProg_->set1f("uContrast", 1.08f);
  composeProg_->set1f("uBoost", fx.exposure);
  composeProg_->setVec3("uGradeA", 0.35f, 0.22f, 0.55f);
  composeProg_->setVec3("uGradeB", 1.1f, 0.92f, 1.0f);
  composeProg_->set1f("uGlitch", fx.glitch);
  composeProg_->set1f("uBloomMul", fx.bloom);
  composeProg_->set1f("uHeat", fx.heat);
  composeProg_->set1f("uDirt", fx.dirt);
  composeProg_->set1f("uKick", fx.kick);
  composeProg_->set1f("uLanding", (float)fx.landing);

  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, src);
  for (int i = 0; i < 6; i++) {
    ::glActiveTexture(::gl::TEXTURE1 + i);
    if (i < (int)bloomLevels_.size()) ::glBindTexture(::gl::TEXTURE_2D, bloomLevels_[i].colorTex());
    else ::glBindTexture(::gl::TEXTURE_2D, blackTex_.tex);
  }
  ::glActiveTexture(::gl::TEXTURE7);
  ::glBindTexture(::gl::TEXTURE_2D, prevFrame.colorTex());
  ::glActiveTexture(::gl::TEXTURE8);
  ::glBindTexture(::gl::TEXTURE_2D, flashTex);
  r.fsTriangle.draw(3);
  prevLdr_ = !prevLdr_;

  // --- FXAA to screen ----------------------------------------------------------
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, r.viewW, r.viewH);
  fxaaProg_->use();
  fxaaProg_->set1i("uTex", 0);
  fxaaProg_->setVec2("uRes", (float)r.viewW, (float)r.viewH);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, writeLdr.colorTex());
  r.fsTriangle.draw(3);

  perf_.endFrame();
  // periodic note so authors can see what the whole post stack costs
  if (perf_.logDue()) {
    Log::info("POST", perf_.logLine() + " (bloom " + std::to_string(bloomLevels_.size()) +
               " levels)");
  }
}

}  // namespace ns
