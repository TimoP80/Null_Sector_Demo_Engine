#include "engine/degraded.hpp"
#include "engine/assets.hpp"
#include "engine/audio.hpp"
#include "engine/gl.hpp"
#include "engine/math.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include <cstdio>

namespace ns {

bool DegradedFX::init(const Assets* assets) {
  assets_ = assets;
  ok_ = false;
  try {
    staticProg_ = std::make_unique<Shader>("fullscreen.vert", "degraded.frag");
    textProg_ = std::make_unique<Shader>("text.vert", "text.frag");
    blitProg_ = std::make_unique<Shader>("fullscreen.vert", "passthrough.frag");
    ok_ = true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[DEGRADED] placeholder shaders failed: %s\n", e.what());
  }
  return ok_;
}

void DegradedFX::render(EffectContext& ctx, const std::string& scene) {
  ctx.camera->dofAperture = 0;
  const Renderer& r = *ctx.r;
  const float energy = ctx.audio->react.energy.load();
  const float onset = ctx.audio->react.onset.load();
  const float kick = ctx.audio->react.kick.load();

  if (staticProg_) {
    ::glDisable(::gl::BLEND);
    ::glDisable(::gl::DEPTH_TEST);
    ::glDisable(::gl::CULL_FACE);
    staticProg_->use();
    staticProg_->set2f("uRes", (float)r.resW, (float)r.resH);
    staticProg_->set1f("uTime", ctx.time);
    staticProg_->set1f("uStatic", 0.5f + energy * 0.5f);
    staticProg_->set1f("uTear", 0.25f + onset * 0.6f);
    staticProg_->set1f("uKick", kick);
    r.fsTriangle.draw(3);
  }

  if (textProg_ && assets_) {
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    const float pulse = 0.5f + 0.5f * std::sin(ctx.time * 4.0f);
    const float beat = ctx.timeline->s.beatPulse;
    const int viewW = r.viewW, viewH = r.viewH;

    drawText("SIGNAL LOST", 0.95f, 0.55f + pulse * 0.35f, beat, 52, viewW, viewH);
    drawText("SCENE // " + scene, 0.95f, 0.5f, beat, 40, viewW, viewH);
    drawText("SYSTEM FAULT // AWAITING RECOVERY", 0.6f, 0.4f, beat, 24, viewW, viewH);

    ::glDisable(::gl::BLEND);
  }
}

void DegradedFX::blit(unsigned tex, const EffectContext& ctx) {
  if (!blitProg_) return;
  const Renderer& r = *ctx.r;
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, r.viewW, r.viewH);
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);
  blitProg_->use();
  blitProg_->set1i("uTex", 0);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, tex);
  r.fsTriangle.draw(3);
}

void DegradedFX::drawText(const std::string& text, float alpha, float glow, float beat, int sizePx, int viewW, int viewH) {
  if (!textProg_ || !assets_) return;
  textMesh_.build({{text, -1.0f, 0.3f}}, assets_->fontMetrics, {viewW, viewH, sizePx}, 0.55f);
  textProg_->use();
  textProg_->set1i("uTex", 0);
  textProg_->set1f("uAlpha", alpha);
  textProg_->set1f("uGlow", glow);
  textProg_->set1f("uPulse", beat);
  textProg_->set1f("uTime", 0);
  textProg_->set1f("uCycle", 0);
  textProg_->set1f("uWhite", 1);
  assets_->fontTex.bind(0);
  textMesh_.draw();
}

}  // namespace ns
