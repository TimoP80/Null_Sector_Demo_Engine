#version 300 es
// PostStack pass: animated film grain.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uTime;
uniform float uAmount;   // 0..1

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float g = hash12(vec2(fract(uv.x * uRes.x), fract(uv.y * uRes.y) * 17.0) + fract(uTime) * 91.7);
  vec3 col = texture(uTex, uv).rgb + (g - 0.5) * uAmount * 0.14;
  fragColor = vec4(col, texture(uTex, uv).a);
}
