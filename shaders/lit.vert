#version 300 es
// Forward-lit mesh vertex shader (PBR-ish material model).
#include <common>

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;  // xyz tangent, w = handedness

uniform mat4 uModel;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;

void main() {
  vec4 wp = uModel * vec4(aPos, 1.0);
  vWorldPos = wp.xyz;
  vNormal = mat3(uModel) * aNormal;
  vUV = aUV;
  vec3 T = normalize(mat3(uModel) * aTangent.xyz);
  vec3 N = normalize(vNormal);
  vec3 B = cross(N, T) * aTangent.w;
  vTBN = mat3(T, B, N);
  gl_Position = Null.uProj * Null.uView * wp;
}
