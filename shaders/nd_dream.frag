#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 6 // THE DREAM
// ---------------------------------------------------------------------------
// A beautiful surreal environment: a dark ocean-like surface, floating
// geometric islands, giant translucent structures, a glowing artificial sun,
// atmospheric particles and volumetric light. The AI inventing a world
// rather than remembering one - deliberately calm and gorgeous, a hard
// contrast with the technical corruption that preceded it.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;      // per-kick strobe (calm: gentle swells)
uniform float uMode;
uniform float uHigh;       // react.high (motes)
uniform float uVolume;
uniform vec2  uSceneRes;

out vec4 fragColor;

// --- dream palette: deep indigo ocean, teal islands, warm sun -----------------
vec3 palDream(float t) {
  vec3 deep = vec3(0.02, 0.05, 0.10);
  vec3 teal = vec3(0.05, 0.45, 0.50);
  vec3 cyan = vec3(0.55, 0.90, 0.95);
  vec3 warm = vec3(1.0, 0.72, 0.42);
  if (t < 0.4) return mix(deep, teal, t / 0.4);
  if (t < 0.7) return mix(teal, cyan, (t - 0.4) / 0.3);
  return mix(cyan, warm, (t - 0.7) / 0.3);
}

// --- SDFs --------------------------------------------------------------------
float sdOcta(vec3 p, float s) {
  p = abs(p);
  return (p.x + p.y + p.z - s) * 0.57735;
}
float sdTorus(vec3 p, vec2 t) {
  vec2 q = vec2(length(p.xz) - t.x, p.y);
  return length(q) - t.y;
}

float sdPlane(vec3 p, float h) { return p.y - h; }

/** ocean surface height at (x,z): gentle swell */
float oceanH(vec2 xz) {
  float w1 = fbm2(xz * 0.09 + vec2(3.0, 1.0));
  float w2 = fbm2(xz * 0.25 + vec2(9.0));
  return -0.35 + (w1 - 0.5) * 0.5 + (w2 - 0.5) * 0.2 + 0.15 * sin(xz.x * 0.3 + Null.uTime * 0.4);
}

// --- scene --------------------------------------------------------------------
struct Hit { float d; float id; };

/** island center + size (precomputed per frame, deterministic) */
vec3 islandC(int i, float time) {
  float fi = float(i);
  vec3 c = vec3((hash13(vec3(fi, 1.0, 5.0)) - 0.5) * 16.0,
                1.6 + hash13(vec3(fi, 2.0, 5.0)) * 4.5,
                (hash13(vec3(fi, 3.0, 5.0)) - 0.5) * 16.0);
  c.y += 0.35 * sin(time * 0.3 + fi * 2.1);        // slow float
  return c;
}

Hit map(vec3 p, float time, vec3 islands[7], float sizes[7], vec3 structs[2]) {
  Hit h;
  h.d = 1e9;
  h.id = 0.0;
  // ocean
  float od = p.y - oceanH(p.xz);
  h.d = od; h.id = 1.0;
  // islands: flattened octahedra, glowing
  for (int i = 0; i < 7; i++) {
    vec3 q = p - islands[i];
    q.y *= 0.55;                                    // flattened
    float d = sdOcta(q, sizes[i]);
    if (d < h.d) { h.d = d; h.id = 2.0 + float(i) * 0.01; }
  }
  // giant translucent structures: two huge rings in the sky
  for (int s = 0; s < 2; s++) {
    float d = sdTorus(p - structs[s], vec2(2.6, 0.28));
    if (d < h.d) { h.d = d; h.id = 9.0 + float(s); }
  }
  return h;
}

