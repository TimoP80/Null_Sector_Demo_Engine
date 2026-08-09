#version 300 es
// ---------------------------------------------------------------------------
// Cinematic text shader (port of src/shaders/text.frag + the per-style
// treatments). One program, eight visual treatments selected by uStyle:
//   0 terminal      - phosphor green, scanlines, typewriter cursor, flicker
//   1 holo          - translucent blue hologram, wobble + vertical scan
//   2 glitch        - rgb split + horizontal slice jumps (corruption)
//   3 neon          - additive neon signage with halo
//   4 scan          - CRT raster reveal: a bright scan band sweeps down
//   5 dissolve      - pixel assemble / dissolve driven by uProgress + hash
//   6 chrome        - brushed metal gradient, top light, palette sheen
//   7 outline       - hollow vector stroke (wireframe lettering)
// The same bitmap atlas (uTex) + quad layout as text.frag is reused; the
// per-char seed (vSeed) plus a per-line seed (uSeed) drive the variation.
// uProgress is style-dependent (scan position / dissolve fraction / flicker
// intensity); uEnergy drives beat-synced brightness.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTex;
uniform float uTime;
uniform float uAlpha;
uniform float uGlow;
uniform float uSeed;      // per-instance variation
uniform float uProgress;  // style-dependent animation state (0..1)
uniform float uEnergy;    // music energy (0..1) - beat brightness
uniform int uStyle;       // 0..7 treatment selector

in vec2 vUV;
in float vSeed;

out vec4 fragColor;

/** atlas-space cell fraction (16 cols) - for glyph-neighbor taps */
const float CELL = 0.0625;

/** cheap hash for per-pixel dissolve / flicker */
float chash(vec2 p) { return hash12(p + uSeed * 7.3); }

