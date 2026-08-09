// ---------------------------------------------------------------------------
// SharedBlock: the std140 uniform buffer carrying every per-frame shared
// value the shaders read. Mirrors the NullBlock declaration in
// shaders/common.glsl exactly (port of src/engine/ubo.ts).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include "engine/camera.hpp"
#include "engine/timeline.hpp"
#include "effects/base_fwd.hpp"
#include <array>

namespace ns {

// float offsets into the 64-float block (256 bytes). Ordering + padding match
// the std140 layout of NullBlock in common.glsl exactly.
enum {
  OFF_URES = 0,     // vec2
  OFF_UTIME = 2,
  OFF_UFOVTAN = 3,
  OFF_UCAMPOS = 4,  // vec3 (+1 pad)
  OFF_UCAMROT = 8,  // mat3 -> 3 vec4 columns at 8, 12, 16
  OFF_UVIEW = 20,   // mat4
  OFF_UPROJ = 36,   // mat4
  OFF_UBEAT = 52,
  OFF_UPULSE = 53,
  OFF_UINTENSITY = 54,
  OFF_USECTIONLOCAL = 55,
  OFF_UBASS = 56,
  OFF_UONSET = 57,
  OFF_UANTICIPATION = 58,
  OFF_UEXITRAMP = 59,
  OFF_UMUSICHUE = 60,   // chord hue of the current bar
  OFF_UMUSICHUE2 = 61,  // chord hue of the next bar
  OFF_UBARPHASE = 62,   // 0..1 progress within the bar
  OFF_UBAR = 63,        // absolute bar index
  OFF_UASSEMBLY = 64,   // 0..1 per-kick assembly ratchet (cathedral / machine)
  OFF_USECTIONDUR = 65, // current section duration (seconds)
  OFF_USECBAR = 66,     // seconds per bar (BAR)
};

// block size rounded to next vec4 boundary (68 floats = 272 bytes std140)
constexpr int UBO_FLOATS = 68;

class SharedBlock {
public:
  unsigned buf = 0;
  std::array<float, 68> data{};
  bool dirty = true;

  SharedBlock() { ::glGenBuffers(1, &buf); }
  ~SharedBlock() { if (buf) ::glDeleteBuffers(1, &buf); }
  SharedBlock(const SharedBlock&) = delete;
  SharedBlock& operator=(const SharedBlock&) = delete;

  void write(const EffectContext* ctx);
  void commit() {
    if (!dirty) return;
    dirty = false;
    ::glBindBuffer(::gl::UNIFORM_BUFFER, buf);
    ::glBufferData(::gl::UNIFORM_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), ::gl::DYNAMIC_DRAW);
    ::glBindBufferBase(::gl::UNIFORM_BUFFER, 0, buf);
  }
};

}  // namespace ns
