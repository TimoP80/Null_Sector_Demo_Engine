#version 300 es
// ---------------------------------------------------------------------------
// SCENES 1 + 8 - Quantum Tunnel
// Replaces the old smooth neon tube with fragmented rings, impossible
// geometry, portals, procedural glyphs, and changing topology.
//
// uMode 0 = tunnel (first flight after the intro)
// uMode 1 = reprise (faster, topology disintegrates, heavy glitch)
// ---------------------------------------------------------------------------
#include <common>

uniform float uMode;  // 0 = tunnel, 1 = reprise
uniform float uFlash; // 0..1 strobe on each kick drum hit
// in-scene handoff: the outgoing scene (intro logo at section 1, the logo
// climax at the reprise) dissolves into the tunnel as the camera flies in -
// mixed before the reprise glitch so the ghost tears the fading frame apart
uniform float uTransition;    // 0..1 handoff window (1 = handoff done)
uniform sampler2D uPrevScene; // previous scene's final frame (unit 9)

out vec4 fragColor;

// --- ring topology: cross-section of the tunnel at a given z -----------------
// Returns the "radius" of the tunnel wall in direction phi at depth z.
// The topology morphs from circle -> square -> star as intensity increases.
float topologyRadius(float phi, float z, float topo) {
  // circle
  float r = 1.0;
  // square (smooth box in polar)
  float sq = 1.0 / max(abs(cos(phi)), abs(sin(phi)));
  // star (3-pointed)
  float st = 1.0 + 0.25 * cos(phi * 3.0 + z * 0.3 + Null.uTime * 0.5);
  // morph between them based on topo parameter
  r = mix(r, sq, topo * 0.30);
  r = mix(r, st, max(0.0, topo - 0.30) * 1.43);
  return r;
}

// --- ring fragment: a single ring slice at depth z ---------------------------
// Returns true if the ray at (phi, z) hits the ring fragment.
// Each ring is independently modulated: missing segments, rotation, twist.
float ringFragment(float phi, float z, float ringZ, float ringWidth) {
  // Distance from the ring center
  float dz = abs(z - ringZ);
  if (dz > ringWidth * 0.5) return 0.0;

  // Fragmentation: each ring has missing arc segments
  float segs = 4.0 + 6.0 * hash13(vec3(floor(ringZ * 0.5), 0.0, 0.0));
  float segPhase = hash13(vec3(floor(ringZ * 0.5), 1.0, 0.0)) * TAU;
  float seg = sin(phi * segs + segPhase);
  float frag = step(seg, 0.5 + 0.3 * sin(Null.uTime * 0.5 + ringZ * 0.3));

  // Twist: each ring rotates around the axis
  float twist = hash13(vec3(floor(ringZ * 0.5), 2.0, 0.0)) * TAU;
  float twistA = twist + Null.uTime * (0.2 + 0.5 * hash13(vec3(floor(ringZ * 0.5), 3.0, 0.0)));

  // Ring width pulse
  float pulse = 1.0 + 0.3 * sin(Null.uTime * (0.5 + hash13(vec3(floor(ringZ * 0.5), 4.0, 0.0)) * 0.5) + ringZ * 0.2);
  pulse += Null.uPulse * 0.2;

  return frag * pulse;
}

// --- glyph: floating procedural symbol at a position -------------------------
float glyph(vec3 pos, float seed) {
  // A simple procedural glyph: combination of circles, lines, and arcs
  float d = 1e9;
  float r = 0.15 + 0.08 * sin(Null.uTime * 0.3 + seed * 6.28);
  // Outer ring
  d = min(d, abs(length(pos.xy) - r));
  // Inner dot
  d = min(d, length(pos.xy - vec2(0.0, 0.0)));
  // Cross lines
  d = min(d, abs(pos.x));
  d = min(d, abs(pos.y));
  // Diagonal
  d = min(d, abs(pos.x - pos.y) * 0.707);
  // Arc segment
  float arc = length(pos.xy) - r * 0.6;
  d = min(d, abs(arc));

  // Animated rotation
  float a = Null.uTime * 0.4 + seed * 6.28;
  vec2 rp = rotate2(pos.xy, a);
  d = min(d, abs(rp.x) * 0.5);
  d = min(d, abs(rp.y) * 0.5);

  return exp(-d * 8.0);
}

