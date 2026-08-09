#version 300 es
// PostStack pass: retro pixelation (block size in pixels).
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uPixels;   // block size (>=1)

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float b = max(1.0, uPixels);
  vec2 puv = (floor(uv * uRes / b) + 0.5) * b / uRes;
  fragColor = texture(uTex, puv);
}
