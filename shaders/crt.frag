#version 300 es
// CRT power-on: expanding scanline + white flash + persistent flicker.
#include <common>

uniform vec2 uRes;
uniform float uTime;
uniform float uT; // 0..1 through the intro

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float bright = 0.0;

  // phase 1: vertical scanline sweeps from top to center
  if (uT < 0.3) {
    float y = 1.0 - (uT / 0.3) * 0.55;
    bright = exp(-abs(uv.y - y) * 90.0) * 1.3;
    bright += exp(-abs(uv.y - y + 0.02) * 500.0) * 0.35;
    bright += exp(-abs(uv.y - y - 0.015) * 300.0) * 0.2;
  } else if (uT < 0.55) {
    // phase 2: screen floods with light, settling
    float k = smoothstep(0.3, 0.55, uT);
    bright = (pow(uv.y, 3.0) * 0.5 + 0.1) * k;
    bright *= 0.55 + 0.45 * sin(uTime * 70.0) * 0.5;
    bright += exp(-abs(uv.y - 0.5) * 8.0) * 0.15 * k;
  } else {
    bright = 0.0;
  }

  // subtle persistent flicker late in the intro
  float flick = 1.0;
  if (uT > 0.7) {
    flick = 0.96 + 0.04 * sin(uTime * 37.0) * sin(uTime * 91.0 + 1.0);
  }

  // faint scanlines even when idle
  float scan = 0.5 + 0.5 * sin(uv.y * uRes.y * PI);
  float a = clamp(bright * flick, 0.0, 1.0);
  vec3 col = vec3(0.8, 0.92, 1.0) * a;
  col *= 0.9 + 0.1 * scan;
  fragColor = vec4(col, a);
}
