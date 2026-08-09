#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 8 - DiagnosticText: minimal boot messages with glitch
// reconstruction. Lines assemble character-by-character (uProgress), and
// unrevealed characters flicker through random atlas glyphs + horizontal
// slice jumps instead of fading - the "letters appear through glitch
// reconstruction" look.
//
// Readability: glyphs are drawn as a dark-halo outline + crisp bright core
// (non-additive blend in intro.cpp), so text stays legible even over the
// bright diagnostics grid instead of blowing out into a white glow.
// Cyan / white / dark-slate only.
// Drawn with text.vert (TextMesh), same atlas + quad layout as text.frag.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTex;    // font atlas (unit 0)
uniform float uTime;
uniform float uAlpha;
uniform float uProgress;   // 0..1 fraction of the line revealed
uniform float uSeed;       // line seed (== line.colorSeed)
uniform float uChars;      // character count of the line
uniform int uStyle;        // 0 boot line, 1 title (brighter / no scramble)
uniform vec2 uAtlas;       // atlas size (used for outline dilation)
uniform vec2 uCell;        // glyph cell size

in vec2 vUV;
in float vSeed;

out vec4 fragColor;

void main() {
  float a = texture(uTex, vUV).a;
  if (a < 0.02) discard;

  // character index from the per-char seed (TextMesh: seed = uSeed + ci*0.0137)
  int ci = int(round((vSeed - uSeed) / 0.0137));
  float cF = float(ci);

  // reveal: each char turns on as uProgress passes its index
  float rev = clamp(uProgress * uChars, 0.0, uChars);
  float on = smoothstep(cF - 0.5, cF + 0.5, rev);
  if (on < 0.01) discard;

  // --- glitch slice jumps (whole char shifts horizontally in bands) ---------
  float slice = floor(vUV.y * 5.0);
  float doJump = step(0.65, hash12(vec2(slice + uSeed * 1.7, floor(uTime * 11.0))));
  float jx = (hash12(vec2(slice * 1.3 + uSeed, floor(uTime * 13.0))) - 0.5) * doJump;
  vec2 suv = vUV + vec2(jx * 0.05, 0.0);

  float g = texture(uTex, clamp(suv, 0.0, 1.0)).a;
  float crisp = smoothstep(0.35, 0.75, g);

  // --- dark halo: dilate the glyph coverage, then keep only the rim ---------
  // The dilation is clamped to this glyph's own atlas cell so the rim can
  // never pick up neighbouring glyphs' pixels (matters with a packed TTF
  // atlas where every cell is filled).
  vec2 cellSize = uCell / uAtlas;                       // one cell in uv
  vec2 cell0 = floor(vUV / cellSize) * cellSize;        // this cell's origin
  vec2 dil = cellSize * 0.32;
  float dl = g;
  dl = max(dl, texture(uTex, clamp(suv + vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uTex, clamp(suv - vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uTex, clamp(suv + vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uTex, clamp(suv - vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  float halo = smoothstep(0.15, 0.45, dl) * (1.0 - crisp);

  // --- unrevealed: random glyph flicker (reconstruction scramble) -----------
  float rc = 0.0;
  if (on < 0.99 && uStyle == 0) {
    float rnd = hash12(vec2(cF, floor(uTime * 18.0) + uSeed * 3.0));
    // remap this glyph cell to a random atlas cell
    vec2 luv = fract(vUV * vec2(16.0, 8.0));           // 0..1 inside the cell
    vec2 target = vec2(fract(rnd * 16.0), fract(rnd * 8.0));
    vec2 ruv = (target + luv) / vec2(16.0, 8.0);
    rc = smoothstep(0.35, 0.75, texture(uTex, ruv).a);
  }

  // --- color: dark halo rim + crisp core + restrained cyan accents ----------
  vec3 col = vec3(0.0, 0.015, 0.03) * halo;             // near-black slate rim
  col += vec3(0.92, 0.98, 1.0) * crisp;                 // bright core
  col += vec3(0.25, 0.85, 1.0) * crisp * 0.22;          // subtle cyan tint
  col += vec3(0.2, 0.8, 1.0) * rc * 0.45 * (1.0 - on);  // scramble flicker
  col += vec3(0.8, 1.0, 1.0) * rc * 0.15 * (1.0 - on);
  // faint scanline sheen, mostly on the resolved core
  col += vec3(0.3, 0.9, 1.0) * 0.04 * (0.5 + 0.5 * sin(vUV.y * 240.0 + uTime * 2.0)) * crisp;

  float alpha = (crisp + halo) * on * uAlpha + rc * (1.0 - on) * 0.4 * uAlpha;
  if (alpha <= 0.01) discard;
  fragColor = vec4(col, alpha);
}
