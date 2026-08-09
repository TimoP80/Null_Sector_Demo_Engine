#include "app/shadermanager.hpp"
#include "engine/paths.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ns {

static constexpr int kMaxIncludeDepth = 16;

std::string ShaderManager::readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string ShaderManager::resolveSource(const std::string& path, const std::string& shaderDir,
                                         std::vector<std::string>& deps,
                                         std::vector<std::string>& missing, int depth) {
  const std::string src = readFile(path);
  // record the dependency before the read-EMPTINESS check: a missing file
  // must stay in the dep list so isDirty() can see it reappear
  // (deleted-then-restored shaders reload live). If we only recorded on
  // success, a failed compile would drop the file from deps and hot reload
  // would never detect its return.
  if (std::find(deps.begin(), deps.end(), path) == deps.end()) deps.push_back(path);
  if (src.empty()) {
    missing.push_back(path);
    return "";
  }

  std::istringstream in(src);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    // strip ES precision statements (desktop GLSL 330 has no precision qualifiers)
    if (line.find("precision ") == 0 && line.find(';') != std::string::npos) continue;
    // rewrite the ES version line for desktop GLSL
    if (line.find("#version 300 es") != std::string::npos) {
      out << "#version 330 core\n";
      continue;
    }
    // generic include: #include <common> or #include "file.glsl"
    const size_t p = line.find("#include");
    if (p != std::string::npos) {
      const size_t a = line.find_first_of("<\"", p);
      const size_t b = line.find_first_of(">\"", a + 1);
      if (a != std::string::npos && b != std::string::npos) {
        const std::string inc = line.substr(a + 1, b - a - 1);
        // Resolve the include: try the literal name first (e.g. `#include
        // "noise.glsl"`), then append the .glsl extension so `#include
        // <common>` finds common.glsl (mirrors the engine Shader class).
        std::string incPath = shaderDir + "/" + inc;
        if (!std::filesystem::exists(incPath)) incPath = shaderDir + "/" + inc + ".glsl";
        if (depth < kMaxIncludeDepth) {
          out << resolveSource(incPath, shaderDir, deps, missing, depth + 1);
        } else {
          Log::warn("SHADER", "include depth exceeded at " + inc + " (cycle?)");
        }
        continue;
      }
    }
    out << line << "\n";
  }
  return out.str();
}

bool ShaderManager::isDirty(const std::shared_ptr<ProgramState>& st) const {
  const std::string dir = shaderDir_.empty() ? resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders") : shaderDir_;
  std::error_code ec;
  for (const auto& dep : st->deps) {
    if (!std::filesystem::exists(dep, ec)) return true;
    const auto mt = std::filesystem::last_write_time(dep, ec);
    if (mt > st->mtime_) return true;
  }
  return false;
}

bool ShaderManager::compileProgram(const std::shared_ptr<ProgramState>& st) {
  const std::string dir = shaderDir_.empty() ? resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders") : shaderDir_;
  std::string vpath = dir + "/" + st->vertKey;
  std::string fpath = dir + "/" + st->fragKey;
  // a key that is itself an existing file (an absolute path from the editor's
  // asset-browser drop of a shader living OUTSIDE the shader dir) resolves
  // directly instead of silently compiling an empty source
  if (!std::filesystem::exists(fpath) && std::filesystem::exists(st->fragKey))
    fpath = st->fragKey;
  if (!std::filesystem::exists(vpath) && std::filesystem::exists(st->vertKey))
    vpath = st->vertKey;

  std::vector<std::string> vDeps, fDeps, vMiss, fMiss;
  const std::string vsrc = resolveSource(vpath, dir, vDeps, vMiss, 0);
  const std::string fsrc = resolveSource(fpath, dir, fDeps, fMiss, 0);
  st->deps.clear();
  st->deps.insert(st->deps.end(), vDeps.begin(), vDeps.end());
  st->deps.insert(st->deps.end(), fDeps.begin(), fDeps.end());
  st->missingDeps.clear();
  st->missingDeps.insert(st->missingDeps.end(), vMiss.begin(), vMiss.end());
  st->missingDeps.insert(st->missingDeps.end(), fMiss.begin(), fMiss.end());

  const auto compileStage = [&](unsigned type, const std::string& src, const std::string& label) {
    const unsigned sh = ::glCreateShader(type);
    const char* c = src.c_str();
    ::glShaderSource(sh, 1, &c, nullptr);
    ::glCompileShader(sh);
    int status = 0;
    ::glGetShaderiv(sh, ::gl::COMPILE_STATUS, &status);
    if (!status) {
      char log[4096];
      int len = 0;
      ::glGetShaderInfoLog(sh, sizeof(log), &len, log);
      ::glDeleteShader(sh);
      throw std::runtime_error("compile error (" + label + "):\n" + std::string(log, len > 0 ? len : 0));
    }
    return sh;
  };

  try {
    // A missing source or include file must say so - compiling an empty
    // source yields a cryptic GLSL log ('undefined variable ...') that never
    // names the file (this is how the <common> include bug surfaced).
    if (!vMiss.empty()) {
      throw std::runtime_error("shader file not found: " + vMiss[0] +
                               " (vert '" + st->vertKey + "', searched " + dir +
                               (vMiss[0] != vpath ? ", missing include" : "") + ")");
    }
    if (!fMiss.empty()) {
      throw std::runtime_error("shader file not found: " + fMiss[0] +
                               " (frag '" + st->fragKey + "', searched " + dir +
                               (fMiss[0] != fpath ? ", missing include" : "") + ")");
    }
    const unsigned vs = compileStage(::gl::VERTEX_SHADER, vsrc, st->vertKey);
    const unsigned fs = compileStage(::gl::FRAGMENT_SHADER, fsrc, st->fragKey);
    const unsigned prog = ::glCreateProgram();
    ::glAttachShader(prog, vs);
    ::glAttachShader(prog, fs);
    ::glLinkProgram(prog);
    ::glDeleteShader(vs);
    ::glDeleteShader(fs);
    int status = 0;
    ::glGetProgramiv(prog, ::gl::LINK_STATUS, &status);
    if (!status) {
      char log[4096];
      int len = 0;
      ::glGetProgramInfoLog(prog, sizeof(log), &len, log);
      ::glDeleteProgram(prog);
      throw std::runtime_error("link error (" + st->vertKey + " + " + st->fragKey + "):\n" +
                               std::string(log, len > 0 ? len : 0));
    }
    // bind the shared NullBlock UBO to slot 0 when present
    const unsigned blockIdx = ::glGetUniformBlockIndex(prog, "NullBlock");
    if (blockIdx != ::gl::INVALID_INDEX) ::glUniformBlockBinding(prog, blockIdx, 0);

    // reflection: active uniforms (GL_ACTIVE_UNIFORMS = 0x8B86)
    std::vector<ShaderUniform> uniforms;
    int count = 0;
    ::glGetProgramiv(prog, (GLenum)0x8B86, &count);
    for (int i = 0; i < count; i++) {
      char name[256];
      GLsizei len = 0;
      GLint size = 0;
      GLenum type = 0;
      ::glGetActiveUniform(prog, (GLuint)i, sizeof(name), &len, &size, &type, name);
      if (len <= 0) continue;
      ShaderUniform u;
      u.name = std::string(name, (size_t)len);
      u.size = size;
      u.type = type;
      u.location = ::glGetUniformLocation(prog, name);
      uniforms.push_back(std::move(u));
    }
    std::sort(uniforms.begin(), uniforms.end(),
              [](const ShaderUniform& a, const ShaderUniform& b) { return a.location < b.location; });

    // swap in the new program
    if (st->id) ::glDeleteProgram(st->id);
    st->id = prog;
    st->uniforms = std::move(uniforms);
    st->clearLocs();
    st->error.clear();
    st->ok = true;
    std::error_code ec;
    st->mtime_ = std::filesystem::file_time_type::clock::now();
    return true;
  } catch (const std::exception& e) {
    st->error = e.what();
    st->ok = false;
    return false;
  }
}

