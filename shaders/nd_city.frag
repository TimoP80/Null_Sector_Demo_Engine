#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 4 // LOST CITY
// ---------------------------------------------------------------------------
// A gigantic abandoned futuristic city, procedurally built with a 2D grid DDA
// march (the flagship's voxel approach - GPU-friendly instancing-style
// repetition). Dense skyline, repeating towers, emissive windows, roads,
// atmospheric fog, distant lights. No people. The scene appears stable and
// believable - a memory reconstruction that looks convincing but is actually
// incomplete (the corruption comes in SCENE 5).
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;      // per-kick strobe (calm city: subtle)
uniform float uMode;
uniform float uHigh;
uniform float uTransition; // in-scene handoff (crystallizes from the tunnel)
uniform sampler2D uPrevScene;
uniform vec2  uSceneRes;

out vec4 fragColor;

const float CELL = 8.0;
const float CITY = 150.0;   // grid extent (+/-)

float bHeight(vec2 cell) {
  float h = hash12(cell + 100.0);
  float d = length(cell) * 0.09;
  float zone = smoothstep(1.6, 0.0, d);          // downtown cluster
  h = mix(h * 0.35, h * 1.1, zone);
  h *= 16.0;
  h += fbm2(cell * 0.07) * 8.0;
  return clamp(h, 2.5, 46.0);
}

bool hasBuilding(vec2 cell) { return hash12(cell + 3.7) > 0.24; }

/** window grid on a tower face: believable lit windows, some dark */
vec3 windowShade(vec3 wp, vec2 cell, float h) {
  float ux = fract(wp.x / CELL + 0.5) * 3.0;
  float uz = fract(wp.z / CELL + 0.5) * 3.0;
  float colIdx = floor(ux);
  float row = floor((h - wp.y) * 1.1);
  float cellh = hash12(cell + vec2(colIdx, row) * 1.7 + 0.5);

  float lit = step(0.58, cellh);                 // ~40% windows lit
  // rare slow flicker (abandoned grid, mostly steady)
  float flick = 0.9 + 0.1 * sin(Null.uTime * (1.0 + cellh * 3.0) + cellh * 40.0);
  flick *= step(0.97, fract(hash12(cell + vec2(row) * 3.1) + Null.uTime * 0.02));

  vec3 win = vec3(1.0, 0.86, 0.58) * lit * flick * 1.1;
  vec3 darkWin = vec3(0.05, 0.06, 0.12) * (1.0 - lit);

  // warm lobby glow at the base of some towers
  float lobby = step(0.85, hash12(cell + 21.0));
  float base = smoothstep(0.12, 0.0, wp.y) * lobby;
  win += vec3(1.0, 0.8, 0.5) * base * (0.6 + 0.4 * Null.uPulse) * 0.8;

  // roof beacon / antenna light on the tallest towers
  float beacon = step(0.995, hash12(cell + 7.0));
  float roofBand = smoothstep(0.06, 0.0, abs(wp.y - h));
  float blink = 0.5 + 0.5 * sin(Null.uTime * 2.2 + cellh * 30.0);
  vec3 roof = palVoid(cellh * 0.4 + musicHue(0.4)) * roofBand * beacon * (0.4 + blink * 0.9);

  // faint cold facade ambient
  vec3 facade = vec3(0.03, 0.04, 0.08);

  // subtle data strip on rare towers (kept minimal - believable)
  float strip = step(0.995, hash12(cell + 11.0));
  if (strip > 0.5) {
    float phase = fract(Null.uTime * 0.4 + wp.y * 0.05 + ux * 0.3);
    float streak = exp(-min(phase, 1.0 - phase) * 18.0);
    win += vec3(0.3, 0.75, 1.0) * streak * 0.5;
  }

  return facade + darkWin + win + roof;
}

