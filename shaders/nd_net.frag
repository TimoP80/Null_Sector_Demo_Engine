#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN - THE REVEAL
// ---------------------------------------------------------------------------
// Beneath the surface the ocean is revealed as another enormous neural
// network: a dense grid of nodes riding the same wave field, connected by
// glowing synapses. A large-scale pulse travels through the network synced
// to the beat. As the section progresses the network destabilizes - nodes
// jitter, connections fray - handing off into SYSTEM FAILURE.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;
uniform float uMode;
uniform float uHigh;
uniform float uTransition;
uniform sampler2D uPrevScene;
uniform vec2  uSceneRes;

out vec4 fragColor;

const int GX = 12;
const int GY = 12;

float sdSphere(vec3 p, float r) { return length(p) - r; }
float sdCylinder(vec3 p, vec3 a, vec3 b, float r) {
  vec3 ab = b - a;
  vec3 ap = p - a;
  float t = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
  return length(p - a - ab * t) - r;
}

float oceanY(vec2 xz, float t) {
  float w1 = fbm2(xz * 0.10 + vec2(3.0, 1.0));
  float w2 = fbm2(xz * 0.30 + vec2(9.0));
  return (w1 - 0.5) * 1.1 + (w2 - 0.5) * 0.45 + 0.25 * sin(xz.x * 0.25 + t * 0.5);
}

void nodeData(int gx, int gy, float t, float destab, out vec3 pos, out float rad) {
  float fx = float(gx), fy = float(gy);
  vec2 xz = vec2(fx * 2.8 - 15.4, fy * 2.8 - 15.4);
  float y = oceanY(xz, t) - 2.0;                       // just below the sheet
  // per-node bob + destabilization jitter
  float h = hash12(vec2(fx, fy) + 7.0);
  y += 0.3 * sin(t * 0.9 + fx * 1.3 + fy * 2.7 + h * 6.28);
  vec3 jit = vec3(hash12(vec2(fx, fy) + 1.0) - 0.5,
                  hash12(vec2(fx, fy) + 2.0) - 0.5,
                  hash12(vec2(fx, fy) + 3.0) - 0.5) * (0.3 + 2.2 * destab);
  pos = vec3(xz.x, y, xz.y) + jit;
  rad = 0.16 + 0.12 * hash12(vec2(fx, fy) + 5.0);
}

float map(vec3 p, vec3 nodes[144], float rads[144], float t,
          float destab, float pulse, out float matID, out float glow) {
  glow = 0.0;
  float d = 1e9;
  int nearest = 0;
  float nearestD = 1e9;
  for (int i = 0; i < 144; i++) {
    vec3 nq = nodes[i];
    float dist = length(p - nq);
    float nd = dist - rads[i];
    if (nd < d) { d = nd; nearest = i; }
    if (dist < nearestD) { nearestD = dist; nearest = i; }
    if (dist > 2.0) {
      float ang = max(dot(p - nq, nq) / max(dist, 1e-3), 0.0);
      glow += ang * exp(-dist * 0.2) * 0.05;
    }
  }
  // grid links from the nearest node: right + up + diagonal
  int gx = nearest % GX;
  int gy = nearest / GX;
  for (int k = 0; k < 3; k++) {
    int nx = gx, ny = gy;
    if (k == 0) nx = gx + 1;
    else if (k == 1) ny = gy + 1;
    else { nx = gx + 1; ny = gy + 1; }
    if (nx >= GX || ny >= GY) continue;
    int j = ny * GX + nx;
    vec3 qa = nodes[nearest];
    vec3 qb = nodes[j];
    float r0 = 0.02 + 0.03 * hash12(vec2(float(gx), float(gy)) + 9.0);
    float grow = 0.4 + 0.8 * pulse + 0.3 * Null.uBass + 0.5 * uFlash;
    // fraying: connections thin as the network destabilizes
    float fray = 1.0 - 0.65 * destab * hash12(vec2(float(gx), float(gy)) + 11.0);
    float cr = r0 * grow * fray;
    if (cr < 0.003) continue;
    float cd = sdCylinder(p, qa, qb, cr);
    if (cd < d) { d = cd; matID = 1.0; }
  }
  // the surface sheet far overhead: a faint glowing ceiling (depth cue)
  float sheet = p.y - oceanY(p.xz, t);
  if (sheet < d && p.y > 2.0) { d = sheet; matID = 2.0; }
  return d;
}

