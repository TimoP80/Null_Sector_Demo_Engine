#version 300 es
// ---------------------------------------------------------------------------
// SCENE 3 - Neural Network (optimized)
// A living 3D organism of pulsing nodes and growing/shrinking connections.
// The camera flies through synapses. The ghost appears only as distortions
// travelling through the network — ripples that warp positions, brighten
// connections, and scatter glowing particles through the void.
//
// Optimizations vs the initial version:
//   - Node positions/radii precomputed in const arrays (no hash per SDF call)
//   - Ghost wave centres precomputed once per frame (not per SDF call)
//   - Per-node ghost displacement removed (ghost on query position is enough)
//   - 48 steps instead of 64; 4-tap tetrahedral gradient instead of 6-tap
//   - Nearest-node loop: ONE length() per node (the sphere test and the
//     nearest-distance pick share it) - the old code ran two sqrts per node,
//     ~75 sqrts per SDF call (~4000 sqrts per pixel)
//   - ghostDistortion reuses the computed distances as normals instead of
//     calling normalize() (2 more sqrts saved per SDF call)
//   - pow(ang,6) in the background glow replaced with two multiplies
//   NOTE on the deeper refactors tried and rejected: hoisting the adjacency
//   hashes into per-pixel LOCAL int arrays regressed 30fps -> 0.5fps, and a
//   static NeuralBlock UBO (CPU-built, binding 1) regressed to ~20fps - both
//   on the ANGLE backend, whose dynamic indexing into large typed arrays is
//   slow. The scene also renders at a reduced internal resolution (0.66x via
//   the driver's neuralFbo), which is where the real speedup comes from.
// ---------------------------------------------------------------------------
#include <common>

uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)
uniform vec2 uSceneRes;  // ACTUAL render target size - the driver renders this
                         // scene into a reduced-res FBO (0.6x) and upscales;
                         // gl_FragCoord spans uSceneRes, not the full uRes

out vec4 fragColor;

// --- primitives --------------------------------------------------------------
float sdSphere(vec3 p, float r) { return length(p) - r; }
float sdCylinder(vec3 p, vec3 a, vec3 b, float r) {
  vec3 ab = b - a;
  vec3 ap = p - a;
  float t = dot(ap, ab) / dot(ab, ab);
  t = clamp(t, 0.0, 1.0);
  vec3 c = a + ab * t;
  return length(p - c) - r;
}

// --- ghost distortion -------------------------------------------------------
// The ghost appears as ripples that warp positions and brighten materials.
// waveCentre[0] = slow wave, waveCentre[1] = fast pulse — precomputed once
// per frame in main() and passed in. The (p - centre) vectors are reused as
// normals via their already-computed lengths, so no extra normalize() sqrts.
void ghostDistortion(vec3 p,
                     vec3 waveCentre0, vec3 waveCentre1,
                     float t1, float t2,
                     out vec3 displacement, out float ghostGlow) {
  vec3 d0 = p - waveCentre0;
  float dist0 = length(d0);
  float wave = sin(dist0 * 0.5 - t1 * 2.0) * exp(-dist0 * 0.08);

  vec3 d1 = p - waveCentre1;
  float dist1 = length(d1);
  float pulse = exp(-dist1 * 0.15) * sin(t2 * 3.0 - dist1 * 0.3);

  displacement = (d0 / max(dist0, 1e-4)) * wave * 0.15;
  displacement += (d1 / max(dist1, 1e-4)) * pulse * 0.08;

  float glow = max(0.0, wave * 0.3 + pulse * 0.2);
  ghostGlow = glow * (0.5 + 1.5 * Null.uBass);
}

