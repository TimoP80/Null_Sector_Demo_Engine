#version 300 es
// SIGNAL LOST placeholder - deliberately self-contained (no #include) so it
// keeps rendering even if common.glsl is the shader that broke. Music-driven
// glitch static: tears, chromatic noise, interference bands, kick shockwave.
precision highp float;

uniform vec2 uRes;
uniform float uTime;
uniform float uStatic;   // 0..1 static intensity (music energy)
uniform float uTear;     // 0..1 tear slicing (music onsets)
uniform float uKick;     // 0..1 low-end shockwave

in vec2 vUV;
out vec4 fragColor;

float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(
    mix(hash21(i), hash21(i + vec2(1.0, 0.0)), f.x),
    mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), f.x),
    f.y);
}

void main() {
  float t = uTime;
  vec2 uv = vUV;

  // animated horizontal tear bands: whole-row offsets slicing the frame
  float tearN = vnoise(vec2(uv.y * 10.0, floor(t * 5.0)));
  float sliceMask = step(0.86 - uTear * 0.25, tearN);
  float shift = (hash21(vec2(floor(uv.y * 22.0), floor(t * 8.0))) - 0.5)
    * (0.3 * sliceMask + 0.02) * (0.5 + uTear);
  vec2 p = uv + vec2(shift, 0.0);

  // high-frequency static, 3 channels offset = chromatic noise
  vec2 g = p * uRes * 0.55 + vec2(0.0, t * 55.0);
  float r = vnoise(g);
  float gr = vnoise(g + vec2(7.0, 0.0));
  float b = vnoise(g + vec2(14.0, 0.0));
  float amp = 0.45 + 0.55 * uStatic;
  vec3 col = vec3(r, gr, b) * amp;

  // slow rolling interference bands over the static
  float band = 0.5 + 0.5 * sin(p.y * 70.0 + t * 2.1) * sin(p.x * 55.0 - t * 1.7);
  col += vec3(band) * 0.18;

  // kick: expanding shockwave ring in the danger palette
  float rad = length(uv - 0.5);
  col += vec3(1.0, 0.3, 0.9) * smoothstep(0.0, 0.05, 1.0 - abs(rad - (0.12 + uKick * 0.55))) * uKick * 1.5;

  // grade: near-black base, magenta static, cyan undertone
  col = col * vec3(0.9, 0.25, 1.0) + vec3(0.01, 0.02, 0.05);

  // scanlines + vignette
  col *= 0.86 + 0.14 * sin(p.y * uRes.y * 1.2);
  col *= 1.0 - 0.55 * smoothstep(0.42, 0.95, length(uv - 0.5) * 1.35);

  // rare full-frame sync breaks (channel swap)
  float sync = step(0.985, hash21(vec2(floor(t * 2.5), 1.0)));
  col = mix(col, col.gbr, sync * 0.85);

  fragColor = vec4(col, 1.0);
}
