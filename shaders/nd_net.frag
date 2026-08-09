#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN - THE REVEAL (fragment)
// ---------------------------------------------------------------------------
// Node sprites (uPrim 0): feathered additive glow dots - the same sprite
// model as the particle ocean, so the network reads as the ocean resolving
// into structure. Synapse quads (uPrim 1): thin glowing filaments with round
// caps and soft edges. uMode 1 (SYSTEM FAILURE) tears both with the shared
// beat-locked glitch model.
// ---------------------------------------------------------------------------
#include <common>

uniform float uPrim;
uniform float uMode;
uniform float uFlash;
uniform float uHigh;

in vec3 vCol;
in float vAlpha;
in float vDist;
in vec2 vT;
in float vSeed;

out vec4 fragColor;

void main() {
  vec3 col = vCol;
  float a = vAlpha;

  if (uPrim < 0.5) {
    // --- node: soft additive sprite with a feathered core --------------------
    vec2 c = gl_PointCoord - 0.5;
    float d = length(c) * 2.0;
    a *= smoothstep(1.0, 0.0, d);
    a *= a * (1.7 - d * 0.7);
    // fresnel-ish rim + treble sparkle
    col += vec3(1.0, 0.98, 1.0) * smoothstep(0.85, 0.3, d) * 0.3 * (0.4 + uFlash);
    col += vec3(1.0, 0.9, 1.0) * uHigh * 0.4 * (1.0 - d);
  } else {
    // --- synapse: soft edges + round caps ------------------------------------
    a *= smoothstep(0.55, 0.2, abs(vT.y));
    a *= smoothstep(0.0, 0.09, vT.x) * smoothstep(1.0, 0.91, vT.x);
    // fraying synapses spark (destabilize / failure)
    if (uMode > 0.5) {
      float sp = step(0.9, hash12(floor(gl_FragCoord.xy * 0.5) + Null.uTime + vSeed * 7.0));
      col += vec3(1.0, 0.8, 1.0) * sp * 0.6;
    }
  }

  // --- SYSTEM FAILURE: beat-locked glitch tears (shared model) ---------------
  if (uMode > 0.5) {
    float part = glitchParticipation(1.0, uFlash, Null.uBass, 1.0);
    float g = hash13(floor(gl_FragCoord.xyy * 0.02 + floor(Null.uTime * 8.0) * vec3(5.0, 11.0, 1.0)));
    float glitch = step(max(0.55, 0.96 - part * 0.41), g);
    col = mix(col, palVoid(g + musicHue()) * 1.6, glitch * 0.85);
    col *= 0.55 + 0.45 * sin(Null.uTime * 30.0 + length(gl_FragCoord.xy) * 0.01);
    col += vec3(1.0, 0.6, 1.0) * uFlash * 0.6;
  }

  // kick strobe over everything
  col *= 1.0 + uFlash * 0.35;
  col += vec3(0.9, 0.97, 1.0) * uFlash * 0.15;

  fragColor = vec4(col * a, a);
}
