#version 300 es
// Fullscreen image card (pre-show splash + end-of-show outro): aspect-fitted
// assets PNG with a fade-in, a held display, a short glitch burst (slice
// displacement + RGB split + scanline shimmer), then a fade-out. Rendered
// straight to the default framebuffer (no HDR, no post pipeline), so the
// image appears exactly as authored.
precision highp float;

uniform sampler2D uTex;
uniform vec2  uRes;      // window size (px)
uniform vec2  uTexSize;  // logo texture size (px)
uniform float uTime;     // splash-local seconds
uniform float uAlpha;    // master fade 0..1
uniform float uGlitch;   // glitch intensity 0..1

in vec2 vUV;
out vec4 fragColor;

float hash(float n) { return fract(sin(n) * 43758.5453123); }

void main() {
  // screen uv, un-flipped (stb_image rows are top-down; texture row 0 = image top)
  vec2 uv = vec2(vUV.x, 1.0 - vUV.y);

  // aspect-fit the logo inside the frame (letterboxed, no stretch)
  float winAspect = uRes.x / max(uRes.y, 1.0);
  float texAspect = uTexSize.x / max(uTexSize.y, 1.0);
  vec2 fit = vec2(1.0);
  if (winAspect > texAspect) fit.x = texAspect / winAspect;
  else fit.y = winAspect / texAspect;
  uv = (uv - 0.5) * fit + 0.5;

  // fitted-rect mask: only the logo area carries colour
  float inside = step(0.0, uv.x) * step(uv.x, 1.0)
               * step(0.0, uv.y) * step(uv.y, 1.0);

  float g = uGlitch;
  float t = uTime;

  // per-band horizontal slice displacement
  vec2 off = vec2(0.0);
  float band = floor(uv.y * 16.0);
  float r1 = hash(band + floor(t * 14.0));
  if (g > 0.001 && r1 > 1.0 - g * 0.6) {
    off.x = (r1 - 0.5) * 0.18 * g * (1.0 - abs(uv.y - 0.5) * 1.2);
  }
  // occasional whole-frame jump (horizontal tear + tiny vertical nudge)
  float j = floor(t * 9.0);
  float jump = step(0.97 - g * 0.45, hash(j));
  off.x += (hash(j + 13.0) - 0.5) * 0.10 * g * jump;
  off.y += (hash(j + 29.0) - 0.5) * 0.025 * g * jump;

  // RGB split (the three channels sample slightly different places)
  vec2 oR = uv + off * 1.5;
  vec2 oG = uv + off;
  vec2 oB = uv - off * 0.9;

  vec4 cR = texture(uTex, oR);
  vec4 cG = texture(uTex, oG);
  vec4 cB = texture(uTex, oB);

  vec3 col = vec3(cR.r, cG.g, cB.b);

  // scanline shimmer while glitching
  float scan = 1.0 - 0.06 * g * (0.5 + 0.5 * sin(uv.y * uTexSize.y * 0.35 + t * 55.0));
  col *= scan;

  float a = cG.a * inside * uAlpha;
  fragColor = vec4(col, a);
}
