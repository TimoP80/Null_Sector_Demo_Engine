#version 300 es
// ---------------------------------------------------------------------------
// Logo material pass (the climax): resolves the particle data into the logo
// itself across four sub-phases -
//   WIREFRAME:  holographic grid resolves over the masked logo
//   CHROME:     solid reflective chrome with emissive circuitry + fresnel
//   FRACTURE:   voronoi shards rip the logo apart (rotating, flying out)
//   REFORM:     the shards converge and the logo reforms before the blast
//
// The chrome phase also casts a soft procedural halo behind the letters - the
// hero crop removed the baked sun backdrop the logo used to sit on, so it gets
// its own gentle wordmark-shaped light that breathes with the music.
// ---------------------------------------------------------------------------
precision highp float;
#include <common>

in vec2 vUV;

uniform sampler2D uTex;        // logo image (alpha = mask)
uniform vec2 uRes;
uniform float uTime;
uniform float uImageAspect;
uniform float uWire;           // 0..1 wireframe
uniform float uChrome;         // 0..1 chrome
uniform float uFrac;           // 0..1 fracture progress
uniform float uReform;         // 0..1 reform progress
uniform float uPulse;          // beat pulse
uniform float uEnergy;         // music energy (emissive drive)
uniform vec2 uOffset;          // camera parallax offset
uniform float uFlash;          // 0..1 additive white flash (fracture + blast)

out vec4 fragColor;

void main() {
  if (uFlash > 0.001) {
    fragColor = vec4(vec3(1.0, 0.92, 1.0) * uFlash, uFlash);
    return;
  }

  // fit the image into the screen (letterbox)
  float screenAspect = uRes.x / uRes.y;
  vec2 imgSize;
  if (screenAspect > uImageAspect) {
    imgSize = vec2(uImageAspect / screenAspect, 1.0);
  } else {
    imgSize = vec2(1.0, screenAspect / uImageAspect);
  }
  vec2 center = (vUV - 0.5) / imgSize + 0.5;
  if (center.x < 0.0 || center.x > 1.0 || center.y < 0.0 || center.y > 1.0) {
    fragColor = vec4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  // WebGL uploads the canvas top row at v=0, so mirror to show upright
  vec2 z = center;
  z.y = 1.0 - z.y;
  z += uOffset * 0.02;

  float mask = texture(uTex, z).a;

  // --- halo: soft procedural backlight hugging the letterforms ------------------
  // A 4-tap box blur of the mask, offset by a few screen pixels, gives a glow
  // that extends past the letters; carving out the letter cores leaves just the
  // outer rim of light. NULL SECTOR palette, breathing with the beat + energy.
  // Gated on uChrome, so it dies with the chrome as the logo fractures.
  float haloPx = 9.0; // glow radius in screen pixels
  float k = haloPx / (imgSize.y * uRes.y);
  float haloBlur = texture(uTex, z + vec2(k, 0.0)).a
                 + texture(uTex, z - vec2(k, 0.0)).a
                 + texture(uTex, z + vec2(0.0, k)).a
                 + texture(uTex, z - vec2(0.0, k)).a;
  float halo = smoothstep(0.08, 0.45, haloBlur * 0.25) * (1.0 - smoothstep(0.15, 0.75, mask));

  // --- voronoi shard displacement (fracture + reform) --------------------------
  vec2 vp = center * vec2(26.0, 26.0 / uImageAspect) + uTime * 0.05;
  vec2 cell = floor(vp);
  vec2 hv = vec2(hash21(cell), hash21(cell + 7.7)); // per-cell hash pair
  float edge = voronoiEdge(vp);
  vec2 shardDir = normalize(hv - 0.5 + 1e-4);
  float fracDisp = uFrac * (1.0 - uReform);
  vec2 suv = z + shardDir * fracDisp * 0.9 + (center - 0.5) * fracDisp * 0.6;

  // --- wireframe grid -------------------------------------------------------------
  vec2 g = abs(fract(suv * vec2(46.0, 46.0 / uImageAspect)) - 0.5);
  float grid = smoothstep(0.44, 0.5, max(g.x, g.y));
  // animated scan sweep
  float scan = smoothstep(0.15, 0.0, abs(fract(suv.y * 3.0 - uTime * 0.4) - 0.5));

  // --- chrome material (procedural, PBR-flavored) ----------------------------------
  vec3 n = normalize(vec3(suv - 0.5, 0.4));                    // fake normal
  float fres = pow(1.0 - max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0), 3.0);
  // environment bands
  vec3 chrome = mix(vec3(0.16, 0.2, 0.3), vec3(0.75, 0.8, 0.95),
                    0.5 + 0.5 * sin(suv.y * 18.0 + suv.x * 12.0));
  chrome += palVoid(musicHue() * 0.6 + suv.x * 0.8 + suv.y * 0.6) * 0.22;
  chrome += vec3(1.0, 0.95, 0.9) * pow(max(suv.y * 1.4 - 0.35, 0.0), 2.0) * 0.5;
  // emissive circuitry: neon veins that pulse with the music
  float vein = smoothstep(0.03, 0.0, abs(hash12(floor(suv * vec2(9.0, 9.0 / uImageAspect))) - 0.5) * 0.6 - 0.2);
  float veinPulse = (0.5 + 0.5 * uPulse) * (0.5 + uEnergy);
  chrome += palVoid(musicHue(0.2) + hv.x * 0.4) * vein * veinPulse * 2.2;
  chrome *= 0.7 + 0.5 * fres;                                   // fresnel edge brightening

  // --- composite the phases ---------------------------------------------------------
  vec3 col = vec3(0.0);
  float alpha = 0.0;

  // wireframe: grid lines + scan sweep over the mask, holographic blue
  vec3 wireCol = palVoid(0.55) * grid * (0.8 + 0.6 * uPulse) + vec3(0.4, 0.8, 1.0) * scan * 0.35;
  col += wireCol * mask * uWire;
  alpha += mask * uWire * 0.8;

  // chrome: the solid material
  col += chrome * mask * uChrome;
  alpha += mask * uChrome;

  // halo: subtle backlight rim behind the chrome letters (beat-reactive)
  vec3 haloCol = palVoid(0.55) * (0.25 + 0.2 * uPulse) * (0.4 + uEnergy * 0.6) * uChrome;
  col += haloCol * halo;
  alpha += halo * uChrome * 0.4;

  // reform: the healing logo glows solid again
  alpha += mask * uReform * 0.85;

  // fracture seams: glowing cracks along the voronoi cell boundaries + shard shading
  float crack = smoothstep(0.08, 0.0, edge) * mask;
  col += palVoid(musicHue(0.3) + hv.y * 0.4) * crack * fracDisp * 4.0;
  float facet = 0.6 + 0.4 * dot(normalize(shardDir), normalize(vec2(-0.7, -0.5)));
  col *= mix(1.0, facet * 1.5, uFrac);

  if (uReform > 0.0) {
    // reforming: cracks heal, glow swells
    col += palVoid(0.75) * mask * uReform * max(0.0, 1.0 - edge * 2.0) * (0.3 + uEnergy);
  }

  // subtle constant presence so the logo never fully vanishes
  col += chrome * mask * 0.06 + vec3(0.3, 0.15, 0.6) * mask * 0.05;

  // `alpha` must be driven only by the logo mask or its local halo.  A
  // constant alpha here turns the transparent part of the source image into
  // a full rectangular source-over layer, visibly dimming everything behind
  // the wordmark.
  fragColor = vec4(col, clamp(alpha, 0.0, 1.0));
}
