#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 3 - FFTBars: procedural spectrum bars along the bottom edge.
// Heights come from a hash lattice driven by time + the audio analyser
// (bass lifts the low end, onset spikes everything), with beat pulses.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uDiag;
uniform float uTime;
uniform float uSeed;

out vec4 fragColor;

void main() {
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec3 col = vec3(0.0);
  if (uDiag < 0.001) { fragColor = vec4(0.0); return; }

  const int N = 24;
  float bandW = 0.018;
  float gap = 0.006;
  float x0 = -0.5 * float(N) * (bandW + gap);
  float baseY = -0.46;

  for (int i = 0; i < N; i++) {
    float fi = float(i);
    float bx = x0 + fi * (bandW + gap) + bandW * 0.5;
    // hash lattice stepping ~3x/sec
    float h = hash12(vec2(fi * 1.7, uSeed + floor(uTime * 3.0) * 0.31));
    float energy = mix(0.3, 1.0, Null.uBass * 0.55 + Null.uOnset * 0.9);
    // low bars ride the bass harder
    float lowW = 1.0 - sat01(fi / float(N) * 1.6) * 0.5;
    float pulse = 0.75 + 0.35 * Null.uPulse * step(0.7, h);
    float bh = 0.05 + h * 0.17 * energy * lowW * pulse;

    float top = baseY + bh;
    float m = step(abs(p.x - bx), bandW * 0.5)
            * smoothstep(baseY, baseY + 0.006, p.y)
            * (1.0 - smoothstep(top - 0.006, top, p.y));
    col += vec3(0.1, 0.7, 1.0) * m * uDiag * (0.4 + 0.6 * h);
    // bright cap
    col += vec3(0.85, 1.0, 1.0) * step(abs(p.x - bx), bandW * 0.5)
         * (1.0 - smoothstep(top - 0.004, top, p.y)) * uDiag * 0.7;
  }

  // baseline
  col += vec3(0.2, 0.75, 1.0) * exp(-abs(p.y - baseY) * 400.0) * uDiag * 0.4;

  fragColor = vec4(col, 1.0);
}
