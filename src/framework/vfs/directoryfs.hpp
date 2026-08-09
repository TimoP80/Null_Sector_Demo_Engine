// ---------------------------------------------------------------------------
// DirectoryFileSystem - a VirtualFileSystem over the development tree.
//
// Mounts map virtual path prefixes onto real directories:
//
//   mount("data",    "C:/proj/data")
//   mount("shaders", "C:/proj/shaders")
//   mount("assets",  "C:/proj/assets")
//   mount("",        "C:/proj")            // catch-all root
//
//   read("data/demo.nsd")  ->  C:/proj/data/demo.nsd
//   read("shaders/a.frag") ->  C:/proj/shaders/a.frag
//
// Longest-prefix wins, so "data/..." never falls through to the catch-all.
// Every lookup normalizes the virtual path first and rejects traversal
// (../, absolute, drive letters), so a VIRTUAL path can never escape the
// mounts. Note the boundary of that guarantee: DirectoryFS then resolves to
// the real filesystem and TRUSTS the mounted development tree itself. A
// symlink/junction placed inside a mount resolves to wherever it points -
// that is dev-tooling behavior, not a security sandbox, and the packer
// documents the same trust boundary. This is the VFS the app installs for
// normal development playback; the editor keeps direct std::filesystem
// access for authoring.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/vfs/vfs.hpp"

#include <string>
#include <vector>

namespace ns {

class DirectoryFileSystem : public VirtualFileSystem {
public:
  /** mount `prefix` ("" = catch-all root, longest-prefix match wins) onto
   *  the real directory `realDir`. */
  void mount(const std::string& prefix, const std::string& realDir);

  bool exists(const std::string& vpath) const override;
  std::vector<uint8_t> read(const std::string& vpath) const override;
  std::string readText(const std::string& vpath) const override;
  VFileInfo stat(const std::string& vpath) const override;
  std::vector<std::string> list(const std::string& vdir) const override;

  /** resolve a virtual path to the real filesystem path ("" when unsafe or
   *  unmounted) - used by dev tooling that needs a real path. */
  std::string resolve(const std::string& vpath) const;

private:
  struct Mount {
    std::string prefix;  // "" = catch-all
    std::string realDir;
  };
  /** longest-prefix mount lookup; fills realPath, returns false on unsafe
   *  paths or when nothing is mounted. */
  bool resolvePath(const std::string& vpath, std::string& realPath) const;
  std::vector<Mount> mounts_;
};

/** recursively collect every FILE under a virtual directory ("" = root),
 *  as full virtual paths. Shared by the packer (shader/font/track scans)
 *  and tests. */
void walkVirtualFiles(const VirtualFileSystem& fs, const std::string& root,
                      std::vector<std::string>& out);

}  // namespace ns
