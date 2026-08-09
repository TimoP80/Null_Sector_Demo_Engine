#version 300 es
// ---------------------------------------------------------------------------
// SCENES 1 + 8 - Infinite Neon Tunnel
// March a camera through a tube whose radius breathes with the beat.
// uMode 0 = tunnel, 1 = reprise (faster, morphing cross-section, heavy glitch)
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uMode;  // 0 = tunnel, 1 = reprise
uniform float uFlash; // 0..1 strobe on each kick drum hit (audio analyser)

out vec4 fragColor;

// ring stripe pattern that sweeps toward the camera
float ringStripes(float z, float speed, float freq) {
  return fract(z * freq + Null.uTime * speed);
}

// digital rain: columns of glyphs falling along the wall
float digitalRain(vec2 pq, float speed, float cell) {
  vec2 id = floor(pq / cell);
  float t = Null.uTime * speed + id.x * 3.7;
  float y = fract(t * 0.4 + id.y * 0.3);
  float trail = smoothstep(0.0, 0.06, y) * smoothstep(0.22, 0.06, y);
  float glyph = step(0.82, hash12(id + floor(t * 3.0)));
  return trail * glyph;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  float hit = 0.0;

  const int STEPS = 80;
  float stepLen = mix(0.42, 0.3, Null.uIntensity);

  for (int i = 0; i < STEPS; i++) {
    p = ro + rd * t;
    float z = p.z;

    // radius function: base + breathing + traveling sine + morph for reprise
    float R = 3.2 + 0.35 * sin(z * 0.55 + Null.uTime * 0.8) + 0.55 * Null.uPulse;
    if (uMode > 0.5) {
      // reprise: cross-section morphs (radius modulation by angle) + speed up
      float phi = atan(p.y, p.x);
      // uSectionLocal is raw SECONDS (0..section length): a raw multiplier
      // would drive the radius negative for part of the cycle, which makes
      // r > R true at t=0 for every ray -> flat gray. Ramp the amplitude in
      // 0..1 instead (full morph after ~4s) and floor R so the wall is real.
      float warpA = 0.75 * sin(phi * 3.0 + Null.uTime * 6.0) * clamp(Null.uSectionLocal / 4.0, 0.0, 1.0);
      float glitchAmp = step(0.92, hash13(floor(p * 2.0 + floor(Null.uTime * 4.0)))) * 0.9;
      R += warpA + glitchAmp * R;
      R = max(R, 0.5);
    }

    float r = length(p.xy);
    // hit the WALL when the ray reaches the tube surface (r grows from the
    // axis; the camera flies INSIDE the tunnel, so an interior point is not
    // a hit - it's where the march starts)
    if (r > R) { hit = 1.0; break; }

    t += stepLen;
    if (t > 90.0) break;
  }

  if (hit < 0.5) {
    // void at far end: reprise reveals the voxel city skyline as it overloads
    vec3 voidCol = palVoid(musicHue(0.1) + Null.uIntensity * 0.3) * 0.06;
    float centerGlow = exp(-length(uv) * 1.8);
    vec3 col = voidCol + palVoid(musicHue(0.2) + Null.uBeat * 0.002) * centerGlow * (0.3 + Null.uPulse);
    if (uMode > 0.5 && Null.uExitRamp > 0.01) {
      // distant city: silhouettes + neon horizon rising as the tunnel dissolves
      float sky = 0.0;
      float h = 0.06 + 0.10 * Null.uExitRamp;
      float build = hash12(vec2(floor((uv.x + 1.0) * 14.0), 0.0));
      sky = step(build * h, uv.y + 0.02) * step(uv.y, h * 1.4);
      sky *= smoothstep(-1.0, -0.2, uv.y);
      col = mix(col, palVoid(musicHue(0.4) + build * 0.3) * (0.25 + 0.3 * Null.uExitRamp), sky * 0.9);
      col += vec3(0.2, 0.5, 1.0) * exp(-abs(uv.y + 0.02) * 30.0) * Null.uExitRamp * 0.3;
    }
    fragColor = vec4(col, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  float z = p.z;
  float depth = t;

  // --- wall shading ---------------------------------------------------------
  float phi = atan(p.y, p.x) / TAU + 0.5;

  // rings: emissive bands sweeping along z
  float ring = ringStripes(z, mix(1.2, 4.5, uMode), 1.0);
  float ringGlow = exp(-min(ring, 1.0 - ring) * 9.0);

  // longitudinal grid lines on the wall
  float grid = smoothstep(0.035, 0.0, abs(fract(phi * 36.0) - 0.5));

  // spiral cables along the wall
  float cable = exp(-abs(sin(phi * TAU * 4.0 + z * 0.7) - 0.97) * 24.0);

  // palette by depth: musical chord hue (bar-quantized) + depth + reprise shift
  float hue = musicHue() + depth * 0.004 + uMode * 0.12;
  vec3 base = palVoid(hue);

  // beat pulses travel as rings
  float beatRing = ringStripes(z, 1.2, 1.0);
  float pulseWave = exp(-min(abs(beatRing - 0.0), min(abs(beatRing - 0.25), abs(beatRing - 0.5))) * 8.0);

  vec3 col = base * 0.12;                       // ambient wall
  col += base * ringGlow * (1.6 + Null.uPulse * 2.0); // emissive rings
  col += vec3(1.0, 0.9, 1.0) * grid * 0.25;     // grid lines
  col += vec3(0.6, 0.7, 1.0) * cable * 0.5;     // spiral cables
  col += palVoid(hue + 0.5) * pulseWave * Null.uPulse * 2.5; // beat pulse rings

  // data streams: bright columns of light riding the wall toward the camera
  float streamX = hash12(vec2(floor(phi * 60.0), 0.0));
  float stream = exp(-abs(streamX - 0.12) * 40.0) * exp(-min(fract(z * 0.5 - Null.uTime * 3.0), 1.0 - fract(z * 0.5 - Null.uTime * 3.0)) * 3.0);
  col += vec3(0.7, 0.9, 1.0) * stream * (0.25 + Null.uIntensity * 0.5) * (1.0 + Null.uOnset * 1.5);

  // digital rain glyphs on the lower wall
  float rain = digitalRain(vec2(phi * 8.0, z * 0.5), 1.2 + uMode * 1.4, 0.07);
  col += vec3(0.3, 1.0, 0.7) * rain * 0.35 * Null.uIntensity;

  // core glow line down the tunnel center
  float rNow = length(p.xy);
  float core = exp(-rNow * 2.2) * (0.4 + 0.6 * Null.uPulse);
  col += vec3(0.8, 0.9, 1.0) * core;

  // --- kick strobe: bright white flash that slams the whole tunnel on each
  // kick drum hit (uFlash from the audio analyser, not the beat grid)
  col += vec3(0.9, 0.97, 1.0) * uFlash * (0.5 + 0.5 * exp(-rNow * 1.4));
  col += base * uFlash * 0.6;
  col += vec3(1.0, 0.95, 1.0) * ringGlow * uFlash * 0.9;

  // --- glitch color displacement (reprise + occasional flashes) ---------------
  float glitch = 0.0;
  if (uMode > 0.5) {
    float g = hash13(floor(p * 3.0 + floor(Null.uTime * 8.0) * vec3(7.0, 13.0, 1.0)));
    glitch = step(0.97, g);
    col = mix(col, palVoid(g + musicHue()) * 1.5, glitch * 0.9);
    // horizontal tear: offset hue blocks
    float tear = step(0.94, hash12(vec2(floor(Null.uTime * 10.0), floor(z * 4.0) * 0.1)));
    if (tear > 0.5 && uMode > 0.5) {
      col += palVoid(musicHue(0.3) + hash12(vec2(z, floor(Null.uTime * 12.0))) * 0.4) * 0.8;
    }
  }

  // --- melt: the reprise tunnel softens and dissolves as the section ends ------
  if (uMode > 0.5 && Null.uExitRamp > 0.01) {
    float melt = Null.uExitRamp * (0.5 + 0.5 * Null.uOnset);
    // drip the color down + wobble the wall
    float wob = fbm2(vec2(phi * 9.0, z * 0.4) + Null.uTime * 0.4) - 0.5;
    col *= 1.0 - melt * 0.5 * (0.5 + 0.5 * wob);
    col += palVoid(hue + 0.2) * melt * 0.4;
    // wall tears open: brightness bleeds through
    float tear2 = step(0.9, hash12(vec2(floor(phi * 30.0), floor(z * 2.0))));
    col = mix(col, vec3(1.0, 0.95, 1.0) * 0.9, tear2 * melt * 0.8);
  }

  // --- volumetric fog (denser with musical energy) ------------------------------
  float fogD = 1.0 - exp(-depth * (0.06 + 0.1 * Null.uIntensity + 0.05 * Null.uBass));
  vec3 fogCol = palVoid(musicHue(0.1) + uMode * 0.15) * (0.15 + Null.uIntensity * 0.25) + vec3(0.3, 0.2, 0.5) * 0.05;
  col = mix(col, fogCol, sat01(fogD));

  // depth fade to avoid hard cutoff
  col *= exp(-max(depth - 70.0, 0.0) * 0.15);

  // distance-based depth for DOF / motion blur (packed in alpha)
  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
