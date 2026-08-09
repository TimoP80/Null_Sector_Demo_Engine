// ---------------------------------------------------------------------------
// PostStack - a configurable, stackable post-processing pipeline.
//
// The chain is built from data (a JSON preset or a script `post preset` /
// `post { ... }` block) instead of being hardcoded into the renderer:
//
//   { "passes": [
//       { "name": "bloom",      "threshold": 1.2, "intensity": 1.4 },
//       { "name": "vignette",   "amount": 0.4 },
//       { "name": "grain",      "amount": 0.18 },
//       { "name": "scanlines",  "amount": 0.3 },
//       { "name": "grade",      "tonemap": true, "saturation": 1.05 },
//       { "name": "fxaa" }
//   ] }
//
// Available passes: dof, bloom, chromatic, vignette, grain, scanlines,
// pixelate, grade, fog, fxaa, copy. Passes ping-pong between two RGBA16F targets;
// depth-dependent passes (dof, fog) must come first - the scene shaders pack
// depth in the alpha channel and later passes copy it through, but fxaa does
// not, so fxaa belongs at the end.
//
// The music-reactive params (kick flash, glitch, exposure) ride along via
// PostCtx and map onto bloom intensity / grade exposure / chromatic amount,
// so preset-driven stacks still slam with the drum.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/framebuffer.hpp"
#include "engine/renderer.hpp"
#include "app/shadermanager.hpp"
#include "framework/core/value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ns {

class Camera;

struct PostCtx {
  float time = 0;
  float dt = 0;
  const Camera* camera = nullptr;   // dof focus/aperture + fog
  const Renderer* r = nullptr;      // fsTriangle VAO for the pass draws (core
                                    // profile: raw glDrawArrays w/o a VAO is a
                                    // no-op, so every pass needs this)
  float motion = 0;
  // music-reactive params (fed from the effects' PostFX::fx writes)
  float bloomMul = 1.0f;
  float glitch = 0.0f;
  float exposure = 1.0f;
  float heat = 0.0f;
  float kick = 0.0f;
};

class PostPass {
public:
  std::string name;
  Value params;
  virtual ~PostPass() = default;
  virtual bool init(ShaderManager& sm) = 0;
  virtual void resize(int w, int h) {}
  /** render src -> dst at w x h (FBOs already bound by the stack caller) */
  virtual void render(unsigned src, unsigned dst, int w, int h, const PostCtx& ctx) = 0;
};

class PostStack {
public:
  explicit PostStack(Renderer& r) : r_(r) {}
  ~PostStack();

  /** build the chain from a JSON/script value; throws on unknown passes */
  bool loadPreset(const Value& preset, ShaderManager& sm);
  /** build from a JSON file (data/post/<file>) */
  bool loadPresetFile(const std::string& path, ShaderManager& sm);

  void clear();
  void resize(int w, int h);

  /** run the chain; returns the final texture (== input when the chain is
   *  empty, or -1 on error). Depth alpha is preserved through the passes. */
  unsigned process(unsigned input, const PostCtx& ctx, int w, int h);

  /** blit the final texture to the default framebuffer (tonemaps if the
   *  chain has no grade pass) */
  void present(unsigned finalTex, int viewW, int viewH);

  /** the chain's last output (used as the prev-frame for in-scene handoffs) */
  unsigned lastOutput() const { return lastOutput_; }

  std::vector<std::string> chain() const;
  bool empty() const { return passes_.empty(); }
  bool hasTonemap() const;

  void setDefaultParam(const std::string& pass, const std::string& key, double v);

private:
  Renderer& r_;
  std::vector<std::unique_ptr<PostPass>> passes_;
  std::vector<FrameTarget> pp_;     // ping-pong targets (0/1)
  std::shared_ptr<ProgramState> copyProg_;    // lossless HDR copy
  std::shared_ptr<ProgramState> presentProg_;// final blit
  unsigned lastOutput_ = 0;
  int w_ = 1, h_ = 1;

  void ensureTargets(int w, int h);
  void drawPass(PostPass& p, unsigned src, unsigned dst, int w, int h, const PostCtx& ctx);
};

}  // namespace ns
