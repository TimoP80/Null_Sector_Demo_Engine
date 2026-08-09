// ---------------------------------------------------------------------------
// Editor production packaging.
// Builds a distributable folder from the currently loaded .nsd:
//   <project>.nsp       packaged production assets
//   <project>.exe       copied engine executable
//   launch.bat          packaged playback command
// and archives those files into a ZIP distribution.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

namespace ns {

struct EditorPackageResult {
  bool ok = false;
  std::string outputZip;
  std::string message;
  uint64_t nspBytes = 0;
  uint64_t zipBytes = 0;
  size_t assetCount = 0;
};

/** Build a self-contained Windows distribution for SCRIPT_PATH.
 *  The packer reuses the production NSP packer and the engine executable at
 *  EXECUTABLE_PATH. OUTPUT_ZIP is the final distribution archive. */
EditorPackageResult packageEditorProject(const std::string& projectRoot,
                                         const std::string& scriptPath,
                                         const std::string& trackPath,
                                         const std::string& executablePath,
                                         const std::string& outputZip);

}  // namespace ns
