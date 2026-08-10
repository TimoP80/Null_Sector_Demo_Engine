#version 300 es
// ---------------------------------------------------------------------------
// SCENE 7 - NULL SECTOR logo reveal (the climax).
// Screen-space glitch reconstruction: the wordmark mask (assets/splash.png -
// the same image as the pre-show splash) reassembles from fragments over the
// first half of the section, holds through the climax and slams on every kick.
//
// In-scene handoff: during the first two beats the outgoing voxel grid fades
// into the logo while fragments over its bright city lights ignite first -
// the logo is born from the voxel grid's neon (SceneFX drives uTransition
// from timeline.s.transition and binds the previous frame on unit 9).
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;       // 0..1 per-kick strobe (audio kick analyser)
uniform float uTransition;  // 0..1 handoff window (1 = handoff done)
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)
uniform sampler2D uLogoTex;   // logo mask (unit 10)
uniform sampler2D uFont;      // TrueType font atlas (unit 11)
uniform vec2 uAtlas;          // font atlas size (px)
uniform vec2 uCell;           // glyph cell size (px)

out vec4 fragColor;

/** sample one atlas glyph by ASCII code + cell-local uv (luv.y 0 = bottom).
 *  Atlas cells are indexed by the RAW ascii code (code % 16, code / 16),
 *  exactly like TextMesh::glyphUVs - do NOT subtract 32 here. */
float glyph(int code, vec2 luv) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

