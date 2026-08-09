// ---------------------------------------------------------------------------
// DemoEditor MP4 export (File > Export MP4...).
//
// The capture itself is Mp4Export (editor/exportmp4.cpp): this file owns the
// editor-side wiring - the save dialog, starting the export (which restarts
// the show from 0:00 so the capture is the full production, music included),
// feeding it one presented frame per capture boundary, and the progress/
// result UI. The export runs IN PROCESS: the muxed audio track is the very
// audio the show reacts to, so the MP4 is sample-accurately synced by
// construction, and unsaved document edits are included (the preview stays
// live while it captures).
// ---------------------------------------------------------------------------
#include "editor/editor.hpp"

#include "framework/core/log.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace ns {

void DemoEditor::openExportDialog() {
  if (exportPath_[0] == '\0' && w_.app) {
    std::string base = std::filesystem::path(w_.app->scriptPath()).stem().string();
    if (base.empty()) base = "export";
    std::snprintf(exportPath_, sizeof(exportPath_), "%s.mp4", base.c_str());
  }
  exportDialogOpen_ = true;
}

void DemoEditor::startExport(const std::string& path,
                             const std::string& audioOverride) {
  if (export_.running()) return;
  int fbW = 0, fbH = 0;
  glfwGetFramebufferSize(w_.window, &fbW, &fbH);
  if (fbW < 2 || fbH < 2) {
    Log::error("EDITOR", "export: no framebuffer to capture");
    return;
  }
  std::string audioPath = audioOverride;
  if (audioPath.empty() && exportAudio_) {
    audioPath = w_.audio ? w_.audio->trackPath() : "";
    if (!audioPath.empty() && !std::filesystem::exists(audioPath)) {
      Log::warn("EDITOR",
                "export: track is not a real file (inside a package?) - video only");
      audioPath.clear();
    }
  }
  if (!export_.start(path, fbW, fbH, exportFps_, audioPath)) {
    Log::error("EDITOR", "export failed to start: " + export_.error());
    return;
  }
  // run the production once from 0:00 (music included, so the muxed track
  // matches what the show reacts to)
  if (w_.director) {
    w_.director->init(0);
    w_.director->paused = false;
  }
  if (w_.app) w_.app->seek(0);
  if (w_.audio) w_.audio->seekTrack(0);
  exportElapsed_ = 0.0f;
  exportNextT_ = 0.0;
  Log::info("EDITOR", "export started: " + path +
                          (audioPath.empty() ? " (video only)"
                                             : " (muxing " + audioPath + ")"));
}

void DemoEditor::cancelExport() {
  if (!export_.running()) return;
  export_.cancel();
  onExportFinished();
}

void DemoEditor::pumpExport(float dt, int fbW, int fbH) {
  if (!export_.running()) return;
  // a paused show would freeze the capture while the muxed audio keeps
  // playing - force playback for the duration of the export
  if (w_.director && w_.director->paused) w_.director->paused = false;
  exportElapsed_ += dt;
  const float dur = w_.app ? w_.app->editor().duration : 0.0f;
  // the smoke caps the capture; a real export runs the whole production
  const float cap = smokeExportSeconds_ > 0.0f ? smokeExportSeconds_ : dur;
  if (cap > 0.0f && exportElapsed_ >= cap) {
    // drain the final boundaries so the video ends exactly at the cap /
    // show end (duplicates of the last rendered frame, like the CLI path)
    while (exportNextT_ <= (double)cap) {
      export_.pushFrame();
      exportNextT_ += 1.0 / exportFps_;
    }
    export_.finish();
    onExportFinished();
  }
}

void DemoEditor::drawExportDialog() {
  if (!exportDialogOpen_) return;
  ImGui::OpenPopup("Export MP4");
  if (!ImGui::BeginPopupModal("Export MP4", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const Mp4Export::State st = export_.state();
  if (st == Mp4Export::State::Running) {
    const float dur = w_.app ? w_.app->editor().duration : 0.0f;
    const float cap = smokeExportSeconds_ > 0.0f ? smokeExportSeconds_ : dur;
    const float pct = cap > 0.0f ? std::min(exportElapsed_ / cap, 1.0f) : 0.0f;
    ImGui::ProgressBar(pct, ImVec2(340.0f, 0.0f));
    ImGui::TextUnformatted(export_.path().c_str());
    ImGui::TextDisabled("%.1fs / %.1fs   |   %zu frames   |   %zu dropped",
                        exportElapsed_, cap > 0.0f ? cap : 0.0f,
                        export_.framesWritten(), export_.framesDropped());
    ImGui::TextDisabled("the preview stays live - don't scrub or switch the track");
    if (ImGui::Button("Cancel Export")) cancelExport();
  } else if (st == Mp4Export::State::Done ||
             st == Mp4Export::State::Cancelled) {
    const bool ok = st == Mp4Export::State::Done;
    ImGui::TextUnformatted(ok ? "Export complete." : "Export cancelled.");
    ImGui::TextDisabled("%s", export_.path().c_str());
    ImGui::TextDisabled("%zu frames, %zu dropped, ffmpeg rc=%d",
                        export_.framesWritten(), export_.framesDropped(),
                        export_.exitCode());
    if (ImGui::Button("Close")) exportDialogOpen_ = false;
  } else if (st == Mp4Export::State::Failed) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                       export_.error().c_str());
    if (ImGui::Button("Close")) exportDialogOpen_ = false;
  } else {
    // Idle: the setup form
    ImGui::InputText("Output file", exportPath_, sizeof(exportPath_));
    ImGui::SetItemTooltip("absolute path, or relative to the working directory");
    ImGui::InputFloat("FPS", &exportFps_, 1.0f, 10.0f, "%.1f");
    exportFps_ = std::max(1.0f, std::min(exportFps_, 240.0f));
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(w_.window, &fbW, &fbH);
    ImGui::TextDisabled("Resolution: %dx%d (resize the editor window to change)",
                        fbW, fbH);
    ImGui::Checkbox("Mux the playing audio track", &exportAudio_);
    ImGui::TextDisabled("The show restarts at 0:00 and runs once; the preview");
    ImGui::TextDisabled("stays live, so unsaved edits are included. Don't scrub");
    ImGui::TextDisabled("or switch the track while it captures.");
    if (ImGui::Button("Export")) startExport(exportPath_);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) exportDialogOpen_ = false;
  }
  ImGui::EndPopup();
}

void DemoEditor::onExportFinished() {
  const bool ok = export_.state() == Mp4Export::State::Done &&
                  export_.framesWritten() > 0;
  Log::info("EDITOR", std::string(ok ? "export done" : "export finished") +
                          ": " + export_.path() + " (" +
                          std::to_string(export_.framesWritten()) + " frames, " +
                          std::to_string(export_.framesDropped()) +
                          " dropped, rc=" + std::to_string(export_.exitCode()) + ")");
  if (smokeExport_) {
    std::fprintf(stderr,
                 "[EDITOR-EXPORT-SMOKE] %s: %zu frames, %zu dropped (rc=%d) -> %s\n",
                 ok ? "OK" : "FAIL", export_.framesWritten(),
                 export_.framesDropped(), export_.exitCode(),
                 export_.path().c_str());
  }
}

}  // namespace ns
