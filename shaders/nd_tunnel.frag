#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 3 // PROCEDURAL MEMORY TUNNEL
// ---------------------------------------------------------------------------
// Inside the core: a fast forward-flight tunnel whose walls are built from
// FRAGMENTED MEMORIES - tiles of buildings, machines, geometric structures,
// symbols and typography (atlas glyphs), with missing segments so the wall
// reads as a broken mosaic rather than a smooth tube. Clear depth + motion.
//
// Audio mapping (react.*, all in-shader - the .nsd owns only the length):
//   uFlash (kick)  -> tunnel expansion (R0 kicks outward)
//   Null.uBass     -> wall deformation (ring cross-section warps deeper)
//   Null.uOnset    -> geometry displacement (fragments jump along the wall)
//   uHigh          -> drifting spark particles
//   Null.uPulse    -> beat camera impulse (uv shake) + glow breathing
//   musicHue()     -> per-bar palette transition (engine chord clock)
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;      // per-kick strobe
uniform float uMode;
uniform float uHigh;       // react.high
uniform vec2  uSceneRes;
uniform sampler2D uFont;   // font atlas (unit 11) for wall typography
uniform vec2  uAtlas;
uniform vec2  uCell;

out vec4 fragColor;

float glyphAt(int code, vec2 luv) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

/** ring key for hashing */
float ringKey(float ringZ) { return floor(ringZ * 0.5); }

// --- tile fragment patterns (drawn in tile-local uv, uu 0..1 across the
//     sector, vv 0..1 along the tunnel) --------------------------------------

/** fragment mask: a tile only occupies part of its sector (broken mosaic) */
float tileMask(vec2 uvv, float seed, float time) {
  float m = hash12(uvv * 6.0 + vec2(seed * 13.7, 0.0));
  float on = step(m, 0.78);
  // occasional missing chunk
  float chunk = step(0.5, hash12(vec2(seed, floor(time * 0.7))));
  return on * chunk + (1.0 - chunk) * 0.0;
}

/** remembered city fragment: building silhouette with lit windows */
vec3 tileBuilding(vec2 uvv, float seed, float time) {
  vec2 c = uvv - 0.5;
  // box
  float box = smoothstep(0.5, 0.42, max(abs(c.x) * 1.4, abs(c.y) * 2.6 - 0.15));
  vec3 col = vec3(0.02, 0.03, 0.08) * box;
  // window grid
  vec2 w = fract(uvv * vec2(8.0, 6.0)) - 0.5;
  float win = smoothstep(0.28, 0.16, length(w));
  float lit = step(0.6, hash12(vec2(floor(uvv.x * 8.0), floor(uvv.y * 6.0)) + seed));
  float flick = 0.8 + 0.2 * sin(time * 14.0 + seed * 40.0);
  col += vec3(1.0, 0.82, 0.5) * win * lit * flick * 1.4 * box;
  // roof neon strip
  float roof = smoothstep(0.02, 0.0, abs(uvv.y - 0.86));
  col += palVoid(seed * 0.3 + musicHue(0.2)) * roof * 1.6 * box;
  return col;
}

/** remembered machine: rotating gear with spokes */
vec3 tileMachine(vec2 uvv, float seed, float time) {
  vec2 c = uvv - 0.5;
  float r = length(c);
  float a = atan(c.y, c.x);
  float rot = time * (0.6 + 0.4 * hash12(vec2(seed, 1.0)));
  vec3 col = vec3(0.0);
  // ring
  float ring = exp(-abs(r - 0.28) * 26.0);
  // teeth
  float teeth = smoothstep(0.30, 0.26, abs(fract((a + rot) * 6.0 / TAU) - 0.5)) * step(0.2, r);
  // spokes
  float spoke = exp(-abs(sin((a + rot) * 3.0)) * 3.0) * step(r, 0.26) * step(0.05, r);
  col += vec3(0.35, 0.85, 1.0) * (ring + teeth + spoke) * (0.7 + 0.5 * Null.uPulse);
  col += vec3(0.9, 0.99, 1.0) * exp(-abs(r - 0.07) * 30.0) * 0.9;
  return col;
}

/** geometric structure: rotating star / cross outline */
vec3 tileGeom(vec2 uvv, float seed, float time) {
  vec2 c = uvv - 0.5;
  float rot = time * 0.5 + seed;
  vec2 rc = rotate2(c, rot);
  vec3 col = vec3(0.0);
  float star = 0.0;
  for (int i = 0; i < 4; i++) {
    float ang = float(i) * 1.5708 + seed * 0.7;
    vec2 d = vec2(cos(ang), sin(ang)) * 0.3;
    vec2 rp = rotate2(c - d, rot);
    float seg = exp(-abs(length(rp) - 0.1) * 40.0);
    star = max(star, seg);
  }
  float ring = exp(-abs(length(rc) - 0.30) * 30.0);
  col += palVoid(seed * 0.4 + musicHue(0.3)) * (star + ring) * (0.8 + 0.4 * Null.uPulse);
  return col;
}

