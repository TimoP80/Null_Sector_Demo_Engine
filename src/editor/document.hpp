// ---------------------------------------------------------------------------
// EditorDocument - the demo editor's document model.
//
// The document is the parsed production script (a Script AST), its file
// path, and its dirty state. The runtime (DemoApp's ScriptEngine, scene
// graph, timeline and animation system) is a DERIVED view of this document:
// authoring operations mutate the AST, mark it dirty, and either push a
// lightweight live update to the runtime (keyframe drags) or commit the
// whole document to disk and reload the show (save, add-scene, undo).
//
// Undo/redo is snapshot-based: beginEdit() records the AST before a gesture
// (repeated calls of the same name inside one gesture coalesce into a single
// undo step - drags call beginEdit once per frame), and endEdit() pushes one
// undo entry per gesture, skipping no-ops. This makes every document op
// undoable with one mechanism. The snapshot is a full deep copy of the AST,
// which is fine for data-driven productions (hundreds of KB at most).
// ---------------------------------------------------------------------------
#pragma once

#include "framework/script/scriptparser.hpp"
#include <string>
#include <vector>

namespace ns {

class EditorDocument {
public:
  Script ast;              // the document (source of truth)
  std::string path;        // .nsd file this document maps to ("" = none)
  bool dirty = false;      // unsaved edits since the last save()

  static constexpr size_t kMaxUndo = 64;

  // --- lifecycle -------------------------------------------------------------
  /** adopt a freshly parsed script (boot, external reload); resets undo */
  void adopt(const Script& s, const std::string& p);
  /** parse PATH (throws ScriptError on syntax errors); resets undo */
  void load(const std::string& p);

  std::string serialize() const;
  /** write the serialized AST to path; returns false on I/O failure */
  bool save();
  /** write the current AST to path WITHOUT clearing dirty (undo/redo need
   *  the runtime refreshed, but the document still differs from the last
   *  explicit save) */
  bool write();

  // --- editing / undo ---------------------------------------------------------
  /** start an edit gesture named NAME; coalesces with a pending gesture of
   *  the same name younger than 0.6 s (drags call beginEdit once per frame) */
  void beginEdit(const char* name);
  /** end the gesture: push one undo entry if the AST actually changed */
  void endEdit();
  /** discard the pending gesture (a drag that ended without changes) */
  void cancelEdit();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  void undo();
  void redo();
  const std::string& lastOpName() const { return lastOp_; }

  // --- document queries used by the editor panels ------------------------------
  SceneDef* findScene(const std::string& name);
  const SceneDef* findScene(const std::string& name) const;
  /** all `anim` commands anywhere in the script (scene setup/blocks + main),
   *  as stable pointers into the AST (valid until the next adopt/undo/redo) */
  std::vector<Cmd*> animCmds();
  /** the top-level block implementing `marker NAME`, or nullptr */
  ScriptBlock* findMarker(const std::string& name);

  // --- marker ops (the CALLER wraps these in beginEdit/endEdit) ---------------
  bool addMarker(const std::string& name, float t);
  bool removeMarker(const std::string& name);
  bool renameMarker(const std::string& oldName, const std::string& newName);
  /** move `marker NAME` to time T; splits a shared block into its own
   *  `at T { marker NAME }` block when the marker is not alone in it */
  bool moveMarker(const std::string& name, float t);
  std::vector<std::string> markerNames() const;

private:
  struct UndoEntry {
    std::string name;
    Script ast;   // AST before the edit
  };
  struct Pending {
    bool valid = false;
    std::string name;
    Script ast;   // snapshot taken at beginEdit
    double t0 = 0;
  };
  std::vector<UndoEntry> undo_, redo_;
  Pending pending_;
  std::string lastOp_;

  static double now();
};

}  // namespace ns
