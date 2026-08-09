// ---------------------------------------------------------------------------
// NetworkFX - the NEURAL DUST ocean reveal (SCENE 7 + the SYSTEM FAILURE
// revisit): the network beneath the particle ocean, drawn as additive point
// sprites (nodes) + camera-facing quads (synapses) instead of the old
// per-pixel SDF raymarch (nd_net.frag v1 was ~200 ms/frame at 1600x900 - the
// geometry version is ~1-2 ms). Node positions are computed per-vertex from
// the same wave field the raymarch used (see nd_net.vert nodePos), so the
// reveal looks identical. The ocean particles stay live behind it (both are
// additive), reading as the sea resolving into structure.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "effects/kickflash.hpp"
#include "engine/mesh.hpp"
#include "engine/perftimer.hpp"
#include "engine/shader.hpp"
#include <memory>

namespace ns {

class NetworkFX : public Effect {
public:
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  float mode = 0.0f;   // 1 = SYSTEM FAILURE (network already torn)

  /** stable run snapshot (--perf-json exit dump). */
  PerfSample perfSample() const override { return perf_.sample(); }

  /** current EMA ms/frame (0 until the first sample - the per-second
   *  --perf-csv rows use this). */
  double emaMs() const { return perf_.emaMs(); }

  /** most recent raw sample (unsmoothed - the --perf-raw rows). */
  double lastRawMs() const { return perf_.lastRawMs(); }

private:
  std::unique_ptr<Shader> prog_;
  std::unique_ptr<Shader> voidProg_;   // screen-space void/glitch haze pass
  Mesh nodes_{::gl::POINTS};
  Mesh links_{::gl::TRIANGLES};
  int linkVerts_ = 0;
  KickFlash kick_;
  PerfTimer perf_;  // GL_TIMESTAMP ring: periodic "X ms/frame GPU" log line
};

}  // namespace ns
