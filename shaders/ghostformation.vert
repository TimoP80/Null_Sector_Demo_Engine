#version 300 es
// ---------------------------------------------------------------------------
// SCENE 5 - Ghost Formation vertex shader.
// The emotional centerpiece: a cloud of particles that almost becomes a
// face. Each particle has a home on a procedural head + shoulders volume
// (with subtle eye / nose / mouth regions). A 4-bar form cycle gathers the
// cloud toward home, holds the "almost face" for a moment, then dissolves
// it just as the viewer starts to recognise it - the ghost is never
// completely visible. A permanent micro-shimmer means it never quite
// settles, even at peak formation.
// ---------------------------------------------------------------------------
#include <common>

layout(location = 0) in vec4 aSeed;   // xyz = seeds
layout(location = 1) in vec4 aSeed2;  // x = radius seed, y = speed seed, z = color seed

uniform float uExplode;   // 0..1 beat-synced burst (rattles the dissolving cloud)
uniform float uTrail;     // unused - kept for driver parity
uniform float uPointSize;
uniform float uFlash;     // 0..1 per-kick strobe (audio kick analyser)
uniform float uMode;      // 0 = ghost formation (5), 1 = stillness (12): bass-reactive face flicker
// in-scene handoff: the ghost is born from the machine it escaped - sparks
// over the outgoing frame's bright circuitry ignite and inherit its light
uniform float uTransition;    // 0..1 handoff window (1 = handoff done)
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)

out vec3 vCol;
out float vAlpha;
out float vDist;

const float PI2 = 6.28318530717959;

/** procedural home position on the ghost volume: head, shoulders, face. */
vec3 ghostHome(vec2 s) {
  float r = fract(s.x * 7.31 + s.y * 3.71);
  float u1 = fract(s.x * 13.71);
  float u2 = fract(s.y * 29.31);
  float u3 = fract(s.x * 3.13 + s.y * 1.7);
  if (r < 0.48) {
    // head: filled ellipsoid, a touch wider than deep, slightly forward-biased
    vec3 d = vec3(cos(u1 * PI2) * sin(u2 * 3.14159265),
                  cos(u2 * 3.14159265),
                  sin(u1 * PI2) * sin(u2 * 3.14159265)) * vec3(1.22, 1.15, 1.0);
    d = normalize(d);
    return vec3(0.0, 1.62, 0.0) + d * mix(0.20, 0.30, u3);
  }
  if (r < 0.78) {
    // shoulders + torso: wide, flattened ellipsoid
    float y = mix(0.28, 1.30, fract(s.x * 5.31));
    float rr = mix(0.10, 0.55, fract(s.y * 7.71));
    float a = u1 * PI2;
    return vec3(cos(a) * rr, y, sin(a) * rr * 0.32);
  }
  // face features: subtle eye / nose / mouth regions on the head's front
  float f = fract(s.y * 11.13);
  if (f < 0.4) {
    float side = f < 0.2 ? -1.0 : 1.0;
    return vec3(side * 0.085, 1.71 + u2 * 0.015, 0.30) + vec3(0.0, 0.0, u3 * 0.02);
  }
  if (f < 0.7) {
    return vec3(0.0, 1.58 + u1 * 0.14, 0.33) + vec3(0.0, 0.0, u3 * 0.02);
  }
  return vec3((u1 - 0.5) * 0.17, 1.45, 0.315) + vec3(0.0, 0.0, u3 * 0.02);
}

/** section-local bars (derived from uSecBar so re-times stay in sync) */
float barsInSection() { return Null.uSectionLocal / max(Null.uSecBar, 1e-4); }

/** 0..1 formation: gathers over the first bars of each 4-bar phrase, holds
 *  the face for a moment, then dissolves as the phrase downbeat lands -
 *  "every time they think they recognise it... it comes apart."
 *  Section-local, so the ghost always starts dissolved and each of the
 *  section's phrases runs a full cycle. */
float formCurve() {
  float phraseT = fract(barsInSection() / 4.0);
  return smoothstep(0.02, 0.38, phraseT) * (1.0 - smoothstep(0.55, 0.96, phraseT));
}

/** 0..1 dissolve window: nonzero only while the face is coming apart
 *  (phraseT 0.55-0.96), so the shatter never rattles the gathering cloud. */
float dissolveWindow() {
  float phraseT = fract(barsInSection() / 4.0);
  return smoothstep(0.55, 0.62, phraseT) * (1.0 - smoothstep(0.9, 0.96, phraseT));
}

