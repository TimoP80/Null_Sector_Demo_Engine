#version 300 es
// ---------------------------------------------------------------------------
// SCENE 9 - Greetings: stylized group logo marks.
// Each group name is re-rendered above its plain listing in a per-group style
// (chrome, distress, glitch, outline, pixel, neon) - the oldschool poster
// treatment where every group had its own logo font. The style is fixed per
// group (matching the reference poster), while a per-column seed (uSeed) plus
// the per-character seed (vSeed) shift the in-style variation: tint, wear
// patterns, glitch phase, pixel dither.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTex;
uniform float uTime;
uniform float uCycle;
uniform float uAlpha;
uniform float uGlow;
uniform float uSeed;    // per-column seed: variation within each style
uniform int uStyle;     // 0 chrome, 1 distress, 2 glitch, 3 outline, 4 pixel, 5 neon
uniform float uTint;    // per-group hue shift (1 = green; reference TRISTAR logo)
uniform float uShadow;  // >0.5 = solid black backing pass (readability)

in vec2 vUV;
in float vSeed;

out vec4 fragColor;

/** atlas-space cell fraction (16 cols) - used for glyph-neighbor taps */
const float CELL = 0.0625;

void main() {
  float a = texture(uTex, vUV).a;
  float crisp = smoothstep(0.35, 0.75, a);

  // solid black backing pass (uShadow > 0.5): drawn from the RAW glyph
  // coverage before any style math, so even the sparsest styles (outline /
  // pixel / distress) get a full dark letterform under the colored fill.
  // The old code keyed the backing off each style's alpha, which was ~0 for
  // the pixel style's broken quantization - so that mark silently vanished.
  if (uShadow > 0.5) {
    float ba = smoothstep(0.2, 0.6, a) * uAlpha;
    if (ba <= 0.01) discard;
    fragColor = vec4(0.0, 0.0, 0.01, ba);
    return;
  }

  // dark halo rim: dilated coverage minus the crisp core - darkens the busy
  // synth backdrop (scanline sun, mountains, grid) underneath each glyph so
  // the mark reads as bright letters on a black outline instead of thin
  // colored strokes lost in the glow. Same trick the logo scene uses.
  float e = CELL * 0.16;
  float aL = texture(uTex, vUV - vec2(e, 0.0)).a;
  float aR = texture(uTex, vUV + vec2(e, 0.0)).a;
  float aU = texture(uTex, vUV + vec2(0.0, e)).a;
  float aD = texture(uTex, vUV - vec2(0.0, e)).a;
  float dil = max(max(aL, aR), max(aU, aD));
  if (a < 0.02 && dil < 0.02) discard;
  float rim = smoothstep(0.35, 0.75, dil) * (1.0 - crisp);

  vec3 col = palVoid(vSeed + uCycle + uSeed);
  float alpha = crisp * uAlpha;

  if (uStyle == 0) {
    // chrome: steel gradient + top light + faint palette sheen (metallic)
    float band = 0.5 + 0.5 * sin(vUV.x * 11.0 + vUV.y * 6.0 + uTime * 0.4 + uSeed * 6.0);
    col = mix(vec3(0.4, 0.45, 0.58), vec3(0.95, 0.97, 1.0), band);
    col += vec3(1.0, 0.92, 0.85) * pow(max(vUV.y * 2.2 - 0.4, 0.0), 2.0) * 0.5;
    col += palVoid(vSeed * 0.5 + musicHue() * 0.5) * 0.2;
  } else if (uStyle == 1) {
    // distress / grunge: worn patches + chunked glyph edges
    float n = hash12(vUV * 42.0 + floor(uTime * 6.0) * 0.13 + uSeed * 31.0);
    col = mix(col, vec3(0.04, 0.035, 0.07), smoothstep(0.5, 0.9, n) * 0.6);
    float edgeNoise = hash12(vUV * 31.0 + vSeed * 7.0 + uSeed * 17.0);
    alpha *= step(0.28, edgeNoise * 0.35 + crisp * 0.9);
    col *= 0.7 + 0.5 * crisp;
  } else if (uStyle == 2) {
    // glitch: horizontal slices shifted + rgb channel split
    float slice = floor(vUV.y * (7.0 + 5.0 * hash12(vec2(vSeed + uSeed))));
    float jump = step(0.55, hash12(vec2(slice * 3.7 + uSeed, floor(uTime * 10.0))));
    float jx = (hash12(vec2(slice * 1.3 + floor(uTime * 12.0))) - 0.5) * 0.3 * jump;
    float ga = texture(uTex, vec2(vUV.x + jx * CELL, vUV.y)).a;
    float gc = smoothstep(0.35, 0.75, ga);
    col = mix(col, palVoid(vSeed + uCycle + uSeed + 0.1), 0.5) * gc + col * crisp * 0.5;
    float rs = texture(uTex, vec2(vUV.x + 0.006, vUV.y)).a * step(0.5, hash12(vec2(slice, floor(uTime * 14.0))));
    float bs = texture(uTex, vec2(vUV.x - 0.006, vUV.y)).a * step(0.5, hash12(vec2(slice + 9.0, floor(uTime * 14.0))));
    col = vec3(col.r + rs * 0.9, col.g, col.b + bs * 0.9);
    alpha = clamp(crisp + rs * 0.4 + bs * 0.4, 0.0, 1.0) * uAlpha;
  } else if (uStyle == 3) {
    // outline: hollow glyph - bright where a filled neighbor borders an empty cell
    float e = CELL * 0.16;
    float aL = texture(uTex, vUV - vec2(e, 0.0)).a;
    float aR = texture(uTex, vUV + vec2(e, 0.0)).a;
    float aU = texture(uTex, vUV + vec2(0.0, e)).a;
    float aD = texture(uTex, vUV - vec2(0.0, e)).a;
    float edge = max(max(aL, aR), max(aU, aD)) * (1.0 - crisp);
    col = palVoid(vSeed + uCycle + uSeed + 0.15) * (0.7 + edge * 2.2);
    // hollow interior gets a moderate fill so the letter reads on the black
    // backing instead of collapsing to a sliver of bright rim.
    alpha = clamp(edge * 3.2 + crisp * 0.35, 0.0, 1.0) * uAlpha;
  } else if (uStyle == 4) {
    // pixel: blocky quantization of the glyph WITHIN its atlas cell. The
    // old code quantized the whole-atlas UV (2 samples across a 96px cell)
    // and the thin TrueType strokes fell between the samples - the glyph
    // rendered as empty. 8 blocks per cell with a max-over-2x2-neighborhood
    // tap keeps every stroke solid (no gappy pixels) while still reading as
    // chunky retro pixels.
    const vec2 grid = vec2(16.0, 8.0);
    vec2 base = floor(vUV * grid) / grid;
    vec2 rel = vUV * grid - floor(vUV * grid);
    const float pxq = 8.0;
    vec2 qrel = (floor(rel * pxq) + 0.5) / pxq;
    vec2 quv = base + qrel / grid;
    vec2 stepq = vec2(1.0) / (grid * pxq);
    float qa = max(max(texture(uTex, quv).a,
                       texture(uTex, quv + vec2(stepq.x, 0.0)).a),
                   max(texture(uTex, quv + vec2(0.0, stepq.y)).a,
                       texture(uTex, quv + stepq).a));
    float qc = smoothstep(0.35, 0.75, qa);
    col = mix(palVoid(vSeed + uCycle + uSeed), vec3(0.15, 0.45, 1.0), 0.35);
    col *= qc * (0.7 + 0.5 * uGlow);
    alpha = qc * uAlpha;
  } else {
    // neon: glowing outline + additive halo (futuristic)
    float e = CELL * 0.14;
    float aL = texture(uTex, vUV - vec2(e, 0.0)).a;
    float aR = texture(uTex, vUV + vec2(e, 0.0)).a;
    float aU = texture(uTex, vUV + vec2(0.0, e)).a;
    float aD = texture(uTex, vUV - vec2(0.0, e)).a;
    float edge = max(max(aL, aR), max(aU, aD)) * (1.0 - crisp);
    col = palVoid(vSeed + uCycle + uSeed + 0.1);
    col += col * (crisp * 1.2 + edge * 1.6);
    col += palVoid(vSeed + uCycle) * a * uGlow * 1.6;
    alpha = clamp(crisp + edge, 0.0, 1.0) * uAlpha;
  }

  // per-group hue shift (reference: TRISTAR's sharp logo reads green).
  // Multiplicative so metallic shading survives the tint.
  col = mix(col, col * vec3(0.35, 1.7, 0.55) + vec3(0.0, 0.25, 0.0), uTint);
  col = textSurface(col, vUV, vSeed + uSeed + uCycle, uTime, gl_FragCoord.xy, 0.48);

  // lay the dark rim UNDER the glyph: blend the scene toward black where the
  // dilated coverage is present but the crisp core is not, then draw the
  // bright letter on top of it. The core itself also gets a brightness push
  // so the styled letters read clearly against the dark floor - clamped so
  // the loudest kick frames can't blow the palette into white clip.
  float coreBoost = 1.0 + 0.9 * crisp;
  col = min(col * coreBoost, vec3(1.4));

  if (rim > 0.01) {
    col = mix(col, vec3(0.0, 0.0, 0.02), rim);
    alpha = max(alpha, rim * uAlpha);
  }

  if (alpha <= 0.01) discard;
  fragColor = vec4(col, alpha);
}
