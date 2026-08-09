#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 1 - GridRenderer / System Wakeup
// Black field with faint digital noise + CRT glow. A technical grid
// materializes outward from the center, thin hairlines fade in, concentric
// circles expand slowly. uWake drives the whole materialization; uParallax
// (from the camera drift) gives the plane subtle depth.
// 0:00 black -> 0:03 wakeup
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uWake;       // 0..1 materialization envelope
uniform vec2 uParallax;    // camera-driven offset (slow drift)
uniform float uTime;
uniform float uIntensity;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;  // aspect-corrected, center origin
  vec2 pp = p - uParallax * 0.08;

  // faint digital noise floor (barely-there before uWake)
  float noise = vnoise2(pp * 6.0 + uTime * 0.04);
  vec3 col = vec3(0.012 + 0.02 * noise);

  // concentric circles expanding (fade in 0:03)
  float rings = 0.0;
  for (int i = 0; i < 4; i++) {
    float fi = float(i);
    float r = 0.12 + fi * 0.11 + uWake * 0.45;
    float rr = abs(length(pp) - r);
    rings += exp(-rr * 90.0) * (0.5 + 0.5 * sin(uTime * 0.8 + fi * 2.1));
  }
  rings *= uWake * 0.22;

  // technical grid materializing from the center outward
  float gridLine = 0.0;
  {
    vec2 g = abs(fract(pp / 0.14 - uTime * 0.012) - 0.5);
    float line = smoothstep(0.045, 0.0, min(g.x, g.y));
    float gdist = length(pp);
    float reveal = smoothstep(0.85, 0.08, gdist) * uWake;
    gridLine = line * reveal;
    // sparse grid nodes
    float node = step(0.93, hash12(floor(pp / 0.14) + 3.7));
    gridLine += node * smoothstep(0.85, 0.15, gdist) * uWake * 0.5;
  }

  // thin hairlines near the frame edges
  float hl = 0.0;
  hl += exp(-abs(abs(pp.x) - 0.9) * 260.0) * uWake;
  hl += exp(-abs(abs(pp.y) - 0.5) * 320.0) * uWake;

  vec3 cyan = vec3(0.2, 0.85, 1.0);
  col += cyan * (rings * 0.5 + gridLine * 0.22 + hl * 0.35);

  // --- status LEDs: a small row of blinking dots (awakening) ---------------
  {
    for (int i = 0; i < 7; i++) {
      float fi = float(i);
      vec2 lp = vec2(-0.28 + fi * 0.085, -0.36);
      float phase = hash12(vec2(fi * 3.1, 7.7));
      float blink = step(0.55, 0.5 + 0.5 * sin(uTime * (1.2 + phase * 2.0) + phase * 40.0));
      float led = exp(-length(pp - lp) * 900.0) * blink * uWake;
      col += vec3(0.3, 1.0, 0.8) * led * 0.5;                 // soft green LEDs
      col += vec3(0.85, 1.0, 1.0) * exp(-length(pp - lp) * 1600.0) * blink * uWake * 0.35;
    }
  }

  // --- occasional subtle data pulses travelling the bottom hairline --------
  {
    float ph = fract(uTime * 0.12);                            // ~8s loop
    vec2 dp = vec2(-0.9 + ph * 1.8, -0.47);
    float occ = smoothstep(0.0, 0.12, ph) * (1.0 - smoothstep(0.8, 0.95, ph));
    col += vec3(0.2, 0.85, 1.0) * exp(-length(pp - dp) * 700.0) * occ * uWake * 0.5;
    col += vec3(0.9, 1.0, 1.0) * exp(-length(pp - dp) * 2000.0) * occ * uWake * 0.3;
  }

  // subtle CRT glow toward the edges
  col += vec3(0.04, 0.09, 0.11) * (1.0 - smoothstep(0.25, 1.0, length(p))) * uWake;

  fragColor = vec4(col, 1.0);
}
