#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 9 // FINAL RECONSTRUCTION
// ---------------------------------------------------------------------------
// Silence. One particle appears; thousands reconstruct the NULL SECTOR
// wordmark from the shared font atlas. The reconstruction stays slightly
// incomplete (dropout + one dim glyph). Data readout appears below
// (MEMORY RECOVERY COMPLETE / DATA INTEGRITY: 0.003% / ORIGINAL REALITY:
// UNKNOWN / CREATING NEW REALITY...). The logo scatters, a final
// synchronized impact hits, then black.
//
// All cues are driven by uSectionLocal - the .nsd scene owns the length.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;
uniform float uMode;
uniform float uVolume;     // react.energy (near-silent section)
uniform vec2  uSceneRes;
uniform sampler2D uFont;   // atlas (unit 11)
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

/** clean readout line: chars reveal left-to-right, hold, fade */
void readLine(vec2 p, const int codes[40], int n, float charW, float charH,
              float cx, float cy, float t0, float hold, float t, float seed,
              vec3 tint, float baseA) {
  float on = smoothstep(t0, t0 + 0.35, t);
  if (on <= 0.01) return;
  float a = on * (1.0 - smoothstep(t0 + hold, t0 + hold + 0.8, t));
  if (a <= 0.01) return;
  float total = float(n) * charW;
  float x0 = cx - total * 0.5;
  if (p.x < x0 || p.x > x0 + total) return;
  if (p.y < cy - charH * 0.5 || p.y > cy + charH * 0.5) return;
  int ci = int(clamp(floor((p.x - x0) / charW), 0.0, float(n) - 1.0));
  if (ci >= n) return;
  float cF = float(ci);
  float lx = (p.x - (x0 + cF * charW)) / charW;
  float ly = (p.y - (cy - charH * 0.5)) / charH;
  float rev = t - t0;
  float cRev = clamp((rev - cF * 0.09) / 0.4, 0.0, 1.0);
  if (cRev <= 0.001) return;
  float g = glyphAt(codes[ci], vec2(lx, ly));
  float crisp = smoothstep(0.35, 0.75, g) * cRev;
  g_text += tint * crisp * a * baseA;
}

