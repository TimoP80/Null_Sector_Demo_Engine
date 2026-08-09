#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 7 - GhostPass: the interface starts misbehaving (0:15).
// Screen-space distortion ripples travel across the composed frame and a
// human silhouette almost materializes - then dissolves. It is never fully
// revealed: the body is eroded by noise, sliced by scanlines and glitches.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTex;   // composed scene (unit 0)
uniform vec2 uRes;
uniform float uTime;
uniform float uGhost;     // 0..1 presence envelope
uniform vec2 uParallax;

out vec4 fragColor;

/** signed distance to a soft 2D humanoid silhouette centered at origin.
 *  Stylized: head + shoulders + torso + hint of arms. */
float silDist(vec2 p) {
  // head
  float head = length(p - vec2(0.0, 0.34)) - 0.075;
  // torso
  float torso = length(p - vec2(0.0, 0.0)) - 0.09;
  torso = max(torso, abs(p.y + 0.02) - 0.16);  // straighten the bottom
  // shoulders: a wider block across the top of the torso
  float shoulder = max(abs(p.x) - 0.16, 0.12 - p.y);
  shoulder = max(shoulder, p.y - 0.2);
  // combine: union of head, torso band, shoulder block
  float d = min(min(head, torso), shoulder);
  return d;
}

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  vec2 c = vec2(0.0, 0.06) + uParallax * 0.4;

  // --- distortion ripples traveling outward from the silhouette -------------
  vec2 rv = uv - (c * 0.5 + 0.5);
  float rr = length(rv);
  float ripple = sin(rr * 55.0 - uTime * 9.0) * exp(-rr * 4.5);
  vec2 dUV = uv + normalize(rv + 1e-4) * ripple * uGhost * 0.03;
  // secondary ripple from a drifting point
  vec2 c2 = vec2(0.2, 0.35) + 0.15 * vec2(sin(uTime * 0.5), cos(uTime * 0.4));
  vec2 rv2 = uv - c2;
  float rr2 = length(rv2);
  dUV += normalize(rv2 + 1e-4) * sin(rr2 * 90.0 - uTime * 13.0) * exp(-rr2 * 6.0) * uGhost * 0.012;

  // --- glitch slices (horizontal displacement bands) ------------------------
  float slice = hash12(vec2(floor(uv.y * 42.0), floor(uTime * 9.0)));
  dUV.x += (slice - 0.5) * step(0.86, slice) * uGhost * 0.02;
  float slice2 = hash12(vec2(floor(uv.y * 17.0 + uTime), floor(uTime * 5.0)));
  dUV.x += (slice2 - 0.5) * step(0.95, slice2) * uGhost * 0.05;

  // --- sample the distorted scene -------------------------------------------
  dUV = clamp(dUV, 0.0, 1.0);
  vec3 col = texture(uTex, dUV).rgb;

  // --- the ghost silhouette ---------------------------------------------------
  if (uGhost > 0.01) {
    vec2 sp = p - c;
    sp.y /= 0.9;  // subtle aspect
    float d = silDist(sp);

    // presence envelope in time (fades in and out)
    float pres = uGhost;
    // the body is eroded by noise - it never forms cleanly
    float n = fbm2(sp * 14.0 + uTime * 1.2);
    float edge = smoothstep(0.012, -0.006, d);
    edge *= 0.35 + 0.65 * n;
    // scanline slicing
    float scan = 0.5 + 0.5 * sin(sp.y * 500.0 + uTime * 20.0);
    edge *= 0.5 + 0.5 * scan * pres;
    // vertical wipe: assembles top-down, never completes
    float wipe = smoothstep(-0.3, 0.05, sp.y) * (1.0 - smoothstep(0.32, 0.36, sp.y));
    edge *= wipe;

    // cyan interference glow + faint white core
    vec3 ghostCol = vec3(0.25, 0.9, 1.0) * edge * pres * 0.5;
    ghostCol += vec3(0.9, 1.0, 1.0) * pow(edge * pres, 3.0) * 0.35;
    // rgb split ghosting
    ghostCol += vec3(1.0, 0.2, 0.4) * edge * pres * 0.08;

    col += ghostCol;
  }

  // --- micro glitch chroma shift ---------------------------------------------
  col += (hash12(uv * 700.0 + vec2(uTime * 40.0)) - 0.5) * uGhost * 0.02;

  fragColor = vec4(col, 1.0);
}
