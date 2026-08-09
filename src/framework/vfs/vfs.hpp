// ---------------------------------------------------------------------------
// VirtualFileSystem - the engine's asset-access abstraction.
//
// The engine never cares whether an asset lives on the development filesystem
// (data/, shaders/, assets/) or inside a packaged .nsp file: every runtime
// asset read goes through a VirtualFileSystem implementation.
//
//                       VirtualFileSystem
//                              |
//              +---------------+---------------+
//              |               |               |
//              v               v               v
//        DirectoryFS       PackageFS       EmbeddedFS (future)
//        development       .nsp            baked into the exe
//
// The VFS answers "where are these bytes?" (read/stat/exists/list). It knows
// NOTHING about what the bytes are - shaders, textures, models, fonts and
// audio are interpreted by the higher asset layer.
//
// Virtual path conventions (Phase 8):
//   - '/' is the separator on every host OS
//   - no leading '/', no drive letters, no '.'/'..' segments
//   - production-relative:  data/..., shaders/..., assets/...
//   - normalizeVirtualPath() enforces these rules and rejects traversal
//
// One process-wide runtime VFS is installed once near application init
// (setRuntimeFS); every runtime asset load goes through runtimeFS(). The
// editor keeps direct std::filesystem access for authoring/saving/watching.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ns {

/** stat result for a virtual path (0/missing when the path does not exist) */
struct VFileInfo {
  bool exists = false;
  bool isDir = false;
  uint64_t size = 0;   // file size in bytes (uncompressed)
  double mtime = 0.0;  // last-modified epoch seconds; 0 = immutable/unknown
                       // (packaged files never change, so hot reload sees 0)
};

/** the VFS interface - answers "where are these bytes?" */
class VirtualFileSystem {
public:
  virtual ~VirtualFileSystem() = default;

  /** does a file exist at this virtual path? (directories report false) */
  virtual bool exists(const std::string& vpath) const = 0;

  /** read the whole file as bytes; empty on a missing/unreadable file */
  virtual std::vector<uint8_t> read(const std::string& vpath) const = 0;

  /** read the whole file as text; empty on a missing/unreadable file */
  virtual std::string readText(const std::string& vpath) const = 0;

  /** stat a virtual path (files and directories) */
  virtual VFileInfo stat(const std::string& vpath) const = 0;

  /** direct children of a virtual directory, as FULL virtual paths (both
   *  files and directories; "" or "/" lists the root). Callers filter by
   *  stat() when they need one kind. */
  virtual std::vector<std::string> list(const std::string& vdir) const = 0;

  /** true for PackageFileSystem (--play mode); the app uses this to decide
   *  whether dev-only features (hot reload, editor) have real files to
   *  watch. Default false. */
  virtual bool isPackage() const { return false; }
};

// --- path rules (Phase 8) ----------------------------------------------------

/** normalize a virtual path: '\' -> '/', drop leading "./", collapse "//"
 *  and "." segments. Returns "" when the path is invalid (absolute, drive
 *  letter, ".." traversal, empty). */
std::string normalizeVirtualPath(const std::string& p);

/** true when normalizeVirtualPath(p) round-trips (safe to use as a key) */
bool isSafeVirtualPath(const std::string& p);

/** join a + "/" + b with normalization; "" when the result is unsafe */
std::string joinVirtualPath(const std::string& a, const std::string& b);

/** parent of a virtual path: "data/a/b.frag" -> "data/a"; "" for a bare
 *  filename. */
std::string parentVirtualPath(const std::string& p);

/** file name (last segment) of a virtual path; "" for "" or ".." */
std::string fileNameVirtualPath(const std::string& p);

// --- process-wide runtime VFS -------------------------------------------------

/** install the runtime VFS (takes ownership). Called once near application
 *  init: a PackageFileSystem for --play, a DirectoryFileSystem otherwise. */
void setRuntimeFS(std::unique_ptr<VirtualFileSystem> fs);

/** the process-wide runtime VFS. When nothing was installed, lazily returns
 *  a DirectoryFileSystem rooted at the current working directory, so
 *  GL-free preflight paths (--check-*, --pack) work without setup. */
VirtualFileSystem& runtimeFS();

/** true when the installed runtime VFS is a package (--play mode). */
bool runtimeFSIsPackage();

}  // namespace ns
