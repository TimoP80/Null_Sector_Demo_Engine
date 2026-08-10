// ---------------------------------------------------------------------------
// SceneFX + ParticleStormFX - see scene.hpp. Each render drives the shared
// KickFlash detector (edge-detects the audio kick analyser, decays the flash
// envelope) and applies it to the uFlash uniform + the post params, so every
// scene slams on the kick drum exactly like the tunnel does.
// ---------------------------------------------------------------------------
#include "effects/scene.hpp"
#include "engine/assets.hpp"    // Assets (fontTex + fontMetrics full defs)
#include "engine/audio.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ns {

// ---------------------------------------------------------------------------
// SceneFX
// ---------------------------------------------------------------------------
void SceneFX::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", fragFile_);
  perf_.setLabel(fragFile_);
  if (!fontFile_.empty()) {
    auto atlas = std::make_unique<Assets>(buildTrueTypeFontAtlas(fontFile_));
    if (atlas->fontTex.tex) fontAsset_ = std::move(atlas);
    else Log::warn("SCENE", "font load failed for '" + fontFile_ + "'; using default atlas");
  }
  if (!textureFile_.empty()) {
    auto texture = std::make_unique<Texture>();
    if (loadPngAsset(textureFile_, *texture)) textureAsset_ = std::move(texture);
    else Log::warn("SCENE", "texture load failed for '" + textureFile_ + "'");
  }
}

void SceneFX::setFontFile(const std::string& path) {
  if (fontFile_ == path) return;
  fontFile_ = path;
  fontAsset_.reset();
  if (fontFile_.empty() || !prog_) return;
  auto atlas = std::make_unique<Assets>(buildTrueTypeFontAtlas(fontFile_));
  if (atlas->fontTex.tex) fontAsset_ = std::move(atlas);
  else Log::warn("SCENE", "font load failed for '" + fontFile_ + "'; using default atlas");
}

void SceneFX::setTextureFile(const std::string& path, const char* uniform, int unit) {
  textureFile_ = path;
  textureUniform_ = uniform ? uniform : "uFillTexture";
  textureUnit_ = std::max(0, unit);
  textureAsset_.reset();
  if (textureFile_.empty() || !prog_) return;
  auto texture = std::make_unique<Texture>();
  if (loadPngAsset(textureFile_, *texture)) textureAsset_ = std::move(texture);
  else Log::warn("SCENE", "texture load failed for '" + textureFile_ + "'");
}

