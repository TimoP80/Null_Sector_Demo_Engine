#version 300 es
// PostStack pass: radial chromatic aberration.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uAmount;   // 0..1

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = uv - 0.5;
  float r2 = dot(p, p);
  // aberration grows toward the edges
  float ca = uAmount * (0.4 + r2 * 2.4);
  vec2 rUV = uv + p * ca * 0.01;
  vec2 bUV = uv - p * ca * 0.01;
  vec3 col;
  col.r = texture(uTex, rUV).r;
  col.g = texture(uTex, uv).g;
  col.b = texture(uTex, bUV).b;
  fragColor = vec4(col, texture(uTex, uv).a);
}
