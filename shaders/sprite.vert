#version 300 es
// Sprite / textured quad with a model transform.
#include <common>

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uModel;

out vec2 vUV;

void main() {
  vUV = aUV;
  gl_Position = uModel * vec4(aPos, 0.0, 1.0);
}
