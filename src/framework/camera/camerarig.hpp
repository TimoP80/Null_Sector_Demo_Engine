// ---------------------------------------------------------------------------
// CameraRig - data-driven camera behaviors. The engine provides the camera
// MATH (splines, shake, DOF); the rig layer composes behaviors from data:
//
//   static    fixed pose (pos + target + fov)
//   drift     slow procedural float (awakening intro drift)
//   fly       forward flight through a tube (tunnel / machine shafts)
//   nave      gliding descent down a nave (cathedral)
//   orbit     orbit around a target center (neuralnet / synapse)
//   spiral    tighter, faster orbit (climax)
//   hover     near-stationary close-up with tiny drift (ghost close-up)
//   city      high flyover above a city grid (voxel)
//   descend   slow descending drift (deconstruction)
//   path      Catmull-Rom keyframed spline (full control)
//
// Rig parameters come from the script:
//   camera IntroCam { rig drift; pos (0,0,2.4); fov (50,64);
//                    buildUp (49,58); amp 2.8; freq 0.19; handheld (0.05,0.4) }
// where scalar options accept either a number or a (base,target) ramp driven
// by the build-up window.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/camera.hpp"
#include "framework/core/value.hpp"
#include "framework/script/scriptparser.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ns {

struct RigSample {
  V3 pos{0, 0, 0};
  V3 target{0, 0, 0};
  float fovDeg = 62.0f;
  float handheld = 0.0f;
  float dofFocus = 8.0f;
  float dofAperture = 0.0f;
};

class CameraRig {
public:
  std::string type = "static";

  // base parameters (all optional, sane defaults)
  V3 pos{0, 0, 0};
  V3 target{0, 0, 0};
  V3 sway{1, 1, 1};          // per-axis procedural sway amplitude
  float amp = 1.0f;          // build-up target amplitude (drift/hover)
  float ampBase = 1.0f;      // amplitude at build-up start
  float freq = 1.0f;         // procedural frequency target
  float freqBase = 0.0f;     // 0 = freq * 0.5
  float speed = 1.0f;        // forward speed (fly/city/nave/descend), units/s
  float radius = 4.5f;       // orbit radius
  float rDrift = 0.0f;       // orbit radius drift (0 = radius * 0.33)
  float omega = 1.0f;        // orbit angular speed (rad/s)
  float fov = 62.0f;
  float fovBase = 0.0f;      // 0 = no fov ramp
  float handheld = 0.0f;
  float handheldBase = 0.0f;
  float dofFocus = 8.0f;
  float dofAperture = 0.0f;
  float buildUpStart = -1.0f, buildUpEnd = -1.0f;  // ramp window in show seconds
  float descendCap = 7.0f;   // descend: max drop (world units)
  float descendRate = 0.45f; // descend: drop per second
  float anticipation = 0.0f; // path rig: pull-back near path end
  float pathDuration = 1.0f; // path rig: seconds for one loop of the spline

  // keyframed spline (rig type "path"): t in 0..1
  std::vector<CamKey> path;

  RigSample sample(float showT, float localT) const;

  /** apply the sampled pose to an engine Camera (pos/lookAt/fov/DOF) */
  void apply(Camera& cam, float showT, float localT) const {
    const RigSample s = sample(showT, localT);
    cam.pos = s.pos;
    cam.lookAt(s.target);
    cam.fov = s.fovDeg * 3.14159265f / 180.0f;
    cam.handheld = s.handheld;
    cam.dofFocus = s.dofFocus;
    cam.dofAperture = s.dofAperture;
  }

  /** build a rig from a script `camera NAME { ... }` command */
  static std::unique_ptr<CameraRig> fromCmd(const Cmd& cmd);
  /** build from a JSON object (scene files) */
  static std::unique_ptr<CameraRig> fromJson(const Value& v);
  Value toJson() const;

private:
  /** 0..1 build-up ramp (1 when no window configured) */
  float ramp(float showT) const;
  float rampParam(float base, float target, float u) const { return base + (target - base) * u; }
};

}  // namespace ns
