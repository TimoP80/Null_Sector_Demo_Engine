#include "engine/assets.hpp"
#include "engine/font8x8.hpp"
#include "engine/gl.hpp"
#include "engine/paths.hpp"
#include "framework/vfs/vfs.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace ns {

/** assets dir: env override, else the baked source-tree path if present,
 *  else exe-dir/assets, else ./assets (packaged builds) */
std::string assetDir() {
  return resolveRuntimeDir("NULLSECTOR_ASSET_DIR", NULLSECTOR_ASSET_DIR, "assets");
}

std::vector<unsigned char> rasterizeFontAtlasPixels() {
  const int cols = 16, rows = 8, cell = 8;
  const int atlasW = cols * cell, atlasH = rows * cell;
  std::vector<unsigned char> px((size_t)atlasW * atlasH * 4, 0);

  for (int c = 32; c < 128; c++) {
    const int gi = c - 32;
    if (gi < 0 || gi >= (int)FONT8X8.size()) continue;
    const int col = c % cols;
    const int row = c / cols;
    const int ox = col * cell, oy = row * cell;
    for (int y = 0; y < cell; y++) {
      const char* gline = FONT8X8[gi][y];
      for (int x = 0; x < cell; x++) {
        if (gline[x] == '#') {
          const int i = ((oy + y) * atlasW + (ox + x)) * 4;
          px[i] = px[i + 1] = px[i + 2] = 255;
          px[i + 3] = 255;
        }
      }
    }
  }
  return px;
}

Assets buildFontAtlas() {
  const int cols = 16, rows = 8, cell = 8;
  const int atlasW = cols * cell, atlasH = rows * cell;
  const std::vector<unsigned char> px = rasterizeFontAtlasPixels();

  Assets a;
  a.fontTex = Texture::fromRGBA(atlasW, atlasH, px.data(), {::gl::NEAREST, ::gl::NEAREST, ::gl::CLAMP_TO_EDGE, false});
  a.fontMetrics.cols = cols;
  a.fontMetrics.rows = rows;
  a.fontMetrics.cellW = cell;
  a.fontMetrics.cellH = cell;
  a.fontMetrics.advance = (int)(cell * 0.62f);
  a.fontMetrics.atlasW = atlasW;
  a.fontMetrics.atlasH = atlasH;
  return a;
}

Assets buildTrueTypeFontAtlas(const std::string& path) {
  Assets a;

  // load the font file bytes (virtual path through the runtime VFS, with a
  // direct-file fallback for absolute --font= paths)
  std::vector<unsigned char> ttf = runtimeFS().read(path);
  if (ttf.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path, std::ios::binary);
      if (f) ttf.assign(std::istreambuf_iterator<char>(f),
                        std::istreambuf_iterator<char>());
    }
  }
  if (ttf.empty()) {
    std::fprintf(stderr, "[ASSETS] cannot open font %s\n", path.c_str());
    return a;
  }

  stbtt_fontinfo fi;
  if (!stbtt_InitFont(&fi, ttf.data(), 0)) {
    std::fprintf(stderr, "[ASSETS] font %s is not a parseable TTF/OTF\n", path.c_str());
    return a;
  }

  // same 16x8 fixed-cell layout as the 8x8 font, but with much larger,
  // anti-aliased cells so glyphs stay crisp at the 22-46px boot text sizes
  const int cols = 16, rows = 8;
  const int cell = 96;                                   // px per glyph cell
  const int atlasW = cols * cell, atlasH = rows * cell;

  int ascent = 0, descent = 0, lineGap = 0;
  stbtt_GetFontVMetrics(&fi, &ascent, &descent, &lineGap);
  const float scale = stbtt_ScaleForPixelHeight(&fi, (float)cell * 0.8f);
  const int baselineY = (int)(ascent * scale + 0.5f);    // from cell top

  std::vector<unsigned char> px((size_t)atlasW * atlasH * 4, 0);
  std::vector<unsigned char> glyph;

  for (int c = 32; c < 128; c++) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&fi, c, scale, scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0, gh = y1 - y0;
    if (gw <= 0 || gh <= 0) continue;
    if (gw > cell || gh > cell) continue;                // oversized glyph (rare)

    glyph.assign((size_t)gw * gh, 0);
    stbtt_MakeCodepointBitmap(&fi, glyph.data(), gw, gh, gw, scale, scale, c);

    const int col = c % cols;                            // raw ASCII index,
    const int row = c / cols;                            // like TextMesh::glyphUVs
    const int ox = (cell - gw) / 2;                      // center horizontally
    const int oy = baselineY + y0;                       // baseline-anchored

    for (int gy = 0; gy < gh; gy++) {
      const int py = row * cell + oy + gy;
      if (py < 0 || py >= atlasH) continue;
      for (int gx = 0; gx < gw; gx++) {
        const int px_ = col * cell + ox + gx;
        if (px_ < 0 || px_ >= atlasW) continue;
        const unsigned char a = glyph[(size_t)gy * gw + gx];
        if (a == 0) continue;
        const size_t i = ((size_t)py * atlasW + px_) * 4;
        px[i] = px[i + 1] = px[i + 2] = 255;
        px[i + 3] = a;
      }
    }
  }

  a.fontTex = Texture::fromRGBA(atlasW, atlasH, px.data(),
                                {::gl::LINEAR_MIPMAP_LINEAR, ::gl::LINEAR,
                                 ::gl::CLAMP_TO_EDGE, true});
  a.fontMetrics.cols = cols;
  a.fontMetrics.rows = rows;
  a.fontMetrics.cellW = cell;
  a.fontMetrics.cellH = cell;
  a.fontMetrics.advance = (int)(cell * 0.62f);
  a.fontMetrics.atlasW = atlasW;
  a.fontMetrics.atlasH = atlasH;
  std::printf("[ASSETS] TrueType font atlas: %s (%dx%d, %dpx cells)\n",
              path.c_str(), atlasW, atlasH, cell);
  std::fflush(stdout);   // survives hard kills in smoke runs
  return a;
}

