// ---------------------------------------------------------------------------
// NetworkFX - see network.hpp. The driver mirrors ParticleStormFX: static
// seed VBOs, additive blending, shared KickFlash + PerfTimer, and audio via
// the NullBlock shared UBO (the vertex shader reads Null.uPulse/uBass/...).
// ---------------------------------------------------------------------------
#include "effects/network.hpp"
#include "engine/audio.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include "framework/core/log.hpp"

#include <vector>

namespace ns {

namespace {
constexpr int GX = 12;   // grid extent - MUST match the .vert shader
constexpr int GY = 12;
}  // namespace

void NetworkFX::init(EffectContext&) {
  prog_ = std::make_unique<Shader>("nd_net.vert", "nd_net.frag");
  voidProg_ = std::make_unique<Shader>("fullscreen.vert", "nd_net_void.frag");
  perf_.setLabel("nd_net");

  // --- node sprites: one vec4 per node (gx, gy, 0, 0) ------------------------
  std::vector<float> nodes;
  nodes.reserve((size_t)GX * GY * 4);
  for (int gy = 0; gy < GY; gy++) {
    for (int gx = 0; gx < GX; gx++) {
      nodes.push_back((float)gx);
      nodes.push_back((float)gy);
      nodes.push_back(0.0f);
      nodes.push_back(0.0f);
    }
  }
  nodes_.setBuffer(0, nodes.data(), (int)nodes.size(), 4, ::gl::STATIC_DRAW);

  // --- synapse quads: 6 verts per link (gxA, gyA, gxB, gyB) ------------------
  // adjacency: right + up + diagonal (the raymarch's links from the nearest
  // node) - deterministic mesh-ish grid connectivity
  std::vector<float> links;
  links.reserve((size_t)GX * GY * 3 * 6 * 4);
  for (int gy = 0; gy < GY; gy++) {
    for (int gx = 0; gx < GX; gx++) {
      const int dx[3] = {1, 0, 1};
      const int dy[3] = {0, 1, 1};
      for (int k = 0; k < 3; k++) {
        const int nx = gx + dx[k];
        const int ny = gy + dy[k];
        if (nx >= GX || ny >= GY) continue;
        for (int v = 0; v < 6; v++) {
          links.push_back((float)gx);
          links.push_back((float)gy);
          links.push_back((float)nx);
          links.push_back((float)ny);
        }
      }
    }
  }
  linkVerts_ = (int)links.size() / 4;
  links_.setBuffer(0, links.data(), (int)links.size(), 4, ::gl::STATIC_DRAW);
}

void NetworkFX::render(EffectContext& ctx) {
  if (!prog_) return;
  perf_.beginFrame();  // GPU timing: PerfTimer ring (see perftimer.hpp)
  const float flash = kick_.update(ctx);

  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::ONE, ::gl::ONE);   // additive over the particle ocean

  // void pass first: base tint + traveling pulse glow + destabilize glitch
  // (the raymarch's miss path, ported as a cheap fullscreen additive pass)
  if (voidProg_) {
    voidProg_->use();
    voidProg_->set1f("uFlash", flash);
    voidProg_->set1f("uMode", mode);
    ctx.r->fsTriangle.draw(3);
  }

  ::glEnable(::gl::PROGRAM_POINT_SIZE);
  prog_->use();
  prog_->set1f("uFlash", flash);
  prog_->set1f("uMode", mode);   // no-op semantics: uMode 1 = already torn
  prog_->set1f("uHigh", ctx.audio ? ctx.audio->react.treble.load() : 0.0f);

  prog_->set1f("uPrim", 0.0f);
  nodes_.draw(GX * GY);
  prog_->set1f("uPrim", 1.0f);
  links_.draw(linkVerts_);

  ::glDisable(::gl::PROGRAM_POINT_SIZE);
  ::glDisable(::gl::BLEND);

  if (ctx.post) kick_.applyPost(ctx, flash, ctx.timeline->s.beatPulse);

  perf_.endFrame();
  // periodic note so authors can see what the network costs
  if (perf_.logDue()) {
    Log::info("SCENE", perf_.logLine() + " (network " + std::to_string(GX * GY) + " nodes)");
  }
}

}  // namespace ns
