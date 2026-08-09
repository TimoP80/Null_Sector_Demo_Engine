#version 300 es
// ---------------------------------------------------------------------------
// SCENE 6 - Voxel City
// Procedural cyberpunk city: 2D grid DDA march over building columns,
// flickering windows, neon billboards, fog, night atmosphere.
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)

out vec4 fragColor;

// in-scene handoff: the city crystallizes out of metaball fluid, and the
// outgoing particle storm's frame dissolves through. Driven by main.ts (the
// same first-two-beats window the generic crossfade would use).
uniform float uTransition;    // 0..1 handoff window
uniform sampler2D uPrevScene; // previous scene's final frame (bound on unit 9)
uniform float uFlash;         // 0..1 per-kick strobe (audio kick analyser)

// building footprint size
const float CELL = 6.0;
const float CITY = 120.0; // grid extent (+/-)

// metaball field the voxel towers condense out of (handoff morph)
float metaField(vec3 p) {
  float f = 0.0;
  for (int i = 0; i < 6; i++) {
    float fi = float(i);
    vec3 c = vec3(
      8.0 * sin(Null.uTime * 0.5 + fi * 1.7),
      2.5 + 3.0 * sin(Null.uTime * 0.4 + fi * 2.1),
      8.0 * cos(Null.uTime * 0.45 + fi * 1.3));
    vec3 d = p - c;
    f += 2.4 / (dot(d, d) + 1.0);
  }
  return f;
}

// building height for a cell
float bHeight(vec2 cell) {
  float h = hash12(cell + 100.0);
  // taller downtown cluster near origin
  float d = length(cell) * 0.12;
  float zone = smoothstep(1.0, 0.0, d);
  h = mix(h * 0.4, h * 1.0, zone);
  h *= 14.0;
  h += fbm2(cell * 0.08) * 6.0;
  // handoff: buildings grow out of the metaball fluid (blobby -> voxel towers)
  if (uTransition < 0.999) {
    float m = metaField(vec3(cell.x * CELL, 0.0, cell.y * CELL));
    h = mix(1.5 + m * 7.0, h, uTransition);
  }
  return clamp(h, 2.0, 40.0);
}

// is there a building in this cell?
bool hasBuilding(vec2 cell) {
  return hash12(cell + 3.7) > 0.35;
}

// window shading at a wall point: returns lit windows color + flicker
vec3 windowShade(vec3 wp, vec2 cell, float h) {
  // local coords within building
  float ux = fract(wp.x / CELL + 0.5) * 3.0;
  float uz = fract(wp.z / CELL + 0.5) * 3.0;
  float colIdx = floor(ux);
  float row = floor((h - wp.y) * 1.6);
  float cellh = hash12(cell + vec2(colIdx, row) * 1.7 + 0.5);

  // neon strips on roof
  float roofBand = smoothstep(0.05, 0.0, abs(wp.y - h));

  // windows: lit ~45%, flickering via time noise
  float lit = step(0.55, cellh);
  float flick = 0.85 + 0.15 * sin(Null.uTime * 20.0 + cellh * 40.0 + hash12(cell + vec2(row)) * 30.0);
  flick *= step(0.9, fract(hash12(vec2(cellh * 3.3) + floor(Null.uTime * 2.0) * 0.13)));

  // weathering: grime accumulates near the ground, streaks on upper floors
  float grime = smoothstep(0.0, 0.5, wp.y / max(h, 1.0)) * 0.25;

  vec3 win = vec3(1.0, 0.85, 0.55) * lit * flick * 1.2;
  vec3 neon = palVoid(cellh * 0.5 + musicHue() * 0.5) * (0.6 + 0.8 * Null.uPulse);
  vec3 col = win + neon * roofBand * 1.4;

  // data streams: light columns racing up certain tower faces
  float stream = step(0.94, hash12(cell + 5.0));
  if (stream > 0.5) {
    float phase = fract(Null.uTime * (0.6 + cellh * 0.8) + wp.y * 0.12 + ux * 0.5);
    float streak = exp(-min(phase, 1.0 - phase) * 14.0);
    col += vec3(0.4, 0.8, 1.0) * streak * (0.5 + Null.uPulse * 0.8);
  }

  // digital rain on the darkest facades
  float rainFace = step(0.97, hash12(cell + 9.0));
  if (rainFace > 0.5) {
    float glyph = step(0.85, hash12(vec2(ux * 7.0, floor(Null.uTime * 2.0) * 0.7 + row * 0.3)));
    col += vec3(0.2, 1.0, 0.6) * glyph * 0.4;
  }

  // animated billboards on some towers
  float bill = step(0.93, hash12(cell + 17.0));
  if (bill > 0.5) {
    // a glowing sign sweeping around the tower
    float sweep = fract(Null.uTime * 0.1 + wp.y * 0.05);
    float sign = smoothstep(0.02, 0.0, abs(sweep - 0.5)) * (0.5 + 0.5 * sin(Null.uTime * 3.0 + cellh * 20.0));
    col += palVoid(cellh * 0.4 + musicHue() * 0.6) * sign * 3.0;
  }
  return col;
}

