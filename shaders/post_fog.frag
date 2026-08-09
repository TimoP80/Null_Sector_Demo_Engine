#version 300 es
// PostStack pass: distance fog. Reads the depth the scene shaders pack into
// the alpha channel (depthFromViewZ space, 0 = near) and blends toward the
// fog color beyond the fog start distance.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uDensity;   // 0..1
uniform float uStart;     // depth where fog begins (0..1)
uniform vec3 uColor;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  float d = texture(uTex, uv).a;
  float f = sat01((d - uStart) / max(1.0 - uStart, 0.01));
  f = 1.0 - exp(-f * f * uDensity * 6.0);
  vec3 col = mix(texture(uTex, uv).rgb, uColor, f);
  fragColor = vec4(col, texture(uTex, uv).a);
}
