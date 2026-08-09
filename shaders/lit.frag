#version 300 es
// Forward-lit PBR-ish material: base color / metallic / roughness / AO /
// emission / opacity, optional albedo + normal maps, up to 4 lights
// (directional / point / spot) and a cheap ambient + IBL-ish fill.
#include <common>

uniform vec4  uBaseColor;   // linear RGBA
uniform float uMetallic;
uniform float uRoughness;
uniform float uAO;
uniform vec3  uEmission;
uniform float uOpacity;
uniform sampler2D uAlbedo;     // optional (unit 0)
uniform float uHasAlbedo;
uniform sampler2D uNormalMap;  // optional (unit 1)
uniform float uHasNormal;
uniform float uAmbient;        // ambient light level

uniform int   uLightCount;
uniform vec4  uLightPos[4];    // xyz pos (point/spot) or dir (directional)
uniform vec4  uLightColor[4];  // rgb + intensity
uniform int   uLightType[4];   // 0 dir, 1 point, 2 spot
uniform float uLightRange[4];
uniform float uLightAngle[4];  // spot cone half-angle (cos)

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in mat3 vTBN;

out vec4 fragColor;

void main() {
  vec3 N = normalize(vNormal);
  if (uHasNormal > 0.5) {
    vec3 nrm = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
    N = normalize(vTBN * nrm);
  }
  vec3 V = normalize(Null.uCamPos - vWorldPos);
  vec3 base = uBaseColor.rgb;
  if (uHasAlbedo > 0.5) base *= texture(uAlbedo, vUV).rgb;

  float metal = clamp(uMetallic, 0.0, 1.0);
  float rough = clamp(uRoughness, 0.04, 1.0);
  vec3 F0 = mix(vec3(0.04), base, metal);

  vec3 col = vec3(0.0);
  for (int i = 0; i < uLightCount; i++) {
    vec3 L;
    float atten = 1.0;
    if (uLightType[i] == 0) {
      L = normalize(-uLightPos[i].xyz);
    } else if (uLightType[i] == 2) {
      vec3 toL = uLightPos[i].xyz - vWorldPos;
      float d = length(toL);
      L = toL / max(d, 1e-4);
      float spot = smoothstep(uLightAngle[i], uLightAngle[i] + 0.15, dot(-L, normalize(uLightPos[i].xyz)));
      atten = spot / (1.0 + d * d / max(uLightRange[i] * uLightRange[i], 1e-4));
    } else {
      vec3 toL = uLightPos[i].xyz - vWorldPos;
      float d = length(toL);
      L = toL / max(d, 1e-4);
      atten = 1.0 / (1.0 + d * d / max(uLightRange[i] * uLightRange[i], 1e-4));
      atten *= step(d, uLightRange[i]);
    }
    float NdotL = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    // Cook-Torrance-ish: diffuse (non-metal) + specular (Blinn)
    vec3 diffuse = base * (1.0 - metal);
    float specPow = mix(256.0, 8.0, rough);
    float spec = pow(NdotH, specPow) * mix(1.0, 0.5, rough);
    float ndv = max(dot(N, V), 0.0);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - ndv, 5.0);
    col += uLightColor[i].rgb * uLightColor[i].a * atten * NdotL *
           (diffuse + fresnel * spec);
  }

  // ambient fill (sky-ish, chord-tinted) + emission
  vec3 amb = base * uAO * uAmbient * (0.6 + 0.4 * palVoid(Null.uMusicHue + 0.5));
  col += amb + uEmission;

  float a = uOpacity;
  fragColor = vec4(col, a);
}
