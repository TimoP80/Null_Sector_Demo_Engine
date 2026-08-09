#version 300 es
// Sprite: sampled texture, tinted, with opacity.
#include <common>

uniform sampler2D uTex;
uniform vec4 uColor;
uniform float uOpacity;

in vec2 vUV;
out vec4 fragColor;

void main() {
  vec4 c = texture(uTex, vUV);
  fragColor = vec4(c.rgb * uColor.rgb, c.a * uColor.a * uOpacity);
}
