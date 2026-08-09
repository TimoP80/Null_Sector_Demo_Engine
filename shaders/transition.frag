#version 300 es
// ---------------------------------------------------------------------------
// Scene transitions as visual effects. Driven by the soundtrack: uOnset/uKick
// inject transient energy, uMode selects the flavor per boundary:
//   0 = digital glitch blocks (default)
//   1 = data-stream dissolve (bars sweep like digital rain)
//   2 = melt (geometry softens and drips away)
//   3 = shockwave (impact ring from the drop)
// The blend always carries the new scene's depth through for DOF.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTexA;  // previous scene
uniform sampler2D uTexB;  // new scene
uniform vec2 uRes;
uniform float uTime;
uniform float uT;         // 0..1 progress
uniform float uSeed;
uniform float uMode;
uniform float uOnset;     // musical transient energy 0..1
uniform float uKick;      // low-end onset 0..1

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = uv;

  // kick shockwave ripples the whole frame at impact
  float wave = exp(-uT * 5.0) * (0.5 + 0.5 * uKick);

  if (uMode < 0.5) {
    // --- digital glitch blocks -------------------------------------------------
    float blocks = 8.0;
    vec2 blockID = floor(p * blocks);
    float bh = hash12(blockID + uSeed);
    float tear = step(0.85, bh);
    p.x += (bh - 0.5) * tear * uT * 0.12;

    float slice = hash12(vec2(floor(uv.y * 40.0), uSeed));
    p.x += (slice - 0.5) * step(0.92, slice) * uT * 0.25;

    float j = hash12(vec2(floor(uTime * 40.0), uSeed));
    p += (j - 0.5) * uT * 0.03;

    // onset tears: fresh glitches at every transient
    float ot = hash12(vec2(floor(uv.y * 50.0), floor(uTime * 24.0)));
    p.x += (ot - 0.5) * step(0.9, ot) * uOnset * 0.06;
  } else if (uMode < 1.5) {
    // --- data-stream dissolve ----------------------------------------------------
    // vertical bars sweep in with the beat, each streaming downward
    vec2 barID = vec2(floor(p.x * 46.0), floor(p.y * 20.0));
    float bar = hash12(barID + uSeed);
    float sweep = fract(p.y * 3.0 + uTime * (1.5 + uOnset * 3.0) + bar * 7.0);
    float mask = step(uT * (0.7 + 0.3 * bar), sweep);
    p.x += (bar - 0.5) * 0.05 * mask;
    p.y += (hash12(barID) - 0.5) * uT * 0.02;
    // dissolve gate
    float gate = step(fract(uT * 24.0 + bar), uT);
    p = mix(p, uv, gate);
  } else if (uMode < 2.5) {
    // --- melt ----------------------------------------------------------------------
    // vertical displacement melts the outgoing scene downward
    float melt = uT * (0.6 + 0.4 * uOnset);
    float wob = fbm2(p * 3.0 + uTime * 0.5 + uSeed);
    p.y += (wob - 0.5) * melt * 0.12;
    p.y -= melt * 0.1 * (1.0 - p.y);
    // drips
    vec2 dripID = vec2(floor(p.x * 24.0), floor(uTime * 6.0));
    float drip = step(0.985, hash12(dripID + uSeed));
    p.y -= drip * melt * 0.5 * hash12(dripID + 3.0);
    p.x += (wob - 0.5) * melt * 0.03;
  } else {
    // --- shockwave -------------------------------------------------------------------
    vec2 c = uv - 0.5;
    float r = length(c);
    float ring = exp(-abs(r - uT * 0.9) * 40.0) * wave;
    p += normalize(c + 1e-4) * ring * 0.05;
    p += (hash12(floor(uv * 300.0) + floor(uTime * 20.0)) - 0.5) * ring * 0.02;
  }

  // RGB split (scales with progress + transient energy)
  vec2 ca = vec2((uT * 0.006 + uOnset * 0.004) * (1.0 + wave), 0.0);
  vec3 cA = vec3(
    texture(uTexA, p + ca).r,
    texture(uTexA, p).g,
    texture(uTexA, p - ca).b);
  vec3 cB = vec3(
    texture(uTexB, p + ca).r,
    texture(uTexB, p).g,
    texture(uTexB, p - ca).b);

  vec3 col = mix(cA, cB, uT);

  // noise dissolve bars carry the handoff
  float n = vnoise2(p * 40.0 + uSeed);
  float mask = step(uT, n);
  col = mix(col, cB, mask);

  // occasional full glitch flashes + impact flash
  float flash = step(0.995, hash12(vec2(floor(uTime * 30.0), uSeed)));
  col = mix(col, palVoid(musicHue(0.2) + hash12(floor(p * 24.0)) * 0.4) * 0.8, flash * uT);
  col += vec3(1.0, 0.92, 1.0) * wave * 0.25;

  // carry the new scene's depth (alpha) through the blend for DOF
  fragColor = vec4(col, texture(uTexB, uv).a);
}
