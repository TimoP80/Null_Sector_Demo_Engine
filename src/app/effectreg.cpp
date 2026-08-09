#include "app/effectreg.hpp"
#include "app/shadertoy.hpp"
#include "effects/greetings.hpp"
#include "effects/intro.hpp"
#include "effects/scene.hpp"
#include "effects/tunnel.hpp"
#include "framework/core/log.hpp"

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ns {

void registerBuiltinEffects() {
  // --- fullscreen raymarcher scenes ----------------------------------------
  registerEffect("cathedral", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("cathedral.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(1);
    return e;
  });
  registerEffect("neuralnet", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("neuralnet.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.6f);
    return e;
  });
  registerEffect("infinitemachine", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("infinitemachine.frag");
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("voxel", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("voxel.frag", /*handoff=*/true);
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("logo", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("logo.frag", /*handoff=*/true);
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("tunnel", [](const Value& p) {
    auto e = std::make_unique<TunnelFX>();
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("ghostformation", [](const Value& p) {
    auto e = std::make_unique<ParticleStormFX>("ghostformation.vert", "ghostformation.frag",
                                                p.get("count").asInt(400000),
                                                "ghostformation_prev.frag");
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("intro", [](const Value&) { return std::make_unique<IntroFX>(); });
  registerEffect("greetings", [](const Value& p) {
    auto e = std::make_unique<GreetingsFX>();
    e->mode = p.get("mode").asFloat(0);
    return e;
  });


  // --- NULL SECTOR // NEURAL DUST - production scenes -------------------------
  // (scene shaders nd_*.frag live flat in shaders/ next to the flagship's;
  //  ndboot/ndlogo draw atlas glyphs in-shader via SceneFX::useFont, unit 11)
  registerEffect("ndboot", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_boot.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(1);
    e->useFont(true);
    return e;
  });
  registerEffect("ndcore", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_core.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.6f);
    return e;
  });
  registerEffect("ndtunnel", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_tunnel.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.75f);
    e->useFont(true);   // wall typography samples the atlas (unit 11)
    return e;
  });
  registerEffect("ndcity", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_city.frag", /*handoff=*/true);
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.75f);
    return e;
  });
  registerEffect("ndcorrupt", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_corrupt.frag", /*handoff=*/true);
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.75f);
    e->useFont(true);   // diagnostics text samples the atlas (unit 11)
    return e;
  });
  registerEffect("nddream", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_dream.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.75f);
    return e;
  });
  registerEffect("ndocean", [](const Value& p) {
    auto e = std::make_unique<ParticleStormFX>("nd_ocean.vert", "nd_ocean.frag",
                                                p.get("count").asInt(60000),
                                                "nd_ocean_prev.frag");
    e->mode = p.get("mode").asFloat(0);
    return e;
  });
  registerEffect("ndnet", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_net.frag", /*handoff=*/true);
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(0.6f);
    return e;
  });
  registerEffect("ndlogo", [](const Value& p) {
    auto e = std::make_unique<SceneFX>("nd_logo.frag");
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(1);
    e->useFont(true);
    return e;
  });

  // --- generic fullscreen shader (frag file from params) ---------------------
  registerEffect("quad", [](const Value& p) {
    auto e = std::make_unique<SceneFX>(p.get("frag").asStr("passthrough.frag").c_str(),
                                       p.get("handoff").asBool(false));
    e->mode = p.get("mode").asFloat(0);
    e->renderScale = p.get("renderScale").asFloat(1);
    return e;
  });

  // --- Shadertoy importer -----------------------------------------------------
  registerEffect("shadertoy", [](const Value& p) {
    return std::make_unique<ShadertoyFX>(p.get("file").asStr(),
                                         p.get("tex").asStr(), p.get("width").asInt(0),
                                         p.get("height").asInt(0));
  });

  Log::info("FX", "registered " + std::to_string(effectFactory().count()) + " built-in effects");
}

// ---------------------------------------------------------------------------
// dynamic plugin loading
// ---------------------------------------------------------------------------
bool loadEffectPlugin(const std::string& path) {
#ifdef _WIN32
  HMODULE h = ::LoadLibraryA(path.c_str());
  if (!h) {
    Log::error("FX", "plugin load failed (LoadLibrary): " + path);
    return false;
  }
  using RegFn = void (*)();
  const RegFn fn = reinterpret_cast<RegFn>(::GetProcAddress(h, "ns_register_plugins"));
#else
  void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    Log::error("FX", std::string("plugin load failed (dlopen): ") + path + " - " + ::dlerror());
    return false;
  }
  using RegFn = void (*)();
  const RegFn fn = reinterpret_cast<RegFn>(::dlsym(h, "ns_register_plugins"));
#endif
  if (!fn) {
    Log::error("FX", "plugin '" + path + "' does not export ns_register_plugins()");
    return false;
  }
  fn();
  Log::info("FX", "plugin loaded: " + path + " (" + std::to_string(effectFactory().count()) + " effects registered)");
  return true;
}

}  // namespace ns
