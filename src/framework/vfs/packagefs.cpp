#include "framework/vfs/packagefs.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace ns {

bool PackageFileSystem::open(const std::string& path, std::string* err) {
  auto r = std::make_unique<PackageReader>();
  if (!r->open(path, err)) return false;
  reader_ = std::move(r);
  return true;
}

bool PackageFileSystem::exists(const std::string& vpath) const {
  if (!reader_) return false;
  const std::string n = normalizeVirtualPath(vpath);
  return !n.empty() && reader_->has(n);
}

std::vector<uint8_t> PackageFileSystem::read(const std::string& vpath) const {
  if (!reader_) return {};
  return reader_->read(vpath);
}

std::string PackageFileSystem::readText(const std::string& vpath) const {
  if (!reader_) return {};
  return reader_->readText(vpath);
}

VFileInfo PackageFileSystem::stat(const std::string& vpath) const {
  VFileInfo out;
  if (!reader_) return out;
  const std::string n = normalizeVirtualPath(vpath);
  if (n.empty()) return out;
  if (reader_->has(n)) {
    out.exists = true;
    out.size = reader_->fileSize(n);
    out.mtime = 0;  // immutable
    return out;
  }
  // a directory: exists when any packaged file lives under n + "/"
  const std::string prefix = n + "/";
  for (const auto& f : reader_->fileList()) {
    if (f.size() > prefix.size() && f.compare(0, prefix.size(), prefix) == 0) {
      out.exists = true;
      out.isDir = true;
      return out;
    }
  }
  return out;
}

std::vector<std::string> PackageFileSystem::list(const std::string& vdir) const {
  std::vector<std::string> out;
  if (!reader_) return out;
  const std::string n = normalizeVirtualPath(vdir);
  if (n.empty()) {
    // root: first segment of every file + bare root files
    std::set<std::string> roots;
    for (const auto& f : reader_->fileList()) {
      const size_t slash = f.find('/');
      if (slash == std::string::npos) roots.insert(f);
      else roots.insert(f.substr(0, slash));
    }
    out.assign(roots.begin(), roots.end());
    return out;
  }
  const std::string prefix = n + "/";
  std::set<std::string> children;
  for (const auto& f : reader_->fileList()) {
    if (f.size() <= prefix.size() || f.compare(0, prefix.size(), prefix) != 0)
      continue;
    const std::string rest = f.substr(prefix.size());
    const size_t slash = rest.find('/');
    children.insert(prefix + (slash == std::string::npos ? rest : rest.substr(0, slash)));
  }
  out.assign(children.begin(), children.end());
  return out;
}

std::string PackageFileSystem::productionScriptPath() const {
  if (!reader_) return "";
  const std::string v = reader_->readText(kNspProductionMarker);
  const std::string n = normalizeVirtualPath(v);
  if (n.empty()) return "";
  // the marker must point at a file that is actually packaged
  return reader_->has(n) ? n : "";
}

}  // namespace ns
