#include "framework/vfs/vfs.hpp"
#include "framework/vfs/directoryfs.hpp"

#include <algorithm>
#include <filesystem>

namespace ns {

// ---------------------------------------------------------------------------
// path normalization (Phase 8)
// ---------------------------------------------------------------------------
std::string normalizeVirtualPath(const std::string& p) {
  if (p.empty()) return "";
  std::string s = p;
  // Windows separators are virtual separators too
  std::replace(s.begin(), s.end(), '\\', '/');
  // reject absolute paths, drive letters and UNC
  if (s[0] == '/') return "";
  if (s.size() >= 2 && s[1] == ':') return "";
  if (s.find(":/") != std::string::npos) return "";
  if (s.find(":") != std::string::npos) return "";  // any colon is suspicious

  // split into segments, resolving '.' and rejecting '..'
  std::vector<std::string> segs;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t slash = s.find('/', pos);
    const std::string seg = s.substr(
        pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (!seg.empty() && seg != ".") {
      if (seg == "..") return "";  // traversal rejected
      segs.push_back(seg);
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  if (segs.empty()) return "";

  std::string out;
  for (size_t i = 0; i < segs.size(); i++) {
    if (i) out += '/';
    out += segs[i];
  }
  return out;
}

bool isSafeVirtualPath(const std::string& p) {
  return !p.empty() && normalizeVirtualPath(p) == p;
}

std::string joinVirtualPath(const std::string& a, const std::string& b) {
  if (a.empty()) return normalizeVirtualPath(b);
  if (b.empty()) return normalizeVirtualPath(a);
  std::string joined = a;
  if (joined.back() != '/') joined += '/';
  joined += b;
  return normalizeVirtualPath(joined);
}

std::string parentVirtualPath(const std::string& p) {
  const std::string n = normalizeVirtualPath(p);
  if (n.empty()) return "";
  const size_t slash = n.rfind('/');
  if (slash == std::string::npos) return "";
  return n.substr(0, slash);
}

std::string fileNameVirtualPath(const std::string& p) {
  const std::string n = normalizeVirtualPath(p);
  if (n.empty()) return "";
  const size_t slash = n.rfind('/');
  return slash == std::string::npos ? n : n.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// process-wide runtime VFS
// ---------------------------------------------------------------------------
namespace {
std::unique_ptr<VirtualFileSystem>& fsSlot() {
  static std::unique_ptr<VirtualFileSystem> fs;
  return fs;
}
}  // namespace

void setRuntimeFS(std::unique_ptr<VirtualFileSystem> fs) {
  fsSlot() = std::move(fs);
}

VirtualFileSystem& runtimeFS() {
  auto& slot = fsSlot();
  if (!slot) {
    // GL-free default: a DirectoryFileSystem rooted at the cwd. The app
    // installs the fully-mounted dev VFS (data/shaders/assets) in main().
    auto fs = std::make_unique<DirectoryFileSystem>();
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    if (!ec) fs->mount("", cwd);
    slot = std::move(fs);
  }
  return *slot;
}

bool runtimeFSIsPackage() {
  // PackageFileSystem sets a flag; the default/install path never does.
  // Implemented via a dynamic cast so the framework core has no hard
  // dependency on the package reader (tests build the lib without it).
  return fsSlot() != nullptr && fsSlot()->isPackage();
}

}  // namespace ns