/** streets between blocks: asphalt + lane lines + streetlight pools */
vec3 groundCol(vec3 wp, vec3 rd) {
  vec3 col = vec3(0.012, 0.014, 0.03);
  // road grid: the street corridors between blocks
  vec2 g = abs(fract(wp.xz / CELL) - 0.5);
  float road = step(0.42, max(g.x, g.y));
  col = mix(col, vec3(0.02, 0.022, 0.045), road);
  // lane dashes
  float lane = step(0.5, max(g.x, g.y)) * step(0.49, max(g.x, g.y));
  float dash = step(0.5, fract(wp.x * 0.25 + wp.z * 0.25 + Null.uTime * 0.02));
  col += vec3(0.5, 0.55, 0.6) * lane * dash * 0.06;
  // streetlight pools (occasional warm pools along the roads)
  float lamp = step(0.985, hash12(floor(wp.xz * 0.35)));
  col += vec3(1.0, 0.85, 0.6) * lamp * exp(-length(fract(wp.xz * 0.35) - 0.5) * 6.0) * 0.12;
  // faint wet sheen reflecting the sky
  col += vec3(0.03, 0.05, 0.1) * (0.3 + 0.7 * hash12(floor(wp.xz * 0.5))) * 0.3;
  return col;
}

void main() {
  float t = Null.uSectionLocal;
  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float tt = 0.0;
  vec3 p = ro;
  vec3 col = vec3(0.008, 0.01, 0.028);           // night sky
  float hitDist = -1.0;

  // --- ground plane -----------------------------------------------------------
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
  vec3 tDelta = abs(CELL * invD);
  vec3 tMax = (cellPos + stp * 0.5 + 0.5 - ro / CELL) * CELL * invD;

  vec2 hitCell = vec2(0.0);
  float hitH = 0.0;
  bool found = false;
  for (int i = 0; i < 96; i++) {
    if (tMax.x < tMax.y) { tt = tMax.x + 0.001; tMax.x += tDelta.x; cellPos.x += stp.x; }
    else { tt = tMax.y + 0.001; tMax.y += tDelta.y; cellPos.y += stp.y; }
    vec2 cell = cellPos.xy;
    if (abs(cell.x) > CITY / CELL || abs(cell.y) > CITY / CELL) break;
    if (!hasBuilding(cell)) continue;
    p = ro + rd * tt;
    float h = bHeight(cell);
    if (p.y < h) { hitCell = cell; hitH = h; hitDist = tt; found = true; break; }
    if (tt > 260.0) break;
  }

  if (found) {
    p = ro + rd * hitDist;
    col = windowShade(p, hitCell, hitH);
    // building edge rim highlight (subtle cool)
    float ex = abs(fract(p.x / CELL + 0.5) - 0.5);
    float ez = abs(fract(p.z / CELL + 0.5) - 0.5);
    float edge = smoothstep(0.48, 0.5, max(ex, ez));
    col += vec3(0.2, 0.45, 0.9) * edge * 0.8;
    // tall-tower depth fog darkens far buildings
    float fd = max(hitDist, 0.0);
    float fog = 1.0 - exp(-fd * 0.011);
    col = mix(col, vec3(0.05, 0.07, 0.13), fog);
  }

  // --- atmosphere ---------------------------------------------------------------
  float fd = max(hitDist, 0.0);
  // horizon glow: distant city light spill
  col += palVoid(musicHue(0.3)) * exp(-fd * 0.02) * 0.10;
  col += vec3(0.9, 0.6, 0.35) * exp(-fd * 0.01) * 0.05;
  // volumetric-ish haze band near the horizon
  float horizon = exp(-abs(uv.y) * 6.0);
  col += mix(vec3(0.35, 0.2, 0.5), vec3(0.1, 0.3, 0.55), Null.uPulse * 0.4)
       * horizon * 0.35;

  // kick strobe (subtle - the city is calm)
  col *= 1.0 + uFlash * 0.15;
  col += vec3(0.9, 0.96, 1.0) * uFlash * 0.08;

  // handoff from the tunnel: crystallize over the outgoing frame
  if (uTransition < 0.999) {
    vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
    col = mix(prev, col, uTransition);
  }

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = hitDist >= 0.0 ? depthFromViewZ(viewZ) : 1.0;
  gl_FragDepth = d01;
  fragColor = vec4(col, d01);
}
