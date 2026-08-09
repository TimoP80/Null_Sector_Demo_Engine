#version 300 es
// ---------------------------------------------------------------------------
// SCENE 5 - Particle Storm vertex shader.
// Fully GPU-driven: position computed from two per-particle seed vectors.
// Attractor orbits + curl noise + beat-synced explosions.
// ---------------------------------------------------------------------------
#include <common>

layout(location = 0) in vec4 aSeed;   // xyz = phase seeds, w = attractor index
layout(location = 1) in vec4 aSeed2;  // x = orbit radius seed, y = speed seed, z = color seed

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uExplode;  // 0..1 explosion envelope
uniform float uTrail;    // time offset for trail slices
uniform float uPointSize;
uniform float uFlash;    // 0..1 per-kick strobe (audio kick analyser)
// in-scene handoff: the storm is born from the outgoing machine's bright
// circuitry - particles sample the previous frame at their projected screen
// position and ignite from its light (0 = pure previous scene, 1 = storm)
uniform float uTransition;    // 0..1 handoff window
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)

out vec3 vCol;
out float vAlpha;
out float vDist;

// curl-ish flow noise via central differences of a 2-octave fbm potential
// (kept cheap: 6 noise samples per particle)
vec3 curlNoise(vec3 p, float t) {
  float e = 0.22;
  p.y += t;
  float fx = fbm3q(p + vec3(e, 0.0, 0.0)) - fbm3q(p - vec3(e, 0.0, 0.0));
  float fy = fbm3q(p + vec3(0.0, e, 0.0)) - fbm3q(p - vec3(0.0, e, 0.0));
  float fz = fbm3q(p + vec3(0.0, 0.0, e)) - fbm3q(p - vec3(0.0, 0.0, e));
  return normalize(vec3(fy - fz, fz - fx, fx - fy));
}

void main() {
  vec3 s = aSeed.xyz;
  float ai = aSeed.w;      // attractor index
  float rSeed = aSeed2.x;  // orbit radius seed
  float spd = aSeed2.y;
  float cSeed = aSeed2.z;

  // --- attractor centers (animated) -------------------------------------------
  vec3 attractors[4];
  for (int i = 0; i < 4; i++) {
    float fi = float(i);
    attractors[i] = vec3(
      5.0 * sin(Null.uTime * (0.3 + 0.05 * fi) + fi * 2.1),
      3.0 * sin(Null.uTime * (0.26 + 0.04 * fi) + fi * 1.3) * 0.7,
      5.0 * cos(Null.uTime * (0.32 + 0.05 * fi) + fi * 0.9));
  }

  // choose attractor: nearest by seed
  vec3 C = attractors[int(ai)];

  // --- orbit around attractor --------------------------------------------------
  float ph1 = s.x * TAU + Null.uTime * spd * 0.4;
  float ph2 = s.y * TAU - Null.uTime * spd * 0.31;
  float R = mix(0.4, 2.2, rSeed);
  vec3 pos = vec3(
    cos(ph1) * R * (0.8 + 0.4 * sin(ph2)),
    sin(ph2) * R * 0.7,
    sin(ph1) * R * (0.8 + 0.4 * cos(ph2)));

  // --- curl noise drift ----------------------------------------------------------
  pos += curlNoise(pos * 0.35 + C * 0.1, Null.uTime * 0.25) * 1.1 * Null.uIntensity;

  // --- beat explosions: radial burst from nearest attractor ---------------------
  float boom = uExplode * (0.6 + 0.4 * hash13(s + 13.0));
  pos += normalize(pos + 0.001) * boom * 3.2;

  // --- bass kick ripple ----------------------------------------------------------
  float kickPulse = Null.uPulse * (0.5 + 0.5 * Null.uBass);
  pos *= 1.0 + kickPulse * (0.25 + 0.3 * hash13(s));

  vec3 wpos = C + pos;

  // --- trails: sample a slightly earlier position (simulated motion) -------------
  // (cheap approximation: shift along the flow field by the trail time delta)
  vec3 dpos = curlNoise(wpos * 0.35, Null.uTime * 0.25 + uTrail) * 1.1 * Null.uIntensity;
  wpos -= dpos * uTrail * 9.0;

  vec4 viewPos = Null.uView * vec4(wpos, 1.0);
  gl_Position = Null.uProj * viewPos;
  vDist = -viewPos.z;

  // --- color ----------------------------------------------------------------------
  vCol = palVoid(cSeed * 0.4 + musicHue() * 0.6 + ai * 0.17);
  // brighten with energy + per-kick strobe
  vCol *= 0.6 + 0.8 * Null.uIntensity + 0.5 * Null.uBass;
  vCol *= 1.0 + uFlash * 1.5;

  // --- size ------------------------------------------------------------------------
  float size = mix(2.2, 1.2, Null.uIntensity) * uPointSize;
  size *= 60.0 / max(vDist, 0.5);
  gl_PointSize = clamp(size, 0.5, 8.0);

  // fade with distance + alpha by intensity (trail slices are dimmer)
  vAlpha = clamp(2.2 - vDist * 0.02, 0.0, 1.0) * (1.0 - uTrail * 0.6);
  vAlpha *= 0.35 + 0.65 * Null.uIntensity;

  // --- in-scene handoff: ignite from the machine's bright pixels -------------
  // while the outgoing frame lingers, particles born from luminous circuitry
  // (windows, conduits, emissive cores) come alive first and inherit its
  // light - the data visibly tears out of the machine it was born in
  float handoff = 1.0 - uTransition;
  if (handoff > 0.001 && gl_Position.w > 0.0) {
    // clip -> NDC -> UV (w != 1 under the perspective projection, so the
    // divide is required or near/far particles sample wrong screen spots)
    vec2 ndc = gl_Position.xy / gl_Position.w;
    vec2 suv = clamp(ndc * 0.5 + 0.5, 0.001, 0.999);
    vec3 prevCol = texture(uPrevScene, suv).rgb;
    float lum = max(max(prevCol.r, prevCol.g), prevCol.b);
    float ign = smoothstep(0.04, 0.5, lum);
    // bright machine pixels swell the spark + inherit its color
    gl_PointSize = clamp(gl_PointSize * (1.0 + handoff * ign * 2.2), 0.5, 12.0);
    vAlpha *= mix(0.3, 1.0, ign);
    vCol = mix(vCol, prevCol * 1.8 + vec3(0.4, 0.6, 1.0) * lum, handoff * ign * 0.7);
  }
}
