#version 300 es
// PostStack internal: lossless HDR copy (no clamp, no tonemap).
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  fragColor = texture(uTex, uv);
}
