// ---------------------------------------------------------------------------
// ShadertoyFX - a Shadertoy shader importer.
//
// Loads a Shadertoy-style fragment shader from data/shadertoy/<file>.glsl and
// turns it into a fullscreen engine effect. The importer:
//
//   - wraps the user's code with desktop GLSL 330 + the Shadertoy uniforms:
//       iTime, iTimeDelta, iFrame, iResolution, iMouse, iDate,
//       iChannelTime[4], iChannelResolution[4], iChannel0..3
//   - calls mainImage(fragColor, gl_FragCoord.xy) per pixel
//   - maps the ES texture helpers (#define texture2D texture) so older
//     Shadertoy code compiles unchanged
//   - renders multi-pass files: `// pass: common`, `// pass: buffer_a`,
//     `// pass: image` marker lines split the file; buffers A..D render in
//     order into RGBA16F targets, the image pass composes into the HDR scene.
//     A marker must be the FIRST comment on a whitespace-only line and its
//     name is a single token - prose that merely mentions "// pass:" (or
//     nested "//   // pass: x" descriptions) never splits the file. The
//     common pass is prepended to every other pass.
//     Channel defaults (documented, override per-pass if you need more):
//       buffer A   iChannel0 = external texture, else a snapshot of the
//                  live scene taken when this effect starts rendering
//       buffer N>0 iChannel0 = buffer N-1 output
//       image      iChannel0 = last buffer (or external texture), iChannel1 =
//                  the live scene snapshot. The image pass draws into the
//                  caller's scene target (ctx.hdr) - the snapshot breaks the
//                  feedback loop that would occur sampling the target it
//                  writes (solid colour on most drivers)
//   - per-file options: `// option: renderScale 0.5` renders the buffer
//     passes at half resolution (floor of res * scale; image pass + scene
//     snapshot stay full-res), for heavy multi-pass files. Clamped to (0,1].
//   - GPU timing: every render is bracketed by GL_TIMESTAMP queries, the
//     EMA is logged periodically as "[SHADERTOY] <file>: X.XX ms/frame GPU
//     (renderScale ...)" so authors can see what the option is worth, and
//     beginProfile()/endProfileMs() give tools (--check-shadertoy) a mean
//     over N frames. Timestamps (not TIME_ELAPSED) so several effects can
//     be timed in one frame - GL allows only one active TIME_ELAPSED query
//     per context. The measured time is only this effect's passes.
//   - iDate comes from the wall clock; iMouse is (0,0,0,0) unless the app
//     drives it (show mode has no cursor)
//
// Hot reload: the DemoApp re-inits the effect when the source file changes.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/framebuffer.hpp"
#include "engine/perftimer.hpp"
#include "engine/texture.hpp"
#include "app/shadermanager.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns {

class ShadertoyFX : public Effect {
public:
  /** file: path under the shadertoy data dir; texPath: optional external
   *  texture for iChannel0 of the first pass; fixedW/H force a render size.
   *  scaleOverride (>0) forces renderScale instead of the file's
   *  `// option: renderScale` - used by --check-shadertoy to A/B the same
   *  file at different scales. */
  ShadertoyFX(std::string file, std::string texPath = "", int fixedW = 0, int fixedH = 0,
              float scaleOverride = 0.0f)
      : file_(std::move(file)), texPath_(std::move(texPath)), fixedW_(fixedW), fixedH_(fixedH),
        scaleOverride_(scaleOverride) {}
  ~ShadertoyFX() override {
    destroyPrograms();
    if (blackTex_) { ::glDeleteTextures(1, &blackTex_); blackTex_ = 0; }
  }

  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  void resize(EffectContext& ctx) override;

  bool valid() const { return !passes_.empty() && passes_.back().prog != 0; }

  /** per-file renderScale option (1.0 = full res); buffer passes render at
   *  this fraction of the output resolution */
  float renderScale() const { return renderScale_; }