/** remembered symbol: procedural glyph (rings, cross, dot) */
vec3 tileSymbol(vec2 uvv, float seed, float time) {
  vec2 c = uvv - 0.5;
  float r = length(c);
  vec3 col = vec3(0.0);
  float ring = exp(-abs(r - 0.18) * 34.0);
  float dot = exp(-length(c) * 60.0);
  float cr = exp(-abs(c.x) * 34.0) * step(r, 0.18);
  float c2 = exp(-abs(c.y) * 34.0) * step(r, 0.18);
  col += vec3(0.75, 0.95, 1.0) * (ring + dot + cr + c2) * (0.9 + 0.4 * sin(time * 3.0 + seed));
  col += vec3(0.9, 0.4, 1.0) * ring * 0.6;
  return col;
}

/** typography: a remembered letter sampled from the shared atlas */
vec3 tileText(vec2 uvv, float seed, float time) {
  int codes[6];
  codes[0] = 77; codes[1] = 69; codes[2] = 77; codes[3] = 79; codes[4] = 82; codes[5] = 89;
  int ci = int(floor(hash12(vec2(seed, 2.0)) * 6.0));
  ci = clamp(ci, 0, 5);
  float g = glyphAt(codes[ci], uvv);
  float crisp = smoothstep(0.3, 0.7, g);
  vec3 col = vec3(0.95, 0.99, 1.0) * crisp * (0.85 + 0.3 * Null.uPulse);
  col += vec3(0.3, 0.85, 1.0) * crisp * 0.25;
  // partial reveal scramble
  float rc = smoothstep(0.35, 0.75, g);
  col += vec3(0.2, 0.8, 1.0) * rc * 0.3 * (0.5 + 0.5 * sin(time * 8.0 + seed * 9.0));
  return col;
}

/** one wall tile: pick a memory fragment by hash, draw it, mask it */
vec3 drawTile(float phi, float z, float ringZ, float time) {
  int S = 7;                                             // tiles per ring
  float seg = floor(phi * float(S) / TAU);
  float segF = fract(phi * float(S) / TAU);
  float ringW = 1.15;
  float vv = clamp((z - ringZ) / ringW + 0.5, 0.0, 1.0);
  float seed = hash13(vec3(ringKey(ringZ), seg, 3.0));
  // onset (snare): fragments jump sideways along the wall BEFORE sampling
  float disp = Null.uOnset * (hash12(vec2(seed, floor(time * 6.0))) - 0.5) * 0.4;
  segF = fract(segF + disp);
  vec2 uvv = vec2(segF, vv);
  int type = int(floor(seed * 6.0));

  vec3 col = vec3(0.008, 0.012, 0.03);                   // dark panel base
  // panel edge glow
  float edge = smoothstep(0.06, 0.0, min(segF, 1.0 - segF))
             + smoothstep(0.06, 0.0, min(vv, 1.0 - vv));
  col += palVoid(seed * 0.3 + musicHue(0.1)) * edge * 0.35;

  if (type == 0) col += tileBuilding(uvv, seed, time);
  else if (type == 1) col += tileMachine(uvv, seed, time);
  else if (type == 2) col += tileGeom(uvv, seed, time);
  else if (type == 3) col += tileSymbol(uvv, seed, time);
  else if (type == 4) col += tileText(uvv, seed, time);
  else col += vec3(0.04, 0.05, 0.1) * (0.5 + 0.5 * sin(time + seed * 20.0));

  // kick: tile brightness slams
  col *= 1.0 + uFlash * 0.5;
  col += vec3(0.9, 0.98, 1.0) * uFlash * 0.12;

  // fragment mask: missing segments leave holes to the void behind
  float mask = tileMask(vec2(segF, vv), seed, time);
  return col * mask;
}

