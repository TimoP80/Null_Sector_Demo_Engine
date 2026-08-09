#include "framework/resources/filewatcher.hpp"
#include "framework/core/log.hpp"

#include <algorithm>

namespace ns {

void FileWatcher::add(const std::string& path, const std::vector<std::string>& extensions) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    scanDir(path, extensions);
  } else {
    std::filesystem::path p(path);
    if (std::filesystem::exists(p, ec)) {
      std::string canon = p.lexically_normal().string();
      std::error_code ec2;
      watched_[canon] = {std::filesystem::last_write_time(p, ec2), std::filesystem::file_size(p, ec2),
                         extensions};
    } else {
      Log::warn("WATCH", "add: no such file: " + path);
    }
  }
}

void FileWatcher::remove(const std::string& path) {
  const std::string canon = std::filesystem::path(path).lexically_normal().string();
  for (auto it = watched_.begin(); it != watched_.end();) {
    if (it->first == canon) it = watched_.erase(it);
    else ++it;
  }
}

void FileWatcher::clear() { watched_.clear(); }

bool FileWatcher::matches(const std::string& file, const std::vector<std::string>& exts) const {
  if (exts.empty()) return true;
  const std::string ext = std::filesystem::path(file).extension().string();
  return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

void FileWatcher::scanDir(const std::string& dir, const std::vector<std::string>& exts) {
  std::error_code ec;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec)) continue;
    const std::string file = e.path().lexically_normal().string();
    if (!matches(file, exts)) continue;
    std::error_code ec2;
    watched_[file] = {std::filesystem::last_write_time(e.path(), ec2),
                      std::filesystem::file_size(e.path(), ec2), exts};
  }
}

int FileWatcher::poll() {
  changed_.clear();
  int n = 0;
  for (auto& kv : watched_) {
    const std::string& file = kv.first;
    Entry& e = kv.second;
    std::error_code ec;
    const std::filesystem::file_time_type mt = std::filesystem::last_write_time(file, ec);
    if (ec) {
      // file disappeared: report it ONCE (a missing dependency must fail
      // loudly, not silently drop off the watch set), then stay in the watch
      // set so a later recreation is detected and re-reported too
      if (!e.missing) {
        e.missing = true;
        if (scanned_) {
          changed_.push_back(file);
          if (onChanged_) onChanged_(file);
          n++;
        }
      }
      continue;
    }
    const std::uintmax_t sz = std::filesystem::file_size(file, ec);
    if (e.missing) {
      // the file came back: re-arm the watch and report the recreation
      e.missing = false;
      e.mtime = mt;
      e.size = sz;
      if (scanned_) {
        changed_.push_back(file);
        if (onChanged_) onChanged_(file);
        n++;
      }
      continue;
    }
    if (mt != e.mtime || sz != e.size) {
      // ignore the initial scan (stamp matches), then report real changes
      if (scanned_) {
        changed_.push_back(file);
        if (onChanged_) onChanged_(file);
        n++;
      }
      e.mtime = mt;
      e.size = sz;
    }
  }
  scanned_ = true;
  return n;
}

}  // namespace ns