void SceneFX::render(EffectContext& ctx) {
  if (!prog_) return;
  perf_.beginFrame();  // GPU timing: PerfTimer ring (see perftimer.hpp)
  const float flash = kick_.update(ctx);

  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);

  prog_->use();
  // Shader Lab and hand-authored quad shaders receive the same stable inputs
  // as the shared UBO-driven production effects. Missing uniforms are cheap
  // no-ops, so existing scenes remain unchanged.
  if (ctx.r) prog_->set2f("uResolution", (float)ctx.r->resW, (float)ctx.r->resH);
  prog_->set1f("uTime", ctx.time);
  prog_->set1f("uDeltaTime", ctx.dt);
  const TimelineState* ts = ctx.timeline ? &ctx.timeline->s : nullptr;
  const float bpm = ctx.timeline && ctx.timeline->beatSec() > 0
                        ? 60.0f / ctx.timeline->beatSec() : 120.0f;
  prog_->set1f("uBPM", bpm);
  prog_->set1f("uBeat", ts ? ts->beat : 0.0f);
  prog_->set1f("uBar", ts ? ts->bar : 0.0f);
  prog_->set1f("uBeatPhase", ts ? ts->beatPhase : 0.0f);
  prog_->set1f("uProgress", ts ? ts->sectionProgress : 0.0f);
  prog_->set1f("uIntensity", ts ? ts->intensity : 1.0f);
  prog_->set1f("uBass", ctx.audio ? ctx.audio->react.bass.load() : 0.0f);
  prog_->set1f("uMid", ctx.audio ? ctx.audio->react.mid.load() : 0.0f);
  prog_->set1f("uTreble", ctx.audio ? ctx.audio->react.treble.load() : 0.0f);
  prog_->set1f("uAudioLevel", ctx.audio ? ctx.audio->react.energy.load() : 0.0f);
  prog_->set1f("uKick", ctx.audio ? ctx.audio->react.kick.load() : 0.0f);
  prog_->set1f("uSnare", ctx.audio ? ctx.audio->react.onset.load() : 0.0f);
  prog_->set4f("uColor", 0.05f, 0.95f, 0.85f, 1.0f);
  prog_->set4f("uColor2", 0.35f, 0.15f, 1.0f, 1.0f);
  prog_->set1f("uSpeed", 1.0f);
  prog_->set1f("uScale", 1.0f);
  prog_->set1f("uTextScale", 1.0f);
  prog_->set2f("uTextPos", 0.0f, 0.0f);
  prog_->set1f("uTextRotation", 0.0f);
  prog_->set1f("uTextOpacity", 1.0f);
  prog_->set1f("uFlash", flash);
  prog_->set1f("uMode", mode);   // no-op for shaders that don't declare it
  // when the scene renders into a reduced-size target (neuralnet), hand the
  // shader the ACTUAL render size so its gl_FragCoord-derived rays stay
  // consistent - the shared uRes describes the full-res hdr target
  if (renderScale < 1.0f) {
    prog_->set2f("uSceneRes", (float)(ctx.r->resW * renderScale),
                              (float)(ctx.r->resH * renderScale));
  }
  if (handoff_) {
    // in-scene handoff: uTransition 0..1 over the section's first two beats.
    // Shaders that declare uPrevScene mix/ignite from the outgoing frame -
    // the prev texture must be bound or that blend reads garbage.
    const float trans = ctx.timeline ? ctx.timeline->s.transition : 1.0f;
    const unsigned prevTex = ctx.post ? ctx.post->prevFrameTex() : 0;
    prog_->set1f("uTransition", trans);
    // bind the outgoing frame only while the handoff is live; after the
    // two-beat window the shaders' uTransition<0.999 gates go dormant
    if (prevTex && trans < 0.999f) {
      prog_->set1i("uPrevScene", 9);
      ::glActiveTexture(::gl::TEXTURE9);
      ::glBindTexture(::gl::TEXTURE_2D, prevTex);
    }
  } else {
    // standalone render: no in-scene handoff. voxel.frag defaults uTransition
    // to 0, which would activate its prev-scene blend against an unbound
    // texture and collapse buildings to metaball blobs - pin it to 1.
    prog_->set1f("uTransition", 1.0f);
  }
  if (texName_ && tex_) {
    prog_->set1i(texName_, texUnit_);
    ::glActiveTexture(::gl::TEXTURE0 + texUnit_);
    ::glBindTexture(::gl::TEXTURE_2D, tex_);
  }
  prog_->set1f("uTextureEnabled", textureAsset_ ? 1.0f : 0.0f);
  prog_->set1f("uTextureMix", 1.0f);
  prog_->set1f("uTextureScale", 1.0f);
  prog_->set1f("uTextureScroll", 0.0f);
  if (textureAsset_) {
    prog_->set1i(textureUniform_.c_str(), textureUnit_);
    textureAsset_->bind(textureUnit_);
  }
  for (const auto& kv : extraUniforms) {
    prog_->set1f(kv.first.c_str(), kv.second);
  }
  const Assets* fontAssets = fontAsset_ ? fontAsset_.get() : ctx.assets;
  if (useFont_ && fontAssets && fontAssets->fontTex.tex) {
    // TrueType atlas for in-shader glyph drawing (uFont on unit 11, which the
    // post pipeline never touches - it uses units 0..8, prev 9, scene tex 10).
    // A shader can select a project font without changing the global runtime
    // font used by other scenes.
    const FontMetrics& fm = fontAssets->fontMetrics;
    prog_->set1i("uFont", 11);
    // `uText` is the same atlas for Shader Lab exports; advanced shaders can
    // sample it directly, while production shaders continue using uFont.
    prog_->set1i("uText", 11);
    prog_->setVec2("uAtlas", (float)fm.atlasW, (float)fm.atlasH);
    prog_->setVec2("uCell", (float)fm.cellW, (float)fm.cellH);
    ::glActiveTexture(::gl::TEXTURE0 + 11);
    ::glBindTexture(::gl::TEXTURE_2D, fontAssets->fontTex.tex);
  }
  ctx.r->fsTriangle.draw(3);

  if (ctx.post) kick_.applyPost(ctx, flash, ctx.timeline->s.beatPulse);

  perf_.endFrame();
  // periodic note so authors can see what a scene + its renderScale costs
  if (perf_.logDue()) {
    const std::string extra = renderScale < 1.0f
                                  ? " (renderScale " + fmtMs(renderScale) + ")"
                                  : "";
    Log::info("SCENE", perf_.logLine() + extra);
  }
}

