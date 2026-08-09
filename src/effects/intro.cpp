// ---------------------------------------------------------------------------
// NULL SECTOR // GHOST IN THE MACHINE - intro prototype implementation.
// See intro.hpp for the timeline / architecture notes.
// ---------------------------------------------------------------------------
#include "effects/intro.hpp"
#include "engine/assets.hpp"
#include "engine/camera.hpp"
#include "engine/gl.hpp"
#include "engine/math.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include <cmath>

namespace ns {

// --- shared uniform helpers ----------------------------------------------------

static void setCommon(Shader& p, const EffectContext& ctx, float parScale) {
  const Renderer& r = *ctx.r;
  p.set2f("uRes", (float)r.resW, (float)r.resH);
  p.set1f("uTime", ctx.time);
  p.setVec2("uParallax", ctx.camera->pos[0] * parScale, ctx.camera->pos[1] * parScale);
}

static void setFontUniforms(Shader& p, const EffectContext& ctx) {
  const FontMetrics& fm = ctx.assets->fontMetrics;
  p.setVec2("uAtlas", (float)fm.atlasW, (float)fm.atlasH);
  p.setVec2("uCell", (float)fm.cellW, (float)fm.cellH);
  p.setVec2("uFontGrid", (float)fm.cols, (float)fm.rows);
  p.set1i("uFont", 1);
  ctx.assets->fontTex.bind(1);
}

// --- GridRenderer ---------------------------------------------------------------

void GridRenderer::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_grid.frag");
}

void GridRenderer::render(EffectContext& ctx, float wake) {
  if (wake <= 0.001f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.08f);
  prog_->set1f("uWake", wake);
  prog_->set1f("uIntensity", ctx.timeline->s.intensity);
  ctx.r->fsTriangle.draw(3);
}

// --- CircularScanner -------------------------------------------------------------

void CircularScanner::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_rings.frag");
}

void CircularScanner::render(EffectContext& ctx, float diag, float quiet, float build) {
  if (diag <= 0.001f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.16f);
  prog_->set1f("uDiag", diag);
  prog_->set1f("uQuiet", quiet);
  prog_->set1f("uBuild", build);
  prog_->set1f("uSeed", 0.7f);
  ctx.r->fsTriangle.draw(3);
}

// --- FFTBars ----------------------------------------------------------------------

void FFTBars::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_fft.frag");
}

void FFTBars::render(EffectContext& ctx, float diag) {
  if (diag <= 0.001f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.0f);
  prog_->set1f("uDiag", diag);
  prog_->set1f("uSeed", 2.3f);
  ctx.r->fsTriangle.draw(3);
}

// --- HexBackground -----------------------------------------------------------------

void HexBackground::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_hex.frag");
}

void HexBackground::render(EffectContext& ctx, float diag) {
  if (diag <= 0.001f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.1f);
  prog_->set1f("uDiag", diag);
  setFontUniforms(*prog_, ctx);
  ctx.r->fsTriangle.draw(3);
}

// --- NodeGraph ----------------------------------------------------------------------

void NodeGraph::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_graph.frag");
}

void NodeGraph::render(EffectContext& ctx, float diag) {
  if (diag <= 0.001f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.18f);
  prog_->set1f("uDiag", diag);
  ctx.r->fsTriangle.draw(3);
}

// --- ParticleOverlay -----------------------------------------------------------------

void ParticleOverlay::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_particles.frag");
}

void ParticleOverlay::render(EffectContext& ctx, float alpha, float flow, float burst, float stream) {
  if (alpha <= 0.01f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.12f);
  prog_->set1f("uAlpha", alpha);
  prog_->set1f("uFlow", flow);
  prog_->set1f("uBurst", burst);
  prog_->set1f("uStream", stream);
  ctx.r->fsTriangle.draw(3);
}

// --- GhostPass --------------------------------------------------------------------------

void GhostPass::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_ghost.frag");
}

void GhostPass::render(EffectContext& ctx, unsigned sceneTex, float ghost) {
  if (ghost <= 0.004f || !prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.4f);
  prog_->set1f("uGhost", ghost);
  prog_->set1i("uTex", 0);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, sceneTex);
  ctx.r->fsTriangle.draw(3);
}

// --- LogoAssembler -----------------------------------------------------------------------

void LogoAssembler::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("fullscreen.vert", "intro_logo.frag");
}

void LogoAssembler::render(EffectContext& ctx, unsigned sceneTex, float asmb, float scan,
                           float dark, float zoom) {
  if (!prog_) return;
  prog_->use();
  setCommon(*prog_, ctx, 0.2f);
  prog_->set1f("uAsmb", asmb);
  prog_->set1f("uScan", scan);
  prog_->set1f("uDark", dark);
  prog_->set1f("uZoom", zoom);
  prog_->set1i("uScene", 0);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, sceneTex);
  setFontUniforms(*prog_, ctx);
  ctx.r->fsTriangle.draw(3);
}

// --- DiagnosticText -----------------------------------------------------------------------

