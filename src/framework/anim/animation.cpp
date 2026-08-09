#include "framework/anim/animation.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace ns {

// ---------------------------------------------------------------------------
// interpolators
// ---------------------------------------------------------------------------
Interp parseInterp(const std::string& name) {
  if (name == "smooth" || name == "smoothstep") return Interp::Smooth;
  if (name == "cubic") return Interp::Cubic;
  if (name == "bezier") return Interp::Bezier;
  if (name == "ease-in") return Interp::EaseIn;
  if (name == "ease-out") return Interp::EaseOut;
  if (name == "ease-in-out") return Interp::EaseInOut;
  if (name == "bounce") return Interp::Bounce;
  if (name == "elastic") return Interp::Elastic;
  return Interp::Linear;
}

const char* interpName(Interp i) {
  switch (i) {
    case Interp::Linear: return "linear";
    case Interp::Smooth: return "smooth";
    case Interp::Cubic: return "cubic";
    case Interp::Bezier: return "bezier";
    case Interp::EaseIn: return "ease-in";
    case Interp::EaseOut: return "ease-out";
    case Interp::EaseInOut: return "ease-in-out";
    case Interp::Bounce: return "bounce";
    case Interp::Elastic: return "elastic";
  }
  return "linear";
}

namespace {
float easeOutBack(float t) { return 1.0f + 2.70158f * std::pow(t - 1.0f, 3.0f) + 1.70158f * std::pow(t - 1.0f, 2.0f); }
float easeOutBounce(float t) {
  const float n1 = 7.5625f, d1 = 2.75f;
  if (t < 1.0f / d1) return n1 * t * t;
  if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
  if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
  t -= 2.625f / d1;
  return n1 * t * t + 0.984375f;
}
float easeOutElastic(float t) {
  const float c4 = 2.0943951f;  // 2*pi/3
  return t <= 0 ? 0 : t >= 1 ? 1 : std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}
}  // namespace

