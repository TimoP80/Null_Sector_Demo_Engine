// ---------------------------------------------------------------------------
// SceneFX + ParticleStormFX - generic drivers for the scene shaders that
// previously fell through to the DegradedFX placeholder. Both share the
// KickFlash detector so every scene slams on real kick drum hits (audio
// analyser react.kick, tempo-independent) and drives the same post params.
//
//   SceneFX        fullscreen raymarcher scenes: cathedral, neuralnet,
//                  infinitemachine, voxel (any .frag reading the shared NullBlock)
//   ParticleStormFX  GPU-driven point cloud with additive blending + beat
//                  explosions + kick flash. Shader pair + particle count are
//                  configurable: the generic storm (particles.vert/frag) and
//                  the Ghost Formation (ghostformation.vert/frag) share it.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "effects/kickflash.hpp"
#include "engine/assets.hpp"
#include "engine/mesh.hpp"
#include "engine/perftimer.hpp"
#include "engine/shader.hpp"
#include <map>
#include <memory>
#include <string>

namespace ns {

/** generic fullscreen scene: renders a raymarcher .frag over the fs triangle.
 *  With handoff = true the scene runs the in-scene handoff: uTransition is
 *  driven from timeline.s.transition (0..1 over the section's first two
 *  beats) and uPrevScene (unit 9) is bound to the previous frame, so shaders
 *  that declare those uniforms (voxel.frag, logo.frag) ignite from and fade
 *  out the outgoing scene instead of reading garbage. Scenes without the
 *  uniforms are unaffected (set1f/set1i are no-ops). */
class SceneFX : public Effect {
public:
  explicit SceneFX(const char* fragFile, bool handoff = false)
      : fragFile_(fragFile), handoff_(handoff) {}
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  float mode = 0.0f;   // per-scene mode uniform (cathedral: 0 assemble, 1 deconstruct)

  /** render target scale (<1 renders into a reduced-size FBO, the driver
   *  upscales to the window). The scene shader reads uSceneRes for its ray
   *  setup so gl_FragCoord stays consistent with the reduced target; shaders
   *  without uSceneRes are unaffected (set2f is a no-op). */
  float renderScale = 1.0f;

  /** bind an extra per-scene texture (e.g. the logo mask) on a dedicated unit.
   *  The Texture must outlive the effect; tex 0 unbinds. */
  void setTex(const char* name, unsigned tex, int unit) {
    texName_ = name; tex_ = tex; texUnit_ = unit;
  }

  /** bind the shared TrueType font atlas (uFont on unit 11, uAtlas/uCell from
   *  assets->fontMetrics) so a scene shader can draw atlas glyphs (e.g. the
   *  logo scene's NULL SECTOR DEMO ENGINE subtitle). The atlas is owned by
   *  Assets for the whole show, so no lifetime concerns. */
  void useFont(bool on) { useFont_ = on; }
  /** Select a per-effect TrueType atlas. The path may be a runtime virtual
   *  path such as assets/fonts/intro.ttf or a development absolute path. */
  void setFontFile(const std::string& path);
  /** Select a per-effect fill texture for generated/textured shaders. */
  void setTextureFile(const std::string& path, const char* uniform = "uFillTexture",
                      int unit = 1);

  /** stable run snapshot (--perf-json exit dump). */
  PerfSample perfSample() const override { return perf_.sample(); }

  /** current EMA ms/frame (0 until the first sample - the per-second
   *  --perf-csv rows use this). */
  double emaMs() const { return perf_.emaMs(); }

  /** most recent raw sample (unsmoothed - the --perf-raw rows). */
  double lastRawMs() const { return perf_.lastRawMs(); }

  /** extra float uniforms applied every render (driven by the animation
   *  system: `anim logo uniform:uGlow ...`). Names missing from the shader
   *  are ignored. */
  std::map<std::string, float> extraUniforms;

private:
  std::string fragFile_;
  bool handoff_ = false;
  bool useFont_ = false;
  std::string fontFile_;
  std::unique_ptr<Assets> fontAsset_;
  std::string textureFile_;
  std::string textureUniform_ = "uFillTexture";
  int textureUnit_ = 1;
  std::unique_ptr<Texture> textureAsset_;
  const char* texName_ = nullptr;
  unsigned tex_ = 0;
  int texUnit_ = 10;
  std::unique_ptr<Shader> prog_;
  KickFlash kick_;
  PerfTimer perf_;  // GL_TIMESTAMP ring: periodic "X ms/frame GPU" log line
};

/** GPU-driven particle cloud (configurable shader pair + count). During the
 *  first two beats of its section (timeline.s.transition) it runs an in-scene
 *  handoff: the previous frame fades out as a base layer while particles
 *  over its bright pixels ignite and inherit its light. */
class ParticleStormFX : public Effect {
public:
  explicit ParticleStormFX(const char* vert = "particles.vert",
                           const char* frag = "particles.frag",
                           int count = 5000,
                           const char* prevFrag = "particles_prev.frag")
      : vertFile_(vert), fragFile_(frag), prevFrag_(prevFrag), count_(count) {}
  void init(EffectContext& ctx) override;
  void render(EffectContext& ctx) override;
  float mode = 0.0f;   // per-scene mode uniform (ghostformation: 0 formation, 1 stillness bass flicker)

  /** particle count (the --perf-json dump labels the storm by size). */
  int count() const { return count_; }

  /** stable run snapshot (--perf-json exit dump). */
  PerfSample perfSample() const override { return perf_.sample(); }

  /** current EMA ms/frame (0 until the first sample - the per-second
   *  --perf-csv rows use this). */
  double emaMs() const { return perf_.emaMs(); }

  /** most recent raw sample (unsmoothed - the --perf-raw rows). */
  double lastRawMs() const { return perf_.lastRawMs(); }

private:
  std::string vertFile_, fragFile_, prevFrag_;
  std::unique_ptr<Shader> prog_, prevProg_;
  Mesh cloud_{::gl::POINTS};
  KickFlash kick_;
  PerfTimer perf_;  // GL_TIMESTAMP ring: periodic "X ms/frame GPU" log line
  int count_ = 5000;
};

}  // namespace ns
