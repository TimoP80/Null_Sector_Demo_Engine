#include "framework/camera/camerarig.hpp"
#include "framework/core/log.hpp"

#include <cmath>

namespace ns {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {
void readVec3(const Value& v, V3& out, const V3& dflt) {
  float f[3];
  const int n = v.toFloats(f, 3);
  if (n == 3) out = {f[0], f[1], f[2]};
  else if (n > 0) out = {f[0], f[0], f[0]};
  else out = dflt;
}
/** scalar or (base,target) ramp option */
void readRamp(const Value& v, float& single, float& base) {
  float f[2];
  const int n = v.toFloats(f, 2);
  if (n >= 2) { single = f[1]; base = f[0]; }
  else if (n == 1) { single = f[0]; base = 0.0f; }
}
}  // namespace

float CameraRig::ramp(float showT) const {
  if (buildUpEnd > buildUpStart) {
    const float u = satf((showT - buildUpStart) / (buildUpEnd - buildUpStart));
    return u * u * (3.0f - 2.0f * u);
  }
  return 1.0f;
}

RigSample CameraRig::sample(float showT, float localT) const {
  RigSample s;
  const float u = ramp(showT);
  const float ampEff = rampParam(ampBase, amp, u);
  const float freqEff = rampParam(freqBase > 0 ? freqBase : freq * 0.5f, freq, u);
  const float fovEff = fovBase > 0 ? rampParam(fovBase, fov, u) : fov;
  const float handEff = handheldBase > 0 ? rampParam(handheldBase, handheld, u) : handheld;

  s.fovDeg = fovEff;
  s.handheld = handEff;
  s.dofFocus = dofFocus;
  s.dofAperture = dofAperture;
  s.target = target;

  if (type == "drift") {
    s.pos = {pos[0] + ampEff * sway[0] * std::sin(showT * freqEff),
             pos[1] + ampEff * sway[1] * std::cos(showT * freqEff * 0.85f),
             pos[2]};
    s.target = target;
  } else if (type == "fly") {
    s.pos = {sway[0] * std::sin(showT * freqEff),
             sway[1] * std::sin(showT * freqEff * 1.2f),
             pos[2] - localT * speed};
    s.target = {s.pos[0], s.pos[1], s.pos[2] - 1.0f};
  } else if (type == "nave") {
    s.pos = {sway[0] * std::sin(showT * freqEff),
             pos[1] + sway[1] * std::sin(showT * freqEff * 0.95f),
             pos[2] - localT * speed};
    s.target = {0.0f, 5.5f, s.pos[2] - 12.0f};
  } else if (type == "orbit" || type == "spiral") {
    const float om = type == "spiral" ? omega * 1.4f : omega;
    const float rd = rDrift > 0 ? rDrift : radius * (type == "spiral" ? 0.4f : 0.33f);
    const float r = radius + rd * std::sin(localT * 0.2f);
    const float angle = localT * om;
    s.pos = {r * std::cos(angle),
             pos[1] + sway[1] * std::sin(localT * 0.18f),
             r * std::sin(angle)};
    s.target = target;
  } else if (type == "hover") {
    s.pos = {pos[0] + sway[0] * std::sin(showT * 0.12f),
             pos[1] + sway[1] * std::sin(showT * 0.09f),
             pos[2]};
    s.target = target;
  } else if (type == "city") {
    s.pos = {sway[0] * std::sin(showT * 0.09f),
             pos[1] + sway[1] * std::sin(showT * 0.15f),
             pos[2] - localT * speed};
    s.target = {0.0f, 8.0f, s.pos[2] - 12.0f};
  } else if (type == "descend") {
    s.pos = {sway[0] * std::sin(showT * 0.18f),
             pos[1] - std::min(localT, descendCap) * descendRate,
             pos[2] - localT * speed};
    s.target = {0.0f, 5.0f, s.pos[2] - 10.0f};
  } else if (type == "path") {
    if (!path.empty()) {
      CamPath p(path);
      const float t = pathDuration > 0 ? localT / pathDuration : 0.0f;
      const CamPath::Sample sm = p.sampleEased(t, anticipation);
      s.pos = sm.pos;
      s.target = sm.target;
      s.fovDeg = sm.fov;
    } else {
      s.pos = pos;
      s.target = target;
    }
  } else {  // static
    s.pos = pos;
    s.target = target;
  }
  return s;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------
static void readBaseParams(CameraRig& r, const Value& opts) {
  const V3 zero{0, 0, 0};
  readVec3(opts.get("pos"), r.pos, zero);
  readVec3(opts.get("target"), r.target, zero);
  readVec3(opts.get("sway"), r.sway, V3{1, 1, 1});
  readRamp(opts.get("fov"), r.fov, r.fovBase);
  readRamp(opts.get("handheld"), r.handheld, r.handheldBase);
  readRamp(opts.get("amp"), r.amp, r.ampBase);
  readRamp(opts.get("freq"), r.freq, r.freqBase);
  r.speed = opts.get("speed").asFloat(r.speed);
  r.radius = opts.get("radius").asFloat(r.radius);
  r.rDrift = opts.get("rDrift").asFloat(r.rDrift);
  r.omega = opts.get("omega").asFloat(r.omega);
  r.dofFocus = opts.get("dofFocus").asFloat(r.dofFocus);
  r.dofAperture = opts.get("dofAperture").asFloat(r.dofAperture);
  r.descendCap = opts.get("descendCap").asFloat(r.descendCap);
  r.descendRate = opts.get("descendRate").asFloat(r.descendRate);
  r.anticipation = opts.get("anticipation").asFloat(r.anticipation);
  r.pathDuration = opts.get("pathDuration").asFloat(r.pathDuration);
  const Value& bu = opts.get("buildUp");
  if (!bu.isNull()) {
    float f[2];
    const int n = bu.toFloats(f, 2);
    if (n == 2) { r.buildUpStart = f[0]; r.buildUpEnd = f[1]; }
  }
}

std::unique_ptr<CameraRig> CameraRig::fromCmd(const Cmd& cmd) {
  auto r = std::make_unique<CameraRig>();
  r->type = cmd.opts.get("rig").asStr(cmd.s("type", "static"));
  readBaseParams(*r, cmd.opts);

  // path spline keys: each row = t (pos) (target) fov
  for (const auto& k : cmd.keys) {
    CamKey ck;
    ck.t = k.t;
    const Value& v = k.v;
    if (v.isArr() && v.size() >= 2) {
      readVec3(v.atIndex(0), ck.pos, r->pos);
      readVec3(v.atIndex(1), ck.target, r->target);
      ck.fov = v.atIndex(2).asFloat(r->fov);
    } else {
      readVec3(v, ck.pos, r->pos);
      ck.target = r->target;
      ck.fov = r->fov;
    }
    r->path.push_back(ck);
  }
  return r;
}

std::unique_ptr<CameraRig> CameraRig::fromJson(const Value& v) {
  auto r = std::make_unique<CameraRig>();
  r->type = v.get("type").asStr("static");
  readBaseParams(*r, v);
  for (const auto& p : v.get("path").asArr()) {
    CamKey ck;
    ck.t = (float)p.get("t").asNum();
    readVec3(p.get("pos"), ck.pos, r->pos);
    readVec3(p.get("target"), ck.target, r->target);
    ck.fov = (float)p.get("fov").asNum(r->fov);
    r->path.push_back(ck);
  }
  return r;
}

Value CameraRig::toJson() const {
  Value o = Value::object();
  o.set("type") = Value(type);
  Value p = Value::array();
  for (float v : pos) p.push(Value((double)v));
  o.set("pos") = std::move(p);
  Value t = Value::array();
  for (float v : target) t.push(Value((double)v));
  o.set("target") = std::move(t);
  Value sw = Value::array();
  for (float v : sway) sw.push(Value((double)v));
  o.set("sway") = std::move(sw);
  o.set("amp") = Value((double)amp);
  o.set("freq") = Value((double)freq);
  o.set("speed") = Value((double)speed);
  o.set("radius") = Value((double)radius);
  o.set("omega") = Value((double)omega);
  o.set("fov") = Value((double)fov);
  o.set("handheld") = Value((double)handheld);
  o.set("dofFocus") = Value((double)dofFocus);
  o.set("dofAperture") = Value((double)dofAperture);
  if (buildUpEnd > buildUpStart) {
    Value b = Value::array();
    b.push(Value((double)buildUpStart));
    b.push(Value((double)buildUpEnd));
    o.set("buildUp") = std::move(b);
  }
  Value pk = Value::array();
  for (const auto& ck : path) {
    Value k = Value::object();
    k.set("t") = Value((double)ck.t);
    Value pp = Value::array();
    for (float v : ck.pos) pp.push(Value((double)v));
    k.set("pos") = std::move(pp);
    Value tt = Value::array();
    for (float v : ck.target) tt.push(Value((double)v));
    k.set("target") = std::move(tt);
    k.set("fov") = Value((double)ck.fov);
    pk.push(std::move(k));
  }
  o.set("path") = std::move(pk);
  return o;
}

}  // namespace ns