void main() {
  gl_FragDepth = 1.0;   // far plane

  // screen uv, un-flipped (stb_image rows are top-down) + aspect-fit letterbox
  vec2 uv = vec2(gl_FragCoord.x / Null.uRes.x, 1.0 - gl_FragCoord.y / Null.uRes.y);
  float winAspect = Null.uRes.x / max(Null.uRes.y, 1.0);
  vec2 texSize = vec2(textureSize(uLogoTex, 0));
  float texAspect = texSize.x / max(texSize.y, 1.0);
  vec2 fit = vec2(1.0);
  if (winAspect > texAspect) fit.x = texAspect / winAspect;
  else fit.y = winAspect / texAspect;
  vec2 tuv = (uv - 0.5) * fit + 0.5;
  float inside = step(0.0, tuv.x) * step(tuv.x, 1.0) * step(0.0, tuv.y) * step(tuv.y, 1.0);

  // luminance mask: bright wordmark over the dark backdrop. Guarded on the
  // texture actually being bound (splash.png missing -> mask 0, never sample
  // whatever texture happens to linger on the sampler's default unit).
  float hasLogo = float(textureSize(uLogoTex, 0).x > 0);
  vec4 logo = texture(uLogoTex, tuv);
  float mask = smoothstep(0.10, 0.38, max(max(logo.r, logo.g), logo.b)) * inside * hasLogo;

  // wordmark band: the reveal shows only the NULL SECTOR wordmark rows of the
  // poster (the baked caption above it and the bottom line are excluded - the
  // subtitle is drawn procedurally beneath it via the TrueType atlas, so the
  // scene matches the brief: wordmark + procedural sub-title underneath).
  float band = smoothstep(0.478, 0.500, tuv.y) * (1.0 - smoothstep(0.638, 0.660, tuv.y));
  mask *= band;

  // assembly envelope: fragments flip in over the first half of the section
  // (normalized by the real duration so a schedule re-time stays in sync),
  // then hold through the climax
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  float asmb = smoothstep(0.03, 0.52, secT);
  // scanline sweep + per-cell hash: the letters appear through reconstruction
  float wave = 0.5 + 0.5 * sin(tuv.y * 26.0 - Null.uTime * 7.0);
  float frag = hash12(floor(tuv * vec2(150.0, 84.0)) + floor(Null.uTime * 5.0) * 1.7);
  float reveal = smoothstep(0.30, 0.72, asmb * 1.35 + wave * 0.24 + frag * 0.16);

  // handoff: fragments over the voxel grid's bright pixels ignite first
  float handoff = 1.0 - uTransition;
  vec3 prevCol = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
  float lum = max(max(prevCol.r, prevCol.g), prevCol.b);
  reveal = sat01(reveal + handoff * lum * 1.5);

  vec3 logoCol = palVoid(musicHue(0.15)) * 0.9 + vec3(1.0, 0.98, 1.0) * 0.9;
  vec3 col = vec3(0.006, 0.008, 0.03);
  col += logoCol * mask * reveal * (0.55 + 0.5 * Null.uPulse);
  col += vec3(1.0, 0.99, 1.0) * mask * reveal * 0.25;   // hot core
  col += vec3(1.0, 0.98, 1.0) * mask * uFlash * 0.45;   // kick slam

  // handoff: the voxel grid fades into the assembling logo
  if (handoff > 0.001) col = mix(prevCol * 1.15, col, uTransition);

  // --- subtitle: NULL SECTOR DEMO ENGINE (TrueType atlas, unit 11) --------------
  // Assembles beneath the wordmark after it resolves - the brief's sub-title,
  // constructed procedurally (staggered left-to-right reconstruction with hash
  // scatter while off, no fade-in). Anchored in image space just below the
  // wordmark band, so it tracks the wordmark through any window aspect ratio.
  // p-space: y up, vertical span +-0.5 (uRes.y = 1.0).
  float subAsmb = sat01((secT - 0.42) / 0.30);   // wordmark done ~0.40, sub follows
  // ghost corruption: hash-slice tears + RGB split sweep the line late in the
  // climax, just before the reprise tunnel tears the whole frame apart. The
  // reprise handoff carries this corrupted frame into the tunnel, which then
  // glitches/tears it further - the ghost visibly seizing the title it made.
  float ghostT = sat01((secT - 0.82) / 0.18);    // 0 at 0.82, 1 at section end
  vec2 sp = (gl_FragCoord.xy - 0.5 * Null.uRes) / Null.uRes.y;
  const float subYImg = 0.725;                  // subtitle center, image frac
  const float subHImg = 0.030;                  // char height, image frac
  float subY = (0.5 - subYImg) / fit.y;
  float charH = subHImg / fit.y;                // cellW == cellH -> charW == charH
  const int n = 23;
  float total = float(n) * charH;
  float x0 = -total * 0.5;

  // y-band gate folded into the condition (no early return - the fragColor
  // write below must run for every fragment)
  if (subAsmb > 0.001 && sp.y >= subY - charH * 0.5 && sp.y <= subY + charH * 0.5) {
    int codes[23];
    for (int ci = 0; ci < 23; ci++) codes[ci] = 32;
    codes[0] = 78; codes[1] = 85; codes[2] = 76; codes[3] = 76; codes[4] = 32;
    codes[5] = 83; codes[6] = 69; codes[7] = 67; codes[8] = 84; codes[9] = 79; codes[10] = 82; codes[11] = 32;
    codes[12] = 68; codes[13] = 69; codes[14] = 77; codes[15] = 79; codes[16] = 32;
    codes[17] = 69; codes[18] = 78; codes[19] = 71; codes[20] = 73; codes[21] = 78; codes[22] = 69;

    vec3 subCol = vec3(0.0);
    // music-reactive (computed only inside the gate, so the dormant path is
    // zero-cost): every kick flash and sub-bass hit amplifies the tears + RGB
    // split, and each downbeat in the final ramp fires a full slice burst -
    // exp(-uBarPhase*5) peaks exactly ON the downbeat and decays across the
    // bar, so the burst stays locked to the kick grid
    float kickE = uFlash;             // per-kick strobe (audio kick analyser)
    float bassE = Null.uBass;         // sub-bass analyser (0..1)
    for (int i = 0; i < 23; i++) {
      float gx = x0 + float(i) * charH;
      if (sp.x < gx || sp.x > gx + charH) continue;

      // per-char assembly: glyphs resolve left to right with stagger
      float a = sat01(subAsmb - float(i) * 0.02);
      vec2 cid = vec2(float(i), 2.0);
      float h = hash12(cid + floor(sp * 900.0) * 0.013);
      float fragOn = step(h, a * 1.15 - 0.07);

      // scatter: while off, the glyph is sampled from a displaced fragment
      vec2 luv = vec2((sp.x - gx) / charH, (sp.y - (subY - charH * 0.5)) / charH);
      if (fragOn < 0.5) {
        float da = 1.0 - a;
        luv = fract(luv + vec2(h * 3.0 - 1.5, hash12(cid + h) * 3.0 - 1.5) * da * 2.0);
      }
      float g = glyph(codes[i], luv);
      if (g < 0.02) continue;
      float crisp = smoothstep(0.3, 0.7, g) * fragOn;

      // ghost corruption: per-band hash tear + RGB split displace the atlas
      // sample. Base path keeps the single sample; only while the corruption
      // ramp is live do we pay the 3x channel-separated sampling.
      float crispR = crisp, crispG = crisp, crispB = crisp;
      if (ghostT > 0.001) {
        // shared music-reactive tear + RGB split (glitchSlice in common.glsl) -
        // the SAME model drives the reprise tunnel's seizure, so every glitch
        // scene stays visually consistent. band = char-cell y (line-local
        // slices that shatter the line regardless of window aspect), 14 bands,
        // seed decorrelates the per-glyph tears; ghostT doubles as the burst
        // escalation so downbeats hit harder as the climax peaks. The 0.45
        // clamp inside keeps the title readable (ink only fills ~0.6 of a
        // cell; beyond ~0.19 cell the sample lands in atlas padding and
        // ERASES strokes instead of shearing them - verified in capture).
        vec2 gs = glitchSlice(luv.y, 14.0, ghostT, kickE, bassE, float(i) * 0.13, ghostT);
        float tear = gs.x;
        float split = gs.y;
        float r = smoothstep(0.3, 0.7, glyph(codes[i], vec2(fract(luv.x + tear + split), luv.y))) * fragOn;
        float g = smoothstep(0.3, 0.7, glyph(codes[i], vec2(fract(luv.x + tear), luv.y))) * fragOn;
        float b = smoothstep(0.3, 0.7, glyph(codes[i], vec2(fract(luv.x + tear - split), luv.y))) * fragOn;
        // clean-core blend: the undisplaced crisp always anchors a readable
        // core (100% clean at ghostT 0 -> 40% at full corruption), so the torn
        // copies smear over it as ghost slices while the title itself survives
        float anchor = mix(1.0, 0.40, ghostT);
        crispR = crisp * anchor + r * (1.0 - anchor);
        crispG = crisp * anchor + g * (1.0 - anchor);
        crispB = crisp * anchor + b * (1.0 - anchor);
      }

      // restrained cyan core + musical tint, kick slam, assembly scan sweep.
      // The core splits per-channel so the tear reads as chromatic aberration.
      vec3 tint = palVoid(musicHue(0.2) + float(i) * 0.004);
      subCol += vec3(0.92, 0.97, 1.0) * vec3(crispR, crispG, crispB) * 0.80;
      subCol += tint * crispG * (0.20 + 0.18 * (0.5 + 0.5 * sin(Null.uTime * 2.0 + float(i))));
      subCol += vec3(1.0, 0.98, 1.0) * crispG * uFlash * 0.35;
      // ghost's magenta cast rides the corruption ramp + the hits
      subCol += vec3(1.0, 0.25, 0.85) * crispR * ghostT * 0.18 * (0.5 + 0.9 * kickE + 0.6 * bassE);
      float sweep = 1.0 - smoothstep(0.0, 0.1, abs(luv.y - fract(Null.uTime * 0.5) * 1.2));
      subCol += vec3(0.9, 1.0, 1.0) * sweep * crispG * 0.28 * a;
    }
    col += subCol * (0.55 + 0.5 * Null.uPulse);
  }

  fragColor = vec4(col, 1.0);
}
