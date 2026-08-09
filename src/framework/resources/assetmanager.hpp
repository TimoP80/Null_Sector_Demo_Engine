// ---------------------------------------------------------------------------
// AssetManager - centralized, refcounted asset registry.
//
//   kinds      texture | shader | font | model | music | sample | script |
//              video | cubemap | material | timeline | scene | plugin
//   caching    each path loads once; acquire() bumps a refcount
//   unload     release() drops a ref and frees the handle at zero
//   reload     markDirty() (from a FileWatcher) then reloadDirty() re-runs
//              the kind's reload function and bumps the version counter, so
//              live reload never requires an engine restart.
//
// The manager is GL-free: loaders/reloaders return opaque void* handles that
// the app-side (GL) loader functions own. Framework classes only see paths,
// refcounts and versions.
//
// All path comparisons inside the manager use a canonical form (separators
// normalized to '/', "." / ".." resolved), so paths reported by the
// FileWatcher (native separators) and paths the app acquires with "/"
// concatenation always match - live reload must never silently miss an asset
// because of a Windows backslash.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ns {

/** canonical path form used for every AssetManager lookup + comparison.
 *  Syntactic only (no filesystem access, works on not-yet-existing files), so
 *  both sides must be derived from the same base - which they always are in
 *  this engine: the watcher scans the same resolved dir the app acquires
 *  from (resolveRuntimeDir). */
inline std::string assetCanonicalPath(const std::string& p) {
  return std::filesystem::path(p).lexically_normal().generic_string();
}

enum class AssetKind : int {
  Texture, Shader, Font, Model, Music, Sample, Script, Video, Cubemap,
  Material, Timeline, Scene, Plugin,
};

const char* assetKindName(AssetKind k);
AssetKind assetKindFromName(const std::string& n);

struct AssetInfo {
  std::string path;
  std::string kind;         // kind name
  int refs = 0;
  uint64_t version = 0;     // bumped on every successful (re)load
  bool loaded = false;
  bool dirty = false;       // source changed on disk, needs reload
  std::string error;        // last load/reload error (informative diagnostics)
  void* handle = nullptr;   // opaque app-owned pointer
  uint64_t bytes = 0;       // optional: approximate memory footprint
};

class AssetManager {
public:
  using LoadFn = std::function<void*(const std::string& path)>;      // new handle or nullptr
  using FreeFn = std::function<void(void*)>;
  using ReloadFn = std::function<bool(const std::string& path, void*& handle)>;  // true = swapped

  void registerKind(const std::string& kind, LoadFn load, FreeFn free, ReloadFn reload = {});

  /** load (once) + refcount++ ; returns the opaque handle (nullptr on error) */
  void* acquire(const std::string& path, const std::string& kind);
  /** refcount-- ; frees the handle at zero and removes the entry */
  void release(const std::string& path, const std::string& kind);

  AssetInfo* find(const std::string& path, const std::string& kind);
  const AssetInfo* find(const std::string& path, const std::string& kind) const;

  /** mark a source file dirty (from FileWatcher); reload happens on the next
   *  reloadDirty() call (deferred so it never happens mid-render) */
  void markDirty(const std::string& path);
  /** reload every dirty asset (also retrying never-loaded ones whose
   *  load failed earlier - a fixed file is picked up live); returns how
   *  many were reloaded */
  int reloadDirty();
  /** true when any managed asset is dirty (e.g. to pause the show clock) */
  bool anyDirty() const;

  uint64_t version(const std::string& path, const std::string& kind) const;

  std::vector<AssetInfo*> all();
  std::vector<const AssetInfo*> all() const;

  void clear();  // release everything (debug / reload-all)

private:
  struct KindOps {
    LoadFn load;
    FreeFn free;
    ReloadFn reload;
  };
  std::map<std::string, KindOps> kinds_;
  std::map<std::string, AssetInfo> assets_;  // key: kind + "\x1f" + path

  static std::string key(const std::string& path, const std::string& kind) {
    return kind + "\x1f" + assetCanonicalPath(path);
  }
};

}  // namespace ns
