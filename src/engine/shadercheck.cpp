// ---------------------------------------------------------------------------
// Dev-only shader preflight (port of src/engine/shadercheck.ts).
// Compiles every .vert/.frag stage in the shaders dir up front and reports
// GLSL errors to stderr BEFORE the show reaches the scene that uses them,
// instead of crashing mid-production. Type errors like assigning the float
// hash21() to a vec2 show up here immediately, named by file.
//
// Activation: --check-shaders on the command line.
//
// The check is read-only: it never links programs and never throws, so the
// demo continues to run even with a broken shader. Every broken file is
// reported up front with a clean label instead of one confusing crash.
//
// Scope note: this validates *stage* compilation only. Vertex/fragment
// varying mismatches surface at link time, which is already covered because
// every effect constructs its Shader (compile + link) during boot-time init.
// common.glsl is a library without main() and is intentionally excluded by
// the .vert/.frag extension filter - it is injected via #include.
// ---------------------------------------------------------------------------
#include "engine/shadercheck.hpp"
#include "engine/assets.hpp"
#include "engine/font8x8.hpp"
#include "engine/gl.hpp"
#include "engine/paths.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include "engine/texture.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ns {

static std::string shaderDir() {
  return resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders");
}

static std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/** compile a single .vert/.frag stage for the preflight; true on success */
bool checkShaderFile(const std::string& file) {
  const std::string src = readFile(shaderDir() + "/" + file);
  if (src.empty()) {
    std::fprintf(stderr, "[SHADER-PRECHECK] FAILED %s (unreadable)\n", file.c_str());
    return false;
  }
  const bool isVert = file.size() > 5 && file.compare(file.size() - 5, 5, ".vert") == 0;
  const unsigned type = isVert ? ::gl::VERTEX_SHADER : ::gl::FRAGMENT_SHADER;
  try {
    const std::string resolved = preprocessShaderSource(src, file);
    const unsigned sh = Shader::compileStage(type, resolved, file);
    ::glDeleteShader(sh);
    return true;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "[SHADER-PRECHECK] FAILED %s\n%s\n", file.c_str(), ex.what());
    return false;
  }
}

ShaderCheckResult compileAllShaders() {
  ShaderCheckResult r;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(shaderDir(), ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    const bool isShader = name.size() > 5 &&
      (name.compare(name.size() - 5, 5, ".frag") == 0 ||
       name.compare(name.size() - 5, 5, ".vert") == 0);
    if (!isShader) continue;
    r.total++;
    if (checkShaderFile(name)) r.ok++;
    else r.failedFiles.push_back(name);
  }
  r.failed = (int)r.failedFiles.size();

  if (r.total == 0) {
    std::fprintf(stderr, "[SHADER-PRECHECK] no shader sources found in %s\n", shaderDir().c_str());
  } else if (r.failed == 0) {
    std::printf("[SHADER-PRECHECK] %d/%d shader stages compiled clean\n", r.ok, r.total);
  } else {
    std::fprintf(stderr, "[SHADER-PRECHECK] %d of %d shader stages FAILED (%d ok)\n",
                 r.failed, r.total, r.ok);
  }
  return r;
}

/** read the font texture back into CPU memory via an FBO (glGetTexImage is
 *  not in the project's minimal loader; FBO+glReadPixels is core 3.3). */
static bool readbackTexture(const Texture& tex, std::vector<unsigned char>& out) {
  out.assign((size_t)tex.w * tex.h * 4, 0);
  GLint prevFbo = 0;
  GLint prevPackAlign = 4;
  ::glGetIntegerv(::gl::FRAMEBUFFER_BINDING, &prevFbo);
  ::glGetIntegerv(::gl::PACK_ALIGNMENT, &prevPackAlign);
  unsigned fbo = 0;
  ::glGenFramebuffers(1, &fbo);
  ::glBindFramebuffer(::gl::FRAMEBUFFER, fbo);
  ::glFramebufferTexture2D(::gl::FRAMEBUFFER, ::gl::COLOR_ATTACHMENT0, ::gl::TEXTURE_2D, tex.tex, 0);
  const bool ok = ::glCheckFramebufferStatus(::gl::FRAMEBUFFER) == ::gl::FRAMEBUFFER_COMPLETE;
  if (ok) {
    ::glPixelStorei(::gl::PACK_ALIGNMENT, 1);  // tight RGBA rows
    ::glReadPixels(0, 0, tex.w, tex.h, ::gl::RGBA, ::gl::UNSIGNED_BYTE, out.data());
    ::glPixelStorei(::gl::PACK_ALIGNMENT, prevPackAlign);  // restore global state
  }
  ::glBindFramebuffer(::gl::FRAMEBUFFER, (GLuint)prevFbo);
  ::glDeleteFramebuffers(1, &fbo);
  return ok;
}

