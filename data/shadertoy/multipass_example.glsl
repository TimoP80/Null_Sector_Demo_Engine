// ---------------------------------------------------------------------------
// MULTIPASS EXAMPLE - a Shadertoy-style file with several passes.
//
// This file demonstrates the engine's multi-pass `// pass:` marker format:
//
//     // pass: common     - shared helpers, prepended to every other pass
//     // pass: buffer_a   - first buffer pass
//     // pass: buffer_b   - second buffer pass
//     // pass: image      - the final compose pass
//
// Channel wiring is declared with `// channel:` comments; a bare pass name
// means "sample that buffer", and audio/keyboard channels are replaced with
// black:
//
//     // channel: iChannel0 = buffer_a      (buffer_a samples nothing -> unbound)
//     // channel: iChannel1 = audio         (buffer_a adds audio -> replaced)
//     // channel: iChannel2 = keyboard      (image reads keyboard -> replaced)
//
// Defaults when no comments are given: buffer N samples buffer N-1 on
// iChannel0 (the first buffer samples an external texture), and the image
// pass samples the LAST buffer on iChannel0. `// option: renderScale 0.5`
// renders the buffer passes at half resolution.
//
// Run it directly in the engine:
//     load shadertoy multipass_example.glsl
//     show shadertoy:multipass_example.glsl
//
// Or fold every pass into ONE portable fragment shader (no window needed):
//     ns_shader_ai --convert-shadertoy=data/shadertoy/multipass_example.glsl \
//                  --out=multipass_example.frag
// ---------------------------------------------------------------------------

// option: renderScale 0.5

// pass: common
// Shared helpers, prepended to every pass by the runtime and the converter.
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// pass: buffer_a
// First buffer: animated diagonal gradient, boosted by (replaced) audio.
// channel: iChannel1 = audio
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float g = 0.5 + 0.5 * sin(uv.x * 6.2831 + iTime);
    vec3 col = mix(vec3(0.05, 0.0, 0.12), vec3(0.85, 0.15, 0.6), g);
    vec3 audio = texture(iChannel1, uv).rgb;  // audio -> vec4(0.0): adds nothing
    fragColor = vec4(col + audio * 0.5, 1.0);
}

// pass: buffer_b
// Second buffer: samples the previous buffer (default iChannel0 chain)
// and sprinkles hash noise on top.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 prev = texture(iChannel0, uv).rgb;
    float n = hash21(fragCoord * 0.1 + iTime * 3.0);
    fragColor = vec4(prev + n * 0.15, 1.0);
}

// pass: image
// Final compose: sample the last buffer (buffer_b), vignette, scan a
// keyboard channel that the converter replaces with black.
// channel: iChannel2 = keyboard
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 scene = texture(iChannel0, uv).rgb;
    vec3 kb = texture(iChannel2, uv).rgb;  // keyboard -> vec4(0.0)
    float vig = 1.0 - 0.35 * length(uv - 0.5);
    fragColor = vec4(scene * vig + hash21(fragCoord) * 0.02 + kb * 0.1, 1.0);
}
