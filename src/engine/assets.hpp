// ---------------------------------------------------------------------------
// Procedural assets: bitmap font atlas + logo wordmark (port of assets.ts +
// logotex.ts). The 8x8 embedded font replaces the browser canvas rendering;
// the poster loads via stb_image.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include "engine/textmesh.hpp"
#include "engine/texture.hpp"
#include <string>
#include <vector>

namespace ns {

struct Assets {
  Texture fontTex;
  FontMetrics fontMetrics;
};

/** build the fixed-cell font atlas from the embedded 8x8 bitmap font */
Assets buildFontAtlas();

/** rasterize a TrueType font (assets/fonts, or --font=FILE) into the
 *  same fixed-cell 16x8 atlas layout, at high cell resolution with LINEAR
 *  filtering for smooth anti-aliased glyphs. Returns an Assets with a null
 *  texture on failure so callers can fall back to the 8x8 bitmap font. */
Assets buildTrueTypeFontAtlas(const std::string& path);

/** rasterize the font atlas to RGBA (row 0 = atlas top = glyph row 0). Shared
 *  by buildFontAtlas() and the --check-shaders orientation check so the two
 *  can never disagree about what the texture is supposed to contain. */
std::vector<unsigned char> rasterizeFontAtlasPixels();

// --- logo wordmark (shared by the logo climax + greetings poster) -----------

constexpr float HERO_CROP_TOP = 0.16f;      // rows 0.16..0.56 of the poster
constexpr float HERO_CROP_HEIGHT = 0.40f;

struct LogoAsset {
  Texture tex;
  int w = 0, h = 0;                 // source pixel size (cropped)
  std::vector<unsigned char> rgba;  // cropped RGBA (for particle sampling)
  float aspect = 1;
};

/** load assets/logo.png (RGB poster -> luminance alpha). Returns false if the
 *  file is missing (callers then render their procedural fallbacks). */
bool loadLogoTexture(LogoAsset& out);

/** load assets/splash.png (RGBA as-is) for the pre-show splash screen.
 *  Returns false if the file is missing (main() then skips the splash and
 *  starts the show + music immediately). */
bool loadSplashLogo(Texture& out);

/** load an arbitrary RGBA PNG from the assets dir (e.g. the end-of-show
 *  ghost_outro.png card). Returns false if the file is missing. */
bool loadPngAsset(const std::string& file, Texture& out);

/** crop a loaded logo asset to the shared hero band and rebuild a texture */
bool cropLogoAsset(LogoAsset& out, float topFrac, float heightFrac);

/** compile-time asset dir (override with NULLSECTOR_ASSET_DIR env) */
std::string assetDir();

}  // namespace ns
