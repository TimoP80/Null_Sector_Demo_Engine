// ---------------------------------------------------------------------------
// ScriptEngine - executes the demo DSL against the framework.
//
//   load(path)                 parse the script (throws ScriptError)
//   build(editor)              flatten every `at` block + scene activation
//                              into timeline events (absolute seconds)
//   scenes()                   activation bundles (setup commands + relative
//                              at-sub-blocks) for the director
//   sections()                 the show's section schedule (name, start, end,
//                              duration, intensity, chapter) derived from the
//                              scene activations - feeds the engine's beat
//                              clock so shaders keep their bar-synced math
//
// `show X` events (where X is a scene) create the section boundaries and
// schedule the scene's setup + sub-blocks relative to its activation time.
// The script itself never references seconds it doesn't need to: times accept
// seconds, mm:ss, or bar/beat units at the declared bpm.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/script/scriptparser.hpp"
#include "framework/timeline/timelineeditor.hpp"

#include <map>
#include <string>
#include <vector>

namespace ns {

struct SceneBundle {
  std::string name;
  std::string title;
  std::vector<Cmd> setup;        // run when the scene activates
  std::vector<ScriptBlock> blocks;  // scene-relative at-sub-blocks
};

struct SceneSection {
  std::string name;
  std::string title;
  float start = 0;
  float end = 0;
  float duration = 0;
  float intensity = 0.5f;
  int chapter = 0;
};

class ScriptEngine {
public:
  /** parse a script file; throws ScriptError on syntax errors */
  bool load(const std::string& path);
  bool loadText(const std::string& text, const std::string& label);

  /** flatten the script into timeline events (absolute times) */
  void build(TimelineEditor& editor);

  const Script& script() const { return script_; }
  const std::vector<SceneBundle>& scenes() const { return bundles_; }
  const std::vector<SceneSection>& sections() const { return sections_; }

  /** find a scene bundle by name (nullptr when absent) */
  const SceneBundle* scene(const std::string& name) const;

  float bpm() const { return bpm_; }
  float duration() const { return script_.duration > 0 ? script_.duration : total_; }

  /** true when a `show X` event refers to an unknown scene/effect (diagnostic) */
  std::vector<std::string> unresolved() const { return unresolved_; }

  void clear();

private:
  Script script_;
  float bpm_ = 216.0f;
  float total_ = 0;
  std::vector<SceneBundle> bundles_;
  std::vector<SceneSection> sections_;
  std::vector<std::string> unresolved_;

  void buildBundles();
  void collectActivations(std::vector<std::pair<float, std::string>>& out) const;
  void activate(TimelineEditor& editor, const std::string& name, float at,
                std::map<std::string, float>& activeAt);
  float sceneDuration(const SceneBundle& b, float start, float nextStart) const;
};

}  // namespace ns
