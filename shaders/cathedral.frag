#version 300 es
// ---------------------------------------------------------------------------
// SCENE 2 - The Data Cathedral
// An enormous procedural structure of glowing lines and floating panels:
// infinite pillars on a grid, procedural arches, floating circuitry slabs,
// soft volumetric fog. The camera glides down the nave while the structure
// builds itself - pillars rise, arches lift and panels materialize, staggered
// per cell, and kick hits snap pieces forward ("as the beat intensifies, the
// cathedral assembles").
//
// Materials: 0 floor grid, 1 pillar, 2 arch, 3 floating panel.
// ---------------------------------------------------------------------------
#include <common>

// camera + music + timeline state comes from the shared NullBlock (common.glsl)
uniform float uFlash;  // 0..1 per-kick strobe (audio kick analyser)
uniform float uMode;   // 0 = assembly (section 2), 1 = deconstruction (reprise)

out vec4 fragColor;

const float SP = 7.0;          // pillar grid spacing (world units)
const float PILLAR_R = 0.24;   // pillar radius
const float ARCH_TUBE = 0.10;  // arch tube radius
const float PILLAR_MAX = 60.0; // fully-built pillar height (beyond the fog)

// --- sdf primitives ----------------------------------------------------------
float sdBox(vec3 p, vec3 b) {
  vec3 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
// capped cylinder (top cap at y=h, open bottom - the floor SDF hides it below 0)
float sdCapCyl(vec3 p, float r, float h) {
  vec2 d = vec2(length(p.xz) - r, p.y - h);
  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
// torus in a vertical plane: axis 0 = along z (arch spans x), 1 = along x (arch spans z)
float sdArchTorus(vec3 p, float R, float r, float axis) {
  vec2 q = axis < 0.5
    ? vec2(length(vec2(p.x, p.y)) - R, p.z)
    : vec2(length(vec2(p.z, p.y)) - R, p.x);
  return length(q) - r;
}

// --- cathedral state -----------------------------------------------------------
vec2 cellId(vec3 p) { return floor(p.xz / SP); }
vec2 cellCenter(vec2 id) { return id * SP + SP * 0.5; }

/** assembly envelope 0..1: how much of the cathedral is built. Driven by
 *  Null.uAssembly — a one-way per-kick ratchet computed in ubo.cpp from the
 *  audio analyser's kick detector. Each real kick permanently snaps the
 *  pillars/arches forward by ~0.055 (~18 kicks to full assembly). Materials
 *  still slam with uFlash/kick, but the structure itself ratchets forward
 *  and never slides back.
 *
 *  In deconstruction mode (uMode = 1 - the deconstruction section, and the
 *  dormant reprise) uAssembly is seeded to 1 on section entry and decays in
 *  ubo.cpp; buildCur tracks it DOWN so the cathedral visibly falls as the
 *  ghost undoes it. sqrt holds the structure up through the early decay,
 *  then it snaps down hard as assembly bottoms out. */
float buildCur() {
  if (uMode > 0.5) {
    return sqrt(sat01(Null.uAssembly));
  }
  return sat01(Null.uAssembly);
}

float pillarHeight(vec2 id, float build) {
  return PILLAR_MAX * sat01(build * 1.6 - hash21(id) * 0.5);
}

/** the single floating panel of a cell: returns its SDF distance */
float cellPanel(vec3 p, vec2 id, float build) {
  float h2 = hash12(id + 7.3);
  float grow = sat01(build * 2.6 - fract(h2 * 3.0) * 1.3);
  float ph = 5.0 + 9.0 * fract(hash21(id) * 7.7);
  vec2 po = (hash12(id + 11.0) - 0.5) * vec2(SP * 0.6, SP * 0.6);
  float ang = h2 * 6.2831853;
  vec2 lp = rotate2(vec2(p.x, p.z) - (cellCenter(id) + po), -ang);
  vec3 lc = vec3(lp.x, p.y - ph, lp.y);
  // scale-in materialize; never fully zero so the SDF stays continuous
  vec3 half = vec3(0.05, 0.55, 0.9) * (0.001 + 0.999 * grow);
  return sdBox(lc, half);
}

// --- scene ---------------------------------------------------------------------
float map(vec3 p, out float matID) {
  vec2 id = cellId(p);
  vec2 cc = cellCenter(id);
  float h1 = hash21(id);
  float h2 = hash12(id + 7.3);
  float build = buildCur();

  // floor (dark grid plane grounding the nave)
  float d = p.y;
  matID = 0.0;

  // pillar: capped cylinder rising with the assembly envelope
  float ph = pillarHeight(id, build);
  float pillar = sdCapCyl(p - vec3(cc.x, 0.0, cc.y), PILLAR_R, ph);
  if (pillar < d) { d = pillar; matID = 1.0; }

  // arches: doorway arcs to the +x and +z neighbours, lifting off the floor
  float archH = 9.0 + 3.0 * h2;
  float archY = archH * sat01(build * 2.2 - h1 * 0.7);
  vec3 ax = p - vec3(cc.x + SP * 0.5, archY, cc.y);
  float archX = max(sdArchTorus(ax, SP * 0.5, ARCH_TUBE, 0.0), archY - p.y);
  vec3 az = p - vec3(cc.x, archY, cc.y + SP * 0.5);
  float archZ = max(sdArchTorus(az, SP * 0.5, ARCH_TUBE, 1.0), archY - p.y);
  if (archX < d) { d = archX; matID = 2.0; }
  if (archZ < d) { d = archZ; matID = 2.0; }

  // floating circuitry panel
  float panel = cellPanel(p, id, build);
  if (panel < d) { d = panel; matID = 3.0; }

  return d;
}

float mapD(vec3 p) { float id; return map(p, id); }

vec3 calcN(vec3 p) {
  vec2 e = vec2(0.0012, 0.0);
  return normalize(vec3(
    mapD(p + e.xyy) - mapD(p - e.xyy),
    mapD(p + e.yxy) - mapD(p - e.yxy),
    mapD(p + e.yyx) - mapD(p - e.yyx)));
}

// --- shading --------------------------------------------------------------------
vec3 shadeFloor(vec3 p) {
  vec2 g = abs(fract(p.xz) - 0.5);
  float grid = smoothstep(0.46, 0.5, max(g.x, g.y));
  vec3 col = vec3(0.016, 0.02, 0.045);
  col += vec3(0.15, 0.45, 0.95) * grid * (0.22 + 0.45 * Null.uPulse);
  col += vec3(1.0, 1.0, 1.0) * grid * 0.04;
  return col;
}

vec3 shadePillar(vec3 p, vec3 n, float fres) {
  vec2 id = cellId(p);
  vec2 cc = cellCenter(id);
  float h1 = hash21(id);
  float build = buildCur();
  float ph = pillarHeight(id, build);
  vec3 pp = p - vec3(cc.x, 0.0, cc.y);
  float a = atan(pp.z, pp.x);

  vec3 col = vec3(0.03, 0.04, 0.09) * (0.55 + 0.45 * fres);

  // vertical emissive strips around the column
  float strip = 0.0;
  for (int k = 0; k < 4; k++) {
    float phase = h1 * 6.2831853 + float(k) * 1.5707963;
    strip += smoothstep(0.30, 0.06, abs(cos(a - phase)));
  }
  vec3 stripCol = palVoid(musicHue(0.12)) * (0.6 + 0.6 * Null.uPulse);
  col += stripCol * strip * (0.7 + 0.6 * fres);

  // horizontal data rings
  float ring = smoothstep(0.05, 0.0, abs(fract(pp.y * 0.6 + h1) - 0.5));
  col += vec3(0.5, 0.8, 1.0) * ring * 0.32 * (0.6 + 0.5 * Null.uPulse);

  // a bright data packet climbing the front meridian
  float dotY = fract(pp.y * 1.2 - Null.uTime * 1.2 + h1 * 7.0);
  float dotA = abs(cos(a - h1 * 6.2831853));
  float dot = smoothstep(0.14, 0.0, dotA) * smoothstep(0.10, 0.0, abs(dotY - 0.35));
  col += vec3(1.0, 1.0, 1.0) * dot * (0.5 + 0.6 * Null.uPulse);

  // assembling tip glow (the growing head of the pillar; fades with the
  // height so a deconstructed stub doesn't leave a bright ground dot)
  col += vec3(1.0, 0.9, 1.0) * smoothstep(2.2, 0.0, abs(pp.y - ph)) * 1.3 * sat01(ph * 0.08);
  // base ring where it meets the floor
  col += vec3(0.4, 0.7, 1.0) * smoothstep(0.4, 0.0, pp.y) * 0.15;

  // faint key light from above
  col += vec3(0.2, 0.3, 0.55) * max(n.y, 0.0) * 0.22;
  return col;
}

vec3 shadeArch(vec3 p, float fres) {
  vec2 id = cellId(p);
  vec2 cc = cellCenter(id);
  float h1 = hash21(id);
  float h2 = hash12(id + 7.3);
  float build = buildCur();
  float archBuild = sat01(build * 2.2 - h1 * 0.7);
  float archY = (9.0 + 3.0 * h2) * archBuild;

  // pick the nearer arch (x-spanning or z-spanning)
  vec3 ax = p - vec3(cc.x + SP * 0.5, archY, cc.y);
  float dx = abs(length(vec2(ax.x, ax.y)) - SP * 0.5);
  vec3 az = p - vec3(cc.x, archY, cc.y + SP * 0.5);
  float dz = abs(length(vec2(az.z, az.y)) - SP * 0.5);
  bool isX = dx < dz;
  vec3 ap = isX ? ax : az;
  float a = isX ? atan(ap.y, ap.x) : atan(ap.y, ap.z);
  // symmetric foot->apex->foot gradient (0 at both feet, 1 at the apex)
  float u = clamp(sin(a), 0.0, 1.0);

  vec3 col = vec3(0.035, 0.045, 0.09) * (0.5 + 0.5 * fres);
  vec3 archCol = mix(palVoid(musicHue(0.25)), vec3(1.0, 0.95, 1.0), 0.55);
  // emissive gradient: brightest at the apex, fading down both legs
  col += archCol * (0.35 + 0.7 * pow(u, 1.5)) * (0.05 + 0.95 * archBuild) * (0.7 + 0.5 * Null.uPulse);
  // feet glow where the arch meets the pillars
  col += vec3(1.0, 1.0, 1.0) * smoothstep(0.15, 0.0, u) * 0.7 * archBuild;
  // fresnel rim
  col += archCol * fres * 0.6;
  return col;
}

vec3 shadePanel(vec3 p, float fres) {
  vec2 id = cellId(p);
  float h2 = hash12(id + 7.3);
  float build = buildCur();
  float grow = sat01(build * 2.6 - fract(h2 * 3.0) * 1.3);
  float ph = 5.0 + 9.0 * fract(hash21(id) * 7.7);
  vec2 po = (hash12(id + 11.0) - 0.5) * vec2(SP * 0.6, SP * 0.6);
  float ang = h2 * 6.2831853;
  vec2 lp = rotate2(vec2(p.x, p.z) - (cellCenter(id) + po), -ang);
  // face UVs (-1..1 over the wide faces)
  vec2 uvc = vec2((p.y - ph) / 0.55, lp.y / 0.9);

  vec3 col = vec3(0.02, 0.04, 0.09) * (0.5 + 0.5 * fres);
  // edge frame
  float edge = smoothstep(0.82, 0.97, max(abs(uvc.x), abs(uvc.y)));
  col += vec3(0.2, 0.75, 1.0) * edge * (0.4 + 0.7 * grow) * (0.6 + 0.5 * Null.uPulse);
  // vertical circuit traces
  float tr = 0.0;
  for (int k = 0; k < 3; k++) {
    float tp = (hash12(id * 0.7 + vec2(float(k) * 3.1)) - 0.5) * 1.5;
    tr += smoothstep(0.045, 0.0, abs(uvc.x - tp));
  }
  col += palVoid(musicHue(0.4)) * tr * (0.5 + 0.5 * grow) * (0.7 + 0.6 * Null.uPulse);
  // component pads
  float pd = 0.0;
  for (int k = 0; k < 4; k++) {
    vec2 pp2 = (hash12(id * 1.3 + vec2(float(k) * 3.7, float(k) * 5.3)) - 0.5) * vec2(1.5, 1.7);
    pd += smoothstep(0.11, 0.0, length(uvc - pp2));
  }
  col += vec3(1.0, 0.95, 1.0) * pd * (0.3 + 0.5 * grow);
  // scrolling data line across the face
  float dat = smoothstep(0.06, 0.0, abs(fract(uvc.y * 1.5 - Null.uTime * 0.9) - 0.5));
  col += vec3(0.5, 1.0, 1.0) * dat * (0.4 + 0.5 * grow) * (0.5 + 0.6 * Null.uPulse);
  // kick strobe on the face
  col += vec3(1.0) * uFlash * 0.4 * grow;
  // materialize
  col *= 0.001 + 0.999 * grow;
  return col;
}

// --- volumetric light shafts + drifting dust -----------------------------------
/** Cheap single-pass volumetric pass integrated along the ray between the
 *  camera and the nearest surface (or a far clip). Brightness concentrates
 *  near the nave axis - the rose-window beams - and pulses with the bass
 *  analyser + kick; sparse hash-field dust motes drift through the fog.
 *  Additive, so the post bloom spreads the glow. */
vec3 volumetric(vec3 ro, vec3 rd, float tmax) {
  vec3 fwd = normalize(vec3(0.0, 1.4, -1.0));   // nave axis (matches background)
  float bass = 0.5 + 1.0 * Null.uBass + 0.5 * uFlash + 0.4 * Null.uPulse;
  vec3 shaftCol = palVoid(musicHue(0.2)) * 1.2 + vec3(1.0, 0.97, 1.0) * 0.7;
  shaftCol = mix(shaftCol, vec3(1.0, 0.55, 0.4), uMode * 0.45);   // reprise: warmer shafts

  float span = max(tmax, 1.0);
  float stepL = span / 12.0;
  float norm = stepL / span;       // = 1/12: mean density (not a raw sum) over the span
  vec3 acc = vec3(0.0);
  float t = 0.4;                   // skip the near field so motes never flash at t=0
  for (int i = 0; i < 12; i++) {
    if (t >= tmax) break;          // never sample past the nearest surface
    vec3 p = ro + rd * t;
    float dAxis = length(cross(p, fwd));          // perpendicular distance to the axis
    float shaft = exp(-dAxis * dAxis * 0.07) * sat01(t * 0.05) * bass;
    acc += shaftCol * shaft * norm;

    // dust motes: two sparse hash layers (coarse + fine). In the reprise the
    // dust becomes rising embers - warmer, denser, drifting upward.
    vec3 drift = uMode > 0.5
      ? vec3(Null.uTime * 0.10, -Null.uTime * 0.24, Null.uTime * 0.06)
      : vec3(Null.uTime * 0.10, Null.uTime * 0.07, Null.uTime * 0.09);
    float mote = step(uMode > 0.5 ? 0.93 : 0.955, hash13(floor(p * 0.30 + drift)));
    float mote2 = step(0.99, hash13(floor(p * 0.90 - drift * 1.4)));
    // near-field fade so a mote never flashes right in front of the lens
    float mB = exp(-dAxis * dAxis * 0.06) * exp(-t * 0.05) * sat01((t - 0.4) * 1.5);
    vec3 moteCol = uMode > 0.5 ? vec3(1.0, 0.45, 0.25) : vec3(1.0, 0.98, 1.0);
    acc += moteCol * (mote * 1.4 + mote2 * 2.2) * mB * (0.5 + 1.0 * bass);

    t += stepL;
  }
  return acc;
}

// --- main -----------------------------------------------------------------------
void main() {
  vec2 uv = (gl_FragCoord.xy * 2.0 - Null.uRes) / Null.uRes.y;

  // --- ghost corruption ------------------------------------------------------
  // The ghost's grip on the cathedral: slice tears + RGB split ride the
  // SHARED glitch model (glitchSlice/glitchParticipation in common.glsl), so
  // the cathedral seizes exactly like the neural net, the logo sub-title and
  // the reprise tunnel. In assembly mode (scene 2) the corruption builds
  // through the section (pristine opening, full seizure by the end); in
  // deconstruction mode (reprise) it follows the collapse itself.
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  // mode 1 (deconstruction) ties corruption to the collapse: buildCur falls as
  // assembly decays, so ghostT rises as the ghost undoes the cathedral. NOTE:
  // the current schedule renders the reprise via the quantum tunnel (uMode
  // stays 0 here), so this branch is dormant until the cathedral is wired to
  // a deconstruction section.
  float ghostT = uMode > 0.5 ? (1.0 - buildCur()) : smoothstep(0.35, 0.90, secT);
  // (deconstruction: ghostT rides 1-buildCur up as the cathedral falls, so
  // the corruption crests exactly when the last arch collapses)
  float kickE = uFlash;
  float bassE = Null.uBass;
  float burstScale = ghostT;   // downbeats hit harder as the section peaks

  // whole horizontal bands of the VIEW slice sideways on kick/bass/downbeats
  // (shared glitchSlice, scaled to uv units - heavier than the neural net's
  // 0.12, lighter than the tunnel's 0.18, so the architecture reads clearly)
  vec2 gs = glitchSlice(gl_FragCoord.y / Null.uRes.y, 32.0, ghostT, kickE, bassE, 17.3, burstScale);
  uv.x += gs.x * 0.15;

  vec3 rd = normalize(Null.uCamRot * vec3(uv * Null.uFovTan, -1.0));
  vec3 ro = Null.uCamPos;

  // --- background: deep fog + the luminous rose window down the nave -----------
  vec3 fwd = normalize(vec3(0.0, 1.4, -1.0));
  float apse = pow(max(dot(rd, fwd), 0.0), 7.0);
  vec3 bg = vec3(0.012, 0.016, 0.06);
  vec3 apseCol = palVoid(musicHue(0.2)) * 1.3 + vec3(1.0, 0.97, 1.0) * 0.5;
  bg += apseCol * apse * 1.2;
  bg += vec3(0.6, 0.7, 1.0) * pow(apse, 2.0) * 1.6;   // tight window core
  float hz = 0.5 + 0.5 * sin(Null.uTime * 0.4 + dot(rd, fwd) * 5.0);
  bg += palVoid(musicHue()) * hz * 0.06;              // drifting haze

  // --- march ----------------------------------------------------------------------
  // t starts slightly forward so a camera that begins inside a thin slab (a
  // floating panel at eye level, or a low arch mid-assembly) doesn't trigger a
  // degenerate t=0 interior hit - the guard pushes past embedded geometry.
  float t = 0.03;
  vec3 p = ro;
  float matID = 0.0;
  float hit = 0.0;
  for (int i = 0; i < 72; i++) {
    p = ro + rd * t;
    float d = map(p, matID);
    if (d < 0.0015 * t) {
      if (d < 0.0) { t += 0.04; continue; }  // still inside geometry: keep marching out
      hit = 1.0; break;
    }
    t += d * 0.9;
    if (t > 60.0) break;
  }

  if (hit < 0.5) {
    bg += volumetric(ro, rd, 46.0);   // shafts + dust fill the open nave

    // the ghost tears through the nave fog too - a thin ghost-hue glow on the
    // empty space, widening with the hits (shared model; the full chromatic
    // split lives on the surfaces below)
    vec2 gsv = glitchSlice(gl_FragCoord.y / Null.uRes.y, 24.0, ghostT, kickE, bassE, 31.7, burstScale);
    if (abs(gsv.x) > 0.004) {
      bg += palVoid(musicHue(0.4) + gsv.y * 2.0) * (0.05 + gsv.y * 0.6);
    }

    fragColor = vec4(bg, 1.0);
    gl_FragDepth = 1.0;
    return;
  }

  vec3 n = calcN(p);
  vec3 V = normalize(ro - p);
  float fres = pow(1.0 - max(dot(n, V), 0.0), 2.5);

  vec3 col;
  if (matID < 0.5) col = shadeFloor(p);
  else if (matID < 1.5) col = shadePillar(p, n, fres);
  else if (matID < 2.5) col = shadeArch(p, fres);
  else col = shadePanel(p, fres);

  // soft volumetric fog toward the nave background
  float fog = 1.0 - exp(-t * 0.035);
  col = mix(col, bg, fog);
  // god-ray haze along the nave (strongest near the rose window)
  col += vec3(0.4, 0.55, 1.0) * apse * exp(-t * 0.03) * 0.5 * (0.5 + 0.5 * sin(Null.uTime * 0.6));

  // music + kick: the whole cathedral slams on the drum
  col *= 1.0 + uFlash * 0.45 + Null.uPulse * 0.18 + Null.uBass * 0.25;
  col += vec3(1.0, 0.97, 1.0) * uFlash * 0.35;

  // volumetric light shafts + drifting dust between the camera and the surface
  col += volumetric(ro, rd, t);

  // --- ghost tear: chromatic smear across the architecture ---------------------
  // Depth-banded, shared glitchSlice - R/B channels separate around the
  // centre hue, widening with the hits, exactly like the neural net and the
  // tunnel. The blend rides the hits too, so quiet bars stay clean and kicks
  // bite through.
  vec2 gs2 = glitchSlice(t * 0.5, 2.0, ghostT, kickE, bassE, 7.3, burstScale);
  if (abs(gs2.x) > 0.002) {
    float hue = musicHue(0.3) + hash12(vec2(floor(p.x * 4.0), floor(Null.uTime * 12.0))) * 0.4;
    float chroma = 0.02 + gs2.y * 0.6;
    vec3 tearCol = vec3(palVoid(hue + chroma).r, palVoid(hue).g, palVoid(hue - chroma).b);
    float bite = 0.25 + 0.35 * kickE + 0.2 * bassE;
    col = mix(col, tearCol * 1.3, bite);
    col += tearCol * (0.2 + 0.4 * kickE + 0.25 * bassE);
  }

  // --- reprise mode: the ghost corrupts the failing cathedral ------------------
  if (uMode > 0.5) {
    float ghost = 1.0 - buildCur();   // 0 fresh -> 1 fully deconstructed
    // hue tears toward magenta as the structure fails
    col = mix(col, col * vec3(1.2, 0.55, 0.75) + vec3(0.7, 0.08, 0.25) * 0.35, ghost * 0.55);
    // glitch flicker: random darkening pulses, harder as it falls apart
    float flick = hash13(vec3(gl_FragCoord.xy * 0.11, floor(Null.uTime * 14.0) * 0.13));
    col *= mix(1.0, 0.35, step(1.0 - ghost * 0.6, flick) * ghost);
  }

  float viewZ = -dot(p - ro, -Null.uCamRot[2]);
  float d01 = depthFromViewZ(viewZ);
  gl_FragDepth = d01;

  fragColor = vec4(col, d01);
}
