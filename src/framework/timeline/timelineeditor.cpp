#include "framework/timeline/timelineeditor.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace ns {

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------
void TimelineEditor::play() {
  playing = true;
  if (looping && time < loopStart) time = loopStart;
  if (looping && time >= loopEnd) time = loopStart;
}

void TimelineEditor::pause() { playing = false; }

void TimelineEditor::toggle() { playing ? pause() : play(); }

void TimelineEditor::seek(float t) {
  time = std::max(0.0f, t);
  // re-arm the fire boundary: nothing between the new time and the old
  // boundary refires until it is crossed again
  lastFire_ = time;
  loops_.clear();  // repeating events restart their schedule from here
}

void TimelineEditor::setLoop(bool on, float start, float end) {
  looping = on;
  loopStart = start;
  loopEnd = end > start ? end : duration;
}

void TimelineEditor::toggleLoop() {
  if (!looping) {
    // loop the current region: from the last marker (or 0) to the next (or end)
    float s = 0, e = duration;
    for (const auto& m : markers) {
      if (m.time <= time + 0.25f) s = std::max(s, m.time);
    }
    for (const auto& m : markers) {
      if (m.time > time + 0.25f) { e = m.time; break; }
    }
    setLoop(true, s, e);
  } else {
    looping = false;
  }
}

// ---------------------------------------------------------------------------
// update / firing
// ---------------------------------------------------------------------------
void TimelineEditor::update(float dt) {
  if (!playing) return;

  float newTime = time + dt * speed;

  // loop wrap: reset the fire boundary so looped events refire
  if (looping && loopEnd > loopStart && newTime >= loopEnd && time < loopEnd) {
    const float span = loopEnd - loopStart;
    newTime = loopStart + std::fmod(newTime - loopEnd, span);
    lastFire_ = loopStart - 0.0001f;
  }

  if (newTime < time && newTime < lastFire_) {
    // scrubbed backwards via speed<0 or manual time change: re-arm
    lastFire_ = newTime;
  }

  time = newTime;
  if (time > duration && duration > 0) time = duration;

  ensureSorted();

  // walk the sorted event list once per frame; events whose fire time lies in
  // (lastFire_, time] fire exactly once. fired_ receives stable COPIES so the
  // caller can safely keep inspecting them while we keep running.
  const float lo = std::min(lastFire_, time);
  const float hi = std::max(lastFire_, time);
  for (const auto& ev : events) {
    if (ev.time < lo) continue;
    if (ev.time > hi) break;  // sorted: no later event can be in the window
    if (!ev.enabled) continue;
    if (ev.loopEvery > 0) {
      // repeating event: register a loop entry; its first fire may already be
      // inside this window (e.g. the loop started mid-window)
      bool found = false;
      for (auto& l : loops_) {
        if (l.ev == &ev) { found = true; break; }
      }
      if (!found) loops_.push_back({&ev, ev.time});
    } else {
      fired_.push_back(ev);
    }
  }

  // repeating events: fire while nextFire is inside the window, then advance.
  // Disabled events drop out of the repeat schedule.
  loops_.erase(std::remove_if(loops_.begin(), loops_.end(),
                              [](const Loop& l) { return !l.ev->enabled || l.ev->loopEvery <= 0; }),
               loops_.end());
  for (auto& l : loops_) {
    while (l.nextFire <= hi) {
      if (l.nextFire >= lo) fired_.push_back(*l.ev);
      l.nextFire += l.ev->loopEvery;
    }
  }

  lastFire_ = time;
}

void TimelineEditor::fireWindow(float lo, float hi) {
  const float l = std::min(lo, hi);
  const float h = std::max(lo, hi);
  ensureSorted();
  for (const auto& ev : events) {
    if (ev.time <= l) continue;
    if (ev.time > h) break;  // sorted: no later event can be in the window
    if (!ev.enabled) continue;
    if (ev.loopEvery > 0) {
      // repeating event: register into the repeat schedule (like update())
      bool found = false;
      for (auto& lp : loops_) {
        if (lp.ev == &ev) { found = true; break; }
      }
      if (!found) loops_.push_back({&ev, ev.time});
    } else {
      fired_.push_back(ev);
    }
  }
}

