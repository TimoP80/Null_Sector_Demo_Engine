#version 300 es
// ---------------------------------------------------------------------------
// NULL SECTOR // NEURAL DUST - SCENE 7 // NEURAL OCEAN - THE REVEAL (vertex)
// ---------------------------------------------------------------------------
// Geometry version of the reveal: the network beneath the particle ocean is
// drawn as additive point sprites (nodes) + billboarded quads (synapses)
// instead of the old per-pixel SDF raymarch. The node positions use the
// EXACT same wave field as the raymarch (oceanY + per-node bob + destabilize
// jitter + pulse), but evaluated once per VERTEX - 144 nodes + ~385 synapse
// quads per frame instead of ~144 node evals per PIXEL, which is what took
// the raymarch from ~4 fps to 60.
// ---------------------------------------------------------------------------
#include <common>

layout(location = 0) in vec4 aNode;   // nodes: (gx, gy, 0, 0)
                                      // links: (gxA, gyA, gxB, gyB)
uniform float uPrim;        // 0 = node sprite, 1 = synapse quad
uniform float uMode;        // 1 = SYSTEM FAILURE (network already torn)
uniform float uFlash;       // per-kick strobe (audio kick analyser)
uniform float uHigh;        // react.high (treble sparkle)

out vec3 vCol;
out float vAlpha;
out float vDist;
out vec2 vT;                // synapse quad: (along 0..1, across in half-widths)
out float vSeed;

float oceanY(vec2 xz, float t) {
  float w1 = fbm2(xz * 0.10 + vec2(3.0, 1.0));
  float w2 = fbm2(xz * 0.30 + vec2(9.0));
  return (w1 - 0.5) * 1.1 + (w2 - 0.5) * 0.45 + 0.25 * sin(xz.x * 0.25 + t * 0.5);
}

/** node on the 12x12 grid - IDENTICAL to the raymarch's nodeData() */
vec3 nodePos(int gx, int gy, float t, float destab) {
  float fx = float(gx), fy = float(gy);
  vec2 xz = vec2(fx * 2.8 - 15.4, fy * 2.8 - 15.4);
  float y = oceanY(xz, t) - 2.0;                       // just below the sheet
  float h = hash12(vec2(fx, fy) + 7.0);
  y += 0.3 * sin(t * 0.9 + fx * 1.3 + fy * 2.7 + h * 6.28);
  vec3 jit = vec3(hash12(vec2(fx, fy) + 1.0) - 0.5,
                  hash12(vec2(fx, fy) + 2.0) - 0.5,
                  hash12(vec2(fx, fy) + 3.0) - 0.5) * (0.3 + 2.2 * destab);
  return vec3(xz.x, y, xz.y) + jit;
}

void main() {
  float t = Null.uTime;
  float secT = sat01(Null.uSectionLocal / max(Null.uSectionDur, 1e-4));
  float destab = smoothstep(0.15, 0.95, secT);        // destabilize ramp
  if (uMode > 0.5) destab = max(destab, 0.92);         // failure: already torn

  // the traveling pulse + bass (same field as the raymarch's pulse glow)
  float pulse = 0.25 + 1.1 * Null.uPulse + 0.55 * Null.uBass;
  vec2 pc = vec2(7.0 * sin(t * 0.22), 7.0 * cos(t * 0.18));

  if (uPrim < 0.5) {
    // --- node sprite ---------------------------------------------------------
    int gx = int(aNode.x + 0.5);
    int gy = int(aNode.y + 0.5);
    vec3 home = nodePos(gx, gy, t, destab);
    float radSeed = hash12(vec2(float(gx), float(gy)) + 5.0);
    float cSeed = hash12(vec2(float(gx), float(gy)) + 3.0);

    vec4 viewPos = Null.uView * vec4(home, 1.0);
    gl_Position = Null.uProj * viewPos;
    vDist = -viewPos.z;

    // perspective-scaled sprite: world radius -> pixels (the raymarch's node
    // radius 0.16-0.28, swelled by the traveling pulse + kick strobe)
    float pd = length(home.xz - pc);
    float nodePulse = exp(-pd * 0.22) * pulse;
    float rad = (0.16 + 0.12 * radSeed) * (1.0 + 0.7 * nodePulse + 0.5 * uFlash);
    float px = rad * Null.uRes.y * 0.5 / max(vDist * max(Null.uFovTan, 0.3), 0.5);
    gl_PointSize = clamp(px, 0.5, 16.0);

    vCol = palVoid(musicHue(0.15 + cSeed * 0.1));
    float bright = 0.35 + 1.1 * nodePulse + 0.3 * uFlash + 0.2 * Null.uBass;
    vCol *= bright;
    vCol += vec3(1.0, 0.98, 1.0) * uFlash * 0.25;
    vSeed = cSeed;
    vT = vec2(0.5, 0.0);
    vAlpha = clamp(1.6 - vDist * 0.03, 0.0, 1.0) * (0.5 + 0.5 * sat01(nodePulse + pulse));
  } else {
    // --- synapse quad (camera-facing billboard) ------------------------------
    int gxA = int(aNode.x + 0.5);
    int gyA = int(aNode.y + 0.5);
    int gxB = int(aNode.z + 0.5);
    int gyB = int(aNode.w + 0.5);
    vec3 A = nodePos(gxA, gyA, t, destab);
    vec3 B = nodePos(gxB, gyB, t, destab);
    float linkLen = length(B - A);
    if (linkLen < 1e-3) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec3 dir = (B - A) / linkLen;
    vec3 mid = (A + B) * 0.5;
    // camera-facing perpendicular (robust when the view is parallel to the link)
    vec3 ref = abs(dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 perp = normalize(cross(dir, ref));

    // grow with the pulse + bass + flash; fray as the network destabilizes
    float grow = 0.4 + 0.8 * pulse + 0.3 * Null.uBass + 0.5 * uFlash;
    float fray = 1.0 - 0.65 * destab * hash12(vec2(float(gxA), float(gyA)) + 11.0);
    float r0 = 0.02 + 0.03 * hash12(vec2(float(gxA), float(gyA)) + 9.0);
    float w = r0 * grow * max(fray, 0.0) * 2.0;        // quad half-width
    if (w < 0.004) w = 0.004;

    float cap = 0.12 + linkLen * 0.02;                 // rounded-cap overhang
    float extent = linkLen + 2.0 * cap;
    const vec2 corners[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                   vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(0.0, 0.0));
    vec2 c = corners[gl_VertexID % 6];
    vec3 pos = A + dir * (c.x * extent - cap) + perp * (c.y - 0.5) * w;

    vec4 viewPos = Null.uView * vec4(pos, 1.0);
    gl_Position = Null.uProj * viewPos;
    vDist = -viewPos.z;
    vT = vec2(c.x, c.y - 0.5);
    vSeed = hash12(vec2(float(gxA), float(gyA)) + 9.0);

    vec3 connCol = palVoid(musicHue(0.1));
    float bright = 0.4 + 0.8 * pulse + 0.4 * uFlash;
    vCol = connCol * bright;
    vCol += vec3(1.0, 0.96, 1.0) * uFlash * 0.5;
    vAlpha = clamp(1.6 - vDist * 0.03, 0.0, 1.0) * (0.6 + 0.4 * sat01(pulse));
  }
}
