// ===========================================================================
// NULL SECTOR // data/shadertoy/plasma.glsl
// Single-pass Shadertoy sample (no `// pass:` markers = image pass).
// Exercises the importer: iTime, iTimeDelta, iFrame, iResolution, iDate,
// iChannel0 (black when nothing is bound) + iChannelResolution.
// ===========================================================================

#define TAU 6.2831853

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
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
    for (int i = 0; i < 4; i++) {
        v += a * noise(p);
        p = p * 2.03 + vec2(7.7, 3.3);
        a *= 0.5;
    }
    return v;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float t = iTime * 0.35;

    // domain-warped fbm plasma
    float q = fbm(uv * 1.6 + vec2(t, -t * 0.7));
    float d = fbm(uv * 3.2 + q * 1.7 + vec2(t * 0.5, 0.0));

    // audio-style hue bands (palVoid space)
    vec3 col = 0.5 + 0.5 * cos(TAU * (vec3(0.0, 0.33, 0.67) * 2.0 + d * 1.4 + t * 0.12));

    // iFrame: settle the first two frames (uniform noise seeds)
    float settle = smoothstep(0.0, 2.0, float(iFrame));
    col *= 0.35 + 0.65 * settle;

    // iDate: subtle year-since-epoch drift on the palette
    col = mix(col, col.gbr, 0.02 * sin(iDate.x * 0.0001 + d * 3.0));

    // iChannel0: add texture content when a channel is bound (black = no-op)
    vec3 ch = texture(iChannel0, fragCoord / iResolution.xy).rgb;
    col += ch * 0.18 * (0.5 + 0.5 * sin(iTime * 0.8));

    // soft vignette
    vec2 q2 = fragCoord / iResolution.xy - 0.5;
    col *= 1.0 - 0.35 * dot(q2, q2);

    fragColor = vec4(col, 1.0);
}
