#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN (fragment)
// Soft additive point sprite with a feathered core - the swarm reads as a
// luminous neural ocean rather than discrete dots. Bloom spreads the motes.
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
  a = a * a * (1.7 - d * 0.7);      // brighter core, soft wide falloff
  fragColor = vec4(vCol * a * vAlpha, a * vAlpha);
}
