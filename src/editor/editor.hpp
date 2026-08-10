// ---------------------------------------------------------------------------
// DemoEditor - the dockable demo editor (ImGui docking, GLFW + OpenGL 3.3).
//
// Runs the real engine every frame (audio -> director -> timeline -> DemoApp
// update/render), captures the presented frame into a viewport texture, and
// wraps it in a Unity/Godot-style dockable shell:
//
//   Toolbar (transport + show state)    Menu bar (File/Transport/View/Debug)
//   Hierarchy (scene graph + effects)   Inspector (node transform/payload)
//   Viewport (live preview + HUD)       Timeline (tracks/events/markers)
//   Console (captured log)              Assets (resource browser)
//   Profiler (frame graph + per-effect GPU ms)
//
// Layouts persist automatically through the ImGui ini file; "Layout > Reset"
// rebuilds the default docking arrangement. All state edits (scene graph,
// transform, payloads) go through the director-owned graph, which is re-read
// every frame - so inspector changes update the live preview immediately.
// ---------------------------------------------------------------------------
#pragma once

#include "app/demoapp.hpp"
#include "engine/camera.hpp"
#include "engine/directortime.hpp"
#include "engine/framebuffer.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include "framework/core/json.hpp"  // Value: panel-visibility persistence
#include "framework/script/scriptparser.hpp"  // beatSec()
#include "editor/document.hpp"            // EditorDocument (AST + undo)
#include "editor/exportmp4.hpp"           // Mp4Export (File > Export MP4...)
#include "editor/packaging.hpp"            // project NSP/ZIP distribution
#include "editor/shaderlab.hpp"            // typography-focused shader workspace

#include <GLFW/glfw3.h>
#include <string>
#include <vector>

namespace ns {

/** small keyframe button drawn with the draw list (the default ImGui font
 *  has no diamond glyph); used by the inspector to keyframe a property row */
bool editorKeyframeButton(const char* id);

class DemoEditor {
public:
  struct Wiring {
    DemoApp* app = nullptr;
    Renderer* r = nullptr;
    Camera* camera = nullptr;
    AudioEngine* audio = nullptr;
    Assets* assets = nullptr;
    Timeline* timeline = nullptr;
    PostFX* postfx = nullptr;
    DirectorTime* director = nullptr;
    GLFWwindow* window = nullptr;
    void (*toggleFullscreen)() = nullptr;
    std::string shaderDir, assetDir, dataDir;
    float maxSeconds = 0;   // >0: auto-close after N seconds (CI smoke mode)
    bool noTrack = false;   // --no-track: run silent, never restore a saved track
    std::string trackOverride;  // --track=FILE: explicit track, never restore a saved one
  };

  explicit DemoEditor(const Wiring& w);
  ~DemoEditor();
  DemoEditor(const DemoEditor&) = delete;
  DemoEditor& operator=(const DemoEditor&) = delete;

  /** one full editor frame; returns false when the window should close */
  bool frame();
  void shutdown();

  /** queue "+ Scene": appends a new scene block to the demo script and
   *  reloads the show. Applied at the next frame start (safe point). */
  void queueAddScene() { sceneAddQueued_ = true; }
  /** open the New Asset dialog (creates a data file from a template) */
  void openAssetDialog();

private:
  struct ConsoleLine {
    int level = 2;          // 0 err, 1 warn, 2 info, 3 debug
    std::string text;
  };

  Wiring w_;
  FrameTarget viewport_;     // blit target for the live preview
  double last_ = 0;
  bool stepPending_ = false;
  bool layoutBuilt_ = false;
  bool uiUp_ = false;        // ImGui context created (shutdown is idempotent)
  bool uiFontLoaded_ = false;
  float fps_ = 0, frameMs_ = 0;
  std::vector<float> hist_; // frame-time ring for the profiler sparkline

