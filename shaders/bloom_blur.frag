#version 300 es
// Separable gaussian blur for bloom.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform vec2 uDir; // (1,0) or (0,1)
uniform float uRadius;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 texel = uDir * uRadius / uRes;

  vec3 col = texture(uTex, uv).rgb * 0.227027;
  col += texture(uTex, uv + texel).rgb * 0.1945946;
  col += texture(uTex, uv - texel).rgb * 0.1945946;
  col += texture(uTex, uv + texel * 2.0).rgb * 0.1216216;
  col += texture(uTex, uv - texel * 2.0).rgb * 0.1216216;
  col += texture(uTex, uv + texel * 3.0).rgb * 0.054054;
  col += texture(uTex, uv - texel * 3.0).rgb * 0.054054;
  fragColor = vec4(col, 1.0);
}
