// ---------------------------------------------------------------------------
// shadertoycheck - see shadertoycheck.hpp. Renders every shipped
// data/shadertoy/*.glsl into an offscreen target and verifies the output:
//
//   1. TOUCHED   - the frame actually reached the scene target. The image
//                  pass is what writes the final composite, and it must draw
//                  into ctx.hdr (the target the caller bound). If it draws
//                  into the last buffer instead (a regression in the pass
//                  pipeline), the scene target stays at the clear colour and
//                  the show goes black - the readback catches that.
//   2. VARIED    - the frame has spatial variance (max-min across sampled
//                  pixels). A feedback loop (image pass sampling the very
//                  buffer it writes) collapses to a solid colour on most
//                  drivers - the readback catches that too.
// ---------------------------------------------------------------------------
#include "app/shadertoycheck.hpp"
#include "app/appassets.hpp"
#include "app/shadertoy.hpp"
#include "app/shadertoyparse.hpp"
#include "effects/base_fwd.hpp"
#include "engine/framebuffer.hpp"
#include "engine/renderer.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ns {

namespace {

constexpr int kCheckSize = 256;
constexpr int kSampleGrid = 7;   // 7x7 sample grid (49 pixels)
// Perf is measured into a LARGER offscreen target: at the 256px correctness
// size the passes take well under 0.1ms and the comparison drowns in timer
// noise (first run measured "faster" at full res). 1280x720 gives the fbm
// passes real work so the renderScale win is actually measurable.
constexpr int kProfileW = 1280, kProfileH = 720;
constexpr int kProfileWarmup = 10;  // unmeasured frames first (targets' first touch, clock ramp)
constexpr int kProfileFrames = 40;  // frames per measured GPU-time median

std::string readTextFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/** render one shadertoy file into an offscreen target; returns true when the
 *  frame both reaches the target and has spatial variance. */
bool renderAndInspect(ShadertoyFX& fx, EffectContext& ctx, FrameTarget& out) {
  out.bind();
  // distinctive clear colour: (1,0,1) magenta. If the image pass draws into
  // the wrong target the whole frame stays magenta -> untouched -> fail.
  ::glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);

  // render a few frames with advancing time: shaders that settle over the
  // first frames (plasma's iFrame smoothstep) would otherwise false-fail on
  // frame 1; the LAST frame is the one inspected
  for (int f = 0; f < 3; f++) {
    ctx.time = 1.0f + (float)f * 0.5f;
    ctx.dt = 1.0f / 60.0f;
    fx.render(ctx);
  }

  // the effect may have rebound internally (buffer passes bind their own
  // targets), so rebind the scene target BEFORE the readback - otherwise
  // glReadPixels reads the last buffer the effect bound, not what reached
  // the scene, and a wrong-target regression would pass silently
  out.bind();

  // sample a grid; collect min/max per channel + count pixels off the clear
  int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
  int touched = 0;
  for (int gy = 0; gy < kSampleGrid; gy++) {
    for (int gx = 0; gx < kSampleGrid; gx++) {
      const int px = kCheckSize * (2 * gx + 1) / (2 * kSampleGrid);
      const int py = kCheckSize * (2 * gy + 1) / (2 * kSampleGrid);
      unsigned char p[4] = {0, 0, 0, 0};
      ::glReadPixels(px, py, 1, 1, ::gl::RGBA, ::gl::UNSIGNED_BYTE, p);
      if (!(p[0] == 255 && p[1] == 0 && p[2] == 255)) touched++;
      for (int c = 0; c < 3; c++) {
        mn[c] = std::min(mn[c], (int)p[c]);
        mx[c] = std::max(mx[c], (int)p[c]);
      }
    }
  }
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);

  if (touched == 0) {
    Log::error("SHADERTOY-CHECK", "frame never reached the scene target (stuck at the clear colour)");
    return false;
  }
  const int spread = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
  if (spread < 8) {
    Log::error("SHADERTOY-CHECK", "output is a solid colour (max channel spread " +
                                   std::to_string(spread) + " across " +
                                   std::to_string(kSampleGrid * kSampleGrid) + " samples)");
    return false;
  }
  return true;
}

/** median GPU ms/frame of one effect: kProfileWarmup unmeasured frames to
 *  settle the GPU (fresh target first-touch, clock ramp), then kProfileFrames
 *  measured ones. The MEDIAN is reported, not the mean - a single outlier
 *  frame (driver hiccup, allocator stall) can inflate a 30-frame mean by
 *  2-3x on sub-ms work, and the median ignores it. Returns -1 when the
 *  driver produced no samples (timer queries unsupported). */
double measureGpuMs(ShadertoyFX& fx, EffectContext& ctx, FrameTarget& target) {
  ctx.hdr = &target;
  target.bind();
  ::glClearColor(0, 0, 0, 1);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  for (int f = 0; f < kProfileWarmup; f++) {
    ctx.time = 1.0f + (float)f * 0.5f;
    ctx.dt = 1.0f / 60.0f;
    fx.render(ctx);
  }
  fx.drainPending();  // keep the warmup frames OUT of the measured window
  fx.beginProfile();
  for (int f = kProfileWarmup; f < kProfileWarmup + kProfileFrames; f++) {
    ctx.time = 1.0f + (float)f * 0.5f;
    ctx.dt = 1.0f / 60.0f;
    fx.render(ctx);
  }
  fx.endProfileMs();  // blocking-flushes the in-flight pair, stops the window
  return fx.gpuMedianMs();
}