  // panels
  bool showToolbar_ = true;
  bool showHierarchy_ = true, showInspector_ = true, showTimeline_ = true;
  bool showConsole_ = true, showAssets_ = true, showProfiler_ = true;
  bool showShaderLab_ = false;
  bool showNsdCommands_ = false;
  bool fullscreenPreview_ = false;
  // panel visibility snapshot taken on entering fullscreen preview
  bool savedToolbar_ = true, savedHierarchy_ = true, savedInspector_ = true,
       savedTimeline_ = true, savedConsole_ = true, savedAssets_ = true, savedProfiler_ = true;
  bool showDemo_ = false, showMetrics_ = false, showAbout_ = false;

  // console ring
  std::vector<ConsoleLine> console_;
  char filter_[160] = "";
  int levelFilter_ = 3;      // max level shown (0 err only .. 3 all)
  bool consoleFollow_ = true;

  // selection
  SceneNode* selNode_ = nullptr;
  std::string lastActiveScene_;
  std::string selEffect_;
  std::string selAsset_;
  std::string selScene_;       // selected declaration from the loaded .nsd
  char sceneFilter_[96] = "";
  char sceneTitleBuf_[256] = "";
  std::vector<char> sceneSetupBuf_;  // editable setup command source
  std::string sceneEditScene_;       // scene represented by the buffers
  bool sceneSetupDirty_ = false;
  // Persistent text-editor storage. Keeping this outside inspectNode() avoids
  // re-seeding ImGui's active InputText state from a short-lived stack buffer
  // every frame, which could drop edits while the field was still focused.
  std::vector<char> textEditBuf_ = std::vector<char>(4096, '\0');
  std::string textEditNodeKey_;
  std::vector<char> textFontEditBuf_ = std::vector<char>(512, '\0');
  std::string textFontEditNodeKey_;

  // Screen-space text placement in the live viewport. Text nodes are rendered
  // as 2D overlays, so their position is edited in normalized screen space
  // and then written back to the text command's `pos` option on mouse release.
  bool textNodeDragging_ = false;
  std::string textDragNodeKey_;
  bool textDragMoved_ = false;

  void drawSceneList();
  void inspectScene();
  void loadSceneEditorBuffers();
  bool applySceneSetup();


  // timeline view
  float tlZoom_ = 75.0f;     // visible seconds
  float tlT0_ = 0.0f;        // left edge (seconds)
  // fit toggle: the view saved before fitting the whole show, so F (or the
  // Fit button) can restore it. fitZoom_ < 0 means no saved view.
  float tlFitZoom_ = -1.0f;
  float tlFitT0_ = 0.0f;

  // --- viewport input forwarding / fly camera --------------------------------
  bool viewportFocused_ = false;   // viewport clicked (transport "belongs" to it)
  bool viewportHovered_ = false;   // mouse over the viewport image (last frame)
  bool flyActive_ = false;         // right-drag fly camera engaged
  bool flyByRmb_ = false;          // entered via RMB hold (exit on release) vs menu/smoke
  float flyYaw_ = 0, flyPitch_ = 0;
  V3 flyPos_{0, 0, 0};
  float flySpeed_ = 8.0f;          // world units / second
  // Cursor-position callback accumulator. Polling glfwGetCursorPos() while
  // GLFW_CURSOR_DISABLED is unreliable on some Windows/driver combinations;
  // the callback provides relative motion even when the cursor is locked.
  double flyMouseDx_ = 0, flyMouseDy_ = 0;
  double flyMouseLastX_ = 0, flyMouseLastY_ = 0;
  bool flyMousePrimed_ = false;
  GLFWcursorposfun previousCursorPos_ = nullptr;
  bool flySpacePrev_ = false;      // edge-triggered pause while flying

  // programmatic fly smoke (NS_EDITOR_FLY_SMOKE=1 + --editor-seconds=N):
  // enter the fly camera, move it, exit - CI proof of the capture/override path
  bool smokeFly_ = false;
  bool smokeFlyEntered_ = false;
  bool smokeFlyExited_ = false;
  double flySmokeT_ = 0;

