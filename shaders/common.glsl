// ---------------------------------------------------------------------------
// NULL SECTOR - shared GLSL library (injected into every shader via #include)
// ---------------------------------------------------------------------------

precision highp float;
precision highp int;

// ---------------------------------------------------------------------------
// Shared per-frame state: camera + the timeline/music values every raymarcher
// actually consumes. One std140 uniform block at binding point 0, written
// once per frame - see engine/ubo.ts. Members are read as Null.<name>; the
// instance name keeps them from colliding with standalone uniforms in
// screen-space shaders.
//
// Only members that shaders genuinely read live here. The pruned set
// (uBeatPhase, uBar, uBarPhase, uDrop, uSection, uSectionProgress, uMid,
// uTreble, uRMS, uCentroid, uKick, uEnergy, uPeak, uPhrase, uChapter,
// uQuality, uFwd) had zero shader reads - TS-side state (timeline.s,
// audio.react) is consumed directly by the director/effects, not through the
// UBO. uView survives for particles.vert's world->view transform; uProj
// drives depthFromViewZ; uCamRot + uFovTan build rays in every raymarcher.
// ---------------------------------------------------------------------------
layout(std140) uniform NullBlock {
  vec2  uRes;
  float uTime;
  float uFovTan;
  vec3  uCamPos;
  mat3  uCamRot;
  mat4  uView;
  mat4  uProj;
  float uBeat;
  float uPulse;
  float uIntensity;
  float uSectionLocal;
  float uBass;
  float uOnset;
  float uAnticipation;
  float uExitRamp;
  float uMusicHue;   // chord hue of the current bar (0..1 palVoid space)
  float uMusicHue2;  // chord hue of the next bar (in-bar interpolation)
  float uBarPhase;   // 0..1 progress within the current bar
  float uBar;        // absolute bar index (float)
  float uAssembly;   // 0..1 per-kick assembly ratchet (cathedral / machine)
  float uSectionDur; // current section duration (seconds)
  float uSecBar;     // seconds per bar (BAR) - drives bar-quantized cycles
} Null;

// --- math -------------------------------------------------------------------
const float PI = 3.14159265358979;
const float TAU = 6.28318530717959;

float sat01(float x) { return clamp(x, 0.0, 1.0); }
vec3 satV(vec3 v) { return clamp(v, 0.0, 1.0); }
vec2 rotate2(vec2 p, float a) { float c = cos(a); float s = sin(a); return vec2(c * p.x - s * p.y, s * p.x + c * p.y); }

// --- hashing -----------------------------------------------------------------
float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float hash13(vec3 p3) {
  p3 = fract(p3 * 0.1031);
  p3 += dot(p3, p3.zyx + 31.32);
  return fract((p3.x + p3.y) * p3.z);
}

// --- music-reactive glitch (shared by every glitch scene) ----------------------
// The logo sub-title corruption (logo.frag) and the reprise tunnel seizure
// (quantum_tunnel.frag uMode 1) are driven by one music-reactive model: per-
// band horizontal slice tears whose participation and amplitude ride the
// kick strobe, sub-bass analyser and downbeat bursts, plus an RGB split that
// widens with the hits. Keeping it here means every glitch scene tears the
// same way - a single source of truth for the beat-locked corruption.

/** downbeat burst envelope: peaks exactly on the downbeat (uBarPhase = 0) and
 *  decays across the bar. `scale` is the section-relative escalation (0 at the
 *  start, ramping to 1) so the bursts build as the section progresses. */
float glitchBurst(float scale) {
  return exp(-Null.uBarPhase * 5.0) * scale;
}

/** slice participation: kicks, sub-bass and downbeat bursts raise how many
 *  bands tear and how hard. Shared curve - glitchSlice and scenes that need
 *  the same participation threshold (e.g. the tunnel's palette glitch) use
 *  this one source of truth. */
float glitchParticipation(float ghost, float kick, float bass, float burstScale) {
  return sat01(ghost * 0.8 + kick * 0.35 + bass * 0.30 + glitchBurst(burstScale) * 0.55);
}

