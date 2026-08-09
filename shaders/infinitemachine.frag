#version 300 es
// ---------------------------------------------------------------------------
// SCENE 4 - The Infinite Machine
// A gigantic procedural machine - "the inside of the machine." Abstract and
// elegant, not realistic: the camera flies forward through an endless shaft
// lined with precessing ring-gyros, spinning turbine gears and energy pipes.
// The machine repeats along z (period PERIOD), so the flight feels infinite.
//
//   ring   - large precessing tori (their tilt sweeps around the axis)
//   gear   - flat turbine discs with radial teeth, spinning on the shaft
//   pipe   - infinite conduits with energy pulses flowing along them
//   wall   - the faint cylindrical grid of the machine hull
//   core   - the luminous spine down the axis (miss path)
//
// Everything self-illuminates (dark metal + music-hued emissives), pulses
// with the beat and bass, and slams on every kick (uFlash).
//
// Ghost takeover: as the kick ratchet (uAssembly) peaks late in the section,
// the ghost seizes the machine - rings stutter and reverse, gear teeth glitch,
// distortion ripples travel down the shaft, and the palette tears toward
// magenta with glitch flicker.
//
// The reascension reprise (section 13, uMode = 1) reuses this shader as the
// ghost-driven rebuild: the machine revs up from cold again, and a base
// brightness/warmth lift keeps it readable against the climbing music while
// the camera glides slower than the original flight.
//
// Performance: the per-cell hashes are hoisted into per-frame arrays (map()
// is called ~80x per pixel and every call used to re-hash the same cells);
// gear spin is folded into the tooth angle (no per-step point rotation);
// the pipe stations are compile-time literals (no runtime cos/sin); and the
// hull + pipes get analytic normals so the 6-sample SDF gradient is skipped
// for the majority of pixels.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)
uniform float uMode;   // 0 = section 4, 1 = reascension reprise (ghost-driven)

out vec4 fragColor;

const float REASC_LIFT = 0.45;  // reascension (uMode 1) base brightness lift
const float REASC_WARM = 0.35;  // reascension (uMode 1) warm blend amount
const float PERIOD = 6.0;     // machine repeats along the shaft (z)
const float WALL_R = 8.0;     // hull inner radius
const float WALL_T = 0.5;     // hull thickness
const float PIPE_R = 0.16;    // conduit radius
const int NC = 12;            // precomputed cell window (covers 72 units of z)
// Cell-window coupling: the window must cover every ray sample, so the
// offset (64) must stay 12+ units larger than the far clip (52) plus the
// interior-guard pushes and any +z-pointing FOV-edge rays. If you change
// the far clip, NC, or the 64.0 offset in main(), keep them in sync or the
// clamped cell hashes silently return wrong geometry at range.

// fixed conduit stations at radius 3.3, angles 0.7 / 2.7944 / 4.8888
const vec2 PIPE0 = vec2(2.5240, 2.1259);
const vec2 PIPE1 = vec2(-3.1028, 1.1223);
const vec2 PIPE2 = vec2(0.5805, -3.2486);

/** one-way per-kick ratchet (0..1, uAssembly): each real kick permanently revs
 *  the machine up - gear spin and ring precession climb to ~2x, pipes flow
 *  faster and hotter. Materials still slam with uFlash on the hit itself. */
float revScale() { return 1.0 + Null.uAssembly * 2.0; }

/** ghost takeover strength 0..1: builds late in the machine section as the
 *  rev peaks (uAssembly > ~0.55 and past 45% of the section) - normalized by
 *  the real section duration so a schedule re-time stays in sync. */
float ghostAmount() {
  float rev = sat01(Null.uAssembly);
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  return sat01(rev - 0.55) * 3.0 * sat01((secT - 0.45) * 3.0);
}

/** per-cell ring stutter: at 4Hz a quarter of the rings reverse direction and
 *  a tenth hesitate at 6Hz - the rings stutter as the ghost fights them.
 *  Deterministic on (cell, time-slice) so map() and shadeRing() stay in sync. */
