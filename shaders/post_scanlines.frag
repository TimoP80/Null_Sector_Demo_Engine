#version 300 es
// PostStack pass: CRT scanlines.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uAmount;   // 0..1

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float scan = 0.82 + 0.18 * sin(uv.y * uRes.y * PI);
  vec3 col = texture(uTex, uv).rgb * (1.0 - uAmount * (1.0 - scan * 0.7) * 0.16);
  fragColor = vec4(col, texture(uTex, uv).a);
}
