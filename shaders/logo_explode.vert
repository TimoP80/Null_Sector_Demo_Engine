#version 300 es
// ---------------------------------------------------------------------------
// Logo explosion: particles sampled from the logo image start at their exact
// screen positions (matching the reveal), then blast outward with staggered
// ignition, swirl and noise. Screen-space pass - no camera needed.
// ---------------------------------------------------------------------------
precision highp float;
#include <common>

layout(location = 0) in vec4 aSeed;   // x = image u, y = image v, z = stagger, w = size seed
layout(location = 1) in vec4 aSeed2;  // x = jitter, y = color seed, z = speed seed

uniform vec2 uRes;
uniform float uTime;
uniform float uExplode;     // 0..1 explosion progress
uniform float uImageAspect; // logo aspect ratio (w/h)

out vec2 vUV;
out vec3 vCol;
out float vAlpha;

void main() {
  vUV = aSeed.xy;

  // image -> screen letterbox (same mapping as the reveal)
  float screenAspect = uRes.x / uRes.y;
  vec2 imgSize;
  if (screenAspect > uImageAspect) {
    imgSize = vec2(uImageAspect / screenAspect, 1.0);
  } else {
    imgSize = vec2(1.0, screenAspect / uImageAspect);
  }
  vec2 base = (aSeed.xy - 0.5) * imgSize * 2.0; // centered clip-space start

  // staggered ignition: outer particles go first
  float st = aSeed.z;
  float t = sat01((uExplode * 3.0 - st * 1.6) * 2.5);
  float e = t * t * (3.0 - 2.0 * t); // smoothstep ease

  vec2 dir = normalize(base + vec2(1e-4, 1e-4));
  float speed = 0.5 + aSeed2.z * 1.7;
  vec2 pos = base + dir * e * speed * 1.5;

  // swirl around the origin + noise tumble
  pos = rotate2(pos, e * (aSeed2.x - 0.5) * 2.4);
  pos += vec2(
    hash12(aSeed.xy * 13.7 + floor(uTime * 5.0)) - 0.5,
    hash12(aSeed.xy * 7.3 + 5.1 + floor(uTime * 4.0)) - 0.5
  ) * 0.3 * e;

  // slow outward drift after the burst so the debris keeps moving
  float drift = sat01(uExplode * 1.1 - 0.8);
  pos += dir * drift * 0.3 * speed;

  gl_Position = vec4(pos, 0.0, 1.0);
  gl_PointSize = clamp((1.6 + aSeed.w * 2.8) * (1.0 + e * 2.0) * (1.0 - uExplode * 0.25), 1.0, 16.0);

  vCol = palVoid(aSeed2.y + uTime * 0.03) * (0.55 + 0.6 * aSeed2.x);
  // fade in briefly, then out gradually across the rest of the section
  float alpha = sat01(e * 6.0) * (1.0 - sat01(uExplode * 1.15 - 0.7));
  vAlpha = alpha * alpha;
}
