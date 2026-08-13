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
#include "engine/renderer.hpp"
#include "engine/renderprobe.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include "engine/texture.hpp"
#include "engine/ubo.hpp"

#include <array>
#include <cmath>
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

/** bind the standard runtime uniform set used by authored content shaders
 *  (Shader::set* no-ops on missing locations, so one binder covers both the
 *  engine's standalone per-effect convention - uRes/uColA/uColB - and the
 *  AI-workstation set). */
static void bindProbeUniforms(Shader& sh, float t, int w, int h) {
  const float bpm = 128.0f;
  const float beat = t * bpm / 60.0f;
  const float bar = beat / 4.0f;
  const float phase = beat - std::floor(beat);
  sh.set2f("uRes", (float)w, (float)h);
  sh.set2f("uResolution", (float)w, (float)h);
  sh.set1f("uTime", t);
  sh.set1f("uBPM", bpm); sh.set1f("uBeat", beat); sh.set1f("uBar", bar); sh.set1f("uBeatPhase", phase);
  sh.set1f("uAudioLevel", 0.1f); sh.set1f("uBass", 0.1f); sh.set1f("uMid", 0.1f); sh.set1f("uTreble", 0.1f);
  sh.set1f("uKick", 0.05f); sh.set1f("uSnare", 0.05f);
  sh.set3f("uColA", 0.1f, 0.85f, 1.0f);
  sh.set3f("uColB", 0.9f, 0.1f, 0.8f);
  sh.set4f("uColor", 0.0f, 0.85f, 1.0f, 1.0f);
  sh.set4f("uColor2", 0.8f, 0.05f, 0.75f, 1.0f);
  sh.set1f("uIntensity", 1.0f); sh.set1f("uSpeed", 1.0f); sh.set1f("uScale", 1.0f);
  sh.set1f("uFlash", 0.1f); sh.set1f("uMode", 0.0f); sh.set1f("uPulse", 0.5f);
  // per-effect gate uniforms seen in the shipped content set - bound to
  // benign "everything enabled" defaults so a shader is not misread as flat
  // because its section/diagnostic envelope accidentally gated it to solid.
  sh.set1f("uDiag", 1.0f); sh.set1f("uAlpha", 1.0f); sh.set1f("uSeed", 1.0f);
  sh.set1f("uWake", 1.0f); sh.set1f("uFlow", 1.0f); sh.set1f("uBurst", 1.0f);
  sh.set1f("uStream", 0.5f); sh.set1f("uQuiet", 1.0f); sh.set1f("uBuild", 1.0f);
  sh.set1f("uDim", 1.0f); sh.set1f("uSunScale", 1.0f); sh.set1f("uLanding", 1.0f);
  sh.set1f("uLandingT", 1.0f); sh.set1f("uHigh", 0.1f); sh.set1f("uVolume", 0.1f);
  sh.set2f("uSceneRes", (float)w, (float)h);
  sh.set2f("uParallax", 0.0f, 0.0f);
  sh.set1f("uTimeDelta", 0.016f); sh.set1i("uFrame", 1);
  sh.set4f("uMouse", 0.0f, 0.0f, 0.0f, 0.0f);
  sh.set4f("uDate", 2026.0f, 8.0f, 10.0f, 0.0f);
  sh.set4f("uChannelTime", t, t, t, t);
  sh.set1f("uDeltaTime", 1.0f / 60.0f);
  sh.set1f("uProgress", 0.5f);
}

/** plausible values for the shared NullBlock UBO (shaders/../common.glsl
 *  `Null` layout, offsets in engine/ubo.hpp). Written per sampled instant;
 *  the Shader constructor already bound the block to slot 0. */
