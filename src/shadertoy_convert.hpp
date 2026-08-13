// ---------------------------------------------------------------------------
// shadertoy_convert.hpp - Shadertoy -> single fragment shader converter.
//
// GL-free text transformer: takes Shadertoy GLSL (single-pass, or the engine's
// multi-pass `// pass:` marker format) or a Shadertoy JSON API export (the
// renderpass code blocks are extracted and mapped onto the same passes) and
// emits ONE Null Sector fragment shader with every channel folded into the
// same file:
//
//   * buffer passes   -> vec4 helper functions, texture(iChannelN, uv) calls
//                        become bufferFunction(uv * iResolution.xy)
//   * texture passes  -> uniform sampler2D uChannelN (bind at runtime)
//   * audio/keyboard  -> replaced with vec4(0.0) and noted
//
// Shadertoy uniforms are remapped to the Null Sector set (uResolution, uTime,
// uBass, ...) through a small #define shim block, so the converted shader
// compiles and previews anywhere the standard Null Sector uniforms exist.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <string>
#include <vector>

namespace ns {

enum class ShadertoyChannelKind { Auto, None, Texture, Buffer, Audio, Keyboard };

/** Explicit wiring for one iChannelN of the image pass. With Auto the
 *  converter infers the engine convention: iChannel0 = last folded buffer,
 *  iChannel1 = live scene (kept as a sampler), the rest unbound. */
struct ShadertoyChannelBind {
  ShadertoyChannelKind kind = ShadertoyChannelKind::Auto;
  std::string target;  // Buffer: pass name ("buffer_a"); Texture: label/path
};

struct ShadertoyConvertOptions {
  std::array<ShadertoyChannelBind, 4> channels;  // image-pass wiring (Auto = infer)
  std::string sourceLabel;                       // for the emitted header/notes
};

struct ShadertoyConvertResult {
  bool ok = false;
  std::string error;
  std::string fragment;
  std::vector<std::string> notes;
  /** sampler2D uniforms the user still has to bind ("uChannel0", ...) */
  std::vector<std::string> requiredTextures;
  std::vector<std::string> foldedBuffers;
  /** extra uniforms declared by the shim (for preview binding hints) */
  std::vector<std::string> uniforms;
};

/** Convert Shadertoy source (single-pass or `// pass:` markers) into a single
 *  Null Sector fragment shader. Never touches the GL; safe to unit test. */
ShadertoyConvertResult convertShadertoyToFragment(const std::string& source,
                                                  const ShadertoyConvertOptions& opts = {});

}  // namespace ns