ProgramRef ShaderManager::get(const std::string& vert, const std::string& frag) {
  const std::string k = key(vert, frag);
  auto it = programs_.find(k);
  if (it != programs_.end()) {
    ProgramRef r;
    r.state = it->second;
    if (!r.ok()) throw std::runtime_error("shader '" + vert + " + " + frag + "' previously failed: " + it->second->error);
    return r;
  }
  auto st = std::make_shared<ProgramState>();
  st->vertKey = vert;
  st->fragKey = frag;
  if (!compileProgram(st)) {
    throw std::runtime_error("shader compile failed (" + vert + " + " + frag + "):\n" + st->error);
  }
  programs_[k] = st;
  ProgramRef r;
  r.state = st;
  return r;
}

int ShaderManager::pollHotReload(const std::vector<std::string>& changedFiles) {
  lastChanged_ = changedFiles;
  int n = 0;
  for (auto& kv : programs_) {
    auto& st = kv.second;
    // NOTE: no st->ok gate here. A failed reload must NOT permanently disable
    // hot reload for the program (compileProgram sets ok=false on failure):
    // the file will be fixed and the next change has to retry. compileProgram
    // keeps st->id on failure, so the previous working program stays live -
    // the show never goes dark.
    if (isDirty(st)) {
      // throttle retry attempts: isDirty() short-circuits on a MISSING dep
      // (before the mtime check), so a deleted shader file would otherwise
      // fail + warn every poll (~60/s). One attempt per second max.
      const auto now = std::filesystem::file_time_type::clock::now();
      if (now - st->lastAttempt_ < std::chrono::seconds(1)) continue;
      st->lastAttempt_ = now;
      if (compileProgram(st)) {
        Log::info("SHADER", "hot-reloaded '" + st->vertKey + " + " + st->fragKey + "'");
        n++;
      } else {
        // mark the program permanently dirty (mtime = min): ANY restore is
        // retried, even one that preserves an older file timestamp (cp -p,
        // git checkout, archive extraction). The 1s throttle above prevents
        // per-frame attempts while it stays broken.
        st->mtime_ = std::filesystem::file_time_type::min();
        Log::warn("SHADER", "hot-reload failed for '" + st->vertKey + " + " + st->fragKey +
                                "' - keeping previous version:\n" + st->error);
      }
    }
  }
  return n;
}

int ShaderManager::reloadAll() {
  for (auto& kv : programs_) {
    kv.second->mtime_ = std::filesystem::file_time_type::clock::time_point::min();
  }
  int n = 0;
  for (auto& kv : programs_) {
    if (compileProgram(kv.second)) n++;
  }
  return n;
}

std::vector<std::string> ShaderManager::programKeys() const {
  std::vector<std::string> out;
  for (const auto& kv : programs_) out.push_back(kv.first);
  return out;
}

std::shared_ptr<ProgramState> ShaderManager::state(const std::string& k) const {
  auto it = programs_.find(k);
  return it != programs_.end() ? it->second : nullptr;
}

}  // namespace ns
