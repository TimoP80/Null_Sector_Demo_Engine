#version 300 es
// PostStack pass: add the blurred bloom levels over the base HDR scene.
#include <common>

uniform sampler2D uBase;
uniform sampler2D uBloom0;
uniform sampler2D uBloom1;
uniform sampler2D uBloom2;
uniform vec2 uRes;
uniform float uIntensity;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec3 base = texture(uBase, uv).rgb;
  vec3 bloom = texture(uBloom0, uv).rgb * 1.0 +
               texture(uBloom1, uv).rgb * 0.55 +
               texture(uBloom2, uv).rgb * 0.30;
  fragColor = vec4(base + bloom * uIntensity, texture(uBase, uv).a);
}
