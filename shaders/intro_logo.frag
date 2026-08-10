#version 300 es
// ---------------------------------------------------------------------------
// INTRO SYSTEM 9 - LogoAssembler: the 0:20 reveal. A large circular scanner
// expands while the background darkens and the "NULL SECTOR" wordmark
// assembles from fragmented geometry (never fades - every fragment hashes to
// a scatter offset that converges as uAsmb grows). "Null Sector Demo Engine"
// constructs underneath with a scan sweep. All letters are sampled from the
// font atlas, so the wordmark shares the engine's exact glyph style.
//
// Readability: every glyph also contributes a dark halo rim (dilated coverage
// minus the crisp core) that darkens the scene underneath, so the wordmark
// reads as bright core + dark outline instead of a blown-out white glow.
// Final pass: composites over the incoming composed scene (uScene), applies
// the background darkening and the end-of-intro camera zoom.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uScene;  // composed scene so far (unit 0)
uniform sampler2D uFont;   // font atlas (unit 1)
uniform vec2 uRes;
uniform float uTime;
uniform float uAsmb;       // 0..1 wordmark assembly progress
uniform float uScan;       // 0..1 scanner expansion
uniform float uDark;       // 0..1 background darkening
uniform float uZoom;       // 0..1 camera accel through the logo (end)
uniform vec2 uParallax;
uniform vec2 uAtlas;
uniform vec2 uCell;

out vec4 fragColor;

vec3 g_col = vec3(0.0);    // accumulated bright logo layer color
float g_dark = 0.0;        // accumulated dark halo mask (darkens the scene)

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

/** dilated coverage: sample the glyph at a small atlas offset for the rim.
 *  luv is cell-local, so clamping (luv + off) to [0,1] keeps the sample
 *  inside this glyph's own atlas cell - never bleeding into neighbours. */
float glyphDil(int code, vec2 luv, vec2 off) {
  int gx = code % 16;
  int gy = code / 16;
  vec2 u0 = (vec2(float(gx), float(gy)) * uCell) / uAtlas;
  vec2 u1 = (vec2(float(gx) + 1.0, float(gy) + 1.0) * uCell) / uAtlas;
  vec2 uv = vec2(mix(u0.x, u1.x, luv.x), mix(u0.y, u1.y, 1.0 - luv.y));
  return texture(uFont, uv).a;
}

