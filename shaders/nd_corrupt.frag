#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 5 // MEMORY CORRUPTION
// ---------------------------------------------------------------------------
// The believable city begins to rot: geometry stretching, vertex
// displacement, duplicated buildings, impossible architecture, recursive
// structures, horizon distortion and beat-locked glitch tears. Diagnostics
// flash (RECONSTRUCTION ERROR / MEMORY CONFLICT / VERSION 001/014/927 /
// NO ORIGINAL FOUND), then the city dissolves into particles.
//
// Corruption escalates with the music: kick/bass/onset push corruptE past
// the section ramp so every hit tears harder.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;      // per-kick strobe
uniform float uMode;
uniform float uHigh;
uniform float uTransition; // handoff from the clean city
uniform sampler2D uPrevScene;
uniform vec2  uSceneRes;
uniform sampler2D uFont;   // atlas for diagnostics (unit 11)
uniform vec2  uAtlas;
uniform vec2  uCell;

out vec4 fragColor;

vec3 g_text = vec3(0.0);

float glyphAt(int code, vec2 luv) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

/** glitchy diagnostic line: chars flicker in, slice jumps, hard fade */
void diagLine(vec2 p, const int codes[32], int n, float charW, float charH,
              float cx, float cy, float t0, float hold, float t, float corrupt,
              float seed, vec3 tint, float baseA) {
  float on = smoothstep(t0, t0 + 0.25, t);
  if (on <= 0.01) return;
  float a = on * (1.0 - smoothstep(t0 + hold, t0 + hold + 0.8, t));
  if (a <= 0.01) return;
  float total = float(n) * charW;
  float x0 = cx - total * 0.5;
  if (p.x < x0 || p.x > x0 + total) return;
  if (p.y < cy - charH * 0.5 || p.y > cy + charH * 0.5) return;

  int ci = int(clamp(floor((p.x - x0) / charW), 0.0, float(n) - 1.0));
  if (ci >= n) return;
  float lx = (p.x - (x0 + float(ci) * charW)) / charW;
  float ly = (p.y - (cy - charH * 0.5)) / charH;

  float g = glyphAt(codes[ci], vec2(lx, ly));
  float crisp = smoothstep(0.35, 0.75, g);
  // per-char dropout with corruption
  float drop = step(hash12(vec2(float(ci) * 3.7 + seed, floor(t * 10.0) * 0.13)), 0.7 + 0.3 * corrupt);
  crisp *= drop;
  // slice jump
  float slice = floor(ly * 4.0);
  float jump = step(0.80 - corrupt * 0.35, hash12(vec2(slice + seed, floor(t * 12.0))));
  float jx = (hash12(vec2(slice * 1.3 + seed, floor(t * 14.0))) - 0.5) * jump * 0.06;
  vec2 suv = vec2(lx + jx, ly);
  float g2 = glyphAt(codes[ci], clamp(suv, 0.0, 1.0));
  float crisp2 = smoothstep(0.35, 0.75, g2);
  // RGB split smear
  vec3 col = tint * (crisp2 * 0.9 + crisp * 0.4);
  float split = 0.02 + corrupt * 0.05;
  float r = glyphAt(codes[ci], clamp(vec2(lx + split, ly), 0.0, 1.0));
  float b = glyphAt(codes[ci], clamp(vec2(lx - split, ly), 0.0, 1.0));
  col.r += r * 0.8;
  col.b += b * 0.8;
  // scramble flicker while resolving
  float rc = smoothstep(0.3, 0.7, glyphAt(codes[ci], fract(vec2(lx, ly) + hash12(vec2(float(ci), floor(t * 16.0))) * 3.0)));
  col += vec3(0.4, 0.1, 1.0) * rc * 0.5 * (1.0 - drop * 0.5);
  g_text += col * a * baseA;
}

const float CELL = 8.0;
const float CITY = 150.0;

float bHeight(vec2 cell) {
  float h = hash12(cell + 100.0);
  float d = length(cell) * 0.09;
  float zone = smoothstep(1.6, 0.0, d);
  h = mix(h * 0.35, h * 1.1, zone);
  h *= 16.0;
  h += fbm2(cell * 0.07) * 8.0;
  return clamp(h, 2.5, 46.0);
}

bool hasBuilding(vec2 cell) { return hash12(cell + 3.7) > 0.24; }

