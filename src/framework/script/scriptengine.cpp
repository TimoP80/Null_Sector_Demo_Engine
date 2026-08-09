#include "framework/script/scriptengine.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ns {

namespace {
bool cmdShows(const Cmd& c, const std::string& sceneName) {
  return c.name == "show" && !c.args.empty() && c.args[0].asStr() == sceneName;
}
}  // namespace

void ScriptEngine::clear() {
  script_ = Script{};
  bundles_.clear();
  sections_.clear();
  unresolved_.clear();
  total_ = 0;
}

bool ScriptEngine::loadText(const std::string& text, const std::string& label) {
  script_ = ScriptParser::parse(text, label);
  bpm_ = script_.bpm > 0 ? script_.bpm : 216.0f;
  buildBundles();
  return true;
}

bool ScriptEngine::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    Log::error("SCRIPT", "cannot open script: " + path);
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return loadText(ss.str(), path);
}

void ScriptEngine::buildBundles() {
  bundles_.clear();
  for (const auto& s : script_.scenes) {
    SceneBundle b;
    b.name = s.name;
    b.title = s.title;
    b.setup = s.setup;
    b.blocks = s.blocks;
    bundles_.push_back(std::move(b));
  }
}

const SceneBundle* ScriptEngine::scene(const std::string& name) const {
  for (const auto& b : bundles_) if (b.name == name) return &b;
  return nullptr;
}

float ScriptEngine::sceneDuration(const SceneBundle& b, float start, float nextStart) const {
  // 1. explicit bars at the declared bpm
  for (const auto& s : script_.scenes) {
    if (s.name == b.name) {
      if (s.bars > 0) return (float)s.bars * barSec(bpm_);
      if (s.duration > 0) return s.duration;
      break;
    }
  }
  // 2. up to the next scene
  if (nextStart > start) return nextStart - start;
  // 3. last scene: up to the explicit duration, else 30s default
  return script_.duration > 0 ? script_.duration - start : 30.0f;
}

void ScriptEngine::collectActivations(std::vector<std::pair<float, std::string>>& out) const {
  for (const auto& blk : script_.main) {
    for (const auto& c : blk.cmds) {
      if (c.name == "show" && !c.args.empty()) {
        const std::string& tgt = c.args[0].asStr();
        if (scene(tgt)) out.emplace_back(blk.time, tgt);
      }
    }
  }
}

void ScriptEngine::activate(TimelineEditor& editor, const std::string& name, float at,
                            std::map<std::string, float>& activeAt) {
  // guard against recursive scene shows (A shows B shows A ...)
  if (activeAt.count(name)) return;
  activeAt[name] = at;
  const SceneBundle* b = scene(name);
  if (!b) return;

  // the activation itself: scene setup commands at time `at`
  if (!b->setup.empty()) {
    TimelineEvent ev;
    ev.name = "scene:" + name;
    ev.time = at;
    ev.cmds = b->setup;
    editor.addEvent(std::move(ev));
  }
  // scene-relative sub-blocks
  for (const auto& blk : b->blocks) {
    TimelineEvent ev;
    ev.name = "scene:" + name + ":at:" + std::to_string(blk.time);
    ev.time = at + blk.time;
    ev.cmds = blk.cmds;
    editor.addEvent(std::move(ev));
  }
  // nested scene activations from the setup
  for (const auto& c : b->setup) {
    if (c.name == "show" && !c.args.empty()) {
      const std::string& tgt = c.args[0].asStr();
      if (scene(tgt)) activate(editor, tgt, at, activeAt);
    }
  }
}

void ScriptEngine::build(TimelineEditor& editor) {
  // 1. top-level at-blocks
  int idx = 0;
  for (const auto& blk : script_.main) {
    TimelineEvent ev;
    ev.name = "at:" + std::to_string(idx++) + ":" + std::to_string((int)(blk.time * 1000));
    ev.time = blk.time;
    ev.cmds = blk.cmds;
    editor.addEvent(std::move(ev));
  }

  // 2. scene activations -> setup events + section schedule
  std::vector<std::pair<float, std::string>> acts;
  collectActivations(acts);
  std::sort(acts.begin(), acts.end());

  std::map<std::string, float> activeAt;
  for (const auto& [t, name] : acts) {
    activate(editor, name, t, activeAt);
  }

  // 3. section schedule (one per activation, ordered)
  sections_.clear();
  unresolved_.clear();
  std::vector<std::pair<float, std::string>> seen;
  for (const auto& [t, name] : acts) {
    if (std::find(seen.begin(), seen.end(), std::make_pair(t, name)) != seen.end()) continue;
    seen.emplace_back(t, name);
    const SceneBundle* b = scene(name);
    if (!b) { unresolved_.push_back(name); continue; }
    SceneSection s;
    s.name = name;
    s.title = b->title;
    s.start = t;
    for (const auto& sd : script_.scenes) {
      if (sd.name == name) { s.intensity = sd.intensity; s.chapter = sd.chapter; break; }
    }
    // end = next activation start, or explicit duration
    float nextStart = script_.duration > 0 ? script_.duration : t + 30.0f;
    for (const auto& [t2, n2] : acts) {
      if (t2 > t + 0.001f) { nextStart = t2; break; }
    }
    s.duration = sceneDuration(*b, t, nextStart);
    s.end = s.start + s.duration;
    sections_.push_back(s);
  }
  total_ = 0;
  for (const auto& s : sections_) total_ = std::max(total_, s.end);
  if (script_.duration > 0) total_ = script_.duration;

  editor.setDuration(total_);
}

}  // namespace ns
