#version 300 es
// Bloom extraction: thresholded bright pass.
#include <common>

uniform sampler2D uTex;
uniform sampler2D uFlash;  // landing readout (additive HDR overlay; black when inactive)
uniform vec2 uRes;
uniform float uThreshold;
uniform float uIntensity;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  // the landing readout is folded in BEFORE thresholding, so its bright glyphs
  // genuinely bloom through the multi-pass chain instead of a procedural halo
  vec3 c = texture(uTex, uv).rgb + texture(uFlash, uv).rgb;
  float l = max(c.r, max(c.g, c.b));
  float k = smoothstep(uThreshold, uThreshold * 2.0, l);
  fragColor = vec4(c * k * uIntensity, 1.0);
}