static bool loadPngRGBA(const std::string& path, std::vector<unsigned char>& out, int& w, int& h) {
  // virtual path through the runtime VFS (dev tree or package); absolute
  // editor paths fall through to a direct file read
  std::vector<unsigned char> bytes = runtimeFS().read(path);
  if (bytes.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path, std::ios::binary);
      if (f) bytes.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
    }
  }
  int comp = 0;
  unsigned char* data = bytes.empty()
                            ? nullptr
                            : stbi_load_from_memory(bytes.data(), (int)bytes.size(),
                                                    &w, &h, &comp, 4);
  if (!data) {
    std::fprintf(stderr, "[ASSETS] failed to load %s: %s\n", path.c_str(), stbi_failure_reason());
    return false;
  }
  out.assign(data, data + (size_t)w * h * 4);
  stbi_image_free(data);
  return true;
}

bool loadLogoTexture(LogoAsset& out) {

  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!loadPngRGBA("assets/logo.png", px, w, h)) return false;
  // luminance -> alpha: chrome letters (bright) opaque, backdrop (dark) clear
  for (int i = 0; i < w * h; i++) {
    const int j = i * 4;
    const float lum = 0.299f * px[j] + 0.587f * px[j + 1] + 0.114f * px[j + 2];
    float a = (lum - 24.0f) / 150.0f;  // soft ramp 24..174 -> 0..1
    a = a < 0 ? 0 : (a > 1 ? 1 : a);
    px[j + 3] = (unsigned char)(a * 255.0f + 0.5f);
  }

  out.w = w;
  out.h = h;
  out.rgba = std::move(px);
  out.aspect = (float)w / h;
  out.tex = Texture::fromRGBA(w, h, out.rgba.data(), {::gl::LINEAR_MIPMAP_LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, true});
  return true;
}

bool loadPngAsset(const std::string& file, Texture& out) {
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!loadPngRGBA("assets/" + file, px, w, h)) return false;
  out = Texture::fromRGBA(w, h, px.data(),
                          {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false});
  std::printf("[ASSETS] %s loaded (%dx%d)\n", file.c_str(), w, h);
  std::fflush(stdout);
  return true;
}

bool loadSplashLogo(Texture& out) {
  return loadPngAsset("splash.png", out);
}

bool cropLogoAsset(LogoAsset& out, float topFrac, float heightFrac) {
  if (out.rgba.empty() || out.w == 0 || out.h == 0) return false;
  const int srcW = out.w, srcH = out.h;
  const int y0 = (int)(srcH * topFrac);
  const int h = std::max(1, (int)(srcH * heightFrac));
  std::vector<unsigned char> crop((size_t)srcW * h * 4);
  for (int y = 0; y < h; y++) {
    const int sy = y0 + y;
    if (sy >= srcH) break;
    std::memcpy(crop.data() + (size_t)y * srcW * 4, out.rgba.data() + (size_t)sy * srcW * 4, (size_t)srcW * 4);
  }
  out.w = srcW;
  out.h = h;
  out.rgba = std::move(crop);
  out.aspect = (float)srcW / h;
  out.tex = Texture::fromRGBA(srcW, h, out.rgba.data(), {::gl::LINEAR_MIPMAP_NEAREST, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, true});
  return true;
}

}  // namespace ns