float ringGlitch(float zc, float g) {
  float stut = step(0.75, hash13(vec3(zc + 0.37, floor(Null.uTime * 4.0), 3.3)));
  float jit = mix(1.0, 0.35, g * step(0.9, hash13(vec3(zc + 0.9, floor(Null.uTime * 6.0), 7.7))));
  return jit * (1.0 - 2.0 * g * stut);
}

// --- sdf primitives ----------------------------------------------------------
float sdTorusZ(vec3 p, vec2 t) {
  vec2 q = vec2(length(p.xy) - t.x, p.z);
  return length(q) - t.y;
}
/** flat gear / turbine disc: radius modulated by `teeth` angular notches.
 *  `spin` folds the rotation into the tooth angle, so no per-step point
 *  rotation is needed (length(q.xy) is invariant under z-rotation). */
float sdGearZSpin(vec3 p, float R, float teeth, float depth, float halfT, float spin, float glitch) {
  float ang = atan(p.y, p.x) * teeth + spin + glitch;
  float rr = R + depth * cos(ang);
  vec2 q = vec2(length(p.xy) - rr, p.z);
  return length(q) - halfT;
}

// --- scene ---------------------------------------------------------------------
float map(vec3 p, float h1v[NC], float h2v[NC], float h3v[NC], int cellBase, out float matID) {
  // ghost takeover: distortion ripples travel down the shaft, warping every
  // element (hull, rings, gears, pipes) as each wavefront passes. The whole
  // block is uniform-gated: until the takeover begins it costs nothing.
  float ghost = ghostAmount();
  if (ghost > 0.001) {
    // deep-bass-driven seizure: every sub-bass hit wracks the failing machine
    // harder - the ripple amplitude spikes with the bass analyser
    float bassAmp = 0.22 * (1.0 + 1.6 * Null.uBass);
    float ri1 = ghost * bassAmp * sin(p.z * 1.6 - Null.uTime * 6.5);
    float ri2 = ghost * bassAmp * sin(p.z * 2.4 - Null.uTime * 9.0 + 1.7);
    p.x += ri1;
    p.y += ri2;
  }

  // hull: infinite hollow cylinder (the machine's skin)
  float d = abs(length(p.xz) - WALL_R) - WALL_T;
  matID = 0.0;  // wall

  // cell-local frame along the shaft, from the precomputed layout
  int cellIdx = int(floor(p.z / PERIOD)) - cellBase;
  cellIdx = min(max(cellIdx, 0), NC - 1);
  float h1 = h1v[cellIdx];
  float h2 = h2v[cellIdx];
  float h3 = h3v[cellIdx];
  float zc = float(cellIdx + cellBase);
  vec3 lp = p - vec3(0.0, 0.0, zc * PERIOD);

  // --- ring 1: large precessing torus ------------------------------------------
  float tilt1 = (h1 - 0.5) * 1.7;
  float pre1 = Null.uTime * (0.22 + 0.55 * h2) * revScale();
  vec3 q1 = lp;
  q1.yz = rotate2(q1.yz, -tilt1);
  q1.xy = rotate2(q1.xy, -pre1);
  float r1 = sdTorusZ(q1, vec2(4.6, 0.12 + 0.05 * h2));
  if (r1 < d) { d = r1; matID = 1.0; }

  // --- ring 2: thinner counter-precessing ring ---------------------------------
  float tilt2 = (h2 - 0.5) * 2.2;
  float pre2 = -Null.uTime * (0.18 + 0.5 * h1) * revScale();
  // ghost stutter: rings reverse / hesitate - uniform-gated so the cost is
  // zero until the takeover begins
  if (ghost > 0.001) {
    pre1 *= ringGlitch(zc, ghost);
    pre2 *= ringGlitch(zc + 0.61, ghost);
  }
  vec3 q2 = lp;
  q2.yz = rotate2(q2.yz, -tilt2);
  q2.xy = rotate2(q2.xy, -pre2);
  float r2 = sdTorusZ(q2, vec2(6.5, 0.08 + 0.03 * h3));
  if (r2 < d) { d = r2; matID = 1.5; }

  // --- gears: two turbine discs spinning on the shaft ----------------------------
  // z-centered mid-cell ([0.3, 2.7] + half-thickness 0.24 stays fully inside
  // the cell domain) so every gear actually renders.
  {
    float gz1 = PERIOD * 0.5 + (h3 - 0.5) * PERIOD * 0.4;
    float ga1 = h1 * 6.2831853;
    float gr1 = 3.8 + 1.8 * h2;
    vec3 g1p = vec3(gr1 * cos(ga1), gr1 * sin(ga1), gz1);
    vec3 qg1 = lp - g1p;
    float teeth1 = 10.0 + floor(h1 * 6.0);
    float spin1 = -Null.uTime * (0.35 + 0.7 * h2) * teeth1 * revScale();
    float glitch1 = 0.0;
    if (ghost > 0.001) glitch1 = (hash13(vec3(zc, floor(Null.uTime * 8.0), 1.1)) - 0.5) * 12.0;
    float g1 = sdGearZSpin(qg1, gr1, teeth1, 0.16, 0.24, spin1, glitch1);
    if (g1 < d) { d = g1; matID = 2.0; }
  }
  {
    float gz2 = PERIOD * 0.5 + (h1 - 0.5) * PERIOD * 0.4;
    float ga2 = h2 * 6.2831853 + 3.0;
    float gr2 = 3.4 + 2.2 * h3;
    vec3 g2p = vec3(gr2 * cos(ga2), gr2 * sin(ga2), gz2);
    vec3 qg2 = lp - g2p;
    float teeth2 = 14.0 + floor(h2 * 5.0);
    float spin2 = -Null.uTime * (0.28 + 0.6 * h3) * teeth2 * revScale();
    float glitch2 = 0.0;
    if (ghost > 0.001) glitch2 = (hash13(vec3(zc, floor(Null.uTime * 8.0), 4.7)) - 0.5) * 12.0;
    float g2 = sdGearZSpin(qg2, gr2, teeth2, 0.13, 0.18, spin2, glitch2);
    if (g2 < d) { d = g2; matID = 2.2; }
  }

  // --- pipes: three infinite conduits at fixed stations --------------------------
  // infinite in z, so they cross cell boundaries seamlessly
  float pp = min(length(p.xy - PIPE0), min(length(p.xy - PIPE1), length(p.xy - PIPE2))) - PIPE_R;
  if (pp < d) { d = pp; matID = 3.0; }

  return d;
}

