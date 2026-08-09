// ---------------------------------------------------------------------------
// NULL SECTOR // GHOST IN THE MACHINE - native app shell (thin).
// Creates the window + GL context + the engine subsystems, wires them into
// the DemoApp data-driven director, and runs the frame loop. There is NO show
// logic here: the demo itself (scenes, cameras, effects, animations, post,
// models, shadertoys) lives entirely in data/demo.nsd + the JSON/shaders/
// assets it references. Edit those files while it runs and they hot-reload.
//
// Keys:
//   ESC       quit          Space     pause / resume
//   Left/Right scrub a bar  R         restart at 0:00
//   0 / 1     jump section  F11       toggle fullscreen
//   M         mark cue      L         toggle section loop
//   +/-       timescale     F2        reload the demo script
//
// Flags:
//   --check-production[=P]  headless production validation (GL-free, runs
//                           before the window opens): parse the .nsd, build
//                           the timeline, and verify every scene/effect/
//                           shadertoy/model/material/preset/rig reference
//                           resolves against the registry + files on disk;
//                           prints a checklist and exits 0/1 for CI
//   --check-shaders         compile every engine + app shader program, then exit
//   --check-models          headless-ish 3D pipeline preflight (OBJ -> lit
//                           shader -> draw readback + shipped data), then exit
//   --check-hotreload       live-reload smoke: boot the app, break + fix a
//                           temp shader, verify keep-previous + recompile
//                           programmatically, then exit 0/1 for CI
//   --check-shadertoy       render every data/shadertoy/*.glsl offscreen +
//                           pixel readback (catches the image-pass feedback
//                           / wrong-target regressions), then exit
//   --smoke-audio           decode the track headless + analyser self-test
//   --demo=PATH             demo script (default: data/demo.nsd)
//   --plugin=DIR            effect plugin directory (default: data/plugins)
//   --track=FILE            use a specific WAV/MP3 (default: auto-search)
//   --no-track              run with no music file (silent)
//   --font=FILE             TrueType font for text (default: assets/fonts/*.ttf)
//   --perf-json[=PATH]      write one GPU-time sample per effect (shadertoy/
//                           scene/particles) + the post stack to PATH
//                           (default perf.json) at exit, for scripted A/B
//                           comparisons of renderScale options
//   --perf-csv[=PATH]       append one row per second (t, kind, name,
//                           context, ms) for every ACTIVE timed effect +
//                           the post stack, so renderScale tuning can be
//                           plotted over time (default perf.csv)
//   --perf-raw[=PATH]       append one row per COLLECTED sample with the
//                           UNSMOOTHED ms (t, kind, name, context, ms) -
//                           the spikes the per-second EMA hides (default
//                           perf.raw.csv; plot with --mode=raw)
//   --perf-seconds=N        with --perf-json/--perf-csv/--perf-raw:
//                           auto-exit after N seconds and dump (the show
//                           loops forever otherwise)
//   --editor                dockable Demo Editor (ImGui): live viewport,
//                           scene hierarchy, inspector, timeline, console,
//                           assets, profiler; layouts persist in imgui.ini
//   --editor-seconds=N      with --editor: auto-close after N seconds (CI)
//   --windowed              start in a window (default is fullscreen)
//   --fullscreen            start in fullscreen (default)
//   --window=WxH            initial window size when windowed (default 1600x900)
// ---------------------------------------------------------------------------
#include "app/demoapp.hpp"
#include "app/effectreg.hpp"
#include "app/hotreloadcheck.hpp"
#include "app/modelcheck.hpp"
#include "app/prodcheck.hpp"
#include "app/shadermanager.hpp"
#include "app/shadertoycheck.hpp"
#include "editor/editor.hpp"
#include "engine/audio.hpp"
#include "engine/assets.hpp"
#include "engine/camera.hpp"
#include "engine/directortime.hpp"
#include "engine/gl.hpp"
#include "engine/postprocess.hpp"
#include "engine/renderer.hpp"
#include "engine/shadercheck.hpp"
#include "engine/timeline.hpp"
#include "engine/ubo.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace ns {}  // keep everything in the global TU scope below

using namespace ns;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static double wallNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** find a playable track: common names in cwd/exe dir. (An explicit --track
 *  is handled by splitTrackList - the first entry plays at boot.) */
static std::string findTrack() {
  const char* cands[] = {"ghostinthemachine.mp3", "audio.mp3", "assets/audio.mp3",
                         "ghostinthemachine.wav", "audio.wav", "assets/audio.wav"};
  for (const char* c : cands) {
    if (std::filesystem::exists(c)) return c;
  }
  return "";
}