void DiagnosticText::init(EffectContext& ctx) {
  prog_ = std::make_unique<Shader>("text.vert", "intro_boot.frag");
  font_ = ctx.assets->fontMetrics;
}

void DiagnosticText::line(EffectContext& ctx, const std::string& text, float centerY, int sizePx,
                          float progress, float alpha, float seed, int style, float centerX) {
  if (alpha <= 0.01f || text.empty() || !prog_) return;
  const Renderer& r = *ctx.r;
  mesh_.build({{text, -1.0f, seed}}, font_, {r.viewW, r.viewH, sizePx, 0, centerX}, centerY);
  if (mesh_.empty()) return;
  prog_->use();
  prog_->set1i("uTex", 0);
  prog_->set1f("uTime", ctx.time);
  prog_->set1f("uAlpha", alpha);
  prog_->set1f("uProgress", progress);
  prog_->set1f("uSeed", seed);
  prog_->set1f("uChars", (float)text.size());
  prog_->set1i("uStyle", style);
  // atlas dims for the dark-halo dilation in intro_boot.frag
  prog_->setVec2("uAtlas", (float)font_.atlasW, (float)font_.atlasH);
  prog_->setVec2("uCell", (float)font_.cellW, (float)font_.cellH);
  ctx.assets->fontTex.bind(0);
  mesh_.draw();
}

// --- IntroFX ---------------------------------------------------------------------------------

