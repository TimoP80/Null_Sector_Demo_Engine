#version 300 es
precision highp float;

out vec4 fragColor;

// @param uResolution vec2 viewport resolution in pixels
uniform vec2 uResolution;
// @param uTime float elapsed time in seconds
uniform float uTime;
// @param uBPM float tempo in beats per minute
uniform float uBPM;
// @param uBeat float beat counter
uniform float uBeat;
// @param uBar float bar counter
uniform float uBar;
// @param uBeatPhase float normalized beat phase 0..1
uniform float uBeatPhase;
// @param uAudioLevel float broadband audio level 0..1+
uniform float uAudioLevel;
// @param uBass float bass band level 0..1+
uniform float uBass;
// @param uMid float mid band level 0..1+
uniform float uMid;
// @param uTreble float treble band level 0..1+
uniform float uTreble;
// @param uKick float kick trigger/envelope 0..1+
uniform float uKick;
// @param uSnare float snare trigger/envelope 0..1+
uniform float uSnare;
// @param uColor vec3 primary neon color, cyan by default when black
uniform vec3 uColor;
// @param uColor2 vec3 secondary neon color, violet by default when black
uniform vec3 uColor2;
// @param uIntensity float final glow/brightness multiplier
uniform float uIntensity;
// @param uSpeed float forward tunnel speed multiplier
uniform float uSpeed;
// @param uScale float tunnel scale/zoom multiplier
uniform float uScale;

#define PI 3.141592653589793
#define TAU 6.283185307179586

float sat(float x) { return clamp(x, 0.0, 1.0); }
vec3 sat(vec3 x) { return clamp(x, 0.0, 1.0); }

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float linePulse(float x, float width, float feather) {
    return 1.0 - smoothstep(width, width + feather, abs(x));
}

float glowLine(float x, float width) {
    float d = abs(x);
    return exp(-d * d / max(width * width, 0.00001));
}

vec3 getPrimaryColor() {
    float hasUser = step(0.05, length(uColor));
    return mix(vec3(0.0, 0.92, 1.0), uColor, hasUser);
}

vec3 getSecondaryColor() {
    float hasUser = step(0.05, length(uColor2));
    return mix(vec3(0.72, 0.08, 1.0), uColor2, hasUser);
}

