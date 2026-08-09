#version 300 es
// ---------------------------------------------------------------------------
// SCENE 5 (handoff) - Particle Storm backdrop.
// The outgoing raymarched machine frame fades out over the transition window
// while the storm's particles ignite from its bright circuitry (see
// particles.vert). Rendered as the base layer; the storm draws additively on
// top, so the machine literally dissolves into the data it produces.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)
uniform float uTransition;    // 0..1 handoff window (0 = pure previous scene)

out vec4 fragColor;

void main() {
  vec3 col = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
  // faint beat pulse so the fading machine still breathes with the track
  col *= 1.0 + Null.uPulse * 0.18 + Null.uBass * 0.12;
  // source-over blends by src.a, so the un-dimmed color + alpha gives a
  // clean linear fade to black over the cleared target (no double-fade)
  fragColor = vec4(col, 1.0 - uTransition);
}