/** the wordmark: NULL SECTOR reconstructed from particles */
void wordmark(vec2 p, float asmb, float t, float d, vec2 res) {
  const float charW = 0.105;
  const float charH = 0.105;
  const float cy = 0.10;
  int codes[11];
  codes[0]=78; codes[1]=85; codes[2]=76; codes[3]=76; codes[4]=32;
  codes[5]=83; codes[6]=69; codes[7]=67; codes[8]=84; codes[9]=79; codes[10]=82;

  float total = 11.0 * charW;
  float x0 = -total * 0.5;
  if (p.x < x0 || p.x > x0 + total) return;
  if (p.y < cy - charH * 0.5 || p.y > cy + charH * 0.5) return;
  int ci = int(clamp(floor((p.x - x0) / charW), 0.0, 10.0));
  float cF = float(ci);
  float lx = (p.x - (x0 + cF * charW)) / charW;
  float ly = (p.y - (cy - charH * 0.5)) / charH;

  // per-char assembly: glyphs resolve left-to-right, each char's pixels pop
  // in with per-pixel hash stagger - "thousands of particles reconstruct"
  float charA = sat01(asmb * 1.5 - cF * 0.075);
  vec2 pid = floor(p * res * 0.5);                     // per-pixel id
  float ph = hash12(pid + cF * 3.7);
  float aPix = sat01((charA * 1.4 - ph * 0.6) * 1.6);  // staggered arrival

  // scatter: unarrived pixels show as dim drifting motes elsewhere in the box
  float g = glyphAt(codes[ci], vec2(lx, ly));
  float crisp = smoothstep(0.35, 0.75, g);

  // slight incompleteness: the final R stays faint; random dropout
  float incomplete = mix(1.0, 0.55, step(9.5, cF));
  float drop = step(hash12(pid * 1.3 + cF), 0.93 + 0.05 * ph);
  float cover = crisp * aPix * incomplete * drop;

  // drifting particles while assembling / scattering
  float motion = asmb < 0.999 ? (1.0 - aPix) : d;
  float mote = step(0.997, hash12(pid + vec2(0.0, floor(t * 3.0))));
  float drift = exp(-length(p - vec2(0.0, cy)) * 3.0);
  vec3 moteCol = palVoid(ph * 0.4 + musicHue(0.2)) * mote * motion * drift;

  // dark halo rim for legibility
  vec2 cellSize = uCell / uAtlas;
  vec2 cell0 = floor(vec2(lx, ly) / cellSize) * cellSize;
  vec2 dil = cellSize * 0.30;
  vec2 suv = vec2(lx, ly);
  float dl = g;
  dl = max(dl, texture(uFont, clamp(suv + vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv - vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv + vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv - vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  float halo = smoothstep(0.15, 0.45, dl) * (1.0 - crisp) * cover * 0.6;

  vec3 col = vec3(0.0);
  col += vec3(0.0, 0.015, 0.03) * halo;
  col += vec3(0.95, 0.99, 1.0) * cover;
  col += vec3(0.30, 0.85, 1.0) * cover * 0.25;
  col += vec3(0.5, 0.9, 1.0) * cover * (0.2 + 0.15 * sin(t * 2.0 + cF));
  // dissolving outward: particles fly off as d grows
  col *= 1.0 - d;
  col += vec3(0.6, 0.9, 1.0) * mote * d * 0.8;

  g_text += col + moteCol * 0.5;
}

void main() {
  float t = Null.uSectionLocal;
  float time = Null.uTime;
  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec2 p = (gl_FragCoord.xy - 0.5 * res) / res.y;
  float sizeS = clamp(res.y / 900.0, 0.7, 1.6);

  // --- timeline -------------------------------------------------------------
  float asmb = smoothstep(1.2, 8.0, t);        // particles reconstruct 1-8s
  float d = smoothstep(14.2, 16.2, t);         // logo scatters away
  float hold = asmb * (1.0 - d);

  // --- background: deep black with faint blue void ---------------------------
  vec3 col = vec3(0.002, 0.003, 0.008);
  col += vec3(0.01, 0.03, 0.08) * pow(1.0 - abs(uv.y) * 0.9, 2.0) * 0.4;

  // --- the one particle that appears first -----------------------------------
  float one = smoothstep(0.5, 1.2, t) * (1.0 - smoothstep(14.2, 15.5, t));
  if (one > 0.01) {
    float od = length(p - vec2(0.0, 0.10));
    col += vec3(0.8, 0.97, 1.0) * exp(-od * 300.0) * one;
    col += vec3(0.2, 0.7, 1.0) * exp(-od * 40.0) * one * 0.6;
  }

  // --- wordmark + readout ----------------------------------------------------
  g_text = vec3(0.0);
  wordmark(p, asmb, time, d, res);

  // readout lines
  int m1[40];
  for (int i = 0; i < 40; i++) m1[i] = 32;
  m1[0]=77; m1[1]=69; m1[2]=77; m1[3]=79; m1[4]=82; m1[5]=89; m1[6]=32; m1[7]=82;
  m1[8]=69; m1[9]=67; m1[10]=79; m1[11]=86; m1[12]=69; m1[13]=82; m1[14]=89; m1[15]=32;
  m1[16]=67; m1[17]=79; m1[18]=77; m1[19]=80; m1[20]=76; m1[21]=69; m1[22]=84; m1[23]=69;
  readLine(p, m1, 24, 0.040 * sizeS, 0.040 * sizeS, 0.0, -0.08, 3.0, 6.0, t, 1.0,
           vec3(0.55, 0.9, 1.0), hold);

  int m2[40];
  for (int i = 0; i < 40; i++) m2[i] = 32;
  m2[0]=68; m2[1]=65; m2[2]=84; m2[3]=65; m2[4]=32; m2[5]=73; m2[6]=78; m2[7]=84;
  m2[8]=69; m2[9]=71; m2[10]=82; m2[11]=73; m2[12]=84; m2[13]=89; m2[14]=58; m2[15]=32;
  m2[16]=48; m2[17]=46; m2[18]=48; m2[19]=48; m2[20]=51; m2[21]=37;
  readLine(p, m2, 22, 0.040 * sizeS, 0.040 * sizeS, 0.0, -0.155, 4.2, 6.0, t, 2.0,
           vec3(0.55, 0.9, 1.0), hold);

  int m3[40];
  for (int i = 0; i < 40; i++) m3[i] = 32;
  m3[0]=79; m3[1]=82; m3[2]=73; m3[3]=71; m3[4]=73; m3[5]=78; m3[6]=65; m3[7]=76;
  m3[8]=32; m3[9]=82; m3[10]=69; m3[11]=65; m3[12]=76; m3[13]=73; m3[14]=84; m3[15]=89;
  m3[16]=58; m3[17]=32; m3[18]=85; m3[19]=78; m3[20]=75; m3[21]=78; m3[22]=79; m3[23]=87; m3[24]=78;
  readLine(p, m3, 25, 0.040 * sizeS, 0.040 * sizeS, 0.0, -0.23, 5.4, 6.0, t, 3.0,
           vec3(0.55, 0.9, 1.0), hold);

  int m4[40];
  for (int i = 0; i < 40; i++) m4[i] = 32;
  m4[0]=67; m4[1]=82; m4[2]=69; m4[3]=65; m4[4]=84; m4[5]=73; m4[6]=78; m4[7]=71;
  m4[8]=32; m4[9]=78; m4[10]=69; m4[11]=87; m4[12]=32; m4[13]=82; m4[14]=69; m4[15]=65;
  m4[16]=76; m4[17]=73; m4[18]=84; m4[19]=89; m4[20]=46; m4[21]=46; m4[22]=46;
  readLine(p, m4, 23, 0.040 * sizeS, 0.040 * sizeS, 0.0, -0.30, 11.0, 5.5, t, 4.0,
           vec3(0.8, 0.97, 1.0), hold);

  // tagline: small, dim, below everything
  int tag[40];
  for (int i = 0; i < 40; i++) tag[i] = 32;
  tag[0]=82; tag[1]=69; tag[2]=65; tag[3]=76; tag[4]=73; tag[5]=84; tag[6]=89; tag[7]=32;
  tag[8]=73; tag[9]=83; tag[10]=32; tag[11]=79; tag[12]=78; tag[13]=76; tag[14]=89; tag[15]=32;
  tag[16]=84; tag[17]=72; tag[18]=69; tag[19]=32; tag[20]=82; tag[21]=69; tag[22]=78; tag[23]=68;
  tag[24]=69; tag[25]=82; tag[26]=69; tag[27]=68; tag[28]=32; tag[29]=82; tag[30]=69; tag[31]=83;
  tag[32]=85; tag[33]=76; tag[34]=84;
  readLine(p, tag, 35, 0.026 * sizeS, 0.026 * sizeS, 0.0, -0.44, 9.0, 8.0, t, 5.0,
           vec3(0.4, 0.7, 1.0), hold * 0.55);

  col += g_text;

  // --- final impact (synced to the track's impact at ~16.4s) -------------------
  float imp = smoothstep(16.35, 16.55, t) * (1.0 - smoothstep(16.6, 17.4, t));
  col += vec3(0.95, 0.99, 1.0) * imp * 1.4;
  col += palVoid(musicHue(0.3)) * imp * 0.5;
  // after the impact: pure black
  col *= 1.0 - smoothstep(17.2, 18.0, t);

  fragColor = vec4(col, 1.0);
  gl_FragDepth = 1.0;
}
