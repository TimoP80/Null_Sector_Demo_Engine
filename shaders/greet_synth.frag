#version 300 es
// ---------------------------------------------------------------------------
// SCENE 9 - Greetings backdrop: oldschool synthwave poster.
// Scanline sun over wireframe mountains, perspective grid floor, copper
// border frame - the classic demoscene greetings board, reimagined.
// ---------------------------------------------------------------------------
#include <common>

uniform vec2 uRes;
uniform float uTime;
uniform float uIntensity;
uniform float uPulse;
uniform float uDim;    // overall dim (credits mode: the poster recedes)
uniform float uSunScale;  // 0..1 sun size (greetings leaves room for the marks)

out vec4 fragColor;

/** layered value-noise ridgeline, drifting slowly with the track */
float ridge1(vec2 q) {
  return fbm2(q * 3.0 + vec2(0.0, uTime * 0.02));
}

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float aspect = uRes.x / uRes.y;
  vec2 p = vec2(uv.x * aspect, uv.y);
  float horizon = 0.38;

  vec3 col = vec3(0.01, 0.005, 0.04);

  // --- sky gradient ----------------------------------------------------------
  float sky = smoothstep(horizon, 1.0, uv.y);
  col = mix(col, vec3(0.14, 0.04, 0.3), sky * 0.9);

  // stars (twinkle with the pulse; grid on aspect-corrected p so cells are
  // square in pixels -> round stars on any aspect ratio)
  vec2 sc = floor(p * 220.0);
  float star = step(0.9982, hash21(sc + floor(uTime * 2.0) * 0.7));
  col += vec3(0.9, 0.7, 1.0) * star * (0.4 + 0.6 * uPulse);

  // --- scanline sun ------------------------------------------------------------
  vec2 sunC = vec2(0.5 * aspect, horizon + 0.045);
  float sunD = length(p - sunC);
  float sunR = 0.085 * aspect * max(uSunScale, 0.05);
  float disc = smoothstep(sunR, sunR * 0.93, sunD);
  // horizontal scanlines across the disc
  float scan = step(0.5, fract(uv.y * 240.0)) * 0.6 + 0.4;
  // radial gradient: hot core -> cool rim
  float gn = sunD / sunR;
  vec3 sunCol = mix(vec3(1.0, 0.55, 0.3), vec3(1.0, 0.25, 0.65), smoothstep(0.0, 1.0, gn));
  sunCol = mix(sunCol, vec3(0.55, 0.1, 0.9), smoothstep(0.7, 1.4, gn));
  col += sunCol * disc * scan * (0.9 + 0.4 * uIntensity + 0.5 * uPulse);
  // halo
  col += sunCol * exp(-gn * gn * 3.0) * (0.12 + 0.2 * uPulse);

  // --- wireframe mountains ------------------------------------------------------
  float r1 = ridge1(vec2(uv.x * aspect, 0.0));
  float r2 = ridge1(vec2(uv.x * aspect * 1.7 + 5.0, 0.0));
  float m1 = horizon - r1 * 0.16;
  float m2 = horizon - 0.06 - r2 * 0.12;

  // two mountain silhouettes (fill + wireframe edges)
  float m1F = smoothstep(m1, m1 + 0.004, uv.y) * step(uv.y, horizon);
  float m2F = smoothstep(m2, m2 + 0.004, uv.y) * step(uv.y, m1);
  col += vec3(0.03, 0.01, 0.1) * (m1F + m2F) * 0.8;

  // wireframe: vertical grid + horizontal contours + ridge edges
  float vgrid = 1.0 - smoothstep(0.0, 0.02, abs(fract(uv.x * aspect * 8.0 + uTime * 0.02) - 0.5));
  float hgrid = 1.0 - smoothstep(0.0, 0.02, abs(fract(uv.y * 16.0) - 0.5));
  float m1Edge = 1.0 - smoothstep(0.0, 0.003, abs(uv.y - m1));
  float m2Edge = 1.0 - smoothstep(0.0, 0.003, abs(uv.y - m2));
  vec3 wire = vec3(0.5, 0.15, 1.0);
  col += wire * (m1F * (vgrid * 0.25 + hgrid * 0.18) + m2F * (vgrid * 0.2 + hgrid * 0.15));
  col += wire * (m1Edge * 0.9 + m2Edge * 0.7) * (0.6 + 0.4 * uIntensity);

  // --- perspective grid floor ----------------------------------------------------
  if (uv.y < horizon) {
    float gy = (horizon - uv.y) / horizon; // 0 at horizon, 1 at bottom
    float invZ = 1.0 / max(gy, 0.02);
    // verticals converge to the vanishing point
    float vline = 1.0 - smoothstep(0.0, 0.03, abs(fract((uv.x - 0.5) * invZ * 0.9 + uTime * 0.03) - 0.5));
    // horizontals recede exponentially
    float hline = 1.0 - smoothstep(0.0, 0.03, abs(fract(gy * 10.0) - 0.5));
    float grid = max(vline, hline) * smoothstep(0.0, 0.15, gy);
    // floor tint toward the sun color, brighten with the pulse
    col += mix(vec3(0.35, 0.1, 0.8), vec3(0.9, 0.3, 0.7), invZ * 0.02) * grid * (0.25 + 0.3 * uIntensity + 0.25 * uPulse);
    // sun reflection pool
    col += sunCol * exp(-pow((uv.x - 0.5) * 3.0, 2.0)) * smoothstep(0.0, 0.5, gy) * 0.06;
  }

  // --- copper border frame (angled corners) ---------------------------------------
  float m = 0.02;
  // chamfered rect: signed distance to the inset rect edges, 45deg corner cuts
  vec2 q = uv - 0.5;
  vec2 qa = abs(q);
  float corner = 0.045;
  float bx = 0.5 - m;
  float edge = max(qa.x - bx, qa.y - bx); // 0 on the inset rect boundary
  float frame = 1.0 - smoothstep(0.0, 0.006, abs(edge));
  // chamfer: cut the four corners at 45deg
  float cham = smoothstep(corner, corner + 0.006, abs(qa.x - qa.y));
  frame *= cham;

  float hue = 0.55 + musicHue() * 0.2;
  vec3 frameCol = palVoid(hue) * (0.7 + 0.5 * uPulse + 0.4 * uIntensity);
  col += frameCol * frame * 0.85;

  // vignette
  col *= 0.75 + 0.25 * (1.0 - length(uv - 0.5) * 1.3);
  col *= uDim;

  fragColor = vec4(col, 1.0);
}