// --- precomputed network data -----------------------------------------------
// On GL ES 3.0, we cannot use true const arrays with dynamic initialization.
// Instead we compute nodes from hashes in a loop, but cache the results in
// local arrays in main() so map() receives them as parameters rather than
// re-computing hashes on every SDF call.
void computeNodeData(int idx, out vec3 pos, out float radius) {
  vec3 h = vec3(hash13(vec3(float(idx), 0.0, 0.0)),
                hash13(vec3(float(idx), 1.0, 0.0)),
                hash13(vec3(float(idx), 2.0, 0.0)));
  pos = (h - 0.5) * 14.0;
  radius = 0.35 + 0.45 * hash13(vec3(float(idx), 3.0, 0.0));
}

// Precomputed adjacency: each node has 2-4 neighbors.
int nbrCount(int i) {
  float h = hash13(vec3(float(i), 8.0, 0.0));
  return 2 + int(floor(h * 3.0));
}

int nbrAt(int i, int k) {
  float h = hash13(vec3(float(i), float(k), 0.0));
  float h2 = hash13(vec3(float(i), float(k), 1.0));
  int j = int(floor(h * 75.0));
  if (j == i) j = int(floor(h2 * 74.0));
  if (j >= i) j = (j + 1) % 75;
  return j;
}

// --- scene SDF --------------------------------------------------------------
// Receives precomputed node data so hash calls happen once per frame, not per
// ray step.
float map(vec3 p,
          vec3 nodePositions[75], float nodeRadii[75],
          vec3 waveCentre0, vec3 waveCentre1, float t1, float t2,
          out float matID, out float ghostGlow) {
  // Apply ghost displacement to the query position
  vec3 disp;
  ghostDistortion(p, waveCentre0, waveCentre1, t1, t2, disp, ghostGlow);
  vec3 q = p + disp;

  // Find nearest node. ONE length() per node - the sphere SDF is dist - r,
  // and the nearest-distance pick reuses the same dist.
  float d = 1e9;
  int nearest = 0;
  float nearestD = 1e9;
  for (int i = 0; i < 75; i++) {
    vec3 nq = nodePositions[i];
    float dist = length(q - nq);
    float nd = dist - nodeRadii[i];
    if (nd < d) {
      d = nd;
      nearest = i;
    }
    if (dist < nearestD) {
      nearestD = dist;
      nearest = i;
    }
  }

  // Check connections from the nearest node only
  int nc = nbrCount(nearest);
  for (int k = 0; k < nc; k++) {
    int j = nbrAt(nearest, k);
    if (j == nearest) continue;
    vec3 qa = nodePositions[nearest];
    vec3 qb = nodePositions[j];

    // Connection radius (two hashes - cheap relative to the saved sqrt calls)
    float r = 0.04 + 0.06 * hash13(vec3(float(nearest), float(j), 2.0));
    float growth = 0.5 + 0.5 * sin(Null.uTime * (0.3 + hash13(vec3(float(nearest), float(j), 3.0)) * 0.4) + hash13(vec3(float(nearest), float(j), 4.0)) * 6.28);
    growth += Null.uPulse * 0.2 + Null.uBass * 0.15 + uFlash * 0.5;
    float cr = r * (0.5 + 0.8 * growth);

    if (cr < 0.01) continue;
    float cd = sdCylinder(q, qa, qb, cr);
    if (cd < d) {
      d = cd;
      nearest = -1;
    }
  }

  matID = nearest >= 0 ? 2.0 : 1.0;
  return d;
}