/** corrupted window shading: hard flicker, scrambled hues, recursion insets */
vec3 corruptShade(vec3 wp, vec2 cell, float h, float corrupt, float time) {
  float ux = fract(wp.x / CELL + 0.5) * 3.0;
  float uz = fract(wp.z / CELL + 0.5) * 3.0;
  float colIdx = floor(ux);
  float row = floor((h - wp.y) * 1.1);
  float cellh = hash12(cell + vec2(colIdx, row) * 1.7 + 0.5);

  // windows flicker hard, colors scramble with corruption
  float lit = step(0.58, cellh);
  float flick = 0.4 + 0.6 * sin(Null.uTime * (6.0 + corrupt * 30.0) + cellh * 40.0
                                + uFlash * 60.0);
  float scramble = step(hash12(vec2(cellh * 3.3 + corrupt, floor(Null.uTime * 12.0) * 0.13)), 0.4 + 0.5 * corrupt);
  vec3 win = palVoid(cellh * 0.7 + musicHue() * 0.5 + scramble * 0.6)
           * lit * abs(flick) * 1.4;
  win = mix(win, vec3(1.0, 0.4, 0.5), step(0.96, cellh) * corrupt);  // red alerts

  vec3 facade = vec3(0.03, 0.04, 0.08);
  // geometry stretching: streaks along the tower
  facade += vec3(0.05, 0.03, 0.1) * smoothstep(0.4, 0.0, abs(fract(wp.y * 0.4) - 0.5)) * corrupt;

  // recursive structures: nested inset frames on the face
  vec2 rec = abs(fract(wp.xz / CELL) - 0.5);
  float recEdges = 0.0;
  for (int k = 1; k <= 3; k++) {
    float s = 0.06 + float(k) * 0.11 * (0.4 + 0.6 * corrupt);
    float inside = step(max(rec.x, rec.y), 0.5 - s);
    float e = step(0.5 - s - 0.012, max(rec.x, rec.y));
    recEdges += inside * e * (0.7 + 0.3 * hash12(vec2(float(k), cellh)));
  }
  win += palVoid(musicHue(0.3) + recEdges * 0.2) * recEdges * (0.6 + 0.8 * corrupt);

  // roof band scrambled
  float roofBand = smoothstep(0.06, 0.0, abs(wp.y - h));
  win += palVoid(hash12(cell + 7.0) * 0.7) * roofBand * (1.0 + 1.5 * corrupt);

  return facade + win;
}

vec3 groundCol(vec3 wp, vec3 rd, float corrupt) {
  vec3 col = vec3(0.012, 0.014, 0.03);
  vec2 g = abs(fract(wp.xz / CELL) - 0.5);
  float road = step(0.42, max(g.x, g.y));
  col = mix(col, vec3(0.02, 0.022, 0.045), road);
  // street grid warps with corruption
  float gridW = smoothstep(0.5, 0.0, abs(fbm2(wp.xz * 0.1) - 0.5)) * corrupt;
  col += palVoid(hash12(floor(wp.xz * 0.4)) * 0.5 + musicHue(0.4)) * gridW * 0.3;
  // glowing fracture lines
  float fract = step(0.995 - corrupt * 0.01, hash12(floor(wp.xz * 0.6)));
  col += palVoid(musicHue(0.2) + corrupt) * fract * (0.4 + corrupt);
  return col;
}

