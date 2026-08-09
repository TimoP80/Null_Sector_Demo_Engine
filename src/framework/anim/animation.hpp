// ---------------------------------------------------------------------------
// AnimationSystem - keyframe animation for anything addressable by a property
// path: camera (fov/pos/target), scene nodes (position/rotation/scale/color/
// opacity), lights (intensity/color), effects (shader uniforms), post params
// and text (values). Fully data-driven: animations are built from script
// blocks or JSON, played/stopped by timeline events, and sampled per frame.
//
//   anim camera.fov cubic { 0 50; 8 64; 16 58 }      // one channel
//   anim camera.pos {                                 // multiple channels
//       0 (0,0,2.4) linear
//       8 (1,0,2.4) smooth
//   }
//
// Interpolators: linear, smooth (smoothstep), cubic (Catmull-Rom),
// bezier (cubic bezier with neighbor-derived handles), ease-in, ease-out,
// ease-in-out, bounce, elastic.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/value.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns {

enum class Interp {
  Linear, Smooth, Cubic, Bezier, EaseIn, EaseOut, EaseInOut, Bounce, Elastic,
};

Interp parseInterp(const std::string& name);
const char* interpName(Interp i);

struct AnimKey {
  float t = 0;
  Value v;              // scalar or vector value at the key
  Interp interp = Interp::Linear;
};

struct AnimChannel {
  std::string target;    // camera | effect:name | node:name | light:name | post | text:name | particle:name
  std::string property;  // fov | pos | rot | scale | color | opacity | intensity | bloom | uniform:uX ...
  std::vector<AnimKey> keys;
};

struct Animation {
  std::string name;
  float duration = 0;
  bool loop = false;
  std::vector<AnimChannel> channels;
};

/** one sampled value per frame (target.property = value) */
struct AnimSample {
  std::string target;
  std::string property;
  Value value;
};

class AnimationSystem {
public:
  // --- library --------------------------------------------------------------
  std::map<std::string, std::shared_ptr<Animation>> library;
  void add(std::shared_ptr<Animation> a);
  std::shared_ptr<Animation> get(const std::string& name) const;

  // --- runtime --------------------------------------------------------------
  void play(const std::string& name, float startT = 0, float speed = 1.0f);
  void stop(const std::string& name);
  void stopTarget(const std::string& target);
  void stopAll();
  bool isPlaying(const std::string& name) const;
  void seek(const std::string& name, float t);

  /** advance active animations; appends samples (drain via consumeSamples) */
  void update(float dt);
  const std::vector<AnimSample>& samples() const { return samples_; }
  void consumeSamples() { samples_.clear(); }

  // --- evaluation (static, reusable) ----------------------------------------
  /** interpolate one scalar over [a, b] with the given easing */
  static float interpValue(Interp i, float t01, float a, float b);

  /** sample a channel at time t into out[] (maxN floats); returns count.
   *  Scalar keys produce 1 float, vector keys produce N. Out of range clamps
   *  to the nearest key. */
  static int sampleChannel(const AnimChannel& ch, float t, float* out, int maxN);

  /** build a Value (array of floats) from a channel sample - convenience */
  static Value sampleValue(const AnimChannel& ch, float t);

  /** sample with the per-key interp of the segment destination key */
  static Interp segmentInterp(const AnimChannel& ch, size_t i);

private:
  struct Active {
    std::shared_ptr<Animation> anim;
    float t = 0;
    float speed = 1.0f;
    bool loop = false;
  };
  std::vector<Active> active_;
  std::vector<AnimSample> samples_;

  static bool keysEqualSize(const AnimKey& a, const AnimKey& b);
};

}  // namespace ns
