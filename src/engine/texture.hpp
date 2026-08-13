// ---------------------------------------------------------------------------
// Texture wrapper (port of src/engine/texture.ts).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include <cstdint>
#include <vector>

namespace ns {

struct TextureOpts {
  unsigned min = 0;  // 0 = use default below
  unsigned mag = 0;
  unsigned wrap = 0;
  bool mips = false;
};

class Texture {
public:
  unsigned tex = 0;
  int w = 0;
  int h = 0;

  Texture() = default;
  ~Texture() { destroy(); }
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;
  Texture(Texture&& o) noexcept { *this = std::move(o); }
  Texture& operator=(Texture&& o) noexcept {
    if (this != &o) { destroy(); tex = o.tex; w = o.w; h = o.h; o.tex = 0; o.w = o.h = 0; }
    return *this;
  }

  void destroy() {
    if (tex) { ::glDeleteTextures(1, &tex); tex = 0; }
  }

  void bind(int unit = 0) const {
    ::glActiveTexture(::gl::TEXTURE0 + unit);
    ::glBindTexture(::gl::TEXTURE_2D, tex);
  }

  /** RGBA8 from CPU pixels */
  static Texture fromRGBA(int w, int h, const void* data, const TextureOpts& o = {}) {
    Texture t;
    t.w = w; t.h = h;
    ::glGenTextures(1, &t.tex);
    ::glBindTexture(::gl::TEXTURE_2D, t.tex);
    const unsigned minF = o.min ? o.min : (o.mips ? ::gl::LINEAR_MIPMAP_LINEAR : ::gl::NEAREST);
    const unsigned magF = o.mag ? o.mag : ::gl::NEAREST;
    const unsigned wrap = o.wrap ? o.wrap : ::gl::CLAMP_TO_EDGE;
    ::glPixelStorei(::gl::UNPACK_ALIGNMENT, 1);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MIN_FILTER, (int)minF);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MAG_FILTER, (int)magF);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_S, (int)wrap);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_T, (int)wrap);
    ::glTexImage2D(::gl::TEXTURE_2D, 0, (int)::gl::RGBA8, w, h, 0, ::gl::RGBA, ::gl::UNSIGNED_BYTE, data);
    if (o.mips) ::glGenerateMipmap(::gl::TEXTURE_2D);
    return t;
  }

  /** Update an existing RGBA8 texture without reallocating its storage. */
  void updateRGBA(int width, int height, const void* data) {
    if (!tex || width != w || height != h) return;
    ::glBindTexture(::gl::TEXTURE_2D, tex);
    ::glPixelStorei(::gl::UNPACK_ALIGNMENT, 1);
    ::glTexSubImage2D(::gl::TEXTURE_2D, 0, 0, 0, width, height,
                      ::gl::RGBA, ::gl::UNSIGNED_BYTE, data);
  }

  /** 24-bit depth texture attachment (for the 3D model pass) */
  static Texture blankDepth(int w, int h) {
    Texture t;
    t.w = w; t.h = h;
    ::glGenTextures(1, &t.tex);
    ::glBindTexture(::gl::TEXTURE_2D, t.tex);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MIN_FILTER, (int)::gl::NEAREST);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MAG_FILTER, (int)::gl::NEAREST);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_S, (int)::gl::CLAMP_TO_EDGE);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_T, (int)::gl::CLAMP_TO_EDGE);
    ::glTexImage2D(::gl::TEXTURE_2D, 0, (int)::gl::DEPTH_COMPONENT24, w, h, 0,
                   ::gl::DEPTH_COMPONENT, ::gl::UNSIGNED_INT, nullptr);
    return t;
  }

  /** float/HDR format attachment */
  static Texture blank(int w, int h, int internal, int format, int type, const TextureOpts& o = {}) {
    Texture t;
    t.w = w; t.h = h;
    ::glGenTextures(1, &t.tex);
    ::glBindTexture(::gl::TEXTURE_2D, t.tex);
    const unsigned minF = o.min ? o.min : ::gl::LINEAR;
    const unsigned magF = o.mag ? o.mag : ::gl::LINEAR;
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MIN_FILTER, (int)minF);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_MAG_FILTER, (int)magF);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_S, (int)::gl::CLAMP_TO_EDGE);
    ::glTexParameteri(::gl::TEXTURE_2D, ::gl::TEXTURE_WRAP_T, (int)::gl::CLAMP_TO_EDGE);
    ::glTexImage2D(::gl::TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
    return t;
  }
};

}  // namespace ns