// ground: dark asphalt + neon grid + glow pools + laser grid + wet reflections
vec3 groundCol(vec3 wp, vec3 rd) {
  vec3 col = vec3(0.02, 0.02, 0.05);
  // street grid lines
  vec2 g = abs(fract(wp.xz / CELL) - 0.5);
  float line = smoothstep(0.48, 0.5, max(g.x, g.y));
  col += vec3(0.2, 0.5, 1.0) * line * (0.2 + 0.3 * Null.uPulse);
  // wet reflections of buildings above (fake with hash glow)
  float refl = hash12(floor(wp.xz * 0.4)) * step(0.92, hash12(floor(wp.xz * 0.4) + 9.0));
  col += palVoid(hash12(floor(wp.xz * 0.4)) * 0.5 + musicHue() * 0.5) * refl * 0.4;
  // laser grid sweeping the streets (pulses with the bass)
  float laser = step(0.5, abs(fract(wp.x * 0.55 + Null.uTime * 1.5) - 0.5) * (2.0 - 0.4 * Null.uBass));
  col += vec3(1.0, 0.2, 0.8) * (1.0 - laser) * (0.05 + 0.4 * Null.uBass) * (0.4 + 0.6 * Null.uPulse);
  // puddle sheen
  float puddle = step(0.985, hash12(floor(wp.xz * 0.8)));
  col += palVoid(musicHue() * 0.5 + hash12(floor(wp.xz * 0.8)) * 0.5) * puddle * 0.5;
  // handoff: metaball blobs pool in the streets, fading as towers crystallize
  if (uTransition < 0.999) {
    float m = metaField(wp + vec3(0.0, 1.2, 0.0));
    col += palVoid(musicHue() * 0.4 + 0.45 + m * 0.06) * (0.35 + 0.65 * m) * (1.0 - uTransition) * 1.2;
  }
  return col;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;

  // --- ghost corruption ------------------------------------------------------
  // The ghost's grip on the city: slice tears + RGB split ride the SHARED
  // glitch model (glitchSlice in common.glsl) - same beat lock as the neural
  // net, cathedral, logo sub-title and reprise tunnel. The ramp peaks late in
  // the section so the seizure crests exactly as the logo ignites from the
  // grid (the handoff below stays clean - it mixes before the smear is added
  // to the city render, and by then ghostT is ~0 at the section's start).
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  float ghostT = smoothstep(0.45, 0.95, secT);   // late ramp: peaks pre-logo
  float kickE = uFlash;
  float bassE = Null.uBass;
  float burstScale = ghostT;   // downbeats hit harder as the section peaks

  // whole horizontal bands of the VIEW slice sideways on kick/bass/downbeats
  // (shared glitchSlice, scaled to uv units - same as the cathedral's 0.15)
  vec2 gs = glitchSlice(gl_FragCoord.y / Null.uRes.y, 32.0, ghostT, kickE, bassE, 41.3, burstScale);
  uv.x += gs.x * 0.15;

  // --- kick camera shake ----------------------------------------------------
  // The city rattles on every real kick: a per-kick hash-direction jolt that
  // peaks with the uFlash envelope and decays with it, scaled by ghostT so
  // the pristine opening stays steady and the rattling intensifies as the
  // ghost seizes the grid (complementing the slice tears). Displacing uv
  // before the ray is built shakes the whole view; the handoff prev frame is
  // sampled at gl_FragCoord so it stays stable while the city rattles.
  // kick + downbeat shake: kicks rattle (uFlash) and downbeats add a burst
  // kick so the camera flinches exactly when the shared tears throw - same
  // beat-lock story (glitchBurst in common.glsl). +-0.06 uv on a full kick,
  // +-0.03 more on a full downbeat burst at full takeover.
  float shake = ghostT * (uFlash * 0.06 + glitchBurst(ghostT) * 0.03);
  if (shake > 0.0005) {
    float sk = floor(Null.uTime * 24.0);
    vec2 jolt = vec2(hash12(vec2(sk, 51.7)), hash12(vec2(sk, 73.3))) - 0.5;
    uv += jolt * shake * 2.0;             // per-axis, jolt normalized to [-1,1]
  }

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  vec3 col = vec3(0.02, 0.01, 0.05); // night sky
  float hitDist = -1.0;

  // ray-plane: ground is y=0
  float tGround = -ro.y / max(rd.y, 1e-4);
  if (rd.y < -0.001) {
    vec3 gp = ro + rd * tGround;
    if (abs(gp.x) < CITY && abs(gp.z) < CITY) {
      col = groundCol(gp, rd);
      hitDist = tGround;
    }
  }

  // --- grid DDA march ---------------------------------------------------------
  vec3 invD = 1.0 / rd;
  vec3 stp = sign(rd);
  vec3 cellPos = floor(ro / CELL);
  // tDelta = time between cell boundaries per axis; tMax = time to next boundary
  vec3 tDelta = abs(CELL * invD);
  vec3 tMax = (cellPos + stp * 0.5 + 0.5 - ro / CELL) * CELL * invD;

  vec2 hitCell = vec2(0.0);
  float hitH = 0.0;
  bool found = false;

  for (int i = 0; i < 90; i++) {
    // advance to next cell boundary
    if (tMax.x < tMax.y) {
      t = tMax.x + 0.001;
      tMax.x += tDelta.x;
      cellPos.x += stp.x;
    } else {
      t = tMax.y + 0.001;
      tMax.y += tDelta.y;
      cellPos.y += stp.y;
    }
    vec2 cell = cellPos.xy;
    if (abs(cell.x) > CITY / CELL || abs(cell.y) > CITY / CELL) break;
    if (!hasBuilding(cell)) continue;

    p = ro + rd * t;
    float h = bHeight(cell);
    if (p.y < h) {
      hitCell = cell;
      hitH = h;
      hitDist = t;
      found = true;
      break;
    }
    if (t > 220.0) break;
  }

  if (found) {
    // check front face orientation for shading
    p = ro + rd * hitDist;
    vec3 base = vec3(0.03, 0.03, 0.07);
    vec3 win = windowShade(p, hitCell, hitH);
    col = base + win;
    // building edge neon (rim highlight)
    float ex = abs(fract(p.x / CELL + 0.5) - 0.5);
    float ez = abs(fract(p.z / CELL + 0.5) - 0.5);
    float edge = smoothstep(0.48, 0.5, max(ex, ez));
    col += palVoid(hash12(hitCell) * 0.5 + musicHue() * 0.5) * edge * 1.6;
  }

  // --- fog (denser + more reactive) ------------------------------------------------
  float fd = max(hitDist, 0.0);
  float fog = 1.0 - exp(-fd * (0.014 + 0.008 * Null.uBass));
  vec3 fogCol = mix(vec3(0.25, 0.1, 0.5), vec3(0.1, 0.3, 0.6), Null.uPulse * 0.5);
  col = mix(col, fogCol * (0.25 + Null.uIntensity * 0.2), fog);

  // light shafts from the center towers (volume-ish god rays toward the origin)
  float shaft = exp(-abs(atan(p.x, p.z) * 2.0 - 1.2) * 8.0) * exp(-fd * 0.02);
  col += vec3(0.3, 0.4, 1.0) * shaft * (0.05 + 0.12 * Null.uIntensity) * (1.0 + Null.uOnset * 2.0);

  // distant glow horizon
  col += palVoid(musicHue() * 0.5) * exp(-fd * 0.03) * 0.15;

  // anticipation: everything brightens and pushes before the overload handoff
  col *= 1.0 + Null.uAnticipation * 0.5;
  col += vec3(0.4, 0.3, 0.8) * Null.uAnticipation * 0.2;

  // the ghost tears through the sky on the miss path - a thin ghost-hue glow
  // on the empty night, widening with the hits (shared model). The ghostT
  // gate keeps the pristine opening zero-cost.
  if (ghostT > 0.001 && hitDist < 0.0) {
    vec2 gsv = glitchSlice(gl_FragCoord.y / Null.uRes.y, 24.0, ghostT, kickE, bassE, 31.7, burstScale);
    if (abs(gsv.x) > 0.004) {
      col += palVoid(musicHue(0.4) + gsv.y * 2.0) * (0.05 + gsv.y * 0.6);
    }
  }

  // per-kick strobe: city lights flash with the kick drum
  col *= 1.0 + uFlash * 0.35;
  col += vec3(0.8, 0.9, 1.0) * uFlash * 0.2;

  // --- ghost tear: chromatic smear across the city -----------------------------
  // Depth-banded, shared glitchSlice - R/B channels separate around the
  // centre hue, widening with the hits, exactly like the cathedral and neural
  // net. Hit pixels only (the sky tear below owns the miss path; on a miss
  // hitDist=-1 would collapse every pixel to one band and flash the whole sky
  // uniformly instead of slicing it). Placed BEFORE the handoff mix so the
  // outgoing scene dissolves clean; the smear rides the hits so quiet bars
  // stay clean and kicks bite through. The ghostT gate makes the pristine
  // opening zero-cost (the helper would return 0,0 anyway).
  if (ghostT > 0.001 && hitDist >= 0.0) {
    vec2 gs2 = glitchSlice(hitDist * 0.5, 2.0, ghostT, kickE, bassE, 7.3, burstScale);
    if (abs(gs2.x) > 0.002) {
      float hue = musicHue(0.3) + hash12(vec2(floor(p.x * 4.0), floor(Null.uTime * 12.0))) * 0.4;
      float chroma = 0.02 + gs2.y * 0.6;
      vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
      float bite = 0.25 + 0.35 * kickE + 0.2 * bassE;
      col = mix(col, tearCol * 1.3, bite);
      col += tearCol * (0.2 + 0.4 * kickE + 0.25 * bassE);
    }
  }

  // --- in-scene handoff dissolve -----------------------------------------------
  // the outgoing particle storm's frame is the base layer; the crystallizing
  // city renders over it as the window closes (0 = pure previous scene)
  if (uTransition < 0.999) {
    vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
    col = mix(prev, col, uTransition);
    // blob glow over the whole frame so the morph reads everywhere
    float m = metaField(p + vec3(0.0, 1.5, 0.0));
    col += palVoid(musicHue() * 0.4 + 0.5 + m * 0.05) * (0.25 + 0.5 * m) * (1.0 - uTransition);
  }

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
