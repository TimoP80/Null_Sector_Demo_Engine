// ---------------------------------------------------------------------------
// Bitmap font text rendering (port of src/engine/textmesh.ts).
// Builds quads in clip space from the fixed-cell font atlas.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/mesh.hpp"
#include <string>
#include <vector>

namespace ns {

struct FontMetrics {
  int cols = 16;
  int rows = 8;
  int cellW = 8;   // px
  int cellH = 8;   // px
  int advance = 6; // px between char origins
  int atlasW = 128;
  int atlasH = 64;
};

struct TextLine {
  std::string text;
  /** -1 center, 0..1 fraction from left, or 2 right */
  float align = -1;
  float colorSeed = 0;
};

struct TextLayoutOpts {
  int screenW = 1;
  int screenH = 1;
  int sizePx = 16;      // character cell height on screen in px
  int spacing = 0;      // extra space between chars, px
  float centerX = 0;    // NDC horizontal offset of the whole block
};

class TextMesh {
public:
  Mesh mesh{::gl::TRIANGLES};
  std::vector<float> verts;     // interleaved: pos2 + uv2 + seed1
  std::vector<uint16_t> indices;
  int count = 0;

  TextMesh() {
    verts.assign((size_t)512 * 4 * 5, 0.0f);
    indices.assign((size_t)512 * 6, 0);
    mesh.setBuffer(0, verts.data(), (int)verts.size(), 2, ::gl::DYNAMIC_DRAW, 5, 0);
    mesh.setBuffer(1, verts.data(), (int)verts.size(), 2, ::gl::DYNAMIC_DRAW, 5, 2);
    mesh.setBuffer(2, verts.data(), (int)verts.size(), 1, ::gl::DYNAMIC_DRAW, 5, 4);
  }

  /** atlas UVs for one ASCII code, raw-cell-fraction convention (v=0 = atlas
   *  top = glyph row 0). Shared by build() and the --check-shaders orientation
   *  check so the two can never drift: if someone re-introduces the `1 - x`
   *  flip or a flipped upload, build() and the check stay in lockstep. */
  static void glyphUVs(int code, const FontMetrics& m,
                       float& u0, float& v0, float& u1, float& v1) {
    const int cell = code % (m.cols * m.rows);
    const int cx = cell % m.cols;
    const int cy = cell / m.cols;
    u0 = (float)(cx * m.cellW) / m.atlasW;
    v0 = (float)((cy + 1) * m.cellH) / m.atlasH;
    u1 = (float)((cx + 1) * m.cellW) / m.atlasW;
    v1 = (float)(cy * m.cellH) / m.atlasH;
  }

  /** rebuild quads for the given lines. centerY = NDC center of the block. */
  void build(const std::vector<TextLine>& lines, const FontMetrics& m, const TextLayoutOpts& o, float centerY) {
    const float charH = (float)o.sizePx;
    const float charW = ((float)m.cellW / m.cellH) * charH;
    const float lineH = charH * 1.5f;
    const float spacing = (float)o.spacing;

    const float totalH = (float)lines.size() * lineH;
    int vcount = 0;
    int icount = 0;

    for (size_t li = 0; li < lines.size(); li++) {
      const TextLine& line = lines[li];
      const float textW = (float)line.text.size() * (charW + spacing);
      float x0 = 0;
      // x is converted to NDC as (x / screenW) * 2 - 1, i.e. x is measured in
      // pixels from the screen's LEFT edge - so a centered block must be
      // anchored at screenW/2 - textW/2. (The old -textW/2 treated x as if
      // measured from the screen center, which put the block's CENTER at the
      // left edge and pushed every centered line off to the left.)
      if (line.align < -0.5f) x0 = o.screenW * 0.5f - textW / 2;
      else if (line.align > 1.5f) x0 = o.screenW - textW;
      else x0 = line.align * o.screenW - textW / 2;

      const float y0 = centerY * (o.screenH * 0.5f) + (float)(lines.size() - 1 - li) * lineH - totalH / 2;

      for (size_t ci = 0; ci < line.text.size(); ci++) {
        const int code = (unsigned char)line.text[ci];
        if (code < 32 || code > 127) continue;

        const float x = x0 + (float)ci * (charW + spacing) + 0.5f * spacing * (ci > 0 ? 1 : 0);
        const float y = y0;

        // NOTE: the CPU atlas is uploaded with glTexImage2D and no
        // GL_UNPACK_FLIP_Y, so texture v=0 is the atlas's top row (glyph row
        // 0). The web build's canvas atlas uploads the same way (verified:
        // canvas top row lands at v=0), so BOTH text meshes use the raw cell
        // fractions below - keep the two in sync. The old `1 - x` flip here
        // sampled the wrong rows and every text quad rendered as a faint
        // garbage rectangle.
        float u0, v0, u1, v1;
        glyphUVs(code, m, u0, v0, u1, v1);

        const float seed = line.colorSeed + (float)ci * 0.0137f;

        // px -> ndc (+ block-level horizontal offset)
        const float nx = (x / o.screenW) * 2.0f - 1.0f + o.centerX;
        const float ny = (y / o.screenH) * 2.0f;
        const float nw = (charW / o.screenW) * 2.0f;
        const float nh = (charH / o.screenH) * 2.0f;

        const int base = vcount * 5;
        // bottom-left, bottom-right, top-right, top-left
        verts[base + 0] = nx; verts[base + 1] = ny; verts[base + 2] = u0; verts[base + 3] = v0; verts[base + 4] = seed;
        verts[base + 5] = nx + nw; verts[base + 6] = ny; verts[base + 7] = u1; verts[base + 8] = v0; verts[base + 9] = seed;
        verts[base + 10] = nx + nw; verts[base + 11] = ny + nh; verts[base + 12] = u1; verts[base + 13] = v1; verts[base + 14] = seed;
        verts[base + 15] = nx; verts[base + 16] = ny + nh; verts[base + 17] = u0; verts[base + 18] = v1; verts[base + 19] = seed;

        const int ib = vcount;
        const int ibase = icount * 6;
        indices[ibase + 0] = (uint16_t)ib;
        indices[ibase + 1] = (uint16_t)(ib + 1);
        indices[ibase + 2] = (uint16_t)(ib + 2);
        indices[ibase + 3] = (uint16_t)ib;
        indices[ibase + 4] = (uint16_t)(ib + 2);
        indices[ibase + 5] = (uint16_t)(ib + 3);

        vcount += 4;
        icount += 1;
      }
    }

    count = icount * 6;
    if (count > 0) {
      mesh.setBuffer(0, verts.data(), vcount * 5, 2, ::gl::DYNAMIC_DRAW, 5, 0);
      mesh.setBuffer(1, verts.data(), vcount * 5, 2, ::gl::DYNAMIC_DRAW, 5, 2);
      mesh.setBuffer(2, verts.data(), vcount * 5, 1, ::gl::DYNAMIC_DRAW, 5, 4);
      mesh.setIndices(indices.data(), count);
    }
  }

  bool empty() const { return count == 0; }
  void draw() const { if (count > 0) mesh.draw(count); }
};

}  // namespace ns
