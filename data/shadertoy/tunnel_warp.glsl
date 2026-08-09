// ===========================================================================
// NULL SECTOR // data/shadertoy/tunnel_warp.glsl
// Multi-pass Shadertoy sample demonstrating the importer's buffer pipeline:
//
//   // pass: common    - shared helpers (no program of its own)
//   // pass: buffer_a  - renders into the RGBA16F buffer A target
//   // pass: image     - composes buffer A (iChannel0) over the LIVE scene
//                       (iChannel1 = the HDR input)
//
// Hot reload: touch the file and the running effect recompiles in place.
//
// The buffer passes do most of the work (domain-warped fbm), so render them
// at half resolution; the image pass + scene snapshot stay full-res.
// option: renderScale 0.5
// ===========================================================================

// pass: common

#define TAU 6.2831853

float hash21(vec2 p) {
    p = fract(p * vec2(233.34, 765.21));
    p += dot(p, p + 73.2);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = p * 2.03 + vec2(9.2, 4.7);
        a *= 0.5;
    }
    return v;
}

// scrolling domain-warp field (shared by buffer_a + the image dissolve)
vec2 warp(vec2 uv, float t) {
    float n = fbm(uv * 2.4 + vec2(t * 0.25, 0.0));
    return uv + 0.14 * vec2(fbm(uv * 3.0 + n + t * 0.18),
                            fbm(uv * 3.0 + vec2(0.0, t * 0.22)));
}

// pass: buffer_a

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime;
    vec2 w = warp(uv, t);

    // radial energy bands
    vec2 c = uv - 0.5;
    float r = length(c * vec2(1.0, 1.35));
    float ring = sin((r * 22.0 - t * 2.2) * TAU * 0.25);
    vec3 col = vec3(0.05, 0.18, 0.42) + vec3(0.1, 0.55, 1.0) * (0.5 + 0.5 * ring) * smoothstep(0.6, 0.0, r);

    // fbm overlay tinted cyan
    float f = fbm(w * 4.0 + t * 0.12);
    col += vec3(0.02, 0.3, 0.5) * f;

    // feedback: mix in iChannel0 (previous buffer state / scene / black)
    vec3 fb = texture(iChannel0, uv).rgb;
    col = mix(col, fb, 0.22 * (0.5 + 0.5 * sin(t * 0.6)));

    fragColor = vec4(col, 1.0);
}

// pass: image

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec4 buf = texture(iChannel0, uv);
    vec4 scene = texture(iChannel1, uv);

    // warp-driven dissolve: the field breathes the buffer over the scene
    vec2 w = warp(uv, iTime * 0.8);
    float m = 0.55 + 0.35 * sin(fbm(w * 3.0 + iTime * 0.1) * 6.2831 + iTime * 0.4);
    m = clamp(m, 0.15, 0.9);

    fragColor = vec4(mix(scene.rgb, buf.rgb, m), 1.0);
}
