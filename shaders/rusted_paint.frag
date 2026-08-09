#version 300 es
// ---------------------------------------------------------------------------
// SCENE 4 - Rusted Paint Tessellation (raymarch)
// Port of the Shadertoy demo of the same name. A floor of tessellated random
// polygons is extruded into "pylons" whose height ripples with time. Each
// pylon gets a procedural rusted-paint material: paint patches, peeled bare
// metal, rust creep around the edges and thresholded noise scratches, shaded
// with a cheap PBR-ish BRDF, soft shadows, AO, curvature + bump mapping.
//
// Engine notes: time/resolution come from the shared NullBlock (common.glsl)
// instead of iTime/iResolution; the original's iChannel0 environment texture
// is replaced by a procedural env map; the final gamma step is dropped (the
// engine's HDR post pipeline tonemaps). Depth is packed in alpha like the
// other raymarchers.
// ---------------------------------------------------------------------------
#include <common>

out vec4 fragColor;

const float FAR = 20.0;

// --- hashes (IQ style) -------------------------------------------------------
float hash22(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float hash31(vec3 p3) {
  p3 = fract(p3 * 0.1031);
  p3 += dot(p3, p3.zyx + 31.32);
  return fract((p3.x + p3.y) * p3.z);
}
mat2 rot2(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }
float cross2(vec2 u, vec2 v) { return u.x * v.y - u.y * v.x; }

// IQ's polynomial smooth max
float smax(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(a, b, h) + k * h * (1.0 - h);
}

// --- parameterized fbm (reuses the engine's 3D value noise) ------------------
float fBm(vec3 p, float lac, float gain, int oct) {
  float v = 0.0;
  float a = 1.0;
  for (int i = 0; i < oct; i++) {
    v += a * vnoise3(p);
    p *= lac;
    a *= gain;
  }
  return v;
}

// --- polygon tessellation ------------------------------------------------------
// Every unit cell of the XY plane holds a random convex polygon (the pylon
// footprint). distField() returns the 2D SDF of all polygons (nearest over a
// 3x3 neighborhood) plus the winning cell id. The cell the ray currently
// sits in has its vertices cached in vP[] so the raymarch can delimit each
// step at the polygon walls (artifact-free traversal of the towering pylons).
vec2 gP;     // ray position in the tessellation plane (set by distField)
vec2 vP[8];  // polygon vertices of the cell containing the ray
int pID;     // vertex count (4..7)
float gSc = 1.0;  // tessellation scale

vec2 svRd;   // ray direction projected onto the plane
vec3 gRd;    // full ray direction (for the plane projection)
float gCD;   // distance to the nearest polygon wall along the ray
vec4 svVal;  // distField result: distance, cell id.xy, pylon height
vec4 vObj;   // per-object distances: floor, pylon, frame, unused

// the i-th vertex of the polygon in cell c (n vertices)
vec2 polyVert(vec2 c, int n, int i) {
  float a = (float(i) + 0.5) / float(n) * TAU + (hash22(c + vec2(float(i) * 7.13, 1.37)) - 0.5) * 0.7;
  float r = mix(0.30, 0.48, hash22(c + vec2(float(i) * 3.71, 2.17)));
  return c + vec2(0.5) + vec2(cos(a), sin(a)) * r;
}

// signed distance to the (always convex) polygon in cell c.
// Interior: min perpendicular distance to the edge lines. Exterior: min
// distance to the clamped edge segments. Both are exact for convex polygons,
// and this avoids the winding-number parity test (and its branches) that the
// general polygon SDF needs. Vertices are cached in a local array so each
// edge shares the two trig evaluations.
float polyDistAt(vec2 p, vec2 c) {
  float rnd = hash22(c + vec2(0.913, 0.274));
  int n = 4 + int(rnd * 4.0);
  vec2 vv[8];
  for (int i = 0; i < n; i++) vv[i] = polyVert(c, n, i);
  float d = 1e5;  // min squared distance to the edge segments
  float s = 1e5;  // min inward signed edge distance (>0 = strictly inside)
  for (int i = 0; i < n; i++) {
    vec2 a = vv[i];
    vec2 b = vv[i + 1 < n ? i + 1 : 0];
    vec2 e = b - a;
    float len = sqrt(dot(e, e));
    vec2 w = p - a;
    float h = clamp(dot(w, e) / dot(e, e), 0.0, 1.0);
    vec2 b2 = w - e * h;
    d = min(d, dot(b2, b2));
    s = min(s, cross2(e, w) / len);
  }
  return s >= 0.0 ? -s : sqrt(d);
}

// cache the polygon vertices of the cell containing gP
void precalc() {
  vec2 c = floor(gP);
  float rnd = hash22(c + vec2(0.913, 0.274));
  pID = 4 + int(rnd * 4.0);
  for (int i = 0; i < 8; i++) {
    if (i >= pID) break;
    vP[i] = polyVert(c, pID, i);
  }
}

// 2D distance field of the whole tessellation + nearest cell id.
// Each polygon stays within a radius ~0.48 of its cell center, so a cell can
// only beat the running minimum if the point is within ~md of its center -
// cells that can't win are pruned without touching their vertices (this is
// exact, not an approximation: |dist to polygon| >= |dist to center| - r).
vec4 distField(vec2 p) {
  vec2 ip = floor(p);
  float md = 1e5;
  vec2 id = vec2(0.0);
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
      vec2 c = ip + vec2(float(x), float(y));
      vec2 dc = p - (c + vec2(0.5));
      float dc2 = dot(dc, dc);
      if (dc2 > (md + 0.5) * (md + 0.5)) continue;  // cannot beat the best
      float d = polyDistAt(p, c);
      if (d < md) { md = d; id = c; }
    }
  // cache the current cell's polygon for wall-distance step delimiting
  gP = p;
  precalc();
  return vec4(md, id, 0.0);
}

