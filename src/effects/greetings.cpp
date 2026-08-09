// ---------------------------------------------------------------------------
// NULL SECTOR // GHOST IN THE MACHINE - greetings poster + credits driver.
// See greetings.hpp for the composition notes. The driver renders three
// fullscreen/quad passes for the poster (backdrop, wordmark, group marks)
// and one atlas-text pass for the credits roll, all against the shared
// timeline so the envelopes stay bar-locked.
// ---------------------------------------------------------------------------
#include "effects/greetings.hpp"

#include "engine/assets.hpp"
#include "engine/gl.hpp"
#include "engine/math.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"

namespace ns {

// ---------------------------------------------------------------------------
// shared per-pass uniforms: the greet shaders read the standalone uRes/uTime
// + the shared timeline values. Greet shaders are NOT raymarchers - they do
// not consume the camera, so the static fallback camera is fine.
// ---------------------------------------------------------------------------

static void setPassCommon(Shader& p, const EffectContext& ctx) {
  const Renderer& r = *ctx.r;
  p.set2f("uRes", (float)r.resW, (float)r.resH);
  p.set1f("uTime", ctx.time);
  p.set1f("uIntensity", ctx.timeline->s.intensity);
  p.set1f("uPulse", ctx.timeline->s.beatPulse);
  p.set1f("uDim", 1.0f);         // full brightness unless the credits mode dims
  p.set1f("uSunScale", 1.0f);    // full sun unless the greetings mode shrinks it
}

void GreetingsFX::init(EffectContext& ctx) {
  assets_ = ctx.assets;
  // Shader throws on build failure; main.cpp wraps this in try/catch like
  // every other effect, so a missing greet shader is fatal rather than a
  // silent SIGNAL LOST fallback.
  synthProg_ = std::make_unique<Shader>("fullscreen.vert", "greet_synth.frag");
  logoProg_ = std::make_unique<Shader>("fullscreen.vert", "greet_logo.frag");
  markProg_ = std::make_unique<Shader>("text.vert", "greet_mark.frag");
  creditProg_ = std::make_unique<Shader>("text.vert", "text.frag");
  ok_ = true;
}

void GreetingsFX::render(EffectContext& ctx) {
  if (!ok_) return;
  ctx.camera->dofAperture = 0;
  const float dur = std::max(ctx.timeline->s.duration, 1e-4f);
  const float secT = std::max(0.0f, std::min(ctx.timeline->s.sectionLocal / dur, 1.0f));
  if (mode < 0.5f) renderGreetings(ctx, secT);
  else renderCredits(ctx, secT);
}

// ---------------------------------------------------------------------------
// mode 0 - greetings poster: backdrop, wordmark hero, staggered group marks
// ---------------------------------------------------------------------------

void GreetingsFX::renderGreetings(EffectContext& ctx, float secT) {
  const Renderer& r = *ctx.r;

  // --- 1. synthwave backdrop (opaque) ---------------------------------------
  ::glDisable(::gl::BLEND);
  synthProg_->use();
  setPassCommon(*synthProg_, ctx);
  // shrink the scanline sun so the six group marks have a clean dark band
  // below it (sun bottom edge moves from NDC ~-0.51 up to ~-0.31)
  synthProg_->set1f("uSunScale", 0.62f);
  r.fsTriangle.draw(3);

  // --- 2. NULL SECTOR wordmark hero (alpha-blended) ---------------------------
  if (wordmarkTex_) {
    // settle-in fit: the wordmark starts slightly undersized and eases up to
    // its hero band. The artwork's aspect (1920x1081) nearly matches the
    // window, so uFit is capped well below 1.0 - otherwise the hero pass
    // would fill the whole screen and hide the synth backdrop (sun,
    // mountains, grid) behind it. Entrance fade over the first 15% of the
    // section, exit fade over the last 12% so the credits handoff is clean.
    const float in = smoothstepf(0.0f, 0.15f, secT);
    const float out = 1.0f - smoothstepf(0.86f, 1.0f, secT);
    const float alpha = in * out;
    // the hero sits HIGH and compact so the synth backdrop - scanline sun,
    // wireframe mountains, perspective grid - stays visible in the lower
    // half of the frame (the old centred layout covered it with the
    // artwork's dark backdrop).
    const float fit = 0.44f + 0.10f * smoothstepf(0.05f, 0.45f, secT);
    logoProg_->use();
    setPassCommon(*logoProg_, ctx);
    logoProg_->set1i("uTex", 10);
    ::glActiveTexture(::gl::TEXTURE0 + 10);
    ::glBindTexture(::gl::TEXTURE_2D, wordmarkTex_);
    logoProg_->set1f("uImageAspect", wordmarkAspect_);
    logoProg_->set1f("uFit", fit);
    logoProg_->set1f("uOffsetY", 0.62f);   // hero band sits top-center, clear of the sun
    logoProg_->set1f("uAlpha", alpha);
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    r.fsTriangle.draw(3);
  }

  // --- 3. group marks: six styles, staggered pop-in ----------------------------
  // classic oldschool board - each group's name in its own logo font, with
  // the per-style variation shifting on the music pulse.
  if (assets_ && markProg_) {
    // pop stagger compressed (0.08..0.33) so all six marks are fully in by
    // secT ~0.45 and the complete lineup holds for ~5.5s before the exit
    // fade (0.9..1.0) - the old 0.20..0.90 stagger meant the last marks
    // barely appeared before the fade started, which is what made the
    // poster text read as unreadable.
    const struct { const char* name; int style; float seed; float tint; float pop; } groups[6] = {
      { "NULL SECTOR",          0, 0.10f, 0.0f, 0.08f },
      { "GHOST IN THE MACHINE", 2, 0.55f, 0.0f, 0.13f },
      { "THE INNER MACHINE",    1, 0.72f, 0.0f, 0.18f },
      { "QUANTUM TUNNEL",       3, 0.31f, 0.0f, 0.23f },
      { "NEURAL NETWORK",       4, 0.87f, 0.0f, 0.28f },
      { "REALITY CHECK",        5, 0.44f, 0.0f, 0.33f },
    };
    const float out = 1.0f - smoothstepf(0.9f, 1.0f, secT);
    const int viewW = r.viewW, viewH = r.viewH;

    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    markProg_->use();
    markProg_->set1i("uTex", 1);
    ::glActiveTexture(::gl::TEXTURE1);
    assets_->fontTex.bind(1);
    // each group is drawn from its own quad mesh so it can carry its own
    // style / seed / tint uniforms. The lines stack below the wordmark hero
    // (which occupies roughly the upper third of the frame).
    for (int i = 0; i < 6; i++) {
      const float popIn = smoothstepf(groups[i].pop, groups[i].pop + 0.12f, secT);
      const float alpha = popIn * out;
      if (alpha <= 0.001f) continue;
      // marks stack in the lower half (below the scanline sun), each with
      // its own style. NDC y descends from just under the hero band to the
      // footer so the poster reads: wordmark / sun+mountains / marks.
      // all six marks stack BELOW the shrunken scanline sun (uSunScale 0.62
      // moves its bottom edge up to NDC ~-0.32 including the halo) on the
      // dark floor, so the bright backdrop never washes the glyphs out.
      // Start at -0.39 with 0.105 spacing at 48px - the last mark lands at
      // NDC -0.915 (bottom edge -0.97), fully on-screen.
      const float y = -0.39f - (float)i * 0.105f;
      markMesh_.build({{groups[i].name, -1.0f, groups[i].seed}},
                      assets_->fontMetrics,
                      {viewW, viewH, 48}, y);
      if (markMesh_.empty()) continue;
      markProg_->set1f("uCycle", ctx.timeline->s.musicHue);
      markProg_->set1f("uAlpha", alpha);
      markProg_->set1f("uGlow", 1.4f + 0.8f * ctx.timeline->s.beatPulse);
      markProg_->set1f("uSeed", groups[i].seed);
      markProg_->set1i("uStyle", groups[i].style);
      markProg_->set1f("uTint", groups[i].tint);
      // solid black letterform first (readability backing), styled fill on top
      markProg_->set1f("uShadow", 1.0f);
      markMesh_.draw();
      markProg_->set1f("uShadow", 0.0f);
      markMesh_.draw();
    }
    ::glDisable(::gl::BLEND);
  }
}

// ---------------------------------------------------------------------------
// mode 1 - credits roll: dimmed backdrop + atlas-text credits
// ---------------------------------------------------------------------------

void GreetingsFX::renderCredits(EffectContext& ctx, float secT) {
  const Renderer& r = *ctx.r;

  // dimmed backdrop: the credits breathe over a faint poster (uDim scales
  // the synth pass down) so the section reads as a sign-off rather than a
  // second copy of the greetings poster, and the white text stays crisp.
  ::glDisable(::gl::BLEND);
  synthProg_->use();
  setPassCommon(*synthProg_, ctx);
  synthProg_->set1f("uDim", 0.42f);
  r.fsTriangle.draw(3);

  if (assets_ && creditProg_) {
    const float in = smoothstepf(0.02f, 0.25f, secT);
    const float out = 1.0f - smoothstepf(0.82f, 1.0f, secT);
    const float alpha = in * out * 0.92f;
    const int viewW = r.viewW, viewH = r.viewH;
    creditProg_->use();
    creditProg_->set1i("uTex", 1);
    ::glActiveTexture(::gl::TEXTURE1);
    assets_->fontTex.bind(1);
    creditProg_->set1f("uTime", ctx.time);
    creditProg_->set1f("uCycle", 0.0f);
    creditProg_->set1f("uAlpha", alpha);
    creditProg_->set1f("uGlow", 0.35f);
    creditProg_->set1f("uWhite", 1.0f);
    const TextLine lines[6] = {
      { "NULL SECTOR", -1.0f, 0.1f },
      { "GHOST IN THE MACHINE", -1.0f, 0.2f },
      { "", -1.0f, 0.0f },
      { "MUSIC // THE MACHINE", -1.0f, 0.3f },
      { "CODE // THE MACHINE", -1.0f, 0.3f },
      { "THANKS FOR WATCHING", -1.0f, 0.4f },
    };
    creditMesh_.build({lines, lines + 6}, assets_->fontMetrics,
                      {viewW, viewH, 34}, 0.05f);
    creditMesh_.draw();
    ::glDisable(::gl::BLEND);
  }
}

}  // namespace ns
