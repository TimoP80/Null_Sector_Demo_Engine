#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN (vertex)
// ---------------------------------------------------------------------------
// An enormous ocean of points of light. ~55% ride the wavy surface sheet,
// the rest hang in the volume beneath - neural activity under the surface.
// A large-scale pulse travels through the field synced to the beat
// (uPulse/uBass), brightening the points it passes. The camera dives below
// the sheet (the surface becomes a ceiling of light). Near the end the
// network destabilizes (uExplode from the driver's beatPulse scatter).
// ---------------------------------------------------------------------------
#include <common>

layout(location = 0) in vec4 aSeed;   // xyz = seeds, w = attractor index
layout(location = 1) in vec4 aSeed2;  // x = radius seed, y = speed seed, z = color seed

uniform float uExplode;   // 0..1 destabilize envelope (driver: beatPulse)
uniform float uTrail;
uniform float uPointSize;
uniform float uFlash;     // per-kick strobe
uniform float uMode;
uniform float uHigh;      // react.high (surface shimmer)
uniform float uTransition;    // 0..1 handoff window
uniform sampler2D uPrevScene; // unit 9

out vec3 vCol;
out float vAlpha;
out float vDist;

float oceanY(vec2 xz, float t) {
  float w1 = fbm2(xz * 0.10 + vec2(3.0, 1.0));
  float w2 = fbm2(xz * 0.30 + vec2(9.0));
  float swell = 1.0 + Null.uBass * 1.6;
  return (w1 - 0.5) * 1.1 * swell + (w2 - 0.5) * 0.45 + 0.25 * sin(xz.x * 0.25 + t * 0.5);
}

void main() {
  vec3 s = aSeed.xyz;
  float rSeed = aSeed2.x;
  float spd = aSeed2.y;
  float cSeed = aSeed2.z;
  float t = Null.uTime;

  // sheet vs volume split
  float isSurface = step(0.45, s.y);
  vec2 xz = (s.xz - 0.5) * 38.0;
  float wave = oceanY(xz, t);
  float depth = 0.8 + fract(s.x * 13.7 + s.z * 7.3) * 13.0;
  vec3 home = isSurface > 0.5
      ? vec3(xz.x, wave, xz.y)
      : vec3(xz.x, wave - depth, xz.y);

  // --- destabilize: beat scatter (uExplode from the driver) -------------------
  float boom = uExplode * (0.5 + 0.5 * hash13(s + 7.0));
  home += normalize(vec3(hash13(s + 1.0) - 0.5, hash13(s + 2.0), hash13(s + 3.0) - 0.5))
        * boom * 2.4;
  // --- SYSTEM FAILURE mode (uMode 1): the neural ocean convulses -------------
  if (uMode > 0.5) {
    float chaos = 1.0 + 2.0 * uFlash + 1.4 * Null.uOnset;
    home += vec3(hash13(s + 13.0) - 0.5, hash13(s + 14.0) - 0.5, hash13(s + 15.0) - 0.5)
          * chaos * 2.2;
  }

  // --- the traveling pulse: bright wave sweeping the field on the beat --------
  vec2 pc = vec2(7.0 * sin(t * 0.22), 7.0 * cos(t * 0.18));
  float pd = length(xz - pc);
  float pulse = exp(-pd * 0.22) * (0.25 + 1.1 * Null.uPulse + 0.55 * Null.uBass);
  // secondary ripple rings expanding outward on each beat
  float ringPhase = fract(Null.uBeat * 0.5 - length(xz) * 0.045);
  float ring = exp(-abs(ringPhase - 0.5) * 6.0) * Null.uPulse * 0.7;

  vec4 viewPos = Null.uView * vec4(home, 1.0);
  gl_Position = Null.uProj * viewPos;
  vDist = -viewPos.z;

  // --- color: music-hued points, bright on the pulse ---------------------------
  vCol = palVoid(cSeed * 0.35 + musicHue(0.1) * 0.6);
  // deeper volume points are dimmer and cooler
  vCol *= isSurface > 0.5 ? 1.0 : 0.55;
  vCol *= 0.22 + 0.9 * sat01(pulse + ring) + 0.4 * uFlash + 0.25 * Null.uBass;
  vCol += vec3(1.0, 0.98, 1.0) * uFlash * 0.25;

  // --- size: perspective-scaled, swell on the pulse -----------------------------
  float size = (isSurface > 0.5 ? 2.6 : 2.0) * uPointSize;
  size *= (1.0 + 1.4 * sat01(pulse + ring));
  size *= 70.0 / max(vDist, 0.5);
  gl_PointSize = clamp(size, 0.5, 7.0);

  // --- alpha: fade with distance; underwater the surface above reads as a
  //     glowing ceiling (particles above the camera stay visible, distant
  //     ones dissolve) ----------------------------------------------------------
  float farFade = clamp(1.6 - vDist * 0.03, 0.0, 1.0);
  float alpha = farFade * (0.18 + 0.8 * sat01(pulse + ring)) * (0.35 + 0.65 * isSurface);
  alpha *= 0.6 + 0.6 * Null.uIntensity;
  vAlpha = alpha;

  // --- in-scene handoff: ignite from the outgoing dream's bright pixels --------
  float handoff = 1.0 - uTransition;
  if (handoff > 0.001 && gl_Position.w > 0.0) {
    vec2 ndc = gl_Position.xy / gl_Position.w;
    vec2 suv = clamp(ndc * 0.5 + 0.5, 0.001, 0.999);
    vec3 prevCol = texture(uPrevScene, suv).rgb;
    float lum = max(max(prevCol.r, prevCol.g), prevCol.b);
    float ign = smoothstep(0.05, 0.5, lum);
    gl_PointSize = clamp(gl_PointSize * (1.0 + handoff * ign * 1.8), 0.5, 10.0);
    vAlpha *= mix(0.3, 1.0, ign);
    vCol = mix(vCol, prevCol * 1.6 + palVoid(musicHue(0.4)) * lum, handoff * ign * 0.6);
  }
}