void main() {
  float t = Null.uSectionLocal;
  float time = Null.uTime;
  float kickE = uFlash;
  float bassE = Null.uBass;
  float onsetE = Null.uOnset;

  // corruption ramp + music escalation (kicks tear harder)
  float corrupt = smoothstep(4.0, 26.0, t);
  float corruptE = corrupt * (0.55 + 0.45 * kickE + 0.3 * bassE + 0.3 * onsetE);
  corruptE = min(corruptE, 1.35);
  if (uMode > 0.5) corruptE = max(corruptE, 1.2);   // failure: already rotten
  // city dissolves to particles at the end
  float dissolve = smoothstep(24.5, 29.0, t);

  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;

  // horizon distortion: the skyline bends near the horizon
  uv.y += sin(uv.x * 3.0 + corrupt * 6.0) * 0.04 * corruptE;
  uv.x += sin(uv.y * 2.0) * 0.02 * corruptE;

  // view slice tears (shared beat-locked glitch model)
  float secT = sat01(t / max(Null.uSectionDur, 1e-4));
  vec2 gs = glitchSlice(gl_FragCoord.y / res.y, 32.0, corrupt, kickE, bassE, 61.3, corrupt);
  uv.x += gs.x * 0.16;

  // kick camera shake
  float shake = corruptE * (kickE * 0.05 + glitchBurst(corrupt) * 0.03);
  if (shake > 0.0005) {
    float sk = floor(time * 24.0);
    uv += (vec2(hash12(vec2(sk, 91.7)), hash12(vec2(sk, 37.3))) - 0.5) * shake * 2.0;
  }

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float tt = 0.0;
  vec3 p = ro;
  vec3 col = vec3(0.008, 0.01, 0.028);
  float hitDist = -1.0;

  // --- ground ---------------------------------------------------------------
  float tGround = -ro.y / max(rd.y, 1e-4);
  if (rd.y < -0.001) {
    vec3 gp = ro + rd * tGround;
    if (abs(gp.x) < CITY && abs(gp.z) < CITY) {
      col = groundCol(gp, rd, corruptE);
      hitDist = tGround;
    }
  }

  // --- DDA march (warped + folded cells) -------------------------------------
  vec3 invD = 1.0 / rd;
  vec3 stp = sign(rd);
  vec3 cellPos = floor(ro / CELL);
  vec3 tDelta = abs(CELL * invD);
  vec3 tMax = (cellPos + stp * 0.5 + 0.5 - ro / CELL) * CELL * invD;

  vec2 hitCell = vec2(0.0);
  float hitH = 0.0;
  bool found = false;
  for (int i = 0; i < 96; i++) {
    if (tMax.x < tMax.y) { tt = tMax.x + 0.001; tMax.x += tDelta.x; cellPos.x += stp.x; }
    else { tt = tMax.y + 0.001; tMax.y += tDelta.y; cellPos.y += stp.y; }
    vec2 cell = cellPos.xy;
    if (abs(cell.x) > CITY / CELL || abs(cell.y) > CITY / CELL) break;

    // vertex displacement: cells wobble and tear
    vec2 cw = cell + (vec2(hash12(cell + 5.0), hash12(cell + 9.0)) - 0.5) * corruptE * 1.6;
    // duplicated buildings: fold the grid -> mirrored, duplicated skyline
    if (corrupt > 0.3) cw = abs(cw + 0.5) - 0.5;
    // impossible architecture: some towers swap axes (rotated 90deg)
    if (step(0.90, hash12(cw + 3.0)) * corrupt > 0.1) cw = cw.yx;

    if (!hasBuilding(cw)) continue;
    p = ro + rd * tt;
    // geometry stretching: heights pull with corruption
    float h = bHeight(cw) * (1.0 + corruptE * (hash12(cw + 1.7) - 0.5) * 3.0);
    if (p.y < h) { hitCell = cw; hitH = h; hitDist = tt; found = true; break; }
    if (tt > 260.0) break;
  }

  if (found) {
    p = ro + rd * hitDist;
    col = corruptShade(p, hitCell, hitH, corruptE, time);
    // dissolving: buildings break into drifting particle fields
    if (dissolve > 0.01) {
      col *= 1.0 - dissolve * 0.85;
      vec3 pt = vec3(0.0);
      vec2 gid = floor(p.xz * 1.2);
      float hpt = hash12(gid + vec2(0.0, floor(time * 3.0)));
      float dotOn = step(0.82, hpt);
      vec2 fp = fract(p.xz * 1.2);
      float mot = exp(-length(fp - 0.5) * 5.0);
      pt = palVoid(hash12(gid) * 0.6 + musicHue(0.2)) * dotOn * mot * 1.4;
      col += pt * dissolve;
    }
    float fd = max(hitDist, 0.0);
    float fog = 1.0 - exp(-fd * 0.011);
    col = mix(col, vec3(0.05, 0.03, 0.1), fog);
  }

  // dissolve dust in the air everywhere
  if (dissolve > 0.01) {
    vec2 gid = floor(gl_FragCoord.xy * 0.4);
    float hd = hash12(gid + vec2(0.0, floor(time * 2.0)));
    float dotOn = step(0.995, hd);
    col += palVoid(hash12(gid) * 0.7 + musicHue(0.3)) * dotOn * dissolve * (0.4 + 0.6 * uHigh);
  }

  // chromatic smear across the city (shared glitch model, hit pixels)
  if (corrupt > 0.02 && hitDist >= 0.0) {
    vec2 gs2 = glitchSlice(hitDist * 0.5, 2.0, corruptE, kickE, bassE, 7.3, corrupt);
    if (abs(gs2.x) > 0.002) {
      float hue = musicHue(0.3) + hash12(vec2(floor(p.x * 4.0), floor(time * 12.0))) * 0.4;
      float chroma = 0.02 + gs2.y * 0.7;
      vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
      float bite = 0.2 + 0.5 * kickE + 0.3 * bassE;
      col = mix(col, tearCol * 1.4, bite);
      col += tearCol * (0.2 + 0.5 * kickE + 0.3 * bassE);
    }
  }

  // kick strobe
  col *= 1.0 + kickE * 0.5;
  col += vec3(1.0, 0.9, 1.0) * kickE * 0.25;

  // --- diagnostics ------------------------------------------------------------
  vec2 p2 = (gl_FragCoord.xy - 0.5 * res) / res.y;
  float sizeS = clamp(res.y / 900.0, 0.7, 1.6);
  float dA = corrupt > 0.05 ? 1.0 : 0.0;

  int e1[32];
  for (int i = 0; i < 32; i++) e1[i] = 32;
  e1[0]=82; e1[1]=69; e1[2]=67; e1[3]=79; e1[4]=78; e1[5]=83; e1[6]=84; e1[7]=82;
  e1[8]=85; e1[9]=67; e1[10]=84; e1[11]=73; e1[12]=79; e1[13]=78; e1[14]=32; e1[15]=69;
  e1[16]=82; e1[17]=82; e1[18]=79; e1[19]=82;
  diagLine(p2, e1, 20, 0.078 * sizeS, 0.078 * sizeS, 0.0, 0.34, 2.5, 3.0, t, corrupt,
           1.0, vec3(1.0, 0.45, 0.5), dA);

  int e2[32];
  for (int i = 0; i < 32; i++) e2[i] = 32;
  e2[0]=77; e2[1]=69; e2[2]=77; e2[3]=79; e2[4]=82; e2[5]=89; e2[6]=32; e2[7]=67;
  e2[8]=79; e2[9]=78; e2[10]=70; e2[11]=76; e2[12]=73; e2[13]=67; e2[14]=84;
  diagLine(p2, e2, 15, 0.045 * sizeS, 0.045 * sizeS, 0.0, 0.20, 5.0, 3.0, t, corrupt,
           2.0, vec3(1.0, 0.6, 0.7), dA);

  int v1[32];
  for (int i = 0; i < 32; i++) v1[i] = 32;
  v1[0]=86; v1[1]=69; v1[2]=82; v1[3]=83; v1[4]=73; v1[5]=79; v1[6]=78; v1[7]=32;
  v1[8]=48; v1[9]=48; v1[10]=49;
  diagLine(p2, v1, 11, 0.045 * sizeS, 0.045 * sizeS, 0.0, 0.05, 8.0, 2.0, t, corrupt,
           3.0, vec3(0.7, 0.9, 1.0), dA);
  int v2[32];
  for (int i = 0; i < 32; i++) v2[i] = 32;
  v2[0]=86; v2[1]=69; v2[2]=82; v2[3]=83; v2[4]=73; v2[5]=79; v2[6]=78; v2[7]=32;
  v2[8]=48; v2[9]=49; v2[10]=52;
  diagLine(p2, v2, 11, 0.045 * sizeS, 0.045 * sizeS, 0.0, -0.02, 9.5, 2.0, t, corrupt,
           3.5, vec3(0.7, 0.9, 1.0), dA);
  int v3[32];
  for (int i = 0; i < 32; i++) v3[i] = 32;
  v3[0]=86; v3[1]=69; v3[2]=82; v3[3]=83; v3[4]=73; v3[5]=79; v3[6]=78; v3[7]=32;
  v3[8]=57; v3[9]=50; v3[10]=55;
  diagLine(p2, v3, 11, 0.045 * sizeS, 0.045 * sizeS, 0.0, -0.09, 11.0, 2.0, t, corrupt,
           4.0, vec3(0.7, 0.9, 1.0), dA);

  int n1[32];
  for (int i = 0; i < 32; i++) n1[i] = 32;
  n1[0]=78; n1[1]=79; n1[2]=32; n1[3]=79; n1[4]=82; n1[5]=73; n1[6]=71; n1[7]=73;
  n1[8]=78; n1[9]=65; n1[10]=76; n1[11]=32; n1[12]=70; n1[13]=79; n1[14]=85; n1[15]=78; n1[16]=68;
  diagLine(p2, n1, 17, 0.058 * sizeS, 0.058 * sizeS, 0.0, -0.30, 14.5, 5.0, t, corrupt,
           5.0, vec3(1.0, 0.5, 0.6), 1.0);

  col += g_text;

  // handoff from the clean city
  if (uTransition < 0.999) {
    vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
    col = mix(prev, col, uTransition);
  }

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hitDist >= 0.0 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
