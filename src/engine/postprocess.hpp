// ---------------------------------------------------------------------------
// Post processing: DOF, bloom (ping-pong), HDR composite (tonemap, CA, grain,
// scanlines, vignette, grading, temporal motion blur), FXAA.
// Port of src/engine/postprocess.ts.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/camera.hpp"
#include "engine/framebuffer.hpp"
#include "engine/perftimer.hpp"
#include "engine/renderer.hpp"
#include "engine/shader.hpp"
#include "engine/texture.hpp"
#include <memory>
#include <vector>

namespace ns {

class PostFX {
public:
  explicit PostFX(Renderer& r);
  ~PostFX() = default;
  PostFX(const PostFX&) = delete;
  PostFX& operator=(const PostFX&) = delete;

  void resize();
  void process(unsigned hdrTex, Camera& camera, float motion, float time);

  /** the previous frame's final LDR output (the last thing the viewer saw).
   *  Scenes use it for in-scene handoffs - new particles ignite from the
   *  outgoing frame's bright pixels (unit 9) so the show dissolves scene to
   *  scene. Always returns a valid texture (the targets exist from the
   *  constructor); its content is undefined only before the first post.
   *  Handoffs run late in the show, so this is never observed. */
  unsigned prevFrameTex() const { return (prevLdr_ ? ldrB_ : ldrA_).colorTex(); }

  /** number of bloom levels in the current stack (0 when none). */
  int bloomLevels() const { return (int)bloomLevels_.size(); }

  /** stable run snapshot (--perf-json exit dump). */
  PerfSample perfSample() const { return perf_.sample(); }

  /** current EMA ms/frame for the whole stack (0 until the first sample -
   *  the per-second --perf-csv rows use this). */
  double emaMs() const { return perf_.emaMs(); }

  /** most recent raw sample (unsmoothed - the --perf-raw rows). */
  double lastRawMs() const { return perf_.lastRawMs(); }

  // music-driven post params (set per frame by the director)
  struct Fx {
    float bloom = 1.0f;
    float glitch = 0.0f;
    float exposure = 1.0f;
    float heat = 0.0f;
    float dirt = 0.3f;
    float kick = 0.0f;
    int landing = 0;
    float landingT = 0;
  } fx;

private:
  Renderer& r_;
  FrameTarget dofTarget_;
  FrameTarget flashTarget_;
  std::vector<FrameTarget> bloomLevels_;
  std::vector<FrameTarget> scratchLevels_;
  FrameTarget ldrA_;
  FrameTarget ldrB_;
  bool prevLdr_ = false;
  Texture blackTex_;
  std::unique_ptr<Shader> dofProg_, extractProg_, blurProg_, composeProg_, fxaaProg_, flashProg_;

  int fmtInternal() const;
  int fmtFormat() const;
  int fmtType() const;
  void rebuildBloom();

  PerfTimer perf_;  // GL_TIMESTAMP ring: periodic "X ms/frame GPU" log line
};

}  // namespace ns