float mapD(vec3 p, float h1v[NC], float h2v[NC], float h3v[NC], int cellBase) {
  float id; return map(p, h1v, h2v, h3v, cellBase, id);
}

vec3 calcN(vec3 p, float h1v[NC], float h2v[NC], float h3v[NC], int cellBase) {
  vec2 e = vec2(0.0012, 0.0);
  return normalize(vec3(
    mapD(p + e.xyy, h1v, h2v, h3v, cellBase) - mapD(p - e.xyy, h1v, h2v, h3v, cellBase),
    mapD(p + e.yxy, h1v, h2v, h3v, cellBase) - mapD(p - e.yxy, h1v, h2v, h3v, cellBase),
    mapD(p + e.yyx, h1v, h2v, h3v, cellBase) - mapD(p - e.yyx, h1v, h2v, h3v, cellBase)));
}

// --- shading --------------------------------------------------------------------
vec3 accent(float off) { return palVoid(musicHue(off)) * (0.7 + 0.5 * Null.uPulse); }

vec3 shadeWall(vec3 p, float fres) {
  vec3 col = vec3(0.012, 0.015, 0.035) * (0.5 + 0.5 * fres);
  float a = atan(p.z, p.x) / 6.2831853;   // 0..1 around the hull
  float s = p.y * 0.8;
  // hull grid: radial + longitudinal thin lines
  float g = smoothstep(0.035, 0.0, min(abs(fract(a * 60.0) - 0.5), abs(fract(s) - 0.5)));
  col += vec3(0.05, 0.12, 0.3) * g * (0.25 + 0.5 * Null.uPulse);
  // scrolling data lanes around the hull (rev up with the machine)
  for (int k = 0; k < 3; k++) {
    float lane = fract(a * 60.0 + float(k) * 0.33 - Null.uTime * 0.25 * (1.0 + float(k) * 0.5) * revScale());
    col += accent(0.15) * smoothstep(0.16, 0.0, abs(lane - 0.5)) * 0.25;
  }
  // kick strobe races around the hull
  col += vec3(1.0, 0.95, 1.0) * uFlash * smoothstep(0.9, 0.1, abs(fract(a - Null.uTime * 0.3) - 0.5)) * 0.5;
  return col;
}