// --- portal: a hole in the ring that reveals another part of the tunnel -------
// Returns a portal intensity (0 = no portal, 1 = full portal)
float portal(vec3 p, float z, float phi, float ringZ) {
  float dz = abs(z - ringZ);
  if (dz > 0.4) return 0.0;

  // Each ring may have a portal at a random angular position
  float portalAngle = hash13(vec3(floor(ringZ * 0.5), 5.0, 0.0)) * TAU;
  float portalWidth = 0.15 + 0.10 * hash13(vec3(floor(ringZ * 0.5), 6.0, 0.0));
  float portalDist = abs(mod(phi - portalAngle + PI, TAU) - PI);
  float portalEdge = smoothstep(portalWidth, portalWidth * 0.5, portalDist);

  // Portal reveals a different depth: a tunnel-within-a-tunnel
  // The visible "inside" of the portal is a different ring fragment
  float portalDepth = ringZ + 2.0 + 1.0 * sin(Null.uTime * 0.3 + ringZ * 0.5);
  float portalBright = 0.5 + 0.5 * sin(Null.uTime * 0.7 + ringZ * 0.4);

  return portalEdge * portalBright * 0.5;
}

void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;

  // music-reactive corruption: kick flash, sub-bass and downbeat bursts drive
  // how hard the reprise seizes on the beat - reused by the ray slice
  // displacement below and the glitch/tear block in the hit path, all through
  // the SHARED glitch helpers in common.glsl so the tunnel tears exactly like
  // the logo sub-title (same beat lock, same participation curve)
  float kickE = uFlash;               // per-kick strobe (audio kick analyser)
  float bassE = Null.uBass;           // sub-bass analyser (0..1)
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  // downbeat burst escalation: peaks exactly on the downbeat and decays across
  // the bar; escalates with section progress so the reprise starts with light
  // seizures and builds to a full convulsion by the end (same arc as the
  // logo's ramp)
  float burstScale = 0.15 + 0.85 * secT;

  // reprise slice displacement: whole horizontal bands of the view jump
  // sideways; participation + amplitude ride kick/bass/downbeats + section
  // progress so the whole tunnel seizes with each hit instead of only the
  // title card. Shared glitchSlice (common.glsl) - the same beat-locked model
  // as the logo's text tears, scaled from normalized bands to screen uv
  // (max tear 0.45 -> 0.081 uv, matching the previous cap)
  if (uMode > 0.5) {
    vec2 gs = glitchSlice(gl_FragCoord.y / Null.uRes.y, 48.0, secT, kickE, bassE, 13.7, burstScale);
    uv.x += gs.x * 0.18;
  }

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  float t = 0.0;
  vec3 p = ro;
  float hit = 0.0;
  float handoff = 1.0 - uTransition;
  float hitZ = 0.0;
  float hitPhi = 0.0;
  float hitRing = 0.0;
  float portalHit = 0.0;

  // Topology parameter: morphs with intensity and section-local time
  float topo = Null.uIntensity;
  if (uMode > 0.5) {
    topo = min(1.0, topo + Null.uSectionLocal * 0.05);
  }

  const int STEPS = 80;
  float stepLen = 0.35;

  for (int i = 0; i < STEPS; i++) {
    p = ro + rd * t;
    float z = p.z;

    // Base radius of the tunnel (breathing with music)
    float R0 = 3.2 + 0.35 * sin(z * 0.55 + Null.uTime * 0.8) + 0.55 * Null.uPulse;

    // Ring segmentation: only certain z positions have rings
    float ringSpacing = 1.2 + 0.3 * sin(Null.uTime * 0.2);
    float ringZ = round(z / ringSpacing) * ringSpacing;
    float ringWidth = 0.5 + 0.25 * sin(Null.uTime * 0.3 + ringZ * 0.2);
    ringWidth += Null.uPulse * 0.15 + uFlash * 0.2;

    // Check if we're inside a ring fragment
    float phi = atan(p.y, p.x);
    float topoR = topologyRadius(phi, z, topo);
    float R = R0 * topoR;

    float r = length(p.xy);
    float dz = abs(z - ringZ);
    float inRing = step(dz, ringWidth * 0.5);

    // Fragment mask: is this ring present at this angle?
    float frag = 0.0;
    if (inRing > 0.5) {
      frag = ringFragment(phi, z, ringZ, ringWidth);
    }

    // Portal check: holes in the ring
    float portalHere = 0.0;
    if (inRing > 0.5 && frag > 0.5) {
      portalHere = portal(p, z, phi, ringZ);
    }

    // Hit condition: we hit the wall (r > R) AND there's a ring fragment here
    // and no portal is covering this spot
    if (r > R && inRing > 0.5 && frag > 0.5 && portalHere < 0.5) {
      hit = 1.0;
      hitZ = z;
      hitPhi = phi;
      hitRing = ringZ;
      portalHit = portalHere;
      break;
    }

    // Portal interior: if we're inside a portal, we see through to another depth
    if (r > R && inRing > 0.5 && portalHere > 0.5) {
      // The portal is open — treat as a miss through the wall
      // But we'll mark it as a special portal hit for shading
      portalHit = portalHere;
      hit = 1.0;
      hitZ = z;
      hitPhi = phi;
      hitRing = ringZ;
      break;
    }

    t += stepLen;
    if (t > 90.0) break;
  }

  if (hit < 0.5) {
    // Void: distant glow at the far end
    vec3 voidCol = palVoid(musicHue(0.1) + Null.uIntensity * 0.3) * 0.06;
    float centerGlow = exp(-length(uv) * 1.8);
    vec3 col = voidCol + palVoid(musicHue(0.2) + Null.uBeat * 0.002) * centerGlow * (0.3 + Null.uPulse);
    // handoff: the outgoing scene still fills the void while it fades out
    if (handoff > 0.001) {
      vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
      col = mix(prev * 1.1, col, uTransition);
    }
    fragColor = vec4(col, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  float z = hitZ;
  float phi = hitPhi;
  float depth = t;

  // --- wall shading ---------------------------------------------------------
  float hue = musicHue() + depth * 0.004 + uMode * 0.12;
  vec3 base = palVoid(hue);

  // Ring emissive glow
  float ringGlow = exp(-abs(z - hitRing) * 6.0) * (0.8 + 0.4 * Null.uPulse);

  // Angle-based stripe pattern on the ring
  float stripe = sin(phi * 8.0 + z * 0.5 + Null.uTime * 1.2) * 0.5 + 0.5;
  float stripeGlow = exp(-min(stripe, 1.0 - stripe) * 6.0);

  // Procedural glyphs floating between rings
  float glyphIntensity = 0.0;
  for (int gi = 0; gi < 4; gi++) {
    float gif = float(gi);
    float gz = hitRing + 0.3 + gif * 0.2;
    float gAngle = hash13(vec3(floor(hitRing * 0.5), gif, 0.0)) * TAU;
    vec3 gp = vec3(
      2.0 * cos(gAngle + Null.uTime * 0.1 * hash13(vec3(floor(hitRing * 0.5), gif, 1.0))),
      2.0 * sin(gAngle + Null.uTime * 0.1 * hash13(vec3(floor(hitRing * 0.5), gif, 2.0))),
      gz
    );
    vec3 gd = p - gp;
    float gv = glyph(gd, gif + hitRing);
    glyphIntensity += gv;
  }

  // Portal visualization: if we hit a portal, show the "inside"
  float portalVis = 0.0;
  if (portalHit > 0.5) {
    // The portal reveals a different part of the tunnel — a bright, shifting
    // window into another ring
    portalVis = portalHit * (0.5 + 0.5 * sin(Null.uTime * 1.5 + hitRing * 0.5));
    // The portal's inner view: a different hue
    float portalHue = fract(hue + 0.3 + 0.2 * sin(Null.uTime * 0.4 + hitRing));
    vec3 portalCol = palVoid(portalHue);
    base = mix(base, portalCol * 1.5, portalVis);
  }

  // --- compose the fragment color -------------------------------------------
  vec3 col = vec3(0.0);

  // Ambient ring base
  col += base * 0.15;

  // Ring emissive glow
  col += base * ringGlow * (1.6 + Null.uPulse * 2.0);

  // Angle stripes
  col += vec3(0.8, 0.9, 1.0) * stripeGlow * 0.4;

  // Glyphs
  col += vec3(0.6, 0.8, 1.0) * glyphIntensity * (0.5 + 0.5 * Null.uPulse);

  // Portal glow
  if (portalHit > 0.5) {
    col += vec3(0.9, 0.95, 1.0) * portalVis * 0.8;
  }

  // Data streams along the ring surface
  for (int si = 0; si < 3; si++) {
    float sf = float(si);
    float streamAngle = hash13(vec3(floor(hitRing * 0.5), sf, 3.0)) * TAU;
    float streamWidth = 0.05 + 0.03 * hash13(vec3(floor(hitRing * 0.5), sf, 4.0));
    float streamDist = abs(mod(phi - streamAngle + PI, TAU) - PI);
    float stream = exp(-streamDist * 20.0) * (0.3 + 0.7 * (0.5 + 0.5 * sin(Null.uTime * 2.0 + hitRing * 0.5 + sf)));
    col += vec3(0.3, 0.7, 1.0) * stream * (0.3 + Null.uPulse * 0.5);
  }

  // --- kick strobe -----------------------------------------------------------
  col += vec3(0.9, 0.97, 1.0) * uFlash * (0.5 + 0.5 * ringGlow);
  col += base * uFlash * 0.6;

  // --- in-scene handoff: the outgoing scene dissolves into the tunnel --------
  // Mixed in BEFORE the reprise glitch, so the ghost's glitch/tear/melt visibly
  // corrupts the fading logo frame as it comes apart. Note: the prev frame is
  // sampled at gl_FragCoord (clean - NOT ray-displaced) while the tunnel
  // render behind it seizes - intentional, so the logo dissolves in clean
  // while the tunnel convulses around it
  if (handoff > 0.001) {
    vec3 prev = texture(uPrevScene, gl_FragCoord.xy / Null.uRes).rgb;
    col = mix(prev * 1.1, col, uTransition);
  }

  // --- reprise: glitch, disintegrate, topology warp --------------------------
  if (uMode > 0.5) {
    // Glitch color displacement: participation rides the SHARED participation
    // curve (glitchParticipation, common.glsl) - same beat lock as the logo
    // sub-title; capped (~45% max) so full hits can't white-out the wall
    float part = glitchParticipation(secT, kickE, bassE, burstScale);
    float g = hash13(floor(p * 3.0 + floor(Null.uTime * 8.0) * vec3(7.0, 13.0, 1.0)));
    float glitch = step(max(0.55, 0.96 - part * 0.41), g);
    col = mix(col, palVoid(g + musicHue()) * 1.5,
              glitch * min(0.9 + 0.35 * kickE + 0.25 * bassE, 1.0));

    // Horizontal tear: probability rides the same shared participation curve,
    // and the band reads as an RGB-split slice whose width comes from the
    // shared glitchSlice split (widens with the hits) - the ghost tearing the
    // frame on the beat, consistent with the logo's title corruption
    float th = hash12(vec2(floor(Null.uTime * 10.0), floor(z * 4.0) * 0.1));
    float tear = step(max(0.55, 0.93 - part * 0.38), th);
    if (tear > 0.5) {
      float hue = musicHue(0.3) + hash12(vec2(z, floor(Null.uTime * 12.0))) * 0.4;
      vec2 gs = glitchSlice(z * 0.5, 2.0, secT, kickE, bassE, 7.3, burstScale);
      float chroma = 0.02 + gs.y * 0.6;   // shared RGB split, scaled to hue space
      vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
      col += tearCol * (0.6 + 0.9 * kickE + 0.5 * bassE);
    }

    // Ring disintegration: rings dissolve as the section ends
    if (Null.uExitRamp > 0.01) {
      float melt = Null.uExitRamp * (0.5 + 0.5 * Null.uOnset);
      col *= 1.0 - melt * 0.4;
      col += palVoid(hue + 0.2) * melt * 0.3;

      // Wall tears open
      float tear2 = step(0.88, hash12(vec2(floor(phi * 30.0), floor(z * 2.0))));
      col = mix(col, vec3(1.0, 0.95, 1.0) * 0.9, tear2 * melt * 0.8);
    }
  }

  // --- volumetric fog ---------------------------------------------------------
  float fogD = 1.0 - exp(-depth * (0.06 + 0.1 * Null.uIntensity + 0.05 * Null.uBass));
  vec3 fogCol = palVoid(musicHue(0.1) + uMode * 0.15) * (0.15 + Null.uIntensity * 0.25) + vec3(0.3, 0.2, 0.5) * 0.05;
  col = mix(col, fogCol, sat01(fogD));

  // Depth fade
  col *= exp(-max(depth - 70.0, 0.0) * 0.15);

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}