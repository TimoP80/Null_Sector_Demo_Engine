// ---------------------------------------------------------------------------
// Robust runtime path resolution for shaders + assets.
//
// The compile-time defines (NULLSECTOR_SHADER_DIR / NULLSECTOR_ASSET_DIR) bake
// the build machine's absolute source path into the binary. That breaks a
// packaged folder (exe + shaders/ + track.wav) copied to another machine, or
// a zip extraction elsewhere - every shader load fails and the whole demo
// degrades to SIGNAL LOST placeholders. Mirrors the default-track lookup:
// each runtime dir is resolved by walking candidates in order -
//   1. the NULLSECTOR_*_DIR env override (exact, highest priority)
//   2. the baked compile-time path (source tree) - only if it still exists
//   3. <exe-dir>/<name>   (packaged folder: resources beside the exe)
//   4. ./<name>           (current working directory)
// and falls back to the baked path when nothing exists, so error messages
// still point at the build-time location.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace ns {

/** directory containing the running executable ("" if it cannot be found) */
inline std::string exeDir() {
#ifdef _WIN32
  static const std::string dir = [] {
    char buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::filesystem::path p(buf);
    return p.has_parent_path() ? p.parent_path().string() : std::string();
  }();
  return dir;
#elif defined(__APPLE__)
  static const std::string dir = [] {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) != 0) return std::string();
    std::filesystem::path p(buf);
    return p.has_parent_path() ? p.parent_path().string() : std::string();
  }();
  return dir;
#else  // Linux / POSIX
  static const std::string dir = [] {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::filesystem::path p(buf);
    return p.has_parent_path() ? p.parent_path().string() : std::string();
  }();
  return dir;
#endif
}

/** resolve a runtime resource dir (shaders/assets) - see header comment */
inline std::string resolveRuntimeDir(const char* envVar, const char* baked, const char* name) {
  if (const char* env = std::getenv(envVar)) return env;

  const std::string exe = exeDir();
  const std::string candidates[] = {
    baked,
    exe.empty() ? "" : exe + "/" + name,
    name,  // cwd
  };
  std::error_code ec;
  for (const std::string& c : candidates) {
    if (!c.empty() && std::filesystem::is_directory(c, ec) && !ec) {
      if (c != baked) {
        static std::string noted;  // one line per dir name per process (shared across TUs)
        if (noted != name) {
          noted = name;
          std::fprintf(stderr, "[PATHS] '%s' resolved to '%s' (baked '%s' not present)\n",
                       name, c.c_str(), baked);
        }
      }
      return c;
    }
  }
  return baked;  // nothing found - keep the informative build-time path
}

}  // namespace ns