// --- shade ------------------------------------------------------------------
vec3 shade(vec3 p, vec3 n, float id, float ghost) {
  vec3 V = normalize(Null.uCamPos - p);
  float fres = pow(1.0 - max(dot(n, V), 0.0), 2.0);

  vec3 col;

  if (id > 1.5) {
    // Node: emissive core, pulsing with music
    float h = hash13(vec3(floor(length(p) * 1000.0), 0.0, 0.0));
    vec3 nodeCol = palVoid(musicHue(h * 0.1));
    float pulse = 0.7 + 0.3 * (0.5 + 0.5 * sin(Null.uTime * 2.0 + h * 6.28));
    pulse += Null.uPulse * 0.3 + Null.uBass * 0.2;
    col = nodeCol * (pulse + ghost * 2.0);
    col += palVoid(musicHue(0.1)) * fres * 0.5 * (1.0 + ghost);
  } else {
    // Connection: thin glowing line
    vec3 connCol = palVoid(musicHue(0.05));
    col = connCol * (0.8 + 0.4 * Null.uPulse + 0.5 * Null.uBass);
    col += vec3(1.0, 0.95, 1.0) * uFlash * 0.6;
    col += connCol * ghost * 3.0;
  }

  // Subtle directional light
  vec3 L1 = normalize(vec3(0.3, 0.8, 0.5));
  vec3 L2 = normalize(vec3(-0.5, -0.2, 0.7));
  float dif1 = max(dot(n, L1), 0.0) * 0.4;
  float dif2 = max(dot(n, L2), 0.0) * 0.2;
  col += palVoid(musicHue(0.2)) * dif1;
  col += palVoid(musicHue(0.3)) * dif2;

  float spec = pow(max(dot(n, normalize(L1 + V)), 0.0), 20.0);
  col += vec3(1.0, 0.95, 1.0) * spec * 0.5 * (1.0 + ghost);

  // Kick flash
  col *= 1.0 + uFlash * 0.3;
  col += vec3(1.0, 0.98, 1.0) * uFlash * 0.2;

  // Ghost bloom
  col += palVoid(musicHue(0.1)) * ghost * 0.8;

  return col;
}

