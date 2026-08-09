#version 300 es
// ---------------------------------------------------------------------------
// Logo assembly particles: soft glowing sprites with a hot core and sparkle.
// ---------------------------------------------------------------------------
precision highp float;
#include <common>

in vec3 vCol;
in float vAlpha;

out vec4 fragColor;

void main() {
  vec2 c = gl_PointCoord - 0.5;
  float d = length(c) * 2.0;
  float a = smoothstep(1.0, 0.0, d);
  a = a * a;
  // hot core + soft edge (additive pass: RGB is the glow contribution)
  float core = exp(-d * 5.5) * 0.8;
  vec3 col = vCol * (a * 0.7 + core) * 1.7;
  // sparkle
  col += vCol * hash12(gl_PointCoord * 19.0 + vAlpha * 7.0) * 0.7 * a;
  fragColor = vec4(col * a * vAlpha, 1.0);
}
