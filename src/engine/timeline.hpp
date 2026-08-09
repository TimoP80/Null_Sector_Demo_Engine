// ---------------------------------------------------------------------------
// Timeline: beat/bar/section clock driving every visual.
// Port of src/engine/timeline.ts.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/math.hpp"
#include "engine/schedule.hpp"
#include <algorithm>

namespace ns {

struct TimelineState {
  float time = 0;
  float beat = 0;
  float bar = 0;
  float beatPhase = 0;
  float barPhase = 0;
  float beatPulse = 0;
  float barPulse = 0;
  float drop = 0;
  float intensity = 0;
  SectionInfo section;
  float sectionProgress = 0;
  float sectionLocal = 0;
  int sectionIdx = 0;
  float transition = 0;
  float duration = 0;
  /** 4-bar musical phrase index of the whole show */
  int phrase = 0;
  /** 0..1 anticipation ramp before a section boundary */
  float anticipation = 0;
  /** 0..1 easing-out at the end of a section */
  float exitRamp = 0;
  /** narrative chapter index 0..5 */
  int chapter = 0;
  /** musical chord hue for the current bar (0..1 palVoid space) */
  float musicHue = 0;
  /** chord hue of the next bar (for smooth in-bar interpolation) */
  float musicHue2 = 0;
  /** 0..1 per-kick assembly ratchet. Written into the shared UBO as
   *  uAssembly each frame. ubo.cpp edge-detects the audio kick analyser and
   *  increments this on real kick hits: the cathedral assembles and the
   *  Infinite Machine revs up; the reprise decays it back toward 0 as the
   *  ghost deconstructs the structure. */
  float assembly = 0;
};

class Timeline {
public:
  float time = 0;
  TimelineState s;

  /** replace the hardcoded schedule with a data-driven one (from a demo
   *  script's scene activations). The beat/bar grid and section math then
   *  follow the external data, so a show can be re-timed without recompiling. */
  void setSections(std::vector<SectionInfo> secs) {
    custom_ = std::move(secs);
    useCustom_ = true;
  }
  void clearSections() { useCustom_ = false; }
  /** the active section list (custom or the built-in schedule) */
  const std::vector<SectionInfo>& sections() const {
    return useCustom_ ? custom_ : SECTION_INFO();
  }
  /** data-driven tempo (defaults to the built-in 216 BPM) */
  void setBpm(float bpm) {
    beat_ = 60.0f / (bpm > 1.0f ? bpm : 216.0f);
    bar_ = beat_ * 4.0f;
  }
  float beatSec() const { return beat_; }
  float barSec() const { return bar_; }

  /** advance the clock; called once per frame with current show time */
  void advance(float t) {
    time = t;
    const float beat = t / beat_;
    const float bar = t / bar_;

    const auto& secs = sections();
    int idx = 0;
    for (int i = 0; i < (int)secs.size(); i++) {
      if (t >= secs[i].startSec) idx = i;
      else break;
    }
    const SectionInfo& sec = secs[idx];
    const float local = clampf(t - sec.startSec, 0, sec.duration);
    const float progress = sec.duration > 0 ? local / sec.duration : 0;

    // intensity with smooth ramps into / out of sections
    const SectionInfo& prev = idx > 0 ? secs[idx - 1] : sec;
    const float inT = smoothstepf(sec.startSec - 1.2f, sec.startSec + 1.2f, t);
    const float outT = smoothstepf(sec.endSec - 1.2f, sec.endSec + 1.2f, t);
    const float intensity = mixf(prev.intensity, sec.intensity, inT) * (1 - outT * 0.85f);

    // transition (first two beats of a section)
    const float trans = clampf((t - sec.startSec) / (2 * beat_), 0, 1);
    const float drop = std::pow(std::max(0.0f, 1 - (t - sec.startSec) / (4 * beat_)), 3);

    // anticipation: the beat before a boundary winds up
    const float anticipation = smoothstepf(sec.endSec - 1.4f, sec.endSec, t);
    // exitRamp: final moments of a section (melt / handoff transitions)
    const float exitRamp = smoothstepf(sec.endSec - 3.2f, sec.endSec - 0.6f, t);

    s.time = t;
    s.beat = beat;
    s.bar = bar;

    s.beatPhase = fractf(beat);
    s.barPhase = fractf(bar);
    s.beatPulse = std::pow(clampf(1 - fractf(beat), 0, 1), 3);
    s.barPulse = std::pow(clampf(1 - fractf(bar), 0, 1), 2);
    s.drop = drop;
    s.intensity = intensity;
    s.section = sec;
    s.sectionProgress = progress;
    s.sectionLocal = local;
    s.sectionIdx = idx;
    s.transition = trans;
    s.duration = sec.duration;
    s.phrase = (int)std::floor(bar / 4);
    s.anticipation = anticipation;
    s.exitRamp = exitRamp;
    s.chapter = sec.chapter;

    // chord progression: current bar's hue + next bar's hue (s.bar/s.barPhase
    // are the absolute bar count + in-bar fraction, computed above)
    const int barIdx = (int)std::floor(s.bar);
    s.musicHue = chordHue(barIdx);
    s.musicHue2 = chordHue(barIdx + 1);  }

private:
  std::vector<SectionInfo> custom_;
  bool useCustom_ = false;
  float beat_ = ns::BEAT;
  float bar_ = ns::BAR;
};

}  // namespace ns
