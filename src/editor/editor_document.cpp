// ---------------------------------------------------------------------------
// editor_document.cpp - the document model's integration with the editor:
// lifecycle (adopt at boot, re-sync on external reloads), save (Ctrl+S),
// undo/redo (Ctrl+Z / Ctrl+Y) and "+ Scene" as a document operation.
//
// The document (EditorDocument) holds the parsed Script AST; the runtime
// (DemoApp) is a derived view. Saving serializes the AST back to the .nsd
// (nsdSerialize) and reloads the show; undo/redo restore a snapshot of the
// AST and do the same, so the runtime always reflects the document.
// ---------------------------------------------------------------------------
#include "editor/editor.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace ns {

void DemoEditor::initDocument() {
  if (!w_.app) return;
  try {
    doc_.adopt(w_.app->script().script(), w_.app->scriptPath());
  } catch (const std::exception& e) {
    Log::error("EDITOR", std::string("document init failed: ") + e.what());
  }
  docReloadSeen_ = w_.app->reloadCount();
}

/** the watcher / F2 / --demo switches can reload the script behind our back;
 *  the file is the truth, so re-adopt the freshly parsed AST (this also
 *  discards unsaved in-memory edits - the .nsd on disk wins) */
void DemoEditor::syncDocumentFromApp() {
  if (!w_.app) return;
  const uint64_t n = w_.app->reloadCount();
  if (n == docReloadSeen_) return;
  docReloadSeen_ = n;
  try {
    doc_.adopt(w_.app->script().script(), w_.app->scriptPath());
    Log::info("EDITOR", "document re-synced to the loaded script");
  } catch (const std::exception& e) {
    Log::error("EDITOR", std::string("document re-sync failed: ") + e.what());
  }
}

/** serialize the document to its .nsd, reload the show, and return to where
 *  the user was (reloadScript restarts at 0/playing). The file now matches
 *  the document, so the dirty flag is cleared. */
bool DemoEditor::writeDocument() {
  if (!w_.app || doc_.path.empty()) return false;
  if (!doc_.write()) {
    Log::error("EDITOR", "document write failed: " + doc_.path);
    return false;
  }
  const float keep = w_.director ? w_.director->show : 0;
  const bool wasPaused = w_.director && w_.director->paused;
  // re-baseline the watcher so it doesn't flag the file we just wrote and
  // reload a second time, then one deterministic reload
  w_.app->editableWatcher().poll();
  w_.app->reloadScript();
  docReloadSeen_ = w_.app->reloadCount();  // our own reload: the doc is current
  if (w_.director) {
    w_.director->show = keep;
    w_.director->paused = wasPaused;
  }
  if (w_.app) w_.app->seek(keep);
  if (w_.timeline) w_.timeline->advance(keep);
  doc_.dirty = false;
  return true;
}

void DemoEditor::saveDocument() {
  if (!doc_.dirty) return;
  if (writeDocument()) Log::info("EDITOR", "saved " + doc_.path);
}

bool DemoEditor::writeDocumentAs(const std::string& path) {
  if (!w_.app || path.empty()) return false;
  const std::filesystem::path target = std::filesystem::absolute(path);
  const std::string targetPath = target.string();
  const std::string oldPath = doc_.path;
  const float keep = w_.director ? w_.director->show : 0.0f;
  const bool wasPaused = w_.director && w_.director->paused;

  // Save the old project's view before changing the document key. The new
  // path gets its own timeline view (or the normal fit-all default).
  saveEditorState();
  doc_.path = targetPath;
  if (!doc_.write()) {
    doc_.path = oldPath;
    Log::error("EDITOR", "Save As failed: " + targetPath);
    return false;
  }

  // editorOpenScript validates the written file before changing the runtime's
  // active path. This is deliberately different from writeDocument(): the
  // normal save reloads the existing path, while Save As must switch paths.
  w_.app->editorOpenScript(targetPath);
  if (w_.app->scriptPath() != targetPath) {
    doc_.path = oldPath;
    Log::error("EDITOR", "Save As reload failed; kept current project");
    return false;
  }
  docReloadSeen_ = w_.app->reloadCount();
  doc_.dirty = false;
  selScene_.clear();
  selNode_ = nullptr;
  selEffect_.clear();
  applyTimelineViewForShow(showKey());
  if (w_.director) {
    w_.director->show = keep;
    w_.director->paused = wasPaused;
  }
  w_.app->seek(keep);
  if (w_.timeline) w_.timeline->advance(keep);
  Log::info("EDITOR", "saved project as " + targetPath);
  return true;
}

