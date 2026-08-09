#version 300 es
// PostStack pass: vignette.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uAmount;   // 0..1 strength
uniform float uCurve;    // falloff exponent (2 = smooth quadratic)

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = uv - 0.5;
  float r2 = dot(p, p);
  float v = pow(sat01(r2 * 4.0), uCurve > 0.0 ? uCurve : 2.0);
  vec3 col = texture(uTex, uv).rgb * (1.0 - uAmount * v);
  fragColor = vec4(col, texture(uTex, uv).a);
}
