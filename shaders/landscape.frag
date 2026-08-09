#version 300 es
// ---------------------------------------------------------------------------
// SCENE 2 - Fractal Landscape
// Infinite procedural terrain, synthwave sunset, god rays, clouds, reflective
// water. Heightfield raymarch with lighting + fog.
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)

out vec4 fragColor;

// terrain heightfield (domain warped fbm)
float terrain(vec2 p) {
  float e = warp(p * 0.045 + vec2(3.7, 1.2), Null.uTime * 0.05);
  float h = e * 12.0;
  h += fbm2(p * 0.09 + vec2(11.0)) * 3.0;
  h -= fbm2(p * 0.02 + vec2(5.0)) * 1.5; // large scale valleys
  h += 0.8 * sin(p.x * 0.02) * sin(p.y * 0.017);
  return h;
}

// cheap 3-octave fbm used for the terrain march (full 5-octave only at hit)
float fbm2l(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 3; i++) {
    v += a * vnoise2(p);
    p = p * 2.03 + 17.1;
    a *= 0.5;
  }
  return v;
}

// cheaper terrain for marching - same structure as terrain(), fewer octaves
float terrainLite(vec2 p) {
  vec2 base = p * 0.045 + vec2(3.7, 1.2);
  vec2 q = vec2(
    fbm2l(base + vec2(0.0, Null.uTime * 0.05)),
    fbm2l(base + vec2(5.2, 1.3) + vec2(Null.uTime * 0.05)));
  float e = fbm2l(base + 3.0 * q);
  float h = e * 12.0;
  h += fbm2l(p * 0.09 + vec2(11.0)) * 3.0;
  h -= fbm2l(p * 0.02 + vec2(5.0)) * 1.5;
  return h;
}

