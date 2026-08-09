#version 300 es
// ---------------------------------------------------------------------------
// Logo explosion particles: soft glowy sprites with a hot core.
// ---------------------------------------------------------------------------
precision highp float;
#include <common>

in vec2 vUV;
in vec3 vCol;
in float vAlpha;

out vec4 fragColor;

void main() {
  vec2 c = gl_PointCoord - 0.5;
  float d = length(c) * 2.0;
  float a = smoothstep(1.0, 0.0, d);
  a = a * a;
  // hot core + soft edge (additive pass: RGB is the glow contribution)
  float core = exp(-d * 6.0) * 0.7;
  vec3 col = vCol * (a + core) * 1.6;
  // sparkle
  col += vCol * hash12(gl_PointCoord * 17.0 + fract(vUV.x * 99.0)) * 0.6 * a;
  fragColor = vec4(col * a * vAlpha, 1.0);
}
