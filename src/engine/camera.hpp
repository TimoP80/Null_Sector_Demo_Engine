// ---------------------------------------------------------------------------
// Camera: spline paths, quaternion orientation, shake, beat zoom, DOF focus.
// Port of src/engine/camera.ts.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/math.hpp"
#include <algorithm>
#include <chrono>
#include <vector>

namespace ns {

struct CamKey {
  float t;  // path parameter 0..1
  V3 pos;
  V3 target;
  float fov;  // degrees
};

/** ease a 0..1 path parameter with anticipation pull-back near the end */
inline float cineEase(float t, float anticipation) {
  const float tt = clampf(t, 0, 1);
  const float e = tt * tt * (3 - 2 * tt);
  return e + anticipation * 0.06f * std::sin(std::min(e, 1.0f) * 3.14159265f);
}

/** Catmull-Rom spline camera path (position + look target + fov) */
class CamPath {
public:
  std::vector<CamKey> keys;
  explicit CamPath(std::vector<CamKey> k) : keys(std::move(k)) {}

  struct Sample { V3 pos; V3 target; float fov; };

  Sample sampleEased(float t, float anticipation) const {
    const auto& k = keys;
    if (k.size() == 1) return {k[0].pos, k[0].target, k[0].fov};
    const float tt = clampf(t, 0, 1);
    int i = 0;
    while (i < (int)k.size() - 2 && tt > k[i + 1].t) i++;
    const CamKey& p0 = k[std::max(i - 1, 0)];
    const CamKey& p1 = k[i];
    const CamKey& p2 = k[std::min(i + 1, (int)k.size() - 1)];
    const CamKey& p3 = k[std::min(i + 2, (int)k.size() - 1)];
    const float span = std::max(p2.t - p1.t, 1e-5f);
    const float u = clampf((tt - p1.t) / span, 0, 1);
    // ease the span parameter u (not global t) so each segment gets its own anticipation
    const float ue = cineEase(u, anticipation * (i == (int)k.size() - 2 ? 1.0f : 0.0f));
    auto cat = [&](const V3& a, const V3& b, const V3& c, const V3& d, float p) -> V3 {
      const float p2 = p * p;
      const float p3 = p2 * p;
      const V3 A = vScale(vSub(c, a), 0.5f);
      const V3 B = vScale(vAdd(vSub(vScale(a, 2), vScale(b, 5)), vSub(vScale(c, 4), d)), 0.5f);
      const V3 C = vScale(vAdd(vSub(vScale(b, 3), a), vSub(d, vScale(c, 3))), 0.5f);
      return vAdd(vAdd(vAdd(b, vScale(A, p)), vScale(B, p2)), vScale(C, p3));
    };
    return {
      cat(p0.pos, p1.pos, p2.pos, p3.pos, ue),
      cat(p0.target, p1.target, p2.target, p3.target, ue),
      mixf(p1.fov, p2.fov, ue),
    };
  }
};

class Camera {
public:
  V3 pos{0, 0, 0};
  Q4 quat{0, 0, 0, 1};
  float fov = 62.0f * 3.14159265f / 180.0f;
  float nearP = 0.05f;
  float farP = 400.0f;

  // basis vectors (world space)
  V3 right{1, 0, 0};
  V3 up{0, 1, 0};
  V3 fwd{0, 0, -1};

  Mat4 view{};
  Mat4 proj{};

  float shakeAmp = 0;
  float shakeRoll = 0;
  float fovKick = 0;
  float handheld = 0.0f;
  float crashKick = 0;
  float dofFocus = 8;
  float dofAperture = 0;

  void resize(int w, int h) {
    aspect_ = (float)w / (float)h;
    updateMatrices();
  }

  void lookAt(const V3& target) {
    quat = quatFromLookAt(vSub(target, pos), V3{0, 1, 0});
  }

  void applyPathEased(const CamPath& p, float t, float anticipation) {
    const CamPath::Sample s = p.sampleEased(t, anticipation);
    pos = s.pos;
    fov = s.fov * 3.14159265f / 180.0f;
    lookAt(s.target);
  }

  void addShake(float amp) {
    shakeAmp = std::min(shakeAmp + amp, 2.5f);
    shakeRoll = fractf(shakeRoll + 0.37f);
  }

  void addCrashZoom(float k) {
    crashKick = std::min(crashKick + k, 0.5f);
  }

  void applyBeatZoom(float pulse, float amount = 0.05f) {
    fovKick += easeOut(pulse) * amount;
  }

  void update(float dt) {
    // decay shake + fov kicks
    shakeAmp *= std::pow(0.0008f, dt);
    if (shakeAmp < 0.0005f) shakeAmp = 0;
    fovKick *= std::pow(0.0012f, dt);
    if (fovKick < 0.0005f) fovKick = 0;
    crashKick *= std::pow(0.00005f, dt);
    if (crashKick < 0.001f) crashKick = 0;

    // build basis from quat (rows of the column-major matrix)
    const Mat3 m = mat3FromQuat(quat);
    right = {m[0], m[3], m[6]};
    up = {m[1], m[4], m[7]};
    fwd = {-m[2], -m[5], -m[8]};

    V3 p = pos;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // handheld drift
    if (handheld > 0.001f) {
      const float h = handheld;
      const V3 drift{
        (float)(std::sin(now * 0.71) * 0.5 + std::sin(now * 1.63 + 1.3) * 0.25),
        (float)(std::sin(now * 0.83 + 2.1) * 0.4 + std::sin(now * 1.37) * 0.2),
        (float)(std::sin(now * 0.59 + 0.7) * 0.35),
      };
      p = vAdd(p, vScale(drift, h * 0.03f));
    }

    if (shakeAmp > 0) {
      const float s = shakeAmp;
      const V3 shakeVec{
        (float)((std::sin(now * 61.8 + shakeRoll * 6.28) * 0.5 + std::sin(now * 23.7) * 0.5) * s),
        (float)((std::sin(now * 47.3 + 1.7) * 0.5 + std::sin(now * 19.1) * 0.5) * s),
        (float)(std::sin(now * 37.7 + 3.1) * 0.5 * s),
      };
      p = vAdd(p, shakeVec);
      const float roll = (float)std::sin(now * 29.3) * s * 0.06f;
      const float c = std::cos(roll);
      const float sn = std::sin(roll);
      right = vNorm(vAdd(vScale(right, c), vScale(vCross(fwd, right), sn)));
      up = vNorm(vCross(right, fwd));
    }

    view = mat4FromBasis(right, up, fwd, p);
    updateMatrices();
  }

private:
  float aspect_ = 1;

  void updateMatrices() {
    const float f = clampf(fov + fovKick - crashKick, 0.2f, 2.4f);
    proj = mat4Perspective(f, aspect_, nearP, farP);
  }
};

}  // namespace ns