// cloud layer density at a world point (billboarded fbm plane ~ y=60)
float clouds(vec2 p) {
  float c = warp(p * 0.012 + vec2(0.0, Null.uTime * 0.02), Null.uTime * 0.03);
  return sat01(c * 1.6 - 0.65);
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // --- sky / sun ------------------------------------------------------------
  float sunDirDot = rd.y * 0.9 + rd.x * 0.2; // sun near horizon, east-ish
  // sun disc + glow
  vec3 sunPos = normalize(vec3(0.32, 0.16, 0.35));
  float sunD = dot(rd, sunPos);
  float sunDisc = smoothstep(0.9992, 0.9998, sunD);
  float sunGlow = pow(max(sunD, 0.0), 6.0) * 1.2;

  vec3 skyCol = palSunset(musicHue() * 0.35 + 0.28 + pow(max(sunDirDot, 0.0), 1.5) * 0.45);
  skyCol += palSunset(0.75) * sunGlow * 2.0;
  skyCol += vec3(1.0, 0.85, 0.55) * sunDisc * 30.0; // hdr disc

  // cloud layer (drawn behind terrain only)
  vec3 cloudCol = vec3(0);
  if (rd.y > 0.01) {
    float ct = (60.0 - ro.y) / rd.y;
    if (ct > 0.0) {
      vec2 cp = ro.xz + rd.xz * ct;
      float c = clouds(cp);
      float cloudShade = 0.6 + 0.4 * sin(Null.uTime + cp.x * 0.05);
      cloudCol = vec3(1.0, 0.55, 0.45) * c * (1.0 + sunGlow * 0.8) * cloudShade * 1.4;
    }
  }

  vec3 col = skyCol + cloudCol;

  // --- god rays: sample light shaft density toward sun -------------------------
  // (only near the sun disc, sampled against the cheap terrain surface)
  float rays = 1.0;
  if (sunGlow > 0.05) {
    float rr = 0.0;
    float tt = 0.0;
    for (int i = 0; i < 10; i++) {
      tt += 4.0 + float(i) * 2.0;
      vec3 sp = ro + sunPos * tt;
      if (sp.y > terrainLite(sp.xz)) rr += 0.5; // density above terrain
    }
    rays = exp(-rr * 0.35);
  }
  col += vec3(1.0, 0.6, 0.35) * rays * sunGlow * (0.25 + 0.35 * Null.uIntensity);

  // --- terrain march ----------------------------------------------------------
  float t = 0.0;
  vec3 p = ro;
  float water = 0.0;
  float hit = 0.0;

  // a rising ray can only hit terrain while it stays below the highest peaks
  const float MAX_TERRAIN = 24.0;
  bool mayHit = rd.y < 0.0 || ro.y < MAX_TERRAIN;
  if (mayHit) {
    for (int i = 0; i < 56; i++) {
      p = ro + rd * t;
      // risen above every peak and still going up: no hit possible
      if (rd.y >= 0.0 && p.y > MAX_TERRAIN + 2.0) break;
      float h = terrainLite(p.xz);
      if (p.y < h) { hit = 1.0; break; }
      float step = clamp((p.y - h) * 0.45, 0.35, 3.0);
      t += step;
      if (t > 340.0) break;
    }
  }

  if (hit > 0.5) {
    // the march ran against the cheaper surface - resolve against the real one
    float hF = terrain(p.xz);
    if (hF < -0.02) water = 1.0; // terrain below sea level = reflective water
    p.y = min(p.y, hF);

    if (water > 0.5) {
      // --- reflective water ----------------------------------------------------
      vec3 n = vec3(0.0, 1.0, 0.0);
      // ripple normal
      float w1 = fbm2(p.xz * 0.8 + Null.uTime * 0.35);
      float w2 = fbm2(p.xz * 1.7 - Null.uTime * 0.4 + 7.0);
      n.x = (w1 - 0.5) * 0.12;
      n.z = (w2 - 0.5) * 0.12;
      n = normalize(n);
      vec3 rr = reflect(rd, n);
      // reflected sky
      float rsd = dot(rr, sunPos);
      vec3 refCol = palSunset(musicHue() * 0.35 + 0.28 + pow(max(rr.y * 0.9 + rr.x * 0.2, 0.0), 1.5) * 0.45);
      refCol += vec3(1.0, 0.85, 0.55) * smoothstep(0.9992, 0.9998, rsd) * 30.0;
      refCol += palSunset(0.75) * pow(max(rsd, 0.0), 8.0) * 2.0;
      // reflected terrain approximation (mirrored sample)
      float th = terrain(p.xz + rr.xz * 40.0);
      if (th > 0.5) refCol += palSunset(0.1) * 0.5;
      float fres = pow(1.0 - max(dot(rd, n), 0.0), 3.0);
      col = mix(vec3(0.05, 0.03, 0.09) * 0.5, refCol, clamp(fres * 1.4, 0.15, 1.0));
      col += vec3(1.0, 0.6, 0.3) * sat01(1.0 - abs(rsd - 0.9995) * 4000.0) * 0.4;
    } else {
      // --- terrain shading ------------------------------------------------------
      vec2 e = vec2(0.12, 0.0);
      vec3 n = normalize(vec3(
        terrain(p.xz - e.xy) - terrain(p.xz + e.xy),
        2.0 * e.x,
        terrain(p.xz - e.yx) - terrain(p.xz + e.yx)));

      float diff = max(dot(n, sunPos), 0.0);
      // sunset light: warm from sun, cool ambient
      vec3 amb = mix(palSunset(0.1), palSunset(0.3), p.y * 0.02) * 0.35;
      amb *= 0.75 + 0.5 * musicHue();
      vec3 sunCol = vec3(1.0, 0.45, 0.25);
      vec3 colT = amb + sunCol * diff * (0.8 + Null.uIntensity * 0.3);

      // slope-based color zones (mountain shading)
      float slope = 1.0 - n.y;
      colT = mix(colT, vec3(0.35, 0.12, 0.3) * 1.2, sat01(slope * 2.5 - 0.8));
      colT = mix(colT, vec3(0.12, 0.18, 0.35) * 0.8, sat01(p.y * 0.02 - 0.15)); // water-adjacent dark

      // ambient occlusion by height contrast
      float ao = clamp(terrain(p.xz + n.xz * 2.0) - p.y, -1.0, 1.0);
      colT *= 1.0 - sat01(ao) * 0.4;

      // specular sun glint on wet rock
      vec3 hv = normalize(sunPos - rd);
      float spec = pow(max(dot(n, hv), 0.0), 40.0);
      colT += vec3(1.0, 0.7, 0.4) * spec * 1.2;

      col = colT;
    }

    // distance fog toward horizon color
    float fog = 1.0 - exp(-t * 0.0035);
    col = mix(col, palSunset(0.32) * 0.6, fog);
  }

  // per-kick strobe: the whole landscape slams white-hot with the kick drum
  col *= 1.0 + uFlash * 0.35;
  col += vec3(1.0, 0.95, 0.9) * uFlash * 0.2;

  // depth: far plane for pixels that never hit terrain (keeps DOF sane)
  float viewZ = hit > 0.5 ? -dot(p - ro, -Null.uCamRot[2]) : -400.0;
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