  /** actual buffer target size (after renderScale was applied) */
  int bufferWidth() const { return buffers_.empty() ? 0 : buffers_[0].w; }
  int bufferHeight() const { return buffers_.empty() ? 0 : buffers_[0].h; }

  /** median ms/frame over the last profiling window (see beginProfile). */
  double gpuMedianMs() const { return perf_.medianMs(); }

  /** source file (data/shadertoy/<file>) - the label used in perf dumps. */
  const std::string& file() const { return file_; }

  /** stable run snapshot (--perf-json exit dump). */
  PerfSample perfSample() const override { return perf_.sample(); }

  /** current EMA ms/frame (0 until the first sample - the per-second
   *  --perf-csv rows use this). */
  double emaMs() const { return perf_.emaMs(); }

  /** most recent raw sample (unsmoothed - the --perf-raw rows). */
  double lastRawMs() const { return perf_.lastRawMs(); }

  /** start a profiling window; the mean over the next rendered frames is
   *  reported by endProfileMs(). Used by --check-shadertoy. */
  void beginProfile() { perf_.beginProfile(); }

  /** blocking-drain every pending timestamp pair (oldest first). Tools path
   *  only: lets --check-shadertoy discard unmeasured warmup frames BEFORE
   *  beginProfile(), so the window starts empty. */
  void drainPending() { perf_.drain(); }

  /** stop the window and return the mean GPU ms/frame over it (-1 when no
   *  sample was collected, e.g. timer queries unsupported). Blocking-reads
   *  the in-flight timestamp pairs so the last frames are counted too. */
  double endProfileMs() {
    perf_.endProfile();
    return perf_.meanMs();
  }

  /** re-parse + recompile from disk (live reload) */
  bool reload(EffectContext& ctx);

  /** driver for iMouse (windowed debug); the show leaves it at zero */
  std::array<float, 4> mouse{0, 0, 0, 0};

  /** extra float uniforms set per frame on every pass (audio feeds uBass/
   *  uMid/..., script `anim ... uniform:uX` channels, etc.) */
  std::map<std::string, float> uniforms;

private:
  struct Pass {
    std::string name;               // common | buffer_a..d | image
    std::string src;
    unsigned prog = 0;
  };

  std::string file_;
  std::string texPath_;
  int fixedW_ = 0, fixedH_ = 0;
  float scaleOverride_ = 0.0f;  // >0 forces renderScale (A/B profiling)
  float renderScale_ = 1.0f;  // from `// option: renderScale` in the file

  PerfTimer perf_;  // GL_TIMESTAMP ring + EMA + profile window + log throttle

  std::vector<Pass> passes_;              // common (if any) first, then buffers, image last
  std::vector<FrameTarget> buffers_;      // buffer A..D targets
  Texture extTex_;                        // optional iChannel0 texture
  FrameTarget sceneSnap_;                 // copy of the live scene (iChannel1 source)
  unsigned blackTex_ = 0;                 // 1x1 black fallback for unbound channels
  unsigned vert_ = 0;                     // shared fullscreen.vert program
  unsigned copyProg_ = 0;                 // passthrough program for the scene snapshot
  int frame_ = 0;
  float lastTime_ = -1e9f;
  std::array<float, 4> passStart_{0, 0, 0, 0};  // per-pass iTime origins

  bool parseSource();
  bool compilePasses(EffectContext& ctx);
  void destroyPrograms();
  static unsigned compileStage(unsigned type, const std::string& src, const std::string& label);
  static unsigned linkProgram(const std::vector<unsigned>& stages, const std::string& label);
  static std::string wrap(const std::string& passName, const std::string& src);
  void bindUniforms(EffectContext& ctx, Pass& pass, float passTime,
                    const std::vector<unsigned>& channelTexs, int w, int h);
  unsigned copyScene(EffectContext& ctx);

  /** buffer target size: fixedW/H override, else output res * renderScale
   *  (min 2px). Single source of truth for resize() and render() so the
   *  allocated targets and the per-pass iResolution can never drift. */
  void bufferSize(const EffectContext& ctx, int& w, int& h) const;
};

}  // namespace ns
