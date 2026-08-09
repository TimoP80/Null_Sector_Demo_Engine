// ---------------------------------------------------------------------------
// ShaderManager - modern shader management for the data-driven effects.
//
//   compilation   programs compile on first get() (throw with the GLSL log)
//   include       #include <common> and #include "file.glsl" resolve relative
//                 to the shader dir; dependencies are tracked per program so
//                 a change to common.glsl recompiles every dependent program
//   hot reload    pollHotReload() recompiles programs whose source (or any
//                 include) changed on disk; failures keep the previous
//                 program and log the error - the show never goes dark
//   caching       one ProgramState per (vert,frag) pair, shared by all users
//   reflection    active-uniform list exposed after link (for the inspector)
//   uniform binding  ProgramRef routes set* calls through a cached location
//                 map that is rebuilt on every reload
//
// Effects that need live reloading hold a ProgramRef (shared_ptr to the state)
// instead of a Shader; the id() swaps transparently on reload.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns {

struct ShaderUniform {
  std::string name;
  int location = -1;
  unsigned type = 0;   // GL_FLOAT, GL_FLOAT_VEC3, ...
  int size = 0;        // array size
};

struct ProgramState {
  unsigned id = 0;
  std::string vertKey, fragKey;
  bool ok = false;
  std::string error;
  std::vector<ShaderUniform> uniforms;   // reflection (active uniforms)
  std::vector<std::string> deps;         // resolved include files (for reload)
  std::vector<std::string> missingDeps;  // includes that could not be found
  std::map<std::string, int> locs;       // name -> location cache
  std::filesystem::file_time_type mtime_;      // last successful compile time
  std::filesystem::file_time_type lastAttempt_;  // last (re)compile attempt (retry throttle)

  int loc(const char* name) {
    auto it = locs.find(name);
    if (it != locs.end()) return it->second;
    const int l = ::glGetUniformLocation(id, name);
    locs[name] = l;
    return l;
  }
  void clearLocs() { locs.clear(); }
};

/** stable handle to a manager-owned program; survives hot reloads */
class ProgramRef {
public:
  std::shared_ptr<ProgramState> state;

  bool ok() const { return state && state->id; }
  unsigned id() const { return state ? state->id : 0; }

  void use() const {
    if (state && state->id) ::glUseProgram(state->id);
  }
  int loc(const char* name) const { return state ? state->loc(name) : -1; }

  void set1f(const char* name, float v) { const int l = loc(name); if (l >= 0) ::glUniform1f(l, v); }
  void set2f(const char* name, float x, float y) { const int l = loc(name); if (l >= 0) ::glUniform2f(l, x, y); }
  void set3f(const char* name, float x, float y, float z) { const int l = loc(name); if (l >= 0) ::glUniform3f(l, x, y, z); }
  void set4f(const char* name, float x, float y, float z, float w) { const int l = loc(name); if (l >= 0) ::glUniform4f(l, x, y, z, w); }
  void set1i(const char* name, int v) { const int l = loc(name); if (l >= 0) ::glUniform1i(l, v); }
  void setVec2(const char* name, float x, float y) { set2f(name, x, y); }
  void setVec3(const char* name, float x, float y, float z) { set3f(name, x, y, z); }
  void setVec3(const char* name, const float* v) { const int l = loc(name); if (l >= 0) ::glUniform3fv(l, 1, v); }
  void setVec4(const char* name, const float* v) { const int l = loc(name); if (l >= 0) ::glUniform4fv(l, 1, v); }
  void setMat4(const char* name, const float* m) { const int l = loc(name); if (l >= 0) ::glUniformMatrix4fv(l, 1, GL_FALSE, m); }
};

class ShaderManager {
public:
  /** get (compile + cache) a program; throws std::runtime_error on failure */
  ProgramRef get(const std::string& vert, const std::string& frag);

  /** recompile every cached program whose source or include changed;
   *  returns how many recompiled (0 = nothing changed). A failed recompile
   *  keeps the previous program live AND retries on the next file change -
   *  a broken shader never permanently disables hot reload for a program;
   *  retries are throttled to ~1/s so a persistently broken (or deleted)
   *  shader can't flood the log at frame rate. */
  int pollHotReload(const std::vector<std::string>& changedFiles);
  int reloadAll();

  std::vector<std::string> programKeys() const;
  std::shared_ptr<ProgramState> state(const std::string& key) const;

  /** paths that changed since the last poll (used by the DemoApp to trigger
   *  asset reloads too) */
  const std::vector<std::string>& lastChanged() const { return lastChanged_; }

  void setShaderDir(const std::string& dir) { shaderDir_ = dir; }

private:
  std::map<std::string, std::shared_ptr<ProgramState>> programs_;
  std::string shaderDir_;
  std::vector<std::string> lastChanged_;

  static std::string key(const std::string& v, const std::string& f) { return v + "\x1f" + f; }
  /** resolve includes + ES->desktop conversion, recording dependencies */
  static std::string resolveSource(const std::string& path, const std::string& shaderDir,
                                   std::vector<std::string>& deps,
                                   std::vector<std::string>& missing, int depth);
  bool compileProgram(const std::shared_ptr<ProgramState>& st);
  static std::string readFile(const std::string& path);
  bool isDirty(const std::shared_ptr<ProgramState>& st) const;
};

}  // namespace ns