void main() {
  float t = Null.uSectionLocal;
  float time = Null.uTime;
  float bass = Null.uBass;
  float pulse = Null.uPulse;

  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  vec3 sunDir = normalize(vec3(0.42, 0.38, 0.24));
  vec3 skyTop = vec3(0.01, 0.02, 0.05);
  vec3 skyHor = palDream(0.15) * 0.5;

  // precompute island + structure positions
  vec3 islands[7];
  float sizes[7];
  for (int i = 0; i < 7; i++) {
    islands[i] = islandC(i, time);
    sizes[i] = 0.7 + hash13(vec3(float(i), 4.0, 5.0)) * 1.8;
  }
  vec3 structs[2];
  structs[0] = vec3(4.0 + 1.5 * sin(time * 0.1), 7.5, -6.0);
  structs[1] = vec3(-5.0 + 1.5 * cos(time * 0.08), 9.5, 2.0);

  // --- sky ---------------------------------------------------------------------
  float sunD = dot(rd, sunDir);
  vec3 col = mix(skyHor, skyTop, pow(max(uv.y, 0.0), 0.6));
  // sun disc + halo + radial god rays
  float sunDisc = smoothstep(0.9990, 0.9997, sunD);
  float sunGlow = pow(max(sunD, 0.0), 8.0) * 1.4;
  col += vec3(1.0, 0.78, 0.5) * sunDisc * 28.0;
  col += palDream(0.9) * sunGlow * 0.9;
  // god rays: sparse shafts near the sun
  float rayNoise = fbm2(gl_FragCoord.xy * 0.002 + vec2(0.0, time * 0.02));
  float shafts = smoothstep(0.9985, 1.0, sunD) * (0.3 + 0.7 * rayNoise);
  col += vec3(1.0, 0.8, 0.55) * shafts * 0.5;

  // --- march -------------------------------------------------------------------
  float tt = 0.0;
  vec3 p = ro;
  float id = 0.0;
  float hit = 0.0;
  for (int i = 0; i < 64; i++) {
    p = ro + rd * tt;
    Hit h = map(p, time, islands, sizes, structs);
    id = h.id;
    if (h.d < 0.003 * max(tt, 1.0)) { hit = 1.0; break; }
    tt += max(h.d * 0.8, 0.04);
    if (tt > 90.0) break;
  }

  if (hit > 0.5) {
    vec3 n;
    {
      const float eps = 0.004;
      Hit h1 = map(p + vec3(eps, 0, 0), time, islands, sizes, structs);
      Hit h2 = map(p - vec3(eps, 0, 0), time, islands, sizes, structs);
      Hit h3 = map(p + vec3(0, eps, 0), time, islands, sizes, structs);
      Hit h4 = map(p - vec3(0, eps, 0), time, islands, sizes, structs);
      Hit h5 = map(p + vec3(0, 0, eps), time, islands, sizes, structs);
      Hit h6 = map(p - vec3(0, 0, eps), time, islands, sizes, structs);
      n = normalize(vec3(h1.d - h2.d, h3.d - h4.d, h5.d - h6.d));
    }
    vec3 V = normalize(ro - p);
    float fres = pow(1.0 - max(dot(n, V), 0.0), 2.0);

    if (id > 8.5) {
      // --- giant translucent structure -----------------------------------------
      float s = (id - 9.0);
      vec3 scol = mix(palDream(0.55), palDream(0.8), s * 0.3);
      col = scol * (0.25 + 0.9 * fres) + palDream(0.9) * fres * fres * 2.0;
      // inner glow pulse
      col += palDream(0.6) * (0.15 + 0.2 * pulse) * (1.0 - fres);
      // backlight through the ring (translucency)
      float thru = sat01(0.5 + dot(rd, sunDir));
      col += vec3(1.0, 0.85, 0.6) * thru * 0.35 * fres;
    } else if (id > 1.5) {
      // --- floating island ------------------------------------------------------
      int ii = int(id - 2.0);
      float hb = hash13(vec3(float(ii), 7.0, 5.0));
      vec3 bcol = mix(vec3(0.02, 0.03, 0.06), palDream(0.4), 0.3 + 0.4 * hb);
      col = bcol * (0.6 + 0.5 * bass) + palDream(0.5) * fres * (1.2 + 1.4 * pulse);
      // geometric edge lines (near the octahedron edges)
      vec3 q = p - islands[ii];
      q.y *= 0.55;
      float edge = smoothstep(0.10, 0.0, abs(sdOcta(q, sizes[ii])) - sizes[ii] * 0.55);
      col += palDream(0.65) * edge * (0.8 + 0.6 * pulse) * (1.0 - fres * 0.5);
      // sun-facing rim
      float dif = max(dot(n, sunDir), 0.0);
      col += vec3(1.0, 0.8, 0.55) * dif * 0.5;
      // translucency: light through the island silhouette
      col += palDream(0.6) * fres * 0.5;
    } else {
      // --- ocean ---------------------------------------------------------------
      // reflective water: reflect sky + sun
      vec3 rr = reflect(rd, n);
      float rsd = dot(rr, sunDir);
      vec3 refCol = mix(skyHor, skyTop, pow(max(rr.y, 0.0), 0.6));
      refCol += vec3(1.0, 0.78, 0.5) * smoothstep(0.9990, 0.9997, rsd) * 28.0;
      refCol += palDream(0.9) * pow(max(rsd, 0.0), 10.0) * 1.2;
      float fresW = pow(1.0 - max(dot(rd, n), 0.0), 3.0);
      vec3 deep = vec3(0.01, 0.03, 0.07) + palDream(0.25) * 0.1 * (0.4 + bass * 1.2);
      col = mix(deep, refCol, clamp(fresW * 1.5, 0.08, 0.95));
      // ripples catching the sun
      col += vec3(1.0, 0.85, 0.6) * sat01(1.0 - abs(rsd - 0.9994) * 5000.0) * 0.35;
      // island reflections glinting
      col += palDream(0.6) * pow(max(dot(rr, normalize(vec3(0.2, 0.1, 0.3))), 0.0), 12.0) * 0.3;
    }
    // volumetric fog toward the horizon color
    float fog = 1.0 - exp(-tt * 0.012);
    col = mix(col, palDream(0.1) * 0.55, fog);
  } else {
    // volumetric haze toward the horizon (below the sun line)
    col += palDream(0.15) * exp(-abs(uv.y) * 3.0) * 0.3;
  }

  // --- atmospheric particles: drifting motes (uHigh brightens) -------------------
  {
    vec2 gid = floor(gl_FragCoord.xy * 0.25);
    float h = hash12(gid + vec2(0.0, floor(time * 1.5) * 0.13));
    float mote = step(0.9965, h);
    vec2 fp = fract(gl_FragCoord.xy * 0.25);
    float mot = exp(-length(fp - 0.5) * 7.0);
    col += palDream(0.8) * mote * mot * (0.25 + 0.75 * uHigh) * 0.8;
  }

  // --- SYSTEM FAILURE mode (uMode 1): the invented world comes apart ---------
  if (uMode > 0.5) {
    vec2 gs = glitchSlice(gl_FragCoord.y / res.y, 24.0, 1.0, uFlash, bass, 71.3, 1.0);
    if (abs(gs.x) > 0.004) {
      col = mix(col, palVoid(musicHue(0.4) + gs.y * 2.0), 0.4 + 0.3 * uFlash);
    }
    col = mix(col, palVoid(musicHue(0.2) + hash12(floor(gl_FragCoord.xy * 0.5) + vec2(floor(time * 8.0)))), 0.3 * (0.5 + 0.5 * sin(time * 30.0)));
    col *= 0.6 + 0.5 * uFlash;
  }

  // gentle kick swell (the dream breathes, never slams)
  col *= 1.0 + uFlash * 0.18;
  col += palDream(0.85) * uFlash * 0.08;

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hit > 0.5 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
