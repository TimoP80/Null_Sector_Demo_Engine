#version 300 es
// PostStack pass: color grading (saturation / contrast / exposure / tint) with
// optional ACES tonemap (enable when the chain has no earlier tonemapper).
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uSaturation;   // 1 = neutral
uniform float uContrast;     // 1 = neutral
uniform float uExposure;     // 1 = neutral
uniform vec3 uTintA;         // shadow tint
uniform vec3 uTintB;         // highlight tint
uniform float uTonemap;      // 0/1

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec3 col = texture(uTex, uv).rgb;

  if (uTonemap > 0.5) col = tonemapACES(col);

  float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
  col = mix(vec3(lum), col, uSaturation);
  col = (col - 0.5) * uContrast + 0.5;
  vec3 grade = mix(uTintA, uTintB, lum);
  col *= grade * uExposure;
  col = satV(col);

  fragColor = vec4(col, texture(uTex, uv).a);
}
