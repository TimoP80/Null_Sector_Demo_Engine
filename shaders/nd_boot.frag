#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 1 // BOOT / MEMORY RECOVERY
// ---------------------------------------------------------------------------
// A clean technical cold open. Sparse diagnostic text reconstructs over a
// dark void while a wireframe structure (icosahedron + drifting fragment
// beams) assembles from scattered vertices - an AI rebuilding a vanished
// reality from corrupted memory. Slow, minimal, mysterious.
//
// Text is sampled from the shared TrueType font atlas (bound on unit 11 by
// the ndboot effect), so the production shares the engine's exact glyphs.
// All cues are driven by uSectionLocal (the .nsd scene owns the length).
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;     // per-kick strobe (quiet in boot - no kick yet)
uniform float uMode;
uniform vec2  uSceneRes;  // ACTUAL render target size (renderScale < 1)
uniform sampler2D uFont;  // font atlas (unit 11, wired by the effect)
uniform vec2  uAtlas;     // atlas size in texels
uniform vec2  uCell;      // glyph cell size in texels
uniform float uHigh;      // react.high (treble band)

out vec4 fragColor;

vec3 g_text = vec3(0.0);

// --- atlas glyph helpers (same conventions as intro_logo.frag) --------------
/** coverage of one atlas glyph by ascii code + cell-local uv (y 0 = bottom) */
float glyphAt(int code, vec2 luv) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

/** coverage of a random atlas cell (reconstruction scramble) */
float glyphRandom(vec2 luv, float seed) {
  vec2 lc = fract(luv * vec2(16.0, 8.0));
  vec2 target = vec2(fract(hash12(vec2(seed, 0.1)) * 16.0),
                     fract(hash12(vec2(seed, 0.7)) * 8.0));
  vec2 uv = (target + lc) / vec2(16.0, 8.0);
  return texture(uFont, uv).a;
}

/** one centered/left boot line: chars reconstruct left-to-right with
 *  scramble flicker, bright core + dark halo rim for readability. */
