// ---------------------------------------------------------------------------
// RenderProbe - render a shader program into an offscreen target at several
// instants and classify the output. Single home for the "solid color /
// flashing solids / never drew" readback that the AI Shader Generator, the
// editor Shader Lab and the shader preflight all need; previously each had
// its own near-identical copy (64x64 FBO + two sample times + a spread
// threshold).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include <functional>
#include <string>
#include <vector>

namespace ns {

/** one sampled instant of a probed render */
struct RenderProbeSample {
  float time = 0.0f;
  int lo[3] = {255, 255, 255};  // per-channel min over the readback
  int hi[3] = {0, 0, 0};        // per-channel max over the readback
  int spread = 0;               // max per-channel (hi-lo)
  bool uniform = true;          // spread <= flatDelta
};

/** structured verdict from rendering into an offscreen target */
struct RenderProbeResult {
  bool fboOk = true;   // the offscreen target completed; false = GL problem,
                       // NOT a shader verdict (probe is inconclusive)
  bool touched = false;       // some pixel differs from the clear color
  bool uniform = true;        // every sampled instant was uniform
  bool timeVarying = false;   // output changed between sampled instants
  bool nearBlack = false;     // brightest channel over all samples < nearBlackMax
  int maxSpread = 0;          // largest per-channel spread over all samples
  std::vector<RenderProbeSample> samples;
  std::vector<unsigned char> pixels;  // last sample's full RGBA readback

  /** any degenerate class - the frame never reached the target, is one solid
   *  color (with or without time), or collapsed to near-black. Empty diagnosis
   *  below means healthy, spatial output. */
  bool degenerate() const { return fboOk && (!touched || uniform || nearBlack); }
  /** human-readable "why"; empty when the output is healthy */
  std::string diagnosis() const;
};

/** Render `draw` into a W x H offscreen target at each `times` instant and
 *  read the pixels back. `draw(t)` is invoked once per sample with the
 *  probe's FBO bound and its viewport set; it must bind and draw the program
 *  (fullscreen triangle etc.). The caller's FBO binding, viewport and
 *  PACK_ALIGNMENT are saved and restored.
 *
 *  clearColor (default magenta (1,0,1) - the shadertoy-check convention) is
 *  what `touched` is measured against: a frame stuck at the clear color never
 *  wrote the target. flatDelta is the max per-channel spread (of 255) that
 *  still counts as uniform; nearBlackMax is the max channel value below which
 *  the whole frame counts as near-black. */
RenderProbeResult probeRender(int w, int h, const std::vector<float>& times,
                              const std::function<void(float)>& draw,
                              const float* clearColor = nullptr,
                              int flatDelta = 8, int nearBlackMax = 20);

}  // namespace ns
