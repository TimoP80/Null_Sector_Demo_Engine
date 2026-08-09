#version 300 es
// PostStack: final present - blit the last pass output to the default
// framebuffer. Tonemaps when the chain did not.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uTonemap;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec3 col = texture(uTex, uv).rgb;
  if (uTonemap > 0.5) col = tonemapACES(col);
  fragColor = vec4(satV(col), 1.0);
}