vec3 shadeRing(vec3 p, float fres, float which) {
  float zc = floor(p.z / PERIOD);
  vec3 lp = p - vec3(0.0, 0.0, zc * PERIOD);
  float h1 = hash13(vec3(zc, 1.7, 0.31));
  float h2 = hash13(vec3(zc, 4.1, 0.97));
  float h3 = hash13(vec3(zc, 9.2, 5.51));
  float tilt = which < 0.5 ? (h1 - 0.5) * 1.7 : (h2 - 0.5) * 2.2;
  float g = ghostAmount();
  float gl = g > 0.001 ? ringGlitch(zc + (which < 0.5 ? 0.0 : 0.61), g) : 1.0;
  float pre = (which < 0.5 ? Null.uTime * (0.22 + 0.55 * h2) : -Null.uTime * (0.18 + 0.5 * h1)) * revScale() * gl;
  vec3 q = lp;
  q.yz = rotate2(q.yz, -tilt);
  q.xy = rotate2(q.xy, -pre);
  float ang = atan(q.y, q.x) / 6.2831853;

  vec3 col = vec3(0.03, 0.035, 0.07) * (0.5 + 0.6 * fres);
  // rotating emissive segments around the ring (lockstep with the precession)
  float segs = smoothstep(0.14, 0.0, abs(fract(ang * 8.0 + Null.uTime * 0.6 * (which < 0.5 ? 1.0 : -1.0) * revScale()) - 0.5));
  col += accent(which < 0.5 ? 0.1 : 0.35) * segs * 0.8;
  // travelling data packets around the ring
  float cyc = fract(ang + Null.uTime * 0.55 * (which < 0.5 ? 1.0 : -1.0) * revScale());
  col += vec3(1.0, 0.98, 1.0) * smoothstep(0.12, 0.0, abs(cyc - 0.35)) * (0.6 + 0.6 * Null.uPulse);
  // fresnel rim
  col += accent(0.2) * fres * 0.9;
  return col;
}

vec3 shadeGear(vec3 p, float fres, vec3 center, float R, float spin, float seed) {
  vec3 q = p - center;
  float ang = atan(q.y, q.x) / 6.2831853;
  float rr = length(q.xy);
  vec3 col = vec3(0.028, 0.032, 0.065) * (0.5 + 0.6 * fres);
  // teeth rim: emissive where radius approaches the outer notch radius
  float rim = smoothstep(0.0, 0.22, R - rr);
  col += accent(0.05 + seed * 0.3) * rim * (0.4 + 0.5 * Null.uPulse);
  // hub
  float hub = smoothstep(0.9, 0.3, rr);
  col += accent(seed) * hub * 0.25;
  // spinning mark so rotation reads clearly (revs with the machine)
  float mark = smoothstep(0.1, 0.0, abs(fract(ang * 5.0 - Null.uTime * spin * 0.5 * revScale()) - 0.5));
  col += vec3(1.0, 0.95, 1.0) * mark * 0.35 * (0.5 + 0.5 * Null.uPulse);
  // fresnel rim
  col += accent(0.3 + seed * 0.2) * fres * 0.7;
  return col;
}

