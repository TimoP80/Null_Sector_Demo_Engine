// ---------------------------------------------------------------------------
// --check-hotreload smoke mode - shared bits between the shell (main.cpp,
// which writes the temp shader BEFORE DemoApp init so the FileWatcher's
// baseline scan covers it) and DemoApp::runHotReloadCheck() (which mutates
// it live and verifies the keep-previous + recompile behavior).
//
// The temp fragment pairs with the engine's fullscreen.vert, so the program
// is a plain fullscreen pass - the same family as post_copy.frag. Both
// sources go through resolveSource() (ES version line rewritten, precision
// stripped), exactly like every shipped shader.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace ns {

/** temp shader written into the resolved shader dir (cleaned up on exit) */
inline constexpr const char* kHotReloadCheckFrag = "hotreload_check.frag";

/** compiles: fullscreen.vert + this -> valid program */
inline const std::string kHotReloadFragValid =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vec4(vUV, 0.5, 1.0); }\n";

/** deliberately broken (syntax error at BADTOKEN): must fail to compile */
inline const std::string kHotReloadFragBroken =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor BADTOKEN;\n"
    "void main() { fragColor = vec4(1.0); }\n";

}  // namespace ns
