#version 300 es
// Soft additive point sprite.
#include <common>

in vec3 vCol;
in float vAlpha;
in float vDist;

out vec4 fragColor;

void main() {
  vec2 c = gl_PointCoord - 0.5;
  float d = length(c) * 2.0;
  float a = smoothstep(1.0, 0.0, d);
  a = a * a;
  fragColor = vec4(vCol * a * vAlpha, a * vAlpha);
}
