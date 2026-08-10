// ---------------------------------------------------------------------------
// PackageFileSystem - a VirtualFileSystem backed by a .nsp package.
//
// Mounting a package makes it indistinguishable from the dev tree to every
// runtime asset load: read("shaders/tunnel.frag") works whether the file is
// data/shaders/tunnel.frag or inside NullSectorDemoEngine.nsp.
//
//   PackageFileSystem fs;
//   std::string err;
//   if (!fs.open("NullSectorDemoEngine.nsp", &err)) { ... }
//   setRuntimeFS(std::make_unique<PackageFileSystem>(std::move(fs)));
//
// Reads are random-access + hash-verified per file; structural validation
// happens at open() so malformed packages are rejected safely. Packaged
// files are immutable (mtime 0), which is what turns hot reload off for
// packaged playback - there is nothing on disk to watch.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/vfs/nspack.hpp"
#include "framework/vfs/vfs.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ns {

class PackageFileSystem : public VirtualFileSystem {
public:
  /** open + validate the package; false with err on a malformed file. */
  bool open(const std::string& path, std::string* err);

  bool exists(const std::string& vpath) const override;
  std::vector<uint8_t> read(const std::string& vpath) const override;
  std::string readText(const std::string& vpath) const override;
  VFileInfo stat(const std::string& vpath) const override;
  std::vector<std::string> list(const std::string& vdir) const override;

  bool isPackage() const override { return true; }

  const PackageReader& reader() const { return *reader_; }
  /** the virtual path of the production .nsd (from the .ns-production
   *  marker), or "" when the package has no marker. */
  std::string productionScriptPath() const;

private:
  std::unique_ptr<PackageReader> reader_;
};

}  // namespace ns
