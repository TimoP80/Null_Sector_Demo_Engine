// ---------------------------------------------------------------------------
// KickFlash - reusable per-kick strobe detector (extracted from TunnelFX).
// Every scene that should "slam on the kick drum" uses one of these: rising
// edge on the audio analyser's react.kick (a std::atomic<float> written by
// the audio thread, spiking on real bass transients in the WAV - fully
// tempo-independent, no reliance on the beat grid). A min-interval gate caps
// the strobe rate so a slow-attack kick can't pin the flash at 1.0. The
// decaying 0..1 envelope feeds both a uFlash shader uniform and the post FX
// (bloom/exposure/glitch/heat/kick) so the whole frame reacts per hit.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/audio.hpp"
#include "engine/postprocess.hpp"
#include <algorithm>

namespace ns {

class KickFlash {
public:
  /** advance one frame; returns the current 0..1 flash envelope. Call once per
   *  render, before drawing, and feed the result to the uFlash uniform. */
  float update(EffectContext& ctx) {
    const float kick = ctx.audio->react.kick.load();
    gate_ = std::max(0.0f, gate_ - ctx.dt);
    if (kick > prevKick_ + 0.04f && kick > 0.10f && gate_ <= 0.0f) {
      flash_ = 1.0f;
      gate_ = 0.12f;   // ~max 8 strobes/sec; kicks at 216 BPM are ~0.28s apart
    } else {
      flash_ = std::max(0.0f, flash_ - ctx.dt * 5.0f);
    }
    prevKick_ = kick;
    return flash_;
  }

  /** drive the music post params from the envelope (per scene, per frame) */
  void applyPost(EffectContext& ctx, float flash, float pulse = 0.0f) {
    if (!ctx.post) return;
    ctx.post->fx.bloom = 1.1f + flash * 0.5f;
    ctx.post->fx.glitch = pulse * 0.08f + flash * 0.12f;
    ctx.post->fx.exposure = 1.0f + flash * 0.35f;
    ctx.post->fx.heat = flash * 0.1f;
    ctx.post->fx.kick = flash;
  }

  void reset() { flash_ = 0.0f; prevKick_ = 0.0f; gate_ = 0.0f; }

private:
  float flash_ = 0.0f;    // 0..1 decaying flash per kick hit
  float prevKick_ = 0.0f; // last frame's analyser kick value (edge detect)
  float gate_ = 0.0f;     // min interval between triggers (strobe rate cap)
};

}  // namespace ns
