#include "engine/shader.hpp"
#include "engine/gl.hpp"
#include "engine/paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ns {

// ---------------------------------------------------------------------------
// preprocessing: #include <common>, #version 300 es -> 330 core, strip ES
// precision statements. The shader bodies themselves are untouched, so the
// native build renders the exact same GLSL as the web build.
// ---------------------------------------------------------------------------
static std::string readFile(const std::string& path, const std::string& label) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("shader file not found: " + label + " (" + path + ")");
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/** directory the shaders live in: env override, else the baked source-tree
 *  path if present, else exe-dir/shaders, else ./shaders (packaged builds) */
static std::string shaderDir() {
  return resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders");
}

std::string preprocessShaderSource(const std::string& src, const std::string& pathLabel) {
  std::istringstream in(src);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    // strip the ES precision statements (desktop GLSL 330 has no precision qualifiers)
    if (line.find("precision ") == 0 && line.find(';') != std::string::npos) continue;
    // resolve #include <common> -> the shared library
    if (line.find("#include <common>") != std::string::npos) {
      const std::string common = preprocessShaderSource(readFile(shaderDir() + "/common.glsl", "common.glsl"), "common.glsl");
      out << common << "\n";
      continue;
    }
    // rewrite the ES version line for desktop GLSL
    if (line.find("#version 300 es") != std::string::npos) {
      out << "#version 330 core\n";
      continue;
    }
    out << line << "\n";
  }
  (void)pathLabel;
  return out.str();
}

unsigned Shader::compileStage(unsigned type, const std::string& src, const std::string& label) {
  const unsigned sh = ::glCreateShader(type);
  if (!sh) throw std::runtime_error("createShader failed (" + label + ")");
  const char* s = src.c_str();
  ::glShaderSource(sh, 1, &s, nullptr);
  ::glCompileShader(sh);
  int status = 0;
  ::glGetShaderiv(sh, ::gl::COMPILE_STATUS, &status);
  if (!status) {
    char log[4096];
    int len = 0;
    ::glGetShaderInfoLog(sh, sizeof(log), &len, log);
    ::glDeleteShader(sh);
    std::string msg = "Shader compile error (" + label + "):\n" + std::string(log, len > 0 ? len : 0) + "\n--- source ---\n" + src;
    throw std::runtime_error(msg);
  }
  return sh;
}

Shader::Shader(const std::string& vertFile, const std::string& fragFile) {
  // a file argument that is itself an existing path (an absolute path from
  // the editor's asset-browser drop of a shader living OUTSIDE the shader
  // dir) reads directly instead of concatenating onto the shader dir
  std::string vpath = shaderDir() + "/" + vertFile;
  std::string fpath = shaderDir() + "/" + fragFile;
  if (!std::filesystem::exists(fpath) && std::filesystem::exists(fragFile))
    fpath = fragFile;
  if (!std::filesystem::exists(vpath) && std::filesystem::exists(vertFile))
    vpath = vertFile;
  const std::string vertSrc =
      preprocessShaderSource(readFile(vpath, vertFile), vertFile);
  const std::string fragSrc =
      preprocessShaderSource(readFile(fpath, fragFile), fragFile);
  const unsigned vs = compileStage(::gl::VERTEX_SHADER, vertSrc, vertFile);
  const unsigned fs = compileStage(::gl::FRAGMENT_SHADER, fragSrc, fragFile);
  program_ = ::glCreateProgram();
  ::glAttachShader(program_, vs);
  ::glAttachShader(program_, fs);
  ::glLinkProgram(program_);
  ::glDeleteShader(vs);
  ::glDeleteShader(fs);
  int status = 0;
  ::glGetProgramiv(program_, ::gl::LINK_STATUS, &status);
  if (!status) {
    char log[4096];
    int len = 0;
    ::glGetProgramInfoLog(program_, sizeof(log), &len, log);
    std::string msg = "Program link error (" + vertFile + " + " + fragFile + "):\n" + std::string(log, len > 0 ? len : 0);
    ::glDeleteProgram(program_);
    program_ = 0;
    throw std::runtime_error(msg);
  }
  // bind the shared NullBlock (when present) to slot 0
  const unsigned blockIdx = ::glGetUniformBlockIndex(program_, "NullBlock");
  if (blockIdx != ::gl::INVALID_INDEX) ::glUniformBlockBinding(program_, blockIdx, 0);
}

Shader::~Shader() {
  if (program_) ::glDeleteProgram(program_);
}

int Shader::loc(const char* name) const {
  auto it = cache_.find(name);
  if (it != cache_.end()) return it->second;
  const int l = ::glGetUniformLocation(program_, name);
  cache_[name] = l;
  return l;
}

void Shader::set1f(const char* name, float v) { const int l = loc(name); if (l >= 0) ::glUniform1f(l, v); }
void Shader::set2f(const char* name, float x, float y) { const int l = loc(name); if (l >= 0) ::glUniform2f(l, x, y); }
void Shader::set3f(const char* name, float x, float y, float z) { const int l = loc(name); if (l >= 0) ::glUniform3f(l, x, y, z); }
void Shader::set1i(const char* name, int v) { const int l = loc(name); if (l >= 0) ::glUniform1i(l, v); }
void Shader::setVec2(const char* name, const float* v) { const int l = loc(name); if (l >= 0) ::glUniform2fv(l, 1, v); }
void Shader::setVec3(const char* name, const float* v) { const int l = loc(name); if (l >= 0) ::glUniform3fv(l, 1, v); }
void Shader::setVec2(const char* name, float x, float y) { const int l = loc(name); if (l >= 0) ::glUniform2f(l, x, y); }
void Shader::setVec3(const char* name, float x, float y, float z) { const int l = loc(name); if (l >= 0) ::glUniform3f(l, x, y, z); }

}  // namespace ns
