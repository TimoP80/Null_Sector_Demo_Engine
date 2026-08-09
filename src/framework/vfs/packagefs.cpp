#include "framework/vfs/packagefs.hpp"

#include <algorithm>
#include <cstdio>

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
  // a directory: consulted from the reader's open-time directory index
  if (reader_->isDirectory(n)) {
    out.exists = true;
    out.isDir = true;
    return out;
  }
  return out;
}

std::vector<std::string> PackageFileSystem::list(const std::string& vdir) const {
  if (!reader_) return {};
  // the reader's open-time directory index answers in O(children) - no scan
  return reader_->listDirectory(vdir);
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
