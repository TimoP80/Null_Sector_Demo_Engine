// ---------------------------------------------------------------------------
// dataviz_bg.frag - standalone data-viz backdrop for the scene-graph/model
// section. Deliberately does NOT include common.glsl: it is a plain screen
// quad used as a dim backdrop behind the lit 3D layer, so it only depends on
// gl_FragCoord (the aspect is baked at a nominal 16:9; the gradient is
// cosmetic).
// ---------------------------------------------------------------------------
#version 330 core
out vec4 fragColor;
uniform float uTime;   // optional (bound by SceneFX when declared)

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(1600.0, 900.0);   // nominal 16:9
    vec2 c = uv - 0.5;
    float r = length(c * vec2(1.0, 1.7778));
    float vign = 1.0 - smoothstep(0.35, 0.62, r);

    // deep navy radial field
    vec3 col = mix(vec3(0.012, 0.020, 0.050), vec3(0.030, 0.085, 0.170), vign);

    // faint orthogonal grid, denser toward the horizon
    vec2 g = abs(fract(uv * vec2(20.0, 12.0) - 0.5) - 0.5) / fwidth(uv * vec2(20.0, 12.0));
    float line = min(g.x, g.y);
    col += vec3(0.02, 0.05, 0.10) * (1.0 - smoothstep(0.4, 1.2, line)) * (0.35 + 0.65 * vign);

    // slow scanline shimmer (audio-driven via uTime when available)
    col *= 1.0 + 0.02 * sin((uv.y + uTime * 0.05) * 520.0);

    fragColor = vec4(col, 1.0);
}