// distance along the ray to the infinite wall line through point a, direction b
float rayLine(vec2 ro, vec2 rd, vec2 a, vec2 b) {
  return cross2(a - ro, b) / cross2(rd, b);
}

// --- height function (pylon heights, animated with time) ----------------------
float hf(vec2 p) {
  p *= 3.0;
  return dot(sin(p - cos(p.yx * 1.5) * 1.5 + Null.uTime), vec2(0.25)) + 0.5;
}

// IQ's extrusion formula (2D sdf -> 3D prism, slightly rounded)
float opExtrusion(in float sdf, in float pz, in float h, in float sf) {
  vec2 w = vec2(sdf, abs(pz) - h) + sf;
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0)) - sf;
}

float map(vec3 p) {
  // floor plane
  float fl = -p.z + 2.0;

  // coordinate rotation is used in the distance field, so the ray direction
  // needs to be projected to match
  svRd = gRd.xy;

  // the 2D tessellation pattern
  vec4 d4 = distField(p.xy);
  svVal = d4;
  float d2 = d4.x;
  d2 += 0.008;

  // the individual pylon heights
  float h = hf(svVal.yz * gSc);
  h = h * 0.45 + 0.05;
  svVal.w = h;

  // extruded polygon pylons with metallic polygon-edge frames
  float d = opExtrusion(d2, p.z + h / 2.0, h / 2.0, 0.01);
  d += (d2 + 0.03) * 0.25;

  float th = 0.015;
  float bord = abs(d2 + 0.0125) - 0.0125;
  float frame = opExtrusion(bord, p.z + h - th + 0.01, th, 0.008);

  d = smax(d, -(frame - 0.0015), 0.01);

  // minimum cell wall distance: used as a ray jump delimiter so the towering
  // geometry can never be skipped over (artifact-free traversal)
  float rC = 1e5;
  for (int j = 0, i = pID - 1; j < pID; i = j, j++) {
    float rCI = rayLine(gP, svRd, vP[i], normalize(vP[i] - vP[j]).yx * vec2(1.0, -1.0));
    rC = min(rC, rCI);
  }
  gCD = max(rC, 0.0) + 0.0001;

  // object distances for sorting later
  vObj = vec4(fl, d, frame, 1e5);

  // the minimum scene distance
  return min(fl, min(d, frame));
}

// standard raymarching function (steps are delimited by the polygon walls)
float trace(in vec3 ro, in vec3 rd) {
  gCD = 1e5;
  gRd = rd;

  // start the march just above the camera plane for a bit of extra speed
  float t = (-ro.z - 0.6) / rd.z, d;

  for (int i = 0; i < 40; i++) {
    d = map(ro + rd * t);
    if (abs(d) < 0.001 || t > FAR) break;
    t += min(d * 0.8, gCD);
  }
  return min(t, FAR);
}

