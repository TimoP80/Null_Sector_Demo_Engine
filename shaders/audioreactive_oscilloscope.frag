#version 300 es
precision highp float;

in vec2 vUv;
out vec4 fragColor;

uniform vec2 uResolution; // @param resolution viewport resolution in pixels
uniform float uTime; // @param time elapsed time in seconds
uniform float uBPM; // @param bpm tempo in beats per minute
uniform float uBeat; // @param beat current beat counter
uniform float uBar; // @param bar current bar counter
uniform float uBeatPhase; // @param beatPhase normalized beat phase 0..1
uniform float uAudioLevel; // @param audioLevel full-band audio level 0..1
uniform float uBass; // @param bass low frequency energy 0..1
uniform float uMid; // @param mid mid frequency energy 0..1
uniform float uTreble; // @param treble high frequency energy 0..1
uniform float uKick; // @param kick kick transient strength 0..1
uniform float uSnare; // @param snare snare transient strength 0..1
uniform vec3 uColor; // @param color primary tint, mixed subtly with oscilloscope green
uniform vec3 uColor2; // @param color secondary accent tint
uniform float uIntensity; // @param intensity overall brightness multiplier
uniform float uSpeed; // @param speed animation speed multiplier
uniform float uScale; // @param scale waveform density / zoom

