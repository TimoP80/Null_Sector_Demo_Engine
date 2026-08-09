#version 300 es
// ---------------------------------------------------------------------------
// SCENE 9 - Greetings: the NULL SECTOR wordmark hero.
// The shared logo texture IS the reference artwork (chrome letters on a dark
// backdrop; alpha = letter mask from logotex.ts). This pass shows the image's
// own pixels faithfully - a whisper of a beat-reactive fresnel rim only - so
// the poster reads like the original instead of a procedural chrome repaint.
// ---------------------------------------------------------------------------
#include <common>

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uTime;
uniform float uImageAspect;
uniform float uFit;     // 0..1 - how large the wordmark is on screen
uniform float uOffsetY; // NDC vertical offset (positive = up)
uniform float uAlpha;   // overall fade (poster entrance)
uniform float uPulse;
uniform float uIntensity;

out vec4 fragColor;

void main() {
  // fit the image into a centered box (fit scales the letterboxed size)
  float screenAspect = uRes.x / uRes.y;
  vec2 imgSize;
  if (screenAspect > uImageAspect) {
    imgSize = vec2(uImageAspect / screenAspect, 1.0);
  } else {
    imgSize = vec2(1.0, screenAspect / uImageAspect);
  }
  imgSize *= uFit;

  // uOffsetY in NDC -> [0,1] screen fraction (positive = up), so the hero
  // band can sit top-center while the live columns/footer fill the lower half
  vec2 center = (gl_FragCoord.xy / uRes - 0.5 - vec2(0.0, uOffsetY * 0.5)) / imgSize + 0.5;
  if (center.x < 0.0 || center.x > 1.0 || center.y < 0.0 || center.y > 1.0) {
    fragColor = vec4(0.0, 0.0, 0.0, 0.0);
    return;
  }
  vec2 z = center;
  z.y = 1.0 - z.y; // canvas top row sits at v=0, mirror upright

  vec4 t = texture(uTex, z);
  float mask = t.a;
  if (mask < 0.02) {
    fragColor = vec4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  // --- faithful image pass ------------------------------------------------------
  // Sample the artwork's own pixels - the poster's chrome letters are already
  // the look we want. A gentle fresnel catch keeps a 3D edge and a faint
  // beat-reactive rim lets the logo breathe without repainting it into a
  // glowing blur.
  vec3 n = normalize(vec3(z - 0.5, 0.5));
  float fres = pow(1.0 - max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0), 3.0);
  vec3 col = t.rgb * (1.0 + 0.25 * fres);
  col += palVoid(0.72) * fres * (0.03 + 0.1 * uPulse) * (0.4 + uIntensity);
  col = clamp(col, 0.0, 1.0);

  // un-premultiplied rgb: source-over blending applies src.a itself, so
  // multiplying rgb by mask here would double-attenuate soft letter edges
  fragColor = vec4(col, mask * uAlpha);
}
