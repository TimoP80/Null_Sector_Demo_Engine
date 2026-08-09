// ---------------------------------------------------------------------------
// Fullscreen image card (aspect-fit, straight to the default framebuffer):
// fade in, a held display, a short glitch burst, then fade out - no HDR, no
// post pipeline, self-contained (no common.glsl). Used for the pre-show logo
// splash (assets/splash.png, shown in silence before the show clock starts)
// AND the end-of-show outro card (assets/ghost_outro.png, played after the
// last section with the music still running, then the show loops).
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/texture.hpp"
#include <memory>

namespace ns {

class Shader;

class SplashFX {
public:
  ~SplashFX() = default;

  /** load assets/<png> + compile splash.frag. Returns false (and prints a
   *  warning) when the PNG is missing, so main() can skip the card. */
  bool init(const EffectContext& ctx, const char* png = "splash.png");

  bool valid() const { return prog_ != nullptr; }

  /** draw one splash frame at splash-local time t (seconds). The phase
   *  envelope (fade in / hold / glitch / fade out) lives here so main()
   *  only tracks a wall-clock start time. */
  void render(const EffectContext& ctx, float t);

  // phase boundaries (seconds from splash start)
  static constexpr float kFadeInEnd  = 1.2f;   // 0.0 -> 1.2 fade in
  static constexpr float kHoldEnd    = 3.6f;   // 1.2 -> 3.6 held display
  static constexpr float kGlitchEnd  = 4.4f;   // 3.6 -> 4.4 glitch burst
  static constexpr float kFadeOutEnd = 5.2f;   // 4.4 -> 5.2 fade out, done

private:
  Texture logo_;
  std::unique_ptr<Shader> prog_;
};

}  // namespace ns