static void writeProbeSharedBlock(SharedBlock& shared, float t, int w, int h) {
  const float bpm = 128.0f;
  const float beat = t * bpm / 60.0f;
  float* d = shared.data.data();
  d[OFF_URES] = (float)w;
  d[OFF_URES + 1] = (float)h;
  d[OFF_UTIME] = t;
  d[OFF_UBEAT] = beat;
  d[OFF_UPULSE] = 0.5f;
  d[OFF_UINTENSITY] = 1.0f;
  d[OFF_USECTIONLOCAL] = 0.5f;
  d[OFF_UBASS] = 0.1f;
  d[OFF_UONSET] = 0.05f;
  d[OFF_UANTICIPATION] = 0.0f;
  d[OFF_UEXITRAMP] = 0.0f;
  d[OFF_UMUSICHUE] = 0.6f;
  d[OFF_UMUSICHUE2] = 0.4f;
  d[OFF_UBARPHASE] = beat - std::floor(beat);
  d[OFF_UBAR] = 1.0f;
  d[OFF_UASSEMBLY] = 0.5f;
  d[OFF_USECTIONDUR] = 60.0f;
  d[OFF_USECBAR] = 60.0f * 4.0f / bpm;  // seconds per bar at 128 bpm

  // A plausible "camera in front of the scene" (pos + basis + view + proj) so
  // the 3D content shaders (cathedral, raymarch, neuralnet, ...) render
  // something spatial instead of a degenerate frame from zeroed matrices.
  const float eye[3] = {0.0f, 1.3f, 3.6f};
  const float at[3] = {0.0f, 0.2f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  float fwd[3], right[3], camUp[3];
  for (int i = 0; i < 3; ++i) fwd[i] = at[i] - eye[i];
  const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  for (int i = 0; i < 3; ++i) fwd[i] /= fl;
  right[0] = up[1] * fwd[2] - up[2] * fwd[1];
  right[1] = up[2] * fwd[0] - up[0] * fwd[2];
  right[2] = up[0] * fwd[1] - up[1] * fwd[0];
  const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (int i = 0; i < 3; ++i) right[i] /= rl;
  camUp[0] = fwd[1] * right[2] - fwd[2] * right[1];
  camUp[1] = fwd[2] * right[0] - fwd[0] * right[2];
  camUp[2] = fwd[0] * right[1] - fwd[1] * right[0];
  d[OFF_UCAMPOS + 0] = eye[0]; d[OFF_UCAMPOS + 1] = eye[1]; d[OFF_UCAMPOS + 2] = eye[2];
  d[OFF_UFOVTAN] = std::tan(0.5f * 1.0472f);  // ~60 deg vertical fov
  for (int c = 0; c < 3; ++c) {  // std140 mat3 columns: right, up, -fwd
    d[OFF_UCAMROT + c * 4 + 0] = right[c];
    d[OFF_UCAMROT + c * 4 + 1] = camUp[c];
    d[OFF_UCAMROT + c * 4 + 2] = -fwd[c];
  }
  float view[16];  // column-major lookAt
  view[0] = right[0]; view[1] = right[1]; view[2] = right[2]; view[3] = 0;
  view[4] = camUp[0]; view[5] = camUp[1]; view[6] = camUp[2]; view[7] = 0;
  view[8] = -fwd[0]; view[9] = -fwd[1]; view[10] = -fwd[2]; view[11] = 0;
  view[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
  view[13] = -(camUp[0] * eye[0] + camUp[1] * eye[1] + camUp[2] * eye[2]);
  view[14] = -(-fwd[0] * eye[0] + -fwd[1] * eye[1] + -fwd[2] * eye[2]);
  view[15] = 1;
  for (int i = 0; i < 16; ++i) d[OFF_UVIEW + i] = view[i];
  const float f = 1.0f / std::tan(0.5f * 1.0472f);
  const float zNear = 0.1f, zFar = 100.0f;
  float proj[16];
  proj[0] = f; proj[1] = 0; proj[2] = 0; proj[3] = 0;
  proj[4] = 0; proj[5] = f; proj[6] = 0; proj[7] = 0;
  proj[8] = 0; proj[9] = 0; proj[10] = (zFar + zNear) / (zNear - zFar); proj[11] = -1;
  proj[12] = 0; proj[13] = 0; proj[14] = (2 * zFar * zNear) / (zNear - zFar); proj[15] = 0;
  for (int i = 0; i < 16; ++i) d[OFF_UPROJ + i] = proj[i];

  shared.dirty = true;
  shared.commit();
}

ShaderCheckResult checkShadersRender() {
  ShaderCheckResult r;
  // 16:9 like the real show - several content shaders (intro_graph, ...) place
  // their geometry at screen positions that only exist at the engine aspect.
  constexpr int kW = 256, kH = 144;
  Renderer renderer;
  renderer.resize(kW, kH);
  Mesh& tri = renderer.fsTriangle;
  SharedBlock shared;
  int skipped = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(shaderDir(), ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    const bool isFrag = name.size() > 5 && name.compare(name.size() - 5, 5, ".frag") == 0;
    if (!isFrag) continue;
    const std::string src = readFile(shaderDir() + "/" + name);
    // The probe binds only the standard runtime uniforms + the shared
    // NullBlock. A shader that declares sampler uniforms depends on
    // pipeline-bound textures (post passes, lit/text vertex pairs) and cannot
    // be judged standalone; diag_* files are intentionally-solid test
    // patterns; a foreign `in vec` varying means a custom vertex shader.
    bool foreignVarying = false;
    {
      size_t p = 0;
      while ((p = src.find("in vec", p)) != std::string::npos) {
        const size_t nl = src.find('\n', p);
        const size_t span = nl == std::string::npos ? src.size() - p : nl - p;
        if (src.compare(p, span, "in vec2 vUV") != 0 && src.substr(p, span).find("vUV") == std::string::npos) {
          foreignVarying = true;
          break;
        }
        p += 6;
      }
    }
    // Intentional solid frames are CORRECT to render as one color: fade is a
    // black transition overlay, flash is a full-screen strobe overlay, and
    // diag_* are diagnostic test patterns. landscape.frag only shows variance
    // from a moving camera + live audio (its terrain is a flat plane at the
    // probe's frozen viewpoint) - it is verified by the production checks.
    if (name == "fade.frag" || name == "flash.frag" || name == "landscape.frag" ||
        name.rfind("diag_", 0) == 0) {
      skipped++;
      continue;
    }
    if (src.find("sampler") != std::string::npos || foreignVarying) {
      skipped++;
      continue;
    }
    r.total++;
    try {
      Shader sh("fullscreen.vert", name);
      const RenderProbeResult probe = probeRender(kW, kH, {0.37f, 1.13f}, [&](float t) {
        writeProbeSharedBlock(shared, t, kW, kH);
        sh.use();
        bindProbeUniforms(sh, t, kW, kH);
        tri.draw(3);
      });
      if (probe.degenerate()) {
        r.failed++;
        r.failedFiles.push_back(name + ": " + probe.diagnosis());
        std::fprintf(stderr, "[SHADER-RENDER] FAIL %s: %s\n", name.c_str(), probe.diagnosis().c_str());
      } else {
        r.ok++;
        std::printf("[SHADER-RENDER] ok %s (max channel spread %d)\n", name.c_str(), probe.maxSpread);
      }
    } catch (const std::exception& e) {
      r.total++;
      r.failed++;
      r.failedFiles.push_back(name + ": " + e.what());
      std::fprintf(stderr, "[SHADER-RENDER] FAIL %s: %s\n", name.c_str(), e.what());
    }
  }
  if (r.total == 0 && skipped == 0) {
    std::fprintf(stderr, "[SHADER-RENDER] no .frag sources found in %s\n", shaderDir().c_str());
  } else if (r.failed == 0) {
    std::fprintf(stderr, "[SHADER-RENDER] %d/%d content shaders render with variance (%d pipeline-pass shaders skipped)\n",
                 r.ok, r.total, skipped);
  } else {
    std::fprintf(stderr, "[SHADER-RENDER] %d of %d content shaders FAILED (%d ok, %d skipped)\n",
                 r.failed, r.total, r.ok, skipped);
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
