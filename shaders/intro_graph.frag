#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 5 - NodeGraph: procedural node network + animated polyline
// graphs. Nodes wander on hash orbits, edges connect them, pulses travel
// along the links with the beat. Everything is generated from the pixel
// position + time - no geometry buffers.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uDiag;
uniform float uTime;
uniform vec2 uParallax;

out vec4 fragColor;

float nodePos(vec2 seed, float t, out vec2 pos) {
  float a = t * 0.35 + seed.x * TAU;
  float r = 0.02 + 0.05 * (0.5 + 0.5 * sin(t * 0.23 + seed.y * 9.0));
  pos = vec2(cos(a), sin(a) * 0.85) * r;
  return seed.y;
}

void main() {
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec3 col = vec3(0.0);
  if (uDiag < 0.001) { fragColor = vec4(0.0); return; }

  vec2 c = vec2(0.68, 0.3) - uParallax * 0.18;
  vec2 q = p - c;

  const int NN = 5;
  vec2 pts[NN];
  float seeds[NN];
  for (int i = 0; i < NN; i++) {
    seeds[i] = hash12(vec2(float(i) * 3.7, 1.3));
  }
  for (int i = 0; i < NN; i++) {
    float a = uTime * (0.22 + seeds[i] * 0.2) + seeds[i] * TAU;
    float r = 0.05 + 0.09 * seeds[i] + 0.03 * sin(uTime * 0.4 + float(i) * 2.3);
    pts[i] = vec2(cos(a), sin(a) * 0.8) * r;
  }

  // edges between consecutive nodes
  for (int i = 0; i < NN - 1; i++) {
    vec2 a = pts[i];
    vec2 b = pts[i + 1];
    // segment distance
    vec2 pa = q - a;
    vec2 ba = b - a;
    float hh = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-5), 0.0, 1.0);
    float d = length(pa - ba * hh);
    col += vec3(0.15, 0.7, 0.95) * exp(-d * 220.0) * uDiag * 0.6;

    // pulse traveling along the edge
    float ph = fract(uTime * 0.6 + float(i) * 0.37 + seeds[i]);
    vec2 pp = mix(a, b, ph);
    col += vec3(0.9, 1.0, 1.0) * exp(-length(q - pp) * 320.0) * uDiag * (0.6 + 0.4 * Null.uPulse);
  }

  // nodes
  for (int i = 0; i < NN; i++) {
    col += vec3(0.4, 0.9, 1.0) * exp(-length(q - pts[i]) * 260.0) * uDiag;
    col += vec3(0.9, 1.0, 1.0) * exp(-length(q - pts[i]) * 700.0) * uDiag * 0.6;
  }

  // ring frame
  col += vec3(0.12, 0.5, 0.75) * exp(-abs(length(q) - 0.16) * 260.0) * uDiag * 0.7;

  fragColor = vec4(col, 1.0);
}