void DemoEditor::undoDocument() {
  if (!doc_.canUndo()) return;
  const std::string op = doc_.lastOpName();
  doc_.undo();
  writeDocument();
  Log::info("EDITOR", std::string("undo: ") + doc_.lastOpName());
  (void)op;
}

void DemoEditor::redoDocument() {
  if (!doc_.canRedo()) return;
  doc_.redo();
  writeDocument();
  Log::info("EDITOR", std::string("redo: ") + doc_.lastOpName());
}

bool DemoEditor::createNewProject(const std::string& path) {
  if (!w_.app || path.empty()) return false;
  const std::filesystem::path target = std::filesystem::absolute(path);
  const std::string targetPath = target.string();
  const char* starter = R"NSD(demo "NEW NULL SECTOR PROJECT" {
    bpm 120
    duration 16
}

scene Intro {
    bars 8  intensity 0.35  chapter 0
    title "First Signal"
    camera MainCam { rig static; pos (0,0,2); target (0,0,0); fov 55 }
    text title { text "NEW PROJECT"; pos (0,0.15,0); size 34; style neon }
    fade in 1
    at 7 { transition fade 1 }
}

at 0 { show Intro; marker FirstSignal }
)NSD";

  std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    Log::error("EDITOR", "could not create project: " + targetPath);
    return false;
  }
  out << starter;
  out.close();

  w_.app->editorOpenScript(targetPath);
  if (w_.app->scriptPath() != targetPath) {
    Log::error("EDITOR", "new project failed validation: " + targetPath);
    return false;
  }
  doc_.adopt(w_.app->script().script(), targetPath);
  docReloadSeen_ = w_.app->reloadCount();
  selScene_.clear();
  selNode_ = nullptr;
  selEffect_.clear();
  tlZoom_ = 75.0f;
  tlT0_ = 0.0f;
  tlFitZoom_ = -1.0f;
  tlFitT0_ = 0.0f;
  applyTimelineViewForShow(showKey());
  if (w_.director) {
    w_.director->show = 0.0f;
    // New projects follow the editor's stopped-by-default transport policy.
    w_.director->paused = true;
  }
  w_.app->seek(0.0f);
  if (w_.timeline) w_.timeline->advance(0.0f);
  saveEditorState();
  Log::info("EDITOR", "created new project " + targetPath);
  return true;
}

std::string DemoEditor::docDisplayName() const {
  std::string name = doc_.path.empty()
                         ? "no script"
                         : std::filesystem::path(doc_.path).filename().string();
  if (doc_.dirty) name += " *";
  return name;
}

// ---------------------------------------------------------------------------
// "+ Scene" as a document op: appends a SceneDef + activation block to the
// AST, extends the header duration, then commits (write + reload + land just
// before the activation so the show command actually fires).
// ---------------------------------------------------------------------------
void DemoEditor::addSceneViaDocument() {
  if (!w_.app || !w_.timeline) return;
  const auto& secs = w_.app->sections();
  float nextStart = 0;
  if (!secs.empty()) nextStart = secs.back().end;
  const float dur = w_.app->editor().duration;
  if (dur > nextStart) nextStart = dur;

  // unique scene name (SceneN) against the document's scenes
  int n = (int)secs.size();
  std::string name = "Scene" + std::to_string(n);
  while (doc_.findScene(name)) name = "Scene" + std::to_string(++n);

  // 8 bars at the show's tempo - a blank scene with a camera + placeholder
  // effect, matching the scene-block style of demo.nsd
  const int bars = 8;
  const float barSec = w_.timeline->barSec();
  const float nextEnd = nextStart + bars * barSec;

  doc_.beginEdit("add scene");
  if (doc_.ast.duration <= 0 || doc_.ast.duration < nextEnd)
    doc_.ast.duration = nextEnd;

  SceneDef sc;
  sc.name = name;
  sc.title = name;
  sc.bars = bars;
  sc.intensity = 0.5f;
  sc.chapter = 5;

  Cmd cam;
  cam.name = "camera";
  cam.args.push_back(Value(name + "Cam"));
  cam.opts.set("rig") = Value("static");
  {
    Value::Array a;
    a.push_back(Value(0.0));
    a.push_back(Value(0.0));
    a.push_back(Value(4.0));
    cam.opts.set("pos") = Value(std::move(a));
  }
  {
    Value::Array a;
    a.push_back(Value(0.0));
    a.push_back(Value(0.0));
    a.push_back(Value(0.0));
    cam.opts.set("target") = Value(std::move(a));
  }
  cam.opts.set("fov") = Value(55.0);
  sc.setup.push_back(std::move(cam));

  Cmd show;
  show.name = "show";
  show.args.push_back(Value("tunnel"));
  sc.setup.push_back(std::move(show));

  Cmd fadeIn;
  fadeIn.name = "fade";
  fadeIn.args.push_back(Value("in"));
  fadeIn.args.push_back(Value(1.0));
  sc.setup.push_back(std::move(fadeIn));

  ScriptBlock out;
  out.time = bars * barSec - 3.0f;
  Cmd fadeOut;
  fadeOut.name = "fade";
  fadeOut.args.push_back(Value("out"));
  fadeOut.args.push_back(Value(2.0));
  out.cmds.push_back(std::move(fadeOut));
  sc.blocks.push_back(std::move(out));
  doc_.ast.scenes.push_back(std::move(sc));

  // activation: at <nextStart> { show NAME; marker NAME }
  ScriptBlock act;
  act.time = nextStart;
  Cmd s2;
  s2.name = "show";
  s2.args.push_back(Value(name));
  act.cmds.push_back(std::move(s2));
  Cmd mk;
  mk.name = "marker";
  mk.args.push_back(Value(name));
  act.cmds.push_back(std::move(mk));
  doc_.ast.main.push_back(std::move(act));
  std::stable_sort(doc_.ast.main.begin(), doc_.ast.main.end(),
                   [](const ScriptBlock& a, const ScriptBlock& b) {
                     return a.time < b.time;
                   });
  doc_.endEdit();

  writeDocument();
  // land a hair BEFORE the activation: the next update's forward crossing
  // fires `at <nextStart> { show <name> }`, so the new scene actually
  // activates (a plain seek re-arms the fire boundary without firing it).
  // seekToRaw: this is a programmatic landing, never quantized.
  seekToRaw(nextStart - 0.001f);
  // if the director was paused, resume so that crossing happens immediately
  if (w_.director && w_.director->paused) w_.director->paused = false;
  char lbb[256];
  std::snprintf(lbb, sizeof lbb, "scene '%s' added to %s", name.c_str(), doc_.path.c_str());
  Log::info("EDITOR", lbb);
}


