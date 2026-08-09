#version 300 es
// ---------------------------------------------------------------------------
// Plasma - classic 90s demo plasma, domain-warped + palette cycling.
// Used as the boot screen backdrop and under the credits.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uTime;
uniform float uIntensity;
uniform vec3 uColA;
uniform vec3 uColB;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;

  float t = uTime * 0.5;
  float d1 = sin(uv.x * 6.0 + t * 2.0) + sin(uv.y * 7.0 - t * 1.7) + sin((uv.x + uv.y) * 9.0 + t * 0.9);
  float d2 = sin(uv.y * 9.0 + sin(uv.x * 4.0 + t * 3.0) * 2.0 + t) + sin(uv.x * 11.0 - t * 1.3);
  float d3 = warp(uv * 3.0, t) * 2.0;
  float v = d1 * 0.5 + d2 * 0.3 + d3 * 2.0;

  // palette cycling: anchored to the musical chord hue so the plasma's mood
  // state follows the bar progression instead of wall-clock time
  float cyc = musicHue() + v * 0.18;
  vec3 col = mix(uColA, uColB, 0.5 + 0.5 * sin(cyc * TAU));
  col = mix(col, palVoid(cyc * 0.5), 0.5);
  col *= 0.5 + 0.5 * sat01(v * 0.5);

  fragColor = vec4(col * (0.25 + uIntensity), 1.0);
}
