// ---------------------------------------------------------------------------
// Dev-only shader preflight (port of src/engine/shadercheck.ts).
// Compiles every shader stage up front and reports GLSL errors to stderr
// BEFORE the show reaches the scene that uses them.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace ns {

struct FontMetrics;
class Texture;

struct ShaderCheckResult {
  int total = 0;
  int ok = 0;
  int failed = 0;
  std::vector<std::string> failedFiles;
};

/** compile every stage in the shaders dir; returns the aggregate (never throws) */
ShaderCheckResult compileAllShaders();

/** render every self-contained content .frag (procedural fullscreen effects
 *  with no sampler uniforms - the ones the probe's standard uniform set can
 *  judge) into an offscreen target at several instants and classify the
 *  output via the shared RenderProbe: flags shaders that never drew, render
 *  one uniform color, or collapsed to near-black. Pipeline-pass shaders that
 *  depend on bound textures are skipped. Returns the aggregate (never
 *  throws). Activated with `--check-shaders --render`. */
ShaderCheckResult checkShadersRender();

/** compile a single .vert/.frag file for the preflight; returns true on success */
bool checkShaderFile(const std::string& file);

/** dev preflight: read back the built font atlas and assert it is stored
 *  UNFLIPPED (texture v=0 = atlas top = glyph row 0), the convention the text
 *  mesh UVs rely on. Also asserts TextMesh::glyphUVs() keeps the v0 > v1
 *  ordering (top of a glyph at the smaller v). A flipped upload or a reverted
 *  `1 - x` UV formula fails here instead of surfacing as faint garbage text
 *  quads in the show. Returns true when the atlas matches the rasterizer. */
bool checkFontAtlasOrientation(const Texture& tex, const FontMetrics& fm);

}  // namespace ns
