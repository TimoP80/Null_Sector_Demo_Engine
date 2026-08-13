// ---------------------------------------------------------------------------
// ProdCheck - headless production validation (--check-production[=PATH]).
//
// GL-free: parses the production with the framework ScriptEngine, builds the
// timeline, and verifies every reference the script makes resolves against
// the effect registry and the files on disk:
//
//   show X          X is a scene, a registered effect, or a shadertoy:/quad:
//                   file that exists
//   load shadertoy  data/shadertoy/FILE exists
//   load model      data/models/FILE exists
//   load material   data/materials/NAME.json exists
//   load plugin     the shared library exists
//   shader FILE     shaders/FILE exists
//   post preset     data/post/NAME.json exists
//   camera rig      the rig type is one of the known behaviors
//   mesh/sprite/video the referenced model / texture / video files exist
//   sections        starts are monotonic, ends are within the declared duration
//
// Runs before the window/GL context is created (it needs neither), so CI can
// validate a production without a display. Exit code is 0 when every check
// passes, 1 otherwise.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace ns {

struct ProdCheckResult {
  int ok = 0;
  int total = 0;
  std::vector<std::string> failures;  // human-readable FAIL lines
};

/** validate the production script at `scriptPath` against the effect
 *  registry (builtins must be registered first) and the on-disk assets.
 *  dataDir = data/..., shaderDir = shaders/... (see AppAssets). */
ProdCheckResult checkProduction(const std::string& scriptPath,
                                const std::string& dataDir,
                                const std::string& shaderDir);

}  // namespace ns
