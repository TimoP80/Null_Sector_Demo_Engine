#version 300 es
// ---------------------------------------------------------------------------
// Logo assembly particles (the climax sequence).
// Particles sampled from the logo image start far out in orbiting spiral
// rings, tighten into stream trails, then converge onto their exact image
// positions - data coalescing into the logo.
// ---------------------------------------------------------------------------
precision highp float;
#include <common>

layout(location = 0) in vec4 aSeed;   // x = image u, y = image v, z = stagger, w = size seed
layout(location = 1) in vec4 aSeed2;  // x = jitter, y = color seed, z = speed seed

uniform vec2 uRes;
uniform float uTime;
uniform float uAsmb;        // 0..1 orbit -> settle progress
uniform float uStream;      // 0..1 stream phase (trail elongation)
uniform float uSpin;        // orbital spin speed
uniform float uImageAspect;
uniform float uTrail;       // trail slice offset (draw several slices)
uniform float uEnergy;      // music energy (brightness / size)
// in-scene handoff: the logo's particles are born from the outgoing scene's
// bright pixels (the voxel city's lights) and stream into the logo mask
uniform float uTransition;    // 0..1 handoff window
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)

out vec3 vCol;
out float vAlpha;

void main() {
  // image -> screen letterbox (same mapping as the chrome pass)
  float screenAspect = uRes.x / uRes.y;
  vec2 imgSize;
  if (screenAspect > uImageAspect) {
    imgSize = vec2(uImageAspect / screenAspect, 1.0);
  } else {
    imgSize = vec2(1.0, screenAspect / uImageAspect);
  }
  vec2 base = (aSeed.xy - 0.5) * imgSize * 2.0; // centered clip-space target

  // handoff: sample the outgoing scene at this particle's final screen
  // position - bright pixels (city windows, neon) ignite their particle first
  float handoff = 1.0 - uTransition;
  vec3 prevCol = vec3(0.0);
  float lum = 0.0;
  if (handoff > 0.001) {
    prevCol = texture(uPrevScene, clamp(base * 0.5 + 0.5, 0.001, 0.999)).rgb;
    lum = max(max(prevCol.r, prevCol.g), prevCol.b);
  }

  // --- orbit ring: tighten from far out as the assembly progresses --------------
  float ang = aSeed.z * TAU + uTime * (0.35 + aSeed2.z * 0.55) * uSpin;
  float ringR = mix(2.2, 0.0, uAsmb);
  ringR *= 0.5 + 0.9 * abs(sin(aSeed.x * 9.1 + aSeed.y * 3.7 + aSeed.w * 4.0));
  vec2 ring = vec2(cos(ang), sin(ang) * 0.82) * ringR;

  // stagger: outer particles converge first (wavefront wash)
  float asmb = sat01((uAsmb - aSeed.z * 0.3) * 1.4);
  // handoff: bright outgoing pixels ignite early, dark pixels hold back
  if (handoff > 0.001) {
    asmb = sat01(asmb * (0.25 + 0.75 * smoothstep(0.03, 0.45, lum)));
  }

  vec2 pos = mix(ring, base, asmb);

  // --- stream trails: stretch along the orbital tangent --------------------------
  vec2 dir = vec2(-sin(ang), cos(ang) * 0.82);
  pos -= dir * uTrail * (0.35 + uStream * 1.7) * (1.0 - asmb * 0.7);

  // shimmer: small hash jitter while in flight
  pos += (hash12(aSeed.xy * 31.7 + floor(uTime * 30.0)) - 0.5) * 0.05 * (1.0 - asmb);

  gl_Position = vec4(pos, 0.0, 1.0);
  gl_PointSize = clamp(
    (1.4 + aSeed.w * 3.2) * mix(1.8, 0.8, asmb) * (1.0 + uStream * 2.2) * (0.8 + uEnergy),
    1.0, 26.0);

  vCol = palVoid(aSeed2.y + uTime * 0.02) * (0.7 + 0.5 * aSeed2.x);
  vCol *= 1.0 + uStream * 1.3 + uEnergy * 0.6;
  // handoff: particles inherit the outgoing scene's light while it lingers
  if (handoff > 0.001) {
    vCol = mix(vCol, prevCol * 1.6 + vec3(0.3, 0.4, 0.9) * lum, handoff * 0.45 * (0.4 + lum));
  }

  // fade in at ignition, dim as they settle into the solid chrome
  float fade = sat01(asmb * 4.0) * (1.0 - sat01(uAsmb * 1.5 - 0.85));
  vAlpha = fade * fade * (1.0 - uTrail * 0.55);
  // handoff: only bright outgoing pixels emit until the window closes
  if (handoff > 0.001) {
    vAlpha *= mix(0.25, 1.0, smoothstep(0.03, 0.45, lum));
  }
}