void main() {
  float a = texture(uTex, vUV).a;
  if (a < 0.02) discard;
  float crisp = smoothstep(0.35, 0.75, a);

  // musical chord hue anchors the family color; per-char vSeed keeps local
  // variation so captions stay readable while the mood tracks the bars
  vec3 col = palVoid(vSeed * 0.5 + musicHue() * 0.5 + uSeed);
  float alpha = crisp * uAlpha;
  float energy = 0.7 + 0.5 * uEnergy;

  if (uStyle == 0) {
    // --- terminal: phosphor green + scanlines + typewriter cursor flicker
    col = vec3(0.25, 1.0, 0.5) * (0.75 + 0.45 * crisp);
    col += vec3(0.15, 0.6, 0.3) * a * uGlow;
    float scan = 1.0 - smoothstep(0.0, 0.6, abs(fract(vUV.y * 240.0) - 0.5));
    col *= 0.82 + 0.18 * scan;
    // subtle char-level flicker on the leading edge
    float flick = 0.9 + 0.1 * sin(uTime * 9.0 + vSeed * 47.0);
    col *= flick;
    // NOTE: no shader-side cursor here - the typing cursor is a real '_' glyph
    // appended by CineText::typed, which avoids per-glyph vUV ambiguity.
  } else if (uStyle == 1) {
    // --- holo: translucent blue projection, wobble + sweep
    float wob = sin(vUV.y * 30.0 + uTime * 3.0 + vSeed * 9.0) * 0.012;
    vec2 huv = vUV + vec2(wob, 0.0);
    float ha = texture(uTex, huv).a;
    float hc = smoothstep(0.3, 0.7, ha);
    col = mix(vec3(0.1, 0.35, 0.75), vec3(0.5, 0.85, 1.0), hc);
    col += vec3(0.4, 0.8, 1.0) * a * uGlow * 0.8;
    // horizontal light band drifting downward
    float band = 1.0 - smoothstep(0.0, 0.25, abs(fract(vUV.y * 3.0 - uTime * 0.6) - 0.5));
    col += vec3(0.7, 0.95, 1.0) * band * 0.45 * hc;
    alpha = hc * uAlpha * (0.75 + 0.25 * uEnergy);
  } else if (uStyle == 2) {
    // --- glitch: rgb split + horizontal slice jumps
    float slice = floor(vUV.y * (8.0 + 6.0 * hash12(vec2(vSeed + uSeed))));
    float jump = step(0.45, hash12(vec2(slice * 3.7 + uSeed, floor(uTime * 11.0))));
    float jx = (hash12(vec2(slice * 1.3 + floor(uTime * 13.0))) - 0.5) * 0.35 * jump;
    float ga = texture(uTex, vec2(vUV.x + jx * CELL, vUV.y)).a;
    float gc = smoothstep(0.35, 0.75, ga);
    col = mix(col, palVoid(vSeed * 0.4 + musicHue() * 0.6 + uSeed + 0.12), 0.45) * gc
        + col * crisp * 0.4;
    float rs = texture(uTex, vec2(vUV.x + 0.01, vUV.y)).a * step(0.5, hash12(vec2(slice, floor(uTime * 15.0))));
    float bs = texture(uTex, vec2(vUV.x - 0.01, vUV.y)).a * step(0.5, hash12(vec2(slice + 9.0, floor(uTime * 15.0))));
    col = vec3(col.r + rs * 0.9, col.g, col.b + bs * 0.9);
    alpha = clamp(crisp + rs * 0.4 + bs * 0.4, 0.0, 1.0) * uAlpha;
  } else if (uStyle == 3) {
    // --- neon: additive signage with glow halo
    float e = CELL * 0.13;
    float aL = texture(uTex, vUV - vec2(e, 0.0)).a;
    float aR = texture(uTex, vUV + vec2(e, 0.0)).a;
    float aU = texture(uTex, vUV + vec2(0.0, e)).a;
    float aD = texture(uTex, vUV - vec2(0.0, e)).a;
    float edge = max(max(aL, aR), max(aU, aD)) * (1.0 - crisp);
    col = palVoid(vSeed * 0.5 + musicHue() * 0.5 + uSeed);
    col += col * (crisp * 1.3 + edge * 1.7);
    col += palVoid(vSeed * 0.5 + musicHue() * 0.5) * a * uGlow * (1.2 + uEnergy * 1.2);
    alpha = clamp(crisp + edge, 0.0, 1.0) * uAlpha;
    // neon buzz
    col *= 0.92 + 0.08 * sin(uTime * 50.0 + vSeed * 13.0);
  } else if (uStyle == 4) {
    // --- scan: raster reveal - bright band sweeps down with uProgress
    float scanY = mix(1.15, -0.15, uProgress);
    float band = 1.0 - smoothstep(0.0, 0.06, abs(vUV.y - scanY));
    float above = smoothstep(scanY, scanY + 0.15, vUV.y);  // already-written area
    col = palVoid(vSeed + uSeed + 0.55) * (0.6 + 0.5 * crisp);
    col += vec3(0.9, 1.0, 1.0) * band * 0.9;
    alpha = crisp * uAlpha * (0.25 + 0.75 * above + band * 0.9);
    // raster scanlines over the written area
    float line = smoothstep(0.5, 0.0, abs(fract(vUV.y * 320.0) - 0.5));
    col *= 0.88 + 0.12 * line;
  } else if (uStyle == 5) {
    // --- dissolve: pixels assemble in / dissolve out via uProgress
    float d = chash(vUV * 41.0 + vSeed * 5.0);
    float on = 1.0 - smoothstep(uProgress, uProgress + 0.25, d);
    col = palVoid(vSeed + uSeed + 0.3);
    col += vec3(1.0) * uGlow * 0.4 * a * on;
    alpha = crisp * uAlpha * on;
  } else if (uStyle == 6) {
    // --- chrome: brushed metal + top light + fresnel-ish sheen
    float band = 0.5 + 0.5 * sin(vUV.x * 9.0 + vUV.y * 5.0 + uTime * 0.35 + uSeed * 6.0);
    col = mix(vec3(0.42, 0.46, 0.58), vec3(0.95, 0.97, 1.0), band);
    col += vec3(1.0, 0.92, 0.85) * pow(max(vUV.y * 2.3 - 0.4, 0.0), 2.0) * 0.55;
    col += palVoid(vSeed * 0.5 + musicHue() * 0.5) * 0.22;
    col += vec3(0.6, 0.8, 1.0) * uGlow * 0.25;
    col *= 0.9 + 0.1 * uEnergy;
    // subtle horizontal scanline shimmer
    float shim = 1.0 - smoothstep(0.0, 0.7, abs(fract(vUV.y * 160.0) - 0.5));
    col *= 0.94 + 0.06 * shim;
  } else {
    // --- outline: hollow stroke - bright where filled neighbor borders empty
    float e = CELL * 0.17;
    float aL = texture(uTex, vUV - vec2(e, 0.0)).a;
    float aR = texture(uTex, vUV + vec2(e, 0.0)).a;
    float aU = texture(uTex, vUV + vec2(0.0, e)).a;
    float aD = texture(uTex, vUV - vec2(0.0, e)).a;
    float edge = max(max(aL, aR), max(aU, aD)) * (1.0 - crisp);
    col = palVoid(vSeed + uSeed + 0.15);
    col = mix(col, vec3(0.9, 0.95, 1.0), 0.35);
    col += col * (edge * 2.4);
    alpha = clamp(edge * 3.0 + crisp * 0.12, 0.0, 1.0) * uAlpha;
  }

  // Keep every cinematic caption in the same richly colored, textured family
  // while preserving the distinct style-specific shading above.
  col = textSurface(col, vUV, vSeed + uSeed, uTime, gl_FragCoord.xy, 0.56);

  // energy pulse: everything brightens toward the beat
  col *= energy;

  if (alpha <= 0.01) discard;
  fragColor = vec4(col, alpha);
}
