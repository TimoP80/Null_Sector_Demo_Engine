#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN - THE REVEAL (void)
// ---------------------------------------------------------------------------
// Screen-space additive "void" pass drawn under the network geometry: the
// dark base tint, the traveling pulse glow and the destabilize glitch bands
// that the old fullscreen raymarch used to carry (its miss path). Ported
// 1:1 from nd_net.frag v1, but as a single cheap fullscreen pass (~1 ms)
// instead of 200 ms of per-pixel SDF marching - so the below-network view
// stays alive (glow + beat-locked tears) rather than dead black.
// ---------------------------------------------------------------------------
#include <common>

uniform float uMode;
uniform float uFlash;

out vec4 fragColor;

void main() {
  float t = Null.uTime;
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  float destab = smoothstep(0.15, 0.95, secT);
  if (uMode > 0.5) destab = max(destab, 0.92);

  vec2 res = Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec3 ro = Null.uCamPos;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));

  // the raymarch's miss-path base + ambient node haze: the nodes ride a
  // smooth low-frequency wave field, so their aggregate glow is a smooth
  // field too - one fbm2 sample replaces the 144-node per-pixel sum
  vec3 col = vec3(0.003, 0.006, 0.016);
  float distField = exp(-length(ro - vec3(0.0, -2.0, 0.0)) * 0.10);
  float haze = fbm2(rd.xz * 1.7 + vec2(4.0, 1.0)) * 0.6 + 0.4;
  col += palVoid(musicHue(0.15)) * distField * haze * 0.35 * (0.5 + 0.5 * Null.uPulse);

  // the traveling pulse glow (uniform across the frame, like the raymarch)
  float pulse = 0.25 + 1.1 * Null.uPulse + 0.55 * Null.uBass;
  vec2 pc = vec2(7.0 * sin(t * 0.22), 7.0 * cos(t * 0.18));
  float pd = length(ro.xz - pc);
  col += palVoid(musicHue(0.3)) * exp(-pd * 0.12) * (0.2 + 0.8 * pulse) * 0.5;

  // destabilize: chromatic sparks + beat-locked slice tears (shared model)
  if (destab > 0.01) {
    col *= 1.0 + uFlash * 0.3 * destab;
    col += palVoid(musicHue(0.4) + destab) * destab * (0.2 + 0.3 * uFlash);
    vec2 gs = glitchSlice(gl_FragCoord.y / res.y, 32.0, destab, uFlash, Null.uBass, 17.3, destab);
    uv.x += gs.x * 0.08;
    col += palVoid(musicHue(0.4) + gs.y) * abs(gs.x) * 3.0 * destab;
  }

  // kick strobe over everything
  col *= 1.0 + uFlash * 0.35;
  col += vec3(0.9, 0.97, 1.0) * uFlash * 0.15;

  fragColor = vec4(col, 1.0);
}