float AnimationSystem::interpValue(Interp i, float t01, float a, float b) {
  const float t = std::max(0.0f, std::min(1.0f, t01));
  switch (i) {
    case Interp::Linear: return a + (b - a) * t;
    case Interp::Smooth: return a + (b - a) * (t * t * (3.0f - 2.0f * t));
    case Interp::EaseIn: return a + (b - a) * t * t * t;
    case Interp::EaseOut: return a + (b - a) * (1.0f - std::pow(1.0f - t, 3.0f));
    case Interp::EaseInOut: return a + (b - a) * (t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f);
    case Interp::Bounce: return a + (b - a) * easeOutBounce(t);
    case Interp::Elastic: return a + (b - a) * easeOutElastic(t);
  }
  return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------
void AnimationSystem::add(std::shared_ptr<Animation> a) {
  if (a) library[a->name] = std::move(a);
}

std::shared_ptr<Animation> AnimationSystem::get(const std::string& name) const {
  auto it = library.find(name);
  return it != library.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// runtime
// ---------------------------------------------------------------------------
void AnimationSystem::play(const std::string& name, float startT, float speed) {
  auto anim = get(name);
  if (!anim) {
    Log::warn("ANIM", "play: unknown animation '" + name + "'");
    return;
  }
  // restart if already active
  for (auto& a : active_) {
    if (a.anim->name == name) {
      a.t = startT;
      a.speed = speed;
      a.loop = anim->loop;
      return;
    }
  }
  active_.push_back({anim, startT, speed, anim->loop});
}

void AnimationSystem::stop(const std::string& name) {
  active_.erase(std::remove_if(active_.begin(), active_.end(),
                               [&](const Active& a) { return a.anim->name == name; }),
                active_.end());
}

void AnimationSystem::stopTarget(const std::string& target) {
  active_.erase(std::remove_if(active_.begin(), active_.end(),
                               [&](const Active& a) {
                                 for (const auto& c : a.anim->channels)
                                   if (c.target == target) return true;
                                 return false;
                               }),
                active_.end());
}

void AnimationSystem::stopAll() { active_.clear(); }

bool AnimationSystem::isPlaying(const std::string& name) const {
  for (const auto& a : active_) if (a.anim->name == name) return true;
  return false;
}

void AnimationSystem::seek(const std::string& name, float t) {
  for (auto& a : active_) if (a.anim->name == name) a.t = t;
}

void AnimationSystem::update(float dt) {
  std::vector<Active> keep;
  keep.reserve(active_.size());
  for (auto& a : active_) {
    a.t += dt * a.speed;
    const float dur = a.anim->duration > 0 ? a.anim->duration : 0;
    if (dur > 0 && a.t >= dur) {
      if (a.loop) {
        a.t = std::fmod(a.t, dur);
      } else {
        a.t = dur;  // sample the last key exactly once, then retire
        for (const auto& ch : a.anim->channels) {
          float tmp[8];
          const int n = sampleChannel(ch, a.t, tmp, 8);
          if (n == 0) continue;
          Value arr = Value::array();
          for (int k = 0; k < n; k++) arr.push(Value((double)tmp[k]));
          samples_.push_back({ch.target, ch.property, std::move(arr)});
        }
        continue;  // finished
      }
    }
    for (const auto& ch : a.anim->channels) {
      float tmp[8];
      const int n = sampleChannel(ch, a.t, tmp, 8);
      if (n == 0) continue;
      Value arr = Value::array();
      for (int k = 0; k < n; k++) arr.push(Value((double)tmp[k]));
      samples_.push_back({ch.target, ch.property, std::move(arr)});
    }
    keep.push_back(a);
  }
  active_ = std::move(keep);
}

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------
bool AnimationSystem::keysEqualSize(const AnimKey& a, const AnimKey& b) {
  const int na = (int)a.v.size();
  const int nb = (int)b.v.size();
  return (na == nb && na > 0) || (a.v.isNum() && b.v.isNum());
}

Interp AnimationSystem::segmentInterp(const AnimChannel& ch, size_t i) {
  // the segment into key i+1 is governed by key i+1's interp (falls back to
  // the segment start key, then linear)
  if (i + 1 < ch.keys.size() && ch.keys[i + 1].interp != Interp::Linear) return ch.keys[i + 1].interp;
  if (ch.keys[i].interp != Interp::Linear) return ch.keys[i].interp;
  return Interp::Linear;
}

static float catmull(float p0, float p1, float p2, float p3, float t) {
  const float t2 = t * t, t3 = t2 * t;
  return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                 (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

int AnimationSystem::sampleChannel(const AnimChannel& ch, float t, float* out, int maxN) {
  const size_t n = ch.keys.size();
  if (n == 0) return 0;
  // clamp outside the key range
  if (t <= ch.keys[0].t) {
    return ch.keys[0].v.toFloats(out, maxN);
  }
  if (t >= ch.keys[n - 1].t) {
    return ch.keys[n - 1].v.toFloats(out, maxN);
  }
  // find the segment [i, i+1]
  size_t i = 0;
  while (i + 1 < n && ch.keys[i + 1].t <= t) i++;
  const AnimKey& k0 = ch.keys[i];
  const AnimKey& k1 = ch.keys[i + 1];
  const float span = std::max(k1.t - k0.t, 1e-5f);
  const float u = (t - k0.t) / span;
  const Interp ip = segmentInterp(ch, i);

  // component count: both keys must agree in shape (scalar or vector)
  float a[8], b[8];
  const int na = k0.v.toFloats(a, 8);
  const int nb = k1.v.toFloats(b, 8);
  const int comps = std::min({na, nb, maxN});
  if (comps <= 0) return 0;

  if (ip == Interp::Cubic) {
    // Catmull-Rom across neighbours (clamped at the ends)
    const AnimKey& pm = ch.keys[std::max(i, (size_t)1) - 1];
    const AnimKey& pn = ch.keys[std::min(i + 2, n - 1)];
    float am[8], an[8];
    const int nm = pm.v.toFloats(am, 8);
    const int nn = pn.v.toFloats(an, 8);
    for (int c = 0; c < comps; c++) {
      const float p0 = c < nm ? am[c] : a[c];
      const float p3 = c < nn ? an[c] : b[c];
      out[c] = catmull(p0, a[c], b[c], p3, u);
    }
    return comps;
  }
  if (ip == Interp::Bezier) {
    // cubic bezier with handles derived from the neighbours (smooth chain)
    const AnimKey& pm = ch.keys[std::max(i, (size_t)1) - 1];
    const AnimKey& pn = ch.keys[std::min(i + 2, n - 1)];
    float am[8], an[8];
    const int nm = pm.v.toFloats(am, 8);
    const int nn = pn.v.toFloats(an, 8);
    const float u2 = u * u, u3 = u2 * u;
    for (int c = 0; c < comps; c++) {
      const float p0 = c < nm ? am[c] : a[c];
      const float p3 = c < nn ? an[c] : b[c];
      const float c1 = a[c] + (b[c] - p0) / 6.0f;
      const float c2 = b[c] - (p3 - a[c]) / 6.0f;
      out[c] = (1 - u) * (1 - u) * (1 - u) * a[c] + 3 * (1 - u) * (1 - u) * u * c1 +
               3 * (1 - u) * u * u * c2 + u3 * b[c];
    }
    return comps;
  }
  for (int c = 0; c < comps; c++) {
    out[c] = interpValue(ip, u, a[c], b[c]);
  }
  return comps;
}

Value AnimationSystem::sampleValue(const AnimChannel& ch, float t) {
  float tmp[8];
  const int n = sampleChannel(ch, t, tmp, 8);
  Value arr = Value::array();
  for (int k = 0; k < n; k++) arr.push(Value((double)tmp[k]));
  return arr;
}

}  // namespace ns
