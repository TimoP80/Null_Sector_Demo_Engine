// ---------------------------------------------------------------------------
// TunnelFX - the first realtime 3D scene. After the intro dissolves through
// the logo center, the camera accelerates into the neon tunnel (tunnel.frag,
// the engine's scene 1). Minimal wrapper: renders the raymarched tunnel
// fullscreen and lets the main loop drive the flying camera.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "effects/kickflash.hpp"
#include "engine/shader.hpp"
#include <memory>

namespace ns {

class TunnelFX : public Effect {
public:
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;

  /** shader mode: 0 = tunnel, 1 = reprise (morphing cross-section + glitch) */
  float mode = 0.0f;

private:
  std::unique_ptr<Shader> prog_;
  KickFlash kick_;
};

}  // namespace ns
