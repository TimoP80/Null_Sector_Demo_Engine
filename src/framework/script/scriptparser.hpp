// ---------------------------------------------------------------------------
// ScriptParser - the demo scripting DSL.
//
//   demo "NULL SECTOR DEMO ENGINE" {
//       bpm 216
//   }
//
//   scene Intro {
//       bars 60  intensity 0.12  chapter 0
//       camera IntroCam { rig drift; pos (0,0,2.4); fov 50 }
//       show intro
//       play music
//       fade in 2
//   }
//
//   at 0.0  { show Intro }
//   at 66.7 { hide Intro; show tunnel; camera TunnelCam; transition fade 1 }
//   at 1:48.9 { load shadertoy shadertoy/plasma.glsl }
//   at bar 60 { loop section }
//
// A script is a list of declarations, `scene` blocks and `at` blocks. Every
// `at` block becomes a timeline event; scene blocks are activation bundles
// (section metadata + setup commands + scene-relative `at` sub-blocks).
//
// Grammar (line-oriented, ; or newline separates commands):
//   decl      := 'demo' STRING '{' pairs '}'
//   scene     := 'scene' IDENT '{' { decl-pair | at-sub | cmd }* '}'
//   at        := 'at' TIME '{' cmd* '}'  |  'at' TIME cmd
//   cmd       := IDENT (value | vector)* ('{' pair* '}')?
//   pair      := IDENT value | value value
//   time      := NUMBER | mm:ss(.ms) | 'beat' N | 'bar' N | N ('s'|'beat'|'bar')
//   value     := NUMBER | STRING | IDENT | '(' values ')'
//   comments  := // line, # line, /* block */
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/value.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace ns {

// --- AST ---------------------------------------------------------------------

/** one keyframe row for `anim` style commands: time value [interp] */
struct KeyframeRow {
  float t = 0;
  Value v;
  std::string interp;  // empty = use the animation's default
};

/** a single command: name + positional args + key/value option block */
struct Cmd {
  std::string name;
  std::vector<Value> args;          // positional
  Value opts = Value::object();     // named options from the { } block
  std::vector<KeyframeRow> keys;    // keyframe rows (anim commands)

  /** option helper: `cmd.f("fov", 62.0f)` */
  float f(const char* key, float dflt = 0.0f) const {
    return opts.get(key).asFloat(dflt);
  }
  int i(const char* key, int dflt = 0) const { return opts.get(key).asInt(dflt); }
  bool b(const char* key, bool dflt = false) const { return opts.get(key).asBool(dflt); }
  std::string s(const char* key, const std::string& dflt = {}) const {
    return opts.get(key).asStr(dflt);
  }
  bool has(const char* key) const { return !opts.get(key).isNull(); }
};

/** an `at` block (absolute, or scene-relative when inside a scene) */
struct ScriptBlock {
  float time = 0;  // seconds (already resolved against the declared bpm)
  std::vector<Cmd> cmds;
};

/** a scene declaration = section metadata + setup commands + sub-blocks */
struct SceneDef {
  std::string name;
  std::string title;            // optional display title
  int bars = 0;                 // section length in bars (0 = next scene)
  float duration = 0;           // explicit seconds override
  float intensity = 0.5f;
  int chapter = 0;
  bool visible = true;          // scene participates in section scheduling
  std::vector<Cmd> setup;       // commands run when the scene activates
  std::vector<ScriptBlock> blocks;  // scene-relative at sub-blocks
};

struct Script {
  std::string title;
  float bpm = 216.0f;
  float duration = 0;  // explicit total seconds (0 = inferred)
  std::vector<SceneDef> scenes;
  std::vector<ScriptBlock> main;  // top-level absolute-time blocks
};

class ScriptError : public std::runtime_error {
public:
  explicit ScriptError(const std::string& msg) : std::runtime_error(msg) {}
};

// --- parser -------------------------------------------------------------------

class ScriptParser {
public:
  /** parse a script from text; throws ScriptError on syntax errors */
  static Script parse(const std::string& text, const std::string& label = "script");
};

// --- time helpers -------------------------------------------------------------

/** parse a time token against a bpm: "12.5", "1:05.0", "66.667s",
 *  "beat 128", "bar 32". Throws ScriptError on malformed input. */
float parseTime(const std::string& tok, float bpm);

/** seconds per beat / per bar at a bpm */
inline float beatSec(float bpm) { return 60.0f / bpm; }
inline float barSec(float bpm) { return beatSec(bpm) * 4.0f; }

}  // namespace ns
