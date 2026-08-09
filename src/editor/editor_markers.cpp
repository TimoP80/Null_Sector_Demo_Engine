// ---------------------------------------------------------------------------
// editor_markers.cpp - first-class production marker editing.
//
// Runtime timeline markers are DERIVED from `marker NAME` commands inside
// `at T { ... }` blocks (ScriptEngine::build). The document is the source of
// truth: adding / moving / renaming / removing a marker edits the AST,
// commits the document (write + reload + seek back) and the runtime timeline
// rebuilds from the file.
//
// Timeline ruler interaction:
//   click         jump the transport to the marker
//   drag          move the marker (live runtime preview; committed on release)
//   double-click  open the edit dialog (name / time / snap)
// ---------------------------------------------------------------------------
#include "editor/editor.hpp"
#include "framework/core/log.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ns {

void DemoEditor::openMarkerEdit(const std::string& name) {
  markerEditName_ = name;
  std::snprintf(markerEditNameBuf_, sizeof markerEditNameBuf_, "%s",
                name.c_str());
  const ScriptBlock* b = doc_.findMarker(name);
  std::snprintf(markerEditTimeBuf_, sizeof markerEditTimeBuf_, "%.3f",
                b ? b->time : 0.0f);
}

void DemoEditor::drawMarkerEditDialog() {
  if (markerEditName_.empty()) return;
  if (!ImGui::BeginPopupModal("Edit Marker", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextUnformatted("Marker name:");
  ImGui::InputText("##markername", markerEditNameBuf_,
                   sizeof markerEditNameBuf_);
  ImGui::TextUnformatted("Time (seconds):");
  ImGui::InputText("##markertime", markerEditTimeBuf_,
                   sizeof markerEditTimeBuf_);
  ImGui::TextDisabled("grid snap while dragging: %s (%s, Q toggles)",
                      quantize_ ? "on" : "off",
                      quantizeGrid_ == 1 ? "bar" : "beat");

  bool apply = false, del = false, cancel = false;
  if (ImGui::Button("Apply")) apply = true;
  ImGui::SameLine();
  if (ImGui::Button("Delete")) del = true;
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) cancel = true;

  if (cancel) {
    markerEditName_.clear();
    ImGui::CloseCurrentPopup();
  }
  if (apply) {
    const std::string newName = markerEditNameBuf_;
    const float t = (float)std::atof(markerEditTimeBuf_);
    doc_.beginEdit("edit marker");
    bool ok = false;
    if (!newName.empty() && newName != markerEditName_) {
      ok = doc_.renameMarker(markerEditName_, newName);
      if (ok) {
        markerEditName_ = newName;
        ok = doc_.moveMarker(newName, t);
      } else {
        Log::warn("EDITOR", std::string("marker rename to '") + newName +
                   "' rejected (empty or taken)");
      }
    } else {
      ok = doc_.moveMarker(markerEditName_, t);
    }
    doc_.endEdit();
    if (ok) {
      writeDocument();
      char lbb[256];
      std::snprintf(lbb, sizeof lbb, "marker '%s' -> %.3fs", markerEditName_.c_str(), t);
      Log::info("EDITOR", lbb);
    }
    markerEditName_.clear();
    ImGui::CloseCurrentPopup();
  }
  if (del) {
    doc_.beginEdit("delete marker");
    doc_.removeMarker(markerEditName_);
    doc_.endEdit();
    writeDocument();
    Log::info("EDITOR", std::string("marker '") + markerEditName_ + "' deleted");
    markerEditName_.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DemoEditor::markerDragBegin(const std::string& name) {
  markerDragging_ = true;
  markerDragName_ = name;
  const ScriptBlock* b = doc_.findMarker(name);
  markerDragT0_ = b ? b->time : 0;
  doc_.beginEdit("move marker");
}

/** live preview: move the runtime marker only (drawTimeline re-reads the
 *  timeline each frame); the document is updated at markerDragEnd */
void DemoEditor::markerDragMove(const std::string& name, float t) {
  TimelineEditor& te = w_.app->editableEditor();
  for (auto& m : te.markers) {
    if (m.name == name) {
      m.time = t;
      break;
    }
  }
  std::stable_sort(te.markers.begin(), te.markers.end(),
                   [](const TimelineMarker& a, const TimelineMarker& b) {
                     return a.time < b.time;
                   });
}

void DemoEditor::markerDragEnd() {
  markerDragging_ = false;
  float finalT = markerDragT0_;
  for (const auto& m : w_.app->editor().markers)
    if (m.name == markerDragName_) { finalT = m.time; break; }
  const bool moved = std::fabs(finalT - markerDragT0_) > 0.05f;
  if (moved) {
    doc_.moveMarker(markerDragName_, finalT);
    doc_.endEdit();
    writeDocument();
    Log::info("EDITOR", std::string("marker '") + markerDragName_ + "' moved to " +
              std::to_string((double)finalT) + "s");
  } else {
    // a press without movement is a jump, not an edit: undo any sub-5ms
    // nudge the live preview applied during the press, then jump
    doc_.cancelEdit();
    TimelineEditor& te = w_.app->editableEditor();
    for (auto& m : te.markers)
      if (m.name == markerDragName_) { m.time = markerDragT0_; break; }
    seekTo(markerDragT0_); // click = jump the transport to the marker
  }
  markerDragName_.clear();
}

}  // namespace ns