void main() {
  vec2 s = aSeed.xy;
  float rSeed = aSeed2.x;
  float spd = aSeed2.y;
  float cSeed = aSeed2.z;

  vec3 home = ghostHome(s);
  float form = formCurve();

  // stillness (uMode 1): in the near-silent drop each bass transient snaps
  // the cloud into full face formation - the ghost flickers into view on the
  // sub-bass hits, then relaxes back into the dissolve phrase. uBass is the
  // live per-frame bass band energy (low floor in the drop, so the threshold
  // sits low to catch real transients rather than the beat grid). The gate is
  // tight and sharp so a transient snaps to full instead of fluttering at
  // partial formation on noisy frames.
  float bassFlick = uMode * smoothstep(0.04, 0.10, Null.uBass);
  form = max(form, bassFlick);

  // swirl scatter: the lower the form, the further the particles wander
  float rad = (1.0 - form) * (0.10 + 0.55 * rSeed);
  float a1 = s.x * PI2 + Null.uTime * spd * (0.5 + 1.1 * (1.0 - form));
  float a2 = s.y * PI2 - Null.uTime * spd * 0.7;
  vec3 off = vec3(cos(a1) * rad, sin(a2) * rad * 0.85, sin(a1 + a2) * rad);
  // never quite settles: permanent micro-shimmer + breathing while formed
  off += vec3(hash13(vec3(s, 1.0)), hash13(vec3(s, 2.0)), hash13(vec3(s, 3.0))) * 0.03;
  off += vec3(sin(Null.uTime * 1.9 + s.x * 41.0),
              cos(Null.uTime * 1.6 + s.y * 37.0), 0.0) * 0.018 * form;

  vec3 wpos = home + off;

  // dissolve burst: the phrase's kicks shatter the cloud outward - gated to
  // the dissolve window only, so it never rattles the forming or held face.
  // A bass flicker (stillness) suppresses the burst: the transient pulls the
  // face back together instead of tearing it further apart.
  float burst = uExplode * dissolveWindow() * (1.0 - bassFlick) * (0.5 + 0.5 * hash13(vec3(s, 4.0)));
  wpos += normalize(wpos - vec3(0.0, 1.5, 0.0) + 0.001) * burst * 1.4;

  vec4 viewPos = Null.uView * vec4(wpos, 1.0);
  gl_Position = Null.uProj * viewPos;
  vDist = -viewPos.z;

  // --- color: music-hued, brighter near the face front ---------------------------
  float faceGlow = smoothstep(0.25, 0.33, home.z) * (0.4 + 0.6 * form);
  vCol = palVoid(cSeed * 0.35 + musicHue(0.15) * 0.65);
  vCol = mix(vCol, vec3(1.0, 0.98, 1.0), 0.35);
  vCol *= 0.35 + 0.55 * form + 0.55 * faceGlow;
  vCol *= 1.0 + uFlash * 1.4 + bassFlick * 0.6;   // flash + bass snap share headroom

  // --- size: swells as it forms, perspective-scaled --------------------------------
  // the max is form-dependent so formation visibly swells (dense bright
  // sparks) while the dissolve falls back to fine scattered motes
  float size = mix(1.3, 3.6, form) * uPointSize;
  size *= 70.0 / max(vDist, 0.5);
  gl_PointSize = clamp(size, 0.5, mix(2.5, 8.0, form));

  // --- alpha: densest when formed, fades with distance ------------------------------
  vAlpha = (0.22 + 0.78 * form) * clamp(2.6 - vDist * 0.03, 0.0, 1.0);

  // --- in-scene handoff: born from the machine -------------------------------------
  // while the outgoing frame lingers, sparks landing on its luminous circuitry
  // (bright rings, gears, pipes, the core spine) swell, ignite and inherit
  // its light - the machine visibly tears apart into the ghost
  float handoff = 1.0 - uTransition;
  if (handoff > 0.001 && gl_Position.w > 0.0) {
    // clip -> NDC -> UV (w != 1 under the perspective projection, so the
    // divide is required or near/far particles sample wrong screen spots)
    vec2 ndc = gl_Position.xy / gl_Position.w;
    vec2 suv = clamp(ndc * 0.5 + 0.5, 0.001, 0.999);
    vec3 prevCol = texture(uPrevScene, suv).rgb;
    float lum = max(max(prevCol.r, prevCol.g), prevCol.b);
    float ign = smoothstep(0.04, 0.5, lum);
    // bright machine pixels swell the spark + inherit their color
    gl_PointSize = clamp(gl_PointSize * (1.0 + handoff * ign * 2.2), 0.5, 12.0);
    vAlpha *= mix(0.25, 1.0, ign);
    vCol = mix(vCol, prevCol * 1.9 + palVoid(musicHue(0.4)) * lum, handoff * ign * 0.75);
  }
}