/** music-reactive slice tear + RGB split for a horizontal glitch tear.
 *  band       - coordinate driving the slice banding (glyph-cell y for text,
 *               screen-frac y for whole-view tears, depth for wall tears)
 *  bands      - number of slices across the banded range (tear fineness)
 *  ghost      - 0..1 corruption ramp (0 = clean, 1 = fully seized)
 *  kick       - per-kick strobe 0..1 (uFlash)
 *  bass       - sub-bass analyser 0..1
 *  seed       - per-instance hash seed so scenes/glyphs tear independently
 *  burstScale - escalation passed to glitchBurst (0..1)
 *  Returns vec2(tear displacement, RGB split width) in NORMALIZED units
 *  (1.0 = one band interval); callers scale to their own coordinate space -
 *  glyph-cell units for text (charH), half-height uv units for view tears.
 *  The 0.45 tear clamp is intentional: glyph ink occupies only ~0.6 of a
 *  cell, so displacement beyond ~0.19 cell lands in atlas padding and ERASES
 *  strokes instead of shearing them (verified in capture) - 0.45 stays a
 *  strong-but-readable smear. */
vec2 glitchSlice(float band, float bands, float ghost, float kick, float bass,
                 float seed, float burstScale) {
  float burst = glitchBurst(burstScale);
  float jt = floor(Null.uTime * 5.0);
  float b = floor(band * bands);
  float rt = hash12(vec2(b, floor(Null.uTime * 8.0) + seed));
  // kicks, sub-bass and downbeat bursts raise slice participation + amplitude
  float p = glitchParticipation(ghost, kick, bass, burstScale);
  float react = 0.6 + 0.4 * (0.7 + 0.7 * kick + 0.5 * bass);
  float tear = step(1.0 - p, rt) * (rt - 0.5) * 0.45 * ghost * react;
  // occasional whole-line jump (independent of react so it never stacks)
  tear += step(0.95 - ghost * 0.5 - kick * 0.25, hash12(vec2(jt, 7.3)))
        * (hash12(vec2(jt + 13.0, 2.9)) - 0.5) * 0.5 * ghost;
  // downbeat burst: the whole line throws sideways on each downbeat
  tear += (hash12(vec2(floor(Null.uBar), 5.1)) - 0.5) * 0.5 * burst;
  tear = clamp(tear, -0.45, 0.45);
  // RGB split: channels sample slightly different x offsets (VHS style),
  // widening with the hits
  float split = 0.08 * ghost * (0.4 + 0.6 * hash12(vec2(b, jt)))
              * (1.0 + 1.1 * kick + 0.7 * bass);
  return vec2(tear, split);
}


// --- value noise + fbm ---------------------------------------------------------
float vnoise2(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash21(i);
  float b = hash21(i + vec2(1.0, 0.0));
  float c = hash21(i + vec2(0.0, 1.0));
  float d = hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float vnoise3(vec3 p) {
  vec3 i = floor(p);
  vec3 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float n000 = hash13(i);
  float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
  float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
  float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
  float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
  float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
  float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
  float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
  float nx00 = mix(n000, n100, f.x);
  float nx10 = mix(n010, n110, f.x);
  float nx01 = mix(n001, n101, f.x);
  float nx11 = mix(n011, n111, f.x);
  float nxy0 = mix(nx00, nx10, f.y);
  float nxy1 = mix(nx01, nx11, f.y);
  return mix(nxy0, nxy1, f.z);
}
float fbm2(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 5; i++) {
    v += a * vnoise2(p);
    p = p * 2.03 + 17.1;
    a *= 0.5;
  }
  return v;
}
float fbm3(vec3 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 4; i++) {
    v += a * vnoise3(p);
    p = p * 2.13 + 7.7;
    a *= 0.5;
  }
  return v;
}
/** domain-warped fbm (classic 3-noise warp) */
float warp(vec2 p, float s) {
  vec2 q = vec2(fbm2(p + s), fbm2(p + vec2(5.2, 1.3) + s));
  return fbm2(p + 3.0 * q);
}
/** cheap 2-octave 3D fbm (for particle flow noise) */
float fbm3q(vec3 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 2; i++) {
    v += a * vnoise3(p);
    p = p * 2.17 + 11.1;
    a *= 0.5;
  }
  return v;
}

