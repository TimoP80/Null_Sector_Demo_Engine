#version 300 es
// Bitmap font: sample luminance from atlas, palette cycle per char.
#include <common>

uniform sampler2D uTex;
uniform float uTime;
uniform float uCycle;    // palette cycle amount (0..1)
uniform float uAlpha;
uniform float uGlow;
uniform float uWhite;    // force white (boot text)

in vec2 vUV;
in float vSeed;

out vec4 fragColor;

void main() {
  float a = texture(uTex, vUV).a;
  if (a < 0.02) discard;

  // crisp pixel edges + slight edge glow
  float crisp = smoothstep(0.35, 0.75, a);

  vec3 col;
  if (uWhite > 0.5) {
    col = vec3(1.0) * (0.8 + 0.2 * crisp);
  } else {
    // palette cycling over the char seed
    col = palVoid(vSeed + uCycle);
    col = mix(col, vec3(1.0, 0.95, 0.9), uWhite);
    col *= 0.55 + 0.7 * crisp;
    // additive glow halo
    col += palVoid(vSeed + uCycle) * a * uGlow;
  }

  // Make the chunky bitmap glyphs read as a colored, imperfect display
  // surface: iridescent threads, grain and occasional bright flecks.
  col = textSurface(col, vUV, vSeed + uCycle, uTime, gl_FragCoord.xy, 0.62);

  float alpha = crisp * uAlpha;
  if (alpha <= 0.01) discard;
  fragColor = vec4(col, alpha);
}
