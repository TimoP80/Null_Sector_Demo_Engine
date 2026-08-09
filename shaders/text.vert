#version 300 es
// Bitmap font quad rendering.
// location 0: pos (clip space), location 1: uv (atlas), location 2: char seed
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aSeed;
out vec2 vUV;
out float vSeed;
void main() {
  vUV = aUV;
  vSeed = aSeed;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