bool checkFontAtlasOrientation(const Texture& tex, const FontMetrics& fm) {
  // the orientation check compares against the embedded 8x8 rasterizer; a
  // TrueType atlas (e.g. assets/fonts) has no reference pixels and a
  // different cell size, so skip it instead of failing the size check.
  if (fm.cellW != 8 || fm.cellH != 8 || fm.atlasW != 128 || fm.atlasH != 64) {
    std::printf("[FONT-PRECHECK] skipped (TrueType atlas %dx%d, %dx%dpx cells)\n",
                fm.atlasW, fm.atlasH, fm.cellW, fm.cellH);
    return true;
  }
  const std::vector<unsigned char> expected = rasterizeFontAtlasPixels();
  std::vector<unsigned char> actual;
  if (!readbackTexture(tex, actual)) {
    std::fprintf(stderr, "[FONT-PRECHECK] FAILED: could not read the font atlas back (FBO incomplete)\n");
    return false;
  }
  const size_t expectedBytes = (size_t)fm.atlasW * fm.atlasH * 4;
  if (expected.size() != expectedBytes || actual.size() != expectedBytes) {
    std::fprintf(stderr, "[FONT-PRECHECK] FAILED: atlas size mismatch (texture %dx%d vs metrics %dx%d)\n",
                 tex.w, tex.h, fm.atlasW, fm.atlasH);
    return false;
  }

  // 1) the texture must match the rasterizer row-for-row (glyph row 0 at
  //    texture row 0 = v=0). A flipped upload swaps the rows and fails here.
  int badRow = -1;
  for (int y = 0; y < fm.atlasH; y++) {
    const size_t row = (size_t)y * fm.atlasW * 4;
    if (std::memcmp(actual.data() + row, expected.data() + row, (size_t)fm.atlasW * 4) != 0) {
      badRow = y;
      break;
    }
  }
  if (badRow >= 0) {
    const bool flipped =
      std::memcmp(actual.data(), expected.data() + (size_t)(fm.atlasH - 1) * fm.atlasW * 4,
                  (size_t)fm.atlasW * 4) == 0;
    std::fprintf(stderr,
                 "[FONT-PRECHECK] FAILED: font atlas row %d/%d differs from the rasterizer%s\n"
                 "  Text UVs assume v=0 = glyph row 0 (unflipped upload). A flipped\n"
                 "  upload (GL_UNPACK_FLIP_Y) or a reverted `1 - x` UV formula makes\n"
                 "  every text quad render as a faint garbage rectangle.\n",
                 badRow, fm.atlasH, flipped ? " - looks Y-FLIPPED (v=0 = last glyph row)" : "");
    return false;
  }

  // 2) the shared UV helper must keep the unflipped ordering: a glyph's top
  //    (v1) samples a SMALLER v than its bottom (v0), because v=0 is the
  //    atlas top. The old `1 - x` math produced v0 < v1 and sampled the wrong
  //    rows - assert a spread of codes to catch that regression.
  static const int PROBE_CODES[] = { 'A', 'S', '0', '!', 'x', '\x7f' };
  for (int c : PROBE_CODES) {
    float u0, v0, u1, v1;
    TextMesh::glyphUVs(c, fm, u0, v0, u1, v1);
    if (!(v1 < v0)) {
      std::fprintf(stderr,
                   "[FONT-PRECHECK] FAILED: TextMesh::glyphUVs('%c') ordering v1=%f >= v0=%f - "
                   "the flipped UV convention is back\n", c, v1, v0);
      return false;
    }
  }

  // 3) absolute anchor independent of the rasterizer: the 'A' glyph must be
  //    found at the exact texel FONT8X8 says it lives at. 'A' = 0x41 sits in
  //    cell row cy=4 -> texture row 32 (v=0.5), and its top scanline
  //    "..##...." puts lit pixels at cell cols 2-3 -> atlas col 1*8+2 = 10.
  //    This pins the convention (v=0 = glyph row 0) against the embedded font
  //    itself, so even a both-flipped rasterizer+texture regression fails
  //    here instead of slipping through with a green check.
  {
    const int code = 'A';
    const int cell = code % (fm.cols * fm.rows);
    const int cx = cell % fm.cols;
    const int cy = cell / fm.cols;
    const int gi = code - 32;
    int litCol = -1;
    if (gi >= 0 && gi < (int)FONT8X8.size()) {
      for (int x = 0; x < fm.cellW && litCol < 0; x++) {
        if (FONT8X8[(size_t)gi][0][x] == '#') litCol = x;
      }
    }
    if (litCol < 0) {
      std::fprintf(stderr, "[FONT-PRECHECK] FAILED: could not resolve the 'A' anchor glyph\n");
      return false;
    }
    const int texX = cx * fm.cellW + litCol;
    const int texY = cy * fm.cellH;  // 'A' top row in unflipped texture space
    const unsigned char* t = actual.data() + ((size_t)texY * fm.atlasW + texX) * 4;
    if (t[3] < 200) {
      std::fprintf(stderr,
                   "[FONT-PRECHECK] FAILED: 'A' anchor missing at texel (%d,%d) (alpha=%d) - "
                   "the atlas is NOT stored as v=0 = glyph row 0 (rasterizer and/or "
                   "upload flipped)\n",
                   texX, texY, t[3]);
      return false;
    }
  }

  std::printf("[FONT-PRECHECK] font atlas orientation OK (%dx%d, %d rows match, v=0 = glyph row 0)\n",
              fm.atlasW, fm.atlasH, fm.atlasH);
  return true;
}

}  // namespace ns