float sat(float x) {
    return clamp(x, 0.0, 1.0);
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float waveAt(float x, float layer) {
    float spd = max(uSpeed, 0.01);
    float sc = max(uScale, 0.15);
    float t = uTime * spd;
    float audio = sat(uAudioLevel * 1.35 + uBass * 0.65 + uMid * 0.25);
    float amp = (0.105 + 0.20 * audio + 0.10 * sat(uKick)) / (1.0 + layer * 0.38);
    float density = sc * (5.2 + 5.5 * sat(uMid) + 2.0 * layer);
    float tempo = max(uBPM, 1.0) / 60.0;
    float ph = t * (1.15 + layer * 0.33) + uBeat * 0.031 + uBar * 0.017;

    float y = 0.0;
    y += sin(x * density + ph * 2.25) * 0.55;
    y += sin(x * (density * 1.73 + 1.7) - ph * 1.42 + layer) * 0.28;
    y += sin(x * (density * 0.47 + 4.0) + t * tempo * 6.2831853 + uBeatPhase * 6.2831853) * 0.17;
    y += sin(x * (22.0 + 18.0 * sat(uTreble)) - t * 7.0) * 0.035 * sat(uTreble + uSnare);

    float grit = noise2(vec2(x * 3.0 + t * 0.7, layer * 7.0 + uBar * 0.19)) - 0.5;
    y += grit * 0.105 * sat(uTreble + uSnare * 0.9);
    return y * amp;
}

float traceLayer(vec2 p, float layer) {
    float y = waveAt(p.x + layer * 0.055, layer);
    float d = abs(p.y - y);
    float brightHit = sat(uKick * 0.9 + uSnare * 0.45 + uAudioLevel * 0.45);
    float thick = 0.0045 + 0.0040 * sat(uIntensity) + 0.0045 * brightHit;
    thick /= 1.0 + layer * 0.22;

    float core = 1.0 - smoothstep(0.0, thick, d);
    float innerGlow = exp(-d * (28.0 - 6.0 * sat(uBass))) * (0.62 + brightHit * 0.45);
    float outerGlow = exp(-d * 6.0) * (0.09 + 0.18 * sat(uAudioLevel + uBass));
    return core * 1.85 + innerGlow + outerGlow;
}

float gridLayer(vec2 uv, vec2 p) {
    vec2 cells = uv * vec2(12.0, 8.0);
    vec2 edgeDist = 0.5 - abs(fract(cells) - 0.5);
    float minor = 1.0 - smoothstep(0.0, 0.018, min(edgeDist.x, edgeDist.y));
    float axisX = 1.0 - smoothstep(0.0, 0.006, abs(p.y));
    float axisY = 1.0 - smoothstep(0.0, 0.006, abs(p.x));
    return minor * 0.42 + (axisX + axisY) * 0.42;
}

float spectrumBars(vec2 uv) {
    float bars = 0.0;
    float t = uTime * max(uSpeed, 0.01);
    for (int i = 0; i < 32; ++i) {
        float fi = float(i);
        float x = (fi + 0.5) / 32.0;
        float rnd = hash21(vec2(fi, floor(t * 8.0) + uBeat));
        float band = sin(fi * 0.41 + t * 2.1) * 0.5 + 0.5;
        float level = mix(uBass, uTreble, fi / 31.0) * 0.55 + uMid * 0.22 + uAudioLevel * 0.25;
        float h = 0.035 + 0.22 * sat(level + rnd * 0.18 + band * 0.12);
        float dx = abs(uv.x - x);
        float barMask = (1.0 - smoothstep(0.006, 0.014, dx)) * (1.0 - smoothstep(h, h + 0.018, uv.y));
        bars += barMask * (0.38 + 0.62 * level);
    }
    return bars;
}

float sparkLayer(vec2 uv, float aspect) {
    float sparks = 0.0;
    float t = uTime * max(uSpeed, 0.01);
    for (int i = 0; i < 18; ++i) {
        float fi = float(i);
        vec2 seed = vec2(fi * 11.17, fi * 3.71 + uBar);
        vec2 pos = vec2(hash21(seed), hash21(seed + 9.3));
        pos.x = fract(pos.x + t * (0.012 + 0.010 * hash21(seed + 4.0)));
        pos.y = fract(pos.y + sin(t * 0.7 + fi) * 0.012 + uBeatPhase * 0.018 * sat(uTreble));
        vec2 q = (uv - pos) * vec2(aspect, 1.0);
        float d = length(q);
        float twinkle = 0.45 + 0.55 * sin(t * 6.0 + fi * 2.3);
        sparks += exp(-d * 75.0) * twinkle * sat(0.20 + uTreble + uSnare * 0.8);
    }
    return sparks;
}

void main() {
    vec2 res = max(uResolution, vec2(1.0));
    vec2 uv = gl_FragCoord.xy / res;
    float aspect = res.x / res.y;
    vec2 p = uv * 2.0 - 1.0;
    p.x *= aspect;

    vec3 oscGreen = mix(vec3(0.018, 1.0, 0.145), clamp(uColor, 0.0, 1.0), 0.18);
    vec3 accent = mix(vec3(0.42, 1.0, 0.36), clamp(uColor2, 0.0, 1.0), 0.25);
    vec3 deepGreen = vec3(0.0, 0.055, 0.018);

    float audio = sat(uAudioLevel + uBass * 0.45 + uMid * 0.25 + uTreble * 0.18);
    float beatPulse = exp(-uBeatPhase * 4.5) * sat(0.35 + uKick + audio * 0.35);
    float tempoPulse = 0.5 + 0.5 * sin(6.2831853 * (uTime * max(uBPM, 1.0) / 60.0));

    vec3 col = deepGreen;

    float grid = gridLayer(uv, p);
    col += oscGreen * grid * (0.10 + 0.10 * audio);

    for (int i = 0; i < 3; ++i) {
        float layer = float(i);
        float tr = traceLayer(p, layer);
        float fade = 1.0 / (1.0 + layer * 0.72);
        vec3 layerColor = mix(oscGreen, accent, layer * 0.18);
        col += layerColor * tr * fade * (0.68 + 0.55 * sat(uIntensity));
    }

    float bars = spectrumBars(uv);
    col += oscGreen * bars * (0.34 + 0.48 * sat(uIntensity));

    float sparks = sparkLayer(uv, aspect);
    col += accent * sparks * 0.75;

    float r = length(p);
    float ringRadius = 0.18 + 1.15 * fract(uBeatPhase + 0.04);
    float ring = 1.0 - smoothstep(0.012, 0.045, abs(r - ringRadius));
    col += oscGreen * ring * beatPulse * 0.42;

    float centerBloom = exp(-r * 2.25) * (0.045 + 0.12 * audio + 0.10 * tempoPulse * sat(uKick));
    col += oscGreen * centerBloom;

    float scan = 0.92 + 0.08 * sin(uv.y * res.y * 3.14159265);
    float mask = 0.955 + 0.045 * sin(uv.x * res.x * 2.0943951);
    float vignette = 1.0 - smoothstep(0.35, 1.42, length(vec2(p.x * 0.72, p.y)));

    col *= scan * mask;
    col *= 0.62 + 0.38 * vignette;
    col *= 0.82 + 0.72 * sat(uIntensity);

    col = 1.0 - exp(-col);
    col += oscGreen * pow(max(0.0, 1.0 - r), 4.0) * 0.035;

    fragColor = vec4(col, 1.0);
}