/** draw one centered line of atlas glyphs, per-char staggered assembly */
void wordLine(vec2 p, int n, int codes[23], float charW, float charH,
              float cy, float stagger, float asmb, vec3 tint, float seedOff) {
  float total = float(n) * charW;
  float x0 = -total * 0.5;

  for (int i = 0; i < 23; i++) {
    if (i >= n) continue;
    float gx = x0 + float(i) * charW;
    if (p.x < gx || p.x > gx + charW) continue;
    if (p.y < cy - charH * 0.5 || p.y > cy + charH * 0.5) continue;

    float lx = (p.x - gx) / charW;
    float ly = (p.y - (cy - charH * 0.5)) / charH;

    // per-char assembly: glyphs resolve left to right with stagger
    float a = sat01(asmb - float(i) * stagger);
    vec2 cid = vec2(float(i), seedOff);
    float h = hash12(cid + floor(p * 900.0) * 0.013);
    float fragOn = step(h, a * 1.15 - 0.07);

    // scatter: while off, the glyph is sampled from a displaced fragment
    vec2 luv = vec2(lx, ly);
    if (fragOn < 0.5) {
      float da = 1.0 - a;
      luv = fract(luv + vec2(h * 3.0 - 1.5, hash12(cid + h) * 3.0 - 1.5) * da * 2.0);
    }
    float g = glyph(codes[i], luv);
    if (g < 0.02) continue;
    float crisp = smoothstep(0.3, 0.7, g) * fragOn;

    // dark halo rim: dilated coverage minus the core (darker = more contrast).
    // Dilation offsets are cell-local; glyphDil clamps them inside the cell.
    float dl = 0.0;
    dl = max(dl, glyphDil(codes[i], luv, vec2(0.18, 0.0)));
    dl = max(dl, glyphDil(codes[i], luv, vec2(-0.18, 0.0)));
    dl = max(dl, glyphDil(codes[i], luv, vec2(0.0, 0.18)));
    dl = max(dl, glyphDil(codes[i], luv, vec2(0.0, -0.18)));
    float halo = smoothstep(0.15, 0.45, dl) * (1.0 - crisp);
    g_dark += halo;

    // bright core, restrained cyan accents (no white-out)
    g_col += vec3(0.92, 0.97, 1.0) * crisp * 0.92;
    g_col += tint * crisp * (0.18 + 0.22 * (0.5 + 0.5 * sin(uTime * 2.0 + float(i))));
    g_col += vec3(0.4, 0.9, 1.0) * crisp * 0.12;
    // scan sweep highlight while assembling (toned down)
    float sweep = 1.0 - smoothstep(0.0, 0.1, abs(ly - fract(uTime * 0.5) * 1.2));
    g_col += vec3(0.9, 1.0, 1.0) * sweep * crisp * 0.28 * a;
  }
}

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / uRes.y;
  p -= uParallax * 0.2;

  // --- camera accel through the center: scale coords + scene outward ---------
  float zs = 1.0 + uZoom * 1.6;
  vec2 zp = p * zs;
  vec2 zuv = clamp(0.5 + (uv - 0.5) * zs, 0.0, 1.0);

  // background: incoming scene darkened
  vec3 scene = texture(uScene, zuv).rgb;
  vec3 col = scene * (1.0 - 0.9 * uDark);
  // deep black pool behind the wordmark
  col *= 1.0 - smoothstep(0.35, 0.75, length(zp)) * uDark * 0.85;

  g_col = vec3(0.0);
  g_dark = 0.0;

  // --- circular scanner rings (expand with uScan) ----------------------------
  float r = length(zp);
  for (int i = 0; i < 5; i++) {
    float fi = float(i);
    float rr = 0.22 + fi * 0.16 + uScan * 0.8;
    float ring = exp(-abs(r - rr) * 60.0);
    float a = atan(zp.y, zp.x);
    float arcM = smoothstep(0.0, 0.15, sin(a * 2.0 + uTime * (0.4 + fi * 0.15)));
    g_col += vec3(0.15, 0.7, 1.0) * ring * arcM * uScan * 0.4;
  }
  // rotating scan line + trailing gradient
  {
    float a = atan(zp.y, zp.x);
    float sa = uTime * 1.2;
    float dA = abs(mod(a - sa + PI, TAU) - PI);
    g_col += vec3(0.7, 1.0, 1.0) * exp(-dA * 40.0) * uScan * 0.8;
    g_col += vec3(0.3, 0.8, 1.0) * exp(-dA * 8.0) * uScan * 0.3;
  }
  // tick marks on the outer ring
  for (int i = 0; i < 24; i++) {
    float ta = float(i) / 24.0 * TAU;
    vec2 tp = vec2(cos(ta), sin(ta)) * (0.55 + uScan * 0.3);
    float td = length(zp - tp);
    g_col += vec3(0.3, 0.8, 1.0) * exp(-td * 300.0) * uScan * 0.6;
  }

  // --- wordmark: NULL SECTOR (11 chars) + NULL SECTOR DEMO ENGINE (23 chars) ----
  int word[23];
  for (int i = 0; i < 23; i++) word[i] = 32;
  word[0] = 78; word[1] = 85; word[2] = 76; word[3] = 76; word[4] = 32;
  word[5] = 83; word[6] = 69; word[7] = 67; word[8] = 84; word[9] = 79; word[10] = 82;

  int sub[23];
  for (int i = 0; i < 23; i++) sub[i] = 32;
  sub[0] = 78; sub[1] = 85; sub[2] = 76; sub[3] = 76; sub[4] = 32;
  sub[5] = 83; sub[6] = 69; sub[7] = 67; sub[8] = 84; sub[9] = 79; sub[10] = 82; sub[11] = 32;
  sub[12] = 68; sub[13] = 69; sub[14] = 77; sub[15] = 79; sub[16] = 32;
  sub[17] = 69; sub[18] = 78; sub[19] = 71; sub[20] = 73; sub[21] = 78; sub[22] = 69;

  float subAsmb = sat01((uAsmb - 0.35) / 0.65);

  // the wordmark sits near the screen center; apply zoom toward it
  vec2 wp = zp - uParallax * 0.2;
  wordLine(wp, 11, word, 0.092, 0.092, 0.06, 0.045, uAsmb, vec3(0.0, 0.8, 1.0), 1.0);
  wordLine(wp, 23, sub, 0.036, 0.036, -0.18, 0.03, subAsmb, vec3(0.3, 0.85, 1.0), 2.0);

  // dark halo behind the wordmark first (max contrast), then bright layer
  col *= 1.0 - clamp(g_dark, 0.0, 1.0) * 0.85;

  // vignette the logo layer, add the assembly burst
  g_col *= 1.0 - smoothstep(0.45, 1.3, r) * 0.6;
  float burst = smoothstep(0.55, 0.75, uAsmb) * (1.0 - smoothstep(0.75, 1.0, uAsmb));
  g_col += vec3(0.8, 0.95, 1.0) * burst * 0.22;

  // final white-out as the camera accelerates through (kept, slightly toned)
  col += g_col * (1.0 + uZoom * 0.8);
  col += vec3(0.85, 0.97, 1.0) * smoothstep(0.6, 1.0, uZoom) * 0.75;

  fragColor = vec4(col, 1.0);
}
