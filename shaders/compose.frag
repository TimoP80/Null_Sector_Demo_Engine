#version 300 es
// Final composite: HDR + bloom -> tonemap, lens distortion, chromatic
// aberration, film grain, scanlines, CRT mask, vignette, color grade,
// temporal motion blur blend.
#include <common>

uniform sampler2D uTex;      // HDR scene (after DOF)
uniform sampler2D uBloom0;   // bloom mip chain, high-res -> low-res
uniform sampler2D uBloom1;
uniform sampler2D uBloom2;
uniform sampler2D uBloom3;
uniform sampler2D uBloom4;
uniform sampler2D uBloom5;
uniform sampler2D uPrev;     // previous tonemapped frame (motion blur)
uniform vec2 uRes;
uniform float uTime;
uniform float uMotion;       // 0..1 motion blur amount
uniform float uGrain;        // grain amount
uniform float uScan;         // scanline amount
uniform float uVignette;
uniform float uCA;           // chromatic aberration
uniform float uDistort;      // lens distortion
uniform float uSaturation;
uniform float uContrast;
uniform float uBoost;        // exposure / brightness
uniform vec3 uGradeA;        // shadow tint
uniform vec3 uGradeB;        // highlight tint
uniform float uGlitch;       // global glitch amount (reprise / drops)
uniform float uBloomMul;     // music-driven bloom intensity
uniform float uHeat;         // heat haze (0..1)
uniform float uDirt;         // lens dirt / dust (0..1)
uniform float uKick;         // low-end onset (shockwave ripples)
uniform sampler2D uFlash;  // landing readout (additive HDR overlay; black when inactive)
uniform float uLanding;    // >0 when the landing readout is present

out vec4 fragColor;

void main() {
  vec2 uv = gl_FragCoord.xy / uRes;
  vec2 p = uv - 0.5;

  // --- lens distortion (barrel) ----------------------------------------------
  float r2 = dot(p, p);
  vec2 dUV = p * (1.0 + uDistort * r2);
  vec2 cUV = dUV + 0.5;

  // glitch slice offsets (scene-level, driven by uGlitch + kick impact)
  float glitchAmp = uGlitch + uKick * 0.5;
  float slice = hash12(vec2(floor(uv.y * 36.0), floor(uTime * 12.0)));
  cUV.x += (slice - 0.5) * step(0.965, slice) * glitchAmp * 0.02;
  cUV.x += (hash12(vec2(floor(uv.y * 90.0), floor(uTime * 8.0))) - 0.5) * step(0.99, hash12(vec2(floor(uTime * 30.0), 0.0))) * glitchAmp * 0.04;

  // --- heat haze: wobbly refractive band above the horizon -----------------------
  if (uHeat > 0.001) {
    float band = exp(-pow((uv.y - 0.5) * 2.4, 2.0));
    float wob = vnoise2(vec2(uv.x * 14.0, uTime * 1.4)) - 0.5;
    cUV.x += wob * band * uHeat * 0.008;
  }

  vec2 cUVr = cUV;
  vec2 cUVb = cUV;

  // --- chromatic aberration ----------------------------------------------------
  if (uCA > 0.0) {
    vec2 ca = p * uCA * 0.004;
    cUVr += ca;
    cUVb -= ca;
  }

  vec3 col;
  col.r = texture(uTex, cUVr).r;
  col.g = texture(uTex, cUV).g;
  col.b = texture(uTex, cUVb).b;

  // --- bloom: accumulate the whole mip chain (wide soft glow from low levels) ---
  // (constant sampler indexing - dynamic sampler array indexing is not portable)
  vec3 bloom =
    texture(uBloom0, cUV).rgb * 1.0 +
    texture(uBloom1, cUV).rgb * 0.55 +
    texture(uBloom2, cUV).rgb * 0.30 +
    texture(uBloom3, cUV).rgb * 0.17 +
    texture(uBloom4, cUV).rgb * 0.09 +
    texture(uBloom5, cUV).rgb * 0.05;
  col += bloom * (1.0 + uBloomMul * 1.2);

  // --- rehearsal landing verdict flash (HIT / FLOATED) ---------------------------
  // The readout was rendered into a full-res HDR target BEFORE bloom extraction
  // (see engine/postprocess.ts) so its bright glyphs genuinely bloom through the
  // multi-pass chain. Here we add the same readout to the scene color pre-tonemap,
  // so it is visible in-frame AND graded with the frame (grain, scanlines,
  // vignette and motion blur all apply to it too). The uFlash texture is black
  // when no flash is active, so this is a no-op then.
  if (uLanding > 0.001) {
    col += texture(uFlash, cUV).rgb;
  }

  // --- color grading (lift/gamma/gain + saturation + contrast) ------------------
  float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
  col = mix(vec3(lum), col, uSaturation);
  col = (col - 0.5) * uContrast + 0.5;
  vec3 grade = mix(uGradeA, uGradeB, lum);
  col *= grade * uBoost;

  // --- tonemap --------------------------------------------------------------------
  col = tonemapACES(col);
  // exposure boost slider folded into grade
  col = satV(col);

  // --- film grain -------------------------------------------------------------------
  if (uGrain > 0.0) {
    float g = hash12(vec2(fract(uv.x * uRes.x), fract(uv.y * uRes.y) * 17.0) + fract(uTime) * 91.7);
    col += (g - 0.5) * uGrain * 0.09;
  }

  // --- lens dirt: subtle procedural smudges + dust specks -------------------------
  if (uDirt > 0.001) {
    vec2 dp = uv * vec2(1.3, 1.0);
    float smudge = vnoise2(dp * 1.7 + 7.3);
    smudge = pow(smudge, 2.2);
    col *= 1.0 - uDirt * 0.14 * smudge;
  }

  // --- temporal motion blur (blend with previous frame) ----------------------------
  if (uMotion > 0.0) {
    vec3 prev = texture(uPrev, cUV).rgb;
    col = mix(prev, col, 1.0 - uMotion * 0.55);
  }

  // --- vignette ---------------------------------------------------------------------
  col *= 1.0 - uVignette * sat01(r2 * 2.6);

  // --- scanlines ---------------------------------------------------------------------
  if (uScan > 0.0) {
    float scan = 0.82 + 0.18 * sin(p.y * uRes.y * PI);
    col *= 1.0 - uScan * (1.0 - scan * 0.7) * 0.12;
  }

  // --- CRT RGB mask ------------------------------------------------------------------
  float mask = 0.92 + 0.08 * sin(uv.x * uRes.x * PI * 0.66);
  col *= mask;

  fragColor = vec4(col, 1.0);
}
