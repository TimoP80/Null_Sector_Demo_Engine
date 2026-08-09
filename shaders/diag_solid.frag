#version 300 es
// ---------------------------------------------------------------------------
// DIAGNOSTIC - solid colour fullscreen test. Ignores every uniform on purpose:
// the only thing that can make this fail is the render path itself (scene
// activation -> shader bind -> fs triangle -> composite -> post -> present).
// Magenta = visible on every dark background.
// ---------------------------------------------------------------------------
precision highp float;

out vec4 fragColor;

void main() {
  fragColor = vec4(1.0, 0.0, 1.0, 1.0);
}
