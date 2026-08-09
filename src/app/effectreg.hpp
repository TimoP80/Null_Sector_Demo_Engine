// ---------------------------------------------------------------------------
// EffectRegistry - the effect plugin system.
//
//   effectFactory()   the process-wide Factory<Effect> keyed by name
//   registerBuiltinEffects()  registers every built-in effect (intro, tunnel,
//                     cathedral, neuralnet, ..., shadertoy, quad) from the
//                     content library - the engine loop only sees names
//   loadEffectPlugin(path)  dlopen/LoadLibrary a plugin that exports
//                     `extern "C" void ns_register_plugins()`, which calls
//                     ns::effectFactory().reg(...) to add new effects at
//                     runtime without touching the engine
//
// New content = a new effect class + one registration line. The demo script
// addresses effects by name, so the whole show is data.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "framework/core/factory.hpp"

#include <memory>
#include <string>

namespace ns {

/** the process-wide effect factory */
inline Factory<Effect>& effectFactory() { return factory<Effect>(); }

/** register an effect factory (also used by plugins) */
inline void registerEffect(const std::string& name, Factory<Effect>::Fn fn) {
  effectFactory().reg(name, std::move(fn));
}

/** register every built-in effect; call once at app boot */
void registerBuiltinEffects();

/** load a plugin shared library that exports ns_register_plugins(); returns
 *  false (with a log) on failure. Effect names from the plugin become
 *  available to scripts immediately. */
bool loadEffectPlugin(const std::string& path);

}  // namespace ns
