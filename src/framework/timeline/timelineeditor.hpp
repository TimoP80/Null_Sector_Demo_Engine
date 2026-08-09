// ---------------------------------------------------------------------------
// TimelineEditor - the timeline backend for the data-driven director.
//
//   tracks    named lanes (effects / camera / post / audio / animation / event)
//   events    scheduled commands (what the script's `at` blocks become) with
//             start time + duration; one-shot or repeating (loopEvery)
//   clips     named regions that group events (offset moves them together)
//   markers   named cue points (for scrubbing / rehearsal)
//   keyframes belong to the AnimationSystem; the timeline starts/stops them
//
// Transport: play / pause / seek / jump / loop / speed, all scriptable and
// keyboard-drivable. Effects start and stop automatically: a `show X` event
// fires when its time is crossed, `hide X` stops it.
//
// Events are kept sorted; each frame advance() returns the events whose time
// fell inside the crossed window, so the director reacts exactly once per
// event regardless of frame rate. Scrubbing backwards re-arms the boundary so
// events are not re-fired until they are genuinely re-crossed.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/value.hpp"
#include "framework/script/scriptparser.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ns {

struct TimelineTrack {
  std::string name;
  std::string type;    // effect | camera | post | audio | animation | event
  bool muted = false;
};

struct TimelineEvent {
  std::string name;
  float time = 0;
  float duration = 0;
  std::vector<Cmd> cmds;    // fired when the event triggers
  int track = -1;           // index into tracks (or -1)
  std::string clip;         // owning clip name ("" = none)
  float loopEvery = 0;      // >0 repeats every N seconds
  bool enabled = true;
};

struct TimelineMarker {
  std::string name;
  float time = 0;
};

struct TimelineClip {
  std::string name;
  float start = 0;
  float end = 0;
  int track = -1;
};

class TimelineEditor {
public:
  // --- data -----------------------------------------------------------------
  std::vector<TimelineTrack> tracks;
  std::vector<TimelineEvent> events;   // kept sorted by time
  std::vector<TimelineMarker> markers;
  std::vector<TimelineClip> clips;

  // --- transport state ------------------------------------------------------
  float time = 0;
  float duration = 0;      // total show duration (seconds)
  float speed = 1.0f;
  bool playing = false;
  bool looping = false;
  float loopStart = 0, loopEnd = 0;

  // --- transport controls -----------------------------------------------------
  void play();
  void pause();
  void toggle();
  /** seek to an absolute time; refires nothing (see header comment) */
  void seek(float t);
  /** jump is an alias for seek with an optional "force" (used by cue jumps) */
  void jump(float t) { seek(t); }
  void setSpeed(float s) { speed = s; }
  void setLoop(bool on, float start = 0, float end = 0);
  void toggleLoop();
  void setDuration(float d) { duration = d; }

  /** advance the clock by dt; fires crossed events. Call once per frame while
   *  playing (also call after a seek while paused to re-arm). */
  void update(float dt);

  /** fire every enabled event whose time lies in (lo, hi] (hi inclusive, lo
   *  exclusive), in time order, WITHOUT moving the clock or the fire
   *  boundary. The app uses this to catch up on events a seek/scrub jumped
   *  over (scrubbing otherwise skips every crossed `show`/fade/load and the
   *  show keeps rendering the pre-scrub scene). Loop events register into
   *  the repeat schedule exactly like update() would. */
  void fireWindow(float lo, float hi);

  /** events fired since the last call (stable copies; safe to inspect while
   *  the editor keeps updating). Consumed via consumeFired(). */
  const std::vector<TimelineEvent>& fired() const { return fired_; }
  void consumeFired() { fired_.clear(); }

  // --- editing ----------------------------------------------------------------
  int addTrack(const std::string& name, const std::string& type);
  void addEvent(TimelineEvent ev);
  void addMarker(const std::string& name, float t);
  void addClip(const std::string& name, float start, float end, int track = -1);
  /** shift every event belonging to clip by delta seconds */
  void offsetClip(const std::string& clipName, float delta);
  void clear();

  /** all events within [t0, t1] (for the editor UI / cue sheet) */
  std::vector<const TimelineEvent*> eventsIn(float t0, float t1) const;

  // --- serialization -----------------------------------------------------------
  Value toJson() const;
  void fromJson(const Value& v);

private:
  struct Loop {
    const TimelineEvent* ev = nullptr;  // points into events (stable)
    float nextFire = 0;
  };
  float lastFire_ = 0;    // upper boundary of already-fired time
  std::vector<TimelineEvent> fired_;
  std::vector<Loop> loops_;  // active repeating events
  bool sorted_ = true;

  void ensureSorted();
};

}  // namespace ns
