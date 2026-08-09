// ---------------------------------------------------------------------------
// DirectorTime - hidden rehearsal layer over the show clock.
// Port of src/engine/directortime.ts. Audio keeps playing (its live analysis
// keeps driving the visuals); this layer transforms the raw audio clock into
// the show clock feeding the timeline.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/schedule.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ns {

class DirectorTime {
public:
  float show = 0;
  bool paused = false;
  bool quantize = false;
  bool loop = false;
  float loopStart = 0, loopEnd = 0;
  std::vector<float> marks;  // cue points (sorted)
  int cue = -1;

  void init(float t) {
    show = std::max(0.0f, std::min(t, ns::TOTAL_SECONDS()));
  }

  void advance(float audioNow) {
    if (!primed_) {
      primed_ = true;
      lastAudio_ = audioNow;
      return;
    }
    const float delta = audioNow - lastAudio_;
    lastAudio_ = audioNow;
    if (!paused) {
      const float prev = show;
      show += delta * scale_;
      if (loop && loopEnd > loopStart && prev < loopEnd && show >= loopEnd) {
        const float span = loopEnd - loopStart;
        show = loopStart + std::fmod(show - loopEnd, span);
      }
    }
    // keep cue highlight fresh
    cue = cueAt(show);
  }

  void togglePause() { paused = !paused; }
  void scrub(float delta) {
    float s = show + delta;
    if (quantize) s = std::round(s / SIXTEENTH) * SIXTEENTH;
    show = std::max(0.0f, std::min(s, ns::TOTAL_SECONDS()));
  }
  void scrubBar(int dir) {
    const float b = show / ns::BAR;
    const float target = dir > 0 ? std::floor(b) + 1 : std::ceil(b) - 1;
    show = std::max(0.0f, std::min(target * ns::BAR, ns::TOTAL_SECONDS()));
  }
  void toggleQuantize() {
    quantize = !quantize;
    if (quantize) show = std::max(0.0f, std::min(std::round(show / SIXTEENTH) * SIXTEENTH, ns::TOTAL_SECONDS()));
  }
  void jumpToSection(int offset) {
    const SectionInfo& cur = ns::sectionAt(show);
    const int idx = std::max(0, std::min((int)ns::SECTION_INFO().size() - 1, cur.index + offset));
    show = ns::SECTION_INFO()[idx].startSec;
  }
  void mark() {
    for (float m : marks) if (std::abs(m - show) < 0.5f) return;
    marks.push_back(show);
    std::sort(marks.begin(), marks.end());
  }
  void cycleCue(int dir) {
    if (marks.empty()) return;
    int next = -1;
    if (dir > 0) {
      for (int i = 0; i < (int)marks.size(); i++) if (marks[i] > show + 0.25f) { next = i; break; }
      if (next < 0) next = 0;
    } else {
      for (int i = (int)marks.size() - 1; i >= 0; i--) if (marks[i] < show - 0.25f) { next = i; break; }
      if (next < 0) next = (int)marks.size() - 1;
    }
    show = marks[next];
  }
  void clearCues() { marks.clear(); }
  void toggleLoop() {
    if (loop) { loop = false; return; }
    const SectionInfo& sec = ns::sectionAt(show);
    loop = true;
    loopStart = sec.startSec;
    loopEnd = sec.endSec;
    if (show >= loopEnd) show = loopStart;
  }
  void setScale(float s) { scale_ = s; }
  float scale() const { return scale_; }

  // keep this out of the header's translation-unit-scope: fixed 16th grid
  static constexpr float SIXTEENTH = ns::BEAT / 4.0f;

private:
  bool primed_ = false;
  float lastAudio_ = 0;
  float scale_ = 1.0f;

  int cueAt(float t) const {
    for (int i = 0; i < (int)marks.size(); i++) {
      if (std::abs(marks[i] - t) <= 0.5f) return i;
    }
    return -1;
  }
};

}  // namespace ns
