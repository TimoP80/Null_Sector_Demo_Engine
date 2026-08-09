#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 (handoff) - Neural Ocean backdrop.
// The outgoing dream frame fades out while the ocean's particles ignite from
// its bright sun and structures - the dream dissolves into the data ocean.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)
uniform float uTransition;    // 0..1 handoff window

out vec4 fragColor;

void main() {
  vec3 col = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
  // the fading dream still breathes with the track
  col *= 1.0 + Null.uPulse * 0.15 + Null.uBass * 0.1;
  fragColor = vec4(col, 1.0 - uTransition);
}