void bootLine(vec2 p, const int codes[32], int n, float charW, float charH,
              float cx, float cy, float t0, float hold, float fade,
              float seed, float t, bool title) {
  float start = max(t0, 0.0);
  float on = smoothstep(start, start + 0.9, t);              // reveal window
  if (on <= 0.01) return;
  float a = on * (1.0 - smoothstep(start + hold, start + hold + fade, t));
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

  // per-char reveal with a scramble: unrevealed glyphs flicker random cells
  float rev = t - start;
  float cRev = clamp((rev - cF * 0.06) / 0.5, 0.0, 1.0);
  if (cRev <= 0.001) return;
  vec2 suv = vec2(lx, ly);
  float g = glyphAt(codes[ci], suv);
  float crisp = smoothstep(0.35, 0.75, g) * cRev;

  // scramble: random glyph flicker while this char is still resolving
  float rc = 0.0;
  if (cRev < 0.99) {
    rc = smoothstep(0.35, 0.75,
                    glyphRandom(suv, seed + cF * 3.7 + floor(t * 14.0) * 0.13));
  }

  // dark halo rim (dilated coverage minus the core) for legibility
  vec2 cellSize = uCell / uAtlas;
  vec2 cell0 = floor(suv / cellSize) * cellSize;
  vec2 dil = cellSize * 0.30;
  float dl = g;
  dl = max(dl, texture(uFont, clamp(suv + vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv - vec2(dil.x, 0.0), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv + vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  dl = max(dl, texture(uFont, clamp(suv - vec2(0.0, dil.y), cell0, cell0 + cellSize)).a);
  float halo = smoothstep(0.15, 0.45, dl) * (1.0 - crisp);

  vec3 col = vec3(0.0);
  col += vec3(0.0, 0.015, 0.03) * halo;                       // slate rim
  if (title) {
    col += vec3(0.95, 0.99, 1.0) * crisp;                     // bright title
    col += vec3(0.30, 0.85, 1.0) * crisp * 0.25;
  } else {
    col += vec3(0.85, 0.96, 1.0) * crisp * 0.9;
    col += vec3(0.25, 0.8, 1.0) * crisp * 0.18;
  }
  col += vec3(0.2, 0.8, 1.0) * rc * 0.4 * (1.0 - cRev);       // scramble flicker
  // horizontal slice jump glitch while resolving
  float slice = floor(ly * 5.0);
  float jump = step(0.94, hash12(vec2(slice + seed, floor(t * 9.0))));
  col *= 1.0 - jump * 0.55 * (1.0 - cRev);

  g_text += col * a;
}

// ---------------------------------------------------------------------------
// wireframe: a slowly assembling icosahedron + drifting fragment beams
// ---------------------------------------------------------------------------
const float PHI = 1.6180339887;

vec3 icoVertex(int i) {
  // 12 icosahedron vertices from permutations of (+-1, +-phi, 0)
  int q = i / 4;          // 0..2 axis group
  int r = i % 4;          // 0..3 sign pattern
  vec3 v = vec3(0.0);
  float s0 = (r & 1) == 0 ? 1.0 : -1.0;
  float s1 = (r & 2) == 0 ? 1.0 : -1.0;
  if (q == 0)      v = vec3(0.0, s0 * 1.0, s1 * PHI);
  else if (q == 1) v = vec3(s0 * 1.0, s1 * PHI, 0.0);
  else             v = vec3(s0 * PHI, 0.0, s1 * 1.0);
  return normalize(v);
}

// hardcoded adjacency: pairs (i,j) of icosahedron vertices (30 edges)
const int E0[30] = int[30](0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
                           3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9);
const int E1[30] = int[30](1, 2, 3, 4, 5, 6, 7, 8, 9, 6, 7, 8, 10, 6, 7,
                           9, 11, 8, 10, 11, 8, 9, 10, 10, 11, 9, 10, 11, 9, 11);

/** assembled vertex: scatter -> target as uAsmb grows; per-vertex stagger */
vec3 icoPoint(int i, float asmb, float time) {
  vec3 tgt = icoVertex(i) * 5.2;
  vec3 scatter = normalize(vec3(hash13(vec3(float(i), 1.0, 7.0)) - 0.5,
                                hash13(vec3(float(i), 2.0, 7.0)) - 0.5,
                                hash13(vec3(float(i), 3.0, 7.0)) - 0.5)) * 9.0;
  float a = sat01(asmb * 1.35 - float(i) * 0.02);            // staggered
  vec3 p = mix(tgt + scatter, tgt, smoothstep(0.0, 1.0, a));
  // breathing + slow rotation of the whole frame
  p.xz = rotate2(p.xz, time * 0.05 + 0.3);
  p.xy = rotate2(p.xy, time * 0.035);
  return p;
}

/** point-to-segment SQUARED distance (no sqrt - min-finding + the step
 *  threshold below use it; one sqrt at the end of the frame loop). */
float sdSeg2(vec3 p, vec3 a, vec3 ab, float invLen2) {
  vec3 ap = p - a;
  float t = clamp(dot(ap, ab) * invLen2, 0.0, 1.0);
  vec3 q = ap - ab * t;
  return dot(q, q);
}

/** beam endpoints (precomputed once per frame) */
void beamEnds(int bi, float time, out vec3 a, out vec3 b) {
  float fb = float(b);
  float phase = hash13(vec3(fb, 9.0, 2.0)) * 6.28;
  float rad = 6.5 + 3.0 * hash13(vec3(fb, 4.0, 2.0));
  vec3 c = vec3(cos(time * 0.16 + phase) * rad,
                sin(time * 0.13 + phase * 1.7) * 3.2,
                sin(time * 0.14 + phase * 0.6) * rad);
  vec3 dir = normalize(vec3(hash13(vec3(fb, 6.0, 2.0)) - 0.5,
                            hash13(vec3(fb, 7.0, 2.0)) - 0.5,
                            hash13(vec3(fb, 8.0, 2.0)) - 0.5));
  float bl = 0.8 + 2.2 * hash13(vec3(fb, 5.0, 2.0));
  a = c - dir * bl * 0.5;
  b = c + dir * bl * 0.5;
}

/** wireframe + fragments: distance + glow over PRECOMPUTED per-frame geometry */
float wireframe(vec3 p, float asmb,
                vec3 ea[30], vec3 eab[30], float einv[30],
                vec3 ba[8], vec3 bab[8], float binv[8],
                vec3 vp[8], float ew[30], float bw[8],
                out float edgeGlow) {
  edgeGlow = 0.0;
  float d2 = 1e18;   // min SQUARED distance - no sqrts in the segment loops
  float reveal = 0.0;
  for (int ei = 0; ei < 30; ei++) {
    // per-edge reveal: edges light up staggered as the frame assembles
    float edgeA = sat01(asmb * 2.6 - float(ei) * 0.03);
    if (edgeA <= 0.001) continue;
    float s2 = sdSeg2(p, ea[ei], eab[ei], einv[ei]);
    if (s2 < d2) { d2 = s2; reveal = edgeA * ew[ei]; }
  }
  // drifting fragment beams: fragmented geometry pieces orbiting the frame
  for (int bi = 0; bi < 8; bi++) {
    float fb = float(bi);
    float bA = sat01(asmb * 3.0 - 0.5 - fb * 0.06);          // beams follow
    float s2 = sdSeg2(p, ba[bi], bab[bi], binv[bi]);
    if (s2 < d2) { d2 = s2; reveal = bA * bw[bi]; }
  }
  // occasional bright vertex sparkles (the assembly "pins")
  for (int vi = 0; vi < 8; vi++) {
    vec3 q = p - vp[vi];
    float s2 = dot(q, q);
    if (s2 < d2) { d2 = s2; reveal = 0.8 * sat01(asmb * 2.2 - float(vi) * 0.04); }
  }
  // ONE exp at the nearest distance (exp(-26*d) ~ 1e-4 beyond d=0.35, so only
  // the closest segment contributes visibly) - weighted by that segment's own
  // reveal/energy so the staggered assembly still reads. The old per-segment
  // accumulation was ~46 exp()+sqrt() per march step; this is 1.
  if (d2 < 0.25) edgeGlow = exp(-sqrt(d2) * 7.0) * reveal;
  return sqrt(d2);
}

void main() {
  float t = Null.uSectionLocal;                    // 0..scene length (24.4s)
  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // --- timeline envelope ----------------------------------------------------
  float asmb = smoothstep(9.0, 22.0, t);           // frame assembles 9-22s
  float textEnv = 1.0 - smoothstep(20.0, 23.5, t); // text fades as the
                                                   // structure takes over
  float bassPulse = 0.4 + 0.6 * Null.uBass;        // sub drone breathing

  // --- background -------------------------------------------------------------
  vec3 col = vec3(0.004, 0.006, 0.014);
  // faint vertical gradient + distant glow behind the frame
  col += vec3(0.02, 0.05, 0.12) * pow(1.0 - abs(uv.y) * 0.85, 2.0) * 0.5;
  // sparse data-dust glints (screen-space, cheap)
  {
    vec2 gid = floor(gl_FragCoord.xy * 0.35);
    float h = hash12(gid + vec2(floor(t * 2.0) * 0.13, 3.0));
    float glint = step(0.9975, h);
    vec2 fp = fract(gl_FragCoord.xy * 0.35);
    float mot = exp(-length(fp - vec2(0.5)) * 8.0);
    col += vec3(0.35, 0.8, 1.0) * glint * mot * (0.3 + 0.7 * uHigh) * 0.5;
  }

  // --- raymarch the wireframe -------------------------------------------------
  // precompute the assembled geometry ONCE per frame (the march then only
  // measures point-to-segment distances - this was the whole cost before)
  vec3 ea[30], eb[30], eab[30];
  float einv[30];
  for (int ei = 0; ei < 30; ei++) {
    ea[ei] = icoPoint(E0[ei], asmb, Null.uTime);
    eb[ei] = icoPoint(E1[ei], asmb, Null.uTime);
    eab[ei] = eb[ei] - ea[ei];
    einv[ei] = 1.0 / max(dot(eab[ei], eab[ei]), 1e-6);   // precomputed: the
    // per-step sdSeg2 then needs no division (was ~650 divs/pixel)
  }
  vec3 ba[8], bb[8], bab[8];
  float binv[8];
  for (int bi = 0; bi < 8; bi++) {
    beamEnds(bi, Null.uTime, ba[bi], bb[bi]);
    bab[bi] = bb[bi] - ba[bi];
    binv[bi] = 1.0 / max(dot(bab[bi], bab[bi]), 1e-6);
  }
  vec3 vp[8];
  for (int vi = 0; vi < 8; vi++) vp[vi] = icoPoint(vi, asmb, Null.uTime);
  // per-segment glow weights are loop-invariant: compute once per frame
  float ew[30];
  for (int ei = 0; ei < 30; ei++) ew[ei] = 0.5 + 0.5 * hash13(vec3(float(ei), 0.5, 1.0));
  float bw[8];
  for (int bi = 0; bi < 8; bi++) bw[bi] = 0.35 + 0.65 * hash13(vec3(float(bi), 3.0, 2.0));

  // bounding sphere: skip the whole march for rays that miss the frame
  vec3 oc = ro;
  float bq2 = dot(oc, rd);
  float c2 = dot(oc, oc) - 7.2 * 7.2;
  float disc = bq2 * bq2 - c2;
  if (disc > 0.0) {
    float tnear = max(0.0, -bq2 - sqrt(disc));
    float tfar = max(0.0, -bq2 + sqrt(disc));
    if (tfar > 0.0 && tnear < 24.0) {
      float tt = tnear;
      float glow = 0.0;
      for (int i = 0; i < 8; i++) {
        vec3 p = ro + rd * tt;
        float edge = 0.0;
        float sd = wireframe(p, asmb, ea, eab, einv, ba, bab, binv, vp, ew, bw, edge);
        glow += edge * 0.14;             // halo integrates along the ray: each
        // step samples the glow field, so the wire reads as a soft luminous
        // line instead of a hard edge (single-exp wireframe keeps it cheap)
        if (sd < 0.006) break;
        tt += max(sd * 0.6, 0.09);
        if (tt > tfar + 2.0 || tt > 30.0) break;
      }
      glow = sat01(glow);
      // wire: cool cyan core + faint halo; vertex pins whiter
      col += vec3(0.10, 0.55, 1.0) * glow * (0.6 + 0.6 * bassPulse);
      col += vec3(0.7, 0.95, 1.0) * glow * glow * 1.2;
      col += vec3(0.05, 0.2, 0.5) * glow * 0.8;
    }
  }

  // --- boot diagnostics ---------------------------------------------------------
  vec2 p = (gl_FragCoord.xy - 0.5 * res) / res.y;
  float sizeS = clamp(res.y / 900.0, 0.7, 1.6);

  // NULL SECTOR SYSTEM (title) / MEMORY RECOVERY PROTOCOL
  int w1[32];
  for (int i = 0; i < 32; i++) w1[i] = 32;
  w1[0]=78; w1[1]=85; w1[2]=76; w1[3]=76; w1[4]=32; w1[5]=83; w1[6]=69; w1[7]=67;
  w1[8]=84; w1[9]=79; w1[10]=82; w1[11]=32; w1[12]=83; w1[13]=89; w1[14]=83; w1[15]=84;
  w1[16]=69; w1[17]=77;
  bootLine(p, w1, 18, 0.085 * sizeS, 0.085 * sizeS, 0.0, 0.34, 1.0, 7.0, 1.2, 1.0, t, true);

  int w2[32];
  for (int i = 0; i < 32; i++) w2[i] = 32;
  w2[0]=77; w2[1]=69; w2[2]=77; w2[3]=79; w2[4]=82; w2[5]=89; w2[6]=32; w2[7]=82;
  w2[8]=69; w2[9]=67; w2[10]=79; w2[11]=86; w2[12]=69; w2[13]=82; w2[14]=89; w2[15]=32;
  w2[16]=80; w2[17]=82; w2[18]=79; w2[19]=84; w2[20]=79; w2[21]=67; w2[22]=79; w2[23]=76;
  bootLine(p, w2, 24, 0.05 * sizeS, 0.05 * sizeS, 0.0, 0.20, 2.2, 6.0, 1.0, 2.0, t, false);

  // stats block (left column)
  int s1[32];
  for (int i = 0; i < 32; i++) s1[i] = 32;
  s1[0]=66; s1[1]=76; s1[2]=79; s1[3]=67; s1[4]=75; s1[5]=83; s1[6]=32; s1[7]=70;
  s1[8]=79; s1[9]=85; s1[10]=78; s1[11]=68; s1[12]=58; s1[13]=32; s1[14]=48; s1[15]=48;
  s1[16]=48; s1[17]=48; s1[18]=48; s1[19]=48; s1[20]=49; s1[21]=55;
  bootLine(p, s1, 22, 0.042 * sizeS, 0.042 * sizeS, -0.62, 0.05, 5.5, 6.0, 1.0, 3.0, t, false);

  int s2[32];
  for (int i = 0; i < 32; i++) s2[i] = 32;
  s2[0]=66; s2[1]=76; s2[2]=79; s2[3]=67; s2[4]=75; s2[5]=83; s2[6]=32; s2[7]=67;
  s2[8]=79; s2[9]=82; s2[10]=82; s2[11]=85; s2[12]=80; s2[13]=84; s2[14]=69; s2[15]=68;
  s2[16]=58; s2[17]=32; s2[18]=48; s2[19]=48; s2[20]=48; s2[21]=48; s2[22]=48; s2[23]=49; s2[24]=49;
  bootLine(p, s2, 25, 0.042 * sizeS, 0.042 * sizeS, -0.62, -0.02, 7.0, 6.0, 1.0, 4.0, t, false);

  int s3[32];
  for (int i = 0; i < 32; i++) s3[i] = 32;
  s3[0]=83; s3[1]=73; s3[2]=71; s3[3]=78; s3[4]=65; s3[5]=76; s3[6]=32; s3[7]=73;
  s3[8]=78; s3[9]=84; s3[10]=69; s3[11]=71; s3[12]=82; s3[13]=73; s3[14]=84; s3[15]=89;
  s3[16]=58; s3[17]=32; s3[18]=49; s3[19]=51; s3[20]=46; s3[21]=55; s3[22]=37;
  bootLine(p, s3, 23, 0.042 * sizeS, 0.042 * sizeS, -0.62, -0.09, 8.5, 6.0, 1.0, 5.0, t, false);

  // RECONSTRUCTION STARTED... + blinking cursor
  int s4[32];
  for (int i = 0; i < 32; i++) s4[i] = 32;
  s4[0]=82; s4[1]=69; s4[2]=67; s4[3]=79; s4[4]=78; s4[5]=83; s4[6]=84; s4[7]=82;
  s4[8]=85; s4[9]=67; s4[10]=84; s4[11]=73; s4[12]=79; s4[13]=78; s4[14]=32; s4[15]=83;
  s4[16]=84; s4[17]=65; s4[18]=82; s4[19]=84; s4[20]=69; s4[21]=68; s4[22]=46; s4[23]=46; s4[24]=46;
  bootLine(p, s4, 25, 0.05 * sizeS, 0.05 * sizeS, 0.0, -0.30, 10.5, 40.0, 1.0, 6.0, t, false);
  // cursor block
  float cur = smoothstep(11.0, 11.6, t) * (1.0 - smoothstep(23.0, 24.0, t));
  float blink = 0.5 + 0.5 * sin(t * 6.0);
  if (cur > 0.01 && abs(p.y + 0.30) < 0.025 * sizeS && p.x > 0.30 && p.x < 0.345) {
    g_text += vec3(0.7, 1.0, 1.0) * blink * cur;
  }

  col += g_text * textEnv;

  // subtle scanline sheen over everything (technical feel)
  col *= 1.0 + 0.05 * sin(gl_FragCoord.y * 0.9 + Null.uTime * 3.0) * 0.5;

  fragColor = vec4(col, 1.0);
  gl_FragDepth = 1.0;
}
