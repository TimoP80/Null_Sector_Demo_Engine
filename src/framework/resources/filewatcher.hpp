// ---------------------------------------------------------------------------
// FileWatcher - lightweight polling watcher for live reload. Cross-platform
// (std::filesystem only), watches files (or all files under a directory with
// matching extensions) and reports changes via a callback. Polling every
// frame costs nothing (two stat calls per file); the real editor pipelines
// (inotify/ReadDirectoryChangesW) can replace it later without changing the
// consumer contract.
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ns {

class FileWatcher {
public:
  using Cb = std::function<void(const std::string& path)>;

  FileWatcher() = default;  // polling only (no callback; read changed())
  explicit FileWatcher(Cb onChanged) : onChanged_(std::move(onChanged)) {}

  /** watch a single file (or a directory, recursively) */
  void add(const std::string& path, const std::vector<std::string>& extensions = {});
  void remove(const std::string& path);
  void clear();

  /** call once per frame: detects modifications and invokes the callback.
   *  Returns the number of files that changed. Deleted files are reported
   *  once (a missing dependency fails loudly) and stay in the watch set, so
   *  a deleted-then-recreated file is reported again when it comes back. */
  int poll();

  /** files known to be modified since the last poll */
  const std::vector<std::string>& changed() const { return changed_; }

private:
  struct Entry {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::vector<std::string> extensions;  // empty = all
    bool missing = false;                 // currently absent (reported once;
                                          // re-reported when it reappears)
  };

  Cb onChanged_;
  std::map<std::string, Entry> watched_;  // canonical path -> stamp
  std::vector<std::string> changed_;
  bool scanned_ = false;

  void scanDir(const std::string& dir, const std::vector<std::string>& exts);
  bool matches(const std::string& file, const std::vector<std::string>& exts) const;
};

}  // namespace ns