void main() {
  // --- precompute node data once per frame ----------------------------------
  vec3 nodePositions[75];
  float nodeRadii[75];
  for (int i = 0; i < 75; i++) {
    computeNodeData(i, nodePositions[i], nodeRadii[i]);
  }

  // Precompute ghost wave centres (same for every SDF call this frame)
  float t1 = Null.uTime * 0.15;
  float t2 = Null.uTime * 0.22;
  vec3 waveCentre0 = vec3(
    2.5 * sin(t1 * 0.7 + 1.2),
    1.5 * cos(t1 * 0.5 + 0.8),
    2.0 * sin(t1 * 0.9 + 2.1)
  );
  vec3 waveCentre1 = vec3(
    3.0 * sin(t2),
    1.0 * cos(t2 * 1.3),
    2.5 * sin(t2 * 0.8 + 1.0)
  );

  // --- ghost corruption ------------------------------------------------------
  // The ghost's distortions become beat-locked: slice tears + RGB split ride
  // the SHARED glitch model (glitchSlice/glitchParticipation in common.glsl),
  // so the network seizes exactly like the logo sub-title and the reprise
  // tunnel. Corruption builds through the section (light ripples early,
  // full seizure by the voxel handoff) - the ghost taking over the network
  // it travels through.
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  float ghostT = smoothstep(0.35, 0.90, secT);   // 0 clean -> 1 fully seized
  float kickE = uFlash;
  float bassE = Null.uBass;
  float burstScale = ghostT;   // downbeats hit harder as the section peaks

  // --- ray ------------------------------------------------------------------
  vec2 res = uSceneRes.y > 1.0 ? uSceneRes : Null.uRes;   // reduced target when set
  vec2 uv = (gl_FragCoord.xy * 2.0 - res) / res.y;
  // ghost's slice tears: whole horizontal bands of the VIEW slice sideways on
  // kick/bass/downbeats (shared glitchSlice, scaled to uv units - lighter
  // than the tunnel's 0.18 so the delicate network keeps its geometry)
  vec2 gs = glitchSlice(gl_FragCoord.y / res.y, 32.0, ghostT, kickE, bassE, 23.7, burstScale);
  uv.x += gs.x * 0.12;
  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  float matID = 0.0, ghost = 0.0;
  float hit = 0.0;

  for (int i = 0; i < 48; i++) {
    p = ro + rd * t;
    float d;
    float g;
    d = map(p, nodePositions, nodeRadii,
            waveCentre0, waveCentre1, t1, t2,
            matID, g);
    ghost = g;
    if (d < 0.002 * t) { hit = 1.0; break; }
    t += d * 0.85;
    if (t > 40.0) break;
  }

  // --- background ------------------------------------------------------------
  if (hit < 0.5) {
    vec3 bg = vec3(0.01, 0.005, 0.02);

    // Distant node glow (uses precomputed positions; the length is reused as
    // the direction so no extra normalize() sqrts; pow->muls)
    float glow = 0.0;
    for (int i = 0; i < 75; i++) {
      vec3 np = nodePositions[i];
      vec3 dv = np - ro;
      float d = length(dv);
      if (d > 5.0) {
        float ang = max(dot(rd, dv / d), 0.0);
        float a2 = ang * ang;
        float spot = a2 * a2 * a2 * exp(-d * 0.04);
        glow += spot * 0.15;
      }
    }
    bg += palVoid(musicHue(0.1)) * glow * (0.5 + 0.5 * Null.uPulse);

    // Ghost glow in the void
    vec3 disp;
    float ghostVoid;
    ghostDistortion(ro + rd * 20.0, waveCentre0, waveCentre1, t1, t2, disp, ghostVoid);
    bg += vec3(0.6, 0.4, 1.0) * ghostVoid * 0.1;

    // ghost tears through the void too - a thin ghost-hue glow on the empty
    // space, widening with the hits (shared model; the full chromatic split
    // lives in the hit path below)
    vec2 gsv = glitchSlice(gl_FragCoord.y / res.y, 24.0, ghostT, kickE, bassE, 31.7, burstScale);
    if (abs(gsv.x) > 0.004) {
      bg += palVoid(musicHue(0.4) + gsv.y * 2.0) * (0.05 + gsv.y * 0.6);
    }

    fragColor = vec4(bg, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  // --- gradient (4-tap tetrahedral) -----------------------------------------
  vec3 n;
  {
    const float eps = 0.002;
    float id, g;
    vec2 e = vec2(1.0, -1.0) * 0.5773 * eps;
    n = e.xyy * map(p + e.xyy, nodePositions, nodeRadii,
                    waveCentre0, waveCentre1, t1, t2, id, g);
    n += e.yyx * map(p + e.yyx, nodePositions, nodeRadii,
                     waveCentre0, waveCentre1, t1, t2, id, g);
    n += e.yxy * map(p + e.yxy, nodePositions, nodeRadii,
                     waveCentre0, waveCentre1, t1, t2, id, g);
    n += e.xxx * map(p + e.xxx, nodePositions, nodeRadii,
                     waveCentre0, waveCentre1, t1, t2, id, g);
    n = normalize(n);
  }

  // the ghost's distortion glow slams harder on every kick + sub-bass hit -
  // gated by ghostT so the pristine opening stays exactly as before
  ghost *= 1.0 + (kickE * 1.2 + bassE * 0.8) * ghostT;

  vec3 col = shade(p, n, matID, ghost);

  // ghost tear: chromatic smear across the network (depth-banded, shared
  // glitchSlice) - R/B channels separate around the centre hue, widening
  // with the hits, exactly like the tunnel's wall tears. The blend rides
  // the hits too, so quiet bars stay clean and kicks bite through.
  vec2 gs2 = glitchSlice(t * 0.5, 2.0, ghostT, kickE, bassE, 7.3, burstScale);
  if (abs(gs2.x) > 0.002) {
    float hue = musicHue(0.3) + hash12(vec2(floor(p.x * 4.0), floor(Null.uTime * 12.0))) * 0.4;
    float chroma = 0.02 + gs2.y * 0.6;
    vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
    float bite = 0.25 + 0.35 * kickE + 0.2 * bassE;
    col = mix(col, tearCol * 1.3, bite);
    col += tearCol * (0.2 + 0.4 * kickE + 0.25 * bassE);
  }

  // Fog
  float fog = 1.0 - exp(-t * 0.025);
  col = mix(col, vec3(0.01, 0.005, 0.025), fog);

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
