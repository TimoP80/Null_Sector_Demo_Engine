#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 6 - ParticleOverlay: fine drifting particles over the whole
// frame. Three depth layers (parallax from the camera), slow orbital drift,
// direction flips during the ghost, and a burst when the logo arrives.
// Procedural per-pixel - no point buffers.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uTime;
uniform float uAlpha;    // master overlay alpha
uniform float uFlow;     // -1..1 stream direction (flips at the ghost)
uniform float uBurst;    // 0..1 outward burst (logo arrival)
uniform float uStream;   // 0..1 build-up: particles align into directional streams
uniform vec2 uParallax;

out vec4 fragColor;

void main() {
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec3 col = vec3(0.0);

  for (int L = 0; L < 3; L++) {
    float fl = float(L);
    float cell = 0.02 + fl * 0.014;
    vec2 id = floor(p / cell);
    float h = hash12(id + fl * 9.1);
    vec2 jitter = vec2(hash12(id * 1.7 + fl * 3.3), hash12(id * 2.3 + fl * 7.7));
    vec2 pos = (id + jitter) * cell;

    float t = uTime * (0.15 + fl * 0.1) + h * 40.0;
    // drift: primary stream + gentle orbital sway. During the build-up
    // (uStream -> 1) the stream dominates and the sway collapses, so the
    // particles read as coherent directional streams.
    float sway = 1.0 - uStream * 0.85;
    pos.x += (uFlow * (0.35 + uStream * 0.9) + sin(t * 0.8 + h * 6.0) * 0.5 * sway) * cell * (1.0 + fl * 0.6);
    pos.y += sin(t * 0.6 + h * 3.0) * cell * 0.7 * sway;

    // parallax depth
    pos += uParallax * (0.15 + fl * 0.12);

    // burst: particles blow outward from the center at the logo arrival
    if (uBurst > 0.001) {
      vec2 toC = pos - vec2(0.0, 0.1);
      pos += normalize(toC + 1e-4) * uBurst * (0.3 + h * 0.9) * cell * 4.0;
    }

    // streak falloff: stretch along the flow axis when streaming
    vec2 dpos = p - pos;
    float m;
    if (uStream > 0.01) {
      vec2 axis = vec2(uFlow, 0.0);
      float along = dot(dpos, axis);
      float perp = length(dpos - axis * along);
      m = exp(-(abs(along) * 70.0 + perp * 260.0)) * (0.3 + 0.7 * h);
    } else {
      float d = length(dpos);
      m = exp(-d * 150.0) * (0.3 + 0.7 * h);
    }
    // twinkle
    m *= 0.7 + 0.3 * sin(uTime * 3.0 + h * 20.0);
    col += vec3(0.5, 0.9, 1.0) * m * uAlpha * (0.5 + fl * 0.25);
  }

  fragColor = vec4(col, 1.0);
}
