// ---------------------------------------------------------------------------
// ProductionPacker - packages a .nsd production into a .nsp file (GL-free).
//
// The packer starts from the production script and FOLLOWS its references
// (shadertoys, models, materials, post presets, textures, fonts, audio) - it
// never dumps the whole data/ directory. Dependency discovery reuses the
// existing .nsd parser (ScriptEngine) instead of string-searching the file:
// a clean reference walker mirrors the runtime's own resolution rules.
//
// Everything the packaged production needs at runtime is collected as VIRTUAL
// paths (data/..., shaders/..., assets/...), so the same package plays under
// PackageFileSystem with no path remapping.
//
//   ns_demo --pack data/demo.nsd --output NullSectorDemoEngine.nsp
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace ns {

/** pack `scriptArg` (a .nsd path relative to `rootDir`) into `outputPath`.
 *  `trackOverride` is an optional --track file to bundle as audio. Returns
 *  0 on success, non-zero with a printed error on failure. GL-free. */
int runProductionPacker(const std::string& rootDir, const std::string& scriptArg,
                        const std::string& trackOverride, const std::string& outputPath);

}  // namespace ns
