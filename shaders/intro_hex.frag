#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 4 - HexBackground: scrolling hexadecimal value columns.
// Digits are sampled straight from the 8x8 font atlas, so they render as the
// same crisp bitmap glyphs as every other text in the engine. Columns scroll
// at staggered speeds, faintly, with occasional bright decode highlights.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uDiag;
uniform float uTime;
uniform vec2 uParallax;
uniform sampler2D uFont;    // font atlas (unit 1)
uniform vec2 uAtlas;        // atlas size (128,64)
uniform vec2 uCell;         // glyph cell (8,8)
uniform vec2 uFontGrid;     // cols, rows (16,8)

out vec4 fragColor;

/** map a hex digit value (0..15) to the ASCII code of its glyph */
int hexCode(int d) { return d < 10 ? 48 + d : 65 + d - 10; }

/** sample one atlas glyph by ASCII code + cell-local uv (luv.y 0 = glyph
 *  bottom, 1 = glyph top - matches the unflipped atlas convention where the
 *  glyph's top row sits at the SMALLER v, exactly like TextMesh::glyphUVs).
 *  Atlas cells are indexed by the RAW ascii code (code % 16, code / 16),
 *  matching the rasterizer - do NOT subtract 32 here. */
float glyph(int code, vec2 luv) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec3 col = vec3(0.0);
  if (uDiag < 0.001) { fragColor = vec4(0.0); return; }

  float cellH = 0.026;   // glyph height on screen
  float cellW = 0.024;

  // three columns at staggered depths (parallax)
  for (int colIdx = 0; colIdx < 3; colIdx++) {
    float ci = float(colIdx);
    float cx = -0.88 + ci * 0.055 - uParallax.x * (0.5 + ci * 0.25);
    float speed = 0.5 + ci * 0.25 + 0.2 * sin(uTime * 0.2 + ci * 2.0);

    // each pixel belongs to exactly one scrolling row
    float rowF = (p.y + uTime * speed) / cellH;
    int row = int(floor(rowF));
    float ly = fract(rowF);            // 0 = bottom of cell, 1 = top

    float h = hash12(vec2(ci * 9.7, float(row) * 1.31));
    int d1 = int(h * 15.999);
    int d2 = int(fract(h * 7.31) * 15.999);
    float bright = 0.25 + 0.75 * step(0.9, h);

    for (int g = 0; g < 2; g++) {
      int d = g == 0 ? d1 : d2;
      float gx = cx + float(g) * cellW;
      float lx = clamp((p.x - gx) / cellW, 0.0, 1.0);
      if (lx < 0.0 || lx > 1.0) continue;
      float a = glyph(hexCode(d), vec2(lx, ly));
      if (a < 0.02) continue;
      col += vec3(0.2, 0.75, 1.0) * a * bright * uDiag * 0.4;
    }
  }

  fragColor = vec4(col, 1.0);
}