void main() {
  float t = Null.uSectionLocal;
  float time = Null.uTime;
  float kickE = uFlash;
  float bassE = Null.uBass;
  float onsetE = Null.uOnset;
  float pulse = Null.uPulse;

  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;

  // beat camera impulse: whole view jolts on the beat, harder with kick
  float shake = pulse * (0.5 + kickE * 1.2);
  if (shake > 0.002) {
    float sk = floor(time * 24.0);
    uv += (vec2(hash12(vec2(sk, 11.7)), hash12(vec2(sk, 23.3))) - 0.5) * shake * 0.02;
  }

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // --- march ----------------------------------------------------------------
  float tmax = 110.0;
  float stepLen = 0.38;
  float tt = 0.0;
  float hit = 0.0;
  float hitZ = 0.0, hitPhi = 0.0, hitRing = 0.0;
  vec3 p = ro;

  // sparks: drifting memory particles (react to uHigh)
  vec3 sparks[10];
  for (int s = 0; s < 10; s++) {
    float fs = float(s);
    sparks[s] = vec3((hash13(vec3(fs, 5.0, 1.0)) - 0.5) * 6.0,
                     (hash13(vec3(fs, 6.0, 1.0)) - 0.5) * 6.0,
                     fract(time * (1.0 + fs * 0.1) * 0.6 + fs * 0.37) * 60.0 - 10.0);
  }

  for (int i = 0; i < 84; i++) {
    p = ro + rd * tt;
    float z = p.z;
    // kick -> tunnel expansion
    float R0 = 3.4 + 0.4 * sin(z * 0.5 + time * 0.9) + pulse * 0.5 + kickE * 1.0;
    // bass -> wall deformation: ring cross-section warps deeper with bass
    float phi = atan(p.y, p.x);
    float warpA = 0.16 + bassE * 0.55;
    float deform = 1.0 + warpA * sin(phi * 5.0 + z * 0.45 + bassE * 10.0)
                         + warpA * 0.5 * sin(phi * 11.0 - z * 0.3);
    float R = R0 * deform;
    // rings
    float ringSpacing = 1.35 + 0.25 * sin(time * 0.25);
    float ringZ = round(z / ringSpacing) * ringSpacing;
    float r = length(p.xy);
    // onset -> geometry displacement: whole ring shifts axially
    ringZ += onsetE * (hash12(vec2(ringKey(ringZ), floor(time * 5.0))) - 0.5) * 1.6;
    float ringW = 1.15 + pulse * 0.2 + kickE * 0.3;
    float dz = abs(z - ringZ);
    bool inRing = dz < ringW * 0.5;
    if (r > R && inRing) { hit = 1.0; hitZ = z; hitPhi = phi; hitRing = ringZ; break; }
    tt += stepLen;
    if (tt > tmax) break;
  }

  vec3 col = vec3(0.0);

  if (hit < 0.5) {
    // --- void: depth glow + sparks + drift -----------------------------------
    vec3 voidCol = palVoid(musicHue(0.1) + t * 0.001) * 0.05;
    col = voidCol;
    float centerGlow = exp(-length(uv) * 1.6);
    col += palVoid(musicHue(0.2)) * centerGlow * (0.25 + pulse * 0.4);
    // sparks: glowing motes drifting with the flow (uHigh brightens)
    for (int s = 0; s < 10; s++) {
      vec3 sp = sparks[s];
      vec3 d = p - sp;
      float dd = dot(d, d);
      float g = exp(-dd * 3.0) * (0.3 + 0.8 * uHigh);
      col += vec3(0.7, 0.95, 1.0) * g;
    }
    // far-end glow
    col += palVoid(musicHue(0.35)) * exp(-length(uv) * 2.4) * 0.2;
  } else {
    // --- wall shading ---------------------------------------------------------
    float hue = musicHue() + t * 0.002;
    col = drawTile(hitPhi, hitZ, hitRing, time);
    // depth-based light falloff
    float fall = exp(-tt * 0.02);
    col *= 0.5 + 0.6 * fall;
    // volumetric fog
    float fogD = 1.0 - exp(-tt * (0.045 + 0.05 * bassE));
    vec3 fogCol = palVoid(musicHue(0.15)) * (0.10 + 0.2 * bassE);
    col = mix(col, fogCol, sat01(fogD));
  }

  // --- SYSTEM FAILURE mode (uMode 1): the tunnel seizes and disintegrates ----
  if (uMode > 0.5) {
    float part = glitchParticipation(1.0, kickE, bassE, 1.0);
    float g = hash13(floor(p * 3.0 + floor(time * 8.0) * vec3(7.0, 13.0, 1.0)));
    float glitch = step(max(0.55, 0.96 - part * 0.41), g);
    col = mix(col, palVoid(g + musicHue()) * 1.5, glitch * 0.9);
    col *= 0.5 + 0.5 * sin(time * 34.0 + p.z * 3.0);
    vec2 gs2 = glitchSlice(t * 0.5, 2.0, 1.0, kickE, bassE, 7.3, 1.0);
    if (abs(gs2.x) > 0.004) {
      float hue = musicHue(0.3) + hash12(vec2(floor(p.x * 4.0), floor(time * 12.0))) * 0.4;
      float chroma = 0.02 + gs2.y * 0.6;
      vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
      col += tearCol * (0.6 + 0.9 * kickE + 0.5 * bassE);
    }
  }

  // kick strobe over everything
  col *= 1.0 + kickE * 0.4;
  col += vec3(0.9, 0.97, 1.0) * kickE * 0.18;

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hit > 0.5 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
