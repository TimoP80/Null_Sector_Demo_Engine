// ---------------------------------------------------------------------------
// NULL SECTOR // DEMO ENGINE - intro prototype effect layer.
//
// The opening 20-30s as a set of independent, reusable renderers composed by
// IntroFX. Each system owns its shader program + state and can be dropped
// into any later scene (the whole point of the prototype):
//
//   GridRenderer     intro_grid.frag     - wakeup grid / circles / hairlines
//   CircularScanner  intro_rings.frag    - rotating arcs, radial scanner, scope
//   FFTBars          intro_fft.frag      - spectrum bars along the bottom
//   HexBackground    intro_hex.frag      - scrolling hex value columns
//   NodeGraph        intro_graph.frag    - wandering node network
//   ParticleOverlay  intro_particles.frag- drifting depth particles
//   GhostPass        intro_ghost.frag    - distortion ripples + silhouette
//   LogoAssembler    intro_logo.frag     - circular scanner + NULL SECTOR logo
//   DiagnosticText   intro_boot.frag     - glitch-reconstructed boot messages
//
// Timeline (intro section, 0..~66.4s - three music-timed phases):
//   0:00-0:20  Awakening    black, particles, lone slow scanner, thin grid,
//                           LEDs, data pulses. No text.
//   0:21-0:48  Communication boot log reconstructs, diagnostics cluster in
//                           (rings / fft / hex / node graph), small glitches
//   0:49-1:06  Build-up     scanner accelerates, particles stream, rings
//                           expand, distortion ripples, ghost silhouette,
//                           camera grows aggressive
//   1:03+      Climax       circular scanner expands, wordmark assembles,
//                           camera accelerates through -> tunnel (TunnelFX)
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/framebuffer.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include <memory>
#include <string>

namespace ns {

// --- reusable intro renderers -------------------------------------------------

class GridRenderer {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float wake);
private:
  std::unique_ptr<Shader> prog_;
};

/** diagnostics rings. quiet=1 draws the lone awakening scanner, quiet=0 the
 *  full cluster; build (0..1) accelerates rotation + expands the rings. */
class CircularScanner {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float diag, float quiet, float build);
private:
  std::unique_ptr<Shader> prog_;
};

class FFTBars {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float diag);
private:
  std::unique_ptr<Shader> prog_;
};

class HexBackground {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float diag);
private:
  std::unique_ptr<Shader> prog_;
};

class NodeGraph {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float diag);
private:
  std::unique_ptr<Shader> prog_;
};

class ParticleOverlay {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, float alpha, float flow, float burst, float stream);
private:
  std::unique_ptr<Shader> prog_;
};

/** samples the composed scene (unit 0) and adds distortion + the silhouette */
class GhostPass {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, unsigned sceneTex, float ghost);
private:
  std::unique_ptr<Shader> prog_;
};

/** samples the composed scene (unit 0) + font atlas (unit 1); final pass */
class LogoAssembler {
public:
  void init(EffectContext& ctx);
  void render(EffectContext& ctx, unsigned sceneTex, float asmb, float scan,
              float dark, float zoom);
private:
  std::unique_ptr<Shader> prog_;
};

/** boot messages via text.vert + intro_boot.frag (glitch reconstruction) */
class DiagnosticText {
public:
  void init(EffectContext& ctx);
  void line(EffectContext& ctx, const std::string& text, float centerY, int sizePx,
            float progress, float alpha, float seed, int style, float centerX = 0);
private:
  std::unique_ptr<Shader> prog_;
  TextMesh mesh_;
  FontMetrics font_;
};

// --- the intro scene ----------------------------------------------------------

class IntroFX : public Effect {
public:
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  void resize(EffectContext& ctx) override;

private:
  void drawBoot(EffectContext& ctx, float t);

  GridRenderer grid_;
  CircularScanner scanner_;
  FFTBars fft_;
  HexBackground hex_;
  NodeGraph graph_;
  ParticleOverlay particles_;
  GhostPass ghost_;
  LogoAssembler logo_;
  DiagnosticText boot_;
  std::unique_ptr<Shader> passthrough_;
  FrameTarget sceneA_, sceneB_;   // ping-pong for ghost/logo compositing
};

}  // namespace ns
