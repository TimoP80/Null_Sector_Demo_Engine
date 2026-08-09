// ---------------------------------------------------------------------------
// GreetingsFX - the show's greetings poster + credits roll (sections 9/10).
// These sections used to be the show's ending, but the full-track extension
// left them mid-show rendering the SIGNAL LOST placeholder. The intended
// content was authored but never wired: three ready shaders compose the
// oldschool demoscene poster -
//
//   greet_synth.frag   fullscreen synthwave backdrop (scanline sun, wireframe
//                      mountains, perspective grid, copper frame)
//   greet_logo.frag    NULL SECTOR wordmark hero, faithful to the artwork
//   greet_mark.frag    per-group logo marks, one style per group (chrome,
//                      distress, glitch, outline, pixel, neon) drawn from the
//                      shared TrueType atlas via text.vert
//
// mode 0 (greetings): backdrop + wordmark hero (settle-in fit) + the six
// group marks popping in staggered. mode 1 (credits): dimmed backdrop + the
// credit lines rolling through the atlas text pass. All envelopes are
// secT-based so a schedule re-time stays in sync; the backdrop, frame and
// glow all ride the pulse/intensity.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include <memory>

namespace ns {

class GreetingsFX : public Effect {
public:
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  float mode = 0.0f;   // 0 = greetings poster, 1 = credits roll

  /** bind the wordmark artwork (the same texture the logo scene uses). */
  void setWordmark(unsigned tex, float aspect) {
    wordmarkTex_ = tex;
    wordmarkAspect_ = aspect;
  }

private:
  void renderGreetings(EffectContext& ctx, float secT);
  void renderCredits(EffectContext& ctx, float secT);

  std::unique_ptr<Shader> synthProg_;   // greet_synth.frag
  std::unique_ptr<Shader> logoProg_;    // greet_logo.frag
  std::unique_ptr<Shader> markProg_;    // text.vert + greet_mark.frag
  std::unique_ptr<Shader> creditProg_;  // text.vert + text.frag
  TextMesh markMesh_;
  TextMesh creditMesh_;
  const Assets* assets_ = nullptr;
  unsigned wordmarkTex_ = 0;
  float wordmarkAspect_ = 1.0f;
  bool ok_ = false;
};

}  // namespace ns