// --- voronoi --------------------------------------------------------------------
/** voronoi with edge-distance (used for fracture / shatter masks) */
float voronoiEdge(vec2 p) {
  vec2 ip = floor(p);
  float md = 8.0;
  float md2 = 8.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      vec2 off = vec2(float(x), float(y));
      vec2 pt = ip + off + hash21(ip + off);
      vec2 d = pt - p;
      float dd = dot(d, d);
      if (dd < md) { md2 = md; md = dd; }
      else if (dd < md2) md2 = dd;
    }
  }
  // true distance to the cell boundary (low near seams, high in cell interior)
  return clamp(sqrt(md2) - sqrt(md), 0.0, 1.0);
}

// --- palettes -------------------------------------------------------------------
/** NULL SECTOR palette: purple -> blue -> cyan -> magenta */
vec3 palVoid(float t) {
  t = fract(t);
  vec3 purple = vec3(0.45, 0.15, 0.95);
  vec3 blue = vec3(0.1, 0.4, 1.0);
  vec3 cyan = vec3(0.0, 0.9, 1.0);
  vec3 magenta = vec3(1.0, 0.15, 0.85);
  if (t < 0.33) return mix(purple, blue, t / 0.33);
  if (t < 0.66) return mix(blue, cyan, (t - 0.33) / 0.33);
  return mix(cyan, magenta, (t - 0.66) / 0.34);
}
/** musical chord hue: current bar's chord color, gliding toward the next
 *  bar's chord across the bar so each bar reads as a distinct color state
 *  while chord changes stay smooth. `offset` shifts within palVoid space
 *  (use for secondary accents / per-scene variation). */
float musicHue() {
  return mix(Null.uMusicHue, Null.uMusicHue2, sat01(Null.uBarPhase));
}
float musicHue(float offset) { return fract(musicHue() + offset); }

/** sunset synthwave palette */
vec3 palSunset(float t) {
  vec3 deep = vec3(0.35, 0.05, 0.4);
  vec3 mid = vec3(1.0, 0.25, 0.35);
  vec3 sun = vec3(1.0, 0.7, 0.3);
  if (t < 0.5) return mix(deep, mid, t * 2.0);
  return mix(mid, sun, (t - 0.5) * 2.0);
}

/** iridescent scanline weave for bitmap text. `glyphUV` is the atlas UV, so
 *  the pattern restarts inside each glyph instead of becoming a screen-sized
 *  overlay. `pixel` is supplied by fragment shaders to keep this helper valid
 *  for both vertex and fragment stages that include common.glsl. */
vec3 textSurface(vec3 base, vec2 glyphUV, float seed, float time, vec2 pixel, float amount) {
  vec2 local = fract(glyphUV * vec2(16.0, 8.0));
  float weave = 0.5 + 0.5 * sin(local.y * 30.0 + local.x * 9.0 + seed * 29.0 + time * 1.2);
  float grain = hash12(floor(pixel * 0.7) + vec2(floor(time * 8.0), seed * 17.0));
  float fleck = step(0.86, hash12(floor(local * vec2(7.0, 9.0)) + vec2(seed * 13.0, floor(time * 3.0))));
  vec3 accent = palVoid(seed + local.x * 0.30 - local.y * 0.22 + time * 0.04);
  vec3 textured = base * (0.74 + 0.42 * weave) + accent * (0.08 + grain * 0.16 + fleck * 0.32);
  return mix(base, textured, amount);
}

// --- projection depth -----------------------------------------------------------
/** gl_FragDepth from positive view-space distance */
float depthFromViewZ(float viewZ) {
  vec4 p = Null.uProj * vec4(0.0, 0.0, viewZ, 1.0);
  return clamp(p.z / p.w * 0.5 + 0.5, 0.0, 1.0);
}

// --- hdr / tonemap ---------------------------------------------------------------
vec3 aces(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 tonemapACES(vec3 c) {
  return aces(c * 0.92);
}
