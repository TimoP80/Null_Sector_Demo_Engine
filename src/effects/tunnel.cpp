// ---------------------------------------------------------------------------
// TunnelFX - renders the neon tunnel raymarcher (tunnel.frag, uMode 0) and
// the reprise (uMode 1, morphing + glitch). The per-kick strobe is driven by
// the shared KickFlash detector (audio analyser react.kick - every real kick
// drum hit retriggers the flash, tempo-independent).
// ---------------------------------------------------------------------------
#include "effects/tunnel.hpp"
#include "engine/audio.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"

namespace ns {

void TunnelFX::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "quantum_tunnel.frag");
}

void TunnelFX::render(EffectContext& ctx) {
  if (!prog_) return;
  const float flash = kick_.update(ctx);

  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);

  // in-scene handoff: uTransition 0..1 over the section's first two beats.
  // The outgoing scene (intro logo at section 1, the logo climax at the
  // reprise) dissolves into the tunnel - the shader mixes it in before its
  // reprise glitch so the ghost tears the fading frame apart.
  const float trans = ctx.timeline ? ctx.timeline->s.transition : 1.0f;
  const unsigned prevTex = ctx.post ? ctx.post->prevFrameTex() : 0;

  prog_->use();
  prog_->set1f("uMode", mode);
  prog_->set1f("uFlash", flash);
  prog_->set1f("uTransition", trans);
  if (prevTex && trans < 0.999f) {
    prog_->set1i("uPrevScene", 9);
    ::glActiveTexture(::gl::TEXTURE9);
    ::glBindTexture(::gl::TEXTURE_2D, prevTex);
  }
  ctx.r->fsTriangle.draw(3);

  if (ctx.post) kick_.applyPost(ctx, flash, ctx.timeline->s.beatPulse);
}

}  // namespace ns