vec3 shadePipe(vec3 p, float fres, float seed) {
  vec3 col = vec3(0.02, 0.024, 0.05) * (0.5 + 0.5 * fres);
  // all conduits share the same orbit radius - the hit point's own radial
  // distance identifies the pipe (no phantom-station mismatch)
  float r = abs(length(p.xy) - 3.3);
  float glow = smoothstep(PIPE_R, 0.0, r);
  col += accent(0.25) * glow * (0.5 + 0.5 * Null.uAssembly);
  // energy pulses flowing along the pipe (toward the camera, into the machine;
  // flow speeds up as the machine revs)
  float e = fract(p.z * 0.7 - Null.uTime * (1.4 + Null.uAssembly * 1.6) + seed * 3.0);
  col += vec3(1.0, 0.99, 1.0) * smoothstep(0.14, 0.0, abs(e - 0.2)) * (0.8 + 0.9 * Null.uPulse + uFlash * 1.2);
  // faint spiral stripe for motion
  float spiral = smoothstep(0.25, 0.0, abs(fract(r * 6.0 - Null.uTime * 0.8 + seed) - 0.5));
  col += accent(0.1) * spiral * 0.12;
  return col;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // --- per-frame cell layout: hoisted hashes for a 12-cell window ahead of ---
  // the camera (covers the 52-unit far clip + guard margin). map() then only
  // does two array lookups per step instead of three full hash13 calls.
  int cellBase = int(floor((ro.z - 64.0) / PERIOD));
  float h1v[NC], h2v[NC], h3v[NC];
  for (int i = 0; i < NC; i++) {
    float z = float(i + cellBase);
    h1v[i] = hash13(vec3(z, 1.7, 0.31));
    h2v[i] = hash13(vec3(z, 4.1, 0.97));
    h3v[i] = hash13(vec3(z, 9.2, 5.51));
  }

  // --- background: the luminous core spine + deep music-hued haze ---------------
  vec3 coreDir = normalize(vec3(0.0, 0.0, -1.0));
  float core = exp(-length(cross(rd, coreDir)) * 3.0) * 0.35;
  vec3 bg = vec3(0.008, 0.012, 0.045);
  bg += palVoid(musicHue(0.1)) * core * 0.9;
  bg += vec3(0.5, 0.8, 1.0) * pow(max(dot(rd, coreDir), 0.0), 3.0) * 0.25 * (0.6 + 0.5 * Null.uPulse);

  // --- march ----------------------------------------------------------------------
  float t = 0.04;
  vec3 p = ro;
  float matID = 0.0;
  float hit = 0.0;
  for (int i = 0; i < 72; i++) {
    p = ro + rd * t;
    float d = map(p, h1v, h2v, h3v, cellBase, matID);
    if (d < 0.0015 * t) {
      if (d < 0.0) { t += 0.05; continue; }
      hit = 1.0; break;
    }
    t += d * 0.9;
    if (t > 52.0) break;
  }

  if (hit < 0.5) {
    bg += vec3(0.4, 0.55, 1.0) * exp(-t * 0.05) * 0.1;
    // ghost takeover: the core spine tears magenta and flickers
    float g = ghostAmount();
    if (g > 0.001) {
      bg = mix(bg, bg * vec3(1.2, 0.5, 0.95) + vec3(0.6, 0.05, 0.3) * 0.45, g * 0.55);
      // the core spine throbs with every sub-bass hit
      bg *= 1.0 + g * 0.35 * Null.uBass;
      float flick = hash13(vec3(gl_FragCoord.xy * 0.13, floor(Null.uTime * 11.0) * 0.17));
      bg *= mix(1.0, 0.45, step(1.0 - g * 0.55, flick) * g);
    }
    fragColor = vec4(bg, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  // --- normals: analytic for the smooth primitives (hull + pipes) so the ---
  // 6-sample SDF gradient only runs for ring/gear hits
  vec3 n;
  if (matID < 0.5) {
    n = vec3(p.x, 0.0, p.z) / max(length(p.xz), 1e-4);   // hull: radial outward
  } else if (matID >= 3.0) {
    float d0 = length(p.xy - PIPE0);
    float d1 = length(p.xy - PIPE1);
    float d2 = length(p.xy - PIPE2);
    vec2 c = d0 < d1 ? (d0 < d2 ? PIPE0 : PIPE2) : (d1 < d2 ? PIPE1 : PIPE2);
    n = normalize(vec3(p.xy - c, 0.0));                  // pipe: radial from station
  } else {
    n = calcN(p, h1v, h2v, h3v, cellBase);
  }
  vec3 V = normalize(ro - p);
  float fres = pow(1.0 - max(dot(n, V), 0.0), 2.5);

  vec3 col;
  if (matID < 0.5) col = shadeWall(p, fres);
  else if (matID < 1.5) col = shadeRing(p, fres, 0.0);
  else if (matID < 2.0) col = shadeRing(p, fres, 1.0);
  else if (matID < 2.1) {
    float zc = floor(p.z / PERIOD);
    vec3 lp = p - vec3(0.0, 0.0, zc * PERIOD);
    float h1 = hash13(vec3(zc, 1.7, 0.31));
    float h2 = hash13(vec3(zc, 4.1, 0.97));
    float h3 = hash13(vec3(zc, 9.2, 5.51));
    float gz1 = PERIOD * 0.5 + (h3 - 0.5) * PERIOD * 0.4;
    float ga1 = h1 * 6.2831853;
    float gr1 = 3.8 + 1.8 * h2;
    vec3 g1p = vec3(gr1 * cos(ga1), gr1 * sin(ga1), gz1);
    col = shadeGear(p, fres, g1p, gr1, 0.35 + 0.7 * h2, h1);
  } else if (matID < 3.0) {
    float zc = floor(p.z / PERIOD);
    vec3 lp = p - vec3(0.0, 0.0, zc * PERIOD);
    float h1 = hash13(vec3(zc, 1.7, 0.31));
    float h2 = hash13(vec3(zc, 4.1, 0.97));
    float h3 = hash13(vec3(zc, 9.2, 5.51));
    float gz2 = PERIOD * 0.5 + (h1 - 0.5) * PERIOD * 0.4;
    float ga2 = h2 * 6.2831853 + 3.0;
    float gr2 = 3.4 + 2.2 * h3;
    vec3 g2p = vec3(gr2 * cos(ga2), gr2 * sin(ga2), gz2);
    col = shadeGear(p, fres, g2p, gr2, 0.28 + 0.6 * h3, h2);
  } else {
    float zc = floor(p.z / PERIOD);
    float seed = fract(hash13(vec3(zc, 7.7, 2.2)));
    col = shadePipe(p, fres, seed);
  }

  // music + kick: the whole machine slams on the drum
  col *= 1.0 + uFlash * 0.45 + Null.uPulse * 0.18 + Null.uBass * 0.25;
  col += vec3(1.0, 0.97, 1.0) * uFlash * 0.3;

  // reascension (uMode 1): the ghost-driven rebuild reads hotter than the
  // original - a base brightness lift plus a faint warm push, so the rev-up
  // stays visible even while the machine is still spinning up from cold.
  // The lift fades out as the ghost takes over, so the climax fails dark
  // instead of clipping white on the kick flashes.
  float ghost = ghostAmount();
  float lift = uMode * (1.0 - 0.55 * ghost);
  col *= 1.0 + lift * REASC_LIFT;
  col = mix(col, col * vec3(1.12, 0.96, 1.02) + vec3(0.12, 0.04, 0.08) * 0.3, lift * REASC_WARM);

  // ghost takeover: hue tears toward magenta, a brightness wave travels down
  // the shaft, and random glitch flicker darkens the failing machine
  if (ghost > 0.001) {
    col = mix(col, col * vec3(1.25, 0.55, 0.8) + vec3(0.7, 0.06, 0.3) * 0.4, ghost * 0.45);
    // the brightness wave + glitch flicker also ride the bass
    float bassAmp = 0.35 * (1.0 + 1.5 * Null.uBass);
    col *= 1.0 + ghost * bassAmp * (0.5 + 0.5 * sin(p.z * 1.8 - Null.uTime * 7.0));
    float flick = hash13(vec3(gl_FragCoord.xy * 0.11, floor(Null.uTime * 14.0) * 0.13));
    col *= mix(1.0, 0.45 - 0.25 * Null.uBass, step(1.0 - ghost * 0.6, flick) * ghost);
  }

  // depth fog toward the hull haze
  float fog = 1.0 - exp(-t * 0.05);
  col = mix(col, bg + palVoid(musicHue(0.2)) * 0.08, fog);

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
