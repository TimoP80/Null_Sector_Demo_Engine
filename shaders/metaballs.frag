#version 300 es
// ---------------------------------------------------------------------------
// SCENE 3 - Metaballs
// Classic 90s metaballs as an implicit isosurface (sum of r^2/d^2),
// chrome material with procedural HDR environment reflections.
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)

out vec4 fragColor;

// blob field: sum of (r^2 / dist^2)
// Each blob orbits at its OWN radius (1.8..3.6). A fixed ring (the old 2.6)
// saturates the field everywhere: the inter-blob valleys sat at ~3.8 while the
// isosurface threshold is 0.82, so the whole thing collapsed into ONE
// featureless ball. Varied radii keep the valleys near the surface, so the
// mass reads as a lumpy, organic cluster of chrome orbs.
float field(vec3 p) {
  float f = 0.0;
  for (int i = 0; i < 14; i++) {
    float fi = float(i);
    float a1 = Null.uTime * (0.5 + 0.12 * hash13(vec3(fi, 1.0, 0.0)));
    float a2 = Null.uTime * (0.35 + 0.1 * hash13(vec3(fi, 2.0, 1.0)));
    float a3 = Null.uTime * (0.42 + 0.1 * hash13(vec3(fi, 3.0, 2.0)));
    float R = 1.8 + 1.8 * hash13(vec3(fi, 4.0, 3.0));
    vec3 off = vec3(
      R * sin(a1 + fi * 1.7),
      2.6 * sin(a2 + fi * 2.3) * cos(a1 * 0.7),
      R * cos(a3 + fi * 1.3));
    float r = 0.72 + 0.15 * sin(a1 + fi) + 0.12 * Null.uBass;
    vec3 c = off;
    vec3 d = p - c;
    f += (r * r) / (dot(d, d) + 0.12);
  }
  return f;
}

// numeric gradient of the field
vec3 fieldN(vec3 p) {
  vec2 e = vec2(0.01, 0.0);
  return normalize(vec3(
    field(p + e.xyy) - field(p - e.xyy),
    field(p + e.yxy) - field(p - e.yxy),
    field(p + e.yyx) - field(p - e.yyx)));
}

// procedural chrome environment
vec3 env(vec3 d) {
  // vertical gradient + horizontal neon banding
  float band = sin(d.y * 18.0) * sin(d.x * 14.0) * sin(d.z * 12.0);
  vec3 base = mix(vec3(0.35, 0.45, 0.65), vec3(0.08, 0.1, 0.16), d.y * 0.5 + 0.5);
  base += palVoid(musicHue() + band * 0.02) * 0.3;
  // cool-tinted sheen instead of the old pure-white top band (which turned
  // the whole blob cluster into one giant white ball)
  base += vec3(0.85, 0.92, 1.0) * pow(max(d.y, 0.0), 3.0) * 0.55;
  // neon grid floor reflection
  float g = step(0.05, abs(fract(d.x * 3.0) - 0.5) * fract(d.z * 3.0) - 0.5);
  base += vec3(0.2, 0.6, 1.0) * (1.0 - g) * 0.15;
  return base;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  float hit = 0.0;

  for (int i = 0; i < 80; i++) {
    p = ro + rd * t;
    float f = field(p);
    if (f > 0.82) { hit = 1.0; break; }
    // step grows when far from the surface
    float st = 0.18 + sat01(1.0 - f) * 0.35;
    t += st;
    if (t > 30.0) break;
  }

  if (hit < 0.5) {
    // background: dark room with drifting glow
    vec3 bg = env(rd) * 0.06;
    float halo = pow(max(dot(rd, normalize(vec3(0.0, 0.0, -1.0))), 0.0), 3.0);
    bg += palVoid(musicHue() * 0.5) * halo * (0.04 + Null.uPulse * 0.05);
    fragColor = vec4(bg, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  vec3 n = fieldN(p);

  // --- chrome / PBR-ish shading ----------------------------------------------
  vec3 V = normalize(ro - p);
  vec3 R = reflect(-V, n);

  float fres = pow(1.0 - max(dot(n, V), 0.0), 3.0);

  // environment reflection (chrome)
  vec3 refl = env(R) * (1.6 + fres * 2.0);
  vec3 diff = env(n) * 0.25;

  // dynamic colored point lights orbiting the mass
  vec3 acc = vec3(0.0);
  for (int i = 0; i < 3; i++) {
    float fi = float(i);
    vec3 lp = vec3(
      7.5 * sin(Null.uTime * 0.7 + fi * 2.2),
      2.2 + 2.8 * sin(Null.uTime * 0.9 + fi * 1.3),
      5.5 * cos(Null.uTime * 0.55 + fi * 1.9));
    vec3 L = normalize(lp - p);
    vec3 lc = palVoid(musicHue(0.1) + fi * 0.33);
    float dif = max(dot(n, L), 0.0);
    vec3 H = normalize(L + V);
    float spc = pow(max(dot(n, H), 0.0), 60.0);
    float att = 1.0 / (1.0 + dot(lp - p, lp - p) * 0.1);
    acc += lc * (dif * 0.9 + spc * 3.0) * att;
  }

  // accent light tied to bass
  vec3 bassL = normalize(vec3(sin(Null.uTime * 0.8), -0.4, cos(Null.uTime * 0.8)) - p);
  acc += vec3(0.5, 0.8, 1.0) * max(dot(n, bassL), 0.0) * Null.uBass * 1.6;

  vec3 col = diff + refl + acc * (1.0 + fres * 0.5);
  col *= 0.9 + 0.3 * Null.uPulse;

  // subtle AO from field gradient
  col *= 1.0 - sat01(field(p + n * 0.35) - field(p)) * 0.5;

  // per-kick strobe: chrome flairs and the whole mass brightens with the kick
  col *= 1.0 + uFlash * 0.4;
  col += vec3(0.9, 0.95, 1.0) * uFlash * 0.25;

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
