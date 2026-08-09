// ---------------------------------------------------------------------------
// RehearsalLog - records a pass (boundary crossings, transports, notes) and
// prints a cue sheet / A/B diff to stdout. Console subset of the web build's
// JSON export.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ns {

struct LogEvent {
  std::string kind;     // boundary | transport | camera | note
  float show = 0;
  double wall = 0;
  std::string section;
  float intensity = 0;
  float onset = 0, kick = 0, energy = 0, bass = 0, peak = 0;
  std::string landing;  // HIT / FLOATED
  std::string label;    // note label
};

class RehearsalLog {
public:
  bool recording = false;
  std::string label = "GOOD";
  int count = 0;
  bool hasCompare = false;

  void begin() {
    recording = true;
    count = 0;
    events_.clear();
    std::printf("[REHEARSAL] pass started\n");
  }

  void end() {
    recording = false;
    std::printf("[REHEARSAL] pass ended (%d events)\n", (int)events_.size());
  }

  void logBoundary(float show, const std::string& section, float intensity,
                   float onset, float kick, float energy, float bass, float peak,
                   const std::string& landing) {
    if (!recording) return;
    LogEvent e;
    e.kind = "boundary"; e.show = show; e.wall = nowWall();
    e.section = section; e.intensity = intensity;
    e.onset = onset; e.kick = kick; e.energy = energy; e.bass = bass; e.peak = peak;
    e.landing = landing;
    events_.push_back(e);
    count++;
  }

  void logTransport(float show, const std::string& action, float value = 0) {
    if (!recording) return;
    LogEvent e;
    e.kind = "transport"; e.show = show; e.wall = nowWall();
    e.label = action + "=" + std::to_string(value);
    events_.push_back(e);
    count++;
  }

  void note(float show) {
    if (!recording) return;
    LogEvent e;
    e.kind = "note"; e.show = show; e.wall = nowWall();
    e.label = label;
    events_.push_back(e);
    count++;
  }

  void cycleLabel(int dir) {
    static const char* LABELS[] = {"GOOD", "RETAKE", "LATE", "HOLD"};
    static int idx = 0;
    idx = (idx + dir + 4) % 4;
    label = LABELS[idx];
  }

  /** dump the cue sheet to stdout */
  void exportSheet() {
    std::printf("--- CUE SHEET (%d events) ---\n", (int)events_.size());
    for (const auto& e : events_) {
      std::printf("t=%7.2f  %-9s  %s", e.show, e.kind.c_str(), e.section.c_str());
      if (e.kind == "boundary") {
        std::printf("  int=%.2f onset=%.2f kick=%.2f en=%.2f -> %s", e.intensity, e.onset, e.kick, e.energy, e.landing.c_str());
      } else if (e.kind == "note") {
        std::printf("  [%s]", e.label.c_str());
      } else {
        std::printf("  %s", e.label.c_str());
      }
      std::printf("\n");
    }
    std::printf("--- end sheet ---\n");
  }

  /** A/B compare: diff the current pass against a stored reference pass */
  void compare(RehearsalLog* other) {
    hasCompare = true;
    std::printf("--- A/B DIFF (this vs stored) ---\n");
    size_t n = std::min(events_.size(), other ? other->events_.size() : 0);
    for (size_t i = 0; i < n; i++) {
      const auto& a = events_[i];
      const auto& b = other->events_[i];
      if (a.kind == "boundary" && b.kind == "boundary") {
        const float dt = a.show - b.show;
        std::printf("boundary %2zu  this=%7.2f  stored=%7.2f  d=%+5.2f  %s\n",
                    i, a.show, b.show, dt,
                    a.landing == b.landing ? a.landing.c_str() : (a.landing + "/" + b.landing).c_str());
      }
    }
    std::printf("--- end diff ---\n");
  }

  RehearsalLog* stored = nullptr;  // previous pass for A/B

private:
  std::vector<LogEvent> events_;
  static double nowWall() {
    return 0;  // wall clock optional in this port; show clock is authoritative
  }
};

}  // namespace ns