// cheap soft shadows (distance-averaged)
float softShadow(vec3 ro, vec3 rd, vec3 n, float lDist, float k) {
  ro += n * 0.0015;
  float shade = 1.0;
  float t = 0.0;

  // a touch of jitter to alleviate banding
  ro += rd * hash31(ro + n * 57.13) * 0.01;

  // ray direction for the wall-distance culling inside map()
  gRd = rd;

  // distance to the floor plane (from the original)
  lDist = (-ro.z - 0.6) / rd.z;

  for (int i = 0; i < 20; i++) {
    float d = map(ro + rd * t);
    shade = min(shade, k * d / t);
    if (d < 0.0 || t > lDist) break;
    t += clamp(min(d, gCD), 0.01, 0.2);
  }
  return max(shade, 0.0);
}

// normal via a 4-tap tetrahedral gradient (cheaper than the symmetric
// six-tap version, visually equivalent for this scene)
vec3 normal(in vec3 p) {
  const float e = 0.0017;
  float d0 = map(p);
  vec3 n = vec3(
    map(p + vec3(e, -e, -e)),
    map(p + vec3(-e, e, -e)),
    map(p + vec3(-e, -e, e)));
  return normalize(n - vec3(d0));
}

// approximate self-occlusion around a surface point
float calcAO(in vec3 p, in vec3 n) {
  float sca = 2.0, occ = 0.0;
  for (int i = 0; i < 4; i++) {
    float hr = 0.003 + float(i) * 0.2 / 4.0;
    float d = map(p + n * hr);
    occ += (hr - d) * sca;
    sca *= 0.7;
  }
  return clamp(1.0 - occ, 0.0, 1.0);
}

// --- bump mapping ------------------------------------------------------------
float gHgt;  // pylon height of the hit cell (for local texture coords)
vec2 gID;    // hit cell id

float bumpSurf3D(in vec3 p, in vec3 n) {
  vec3 oTxP = p - vec3(0.0, 0.0, -gHgt);
  vec3 txP = oTxP * 5.0;
  txP.xy += hash22(gID + 0.4);
  float ns = fBm(txP, 2.5, 0.5, 3);

  txP = oTxP * 128.0;
  float rustNoise = fBm(txP, 2.0, 0.5, 3);

  // noise mixing mask: paint pitting where the paint is peeled
  float nsMask = ns - 0.45 - (rustNoise - 0.5) * 0.05;

  return -(smoothstep(0.0, 0.01, -nsMask)) * (rustNoise * 0.25 + 0.75);
}

// standard four-tap function-based bump mapping
vec3 doBumpMap(in vec3 p, in vec3 n, float bumpfactor) {
  const vec2 e = vec2(0.001, 0.0);
  vec3 v0 = e.xyy;
  vec3 v1 = e.yxy;
  vec3 v2 = e.yyx;

  mat4x3 p4 = mat4x3(p, p - v0, p - v1, p - v2);

  vec4 b4;
  for (int i = 0; i < 4; i++) {
    b4[i] = bumpSurf3D(p4[i], n);
    if (n.x > 1e5) break;  // fake break to keep the compiler honest
  }

  // gradient vector: vec3(df/dx, df/dy, df/dz)
  vec3 grad = (b4.yzw - b4.x) / e.x;

  // Gram-Schmidt: make the gradient perpendicular to the normal
  grad -= n * dot(n, grad);

  return normalize(n + grad * bumpfactor);
}

