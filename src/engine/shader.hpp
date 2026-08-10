// ---------------------------------------------------------------------------
// Shader program compilation + uniform helpers (port of src/engine/shader.ts).
// Loads GLSL from disk, resolves #include <common>, converts the GLSL ES 3.00
// sources (copied verbatim from the web build) to desktop GLSL 330, links and
// binds the shared NullBlock UBO to slot 0.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include <string>
#include <unordered_map>

namespace ns {

/** resolve #include directives + ES->desktop conversion for one source */
std::string preprocessShaderSource(const std::string& src, const std::string& pathLabel);

class Shader {
public:
  /** load from the given .vert / .frag files (relative to the shader dir) */
  Shader(const std::string& vertFile, const std::string& fragFile);
  ~Shader();
  Shader(const Shader&) = delete;
  Shader& operator=(const Shader&) = delete;

  unsigned id() const { return program_; }
  void use() const { ::glUseProgram(program_); }

  void set1f(const char* name, float v);
  void set2f(const char* name, float x, float y);
  void set3f(const char* name, float x, float y, float z);
  void set4f(const char* name, float x, float y, float z, float w);
  void set1i(const char* name, int v);
  void setVec2(const char* name, const float* v);
  void setVec3(const char* name, const float* v);
  /** scalar forms (no temporary arrays needed by callers) */
  void setVec2(const char* name, float x, float y);
  void setVec3(const char* name, float x, float y, float z);

  /** compile a single stage (for the preflight check); throws on error */
  static unsigned compileStage(unsigned type, const std::string& src, const std::string& label);

private:
  int loc(const char* name) const;
  unsigned program_ = 0;
  mutable std::unordered_map<std::string, int> cache_;
};

}  // namespace ns