/** every playable track on disk under the standard folders (cwd, assets/,
 *  data/) - the runtime T / Shift+T cycling source. Mirrors the editor's
 *  candidate scan so the plain demo and the editor agree on what exists. */
static std::vector<std::string> scanTracks() {
  std::vector<std::string> out;
  const char* dirs[] = {".", "assets", "data"};
  for (const char* d : dirs) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
      if (ec) break;
      if (e.is_directory()) continue;
      const std::string ext = e.path().extension().string();
      if (ext == ".wav" || ext == ".mp3") out.push_back(e.path().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

/** --track=FILE1,FILE2,FILE3: split into the explicit cycle list (whitespace
 *  trimmed, empty entries dropped). T / Shift+T then rotates THIS list instead
 *  of the disk scan, so scripted A/B sessions pin the exact files. */
static std::vector<std::string> splitTrackList(const std::string& in) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= in.size()) {
    const size_t comma = in.find(',', pos);
    std::string e = in.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const auto b = e.find_first_not_of(" \t");
    if (b != std::string::npos) {
      const auto en = e.find_last_not_of(" \t");
      e = e.substr(b, en - b + 1);
    } else {
      e.clear();
    }
    if (!e.empty()) out.push_back(std::move(e));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

static GLFWwindow* g_window = nullptr;
static bool g_fullscreen = false;
static int g_winX = 0, g_winY = 0, g_winW = 1600, g_winH = 900;

static void toggleFullscreen() {
  g_fullscreen = !g_fullscreen;
  if (g_fullscreen) {
    glfwGetWindowPos(g_window, &g_winX, &g_winY);
    glfwGetWindowSize(g_window, &g_winW, &g_winH);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = glfwGetVideoMode(mon);
    glfwSetWindowMonitor(g_window, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
  } else {
    glfwSetWindowMonitor(g_window, nullptr, g_winX, g_winY, g_winW, g_winH, 0);
  }
}

// director handle for the captureless speed lambda (wired into the DemoApp)
static DirectorTime* g_director = nullptr;

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  std::string trackOverride, fontOverride, demoPath, pluginDir, perfJsonPath, perfCsvPath,
      perfRawPath, checkProductionPath, shotFile;
  bool noTrack = false, checkShaders = false, smokeAudio = false, checkModels = false,
       checkHotReload = false, checkShadertoy = false;
  float shotSeconds = -1;  // --shot=SECONDS:FILE.bmp: seek there, save one frame, exit
  bool shotNoSeek = false;  // --shot-noseek: run from 0, capture at target
  bool editorMode = false;  // --editor: dockable demo editor instead of the plain loop
  bool fullscreen = true;   // the demo is a show: fullscreen by default
  int winW = 1600, winH = 900;
  float perfSeconds = 0;    // --perf-json/--perf-csv/--perf-raw runs: auto-exit after N s
  float editorSeconds = 0;  // --editor-seconds=N: auto-close the editor after N s (CI)

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--check-production") checkProductionPath = AppAssets::dataDir() + "/demo.nsd";
    else if (a.rfind("--check-production=", 0) == 0) checkProductionPath = a.substr(19);
    else if (a == "--check-shaders") checkShaders = true;
    else if (a == "--check-models") checkModels = true;
    else if (a == "--check-hotreload") checkHotReload = true;
    else if (a == "--check-shadertoy") checkShadertoy = true;
    else if (a == "--smoke-audio") smokeAudio = true;
    else if (a == "--no-track") noTrack = true;
    else if (a == "--fullscreen") fullscreen = true;
    else if (a == "--windowed") fullscreen = false;
    else if (a.rfind("--track=", 0) == 0) trackOverride = a.substr(8);
    else if (a.rfind("--font=", 0) == 0) fontOverride = a.substr(7);
    else if (a.rfind("--demo=", 0) == 0) demoPath = a.substr(7);
    else if (a.rfind("--plugin=", 0) == 0) pluginDir = a.substr(9);
    else if (a.rfind("--window=", 0) == 0) {
      int w = 0, h = 0;
      if (std::sscanf(a.c_str() + 9, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { winW = w; winH = h; }
    } else if (a == "--perf-json") perfJsonPath = "perf.json";
    else if (a.rfind("--perf-json=", 0) == 0) perfJsonPath = a.substr(12);
    else if (a == "--perf-csv") perfCsvPath = "perf.csv";
    else if (a.rfind("--perf-csv=", 0) == 0) perfCsvPath = a.substr(11);
    else if (a == "--perf-raw") perfRawPath = "perf.raw.csv";
    else if (a.rfind("--perf-raw=", 0) == 0) perfRawPath = a.substr(11);
    else if (a.rfind("--perf-seconds=", 0) == 0) perfSeconds = (float)std::atof(a.c_str() + 15);
    else if (a == "--shot-noseek") shotNoSeek = true;
    else if (a.rfind("--shot=", 0) == 0) {
      const std::string v = a.substr(7);
      const size_t c = v.find(':');
      if (c != std::string::npos) {
        shotSeconds = (float)std::atof(v.substr(0, c).c_str());
        shotFile = v.substr(c + 1);
      }
    } else if (a == "--editor") editorMode = true;
    else if (a.rfind("--editor-seconds=", 0) == 0) editorSeconds = (float)std::atof(a.c_str() + 17);
    else if (a == "--help" || a == "-h") {
      std::printf("NULL SECTOR // GHOST IN THE MACHINE (data-driven)\\n"
                  "  --check-production[=PATH]  headless production validation (no GL): parse\\n"
                  "                  the .nsd + verify every scene/effect/asset/rig reference\\n"
                  "                  resolves (default: data/demo.nsd), then exit 0/1\\n"
                  "  --check-shaders  compile every engine + app shader stage, then exit\\n"
                  "  --check-models   headless-ish 3D pipeline preflight, then exit\\n"
                  "  --check-hotreload  live-reload smoke (break+fix a temp shader), then exit\\n"
                  "  --check-shadertoy  render data/shadertoy/*.glsl offscreen + readback, then exit\\n"
                  "  --smoke-audio    headless track decode + analyser self-test\\n"
                  "  --demo=PATH      demo script (default: data/demo.nsd)\\n"
                  "  --plugin=DIR     effect plugin directory (default: data/plugins)\\n"
                  "  --track=F1,F2,..  play F1 at boot; T / Shift+T cycles the comma list\\n"
                  "                  (entries can't contain commas; default: scan cwd/assets/data)\\n"
                  "  --no-track       run with no music file (silent)\\n"
                  "  --font=FILE      TrueType font for text (default: assets/fonts/*.ttf)\\n"
                  "  --perf-json[=PATH]  write per-effect GPU samples (default perf.json) at exit\\n"
                  "  --perf-csv[=PATH]   append per-second GPU-time rows (default perf.csv)\\n"
                  "  --perf-raw[=PATH]   append every collected raw sample (default perf.raw.csv)\\n"
                  "  --perf-seconds=N    auto-exit after N s + dump (scripted A/B runs)\\n"
                  "  --editor         dockable demo editor (ImGui) - live preview + timeline\\n"
                  "  --editor-seconds=N  with --editor: auto-close after N s (CI smoke)\\n"
                  "  --windowed       start in a window (default is fullscreen)\\n"
                  "  --fullscreen     start in fullscreen (default)\\n"
                  "  --shot=SEC:FILE.bmp  seek to SEC, save one presented frame, exit\n" \
                  "  --window=WxH     window size when windowed (default 1600x900)\\n");
      return 0;
    }
  }

  // --perf-seconds only makes sense with a perf output (the auto-exit is
  // what lets scripted A/B runs finish); warn instead of silently doing
  // nothing
  if (perfSeconds > 0 && perfJsonPath.empty() && perfCsvPath.empty() && perfRawPath.empty()) {
    std::fprintf(stderr, "[MAIN] --perf-seconds=N without --perf-json/--perf-csv/--perf-raw "
                         "has no effect - add one to enable the scripted perf dump\n");
  }
  if (editorMode && (!perfJsonPath.empty() || !perfCsvPath.empty() || !perfRawPath.empty())) {
    std::fprintf(stderr, "[MAIN] --editor ignores --perf-json/--perf-csv/--perf-raw "
                         "(the editor has its own Profiler panel)\n");
  }

  // the editor is a tool, not the show: windowed by default, a sensible size
  if (editorMode) {
    fullscreen = false;
    if (winW == 1600 && winH == 900) { winW = 1680; winH = 960; }
  }

  // dev preflight: validate the production HEADLESSLY - no GL, no window. The
  // parser + timeline + effect registry + filesystem are all the check needs,
  // so it runs before the context exists (CI without a display can use it).
  if (!checkProductionPath.empty()) {
    registerBuiltinEffects();
    const ProdCheckResult r = checkProduction(checkProductionPath, AppAssets::dataDir(),
                                              AppAssets::shaderDir());
    for (const auto& f : r.failures) std::fprintf(stderr, "[PRODCHECK] FAIL: %s\n", f.c_str());
    std::fprintf(stderr, "[PRODCHECK] preflight: %d/%d checks ok - %s\n", r.ok, r.total,
                 r.failures.empty() ? "production valid" : "production INVALID");
    return r.failures.empty() ? 0 : 1;
  }

  // --- window + GL -----------------------------------------------------------
  if (!glfwInit()) { std::fprintf(stderr, "[MAIN] glfwInit failed\n"); return 1; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  g_window = glfwCreateWindow(winW, winH,
                              editorMode ? "NULL SECTOR // DEMO EDITOR"
                                         : "NULL SECTOR // GHOST IN THE MACHINE",
                              nullptr, nullptr);
  if (!g_window) { std::fprintf(stderr, "[MAIN] window creation failed\n"); glfwTerminate(); return 1; }
  glfwMakeContextCurrent(g_window);
  glfwSwapInterval(1);
  // the smoke modes run headless-ish: keep them in a window (some CI boxes
  // have no real display mode to enter)
  if (fullscreen && !checkHotReload && !checkShadertoy) toggleFullscreen();
  if (!glLoadFunctions()) { std::fprintf(stderr, "[MAIN] GL function load failed\n"); return 1; }

  int fbW = 0, fbH = 0;
  glfwGetFramebufferSize(g_window, &fbW, &fbH);

  // dev preflight: compile every ENGINE shader stage up front, plus the
  // app-layer forward-lit program (ShaderManager) - catches include/UBO
  // regressions at dev time instead of mid-show
  if (checkShaders) {
    const ShaderCheckResult engine = compileAllShaders();
    std::fprintf(stderr, "[SHADER] engine preflight: %d/%d stages ok\n", engine.ok, engine.total);
    int rc = engine.failed > 0 ? 1 : 0;
    try {
      ShaderManager sm;
      sm.get("lit.vert", "lit.frag");
      std::fprintf(stderr, "[SHADER] app preflight: lit.vert + lit.frag ok\n");
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[SHADER] app preflight failed: %s\n", e.what());
      rc = 1;
    }
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return rc;
  }

  // dev preflight: exercise the 3D pipeline (OBJ import -> lit shader ->
  // ModelRenderer draw + pixel readback, plus the shipped models/materials)
  // so model/material regressions fail before the show, not in it
  if (checkModels) {
    const ModelCheckResult r = checkModelPipeline();
    std::fprintf(stderr, "[MODEL] preflight: %d/%d checks ok\n", r.ok, r.total);
    for (const auto& f : r.failedItems) std::fprintf(stderr, "[MODEL] FAIL: %s\n", f.c_str());
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return r.failed > 0 ? 1 : 0;
  }

  // dev preflight: compile + render every shipped shadertoy offscreen with a
  // pixel readback, so a pass-pipeline regression (image pass drawing into
  // the last buffer instead of the scene target, or a feedback loop going
  // solid) fails before the show, not in it
  if (checkShadertoy) {
    const ShadertoyCheckResult r = checkShadertoyPipeline();
    std::fprintf(stderr, "[SHADERTOY] preflight: %d/%d checks ok\n", r.ok, r.total);
    for (const auto& f : r.failedItems) std::fprintf(stderr, "[SHADERTOY] FAIL: %s\n", f.c_str());
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return r.failed > 0 ? 1 : 0;
  }

  // --- engine objects ----------------------------------------------------------
  Renderer renderer;
  renderer.resize(fbW, fbH);

  // --- text font: TrueType override, else the embedded 8x8 bitmap font --------
  Assets assets = buildFontAtlas();
  {
    std::string fontPath = fontOverride;
    if (fontPath.empty()) {
      const std::string pref = assetDir() + "/fonts/intro.ttf";
      if (std::filesystem::exists(pref)) {
        fontPath = pref;
      } else {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(assetDir() + "/fonts", ec)) {
          if (ec) break;
          const std::string ext = e.path().extension().string();
          if (ext == ".ttf" || ext == ".otf") { fontPath = e.path().string(); break; }
        }
      }
    }
    if (!fontPath.empty()) {
      Assets ttf = buildTrueTypeFontAtlas(fontPath);
      if (ttf.fontTex.tex) assets = std::move(ttf);
      else std::fprintf(stderr, "[MAIN] TrueType load failed - using the 8x8 bitmap font\n");
    } else {
      std::printf("[MAIN] no TrueType font found - using the embedded 8x8 bitmap font\n");
    }
  }

  SharedBlock shared;
  PostFX postfx(renderer);
  Camera camera;
  camera.resize(fbW, fbH);

  // --- audio -------------------------------------------------------------------
  AudioEngine audio;
  audio.init();
  // --track=F1,F2,...: an explicit cycle list for scripted A/B runs - the
  // first entry plays at boot and T / Shift+T rotates the whole list
  const std::vector<std::string> cliTracks = splitTrackList(trackOverride);
  if (!noTrack) {
    const std::string track = cliTracks.empty() ? findTrack() : cliTracks[0];
    if (!track.empty()) {
      audio.loadTrack(track);
    } else {
      std::fprintf(stderr, "[MAIN] no WAV/MP3 track found - the show will run silent (no built-in synth)\n");
    }
  } else {
    std::printf("[MAIN] --no-track: running silent (no music file)\n");
  }

  if (smokeAudio) {
    if (!audio.trackMode) {
      std::fprintf(stderr, "[AUDIO-SMOKE] no track loaded - use --track=FILE or drop a WAV/MP3 in cwd\n");
    } else {
      audio.selfTest();
    }
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
  }

  // --- director / timeline -------------------------------------------------------
  Timeline timeline;
  DirectorTime director;
  director.init(0);
  g_director = &director;

  // --- the data-driven show --------------------------------------------------------
  DemoApp app;
  DemoApp::Input in;
  in.r = &renderer;
  in.assets = &assets;
  in.shared = &shared;
  in.postfx = &postfx;
  in.camera = &camera;
  in.audio = &audio;
  in.timeline = &timeline;
  in.showClock = &director.show;
  in.directorPaused = &director.paused;
  in.setDirectorScale = [](float s) { g_director->setScale(s); };
  in.scriptPath = demoPath.empty() ? AppAssets::dataDir() + "/demo.nsd" : demoPath;
  in.pluginDir = pluginDir.empty() ? AppAssets::dataDir() + "/plugins" : pluginDir;

  // --check-hotreload: the temp shader must exist BEFORE init so the
  // watcher's baseline scan covers it - the whole watcher -> pollLiveReload
  // -> ShaderManager chain is then exercised when we mutate it live
  std::string hrFile;
  if (checkHotReload) {
    hrFile = AppAssets::shaderDir() + "/" + kHotReloadCheckFrag;
    std::error_code ec;
    std::filesystem::remove(hrFile, ec);  // clear a stale leftover
    {
      std::ofstream f(hrFile);
      f << kHotReloadFragValid;
    }
  }

  try {
    app.init(in);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[MAIN] DemoApp init failed: %s\n", e.what());
    if (checkHotReload) std::filesystem::remove(hrFile);
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 1;
  }

  // --perf-csv / --perf-raw: the app starts appending rows from the main loop
  if (!perfCsvPath.empty()) app.beginPerfCsv(perfCsvPath);
  if (!perfRawPath.empty()) app.beginPerfRaw(perfRawPath);

  // live-reload smoke mode: verify keep-previous + recompile programmatically
  if (checkHotReload) {
    int rc = 1;
    try {
      rc = app.runHotReloadCheck();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[HOTRELOAD] FAIL: check aborted: %s\n", e.what());
      std::filesystem::remove(hrFile);
    }
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return rc;
  }

  // music + show clock start together (the script fades in from black);
  // without a track the show runs silent but the clock keeps moving
  audio.start();

  // --shot=SECONDS:FILE.bmp: jump the show + track straight to the target
  // (a little early so the audio analyser has ~1s of history when the frame
  // is read) - used for frame capture during visual iteration
  if (shotSeconds >= 0.0f) {
    if (shotNoSeek) {
      std::fprintf(stderr, "[SHOT] no-seek mode: capturing %s at %.1fs\n",
                   shotFile.c_str(), shotSeconds);
    } else {
      const float target = std::max(0.0f, shotSeconds - 3.5f);
      audio.seekTrack(target);
      director.init(target);
      app.seek(target);
      std::fprintf(stderr, "[SHOT] seeking to %.1fs, will save %s at %.1fs\n",
                   target, shotFile.c_str(), shotSeconds);
    }
  }

  // --- demo editor mode: the engine runs inside a dockable ImGui shell -----------
  if (editorMode) {
    DemoEditor::Wiring ew;
    ew.app = &app;
    ew.r = &renderer;
    ew.camera = &camera;
    ew.audio = &audio;
    ew.timeline = &timeline;
    ew.postfx = &postfx;
    ew.director = &director;
    ew.window = g_window;
    ew.toggleFullscreen = toggleFullscreen;
    ew.shaderDir = AppAssets::shaderDir();
    ew.assetDir = assetDir();
    ew.dataDir = AppAssets::dataDir();
    ew.maxSeconds = editorSeconds;
    ew.noTrack = noTrack;
    ew.trackOverride = trackOverride;
    {
      DemoEditor editor(ew);
      const double editorStart = wallNow();
      while (editor.frame()) {
        if (editorSeconds > 0 && wallNow() - editorStart >= editorSeconds) {
          glfwSetWindowShouldClose(g_window, 1);
        }
      }
      editor.shutdown();
    }
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
  }

  // --- input state ----------------------------------------------------------------
  std::array<bool, 512> prevKeys{};

  // --- main loop --------------------------------------------------------------------
  double last = wallNow();
  const double perfStart = wallNow();
  std::fprintf(stderr, "[MAIN] NULL SECTOR // GHOST IN THE MACHINE - data-driven demo\n");

  // runtime track switching (T / Shift+T): an explicit --track list, or the
  // playable files on disk; swapped in asynchronously so the decode never
  // hitches the show
  std::vector<std::string> demoTracks = cliTracks;
  const bool cliCycle = !cliTracks.empty();  // explicit list: never re-scan
  int demoTrackIdx = -1;
  bool demoTrackHint = false, demoSwapPending = false;
  std::string demoSwapPath;
  // on-screen track readout (bottom-left, demo caption style): text + seconds
  // remaining; 0 hides it. Set on every phase (decoding -> committed/failed).
  std::string demoTrackToast;
  float demoTrackToastT = 0.0f;
  const float kDemoToastSec = 3.0f;

  while (!glfwWindowShouldClose(g_window)) {
    // --perf-json/--perf-csv scripted runs: exit after N seconds so
    // renderScale A/B comparisons can be automated (the show loops forever
    // otherwise)
    if (perfSeconds > 0 &&
        (!perfJsonPath.empty() || !perfCsvPath.empty() || !perfRawPath.empty()) &&
        wallNow() - perfStart >= perfSeconds) {
      std::fprintf(stderr, "[PERF] run complete after %.1fs - dumping\n", perfSeconds);
      glfwSetWindowShouldClose(g_window, 1);
      break;
    }

    const double frameStart = wallNow();
    glfwPollEvents();
    if (glfwGetKey(g_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(g_window, 1);

    // --- resize ---------------------------------------------------------------------
    int cw = 0, ch = 0;
    glfwGetFramebufferSize(g_window, &cw, &ch);
    if (cw != renderer.viewW || ch != renderer.viewH) {
      renderer.resize(cw, ch);
      postfx.resize();
      camera.resize(cw, ch);
      app.resize(cw, ch);
    }

    // discoverability: the plain demo has no UI, so say once what T does when
    // tracks exist (scripted --perf runs keep their stdout clean)
    if (!demoTrackHint && perfSeconds <= 0) {
      demoTrackHint = true;
      if (cliCycle) {
        std::fprintf(stderr,
                     "[AUDIO] --track list: %d file(s) - T / Shift+T cycles them\n",
                     (int)cliTracks.size());
        // a typo'd A/B list surfaces at boot, not on the first T press
        int missing = 0;
        for (const auto& t : cliTracks)
          if (!std::filesystem::exists(t)) missing++;
        if (missing > 0)
          std::fprintf(stderr,
                       "[AUDIO] note: %d of %d --track file(s) missing "
                       "(those presses fail and keep the current source)\n",
                       missing, (int)cliTracks.size());
      } else {
        demoTracks = scanTracks();
        if (!demoTracks.empty())
          std::fprintf(stderr,
                       "[AUDIO] %d track(s) on disk - press T / Shift+T to switch\n",
                       (int)demoTracks.size());
      }
    }

    // --- show clock -----------------------------------------------------------------
    audio.update();
    director.advance(audio.now());
    timeline.advance(director.show);

    // end of show: loop from 0:00 (the app re-arms its timeline on the jump)
    if (director.show >= app.editor().duration - 0.01f && app.editor().duration > 0) {
      director.init(0);
      app.seek(0);
    }

    // commit a finished background decode (a T / Shift+T track switch)
    if (demoSwapPending) {
      const auto st = audio.asyncStatus();
      if (st == ns::AudioEngine::AsyncState::Ready) {
        demoSwapPending = false;
        if (audio.applyAsyncSwap()) {
          const std::string name =
              std::filesystem::path(audio.trackPath()).filename().string();
          std::fprintf(stderr, "[AUDIO] demo track %d/%d: %s\n",
                       demoTrackIdx + 1, (int)demoTracks.size(),
                       audio.trackPath().c_str());
          glfwSetWindowTitle(g_window, ("NULL SECTOR - " + name).c_str());
          demoTrackToast = "track " + std::to_string(demoTrackIdx + 1) + "/" +
                           std::to_string((int)demoTracks.size()) + ": " + name;
          demoTrackToastT = kDemoToastSec;
        } else {
          std::fprintf(stderr, "[AUDIO] swap failed: %s (kept previous)\n",
                       demoSwapPath.c_str());
          demoTrackToast = "track load failed: " +
                           std::filesystem::path(demoSwapPath)
                               .filename()
                               .string();
          demoTrackToastT = kDemoToastSec;
        }
      } else if (st == ns::AudioEngine::AsyncState::Failed) {
        demoSwapPending = false;
        std::fprintf(stderr, "[AUDIO] decode failed: %s (kept previous)\n",
                     demoSwapPath.c_str());
        demoTrackToast =
            "track load failed: " +
            std::filesystem::path(demoSwapPath).filename().string();
        demoTrackToastT = kDemoToastSec;
      }
    }

    // edge-triggered keys
    auto pressed = [&](int k) {
      const bool now = glfwGetKey(g_window, k) == GLFW_PRESS;
      const bool p = now && !prevKeys[k];
      prevKeys[k] = now;
      return p;
    };
    if (pressed(GLFW_KEY_SPACE)) director.togglePause();
    if (pressed(GLFW_KEY_LEFT)) director.scrubBar(-1);
    if (pressed(GLFW_KEY_RIGHT)) director.scrubBar(1);
    if (pressed(GLFW_KEY_R)) { director.init(0); app.seek(0); }
    if (pressed(GLFW_KEY_F11)) toggleFullscreen();
    if (pressed(GLFW_KEY_F2)) app.reloadScript();
    // T / Shift+T: switch the playing audio to the next/previous track found
    // on disk (async decode - the show keeps playing until the swap commits)
    if (pressed(GLFW_KEY_T)) {
      if (demoTracks.empty()) demoTracks = scanTracks();
      if (demoTracks.empty()) {
        std::fprintf(stderr,
                     "[AUDIO] no .wav/.mp3 found (cwd, assets/, data/) - "
                     "drop one in or use --track=FILE\n");
      } else {
        const bool back =
            glfwGetKey(g_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(g_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        const int n = (int)demoTracks.size();
        if (demoTrackIdx < 0) {  // nothing selected yet: land on the ends
          demoTrackIdx = back ? n - 1 : 0;
        } else {
          const int next = (demoTrackIdx + (back ? -1 : 1) + n) % n;
          // wrapped past the end/start: re-scan so files dropped mid-run
          // appear (the editor rescans live; the demo does it on the wrap) -
          // an explicit --track list is never re-scanned
          if (!cliCycle && next == (back ? n - 1 : 0)) demoTracks = scanTracks();
          demoTrackIdx = next;
        }
        // the fresh scan may have shrunk the list (files deleted mid-run)
        if (demoTrackIdx >= (int)demoTracks.size())
          demoTrackIdx = (int)demoTracks.size() - 1;
        demoSwapPath = demoTracks[demoTrackIdx];
        audio.beginAsyncSwap(demoSwapPath, director.show);
        demoSwapPending = true;
        demoTrackToast =
            "decoding " +
            std::filesystem::path(demoSwapPath).filename().string() + "...";
        demoTrackToastT = kDemoToastSec;
        std::fprintf(stderr, "[AUDIO] decoding %s\n", demoSwapPath.c_str());
      }
    }
    if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) director.setScale(director.scale() * 1.4f);
    if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) director.setScale(director.scale() / 1.4f);
    if (pressed(GLFW_KEY_M)) director.mark();
    if (pressed(GLFW_KEY_L)) director.toggleLoop();
    if (pressed(GLFW_KEY_0)) app.jumpSection(-1);
    if (pressed(GLFW_KEY_1)) app.jumpSection(1);

    const double now = wallNow();
    const float dt = (float)(now - last);
    last = now;

    // --- the show (all data-driven) ---------------------------------------------------
    app.update(director.show, dt);
    camera.update(dt);       // handheld shake / smoothing on top of the rig
    app.render();

    // the on-screen track readout: fade in fast, hold ~2.4s, fade out over
    // the last 0.6s (drawn over the presented frame, so it never affects the
    // show's own rendering)
    if (demoTrackToastT > 0) {
      demoTrackToastT -= dt;
      if (demoTrackToastT < 0.0f) demoTrackToastT = 0.0f;  // dt is unclamped
      const float age = kDemoToastSec - demoTrackToastT;  // 0..kDemoToastSec
      float a = 1.0f;
      if (age < 0.15f) a = age / 0.15f;
      else if (demoTrackToastT < 0.6f) a = demoTrackToastT / 0.6f;
      app.drawToast(demoTrackToast, a);
    }

    // --perf-csv: one row per second of the current EMA of every active
    // timed effect + the post stack (reads stats only - no GL)
    if (!perfCsvPath.empty()) app.perfCsvTick(dt);

    // --perf-raw: one row per newly collected UNSMOOTHED sample (spikes)
    if (!perfRawPath.empty()) app.perfRawTick(dt);

    // --shot: once the show clock reaches the target, read the presented
    // frame (default framebuffer, before the swap) and write a 24-bit BMP
    if (shotSeconds >= 0.0f && !shotFile.empty() && director.show >= shotSeconds) {
      int fbW = 0, fbH = 0;
      glfwGetFramebufferSize(g_window, &fbW, &fbH);
      if (fbW > 0 && fbH > 0) {
        std::vector<unsigned char> px((size_t)fbW * fbH * 3);
        ::glReadPixels(0, 0, fbW, fbH, ::gl::RGB, ::gl::UNSIGNED_BYTE, px.data());
        const size_t rowBytes = (size_t)fbW * 3;
        const size_t stride = (rowBytes + 3u) & ~3u;
        const size_t dataSize = stride * (size_t)fbH;
        std::vector<unsigned char> bmp(14 + 40 + dataSize, 0);
        bmp[0] = 'B'; bmp[1] = 'M';
        const uint32_t fileSize = (uint32_t)(14 + 40 + dataSize);
        std::memcpy(&bmp[2], &fileSize, 4);
        const uint32_t off = 54;
        std::memcpy(&bmp[10], &off, 4);
        bmp[14] = 40;  // BITMAPINFOHEADER size
        std::memcpy(&bmp[18], &fbW, 4);
        std::memcpy(&bmp[22], &fbH, 4);
        bmp[26] = 1; bmp[27] = 0;   // planes
        bmp[28] = 24; bmp[29] = 0;  // bpp
        // GL rows are bottom-up (row 0 = bottom) == BMP row order; only the
        // byte order differs (BGR). Flip RGB -> BGR on the fly.
        for (int y = 0; y < fbH; y++) {
          const unsigned char* src = px.data() + (size_t)y * rowBytes;
          unsigned char* dst = bmp.data() + 54 + (size_t)y * stride;
          for (int x = 0; x < fbW; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
          }
        }
        std::ofstream out(shotFile, std::ios::binary);
        out.write((const char*)bmp.data(), (std::streamsize)bmp.size());
        out.close();
        {
          // diagnostic: pixel stats over the READBACK, so a shot run reports
          // pass/fail (content vs black) without needing to open the BMP
          unsigned long long sum = 0;
          int mn = 256, mx = -1, n = fbW * fbH;
          for (int i = 0; i < n; i++) {
            const int v = (int)px[(size_t)i * 3] + (int)px[(size_t)i * 3 + 1] + (int)px[(size_t)i * 3 + 2];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += (unsigned)v;
          }
          const int mean = n > 0 ? (int)(sum / (unsigned)n) : 0;
          std::fprintf(stderr, "[SHOT] stats: min=%d mean=%d max=%d (%dx%d)\n",
                       mn / 3, mean / 3, mx / 3, fbW, fbH);
        }
      }
      glfwSetWindowShouldClose(g_window, 1);
    }

    glfwSwapBuffers(g_window);

    const float frameMs = (float)((wallNow() - frameStart) * 1000.0);
    renderer.tick(frameMs, frameMs);
  }

  // --perf-json: one GPU-time sample per effect + the post stack, written at
  // exit while the effects/PostFX still exist (reads stats only - no GL)
  if (!perfJsonPath.empty()) {
    app.writePerfJson(perfJsonPath);
  }

  // --perf-csv / --perf-raw: flush + close the sample files
  app.finishPerfCsv();
  app.finishPerfRaw();

  glfwDestroyWindow(g_window);
  glfwTerminate();
  return 0;
}
