// ---------------------------------------------------------------------------
// Effect layer foundation (port of src/effects/base.ts).
// Every scene renders through an EffectContext; the engine subsystems
// (ubo.cpp, cinetext.hpp, degraded.hpp) already include this header, so it is
// the shared contract between the director loop and the reusable renderers.
// Only forward declarations live here - no engine includes, so there is no
// include cycle with the engine headers that depend on it.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gputimer.hpp"  // PerfSample - GL-free, header-only

namespace ns {

class Renderer;
struct Assets;
class Timeline;
class Camera;
class AudioEngine;
class SharedBlock;
class PostFX;
class FrameTarget;

struct EffectContext {
  Renderer* r = nullptr;        // GL state + fullscreen triangle
  Assets* assets = nullptr;     // font atlas + metrics
  Timeline* timeline = nullptr; // beat/bar/section clock
  Camera* camera = nullptr;     // view/proj + shake + DOF params
  AudioEngine* audio = nullptr; // react analyser values
  SharedBlock* shared = nullptr;// NullBlock UBO (written before render)
  PostFX* post = nullptr;       // music-driven post params (bloom/glitch...)
  FrameTarget* hdr = nullptr;   // current HDR scene target (post FX input)
  float time = 0;               // show clock (seconds)
  float dt = 0;                 // frame delta (seconds)
};

class Effect {
public:
  virtual ~Effect() = default;
  /** build programs + targets once the GL context and assets exist */
  virtual void init(EffectContext& ctx) = 0;
  /** render into ctx->hdr (the target is bound and cleared by the caller) */
  virtual void render(EffectContext& ctx) = 0;
  /** rebuild per-resolution targets after a window resize */
  virtual void resize(EffectContext& ctx) {}
  /** stable GPU-time snapshot over the run (--perf-json / --perf-csv); the
   *  default reports nothing - only effects that own a PerfTimer (the
   *  shadertoy importer, scene drivers, particles) override it. */
  virtual PerfSample perfSample() const { return PerfSample{}; }
};

}  // namespace ns
