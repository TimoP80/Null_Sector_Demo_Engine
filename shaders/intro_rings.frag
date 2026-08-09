#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 2 - CircularScanner / Diagnostics rings
// Two presentation modes driven by uniforms:
//   uQuiet=1  lone awakening scanner: one slow rotating line + faint ring,
//             centered - the "nearly black, one scanner" 0:00-0:20 look.
//   uQuiet=0  full diagnostics cluster: rotating arcs, radial scanner with
//             ticks, oscilloscope - the 0:21-0:48 communication phase.
// uBuild (0..1, build-up ramp from 0:49) speeds the rotation and expands the
// arcs - the interface losing its composure.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uDiag;       // 0..1 envelope (lone scanner or diagnostics)
uniform float uQuiet;      // 1 = lone awakening scanner, 0 = full cluster
uniform float uBuild;      // 0..1 build-up (speed / expansion boost)
uniform vec2 uParallax;
uniform float uTime;
uniform float uSeed;

out vec4 fragColor;

/** arc segment of radius r between angles a0..a1 (radians), edge width w */
float arc(vec2 p, float r, float a0, float a1, float w) {
  float d = abs(length(p) - r);
  float a = atan(p.y, p.x);
  float inA = step(a0, a) * (1.0 - step(a1, a));
  if (a1 < a0) inA = clamp((a >= a0 ? 1.0 : 0.0) + (a <= a1 ? 1.0 : 0.0), 0.0, 1.0);
  return exp(-d * w) * inA;
}

void main() {
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec3 col = vec3(0.0);
  float dg = uDiag;
  if (dg < 0.001) { fragColor = vec4(0.0); return; }

  // --- lone awakening scanner (0:00-0:20): centered, slow, minimal ----------
  if (uQuiet > 0.5) {
    vec2 q = p - uParallax * 0.16;
    float sa = uTime * 0.4;
    float a = atan(q.y, q.x);
    float dAng = abs(mod(a - sa + PI, TAU) - PI);
    col += vec3(0.6, 1.0, 1.0) * exp(-dAng * 70.0) * dg * 0.55;
    col += vec3(0.2, 0.75, 1.0) * exp(-dAng * 14.0) * dg * 0.22;
    col += vec3(0.15, 0.6, 0.85) * exp(-abs(length(q) - 0.3) * 180.0) * dg * 0.35;
    col += vec3(0.1, 0.5, 0.7) * exp(-abs(length(q) - 0.42) * 200.0) * dg * 0.15;
    fragColor = vec4(col, 1.0);
    return;
  }

  // --- full diagnostics cluster (0:21+) -------------------------------------
  vec2 c = vec2(-0.62, 0.18) - uParallax * 0.16;
  vec2 q = p - c;
  float spd = 1.0 + uBuild * 2.5;          // scanner accelerates in build-up
  float grow = 1.0 + uBuild * 0.9;         // circular elements expand

  // rotating arcs (three rings, independent speeds / gaps)
  for (int i = 0; i < 3; i++) {
    float fi = float(i);
    float r = (0.07 + fi * 0.045) * grow;
    float sp = (0.5 + fi * 0.33) * spd;
    float a0 = uTime * sp * (i == 1 ? -1.0 : 1.0) + fi * 2.1;
    float a1 = a0 + 4.3;  // ~246 deg sweep, leaves a gap
    col += vec3(0.2, 0.85, 1.0) * arc(q, r, a0, a1, 120.0) * dg * 0.5;
  }

  // radial scanner: rotating bright line + gradient trail + ticks
  {
    float sa = uTime * (0.9 + uBuild * 2.4);
    float a = atan(q.y, q.x);
    float dAng = abs(mod(a - sa + PI, TAU) - PI);
    float line = exp(-dAng * 90.0);
    float trail = exp(-dAng * 12.0) * 0.4;
    col += vec3(0.7, 1.0, 1.0) * (line + trail) * dg * 0.8;

    // tick marks around the ring
    for (int i = 0; i < 12; i++) {
      float ta = float(i) / 12.0 * TAU;
      vec2 tp = vec2(cos(ta), sin(ta)) * 0.075;
      float td = length(q - tp);
      col += vec3(0.3, 0.7, 0.9) * exp(-td * 260.0) * dg * 0.5;
    }
    // faint disc rim
    col += vec3(0.15, 0.6, 0.85) * exp(-abs(length(q) - 0.075) * 200.0) * dg * 0.6;
  }

  // oscilloscope waveform in a small frame below the scanner
  {
    vec2 wc = c - vec2(0.0, 0.24);
    vec2 wq = p - wc;
    float fx = wq.x / 0.22;               // -1..1 across the box
    float t = uTime * 1.4;
    float bass = Null.uBass;
    float sig = 0.5 * sin(fx * 5.0 + t) + 0.3 * sin(fx * 11.0 - t * 1.7) + 0.2 * sin(fx * 23.0 + t * 0.7);
    sig = sig * (0.35 + 0.65 * bass) + 0.1 * Null.uOnset * sin(fx * 31.0 + uTime * 9.0);
    float y = sig * 0.035;
    float line = exp(-abs(wq.y - y) * 160.0);
    col += vec3(0.25, 0.9, 1.0) * line * dg * 0.9;
    // frame
    float fr = max(abs(wq.x), abs(wq.y));
    col += vec3(0.15, 0.6, 0.85) * exp(-abs(fr - 0.11) * 260.0) * dg * 0.7;
    // moving scan dot
    vec2 dp = vec2(fx * 0.22, y);
    col += vec3(0.9, 1.0, 1.0) * exp(-length(wq - dp) * 300.0) * dg * 0.8;
  }

  fragColor = vec4(col, 1.0);
}
