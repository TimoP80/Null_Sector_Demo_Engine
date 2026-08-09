#version 300 es
// Emergency passthrough: blits a scene texture straight to the canvas when
// the full post pipeline (DOF / bloom / compose / FXAA) failed to build.
// Self-contained by design.
precision highp float;

uniform sampler2D uTex;

in vec2 vUV;
out vec4 fragColor;

void main() {
  fragColor = texture(uTex, vUV);
}