  // --- scene / asset authoring ------------------------------------------------
  bool sceneAddQueued_ = false;   // "+ Scene" applied at frame start
  bool assetOpen_ = false;        // New Asset dialog visible
  int assetKind_ = 0;             // 0 material, 1 post preset, 2 scene, 3 shader, 4 shadertoy
  char assetName_[64] = "";
  bool assetCreated_ = false;
  std::string assetCreatedPath_;  // full path of the last created asset
  bool newProjectConfirmOpen_ = false;
  std::string pendingNewProjectPath_;

  // NS_EDITOR_SCENE_SMOKE=1: programmatic Add Scene (queue -> append -> reload
  // -> land on the new section) so CI can prove the button's data path without
  // a human clicking. Run with --demo=PATH on a copy of the script.
  bool smokeScene_ = false;
  bool sceneSmokeQueued_ = false;
  bool sceneSmokeDone_ = false;
  int sceneSmokeBefore_ = 0;
  double sceneSmokeT_ = 0;

  // NS_EDITOR_ASSET_SMOKE=1: create a material + post preset through the New
  // Asset dialog's code path, load both into the show (proving the templates
  // parse), then delete the smoke files.
  bool smokeAsset_ = false;
  bool assetSmokeDone_ = false;
  double assetSmokeT_ = 0;

  // NS_EDITOR_SCRUB_SMOKE=1: prove scrubbing establishes the scene at the
  // target (a seek used to re-arm the fire boundary past every crossed
  // `show` event, leaving the pre-scrub scene rendering): forward scrub into
  // Cathedral must activate it, backward scrub into Intro must re-establish
  // it.
  bool smokeScrub_ = false;
  bool scrubSmokeQueued_ = false;
  bool scrubSmokeDone_ = false;
  bool scrubSmokePhase2_ = false;
  bool scrubFwdOk_ = false;
  int scrubSmokeWait_ = 0;  // frames since the last seek (dispatch happens next frame)
  double scrubSmokeT_ = 0;

  // --- audio source controls ---------------------------------------------------
  char audioPath_[512] = "";       // path input in the Audio popup
  std::vector<std::string> audioCandidates_;  // *.wav/*.mp3 found on disk
  double audioScanT_ = 0;          // re-scan cadence (dropped files appear)

  // --- shared asset browser -----------------------------------------------------
  // One modal ("Open Asset") serves every file category - audio, texture,
  // shader, model, script - with per-kind scan roots, extensions and actions.
  // Each kind remembers its own scan root + last pick in editor_state.json
  // (browsers.<key>), so every picker reopens where you were. Menu and
  // Browse... actions use the platform picker on Windows, with the existing
  // in-editor browser retained as a cross-platform fallback. Roots + picks
  // are stored as absolute paths, consistent with the persisted track.
  enum class BrowseKind : int { Audio, Texture, Shader, Model, Script, Count };
  struct AssetBrowse {
    bool scanned = false;   // listed once per kind (Rescan forces it)
    char root[512] = "";    // scan root; empty = kind defaults
    std::vector<std::string> files;
    std::string sel;        // last clicked entry (Open applies it)
    double scanT = 0;       // last-scan timestamp (auto-refresh cadence)
    float scanMs = 0;       // last scan duration (cadence scales to it)
    double flashT = -1e9;   // last detected change ("updated" flash)
  };
  bool browseOpen_ = false;     // the modal is visible
  bool browseWasOpen_ = false;  // close-edge detection (persist on close)
  int browseKind_ = 0;          // kind the modal shows (persisted)
  AssetBrowse browse_[(int)BrowseKind::Count];  // per-kind state
  void openBrowse(int kind);          // open the in-editor browser on a category
  /** open the platform file picker; falls back to the in-editor browser when unavailable */
  bool openNativeFileDialog(int kind);
  /** open the popup rooted at ROOT (a dropped folder), listing that folder */
  void openBrowseRoot(int kind, const std::string& root);
  void scanAssetBrowser(int kind);    // (re)list files under the kind's root
  void browseAutoRefresh(int kind);   // cadence re-scan; flashes on change
  void drawBrowse();                  // the "Open Asset" popup
  void pickBrowseFile(int kind, const std::string& path);  // apply a kind action
  /** load a texture pick and assign it to a Sprite target (the pick action's
   *  shared core, used by the modal, the viewport drop and hierarchy drops) */
  void applyTexturePick(const std::string& path, SceneNode* target);
  std::vector<std::string> browseRoots(int kind) const;  // kind default dirs
  /** path under BASE as a relative name ("" if not under it); normalizes
   *  separators + absolutes so Windows native paths compare cleanly */
  static std::string browseRelPath(const std::string& base, const std::string& file);

