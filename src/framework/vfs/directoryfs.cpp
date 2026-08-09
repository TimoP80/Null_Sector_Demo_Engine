#include "framework/vfs/directoryfs.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace ns {

void DirectoryFileSystem::mount(const std::string& prefix, const std::string& realDir) {
  std::string p = prefix;
  // normalize the mount prefix: "data/" -> "data", "" stays the catch-all
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  if (p.size() > 1 && p.front() == '/') p.erase(p.begin());
  mounts_.push_back(Mount{p, realDir});
  // longest prefix first so "data/x" never falls through to ""
  std::stable_sort(mounts_.begin(), mounts_.end(),
                   [](const Mount& a, const Mount& b) {
                     return a.prefix.size() > b.prefix.size();
                   });
}

bool DirectoryFileSystem::resolvePath(const std::string& vpath, std::string& realPath) const {
  // the virtual root ("") maps to the catch-all mount's real dir; anything
  // else that fails normalization (traversal, absolute, drive letters) is
  // rejected outright - unsafe input must never resolve to a real path
  if (vpath.empty() || vpath.find_first_not_of('/') == std::string::npos) {
    for (const auto& m : mounts_) {
      if (m.prefix.empty()) {
        realPath = m.realDir;
        return true;
      }
    }
    return false;
  }
  const std::string n = normalizeVirtualPath(vpath);
  if (n.empty()) return false;
  // longest-prefix match over the sorted mounts
  for (const auto& m : mounts_) {
    if (m.prefix.empty()) {
      realPath = m.realDir.empty() ? n : m.realDir + "/" + n;
      return true;
    }
    if (n.size() > m.prefix.size() && n.compare(0, m.prefix.size(), m.prefix) == 0 &&
        n[m.prefix.size()] == '/') {
      realPath = m.realDir.empty() ? n.substr(m.prefix.size() + 1)
                                   : m.realDir + "/" + n.substr(m.prefix.size() + 1);
      return true;
    }
    if (n == m.prefix) {  // the mount root itself
      realPath = m.realDir;
      return true;
    }
  }
  return false;
}

bool DirectoryFileSystem::exists(const std::string& vpath) const {
  std::string real;
  if (!resolvePath(vpath, real)) return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(real, ec) && !ec;
}

std::vector<uint8_t> DirectoryFileSystem::read(const std::string& vpath) const {
  std::vector<uint8_t> out;
  std::string real;
  if (!resolvePath(vpath, real)) return out;
  std::ifstream f(real, std::ios::binary);
  if (!f) return out;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return out;
}

std::string DirectoryFileSystem::readText(const std::string& vpath) const {
  std::string out;
  std::string real;
  if (!resolvePath(vpath, real)) return out;
  std::ifstream f(real, std::ios::binary);
  if (!f) return out;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

VFileInfo DirectoryFileSystem::stat(const std::string& vpath) const {
  VFileInfo out;
  std::string real;
  if (!resolvePath(vpath, real)) return out;
  std::error_code ec;
  const bool isDir = std::filesystem::is_directory(real, ec);
  if (ec) return out;
  if (!isDir && !std::filesystem::is_regular_file(real, ec)) return out;
  if (ec) return out;
  out.exists = true;
  out.isDir = isDir;
  out.size = isDir ? 0 : (uint64_t)std::filesystem::file_size(real, ec);
  if (ec) out.size = 0;
  const auto mt = std::filesystem::last_write_time(real, ec);
  if (!ec) {
    // file_time_type -> epoch seconds, portable C++17 (no file_clock::to_sys)
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        mt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    out.mtime = std::chrono::duration<double>(sys.time_since_epoch()).count();
  }
  return out;
}

std::vector<std::string> DirectoryFileSystem::list(const std::string& vdir) const {
  std::vector<std::string> out;
  std::string real;
  if (!resolvePath(vdir, real)) return out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(real, ec)) {
    if (ec) break;
    const std::string name = e.path().filename().string();
    std::string vp = vdir.empty() ? name : vdir + "/" + name;
    vp = normalizeVirtualPath(vp);
    if (!vp.empty()) out.push_back(std::move(vp));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string DirectoryFileSystem::resolve(const std::string& vpath) const {
  std::string real;
  return resolvePath(vpath, real) ? real : std::string();
}

void walkVirtualFiles(const VirtualFileSystem& fs, const std::string& root,
                      std::vector<std::string>& out) {
  const std::string dir = root.empty() ? "" : root;
  for (const auto& entry : fs.list(dir)) {
    const VFileInfo fi = fs.stat(entry);
    if (fi.isDir) walkVirtualFiles(fs, entry, out);
    else if (fi.exists) out.push_back(entry);
  }
}

}  // namespace ns
