#version 300 es
// Depth-of-field: blur by depth difference from the focus plane.
#include <common>

uniform sampler2D uTex;
uniform sampler2D uDepth;
uniform vec2 uRes;
uniform float uFocus;   // world-space focus distance
uniform float uAperture;// blur strength

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  // depth is packed in the ALPHA channel by the scene shaders (RGBA HDR,
  // alpha = depthFromViewZ), not the red color channel
  float d = texture(uDepth, uv).a;
  // project the world focus distance into the same 0..1 depth space as `d`
  float fd = depthFromViewZ(-uFocus);
  float coc = clamp((d - fd) / max(1.0 - fd, 0.05), -1.0, 1.0) * uAperture;

  vec3 col = vec3(0.0);
  float wsum = 0.0;
  // poisson-ish ring taps
  for (int i = 0; i < 16; i++) {
    float ang = float(i) * 0.3926991; // 2pi/16
    float rad = (float(i % 4) + 1.0) * 0.5 + 0.25;
    vec2 off = vec2(cos(ang), sin(ang)) * rad * coc * 0.012;
    vec2 suv = uv + off;
    float sd = texture(uDepth, suv).a;
    // gather with coc weight (ignore out-of-focus far values for near blur)
    float w = exp(-abs(sd - d) * 6.0);
    col += texture(uTex, suv).rgb * w;
    wsum += w;
  }
  col /= max(wsum, 1e-4);
  fragColor = vec4(col, 1.0);
}
