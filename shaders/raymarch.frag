#version 300 es
// ---------------------------------------------------------------------------
// SCENE 4 - Raymarched Geometry
// SDF union: sphere, torus, boxes + Menger sponge, twisting, displacement,
// animated by music. PBR-ish metals with soft shadows + AO.
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)

out vec4 fragColor;

// --- sdf primitives ----------------------------------------------------------
float sdSphere(vec3 p, float r) { return length(p) - r; }
float sdBox(vec3 p, vec3 b) {
  vec3 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float sdTorus(vec3 p, vec2 t) {
  vec2 q = vec2(length(p.xz) - t.x, p.y);
  return length(q) - t.y;
}

// menger sponge (classic 3-level)
float menger(vec3 p) {
  float d = sdBox(p, vec3(1.0));
  float s = 1.0;
  for (int m = 0; m < 3; m++) {
    vec3 a = mod(p * s, 2.0) - 1.0;
    s *= 3.0;
    vec3 r = abs(1.0 - 3.0 * abs(a));
    float da = max(r.x, r.y);
    float db = max(r.y, r.z);
    float dc = max(r.z, r.x);
    float c = (min(da, min(db, dc)) - 1.0) / s;
    d = max(d, c);
  }
  return d;
}

// --- scene --------------------------------------------------------------------
float map(vec3 p, out float matID) {
  // global music scale pulse
  float sc = 1.0 + 0.08 * Null.uPulse + 0.05 * Null.uBass;
  p /= sc;

  // twist around Y proportional to height
  float twistA = Null.uTime * (0.4 + 0.5 * Null.uIntensity) + p.y * 0.22;
  p.xz = rotate2(p.xz, twistA);

  // procedural displacement (domain warped noise)
  float disp = 0.25 * (fbm3(p * 1.4 + Null.uTime * 0.15) - 0.5) * Null.uIntensity;
  p += normalize(p + 0.001) * disp * 0.5;

  // floor
  float floorD = p.y + 1.4;

  // central sphere
  float sph = sdSphere(p - vec3(0.0, 0.6, 0.0), 1.0);

  // torus ring
  vec3 tp = p - vec3(2.6, 0.8, 0.0);
  float tor = sdTorus(tp, vec2(1.1, 0.34));

  // menger sponge rotating on the other side
  vec3 mp = p - vec3(-2.7, 0.7, 0.0);
  mp.xy = rotate2(mp.xy, Null.uTime * 0.3);
  float meng = menger(mp * 0.55) / 0.55;

  // small boxes orbiting
  float bd = 1e9;
  for (int i = 0; i < 4; i++) {
    float fi = float(i);
    float a = Null.uTime * 0.6 + fi * 1.7;
    vec3 bp = p - vec3(1.6 * cos(a), -0.3 + 0.5 * sin(a * 0.8), 1.6 * sin(a));
    bd = min(bd, sdBox(bp, vec3(0.22)));
  }

  // material selection
  float d = floorD;
  matID = 0.0;
  if (sph < d) { d = sph; matID = 1.0; }
  if (tor < d) { d = tor; matID = 2.0; }
  if (meng < d) { d = meng; matID = 3.0; }
  if (bd < d) { d = bd; matID = 4.0; }

  return d * sc;
}

float mapD(vec3 p) { float id; return map(p, id); }

vec3 calcN(vec3 p) {
  vec2 e = vec2(0.0012, 0.0);
  float id;
  return normalize(vec3(
    mapD(p + e.xyy) - mapD(p - e.xyy),
    mapD(p + e.yxy) - mapD(p - e.yxy),
    mapD(p + e.yyx) - mapD(p - e.yyx)));
}

float softShadow(vec3 ro, vec3 rd, float k) {
  float res = 1.0;
  float t = 0.05;
  for (int i = 0; i < 24; i++) {
    float d = mapD(ro + rd * t);
    res = min(res, k * d / t);
    t += clamp(d, 0.03, 0.8);
    if (res < 0.01 || t > 20.0) break;
  }
  return clamp(res, 0.0, 1.0);
}

float calcAO(vec3 p, vec3 n) {
  float occ = 0.0;
  float sca = 1.0;
  for (int i = 0; i < 5; i++) {
    float h = 0.01 + 0.12 * float(i);
    float d = mapD(p + n * h);
    occ += (h - d) * sca;
    sca *= 0.75;
  }
  return clamp(1.0 - 2.2 * occ, 0.0, 1.0);
}

vec3 shade(vec3 p, vec3 n, float id) {
  // material colors
  vec3 alb;
  float rough;
  if (id < 0.5) { alb = vec3(0.04, 0.05, 0.09); rough = 0.9; }        // floor
  else if (id < 1.5) { alb = vec3(0.9, 0.2, 0.7); rough = 0.25; }     // sphere
  else if (id < 2.5) { alb = vec3(0.15, 0.8, 1.0); rough = 0.2; }     // torus
  else if (id < 3.5) { alb = vec3(0.95, 0.55, 0.1); rough = 0.35; }   // menger
  else { alb = vec3(0.9, 0.9, 0.1); rough = 0.3; }                    // boxes

  vec3 V = normalize(Null.uCamPos - p);
  vec3 lite = vec3(0.0);
  vec3 env = mix(vec3(0.12, 0.14, 0.3), vec3(0.9, 0.2, 0.8), clamp(p.y * 0.5, 0.0, 1.0)) * 0.4;

  // key light (purple) + rim (cyan) + moving point light
  vec3 L1 = normalize(vec3(0.6, 0.9, 0.3));
  vec3 L2 = normalize(vec3(-0.8, 0.2, -0.5));
  vec3 lp = vec3(4.0 * cos(Null.uBeat * 0.25), 1.5, 4.0 * sin(Null.uBeat * 0.25));

  vec3 lc1 = vec3(0.9, 0.4, 1.0);
  vec3 lc2 = vec3(0.1, 0.9, 1.0);
  vec3 lc3 = vec3(1.0, 1.0, 1.0);

  float dif1 = max(dot(n, L1), 0.0);
  float dif2 = max(dot(n, L2), 0.0);
  vec3 Lp = normalize(lp - p);
  float dif3 = max(dot(n, Lp), 0.0);
  float att = 1.0 / (1.0 + dot(lp - p, lp - p));

  // soft shadows
  float sh1 = softShadow(p + n * 0.02, L1, 6.0);
  float sh3 = softShadow(p + n * 0.02, Lp, 10.0);

  float spec1 = pow(max(dot(n, normalize(L1 + V)), 0.0), 32.0);
  float spec2 = pow(max(dot(n, normalize(L2 + V)), 0.0), 32.0);
  float spec3 = pow(max(dot(n, normalize(Lp + V)), 0.0), 64.0);

  lite += alb * lc1 * dif1 * sh1 * (1.0 - rough);
  lite += alb * lc2 * dif2 * (1.0 - rough) * 0.6;
  lite += alb * lc3 * dif3 * att * sh3 * (1.0 - rough);
  lite += lc1 * spec1 * sh1 * (1.0 - rough * 0.6);
  lite += lc2 * spec2 * (1.0 - rough * 0.6) * 0.4;
  lite += lc3 * spec3 * att * sh3;

  // metal reflections
  vec3 R = reflect(-V, n);
  vec3 refl = mix(env, palVoid(R.y * 0.3 + musicHue() * 0.6) * 0.8, 0.5);
  lite += refl * (1.0 - rough) * (0.5 + 0.5 * sh1);

  // floor grid lines
  if (id < 0.5) {
    vec2 g = abs(fract(p.xz) - 0.5);
    float grid = smoothstep(0.42, 0.5, max(g.x, g.y));
    lite += grid * vec3(0.3, 0.6, 1.0) * (0.3 + 0.5 * Null.uPulse);
  }

  return lite + env * alb * 0.25;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  float matID = 0.0;
  float hit = 0.0;

  for (int i = 0; i < 96; i++) {
    p = ro + rd * t;
    float d = map(p, matID);
    if (d < 0.0015 * t) { hit = 1.0; break; }
    t += d * 0.9;
    if (t > 50.0) break;
  }

  if (hit < 0.5) {
    vec3 bg = mix(vec3(0.02, 0.02, 0.06), vec3(0.3, 0.1, 0.4), clamp(p.y * 0.1, 0.0, 1.0));
    bg += palVoid(musicHue() * 0.4) * 0.05;
    fragColor = vec4(bg, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  vec3 n = calcN(p);
  vec3 col = shade(p, n, matID);
  float ao = calcAO(p, n);
  col *= 0.5 + 0.5 * ao;

  // music-driven pulse on the whole scene
  col *= 1.0 + Null.uPulse * 0.25 + Null.uBass * 0.3;

  // fog
  float fog = 1.0 - exp(-t * 0.03);
  col = mix(col, vec3(0.03, 0.02, 0.09), fog);

  // per-kick strobe: metals + lights slam with the kick drum
  col *= 1.0 + uFlash * 0.4;
  col += vec3(1.0, 0.95, 1.0) * uFlash * 0.3;

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
