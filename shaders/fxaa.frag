#version 300 es
// FXAA 3.9-ish concise implementation for the final pass.
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec3 c = texture(uTex, uv).rgb;

  vec2 rcp = 1.0 / uRes;
  vec3 l = texture(uTex, uv - vec2(1.0, 0.0) * rcp).rgb;
  vec3 r = texture(uTex, uv + vec2(1.0, 0.0) * rcp).rgb;
  vec3 d = texture(uTex, uv - vec2(0.0, 1.0) * rcp).rgb;
  vec3 u = texture(uTex, uv + vec2(0.0, 1.0) * rcp).rgb;

  float lumaC = dot(c, vec3(0.299, 0.587, 0.114));
  float lumaL = dot(l, vec3(0.299, 0.587, 0.114));
  float lumaR = dot(r, vec3(0.299, 0.587, 0.114));
  float lumaD = dot(d, vec3(0.299, 0.587, 0.114));
  float lumaU = dot(u, vec3(0.299, 0.587, 0.114));

  float lumaMin = min(lumaC, min(min(lumaL, lumaR), min(lumaD, lumaU)));
  float lumaMax = max(lumaC, max(max(lumaL, lumaR), max(lumaD, lumaU)));

  // edge detection
  float lumaRange = lumaMax - lumaMin;
  if (lumaRange < max(0.0312, lumaMax * 0.125)) {
    fragColor = vec4(c, 1.0);
    return;
  }

  // horizontal + vertical contrast
  float lumaH = lumaL + lumaR - 2.0 * lumaC;
  float lumaV = lumaD + lumaU - 2.0 * lumaC;

  float off = 1.0;
  vec2 dir = vec2(0.0);
  if (abs(lumaH) > abs(lumaV)) {
    dir = vec2(1.0, 0.0);
  } else {
    dir = vec2(0.0, 1.0);
  }

  // subpixel refinement - sample along the edge
  vec2 px = dir * rcp;
  vec3 e1 = texture(uTex, uv + px).rgb;
  vec3 e2 = texture(uTex, uv - px).rgb;
  float le1 = dot(e1, vec3(0.299, 0.587, 0.114));
  float le2 = dot(e2, vec3(0.299, 0.587, 0.114));
  float k = sat01(0.5 - (lumaC - min(le1, le2)) / (lumaMax - lumaMin + 1e-5) + 0.5);
  off = k * 0.5;

  vec3 res = mix(
    texture(uTex, uv + dir * off * rcp).rgb,
    texture(uTex, uv - dir * off * rcp).rgb,
    0.5);
  fragColor = vec4(mix(res, c, 0.65), 1.0);
}
