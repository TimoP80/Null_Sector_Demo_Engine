// ---------------------------------------------------------------------------
// RenderProbe implementation - see renderprobe.hpp.
// ---------------------------------------------------------------------------
#include "engine/renderprobe.hpp"

#include <algorithm>
#include <cstdlib>

namespace ns {

// GL_VIEWPORT (core 3.3) - the minimal loader in gl.hpp has no constant for
// it, so the raw token is used to save/restore the caller's viewport.
static constexpr GLenum kViewportEnum = 0x0BA2;

std::string RenderProbeResult::diagnosis() const {
  if (!fboOk) return "";
  if (!touched)
    return "the frame never reached the output target (stuck at the clear color - main() "
           "may never write the output, or the draw lands in a different target)";
  if (uniform && timeVarying)
    return "one uniform color that changes with time (time-only output - the color depends "
           "only on uniforms like uTime, never on the pixel position)";
  if (uniform)
    return "one uniform solid color (every pixel identical - the output does not depend on "
           "the pixel position)";
  if (nearBlack)
    return "renders near-black (the brightest pixel is below the near-black threshold - "
           "the output intensity collapses to ~0)";
  return "";
}

RenderProbeResult probeRender(int w, int h, const std::vector<float>& times,
                              const std::function<void(float)>& draw,
                              const float* clearColor, int flatDelta, int nearBlackMax) {
  RenderProbeResult r;
  if (w <= 0 || h <= 0 || times.empty() || !draw) { r.fboOk = false; return r; }

  const float clearR = clearColor ? clearColor[0] : 1.0f;
  const float clearG = clearColor ? clearColor[1] : 0.0f;
  const float clearB = clearColor ? clearColor[2] : 1.0f;
  const int clearPix[3] = {(int)(clearR * 255.0f), (int)(clearG * 255.0f), (int)(clearB * 255.0f)};

  GLint prevFbo = 0, prevPack = 4, prevViewport[4] = {0, 0, 0, 0};
  ::glGetIntegerv(::gl::FRAMEBUFFER_BINDING, &prevFbo);
  ::glGetIntegerv(::gl::PACK_ALIGNMENT, &prevPack);
  ::glGetIntegerv(kViewportEnum, prevViewport);

  unsigned tex = 0, fbo = 0;
  ::glGenTextures(1, &tex);
  ::glBindTexture(::gl::TEXTURE_2D, tex);
  ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MIN_FILTER, (int)::gl::LINEAR);
  ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MAG_FILTER, (int)::gl::LINEAR);
  ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_S, (int)::gl::CLAMP_TO_EDGE);
  ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_T, (int)::gl::CLAMP_TO_EDGE);
  ::glPixelStorei(::gl::UNPACK_ALIGNMENT, 1);
  ::glTexImage2D(::gl::TEXTURE_2D, 0, (int)::gl::RGBA8, w, h, 0, ::gl::RGBA,
                 ::gl::UNSIGNED_BYTE, nullptr);
  ::glGenFramebuffers(1, &fbo);
  ::glBindFramebuffer(::gl::FRAMEBUFFER, fbo);
  ::glFramebufferTexture2D(::gl::FRAMEBUFFER, ::gl::COLOR_ATTACHMENT0, ::gl::TEXTURE_2D, tex, 0);
  r.fboOk = ::glCheckFramebufferStatus(::gl::FRAMEBUFFER) == ::gl::FRAMEBUFFER_COMPLETE;

  if (r.fboOk) {
    ::glDisable(::gl::BLEND);
    ::glDisable(::gl::DEPTH_TEST);
    ::glDisable(::gl::CULL_FACE);
    std::vector<unsigned char> px((size_t)w * h * 4);
    int maxBright = 0;
    for (size_t si = 0; si < times.size(); ++si) {
      const float t = times[si];
      ::glViewport(0, 0, w, h);
      ::glClearColor(clearR, clearG, clearB, 1.0f);
      ::glClear(::gl::COLOR_BUFFER_BIT);
      draw(t);
      // read back while the offscreen target is still bound - reading after
      // unbinding would sample the window's back buffer instead
      ::glPixelStorei(::gl::PACK_ALIGNMENT, 1);
      ::glReadPixels(0, 0, w, h, ::gl::RGBA, ::gl::UNSIGNED_BYTE, px.data());
      ::glPixelStorei(::gl::PACK_ALIGNMENT, prevPack);

      RenderProbeSample s;
      s.time = t;
      int touchedPixels = 0;
      for (size_t i = 0; i < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          const unsigned char v = px[i + (size_t)c];
          if (v < s.lo[c]) s.lo[c] = v;
          if (v > s.hi[c]) s.hi[c] = v;
        }
        const int d = std::max({std::abs((int)px[i] - clearPix[0]),
                                std::abs((int)px[i + 1] - clearPix[1]),
                                std::abs((int)px[i + 2] - clearPix[2])});
        if (d > 8) touchedPixels++;
      }
      if (touchedPixels > 0) r.touched = true;
      s.spread = std::max({s.hi[0] - s.lo[0], s.hi[1] - s.lo[1], s.hi[2] - s.lo[2]});
      s.uniform = s.spread <= flatDelta;
      if (!s.uniform) r.uniform = false;
      r.maxSpread = std::max(r.maxSpread, s.spread);
      for (int c = 0; c < 3; ++c) maxBright = std::max(maxBright, (int)s.hi[c]);

      if (si > 0) {
        const RenderProbeSample& prev = r.samples.back();
        bool changed = false;
        for (int c = 0; c < 3; ++c) {
          if (std::abs((int)s.hi[c] - (int)prev.hi[c]) > flatDelta ||
              std::abs((int)s.lo[c] - (int)prev.lo[c]) > flatDelta)
            changed = true;
        }
        if (changed) r.timeVarying = true;
      }
      r.samples.push_back(std::move(s));
      if (si + 1 == times.size()) r.pixels = std::move(px);  // keep the last frame
    }
    r.nearBlack = maxBright < nearBlackMax;
  }

  ::glBindFramebuffer(::gl::FRAMEBUFFER, (GLuint)prevFbo);
  ::glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
  ::glDeleteFramebuffers(1, &fbo);
  ::glDeleteTextures(1, &tex);
  return r;
}

}  // namespace ns