void TimelineEditor::ensureSorted() {
  if (sorted_) return;
  std::sort(events.begin(), events.end(),
            [](const TimelineEvent& a, const TimelineEvent& b) { return a.time < b.time; });
  sorted_ = true;
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------
int TimelineEditor::addTrack(const std::string& name, const std::string& type) {
  tracks.push_back({name, type, false});
  return (int)tracks.size() - 1;
}

void TimelineEditor::addEvent(TimelineEvent ev) {
  events.push_back(std::move(ev));
  sorted_ = false;
}

void TimelineEditor::addMarker(const std::string& name, float t) {
  markers.push_back({name, t});
  std::sort(markers.begin(), markers.end(),
            [](const TimelineMarker& a, const TimelineMarker& b) { return a.time < b.time; });
}

void TimelineEditor::addClip(const std::string& name, float start, float end, int track) {
  clips.push_back({name, start, end, track});
}

void TimelineEditor::offsetClip(const std::string& clipName, float delta) {
  for (auto& c : clips) {
    if (c.name == clipName) {
      c.start += delta;
      c.end += delta;
    }
  }
  for (auto& ev : events) {
    if (ev.clip == clipName) {
      ev.time += delta;
      sorted_ = false;
    }
  }
  ensureSorted();
}

void TimelineEditor::clear() {
  tracks.clear();
  events.clear();
  markers.clear();
  clips.clear();
  loops_.clear();
  time = 0;
  lastFire_ = 0;
  playing = false;
  sorted_ = true;
}

std::vector<const TimelineEvent*> TimelineEditor::eventsIn(float t0, float t1) const {
  std::vector<const TimelineEvent*> out;
  for (const auto& ev : events) {
    if (ev.time >= t0 && ev.time <= t1) out.push_back(&ev);
  }
  return out;
}

// ---------------------------------------------------------------------------
// serialization
// ---------------------------------------------------------------------------
static Value cmdToJson(const Cmd& c) {
  Value o = Value::object();
  o.set("name") = Value(c.name);
  Value args = Value::array();
  for (const auto& a : c.args) args.push(a);
  o.set("args") = std::move(args);
  o.set("opts") = c.opts;
  Value keys = Value::array();
  for (const auto& k : c.keys) {
    Value ko = Value::object();
    ko.set("t") = Value((double)k.t);
    ko.set("v") = k.v;
    if (!k.interp.empty()) ko.set("interp") = Value(k.interp);
    keys.push(std::move(ko));
  }
  o.set("keys") = std::move(keys);
  return o;
}

static Cmd cmdFromJson(const Value& o) {
  Cmd c;
  c.name = o.get("name").asStr();
  for (const auto& a : o.get("args").asArr()) c.args.push_back(a);
  const Value& opts = o.get("opts");
  if (opts.isObj()) for (const auto& kv : opts.asObj()) c.opts.set(kv.first) = kv.second;
  for (const auto& k : o.get("keys").asArr()) {
    KeyframeRow row;
    row.t = (float)k.get("t").asNum();
    row.v = k.get("v");
    row.interp = k.get("interp").asStr();
    c.keys.push_back(std::move(row));
  }
  return c;
}

Value TimelineEditor::toJson() const {
  Value root = Value::object();
  root.set("duration") = Value((double)duration);
  root.set("bpm_note") = Value("timeline works in seconds; bpm lives in the demo script");

  Value tr = Value::array();
  for (const auto& t : tracks) {
    Value o = Value::object();
    o.set("name") = Value(t.name);
    o.set("type") = Value(t.type);
    tr.push(std::move(o));
  }
  root.set("tracks") = std::move(tr);

  Value ev = Value::array();
  for (const auto& e : events) {
    Value o = Value::object();
    o.set("name") = Value(e.name);
    o.set("time") = Value((double)e.time);
    o.set("duration") = Value((double)e.duration);
    if (e.track >= 0) o.set("track") = Value(e.track);
    if (!e.clip.empty()) o.set("clip") = Value(e.clip);
    if (e.loopEvery > 0) o.set("loop") = Value((double)e.loopEvery);
    Value cmds = Value::array();
    for (const auto& c : e.cmds) cmds.push(cmdToJson(c));
    o.set("cmds") = std::move(cmds);
    ev.push(std::move(o));
  }
  root.set("events") = std::move(ev);

  Value mk = Value::array();
  for (const auto& m : markers) {
    Value o = Value::object();
    o.set("name") = Value(m.name);
    o.set("time") = Value((double)m.time);
    mk.push(std::move(o));
  }
  root.set("markers") = std::move(mk);

  Value cl = Value::array();
  for (const auto& c : clips) {
    Value o = Value::object();
    o.set("name") = Value(c.name);
    o.set("start") = Value((double)c.start);
    o.set("end") = Value((double)c.end);
    cl.push(std::move(o));
  }
  root.set("clips") = std::move(cl);
  return root;
}

void TimelineEditor::fromJson(const Value& root) {
  clear();
  duration = (float)root.get("duration").asNum();
  for (const auto& t : root.get("tracks").asArr()) {
    addTrack(t.get("name").asStr("track"), t.get("type").asStr("event"));
  }
  for (const auto& e : root.get("events").asArr()) {
    TimelineEvent ev;
    ev.name = e.get("name").asStr();
    ev.time = (float)e.get("time").asNum();
    ev.duration = (float)e.get("duration").asNum();
    ev.track = e.get("track").asInt(-1);
    ev.clip = e.get("clip").asStr();
    ev.loopEvery = (float)e.get("loop").asNum();
    for (const auto& c : e.get("cmds").asArr()) ev.cmds.push_back(cmdFromJson(c));
    addEvent(std::move(ev));
  }
  for (const auto& m : root.get("markers").asArr()) {
    addMarker(m.get("name").asStr(), (float)m.get("time").asNum());
  }
  for (const auto& c : root.get("clips").asArr()) {
    addClip(c.get("name").asStr(), (float)c.get("start").asNum(),
            (float)c.get("end").asNum(), c.get("track").asInt(-1));
  }
  ensureSorted();
}

}  // namespace ns