vec3 tunnelLayer(vec2 p, float t, float sampleOffset) {
    vec3 cyan = getPrimaryColor();
    vec3 violet = getSecondaryColor();

    float audio = sat(uAudioLevel);
    float bass = sat(uBass * 0.9 + uKick * 0.9);
    float kick = sat(uKick);
    float treble = sat(uTreble);
    float mid = sat(uMid);

    float zoom = max(0.12, uScale) * (1.0 + 0.075 * kick - 0.035 * bass * sin(uBeatPhase * TAU));
    p *= zoom;

    float r = length(p);
    float a = atan(p.y, p.x);

    float depth = 1.15 / (r + 0.065 + 0.018 * kick);
    float z = depth + t * (1.25 + 1.4 * max(uSpeed, 0.0)) + sampleOffset;

    float twist = 0.42 * sin(z * 0.33 + t * 0.9) + 0.18 * sin(z * 1.17 - t * 1.3);
    twist += bass * 0.28 * sin(z * 1.85 + uBeat * 0.35);
    a += twist;

    float sectorCount = 10.0 + floor(mid * 6.0);
    float sector = a / TAU * sectorCount;
    float sectorId = floor(sector);
    float sectorFrac = fract(sector) - 0.5;

    float depthCells = z * (1.15 + 0.2 * bass);
    float zId = floor(depthCells);
    float zFrac = fract(depthCells) - 0.5;

    float perspective = sat(depth * 0.18);
    float lineWidth = mix(0.010, 0.022, bass) + 0.006 * sampleOffset;

    float angularGrid = glowLine(sectorFrac, lineWidth * (1.2 + r * 0.8));
    float ringGrid = glowLine(zFrac, lineWidth * (1.0 + 0.6 * kick));

    float h = hash21(vec2(sectorId, zId));
    float h2 = hash21(vec2(sectorId + 17.0, zId - 9.0));

    float traceX = glowLine(sectorFrac - (h - 0.5) * 0.38, lineWidth * 0.75);
    float traceZ = glowLine(zFrac - (h2 - 0.5) * 0.42, lineWidth * 0.75);
    float circuitGate = step(0.38, h) * (0.55 + 0.45 * sin(t * 4.0 + h * TAU + uBeat));
    float circuit = max(traceX * linePulse(zFrac, 0.34, 0.02), traceZ * linePulse(sectorFrac, 0.34, 0.02)) * circuitGate;

    float nodeDist = length(vec2(sectorFrac - (h - 0.5) * 0.38, zFrac - (h2 - 0.5) * 0.42));
    float node = exp(-nodeDist * nodeDist / (0.0018 + 0.0025 * kick)) * step(0.62, h2);

    float barcode = 0.0;
    for (int i = 0; i < 5; ++i) {
        float fi = float(i);
        float local = fract(z * (2.0 + fi * 0.37) + h * 3.0 + fi * 0.19) - 0.5;
        barcode += glowLine(local, 0.006 + 0.002 * fi) * step(0.52 + fi * 0.055, hash21(vec2(sectorId + fi, zId)));
    }
    barcode *= 0.22 + 0.45 * treble;

    float centerVoid = smoothstep(0.045, 0.28, r);
    float distanceFade = sat(1.45 - r * 1.05) * sat(depth * 0.32 + 0.15);
    float panels = 0.10 + 0.13 * sin(sectorId * 1.7 + zId * 0.9 + t * 2.1);
    panels += 0.10 * step(0.73, h) * sin(z * 0.8 + h * TAU + t * 3.0);

    float grid = angularGrid * 0.65 + ringGrid * (0.85 + 1.25 * kick);
    float neon = grid + circuit * 1.35 + node * (1.2 + 2.8 * kick) + barcode;
    neon *= centerVoid * distanceFade;

    vec3 wallColor = mix(violet * 0.08, cyan * 0.09, 0.5 + 0.5 * sin(a * 3.0 + z * 0.17));
    vec3 lineColor = mix(violet, cyan, sat(0.5 + 0.5 * sin(sectorId * 0.9 + zId * 0.45 + t * 2.0)));
    lineColor = mix(lineColor, cyan, ringGrid * (0.35 + 0.35 * kick));
    lineColor = mix(lineColor, violet, angularGrid * 0.3);

    vec3 col = wallColor * panels * centerVoid;
    col += lineColor * neon * (1.15 + 1.6 * bass);
    col += cyan * pow(perspective, 2.2) * (0.22 + 0.7 * kick);
    col += violet * pow(max(0.0, 1.0 - r), 5.0) * 0.12;

    return col;
}

void main() {
    vec2 uv = gl_FragCoord.xy / max(uResolution, vec2(1.0));
    vec2 p = (gl_FragCoord.xy - 0.5 * uResolution.xy) / max(uResolution.y, 1.0);

    float t = uTime;
    float bpmClock = max(uBPM, 1.0) / 120.0;
    t *= 0.88 + 0.18 * bpmClock;

    float kick = sat(uKick);
    float bass = sat(uBass);
    float snare = sat(uSnare);

    vec2 shake;
    shake.x = sin(uBeat * 12.989 + uTime * 31.0) * 0.006 * kick;
    shake.y = cos(uBeat * 9.173 + uTime * 27.0) * 0.004 * kick;
    p += shake;

    float beatZoom = 1.0 + 0.035 * kick + 0.018 * bass * sin(uBeatPhase * TAU);
    p *= beatZoom;

    vec3 col = vec3(0.0);
    float totalWeight = 0.0;
    for (int i = 0; i < 4; ++i) {
        float fi = float(i);
        float w = 1.0 - fi * 0.18;
        vec2 pp = p * (1.0 + fi * 0.018 * (0.5 + kick));
        col += tunnelLayer(pp, t - fi * 0.035 * (1.0 + uSpeed), fi * 0.09) * w;
        totalWeight += w;
    }
    col /= max(totalWeight, 0.001);

    vec3 cyan = getPrimaryColor();
    vec3 violet = getSecondaryColor();

    float scan = 0.92 + 0.08 * sin(gl_FragCoord.y * 1.7 + uTime * 60.0);
    float vignette = smoothstep(1.28, 0.16, length(p));
    float beatFlash = exp(-uBeatPhase * 6.0) * kick;

    col *= scan;
    col *= 0.55 + 0.85 * vignette;
    col += (cyan * 0.75 + violet * 0.55) * beatFlash * 0.35;
    col += violet * snare * 0.08 * (0.5 + 0.5 * sin(uv.y * 80.0 + uTime * 20.0));

    float intensity = max(uIntensity, 0.0);
    col *= 0.85 + intensity * 1.35;
    col = col / (1.0 + col * 0.72);
    col = pow(sat(col), vec3(0.82));

    fragColor = vec4(col, 1.0);
}