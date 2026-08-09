// ---------------------------------------------------------------------------
// DegradedFX - the emergency renderer (port of src/engine/degraded.ts).
// When a scene's shaders fail to build, the director marks the scene broken
// and renders this SIGNAL LOST placeholder in its place. Self-contained
// shaders (no #include <common>) so it survives a broken common.glsl.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include <memory>

namespace ns {

struct Assets;

class DegradedFX {
public:
  bool init(const Assets* assets);
  /** draw the SIGNAL LOST placeholder for a broken scene into the current target */
  void render(EffectContext& ctx, const std::string& scene);
  /** no-post fallback: blit a scene texture straight to the screen */
  void blit(unsigned tex, const EffectContext& ctx);

private:
  void drawText(const std::string& text, float alpha, float glow, float beat, int sizePx, int viewW, int viewH);
  std::unique_ptr<Shader> staticProg_;
  std::unique_ptr<Shader> textProg_;
  std::unique_ptr<Shader> blitProg_;
  TextMesh textMesh_;
  const Assets* assets_ = nullptr;
  bool ok_ = false;
};

}  // namespace ns
