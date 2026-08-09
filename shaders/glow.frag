#version 300 es
// Massive radial glow behind the logo assembly. Breathes with the music:
// kick = shockwave ripples, energy = brightness, blast = reform explosion.
#include <common>

uniform vec2 uRes;
uniform float uTime;
uniform float uPulse;
uniform float uDrop;
uniform float uBlast;   // 0..1 reform explosion
uniform float uKick;    // low-end onset
uniform float uEnergy;  // music energy
uniform vec2 uOffset;   // camera parallax
uniform vec3 uCol;
// in-scene handoff: the outgoing scene's frame is the backdrop the glow
// blooms over (the voxel city's light becoming the logo's ignition)
uniform float uTransition;    // 0..1 handoff window
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 c = uv - 0.5 - uOffset * 0.02;
  float r = length(c);

  float a = exp(-r * r * 9.0);
  a *= 1.0 + 1.2 * uDrop + 0.5 * uPulse + uEnergy * 0.8 + uBlast * 3.0;
  // kick shockwave rings
  a += exp(-abs(r - 0.62) * 40.0) * uKick * 0.9;
  a += exp(-abs(r - 0.4) * 60.0) * uKick * 0.5;
  // irregular edge flicker
  a *= 0.8 + 0.2 * sin(atan(c.y, c.x) * 7.0 + uTime * 2.0);
  a *= 0.75 + 0.25 * sin(uTime * 4.0 + r * 30.0);
  a = min(a, 6.0);
  vec3 col = uCol * a * 0.55;
  // handoff: the city's frame fades out under the growing logo glow
  if (uTransition < 0.999) {
    vec3 prev = texture(uPrevScene, uv).rgb;
    col = mix(prev * 1.15, col, uTransition);
  }
  fragColor = vec4(col, min(a, 1.0));
}
