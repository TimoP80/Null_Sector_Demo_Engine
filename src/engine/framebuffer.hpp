// ---------------------------------------------------------------------------
// Framebuffer render targets (port of src/engine/framebuffer.ts).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include "engine/texture.hpp"

namespace ns {

class FrameTarget {
public:
  unsigned fbo = 0;
  Texture col;
  Texture depth;   // optional depth attachment (3D model pass)
  int w = 0;
  int h = 0;

  FrameTarget() = default;
  ~FrameTarget() { destroy(); }
  FrameTarget(const FrameTarget&) = delete;
  FrameTarget& operator=(const FrameTarget&) = delete;
  FrameTarget(FrameTarget&& o) noexcept { *this = std::move(o); }
  FrameTarget& operator=(FrameTarget&& o) noexcept {
    if (this != &o) {
      destroy();
      fbo = o.fbo; col = std::move(o.col); depth = std::move(o.depth);
      w = o.w; h = o.h;
      o.fbo = 0; o.w = o.h = 0;
    }
    return *this;
  }

  void destroy() {
    if (fbo) { ::glDeleteFramebuffers(1, &fbo); fbo = 0; }
    col.destroy();
    depth.destroy();
  }

  void bind() const {
    ::glBindFramebuffer(::gl::FRAMEBUFFER, fbo);
    ::glViewport(0, 0, w, h);
  }

  unsigned colorTex() const { return col.tex; }

  /** single color target */
  static FrameTarget color(int w, int h, int internal, int format, int type, const TextureOpts& o = {}) {
    FrameTarget t;
    t.w = w; t.h = h;
    t.col = Texture::blank(w, h, internal, format, type, o);
    ::glGenFramebuffers(1, &t.fbo);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, t.fbo);
    ::glFramebufferTexture2D(::gl::FRAMEBUFFER, ::gl::COLOR_ATTACHMENT0, ::gl::TEXTURE_2D, t.col.tex, 0);
    // Texture::blank() allocates with glTexImage2D(..., nullptr), so the GPU
    // memory starts UNINITIALIZED. Mid-show rebuilds (adaptive quality steps,
    // window/fullscreen resizes) reallocate the deleted targets' memory, which
    // often still holds the previous frame - the transition crossfade reads
    // g_prevTex and compose reads uPrev, so that stale content would surface as
    // a ghost "copy of the scene with different brightness" rectangle during
    // the next blend. Clearing at allocation makes every target start black
    // (cheap - once, not per frame).
    ::glClearColor(0, 0, 0, 1);
    ::glClear(::gl::COLOR_BUFFER_BIT);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
    return t;
  }

  /** color + depth target (for rendering 3D models with the engine camera) */
  static FrameTarget colorDepth(int w, int h, int internal = ::gl::RGBA16F, int format = ::gl::RGBA,
                                int type = ::gl::HALF_FLOAT, const TextureOpts& o = {}) {
    FrameTarget t;
    t.w = w;
    t.h = h;
    t.col = Texture::blank(w, h, internal, format, type, o);
    t.depth = Texture::blankDepth(w, h);
    ::glGenFramebuffers(1, &t.fbo);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, t.fbo);
    ::glFramebufferTexture2D(::gl::FRAMEBUFFER, ::gl::COLOR_ATTACHMENT0, ::gl::TEXTURE_2D, t.col.tex, 0);
    ::glFramebufferTexture2D(::gl::FRAMEBUFFER, ::gl::DEPTH_ATTACHMENT, ::gl::TEXTURE_2D, t.depth.tex, 0);
    ::glClearColor(0, 0, 0, 0);
    ::glClear(::gl::COLOR_BUFFER_BIT | ::gl::DEPTH_BUFFER_BIT);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
    return t;
  }
};

}  // namespace ns