// ---------------------------------------------------------------------------
// ParticleStormFX
// ---------------------------------------------------------------------------
void ParticleStormFX::init(EffectContext&) {
  prog_ = std::make_unique<Shader>(vertFile_.c_str(), fragFile_.c_str());
  prevProg_ = std::make_unique<Shader>("fullscreen.vert", prevFrag_.c_str());
  perf_.setLabel(fragFile_);

  // per-particle seed pairs (vec4s): phase seeds + attractor index, then
  // orbit radius / speed / color seeds. Deterministic spread so the storm
  // fills the volume without a CPU simulation.
  const int n = count_;
  std::vector<float> a((size_t)n * 4), b((size_t)n * 4);
  uint32_t rng = 0x9E3779B9u;
  for (int i = 0; i < n; i++) {
    auto rnd = [&rng]() {
      rng = rng * 1664525u + 1013904223u;
      return (float)((rng >> 8) & 0xFFFF) / 65535.0f;
    };
    const float ai = (float)(i & 3);   // 4 attractors
    a[(size_t)i * 4 + 0] = rnd();
    a[(size_t)i * 4 + 1] = rnd();
    a[(size_t)i * 4 + 2] = rnd();
    a[(size_t)i * 4 + 3] = ai;
    b[(size_t)i * 4 + 0] = rnd();                  // orbit radius seed
    b[(size_t)i * 4 + 1] = 0.5f + rnd() * 1.5f;    // speed seed
    b[(size_t)i * 4 + 2] = rnd();                  // color seed
    b[(size_t)i * 4 + 3] = 0.0f;
  }
  cloud_.setBuffer(0, a.data(), n * 4, 4, ::gl::DYNAMIC_DRAW);
  cloud_.setBuffer(1, b.data(), n * 4, 4, ::gl::DYNAMIC_DRAW);
}

void ParticleStormFX::render(EffectContext& ctx) {
  if (!prog_) return;
  perf_.beginFrame();  // GPU timing: PerfTimer ring (see perftimer.hpp)
  const float flash = kick_.update(ctx);
  // in-scene handoff: 0..1 over the section's first two beats (1 = handoff
  // done, so the ignite block and the fading base layer both go dormant)
  const float trans = ctx.timeline ? ctx.timeline->s.transition : 1.0f;
  const unsigned prevTex = ctx.post ? ctx.post->prevFrameTex() : 0;

  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);

  // base layer: the outgoing scene's final frame fades out while the swarm
  // tears out of it (source-over on the freshly cleared HDR target)
  if (trans < 0.999f) {
    prevProg_->use();
    prevProg_->set1f("uTransition", trans);
    prevProg_->set1i("uPrevScene", 9);
    ::glActiveTexture(::gl::TEXTURE9);
    ::glBindTexture(::gl::TEXTURE_2D, prevTex);
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    ctx.r->fsTriangle.draw(3);
    ::glDisable(::gl::BLEND);
  }

  ::glEnable(::gl::PROGRAM_POINT_SIZE);
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::ONE, ::gl::ONE);   // additive sparks

  prog_->use();
  prog_->set1f("uExplode", ctx.timeline->s.beatPulse * 0.6f);
  prog_->set1f("uTrail", 0.0f);
  prog_->set1f("uPointSize", 1.0f);
  prog_->set1f("uTransition", trans);
  prog_->set1f("uFlash", flash);
  prog_->set1f("uMode", mode);   // no-op for shaders that don't declare it
  if (prevTex) {
    prog_->set1i("uPrevScene", 9);
    ::glActiveTexture(::gl::TEXTURE9);
    ::glBindTexture(::gl::TEXTURE_2D, prevTex);
  }
  cloud_.draw(count_);

  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::PROGRAM_POINT_SIZE);

  if (ctx.post) kick_.applyPost(ctx, flash, ctx.timeline->s.beatPulse);

  perf_.endFrame();
  if (perf_.logDue()) {
    Log::info("SCENE", perf_.logLine() + " (particles " + std::to_string(count_) + ")");
  }
}

}  // namespace ns
