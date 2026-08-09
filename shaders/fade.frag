#version 300 es
// Solid black overlay with alpha for fades.
#include <common>

uniform float uAlpha;

out vec4 fragColor;

void main() {
  fragColor = vec4(0.0, 0.0, 0.0, uAlpha);
}
