#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 2 // MEMORY CORE
// ---------------------------------------------------------------------------
// The fragmented geometry condenses into a gigantic spherical neural network:
// ~90 nodes distributed on a shell, connected by glowing synapses around a
// central core that pulses with the bass. The camera approaches and flies
// through the core.
//
// Audio reactivity (all data-driven, no keyframing in the script):
//   uBass     -> core scale + node brightness + network deformation
//   uFlash    -> per-kick strobe (node + connection flash)
//   uPulse    -> beat pulse on connection radii + core glow
//   uHigh     -> particle emission sparkle
//   uOnset    -> transient ripple through the shell
//   musicHue  -> per-bar palette transition (engine chord clock)
//
// Renders into a reduced target (renderScale 0.6 - the .nsd), march 48 steps,
// node data computed once per frame (same pattern as neuralnet.frag).
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;      // per-kick strobe (audio kick analyser)
uniform float uMode;
uniform float uHigh;       // react.high (treble band)
uniform vec2  uSceneRes;   // ACTUAL render target size (reduced scale)
uniform float uVolume;     // react.energy

out vec4 fragColor;

const int N = 56;   // 56 shell nodes: dense enough at 0.6 scale, 22% cheaper

float sdSphere(vec3 p, float r) { return length(p) - r; }
float sdCylinder(vec3 p, vec3 a, vec3 b, float r) {
  vec3 ab = b - a;
  vec3 ap = p - a;
  float t = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
  return length(p - a - ab * t) - r;
}

/** node i on the shell: fibonacci-sphere + hash jitter + bass deformation */
void nodeData(int i, float bass, float pulse, float time, out vec3 pos, out float rad) {
  float fi = float(i);
  float golden = 2.3999632297;                      // golden angle
  float y = 1.0 - 2.0 * (fi + 0.5) / float(N);
  float r = sqrt(max(0.0, 1.0 - y * y));
  float th = fi * golden;
  vec3 base = vec3(cos(th) * r, y, sin(th) * r) * 8.6;
  vec3 jit = vec3(hash13(vec3(fi, 1.0, 0.0)) - 0.5,
                  hash13(vec3(fi, 2.0, 0.0)) - 0.5,
                  hash13(vec3(fi, 3.0, 0.0)) - 0.5) * 1.6;
  // per-node slow orbit + bass pushes the shell outward / inward in waves
  float wob = sin(time * 0.35 + fi * 0.31 + hash13(vec3(fi, 4.0, 0.0)) * 6.28)
              + Null.uOnset * 1.4 * sin(time * 9.0 + fi * 0.9);   // snare ripple
  vec3 norm = normalize(base + 1e-4);
  float deform = 1.0 + 0.10 * wob + bass * 0.35 * (0.4 + 0.6 * hash13(vec3(fi, 5.0, 0.0)));
  pos = (base * deform + jit) + norm * pulse * 0.5;
  rad = 0.30 + 0.22 * hash13(vec3(fi, 6.0, 0.0));
}

/** connection partner index (deterministic mesh-ish adjacency) */
int nbr(int i, int k) { return (i + k * 17 + 1) % N; }

/** map: shell nodes + synapses + core + emission sparks */
float map(vec3 p, vec3 nodes[N], float rads[N], vec3 sparks[12],
          float bass, float pulse, float time, float flash, float inside,
          out float matID, out float glow) {
  glow = 0.0;
  float d = 1e9;
  // INSIDE the core sphere the node shell (radius ~8.6) and its synapses are
  // always farther than the far inner wall (~4 units), so evaluating all N
  // nodes per march step is pure waste - skip them (the wall + sparks are the
  // only geometry reachable from inside). This is what keeps the fly-through
  // at 60 FPS: it removes ~85% of the per-step distance evals.
  if (inside > 0.5) {
    float coreR = 2.05 * (1.0 + bass * 0.32 + pulse * 0.10 + flash * 0.06);
    float cd = coreR - length(p);
    matID = 2.0;
    for (int s = 0; s < 12; s++) {
      float sd = length(p - sparks[s]) - 0.045;
      if (sd < cd) { cd = sd; matID = 3.0; }
    }
    return cd;
  }
  int nearest = 0;
  float nearestD = 1e9;
  for (int i = 0; i < N; i++) {
    vec3 nq = nodes[i];
    float dist = length(p - nq);
    float nd = dist - rads[i];
    if (nd < d) { d = nd; nearest = i; }
    if (dist < nearestD) { nearestD = dist; nearest = i; }
  }
  // synapses from the nearest node
  int nc = 3;
  for (int k = 0; k < nc; k++) {
    int j = nbr(nearest, k);
    if (j == nearest) continue;
    vec3 qa = nodes[nearest];
    vec3 qb = nodes[j];
    float r0 = 0.035 + 0.05 * hash13(vec3(float(nearest), float(j), 2.0));
    float grow = 0.5 + 0.5 * sin(time * (0.4 + hash13(vec3(float(nearest), float(j), 3.0)) * 0.5)
                                 + hash13(vec3(float(nearest), float(j), 4.0)) * 6.28);
    grow += pulse * 0.45 + bass * 0.4 + flash * 0.8;   // audio-reactive width
    float cr = r0 * (0.35 + 1.1 * grow);
    if (cr < 0.004) continue;
    float cd = sdCylinder(p, qa, qb, cr);
    if (cd < d) { d = cd; matID = 1.0; }
  }
  // central core: emissive sphere, scale pulses with the bass. When the
  // camera is INSIDE the sphere (inside > 0.5) flip to the shell view: the
  // march then walks outward to the far inner wall (curvature + fresnel rim
  // + parallax) instead of hitting at tt~0 and flooding the frame flat white.
  float coreR = 2.05 * (1.0 + bass * 0.32 + pulse * 0.10 + flash * 0.06);
  float cd = inside > 0.5 ? (coreR - length(p)) : sdSphere(p, coreR);
  if (cd < d) { d = cd; matID = 2.0; }
  // emission sparks streaming outward (react to uHigh + kick)
  for (int s = 0; s < 12; s++) {
    vec3 sp = sparks[s];
    float sd = length(p - sp) - 0.045;
    if (sd < d) { d = sd; matID = 3.0; }
  }
  return d;
}

