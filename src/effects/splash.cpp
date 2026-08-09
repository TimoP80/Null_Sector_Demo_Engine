#include "effects/splash.hpp"
#include "engine/assets.hpp"
#include "engine/gl.hpp"
#include "engine/renderer.hpp"
#include "engine/shader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ns {

bool SplashFX::init(const EffectContext& ctx, const char* png) {
  (void)ctx;
  if (!loadPngAsset(png, logo_)) {
    std::fprintf(stderr, "[SPLASH] assets/%s not found - skipping the card\n", png);
    return false;
  }
  try {
    prog_ = std::make_unique<Shader>("fullscreen.vert", "splash.frag");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[SPLASH] splash shader failed: %s\n", e.what());
    return false;
  }
  std::printf("[SPLASH] %s (%dx%d) - card ready\n", png, logo_.w, logo_.h);
  std::fflush(stdout);
  return true;
}

void SplashFX::render(const EffectContext& ctx, float t) {
  if (!prog_) return;
  const Renderer& r = *ctx.r;

  // --- phase envelope -------------------------------------------------------
  float alpha = 0.0f, glitch = 0.0f;
  if (t < kFadeInEnd) {
    alpha = t / kFadeInEnd;
  } else if (t < kHoldEnd) {
    alpha = 1.0f;
  } else if (t < kGlitchEnd) {
    alpha = 1.0f;
    const float u = (t - kHoldEnd) / (kGlitchEnd - kHoldEnd);
    glitch = std::min(1.0f, u * 1.4f) * (0.55f + 0.45f * std::sin(t * 26.0f));
  } else if (t < kFadeOutEnd) {
    const float u = (t - kGlitchEnd) / (kFadeOutEnd - kGlitchEnd);
    alpha = 1.0f - u;
    glitch = 0.25f + 0.35f * u;   // glitch tail as it fades out
  } else {
    alpha = 0.0f;
  }
  // smoothstep easing on the fades
  alpha = alpha * alpha * (3.0f - 2.0f * alpha);
  glitch = std::max(0.0f, std::min(1.0f, glitch));

  // --- draw straight to the window (no HDR / post pipeline) -----------------
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, r.viewW, r.viewH);
  ::glClearColor(0, 0, 0, 1);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);

  prog_->use();
  prog_->set1i("uTex", 0);
  prog_->set2f("uRes", (float)r.viewW, (float)r.viewH);
  prog_->set2f("uTexSize", (float)logo_.w, (float)logo_.h);
  prog_->set1f("uTime", t);
  prog_->set1f("uAlpha", alpha);
  prog_->set1f("uGlitch", glitch);
  logo_.bind(0);
  r.fsTriangle.draw(3);

  ::glDisable(::gl::BLEND);
}

}  // namespace ns