// ---------------------------------------------------------------------------
// NS_EDITOR_DOC_SMOKE=1: prove the document pipeline inside the running
// editor: add a marker through the document, undo it, redo it, commit (write
// + reload) and verify the runtime timeline derives it from the file. Run
// with --demo on a COPY of a production script.
// ---------------------------------------------------------------------------
void DemoEditor::runDocSmoke(float dt) {
  if (!smokeDoc_) return;
  docSmokeT_ += dt;
  if (docSmokeT_ < 1.5f || docSmokeDone_) return;

  bool ok = true;
  const auto chk = [&](bool c, const char* what) {
    if (!c) {
      ok = false;
      Log::error("EDITOR", std::string("doc smoke FAIL: ") + what);
    }
  };

  if (!docSmokePhase2_) {
    // --- phase 1: add -> undo -> redo -> write ------------------------------
    doc_.beginEdit("smoke marker");
    chk(doc_.addMarker("SMOKE_MARK", 3.0f), "addMarker");
    doc_.endEdit();
    chk(doc_.dirty, "dirty after edit");
    chk(doc_.findMarker("SMOKE_MARK") != nullptr, "marker present in the doc");
    chk(doc_.canUndo(), "undo available");

    doc_.undo();
    chk(doc_.findMarker("SMOKE_MARK") == nullptr, "undo removed the marker");

    doc_.redo();
    chk(doc_.findMarker("SMOKE_MARK") != nullptr, "redo restored the marker");

    chk(writeDocument(), "writeDocument");
    chk(!doc_.dirty, "write cleared dirty");
    chk(doc_.canUndo(), "undo survives a write");

    // marker commands are dispatched by EVENTS (app->update drains
    // fired()), so cross the 3s marker and check on the NEXT frame
    seekTo(4.0f);
    docSmokePhase2_ = true;
    docSmokeOk_ = ok;
    return;
  }

  // --- phase 2: verify the runtime derived the marker, then clean up --------
  ok = docSmokeOk_;
  bool inTimeline = false;
  for (const auto& m : w_.app->editor().markers)
    if (m.name == "SMOKE_MARK") inTimeline = true;
  chk(inTimeline, "marker reached the runtime timeline after crossing it");

  doc_.beginEdit("smoke remove");
  chk(doc_.removeMarker("SMOKE_MARK"), "removeMarker");
  doc_.endEdit();
  chk(writeDocument(), "writeDocument 2");  // leaves the copy script clean

  docSmokeDone_ = true;
  if (ok)
    Log::info("EDITOR", "doc smoke: add -> undo -> redo -> write -> runtime derive - PASS");
  else
    Log::error("EDITOR", "doc smoke: FAIL (see lines above)");
}

}  // namespace ns