void main() {
  float t = Null.uSectionLocal;
  float bass = Null.uBass;
  float pulse = Null.uPulse;
  float flash = uFlash;
  float time = Null.uTime;

  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // inside-core flag for the shell-view SDF (driven by the same core radius
  // formula the map uses, so the flip happens exactly at the surface)
  float insideCore = length(ro) < 2.05 * (1.0 + bass * 0.32 + pulse * 0.10 + flash * 0.06)
                         ? 1.0 : 0.0;

  // --- precompute per-frame state -------------------------------------------
  vec3 nodes[N];
  float rads[N];
  for (int i = 0; i < N; i++) nodeData(i, bass, pulse, time, nodes[i], rads[i]);
  // emission sparks: born at the core surface, stream outward
  vec3 sparks[12];
  for (int s = 0; s < 12; s++) {
    float fs = float(s);
    vec3 dir = normalize(vec3(hash13(vec3(fs, 7.0, 1.0)) - 0.5,
                              hash13(vec3(fs, 8.0, 1.0)) - 0.5,
                              hash13(vec3(fs, 9.0, 1.0)) - 0.5));
    float speed = 1.0 + hash13(vec3(fs, 10.0, 1.0)) * 2.0;
    float rad = 2.3 + fract(time * speed * 0.35 + fs * 0.13) * 7.0;
    sparks[s] = dir * rad;
  }

  // --- ray march (skip rays that miss the whole network) -----------------------
  float tmax = 60.0;
  float tt = 0.0;
  vec3 p = ro;
  float matID = 0.0;
  float hit = 0.0;
  vec3 bsphere = vec3(0.0);
  float bR = 11.0;
  vec3 oc = ro - bsphere;
  float bq = dot(oc, rd);
  float bcc = dot(oc, oc) - bR * bR;
  float bdisc = bq * bq - bcc;
  bool marchable = bdisc > 0.0 && (-bq - sqrt(max(bdisc, 0.0))) < tmax;
  if (marchable) {
    tt = max(0.0, -bq - sqrt(bdisc));
    for (int i = 0; i < 34; i++) {
      p = ro + rd * tt;
      float g = 0.0;
      float d = map(p, nodes, rads, sparks, bass, pulse, time, flash, insideCore, matID, g);
      if (d < 0.004 * max(tt, 1.0)) { hit = 1.0; break; }
      tt += max(d * 0.8, 0.05);
      if (tt > tmax) break;
    }
  }

  // --- background -------------------------------------------------------------
  vec3 col = vec3(0.004, 0.002, 0.012);
  // ambient distant-node glow (single pass, miss path only)
  float glowAcc = 0.0;
  for (int i = 0; i < N; i++) {
    vec3 dv = nodes[i] - ro;
    float d2 = dot(dv, dv);
    if (d2 > 16.0) {
      float dist = sqrt(d2);
      // glow falloff exp(-0.22*d) ~ 1e-3 already at d=30 and the ang^2 term
      // is <=1: skipping the transcendental for far nodes is invisible
      if (dist > 30.0) continue;
      float ang = max(dot(rd, dv / dist), 0.0);
      glowAcc += ang * ang * exp(-dist * 0.22) * 0.14;
    }
  }
  col += palVoid(musicHue(0.1)) * glowAcc * (0.5 + 0.5 * pulse);
  float cg = length(ro);
  float coreGlow = exp(-abs(cg - 2.4) * 0.5) * (0.4 + bass * 0.8);
  col += palVoid(musicHue(0.2)) * coreGlow * 0.4;
  // horizon-less void gradient
  col += vec3(0.02, 0.015, 0.05) * pow(1.0 - abs(uv.y), 2.0) * 0.6;

  if (hit > 0.5) {
    vec3 n;
    {
      const float eps = 0.002;
      float id, g;
      vec2 e = vec2(1.0, -1.0) * 0.5773 * eps;
      n = e.xyy * map(p + e.xyy, nodes, rads, sparks, bass, pulse, time, flash, insideCore, id, g);
      n += e.yyx * map(p + e.yyx, nodes, rads, sparks, bass, pulse, time, flash, insideCore, id, g);
      n += e.yxy * map(p + e.yxy, nodes, rads, sparks, bass, pulse, time, flash, insideCore, id, g);
      n += e.xxx * map(p + e.xxx, nodes, rads, sparks, bass, pulse, time, flash, insideCore, id, g);
      n = normalize(n);
      // camera flying THROUGH the core: at the instant the ray origin is
      // inside the sphere the tetrahedral normal vanishes (all four map()
      // samples are ~equal) and ro - p ~ 0, both of which go NaN and blow
      // the frame white. Fall back to the view direction (a camera-facing
      // fresnel rim) - visually fine, numerically safe.
      if (dot(n, n) < 1e-6) n = -rd;
    }
    vec3 dP = ro - p;
    vec3 V = dot(dP, dP) > 1e-8 ? normalize(dP) : -rd;
    float fres = pow(1.0 - max(dot(n, V), 0.0), 2.0);

    if (matID > 2.5) {
      // emission spark: tiny hot point
      col = vec3(1.0, 0.92, 1.0) * (0.6 + 0.6 * uHigh) * (0.8 + 0.6 * flash);
    } else if (matID > 1.5) {
      // --- central core -----------------------------------------------------
      float h = hash13(vec3(floor(p * 3.0)));
      float turb = fbm3q(p * 1.4 + time * 0.2);
      vec3 coreCol = mix(vec3(0.9, 0.97, 1.0), palVoid(musicHue(0.15)), 0.35 + 0.3 * turb);
      float pulseGlow = 0.75 + 0.45 * bass + 0.3 * pulse + 0.25 * flash;
      // interior view (camera inside the sphere): the shell fills the whole
      // FOV, so drop the HDR body to a readable level or the passage is a flat
      // whiteout - the fresnel rim + turbulence then carry the structure
      float body = insideCore > 0.5 ? (0.30 + 0.45 * turb) : (1.7 + 2.1 * turb);
      col = coreCol * body * pulseGlow;
      col += vec3(1.0, 0.99, 1.0) * fres * (insideCore > 0.5 ? 1.8 : 2.4) * (1.0 + flash);
      col += palVoid(musicHue(0.3)) * fres * (insideCore > 0.5 ? 0.7 : 1.2);
      col *= 1.0 + flash * 0.5;
    } else if (matID > 0.5) {
      // --- synapse ----------------------------------------------------------
      vec3 connCol = palVoid(musicHue(0.05));
      col = connCol * (0.5 + 0.7 * pulse + 0.8 * bass + 0.9 * flash);
      col += vec3(1.0, 0.95, 1.0) * flash * 0.7;
    } else {
      // --- node -------------------------------------------------------------
      float h = hash13(vec3(floor(p * 8.0)));
      vec3 nodeCol = palVoid(musicHue(h * 0.1));
      float bright = 0.55 + 0.45 * sin(time * 2.2 + h * 6.28);
      bright += bass * 0.5 + pulse * 0.3 + flash * 0.8;
      col = nodeCol * bright;
      col += vec3(1.0, 0.97, 1.0) * fres * (0.6 + bass) * 0.6;
      col += vec3(1.0, 0.98, 1.0) * flash * 0.3;
    }
    // faint rim light from the core
    float dCore = length(p);
    col += palVoid(musicHue(0.2)) * exp(-dCore * 0.25) * 0.4;
    // distance fog to void
    float fog = 1.0 - exp(-tt * 0.022);
    col = mix(col, vec3(0.006, 0.004, 0.02), fog);
  }

  // --- SYSTEM FAILURE mode (uMode 1): the network tears itself apart ---------
  if (uMode > 0.5) {
    float part = glitchParticipation(1.0, flash, bass, 1.0);
    float g = hash13(floor(p * 2.0 + floor(time * 8.0) * vec3(5.0, 11.0, 1.0)));
    float glitch = step(max(0.55, 0.96 - part * 0.41), g);
    col = mix(col, palVoid(g + musicHue()) * 1.6, glitch * 0.85);
    col *= 0.55 + 0.45 * sin(time * 30.0 + length(p) * 2.0);
    col += vec3(1.0, 0.6, 1.0) * flash * 0.6;
  }

  // kick strobe over everything
  col *= 1.0 + flash * 0.35;
  col += vec3(0.9, 0.97, 1.0) * flash * 0.15;

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hit > 0.5 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