/** print the measured GPU time at the file's declared scale - and, for files
 *  that scale their buffers, the forced-full-res comparison, so authors can
 *  see what the renderScale option is worth on this machine. Informational
 *  only: perf is not a pass/fail criterion (CI boxes differ too much). */
void profileAndReport(ShadertoyFX& fx, EffectContext& ctx, FrameTarget& prof,
                      const std::string& file, float declared) {
  // Work on a LOCAL copy of ctx: the caller's ctx.hdr must stay on the 256px
  // correctness target for the NEXT file's init(). Mutating the shared ctx
  // here made the next file allocate its buffers for the 1280x720 profile
  // resolution instead, silently failing the buffer-size check (7/7 -> 5/7).
  EffectContext pctx = ctx;
  pctx.hdr = &prof;
  // the effect's buffers were allocated for the 256px correctness target -
  // resize them to the profile resolution so the measured work matches the
  // reported buffer size (and iResolution matches the target it draws into)
  fx.resize(pctx);
  const double scaledMs = measureGpuMs(fx, pctx, prof);
  if (scaledMs < 0.0) {
    Log::warn("SHADERTOY-PERF", file + ": timer queries unsupported on this driver - skipping perf");
    return;
  }
  Log::info("SHADERTOY-PERF", file + " @ renderScale " + fmtMs(declared) + ": " + fmtMs(scaledMs) +
             " ms/frame GPU (buffers " + std::to_string(fx.bufferWidth()) + "x" +
             std::to_string(fx.bufferHeight()) + ")");
  if (declared < 1.0f) {
    ShadertoyFX fxFull(file, "", 0, 0, 1.0f);
    EffectContext fctx = ctx;
    fctx.hdr = &prof;
    fxFull.init(fctx);  // sizes its buffers at full res x the profile target
    const double fullMs = measureGpuMs(fxFull, fctx, prof);
    if (fullMs > 0.0) {
      Log::info("SHADERTOY-PERF", file + ": full-res " + fmtMs(fullMs) + " ms -> scale " +
                 fmtMs(declared) + " " + fmtMs(scaledMs) + " ms (" + fmtMs(fullMs / scaledMs) +
                 "x faster)");
    }
  }
}

}  // namespace

ShadertoyCheckResult checkShadertoyPipeline() {
  ShadertoyCheckResult r;
  const auto check = [&](bool ok, const std::string& label) {
    r.total++;
    if (ok) {
      r.ok++;
      Log::info("SHADERTOY-CHECK", "ok: " + label);
    } else {
      r.failed++;
      r.failedItems.push_back(label);
      Log::error("SHADERTOY-CHECK", "FAIL: " + label);
    }
  };

  try {
    const std::string dir = AppAssets::dataDir() + "/shadertoy";
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (e.path().extension() == ".glsl") files.push_back(e.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    if (ec) {
      check(false, "cannot list shadertoy dir: " + dir);
      return r;
    }
    if (files.empty()) {
      check(false, "no .glsl files in " + dir);
      return r;
    }

    Renderer renderer;
    renderer.resize(kCheckSize, kCheckSize);
    FrameTarget out = FrameTarget::color(kCheckSize, kCheckSize, ::gl::RGBA8, ::gl::RGBA,
                                         ::gl::UNSIGNED_BYTE);
    // perf phase target: larger so the measured times are meaningful
    FrameTarget prof = FrameTarget::color(kProfileW, kProfileH, ::gl::RGBA8, ::gl::RGBA,
                                          ::gl::UNSIGNED_BYTE);

    EffectContext ctx;
    ctx.r = &renderer;
    ctx.hdr = &out;
    ctx.time = 1.0f;
    ctx.dt = 1.0f / 60.0f;

    for (const std::string& file : files) {
      try {
        ShadertoyFX fx(file);
        fx.init(ctx);
        check(true, file + " compiles + links");
        check(renderAndInspect(fx, ctx, out), file + " renders into the scene target with variance");

        // the per-file `// option: renderScale` must be honored: the effect
        // reports the declared scale, and its buffer targets must actually
        // be sized at floor(res * scale)
        const float declared =
            extractShadertoyRenderScale(readTextFile(dir + "/" + file));
        const float applied = fx.renderScale();
        const int expectedBuf = std::max(2, (int)(kCheckSize * declared));
        if (declared < 1.0f) {
          check(std::fabs(applied - declared) < 0.001f,
                file + " applies declared renderScale " + std::to_string(declared));
          check(fx.bufferWidth() == expectedBuf && fx.bufferHeight() == expectedBuf,
                file + " buffer targets sized at " + std::to_string(expectedBuf) + "px (scale " +
                    std::to_string(declared) + ")");
        } else {
          check(std::fabs(applied - 1.0f) < 0.001f, file + " defaults to full-res renderScale");
        }

        // measured GPU time at the declared scale (+ the full-res comparison
        // for scaled files) - informational, printed to the log
        profileAndReport(fx, ctx, prof, file, declared);
      } catch (const std::exception& e) {
        check(false, file + ": " + e.what());
      }
    }
  } catch (const std::exception& e) {
    check(false, std::string("pipeline exception: ") + e.what());
  }
  return r;
}

}  // namespace ns
