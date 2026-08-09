// ---------------------------------------------------------------------------
// EditorDocument implementation.
// ---------------------------------------------------------------------------
#include "editor/document.hpp"
#include "framework/core/log.hpp"
#include "framework/script/nsdwriter.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace ns {

double EditorDocument::now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void EditorDocument::adopt(const Script& s, const std::string& p) {
  ast = s;
  path = p;
  dirty = false;
  undo_.clear();
  redo_.clear();
  pending_.valid = false;
  lastOp_.clear();
}

void EditorDocument::load(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw ScriptError("cannot open script: " + p);
  std::ostringstream ss;
  ss << f.rdbuf();
  adopt(ScriptParser::parse(ss.str(), p), p);
}

std::string EditorDocument::serialize() const { return nsdSerialize(ast); }

bool EditorDocument::save() {
  if (!write()) return false;
  dirty = false;
  return true;
}

bool EditorDocument::write() {
  if (path.empty()) return false;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << serialize();
  return true;
}

// --- editing / undo ----------------------------------------------------------

void EditorDocument::beginEdit(const char* name) {
  if (!pending_.valid || pending_.name != name || now() - pending_.t0 > 0.6) {
    pending_.valid = true;
    pending_.name = name;
    pending_.ast = ast;
    pending_.t0 = now();
  }
}

void EditorDocument::endEdit() {
  if (!pending_.valid) return;
  const Script before = pending_.ast;
  pending_.valid = false;
  // no-op gestures (nothing changed) don't pollute the undo stack
  if (nsdSerialize(before) == nsdSerialize(ast)) return;
  redo_.clear();
  undo_.push_back({pending_.name, std::move(before)});
  if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
  lastOp_ = pending_.name;
  dirty = true;
}

void EditorDocument::cancelEdit() { pending_.valid = false; }

void EditorDocument::undo() {
  if (undo_.empty()) return;
  pending_.valid = false;  // an in-flight gesture is discarded by an undo
  redo_.push_back({undo_.back().name, ast});
  ast = undo_.back().ast;
  lastOp_ = undo_.back().name;
  undo_.pop_back();
  dirty = true;
}

void EditorDocument::redo() {
  if (redo_.empty()) return;
  pending_.valid = false;
  undo_.push_back({redo_.back().name, ast});
  ast = redo_.back().ast;
  lastOp_ = redo_.back().name;
  redo_.pop_back();
  dirty = true;
}

// --- queries -------------------------------------------------------------------

SceneDef* EditorDocument::findScene(const std::string& name) {
  for (auto& s : ast.scenes)
    if (s.name == name) return &s;
  return nullptr;
}

const SceneDef* EditorDocument::findScene(const std::string& name) const {
  for (const auto& s : ast.scenes)
    if (s.name == name) return &s;
  return nullptr;
}

namespace {
void collectAnimCmdsIn(std::vector<Cmd*>& out, std::vector<Cmd>& list) {
  for (auto& c : list)
    if (c.name == "anim") out.push_back(&c);
}
}  // namespace

std::vector<Cmd*> EditorDocument::animCmds() {
  std::vector<Cmd*> out;
  for (auto& sc : ast.scenes) {
    collectAnimCmdsIn(out, sc.setup);
    for (auto& b : sc.blocks) collectAnimCmdsIn(out, b.cmds);
  }
  for (auto& b : ast.main) collectAnimCmdsIn(out, b.cmds);
  return out;
}

ScriptBlock* EditorDocument::findMarker(const std::string& name) {
  for (auto& b : ast.main) {
    for (auto& c : b.cmds) {
      if (c.name == "marker" && !c.args.empty() &&
          c.args[0].asStr() == name) {
        return &b;
      }
    }
  }
  return nullptr;
}

// --- marker ops ---------------------------------------------------------------

bool EditorDocument::addMarker(const std::string& name, float t) {
  if (findMarker(name)) return false;
  ScriptBlock b;
  b.time = t;
  Cmd c;
  c.name = "marker";
  c.args.push_back(Value(name));
  b.cmds.push_back(std::move(c));
  // insert in time order (the runtime sorts markers anyway; keeping the file
  // ordered reads better and keeps diffs stable)
  auto it = std::upper_bound(
      ast.main.begin(), ast.main.end(), t,
      [](float tt, const ScriptBlock& x) { return tt < x.time; });
  ast.main.insert(it, std::move(b));
  return true;
}

bool EditorDocument::removeMarker(const std::string& name) {
  ScriptBlock* b = findMarker(name);
  if (!b) return false;
  b->cmds.erase(
      std::remove_if(b->cmds.begin(), b->cmds.end(),
                     [&](const Cmd& c) {
                       return c.name == "marker" && !c.args.empty() &&
                              c.args[0].asStr() == name;
                     }),
      b->cmds.end());
  if (b->cmds.empty()) {
    ast.main.erase(std::remove_if(ast.main.begin(), ast.main.end(),
                                  [&](const ScriptBlock& x) { return &x == b; }),
                   ast.main.end());
  }
  return true;
}

bool EditorDocument::renameMarker(const std::string& oldName,
                                  const std::string& newName) {
  if (oldName == newName) return false;
  if (newName.empty() || findMarker(newName)) return false;
  ScriptBlock* b = findMarker(oldName);
  if (!b) return false;
  for (auto& c : b->cmds) {
    if (c.name == "marker" && !c.args.empty() && c.args[0].asStr() == oldName) {
      c.args[0] = Value(newName);
      return true;
    }
  }
  return false;
}

bool EditorDocument::moveMarker(const std::string& name, float t) {
  ScriptBlock* b = findMarker(name);
  if (!b) return false;
  const bool alone = b->cmds.size() == 1 && b->cmds[0].name == "marker";
  if (alone) {
    b->time = t;
    // keep the main list sorted by time
    std::stable_sort(ast.main.begin(), ast.main.end(),
                     [](const ScriptBlock& a, const ScriptBlock& x) {
                       return a.time < x.time;
                     });
    return true;
  }
  // the marker shares its block with other commands: split it out into its
  // own `at T { marker NAME }` block
  Cmd marker;
  for (auto it = b->cmds.begin(); it != b->cmds.end(); ++it) {
    if (it->name == "marker" && !it->args.empty() &&
        it->args[0].asStr() == name) {
      marker = std::move(*it);
      b->cmds.erase(it);
      break;
    }
  }
  if (marker.name.empty()) return false;
  ScriptBlock nb;
  nb.time = t;
  nb.cmds.push_back(std::move(marker));
  ast.main.push_back(std::move(nb));
  std::stable_sort(ast.main.begin(), ast.main.end(),
                   [](const ScriptBlock& a, const ScriptBlock& x) {
                     return a.time < x.time;
                   });
  return true;
}

std::vector<std::string> EditorDocument::markerNames() const {
  std::vector<std::string> out;
  for (const auto& b : ast.main)
    for (const auto& c : b.cmds)
      if (c.name == "marker" && !c.args.empty())
        out.push_back(c.args[0].asStr());
  return out;
}

}  // namespace ns
