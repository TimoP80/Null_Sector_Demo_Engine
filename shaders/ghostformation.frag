#version 300 es
// ---------------------------------------------------------------------------
// SCENE 5 - Ghost Formation fragment shader.
// Soft additive point sprite: a bright core with a wide feathered falloff so
// the swarm reads as a luminous cloud rather than discrete dots. The post
// bloom spreads each spark into a soft mote.
// ---------------------------------------------------------------------------
#include <common>

in vec3 vCol;
in float vAlpha;
in float vDist;

out vec4 fragColor;

void main() {
  vec2 c = gl_PointCoord - 0.5;
  float d = length(c) * 2.0;
  float a = smoothstep(1.0, 0.0, d);
  a = a * a * (1.6 - d * 0.6);   // brighter core, soft edges
  fragColor = vec4(vCol * a * vAlpha, a * vAlpha);
}