// --- curvature (discrete Laplacian approximation) ----------------------------
float curve(in vec3 p, vec3 n, in float spr, in float amp) {
  spr /= 450.0;

  vec3 an = (abs(n.x) < 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
  vec3 t1 = normalize(cross(an, n));
  vec3 t2 = cross(n, t1);

  float d = -map(p) * 4.0;
  for (int i = 0; i < 4; i++) {
    if (i == 2) t1 = t2;
    d += map(p + t1 * spr);
    spr = -spr;
  }

  return clamp(d / spr / spr * amp / 16.0 + 0.5, 0.0, 1.0);
}

// --- BRDF helpers (from the original demo's library) --------------------------
vec3 getSpec(vec3 FS, float nh, float nr, float nl, float rough) {
  // distribution term (GGX-ish)
  float den = 1.0 - nh * nh * (1.0 - rough * rough);
  float ggx = rough * rough / (den * den);
  // geometric / visibility term
  float gv = nr * nl;
  float gs = gv / (1.0 - gv + 1e-4);
  // Fresnel-weighted specular
  return FS * (ggx * gs) * (1.0 + rough);
}
vec3 getDiff(vec3 FS, float nl, float rough, float type) {
  // metals carry no diffuse
  float df = nl * (1.0 - rough);
  vec3 diff = vec3(df) - FS * df;
  diff *= (1.0 - type);
  return diff;
}

// --- procedural environment (replaces the original's iChannel0 texture) -------
vec3 envMap(vec3 d) {
  vec3 c = mix(vec3(0.30, 0.32, 0.40), vec3(0.62, 0.68, 0.78), clamp(d.y * 0.5 + 0.5, 0.0, 1.0));
  // warm sun
  float sun = pow(max(dot(d, normalize(vec3(-0.35, 0.8, 0.25))), 0.0), 24.0);
  c += vec3(1.0, 0.9, 0.7) * sun * 1.6;
  // horizon glow
  c += vec3(1.0, 0.8, 0.6) * exp(-abs(d.y) * 12.0) * 0.5;
  // large soft studio panel so the chrome reads
  float panel = exp(-pow((d.y - 0.25) * 3.2, 2.0));
  c += vec3(0.95, 0.97, 1.0) * panel * 0.9;
  // cool floor bounce
  c += vec3(0.25, 0.3, 0.4) * pow(max(-d.y, 0.0), 2.0) * 0.6;
  return c;
}

void main() {
  // aspect-corrected coordinates
  vec2 uv = (gl_FragCoord.xy - Null.uRes * 0.5) / Null.uRes.y;

  // camera: a fixed rig with a slow drift (from the original)
  vec3 rd = normalize(vec3(uv, 1.0));
  rd.xy *= rot2(PI / 8.0);
  rd.xz *= rot2(PI / 12.0);
  rd.yz *= rot2(PI / 6.0);
  vec3 ro = vec3(0.0, 0.0, -1.25);
  ro.xy += vec2(0.0, 1.0) / 16.0 * Null.uTime;

  // precalculate the polygon vertices (warm-up; refreshed per cell in map())
  precalc();

  // raymarch
  float t = trace(ro, rd);

  int objID = vObj.x < vObj.y && vObj.x < vObj.z ? 0 : vObj.y < vObj.z ? 1 : 2;

  // 2D distance, cell id and height
  float d = svVal.x;
  vec2 id = svVal.yz;
  float hgt = svVal.w;

  // global pylon height/id for the bump mapping
  gHgt = hgt;
  gID = id;

  vec3 col = vec3(0.0);

  if (t < FAR) {
    // hit position + normal, then bump mapping
    vec3 sp = ro + rd * t;
    vec3 sn = normal(sp);
    sn = doBumpMap(sp, sn, 0.005);

    // point light
    vec3 lp = ro + vec3(0.25, 1.0, 0.0);
    vec3 ld = lp - sp;
    float lDist = length(ld);
    ld /= max(lDist, 1e-5);

    float atten = 1.0 / (1.0 + lDist * lDist * 0.25);

    // shadow + ambient occlusion + curvature
    float sh = softShadow(sp, ld, sn, lDist, 16.0);
    float ao = calcAO(sp, sn);
    float crv = curve(sp, sn, 8.0, 1.0 / 1.5);

    // randomly colored pylons
    float rnd = hash22(id + 0.11);
    float sat = hash22(id + 0.31);
    if (hash22(id + 0.52) < 0.25) sat = 1.0;
    vec3 oCol = 0.5 + 0.45 * cos(TAU * rnd / 6.0 + vec3(0.0, PI / 2.0, PI) * sat);

    // darker frames
    if (objID == 2) oCol = mix(oCol, vec3(0.25), 0.9);

    // texture coordinates move with the pylons
    vec3 txP = sp - vec3(0.0, 0.0, -hgt);
    vec3 oTxP = txP;

    // base noise for the paint blotches
    txP = oTxP * 5.0;
    txP.xy += hash22(id + 0.4);
    float ns = fBm(txP, 2.5, 0.5, 3);

    // rust noise
    txP = oTxP * 128.0;
    float rustNoise = fBm(txP, 2.0, 0.5, 3);
    vec3 mrCol = (oCol / 2.0 + 0.35) * rustNoise;
    vec3 rCol = (oCol / 4.0 + 0.45) * rustNoise;
    float nsMask = ns - 0.45 - (rustNoise - 0.5) * 0.05;

    // edge wear: rust creeps along the surface curvature discontinuities
    float rustCrv = abs(crv - 0.5) * 2.0;
    nsMask = min(nsMask, 0.5 - rustCrv + ns + (rustNoise - 0.5) * 0.25);

    // subtle extra noise
    oCol *= min(ns * rustNoise * 3.0 + 0.25, 1.0);

    // material type + roughness per region (paint / bare metal / rust)
    float gType = objID == 1 ? 0.0 : 1.0;  // paint, or chrome outside
    float gRough = 0.8 * rustNoise;

    oCol = mix(oCol, mrCol * vec3(1.0, 0.6, 0.4), 1.0 - smoothstep(0.0, 0.025 / 8.0, nsMask));
    gType = mix(gType, 1.0, 1.0 - smoothstep(0.0, 0.025, nsMask));
    gRough = mix(gRough, rustNoise * 0.5 + ns * 0.5, 1.0 - smoothstep(0.0, 0.025 / 8.0, nsMask));
    oCol = mix(oCol, rCol, 1.0 - smoothstep(0.0, 0.025 / 8.0, nsMask + 0.05 / 3.0));

    // --- scratches: stretched thresholded noise strokes ------------------------
    float scale = 1.0 / 96.0;
    vec3 p3 = oTxP;
    p3.xy *= rot2(PI / 3.0);
    p3 /= vec3(32.0, 1.0, 1.0) * scale;
    float lacu = 2.0;
    float fallofff = 0.5;
    int layerN = 3;
    float scrL = fBm(p3, lacu, fallofff, layerN);
    float scratch = -(scrL - 0.63);

    p3 = oTxP;
    p3.xy *= rot2(-PI / 3.0);
    p3.yz *= rot2(-PI / 3.0);
    p3 /= vec3(32.0, 1.0, 1.0) * scale;
    scrL = fBm(p3, lacu, fallofff, layerN);
    scratch = min(scratch, -(scrL - 0.63));
    scratch = 1.0 - smoothstep(0.0, 0.01 * (1.0 + t * 0.125), scratch);

    oCol = mix(oCol, mrCol * vec3(1.0, 0.6, 0.4), scratch);
    gRough = mix(gRough, gRough * 2.0, scratch);
    gType = mix(gType, 1.0, scratch);

    // --- material properties -----------------------------------------------------
    float fresRef = 0.5;  // reflectivity
    float type = 1.0;     // dielectric or metallic
    float rough = 1.0;    // roughness

    if (objID > 0) {
      type = gType; rough = gRough; fresRef = 0.5;
    } else {
      // floor: roughness matches the noise
      rough = rustNoise * 0.5 + 0.5;
    }

    // standard BRDF dot products
    vec3 h = normalize(ld - rd);
    float ndl = dot(sn, ld);
    float nr = clamp(dot(sn, -rd), 0.0, 1.0);
    float nl = clamp(ndl, 0.0, 1.0);
    float nh = clamp(dot(sn, h), 0.0, 1.0);
    float vh = clamp(dot(-rd, h), 0.0, 1.0);

    // F0 for dielectrics in [0, .16]; metals use the base color
    vec3 f0 = vec3(0.16 * (fresRef * fresRef));
    f0 = mix(f0, oCol, type);
    vec3 FS = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);  // Fresnel-Schlick

    vec3 spec = getSpec(FS, nh, nr, nl, rough);
    vec3 diff = getDiff(FS, nl, rough, type);

    // ambient light (blackle's quick lighting trick)
    float amb = pow(length(sin(sn * 2.0) * 0.5 + 0.5), 2.0);

    col = oCol * (amb * 0.7 + diff * sh + spec * sh);

    // environment reflections (chrome pylons + frames)
    float specR = nh;
    rd.yz *= rot2(PI / 6.0);  // tilt the reflection toward the upright surface
    vec3 ref = reflect(rd, sn);
    vec3 rTx = envMap(ref);
    rTx *= rTx;
    float specStr = objID == 2 ? 4.0 : 1.0;
    if (objID > 0) col += rTx * spec * specStr;

    // attenuation + ambient occlusion
    col *= atten * ao;
  } else {
    // no hit: faint environment backdrop
    col = envMap(rd) * 0.12;
  }

  // engine convention: linear HDR color in RGB, view-space depth in alpha
  float d01 = (t >= FAR) ? 1.0 : depthFromViewZ(-t);
  gl_FragDepth = d01;
  fragColor = vec4(max(col, 0.0), d01);
}