  // --- OS-level drag-in (GLFW drop) -----------------------------------------
  // files dropped from Explorer/file managers land on the viewport and reuse
  // the exact same pick dispatch as the browser (kind resolved by extension)
  struct OsDrop {
    std::string path;
    double x = 0, y = 0;  // drop point, GLFW content coords
  };
  std::vector<OsDrop> osDrops_;   // queued by the GLFW callback, drained per frame
  bool vpRectValid_ = false;      // last frame's viewport rect (ImGui coords)
  float vpRectMinX_ = 0, vpRectMinY_ = 0, vpRectMaxX_ = 0, vpRectMaxY_ = 0;
  static void glfwDropCallback(GLFWwindow* w, int count, const char** paths);
  static void glfwFlyCursorPosCallback(GLFWwindow* w, double x, double y);
  void queueOsDrop(const char* path);
  void drainOsDrops();            // position-gate + ext dispatch (per frame)
  static int kindForPath(const std::string& path);  // ext -> BrowseKind, -1 unknown
  /** best BrowseKind for a dropped folder's contents, -1 if none (recursive
   *  peek with an early exit so huge trees stay cheap) */
  static int inferKindForDir(const std::string& dir);

  // on-screen toast ring: recent OS-drop outcomes, drawn stacked over the
  // viewport HUD with per-toast fade-outs (so every outcome of a multi-file
  // drop is visible without reading the console)
  static constexpr int kToastRing = 4;  // newest N kept; oldest expires
  struct Toast {
    std::string text;
    double t0 = 0;    // wallNow() at show (seconds)
    int level = 2;    // 0 error, 1 warn, 2 info
  };
  std::vector<Toast> toasts_;   // newest last
  void showToast(const std::string& text, int level);

  // --- panel-aware OS drop routing -------------------------------------------
  // drops land on the panel under the cursor (z-priority = reverse draw
  // order), not just the viewport: Timeline takes audio/scripts, Console
  // takes shaders into a scratch view, Assets browses the file's folder.
  // Panels with split layouts record SUB-AREA rects too, so a drop lands on
  // the exact zone: the Timeline's waveform strip (audio) vs its lanes
  // (sequence), and the Console's header (tools) vs its log list (debug).
  enum class DropPanel : int { Viewport, Timeline, Console, Assets, Count };
  struct PanelRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // ImGui screen coords
    bool valid = false;
  };
  PanelRect panelRects_[(int)DropPanel::Count];  // filled by each panel's draw
  DropPanel panelAt(float x, float y) const;     // topmost panel under a point
  void recordPanelRect(DropPanel p);             // window rect of the panel