void IntroFX::init(EffectContext& ctx) {
  const Renderer& r = *ctx.r;
  const TextureOpts to{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  sceneA_ = FrameTarget::color(r.resW, r.resH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, to);
  sceneB_ = FrameTarget::color(r.resW, r.resH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, to);

  grid_.init(ctx);
  scanner_.init(ctx);
  fft_.init(ctx);
  hex_.init(ctx);
  graph_.init(ctx);
  particles_.init(ctx);
  ghost_.init(ctx);
  logo_.init(ctx);
  boot_.init(ctx);
  passthrough_ = std::make_unique<Shader>("fullscreen.vert", "passthrough.frag");
}

void IntroFX::resize(EffectContext& ctx) {
  const Renderer& r = *ctx.r;
  const TextureOpts to{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  sceneA_.destroy();
  sceneB_.destroy();
  sceneA_ = FrameTarget::color(r.resW, r.resH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, to);
  sceneB_ = FrameTarget::color(r.resW, r.resH, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, to);
}

void IntroFX::drawBoot(EffectContext& ctx, float t) {
  struct Msg { const char* text; float at; float y; int size; int style; };
  static const Msg msgs[] = {
    // boot sequence reconstructs during the Communication phase (0:21+)
    {"NULL SECTOR", 21.0f, 0.34f, 54, 1},
    {"INITIALIZING...", 21.8f, 0.18f, 26, 0},
    {"LOADING VISUAL CORE", 22.6f, 0.08f, 26, 0},
    {"SYNC ENGINE READY", 23.4f, -0.02f, 26, 0},
    {"AUDIO LINK ESTABLISHED", 24.2f, -0.12f, 26, 0},
    {"SCANNING MEMORY", 25.0f, -0.22f, 26, 0},
    {"SIGNAL FOUND", 25.8f, -0.32f, 26, 0},
    {"BOOT COMPLETE", 26.6f, -0.42f, 28, 1},
    // diagnostic captions drift by as the panels stay up (0:29-0:46)
    {"HEX STREAM ACTIVE", 29.5f, 0.10f, 22, 0},
    {"NODE GRAPH ONLINE", 32.5f, 0.02f, 22, 0},
    {"FFT SYNC LOCKED", 35.5f, -0.06f, 22, 0},
    {"NETWORK DIAGRAM", 38.5f, -0.14f, 22, 0},
    {"ANOMALY DETECTED", 41.5f, -0.22f, 24, 1},
    {"SIGNAL DEGRADING", 44.5f, -0.30f, 22, 0},
  };
  const int N = (int)(sizeof(msgs) / sizeof(msgs[0]));
  // text size is pixel-fixed in the layout; scale it with the view height so
  // the log stays readable on taller windows too (the logo shader sizes are
  // already NDC-relative). Reference design height: 900px.
  const float sizeScale = std::clamp((float)ctx.r->viewH / 900.0f, 1.0f, 2.2f);
  for (int i = 0; i < N; i++) {
    const Msg& m = msgs[i];
    const float prog = smoothstepf(m.at, m.at + 0.7f, t);
    if (prog <= 0.01f) continue;
    // each line holds ~2.2s then fades before the build-up takes over
    const float alpha = prog * (1.0f - smoothstepf(m.at + 2.2f, m.at + 3.4f, t));
    if (alpha <= 0.01f) continue;
    const int size = (int)(m.size * sizeScale + 0.5f);
    boot_.line(ctx, m.text, m.y, size, prog, alpha, 1.0f + (float)i * 0.37f, m.style);
  }
}

void IntroFX::render(EffectContext& ctx) {
  const float t = ctx.timeline->s.sectionLocal;   // 0..~66.4s in the intro section

  // --- timeline envelopes: three music-timed phases ---------------------------
  // Awakening 0:00-0:20 - thin grid + lone scanner + LEDs + drifting particles
  const float wake = smoothstepf(2.0f, 8.0f, t) * (1.0f - smoothstepf(60.0f, 64.0f, t));
  const float lone = smoothstepf(4.0f, 8.0f, t) * (1.0f - smoothstepf(19.0f, 21.0f, t));
  // Communication 0:21-0:48 - diagnostics cluster + boot log (voice-focused)
  const float comm = smoothstepf(21.0f, 24.0f, t);
  const float diag = comm * (1.0f - smoothstepf(46.0f, 49.0f, t));       // fft/hex/graph
  const float scannerEnv = comm * (1.0f - smoothstepf(59.0f, 61.0f, t)); // scanner stays on
  // dim stays up through SIGNAL DEGRADING (44.5s + 3.4s fade) so every
  // caption keeps the same readable backdrop
  const float bootEnv = smoothstepf(20.6f, 21.2f, t) * (1.0f - smoothstepf(46.5f, 48.0f, t));
  const float quiet = 1.0f - 0.55f * bootEnv;
  // Build-up 0:49-1:06 - scanner accelerates, streams, ripples, ghost
  const float build = smoothstepf(49.0f, 54.0f, t);
  const float ghost = smoothstepf(49.0f, 53.0f, t) * (1.0f - smoothstepf(61.0f, 64.0f, t));
  const float flow = 1.0f - 2.0f * ghost;                        // streams reverse at 0:49
  // Climax 1:03+ - logo assembles, camera flies through into the tunnel
  const float asmb = smoothstepf(62.0f, 64.0f, t);
  const float scan = smoothstepf(61.5f, 62.5f, t);
  const float dark = smoothstepf(62.5f, 63.5f, t);
  const float zoom = smoothstepf(64.0f, 66.3f, t);
  const float burst = smoothstepf(63.5f, 64.3f, t);
  const float pAlpha = 0.4f + ghost * 0.4f + burst * 0.6f;

  Renderer& r = *ctx.r;

  // --- pass 1: base composition into sceneA_ -----------------------------------
  sceneA_.bind();
  ::glClearColor(0, 0, 0, 1);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  ::glDisable(::gl::BLEND);

  grid_.render(ctx, wake * quiet);

  // lone awakening scanner (0:00-0:20): one slow rotating ring, no cluster
  if (lone > 0.001f) {
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::ONE, ::gl::ONE);
    scanner_.render(ctx, lone, 1.0f, 0.0f);
    ::glDisable(::gl::BLEND);
  }

  // communication cluster: rings / fft / hex / graph fade in together
  if (diag > 0.001f || scannerEnv > 0.001f) {
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::ONE, ::gl::ONE);
    if (scannerEnv > 0.001f) scanner_.render(ctx, scannerEnv * quiet, 0.0f, build);
    if (diag > 0.001f) {
      fft_.render(ctx, diag * quiet);
      hex_.render(ctx, diag * quiet);
      graph_.render(ctx, diag * quiet);
    }
    ::glDisable(::gl::BLEND);
  }

  // boot messages: non-additive so the dark halo rim occludes the bright
  // diagnostics behind the glyphs (readable text instead of white glow)
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
  drawBoot(ctx, t);
  ::glDisable(::gl::BLEND);

  // particles drift over everything (additive, hold back during the log)
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::ONE, ::gl::ONE);
  particles_.render(ctx, pAlpha * (1.0f - 0.4f * bootEnv), flow, burst, build);
  ::glDisable(::gl::BLEND);

  // --- pass 2: ghost / logo compositing ----------------------------------------
  FrameTarget* cur = &sceneA_;
  if (ghost > 0.004f) {
    sceneB_.bind();
    ghost_.render(ctx, cur->colorTex(), ghost);
    cur = &sceneB_;
  }

  ctx.hdr->bind();
  if (asmb > 0.004f || scan > 0.004f) {
    logo_.render(ctx, cur->colorTex(), asmb, scan, dark, zoom);
  } else {
    // passthrough copy of the composed scene into the HDR target
    passthrough_->use();
    passthrough_->set1i("uTex", 0);
    ::glActiveTexture(::gl::TEXTURE0);
    ::glBindTexture(::gl::TEXTURE_2D, cur->colorTex());
    r.fsTriangle.draw(3);
  }

  // --- music-driven post params --------------------------------------------------
  if (ctx.post) {
    // small glitches during communication, heavier during the build-up ghost
    ctx.post->fx.glitch = diag * 0.05f + ghost * 0.5f + burst * 0.15f;
    // bloom dips only while the log is on screen; logo reveal still swells
    ctx.post->fx.bloom = 0.8f - 0.35f * bootEnv + dark * 0.25f + build * 0.1f;
    ctx.post->fx.exposure = 1.0f - dark * 0.15f + zoom * 0.3f;
    ctx.post->fx.heat = ghost * 0.2f;
    ctx.post->fx.kick = ctx.timeline->s.beatPulse * 0.1f;
  }
}

}  // namespace ns
