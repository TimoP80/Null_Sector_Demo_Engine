#version 300 es
// SIGNAL LOST label text - self-contained (no #include) so the label survives
// even if common.glsl is the shader that broke. Fixed danger palette instead
// of the shared palVoid().
precision highp float;

uniform sampler2D uTex;
uniform float uAlpha;
uniform float uGlow;    // 0..1 glow halo (pulses with the beat)
uniform float uPulse;   // beat pulse 0..1

in vec2 vUV;
in float vSeed;

out vec4 fragColor;

void main() {
  float a = texture(uTex, vUV).a;
  if (a < 0.02) discard;

  float crisp = smoothstep(0.35, 0.75, a);
  // hot core cooling to magenta edges, cyan glow halo
  vec3 col = mix(vec3(1.0, 0.25, 0.45), vec3(1.0, 0.9, 0.95), crisp * 0.7);
  col += vec3(0.4, 0.9, 1.0) * a * uGlow * (0.6 + 0.4 * uPulse);

  float alpha = crisp * uAlpha;
  if (alpha <= 0.01) discard;
  fragColor = vec4(col, alpha);
}
