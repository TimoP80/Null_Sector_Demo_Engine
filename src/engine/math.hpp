// ---------------------------------------------------------------------------
// Tiny vector / quaternion / matrix toolkit (all column-major for GL).
// Direct port of src/engine/math.ts.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cmath>

using V3 = std::array<float, 3>;
using Q4 = std::array<float, 4>;   // xyzw, w = scalar
using Mat3 = std::array<float, 9>;
using Mat4 = std::array<float, 16>;  // column-major

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float satf(float x) { return clampf(x, 0.0f, 1.0f); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float fractf(float x) { return x - std::floor(x); }
inline float smoothstepf(float a, float b, float x) {
  const float t = satf((x - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}
inline float easeOut(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }

// --- vectors -----------------------------------------------------------------
inline V3 vAdd(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 vSub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 vScale(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline float vDot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 vCross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float vLen(const V3& a) { return std::sqrt(vDot(a, a)); }
inline V3 vNorm(const V3& a) {
  const float l = vLen(a);
  return l > 1e-8f ? vScale(a, 1.0f / l) : V3{0, 0, 1};
}

// --- quaternions --------------------------------------------------------------
inline Q4 qNorm(const Q4& q) {
  const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  return l > 1e-8f ? Q4{q[0] / l, q[1] / l, q[2] / l, q[3] / l} : Q4{0, 0, 0, 1};
}

/** rotation matrix (column-major 9) from quaternion */
inline Mat3 mat3FromQuat(const Q4& qin) {
  const Q4 q = qNorm(qin);
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat3 m{};
  m[0] = 1 - 2 * (y * y + z * z);
  m[1] = 2 * (x * y + z * w);
  m[2] = 2 * (x * z - y * w);
  m[3] = 2 * (x * y - z * w);
  m[4] = 1 - 2 * (x * x + z * z);
  m[5] = 2 * (y * z + x * w);
  m[6] = 2 * (x * z + y * w);
  m[7] = 2 * (y * z - x * w);
  m[8] = 1 - 2 * (x * x + y * y);
  return m;
}

/** orientation quaternion that looks along fwd with given up */
inline Q4 quatFromLookAt(const V3& fwd, const V3& up) {
  const V3 f = vNorm(fwd);
  const V3 r = vNorm(vCross(f, up));
  const V3 u = vCross(r, f);
  // rotation matrix with rows r,u,-f
  const float m[9] = {r[0], r[1], r[2], u[0], u[1], u[2], -f[0], -f[1], -f[2]};
  const float m00 = m[0], m01 = m[1], m02 = m[2];
  const float m10 = m[3], m11 = m[4], m12 = m[5];
  const float m20 = m[6], m21 = m[7], m22 = m[8];
  const float trace = m00 + m11 + m22;
  Q4 q{};
  if (trace > 0) {
    const float s = std::sqrt(trace + 1) * 2;
    q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
    q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
  } else if (m11 > m22) {
    const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
    q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
  } else {
    const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
    q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
  }
  return qNorm(q);
}

// --- matrices (column-major) ---------------------------------------------------
inline Mat4 mat4Perspective(float fovY, float aspect, float nearP, float farP) {
  const float f = 1.0f / std::tan(fovY / 2);
  const float nf = 1.0f / (nearP - farP);
  Mat4 m{};
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (farP + nearP) * nf;
  m[11] = -1;
  m[14] = 2 * farP * nearP * nf;
  return m;
}

/** view matrix (world -> view): rotation part has ROWS [r, u, -f] */
inline Mat4 mat4FromBasis(const V3& r, const V3& u, const V3& f, const V3& pos) {
  Mat4 m{};
  m[0] = r[0]; m[1] = u[0]; m[2] = -f[0];
  m[4] = r[1]; m[5] = u[1]; m[6] = -f[1];
  m[8] = r[2]; m[9] = u[2]; m[10] = -f[2];
  m[12] = -vDot(r, pos);
  m[13] = -vDot(u, pos);
  m[14] = vDot(f, pos);
  m[15] = 1;
  return m;
}

/** deterministic rng (matches the TS lcg used for seed buffers) */
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
  float next() {
    s = s * 1664525u + 1013904223u;
    return s / 4294967296.0f;
  }
};