void main() {
  float t = Null.uSectionLocal;
  float time = Null.uTime;
  float secT = sat01(t / max(Null.uSectionDur, 1e-4));
  float destab = smoothstep(0.15, 0.95, secT);         // destabilize ramp
  if (uMode > 0.5) destab = max(destab, 0.92);           // failure: already torn
  float pulse = 0.25 + 1.1 * Null.uPulse + 0.55 * Null.uBass;

  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;

  // destabilization: view ripple tears
  uv += (vec2(hash12(vec2(floor(time * 6.0), 1.0)), hash12(vec2(floor(time * 6.0), 2.0))) - 0.5)
      * destab * 0.05;

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // precompute nodes
  vec3 nodes[144];
  float rads[144];
  for (int i = 0; i < 144; i++) nodeData(i % GX, i / GX, time, destab, nodes[i], rads[i]);

  float tt = 0.0;
  vec3 p = ro;
  float matID = 0.0;
  float hit = 0.0;
  float glowAcc = 0.0;
  for (int i = 0; i < 48; i++) {
    p = ro + rd * tt;
    float g = 0.0;
    float d = map(p, nodes, rads, time, destab, pulse, matID, g);
    glowAcc += g * 0.05;
    if (d < 0.004 * max(tt, 1.0)) { hit = 1.0; break; }
    tt += max(d * 0.8, 0.05);
    if (tt > 60.0) break;
  }

  vec3 col = vec3(0.003, 0.006, 0.016);
  col += palVoid(musicHue(0.15)) * glowAcc * (0.5 + 0.5 * Null.uPulse);
  // pulse glow in the void
  vec2 pc = vec2(7.0 * sin(time * 0.22), 7.0 * cos(time * 0.18));
  float pd = length(ro.xz - pc);
  col += palVoid(musicHue(0.3)) * exp(-pd * 0.12) * (0.2 + 0.8 * pulse) * 0.5;

  if (hit > 0.5) {
    vec3 n;
    {
      const float eps = 0.002;
      float id, g;
      vec2 e = vec2(1.0, -1.0) * 0.5773 * eps;
      n = e.xyy * map(p + e.xyy, nodes, rads, time, destab, pulse, id, g);
      n += e.yyx * map(p + e.yyx, nodes, rads, time, destab, pulse, id, g);
      n += e.yxy * map(p + e.yxy, nodes, rads, time, destab, pulse, id, g);
      n += e.xxx * map(p + e.xxx, nodes, rads, time, destab, pulse, id, g);
      n = normalize(n);
    }
    vec3 V = normalize(ro - p);
    float fres = pow(1.0 - max(dot(n, V), 0.0), 2.0);

    if (matID > 1.5) {
      // surface sheet overhead: faint glowing ceiling
      col = palVoid(musicHue(0.2)) * (0.10 + 0.2 * pulse) + vec3(0.9, 0.97, 1.0) * 0.06;
    } else if (matID > 0.5) {
      // synapse
      vec3 connCol = palVoid(musicHue(0.1));
      col = connCol * (0.4 + 0.8 * pulse + 0.4 * uFlash);
      col += vec3(1.0, 0.96, 1.0) * uFlash * 0.5;
      // fraying synapses spark
      col += vec3(1.0, 0.8, 1.0) * step(0.9, hash12(floor(p.xz * 2.0) + time)) * destab * 0.6;
    } else {
      // node
      float h = hash12(floor(p.xz * 4.0) + 3.0);
      vec3 nodeCol = palVoid(musicHue(h * 0.1));
      float bright = 0.35 + 1.1 * pulse + 0.3 * uFlash + 0.2 * Null.uBass;
      col = nodeCol * bright + vec3(1.0, 0.98, 1.0) * fres * 0.8 * (0.5 + pulse);
      col += vec3(1.0, 0.98, 1.0) * uFlash * 0.25;
    }
    float fog = 1.0 - exp(-tt * 0.03);
    col = mix(col, vec3(0.004, 0.01, 0.03), fog);
  }

  // destabilization: chromatic sparks + glitch
  if (destab > 0.01) {
    col *= 1.0 + uFlash * 0.3 * destab;
    col += palVoid(musicHue(0.4) + destab) * destab * (0.2 + 0.3 * uFlash);
    // beat-locked slice tear (shared model)
    vec2 gs = glitchSlice(gl_FragCoord.y / res.y, 32.0, destab, uFlash, Null.uBass, 17.3, destab);
    uv.x += gs.x * 0.08;
    col += palVoid(musicHue(0.4) + gs.y) * abs(gs.x) * 3.0 * destab;
  }

  // handoff from the ocean particles
  if (uTransition < 0.999) {
    vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
    col = mix(prev, col, uTransition);
  }

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hit > 0.5 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
