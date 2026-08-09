// ---------------------------------------------------------------------------
// shadertoycheck - dev preflight for the Shadertoy importer (the
// --check-shaders / --check-models idea applied to data/shadertoy/*.glsl).
//
// The imported shaders only ever run at demo time, so a regression in the
// pass pipeline (buffer ordering, the image pass's render target, channel
// binding, feedback loops) surfaces mid-show as a black or solid-colour
// screen. checkShadertoyPipeline() compiles and renders every shipped
// shadertoy file into an offscreen target with a pixel readback: the frame
// must actually reach the scene target (not stay at the clear colour) and
// must have spatial variance (not be a feedback-loop solid). Needs a GL
// context; never opens the show.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace ns {

struct ShadertoyCheckResult {
  int total = 0;
  int ok = 0;
  int failed = 0;
  std::vector<std::string> failedItems;
};

/** run the shadertoy pipeline check; returns the aggregate (never throws) */
ShadertoyCheckResult checkShadertoyPipeline();

}  // namespace ns
