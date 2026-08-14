// ---------------------------------------------------------------------------
// DemoEditor implementation (see editor.hpp for the layout overview).
// ImGui docking branch (vendored in third_party/imgui) + GLFW + OpenGL 3.3.
// ---------------------------------------------------------------------------
#include "editor/editor.hpp"

#include "framework/core/json.hpp"
#include "framework/core/log.hpp"
#include "framework/script/nsdwriter.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

namespace ns {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static double wallNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/** wall-clock seconds since the epoch - for PERSISTED timestamps (drop
 *  history): steady_clock time is boot-relative and meaningless after a
 *  restart, while this survives and renders correctly via strftime */
static double wallClockNow() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

static ImU32 c32(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
  return IM_COL32((int)r, (int)g, (int)b, (int)a);
}
static ImU32 c32f(float r, float g, float b, float a = 1.0f) {
  return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(a * 255));
}

// drag-and-drop from the Open Asset browser: rows carry a kind + full path
// payload; the viewport and Sprite nodes accept it and forward to
// pickBrowseFile/applyTexturePick (declared here so the early draw functions
// can reference it)
const char* const kBrowseDragType = "NS_BROWSE_ASSET";
struct BrowseDragPayload {
  int kind = 0;
  char path[512] = {};
};

/** horizontal-scroll clamp for the timeline: the visible window [t0, t0+zoom]
 *  stays inside [0, max(dur, zoom)] - zoomed past the show duration the
 *  window pins at 0. Shared by drawTimeline and the smoke. */
static float clampTlT0(float t0, float zoom, float dur) {
  const float maxT0 = std::max(0.0f, std::max(dur, zoom) - zoom);
  return t0 < 0.0f ? 0.0f : (t0 > maxT0 ? maxT0 : t0);
}

/** geometry of the timeline horizontal scrollbar: maxT0 = furthest left-edge
 *  time, hw = handle width (the window's share of the show, clamped so it
 *  stays usable), span = the distance the handle can travel. Pure so the
 *  smoke can assert the drag mapping, not just the clamp. */
struct TlScrollGeom {
  float maxT0 = 0.0f;
  float hw = 0.0f;
  float span = 1.0f;
};
static TlScrollGeom tlScrollGeom(float w, float zoom, float dur) {
  TlScrollGeom g;
  const float d = std::max(dur, zoom);
  g.maxT0 = std::max(0.0f, d - zoom);
  g.hw = std::clamp(w * zoom / d, 24.0f, w);
  g.span = std::max(1.0f, w - g.hw);
  return g;
}

/** map a cursor x offset (relative to the track's left edge, already centered
 *  on the handle by the caller) to the clamped left-edge time. Shared by
 *  drawTimeline and the smoke. */
static float tlScrollValue(float mx, const TlScrollGeom& g, float zoom,
                           float dur) {
  if (g.maxT0 <= 0.0f) return 0.0f;
  return clampTlT0(g.maxT0 * (mx / g.span), zoom, dur);
}

/** timeline fit toggle: the first call SAVES the current view into
 *  (fitZoom, fitT0) and fits the whole show (zoom = duration, scroll to 0);
 *  the second call RESTORES the saved view and clears the toggle. fitZoom
 *  < 0 means no saved view (not in fit mode). Pure so the smoke can assert
 *  both directions without a GL context. */
static void tlFitApply(float dur, float& zoom, float& t0, float& fitZoom,
                       float& fitT0) {
  if (fitZoom >= 0.0f) {  // already fitted -> restore where we were
    zoom = fitZoom;
    t0 = fitT0;
    fitZoom = -1.0f;
  } else {  // fit the whole show, remembering the current view
    fitZoom = zoom;
    fitT0 = t0;
    zoom = std::max(dur, 8.0f);  // never below the min zoom window
    t0 = 0.0f;
  }
}

/** ScriptEngine names top-level timeline blocks as at:<index>:<milliseconds>.
 * Keep the parser local to the editor so delete/duplicate only touch commands
 * that have a stable, lossless mapping back to EditorDocument::ast.main. */
static bool topLevelTimelineIndex(const std::string& name, int& index) {
  if (name.rfind("at:", 0) != 0) return false;
  const size_t begin = 3;
  const size_t end = name.find(':', begin);
  if (end == std::string::npos || end == begin) return false;
  try {
    size_t used = 0;
    index = std::stoi(name.substr(begin, end - begin), &used);
    return used == end - begin && index >= 0;
  } catch (...) {
    return false;
  }
}

static Value cameraVecValue(const V3& v) {
  Value::Array a;
  a.push_back(Value((double)v[0]));
  a.push_back(Value((double)v[1]));
  a.push_back(Value((double)v[2]));
  return Value(std::move(a));
}

// OS-level drag-in: GLFW drop callbacks carry no user data, so the active
// editor registers itself here (constructor) and forwards drops into its
// per-frame queue. Only one editor exists at a time.
DemoEditor* g_osDropTarget = nullptr;
DemoEditor* g_flyCursorTarget = nullptr;

// the effect name the quad:/shadertoy: dispatch builds for a shader path -
// shared by pickBrowseFile and the OS-drop success check so the two can
// never drift (extension comparison is case-insensitive, like kindForPath)
static std::string browseEffectName(const std::string& path,
                                    const std::string& shaderDir,
                                    const std::string& dataDir) {
  const std::filesystem::path p(path);
  std::string ext = p.extension().string();
  for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  const std::string name = p.filename().string();
  // name form when the file sits in its canonical folder (quad -> shaders/,
  // shadertoy -> data/shadertoy); otherwise the absolute path - the shader
  // manager / shadertoy loader read an existing path directly, so a shader
  // dropped from ANY folder (data/shaders, the exe dir, ...) actually shows
  const std::string abs = std::filesystem::absolute(path).string();
  if (ext == ".glsl") {
    const std::string canon = dataDir + "/shadertoy/" + name;
    return std::filesystem::exists(canon) ? "shadertoy:" + name
                                          : "shadertoy:" + abs;
  }
  const std::string canon = shaderDir + "/" + name;
  return std::filesystem::exists(canon) ? "quad:" + name : "quad:" + abs;
}

// phosphor/CRT palette matching the docs design system
static const ImU32 kPhosphor = c32(94, 240, 200);

// waveform envelope resolution: peak buckets per second of track (~17ms each)
static constexpr float kAudioEnvPerSec = 60.0f;
static const ImU32 kError = c32(255, 96, 96);
static const ImU32 kHot = c32(255, 95, 143);
static const ImU32 kAmber = c32(255, 196, 107);
static const ImU32 kBlue = c32(125, 179, 255);
static const ImU32 kViolet = c32(177, 140, 255);
static const ImU32 kDanger = c32(255, 107, 107);
static const ImU32 kDim = c32(133, 146, 167);
static const ImU32 kFaint = c32(86, 98, 121);
static const ImU32 kPanel = c32(11, 15, 22);
static const ImU32 kLine = c32(29, 38, 52);

// Text-node authoring helpers are defined alongside the other document patch
// helpers below inspectScene; the viewport needs the position patch before
// that section is compiled.
namespace {
bool patchDocumentText(EditorDocument& doc, const std::string& sceneName,
                       const std::string& nodeName, const std::string& text);
bool patchDocumentTextPosition(EditorDocument& doc, const std::string& sceneName,
                               const std::string& nodeName, const V3& pos);
}

// Camera rig types accepted by ScriptParser. Keep the editor selector aligned
// with the runtime list so authors do not have to remember rig spelling.
static constexpr const char* kCameraRigTypes[] = {
    "static", "drift", "fly", "nave", "orbit", "spiral", "hover",
    "city", "descend", "path"};
static constexpr int kCameraRigTypeCount =
    (int)(sizeof(kCameraRigTypes) / sizeof(kCameraRigTypes[0]));

struct NsdCommandInfo {
  const char* name;
  const char* signature;
  const char* description;
  const char* source;
};

// Keep this list aligned with ScriptParser's command table. It is deliberately
// data-driven: adding a parser command here gives the editor a usable template
// immediately, while the source field remains editable for advanced options.
static constexpr NsdCommandInfo kNsdCommands[] = {
    {"show", "show EFFECT_OR_SCENE", "Activate an effect or scene.", "show intro"},
    {"hide", "hide EFFECT", "Remove an active effect.", "hide intro"},
    {"load", "load TYPE ASSET", "Load a shadertoy, material, model, plugin, or effect.", "load material chrome"},
    {"shader", "shader FILE { options }", "Run a fullscreen fragment shader.", "shader shaderlab_glitch.frag"},
    {"camera", "camera NAME { rig ... }", "Select or define the active camera rig.", "camera MainCam { rig static; pos (0,0,2); target (0,0,0); fov 55 }"},
    {"play", "play music", "Emit a music/playback cue.", "play music"},
    {"fade", "fade in|out SECONDS", "Fade the presented image in or out.", "fade in 1"},
    {"transition", "transition TYPE SECONDS", "Start a fade or bloom transition.", "transition fade 1"},
    {"post", "post { option value }", "Change legacy post-processing parameters.", "post { bloom 0.8; glitch 0.1; exposure 1 }"},
    {"anim", "anim NAME TARGET.PROPERTY", "Create a keyframed animation channel.", "anim captionPos caption.pos linear { 0 (0,0,0); 1 (0,0.2,0) }"},
    {"marker", "marker NAME", "Add a named production marker.", "marker DROP"},
    {"speed", "speed VALUE", "Change director playback speed.", "speed 1"},
    {"loop", "loop", "Toggle looping of the show.", "loop"},
    {"jump", "jump SECONDS", "Seek the show transport.", "jump 0"},
    {"mesh", "mesh NAME { model ... }", "Create a reusable 3D model node.", "mesh terrain { model terrain.obj; pos (0,0,0); meshScale 1 }"},
    {"sprite", "sprite NAME { tex ... }", "Create a textured sprite node.", "sprite logo { tex ghost_logo.png; pos (0,0,0); size (2,1,1) }"},
    {"video", "video FILE { ... }", "Play a realtime ffmpeg-backed video texture.", "video intro.mp4 { size (2,1.125,1); width 1280; height 720; fps 30; loop true }"},
    {"image", "image FILE { transition ... }", "Create an image node with an entrance transition.", "image poster.png { transition fade; duration 1 }"},
    {"text", "text NAME { text ... }", "Create a bitmap-font text node.", "text caption { text \"HELLO WORLD\"; pos (0,0.2,0); size 32; style neon }"},
    {"light", "light NAME { ... }", "Create a point, directional, or spot light.", "light Key { type point; pos (0,2,1); color (1,0.8,0.6,1); intensity 2 }"},
    {"particles", "particles NAME { ... }", "Create a GPU particle node.", "particles dust { frag particles.frag; count 5000; renderScale 0.5 }"},
    {"empty", "empty NAME { ... }", "Create an empty scene graph node.", "empty Group { pos (0,0,0) }"},
    {"postnode", "postnode NAME { preset ... }", "Create a scene post-effect node.", "postnode Grade { preset cinematic }"},
    {"quadnode", "quadnode NAME { frag ... }", "Create a shader quad scene node.", "quadnode Background { frag passthrough.frag; handoff false }"},
};
static constexpr int kNsdCommandCount = (int)(sizeof(kNsdCommands) / sizeof(kNsdCommands[0]));

static bool parseEditorCommand(const std::string& source, int target, Cmd& out,
                               std::string& error) {
  try {
    Script parsed;
    if (target == 0) {
      parsed = ScriptParser::parse(
          "demo \"EDITOR COMMAND\" { duration 1 }\n"
          "scene CommandTarget {\n" + source + "\n}\n", "editor command");
      if (parsed.scenes.empty() || parsed.scenes[0].setup.size() != 1) {
        error = "enter exactly one scene-setup command";
        return false;
      }
      out = parsed.scenes[0].setup[0];
    } else {
      parsed = ScriptParser::parse(
          "demo \"EDITOR COMMAND\" { duration 1 }\n"
          "at 0 {\n" + source + "\n}\n", "editor command");
      if (parsed.main.empty() || parsed.main[0].cmds.size() != 1) {
        error = "enter exactly one timeline command";
        return false;
      }
      out = parsed.main[0].cmds[0];
    }
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

static const ImU32 kTrackPalette[8] = { kPhosphor, kHot, kAmber, kBlue, kViolet, c32(255, 170, 90), c32(120, 220, 255), c32(220, 140, 255) };

// drop-history row colors: green for applied/loading, amber for steered /
// ignored / rejected, red for failed, blue for the synthetic session-resume
// markers (the rejected set mirrors rerunDrop's fallback condition so the
// two can never disagree)
static ImU32 dropOutcomeColor(const std::string& o) {
  if (o.rfind("session resumed", 0) == 0) return kBlue;
  if (o == "failed") return kDanger;
  if (o.rfind("ignored", 0) == 0 || o.rfind("steered", 0) == 0 ||
      o.rfind("no supported", 0) == 0 || o.rfind("no drop action", 0) == 0 ||
      o.rfind("not over a panel", 0) == 0)
    return kAmber;
  return kPhosphor;
}

static ImU32 lerpU32(ImU32 a, ImU32 b, float t) {
  t = t < 0 ? 0.0f : (t > 1 ? 1.0f : t);
  const float ia = 1.0f - t;
  const float r = ((a >> IM_COL32_R_SHIFT) & 0xff) * ia + ((b >> IM_COL32_R_SHIFT) & 0xff) * t;
  const float g = ((a >> IM_COL32_G_SHIFT) & 0xff) * ia + ((b >> IM_COL32_G_SHIFT) & 0xff) * t;
  const float bl = ((a >> IM_COL32_B_SHIFT) & 0xff) * ia + ((b >> IM_COL32_B_SHIFT) & 0xff) * t;
  return c32((unsigned)r, (unsigned)g, (unsigned)bl);
}

/** spectrogram heat ramp: navy floor -> phosphor -> amber -> near-white */
static ImU32 specColor(float v) {
  v = v < 0 ? 0.0f : (v > 1 ? 1.0f : v);
  if (v < 0.5f) return lerpU32(c32(7, 11, 19), kPhosphor, v * 2.0f);
  if (v < 0.85f) return lerpU32(kPhosphor, kAmber, (v - 0.5f) / 0.35f);
  return lerpU32(kAmber, c32(255, 246, 220), (v - 0.85f) / 0.15f);
}

/** quaternion -> euler degrees, matching SceneNode::setEuler's XYZ convention
 *  (R = Rx*Ry*Rz), so the inspector's rotation drags round-trip cleanly. */
static V3 quatToEulerDeg(const Q4& qin) {
  const Q4 q = qNorm(qin);
  const float m0 = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
  const float m3 = 2 * (q[0] * q[1] - q[2] * q[3]);
  const float m6 = 2 * (q[0] * q[2] + q[1] * q[3]);
  const float m7 = 2 * (q[1] * q[2] - q[0] * q[3]);
  const float m8 = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
  constexpr float kRad2Deg = 180.0f / 3.14159265f;
  return {
      std::atan2(-m7, m8) * kRad2Deg,
      std::asin(clampf(m6, -1.0f, 1.0f)) * kRad2Deg,
      std::atan2(-m3, m0) * kRad2Deg,
  };
}// --- tiny RIFF WAV writer (the audio smoke's generated 0.25s test track) ----
static void putU32(std::vector<unsigned char>& b, uint32_t v) {
  b.push_back((unsigned char)(v & 0xff));
  b.push_back((unsigned char)((v >> 8) & 0xff));
  b.push_back((unsigned char)((v >> 16) & 0xff));
  b.push_back((unsigned char)((v >> 24) & 0xff));
}
static void putU16(std::vector<unsigned char>& b, uint16_t v) {
  b.push_back((unsigned char)(v & 0xff));
  b.push_back((unsigned char)((v >> 8) & 0xff));
}
static bool writeSmokeWav(const char* path, float seconds = 1.0f) {
  // 1.0 s with real kick transients: low-frequency pulses every 0.25 s (a
  // 120 BPM four-on-the-floor beat), so the beat-marker editor's kick
  // detector has several clean onsets to find. A quiet 440 Hz bed keeps the
  // windowed energy low between kicks.
  const uint32_t sr = 44100;
  const uint32_t n = (uint32_t)(sr * seconds);  // 1.0 s at the default
  const uint32_t dataSize = n * 2;
  std::vector<unsigned char> b;
  const unsigned char riff[] = {'R', 'I', 'F', 'F'};
  b.insert(b.end(), riff, riff + 4);
  putU32(b, 36 + dataSize);
  const unsigned char wave[] = {'W', 'A', 'V', 'E'};
  b.insert(b.end(), wave, wave + 4);
  const unsigned char fmt[] = {'f', 'm', 't', ' '};
  b.insert(b.end(), fmt, fmt + 4);
  putU32(b, 16);
  putU16(b, 1);        // PCM
  putU16(b, 1);        // mono
  putU32(b, sr);
  putU32(b, sr * 2);   // byte rate
  putU16(b, 2);        // block align
  putU16(b, 16);       // bits
  const unsigned char dat[] = {'d', 'a', 't', 'a'};
  b.insert(b.end(), dat, dat + 4);
  putU32(b, dataSize);
  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)i / sr;
    // decaying 55 Hz thump every 0.25 s, over a faint 440 Hz bed
    double kick = 0;
    const double ph = std::fmod(t, 0.25);
    if (ph < 0.04) kick = std::sin(6.2831853 * 55.0 * t) *
                                 std::exp(-ph * 90.0);
    const double bed = 0.06 * std::sin(6.2831853 * 440.0 * t);
    const int16_t s = (int16_t)((kick + bed) * 14000.0);
    b.push_back((unsigned char)(s & 0xff));
    b.push_back((unsigned char)((s >> 8) & 0xff));
  }
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
  return true;
}

// ---------------------------------------------------------------------------
// construction / teardown
// ---------------------------------------------------------------------------
DemoEditor::DemoEditor(const Wiring& w) : w_(w) {
  last_ = wallNow();
  smokeFly_ = std::getenv("NS_EDITOR_FLY_SMOKE") != nullptr;
  smokeScene_ = std::getenv("NS_EDITOR_SCENE_SMOKE") != nullptr;
  smokeAsset_ = std::getenv("NS_EDITOR_ASSET_SMOKE") != nullptr;
  smokeScrub_ = std::getenv("NS_EDITOR_SCRUB_SMOKE") != nullptr;
  smokeAudio_ = std::getenv("NS_EDITOR_AUDIO_SMOKE") != nullptr;  smokeDoc_ = std::getenv("NS_EDITOR_DOC_SMOKE") != nullptr;
  smokeExport_ = std::getenv("NS_EDITOR_EXPORT_SMOKE") != nullptr;
  smokePackage_ = std::getenv("NS_EDITOR_PACKAGE_SMOKE") != nullptr;
  showShaderLab_ = std::getenv("NS_EDITOR_SHADERLAB_SMOKE") != nullptr;
  if (smokePackage_) {
    smokePackagePath_ = std::getenv("NS_EDITOR_PACKAGE_SMOKE");
    if (smokePackagePath_.empty()) smokePackagePath_ = "package_smoke.zip";
  }
  if (smokeExport_) {
    smokeExportPath_ = std::getenv("NS_EDITOR_EXPORT_SMOKE");
    if (const char* s = std::getenv("NS_EDITOR_EXPORT_SECONDS"))
      smokeExportSeconds_ = (float)std::atof(s);
    if (smokeExportSeconds_ <= 0.0f) smokeExportSeconds_ = 3.0f;
  }
  initImGui();
  shaderLab_.init({w_.r, w_.assets, w_.audio, w_.timeline, w_.shaderDir, w_.assetDir,
                   w_.dataDir});

  // OS-level drag-in: files dropped from Explorer/file managers onto the
  // viewport are queued here and applied at the next frame's drain
  g_osDropTarget = this;
  glfwSetDropCallback(w_.window, &DemoEditor::glfwDropCallback);
  // ImGui installs its own cursor callback. Chain through it while the editor
  // is alive so fly-camera relative motion remains available in GLFW's
  // disabled-cursor mode without taking input away from the rest of ImGui.
  g_flyCursorTarget = this;
  previousCursorPos_ = glfwSetCursorPosCallback(
      w_.window, &DemoEditor::glfwFlyCursorPosCallback);

  // console sink: capture every engine/framework log line for the Console panel
  Log::setSink([this](const std::string& line) { pushConsole(line); });

  // restore the track chosen in a previous session (data/editor_state.json);
  // skipped under --no-track (explicit silence) and when no state exists
  restoreEditorState();
  // a restored history gets a session-resume marker so relaunches are visible
  // in the drop-history trail (no-op on a clean slate)
  markSessionResume();

  initDocument();  // adopt the app's parsed script as the editor document
  Log::info("EDITOR", "Demo Editor ready (docking; layouts persist in imgui.ini)");
}

DemoEditor::~DemoEditor() { shutdown(); }

void DemoEditor::shutdown() {
  // a debounced save still pending (e.g. a drop within the last 500ms) must
  // land before we tear down - crash-survival is the whole point of the trail
  if (saveDirty_) flushPendingSave();
  if (!uiUp_) return;
  shaderLab_.shutdown();
  uiUp_ = false;
  if (g_flyCursorTarget == this) {
    glfwSetCursorPosCallback(w_.window, previousCursorPos_);
    g_flyCursorTarget = nullptr;
  }
  g_osDropTarget = nullptr;
  Log::setSink(nullptr);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  if (ImGui::GetCurrentContext()) {
    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    ImGui::DestroyContext();
  }
  viewport_.destroy();
}

// ---------------------------------------------------------------------------
// ImGui bootstrap + theme
// ---------------------------------------------------------------------------
void DemoEditor::initImGui() {
  uiUp_ = true;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // prefer a real UI font so unicode glyphs render; fall back to the embedded
  // font on machines without these paths
  const char* kFontCandidates[] = {
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
  };
  for (const char* p : kFontCandidates) {
    if (io.Fonts->AddFontFromFileTTF(p, 15.0f)) {
      uiFontLoaded_ = true;
      break;
    }
  }
  if (!uiFontLoaded_) io.Fonts->AddFontDefault();

  applyTheme();

  ImGui_ImplGlfw_InitForOpenGL(w_.window, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");
}

void DemoEditor::applyTheme() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = 6.0f;
  s.FrameRounding = 4.0f;
  s.ChildRounding = 4.0f;
  s.PopupRounding = 6.0f;
  s.TabRounding = 4.0f;
  s.ScrollbarRounding = 4.0f;
  s.GrabRounding = 4.0f;
  s.WindowBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.WindowPadding = ImVec2(10, 8);
  s.FramePadding = ImVec2(8, 4);
  s.ItemSpacing = ImVec2(8, 5);
  s.DockingSeparatorSize = 2.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.059f, 0.086f, 1.00f);
  c[ImGuiCol_ChildBg] = ImVec4(0.035f, 0.047f, 0.070f, 1.00f);
  c[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.060f, 0.090f, 0.97f);
  c[ImGuiCol_Border] = ImVec4(0.114f, 0.149f, 0.204f, 1.00f);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = ImVec4(0.070f, 0.090f, 0.130f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.100f, 0.130f, 0.180f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.130f, 0.170f, 0.230f, 1.00f);
  c[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.075f, 0.110f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.080f, 0.110f, 0.160f, 1.00f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.050f, 0.065f, 0.095f, 1.00f);
  c[ImGuiCol_Header] = ImVec4(0.100f, 0.160f, 0.200f, 0.60f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.130f, 0.200f, 0.240f, 0.80f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.160f, 0.240f, 0.280f, 1.00f);
  c[ImGuiCol_Button] = ImVec4(0.080f, 0.105f, 0.150f, 1.00f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.110f, 0.150f, 0.210f, 1.00f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.150f, 0.200f, 0.270f, 1.00f);
  c[ImGuiCol_Separator] = ImVec4(0.114f, 0.149f, 0.204f, 1.00f);
  c[ImGuiCol_SeparatorHovered] = ImVec4(0.250f, 0.500f, 0.400f, 0.60f);
  c[ImGuiCol_Tab] = ImVec4(0.060f, 0.080f, 0.115f, 1.00f);
  c[ImGuiCol_TabHovered] = ImVec4(0.130f, 0.200f, 0.240f, 1.00f);
  c[ImGuiCol_TabActive] = ImVec4(0.090f, 0.140f, 0.200f, 1.00f);
  c[ImGuiCol_TabSelectedOverline] = ImVec4(0.368f, 0.941f, 0.784f, 1.00f);
  c[ImGuiCol_TabDimmed] = ImVec4(0.040f, 0.055f, 0.080f, 1.00f);
  c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.180f, 0.360f, 0.300f, 1.00f);
  c[ImGuiCol_CheckMark] = ImVec4(0.368f, 0.941f, 0.784f, 1.00f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.368f, 0.941f, 0.784f, 0.80f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.500f, 1.000f, 0.850f, 1.00f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.368f, 0.941f, 0.784f, 0.25f);
  c[ImGuiCol_Text] = ImVec4(0.847f, 0.886f, 0.937f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.420f, 0.475f, 0.560f, 1.00f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.030f, 0.040f, 0.060f, 1.00f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.160f, 0.210f, 0.280f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.220f, 0.290f, 0.380f, 1.00f);
}

// ---------------------------------------------------------------------------
// viewport capture: blit the engine's presented frame into a texture
// ---------------------------------------------------------------------------
void DemoEditor::captureViewport() {
  int fbW = 0, fbH = 0;
  glfwGetFramebufferSize(w_.window, &fbW, &fbH);
  if (fbW < 2 || fbH < 2) return;
  if (viewport_.w != fbW || viewport_.h != fbH) {
    viewport_ = FrameTarget::color(fbW, fbH, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE);
  }
  ::glBindFramebuffer(::gl::READ_FRAMEBUFFER, 0);
  ::glBindFramebuffer(::gl::DRAW_FRAMEBUFFER, viewport_.fbo);
  ::glBlitFramebuffer(0, 0, fbW, fbH, 0, 0, fbW, fbH, ::gl::COLOR_BUFFER_BIT, ::gl::NEAREST);
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// default docking layout (first run / reset)
// ---------------------------------------------------------------------------
void DemoEditor::buildDefaultLayout(unsigned dockspaceId) {
  const ImGuiID id = (ImGuiID)dockspaceId;
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

  ImGuiID toolbar, main;
  ImGui::DockBuilderSplitNode(id, ImGuiDir_Up, 0.052f, &toolbar, &main);

  ImGuiID bottom, top;
  ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.30f, &bottom, &top);

  ImGuiID left, centerRight;
  ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.21f, &left, &centerRight);
  ImGuiID right, center;
  ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.26f, &right, &center);

  ImGuiID leftTop, leftBottom;
  ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.72f, &leftTop, &leftBottom);

  ImGuiID tl, tr;
  ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.62f, &tl, &tr);

  ImGui::DockBuilderDockWindow("Toolbar", toolbar);
  ImGui::DockBuilderDockWindow("Viewport", center);
  ImGui::DockBuilderDockWindow("Shader Lab", center);
  ImGui::DockBuilderDockWindow("NSD Commands", right);
  ImGui::DockBuilderDockWindow("Hierarchy", leftTop);
  ImGui::DockBuilderDockWindow("Assets", leftBottom);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Timeline", tl);
  ImGui::DockBuilderDockWindow("Console", tr);
  ImGui::DockBuilderDockWindow("Profiler", tr);
  ImGui::DockBuilderFinish(id);
}

// ---------------------------------------------------------------------------
// main frame
// ---------------------------------------------------------------------------
bool DemoEditor::frame() {
  if (glfwWindowShouldClose(w_.window)) return false;

  const double now = wallNow();
  const float dt = clampf((float)(now - last_), 0.0f, 0.25f);
  last_ = now;

  // trailing debounce: flush the editor-state save once the 500ms window
  // after the last schedule has elapsed (a multi-file drop writes once)
  if (saveDue()) flushPendingSave();

  glfwPollEvents();

  // OS drag-in: GLFW just delivered any Explorer drops - apply them against
  // last frame's viewport rect (the drop happened before this poll)
  drainOsDrops();
  syncDocumentFromApp();  // external reload (watcher / F2 / switch) re-adopts the doc

  // resize propagation (window framebuffer -> renderer/post/camera/app)
  int fbW = 0, fbH = 0;
  glfwGetFramebufferSize(w_.window, &fbW, &fbH);
  if (w_.r && (fbW != w_.r->viewW || fbH != w_.r->viewH)) {
    w_.r->resize(fbW, fbH);
    if (w_.postfx) w_.postfx->resize();
    if (w_.camera) w_.camera->resize(fbW, fbH);
    if (w_.app) w_.app->resize(fbW, fbH);
  }

  // viewport input: raw GLFW state (fly camera enter/exit + mouse-look + WASD).
  // The engine polls GLFW directly, so nothing is "consumed" by ImGui here -
  // while the cursor is captured ImGui simply never sees the mouse.
  pollViewportInput(dt);

  // queued authoring ops (Add Scene): applied before the engine step so the
  // script reload lands in the same frame the user clicked, and the watcher
  // is re-baselined first so the polling path doesn't double-reload.
  applyQueuedActions();

  // commit a background audio decode that just finished (kept the UI
  // responsive - the device stop/start swap is a few ms, not the whole file)
  pumpAsyncAudioSwap();
  // Track restoration happens during construction, before the first frame, so
  // do not make automatic BPM detection depend on the Timeline panel being
  // visible. Runtime swaps are rebuilt in pumpAsyncAudioSwap() below.
  if (w_.audio &&
      (w_.audio->trackPath() != audioEnvPath_ ||
       w_.audio->trackFrames() != audioEnvFrames_)) {
    rebuildAudioEnvelope();
  }

  // MP4 export smoke (NS_EDITOR_EXPORT_SMOKE=path): auto-start one export
  // at boot so CI can prove the capture pipeline (a real track muxes with it)
  if (smokeExport_ && !smokeExportStarted_) {
    smokeExportStarted_ = true;
    smokeAudioPath_ = "data/editor_export_smoke.wav";
    writeSmokeWav(smokeAudioPath_.c_str(), smokeExportSeconds_ + 1.0f);
    exportAudio_ = true;
    startExport(smokeExportPath_, smokeAudioPath_);
  }
  if (smokePackage_ && !smokePackageStarted_) {
    smokePackageStarted_ = true;
    startPackage(smokePackagePath_);
    std::fprintf(stderr, "[EDITOR-PACKAGE-SMOKE] %s: %s\n",
                 packageOk_ ? "OK" : "FAIL", packageMessage_.c_str());
  }

  // --- engine step (audio -> director -> timeline -> app) --------------------
  if (w_.audio) w_.audio->update();
  if (w_.director) {
    w_.director->advance(w_.audio ? w_.audio->now() : 0);
    if (stepPending_) {        // step one frame while paused
      stepPending_ = false;
      w_.director->show += dt;
    }
    const float dur = w_.app ? w_.app->editor().duration : 0;
    if (dur > 0 && w_.director->show >= dur - 0.01f) {  // end of show: loop
      w_.director->init(0);
      if (w_.app) w_.app->seek(0);
      if (w_.audio) w_.audio->seekTrack(0);  // the music restarts with the loop
    }
  }
  if (w_.timeline) w_.timeline->advance(w_.director ? w_.director->show : 0);
  if (w_.app) w_.app->update(w_.director ? w_.director->show : 0, dt);
  if (w_.camera) w_.camera->update(dt);
  if (flyActive_) applyFlyCamera(dt);  // editor fly cam overrides the show camera
  else if (viewportFocusLock_) applyViewportFocus();
  if (w_.app) w_.app->render();
  if (export_.running()) {
    // one captured frame per show-clock crossing of the capture rate (a
    // slow frame duplicates - the video stays exactly the show duration)
    const double show = w_.director ? w_.director->show : 0.0;
    while (exportNextT_ <= show) {
      export_.pushFrame();
      exportNextT_ += 1.0 / exportFps_;
    }
  }
  captureViewport();
  shaderLab_.setVisible(showShaderLab_);
  if (showShaderLab_) shaderLab_.render(w_.director ? w_.director->show : 0.0f,
                                        dt, std::min(fbW, 960), std::min(fbH, 540));
  pumpExport(dt, fbW, fbH);

  // --- UI ---------------------------------------------------------------------
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  handleKeys();
  drawMenuBar();

  // Scene activation rebuilds the scene graph, invalidating any selected node
  // pointer from the previous scene. Drop that selection before the hierarchy
  // or inspector can dereference it. The declaration selection is separate:
  // authors may edit Scene B while the transport is previewing Scene A, so do
  // not silently replace selScene_ whenever playback crosses a section. This
  // also keeps the selected scene stable across an Apply/reload operation.
  const std::string liveScene = w_.app ? w_.app->activeScene() : std::string();
  if (liveScene != lastActiveScene_) {
    if (textNodeDragging_) {
      // A scene transition invalidates the selected runtime node. Cancel an
      // in-flight screen-space gesture rather than committing its coordinates
      // to whichever text node happens to be selected next.
      doc_.cancelEdit();
      textNodeDragging_ = false;
      textDragNodeKey_.clear();
      textDragMoved_ = false;
    }
    if (textViewportEditing_) {
      doc_.cancelEdit();
      textViewportEditing_ = false;
      textViewportChanged_ = false;
      textViewportEditNode_.clear();
      textEditNodeKey_.clear();
    }
    selNode_ = nullptr;
    selEffect_.clear();
    textEditNodeKey_.clear();
    if (selScene_.empty() || !doc_.findScene(selScene_)) {
      if (!liveScene.empty()) selScene_ = liveScene;
    }
    lastActiveScene_ = liveScene;
  }

  // fullscreen dock host (must be built BEFORE the dockable panels so the
  // first-frame DockBuilder layout wins and nothing is created floating)
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.024f, 0.031f, 0.047f, 1.0f));
  ImGui::Begin("##MainHost", nullptr,
               ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
  const ImGuiID dockId = ImGui::GetID("MainDockSpace");
  ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
  if (!layoutBuilt_) {
    layoutBuilt_ = true;
    // the default layout is only applied when there is NO saved ini: ImGui
    // restores the saved docking arrangement from imgui.ini at the first
    // NewFrame, and rebuilding over it would wipe the user's layout on every
    // launch ("Save and restore layouts" is automatic through the ini)
    if (!std::filesystem::exists(ImGui::GetIO().IniFilename)) {
      buildDefaultLayout((unsigned)dockId);
    }
  }
  ImGui::End();

  drawToolbar();
  drawAudioPopup();  // always: Ctrl+T opens it even with the toolbar hidden

  if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
  if (showMetrics_) ImGui::ShowMetricsWindow(&showMetrics_);
  if (showAbout_) {
    ImGui::OpenPopup("About Demo Editor");
    showAbout_ = false;
  }
  if (ImGui::BeginPopupModal("About Demo Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("NULL SECTOR // DEMO ENGINE");
    ImGui::TextDisabled("Demo Editor - dockable data-driven demo authoring");
    ImGui::TextDisabled("ImGui " IMGUI_VERSION " (docking) + GLFW + OpenGL 3.3");
    ImGui::TextDisabled("The show itself is 100%% data-driven: edit data/demo.nsd, ");
    ImGui::TextDisabled("shaders/ and assets/ while the preview runs.");
    ImGui::Separator();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  drawViewportPanel();
  if (showHierarchy_) drawHierarchy();
  if (showInspector_) drawInspector();
  if (showTimeline_) drawTimeline();
  if (showConsole_) drawConsole();
  if (showAssets_) drawAssets();
  if (showProfiler_) drawProfiler();
  drawNsdCommands();
  if (showShaderLab_) {
    shaderLab_.setVisible(true);
    shaderLab_.draw(w_.director ? w_.director->show : 0.0f);
    showShaderLab_ = shaderLab_.visible();
  }
  if (shaderLab_.takeInsertRequest()) insertShaderLabIntoTimeline();
  drawNewAssetDialog();
  drawNewProjectConfirm();
  if (showCurves_) drawCurveEditor();
  drawMarkerEditDialog();
  drawExportDialog();
  drawPackageDialog();
  // ESC closes popups at the ImGui level (NavUpdate in NewFrame); clear our
  // flags BEFORE drawBrowse/drawScratch so their per-frame OpenPopup calls
  // don't resurrect them, and the close-edge save below fires. Skip while a
  // text field is active: the first ESC should cancel typing, not slam a
  // panel shut.
  if ((browseOpen_ || scratchOpen_) && ImGui::IsKeyPressed(ImGuiKey_Escape) &&
      !ImGui::GetIO().WantTextInput) {
    browseOpen_ = false;
    scratchOpen_ = false;
  }
  drawBrowse();
  drawScratch();

  // Timeline input is handled by the ImGui panels after the normal engine
  // update/render pass above. A seek queues the timeline catch-up commands,
  // but without this second, zero-delta pass those commands would not be
  // dispatched until the next editor frame and the captured viewport would
  // visibly lag behind the playhead. Refresh once after all panels have
  // handled input; repeated seek calls during a drag are coalesced.
  refreshSeekPreview();

  // persist every browser's scan root + last pick when the popup closes (any
  // close path: Open, double-click, Cancel or ESC), so reopening a picker
  // lands where the user was
  if (browseWasOpen_ && !browseOpen_) saveEditorState();
  browseWasOpen_ = browseOpen_;

  ImGui::Render();

  // present: clear then draw the UI over the (already captured) frame
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, std::max(fbW, 1), std::max(fbH, 1));
  ::glClearColor(0.024f, 0.031f, 0.047f, 1.0f);
  ::glClear(::gl::COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(w_.window);

  // window title carries the document name + the unsaved-change marker
  const std::string title = "NULL SECTOR // DEMO EDITOR - " + docDisplayName();
  if (title != lastTitle_) {
    lastTitle_ = title;
    glfwSetWindowTitle(w_.window, title.c_str());
  }

  // stats
  frameMs_ = dt * 1000.0f;
  fps_ = dt > 0 ? 1.0f / dt : 0;
  hist_.push_back(frameMs_);
  if (hist_.size() > 240) hist_.erase(hist_.begin());
  if (w_.r) w_.r->tick(frameMs_, frameMs_);  // adaptive quality governor

  // NS_EDITOR_FLY_SMOKE=1 --editor-seconds=N: engage the fly camera
  // programmatically (enter -> move -> exit) so CI can prove the cursor
  // capture + camera-override paths without a human holding the mouse
  if (smokeFly_) {
    flySmokeT_ += dt;
    if (flySmokeT_ >= 1.5f && !smokeFlyEntered_) {
      smokeFlyEntered_ = true;
      enterFly();
    }
    if (smokeFlyEntered_ && flySmokeT_ < 4.5f) {
      // simulate WASD + look: exercise applyFlyCamera's override for real
      const float cy = std::cos(flyPitch_);
      const V3 fwd{(float)(std::sin(flyYaw_) * cy), (float)std::sin(flyPitch_),
                   (float)(std::cos(flyYaw_) * cy)};
      flyPos_ = vAdd(flyPos_, vScale(fwd, dt * 3.0f));
      flyYaw_ += dt * 0.4f;
    } else if (smokeFlyEntered_ && flySmokeT_ >= 4.5f && !smokeFlyExited_) {
      smokeFlyExited_ = true;
      exitFly();
      char sb[128];
      std::snprintf(sb, sizeof sb,
                    "fly smoke: enter -> camera override -> exit OK (fly pos %.1f %.1f %.1f)",
                    flyPos_[0], flyPos_[1], flyPos_[2]);
      Log::info("EDITOR", sb);
    }
  }

  runDocSmoke(dt);  // NS_EDITOR_DOC_SMOKE: document pipeline
  // NS_EDITOR_SCENE_SMOKE=1: run the Add Scene flow programmatically
  // (queue -> append to the script -> reload -> land on the new section) and
  // log the verdict, so CI can prove the button's data path. Point --demo at
  // a copy of the show script so the real demo.nsd is never touched.
  if (smokeScene_) {
    sceneSmokeT_ += dt;
    if (sceneSmokeT_ >= 1.5f && !sceneSmokeQueued_) {
      sceneSmokeQueued_ = true;
      sceneSmokeBefore_ = w_.app ? (int)w_.app->sections().size() : 0;
      queueAddScene();  // applied at the next frame's start (safe point)
    }
    if (sceneSmokeQueued_ && !sceneSmokeDone_ && w_.app &&
        (int)w_.app->sections().size() > sceneSmokeBefore_) {
      sceneSmokeDone_ = true;
      const std::string act = w_.app->activeScene();
      // the verdict asserts the NEW scene (the last schedule entry - the add
      // appends at the show end) actually became the active one
      const bool activated = !w_.app->sections().empty() &&
                             act == w_.app->sections().back().name;
      char sb[160];
      std::snprintf(sb, sizeof sb,
                    "scene smoke: Add Scene grew the schedule %d -> %d sections, landed"
                    " on '%s' at t=%.1f - %s",
                    sceneSmokeBefore_, (int)w_.app->sections().size(), act.c_str(),
                    w_.director ? w_.director->show : -1.0f,
                    activated ? "PASS" : "FAIL (new scene did not activate)");
      Log::info("EDITOR", sb);
    }
  }

  // NS_EDITOR_SCRUB_SMOKE=1: prove scrubbing establishes the scene at the
  // target. seekToRaw() re-arms the timeline's fire boundary at the new time,
  // so without the app's catch-up every crossed `show Scene` event is
  // silently skipped and the viewport keeps rendering the PRE-scrub scene
  // (or goes dark when a crossed event hides it) - "scrubbing broke the
  // graphics". The smoke: forward scrub into Cathedral (13.3-26.7) must
  // activate it, backward scrub into Intro (0-13.3) must re-establish it.
  if (smokeScrub_) {
    scrubSmokeT_ += dt;
    if (scrubSmokeT_ >= 2.5f && !scrubSmokeQueued_) {
      scrubSmokeQueued_ = true;
      scrubSmokeWait_ = 0;
      seekToRaw(20.0f);  // into Cathedral (13.3-26.7)
    }
    if (scrubSmokeQueued_ && !scrubSmokeDone_ && w_.app) {
      // the app dispatches the catch-up in the NEXT frame's update() step,
      // so wait one full frame before asserting (and after the second seek)
      scrubSmokeWait_++;
      if (scrubSmokeWait_ >= 2) {
        scrubSmokeWait_ = 0;
        if (!scrubSmokePhase2_) {
          bool hasCathedral = false;
          for (const auto& e : w_.app->activeEffects())
            if (e == "cathedral") hasCathedral = true;
          scrubFwdOk_ = w_.app->activeScene() == "Cathedral" && hasCathedral;
          scrubSmokePhase2_ = true;
          seekToRaw(5.0f);  // backward into Intro (0-13.3)
        } else {
          scrubSmokeDone_ = true;
          bool hasIntro = false;
          for (const auto& e : w_.app->activeEffects())
            if (e == "intro") hasIntro = true;
          const bool backOk = w_.app->activeScene() == "Intro" && hasIntro;
          char sb[192];
          std::snprintf(sb, sizeof sb,
                        "scrub smoke: fwd->Cathedral %s, back->Intro %s - %s",
                        scrubFwdOk_ ? "PASS" : "FAIL",
                        backOk ? "PASS" : "FAIL",
                        (scrubFwdOk_ && backOk) ? "PASS" : "FAIL");
          Log::info("EDITOR", sb);
        }
      }
    }
  }

  // NS_EDITOR_ASSET_SMOKE=1: create a material + a post preset through the
  // New Asset dialog's code path, load both into the show (the engine's own
  // parsers must accept the templates), then delete the smoke files.
  if (smokeAsset_) {
    assetSmokeT_ += dt;
    if (assetSmokeT_ >= 1.0f && !assetSmokeDone_) {
      assetSmokeDone_ = true;
      std::strncpy(assetName_, "smoketest", sizeof(assetName_) - 1);
      assetKind_ = 0;  // material
      const bool mat = createAssetFromForm();
      assetKind_ = 1;  // post preset
      const bool post = createAssetFromForm();

      if (w_.app && mat) w_.app->editorLoadMaterial("smoketest");
      if (w_.app && post) w_.app->editorLoadPreset("smoketest");

      // Scene kind: a fresh name must pass the exists gate and create the
      // file; re-creating the SAME name must be rejected (that is exactly
      // the dialog's 'already exists' claim, asserted headlessly); and the
      // free-name suggestion must return a name that is actually free.
      assetKind_ = 2;  // scene
      std::strncpy(assetName_, "smoketest", sizeof(assetName_) - 1);
      std::error_code sec;
      std::filesystem::remove(w_.dataDir + "/scenes/smoketest.json", sec);  // stale crash
      const bool scene = createAssetFromForm();      // fresh name -> created
      const bool sceneDup = createAssetFromForm();   // same name -> rejected
      const std::string sugg = suggestFreeAssetName();  // expect "smoketest2"
      bool suggFree = false;
      {
        std::error_code xec;
        suggFree = !sugg.empty() && sugg != "smoketest" &&
                   !std::filesystem::exists(assetTargetPathFor(sugg), xec);
      }
      std::filesystem::remove(w_.dataDir + "/scenes/smoketest.json", sec);

      auto hasLog = [this](const char* needle) {
        for (const auto& l : console_) {
          if (l.text.find(needle) != std::string::npos) return true;
        }
        return false;
      };
      const bool matLoaded = hasLog("material 'smoketest' loaded");
      const bool postLoaded = hasLog("preset 'smoketest' active");

      // cleanup: the smoke files are artifacts of the test run
      std::error_code ec;
      std::filesystem::remove(w_.dataDir + "/materials/smoketest.json", ec);
      std::filesystem::remove(w_.dataDir + "/post/smoketest.json", ec);

      if (mat && post && scene && !sceneDup && suggFree && matLoaded && postLoaded) {
        Log::info("EDITOR",
                  "asset smoke: material + post preset + scene created, dup rejected,"
                  " free-name suggestion OK - PASS");
      } else {
        char sb[192];
        std::snprintf(sb, sizeof sb,
                      "asset smoke: FAIL (mat=%d post=%d scene=%d sceneDup=%d"
                      " suggFree=%d matLoaded=%d postLoaded=%d)",
                      (int)mat, (int)post, (int)scene, (int)sceneDup, (int)suggFree,
                      (int)matLoaded, (int)postLoaded);
        Log::info("EDITOR", sb);
      }
    }
  }

  // NS_EDITOR_AUDIO_SMOKE=1: prove the runtime track swap without needing a
  // real music file on disk - generate a 0.25s WAV, swap it in (silence ->
  // track), verify trackMode + duration, drop back to silence, and check a
  // bogus path fails without losing the current source.
  if (smokeAudio_) {
    audioSmokeT_ += dt;
    if (audioSmokeT_ >= 1.5f && !audioSmokeDone_) {
      audioSmokeDone_ = true;
      const std::string wav = "tmp_audio_smoke.wav";
      bool ok = false, loaded = false, asyncOk = false, toSilent = false, bogus = false;
      bool envOk = false, saved = false, restored = false, kicksOk = false, offOk = false;
      bool quantOk = false, specOk = false, browseOk = false, liveOk = false, multiOk = false;
      bool durOk = false, timelineDurationOk = false;
      bool relOk = false, pickOk = true, panelOk = false;
      float dur = 0;
      std::error_code ec;
      std::filesystem::remove(editorStatePath(), ec);  // stale state from a crashed run
      dropHistory_.clear();  // and any history/marker restored by the ctor from it
      if (writeSmokeWav(wav.c_str()) && w_.audio) {
        // the async path (what the popup uses): begin -> worker decodes on a
        // background thread -> apply once Ready. Wait with a sleep so the
        // worker actually gets scheduled (a tight spin starves it); cap the
        // wait so a hang becomes a smoke failure, not a frozen CI.
        w_.audio->beginAsyncSwap(wav, 0);
        bool sawBusy = false, becameReady = false;
        const auto t0 = wallNow();
        while (!becameReady && wallNow() - t0 < 2.0) {
          const auto st = w_.audio->asyncStatus();
          if (st != ns::AudioEngine::AsyncState::Idle) sawBusy = true;
          if (st != ns::AudioEngine::AsyncState::Decoding) becameReady = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        asyncOk = sawBusy && becameReady && w_.audio->applyAsyncSwap() &&
                  w_.audio->trackMode;
        loaded = w_.audio->swapTrack(wav, 0) && w_.audio->trackMode;
        dur = w_.audio->trackDuration;
        timelineDurationOk =
            std::fabs(timelineContentDuration() -
                      std::max(w_.app ? w_.app->editor().duration : 0.0f, dur)) < 0.05f;
        rebuildAudioEnvelope();  // the waveform strip's data path
        envOk = !audioEnv_.empty() &&
                *std::max_element(audioEnv_.begin(), audioEnv_.end()) > 0.1f;
        // beat-marker editor: the generated track has 4 kicks per second at
        // ~0.25s spacing, so detection must find them and the grid phase must
        // persist through a save -> drop -> restore round-trip
        kicksOk = kickTimes_.size() >= 3;
        if (kicksOk && kickTimes_.size() > 1) {
          const float mean = (kickTimes_.back() - kickTimes_.front()) /
                             (float)(kickTimes_.size() - 1);
          kicksOk = std::fabs(mean - 0.25f) < 0.06f;
        }
        // auto-align: the phase fit must land the grid on the kicks (the
        // kicks are at ~0.25s spacing, so a good fit puts a grid line within
        // a few ms of the first detected kick)
        const float smkBeat =
            w_.timeline ? w_.timeline->beatSec() : 0.2777f;
        beatOffset_ = 0.5f;  // deliberately misaligned before the fit
        autoAlignBeats();
        float bestFit = 1e9f;
        if (!kickTimes_.empty()) {
          for (const float k : kickTimes_) {
            const float d = std::fmod(k - beatOffset_, smkBeat);
            bestFit = std::min(bestFit, std::min(d, smkBeat - d));
          }
        }
        offOk = bestFit < 0.02f;  // a grid line sits within 20ms of a kick
        // the live FFT spectrogram ring must have accumulated columns while
        // the show played (silence or track - the capture is in the callback)
        specOk = w_.audio && w_.audio->spectrumCount() > 0;
        // the in-editor track browser must find the generated WAV: it lives
        // in the exe dir, which is one of the default scan roots
        browse_[(int)BrowseKind::Audio].root[0] = '\0';  // default roots
        scanAssetBrowser((int)BrowseKind::Audio);
        {
          const std::string absWav = std::filesystem::absolute(wav).string();
          for (const auto& f : browse_[(int)BrowseKind::Audio].files) {
            if (f == absWav) { browseOk = true; break; }
          }
        }
        // scrub quantization: seekTo must snap to the nearest ALIGNED grid
        // line (beatOffset_ + n*grid) - beat grid, then bar grid; and the
        // far-left edge must stay reachable (t=0 never snaps away)
        quantOk = true;
        if (w_.timeline) {
          const float qBeat = w_.timeline->beatSec();
          const float qOff = 0.1234f;
          auto snap = [&](float t, float grid) {
            return std::max(0.0f, std::floor((t - qOff) / grid + 0.5f) * grid + qOff);
          };
          beatOffset_ = qOff;
          quantize_ = true;
          quantizeGrid_ = 0;  // beat grid
          seekTo(0.3f);
          quantOk = std::fabs(w_.director->show - snap(0.3f, qBeat)) < 0.001f;
          seekTo(0.9f);
          quantOk = quantOk &&
                    std::fabs(w_.director->show - snap(0.9f, qBeat)) < 0.001f;
          seekTo(0.0f);  // the far-left edge must snap to 0, not the offset
          quantOk = quantOk && w_.director->show == 0.0f;
          quantizeGrid_ = 1;  // bar grid
          seekTo(0.9f);
          quantOk = quantOk &&
                    std::fabs(w_.director->show - snap(0.9f, qBeat * 4.0f)) < 0.001f;
          // leave quantize enabled (on + bar) so the save -> drop -> restore
          // round-trip below proves a TRUE quantize state survives
        }
        // track-browser state must persist too: the scan root + last picked
        // file go into the same save below and must survive the round-trip
        // (the root is stored absolute, like the track)
        const std::string browseRootSave = std::filesystem::absolute("assets").string();
        AssetBrowse& audioB = browse_[(int)BrowseKind::Audio];
        std::strncpy(audioB.root, browseRootSave.c_str(), sizeof(audioB.root) - 1);
        audioB.root[sizeof(audioB.root) - 1] = '\0';
        audioB.sel = std::filesystem::absolute(wav).string();
        beatOffset_ = 0.1234f;
        saveEditorState();
        saved = std::filesystem::exists(editorStatePath());
        // persistence round-trip: save -> stop audio -> restore from state.
        // saveEditorState canonicalizes to an absolute path, so compare that.
        toSilent = w_.audio->swapTrack("", 0) && !w_.audio->trackMode;
        // stopping audio clears the editor grid state (pumpAsyncAudioSwap
        // resets beatOffset_/quantize_); zero them so the restore is provable
        beatOffset_ = 0.0f;
        quantize_ = false;
        quantizeGrid_ = 0;
        audioB.root[0] = '\0';  // zeroed so the restore is provable
        audioB.sel.clear();
        restoreEditorState();
        restored = w_.audio->trackMode &&
                   w_.audio->trackPath() == std::filesystem::absolute(wav).string();
        const bool offSurvived = std::fabs(beatOffset_ - 0.1234f) < 0.001f;
        offOk = offOk && offSurvived;  // fit worked AND phase persisted
        // the quantize ON state (enabled + bar grid) must have survived too
        quantOk = quantOk && offSurvived && quantize_ && quantizeGrid_ == 1;
        // the browser root + last pick must have come back from the file
        const bool browseSurvived =
            std::strcmp(audioB.root, browseRootSave.c_str()) == 0 &&
            audioB.sel == std::filesystem::absolute(wav).string();
        browseOk = browseOk && browseSurvived;  // found on disk AND persisted
        // live browser refresh: a file created in the scanned root must appear
        // in the list (and arm the "updated" flash) with NO manual Rescan, and
        // vanish again when deleted - the auto-refresh path, driven directly
        {
          std::filesystem::create_directories("tmp_browse_smoke", ec);
          std::strncpy(audioB.root, "tmp_browse_smoke", sizeof(audioB.root) - 1);
          audioB.root[sizeof(audioB.root) - 1] = '\0';
          audioB.scanned = true;  // listed once; auto-refresh from here
          scanAssetBrowser((int)BrowseKind::Audio);
          audioB.scanT = 0;  // force the cadence interval to have elapsed
          if (writeSmokeWav("tmp_browse_smoke/live.wav")) {
            const std::string absLive =
                std::filesystem::absolute("tmp_browse_smoke/live.wav").string();
            browseAutoRefresh((int)BrowseKind::Audio);
            const bool appeared =
                audioB.flashT > 0 &&
                std::find(audioB.files.begin(), audioB.files.end(), absLive) !=
                    audioB.files.end();
            std::filesystem::remove("tmp_browse_smoke/live.wav", ec);
            audioB.scanT = 0;  // the second refresh must fire immediately
            browseAutoRefresh((int)BrowseKind::Audio);
            const bool vanished =
                std::find(audioB.files.begin(), audioB.files.end(), absLive) ==
                    audioB.files.end();
            liveOk = appeared && vanished;
          }
          std::filesystem::remove_all("tmp_browse_smoke", ec);
          audioB.root[0] = '\0';  // back to the defaults for next session
          audioB.scanned = false;
          audioB.sel.clear();
        }
        // the shared browser serves other categories too: the script + shader
        // kinds must list real files under their default roots
        {
          browse_[(int)BrowseKind::Script].root[0] = '\0';  // default = data/
          scanAssetBrowser((int)BrowseKind::Script);
          bool foundNsd = false;
          for (const auto& f : browse_[(int)BrowseKind::Script].files) {
            if (f.find(".nsd") != std::string::npos) { foundNsd = true; break; }
          }
          browse_[(int)BrowseKind::Shader].root[0] = '\0';  // default = shaders/
          scanAssetBrowser((int)BrowseKind::Shader);
          multiOk = foundNsd && !browse_[(int)BrowseKind::Shader].files.empty();
        }
        // the pick actions resolve relative names even for native (backslash)
        // absolute scan paths - the normalization must strip the base cleanly,
        // reject look-alike prefixes (textures2) and handle exact-base matches
        relOk = browseRelPath(w_.dataDir + "/textures",
                              w_.dataDir + "/textures\\sub dir\\x.png") ==
                    "sub dir/x.png" &&
                browseRelPath(w_.dataDir + "/textures",
                              w_.dataDir + "/textures2\\x.png").empty() &&
                browseRelPath(w_.dataDir + "/textures",
                              w_.dataDir + "/textures").empty();
        // the drop path reuses pickBrowseFile's dispatch (a real ImGui drag
        // can't be driven headlessly, but a drop target just forwards kind +
        // path into these calls): a shader pick must show the quad effect and
        // a script pick must switch the running demo to the picked file
        if (w_.app) {
          // OS-level drag-in: a real Explorer drop can't be synthesized
          // headlessly, but the drain path is exactly what glfwDropCallback
          // queues - a .frag dropped on the viewport must show the quad
          // effect, while an unknown extension and a drop far outside the
          // viewport rect must both be ignored
          vpRectValid_ = true;
          vpRectMinX_ = vpRectMinY_ = 0; vpRectMaxX_ = vpRectMaxY_ = 2000;
          for (auto& pr : panelRects_) pr.valid = false;  // only the picture gate
          osDrops_.push_back({w_.shaderDir + "/landscape.frag", 100, 100});
          osDrops_.push_back({"drop_unknown.bin", 100, 100});
          osDrops_.push_back({w_.shaderDir + "/plasma.frag", 9999, 9999});
          drainOsDrops();
          // the toast ring reports every outcome: the last queued drop was the
          // one outside every panel, so the newest toast must show the reject
          const bool toastRejected =
              !toasts_.empty() &&
              toasts_.back().text.find("no drop action here") != std::string::npos;
          // a fresh single inside drop must push an "applied" toast
          osDrops_.push_back({w_.shaderDir + "/landscape.frag", 100, 100});
          drainOsDrops();
          const bool toastApplied =
              !toasts_.empty() &&
              toasts_.back().text.find("applied") != std::string::npos &&
              toasts_.back().text.find("landscape.frag") != std::string::npos;
          bool osShown = false, osWrong = false;
          for (const auto& e : w_.app->activeEffects()) {
            if (e == "quad:landscape.frag") osShown = true;
            if (e != "quad:landscape.frag") osWrong = true;  // nothing else may show
          }
          const bool osToastOk = toastRejected && toastApplied;
          // dropping a FOLDER opens the browser rooted at it, with the kind
          // inferred from what's inside: an empty pack must not open it, a
          // wav-only pack opens on Audio and lists the wavs, a mixed pack
          // flips to the dominant kind (more frags than wavs -> Shader)
          bool dirOk = false;
          {
            const std::string packA = "tmp_drop_pack_a";  // wav-only
            const std::string packB = "tmp_drop_pack_b";  // mixed
            const std::string packC = "tmp_drop_pack_c";  // empty
            std::filesystem::create_directories(packA, ec);
            std::filesystem::create_directories(packB, ec);
            std::filesystem::create_directories(packC, ec);
            writeSmokeWav((packA + "/a.wav").c_str());
            writeSmokeWav((packA + "/b.wav").c_str());
            writeSmokeWav((packB + "/one.wav").c_str());
            writeSmokeWav((packB + "/x.frag").c_str());
            writeSmokeWav((packB + "/y.frag").c_str());
            // empty folder first: must NOT open the browser
            browseOpen_ = false;
            osDrops_.push_back({packC, 100, 100});
            drainOsDrops();
            const bool dirEmptyOk =
                !browseOpen_ && !toasts_.empty() &&
                toasts_.back().text.find("no supported assets") != std::string::npos;
            // wav-only pack: browser opens on Audio, rooted at the pack, and
            // a scan lists the dropped wavs
            osDrops_.push_back({packA, 100, 100});
            drainOsDrops();
            AssetBrowse& ab = browse_[(int)BrowseKind::Audio];
            const bool dirAudioOk =
                browseOpen_ && browseKind_ == (int)BrowseKind::Audio &&
                std::strcmp(ab.root, std::filesystem::absolute(packA).string().c_str()) ==
                    0;
            scanAssetBrowser((int)BrowseKind::Audio);
            const bool dirListed =
                std::find(ab.files.begin(), ab.files.end(),
                          std::filesystem::absolute(packA + "/a.wav").string()) !=
                    ab.files.end();
            // mixed pack: the kind flips to Shader and the root follows
            osDrops_.push_back({packB, 100, 100});
            drainOsDrops();
            const bool dirShaderOk =
                browseKind_ == (int)BrowseKind::Shader &&
                std::strcmp(browse_[(int)BrowseKind::Shader].root,
                            std::filesystem::absolute(packB).string().c_str()) == 0;
            dirOk = dirEmptyOk && dirAudioOk && dirListed && dirShaderOk;
            std::filesystem::remove_all(packA, ec);
            std::filesystem::remove_all(packB, ec);
            std::filesystem::remove_all(packC, ec);
            ab.root[0] = '\0';    // back to defaults - the popup-render block
            ab.scanned = false;   // below re-opens Audio and scans the defaults
          }
          // panel-aware routing: every panel has its own drop action - a
          // script on the Timeline switches the show, a shader on the Console
          // opens the scratch view, and a file on the Assets panel opens the
          // browser rooted at the file's folder. Rects are set explicitly so
          // the routing is deterministic regardless of the live layout.
          {
            // Panels with split layouts record SUB-AREA rects, so a drop lands
            // on the exact zone: Timeline lanes (sequence: scripts) vs the
            // waveform strip (audio: tracks), and the Console header (tools:
            // scratch view) vs its log list (debug: filter to the shader).
            // Rects are set explicitly so the routing is deterministic
            // regardless of the live layout; "lanes"/"header" are the panel's
            // default zone, so a point outside a recorded sub-rect routes there.
            auto clearRects = [&] {
              for (auto& pr : panelRects_) pr.valid = false;
              for (auto& sr : subRects_) sr.valid = false;
            };
            const size_t histBefore = dropHistory_.size();  // drops below record
            const std::string tlCopy = "tmp_script_tl.nsd";
            {
              std::ifstream in(w_.app->scriptPath(), std::ios::binary);
              std::ofstream out(tlCopy, std::ios::binary);
              if (in && out) out << in.rdbuf();
            }
            // Timeline LANES: a script drop switches the show
            clearRects();
            panelRects_[(int)DropPanel::Timeline] = {0, 0, 2000, 2000, true};
            subRects_[(int)DropSub::TimelineStrip] = {0, 0, 20, 20, true};
            osDrops_.push_back({tlCopy, 100, 100});  // (100,100) -> lanes
            drainOsDrops();
            const bool tlScriptOk =
                w_.app->scriptPath() == tlCopy && !toasts_.empty() &&
                toasts_.back().text.find("timeline") != std::string::npos;
            // Timeline LANES + wav: steered to the strip, track NOT touched
            const std::string trackBefore = w_.audio ? w_.audio->trackPath() : "";
            osDrops_.push_back({wav, 100, 100});
            drainOsDrops();
            const bool tlSteerOk =
                w_.audio && w_.audio->trackPath() == trackBefore &&
                !toasts_.empty() &&
                toasts_.back().text.find("waveform strip") != std::string::npos;
            // Timeline STRIP + script: scripts are show-level, so they switch
            // even on the audio zone (proven with a DIFFERENT copy - the lanes
            // copy is already active, so a stale pass would be caught)
            const std::string tlCopy2 = "tmp_script_tl2.nsd";
            {
              std::ifstream in(tlCopy, std::ios::binary);
              std::ofstream out(tlCopy2, std::ios::binary);
              if (in && out) out << in.rdbuf();
            }
            clearRects();
            panelRects_[(int)DropPanel::Timeline] = {0, 0, 2000, 2000, true};
            subRects_[(int)DropSub::TimelineStrip] = {0, 0, 2000, 2000, true};
            osDrops_.push_back({tlCopy2, 100, 100});
            drainOsDrops();
            const bool tlStripScriptOk =
                w_.app->scriptPath() == tlCopy2 && !toasts_.empty() &&
                toasts_.back().text.find("timeline") != std::string::npos;
            std::filesystem::remove(tlCopy2, ec);
            // Timeline STRIP + wav: the audio zone loads the track (async -
            // the toast proves the dispatch; the decode commits later)
            clearRects();
            panelRects_[(int)DropPanel::Timeline] = {0, 0, 2000, 2000, true};
            subRects_[(int)DropSub::TimelineStrip] = {0, 0, 2000, 2000, true};
            osDrops_.push_back({wav, 100, 100});
            drainOsDrops();
            const bool tlStripOk =
                !toasts_.empty() &&
                toasts_.back().text.find("loading") != std::string::npos;
            // Console HEADER: a shader opens the scratch view with source
            clearRects();
            panelRects_[(int)DropPanel::Console] = {0, 0, 2000, 2000, true};
            subRects_[(int)DropSub::ConsoleLog] = {0, 0, 20, 20, true};
            osDrops_.push_back({w_.shaderDir + "/plasma.frag", 100, 100});
            drainOsDrops();
            const bool conHdrOk =
                scratchOpen_ && scratchPath_ == w_.shaderDir + "/plasma.frag" &&
                !scratchSrc_.empty() && !toasts_.empty() &&
                toasts_.back().text.find("scratch") != std::string::npos;
            scratchOpen_ = false;  // close so later tests are unaffected
            // Console LOG: the same shader filters the log to its basename
            // instead of opening the scratch view
            clearRects();
            panelRects_[(int)DropPanel::Console] = {0, 0, 2000, 2000, true};
            subRects_[(int)DropSub::ConsoleLog] = {0, 0, 2000, 2000, true};
            filter_[0] = 0;
            osDrops_.push_back({w_.shaderDir + "/plasma.frag", 100, 100});
            drainOsDrops();
            const bool conLogOk =
                std::string(filter_) == "plasma.frag" && !scratchOpen_ &&
                !toasts_.empty() &&
                toasts_.back().text.find("filtered") != std::string::npos;
            filter_[0] = 0;  // reset so later log rendering is unaffected
            // Scratch EDITOR: the source is editable and Save writes it back,
            // poking an immediate recompile. Use a THROWAWAY copy inside the
            // shader dir (so the quad: effect resolves) - plasma.frag itself
            // is never touched.
            const std::string scratchCopy = w_.shaderDir + "/tmp_scratch_edit.frag";
            {
              std::ifstream in(w_.shaderDir + "/plasma.frag", std::ios::binary);
              std::ofstream out(scratchCopy, std::ios::binary);
              if (in && out) out << in.rdbuf();
            }
            openShaderScratch(scratchCopy);
            const std::string origSrc = scratchSrc_;
            std::strcpy(scratchBuf_.data(),
                        (origSrc + "\n// scratch editor smoke\n").c_str());
            scratchDirty_ = true;
            saveScratch();
            std::string onDisk;
            {
              // scoped: the read stream must CLOSE before the temp copy is
              // deleted (MSVCRT read handles block Windows deletion)
              std::ifstream chk(scratchCopy, std::ios::binary);
              onDisk.assign((std::istreambuf_iterator<char>(chk)),
                            std::istreambuf_iterator<char>());
            }
            auto consoleHas = [&](const char* frag) {
              for (const auto& ln : console_)
                if (ln.text.find(frag) != std::string::npos) return true;
              return false;
            };
            const bool saveOk =
                !scratchDirty_ && scratchSrc_ == std::string(scratchBuf_.data()) &&
                origSrc.find("// scratch editor smoke") == std::string::npos &&
                onDisk.find("// scratch editor smoke") != std::string::npos &&
                consoleHas("scratch: saved");
            // back to the real shader for the popup-render phase below (the
            // temp copy is deleted; the scratch stays open as before)
            openShaderScratch(w_.shaderDir + "/plasma.frag");
            std::filesystem::remove(scratchCopy, ec);
            // Assets (no sub-areas): drop the same shader - the browser opens
            // rooted at the file's own folder
            clearRects();
            panelRects_[(int)DropPanel::Assets] = {0, 0, 2000, 2000, true};
            osDrops_.push_back({w_.shaderDir + "/plasma.frag", 100, 100});
            drainOsDrops();
            AssetBrowse& shb = browse_[(int)BrowseKind::Shader];
            const std::string expectRoot =
                std::filesystem::absolute(w_.shaderDir + "/plasma.frag")
                    .parent_path()
                    .string();
            const bool asOk =
                browseOpen_ && browseKind_ == (int)BrowseKind::Shader &&
                std::strcmp(shb.root, expectRoot.c_str()) == 0 && !toasts_.empty() &&
                toasts_.back().text.find("assets") != std::string::npos;
            // Drop history: every OS drop records its routed panel + outcome,
            // and re-running a record re-dispatches it (a rejected record
            // falls back to the file kind's canonical action).
            bool histOk = false;
            {
              bool histRecOk = dropHistory_.size() > histBefore;
              for (const auto& r : dropHistory_) {
                if (r.file.empty() || r.outcome.empty() || r.panel < 0) histRecOk = false;
              }
              // the lanes audio drop must be recorded as steered on lanes;
              // records are COPIED for re-running (routes could record in the
              // future, and a pointer into the vector would go stale)
              bool steerFound = false;
              DropRecord toRerun, steerRec;  // applied landscape / steered audio
              bool haveRerun = false, haveSteer = false;
              for (const auto& r : dropHistory_) {
                if (r.file == "tmp_audio_smoke.wav" &&
                    r.panel == (int)DropPanel::Timeline &&
                    r.sub == (int)DropSub::TimelineLanes && r.outcome == "steered") {
                  steerFound = true;
                  steerRec = r;
                  haveSteer = true;
                }
                if (!haveRerun && r.file == "landscape.frag" && r.outcome == "applied") {
                  toRerun = r;
                  haveRerun = true;
                }
              }
              // re-run through the original route (positive record)
              if (haveRerun) rerunDrop(toRerun);
              const bool rerunOk =
                  haveRerun && !toasts_.empty() &&
                  toasts_.back().text.find("history: re-ran landscape.frag") !=
                      std::string::npos;
              // re-run a REJECTED record: the steered audio falls back to
              // actually loading the track (the toast proves the dispatch)
              if (haveSteer) rerunDrop(steerRec);
              const bool steerRerunOk =
                  haveSteer && !toasts_.empty() &&
                  toasts_.back().text.find("history: re-ran tmp_audio_smoke.wav") !=
                      std::string::npos;
              // row colors: green applied/loading, amber steered/ignored,
              // red failed - and every recorded outcome must map to one
              const bool colorOk =
                  dropOutcomeColor("applied") == kPhosphor &&
                  dropOutcomeColor("loading") == kPhosphor &&
                  dropOutcomeColor("scratch") == kPhosphor &&
                  dropOutcomeColor("steered") == kAmber &&
                  dropOutcomeColor("ignored: unknown type") == kAmber &&
                  dropOutcomeColor("no drop action") == kAmber &&
                  dropOutcomeColor("not over a panel") == kAmber &&
                  dropOutcomeColor("failed") == kDanger;
              bool allColored = !dropHistory_.empty();
              for (const auto& r : dropHistory_) {
                if (dropOutcomeColor(r.outcome) == 0) allColored = false;
              }
              histOk = histRecOk && steerFound && rerunOk && steerRerunOk &&
                       colorOk && allColored;
            }
            // Dragged history rows re-dispatch onto panels (the ImGui drop
            // target is a thin wrapper around routePanelPayload, driven
            // directly here like the OS-drop smoke drives drainOsDrops):
            // audio on the strip loads, on the lanes steers, on the Console
            // log list is rejected, on Assets browses - and every
            // re-dispatch is itself recorded in the history.
            bool dragOk = false, debounceOk = false, tlScrollOk = false,
                 tlFitOk = false, tlViewOk = false, panelsOk = false;
            {
              const size_t histSize = dropHistory_.size();
              const std::string dragPath = wav;  // tmp_audio_smoke.wav
              const int dragKind = kindForPath(dragPath);
              clearRects();
              subRects_[(int)DropSub::TimelineStrip] = {0, 0, 2000, 2000, true};
              const bool dragStripOk =
                  routePanelPayload(DropPanel::Timeline, dragKind, dragPath,
                                    100, 100) == "loading";
              clearRects();
              subRects_[(int)DropSub::TimelineStrip] = {0, 0, 20, 20, true};
              const bool dragLanesOk =
                  routePanelPayload(DropPanel::Timeline, dragKind, dragPath,
                                    100, 100) == "steered";
              clearRects();
              subRects_[(int)DropSub::ConsoleLog] = {0, 0, 2000, 2000, true};
              const bool dragConOk =
                  routePanelPayload(DropPanel::Console, dragKind, dragPath,
                                    100, 100).rfind("ignored", 0) == 0;
              clearRects();
              const bool dragAssetsOk =
                  routePanelPayload(DropPanel::Assets, dragKind, dragPath,
                                    100, 100) == "browsing";
              // the viewport takes the payload through the same route too
              const bool dragVpOk =
                  routePanelPayload(DropPanel::Viewport, dragKind, dragPath,
                                    100, 100) == "loading";
              bool dragRecOk = dropHistory_.size() == histSize + 5;
              for (size_t i = histSize; i < dropHistory_.size(); ++i)
                if (dropHistory_[i].path != dragPath) dragRecOk = false;
              // the history persists across restarts: save, wipe, restore,
              // and verify the records - including their WALL-CLOCK times -
              // come back (restore only fills an empty history, so the wipe
              // is required for the round-trip)
              saveEditorState();
              const size_t histCount = dropHistory_.size();
              // what the serializer actually stores: integer epoch seconds
              const double savedT = (double)(long long)dropHistory_.back().t;
              const std::string savedPath = dropHistory_.back().path;
              dropHistory_.clear();
              restoreEditorState();
              bool histPersistOk = dropHistory_.size() == histCount &&
                                   !dropHistory_.empty();
              if (histPersistOk) {
                const DropRecord& last = dropHistory_.back();
                histPersistOk = last.path == savedPath &&
                                !last.file.empty() &&  // recomputed from path
                                last.outcome == "loading" &&
                                std::fabs(last.t - savedT) < 1e-3 &&
                                last.t > 1e6;  // wall-clock epoch, not boot time
              }
              // session-resume marker: pushed at boot when a restored history
              // is non-empty (the ctor already ran, so call it directly) - a
              // display-only record that must never be re-run or dragged
              markSessionResume();
              const DropRecord& mark = dropHistory_.back();
              const bool markOk =
                  mark.panel == -1 && mark.sub == -1 && mark.kind == -1 &&
                  mark.file == "session" &&
                  mark.outcome.rfind("session resumed from", 0) == 0 &&
                  mark.t > 1e6 &&
                  dropOutcomeColor(mark.outcome) == kBlue;
              // boundary: a marker pushed onto a FULL ring displaces the
              // oldest record but never exceeds the cap
              const size_t markBefore = dropHistory_.size();
              while (dropHistory_.size() < (size_t)kDropHistoryCap)
                dropHistory_.push_back(dropHistory_.back());
              markSessionResume();
              const bool markBoundaryOk =
                  dropHistory_.size() == (size_t)kDropHistoryCap &&
                  dropHistory_.back().panel == -1 &&
                  dropHistory_.back().file == "session";
              dropHistory_.resize(markBefore + 1);  // restore the real ring
              dragOk = dragStripOk && dragLanesOk && dragConOk && dragAssetsOk &&
                       dragVpOk && dragRecOk && histPersistOk && markOk &&
                       markBoundaryOk;
              // debounced persistence: a burst of recordDrops writes the JSON
              // ONCE, at the trailing 500ms edge - not once per file. The write
              // counter proves it: nothing while the window is open, exactly
              // one write after it elapses, and the file then holds the whole
              // burst (a multi-file OS drop is exactly this pattern).
              const long writesBefore = saveWrites_;
              const size_t burstBase = dropHistory_.size();
              std::filesystem::remove(editorStatePath(), ec);
              for (int i = 0; i < 5; i++)
                recordDrop(std::string("tmp_debounce_") + (char)('0' + i) +
                               ".wav",
                           0, 0, 6, "applied");
              const bool debHoldOk =
                  saveWrites_ == writesBefore &&  // no write yet...
                  saveDirty_ &&                   // ...it's pending...
                  !std::filesystem::exists(editorStatePath());  // ...on the timer
              std::this_thread::sleep_for(std::chrono::milliseconds(620));
              const bool debTrailOk = saveDue();  // trailing edge reached
              flushPendingSave();  // the same primitive frame()/shutdown() call
              const bool debWriteOk =
                  saveWrites_ == writesBefore + 1 &&  // exactly ONE write
                  std::filesystem::exists(editorStatePath());
              bool debFileOk = false;
              {
                const Value dbv = Json::parseFile(editorStatePath());
                const Value dh = dbv.get("dropHistory");
                debFileOk =
                    dh.isArr() &&
                    (int)dh.asArr().size() == (int)burstBase + 5;
              }
              debounceOk = debHoldOk && debTrailOk && debWriteOk && debFileOk;
              // horizontal timeline scroll: the window [t0, t0+zoom] clamps
              // inside [0, dur-zoom] - before 0, past the end, unchanged in
              // range, and pinned at 0 when zoomed past the whole show
              const TlScrollGeom sgSm = tlScrollGeom(400.0f, 8.0f, 30.0f);
              // handle ~ 400*8/30=106.7 wide, so the travel span is ~293; the
              // mapping is proportional (mid-span => mid-show) and clamps
              const float midSm =
                  tlScrollValue(sgSm.span * 0.5f, sgSm, 8.0f, 30.0f);
              tlScrollOk =
                  clampTlT0(-5.0f, 8.0f, 30.0f) == 0.0f &&
                  clampTlT0(999.0f, 8.0f, 30.0f) == 22.0f &&
                  clampTlT0(10.0f, 8.0f, 30.0f) == 10.0f &&
                  clampTlT0(5.0f, 40.0f, 30.0f) == 0.0f &&
                  // drag mapping: left edge, right edge, overshoot clamps
                  tlScrollValue(0.0f, sgSm, 8.0f, 30.0f) == 0.0f &&
                  tlScrollValue(sgSm.span, sgSm, 8.0f, 30.0f) == 22.0f &&
                  tlScrollValue(-50.0f, sgSm, 8.0f, 30.0f) == 0.0f &&
                  tlScrollValue(sgSm.span + 50.0f, sgSm, 8.0f, 30.0f) ==
                      22.0f &&
                  // proportional mid-span -> mid-show (within 0.5s)
                  midSm > 10.5f && midSm < 11.5f &&
                  // handle width clamps: zoomed past the whole show -> pinned
                  tlScrollGeom(400.0f, 200.0f, 30.0f).maxT0 == 0.0f &&
                  tlScrollGeom(30.0f, 8.0f, 30.0f).hw == 24.0f;
              // timeline fit toggle: first press saves the view and fits the
              // whole show (zoom = duration, scroll to 0), second press
              // restores the saved view, and the toggle re-arms so a third
              // press re-fits from the restored position
              {
                float fz = 12.0f, ft = 5.0f, fv = -1.0f, f0 = 0.0f;
                tlFitApply(30.0f, fz, ft, fv, f0);
                const bool fitOnce = fz == 30.0f && ft == 0.0f &&
                                     fv == 12.0f && f0 == 5.0f;
                tlFitApply(30.0f, fz, ft, fv, f0);
                const bool fitTwice = fz == 12.0f && ft == 5.0f && fv == -1.0f;
                tlFitApply(30.0f, fz, ft, fv, f0);
                const bool fitThird = fz == 30.0f && ft == 0.0f &&
                                      fv == 12.0f && f0 == 5.0f;
                // no timeline (duration 0): fits to the min zoom window
                float zz = 40.0f, zt = 7.0f, zv = -1.0f, z0 = 0.0f;
                tlFitApply(0.0f, zz, zt, zv, z0);
                const bool fitZero = zz == 8.0f && zt == 0.0f && zv == 40.0f;
                tlFitOk = fitOnce && fitTwice && fitThird && fitZero;
              }
              // per-show timeline view persistence: a save MERGES into the
              // other shows' views (never clobbers them), and restoring lands
              // exactly on the saved window; an unknown show keeps the current
              // view. All read/write through the real editor_state.json path.
              {
                const std::string curKey = showKey();
                const float oldZoom = tlZoom_, oldT0 = tlT0_;
                // seed a second show's view, as a previous session would have
                {
                  Value sv = Value::object();
                  sv.set("zoom") = Value(200.0);
                  sv.set("t0") = Value(60.0);
                  sv.set("fitZoom") = Value(90.0);
                  sv.set("fitT0") = Value(12.0);
                  Value map = Value::object();
                  map.set("showB.nsd") = sv;
                  Value seed = Value::object();
                  seed.set("timelineViews") = map;
                  Json::writeFile(editorStatePath(), seed, 2);
                }
                tlZoom_ = 42.0f; tlT0_ = 7.0f;
                tlFitZoom_ = 12.0f; tlFitT0_ = 3.0f;
                saveEditorState();  // writes the current show's view, merges showB
                bool viewSaved = false, viewMerged = false;
                {
                  const Value map =
                      Json::parseFile(editorStatePath()).get("timelineViews");
                  const Value cv = map.get(curKey);
                  viewSaved = cv.isObj() &&
                              cv.get("zoom").asNum(0.0) == 42.0 &&
                              cv.get("t0").asNum(0.0) == 7.0 &&
                              cv.get("fitZoom").asNum(0.0) == 12.0 &&
                              cv.get("fitT0").asNum(0.0) == 3.0;
                  const Value bv = map.get("showB.nsd");
                  viewMerged = bv.isObj() && bv.get("zoom").asNum(0.0) == 200.0;
                }
                // restore showB's window exactly; an unknown show no-ops
                // (returns false, keeps the current window)
                tlZoom_ = 0; tlT0_ = 0; tlFitZoom_ = -1; tlFitT0_ = 0;
                const bool bApplied = applyTimelineViewForShow("showB.nsd");
                const bool viewB = bApplied && tlZoom_ == 200.0f &&
                                   tlT0_ == 60.0f && tlFitZoom_ == 90.0f &&
                                   tlFitT0_ == 12.0f;
                const bool missApplied =
                    applyTimelineViewForShow("showMissing.nsd");
                const bool viewMiss = !missApplied && tlZoom_ == 200.0f &&
                                      tlFitZoom_ == 90.0f;
                // and back to the current show's saved window
                applyTimelineViewForShow(curKey);
                const bool viewBack = tlZoom_ == 42.0f && tlT0_ == 7.0f &&
                                      tlFitZoom_ == 12.0f && tlFitT0_ == 3.0f;
                tlViewOk = viewSaved && viewMerged && viewB && viewMiss && viewBack;
                std::filesystem::remove(editorStatePath(), ec);
                tlZoom_ = oldZoom; tlT0_ = oldT0;  // leave the editor's own view
                tlFitZoom_ = -1.0f; tlFitT0_ = 0.0f;
              }
              // panel visibility persistence: hidden panels stay hidden across
              // a save/restore round-trip (the Toolbar complaint), and a save
              // taken mid-fullscreen-preview persists the PRE-preview layout,
              // not the temporarily-hidden state
              {
                const bool t0 = showToolbar_, h0 = showHierarchy_,
                              i0 = showInspector_, tm0 = showTimeline_,
                              c0 = showConsole_, a0 = showAssets_,
                              p0 = showProfiler_, fs0 = fullscreenPreview_;
                showToolbar_ = false; showConsole_ = false; showProfiler_ = true;
                saveEditorState();
                bool panelsSaved = false;
                {
                  const Value p = Json::parseFile(editorStatePath()).get("panels");
                  panelsSaved = p.isObj() &&
                                !p.get("toolbar").asBool(true) &&
                                !p.get("console").asBool(true) &&
                                p.get("profiler").asBool(false);
                }
                // restoring flips them back from the same file (no side
                // effects: applyPanelVisibility only touches the show flags)
                showToolbar_ = true; showConsole_ = true; showProfiler_ = false;
                applyPanelVisibility(Json::parseFile(editorStatePath()));
                const bool panelsRestored = !showToolbar_ && !showConsole_ &&
                                            showProfiler_;
                // fullscreen guard: while previewing, every panel is hidden -
                // the save must persist what the user had BEFORE entering
                fullscreenPreview_ = true;
                savedToolbar_ = true; savedConsole_ = true;
                showToolbar_ = false; showConsole_ = false;
                saveEditorState();
                const Value pv = Json::parseFile(editorStatePath()).get("panels");
                const bool panelsPreview =
                    pv.get("toolbar").asBool(false) &&
                    pv.get("console").asBool(false);
                fullscreenPreview_ = fs0;
                showToolbar_ = t0; showHierarchy_ = h0; showInspector_ = i0;
                showTimeline_ = tm0; showConsole_ = c0; showAssets_ = a0;
                showProfiler_ = p0;
                panelsOk = panelsSaved && panelsRestored && panelsPreview;
              }
            }
            panelOk = tlScriptOk && tlStripScriptOk && tlSteerOk && tlStripOk &&
                      conHdrOk && conLogOk && saveOk && asOk && histOk && dragOk &&
                      debounceOk && tlScrollOk && tlFitOk && tlViewOk && panelsOk;
            std::filesystem::remove(tlCopy, ec);
            clearRects();
          }
          pickBrowseFile((int)BrowseKind::Shader, w_.shaderDir + "/plasma.frag");
          bool shown = false;
          for (const auto& e : w_.app->activeEffects()) {
            if (e == "quad:plasma.frag") { shown = true; break; }
          }
          // a shader dropped from OUTSIDE the shader dir (data/shaders, the
          // exe dir, ...) must still show: the effect name falls back to the
          // absolute path and the shader manager reads it directly - this
          // proved 'shaders are not draggable' (silent fail -> red toast)
          const std::string absFrag = "tmp_shader_drag.frag";
          {
            std::ifstream in(w_.shaderDir + "/plasma.frag", std::ios::binary);
            std::ofstream out(absFrag, std::ios::binary);
            if (in && out) out << in.rdbuf();
          }
          pickBrowseFile((int)BrowseKind::Shader, absFrag);
          bool shownAbs = false;
          const std::string absEff =
              "quad:" + std::filesystem::absolute(absFrag).string();
          for (const auto& e : w_.app->activeEffects()) {
            if (e == absEff) { shownAbs = true; break; }
          }
          std::filesystem::remove(absFrag, ec);
          const std::string scriptCopy = "tmp_script_pick.nsd";
          {
            std::ifstream in(w_.app->scriptPath(), std::ios::binary);
            std::ofstream out(scriptCopy, std::ios::binary);
            if (in && out) out << in.rdbuf();
          }
          pickBrowseFile((int)BrowseKind::Script, scriptCopy);
          pickOk = shown && shownAbs && w_.app->scriptPath() == scriptCopy &&
                   osShown && !osWrong && osToastOk && dirOk && panelOk;
          std::filesystem::remove(scriptCopy, ec);
        }
        // exercise the browser popup itself: opening the shared browser must
        // render (OpenPopup / BeginPopup / kind combo / list / footer) without
        // crashing, so drawBrowse() runs for the remaining frames
        browseKind_ = (int)BrowseKind::Audio;
        browse_[browseKind_].scanned = false;
        browseOpen_ = true;
        scratchOpen_ = true;  // the scratch modal renders too (source + buttons)
        dropHistoryOpen_ = true;  // the drop-history popup renders live too
        quantize_ = false;  // clean up so the bogus-path check is unaffected
        bogus = !w_.audio->swapTrack("nonexistent_audio_smoke.wav", 0);
        durOk = std::fabs(dur - 1.0f) < 0.05f;
        ok = loaded && asyncOk && durOk && timelineDurationOk && envOk &&
             kicksOk && saved && offOk && quantOk && specOk && browseOk &&
             liveOk && multiOk && relOk && pickOk && toSilent && restored && bogus;
      }
      std::filesystem::remove(wav, ec);
      std::filesystem::remove(editorStatePath(), ec);
      if (ok) {
        Log::info("EDITOR",
                  "audio smoke: runtime swap silence->wav->silence, waveform envelope,"
                  " kick detection, live spectrogram, beat-grid + quantize"
                  " persist/restore, shared asset browser (live refresh,"
                  " multi-kind scans, per-kind persistence, popup render, OS drag-in"
                  " + toast + folder-drop browse + panel + sub-area routing,"
                  " debounced per-drop save (burst->one write))"
                  " + bogus guard - PASS");
      } else {
        char sb[260];
        std::snprintf(sb, sizeof sb,                      " audio smoke: FAIL (ok=%d loaded=%d async=%d dur=%.2f durOk=%d timelineDur=%d"
                      " env=%d kicks=%zu"
                      " [first=%.2f last=%.2f mean=%.3f] saved=%d off=%d quant=%d"
                      " spec=%d browse=%d live=%d multi=%d rel=%d pick=%d"
                      " panel=%d toSilent=%d restored=%d bogus=%d)",
                      (int)ok, (int)loaded, (int)asyncOk, dur, (int)durOk,
                      (int)timelineDurationOk, (int)envOk, kickTimes_.size(),
                      kickTimes_.empty() ? 0.0f : kickTimes_.front(),
                      kickTimes_.empty() ? 0.0f : kickTimes_.back(),
                      kickTimes_.size() > 1
                          ? (kickTimes_.back() - kickTimes_.front()) /
                                (float)(kickTimes_.size() - 1)
                          : 0.0f,
                      (int)saved, (int)offOk, (int)quantOk, (int)specOk, (int)browseOk,
                      (int)liveOk, (int)multiOk, (int)relOk, (int)pickOk,
                      (int)panelOk, (int)toSilent,
                      (int)restored, (int)bogus);
        Log::info("EDITOR", sb);
      }
    }
  }

  return !glfwWindowShouldClose(w_.window);
}

// ---------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------
void DemoEditor::togglePlayback() {
  // Editor startup is intentionally paused. Start the audio clock only when
  // the author actually presses Play/Space; this keeps a loaded track from
  // running ahead while the transport is stopped.
  viewportFocusLock_ = false;
  if (!w_.director) return;
  const bool wasPaused = w_.director->paused;
  if (wasPaused && w_.audio && !w_.audio->started)
    w_.audio->start();
  w_.director->togglePause();
  // The editor transport and audio clock are one playback control. This is
  // important after startup, where the shared player may already have opened
  // the device before the editor intentionally paused the director.
  if (w_.audio) w_.audio->setPlaying(!w_.director->paused);
}

void DemoEditor::handleKeys() {
  if (flyActive_) return;  // fly mode owns the keyboard (Space is handled there)
  ImGuiIO& io = ImGui::GetIO();
  // Shader Lab owns Ctrl+S while its source editor is open, even when the
  // multiline GLSL field has text focus. Do this before the general text-input
  // guard so saving a shader never accidentally saves the production instead.
  if (showShaderLab_ && io.KeyCtrl && !io.KeyShift &&
      ImGui::IsKeyPressed(ImGuiKey_S)) {
    shaderLab_.saveCurrentAsset();
    return;
  }
  // Text fields keep ownership of Space so shader/source editing remains
  // possible. Handle Space before the generic focused-widget guard: timeline
  // rows, sliders, and buttons can retain ImGui focus while the author still
  // expects Space to pause the running audio.
  if (io.WantTextInput) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
    togglePlayback();
    return;
  }
  if (ImGui::IsAnyItemFocused()) return;
  // Ctrl+T opens the audio source popup even when the toolbar is hidden -
  // the TRK control must never be unreachable (a stale imgui.ini or a closed
  // toolbar hides it, and the popup now renders from frame() so it works)
  if (ImGui::IsKeyPressed(ImGuiKey_T) && io.KeyCtrl && !io.KeyShift)
    ImGui::OpenPopup("Audio");
  // F/Home follow the context: over the viewport they frame the selected/all
  // visible scene content; elsewhere they retain the video-editor timeline
  // fit behavior. This keeps the established timeline shortcut useful while
  // making viewport authoring feel native.
  if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl && !io.KeyShift &&
      !io.KeyAlt) {
    if (viewportFocused_) frameViewportSelection(selNode_ == nullptr);
    else fitTimeline();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Home) && !io.KeyCtrl && !io.KeyShift &&
      !io.KeyAlt) {
    if (viewportFocused_) frameViewportSelection(true);
    else fitTimeline();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_R)) {
    if (w_.director) w_.director->init(0);
    if (w_.app) w_.app->seek(0);
    if (w_.timeline) w_.timeline->advance(0);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Period)) stepPending_ = true;
  // scrub quantization: Q toggles snapping to the aligned beat/bar grid;
  // Shift+Q cycles the grid size. The playhead + audio both snap (seekTo).
  if (ImGui::IsKeyPressed(ImGuiKey_Q) && !io.KeyShift) {
    quantize_ = !quantize_;
    char qb[64];
    if (quantize_) {
      std::snprintf(qb, sizeof qb, "scrub quantize: on (%s)",
                    quantizeGrid_ == 1 ? "bars" : "beats");
    } else {
      std::snprintf(qb, sizeof qb, "scrub quantize: off");
    }
    Log::info("EDITOR", qb);
  } else if (ImGui::IsKeyPressed(ImGuiKey_Q) && io.KeyShift) {
    quantize_ = true;
    quantizeGrid_ = quantizeGrid_ == 1 ? 0 : 1;
    char qb[64];
    std::snprintf(qb, sizeof qb, "scrub quantize: %s",
                  quantizeGrid_ == 1 ? "bars" : "beats");
    Log::info("EDITOR", qb);
  }
  // document authoring: save (Ctrl+S), Save As (Ctrl+Shift+S), undo (Ctrl+Z / Ctrl+Y)
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) saveDocumentAsDialog();
  else if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) saveDocument();
  if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) undoDocument();
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) redoDocument();
  if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y)) redoDocument();
  if (ImGui::IsKeyPressed(ImGuiKey_F2) && w_.app) w_.app->reloadScript();
  if (ImGui::IsKeyPressed(ImGuiKey_F11) && w_.toggleFullscreen) w_.toggleFullscreen();
  if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C))
    createCameraFromView();
  if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D) &&
      !selectedTimelineEvent_.empty())
    duplicateSelectedTimelineEvent();
  if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
    if (selNode_ && selNode_->parent) deleteNode(selNode_);
    else if (!selectedTimelineEvent_.empty()) deleteSelectedTimelineEvent();
  }
}

// ---------------------------------------------------------------------------
// menu bar + toolbar
// ---------------------------------------------------------------------------
void DemoEditor::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Reload Script", "F2")) {
      if (w_.app) w_.app->reloadScript();
    }
    if (ImGui::MenuItem("New Project (.nsd)...")) {
      newProjectDialog();
    }
    if (ImGui::MenuItem("Load Project (.nsd)...")) {
      openNativeFileDialog((int)BrowseKind::Script);
    }
    if (ImGui::MenuItem("Save Project (.nsd)", "Ctrl+S", false,
                        !doc_.path.empty() && doc_.dirty)) {
      saveDocument();
    }
    if (ImGui::MenuItem("Save Project As...", "Ctrl+Shift+S", false,
                        !doc_.path.empty())) {
      saveDocumentAsDialog();
    }
    ImGui::Separator();
    // shared asset browser: one modal, per-kind roots/exts/actions, each
    // category remembering its own scan root + last pick
    if (ImGui::BeginMenu("Open Asset")) {
      if (ImGui::MenuItem("Load Track...")) openNativeFileDialog((int)BrowseKind::Audio);
      if (ImGui::MenuItem("Open Texture...")) openNativeFileDialog((int)BrowseKind::Texture);
      if (ImGui::MenuItem("Open Shader...")) openNativeFileDialog((int)BrowseKind::Shader);
      if (ImGui::MenuItem("Open Model...")) openNativeFileDialog((int)BrowseKind::Model);
      if (ImGui::MenuItem("Open Script...")) openNativeFileDialog((int)BrowseKind::Script);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Layout")) {
      std::remove("imgui.ini");
      layoutBuilt_ = false;  // rebuild the default docking next frame
    }
    if (ImGui::MenuItem("Export MP4...")) openExportDialog();
    if (ImGui::MenuItem("Package Project...")) openPackageDialog();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Esc")) glfwSetWindowShouldClose(w_.window, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc_.canUndo())) undoDocument();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, doc_.canRedo())) redoDocument();
    ImGui::Separator();
    if (ImGui::MenuItem("Save Document", "Ctrl+S", false, doc_.dirty)) saveDocument();
    if (ImGui::MenuItem("Insert NSD Command...")) showNsdCommands_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Transport")) {
    const bool playing = w_.director && !w_.director->paused;
    if (ImGui::MenuItem("Play / Pause", "Space")) {
      togglePlayback();
    }
    if (ImGui::MenuItem("Step One Frame", ".")) stepPending_ = true;
    if (ImGui::MenuItem("Restart", "R")) {
      if (w_.director) w_.director->init(0);
      if (w_.app) w_.app->seek(0);
    }
    if (ImGui::MenuItem("Stop")) {
      if (w_.director) w_.director->paused = true;
      if (w_.audio) w_.audio->setPlaying(false);
      if (w_.director) w_.director->show = 0;
      if (w_.app) w_.app->seek(0);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Next Section")) {
      if (w_.app) w_.app->jumpSection(1);
    }
    if (ImGui::MenuItem("Previous Section")) {
      if (w_.app) w_.app->jumpSection(-1);
    }
    ImGui::EndMenu();
    if (playing) ImGui::Text("  running");
  }
  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Fullscreen Preview")) toggleFullscreenPreview();
    if (ImGui::MenuItem("Fly Camera", "RMB drag")) toggleFly();
    if (ImGui::MenuItem("Create Camera From View", "Ctrl+Alt+C"))
      createCameraFromView();
    ImGui::Separator();
    ImGui::MenuItem("Toolbar", nullptr, &showToolbar_);
    ImGui::MenuItem("Hierarchy", nullptr, &showHierarchy_);
    ImGui::MenuItem("Inspector", nullptr, &showInspector_);
    ImGui::MenuItem("Timeline", nullptr, &showTimeline_);
    ImGui::MenuItem("Console", nullptr, &showConsole_);
    ImGui::MenuItem("Assets", nullptr, &showAssets_);
    ImGui::MenuItem("Profiler", nullptr, &showProfiler_);
    ImGui::MenuItem("Shader Lab", nullptr, &showShaderLab_);
    ImGui::MenuItem("NSD Commands", nullptr, &showNsdCommands_);
    ImGui::MenuItem("Curves", nullptr, &showCurves_);
    ImGui::SeparatorText("Viewport guides");
    if (ImGui::MenuItem("Grid", nullptr, &showGrid_)) scheduleSaveEditorState();
    if (ImGui::MenuItem("World axes", nullptr, &showAxes_)) scheduleSaveEditorState();
    if (ImGui::MenuItem("Safe frame", nullptr, &showSafeFrame_)) scheduleSaveEditorState();
    if (ImGui::MenuItem("Center crosshair", nullptr, &showCrosshair_)) scheduleSaveEditorState();
    // discoverable way to open the drop history without knowing about the
    // Console's right-click (the smoke uses it to render the popup live too)
    if (ImGui::MenuItem("OS Drop History")) dropHistoryOpen_ = true;
    ImGui::Separator();
    ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
    ImGui::MenuItem("ImGui Metrics", nullptr, &showMetrics_);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("About Demo Editor")) showAbout_ = true;
    ImGui::EndMenu();
  }
  // The TRK control must never be unreachable: the floating Toolbar can be
  // closed (View -> Toolbar), docked into a strip too narrow to show its last
  // button, or parked off-screen by a stale imgui.ini - so the audio button
  // also lives here, right-aligned in the main menu bar, which can't be
  // hidden or clipped (Ctrl+T works too, from anywhere).
  {
    const float barW = ImGui::GetWindowWidth();
    const float btnW = 150.0f;
    // right-align when there is room; stay inline on a narrow window
    ImGui::SameLine(
        std::max(ImGui::GetCursorPosX() + 12.0f, barW - btnW - 10.0f));
    if (ImGui::Button(("TRK " + trackLabel()).c_str())) ImGui::OpenPopup("Audio");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Audio source (Ctrl+T): pick a .wav/.mp3, or stop audio");
  }
  ImGui::EndMainMenuBar();
}

void DemoEditor::drawToolbar() {
  if (!showToolbar_) return;
  ImGui::Begin("Toolbar", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  // a stale imgui.ini (e.g. after a monitor change) can park this window
  // off-screen, which silently hides every transport control + the TRK
  // button - pull it back into the main viewport
  {
    const ImVec2 tpos = ImGui::GetWindowPos();
    const ImGuiViewport* tvp = ImGui::GetMainViewport();
    if (tpos.x < tvp->WorkPos.x - 8 || tpos.y < tvp->WorkPos.y - 8 ||
        tpos.x > tvp->WorkPos.x + tvp->WorkSize.x ||
        tpos.y > tvp->WorkPos.y + tvp->WorkSize.y) {
      ImGui::SetWindowPos(ImVec2(tvp->WorkPos.x + 8, tvp->WorkPos.y + 8));
    }
  }

  const bool playing = w_.director && !w_.director->paused;
  if (ImGui::Button(playing ? "|| Pause" : "> Play")) {
    togglePlayback();
  }
  ImGui::SameLine();
  if (ImGui::Button("|> Step")) stepPending_ = true;
  ImGui::SameLine();
  if (ImGui::Button("[] Stop")) {
    if (w_.director) w_.director->paused = true;
    if (w_.audio) w_.audio->setPlaying(false);
    if (w_.director) w_.director->show = 0;
    if (w_.app) w_.app->seek(0);
  }
  ImGui::SameLine();
  if (ImGui::Button("<< Restart")) {
    if (w_.director) w_.director->init(0);
    if (w_.app) w_.app->seek(0);
    if (w_.timeline) w_.timeline->advance(0);
    if (w_.audio) w_.audio->seekTrack(0);
  }
  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

  // audio source - placed EARLY in the row so a narrow or docked toolbar
  // still shows it (it used to be the last button, which is the first to
  // clip when the docked strip is too narrow)
  if (ImGui::Button(("TRK " + trackLabel()).c_str())) ImGui::OpenPopup("Audio");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Audio source (Ctrl+T): pick a .wav/.mp3, or stop audio");
  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

  // authoring: add a scene to the script, or create a new asset file
  if (ImGui::Button("+ Scene")) queueAddScene();
  ImGui::SameLine();
  if (ImGui::Button("+ Asset")) openAssetDialog();
  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

  // time readout + scrub-quantize indicator (Q toggles, Shift+Q cycles grid)
  const float show = w_.director ? w_.director->show : 0;
  const float dur = w_.app ? w_.app->editor().duration : 0;
  ImGui::TextUnformatted(fmtTime(show).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("/ %s", fmtTime(dur).c_str());
  ImGui::SameLine();
  if (quantize_) {
    ImGui::TextColored(ImVec4(0.368f, 0.941f, 0.784f, 1.0f), "snap %s",
                       quantizeGrid_ == 1 ? "bar" : "beat");
  } else {
    ImGui::TextDisabled("snap off");
  }

  // speed
  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  if (ImGui::Button("-")) {
    if (w_.director) w_.director->setScale(clampf(w_.director->scale() / 1.25f, 0.1f, 4.0f));
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.2fx", w_.director ? w_.director->scale() : 1.0f);
  ImGui::SameLine();
  if (ImGui::Button("+")) {
    if (w_.director) w_.director->setScale(clampf(w_.director->scale() * 1.25f, 0.1f, 4.0f));
  }

  // show state
  ImGui::SameLine();
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  const std::string scene = w_.app ? w_.app->activeScene() : "";
  const std::string cam = w_.app ? w_.app->activeCamera() : "";
  ImGui::TextDisabled("scene");
  ImGui::SameLine();
  ImGui::TextUnformatted(scene.empty() ? "-" : scene.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("cam");
  ImGui::SameLine();
  ImGui::TextUnformatted(cam.c_str());

  ImGui::End();
}

// ---------------------------------------------------------------------------
// viewport
// ---------------------------------------------------------------------------
void DemoEditor::drawViewportPanel() {
  if (fullscreenPreview_) {
    // take the whole work area (the other panels were hidden on enter; the
    // toggle restores their previous visibility on exit)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  }
  ImGui::Begin("Viewport", nullptr,
               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoCollapse |
                   (fullscreenPreview_ ? (ImGuiWindowFlags_NoTitleBar |
                                          ImGuiWindowFlags_NoDocking |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus)
                                       : ImGuiWindowFlags_None));
  ImGui::PopStyleVar(2);

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();

  if (viewport_.colorTex() && avail.x > 8 && avail.y > 8) {
    const float vw = (float)std::max(viewport_.w, 1);
    const float vh = (float)std::max(viewport_.h, 1);
    const float scale = std::min(avail.x / vw, avail.y / vh);
    const ImVec2 img(vw * scale, vh * scale);
    const ImVec2 p0(o.x + (avail.x - img.x) * 0.5f, o.y + (avail.y - img.y) * 0.5f);

    // remember the picture rect (ImGui coords) so OS-level drops from Explorer
    // can be position-gated to "onto the viewport" in the next frame's drain
    vpRectMinX_ = p0.x; vpRectMinY_ = p0.y;
    vpRectMaxX_ = p0.x + img.x; vpRectMaxY_ = p0.y + img.y;
    vpRectValid_ = true;

    // interactive item over the picture: hover/click-to-focus feed the input
    // forwarder (fly camera), while the texture itself is a plain draw
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("vp_image", img);
    // OpenGL textures use a bottom-left origin while ImGui's screen-space
    // image coordinates use a top-left origin. Keep the live editor preview
    // upright (the normal playback path never passes through this blit).
    dl->AddImage((ImTextureID)(intptr_t)viewport_.colorTex(), p0,
                 ImVec2(p0.x + img.x, p0.y + img.y),
                 ImVec2(0, 1), ImVec2(1, 0));
    dl->AddRect(p0, ImVec2(p0.x + img.x, p0.y + img.y), kLine);
    viewportHovered_ = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) viewportFocused_ = true;
    if (viewportFocused_ && !fullscreenPreview_) {
      dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p0.x + img.x + 2, p0.y + img.y + 2),
                  kPhosphor, 2.0f, 0, 2.0f);
    }
    // drop target: drag a file from the Open Asset browser or the drop
    // history onto the preview - the payload forwards through the SAME
    // judged + recorded route as an OS drop (shader -> show in viewport,
    // audio -> load track, texture -> selected sprite, ...)
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kBrowseDragType)) {
        BrowseDragPayload d{};
        std::memcpy(&d, pl->Data, std::min<size_t>(pl->DataSize, sizeof(d)));
        const ImVec2 mp = ImGui::GetIO().MousePos;
        routePanelPayload(DropPanel::Viewport, d.kind, d.path, mp.x, mp.y);
      } else if (const ImGuiPayload* hov = ImGui::GetDragDropPayload()) {
        if (hov->IsDataType(kBrowseDragType)) {  // hover feedback while dragging
          dl->AddRect(ImVec2(p0.x - 2, p0.y - 2),
                      ImVec2(p0.x + img.x + 2, p0.y + img.y + 2), kAmber, 2.0f, 0, 2.0f);
        }
      }
      ImGui::EndDragDropTarget();
    }

    // Text nodes are rendered as normalized 2D overlays by DemoApp. When one
    // is selected, expose its screen-space anchor directly in the preview so
    // authors can position it with the same interaction they use in motion-
    // graphics tools instead of guessing NDC values in the Inspector.
    SceneNode* textNode =
        (selNode_ && selNode_->type == NodeType::Text) ? selNode_ : nullptr;
    if (textNode) {
      TextData* textData = textNode->asText();
      const std::string dragKey =
          (w_.app ? w_.app->activeScene() : std::string()) + "\x1f" + textNode->name;
      const float ndcX = textNode->pos[0] * 0.5f;
      const float ndcY = textNode->pos[1];
      const ImVec2 anchor(p0.x + (ndcX + 1.0f) * 0.5f * img.x,
                         p0.y + (1.0f - ndcY) * 0.5f * img.y);
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float dx = mouse.x - anchor.x;
      const float dy = mouse.y - anchor.y;
      const bool nearAnchor = dx * dx + dy * dy <= 26.0f * 26.0f;
      const float viewportScale = img.y / std::max(1.0f, (float)viewport_.h);
      const float textHeight = textData ? std::max(16.0f, textData->sizePx * viewportScale) : 24.0f;
      const float textWidth = textData
                                  ? std::max(36.0f, (float)textData->text.size() *
                                                    textData->sizePx * 0.56f * viewportScale)
                                  : 120.0f;
      const ImVec2 textBoxMin(anchor.x - textWidth * 0.5f,
                              anchor.y - textHeight * 0.5f);
      const ImVec2 textBoxMax(anchor.x + textWidth * 0.5f,
                              anchor.y + textHeight * 0.5f);
      const bool insideTextBox = mouse.x >= textBoxMin.x && mouse.x <= textBoxMax.x &&
                                 mouse.y >= textBoxMin.y && mouse.y <= textBoxMax.y;

      // The handle is intentionally visible even when the text itself is
      // obscured by a shader or post effect; it is an editor affordance, not
      // part of the captured frame.
      dl->AddCircleFilled(anchor, 6.0f,
                          textNodeDragging_ ? kHot : kViolet);
      dl->AddCircle(anchor, 14.0f, textNodeDragging_ ? kHot : kViolet,
                    24, 1.5f);
      dl->AddLine(ImVec2(anchor.x - 20, anchor.y),
                  ImVec2(anchor.x + 20, anchor.y),
                  c32(177, 140, 255, 110), 1.0f);
      dl->AddLine(ImVec2(anchor.x, anchor.y - 20),
                  ImVec2(anchor.x, anchor.y + 20),
                  c32(177, 140, 255, 110), 1.0f);
      if (viewportHovered_ && nearAnchor && !textNodeDragging_ && !textViewportEditing_)
        dl->AddText(ImVec2(anchor.x + 18, anchor.y - 9), kViolet,
                    "drag text");
      if (viewportHovered_ && insideTextBox && !textNodeDragging_ && !textViewportEditing_)
        dl->AddText(ImVec2(textBoxMin.x, textBoxMin.y - 18), kViolet,
                    "double-click to edit text");

      if (textNodeDragging_ && textDragNodeKey_ != dragKey) {
        // Scene activation/reload invalidated the gesture between frames.
        // Do not apply a position to a node from the previous scene.
        doc_.cancelEdit();
        textNodeDragging_ = false;
        textDragNodeKey_.clear();
        textDragMoved_ = false;
      }

      if (!textNodeDragging_ && !textViewportEditing_ && viewportHovered_ &&
          ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && insideTextBox) {
        textViewportEditing_ = true;
        textViewportChanged_ = false;
        textViewportEditNode_ = textNode->name;
        std::fill(textEditBuf_.begin(), textEditBuf_.end(), '\0');
        if (textData)
          std::snprintf(textEditBuf_.data(), textEditBuf_.size(), "%s", textData->text.c_str());
        doc_.beginEdit("edit viewport text");
        viewportFocused_ = true;
      }

      if (!textNodeDragging_ && !textViewportEditing_ && viewportHovered_ && nearAnchor &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        textNodeDragging_ = true;
        textDragNodeKey_ = dragKey;
        textDragMoved_ = false;
        doc_.beginEdit("move text node");
        viewportFocused_ = true;
      }

      if (textViewportEditing_ && textViewportEditNode_ == textNode->name && textData) {
        const ImVec2 restoreCursor = ImGui::GetCursorScreenPos();
        const float editW = std::min(std::max(220.0f, textWidth + 80.0f),
                                     std::max(220.0f, avail.x - 20.0f));
        const float editH = std::min(std::max(54.0f, textHeight + 28.0f), 150.0f);
        const float editX = clampf(textBoxMin.x, o.x + 6.0f,
                                   o.x + std::max(6.0f, avail.x - editW - 6.0f));
        const float editY = clampf(textBoxMin.y, o.y + 6.0f,
                                   o.y + std::max(6.0f, avail.y - editH - 6.0f));
        ImGui::SetCursorScreenPos(ImVec2(editX, editY));
        ImGui::SetNextItemWidth(editW);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && insideTextBox)
          ImGui::SetKeyboardFocusHere();
        const bool changed = ImGui::InputTextMultiline(
            "##viewport_text_edit", textEditBuf_.data(), textEditBuf_.size(),
            ImVec2(editW, editH), ImGuiInputTextFlags_AllowTabInput);
        if (changed) {
          textData->text.assign(textEditBuf_.data());
          textNode->dirty = true;
          patchDocumentText(doc_, w_.app ? w_.app->activeScene() : "",
                            textNode->name, textData->text);
          textViewportChanged_ = true;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Live text edit: click outside or press Escape to finish");
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
        const bool finish = ImGui::IsItemDeactivatedAfterEdit() ||
                            ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;
        ImGui::SetCursorScreenPos(restoreCursor);
        if (cancel) {
          doc_.cancelEdit();
          textViewportEditing_ = false;
          textViewportChanged_ = false;
          textViewportEditNode_.clear();
          textEditNodeKey_.clear();
        } else if (finish) {
          doc_.endEdit();
          textViewportEditing_ = false;
          textEditNodeKey_.clear();
          if (textViewportChanged_) textViewportCommitQueued_ = true;
          textViewportChanged_ = false;
        }
      }

      if (textNodeDragging_) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDown[ImGuiMouseButton_Left]) {
          const float mx = clampf((mouse.x - p0.x) / std::max(img.x, 1.0f), 0.0f, 1.0f);
          const float my = clampf((mouse.y - p0.y) / std::max(img.y, 1.0f), 0.0f, 1.0f);
          // TextMesh consumes centerX as an NDC offset and centerY as an NDC
          // height coordinate. Scene text stores the former in pos.x*0.5,
          // hence the factor of two below.
          const V3 next{(2.0f * (2.0f * mx - 1.0f)),
                        (1.0f - 2.0f * my), textNode->pos[2]};
          if (std::fabs(next[0] - textNode->pos[0]) > 0.0001f ||
              std::fabs(next[1] - textNode->pos[1]) > 0.0001f) {
            textNode->setPos(next);
            textDragMoved_ = true;
            patchDocumentTextPosition(doc_, w_.app ? w_.app->activeScene() : "",
                                       textNode->name, next);
          }
        } else {
          const std::string nodeName = textNode->name;
          if (textDragMoved_) {
            doc_.endEdit();
            // Commit once at the end of the gesture. Reloading every mouse
            // motion would invalidate the selected node and make dragging
            // jump or stop.
            writeDocument();
            selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
            textEditNodeKey_.clear();
          } else {
            doc_.cancelEdit();
          }
          textNodeDragging_ = false;
          textDragNodeKey_.clear();
          textDragMoved_ = false;
        }
      }
    }

    // Optional viewport guides stay disabled by default so the editor remains
    // clean for composition previews. They are editor overlays and never enter
    // the captured/exported frame.
    if (showGrid_) {
      const ImU32 grid = c32(125, 179, 255, 42);
      for (int i = 1; i < 10; ++i) {
        const float x = p0.x + img.x * (float)i / 10.0f;
        const float y = p0.y + img.y * (float)i / 10.0f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + img.y), grid);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + img.x, y), grid);
      }
    }
    if (showSafeFrame_) {
      const ImVec2 a(p0.x + img.x * 0.05f, p0.y + img.y * 0.05f);
      const ImVec2 b(p0.x + img.x * 0.95f, p0.y + img.y * 0.95f);
      dl->AddRect(a, b, c32(255, 196, 107, 150), 0.0f, 0, 1.0f);
    }
    if (showCrosshair_) {
      const ImVec2 c(p0.x + img.x * 0.5f, p0.y + img.y * 0.5f);
      dl->AddLine(ImVec2(c.x - 14, c.y), ImVec2(c.x + 14, c.y), c32(255, 255, 255, 130));
      dl->AddLine(ImVec2(c.x, c.y - 14), ImVec2(c.x, c.y + 14), c32(255, 255, 255, 130));
    }
    if (showAxes_) {
      const ImVec2 a(p0.x + 26.0f, p0.y + img.y - 28.0f);
      dl->AddLine(a, ImVec2(a.x + 22, a.y), c32(255, 95, 143, 220), 2.0f);
      dl->AddLine(a, ImVec2(a.x, a.y - 22), c32(94, 240, 200, 220), 2.0f);
      dl->AddText(ImVec2(a.x + 25, a.y - 7), kHot, "X");
      dl->AddText(ImVec2(a.x - 5, a.y - 34), kPhosphor, "Y");
    }

    // HUD overlay (top-left: perf, top-right: show state, bottom-left: time)
    char buf[192];
    const float show = w_.director ? w_.director->show : 0;
    const float dur = w_.app ? w_.app->editor().duration : 0;
    std::snprintf(buf, sizeof buf, "%d x %d   %.1f fps   %.2f ms", viewport_.w, viewport_.h,
                  fps_, frameMs_);
    dl->AddText(ImVec2(p0.x + 8, p0.y + 6), kPhosphor, buf);
    const std::string scene = w_.app ? w_.app->activeScene() : "";
    if (!scene.empty()) {
      std::snprintf(buf, sizeof buf, "scene %s   cam %s", scene.c_str(),
                    w_.app ? w_.app->activeCamera().c_str() : "-");
      const ImVec2 sz = ImGui::CalcTextSize(buf);
      dl->AddText(ImVec2(p0.x + img.x - sz.x - 8, p0.y + 6), kAmber, buf);
    }
    std::snprintf(buf, sizeof buf, "%s / %s", fmtTime(show).c_str(), fmtTime(dur).c_str());
    dl->AddText(ImVec2(p0.x + 8, p0.y + img.y - 22), kFaint, buf);

    // fly camera HUD + hint
    if (flyActive_) {
      const char* flyLine = "FREE CAMERA  WASD move  RMB look  Shift fast  Ctrl slow  Q/E up/down  RMB release";
      const ImVec2 ts = ImGui::CalcTextSize(flyLine);
      dl->AddText(ImVec2(p0.x + (img.x - ts.x) * 0.5f, p0.y + 8), kHot, flyLine);
      std::snprintf(buf, sizeof buf, "speed %.1f u/s", flySpeed_);
      dl->AddText(ImVec2(p0.x + 8, p0.y + img.y - 46), kFaint, buf);
    } else if (viewportHovered_) {
      dl->AddText(ImVec2(p0.x + img.x - 170, p0.y + img.y - 20), c32(133, 146, 167, 210),
                  "right-drag: fly camera");
    }

    // OS-drop toast ring: recent drop outcomes (apply/ignore), bottom-center
    // stacked newest-lowest, each fading out over its last 0.8 s so feedback
    // from a multi-file drop stays visible and never lingers on the preview
    if (!toasts_.empty()) {
      int slot = 0;
      for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++slot) {
        const double age = wallNow() - it->t0;
        if (age >= 3.5) continue;
        const float alpha = age > 2.7 ? (float)(3.5 - age) / 0.8f : 1.0f;
        const ImU32 kLevel = it->level <= 0 ? kError : (it->level == 1 ? kAmber : kPhosphor);
        ImVec4 tc = ImGui::ColorConvertU32ToFloat4(kLevel);
        tc.w = alpha;
        ImVec4 bg = ImGui::ColorConvertU32ToFloat4(kPanel);
        bg.w = 0.82f * alpha;
        const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str());
        const ImVec2 tp(p0.x + (img.x - ts.x) * 0.5f, p0.y + img.y - 58 - slot * 24.0f);
        dl->AddRectFilled(ImVec2(tp.x - 10, tp.y - 4), ImVec2(tp.x + ts.x + 10, tp.y + ts.y + 4),
                          ImGui::ColorConvertFloat4ToU32(bg), 6.0f);
        dl->AddText(tp, ImGui::ColorConvertFloat4ToU32(tc), it->text.c_str());
      }
    }
  } else {
    viewportHovered_ = false;
    vpRectValid_ = false;  // no picture drawn this frame - the OS-drop gate
                           // must not judge drops against a stale rect
    dl->AddRectFilled(o, ImVec2(o.x + avail.x, o.y + avail.y), kPanel);
    dl->AddText(ImVec2(o.x + 12, o.y + 12), kDim, "no frame yet...");
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// hierarchy (scene graph + effect instances)
// ---------------------------------------------------------------------------
void DemoEditor::drawNodeRec(SceneNode* n) {
  if (!n) return;
  const ImU32 tagCol = (n->type == NodeType::Camera)   ? kBlue
                       : (n->type == NodeType::Light)  ? kAmber
                       : (n->type == NodeType::Mesh)   ? kHot
                       : (n->type == NodeType::Text)   ? kViolet
                       : (n->type == NodeType::Sprite) ? kPhosphor
                       : (n->type == NodeType::Video)  ? kViolet
                       : (n->type == NodeType::Particles) ? kPhosphor
                       : (n->type == NodeType::Quad)   ? kAmber
                       : (n->type == NodeType::Post)   ? kHot
                       : (n->type == NodeType::TimelineSystem) ? kBlue
                       : kDim;

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
  if (selNode_ == n) flags |= ImGuiTreeNodeFlags_Selected;
  const bool hasKids = !n->children.empty();
  if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

  const bool open = ImGui::TreeNodeEx(n->name.c_str(), flags);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    selNode_ = n;
    selEffect_.clear();
  }
  // drop target: a texture dragged from the Open Asset browser lands on THIS
  // Sprite node (only sprites accept it - the sprite pipeline is texture-fed)
  if (n->type == NodeType::Sprite) {
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kBrowseDragType)) {
        BrowseDragPayload d{};
        std::memcpy(&d, pl->Data, std::min<size_t>(pl->DataSize, sizeof(d)));
        if (d.kind == (int)BrowseKind::Texture) applyTexturePick(d.path, n);
      }
      ImGui::EndDragDropTarget();
    }
  }
  ImGui::SameLine();
  ImGui::TextColored(ImVec4((tagCol >> IM_COL32_R_SHIFT) / 255.0f,
                            (tagCol >> IM_COL32_G_SHIFT) / 255.0f,
                            (tagCol >> IM_COL32_B_SHIFT) / 255.0f, 0.75f),
                     iconFor(n->type));
  // right-click menu: duplicate / delete
  if (ImGui::BeginPopupContextItem(n->name.c_str())) {
    if (ImGui::MenuItem("Duplicate")) duplicateNode(n);
    if (ImGui::MenuItem("Delete", "Del")) deleteNode(n);
    ImGui::EndPopup();
  }
  if (open && hasKids) {
    for (auto& c : n->children) drawNodeRec(c.get());
    ImGui::TreePop();
  }
}

void DemoEditor::drawSceneList() {
  if (!w_.app) return;
  if (!ImGui::CollapsingHeader("NSD Scenes", ImGuiTreeNodeFlags_DefaultOpen)) return;

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##scene_filter", "Filter scenes...", sceneFilter_,
                          sizeof(sceneFilter_));
  ImGui::TextDisabled("%zu declarations - click a row to jump and edit",
                      doc_.ast.scenes.size());

  ImGui::BeginChild("##nsd_scene_list", ImVec2(0, 185), true);
  const std::string filter(sceneFilter_);
  for (const auto& sc : doc_.ast.scenes) {
    if (!filter.empty() && sc.name.find(filter) == std::string::npos &&
        sc.title.find(filter) == std::string::npos)
      continue;

    const SceneSection* section = nullptr;
    if (w_.app) {
      for (const auto& candidate : w_.app->sections()) {
        if (candidate.name == sc.name) {
          section = &candidate;
          break;
        }
      }
    }
    const bool active = w_.app->activeScene() == sc.name;
    const bool selected = selScene_ == sc.name;
    char label[384];
    std::snprintf(label, sizeof(label), "%s%s  %s##scene_%s",
                  active ? "▶ " : "   ", sc.visible ? "" : "[hidden]",
                  sc.name.c_str(), sc.name.c_str());
    ImGui::PushID(sc.name.c_str());
    if (ImGui::Selectable(label, selected)) {
      selScene_ = sc.name;
      selNode_ = nullptr;
      selEffect_.clear();
      sceneEditScene_.clear();
      // Scene selection is a transport operation, not a quantized scrub: land
      // exactly on the section boundary so setup commands fire deterministically.
      if (section) seekToRaw(section->start);
    }
    if (ImGui::IsItemHovered()) {
      if (section)
        ImGui::SetTooltip("Jump to %s (%s - %s)\nClick to edit this scene",
                          sc.name.c_str(), fmtTime(section->start).c_str(),
                          fmtTime(section->end).c_str());
      else
        ImGui::SetTooltip("Not scheduled (visible=false or no activation)\nClick to edit metadata");
    }
    ImGui::SameLine();
    if (section)
      ImGui::TextDisabled("%s - %s", fmtTime(section->start).c_str(),
                          fmtTime(section->end).c_str());
    else
      ImGui::TextDisabled("not scheduled");
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void DemoEditor::drawHierarchy() {
  ImGui::Begin("Hierarchy", &showHierarchy_);

  drawSceneList();

  // effect instances (not part of the scene graph)
  if (ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
    const auto& all = w_.app->allEffects();
    if (all.empty()) {
      ImGui::TextDisabled("no effects instanced yet");
    }
    for (const auto& kv : all) {
      const bool active = isEffectActive(kv.first);
      char label[384];
      std::snprintf(label, sizeof label, "%s  %s", active ? "●" : "○", kv.first.c_str());
      if (ImGui::Selectable(label, selEffect_ == kv.first)) {
        selEffect_ = kv.first;
        selNode_ = nullptr;
        selScene_.clear();
      }
    }
  }

  // live scene graph (nodes/effect instances), distinct from the .nsd scene declarations above
  if (ImGui::CollapsingHeader("Scene Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
    SceneGraph& g = w_.app->editableScene();
    SceneNode* root = g.root();
    if (root) {
      for (auto& c : root->children) drawNodeRec(c.get());
    }
  }
  ImGui::End();
}

namespace {

Cmd* sceneCameraCommand(SceneDef* scene) {
  if (!scene) return nullptr;
  for (auto& cmd : scene->setup)
    if (cmd.name == "camera") return &cmd;
  return nullptr;
}

V3 sceneCmdVec3(const Cmd& cmd, const char* key, const V3& fallback) {
  float f[3];
  if (cmd.opts.get(key).toFloats(f, 3) == 3) return {f[0], f[1], f[2]};
  return fallback;
}

void setSceneCmdVec3(Cmd& cmd, const char* key, const V3& value) {
  Value::Array a;
  a.push_back(Value((double)value[0]));
  a.push_back(Value((double)value[1]));
  a.push_back(Value((double)value[2]));
  cmd.opts.set(key) = Value(std::move(a));
}

bool isFadeCommand(const Cmd& cmd, const char* direction) {
  return cmd.name == "fade" && cmd.args.size() >= 1 &&
         cmd.args[0].asStr() == direction;
}

bool isSceneFadeOutCommand(const Cmd& cmd) {
  // Both forms are used by existing productions: `fade out 1` is explicit,
  // while `transition fade 1` is the older scene-relative shorthand.
  return isFadeCommand(cmd, "out") ||
         (cmd.name == "transition" && cmd.args.size() >= 1 &&
          cmd.args[0].asStr() == "fade");
}

Cmd* findSceneFadeIn(SceneDef* scene) {
  if (!scene) return nullptr;
  for (auto& cmd : scene->setup)
    if (isFadeCommand(cmd, "in")) return &cmd;
  return nullptr;
}

struct SceneFadeOutRef {
  ScriptBlock* block = nullptr;
  Cmd* command = nullptr;
};

SceneFadeOutRef findSceneFadeOut(SceneDef* scene) {
  if (!scene) return {};
  for (auto& block : scene->blocks)
    for (auto& cmd : block.cmds)
      if (isSceneFadeOutCommand(cmd)) return {&block, &cmd};
  return {};
}

float sceneDurationForEditor(const SceneDef& scene, const Timeline* timeline,
                            const DemoApp* app) {
  if (scene.duration > 0.0f) return scene.duration;
  if (app) {
    for (const auto& section : app->sections())
      if (section.name == scene.name) return section.duration;
  }
  if (timeline && scene.bars > 0) return scene.bars * timeline->barSec();
  return 0.0f;
}

void setSceneTransition(SceneDef& scene, bool entering, bool enabled,
                        float duration, float sceneDuration) {
  duration = std::max(0.0f, duration);
  if (entering) {
    auto& setup = scene.setup;
    for (auto it = setup.begin(); it != setup.end();) {
      if (isFadeCommand(*it, "in")) it = setup.erase(it);
      else ++it;
    }
    if (enabled && duration > 0.0f) {
      Cmd fade;
      fade.name = "fade";
      fade.args.push_back(Value("in"));
      fade.args.push_back(Value((double)duration));
      setup.insert(setup.begin(), std::move(fade));
    }
    return;
  }

  // Scene-relative fade-out commands are placed at the end of the scene.
  // Remove the old editor-owned form first, then add one at the new cue time.
  for (auto bit = scene.blocks.begin(); bit != scene.blocks.end();) {
    auto& cmds = bit->cmds;
    cmds.erase(std::remove_if(cmds.begin(), cmds.end(),
                              [](const Cmd& c) { return isSceneFadeOutCommand(c); }),
               cmds.end());
    if (cmds.empty()) bit = scene.blocks.erase(bit);
    else ++bit;
  }
  if (!enabled || duration <= 0.0f) return;

  ScriptBlock block;
  block.time = std::max(0.0f, sceneDuration - duration);
  Cmd fade;
  fade.name = "fade";
  fade.args.push_back(Value("out"));
  fade.args.push_back(Value((double)duration));
  block.cmds.push_back(std::move(fade));
  scene.blocks.push_back(std::move(block));
  std::stable_sort(scene.blocks.begin(), scene.blocks.end(),
                   [](const ScriptBlock& a, const ScriptBlock& b) {
                     return a.time < b.time;
                   });
}

}  // namespace

void DemoEditor::loadSceneEditorBuffers() {
  const SceneDef* scene = doc_.findScene(selScene_);
  if (!scene) return;
  std::snprintf(sceneTitleBuf_, sizeof(sceneTitleBuf_), "%s", scene->title.c_str());
  constexpr size_t kSetupCapacity = 32768;
  sceneSetupBuf_.assign(kSetupCapacity, 0);
  size_t used = 0;
  for (const auto& cmd : scene->setup) {
    const std::string line = nsdSerializeCmd(cmd) + "\n";
    if (used + line.size() + 1 >= sceneSetupBuf_.size()) break;
    std::memcpy(sceneSetupBuf_.data() + used, line.data(), line.size());
    used += line.size();
  }
  sceneSetupBuf_[used] = '\0';
  sceneEditScene_ = selScene_;
  sceneSetupDirty_ = false;
}

bool DemoEditor::applySceneSetup() {
  SceneDef* scene = doc_.findScene(selScene_);
  if (!scene || sceneSetupBuf_.empty()) return false;
  const std::string wrapper =
      "demo \"Scene Editor\" { duration 1 }\nscene " + selScene_ +
      " {\n" + std::string(sceneSetupBuf_.data()) + "}\n";
  try {
    const Script parsed = ScriptParser::parse(wrapper, "scene editor");
    const SceneDef* edited = nullptr;
    for (const auto& candidate : parsed.scenes)
      if (candidate.name == selScene_) { edited = &candidate; break; }
    if (!edited) return false;
    doc_.beginEdit("edit scene setup");
    scene->setup = edited->setup;
    doc_.endEdit();
    if (!writeDocument()) return false;
    sceneSetupDirty_ = false;
    loadSceneEditorBuffers();
    Log::info("EDITOR", "scene setup updated: " + selScene_);
    return true;
  } catch (const std::exception& e) {
    Log::error("EDITOR", "scene setup rejected: " + std::string(e.what()));
    return false;
  }
}

void DemoEditor::inspectScene() {
  if (sceneEditScene_ != selScene_) loadSceneEditorBuffers();
  SceneDef* scene = doc_.findScene(selScene_);
  if (!scene) return;

  ImGui::TextDisabled(".nsd scene declaration");
  ImGui::SeparatorText(scene->name.c_str());
  ImGui::TextDisabled("Scene names are stable schedule identifiers");

  auto commitField = [this]() {
    doc_.endEdit();
    writeDocument();
  };

  ImGui::Text("start / end");
  ImGui::SameLine();
  const SceneSection* section = nullptr;
  for (const auto& candidate : w_.app->sections()) {
    if (candidate.name == scene->name) { section = &candidate; break; }
  }
  if (section) {
    ImGui::Text("%s  -  %s", fmtTime(section->start).c_str(),
                fmtTime(section->end).c_str());
    if (ImGui::Button("Jump to start")) seekToRaw(section->start);
  } else {
    ImGui::TextDisabled("not scheduled");
  }

  ImGui::SeparatorText("Metadata");
  if (ImGui::InputText("Title", sceneTitleBuf_, sizeof(sceneTitleBuf_))) {
    doc_.beginEdit("edit scene title");
    scene->title = sceneTitleBuf_;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  int bars = scene->bars;
  if (ImGui::DragInt("Bars (0 = auto)", &bars, 1.0f, 0, 100000)) {
    doc_.beginEdit("edit scene bars");
    scene->bars = bars;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  float duration = scene->duration;
  if (ImGui::DragFloat("Duration sec (0 = bars)", &duration, 0.05f, 0.0f, 100000.0f)) {
    doc_.beginEdit("edit scene duration");
    scene->duration = duration;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  float intensity = scene->intensity;
  if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 1.0f)) {
    doc_.beginEdit("edit scene intensity");
    scene->intensity = intensity;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  int chapter = scene->chapter;
  if (ImGui::DragInt("Chapter", &chapter, 1.0f, 0, 1000)) {
    doc_.beginEdit("edit scene chapter");
    scene->chapter = chapter;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  bool visible = scene->visible;
  if (ImGui::Checkbox("Participates in schedule", &visible)) {
    doc_.beginEdit("edit scene visibility");
    scene->visible = visible;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) commitField();

  ImGui::SeparatorText("Camera Rig");
  if (ImGui::Button("Create Camera From View")) {
    createCameraFromView();
    scene = doc_.findScene(selScene_);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Create or update this scene camera from the current viewport pose");
  Cmd* cameraCmd = sceneCameraCommand(scene);
  if (!cameraCmd) {
    ImGui::TextDisabled("This scene has no camera rig command.");
    if (ImGui::Button("Add camera rig")) {
      doc_.beginEdit("add camera rig");
      Cmd camera;
      camera.name = "camera";
      camera.args.push_back(Value(scene->name + "Cam"));
      camera.opts.set("rig") = Value("static");
      camera.opts.set("pos") = Value(Value::Array{Value(0.0), Value(0.0), Value(4.0)});
      camera.opts.set("target") = Value(Value::Array{Value(0.0), Value(0.0), Value(0.0)});
      camera.opts.set("fov") = Value(55.0);
      scene->setup.insert(scene->setup.begin(), std::move(camera));
      doc_.endEdit();
      writeDocument();
    }
  } else {
    const char* rigTypes[] = {"static", "drift", "fly", "nave", "orbit",
                              "spiral", "hover", "city", "descend", "path"};
    int rigIndex = 0;
    for (int i = 0; i < (int)(sizeof(rigTypes) / sizeof(rigTypes[0])); ++i)
      if (cameraCmd->opts.get("rig").asStr("static") == rigTypes[i]) rigIndex = i;
    if (ImGui::Combo("Type", &rigIndex, rigTypes,
                     (int)(sizeof(rigTypes) / sizeof(rigTypes[0])))) {
      doc_.beginEdit("edit camera rig type");
      cameraCmd->opts.set("rig") = Value(rigTypes[rigIndex]);
      doc_.endEdit();
      writeDocument();
      cameraCmd = sceneCameraCommand(scene);
    }

    auto editCameraVec = [this, &cameraCmd](const char* op, const char* label,
                                             const char* key, V3 fallback) {
      if (!cameraCmd) return;
      V3 value = sceneCmdVec3(*cameraCmd, key, fallback);
      const bool changed = ImGui::DragFloat3(label, value.data(), 0.05f);
      if (changed) {
        doc_.beginEdit(op);
        setSceneCmdVec3(*cameraCmd, key, value);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        doc_.endEdit();
        writeDocument();
      }
    };
    auto editCameraFloat = [this, &cameraCmd](const char* op, const char* label,
                                               const char* key, float fallback,
                                               float speed, float minValue,
                                               float maxValue) {
      if (!cameraCmd) return;
      float value = cameraCmd->opts.get(key).asFloat(fallback);
      const bool changed = ImGui::DragFloat(label, &value, speed, minValue, maxValue);
      if (changed) {
        doc_.beginEdit(op);
        cameraCmd->opts.set(key) = Value((double)value);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        doc_.endEdit();
        writeDocument();
      }
    };

    editCameraVec("edit camera position", "Position", "pos", V3{0, 0, 4});
    editCameraVec("edit camera target", "Target", "target", V3{0, 0, 0});
    editCameraVec("edit camera sway", "Sway", "sway", V3{1, 1, 1});
    editCameraFloat("edit camera fov", "FOV", "fov", 62.0f, 0.5f, 10.0f, 160.0f);

    if (ImGui::TreeNode("Rig motion and lens")) {
      editCameraFloat("edit camera amplitude", "Amplitude", "amp", 1.0f, 0.05f, 0.0f, 100.0f);
      editCameraFloat("edit camera frequency", "Frequency", "freq", 1.0f, 0.01f, 0.0f, 100.0f);
      editCameraFloat("edit camera speed", "Speed", "speed", 1.0f, 0.05f, 0.0f, 500.0f);
      editCameraFloat("edit camera radius", "Orbit radius", "radius", 4.5f, 0.05f, 0.0f, 1000.0f);
      editCameraFloat("edit camera omega", "Orbit omega", "omega", 1.0f, 0.01f, -100.0f, 100.0f);
      editCameraFloat("edit camera handheld", "Handheld", "handheld", 0.0f, 0.01f, 0.0f, 10.0f);
      editCameraFloat("edit camera focus", "DOF focus", "dofFocus", 8.0f, 0.05f, 0.0f, 10000.0f);
      editCameraFloat("edit camera aperture", "DOF aperture", "dofAperture", 0.0f, 0.001f, 0.0f, 10.0f);
      editCameraFloat("edit camera descend rate", "Descend rate", "descendRate", 0.45f, 0.01f, 0.0f, 100.0f);
      editCameraFloat("edit camera descend cap", "Descend cap", "descendCap", 7.0f, 0.05f, 0.0f, 10000.0f);
      ImGui::TreePop();
    }
  }

  ImGui::SeparatorText("Scene transitions");
  const float sceneLength = sceneDurationForEditor(*scene, w_.timeline, w_.app);
  const char* transitionTypes[] = {"None", "Fade"};
  auto editTransition = [this, &scene, &sceneLength, &transitionTypes](const char* id,
                                                       const char* label,
                                                       bool entering) {
    bool enabled = entering ? findSceneFadeIn(scene) != nullptr
                            : findSceneFadeOut(scene).command != nullptr;
    float duration = 1.0f;
    if (entering) {
      if (Cmd* fade = findSceneFadeIn(scene))
        duration = fade->args.size() > 1 ? fade->args[1].asFloat(1.0f) : 1.0f;
    } else if (SceneFadeOutRef fade = findSceneFadeOut(scene); fade.command) {
      duration = fade.command->args.size() > 1
                     ? fade.command->args[1].asFloat(1.0f)
                     : 1.0f;
    }
    int type = enabled ? 1 : 0;
    ImGui::PushID(id);
    if (ImGui::Combo(label, &type, transitionTypes, 2)) {
      doc_.beginEdit(entering ? "edit scene transition in" : "edit scene transition out");
      setSceneTransition(*scene, entering, type != 0, duration,
                         sceneLength > 0.0f ? sceneLength : 60.0f);
      doc_.endEdit();
      writeDocument();
    }
    ImGui::BeginDisabled(type == 0);
    const float maxDuration = sceneLength > 0.0f ? sceneLength : 60.0f;
    const bool changed = ImGui::DragFloat("Duration", &duration, 0.05f, 0.0f,
                                          maxDuration);
    ImGui::EndDisabled();
    if (changed) {
      doc_.beginEdit(entering ? "edit scene transition in duration"
                              : "edit scene transition out duration");
      setSceneTransition(*scene, entering, true, duration,
                         sceneLength > 0.0f ? sceneLength : 60.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      doc_.endEdit();
      writeDocument();
    }
    ImGui::PopID();
  };
  editTransition("transition_in", "In", true);
  editTransition("transition_out", "Out", false);
  ImGui::TextDisabled("Fade commands are written into the scene setup and scene-relative timeline.");

  ImGui::SeparatorText("Setup commands");
  ImGui::TextDisabled("One command per line. These run when the scene activates.");
  ImGui::TextDisabled("Use the timeline for scene-relative 'at' blocks.");
  if (sceneSetupBuf_.empty()) loadSceneEditorBuffers();
  if (ImGui::InputTextMultiline("##scene_setup", sceneSetupBuf_.data(),
                                sceneSetupBuf_.size(), ImVec2(-1, 180)))
    sceneSetupDirty_ = true;
  if (sceneSetupDirty_) {
    if (ImGui::Button("Apply setup")) applySceneSetup();
    ImGui::SameLine();
    if (ImGui::Button("Revert setup")) loadSceneEditorBuffers();
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "unsaved setup edits");
  } else {
    ImGui::TextDisabled("setup matches the .nsd document");
  }
}

// ---------------------------------------------------------------------------
// inspector
// ---------------------------------------------------------------------------
namespace {

bool patchTextCommandList(std::vector<Cmd>& cmds, const std::string& nodeName,
                          const std::string& text) {
  for (auto& cmd : cmds) {
    if (cmd.name != "text") continue;
    const std::string candidate = cmd.args.empty() ? "text" : cmd.args[0].asStr();
    if (candidate != nodeName) continue;        // Keep the node identity stable and put the edited value in the explicit
        // option used by the runtime (`text name { text ... }`). This also makes
        // the edit serializable even when the original command used the compact
        // positional form.

    cmd.opts.set("text") = Value(text);
    return true;
  }
  return false;
}

bool patchDocumentText(EditorDocument& doc, const std::string& sceneName,
                       const std::string& nodeName, const std::string& text) {
  if (SceneDef* scene = doc.findScene(sceneName)) {
    if (patchTextCommandList(scene->setup, nodeName, text)) return true;
    for (auto& block : scene->blocks)
      if (patchTextCommandList(block.cmds, nodeName, text)) return true;
  }
  // Text nodes can also be authored in top-level `at` blocks. The fallback is
  // useful for older productions that did not wrap their scene setup in a
  // scene declaration.
  for (auto& block : doc.ast.main)
    if (patchTextCommandList(block.cmds, nodeName, text)) return true;
  return false;
}

bool patchTextSizeCommandList(std::vector<Cmd>& cmds, const std::string& nodeName,
                              int sizePx) {
  for (auto& cmd : cmds) {
    if (cmd.name != "text") continue;
    const std::string candidate = cmd.args.empty() ? "text" : cmd.args[0].asStr();
    if (candidate != nodeName) continue;
    cmd.opts.set("size") = Value(sizePx);
    return true;
  }
  return false;
}

bool patchDocumentTextSize(EditorDocument& doc, const std::string& sceneName,
                           const std::string& nodeName, int sizePx) {
  if (SceneDef* scene = doc.findScene(sceneName)) {
    if (patchTextSizeCommandList(scene->setup, nodeName, sizePx)) return true;
    for (auto& block : scene->blocks)
      if (patchTextSizeCommandList(block.cmds, nodeName, sizePx)) return true;
  }
  for (auto& block : doc.ast.main)
    if (patchTextSizeCommandList(block.cmds, nodeName, sizePx)) return true;
  return false;
}

bool patchTextFontCommandList(std::vector<Cmd>& cmds, const std::string& nodeName,
                              const std::string& font) {
  for (auto& cmd : cmds) {
    if (cmd.name != "text") continue;
    const std::string candidate = cmd.args.empty() ? "text" : cmd.args[0].asStr();
    if (candidate != nodeName) continue;
    cmd.opts.set("font") = Value(font);
    return true;
  }
  return false;
}

bool patchDocumentTextFont(EditorDocument& doc, const std::string& sceneName,
                           const std::string& nodeName, const std::string& font) {
  if (SceneDef* scene = doc.findScene(sceneName)) {
    if (patchTextFontCommandList(scene->setup, nodeName, font)) return true;
    for (auto& block : scene->blocks)
      if (patchTextFontCommandList(block.cmds, nodeName, font)) return true;
  }
  for (auto& block : doc.ast.main)
    if (patchTextFontCommandList(block.cmds, nodeName, font)) return true;
  return false;
}

bool patchTextPositionCommandList(std::vector<Cmd>& cmds, const std::string& nodeName,
                                  const V3& pos) {
  for (auto& cmd : cmds) {
    if (cmd.name != "text") continue;
    const std::string candidate = cmd.args.empty() ? "text" : cmd.args[0].asStr();
    if (candidate != nodeName) continue;
    Value::Array a;
    a.push_back(Value((double)pos[0]));
    a.push_back(Value((double)pos[1]));
    a.push_back(Value((double)pos[2]));
    cmd.opts.set("pos") = Value(std::move(a));
    return true;
  }
  return false;
}

bool patchDocumentTextPosition(EditorDocument& doc, const std::string& sceneName,
                               const std::string& nodeName, const V3& pos) {
  if (SceneDef* scene = doc.findScene(sceneName)) {
    if (patchTextPositionCommandList(scene->setup, nodeName, pos)) return true;
    for (auto& block : scene->blocks)
      if (patchTextPositionCommandList(block.cmds, nodeName, pos)) return true;
  }
  for (auto& block : doc.ast.main)
    if (patchTextPositionCommandList(block.cmds, nodeName, pos)) return true;
  return false;
}

}  // namespace

void DemoEditor::inspectNode(SceneNode* n) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s", n->name.c_str());
  ImGui::PushID("node_name");
  const bool renamed =
      ImGui::InputText("name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopID();
  if (renamed) {
    // the graph's find()/removeChild() are keyed by name and addNode requires
    // unique names, so a rename that collides is rejected instead of corrupting
    // lookups (delete/duplicate could hit the wrong node otherwise)
    const std::string nm = buf;
    SceneGraph& g = w_.app->editableScene();
    SceneNode* clash = nm.empty() ? nullptr : g.find(nm);
    if (nm.empty() || nm == n->name || clash == nullptr || clash == n) {
      n->name = nm;
    } else {
      Log::warn("EDITOR", "node name '" + nm + "' already exists - rename rejected");
    }
  }
  ImGui::TextDisabled("type: %s", typeLabel(n->type));

  ImGui::Separator();
  ImGui::Checkbox("visible", &n->visible);
  ImGui::SameLine();
  ImGui::Checkbox("enabled", &n->enabled);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::DragInt("layer", &n->layer, 1, 0, 31);

  // transform (round-trips through the graph so world matrices recompute)
  ImGui::SeparatorText("Transform");
  V3 p = n->pos;
  if (editorKeyframeButton("kf_pos")) keyframeNodeProperty(n, "pos");
  ImGui::SameLine();
  if (ImGui::DragFloat3("Position", p.data(), 0.05f)) n->setPos(p);
  V3 e = quatToEulerDeg(n->rot);
  if (editorKeyframeButton("kf_rot")) keyframeNodeProperty(n, "euler");
  ImGui::SameLine();
  if (ImGui::DragFloat3("Rotation (deg)", e.data(), 0.5f)) n->setEuler(e);
  V3 s = n->scale;
  if (editorKeyframeButton("kf_scl")) keyframeNodeProperty(n, "scale");
  ImGui::SameLine();
  if (ImGui::DragFloat3("Scale", s.data(), 0.01f, 0.001f, 1000.0f)) n->setScale(s);

  // payload
  switch (n->type) {
    case NodeType::Camera:
      if (auto* d = n->asCamera()) {
        ImGui::SeparatorText("Camera");
        ImGui::DragFloat("FOV", &d->fov, 0.5f, 10.0f, 160.0f);
        ImGui::DragFloat("Near", &d->nearP, 0.005f, 0.001f, 10.0f);
        ImGui::DragFloat("Far", &d->farP, 5.0f, 1.0f, 5000.0f);
        ImGui::DragFloat3("Target", d->target.data(), 0.05f);

        int rigIndex = 0;
        for (int i = 0; i < kCameraRigTypeCount; ++i) {
          if (d->rig == kCameraRigTypes[i]) {
            rigIndex = i;
            break;
          }
        }
        if (ImGui::Combo("Camera type", &rigIndex, kCameraRigTypes,
                         kCameraRigTypeCount)) {
          d->rig = kCameraRigTypes[rigIndex];
        }
        if (d->rig.empty())
          ImGui::TextDisabled("No rig selected; choose a camera type above.");
        else if (std::find(std::begin(kCameraRigTypes),
                           std::end(kCameraRigTypes), d->rig) ==
                 std::end(kCameraRigTypes))
          ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                             "Unknown rig '%s' (select a supported type)",
                             d->rig.c_str());
      }
      break;
    case NodeType::Light:
      if (auto* d = n->asLight()) {
        ImGui::SeparatorText("Light");
        const char* kinds[] = {"point", "directional", "spot"};
        int k = d->type == "directional" ? 1 : d->type == "spot" ? 2 : 0;
        if (ImGui::Combo("type", &k, kinds, 3)) d->type = kinds[k];
        ImGui::ColorEdit3("Color", d->color.data());
        ImGui::DragFloat("Intensity", &d->intensity, 0.05f, 0.0f, 100.0f);
        ImGui::DragFloat("Range", &d->range, 0.1f, 0.1f, 500.0f);
        ImGui::DragFloat("Angle", &d->angle, 0.5f, 1.0f, 179.0f);
        ImGui::Checkbox("cast shadow", &d->castShadow);
      }
      break;
    case NodeType::Mesh:
      if (auto* d = n->asMesh()) {
        ImGui::SeparatorText("Mesh");
        std::snprintf(buf, sizeof buf, "%s", d->model.c_str());
        if (ImGui::InputText("model", buf, sizeof buf)) d->model = buf;
        std::snprintf(buf, sizeof buf, "%s", d->material.c_str());
        if (ImGui::InputText("material", buf, sizeof buf)) d->material = buf;
        ImGui::DragFloat("scale", &d->scale, 0.01f, 0.001f, 1000.0f);
        ImGui::Checkbox("lit", &d->lit);
      }
      break;
    case NodeType::Particles:
      if (auto* d = n->asParticles()) {
        ImGui::SeparatorText("Particles");
        ImGui::DragInt("count", &d->count, 100, 1, 1000000);
        ImGui::DragFloat("renderScale", &d->renderScale, 0.01f, 0.05f, 1.0f);
        std::snprintf(buf, sizeof buf, "%s", d->frag.c_str());
        if (ImGui::InputText("frag", buf, sizeof buf)) d->frag = buf;
      }
      break;
    case NodeType::Quad:
      if (auto* d = n->asQuad()) {
        ImGui::SeparatorText("Shader Quad");
        std::snprintf(buf, sizeof buf, "%s", d->frag.c_str());
        if (ImGui::InputText("frag", buf, sizeof buf)) d->frag = buf;
        ImGui::DragFloat("renderScale", &d->renderScale, 0.01f, 0.05f, 1.0f);
        ImGui::DragFloat("mode", &d->mode, 0.01f);
        ImGui::Checkbox("handoff", &d->handoff);
      }
      break;
    case NodeType::Sprite:
      if (auto* d = n->asSprite()) {
        ImGui::SeparatorText("Sprite");
        std::snprintf(buf, sizeof buf, "%s", d->tex.c_str());
        if (ImGui::InputText("texture", buf, sizeof buf)) d->tex = buf;
        ImGui::ColorEdit4("Color", d->color.data());
        ImGui::DragFloat("opacity", &d->opacity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat3("Size", d->size.data(), 0.05f);
      }
      break;
    case NodeType::Video:
      if (auto* d = n->asVideo()) {
        ImGui::SeparatorText("Video");
        std::snprintf(buf, sizeof buf, "%s", d->file.c_str());
        if (ImGui::InputText("file", buf, sizeof buf)) d->file = buf;
        ImGui::DragInt("decode width", &d->width, 8, 2, 4096);
        ImGui::DragInt("decode height", &d->height, 8, 2, 4096);
        ImGui::DragFloat("fps", &d->fps, 0.5f, 1.0f, 240.0f);
        ImGui::Checkbox("loop", &d->loop);
        ImGui::DragFloat("opacity", &d->opacity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat3("Size", d->size.data(), 0.05f);
        ImGui::TextDisabled("Use: video clip.mp4 { size (2 2 1) loop true }");
      }
      break;
    case NodeType::Text:
      if (auto* d = n->asText()) {
        ImGui::SeparatorText("Text");
        ImGui::TextDisabled("Drag the purple handle in Viewport to position this text");
        // InputText owns an internal editing buffer while focused. A local
        // stack buffer was repopulated from d->text on every frame, which
        // could make fast edits appear to revert or lose characters. Keep a
        // stable, generously sized buffer and synchronize it only when the
        // selected runtime node changes.
        const std::string textKey =
            (w_.app ? w_.app->activeScene() : std::string()) + "\x1f" + n->name;
        if (textEditNodeKey_ != textKey) {
          textEditNodeKey_ = textKey;
          std::fill(textEditBuf_.begin(), textEditBuf_.end(), '\0');
          std::snprintf(textEditBuf_.data(), textEditBuf_.size(), "%s", d->text.c_str());
        }
        const bool textChanged =
            ImGui::InputText("text", textEditBuf_.data(), textEditBuf_.size());
        if (textChanged) {
          d->text.assign(textEditBuf_.data());
          n->dirty = true;
          // Keep the document model in sync while the preview is edited. The
          // runtime node remains live for immediate feedback; the serialized
          // reload is deferred until the field is committed below.
          doc_.beginEdit("edit text");
          patchDocumentText(doc_, w_.app ? w_.app->activeScene() : "",
                            n->name, d->text);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          const std::string nodeName = n->name;
          doc_.endEdit();
          if (doc_.dirty && writeDocument()) {
            // writeDocument() rebuilds the scene graph, so never retain the
            // old pointer. Re-select the freshly created runtime node and
            // force the editor buffer to be re-seeded next frame.
            selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
            textEditNodeKey_.clear();
            // The reload replaced d, so leave this inspector invocation before
            // any further payload controls dereference the old pointer.
            return;
          }
        }
        // A text node can use any TrueType/OTF file under assets/fonts. The
        // runtime caches named atlases, so changing one node does not replace
        // the project's default font or invalidate another scene's text.
        const std::string fontKey =
            (w_.app ? w_.app->activeScene() : std::string()) + "\x1f" + n->name + "\x1f_font";
        if (textFontEditNodeKey_ != fontKey) {
          textFontEditNodeKey_ = fontKey;
          std::fill(textFontEditBuf_.begin(), textFontEditBuf_.end(), '\0');
          std::snprintf(textFontEditBuf_.data(), textFontEditBuf_.size(), "%s", d->font.c_str());
        }
        if (ImGui::InputText("font file", textFontEditBuf_.data(), textFontEditBuf_.size())) {
          d->font.assign(textFontEditBuf_.data());
          n->dirty = true;
          doc_.beginEdit("edit text font");
          patchDocumentTextFont(doc_, w_.app ? w_.app->activeScene() : "",
                                n->name, d->font);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          const std::string nodeName = n->name;
          doc_.endEdit();
          if (doc_.dirty && writeDocument()) {
            selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
            textEditNodeKey_.clear();
            textFontEditNodeKey_.clear();
            return;
          }
        }
        ImGui::TextDisabled("blank = default font; use assets/fonts/name.ttf or .otf");

        // Keep a long caption inside the viewport while it is being edited.
        // TextMesh intentionally uses pixel-sized cells; without this guard a
        // long line can grow wider than the render target and look as if it
        // disappeared when its size is increased.
        const float cellAspect = w_.assets
                                     ? (float)w_.assets->fontMetrics.cellW /
                                           std::max(1, w_.assets->fontMetrics.cellH)
                                     : 1.0f;
        const int charCount = std::max(1, (int)d->text.size());
        const int viewportMax = w_.r && w_.r->viewW > 0
                                    ? (int)((float)w_.r->viewW * 0.88f /
                                            (charCount * std::max(0.01f, cellAspect)))
                                    : 200;
        const int maxTextSize = std::max(6, std::min(200, viewportMax));
        if (d->sizePx > maxTextSize) {
          const std::string nodeName = n->name;
          d->sizePx = maxTextSize;
          n->dirty = true;
          doc_.beginEdit("fit text size");
          if (patchDocumentTextSize(doc_, w_.app ? w_.app->activeScene() : "",
                                    nodeName, d->sizePx)) {
            doc_.endEdit();
            if (doc_.dirty && writeDocument()) {
              selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
              textEditNodeKey_.clear();
              return;
            }
          } else {
            doc_.cancelEdit();
          }
        }
        if (ImGui::DragInt("size px", &d->sizePx, 1, 6, maxTextSize)) {
          n->dirty = true;
          doc_.beginEdit("edit text size");
          patchDocumentTextSize(doc_, w_.app ? w_.app->activeScene() : "",
                                n->name, d->sizePx);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          const std::string nodeName = n->name;
          doc_.endEdit();
          if (doc_.dirty && writeDocument()) {
            selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
            textEditNodeKey_.clear();
            return;
          }
        }
        const char* styles[] = {"terminal", "holo", "glitch", "neon", "scan",
                                "dissolve", "chrome", "outline"};
        int si = 0;
        for (int i = 0; i < 8; i++) if (d->style == styles[i]) si = i;
        if (ImGui::Combo("style", &si, styles, 8)) d->style = styles[si];
        ImGui::ColorEdit4("Color", d->color.data());
        ImGui::DragFloat("opacity", &d->opacity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("align", &d->align, 0.01f, -1.0f, 2.0f);
      }
      break;
    case NodeType::Post:
      if (auto* d = n->asPost()) {
        ImGui::SeparatorText("Post Effect");
        std::snprintf(buf, sizeof buf, "%s", d->preset.c_str());
        if (ImGui::InputText("preset", buf, sizeof buf)) d->preset = buf;
      }
      break;
    default:
      break;
  }

  if (n->parent && ImGui::Button("Delete Node")) deleteNode(n);
  ImGui::SameLine();
  if (ImGui::Button("Duplicate")) duplicateNode(n);
}

void DemoEditor::inspectEffect(Effect* e) {
  const bool active = isEffectActive(selEffect_);
  ImGui::TextDisabled("effect instance");
  ImGui::TextUnformatted(selEffect_.c_str());
  if (ImGui::Button(active ? "Hide" : "Show")) {
    if (active) w_.app->editorHideEffect(selEffect_);
    else w_.app->editorShowEffect(selEffect_);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(active ? "active" : "inactive");

  ImGui::Separator();
  const PerfSample ps = e->perfSample();
  if (ps.frames > 0) {
    ImGui::SeparatorText("GPU timing");
    ImGui::Text("median  %5.2f ms", ps.medianMs);
    ImGui::Text("mean    %5.2f ms", ps.meanMs);
    ImGui::Text("min     %5.2f ms", ps.minMs);
    ImGui::Text("max     %5.2f ms", ps.maxMs);
    ImGui::TextDisabled("%zu frames sampled", ps.frames);
  } else {
    ImGui::TextDisabled("untimed effect (no GPU sample)");
  }
}

void DemoEditor::drawInspector() {
  ImGui::Begin("Inspector", &showInspector_);
  if (!selScene_.empty()) {
    if (doc_.findScene(selScene_)) inspectScene();
    else selScene_.clear();
  }
  if (!selScene_.empty()) {
    // The selected .nsd declaration owns the production-level controls.
  } else if (selNode_) {
    inspectNode(selNode_);
  } else if (!selEffect_.empty()) {
    Effect* e = w_.app->findEffect(selEffect_);
    if (e) inspectEffect(e);
    else ImGui::TextDisabled("effect gone");
  } else {
    ImGui::TextDisabled("select a scene node or effect instance");
    ImGui::TextWrapped("The scene graph and every effect instance live here; "
                       "edits apply to the live preview immediately.");
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// timeline
// ---------------------------------------------------------------------------
void DemoEditor::drawTimeline() {
  ImGui::Begin("Timeline", &showTimeline_);
  recordPanelRect(DropPanel::Timeline);  // OS-drop target: audio/scripts
  const TimelineEditor& te = w_.app->editor();
  const float contentDuration = timelineContentDuration();
  float show = w_.director ? w_.director->show : 0;

  ImGui::PushItemWidth(130);
  // The slider must be able to represent the entire production. The old
  // fixed 240-second ceiling made a 346-second show impossible to fit and
  // silently clipped the Fit state back to 240 seconds on the next frame.
  const float maxView = std::max(240.0f, std::max(contentDuration, 8.0f));
  if (ImGui::SliderFloat("view", &tlZoom_, 8.0f, maxView, "%.0f s")) {
    tlT0_ = clampTlT0(tlT0_, tlZoom_, contentDuration);  // keep the window in range
    tlFitZoom_ = -1.0f;  // a manual zoom leaves fit mode; the next F re-fits
  }
  // Keep a loaded track's tail reachable even if the project show is shorter.
  // This also handles an async audio swap arriving while the timeline is open.
  tlT0_ = clampTlT0(tlT0_, tlZoom_, contentDuration);
  // the zoom sticks per-show: debounced save once the drag/click releases
  if (ImGui::IsItemDeactivatedAfterEdit()) scheduleSaveEditorState();
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ImGui::Button(tlFitZoom_ >= 0.0f ? "Restore" : "Fit All")) fitTimeline();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Fit the whole show (F / Home); press again to restore the "
                      "previous view");
  // the audio strip lives in this panel, so its control does too: the toolbar
  // TRK button can be lost when the floating toolbar is closed/docked narrow,
  // but this one is always visible (the popup renders from frame())
  ImGui::SameLine();
  if (ImGui::Button("TRK")) ImGui::OpenPopup("Audio");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Audio source (Ctrl+T): pick a .wav/.mp3, or stop audio");
  ImGui::SameLine();
  ImGui::TextDisabled("drag ruler / lanes to scrub • middle-drag to pan • wheel to zoom");
  ImGui::Separator();

  const float w = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
  const float rulerH = 30.0f;
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  auto xOf = [&](float t) { return (t - tlT0_) * w / tlZoom_; };
  auto tOf = [&](float x) { return tlT0_ + x * tlZoom_ / w; };
  auto seekToAndUpdateLabel = [&](float t) {
    seekTo(t);
    show = w_.director ? w_.director->show : t;
  };

  // --- ruler ------------------------------------------------------------------
  ImGui::InvisibleButton("tl_ruler", ImVec2(w, rulerH));
  const ImVec2 r0 = o, r1(o.x + w, o.y + rulerH);
  dl->AddRectFilled(r0, r1, kPanel);
  dl->AddRect(r0, r1, kLine);

  const float beat = w_.timeline ? w_.timeline->beatSec() : 0.2777f;
  const float bar = w_.timeline ? w_.timeline->barSec() : beat * 4.0f;
  const float t0 = tlT0_, t1 = tlT0_ + tlZoom_;
  const float pxPerSec = w / tlZoom_;

  // beat/bar grid (shifted by the beat-marker alignment so the grid lines up
  // with the actual track's kicks): beatOffset_ is added to n*beat
  if (beat * pxPerSec > 5.0f) {
    for (float t = std::floor((t0 - beatOffset_) / beat) * beat + beatOffset_;
         t <= t1; t += beat) {
      const float x = o.x + xOf(t);
      dl->AddLine(ImVec2(x, r1.y - 6), ImVec2(x, r1.y), kFaint);
    }
  }
  for (float t = std::floor((t0 - beatOffset_) / bar) * bar + beatOffset_;
       t <= t1; t += bar) {
    const float x = o.x + xOf(t);
    dl->AddLine(ImVec2(x, r0.y + 4), ImVec2(x, r1.y), kLine);
    char b[32];
    std::snprintf(b, sizeof b, "%d", (int)std::floor((t - beatOffset_) / bar));
    dl->AddText(ImVec2(x + 3, r0.y + 4), kFaint, b);
  }

  // markers: click = jump, drag = move, double-click = edit (the document
  // side lives in editor_markers.cpp)
  const ImGuiIO& mio = ImGui::GetIO();
  for (const auto& m : te.markers) {
    const float x = o.x + xOf(m.time);
    const ImU32 col =
        (markerDragging_ && m.name == markerDragName_) ? kHot : kAmber;
    dl->AddTriangleFilled(ImVec2(x, r0.y + 2), ImVec2(x - 5, r0.y + 11),
                          ImVec2(x + 5, r0.y + 11), col);
    dl->AddText(ImVec2(x + 6, r0.y + 2), col, m.name.c_str());
  }
  if (mio.MousePos.y >= r0.y && mio.MousePos.y <= r1.y) {
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      for (const auto& m : te.markers) {
        const float mx = o.x + xOf(m.time);
        if (std::fabs(mio.MousePos.x - mx) <= 10.0f) {
          if (markerDragging_) {  // the first press of the double-click began a drag
            markerDragging_ = false;
            doc_.cancelEdit();
            markerDragName_.clear();
          }
          openMarkerEdit(m.name);
          break;
        }
      }
    } else if (!markerDragging_ &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      for (const auto& m : te.markers) {
        const float mx = o.x + xOf(m.time);
        if (std::fabs(mio.MousePos.x - mx) <= 8.0f) {
          markerDragBegin(m.name);
          break;
        }
      }
    }
  }
  if (markerDragging_) {
    float mt = tOf(mio.MousePos.x - o.x);
    if (quantize_) mt = snapKeyTime(mt);
    markerDragMove(markerDragName_, std::max(mt, 0.0f));
    if (!mio.MouseDown[ImGuiMouseButton_Left]) markerDragEnd();
  }

  // playhead on the ruler
  {
    const float x = o.x + xOf(show);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, r1.y), kHot);
    char b[32];
    std::snprintf(b, sizeof b, "%s", fmtTime(show).c_str());
    dl->AddText(ImVec2(x + 3, r0.y + 14), kHot, b);
  }

  // scrub on the ruler
  ImGuiIO& io = ImGui::GetIO();
  if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) &&
      ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float mx = io.MousePos.x - o.x;
    if (mx >= 0 && mx <= w) seekToAndUpdateLabel(tOf(mx));
  }

  // --- tracks -------------------------------------------------------------------
  // reserve the audio waveform/analyzer strip below the lanes
  const float stripH = 58.0f;
  const float avail = std::max(ImGui::GetContentRegionAvail().y - stripH, 60.0f);
  // zero child padding so the lane geometry lines up with the ruler (and the
  // audio strip below): the default 10px WindowPadding shifted everything
  // right of the beat grid and clipped the last ~10px of each row
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("tl_tracks", ImVec2(w, avail));
  const ImVec2 to = ImGui::GetCursorScreenPos();

  // section bands behind everything
  for (const auto& sec : w_.app->sections()) {
    const float x0 = to.x + xOf(sec.start), x1 = to.x + xOf(std::min(sec.end, t1));
    if (x1 <= to.x || x0 >= to.x + w) continue;
    dl->AddRectFilled(ImVec2(x0, to.y), ImVec2(x1, to.y + avail), c32(94, 240, 200, 14));
    dl->AddText(ImVec2(x0 + 6, to.y + 4), c32f(0.9f, 0.95f, 1.0f, 0.55f), sec.name.c_str());
  }

  // track rows
  const int rows = std::max<int>(1, (int)te.tracks.size());
  const float rowH = 30.0f;
  for (int r = 0; r < rows; r++) {
    const float y0 = to.y + r * rowH, y1 = y0 + rowH - 2;
    dl->AddRectFilled(ImVec2(to.x, y0), ImVec2(to.x + w, y1),
                      (r % 2) ? c32(18, 24, 36, 120) : c32(14, 19, 29, 120));
    const std::string name = te.tracks.empty() ? "events" : te.tracks[(size_t)r].name;
    dl->AddText(ImVec2(to.x + 6, y0 + 6), kFaint, name.c_str());
  }

  // events -> colored clips on their track rows
  for (const auto& ev : te.events) {
    int r = ev.track >= 0 && ev.track < rows ? ev.track : 0;
    if (!ev.enabled) continue;
    const float eventTime =
        timelineEventDragging_ && selectedTimelineEvent_ == ev.name
            ? timelineEventDragT0_ : ev.time;
    const float x0 = to.x + xOf(eventTime);
    const float dur = std::max(ev.duration, 0.35f);
    const float x1 = to.x + xOf(eventTime + dur);
    if (x1 < to.x || x0 > to.x + w) continue;
    const ImU32 col = kTrackPalette[(size_t)r % 8];
    const bool selected = selectedTimelineEvent_ == ev.name;
    const float y0 = to.y + r * rowH + 6, y1 = to.y + (r + 1) * rowH - 8;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      selected ? (col & 0xA0FFFFFFu) : (col & 0x50000000u));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), selected ? kHot : col,
                2.0f, 0, selected ? 2.0f : 1.0f);
    dl->AddText(ImVec2(x0 + 5, y0 + 4), selected ? kHot : col, ev.name.c_str());
  }

  // clips: brackets
  for (const auto& cl : te.clips) {
    const float x0 = to.x + xOf(cl.start), x1 = to.x + xOf(cl.end);
    if (x1 < to.x || x0 > to.x + w) continue;
    int r = cl.track >= 0 && cl.track < rows ? cl.track : 0;
    const float y0 = to.y + r * rowH + 1, y1 = to.y + (r + 1) * rowH - 3;
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kAmber, 2.0f, 0, 1.5f);
    dl->AddText(ImVec2(x0 + 6, y0 - 13), kAmber, cl.name.c_str());
  }

  // playhead across the lanes + full-lane scrub (content may be taller than
  // the visible child when there are many tracks, so the child scrolls)
  const float contentH = std::max((float)rows * rowH, avail);
  {
    const float x = to.x + xOf(show);
    dl->AddLine(ImVec2(x, to.y), ImVec2(x, to.y + contentH), kHot, 1.5f);
  }
  ImGui::InvisibleButton("tl_canvas", ImVec2(w, contentH));
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    const float mx = io.MousePos.x - to.x;
    const float my = io.MousePos.y - to.y;
    selectedTimelineEvent_.clear();
    for (const auto& ev : te.events) {
      if (!ev.enabled) continue;
      const int er = ev.track >= 0 && ev.track < rows ? ev.track : 0;
      const float ex0 = xOf(ev.time);
      const float ex1 = xOf(ev.time + std::max(ev.duration, 0.35f));
      const float ey0 = er * rowH + 4.0f;
      const float ey1 = (er + 1) * rowH - 5.0f;
      if (mx >= ex0 && mx <= ex1 && my >= ey0 && my <= ey1) {
        selectedTimelineEvent_ = ev.name;
        int topIndex = -1;
        if (topLevelTimelineIndex(ev.name, topIndex)) {
          timelineEventDragging_ = true;
          timelineEventMoved_ = false;
          timelineEventDragT0_ = ev.time;
          doc_.beginEdit("move timeline event");
        }
        break;
      }
    }
    if (!selectedTimelineEvent_.empty()) {
      showToast("selected " + selectedTimelineEvent_ + "  (drag to move, Ctrl+D duplicate, Del delete)", 2);
    } else if (mx >= 0 && mx <= w) {
      seekToAndUpdateLabel(tOf(mx));
    }
  } else if (!timelineEventDragging_ &&
             (ImGui::IsItemHovered() || ImGui::IsItemActive()) &&
             ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float mx = io.MousePos.x - to.x;
    if (mx >= 0 && mx <= w) seekToAndUpdateLabel(tOf(mx));
  }
  if (timelineEventDragging_) {
    int topIndex = -1;
    if (!topLevelTimelineIndex(selectedTimelineEvent_, topIndex) ||
        topIndex < 0 || topIndex >= (int)doc_.ast.main.size()) {
      doc_.cancelEdit();
      timelineEventDragging_ = false;
      timelineEventMoved_ = false;
    } else if (io.MouseDown[ImGuiMouseButton_Left]) {
      const float next = clampf(tOf(io.MousePos.x - to.x), 0.0f,
                                te.duration > 0 ? te.duration : 100000.0f);
      if (std::fabs(next - timelineEventDragT0_) > 0.0005f) {
        doc_.ast.main[(size_t)topIndex].time = next;
        timelineEventDragT0_ = next;
        timelineEventMoved_ = true;
      }
    } else {
      if (timelineEventMoved_) {
        doc_.endEdit();
        if (writeDocument()) showToast("timeline event moved", 2);
      } else {
        doc_.cancelEdit();
      }
      timelineEventDragging_ = false;
      timelineEventMoved_ = false;
      selectedTimelineEvent_.clear();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // --- audio strip: waveform + beat grid + live analyzer + scrub -----------------
  if (w_.audio &&
      (w_.audio->trackPath() != audioEnvPath_ ||
       w_.audio->trackFrames() != audioEnvFrames_)) {
    rebuildAudioEnvelope();  // new track, or the same path re-recorded
  }
  const AudioEngine* ae = w_.audio;
  const ImVec2 ao = ImGui::GetCursorScreenPos();
  const ImVec2 aa1(ao.x + w, ao.y + stripH);
  // the strip is the audio drop zone (vs the lanes above: the sequence zone)
  recordSubRect(DropSub::TimelineStrip, ao.x, ao.y, aa1.x, aa1.y);
  dl->AddRectFilled(ao, aa1, kPanel);
  dl->AddRect(ao, aa1, kLine);

  // layout: waveform in the top ~46%, the FFT spectrogram heat strip below
  const float waveH = stripH * 0.46f;

  // beat / bar grid across the strip (same time mapping + alignment as the
  // ruler - the beat-marker editor's alignment lives here)
  const float s0 = tlT0_, s1 = tlT0_ + tlZoom_;
  if (beat * pxPerSec > 5.0f) {
    for (float t = std::floor((s0 - beatOffset_) / beat) * beat + beatOffset_;
         t <= s1; t += beat) {
      const float x = ao.x + xOf(t);
      dl->AddLine(ImVec2(x, ao.y + 4), ImVec2(x, aa1.y - 4), kFaint);
    }
  }
  for (float t = std::floor((s0 - beatOffset_) / bar) * bar + beatOffset_;
       t <= s1; t += bar) {
    const float x = ao.x + xOf(t);
    dl->AddLine(ImVec2(x, ao.y + 2), ImVec2(x, aa1.y - 2), kLine);
  }

  // detected kick transients: amber tick marks on the top edge, at the exact
  // times the energy detector found a low-frequency attack. Click one to snap
  // the nearest grid line to it; drag a grid line to shift the whole grid.
  if (!kickTimes_.empty() && beat * pxPerSec > 4.0f) {
    for (const float k : kickTimes_) {
      const float x = ao.x + xOf(k);
      if (x < ao.x || x > aa1.x) continue;
      dl->AddLine(ImVec2(x, ao.y), ImVec2(x, ao.y + 7), kAmber, 1.5f);
    }
  }

  // draggable grid-line grab handles: a small triangle on each beat line's top
  // edge marks the beat-marker editor's drag targets
  if (beat * pxPerSec > 5.0f) {
    for (float t = std::floor((s0 - beatOffset_) / beat) * beat + beatOffset_;
         t <= s1; t += beat) {
      const float x = ao.x + xOf(t);
      dl->AddTriangleFilled(ImVec2(x, ao.y + 9), ImVec2(x - 3, ao.y + 15),
                            ImVec2(x + 3, ao.y + 15), kFaint);
    }
  }

  // waveform: the loaded track's precomputed peak envelope (one bucket per
  // ~17ms) in the strip's top half - the spectrogram lives below it
  if (!audioEnv_.empty()) {
    const float midY = ao.y + waveH * 0.5f;
    const float amp = waveH * 0.42f;
    for (float x = 0; x < w; x += 1.0f) {
      const float t = tOf(x);
      if (t < 0) continue;
      const size_t b = (size_t)(t * kAudioEnvPerSec);
      if (b >= audioEnv_.size()) break;  // past the end of the track
      const float h = audioEnv_[b] * amp;
      dl->AddLine(ImVec2(ao.x + x, midY - h), ImVec2(ao.x + x, midY + h),
                  c32(94, 240, 200, 200));
    }
  } else {
    // the strip is also the audio drop zone - say so when no track is loaded
    dl->AddText(ImVec2(ao.x + 12, ao.y + waveH * 0.5f - 8.0f), kDim,
                ae && ae->trackMode
                    ? "waveform..."
                    : "no track - load one (TRK / drop .wav here)");
  }

  // live FFT spectrogram: a column-scrolling heat strip under the waveform,
  // synced to the playhead - each column sits at its capture time, so the
  // newest data is always right at the playhead and older columns stream left
  // as the show advances (scrubbed-ahead times are culled until played again)
  {
    const unsigned bins = ae ? ae->spectrumBins() : 0;
    if (ae && bins > 0) {
      const uint32_t cnt = ae->spectrumCount();
      if (cnt > specSeen_) {
        // after a long stall the ring may have overwritten columns we never
        // copied - skip them rather than snapshot wrong data/times
        if (specSeen_ < cnt - ae->spectrumCap()) specSeen_ = cnt - ae->spectrumCap();
        for (uint32_t i = specSeen_; i < cnt; i++) {
          const float* col = ae->spectrumColumn(i);
          specSnap_.insert(specSnap_.end(), col, col + bins);
          specTime_.push_back(ae->spectrumColumnTime(i));
        }
        specSeen_ = cnt;
        const size_t maxCols = ae->spectrumCap();
        while (specTime_.size() > maxCols) {
          specSnap_.erase(specSnap_.begin(), specSnap_.begin() + bins);
          specTime_.erase(specTime_.begin());
        }
      }
    }
    const float specY0 = ao.y + waveH;
    const float specH = aa1.y - specY0;
    if (bins > 0 && specH > 4.0f && !specSnap_.empty()) {
      // cost-bounded rendering: columns denser than ~1.6px are decimated and
      // bins thinner than ~1.4px are merged, so the strip stays a few
      // thousand prims even fully zoomed out (40 bins x 720 cols otherwise)
      const float rowH = specH / (float)bins;
      const int binStep = rowH > 1.4f ? 1 : (int)std::ceil(1.4f / rowH);
      const float colPx =
          (float)AudioEngine::kSpecHop / (float)(ae ? ae->sampleRate() : 48000) * pxPerSec;
      const size_t nCols = specTime_.size();
      float lastX = -1e9f;
      for (size_t i = 0; i < nCols; i++) {
        const float t = specTime_[i];
        if (t > show + 0.05f || t < show - 10.0f) continue;  // future / too old
        const float x = ao.x + xOf(t);
        if (x < ao.x - 2.0f || x > aa1.x) continue;
        if (x - lastX < 1.6f) continue;  // sub-pixel columns: decimate
        lastX = x;
        const float cw = std::max(2.0f, colPx);  // column width in px
        const float* col = &specSnap_[i * bins];
        for (int b = 0; b < (int)bins; b += binStep) {
          float v = 0.0f;
          for (int bb = b; bb < std::min((int)bins, b + binStep); bb++)
            v = std::max(v, col[bb]);  // merged-bin peak
          if (v < 0.02f) continue;      // silence/floor: let the panel show
          const float y0 = specY0 + (float)(bins - 1 - b) * rowH;
          dl->AddRectFilled(ImVec2(x, y0),
                            ImVec2(x + cw, y0 + (float)binStep * rowH + 0.5f),
                            specColor(v));
        }
      }
    }
  }

  // playhead + drag-to-scrub across the strip
  {
    const float x = ao.x + xOf(show);
    dl->AddLine(ImVec2(x, ao.y), ImVec2(x, aa1.y), kHot, 1.5f);
  }
  ImGui::InvisibleButton("tl_audio_scrub", ImVec2(w, stripH));

  // --- beat-marker editing ------------------------------------------------------
  // The top ~16px of the strip is the grid editor: drag a beat line to shift
  // the grid phase (snaps to nearby kick ticks), click a kick tick to snap the
  // grid to it, right-click to auto-align to all detected kicks. Below that,
  // drag still scrubs.
  const float mx = io.MousePos.x - ao.x;
  const float my = io.MousePos.y - ao.y;
  const float gridZoneY = 16.0f;
  const bool inStrip = ImGui::IsItemHovered() || ImGui::IsItemActive();

  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && inStrip) {
    autoAlignBeats();  // fit the grid to the detected kicks
  }

  // beat-grid editing. The press's distance to the nearest beat line decides
  // between the three actions: within the grab radius (9px) = drag the grid,
  // on a kick tick = snap the grid to it (only if NOT near a line, so a press
  // can't both snap and drag), anywhere else in the zone = scrub. The offset
  // stays UNWRAPPED during the drag (so crossing a beat boundary doesn't make
  // the dragged line jump identity) and is normalized to [0, beat) on release.
  if (inStrip && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float tAt = tOf(mx);
    const int nNear =
        beat > 0 ? (int)std::lround((tAt - beatOffset_) / beat) : 0;
    const float lineX =
        beat > 0 ? ao.x + xOf(beatOffset_ + nNear * beat) : 1e30f;
    const bool nearLine = std::fabs(mx - lineX) < 9.0f;
    if (!beatDrag_) {
      if (beat > 0 && my < gridZoneY && nearLine) {
        beatDrag_ = true;
        beatDragIdx_ = nNear;
        beatDragBase_ = beatOffset_;  // anchor so crossing beats is smooth
      }
    }
    if (beatDrag_) {
      beatOffset_ = beatDragBase_ + (tAt - (beatDragBase_ + beatDragIdx_ * beat));
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && my < gridZoneY &&
               beat > 0 && !kickTimes_.empty() && !nearLine) {
      // snap the grid phase to the clicked kick tick
      float bestD = 8.0f, bestK = -1;
      for (const float k : kickTimes_) {
        const float d = std::fabs(xOf(k) - mx);
        if (d < bestD) { bestD = d; bestK = k; }
      }
      if (bestK >= 0) {
        const int n = (int)std::lround((bestK - beatOffset_) / beat);
        beatOffset_ = bestK - n * beat;
        if (beatOffset_ < 0) beatOffset_ += beat;
        saveEditorState();
      }
    }
  } else if (beatDrag_) {
    beatDrag_ = false;
    beatOffset_ = std::fmod(beatOffset_, beat);  // normalize phase to [0, beat)
    if (beatOffset_ < 0) beatOffset_ += beat;
    saveEditorState();  // the new alignment sticks + is remembered
  }
  // scrub (below the grid editor zone, or any press that isn't on a grid line)
  if (!beatDrag_ && inStrip && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      (my >= gridZoneY || beat <= 0)) {
    if (mx >= 0 && mx <= w) seekToAndUpdateLabel(tOf(mx));
  }

  // alignment readout: the grid phase in ms (and a hint when kicks are shown)
  if (beatOffset_ != 0) {
    char ob[48];
    std::snprintf(ob, sizeof ob, "off %+.0f ms", beatOffset_ * 1000.0f);
    dl->AddText(ImVec2(aa1.x - 70, ao.y + 22), kAmber, ob);
  } else if (!kickTimes_.empty() && my >= gridZoneY) {
    dl->AddText(ImVec2(ao.x + 6, ao.y + 21), kFaint, "drag grid / click kick / RMB auto-align");
  }
  // Direct timeline navigation: middle-drag pans the time window and the
  // wheel zooms around the cursor. The gestures work across the ruler, lanes,
  // and audio strip, including when audio extends beyond the NSD show.
  {
    const bool timelineHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float windowBottom = windowPos.y + ImGui::GetWindowHeight();
    const bool overTimelineContent =
        timelineHovered && io.MousePos.x >= o.x && io.MousePos.x <= o.x + w &&
        io.MousePos.y >= o.y && io.MousePos.y <= windowBottom;
    const bool panInput = io.MouseDown[ImGuiMouseButton_Middle];

    if (overTimelineContent && panInput) {
      if (!timelinePanning_) {
        timelinePanning_ = true;
        timelinePanStartX_ = io.MousePos.x;
        timelinePanStartT0_ = tlT0_;
      }
      const float dx = io.MousePos.x - timelinePanStartX_;
      tlT0_ = clampTlT0(timelinePanStartT0_ - dx * tlZoom_ / w,
                        tlZoom_, contentDuration);
      tlFitZoom_ = -1.0f;
    } else if (timelinePanning_) {
      timelinePanning_ = false;
      scheduleSaveEditorState();
    }

    if (overTimelineContent && !timelinePanning_ && io.MouseWheel != 0.0f) {
      const float oldZoom = tlZoom_;
      const float anchorRatio = clampf((io.MousePos.x - o.x) / w, 0.0f, 1.0f);
      const float anchorTime = tlT0_ + anchorRatio * oldZoom;
      const float zoomFactor = std::pow(0.85f, io.MouseWheel);
      tlZoom_ = clampf(oldZoom * zoomFactor, 8.0f, maxView);
      tlT0_ = clampTlT0(anchorTime - anchorRatio * tlZoom_,
                        tlZoom_, contentDuration);
      tlFitZoom_ = -1.0f;
      scheduleSaveEditorState();
    }
  }

  // --- horizontal scrollbar: pan the visible window [tlT0_, tlT0_+tlZoom_]
  // over the full show/audio content duration. Geometry/mapping live in the
  // pure helpers above so the smoke can regression-test them.
  {
    const float sbH = 14.0f;
    const TlScrollGeom sg = tlScrollGeom(w, tlZoom_, contentDuration);
    ImVec2 so = ImGui::GetCursorScreenPos();
    // pin to the panel's bottom edge: in a docked window taller than the
    // content the bar would otherwise float under the strip with dead space
    const float bot = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    if (so.y + sbH < bot - 4.0f) {
      so.y = bot - sbH - 4.0f;
      ImGui::SetCursorScreenPos(so);
    }
    const ImVec2 sb1(so.x + w, so.y + sbH);
    ImGui::InvisibleButton("tl_hscroll", ImVec2(w, sbH));
    dl->AddRectFilled(so, sb1, kPanel);
    dl->AddRect(so, sb1, kLine);
    if (sg.maxT0 > 0.0f) {
      const float hx = so.x + (tlT0_ / sg.maxT0) * sg.span;
      dl->AddRectFilled(ImVec2(hx, so.y + 2), ImVec2(hx + sg.hw, sb1.y - 2),
                        kLine, 3.0f);
      dl->AddRectFilled(ImVec2(hx + 1, so.y + 3),
                        ImVec2(hx + sg.hw - 1, sb1.y - 3),
                        c32(120, 140, 170, 160), 2.0f);
      if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // drag the handle, or click anywhere on the track to jump there. The
        // hw/2 centering is the same convention as ImGui's own scrollbars
        // (the thumb centers on the click point).
        const float mx = io.MousePos.x - so.x - sg.hw * 0.5f;
        tlT0_ = tlScrollValue(mx, sg, tlZoom_, contentDuration);
        tlFitZoom_ = -1.0f;  // manual scroll leaves fit mode too
      }
      // the pan sticks per-show: debounced save when the drag releases
      if (ImGui::IsItemDeactivated()) scheduleSaveEditorState();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("scroll: %.1f - %.1f s", tlT0_, tlT0_ + tlZoom_);
    }
  }

  // drag-drop target: a browser/history row dropped here re-dispatches via
  // the same route as an OS drop at the cursor (strip loads audio, lanes
  // steer it, scripts switch anywhere)
  panelDragDropTarget(DropPanel::Timeline);
  ImGui::End();
}

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------
void DemoEditor::pushConsole(const std::string& line) {
  int lvl = 2;
  if (line.rfind("[ERR]", 0) == 0) lvl = 0;
  else if (line.rfind("[WRN]", 0) == 0) lvl = 1;
  else if (line.rfind("[DBG]", 0) == 0) lvl = 3;
  console_.push_back({lvl, line});
  if (console_.size() > 2500) {
    console_.erase(console_.begin(), console_.begin() + (console_.size() - 2500));
  }
}

void DemoEditor::drawConsole() {
  ImGui::Begin("Console", &showConsole_);
  recordPanelRect(DropPanel::Console);  // OS-drop target: shader scratch
  const ImVec2 cwPos = ImGui::GetWindowPos();
  const ImVec2 cwSz = ImGui::GetWindowSize();

  // right-click anywhere in the Console (window OR its log child below)
  // opens the OS drop history; the filter box is excluded because ImGui's
  // InputText already owns right-click there (its edit menu)
  const char* levelNames[] = {"errors", "warn+", "info+", "all"};
  const bool overFilter = [&] {
    ImGui::SetNextItemWidth(80);
    ImGui::Combo("##lvlf", &levelFilter_, levelNames, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##flt", "filter...", filter_, sizeof filter_);
    return ImGui::IsItemHovered();
  }();
  const bool wantDropHistory =
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !overFilter;
  ImGui::SameLine();
  if (ImGui::Button("Clear")) console_.clear();
  ImGui::SameLine();
  ImGui::Checkbox("follow", &consoleFollow_);
  ImGui::Separator();
  // exact-zone drop targeting: the header/tools row (scratch view) vs the log
  // list below it (filter to the shader) - the child's rect is the log zone,
  // everything above it in the window is the header zone
  const float hdrBottom = ImGui::GetCursorScreenPos().y;  // top of the log child
  ImGui::BeginChild("log", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
  {
    const ImVec2 lp = ImGui::GetWindowPos();
    const ImVec2 ls = ImGui::GetWindowSize();
    recordSubRect(DropSub::ConsoleLog, lp.x, lp.y, lp.x + ls.x, lp.y + ls.y);
    recordSubRect(DropSub::ConsoleHeader, cwPos.x, cwPos.y,
                  cwPos.x + cwSz.x, hdrBottom);
  }
  const std::string f = filter_;
  for (const auto& ln : console_) {
    if (ln.level > levelFilter_) continue;
    if (!f.empty() && ln.text.find(f) == std::string::npos) continue;
    const ImU32 col = ln.level == 0 ? kDanger : ln.level == 1 ? kAmber
                      : ln.level == 2 ? kDim : kFaint;
    ImGui::TextColored(ImVec4((col >> IM_COL32_R_SHIFT) / 255.0f,
                              (col >> IM_COL32_G_SHIFT) / 255.0f,
                              (col >> IM_COL32_B_SHIFT) / 255.0f, 1.0f),
                       "%s", ln.text.c_str());
  }
  if (consoleFollow_ && !console_.empty()) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();

  // OS drop history: every drop this session with its routed panel + outcome,
  // newest first; clicking a record re-runs the action (a rejected record
  // falls back to the file kind's canonical action so the re-run applies)
  // the popup opens from the right-click above, or from View > OS Drop
  // History - where the flag is a ONE-SHOT pulse: opened + consumed on the
  // same frame, so the popup can be dismissed normally afterwards (ESC /
  // outside click / activating a row) instead of being re-opened forever
  if (wantDropHistory) ImGui::OpenPopup("##dropHistory");
  else if (dropHistoryOpen_) {
    ImGui::OpenPopup("##dropHistory");
    dropHistoryOpen_ = false;
  }
  const bool historyOpen = ImGui::BeginPopup("##dropHistory");
  if (historyOpen) {
    if (dropHistory_.empty()) {
      ImGui::TextDisabled("no OS drops this session");
    } else {
      ImGui::TextDisabled("OS drop history (%d, newest first)",
                         (int)dropHistory_.size());
      ImGui::Separator();
      for (int i = (int)dropHistory_.size() - 1; i >= 0; --i) {
        const DropRecord& rec = dropHistory_[(size_t)i];
        const bool isMarker = rec.panel < 0;  // synthetic session-resume marker
        const char* zone = isMarker ? "session" : "viewport";
        if (rec.panel == (int)DropPanel::Timeline)
          zone = rec.sub == (int)DropSub::TimelineStrip ? "timeline/strip"
                                                        : "timeline/lanes";
        else if (rec.panel == (int)DropPanel::Console)
          zone = rec.sub == (int)DropSub::ConsoleLog ? "console/log"
                                                     : "console/header";
        else if (rec.panel == (int)DropPanel::Assets) zone = "assets";
        char time[16];
        const std::time_t tt = (std::time_t)rec.t;
        std::strftime(time, sizeof time, "%H:%M:%S", std::localtime(&tt));
        char label[460];
        if (isMarker) {  // no file/zone: "14:32:07  session resumed from …"
          std::snprintf(label, sizeof label, "%s  %s", time,
                        rec.outcome.c_str());
        } else {
          std::snprintf(label, sizeof label, "%s  %s  [%s]  %s", time,
                        rec.file.c_str(), zone, rec.outcome.c_str());
        }
        // row color: green applied/loading, amber steered/ignored, red failed,
        // blue session markers
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(
                                  dropOutcomeColor(rec.outcome)));
        const bool clicked = ImGui::MenuItem(label);
        ImGui::PopStyleColor();
        // hover: the full path + the exact wall-clock time it was dropped
        // (markers have no path - nothing to show)
        if (ImGui::IsItemHovered() && !rec.path.empty()) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(rec.path.c_str());
          ImGui::TextDisabled("dropped at %s", time);
          ImGui::EndTooltip();
        }
        // real drops re-run on click and drag as a re-dispatch source;
        // markers are display-only
        if (clicked && !isMarker) rerunDrop(rec);
        if (!isMarker) {
          // drag the row onto any panel to re-dispatch it elsewhere (retry a
          // misdropped file on the right panel): the SAME payload the browser
          // rows carry, so the viewport, Sprite nodes and the panel drop
          // targets all accept it. A press-drag never fires the click re-run
          // above (ImGui only activates MenuItem on a release without drag).
          if (ImGui::BeginDragDropSource()) {
            BrowseDragPayload d{};
            d.kind = kindForPath(rec.path);
            std::strncpy(d.path, rec.path.c_str(), sizeof(d.path) - 1);
            d.path[sizeof(d.path) - 1] = '\0';
            ImGui::SetDragDropPayload(kBrowseDragType, &d, sizeof(d));
            ImGui::TextUnformatted(rec.file.c_str());
            ImGui::EndDragDropSource();
          }
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Clear history")) {
        dropHistory_.clear();
        saveEditorState();  // the cleared state must stick across restarts
      }
    }
    ImGui::EndPopup();
  }
  // drag-drop target: a browser/history row dropped here re-dispatches via
  // the OS-drop console route (header -> scratch view, log list -> filter)
  panelDragDropTarget(DropPanel::Console);
  ImGui::End();
}

// ---------------------------------------------------------------------------
// assets
// ---------------------------------------------------------------------------
void DemoEditor::drawAssets() {
  ImGui::Begin("Assets", &showAssets_);
  recordPanelRect(DropPanel::Assets);  // OS-drop target: browse the file's folder
  if (ImGui::Button("Refresh")) {
    // nothing cached yet - simply re-scan below
  }
  ImGui::SameLine();
  if (ImGui::Button("+ New Asset")) openAssetDialog();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", selAsset_.empty() ? "select a file" : selAsset_.c_str());
  ImGui::Separator();

  struct Root {
    const char* label;
    std::string path;
    ImU32 color;
  };
  const Root roots[] = {
      {"shaders", w_.shaderDir, kPhosphor},
      {"assets", w_.assetDir, kAmber},
      {"data", w_.dataDir, kBlue},
  };

  // The shared Open Asset popup already exposes drag payloads, but the docked
  // Assets panel used to make fragment shader rows selectable only. Keep the
  // payload identical so every existing viewport/panel drop target accepts a
  // shader dragged directly from this list.
  const auto dragFragmentShader = [](const std::string& full,
                                     const char* label) {
    if (std::filesystem::path(full).extension().string() != ".frag") return;
    if (!ImGui::BeginDragDropSource()) return;
    BrowseDragPayload d{};
    d.kind = (int)BrowseKind::Shader;
    std::strncpy(d.path, full.c_str(), sizeof(d.path) - 1);
    d.path[sizeof(d.path) - 1] = '\0';
    ImGui::SetDragDropPayload(kBrowseDragType, &d, sizeof(d));
    ImGui::TextUnformatted(label);
    ImGui::EndDragDropSource();
  };

  for (const Root& root : roots) {
    if (root.path.empty() || !std::filesystem::is_directory(root.path)) continue;
    if (ImGui::CollapsingHeader(root.label, ImGuiTreeNodeFlags_DefaultOpen)) {
      std::error_code ec;
      // one level of subdirectories, then files
      for (const auto& sub : std::filesystem::directory_iterator(root.path, ec)) {
        if (ec) break;
        if (!sub.is_directory()) continue;
        const std::string subName = sub.path().filename().string();
        if (subName[0] == '.') continue;
        if (ImGui::TreeNode(subName.c_str())) {
          std::error_code ec2;
          for (const auto& f : std::filesystem::directory_iterator(sub.path(), ec2)) {
            if (ec2) break;
            if (f.is_directory()) continue;
            const std::string nm = f.path().filename().string();
            if (nm[0] == '.') continue;
            const std::string full = f.path().string();
            if (ImGui::Selectable(nm.c_str(), selAsset_ == full)) selAsset_ = full;
            dragFragmentShader(full, nm.c_str());
          }
          ImGui::TreePop();
        }
      }
      // root-level files
      std::error_code ec3;
      for (const auto& f : std::filesystem::directory_iterator(root.path, ec3)) {
        if (ec3) break;
        if (f.is_directory()) continue;
        const std::string nm = f.path().filename().string();
        if (nm[0] == '.') continue;
        const std::string full = f.path().string();
        if (ImGui::Selectable(nm.c_str(), selAsset_ == full)) selAsset_ = full;
        dragFragmentShader(full, nm.c_str());
      }
    }
  }
  // drag-drop target: a browser/history row dropped here browses the file's
  // folder (or explores a dropped folder) - the OS-drop assets route
  panelDragDropTarget(DropPanel::Assets);
  ImGui::End();
}

// ---------------------------------------------------------------------------
// profiler
// ---------------------------------------------------------------------------
void DemoEditor::drawProfiler() {
  ImGui::Begin("Profiler", &showProfiler_);

  ImGui::Text("fps");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.37f, 0.94f, 0.78f, 1.0f), "%.1f", fps_);
  ImGui::SameLine();
  ImGui::Text(" frame");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.37f, 0.94f, 0.78f, 1.0f), "%.2f ms", frameMs_);
  if (w_.r) {
    ImGui::SameLine();
    ImGui::TextDisabled("engine EMA %.2f ms  res %.0f%%", w_.r->emaMs,
                        w_.r->quality.scale * 100.0f);
  }

  // frame-time sparkline
  const ImVec2 sz = ImGui::GetContentRegionAvail();
  const float graphH = std::min(sz.y * 0.30f, 110.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("hist", ImVec2(sz.x, graphH));
  const ImVec2 p1(p0.x + sz.x, p0.y + graphH);
  dl->AddRectFilled(p0, p1, kPanel);
  dl->AddRect(p0, p1, kLine);
  if (!hist_.empty()) {
    float mx = 0;
    for (float v : hist_) mx = std::max(mx, v);
    mx = std::max(mx, 33.3f);  // 30 fps floor so the 60fps line is visible
    const float step = sz.x / (float)hist_.size();
    // 16.7ms target line
    const float y60 = p1.y - (16.7f / mx) * graphH;
    dl->AddLine(ImVec2(p0.x, y60), ImVec2(p1.x, y60), c32f(0.37f, 0.94f, 0.78f, 0.35f));
    dl->AddText(ImVec2(p0.x + 4, y60 - 14), c32f(0.37f, 0.94f, 0.78f, 0.5f), "60fps");
    ImVec2 prev(0, 0);
    for (size_t i = 0; i < hist_.size(); i++) {
      const float x = p0.x + i * step;
      const float y = p1.y - (hist_[i] / mx) * graphH;
      if (i > 0) dl->AddLine(prev, ImVec2(x, y), kHot);
      prev = ImVec2(x, y);
    }
  }

  // per-effect GPU time
  ImGui::SeparatorText("Active effects (GPU)");
  const std::vector<std::string> active = w_.app->activeEffects();
  for (const std::string& name : active) {
    Effect* e = w_.app->findEffect(name);
    if (!e) continue;
    const PerfSample ps = e->perfSample();
    if (ps.frames > 0) {
      ImGui::Text("  %-28s %5.2f ms", name.c_str(), ps.medianMs);
    } else {
      ImGui::TextDisabled("  %s", name.c_str());
    }
  }
  if (active.empty()) ImGui::TextDisabled("  (no effects on stage)");
  if (w_.postfx) {
    const double ema = w_.postfx->emaMs();
    if (ema > 0) {
      ImGui::SeparatorText("Post stack");
      ImGui::Text("  %-28s %5.2f ms", "post", ema);
    }
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// viewport input forwarding: fly camera
// ---------------------------------------------------------------------------
void DemoEditor::glfwFlyCursorPosCallback(GLFWwindow* window, double x, double y) {
  DemoEditor* editor = g_flyCursorTarget;
  if (editor && editor->w_.window == window && editor->flyActive_ &&
      editor->flyMousePrimed_) {
    editor->flyMouseDx_ += x - editor->flyMouseLastX_;
    editor->flyMouseDy_ += y - editor->flyMouseLastY_;
    editor->flyMouseLastX_ = x;
    editor->flyMouseLastY_ = y;
  }
  // Keep Dear ImGui's installed callback alive; it still needs the events when
  // fly mode exits and the editor resumes normal mouse interaction.
  if (editor && editor->previousCursorPos_ &&
      editor->previousCursorPos_ != &DemoEditor::glfwFlyCursorPosCallback) {
    editor->previousCursorPos_(window, x, y);
  }
}

void DemoEditor::enterFly() {
  flyActive_ = true;
  flyMouseDx_ = flyMouseDy_ = 0.0;
  flyMousePrimed_ = false;
  Camera* cam = w_.camera;
  if (cam) {
    flyPos_ = cam->pos;
    // seed the look angles from the current orientation (fwd is valid after
    // the last update()); conventions match applyFlyCamera below
    const V3 f = cam->fwd;
    flyYaw_ = std::atan2(f[0], f[2]);
    flyPitch_ = std::asin(clampf(f[1], -1.0f, 1.0f));
    // a clean start: no residual show shake / kicks fighting the fly camera
    cam->shakeAmp = 0;
    cam->fovKick = 0;
    cam->crashKick = 0;
    cam->handheld = 0;
  }
  flySpacePrev_ = glfwGetKey(w_.window, GLFW_KEY_SPACE) == GLFW_PRESS;
  // capture the mouse (GLFW disabled mode gives relative cursor callbacks -
  // reliable even when glfwGetCursorPos() remains fixed at the lock point)
  // and blind ImGui to the mouse while flying.
  glfwSetInputMode(w_.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  glfwGetCursorPos(w_.window, &flyMouseLastX_, &flyMouseLastY_);
  flyMousePrimed_ = true;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
  Log::info("EDITOR", "fly camera: WASD move, Shift fast, Q/E up/down, RMB to release");
}

void DemoEditor::exitFly() {
  if (!flyActive_) return;
  flyActive_ = false;
  flyMouseDx_ = flyMouseDy_ = 0.0;
  flyMousePrimed_ = false;
  glfwSetInputMode(w_.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
}

void DemoEditor::toggleFly() {
  if (flyActive_) {
    exitFly();
  } else {
    flyByRmb_ = false;         // menu entry: stays until Escape (no RMB to release)
    viewportHovered_ = true;   // don't require hovering first
    enterFly();
  }
}

void DemoEditor::applyViewportFocus() {
  if (!w_.camera) return;
  w_.camera->pos = viewportFocusPos_;
  w_.camera->quat = viewportFocusQuat_;
  w_.camera->update(0.0f);
}

void DemoEditor::frameViewportSelection(bool allVisible) {
  if (!w_.app || !w_.camera) return;
  SceneGraph& graph = w_.app->editableScene();
  V3 minP{0, 0, 0}, maxP{0, 0, 0};
  bool found = false;
  auto include = [&](SceneNode* node) {
    if (!node || node == graph.root() || !node->isActive()) return;
    const V3 p{node->world[12], node->world[13], node->world[14]};
    if (!found) minP = maxP = p;
    else {
      for (int i = 0; i < 3; ++i) {
        minP[i] = std::min(minP[i], p[i]);
        maxP[i] = std::max(maxP[i], p[i]);
      }
    }
    found = true;
  };
  if (!allVisible && selNode_) include(selNode_);
  if (allVisible || !found) graph.walk(include);
  if (!found) {
    showToast("viewport: nothing visible to frame", 1);
    return;
  }
  const V3 center = vScale(vAdd(minP, maxP), 0.5f);
  const float radius = std::max(0.75f, vLen(vSub(maxP, minP)) * 0.5f);
  V3 fwd = vNorm(w_.camera->fwd);
  if (vLen(fwd) < 0.01f) fwd = V3{0, 0, -1};
  viewportFocusPos_ = vSub(center, vScale(fwd, std::max(2.0f, radius * 2.8f)));
  viewportFocusQuat_ = quatFromLookAt(vSub(center, viewportFocusPos_), V3{0, 1, 0});
  viewportFocusLock_ = true;
  applyViewportFocus();
  showToast(allVisible ? "viewport: framed visible scene" : "viewport: framed selection", 2);
}

/** poll raw GLFW state each frame: engage/disengage fly, mouse-look, WASD move.
 *  Called before the engine step; the accumulated state is applied to the
 *  engine Camera in applyFlyCamera() after the show camera runs. */
void DemoEditor::pollViewportInput(float dt) {
  GLFWwindow* win = w_.window;
  if (!win || !w_.camera) return;
  const bool rmb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

  if (!flyActive_) {
    if (rmb && viewportHovered_) {
      flyByRmb_ = true;  // RMB entry: release hands the mouse back
      enterFly();
    }
    return;
  }

  // hand the mouse back to ImGui: RMB released (RMB entry only), Escape, or
  // the window lost focus (GLFW already restored the cursor; we must clear
  // the NoMouse flag or ImGui would stay mouse-blind - a release while
  // unfocused would otherwise never be seen by glfwGetMouseButton)
  const bool esc = glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS;
  const bool focused = glfwGetWindowAttrib(win, GLFW_FOCUSED) == GLFW_TRUE;
  if (esc || (flyByRmb_ && !rmb) || !focused) {
    exitFly();
    return;
  }

  // mouse look: consume relative motion from the GLFW cursor callback. This
  // avoids depending on absolute cursor coordinates, which can stop changing
  // while GLFW_CURSOR_DISABLED is active on some systems.
  const double mouseDx = flyMouseDx_;
  const double mouseDy = flyMouseDy_;
  flyMouseDx_ = flyMouseDy_ = 0.0;
  if (flyMousePrimed_) {
    constexpr float kSens = 0.0022f;
    // Keep the view's movement aligned with the viewport drag: right/left and
    // up/down gestures now follow the cursor instead of feeling inverted.
    // (The yaw/pitch signs are opposite to the raw cursor axes because the
    // camera looks through the viewport rather than dragging the scene plane.)
    flyYaw_ -= (float)mouseDx * kSens;
    flyPitch_ = clampf(flyPitch_ + (float)mouseDy * kSens, -1.55f, 1.55f);
  }

  // movement (raw GLFW state - ImGui never sees these while flying)
  const bool wKey = glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS;
  const bool sKey = glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS;
  const bool aKey = glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS;
  const bool dKey = glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS;
  const bool qKey = glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS;
  const bool eKey = glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS;
  const bool boost = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
  const bool slow = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

  const float speed = flySpeed_ * (boost ? 4.0f : slow ? 0.25f : 1.0f) * dt;
  const float cy = std::cos(flyPitch_);
  const V3 fwd{(float)(std::sin(flyYaw_) * cy), (float)std::sin(flyPitch_),
               (float)(std::cos(flyYaw_) * cy)};
  const V3 right = vNorm(vCross(fwd, V3{0, 1, 0}));
  if (wKey) flyPos_ = vAdd(flyPos_, vScale(fwd, speed));
  if (sKey) flyPos_ = vSub(flyPos_, vScale(fwd, speed));
  if (dKey) flyPos_ = vAdd(flyPos_, vScale(right, speed));
  if (aKey) flyPos_ = vSub(flyPos_, vScale(right, speed));
  if (qKey) flyPos_[1] -= speed;  // world-space down / up
  if (eKey) flyPos_[1] += speed;

  // mouse wheel adjusts speed
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel != 0) flySpeed_ = clampf(flySpeed_ * (wheel > 0 ? 1.25f : 0.8f), 0.1f, 500.0f);

  // keep Space as a pause toggle while flying (edge-triggered on raw state)
  const bool sp = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
  if (sp && !flySpacePrev_ && w_.director) togglePlayback();
  flySpacePrev_ = sp;
}

/** write the fly pose into the engine Camera (after the show camera applied,
 *  before render, so the UBO/view reflect it). */
void DemoEditor::applyFlyCamera(float dt) {
  (void)dt;
  Camera* cam = w_.camera;
  if (!cam) return;
  const float cy = std::cos(flyPitch_);
  const V3 fwd{(float)(std::sin(flyYaw_) * cy), (float)std::sin(flyPitch_),
               (float)(std::cos(flyYaw_) * cy)};
  cam->pos = flyPos_;
  cam->quat = quatFromLookAt(fwd, V3{0, 1, 0});
  cam->update(0.0f);  // rebuild right/up/fwd basis + view from the fly pose
}

// ---------------------------------------------------------------------------
// misc helpers
// ---------------------------------------------------------------------------
void DemoEditor::toggleFullscreenPreview() {
  fullscreenPreview_ = !fullscreenPreview_;
  if (fullscreenPreview_) {
    savedToolbar_ = showToolbar_; savedHierarchy_ = showHierarchy_;
    savedInspector_ = showInspector_; savedTimeline_ = showTimeline_;
    savedConsole_ = showConsole_; savedAssets_ = showAssets_; savedProfiler_ = showProfiler_;
    showToolbar_ = showHierarchy_ = showInspector_ = showTimeline_ =
        showConsole_ = showAssets_ = showProfiler_ = false;
  } else {
    showToolbar_ = savedToolbar_; showHierarchy_ = savedHierarchy_;
    showInspector_ = savedInspector_; showTimeline_ = savedTimeline_;
    showConsole_ = savedConsole_; showAssets_ = savedAssets_; showProfiler_ = savedProfiler_;
  }
}

void DemoEditor::seekToRaw(float t) {
  t = std::max(0.0f, t);
  if (w_.director) w_.director->show = t;
  if (w_.timeline) w_.timeline->advance(t);
  if (w_.app) w_.app->seek(t);
  if (w_.audio) w_.audio->seekTrack(t);  // scrubbing re-syncs the music too
  // The actual redraw is deferred until all panels finish processing input so
  // a drag that hits ruler, lanes, and strip cannot render the engine several
  // times in one ImGui frame.
  seekPreviewPending_ = true;
}

void DemoEditor::refreshSeekPreview() {
  if (!seekPreviewPending_) return;
  seekPreviewPending_ = false;

  const float show = w_.director ? w_.director->show : 0.0f;
  // seek() has already queued crossed events; update(0) consumes those events,
  // rebuilds the scene graph/camera state, and advances video mailboxes without
  // moving the playhead further.
  if (w_.timeline) w_.timeline->advance(show);
  if (w_.app) w_.app->update(show, 0.0f);
  if (w_.camera) w_.camera->update(0.0f);
  if (flyActive_) applyFlyCamera(0.0f);
  else if (viewportFocusLock_) applyViewportFocus();
  if (w_.app) w_.app->render();
  captureViewport();
}

float DemoEditor::timelineContentDuration() const {
  const float showDuration = w_.app ? w_.app->editor().duration : 0.0f;
  const float audioDuration =
      w_.audio && w_.audio->trackMode ? w_.audio->trackDuration : 0.0f;
  return std::max(showDuration, audioDuration);
}

void DemoEditor::fitTimeline() {
  // F / the Fit button: whole show and loaded audio at a glance, press again to zoom back
  // into where you were. Any manual zoom/scroll clears the saved toggle, so
  // the next F re-fits from the current view instead of restoring stale data.
  const float dur = timelineContentDuration();
  tlFitApply(dur, tlZoom_, tlT0_, tlFitZoom_, tlFitT0_);
  // the show may have changed since the view was saved (script reload,
  // header duration edit): re-clamp the restored window into range
  tlT0_ = clampTlT0(tlT0_, tlZoom_, dur);
  Log::info("EDITOR", tlFitZoom_ >= 0.0f
                ? "timeline fit: whole show (F / Fit to restore)"
                : "timeline view restored");
  scheduleSaveEditorState();  // the fitted (or restored) window sticks
}

std::string DemoEditor::showKey() const {
  // absolute + forward-slash form so the JSON key is stable regardless of the
  // cwd at launch and never carries backslash escapes into the file
  if (!w_.app) return "";
  return std::filesystem::absolute(w_.app->scriptPath())
      .lexically_normal()
      .generic_string();
}

bool DemoEditor::applyTimelineViewForShow(const std::string& key) {
  // A video editor opens a project with the whole production visible. Once
  // the user zooms or pans, the per-project view is restored on the next
  // launch/switch instead of losing that authoring context.
  if (key.empty()) return false;
  const float dur = timelineContentDuration();
  const auto fitWholeShow = [&] {
    // Keep the current zoom as the Restore target, unless we were already in
    // fit mode (then tlFitZoom_ is the user's previous zoomed view).
    if (tlFitZoom_ < 0.0f) {
      tlFitZoom_ = tlZoom_;
      tlFitT0_ = tlT0_;
    }
    tlZoom_ = std::max(dur, 8.0f);
    tlT0_ = 0.0f;
  };
  if (!std::filesystem::exists(editorStatePath())) {
    fitWholeShow();
    return false;
  }
  try {
    const Value vv =
        Json::parseFile(editorStatePath()).get("timelineViews").get(key);
    if (vv.isNull()) {
      fitWholeShow();
      return false;
    }
    tlZoom_ = std::max(8.0f, vv.get("zoom").asFloat(tlZoom_));
    tlT0_ = clampTlT0(vv.get("t0").asFloat(tlT0_), tlZoom_, dur);
    tlFitZoom_ = vv.get("fitZoom").asFloat(-1.0f);
    tlFitT0_ = vv.get("fitT0").asFloat(0.0f);
    Log::info("EDITOR", "timeline view restored for " + key);
    return true;
  } catch (const std::exception& e) {
    Log::error("EDITOR", "timeline view restore failed: " +
                              std::string(e.what()));
    return false;
  }
}

void DemoEditor::switchShow(const std::string& path) {
  // persist the OLD show's view before leaving it (a debounced save could
  // still be pending), switch, then land on the NEW show's saved window
  if (!w_.app) return;
  saveEditorState();
  w_.app->editorOpenScript(path);
  // Existing per-project view is restored; a new project is automatically
  // fitted to its complete duration by applyTimelineViewForShow().
  applyTimelineViewForShow(showKey());
}

void DemoEditor::seekTo(float t) {
  // scrub quantization: snap to the nearest ALIGNED grid line so edits land on
  // the beat/bar (and on the beat-marker alignment, when one is set). The raw
  // seek keeps the show + audio in lockstep either way.
  if (quantize_) {
    const float beat = w_.timeline ? w_.timeline->beatSec() : 0.2777f;
    const float bar = beat * 4.0f;
    const float grid = quantizeGrid_ == 1 ? bar : beat;
    if (grid > 0 && t > 0) {  // t=0 stays reachable - never snap the start
      const float off = beatOffset_;
      const float snapped =
          std::floor((t - off) / grid + 0.5f) * grid + off;  // nearest line
      t = std::max(0.0f, snapped);
    }
  }
  seekToRaw(t);
}

void DemoEditor::deleteSelectedTimelineEvent() {
  if (!w_.app || selectedTimelineEvent_.empty()) return;
  int index = -1;
  if (!topLevelTimelineIndex(selectedTimelineEvent_, index)) {
    showToast("scene-derived events are edited in the Scene inspector", 1);
    return;
  }
  if (index < 0 || index >= (int)doc_.ast.main.size()) {
    selectedTimelineEvent_.clear();
    return;
  }
  doc_.beginEdit("delete timeline event");
  doc_.ast.main.erase(doc_.ast.main.begin() + index);
  doc_.endEdit();
  if (writeDocument()) {
    selectedTimelineEvent_.clear();
    showToast("timeline event deleted", 2);
  }
}

void DemoEditor::duplicateSelectedTimelineEvent() {
  if (!w_.app || selectedTimelineEvent_.empty()) return;
  int index = -1;
  if (!topLevelTimelineIndex(selectedTimelineEvent_, index)) {
    showToast("scene-derived events are duplicated from the Scene inspector", 1);
    return;
  }
  if (index < 0 || index >= (int)doc_.ast.main.size()) {
    selectedTimelineEvent_.clear();
    return;
  }
  const float delta = w_.timeline ? std::max(0.25f, w_.timeline->barSec()) : 1.0f;
  ScriptBlock copy = doc_.ast.main[(size_t)index];
  copy.time += delta;
  doc_.beginEdit("duplicate timeline event");
  doc_.ast.main.push_back(std::move(copy));
  if (doc_.ast.duration < doc_.ast.main.back().time)
    doc_.ast.duration = doc_.ast.main.back().time;
  std::stable_sort(doc_.ast.main.begin(), doc_.ast.main.end(),
                   [](const ScriptBlock& a, const ScriptBlock& b) {
                     return a.time < b.time;
                   });
  doc_.endEdit();
  if (writeDocument()) {
    selectedTimelineEvent_.clear();
    showToast("timeline event duplicated one bar later", 2);
  }
}

void DemoEditor::createCameraFromView() {
  if (!w_.app || !w_.camera) return;
  const std::string sceneName =
      (doc_.findScene(selScene_) ? selScene_ : w_.app->activeScene());
  SceneDef* scene = doc_.findScene(sceneName);
  if (!scene) {
    showToast("select a scheduled scene before creating a camera", 1);
    return;
  }

  const V3 position = w_.camera->pos;
  const V3 direction = vNorm(w_.camera->fwd);
  const V3 target = vAdd(position, vScale(vLen(direction) > 0.01f
                                               ? direction
                                               : V3{0, 0, -1},
                                           10.0f));
  Cmd* camera = sceneCameraCommand(scene);
  doc_.beginEdit("create camera from view");
  if (!camera) {
    Cmd created;
    created.name = "camera";
    created.args.push_back(Value(scene->name + "Cam"));
    created.opts.set("rig") = Value("static");
    scene->setup.insert(scene->setup.begin(), std::move(created));
    camera = sceneCameraCommand(scene);
  }
  if (camera) {
    camera->opts.set("rig") = Value("static");
    camera->opts.set("pos") = cameraVecValue(position);
    camera->opts.set("target") = cameraVecValue(target);
    camera->opts.set("fov") = Value((double)(w_.camera->fov * 180.0f / 3.14159265f));
  }
  doc_.endEdit();
  selScene_ = sceneName;
  if (writeDocument()) {
    showToast("camera created from viewport in " + sceneName, 2);
  }
}

void DemoEditor::duplicateNode(SceneNode* n) {
  if (!n || !w_.app) return;
  SceneGraph& g = w_.app->editableScene();
  SceneNode* parent = n->parent ? n->parent : g.root();
  std::string name = n->name + "_copy";
  int k = 1;
  while (g.find(name)) name = n->name + "_copy" + std::to_string(k++);
  SceneNode* dup = g.addNode(name, n->type, n->payload, parent);
  if (!dup) return;
  dup->pos = n->pos;
  dup->rot = n->rot;
  dup->scale = n->scale;
  dup->visible = n->visible;
  dup->enabled = n->enabled;
  dup->layer = n->layer;
  dup->tags = n->tags;
  selNode_ = dup;
  selEffect_.clear();
}

void DemoEditor::deleteNode(SceneNode* n) {
  if (!n || !n->parent || !w_.app) return;
  SceneNode* p = n->parent;
  p->removeChild(n->name);
  if (selNode_ == n) selNode_ = nullptr;
}

bool DemoEditor::isEffectActive(const std::string& name) const {
  for (const auto& a : w_.app->activeEffects()) {
    if (a == name) return true;
  }
  return false;
}

std::string DemoEditor::fmtTime(float t) const {
  t = std::max(0.0f, t);
  const int m = (int)(t / 60.0f);
  const float s = t - m * 60.0f;
  char b[32];
  std::snprintf(b, sizeof b, "%d:%05.2f", m, s);
  return b;
}

std::string DemoEditor::trackLabel() const {
  // the current audio source for the TRK buttons: the track filename
  // (trimmed to keep the menu-bar right-alignment compact) or "silent" when
  // none is loaded
  const AudioEngine* a = w_.audio;
  std::string src = "silent";
  if (a && a->trackMode) {
    src = std::filesystem::path(a->trackPath()).filename().string();
    if (src.empty()) src = a->trackPath();
    if (src.size() > 22) src = src.substr(0, 19) + "...";
  }
  return src;
}

const char* DemoEditor::iconFor(NodeType t) {
  switch (t) {
    case NodeType::Camera: return "Cam";
    case NodeType::Light: return "Lgt";
    case NodeType::Mesh: return "Mesh";
    case NodeType::Particles: return "Part";
    case NodeType::Quad: return "Quad";
    case NodeType::Sprite: return "Sprt";
    case NodeType::Video: return "Video";
    case NodeType::Text: return "Text";
    case NodeType::Post: return "Post";
    case NodeType::TimelineSystem: return "Time";
    default: return "Empty";
  }
}

const char* DemoEditor::typeLabel(NodeType t) {
  switch (t) {
    case NodeType::Camera: return "Camera";
    case NodeType::Light: return "Light";
    case NodeType::Mesh: return "Mesh";
    case NodeType::Particles: return "Particle System";
    case NodeType::Quad: return "Shader Quad";
    case NodeType::Sprite: return "Sprite";
    case NodeType::Video: return "Video";
    case NodeType::Text: return "Text";
    case NodeType::Post: return "Post Effect";
    case NodeType::TimelineSystem: return "Timeline System";
    default: return "Empty";
  }
}

// ---------------------------------------------------------------------------
// audio source control (toolbar ♪ popup)
// ---------------------------------------------------------------------------
void DemoEditor::drawAudioPopup() {
  if (!ImGui::BeginPopup("Audio")) return;
  AudioEngine* a = w_.audio;
  ImGui::TextDisabled("source");
  ImGui::SameLine();
  if (a && a->trackMode) ImGui::TextUnformatted(a->trackPath().c_str());
  else ImGui::TextUnformatted("no track (silent)");
  if (a && a->trackMode) {
    ImGui::TextDisabled("duration");
    ImGui::SameLine();
    ImGui::Text("%.1f s", a->trackDuration);
    if (detectedBpm_ > 0.0f) {
      ImGui::TextDisabled("project BPM");
      ImGui::SameLine();
      ImGui::Text("%.1f", detectedBpm_);
    }
  }
  // background decode status: a spinner while the worker runs, so loading a
  // large track doesn't freeze the editor (the swap commits when ready)
  if (a && a->asyncStatus() != ns::AudioEngine::AsyncState::Idle) {
    static const char kSpin[] = "|/-\\";
    const int ph = (int)(ImGui::GetTime() * 10.0) & 3;
    ImGui::TextColored(ImVec4(0.98f, 0.77f, 0.42f, 1.0f), "%c decoding %s...",
                       kSpin[ph], a->asyncPath().c_str());
  }
  ImGui::Separator();

  rescanAudioCandidates();
  ImGui::TextDisabled("tracks on disk");
  if (audioCandidates_.empty()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDim),
                       "no .wav/.mp3 found - drop one next to the exe or in assets/");
  }
  for (const auto& c : audioCandidates_) {
    if (ImGui::Selectable(c.c_str())) applyAudioTrack(c);
  }

  ImGui::Separator();
  ImGui::TextDisabled("load a file");
  ImGui::SetNextItemWidth(240);
  ImGui::InputText("##audioPath", audioPath_, sizeof(audioPath_));
  ImGui::SameLine();
  if (ImGui::Button("Browse...")) openNativeFileDialog((int)BrowseKind::Audio);
  ImGui::SameLine();
  // an empty path must NOT be treated as "stop audio" here - that is what the
  // explicit button below is for (swapTrack("") means silence)
  if (ImGui::Button("Load") && audioPath_[0] != '\0') applyAudioTrack(audioPath_);
  if (ImGui::Button("No track (silent)")) applyAudioTrack("");
  ImGui::SameLine();
  if (ImGui::Button("Align beats to kicks")) autoAlignBeats();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDim),
                     "a new track starts at the current show time");
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDim),
                     "beat grid: drag a line in the waveform strip / click a kick tick");
  ImGui::EndPopup();
}

void DemoEditor::rescanAudioCandidates() {
  const double now = wallNow();
  if (now - audioScanT_ < 2.0) return;  // re-scan so dropped files appear
  audioScanT_ = now;
  audioCandidates_.clear();
  const char* dirs[] = {".", "assets", "data"};  // data/ = the demo folder
  for (const char* d : dirs) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
      if (ec) break;
      if (e.is_directory()) continue;
      const std::string ext = e.path().extension().string();
      if (ext == ".wav" || ext == ".mp3") audioCandidates_.push_back(e.path().string());
    }
  }
  std::sort(audioCandidates_.begin(), audioCandidates_.end());
}

// ---------------------------------------------------------------------------
// shared asset browser: one "Open Asset" popup, per-kind roots/exts/actions.
// Every kind remembers its own scan root + last pick (browsers.<key> in
// editor_state.json), so each picker reopens where you were.
// ---------------------------------------------------------------------------
namespace {
struct BrowseKindDef {
  const char* key;     // editor_state.json key (browsers.<key>)
  const char* hint;    // default-root hint under the root input
  const char* action;  // Open/Load button label
  const char* exts[5]; // extension filter
  int extCount;
};
// the modal title is a constant so the popup id never changes when the kind
// combo switches categories (OpenPopup id must equal BeginPopup id)
const char* const kBrowseTitle = "Open Asset";
const char* const kScratchTitle = "Shader Scratch";
const BrowseKindDef kBrowseKinds[] = {
    {"audio", "empty root lists assets/, data/ and the exe dir", "Load track",
     {".wav", ".mp3", ".ogg", ".flac"}, 4},
    {"texture", "empty root lists data/textures", "Open",
     {".png", ".jpg", ".jpeg", ".tga", ".bmp"}, 5},
    {"shader", "empty root lists shaders/, data/shaders and data/shadertoy", "Show",
     {".frag", ".vert", ".glsl"}, 3},
    {"model", "empty root lists data/models", "Load", {".obj", ".glb"}, 2},
    {"script", "empty root lists data/", "Open", {".nsd"}, 1},
    {"video", "empty root lists data/video", "Add to scene", {".mp4", ".mkv", ".webm", ".mov"}, 4},
};
}  // namespace

std::string DemoEditor::browseRelPath(const std::string& base, const std::string& file) {
  // normalize both sides (absolute + generic forward-slash form) so the pick
  // actions resolve relative names even when the scan produced native Windows
  // paths (backslashes) or the base came from a different separator style
  const std::string norm =
      std::filesystem::absolute(file).lexically_normal().generic_string();
  const std::string root =
      std::filesystem::absolute(base).lexically_normal().generic_string();
  if (norm.rfind(root, 0) != 0 || norm.size() <= root.size() ||
      norm[root.size()] != '/') {
    return "";  // not under BASE (or exactly BASE itself)
  }
  return norm.substr(root.size() + 1);
}

std::vector<std::string> DemoEditor::browseRoots(int kind) const {
  std::vector<std::string> roots;
  switch ((BrowseKind)kind) {
    case BrowseKind::Audio:   roots = {"assets", "data", "."}; break;
    case BrowseKind::Texture: roots = {w_.dataDir + "/textures"}; break;
    case BrowseKind::Shader:  roots = {w_.shaderDir, w_.dataDir + "/shaders",
                                       w_.dataDir + "/shadertoy"}; break;
    case BrowseKind::Model:   roots = {w_.dataDir + "/models",
                                       w_.assetDir + "/models"}; break;
    case BrowseKind::Script:  roots = {w_.dataDir}; break;
    case BrowseKind::Video:   roots = {w_.dataDir + "/video", w_.assetDir + "/video"}; break;
    default: break;
  }
  return roots;
}

bool DemoEditor::openNativeFileDialog(int kind) {
  if (kind < 0 || kind >= (int)BrowseKind::Count) return false;
  browseKind_ = kind;

#ifdef _WIN32
  // Use the native Windows picker for menu and Browse... actions. The
  // in-editor recursive browser remains available as a fallback and for
  // dropped folders, but it is not a substitute for navigating arbitrary
  // filesystem locations.
  static const wchar_t kAudioFilter[] =
      L"Audio files\0*.wav;*.mp3;*.ogg;*.flac\0All files\0*.*\0\0";
  static const wchar_t kTextureFilter[] =
      L"Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All files\0*.*\0\0";
  static const wchar_t kShaderFilter[] =
      L"Shaders\0*.frag;*.vert;*.glsl\0All files\0*.*\0\0";
  static const wchar_t kModelFilter[] =
      L"Models\0*.obj;*.glb\0All files\0*.*\0\0";
  static const wchar_t kScriptFilter[] =
      L"Null Sector scripts\0*.nsd\0All files\0*.*\0\0";
  static const wchar_t kVideoFilter[] =
      L"Video files\0*.mp4;*.mkv;*.webm;*.mov\0All files\0*.*\0\0";
  const wchar_t* filter = kAudioFilter;
  switch ((BrowseKind)kind) {
    case BrowseKind::Texture: filter = kTextureFilter; break;
    case BrowseKind::Shader:  filter = kShaderFilter; break;
    case BrowseKind::Model:   filter = kModelFilter; break;
    case BrowseKind::Script:  filter = kScriptFilter; break;
    case BrowseKind::Video:   filter = kVideoFilter; break;
    default: break;
  }

  wchar_t fileName[32768] = {};
  AssetBrowse& b = browse_[kind];
  std::wstring initialDir;
  if (b.root[0] != '\0') {
    initialDir = std::filesystem::path(b.root).wstring();
  } else {
    const auto roots = browseRoots(kind);
    if (!roots.empty()) initialDir = std::filesystem::absolute(roots.front()).wstring();
  }

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = w_.window ? glfwGetWin32Window(w_.window) : nullptr;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = (DWORD)(sizeof(fileName) / sizeof(fileName[0]));
  ofn.lpstrFilter = filter;
  ofn.nFilterIndex = 1;
  ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_HIDEREADONLY;

  if (GetOpenFileNameW(&ofn)) {
    const std::filesystem::path selected{std::wstring(fileName)};
    const std::string path = selected.string();
    b.root[0] = '\0';
    const std::string parent = selected.parent_path().string();
    std::strncpy(b.root, parent.c_str(), sizeof(b.root) - 1);
    b.root[sizeof(b.root) - 1] = '\0';
    b.sel = path;
    b.scanned = false;
    pickBrowseFile(kind, path);
    saveEditorState();
    return true;
  }
  if (CommDlgExtendedError() != 0) {
    Log::warn("EDITOR", "native file dialog failed; using the in-editor browser");
    openBrowse(kind);
    return false;
  }
  return false;  // user cancelled; leave the editor state unchanged
#else
  // Keep the existing cross-platform browser on non-Windows builds. It is
  // still useful in headless/dev environments where no native picker exists.
  openBrowse(kind);
  return true;
#endif
}

void DemoEditor::saveDocumentAsDialog() {
#ifdef _WIN32
  std::filesystem::path defaultPath = doc_.path.empty()
      ? std::filesystem::absolute("project.nsd")
      : std::filesystem::absolute(doc_.path);
  wchar_t fileName[32768] = {};
  const std::wstring defaultName = defaultPath.wstring();
  const std::wstring initialDir = defaultPath.parent_path().wstring();
  std::wcsncpy(fileName, defaultName.c_str(),
               sizeof(fileName) / sizeof(fileName[0]) - 1);
  static const wchar_t kNsdFilter[] =
      L"Null Sector scripts\0*.nsd\0All files\0*.*\0\0";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = w_.window ? glfwGetWin32Window(w_.window) : nullptr;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = (DWORD)(sizeof(fileName) / sizeof(fileName[0]));
  ofn.lpstrFilter = kNsdFilter;
  ofn.nFilterIndex = 1;
  ofn.lpstrInitialDir = initialDir.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return;
  std::string selected = std::filesystem::path(std::wstring(fileName)).string();
  std::string ext = std::filesystem::path(selected).extension().string();
  for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  if (ext != ".nsd") selected += ".nsd";
  writeDocumentAs(selected);
#else
  Log::warn("EDITOR", "Save Project As requires a native file picker on this build");
#endif
}

void DemoEditor::newProjectDialog() {
#ifdef _WIN32
  const std::filesystem::path defaultPath =
      std::filesystem::absolute(std::filesystem::path(w_.dataDir) / "NewProject.nsd");
  wchar_t fileName[32768] = {};
  const std::wstring defaultName = defaultPath.wstring();
  const std::wstring initialDir = defaultPath.parent_path().wstring();
  std::wcsncpy(fileName, defaultName.c_str(),
               sizeof(fileName) / sizeof(fileName[0]) - 1);
  static const wchar_t kNsdFilter[] =
      L"Null Sector scripts\0*.nsd\0All files\0*.*\0\0";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = w_.window ? glfwGetWin32Window(w_.window) : nullptr;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = (DWORD)(sizeof(fileName) / sizeof(fileName[0]));
  ofn.lpstrFilter = kNsdFilter;
  ofn.nFilterIndex = 1;
  ofn.lpstrInitialDir = initialDir.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return;
  std::string selected = std::filesystem::path(std::wstring(fileName)).string();
  std::string ext = std::filesystem::path(selected).extension().string();
  for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  if (ext != ".nsd") selected += ".nsd";
  if (doc_.dirty) {
    pendingNewProjectPath_ = selected;
    newProjectConfirmOpen_ = true;
  } else {
    createNewProject(selected);
  }
#else
  Log::warn("EDITOR", "New Project requires a native file picker on this build");
#endif
}

namespace {

// The distribution ZIP must ship the runtime-only engine player (ns_demo),
// never the editor build itself: the editor exe forces --editor and carries
// the full authoring layer. CMake always builds ns_demo and ns_editor into
// the same output directory, so the player is a sibling of the running
// editor exe. Returns empty when no sibling player is present.
std::string enginePlayerExecutablePath() {
  std::error_code ec;
  std::filesystem::path dir;
#ifdef _WIN32
  wchar_t path[32768] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path,
                                     (DWORD)(sizeof(path) / sizeof(path[0])));
  if (n <= 0 || n >= sizeof(path) / sizeof(path[0])) return std::string();
  dir = std::filesystem::path(std::wstring(path, n)).parent_path();
#else
  dir = std::filesystem::current_path(ec);
  if (ec) return std::string();
#endif
  const std::filesystem::path candidate =
      dir / (std::string("ns_demo") +
#ifdef _WIN32
             ".exe"
#else
             ""
#endif
  );
  if (std::filesystem::is_regular_file(candidate, ec))
    return std::filesystem::absolute(candidate, ec).string();
  return std::string();
}

}  // namespace

void DemoEditor::openPackageDialog() {
  packageHasResult_ = false;
  packageOk_ = false;
  packageMessage_.clear();
  std::string stem = w_.app ? std::filesystem::path(w_.app->scriptPath()).stem().string()
                            : "project";
  if (stem.empty()) stem = "project";
  const std::filesystem::path defaultZip =
      std::filesystem::absolute(stem + "_distribution.zip");

#ifdef _WIN32
  wchar_t fileName[32768] = {};
  const std::wstring defaultName = defaultZip.wstring();
  std::wcsncpy(fileName, defaultName.c_str(),
               sizeof(fileName) / sizeof(fileName[0]) - 1);
  static const wchar_t kZipFilter[] =
      L"ZIP distribution\0*.zip\0All files\0*.*\0\0";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = w_.window ? glfwGetWin32Window(w_.window) : nullptr;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = (DWORD)(sizeof(fileName) / sizeof(fileName[0]));
  ofn.lpstrFilter = kZipFilter;
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return;
  std::string selected = std::filesystem::path(std::wstring(fileName)).string();
  if (std::filesystem::path(selected).extension() != ".zip") selected += ".zip";
  std::strncpy(packageZipPath_, selected.c_str(), sizeof(packageZipPath_) - 1);
  packageZipPath_[sizeof(packageZipPath_) - 1] = '\0';
  packageDialogOpen_ = true;
  startPackage(packageZipPath_);
#else
  std::snprintf(packageZipPath_, sizeof(packageZipPath_), "%s",
                defaultZip.string().c_str());
  packageDialogOpen_ = true;
#endif
}

void DemoEditor::startPackage(const std::string& outputZip) {
  if (!w_.app || outputZip.empty()) return;
  std::string finalOutput = outputZip;
  std::string ext = std::filesystem::path(finalOutput).extension().string();
  for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  if (ext != ".zip") finalOutput += ".zip";
  std::strncpy(packageZipPath_, finalOutput.c_str(), sizeof(packageZipPath_) - 1);
  packageZipPath_[sizeof(packageZipPath_) - 1] = '\0';
  const std::filesystem::path data = std::filesystem::absolute(w_.dataDir);
  const std::filesystem::path root = data.parent_path();
  const std::string track =
      w_.audio && w_.audio->trackMode ? w_.audio->trackPath() : std::string();
  const std::string exe = enginePlayerExecutablePath();
  const EditorPackageResult r = packageEditorProject(
      root.string(), w_.app->scriptPath(), track, exe, finalOutput);
  packageOk_ = r.ok;
  packageHasResult_ = true;
  packageMessage_ = r.message;
  if (r.ok) {
    packageMessage_ += "\nZIP: " + r.outputZip +
                       "\nNSP: " + std::to_string(r.nspBytes / 1048576.0) +
                       " MB, archive: " + std::to_string(r.zipBytes / 1048576.0) + " MB";
    Log::info("EDITOR", "project package complete: " + r.outputZip);
  } else {
    Log::error("EDITOR", "project package failed: " + r.message);
  }
}

void DemoEditor::drawPackageDialog() {
  if (!packageDialogOpen_) return;
  ImGui::OpenPopup("Package Project");
  if (!ImGui::BeginPopupModal("Package Project", &packageDialogOpen_,
                              ImGuiWindowFlags_AlwaysAutoResize)) return;
  if (!packageHasResult_) {
    ImGui::TextUnformatted("Create a standalone project distribution");
    ImGui::TextDisabled("The package contains the NSP assets, a copied engine executable,");
    ImGui::TextDisabled("and launch.bat with the correct --play switch.");
    ImGui::InputText("Output ZIP", packageZipPath_, sizeof(packageZipPath_));
    if (ImGui::Button("Package")) startPackage(packageZipPath_);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) packageDialogOpen_ = false;
  } else {
    ImGui::TextColored(packageOk_ ? ImVec4(0.37f, 0.94f, 0.78f, 1.0f)
                                  : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                       packageOk_ ? "Package complete" : "Package failed");
    ImGui::Separator();
    ImGui::TextWrapped("%s", packageMessage_.c_str());
    if (ImGui::Button("Close")) packageDialogOpen_ = false;
  }
  ImGui::EndPopup();
}

void DemoEditor::openBrowse(int kind) {
  if (kind < 0 || kind >= (int)BrowseKind::Count) return;
  browseKind_ = kind;
  browse_[kind].scanned = false;  // re-list on open (files may have appeared)
  browseOpen_ = true;
}

void DemoEditor::openBrowseRoot(int kind, const std::string& root) {
  if (kind < 0 || kind >= (int)BrowseKind::Count) return;
  openBrowse(kind);
  AssetBrowse& b = browse_[kind];
  std::strncpy(b.root, root.c_str(), sizeof(b.root) - 1);
  b.root[sizeof(b.root) - 1] = '\0';
  // the dropped folder is a fresh context: a selection persisted from a
  // previous session points at a different folder's file (the Open button
  // would dispatch a path that no longer exists). The menu-open path keeps
  // its selection on purpose ("reopen where you were").
  b.sel.clear();
}

void DemoEditor::scanAssetBrowser(int kind) {
  const double t0 = wallNow();  // timed so the auto-refresh cadence can scale
  AssetBrowse& b = browse_[kind];
  b.files.clear();
  std::vector<std::string> roots;
  if (b.root[0] != '\0') roots.push_back(b.root);
  else roots = browseRoots(kind);
  const BrowseKindDef& kd = kBrowseKinds[kind];
  for (const auto& r : roots) {
    std::error_code ec;
    if (!std::filesystem::is_directory(r, ec)) continue;
    // skip_permission_denied: an inaccessible subfolder (e.g. System Volume
    // Information when scanning C:\) must not abort the whole crawl
    std::filesystem::recursive_directory_iterator it(
        r, std::filesystem::directory_options::skip_permission_denied, ec),
        end;
    while (it != end) {
      if (ec) {
        ec.clear();
        break;
      }
      std::error_code fec;
      if (it->is_regular_file(fec)) {
        const std::string ext = it->path().extension().string();
        for (int i = 0; i < kd.extCount; i++) {
          if (ext == kd.exts[i]) {
            // canonical-ish full path so loading works from any cwd
            b.files.push_back(std::filesystem::absolute(it->path()).string());
            break;
          }
        }
      }
      it.increment(ec);
    }
  }
  std::sort(b.files.begin(), b.files.end());
  // the selected entry may have vanished after a rescan - drop the stale pick
  if (!b.sel.empty() &&
      std::find(b.files.begin(), b.files.end(), b.sel) == b.files.end()) {
    b.sel.clear();
  }
  b.scanMs = (float)((wallNow() - t0) * 1000.0);
}

void DemoEditor::browseAutoRefresh(int kind) {
  AssetBrowse& b = browse_[kind];
  // popup-agnostic: called every frame from drawBrowse while open (and directly
  // by the audio smoke). Skips the initial scan (scanned stays false until the
  // first open); after that, re-scan when the cadence elapses. The interval
  // scales with the previous scan's cost: a small tree refreshes every ~2 s, a
  // huge one (C:\) every few seconds, so the list stays live without hitching.
  if (!b.scanned) return;
  const double now = wallNow();
  const float interval = clampf(b.scanMs * 20.0f, 2.0f, 8.0f);
  if (now - b.scanT < interval) return;
  b.scanT = now;
  const std::vector<std::string> prev = b.files;
  scanAssetBrowser(kind);
  if (b.files != prev) b.flashT = now;  // "updated" flash
}

void DemoEditor::applyTexturePick(const std::string& path, SceneNode* target) {
  if (!w_.app) return;
  // the sprite pipeline resolves textures under data/textures
  const std::string rel = browseRelPath(w_.dataDir + "/textures", path);
  if (rel.empty()) {
    Log::warn("EDITOR",
              "texture outside data/textures (the sprite pipeline needs it there): " + path);
    return;
  }
  w_.app->editorLoadTexture(rel);
  if (target && target->type == NodeType::Sprite) {
    target->asSprite()->tex = rel;  // the viewport updates immediately
  }
}

void DemoEditor::addShaderToScene(const std::string& path) {
  if (!w_.app || path.empty()) return;
  const std::filesystem::path source = std::filesystem::absolute(path).lexically_normal();
  if (!std::filesystem::is_regular_file(source)) {
    showToast("shader: file not found - " + source.filename().string(), 0);
    return;
  }

  // Keep scene references portable. A shader outside the project's canonical
  // shaders/ directory is copied into it before the command is serialized;
  // the existing live preview still uses the original path if this fails.
  const std::filesystem::path shaderRoot =
      std::filesystem::absolute(w_.shaderDir).lexically_normal();
  std::filesystem::path target = shaderRoot / source.filename();
  std::error_code ec;
  if (source.parent_path() != shaderRoot) {
    std::filesystem::create_directories(shaderRoot, ec);
    if (ec) {
      showToast("shader: cannot create project shader directory", 0);
      return;
    }
    if (std::filesystem::exists(target)) {
      const std::string stem = source.stem().string();
      const std::string ext = source.extension().string();
      for (int suffix = 2; std::filesystem::exists(target); suffix++) {
        target = shaderRoot / (stem + "_scene" + std::to_string(suffix) + ext);
      }
    }
    std::filesystem::copy_file(source, target,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
      showToast("shader: copy failed - " + source.filename().string(), 0);
      Log::error("EDITOR", "shader copy failed: " + ec.message());
      return;
    }
  }

  const std::string sceneName =
      !selScene_.empty() ? selScene_ : w_.app->activeScene();
  SceneDef* scene = sceneName.empty() ? nullptr : doc_.findScene(sceneName);
  if (!scene) {
    showToast("shader: select a scene first", 1);
    return;
  }
  const std::string shaderRef = target.filename().string();
  bool alreadyPresent = false;
  for (const auto& cmd : scene->setup) {
    if (cmd.name == "shader" && !cmd.args.empty() &&
        cmd.args[0].asStr() == shaderRef) {
      alreadyPresent = true;
      break;
    }
  }

  if (!alreadyPresent) {
    Cmd cmd;
    cmd.name = "shader";
    cmd.args.push_back(Value(shaderRef));
    doc_.beginEdit("add shader to scene");
    scene->setup.push_back(std::move(cmd));
    doc_.endEdit();
    if (!doc_.dirty || !writeDocument()) {
      showToast("shader: could not save scene", 0);
      return;
    }
  }

  const std::string preview = browseEffectName(target.string(), w_.shaderDir, w_.dataDir);
  w_.app->editorShowEffect(preview);
  showToast("shader: live in " + sceneName + " - " + shaderRef, 2);
  Log::info("EDITOR", "shader '" + shaderRef + "' " +
                           (alreadyPresent ? "previewed in " : "added to ") + sceneName);
}

void DemoEditor::addVideoToScene(const std::string& path) {
  if (!w_.app || path.empty()) return;
  const std::filesystem::path source = std::filesystem::absolute(path).lexically_normal();
  if (!std::filesystem::is_regular_file(source)) {
    showToast("video: file not found - " + source.filename().string(), 0);
    return;
  }
  const std::filesystem::path videoRoot =
      std::filesystem::absolute(w_.dataDir + "/video").lexically_normal();
  std::error_code ec;
  std::filesystem::create_directories(videoRoot, ec);
  if (ec) {
    showToast("video: cannot create data/video", 0);
    return;
  }
  std::filesystem::path target = videoRoot / source.filename();
  if (source.parent_path() != videoRoot) {
    if (std::filesystem::exists(target)) {
      const std::string stem = source.stem().string();
      const std::string ext = source.extension().string();
      for (int suffix = 2; std::filesystem::exists(target); ++suffix)
        target = videoRoot / (stem + "_scene" + std::to_string(suffix) + ext);
    }
    std::filesystem::copy_file(source, target,
                                std::filesystem::copy_options::none, ec);
    if (ec) {
      showToast("video: copy failed - " + source.filename().string(), 0);
      return;
    }
  }

  const std::string sceneName = !selScene_.empty() ? selScene_ : w_.app->activeScene();
  SceneDef* scene = sceneName.empty() ? nullptr : doc_.findScene(sceneName);
  if (!scene) {
    showToast("video: select a scene first", 1);
    return;
  }
  const std::string fileRef = target.filename().string();
  bool present = false;
  for (const auto& cmd : scene->setup) {
    if (cmd.name == "video" && !cmd.args.empty() && cmd.args[0].asStr() == fileRef) {
      present = true;
      break;
    }
  }
  if (!present) {
    Cmd cmd;
    cmd.name = "video";
    cmd.args.push_back(Value(fileRef));
    Value size = Value::array();
    size.push(Value(2.0)); size.push(Value(1.125)); size.push(Value(1.0));
    cmd.opts.set("size") = std::move(size);
    doc_.beginEdit("add video to scene");
    scene->setup.push_back(std::move(cmd));
    doc_.endEdit();
    if (!doc_.dirty || !writeDocument()) {
      showToast("video: could not save scene", 0);
      return;
    }
  }
  w_.app->reloadScript();
  showToast("video: " + fileRef + " in " + sceneName, 2);
}

void DemoEditor::pickBrowseFile(int kind, const std::string& path) {
  if (kind == (int)BrowseKind::Audio) { applyAudioTrack(path); return; }
  if (!w_.app) return;
  const std::filesystem::path p(path);
  switch ((BrowseKind)kind) {
    case BrowseKind::Texture: applyTexturePick(path, selNode_); break;
    case BrowseKind::Shader: {
      std::string ext = p.extension().string();
      for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (ext == ".glsl") {  // shadertoy source
        w_.app->editorShowEffect(browseEffectName(path, w_.shaderDir, w_.dataDir));
        showToast("shader: live preview - " + p.filename().string(), 2);
      } else if (ext == ".frag") {
        // quad effects resolve frag by name against the shader dir, so the
        // extension must survive ("quad:plasma" would miss plasma.frag)
        w_.app->editorShowEffect(browseEffectName(path, w_.shaderDir, w_.dataDir));
        showToast("shader: live preview - " + p.filename().string(), 2);
      } else {
        Log::warn("EDITOR", "vertex shaders can't run standalone as a quad yet: " + path);
      }
      break;
    }
    case BrowseKind::Model: {
      // the model cache resolves files under data/models
      const std::string rel = browseRelPath(w_.dataDir + "/models", path);
      if (!rel.empty()) w_.app->editorLoadModel(rel);
      else Log::warn("EDITOR", "model outside data/models: " + path);
      break;
    }
    case BrowseKind::Script: switchShow(path); break;
    case BrowseKind::Video: addVideoToScene(path); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// OS-level drag-in (GLFW drop): files dropped from Explorer/file managers
// ---------------------------------------------------------------------------
void DemoEditor::queueOsDrop(const char* path) {
  if (!path || !*path) return;
  OsDrop d;
  d.path = path;
  // the cursor is at the drop point while the callback runs; GLFW gives
  // content coords, drainOsDrops scales them into ImGui screen space
  if (w_.window) glfwGetCursorPos(w_.window, &d.x, &d.y);
  osDrops_.push_back(std::move(d));
}

void DemoEditor::glfwDropCallback(GLFWwindow*, int count, const char** paths) {
  if (!g_osDropTarget) return;
  for (int i = 0; i < count; ++i) g_osDropTarget->queueOsDrop(paths[i]);
}

void DemoEditor::showToast(const std::string& text, int level) {
  toasts_.push_back({text, wallNow(), level});
  while ((int)toasts_.size() > kToastRing) toasts_.erase(toasts_.begin());
}

// a dropped folder opens the browser rooted at it; the kind is inferred from
// the asset types found inside - a cheap recursive peek with an early exit
// (400 entries is plenty of evidence and bounds the cost on huge trees)
int DemoEditor::inferKindForDir(const std::string& dir) {
  int counts[(int)BrowseKind::Count] = {};
  int seen = 0;     // regular files inspected (the "evidence" bound)
  int walked = 0;   // total entries (hard walk bound - a tree of mostly
                    // directories must not run forever hunting 400 files)
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
  while (it != end && seen < 400 && walked < 2000) {
    ++walked;
    if (ec) { ec.clear(); break; }
    std::error_code fec;
    if (it->is_regular_file(fec)) {
      const std::string ext = it->path().extension().string();
      for (int k = 0; k < (int)BrowseKind::Count; ++k)
        for (int i = 0; i < kBrowseKinds[k].extCount; ++i)
          if (ext == kBrowseKinds[k].exts[i]) { ++counts[k]; break; }
      ++seen;
    }
    it.increment(ec);
  }
  int best = -1;  // first kind with the strictly-greatest count wins ties
  for (int k = 0; k < (int)BrowseKind::Count; ++k)
    if (counts[k] > 0 && (best < 0 || counts[k] > counts[best])) best = k;
  return best;
}

void DemoEditor::recordPanelRect(DropPanel p) {
  if ((int)p < 0 || (int)p >= (int)DropPanel::Count) return;
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 sz = ImGui::GetWindowSize();
  PanelRect& r = panelRects_[(int)p];
  r.x0 = pos.x; r.y0 = pos.y;
  r.x1 = pos.x + sz.x; r.y1 = pos.y + sz.y;
  r.valid = true;
}

DemoEditor::DropPanel DemoEditor::panelAt(float x, float y) const {
  // z-priority = reverse draw order (the last-drawn panel is on top); the
  // Viewport entry uses its picture rect (the established drop surface)
  for (int i = (int)DropPanel::Count - 1; i >= 0; --i) {
    const DropPanel p = (DropPanel)i;
    if (p == DropPanel::Viewport) {
      if (vpRectValid_ && x >= vpRectMinX_ && x <= vpRectMaxX_ &&
          y >= vpRectMinY_ && y <= vpRectMaxY_) {
        return p;
      }
    } else {
      const PanelRect& r = panelRects_[i];
      if (r.valid && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) return p;
    }
  }
  return DropPanel::Viewport;  // no other panel: fall back to the picture gate
}

void DemoEditor::recordSubRect(DropSub s, float x0, float y0, float x1, float y1) {
  if ((int)s < 0 || (int)s >= (int)DropSub::Count) return;
  SubRect& r = subRects_[(int)s];
  r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
  r.valid = true;
}

DemoEditor::DropSub DemoEditor::subAt(DropPanel p, float x, float y) const {
  // the panel's "rest" is its default zone (lanes / header), so every point
  // inside the panel routes to a sub-area even if the split rect was never
  // recorded (e.g. the strip collapsed or the panel was drawn without one)
  switch (p) {
    case DropPanel::Timeline: {
      const SubRect& s = subRects_[(int)DropSub::TimelineStrip];
      if (s.valid && x >= s.x0 && x <= s.x1 && y >= s.y0 && y <= s.y1)
        return DropSub::TimelineStrip;
      return DropSub::TimelineLanes;
    }
    case DropPanel::Console: {
      const SubRect& l = subRects_[(int)DropSub::ConsoleLog];
      if (l.valid && x >= l.x0 && x <= l.x1 && y >= l.y0 && y <= l.y1)
        return DropSub::ConsoleLog;
      return DropSub::ConsoleHeader;
    }
    default:
      return DropSub::TimelineLanes;  // unused for panels without sub-areas
  }
}std::string DemoEditor::routeViewportDrop(const std::string& path) {
  const std::string fn = std::filesystem::path(path).filename().string();
  // a dropped FOLDER opens the Open Asset browser rooted at it, with the
  // kind inferred from what's inside - explore an asset pack from Explorer
  // without typing a path. (A folder whose NAME looks like an asset, e.g.
  // "foo.frag", now gets browsed instead of dispatched as a file.)
  if (std::filesystem::is_directory(path)) {
    const int kind = inferKindForDir(path);
    if (kind < 0) {
      Log::warn("EDITOR", "os drop: no supported assets in folder: " + path);
      showToast("no supported assets in - " + fn, 1);
      return "no supported assets";
    }
    Log::info("EDITOR", "os drop: exploring folder as " +
                             std::string(kBrowseKinds[kind].key) + ": " + path);
    showToast("exploring: " + fn, 2);
    // absolute root, matching the persist-absolute convention (dropped
    // paths are already absolute from Explorer; relative smoke paths too)
    openBrowseRoot(kind, std::filesystem::absolute(path).string());
    return "exploring";
  }
  const int kind = kindForPath(path);
  if (kind < 0) {
    Log::warn("EDITOR", "os drop: unrecognized file type ignored: " + path);
    showToast("ignored: unknown file type - " + fn, 1);
    return "ignored: unknown type";
  }
  Log::info("EDITOR", "os drop: " + path);
  // dispatch first, then judge the outcome so the toast matches reality
  // (a shader that fails to compile must not flash "applied")
  pickBrowseFile(kind, path);
  std::string verb = "applied";
  bool ok = true;
  if (w_.app) {
    if (kind == (int)BrowseKind::Audio) {
      verb = "loading";  // the decode is async; the commit logs when ready
    } else if (kind == (int)BrowseKind::Shader) {
      // a shader that failed to compile never lands in activeEffects
      const auto& acts = w_.app->activeEffects();
      ok = std::find(acts.begin(), acts.end(),
                     browseEffectName(path, w_.shaderDir, w_.dataDir)) != acts.end();
      if (!ok) verb = "failed";
    } else if (kind == (int)BrowseKind::Script) {
      ok = w_.app->scriptPath() == path;
      if (!ok) verb = "failed";
    }
  }
  showToast(verb + ": " + fn, ok ? 2 : 0);
  return verb;
}

std::string DemoEditor::routeTimelineDrop(const std::string& path, DropSub sub) {
  const std::string fn = std::filesystem::path(path).filename().string();
  const int kind = kindForPath(path);
  if (kind == (int)BrowseKind::Script) {
    Log::info("EDITOR", "os drop on Timeline: script " + path);
    showToast("timeline: script " + fn, 2);
    switchShow(path);
    return "applied";
  } else if (kind == (int)BrowseKind::Audio) {
    if (sub == DropSub::TimelineStrip) {
      // the strip is the audio zone: load the track (async decode, the
      // previous source keeps playing until the swap is ready)
      Log::info("EDITOR", "os drop on Timeline strip: loading " + path);
      showToast("timeline: loading " + fn, 2);
      applyAudioTrack(path);
      return "loading";
    }
    // the lanes are the sequence zone: steer audio to the strip below
    Log::warn("EDITOR",
              "os drop on Timeline lanes: audio goes on the waveform strip: " + path);
    showToast("timeline: audio goes on the waveform strip - " + fn, 1);
    return "steered";
  }
  Log::warn("EDITOR", "os drop on Timeline: audio/scripts only: " + path);
  showToast("timeline: audio/scripts only - " + fn, 1);
  return "ignored: audio/scripts only";
}

std::string DemoEditor::routeConsoleDrop(const std::string& path, DropSub sub) {
  const std::string fn = std::filesystem::path(path).filename().string();
  const int kind = kindForPath(path);
  if (kind != (int)BrowseKind::Shader) {
    Log::warn("EDITOR", "os drop on Console: shaders only: " + path);
    showToast("console: shaders only - " + fn, 1);
    return "ignored: shaders only";
  }
  if (sub == DropSub::ConsoleLog) {
    // the log list is the debug zone: filter the console to the shader's own
    // lines (compile errors mention the filename) instead of previewing it -
    // drop-to-debug, the header drop stays for drop-to-preview
    const std::string base = std::filesystem::path(path).filename().string();
    std::strncpy(filter_, base.c_str(), sizeof filter_ - 1);
    filter_[sizeof filter_ - 1] = 0;
    Log::info("EDITOR", "os drop on Console log: filtered to " + base);
    showToast("console: filtered to " + base, 2);
    return "filtered";
  }
  // the header/tools zone: open the shader scratch view (live preview)
  openShaderScratch(path);
  return "scratch";
}

std::string DemoEditor::routeAssetsDrop(const std::string& path) {
  const std::string fn = std::filesystem::path(path).filename().string();
  if (std::filesystem::is_directory(path))  // explore a dropped folder
    return routeViewportDrop(path);
  const int kind = kindForPath(path);
  if (kind < 0) {
    Log::warn("EDITOR", "os drop on Assets: unrecognized: " + path);
    showToast("assets: unrecognized - " + fn, 1);
    return "ignored: unrecognized";
  }
  // open the browser rooted at the file's own folder so its neighbors are
  // visible (kind from the file, so the list actually contains it)
  const std::string parent = std::filesystem::absolute(path).parent_path().string();
  Log::info("EDITOR", "os drop on Assets: browsing near " + path);
  showToast("assets: browsing near " + fn, 2);
  openBrowseRoot(kind, parent);
  return "browsing";
}

void DemoEditor::recordDrop(const std::string& path, int panel, int sub, int kind,
                            const std::string& outcome) {
  DropRecord rec;
  rec.path = path;
  rec.file = std::filesystem::path(path).filename().string();
  rec.panel = panel;
  rec.sub = sub;
  rec.kind = kind;
  rec.outcome = outcome;
  rec.t = wallClockNow();  // wall-clock: survives restarts + renders via strftime
  dropHistory_.push_back(std::move(rec));
  if (dropHistory_.size() > (size_t)kDropHistoryCap)
    dropHistory_.erase(dropHistory_.begin(),
                       dropHistory_.begin() + (dropHistory_.size() - kDropHistoryCap));
  // persist via the trailing debounce: a burst (e.g. a multi-file OS drop)
  // writes the JSON once when the 500ms window elapses instead of once per
  // file; shutdown() flushes a still-pending save immediately, so the trail
  // still survives a crash mid-iteration
  scheduleSaveEditorState();
}

void DemoEditor::scheduleSaveEditorState() {
  if (!saveDirty_) lastSaveWall_ = wallNow();  // trailing: (re)arm the deadline
  saveDirty_ = true;
}

bool DemoEditor::saveDue() const {
  return saveDirty_ && wallNow() - lastSaveWall_ >= kEditorSaveDebounceSec;
}

void DemoEditor::flushPendingSave() {
  if (!saveDirty_) return;
  saveDirty_ = false;
  saveEditorState();
}

void DemoEditor::rerunDrop(const DropRecord& rec) {
  // re-dispatch through the exact route the drop took (panel + sub-area) so
  // clicking a history record redoes its action
  std::string out;
  switch ((DropPanel)rec.panel) {
    case DropPanel::Timeline: out = routeTimelineDrop(rec.path, (DropSub)rec.sub); break;
    case DropPanel::Console:  out = routeConsoleDrop(rec.path, (DropSub)rec.sub); break;
    case DropPanel::Assets:   out = routeAssetsDrop(rec.path); break;
    default:                  out = routeViewportDrop(rec.path); break;
  }
  // a record that was REJECTED (wrong zone / panel / type) falls back to the
  // file kind's canonical action, so re-running it does something useful
  // instead of just re-ignoring it (e.g. audio that landed on the lanes and
  // was steered now actually loads the track)
  if (out.rfind("ignored", 0) == 0 || out.rfind("steered", 0) == 0 ||
      out.rfind("no supported", 0) == 0 || out.rfind("no drop action", 0) == 0 ||
      out.rfind("not over a panel", 0) == 0) {
    const int kind = rec.kind >= 0 ? rec.kind : kindForPath(rec.path);
    if (kind == (int)BrowseKind::Audio) {
      applyAudioTrack(rec.path);
    } else if (kind == (int)BrowseKind::Shader) {
      openShaderScratch(rec.path);
    } else if (kind == (int)BrowseKind::Script) {
      switchShow(rec.path);
    } else if (kind >= 0) {
      pickBrowseFile(kind, rec.path);
    } else {
      Log::warn("EDITOR", "drop history: nothing to re-run for " + rec.path);
      showToast("history: can't re-run - " + rec.file, 1);
      return;
    }
    Log::info("EDITOR", "drop history: re-ran '" + rec.file +
                             "' via its kind action (was: " + out + ")");
  } else if (out == "failed") {
    // the re-run retried a load that failed before (shader compile / script
    // switch) and failed again - say so instead of an info toast
    Log::warn("EDITOR", "drop history: re-ran '" + rec.file + "' but it failed again");
    showToast("history: re-ran " + rec.file + " - failed again (see console)", 0);
    return;
  } else {
    Log::info("EDITOR", "drop history: re-ran '" + rec.file +
                             "' through its original route");
  }
  showToast("history: re-ran " + rec.file, 2);
}

void DemoEditor::markSessionResume() {
  if (dropHistory_.empty()) return;  // clean slate: nothing to resume
  // a full ring: the marker displaces the oldest record - which the next
  // real drop would evict anyway - so it never exceeds the cap
  if (dropHistory_.size() >= (size_t)kDropHistoryCap)
    dropHistory_.erase(dropHistory_.begin());
  DropRecord r;
  r.panel = -1;  // not a real drop: display-only (no re-run, no drag)
  r.sub = -1;
  r.kind = -1;
  r.t = wallClockNow();
  char date[16];
  const std::time_t tt = (std::time_t)r.t;
  std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&tt));
  r.file = "session";
  r.outcome = std::string("session resumed from ") + date;
  dropHistory_.push_back(std::move(r));
  saveEditorState();  // the marker survives with the trail it annotates
}

std::string DemoEditor::routePanelPayload(DropPanel panel, int kind,
                                          const std::string& path, float mx,
                                          float my) {
  // a payload dropped on a panel is an OS drop whose position is the cursor:
  // same sub-area resolution + routes, and it lands in the history too
  const DropSub sub = subAt(panel, mx, my);
  std::string outcome;
  switch (panel) {
    case DropPanel::Timeline: outcome = routeTimelineDrop(path, sub); break;
    case DropPanel::Console:  outcome = routeConsoleDrop(path, sub); break;
    case DropPanel::Assets:   outcome = routeAssetsDrop(path); break;
    default:  // Viewport (or a future panel): the plain apply route - the
              // sub-area is meaningless there and only stored for the record
      outcome = routeViewportDrop(path);
      break;
  }
  recordDrop(path, (int)panel, (int)sub, kind, outcome);
  return outcome;
}

void DemoEditor::panelDragDropTarget(DropPanel panel) {
  // whole-window target (BeginDragDropTarget() would attach to the *last
  // item* only - e.g. the timeline scrollbar - so drops on the lanes would
  // silently miss). The OS-drop route picks the exact sub-area from the
  // cursor position, so the payload target should cover the entire panel.
  if ((int)panel < 0 || (int)panel >= (int)DropPanel::Count) return;
  const PanelRect& pr = panelRects_[(int)panel];
  if (!pr.valid) return;
  const ImRect bb(ImVec2(pr.x0, pr.y0), ImVec2(pr.x1, pr.y1));
  if (!ImGui::BeginDragDropTargetCustom(bb, ImGui::GetID("panel-dt"))) return;
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kBrowseDragType)) {
    BrowseDragPayload d{};
    std::memcpy(&d, pl->Data, std::min<size_t>(pl->DataSize, sizeof(d)));
    const ImVec2 mp = ImGui::GetIO().MousePos;
    routePanelPayload(panel, d.kind, d.path, mp.x, mp.y);
  } else if (const ImGuiPayload* hov = ImGui::GetDragDropPayload()) {
    if (hov->IsDataType(kBrowseDragType)) {  // hover feedback while dragging
      // inset 2px like the viewport ring: a rect exactly on the window edge
      // would be clipped by the window's own clip rect (antialiased edges)
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 ws = ImGui::GetWindowSize();
      dl->AddRect(ImVec2(wp.x + 2, wp.y + 2),
                  ImVec2(wp.x + ws.x - 2, wp.y + ws.y - 2), kAmber, 4.0f, 0, 2.0f);
    }
  }
  ImGui::EndDragDropTarget();
}

void DemoEditor::drainOsDrops() {
  if (osDrops_.empty()) return;
  std::vector<OsDrop> pending;
  pending.swap(osDrops_);
  // ImGui screen coords are framebuffer-scaled; panel rects were stored in
  // that same space, so scale the GLFW content-space drop point to match
  const float sx = ImGui::GetIO().DisplayFramebufferScale.x;
  const float sy = ImGui::GetIO().DisplayFramebufferScale.y;
  for (const OsDrop& d : pending) {
    // the on-screen toast shows the outcome over the viewport; the console
    // log keeps the full path for debugging; the drop history records every
    // outcome so the Console's right-click menu can re-run it
    const std::string fn = std::filesystem::path(d.path).filename().string();
    const int kind = kindForPath(d.path);
    DropPanel panel = panelAt((float)d.x * sx, (float)d.y * sy);
    // a hidden panel keeps its last rect; drops where it used to be must not
    // route to it (fall through to the viewport picture gate instead)
    if ((panel == DropPanel::Timeline && !showTimeline_) ||
        (panel == DropPanel::Console && !showConsole_) ||
        (panel == DropPanel::Assets && !showAssets_)) {
      panel = DropPanel::Viewport;
    }
    if (panel != DropPanel::Viewport) {
      // exact-zone targeting: which sub-area of the panel contains the point
      const DropSub sub = subAt(panel, (float)d.x * sx, (float)d.y * sy);
      std::string outcome;
      switch (panel) {
        case DropPanel::Timeline: outcome = routeTimelineDrop(d.path, sub); break;
        case DropPanel::Console:  outcome = routeConsoleDrop(d.path, sub); break;
        case DropPanel::Assets:   outcome = routeAssetsDrop(d.path); break;
        default:  // a future DropPanel without a route must not swallow silently
          Log::warn("EDITOR", "os drop: no action for panel " +
                                   std::to_string((int)panel) + ": " + d.path);
          showToast("ignored: no drop action here - " + fn, 1);
          outcome = "no drop action";
          break;
      }
      recordDrop(d.path, (int)panel, (int)sub, kind, outcome);
      continue;
    }
    // on the viewport picture: the picture gate applies (panelAt returned
    // Viewport as the fallback only when nothing else contained the point)
    if (vpRectValid_) {
      const float x = (float)d.x * sx, y = (float)d.y * sy;
      if (x < vpRectMinX_ || x > vpRectMaxX_ || y < vpRectMinY_ || y > vpRectMaxY_) {
        Log::warn("EDITOR", "os drop: no drop action at that point: " + d.path);
        showToast("ignored: no drop action here - " + fn, 1);
        recordDrop(d.path, (int)DropPanel::Viewport, -1, kind, "not over a panel");
        continue;
      }
    }
    recordDrop(d.path, (int)DropPanel::Viewport, -1, kind,
               routeViewportDrop(d.path));
  }
}

int DemoEditor::kindForPath(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);  // lowercase, no <cctype> needed
  }
  for (int k = 0; k < (int)BrowseKind::Count; ++k)
    for (int i = 0; i < kBrowseKinds[k].extCount; ++i)
      if (ext == kBrowseKinds[k].exts[i]) return k;
  return -1;
}

// ---------------------------------------------------------------------------
// shader scratch view: a shader dropped on the Console opens here - the
// source is shown and the shader is applied to the viewport (quad: for
// .frag, shadertoy: for .glsl) so the engine's live hot-reload recompiles
// it as you edit the file. ESC or Close dismisses the view (the effect
// stays up until something else replaces it).
// ---------------------------------------------------------------------------
bool DemoEditor::loadScratchSource(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  scratchPath_ = path;
  scratchSrc_.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  if (scratchSrc_.size() > (size_t)kScratchCap) scratchSrc_.resize(kScratchCap);
  // edit buffer: fixed capacity (the same 64KB cap as the load) so typing in
  // InputTextMultiline can never overflow it; the content is NUL-terminated
  // so strlen-based writes stay in bounds
  scratchBuf_.assign(scratchSrc_.begin(), scratchSrc_.end());
  scratchBuf_.push_back(0);
  scratchBuf_.resize((size_t)kScratchCap + 1, 0);
  scratchDirty_ = false;
  return true;
}

void DemoEditor::openShaderScratch(const std::string& path) {
  const std::string fn = std::filesystem::path(path).filename().string();
  if (!loadScratchSource(path)) {
    Log::warn("EDITOR", "scratch: cannot read " + path);
    showToast("scratch: unreadable - " + fn, 1);
    return;
  }
  scratchOpen_ = true;
  // live preview: .frag -> quad:, .glsl -> shadertoy: (a bare .vert can't run
  // standalone; the source view is still useful)
  const std::string eff = browseEffectName(path, w_.shaderDir, w_.dataDir);
  if (w_.app && eff.rfind("quad:", 0) == 0 &&
      std::filesystem::path(path).extension().string() != ".vert") {
    w_.app->editorShowEffect(eff);
  }
  Log::info("EDITOR", "shader scratch: " + path);
  showToast("scratch: " + fn + " (shows in viewport)", 2);
}

void DemoEditor::saveScratch() {
  if (scratchPath_.empty() || !scratchOpen_) return;
  const std::string fn = std::filesystem::path(scratchPath_).filename().string();
  std::ofstream out(scratchPath_, std::ios::binary | std::ios::trunc);
  if (!out) {
    Log::error("EDITOR", "scratch: save failed: " + scratchPath_);
    showToast("scratch: save failed - " + fn, 0);
    return;
  }
  out.write(scratchBuf_.data(), (std::streamsize)std::strlen(scratchBuf_.data()));
  out.close();
  scratchSrc_ = scratchBuf_.data();  // sync the shown source
  scratchDirty_ = false;
  // immediate recompile: the watcher would pick the change up within its
  // cadence anyway, but poking reloadAll() makes the viewport update on the
  // next frame (a broken edit keeps the previous program + logs, exactly
  // like hot reload - the show never goes dark)
  if (w_.app) w_.app->reloadShaders();
  Log::info("EDITOR", "scratch: saved " + scratchPath_);
  showToast("scratch: saved " + fn, 2);
}

void DemoEditor::reloadScratchFromDisk() {
  if (scratchPath_.empty() || !scratchOpen_) return;
  if (!loadScratchSource(scratchPath_)) {
    Log::warn("EDITOR", "scratch: reload failed (file gone?): " + scratchPath_);
    showToast("scratch: reload failed - " +
                  std::filesystem::path(scratchPath_).filename().string(),
              1);
    return;
  }
  Log::info("EDITOR", "scratch: reloaded " + scratchPath_);
}

void DemoEditor::drawScratch() {
  if (!scratchOpen_) return;
  ImGui::OpenPopup(kScratchTitle);
  ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Appearing);
  if (!ImGui::BeginPopup(kScratchTitle)) {
    // the popup closed (ESC / outside click / window teardown): never lose
    // edits - a dirty buffer is saved before the view goes away
    if (scratchOpen_ && scratchDirty_) saveScratch();
    scratchOpen_ = false;  // defensive: don't wedge if the popup can't open
    return;
  }
  ImGui::TextDisabled("shader scratch");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", scratchPath_.c_str());
  if (scratchDirty_) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "* unsaved");
  }
  ImGui::Separator();

  // Ctrl+S saves from anywhere in the popup - even while the editor below
  // has keyboard focus (the transport's Space/R/Q keys stay out via the
  // WantTextInput gate in handleKeys(), so typing never triggers them)
  const bool ctrlS = ImGui::GetIO().KeyCtrl &&
                     ImGui::IsKeyPressed(ImGuiKey_S, false);

  // live source editor: the buffer is editable and Save writes it back, so
  // you can tweak the dropped shader in place and hot-reload recompiles it
  // in the viewport - no external editor needed. Negative size fills the
  // remaining popup space; the buffer is the fixed 64KB edit cap.
  const bool edited = ImGui::InputTextMultiline(
      "##scratchEdit", scratchBuf_.data(), scratchBuf_.size(),
      ImVec2(-1.0f, -56.0f), ImGuiInputTextFlags_AllowTabInput);
  if (edited) scratchDirty_ = true;

  ImGui::BeginDisabled(w_.app == nullptr);
  if (ImGui::Button("Show in viewport")) {
    const std::string eff =
        browseEffectName(scratchPath_, w_.shaderDir, w_.dataDir);
    if (eff.rfind("quad:", 0) == 0 &&
        std::filesystem::path(scratchPath_).extension().string() != ".vert") {
      w_.app->editorShowEffect(eff);
    } else {
      showToast("scratch: can't preview a .vert", 1);
    }
  }
  if (std::filesystem::path(scratchPath_).extension().string() == ".frag") {
    ImGui::SameLine();
    if (ImGui::Button("Add to Scene")) addShaderToScene(scratchPath_);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Save (Ctrl+S)") || ctrlS) saveScratch();
  ImGui::SameLine();
  if (ImGui::Button("Reload from disk")) reloadScratchFromDisk();
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    if (scratchDirty_) saveScratch();  // never lose edits on close
    scratchOpen_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  // (int) cast: %zu is fragile under stb_sprintf builds (codebase convention)
  ImGui::TextDisabled("%d chars%s - hot-reload recompiles it live in the viewport",
                     (int)std::strlen(scratchBuf_.data()),
                     scratchDirty_ ? " (dirty)" : "");
  ImGui::EndPopup();
}

void DemoEditor::drawBrowse() {
  if (!browseOpen_) return;
  browseAutoRefresh(browseKind_);  // files dropped/removed appear without Rescan
  ImGui::OpenPopup(kBrowseTitle);
  ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_Appearing);
  // a regular popup (not a modal): with a modal open, ImGui clears
  // HoveredWindowUnderMovingWindow for anything outside the modal, so drops
  // onto the viewport / hierarchy would silently never register. A popup
  // doesn't block hover, so dragging a file out onto the scene works - same
  // pattern as the audio popup. ESC still closes it (handled in frame()).
  if (!ImGui::BeginPopup(kBrowseTitle)) {
    // defensive: if the popup ever fails to open, don't wedge the flag (the
    // close-edge save in frame() would never fire)
    browseOpen_ = false;
    return;
  }

  // category selector: switching kinds swaps roots/exts/actions; each kind
  // keeps its own scan root + last pick (persisted in editor_state.json)
  const char* kindNames[] = {"Audio", "Texture", "Shader", "Model", "Script", "Video"};
  if (ImGui::Combo("Type", &browseKind_, kindNames, (int)BrowseKind::Count)) {
    AssetBrowse& nb = browse_[browseKind_];
    if (!nb.scanned) {  // first look at this kind: list it now
      nb.scanned = true;
      nb.scanT = wallNow();
      scanAssetBrowser(browseKind_);
    }
  }
  AssetBrowse& cb = browse_[browseKind_];
  const BrowseKindDef& ckd = kBrowseKinds[browseKind_];
  if (!cb.scanned) {  // first open of this kind
    cb.scanned = true;
    cb.scanT = wallNow();
    scanAssetBrowser(browseKind_);
  }

  ImGui::TextDisabled("scan root");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(330);
  const bool entered =
      ImGui::InputText("##browseRoot", cb.root, sizeof(cb.root),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (ImGui::Button("Rescan") || entered) {
    scanAssetBrowser(browseKind_);
    cb.scanT = wallNow();  // re-arm so the auto-refresh isn't due next frame
  }
  ImGui::SameLine();
  if (ImGui::Button("Defaults")) {
    cb.root[0] = '\0';
    scanAssetBrowser(browseKind_);
    cb.scanT = wallNow();
  }
  ImGui::TextDisabled("%s", ckd.hint);
  ImGui::Separator();

  ImGui::BeginChild("##browseList", ImVec2(0, -48), true);
  if (cb.files.empty()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDim), "no matching files found");
  }
  for (const auto& f : cb.files) {
    const std::filesystem::path p(f);
    std::string fn = p.filename().string();
    std::string dir = p.parent_path().string();
    if (dir.size() > 48) dir = "..." + dir.substr(dir.size() - 45);
    if (ImGui::Selectable(fn.c_str(), cb.sel == f)) {
      cb.sel = f;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        pickBrowseFile(browseKind_, f);
        browseOpen_ = false;
        ImGui::CloseCurrentPopup();
      }
    }
    // drag the file out of the browser: the viewport and Sprite nodes are
    // drop targets (shader -> viewport, texture -> sprite, ...)
    if (ImGui::BeginDragDropSource()) {
      BrowseDragPayload d{};
      d.kind = browseKind_;
      std::strncpy(d.path, f.c_str(), sizeof(d.path) - 1);
      d.path[sizeof(d.path) - 1] = '\0';
      ImGui::SetDragDropPayload(kBrowseDragType, &d, sizeof(d));
      ImGui::TextUnformatted(fn.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kFaint), "%s", dir.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
  }
  ImGui::EndChild();

  ImGui::Separator();
  ImGui::BeginDisabled(cb.sel.empty());
  if (ImGui::Button(ckd.action, ImVec2(120, 0))) {
    pickBrowseFile(browseKind_, cb.sel);
    browseOpen_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  if (browseKind_ == (int)BrowseKind::Shader &&
      std::filesystem::path(cb.sel).extension().string() == ".frag") {
    ImGui::SameLine();
    const bool hasScene = w_.app &&
                          (!selScene_.empty() || !w_.app->activeScene().empty());
    ImGui::BeginDisabled(!hasScene || cb.sel.empty());
    if (ImGui::Button("Add to Scene")) {
      addShaderToScene(cb.sel);
      browseOpen_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Copy the shader into shaders/ and add it to the selected scene");
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    browseOpen_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%d file(s)", (int)cb.files.size());
  ImGui::SameLine();
  // live-refresh state: phosphor "updated" flashes after a real change
  if (wallNow() - cb.flashT < 1.5) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPhosphor), "updated");
  } else {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kFaint), "auto-refresh");
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// editor state persistence: the chosen audio track survives relaunches
// ---------------------------------------------------------------------------
void DemoEditor::saveEditorState() {
  std::string track = w_.audio && w_.audio->trackMode ? w_.audio->trackPath() : "";
  // store an absolute path so a relaunch from a different working directory
  // still finds the file (relative paths are cwd-dependent)
  if (!track.empty()) track = std::filesystem::absolute(track).string();
  Value v = Value::object();
  v.set("track") = Value(track);
  v.set("beatOffset") = Value((double)beatOffset_);  // grid phase alignment
  v.set("quantize") = Value(quantize_);              // scrub snapping
  v.set("quantizeGrid") = Value((int)quantizeGrid_); // 0 beat, 1 bar
  // per-kind browser state: each category's scan root + last pick, absolute
  // like the track above, so a relaunch from a different cwd lands every
  // picker on the same folders
  Value browsers = Value::object();
  for (int k = 0; k < (int)BrowseKind::Count; k++) {
    const AssetBrowse& b = browse_[k];
    std::string root = b.root;
    if (!root.empty()) root = std::filesystem::absolute(root).string();
    Value bv = Value::object();
    bv.set("root") = Value(root);
    bv.set("sel") = Value(b.sel);
    browsers.set(kBrowseKinds[k].key) = bv;
  }
  v.set("browsers") = browsers;
  v.set("browserKind") = Value(browseKind_);
  Value guides = Value::object();
  guides.set("grid") = Value(showGrid_);
  guides.set("axes") = Value(showAxes_);
  guides.set("safeFrame") = Value(showSafeFrame_);
  guides.set("crosshair") = Value(showCrosshair_);
  v.set("viewportGuides") = std::move(guides);
  // panel visibility (View menu): a user who hides the Toolbar expects it
  // hidden next launch, not resurfaced. While the fullscreen preview hides
  // every panel, persist the PRE-preview layout (the saved*_ set) instead,
  // so a save taken mid-preview can't record 'all hidden'. (The ImGui
  // Demo/Metrics diagnostic windows are deliberately not persisted.)
  {
    Value panels = Value::object();
    const bool inPreview = fullscreenPreview_;
    panels.set("toolbar") = Value(inPreview ? savedToolbar_ : showToolbar_);
    panels.set("hierarchy") = Value(inPreview ? savedHierarchy_ : showHierarchy_);
    panels.set("inspector") = Value(inPreview ? savedInspector_ : showInspector_);
    panels.set("timeline") = Value(inPreview ? savedTimeline_ : showTimeline_);
    panels.set("console") = Value(inPreview ? savedConsole_ : showConsole_);
    panels.set("assets") = Value(inPreview ? savedAssets_ : showAssets_);
    panels.set("profiler") = Value(inPreview ? savedProfiler_ : showProfiler_);
    v.set("panels") = panels;
  }
  // per-show timeline view: the current show's zoom/scroll/fit window, MERGED
  // with every other show's so switching shows (or a later save) never loses
  // another show's window - this save only updates this show's entry
  {
    Value views = Value::object();
    if (std::filesystem::exists(editorStatePath())) {
      try {
        views = Json::parseFile(editorStatePath()).get("timelineViews");
      } catch (const std::exception& e) {
        Log::warn("EDITOR", "timeline views unreadable, starting fresh: " +
                                 std::string(e.what()));
        views = Value::object();
      }
    }
    if (!views.isObj()) views = Value::object();
    const std::string key = showKey();
    if (!key.empty()) {
      Value vv = Value::object();
      vv.set("zoom") = Value((double)tlZoom_);
      vv.set("t0") = Value((double)tlT0_);
      vv.set("fitZoom") = Value((double)tlFitZoom_);
      vv.set("fitT0") = Value((double)tlFitT0_);
      views.set(key) = vv;
    }
    v.set("timelineViews") = views;
  }
  // OS drop history: capped like the in-memory ring; timestamps are wall-clock
  // so they stay meaningful (and render via strftime) after a relaunch
  Value hist = Value::array();
  for (const auto& r : dropHistory_) {
    Value rv = Value::object();
    rv.set("path") = Value(r.path);
    rv.set("outcome") = Value(r.outcome);
    rv.set("panel") = Value(r.panel);
    rv.set("sub") = Value(r.sub);
    rv.set("kind") = Value(r.kind);
    // integer epoch SECONDS: the JSON writer emits exact integers (%lld), so
    // the timestamp survives the %.9g serializer bit-exactly - the tooltip
    // shows HH:MM:SS, so second precision is all it needs
    rv.set("t") = Value((double)(long long)r.t);
    hist.push(rv);
  }
  v.set("dropHistory") = hist;
  std::error_code ec;
  std::filesystem::create_directories(w_.dataDir, ec);  // fresh project may lack it
  try {
    Json::writeFile(editorStatePath(), v, 2);
    saveWrites_++;  // real write counter (smoke + debug affordance)
  } catch (const std::exception& e) {
    Log::error("EDITOR", "state save failed: " + std::string(e.what()));
  }
}

void DemoEditor::restoreEditorState() {
  const std::string path = editorStatePath();
  if (!std::filesystem::exists(path)) return;  // first run: nothing to restore
  try {
    const Value v = Json::parseFile(path);
    // per-kind browser state (independent of the audio source): restore every
    // category's scan root + last pick first, so even a --no-track session
    // lands the browsers where the user was; the open handlers re-list. Legacy
    // state files carried the audio fields at the top level - fall back to them.
    const Value browsers = v.get("browsers");
    for (int k = 0; k < (int)BrowseKind::Count; k++) {
      AssetBrowse& b = browse_[k];
      const Value bv = browsers.get(kBrowseKinds[k].key);
      const std::string root = bv.get("root").asStr();
      if (!root.empty()) {
        std::strncpy(b.root, root.c_str(), sizeof(b.root) - 1);
        b.root[sizeof(b.root) - 1] = '\0';
      }
      b.sel = bv.get("sel").asStr();
      b.scanned = false;  // re-list under the restored root on next open
    }
    if (browsers.isNull()) {  // genuinely legacy file: top-level keys were the
      // audio picker; a new-format file never falls through here, so a user who
      // cleared the audio root isn't wrongly re-fitted with old values
      AssetBrowse& audioB = browse_[(int)BrowseKind::Audio];
      const std::string root = v.get("browseRoot").asStr();
      if (!root.empty()) {
        std::strncpy(audioB.root, root.c_str(), sizeof(audioB.root) - 1);
        audioB.root[sizeof(audioB.root) - 1] = '\0';
      }
      audioB.sel = v.get("browseSel").asStr();
    }
    browseKind_ = v.get("browserKind").asInt(0);
    if (browseKind_ < 0 || browseKind_ >= (int)BrowseKind::Count) browseKind_ = 0;
    const Value guides = v.get("viewportGuides");
    if (guides.isObj()) {
      showGrid_ = guides.get("grid").asBool(showGrid_);
      showAxes_ = guides.get("axes").asBool(showAxes_);
      showSafeFrame_ = guides.get("safeFrame").asBool(showSafeFrame_);
      showCrosshair_ = guides.get("crosshair").asBool(showCrosshair_);
    }
    // panel visibility (View menu): whatever you hid stays hidden next launch
    // (the fullscreen-preview save guard keeps the pre-preview layout, so the
    // file always holds the user's real panel choices)
    applyPanelVisibility(v);
    // OS drop history (capped, independent of the audio source): restored only
    // when nothing is in memory yet, so a live restore can't clobber newer
    // drops; the filename is recomputed from the path
    if (dropHistory_.empty()) {
      const Value hist = v.get("dropHistory");
      if (hist.isArr()) {
        for (const auto& rv : hist.asArr()) {
          DropRecord r;
          r.path = rv.get("path").asStr();
          r.file = std::filesystem::path(r.path).filename().string();
          r.outcome = rv.get("outcome").asStr();
          r.panel = rv.get("panel").asInt(-1);
          r.sub = rv.get("sub").asInt(-1);
          r.kind = rv.get("kind").asInt(-1);
          r.t = rv.get("t").asNum(0.0);
          if (r.path.empty()) continue;
          dropHistory_.push_back(std::move(r));
          if (dropHistory_.size() >= (size_t)kDropHistoryCap) break;
        }
      }
    }
    // per-show timeline view: reopen this show exactly where you left it (the
    // visible window, not the playhead). Independent of the audio source, so
    // it also applies under --no-track / --track=FILE.
    applyTimelineViewForShow(showKey());
    // explicit CLI intent always wins: --no-track (silence) and --track=FILE both
    // suppress the persisted choice, exactly like they override the auto-search
    if (w_.noTrack || !w_.trackOverride.empty()) return;
    const std::string track = v.get("track").asStr();
    beatOffset_ = v.get("beatOffset").asFloat(0.0f);  // grid phase alignment
    quantize_ = v.get("quantize").asBool(false);      // scrub snapping
    quantizeGrid_ = v.get("quantizeGrid").asInt(0);
    if (track.empty()) return;  // explicitly stopped audio last session
    if (!std::filesystem::exists(track)) {
      Log::warn("EDITOR", "saved track is missing, keeping the current source: " + track);
      return;
    }
    if (!w_.audio) return;
    if (w_.audio->swapTrack(track, 0)) Log::info("EDITOR", "restored track: " + track);
    else Log::error("EDITOR", "restore failed (kept current source): " + track);
  } catch (const std::exception& e) {
    Log::error("EDITOR", "state load failed: " + std::string(e.what()));
  }
}

void DemoEditor::applyPanelVisibility(const Value& v) {
  const Value p = v.get("panels");
  if (p.isNull()) return;  // older state file: keep the in-memory defaults
  showToolbar_ = p.get("toolbar").asBool(showToolbar_);
  showHierarchy_ = p.get("hierarchy").asBool(showHierarchy_);
  showInspector_ = p.get("inspector").asBool(showInspector_);
  showTimeline_ = p.get("timeline").asBool(showTimeline_);
  showConsole_ = p.get("console").asBool(showConsole_);
  showAssets_ = p.get("assets").asBool(showAssets_);
  showProfiler_ = p.get("profiler").asBool(showProfiler_);
}

void DemoEditor::rebuildAudioEnvelope() {
  audioEnv_.clear();
  if (!w_.audio) {
    audioEnvPath_.clear();
    audioEnvFrames_ = 0;
    return;
  }
  // record the source up front so the draw-time invalidation check can't
  // re-trigger a rebuild every frame on an early return
  audioEnvPath_ = w_.audio->trackPath();
  audioEnvFrames_ = w_.audio->trackFrames();
  detectedBpm_ = 0.0f;
  if (!w_.audio->trackMode) return;
  const std::vector<float>& data = w_.audio->trackSamples();
  const uint64_t frames = w_.audio->trackFrames();
  const unsigned sr = w_.audio->sampleRate();
  if (frames < 2 || sr == 0) return;
  // one peak bucket per ~17ms (kAudioEnvPerSec buckets/s) - independent of
  // the zoom/panel width, so any strip resolution just indexes into it
  const uint64_t buckets =
      std::max<uint64_t>(1, (uint64_t)(frames * (uint64_t)kAudioEnvPerSec / sr) + 1);
  audioEnv_.assign((size_t)buckets, 0.0f);
  const uint64_t per = std::max<uint64_t>(1, frames / buckets);
  for (uint64_t b = 0; b < buckets; b++) {
    const uint64_t f0 = b * per, f1 = std::min<uint64_t>(frames, f0 + per);
    float peak = 0;
    for (uint64_t f = f0; f < f1; f++) {
      const float l = std::fabs(data[(size_t)f * 2]);
      const float r = std::fabs(data[(size_t)f * 2 + 1]);
      peak = std::max(peak, std::max(l, r));
    }
    audioEnv_[(size_t)b] = peak;
  }
  detectKicks();
  applyDetectedBpm();
}

void DemoEditor::detectKicks() {
  kickTimes_.clear();
  if (!w_.audio || !w_.audio->trackMode) return;
  const std::vector<float>& data = w_.audio->trackSamples();
  const uint64_t frames = w_.audio->trackFrames();
  const unsigned sr = w_.audio->sampleRate();
  if (frames < 4 || sr == 0) return;

  // kick = a sharp low-frequency transient: short-window RMS energy spiking
  // above an adaptive background (fast-attack / slow-release follower). No FFT
  // needed - the window averages out the mid/high content, so only the
  // low-frequency (kick/bass) transients punch through.
  const uint32_t win = std::max(1u, sr / 200);      // 5 ms window
  const uint32_t hop = std::max(1u, sr / 200);      // 5 ms hop (~200 bins/s)
  const size_t bins = (size_t)(frames / hop);
  if (bins < 8) return;
  std::vector<float> e(bins, 0.0f);
  float maxE = 0;
  for (size_t b = 0; b < bins; b++) {
    const uint64_t f0 = (uint64_t)b * hop;
    const uint64_t f1 = std::min<uint64_t>(frames, f0 + win);
    double acc = 0;
    for (uint64_t f = f0; f < f1; f++) {
      const float l = data[(size_t)f * 2];
      const float r = data[(size_t)f * 2 + 1];
      acc += (double)l * l + (double)r * r;
    }
    e[b] = (float)std::sqrt(acc / (double)std::max<uint64_t>(1, f1 - f0));
    maxE = std::max(maxE, e[b]);
  }
  if (maxE < 1e-6f) {
    std::fprintf(stderr, "[EDITOR] kick detect: digital silence maxE=%g\n", maxE);
    return;
  }

  // kick = a sharp low-frequency transient: short-window RMS energy spiking
  // above the track's background. The background is the MEDIAN window energy
  // - kicks are sparse (a handful per second), so they barely move it, and
  // the steady groove/bed level sits right at it. A follower or sliding mean
  // tracks the kick's own decay and swallows the onsets; the median cannot.
  // A window counts as a kick when it is a local peak and clears the median
  // by ~2.2x.
  const float minBeat = beatSec(216.0f) * 0.4f;   // <= ~0.4 beat spacing
  std::vector<float> es(bins);
  std::copy(e.begin(), e.end(), es.begin());
  const size_t mid = bins / 2;
  std::nth_element(es.begin(), es.begin() + mid, es.end());
  const float bg = es[mid];
  if (bg <= 0) return;
  // scan from bin 0 (a track that kicks on the very first sample would
  // otherwise lose its first transient); the first/last bins only need their
  // one neighbour
  for (size_t b = 0; b < bins; b++) {
    const bool peak = (b == 0 || e[b] > e[b - 1]) && (b + 1 >= bins || e[b] > e[b + 1]);
    if (!peak) continue;
    if (e[b] < bg * 2.2f) continue;                      // not a real transient
    const float t = (float)b * hop / (float)sr;
    if (!kickTimes_.empty() && t - kickTimes_.back() < minBeat) continue;
    kickTimes_.push_back(t);
  }
}

float DemoEditor::estimateTrackBpm() const {
  if (!w_.audio || !w_.audio->trackMode) return 0.0f;

  // Prefer the kick transients: unlike a raw waveform autocorrelation, their
  // spacing maps directly to the musical beat and stays stable when a track
  // contains a sustained bass note. Require a few reasonably regular hits so
  // a handful of unrelated impacts cannot change the project tempo.
  if (kickTimes_.size() >= 3) {
    std::vector<float> intervals;
    intervals.reserve(kickTimes_.size() - 1);
    for (size_t i = 1; i < kickTimes_.size(); ++i) {
      const float d = kickTimes_[i] - kickTimes_[i - 1];
      if (d >= 0.20f && d <= 2.0f) intervals.push_back(d);
    }
    if (intervals.size() >= 2) {
      std::sort(intervals.begin(), intervals.end());
      const float median = intervals[intervals.size() / 2];
      size_t inliers = 0;
      float err = 0.0f;
      for (const float d : intervals) {
        const float rel = std::fabs(d - median) / median;
        if (rel < 0.20f) {
          ++inliers;
          err += rel;
        }
      }
      if (inliers >= std::max<size_t>(2, intervals.size() / 2)) {
        float bpm = 60.0f / median;
        // The detector can miss every other hit, or occasionally report a
        // subdivision. Fold octave errors into a useful demoscene tempo range
        // without permitting implausible values into the project header.
        while (bpm < 60.0f) bpm *= 2.0f;
        while (bpm > 240.0f) bpm *= 0.5f;
        if (bpm >= 60.0f && bpm <= 240.0f &&
            (err / (float)inliers) < 0.20f)
          return std::round(bpm * 2.0f) * 0.5f;
      }
    }
  }

  // Fallback for tracks without sharp kick transients: correlate the editor's
  // 60 Hz peak envelope over the conventional 60..240 BPM range. The
  // normalized score avoids preferring a lag merely because it has more
  // overlapping samples.
  if (audioEnv_.size() < 32) return 0.0f;
  const float mean = std::accumulate(audioEnv_.begin(), audioEnv_.end(), 0.0f) /
                     (float)audioEnv_.size();
  float bestScore = 0.0f;
  float bestBpm = 0.0f;
  for (float bpm = 60.0f; bpm <= 240.0f; bpm += 0.5f) {
    const int lag = (int)std::round(60.0f * 60.0f / bpm);
    if (lag <= 0 || lag >= (int)audioEnv_.size()) continue;
    double dot = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = (size_t)lag; i < audioEnv_.size(); ++i) {
      const float a = audioEnv_[i] - mean;
      const float b = audioEnv_[i - (size_t)lag] - mean;
      dot += (double)a * b;
      aa += (double)a * a;
      bb += (double)b * b;
    }
    const float score = (aa > 1e-9 && bb > 1e-9)
                           ? (float)(dot / std::sqrt(aa * bb))
                           : 0.0f;
    if (score > bestScore) {
      bestScore = score;
      bestBpm = bpm;
    }
  }
  return bestScore >= 0.30f ? bestBpm : 0.0f;
}

void DemoEditor::applyDetectedBpm() {
  const float bpm = estimateTrackBpm();
  if (bpm <= 0.0f) {
    detectedBpm_ = 0.0f;
    return;
  }
  detectedBpm_ = bpm;
  const float current = doc_.ast.bpm > 0.0f ? doc_.ast.bpm : 216.0f;
  if (std::fabs(current - bpm) < 0.5f) {
    Log::info("AUDIO", "track BPM detected: " + std::to_string(bpm) +
                           " (project already matches)");
    return;
  }

  // The audio smoke creates a deliberately tiny temporary track while it is
  // running against the real editor process. Report the estimate there but
  // never rewrite the user's project as a side effect of a diagnostic run.
  if (smokeAudio_) {
    Log::info("AUDIO", "track BPM detected: " + std::to_string(bpm) +
                           " (smoke; project unchanged)");
    return;
  }

  doc_.beginEdit("set project BPM from track");
  doc_.ast.bpm = bpm;
  doc_.endEdit();
  if (doc_.path.empty()) {
    if (w_.timeline) w_.timeline->setBpm(bpm);
    Log::info("AUDIO", "track BPM detected: " + std::to_string(bpm) +
                           " (runtime timeline updated; project has no path)");
    return;
  }
  if (writeDocument()) {
    Log::info("AUDIO", "track BPM detected: " + std::to_string(bpm) +
                           " - project BPM updated");
  } else {
    Log::error("AUDIO", "track BPM detected but project BPM could not be saved");
  }
}

void DemoEditor::autoAlignBeats() {
  const float beat = w_.timeline ? w_.timeline->beatSec()
                                 : beatSec(216.0f);
  if (kickTimes_.empty() || beat <= 0) return;
  // fit the grid phase (offset added to n*beat) that minimizes the total
  // distance from each detected kick to its nearest grid line
  float bestErr = 1e30f;
  float bestOff = 0;
  const float step = 0.005f;  // 5 ms resolution - plenty for alignment
  const int n1 = (int)(kickTimes_.front() / beat) + 1;
  const int n2 = (int)(kickTimes_.back() / beat) + 2;
  for (float off = 0; off < beat; off += step) {
    float err = 0;
    for (const float k : kickTimes_) {
      const float d = std::fmod(k - off, beat);
      const float dist = std::min(d, beat - d);  // wrap-around distance
      err += dist;
    }
    if (err < bestErr) {
      bestErr = err;
      bestOff = off;
    }
  }
  beatOffset_ = bestOff;
  beatDrag_ = false;
  saveEditorState();  // the alignment is worth remembering
  char sb[128];
  std::snprintf(sb, sizeof sb,
                "editor: beat grid aligned to %zu kicks (phase offset %.3fs)",
                kickTimes_.size(), beatOffset_);
  Log::info("AUDIO", sb);
}

void DemoEditor::applyAudioTrack(const std::string& path) {
  // non-blocking: the decode runs on a worker thread and the swap commits in
  // pumpAsyncAudioSwap() when it is ready (the previous source keeps playing
  // meanwhile). The log + state save happen there so they reflect the result.
  if (!w_.audio) return;
  const float show = w_.director ? w_.director->show : 0;
  if (path.empty()) {
    // stopping audio is instant (no decode) - commit immediately
    if (w_.audio->swapTrack("", show)) {
      saveEditorState();
      Log::info("AUDIO", "editor: no track (silent)");
    }
    return;
  }
  std::filesystem::path p(path);
  if (std::error_code ec; !std::filesystem::exists(p, ec)) {
    char sb[512];
    std::snprintf(sb, sizeof sb, "editor: track load failed: %s (kept previous)",
                  path.c_str());
    Log::error("AUDIO", sb);
    return;
  }
  w_.audio->beginAsyncSwap(path, show);
  Log::info("AUDIO", (std::string("editor: decoding ") + path).c_str());
}

void DemoEditor::pumpAsyncAudioSwap() {
  if (!w_.audio) return;
  // only act on a finished decode (Ready = apply, Failed = keep the previous
  // source and log). Decoding is still in flight - keep the old track playing.
  const bool finished = w_.audio->asyncStatus() != ns::AudioEngine::AsyncState::Decoding;
  if (!finished) return;
  const std::string pending = w_.audio->asyncPath();
  const float show = w_.director ? w_.director->show : 0;
  const bool ok = w_.audio->applyAsyncSwap();
  char sb[512];
  if (ok) {
    saveEditorState();  // remember the choice for the next launch
    // a new track invalidates the previous beat alignment (different songs
    // kick on different offsets); detectKicks() runs with the new track via
    // the envelope rebuild at the next draw
    beatOffset_ = 0;
    beatDrag_ = false;
    // Analyse immediately after the decode commits. This keeps BPM detection
    // independent of panel visibility and applies the project tempo before
    // the next engine update/render.
    rebuildAudioEnvelope();
    std::snprintf(sb, sizeof sb, "editor: track -> %s (synced to show time %.1fs)",
                  pending.c_str(), show);
    Log::info("AUDIO", sb);
  } else if (pending != "") {
    std::snprintf(sb, sizeof sb, "editor: track load failed: %s (kept previous)",
                  pending.c_str());
    Log::error("AUDIO", sb);
  }
}

// ---------------------------------------------------------------------------
// scene / asset authoring
// ---------------------------------------------------------------------------
void DemoEditor::openAssetDialog() {
  assetOpen_ = true;
  assetKind_ = 0;
  assetCreated_ = false;
  assetCreatedPath_.clear();
  assetName_[0] = '\0';
}

void DemoEditor::applyQueuedActions() {
  if (textViewportCommitQueued_) {
    textViewportCommitQueued_ = false;
    const std::string nodeName = textViewportEditNode_;
    if (writeDocument()) {
      selNode_ = w_.app ? w_.app->editableScene().find(nodeName) : nullptr;
      showToast("text updated", 2);
    }
    textViewportEditNode_.clear();
  }
  if (sceneAddQueued_) {
    sceneAddQueued_ = false;
    addSceneViaDocument();  // the document op (writes the AST, not raw text)
  }
}

void DemoEditor::drawNewProjectConfirm() {
  if (!newProjectConfirmOpen_) return;
  ImGui::OpenPopup("Create New Project?");
  if (!ImGui::BeginPopupModal("Create New Project?", &newProjectConfirmOpen_,
                              ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextWrapped("The current project has unsaved changes.");
  ImGui::TextWrapped("Create a new project and discard those changes?");
  ImGui::TextDisabled("%s", pendingNewProjectPath_.c_str());
  ImGui::Separator();
  if (ImGui::Button("Create New Project")) {
    const std::string path = pendingNewProjectPath_;
    newProjectConfirmOpen_ = false;
    pendingNewProjectPath_.clear();
    createNewProject(path);
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    newProjectConfirmOpen_ = false;
    pendingNewProjectPath_.clear();
  }
  ImGui::EndPopup();
}

void DemoEditor::seedNsdCommandSource() {
  if (nsdCommandIndex_ < 0 || nsdCommandIndex_ >= kNsdCommandCount) nsdCommandIndex_ = 0;
  const NsdCommandInfo& info = kNsdCommands[nsdCommandIndex_];
  if (nsdCommandSeed_ == info.name) return;
  std::fill(nsdCommandBuf_.begin(), nsdCommandBuf_.end(), '\0');
  std::snprintf(nsdCommandBuf_.data(), nsdCommandBuf_.size(), "%s", info.source);
  nsdCommandSeed_ = info.name;
  nsdCommandDirty_ = false;
}

bool DemoEditor::insertNsdCommand() {
  if (!w_.app || doc_.path.empty()) {
    Log::warn("EDITOR", "NSD command insertion requires a saved .nsd project");
    return false;
  }
  const std::string source = nsdCommandBuf_.data();
  if (source.empty()) {
    Log::warn("EDITOR", "NSD command source is empty");
    return false;
  }

  Cmd command;
  std::string error;
  if (!parseEditorCommand(source, nsdCommandTarget_, command, error)) {
    Log::error("EDITOR", "NSD command rejected: " + error);
    return false;
  }
  const std::string insertedSource = nsdSerializeCmd(command);

  doc_.beginEdit("insert NSD command");
  if (nsdCommandTarget_ == 0) {
    std::string sceneName = selScene_;
    if (sceneName.empty()) sceneName = w_.app->activeScene();
    SceneDef* scene = doc_.findScene(sceneName);
    if (!scene) {
      doc_.cancelEdit();
      Log::warn("EDITOR", "select a scene before inserting a setup command");
      return false;
    }
    scene->setup.push_back(std::move(command));
    selScene_ = sceneName;
  } else {
    ScriptBlock block;
    block.time = std::max(0.0f, w_.director ? w_.director->show : 0.0f);
    block.cmds.push_back(std::move(command));
    doc_.ast.main.push_back(std::move(block));
    std::sort(doc_.ast.main.begin(), doc_.ast.main.end(),
              [](const ScriptBlock& a, const ScriptBlock& b) { return a.time < b.time; });
  }
  doc_.endEdit();

  if (!writeDocument()) {
    Log::error("EDITOR", "NSD command could not be written to the project");
    return false;
  }
  // Reloading the document rebuilds the scene graph and invalidates the
  // inspector's selected node pointer.
  selNode_ = nullptr;
  selEffect_.clear();
  textEditNodeKey_.clear();
  nsdCommandDirty_ = false;
  Log::info("EDITOR", "inserted NSD command: " + insertedSource);
  return true;
}

void DemoEditor::drawNsdCommands() {
  if (!showNsdCommands_ || fullscreenPreview_) return;
  seedNsdCommandSource();
  if (!ImGui::Begin("NSD Commands", &showNsdCommands_)) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Authoring interface for every NSD scripting command");
  ImGui::TextDisabled("Choose a template, then edit the generated source for advanced options.");
  ImGui::Separator();

  const char* commandNames[kNsdCommandCount];
  for (int i = 0; i < kNsdCommandCount; ++i) commandNames[i] = kNsdCommands[i].name;
  if (ImGui::Combo("Command", &nsdCommandIndex_, commandNames, kNsdCommandCount))
    seedNsdCommandSource();

  const NsdCommandInfo& info = kNsdCommands[nsdCommandIndex_];
  ImGui::Text("%s", info.signature);
  ImGui::TextDisabled("%s", info.description);

  const char* targets[] = {"Selected scene setup", "Timeline at playhead"};
  ImGui::Combo("Insert into", &nsdCommandTarget_, targets, 2);
  if (nsdCommandTarget_ == 0) {
    const std::string sceneName = !selScene_.empty()
                                      ? selScene_
                                      : (w_.app ? w_.app->activeScene() : std::string());
    if (sceneName.empty())
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                         "Select a scene in Hierarchy first.");
    else
      ImGui::TextDisabled("target scene: %s", sceneName.c_str());
  } else {
    ImGui::TextDisabled("target time: %s", fmtTime(w_.director ? w_.director->show : 0.0f).c_str());
  }

  ImGui::SeparatorText("Command source");
  if (ImGui::InputTextMultiline("##nsd_command_source", nsdCommandBuf_.data(),
                                nsdCommandBuf_.size(), ImVec2(-1, 150)))
    nsdCommandDirty_ = true;

  if (ImGui::Button("Insert into Project")) insertNsdCommand();
  ImGui::SameLine();
  if (ImGui::Button("Copy Source")) ImGui::SetClipboardText(nsdCommandBuf_.data());
  ImGui::SameLine();
  if (ImGui::Button("Reset Template")) {
    nsdCommandSeed_.clear();
    seedNsdCommandSource();
  }
  ImGui::TextDisabled("Commands are serialized, saved, and reloaded immediately.");
  ImGui::End();
}

void DemoEditor::drawNewAssetDialog() {
  if (!assetOpen_) return;
  ImGui::OpenPopup("New Asset");
  if (!ImGui::BeginPopupModal("New Asset", &assetOpen_,
                              ImGuiWindowFlags_AlwaysAutoResize)) return;

  const char* kinds[] = {"Material", "Post preset", "Scene", "Shader (.frag)",
                         "Shadertoy (.glsl)"};
  if (ImGui::Combo("Type", &assetKind_, kinds, 5)) assetCreated_ = false;
  ImGui::SetNextItemWidth(240);
  if (ImGui::InputText("Name", assetName_, sizeof(assetName_))) assetCreated_ = false;

  const std::string clean = sanitizeAssetName(assetName_);
  const std::string target = assetTargetPath();
  const bool nameOk = !clean.empty();
  const bool exists = nameOk && std::filesystem::exists(target);

  // when the name collides, reveal WHAT it collides with. On Windows the
  // file system matches names case-insensitively, so typing 'Lightbox' can
  // silently clash with an existing 'lightbox.json' - the old one-line
  // "already exists" gave the author no way to see why. Scan the target
  // folder for the real on-disk file, and offer the next free name (the
  // + Scene button's auto-suffix scheme) as a one-click fill.
  std::string clashBase;  // real on-disk basename (sans extension) that exists
  if (exists) {
    // canonical() resolves to the path's REAL on-disk casing (on Windows MSVC
    // uses GetFinalPathNameByHandle), so a 'Lightbox' collision with a stored
    // 'lightbox.json' is visible instead of the sanitized lowercase target
    // hiding it - and no per-frame directory scan is needed.
    std::error_code ec;
    std::filesystem::path real = std::filesystem::canonical(target, ec);
    if (!ec) clashBase = real.stem().string();
  }
  const std::string freeName = exists ? suggestFreeAssetName() : std::string();

  ImGui::Separator();
  ImGui::TextDisabled("writes:");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", target.c_str());
  if (!nameOk)
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError),
                       "name must be a-z 0-9 _ - (lowercase)");
  else if (exists) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kError),
                       "already exists as '%s'",
                       clashBase.empty() ? clean.c_str() : clashBase.c_str());
    if (!clashBase.empty() && assetName_ != clashBase)
      ImGui::TextDisabled("(this file system matches names case-insensitively - "
                          "'%s' collides with '%s')",
                          assetName_, clashBase.c_str());
    if (!freeName.empty() && freeName != clean) {
      ImGui::TextDisabled("next free name:");
      ImGui::SameLine();
      if (ImGui::SmallButton(freeName.c_str())) {
        std::strncpy(assetName_, freeName.c_str(), sizeof(assetName_) - 1);
        assetName_[sizeof(assetName_) - 1] = '\0';
        assetCreated_ = false;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("click to use this name");
    }
  }

  ImGui::TextDisabled("template preview:");
  const std::string tmpl = assetTemplate();
  ImGui::InputTextMultiline("##tmpl", const_cast<char*>(tmpl.c_str()),
                            tmpl.size() + 1, ImVec2(470, 190),
                            ImGuiInputTextFlags_ReadOnly);

  ImGui::BeginDisabled(!nameOk || exists);
  if (ImGui::Button("Create", ImVec2(100, 0))) createAssetFromForm();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (assetCreated_)
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPhosphor), "created - ready to use");
  else ImGui::TextDisabled("(disabled until the name is valid and free)");

  if (assetCreated_) {
    if (assetKind_ == 0) {  // material: apply to the mesh renderer now
      if (ImGui::Button("Load in show")) w_.app->editorLoadMaterial(clean);
    } else if (assetKind_ == 1) {  // post preset: swap the stack now
      if (ImGui::Button("Apply preset")) w_.app->editorLoadPreset(clean);
    } else if (assetKind_ == 3) {  // quad shader: render it in the viewport
      if (ImGui::Button("Show in viewport")) w_.app->editorShowEffect("quad:" + clean);
    } else {
      ImGui::TextDisabled("file created - reference it from the script");
    }
  }
  ImGui::Separator();
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

bool DemoEditor::createAssetFromForm() {
  const std::string clean = sanitizeAssetName(assetName_);
  const std::string target = assetTargetPath();
  if (clean.empty() || std::filesystem::exists(target)) return false;
  // a fresh project may lack data/materials, data/post, ... - make the dir
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(target).parent_path(), ec);
  std::ofstream out(target, std::ios::binary);
  if (!out) {
    Log::error("EDITOR", "asset create failed: " + target);
    return false;
  }
  out << assetTemplate();
  out.close();
  assetCreated_ = true;
  assetCreatedPath_ = target;
  Log::info("EDITOR", "asset created: " + target);
  return true;
}

std::string DemoEditor::assetTargetPath() const {
  return assetTargetPathFor(sanitizeAssetName(assetName_));
}

std::string DemoEditor::assetTargetPathFor(const std::string& clean) const {
  switch (assetKind_) {
    case 0: return w_.dataDir + "/materials/" + clean + ".json";
    case 1: return w_.dataDir + "/post/" + clean + ".json";
    case 2: return w_.dataDir + "/scenes/" + clean + ".json";
    case 3: return w_.shaderDir + "/" + clean + ".frag";
    default: return w_.dataDir + "/shadertoy/" + clean + ".glsl";
  }
}

std::string DemoEditor::suggestFreeAssetName() const {
  // the typed name, if free; else the same auto-suffix scheme the + Scene
  // button uses (SceneN), so "taken" never dead-ends the author
  const std::string clean = sanitizeAssetName(assetName_);
  if (clean.empty()) return std::string();
  if (!std::filesystem::exists(assetTargetPathFor(clean))) return clean;
  for (int n = 2; n < 10000; ++n) {
    const std::string cand = clean + std::to_string(n);
    if (!std::filesystem::exists(assetTargetPathFor(cand))) return cand;
  }
  return std::string();
}

std::string DemoEditor::assetTemplate() const {
  const std::string n = sanitizeAssetName(assetName_);
  switch (assetKind_) {
    case 0:  // material
      return "{\n"
             "  \"name\": \"" + n + "\",\n"
             "  \"baseColor\": [0.75, 0.78, 0.85, 1.0],\n"
             "  \"metallic\": 0.1,\n"
             "  \"roughness\": 0.45,\n"
             "  \"ao\": 1.0,\n"
             "  \"emission\": [0.0, 0.0, 0.0],\n"
             "  \"opacity\": 1.0\n"
             "}\n";
    case 1:  // post preset
      return "{\n"
             "  \"name\": \"" + n + "\",\n"
             "  \"passes\": [\n"
             "    { \"name\": \"grade\", \"tonemap\": true, \"saturation\": 1.0, \"contrast\": 1.02, \"exposure\": 1.0 },\n"
             "    { \"name\": \"fxaa\" }\n"
             "  ]\n"
             "}\n";
    case 2:  // scene graph json
      return "{\n"
             "  \"name\": \"" + n + "\",\n"
             "  \"type\": \"empty\",\n"
             "  \"pos\": [0, 0, 0],\n"
             "  \"rot\": [0, 0, 0, 1],\n"
             "  \"scale\": [1, 1, 1],\n"
             "  \"children\": []\n"
             "}\n";
    case 3:  // quad shader (reads the shared NullBlock - works with `shader NAME`)
      return "#version 300 es\n"
             "// " + n + ".frag - quad shader template. Use it with `shader " + n +
             "` in demo.nsd, or create it and hit Show in viewport.\n"
             "// Reads the shared NullBlock (uRes/uTime/uIntensity/...).\n"
             "#include <common>\n"
             "\n"
             "out vec4 fragColor;\n"
             "\n"
             "void main() {\n"
             "  vec2 uv = gl_FragCoord.xy / Null.uRes;\n"
             "  fragColor = vec4(uv, 0.5 + 0.5 * sin(Null.uTime), 1.0);\n"
             "}\n";
    default:  // shadertoy
      return "// " + n + ".glsl - Shadertoy import template (paste code from shadertoy.com).\n"
             "// The importer maps mainImage() and the i* uniforms (iResolution, iTime,\n"
             "// iFrame, iMouse, iChannel0-3) automatically. Multi-pass shaders: add\n"
             "// `// pass: <name>` markers above each pass block.\n"
             "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
             "  vec2 uv = fragCoord / iResolution.xy;\n"
             "  fragColor = vec4(uv, 0.5 + 0.5 * sin(iTime), 1.0);\n"
             "}\n";
  }
}

std::string DemoEditor::sanitizeAssetName(const char* in) {
  std::string out;
  if (!in) return out;
  for (; *in; ++in) {
    const char c = *in;
    if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
      out += c;
  }
  return out;
}

void DemoEditor::insertShaderLabIntoTimeline() {
  const std::string snippet = shaderLab_.timelineSnippet();
  const std::string path = shaderLab_.exportedShaderPath();
  if (snippet.empty() || path.empty() || !w_.app || doc_.path.empty()) {
    Log::warn("SHADERLAB", "Insert into Timeline requires a saved project and shader asset");
    return;
  }
  Cmd c;
  c.name = "shader";
  c.args.push_back(Value(std::filesystem::path(path).filename().string()));
  c.opts = Value::object();
  // The exported GLSL already contains the selected text as baked glyph
  // geometry. Keep only the atlas binding flag here; adding a `text` option is
  // redundant and can make the runtime treat the export as a second text
  // source when it is inserted into a scene that already has text nodes.
  c.opts.set("useFont") = Value(true);
  const std::string font = shaderLab_.selectedFontPath();
  if (!font.empty())
    c.opts.set("font") = Value("assets/fonts/" +
                                std::filesystem::path(font).filename().string());
  const std::string texture = shaderLab_.selectedTexturePath();
  if (!texture.empty()) {
    c.opts.set("texture") = Value(texture);
    c.opts.set("textureMix") = Value((double)shaderLab_.textureMix());
    c.opts.set("textureScale") = Value((double)shaderLab_.textureScale());
    c.opts.set("textureScroll") = Value((double)shaderLab_.textureScroll());
  }
  ScriptBlock b;
  b.time = w_.director ? w_.director->show : 0.0f;
  b.cmds.push_back(std::move(c));
  doc_.beginEdit("insert shader lab effect");
  doc_.ast.main.push_back(std::move(b));
  std::sort(doc_.ast.main.begin(), doc_.ast.main.end(),
            [](const ScriptBlock& a, const ScriptBlock& b) { return a.time < b.time; });
  doc_.endEdit();
  if (writeDocument()) {
    ImGui::SetClipboardText(snippet.c_str());
    Log::info("SHADERLAB", "inserted " + std::filesystem::path(path).filename().string() +
                             " at " + fmtTime(w_.director ? w_.director->show : 0.0f) +
                             " (snippet copied to clipboard)");
  }
}

}  // namespace ns
