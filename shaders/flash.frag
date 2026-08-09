#version 300 es
// Landing verdict readout (HIT / FLOATED): rendered into a dedicated full-res
// HDR target BEFORE bloom extraction, so the bright glyphs genuinely bloom
// through the multi-pass chain (bloom_extract adds this texture pre-threshold,
// compose adds it to the scene color pre-tonemap). Additive by construction -
// every pixel writes a color (zero where no readout), and the scene is never
// darkened here; the glow itself carries readability.
#include <common>

uniform vec2 uRes;
uniform float uLanding;   // 1 = HIT, 2 = FLOATED
uniform float uLandingT;  // flash window: 1 fresh .. 0 expired (holds at 1 early)

out vec4 fragColor;

// ---------------------------------------------------------------------------
// 3x5 glyph font (H I T F L O A E D): rows are bitmasks, MSB = leftmost
// column, row 0 = top. Index order: H I T F L O A E D.
// ---------------------------------------------------------------------------
const int FONT3x5[45] = int[45](
  5, 5, 7, 5, 5, // H
  7, 2, 2, 2, 7, // I
  7, 2, 2, 2, 2, // T
  7, 4, 6, 4, 4, // F
  4, 4, 4, 4, 7, // L
  2, 5, 5, 5, 2, // O
  2, 5, 7, 5, 5, // A
  7, 4, 6, 4, 7, // E
  6, 5, 5, 5, 6  // D
);

/** glyph index for the ci-th character of the current verdict string */
int glyphAt(int ci) {
  if (uLanding < 1.5) {
    // HIT
    if (ci == 0) return 0;
    if (ci == 1) return 1;
    return 2;
  }
  // FLOATED -> F L O A T E D
  const int gl[7] = int[7](3, 4, 5, 6, 2, 7, 8);
  return gl[ci];
}

/** glyph bit at (gx, gy): 1 = lit */
float glyphBit(int gi, int gx, int gy) {
  int row = FONT3x5[gi * 5 + gy];
  return float((row >> (2 - gx)) & 1);
}

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec3 col = vec3(0.0);
  float env = uLandingT;
  if (env <= 0.001) {
    fragColor = vec4(col, 1.0);
    return;
  }

  vec3 vc = uLanding < 1.5 ? vec3(0.29, 0.87, 0.50) : vec3(0.97, 0.44, 0.44);
  int chars = uLanding < 1.5 ? 3 : 7;
  float cellPx = 9.0;
  float pop = 1.0 + 0.12 * env; // gentle settle as the flash ages

  vec2 p = uv - 0.5;
  float r2 = dot(p, p);

  // colored edge pulse: surges the frame border with the verdict color
  float edge = sat01(r2 * 2.6);
  col += vc * edge * env * 0.30;

  // bar glow + lit border (additive: the readout reads as a glowing band)
  float barW = (float(chars) * 4.0 - 1.0) * cellPx / uRes.x * 0.5 + 0.012;
  float barH = 5.0 * cellPx / uRes.y * 0.5 + 0.008;
  vec2 bd = vec2(abs(uv.x - 0.5) / barW, abs(uv.y - 0.16) / barH);
  float bar = 1.0 - smoothstep(1.0, 1.08, max(bd.x, bd.y));
  col += vc * bar * env * 0.18;
  col += vc * step(0.8, max(bd.x, bd.y)) * bar * env * 0.6;

  // glyph readout: 3x5 font, 4px advance per char (3 lit + 1 gap). Brighter
  // than the bar since there's no dark backing - the glow carries the text.
  vec2 tp = (uv - vec2(0.5, 0.16)) * vec2(uRes.x, uRes.y) / (cellPx * pop);
  tp += vec2((float(chars) * 4.0 - 1.0) * 0.5, 2.5);
  int gx = int(floor(tp.x));
  int gy = int(floor(tp.y));
  if (gx >= 0 && gy >= 0 && gx < chars * 4 - 1 && gy < 5) {
    int ci = gx / 4;
    int gcol = gx % 4;
    if (gcol < 3) col += vc * glyphBit(glyphAt(ci), gcol, gy) * env * 2.0;
  }

  fragColor = vec4(col, 1.0);
}