  enum class DropSub : int {
    TimelineLanes,   // ruler + track lanes (the sequence zone)
    TimelineStrip,   // waveform/spectrum strip (the audio zone)
    ConsoleHeader,   // filter/tool row (the tools zone)
    ConsoleLog,      // scrollable log list (the debug zone)
    Count
  };
  struct SubRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // ImGui screen coords
    bool valid = false;
  };
  SubRect subRects_[(int)DropSub::Count];   // filled by each panel's draw
  /** which sub-area of panel P contains (x, y); the panel's "rest" is the
   *  default zone (lanes / header) so every panel point routes somewhere */
  DropSub subAt(DropPanel p, float x, float y) const;
  void recordSubRect(DropSub s, float x0, float y0, float x1, float y1);

  /** each route returns its outcome ("applied"/"loading"/"steered"/"ignored…")
   *  so the drop history records it and re-runs can judge rejections */
  std::string routeViewportDrop(const std::string& path);  // folder -> browse, file -> pick
  std::string routeTimelineDrop(const std::string& path, DropSub sub);  // lanes/strip zones
  std::string routeConsoleDrop(const std::string& path, DropSub sub);   // header/log zones
  std::string routeAssetsDrop(const std::string& path);    // browse the file's folder

  // --- OS drop history (Console right-click) ---------------------------------
  // every OS drop this session, with the routed panel + sub-area + outcome;
  // clicking a record in the Console's right-click menu re-runs the action
  // (a record that was rejected falls back to the file kind's canonical
  // action, so the re-run does something useful instead of re-ignoring)
  struct DropRecord {
    std::string path;      // full dropped path
    std::string file;      // basename (display)
    std::string outcome;   // applied / loading / failed / steered / ignored…
    int panel = -1;        // DropPanel the drop was routed to
    int sub = -1;          // DropSub (-1 when the panel has no sub-areas)
    int kind = -1;         // BrowseKind by extension (-1 unknown)
    double t = 0;          // wallNow() at the drop
  };
  std::vector<DropRecord> dropHistory_;  // newest last
  static constexpr int kDropHistoryCap = 200;
  /** trailing-debounce window for the per-drop state save: a burst of drops
   *  writes the JSON once (when the window elapses after the last one) instead
   *  of once per file; shutdown() flushes a pending save immediately */
  static constexpr double kEditorSaveDebounceSec = 0.5;
  /** open the history popup without a right-click (View > OS Drop History;
   *  also how the smoke renders it live) - cleared when the popup closes */
  bool dropHistoryOpen_ = false;
  void recordDrop(const std::string& path, int panel, int sub, int kind,
                  const std::string& outcome);
  void rerunDrop(const DropRecord& rec);
  /** push a synthetic "session resumed from <date>" marker when a restored
   *  history is non-empty, so relaunches are visible in the trail (called once
   *  at boot, after restoreEditorState; display-only - never re-run/dragged) */
  void markSessionResume();
  /** route a browser/history drag payload dropped on PANEL at ImGui point
   *  (mx,my): the same routing an OS drop at that point would take (sub-area
   *  from the cursor - strip loads audio, lanes steer it, ...), and the
   *  re-dispatch is itself recorded in the drop history */
  std::string routePanelPayload(DropPanel panel, int kind,
                                const std::string& path, float mx, float my);
  /** ImGui drop target shared by the panels: accepts the browser/history
   *  payload and re-dispatches it via routePanelPayload at the cursor, with
   *  an amber hover border while a payload is dragged over the panel */
  void panelDragDropTarget(DropPanel panel);

  // shader scratch view: a dropped shader opens here as a LIVE SOURCE
  // EDITOR - the text is editable and Save (or Ctrl+S) writes it back to the
  // file and pokes an immediate recompile, so you can tweak a shader in place
  // and watch hot-reload update the viewport (no external editor needed)
  bool scratchOpen_ = false;
  std::string scratchPath_, scratchSrc_;
  std::vector<char> scratchBuf_;  // editable source buffer (InputTextMultiline)
  bool scratchDirty_ = false;     // unsaved edits in scratchBuf_
  static constexpr int kScratchCap = 65536;  // source display/edit cap
  void openShaderScratch(const std::string& path);
  /** add a .frag shader command to the selected/current scene and preview it */
  void addShaderToScene(const std::string& path);
  void drawScratch();
  bool loadScratchSource(const std::string& path);  // fills src + edit buffer
  void saveScratch();             // write the buffer back + poke a reload
  void reloadScratchFromDisk();   // discard edits, re-read the file

  // audio waveform strip (Timeline panel bottom)
  std::vector<float> audioEnv_;    // per-bucket peak envelope of the loaded track
  std::string audioEnvPath_;       // track path the envelope was built from
  uint64_t audioEnvFrames_ = 0;    // track frame count it was built from

  // live FFT spectrogram: per-frame snapshot of the engine's spectrum ring
  // (copied so rendering never races the audio thread's ring writer)
  std::vector<float> specSnap_;    // appended columns (kSpecBins each)
  std::vector<float> specTime_;    // capture time (seconds) per column
  uint32_t specSeen_ = 0;          // engine columns already copied

  // beat-marker editor: detected kick transients + the grid phase offset that
  // re-aligns the show's beat/bar grid to the actual track
  std::vector<float> kickTimes_;   // seconds of detected kick transients
  float beatOffset_ = 0;           // grid phase shift (seconds, added to n*beat)
  bool beatDrag_ = false;          // a beat grid line is being dragged
  int beatDragIdx_ = 0;            // beat index of the dragged line
  float beatDragBase_ = 0;         // offset when the drag started (anchor)
  void detectKicks();              // (re)compute kickTimes_ from the envelope data
  float estimateTrackBpm() const;  // infer tempo from transient spacing/envelope
  void applyDetectedBpm();         // update the active project + runtime timeline
  void autoAlignBeats();           // fit grid phase to the detected kicks
  float detectedBpm_ = 0.0f;       // last reliable estimate, shown in the audio UI

  // scrub quantization: when enabled, seekTo snaps the playhead + audio to the
  // nearest ALIGNED grid line (beatOffset_ + n*grid, so it follows the
  // beat-marker alignment). Q toggles; Shift+Q cycles beat/bar.
  bool quantize_ = false;
  int quantizeGrid_ = 0;           // 0 = beat, 1 = bar
  void seekToRaw(float t);         // unquantized seek (programmatic landings)

  // NS_EDITOR_AUDIO_SMOKE=1: runtime track swap silence->wav->silence + bogus
  // path guard, proven with a generated 0.25s WAV.
  bool smokeAudio_ = false;
  bool audioSmokeDone_ = false;
  double audioSmokeT_ = 0;

  // --- document model + authoring (editor_document.cpp) ---------------------
  EditorDocument doc_;          // the production document (parsed AST + dirty)
  uint64_t docReloadSeen_ = 0;  // app->reloadCount() at the last adopt
  std::string lastTitle_;       // last window title set (dirty-flag updates)
  void initDocument();          // adopt the app's parsed script at boot
  void syncDocumentFromApp();   // re-adopt when an EXTERNAL reload happened
  /** serialize the document to its .nsd + reload the show (clears dirty:
   *  the file now matches the document) */
  bool writeDocument();
  bool writeDocumentAs(const std::string& path);
  void saveDocument();          // Ctrl+S
  void saveDocumentAsDialog();  // Ctrl+Shift+S / File > Save Project As
  void newProjectDialog();      // File > New Project
  bool createNewProject(const std::string& path);
  void undoDocument();          // Ctrl+Z
  void redoDocument();          // Ctrl+Y
  void addSceneViaDocument();   // "+ Scene" as a document op
  /** NS_EDITOR_DOC_SMOKE=1: prove the document pipeline inside the running
   *  editor (add -> undo -> redo -> write -> runtime derivation) */
  void runDocSmoke(float dt);
  bool smokeDoc_ = false;
  bool docSmokeDone_ = false;
  bool docSmokePhase2_ = false;
  bool docSmokeOk_ = true;
  double docSmokeT_ = 0;
  std::string docDisplayName() const;  // "demo.nsd" / "demo.nsd *"

  // --- curve editor (editor_curves.cpp) -------------------------------------
  bool showCurves_ = false;     // View > Curves panel
  std::vector<Cmd*> curveCmds_; // doc_.animCmds() cached per frame
  int curveSel_ = -1;           // index into curveCmds_ (-1 = none)
  std::vector<int> selKeys_;    // selected key indices in the channel
  int dragKey_ = -1;            // key index being dragged (-1 = none)
  std::vector<float> dragOrigT_; // original key times at drag start (multi)
  std::vector<float> dragOrigV_; // original first-component values (multi)
  float dragKeyT0_ = 0, dragKeyV0_ = 0;  // dragged key's original t/v
  bool keyDragging_ = false;
  struct CurveClipKey { float t; Value v; std::string interp; };
  std::vector<CurveClipKey> curveClip_;
  void drawCurveEditor();
  void rebuildCurveList();
  void applyChannelToRuntime(Cmd& c);  // live preview via editorApplyAnim
  /** inspector keyframe button: keyframe node:<name>.<prop> in the active
   *  scene's setup at the current scene-relative time + value */
  void keyframeNodeProperty(SceneNode* n, const char* prop);
  /** snap a key time to the beat/bar grid (quantize_) or 1/60 s */
  float snapKeyTime(float t) const;

  // --- production markers (editor_markers.cpp) -------------------------------
  std::string markerEditName_;      // marker being edited ("" = dialog closed)
  char markerEditNameBuf_[64] = "";
  char markerEditTimeBuf_[32] = "";
  bool markerDragging_ = false;
  std::string markerDragName_;
  float markerDragT0_ = 0;          // marker time at drag start
  void drawMarkerEditDialog();
  void openMarkerEdit(const std::string& name);
  void markerDragBegin(const std::string& name);
  void markerDragMove(const std::string& name, float t);
  void markerDragEnd();

  // --- project packaging (packaging.cpp) -----------------------------------
  bool packageDialogOpen_ = false;
  bool packageHasResult_ = false;
  bool packageOk_ = false;
  char packageZipPath_[512] = "";
  std::string packageMessage_;
  void openPackageDialog();
  void startPackage(const std::string& outputZip);
  void drawPackageDialog();

  // --- MP4 export (editor_export.cpp) -------------------------------------
  bool exportDialogOpen_ = false;
  char exportPath_[512] = "";
  float exportFps_ = 60.0f;
  bool exportAudio_ = true;
  float exportElapsed_ = 0.0f;    // seconds captured so far
  double exportNextT_ = 0.0;      // next capture boundary (show seconds)
  Mp4Export export_;
  void openExportDialog();
  void startExport(const std::string& path, const std::string& audioOverride = "");
  void cancelExport();
  void pumpExport(float dt, int fbW, int fbH);
  void drawExportDialog();
  void onExportFinished();
  // export smoke (NS_EDITOR_EXPORT_SMOKE=path [NS_EDITOR_EXPORT_SECONDS=n]):
  // auto-starts an export at boot so CI can prove the capture pipeline
  bool smokeExport_ = false;
  std::string smokeExportPath_, smokeAudioPath_;
  // NS_EDITOR_PACKAGE_SMOKE=path.zip: package the loaded project at boot and
  // print a machine-readable verdict for CI without opening the menu.
  bool smokePackage_ = false;
  bool smokePackageStarted_ = false;
  std::string smokePackagePath_;
  float smokeExportSeconds_ = 3.0f;
  bool smokeExportStarted_ = false;

  // --- NSD command authoring -------------------------------------------------
  // A discoverable command palette for the full DSL. The generated source is
  // intentionally editable so advanced users can reach every parser option
  // without leaving the editor.
  int nsdCommandIndex_ = 0;
  int nsdCommandTarget_ = 0;  // 0 = selected scene setup, 1 = timeline/playhead
  std::vector<char> nsdCommandBuf_ = std::vector<char>(8192, '\0');
  std::string nsdCommandSeed_;
  bool nsdCommandDirty_ = false;
  void drawNsdCommands();
  void seedNsdCommandSource();
  bool insertNsdCommand();

  // --- shader lab ------------------------------------------------------------
  ShaderLab shaderLab_;
  void insertShaderLabIntoTimeline();

  // --- impl -----------------------------------------------------------------
  void initImGui();
  void applyTheme();
  void captureViewport();
  void buildDefaultLayout(unsigned dockspaceId);
  void pollViewportInput(float dt);
  void applyFlyCamera(float dt);
  void enterFly();
  void exitFly();
  void toggleFly();

  // queued authoring ops (frame start, before the engine step)
  void applyQueuedActions();
  void drawNewAssetDialog();
  void drawNewProjectConfirm();
  void drawAudioPopup();
  void rescanAudioCandidates();
  void applyAudioTrack(const std::string& path);
  void pumpAsyncAudioSwap();  // commit a finished background decode (each frame)
  void rebuildAudioEnvelope();

  // editor state persistence (data/editor_state.json): the chosen audio track
  // survives relaunches; the plain demo path is unaffected. Explicit CLI flags
  // (--no-track / --track=FILE) take precedence over a restored track.
  std::string editorStatePath() const { return w_.dataDir + "/editor_state.json"; }
  void saveEditorState();
  /** trailing debounce: mark the state dirty and (re)arm the 500ms deadline;
   *  frame() flushes once the deadline passes, shutdown() flushes immediately,
   *  so a large multi-file drop writes once instead of N times */
  void scheduleSaveEditorState();
  /** true when a save is pending and the debounce window has elapsed */
  bool saveDue() const;
  /** write now if pending (frame flush / shutdown) */
  void flushPendingSave();
  void restoreEditorState();
  /** apply the persisted panel visibility from a parsed state Value (the
   *  'panels' section) - split out of restore so the smoke can prove it
   *  without re-restoring the track etc. */
  void applyPanelVisibility(const Value& v);
  bool saveDirty_ = false;    // a schedule is waiting on the trailing timer
  double lastSaveWall_ = 0;   // steady_clock time of the last schedule
  long saveWrites_ = 0;       // real JSON writes (smoke + debug affordance)
  /** write the asset file described by the dialog's Type+Name (the New Asset
   *  Create button and the asset smoke share this path); false if invalid */
  bool createAssetFromForm();
  std::string assetTargetPath() const;
  /** same as assetTargetPath() but for a caller-supplied clean name (so the
   *  free-name suggestion can probe candidates without touching the dialog) */
  std::string assetTargetPathFor(const std::string& clean) const;
  /** first free name at the current Type: the typed name, else <name>2,3,...
   *  (like the + Scene button's SceneN auto-naming); empty when invalid */
  std::string suggestFreeAssetName() const;
  std::string assetTemplate() const;
  static std::string sanitizeAssetName(const char* in);

  // panels
  void drawMenuBar();
  void drawToolbar();
  void drawViewportPanel();
  void drawHierarchy();
  void drawInspector();
  void drawTimeline();
  void drawConsole();
  void drawAssets();
  void drawProfiler();

  // helpers
  void drawNodeRec(SceneNode* n);
  void inspectNode(SceneNode* n);
  void inspectEffect(Effect* e);
  void seekTo(float t);
  void fitTimeline();
  // per-show timeline view (data/editor_state.json "timelineViews"): every
  // show script remembers its zoom/scroll/fit window - restored at boot and
  // when switching shows, written on view changes and script switches
  std::string showKey() const;
  /** restore key's saved window; returns false when the show has none yet */
  bool applyTimelineViewForShow(const std::string& key);
  void switchShow(const std::string& path);
  void duplicateNode(SceneNode* n);
  void deleteNode(SceneNode* n);
  void handleKeys();
  void togglePlayback();
  void toggleFullscreenPreview();
  static const char* iconFor(NodeType t);
  static const char* typeLabel(NodeType t);
  std::string fmtTime(float t) const;
  /** current audio source for the TRK buttons: the track filename (trimmed)
   *  or "silent" when none is loaded. Shared so the toolbar + menu bar labels
   *  can't drift apart. */
  std::string trackLabel() const;
  void pushConsole(const std::string& line);
  bool isEffectActive(const std::string& name) const;
};

}  // namespace ns
