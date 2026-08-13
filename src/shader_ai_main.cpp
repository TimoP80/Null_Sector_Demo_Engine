// ---------------------------------------------------------------------------
// NULL SECTOR // AI SHADER GENERATOR
// Standalone demoscene shader workstation. It reuses the engine's real Shader,
// VFS and fullscreen-triangle path; exported files remain portable GLSL.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#pragma warning(disable:4005) // GLFW and Win32 headers both define APIENTRY.
#endif

#include "shader_ai.hpp"
#include "shadertoy_convert.hpp"
#include "engine/audio.hpp"
#include "engine/framebuffer.hpp"
#include "engine/gl.hpp"
#include "engine/mesh.hpp"
#include "engine/paths.hpp"
#include "engine/renderprobe.hpp"
#include "engine/shader.hpp"
#include "engine/texture.hpp"
#include "framework/vfs/directoryfs.hpp"
#include "framework/vfs/vfs.hpp"

// ns_shader_ai is a standalone target that does not compile engine/assets.cpp,
// so the stb_image implementation is provided here (same pattern as assets.cpp).
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <commdlg.h>
#endif

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ns {
namespace {

double nowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string sanitizeFilename(std::string s) {
  std::string out;
  for (char c : s) {
    if (std::isalnum((unsigned char)c) || c == '_' || c == '-') out += (char)std::tolower((unsigned char)c);
  }
  return out.empty() ? "generated_effect" : out;
}

std::vector<char> textBuffer(const std::string& s, size_t cap = 131072) {
  std::vector<char> out(s.begin(), s.end());
  out.push_back(0);
  out.resize(cap + 1, 0);
  return out;
}

std::string readText(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// ---------------------------------------------------------------------------
// Shadertoy channel texture helpers
// ---------------------------------------------------------------------------

/** one background download of a channel texture URL (deduplicated by
 *  destPath, so several channels can share one cached file) */
struct ChannelDownloadTask {
  std::string url;
  std::string destPath;
  std::vector<int> channels;
};
struct ChannelDownloadResult {
  std::string destPath;
  bool ok = false;
  std::string error;
};
struct ChannelDownloadBatchResult {
  int batch = 0;
  std::vector<ChannelDownloadResult> results;
};

/** per-channel download lifecycle, shown in the Texture Channels section */
enum class ChannelDownloadState { None, Pending, Ok, Failed };

uint64_t sourceSignature(const std::string& source);  // defined below

// Channel texture downloads retry transient failures (timeouts, transport
// errors, 5xx) with a short backoff before a channel is marked failed.
// HTTP 4xx responses are permanent - retrying them would never succeed.
// The attempt count and backoff are tunable in Provider settings.

/** run one download attempt function with a short backoff between retries.
 *  Stops early when *cancel is set (reports "cancelled") or the failure is
 *  permanent (HTTP 4xx). The backoff sleep happens *before* each retry and is
 *  checked in small steps so a cancel click interrupts it quickly. onAttempt
 *  (if given) fires with (attempt, maxAttempts) before each attempt, so the
 *  UI can show "retrying N/M..." during both the backoff and the fetch. The
 *  downloader is injectable so the retry/backoff/cancel logic is testable
 *  offline. */
template <typename Downloader>
bool runChannelDownload(Downloader download, const std::atomic<bool>* cancel,
                        std::string* error, int maxAttempts, int backoffMs) {
  return runChannelDownload(download, cancel, error, maxAttempts, backoffMs,
                            [](int, int) {});
}

template <typename Downloader, typename AttemptFn>
bool runChannelDownload(Downloader download, const std::atomic<bool>* cancel,
                        std::string* error, int maxAttempts, int backoffMs,
                        AttemptFn onAttempt) {
  for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
    if (cancel && cancel->load()) { if (error) *error = "cancelled"; return false; }
    // report the active attempt first: the value persists for the whole
    // attempt, so the row shows "retrying N/M..." during both the backoff
    // (attempt > 1 has a growing wait *before* its fetch) and the fetch
    onAttempt(attempt, maxAttempts);
    if (attempt > 1) {
      const int total = backoffMs * (attempt - 1);
      const int step = 100;
      for (int waited = 0; waited < total; waited += step) {
        if (cancel && cancel->load()) { if (error) *error = "cancelled"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(step, total - waited)));
      }
      if (cancel && cancel->load()) { if (error) *error = "cancelled"; return false; }
    }
    std::string err;
    const bool ok = download(err);
    if (ok) return true;
    if (error) *error = err;
    if (cancel && cancel->load()) return false;               // cancelled mid-download
    if (err.rfind("HTTP 4", 0) == 0) return false;           // 4xx is permanent
  }
  return false;
}

/** cache filename for a downloaded channel texture: the last URL path
 *  segment, sanitized; hash fallback when the URL ends in a slash. */
std::string channelTextureFilename(const std::string& url) {
  std::string path = url;
  const size_t q = path.find('?');
  if (q != std::string::npos) path.resize(q);
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  for (char& c : name)
    if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) c = '_';
  if (name.empty() || name == "." || name == "..") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "channel_%016llX", (unsigned long long)sourceSignature(url));
    name = std::string(buf) + ".img";
  }
  if (name.size() > 120) name.resize(120);
  return name;
}

/** which uChannel0..3 samplers does the current fragment actually use? */
std::array<bool, 4> usedSamplerChannels(const std::string& source) {
  std::array<bool, 4> out = {};
  for (int ci = 0; ci < 4; ++ci) {
    const std::string decl = "sampler2D uChannel" + std::to_string(ci);
    const std::string call = "texture(uChannel" + std::to_string(ci);
    if (source.find(decl) != std::string::npos || source.find(call) != std::string::npos)
      out[(size_t)ci] = true;
  }
  return out;
}

/** deterministic tileable value-noise "clouds" texture - the default channel
 *  binding. Wraps so shaders can sample uv beyond [0,1] (Shadertoy default). */
std::vector<unsigned char> makeChannelNoisePixels(int size) {
  const int octaves = 4, baseLattice = 8;
  uint32_t seed = 0x9E3779B9u;
  auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFFFF; };
  std::array<int, 4> offs = {};
  int total = 0;
  for (int o = 0; o < octaves; ++o) {
    const int n = baseLattice << o;
    offs[(size_t)o] = total;
    total += (n + 1) * (n + 1);
  }
  std::vector<float> lattice((size_t)total);
  for (int o = 0; o < octaves; ++o) {
    const int n = baseLattice << o, span = n + 1, base = offs[(size_t)o];
    for (int i = 0; i < span * span; ++i) lattice[(size_t)base + i] = (float)(rnd() & 0xFFFF) / 65535.0f;
    // wrap: last column/row must tile into the first
    for (int y = 0; y < span; ++y) lattice[(size_t)base + y * span + n] = lattice[(size_t)base + y * span];
    for (int x = 0; x < span; ++x) lattice[(size_t)base + n * span + x] = lattice[(size_t)base + x];
  }
  std::vector<unsigned char> px((size_t)size * size * 4, 0);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float u = (float)x / (float)size, v = (float)y / (float)size;
      float sum = 0.0f, ampSum = 0.0f, amp = 1.0f;
      for (int o = 0; o < octaves; ++o) {
        const int n = baseLattice << o, span = n + 1, base = offs[(size_t)o];
        const float gx = u * (float)n, gy = v * (float)n;
        const int ix = (int)gx % n, iy = (int)gy % n;
        const float fx = gx - (float)(int)gx, fy = gy - (float)(int)gy;
        const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        const float a = lattice[(size_t)base + iy * span + ix];
        const float b = lattice[(size_t)base + iy * span + ix + 1];
        const float c = lattice[(size_t)base + (iy + 1) * span + ix];
        const float d = lattice[(size_t)base + (iy + 1) * span + ix + 1];
        sum += amp * (a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy);
        ampSum += amp;
        amp *= 0.5f;
      }
      const float nval = sum / ampSum;  // 0..1 muted blue-gray clouds
      const size_t i = ((size_t)y * size + (size_t)x) * 4;
      px[i] = (unsigned char)(14.0f + 40.0f * nval);
      px[i + 1] = (unsigned char)(20.0f + 46.0f * nval);
      px[i + 2] = (unsigned char)(34.0f + 62.0f * nval);
      px[i + 3] = 255;
    }
  }
  return px;
}

uint64_t sourceSignature(const std::string& source) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : source) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string sourceSignatureText(const std::string& source) {
  char text[32];
  std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)sourceSignature(source));
  return text;
}

std::string usefulSourceLine(const std::string& source, bool last) {
  std::istringstream in(source);
  std::string line, selected;
  while (std::getline(in, line)) {
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    line = line.substr(first);
    if (!last) { selected = line; break; }
    selected = line;
  }
  if (selected.size() > 100) selected.resize(100);
  return selected.empty() ? "<empty>" : selected;
}

std::string shaderAiSettingsPath(const std::string& dataDir) {
#ifdef _WIN32
  if (const char* appData = std::getenv("APPDATA")) {
    if (*appData) return (std::filesystem::path(appData) / "NullSector" / "shader_ai_settings.json").string();
  }
#endif
  return (std::filesystem::path(dataDir) / "shader_ai_settings.json").string();
}

struct ModelRefreshResult {
  std::vector<AvailableModel> models;
  std::string error;
};

#ifdef _WIN32
std::string nativeSaveDialog(const char* filter, const char* initial, const char* title) {
  char file[MAX_PATH] = {};
  std::snprintf(file, sizeof(file), "%s", initial ? initial : "effect");
  OPENFILENAMEA ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = file;
  ofn.nMaxFile = sizeof(file);
  ofn.lpstrFilter = filter;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  return GetSaveFileNameA(&ofn) ? file : std::string();
}
std::string nativeOpenDialog(const char* filter, const char* title) {
  char file[MAX_PATH] = {};
  OPENFILENAMEA ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = file;
  ofn.nMaxFile = sizeof(file);
  ofn.lpstrFilter = filter;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  return GetOpenFileNameA(&ofn) ? file : std::string();
}
#endif

class ShaderAiApp {
public:
  ShaderAiApp(GLFWwindow* window, std::string shaderDir, std::string dataDir)
      : window_(window), shaderDir_(std::move(shaderDir)), dataDir_(std::move(dataDir)) {
    fsTriangle_ = std::make_unique<Mesh>(fullscreenTriangle());
    audioReady_ = audio_.init();
    fragBuf_ = textBuffer("");
    vertBuf_ = textBuffer("");
    promptBuf_ = textBuffer("Create a dark cyberpunk tunnel made from glowing procedural cables. Camera moves forward continuously. React strongly to kick and bass with cyan and violet colors.", 8192);
    specBuf_ = textBuffer("", 8192);
    savePathBuf_ = textBuffer((std::filesystem::path(shaderDir_) / "generated_effect.frag").string(), 1024);
    projectPath_ = (std::filesystem::path(dataDir_) / "shader_ai_project.nsshad").string();
    projectPathBuf_ = textBuffer(projectPath_, 1024);
    settingsPath_ = shaderAiSettingsPath(dataDir_);
    std::string settingsError;
    if (!loadShaderAiSettings(settingsPath_, providerConfig_, &settingsError) &&
        std::filesystem::exists(settingsPath_)) {
      modelStatus_ = "Settings load failed: " + settingsError;
    }
    availableModels_.push_back({providerConfig_.model, "built-in"});
    loadDefault();
  }

  ~ShaderAiApp() { shutdown(); }

  void shutdown() {
    if (!imguiReady_ && !validProgram_ && !preview_.fbo && !fsTriangle_) return;
    if (generationFuture_.valid()) {
      generationFuture_.wait();
      try { generationFuture_.get(); } catch (...) { /* diagnostics are UI-only during shutdown */ }
      generationInFlight_ = false;
    }
    if (modelFuture_.valid()) {
      modelFuture_.wait();
      try { modelFuture_.get(); } catch (...) { /* diagnostics are UI-only during shutdown */ }
      modelRefreshInFlight_ = false;
    }
    if (channelDownloadFuture_.valid()) {
      channelDownloadFuture_.wait();
      try { channelDownloadFuture_.get(); } catch (...) { /* download results are UI-only */ }
      channelDownloadInFlight_ = false;
      pendingChannelTasks_.clear();
    }
    validProgram_.reset();
    preview_.destroy();
    if (fsTriangle_) { fsTriangle_->destroy(); fsTriangle_.reset(); }
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(shaderDir_) / ".shader_ai_preview.frag", ec);
    std::filesystem::remove(std::filesystem::path(shaderDir_) / ".shader_ai_preview.vert", ec);
    shutdownImGui();
  }

  void initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f; style.FrameRounding = 4.0f; style.ChildRounding = 4.0f;
    style.WindowPadding = ImVec2(10, 8); style.FramePadding = ImVec2(8, 4);
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imguiReady_ = true;
  }

  void shutdownImGui() {
    if (!imguiReady_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    imguiReady_ = false;
  }

  void loadDefault() {
    GenerationRequest req;
    req.kind = kind_;
    req.prompt = promptBuf_.data();
    GeneratedShader g = BuiltinDemosceneProvider().generate(req);
    applyGenerated(g, "Version 1");
    compileNow();
  }

  void traceSource(const char* stage, const std::string& source) const {
    std::printf("[SHADER-AI][TRACE] Generate #%d %s: %zu bytes hash=%s first=\"%s\" last=\"%s\"\n",
                generationSerial_, stage, source.size(), sourceSignatureText(source).c_str(),
                usefulSourceLine(source, false).c_str(), usefulSourceLine(source, true).c_str());
    std::fflush(stdout);
  }

  void applyGenerated(const GeneratedShader& g, const std::string& label) {
    const std::string before = fragment_;
    if (!fragment_.empty()) {
      ShaderAiVersion v;
      // The source being preserved is the CURRENT version, so the history
      // entry is labeled with what it actually contains - not with the
      // incoming generation's label, which belongs to the source about to
      // become current (that was an off-by-one history-labeling bug).
      v.label = currentVersionLabel_;
      v.prompt = prompt_; v.fragment = fragment_; v.vertex = vertex_; v.specification = specification_;
      history_.push_back(std::move(v));
    }
    currentVersionLabel_ = label;
    kind_ = g.kind;
    fragment_ = g.fragment;
    vertex_ = g.vertex;
    specification_ = g.specification;
    explanation_ = g.explanation;
    prompt_ = promptBuf_.data();
    // a new generated document has no outstanding channel downloads
    channelDownloadState_ = {};
    channelDownloadUrl_ = {};
    channelDownloadError_ = {};
    resetChannelDownloadProgress();
    fragBuf_ = textBuffer(fragment_);
    vertBuf_ = textBuffer(vertex_);
    specBuf_ = textBuffer(specification_, 8192);
    params_ = parseShaderParams(fragment_);
    ++sourceWidgetRevision_;
    sourceDirty_ = true;
    compileQueued_ = true;
    status_ = label + " generated - compiling";
    std::printf("[SHADER-AI][TRACE] Generate #%d editor assignment: YES before=%s after=%s widget-revision=%llu\n",
                generationSerial_, sourceSignatureText(before).c_str(), sourceSignatureText(fragment_).c_str(),
                (unsigned long long)sourceWidgetRevision_);
    std::printf("[SHADER-AI][TRACE] Generate #%d editor buffer after assignment: hash=%s bytes=%zu\n",
                generationSerial_, sourceSignatureText(fragBuf_.data()).c_str(), fragBuf_.size());
    traceSource("editor source after assignment", fragment_);
    diagnostics_.clear();
    selectedHistory_ = -1;
  }

  void syncSourcesFromUi() {
    fragment_ = fragBuf_.data();
    vertex_ = vertBuf_.data();
    std::printf("[SHADER-AI][TRACE] Generate #%d source widget changed document: hash=%s\n",
                generationSerial_, sourceSignatureText(fragment_).c_str());
    params_ = parseShaderParams(fragment_);
    sourceDirty_ = true;
    compileQueued_ = true;
    editClock_ = nowSeconds();
  }

  /** Probe the compiled program through the shared engine RenderProbe: render
   *  to a tiny target at two non-zero instants and read the pixels back. The
   *  result classifies never-drew / uniform / time-only / near-black outputs.
   *  Sample times avoid t=0 so a spatial term multiplied by sin(uTime) is not
   *  misread as flat. Binds samplers exactly like renderPreview so a
   *  channel-only shader is not misread as flat because its unit is unbound. */
  RenderProbeResult runOutputProbe() {
    if (!validProgram_ || !fsTriangle_) {
      RenderProbeResult inconclusive;  // fboOk=false: no verdict, never "degenerate"
      inconclusive.fboOk = false;
      return inconclusive;
    }
    const int W = 64, H = 64;
    lastProbeW_ = W;
    lastProbeH_ = H;
    const float bpm = 128.0f;
    return probeRender(W, H, {0.37f, 1.13f}, [&](float t) {
      validProgram_->use();
      validProgram_->set2f("uResolution", (float)W, (float)H);
      const float beat = t * bpm / 60.0f;
      const float bar = beat / 4.0f;
      const float phase = beat - std::floor(beat);
      auto set = [&](const char* n, float v) { validProgram_->set1f(n, v); };
      set("uTime", t); set("uBPM", bpm); set("uBeat", beat); set("uBar", bar); set("uBeatPhase", phase);
      set("uAudioLevel", 0.1f); set("uBass", 0.1f); set("uMid", 0.1f); set("uTreble", 0.1f);
      set("uKick", 0.05f); set("uSnare", 0.05f);
      validProgram_->set4f("uColor", 0.0f, 0.85f, 1.0f, 1.0f);
      validProgram_->set4f("uColor2", 0.8f, 0.05f, 0.75f, 1.0f);
      set("uIntensity", 1.0f); set("uSpeed", 1.0f); set("uScale", 1.0f);
      validProgram_->set1f("uTimeDelta", 0.016f);
      validProgram_->set1i("uFrame", 1);
      validProgram_->set4f("uMouse", 0.0f, 0.0f, 0.0f, 0.0f);
      validProgram_->set4f("uDate", 2026.0f, 8.0f, 10.0f, 0.0f);
      validProgram_->set4f("uChannelTime", t, t, t, t);
      for (int ci = 0; ci < 4; ++ci) {
        char n[40]; std::snprintf(n, sizeof(n), "uChannelResolution[%d]", ci);
        validProgram_->set4f(n, (float)W, (float)H, 1.0f, 1.0f);
      }
      for (const ShaderParamDecl& p : params_) {
        const char* n = p.name.c_str();
        if (p.type == ShaderParamType::Float || p.type == ShaderParamType::Int || p.type == ShaderParamType::Bool)
          set(n, p.value);
        else if (p.type == ShaderParamType::Color)
          validProgram_->set4f(n, p.vector[0], p.vector[1], p.vector[2], p.vector[3]);
        else if (p.type == ShaderParamType::Vec2)
          validProgram_->set2f(n, p.vector[0], p.vector[1]);
        else if (p.type == ShaderParamType::Vec3)
          validProgram_->set3f(n, p.vector[0], p.vector[1], p.vector[2]);
        else if (p.type == ShaderParamType::Vec4)
          validProgram_->set4f(n, p.vector[0], p.vector[1], p.vector[2], p.vector[3]);
      }
      // bind samplers exactly like renderPreview so a channel-only shader is
      // not misread as flat because its texture unit is unbound
      ensureDefaultChannelTexture();
      for (int ci = 0; ci < 4; ++ci) {
        if (!channelUsed_[(size_t)ci]) continue;
        char n[32]; std::snprintf(n, sizeof(n), "uChannel%d", ci);
        const int loc = ::glGetUniformLocation(validProgram_->id(), n);
        if (loc < 0) continue;
        const Texture& tex = channelTex_[(size_t)ci].tex ? channelTex_[(size_t)ci] : defaultChannelTex_;
        ::glUniform1i(loc, ci);
        tex.bind(ci);
      }
      ::glActiveTexture(::gl::TEXTURE0);
      fsTriangle_->draw(3);
    });
  }

  bool compileNow() {
    traceSource("source immediately before preview compilation", fragment_);
    std::printf("[SHADER-AI][TRACE] Generate #%d compiler input hash=%s\n",
                generationSerial_, sourceSignatureText(fragment_).c_str());
    std::filesystem::create_directories(shaderDir_);
    const std::string fragFile = ".shader_ai_preview.frag";
    const std::string vertFile = ".shader_ai_preview.vert";
    const std::filesystem::path fragPath = std::filesystem::path(shaderDir_) / fragFile;
    const std::filesystem::path vertPath = std::filesystem::path(shaderDir_) / vertFile;
    std::ofstream f(fragPath, std::ios::binary | std::ios::trunc);
    if (!f) { status_ = "Cannot write preview fragment"; return false; }
    f << fragment_; f.close();
    const bool customVertex = kind_ == ShaderKind::Vertex || kind_ == ShaderKind::Pair;
    if (customVertex) {
      std::ofstream v(vertPath, std::ios::binary | std::ios::trunc);
      if (!v) { status_ = "Cannot write preview vertex"; return false; }
      v << vertex_; v.close();
    }
    try {
      const unsigned previousProgram = validProgram_ ? validProgram_->id() : 0;
      auto next = std::make_unique<Shader>(customVertex ? vertFile : "fullscreen.vert", fragFile);
      validProgram_ = std::move(next);
      validFragment_ = fragment_;
      validVertex_ = vertex_;
      lastCompiled_ = fragment_;
      channelUsed_ = usedSamplerChannels(validFragment_);
      sourceDirty_ = false;
      compileQueued_ = false;
      invalidatePreview();
      previewTime_ = 0.0f;
      // A shader that compiles is not necessarily a shader that shows
      // anything: classify the rendered output (never drew / uniform /
      // time-only / near-black) via the shared engine RenderProbe and surface
      // degenerate frames instead of accepting them silently.
      lastProbe_ = runOutputProbe();
      flatOutput_ = lastProbe_.uniform;  // amber status for the uniform-color class
      repairImageDataUrl_.clear();
      if (lastProbe_.degenerate() && !lastProbe_.pixels.empty() && lastProbeW_ > 0 && lastProbeH_ > 0)
        repairImageDataUrl_ = encodePngDataUrl(lastProbe_.pixels.data(), lastProbeW_, lastProbeH_);
      if (lastProbe_.degenerate()) {
        diagnostics_ = "Preview check: " + lastProbe_.diagnosis() +
                       " Add spatial variation, e.g. declare `in vec2 vUV;` and use it, or "
                       "compute `vec2 uv = gl_FragCoord.xy / uResolution;`, and make the output "
                       "depend on `uv` (not only on uTime).";
        status_ = "Compiled successfully - warning: renders a degenerate frame (see the "
                  "Diagnostics tab) - edit the source or Ask AI to Fix";
      } else {
        diagnostics_.clear();
        status_ = "Compiled successfully - previous valid preview replaced";
      }
      std::printf("[SHADER-AI][TRACE] Generate #%d compile: SUCCESS program=%u previous=%u preview-program: REPLACED\n",
                  generationSerial_, validProgram_->id(), previousProgram);
      std::fflush(stdout);
      return true;
    } catch (const std::exception& e) {
      diagnostics_ = e.what();
      status_ = "Compilation failed - previous valid shader retained";
      compileQueued_ = false;
      std::printf("[SHADER-AI][TRACE] Generate #%d compile: FAILED source-hash=%s\n",
                  generationSerial_, sourceSignatureText(fragment_).c_str());
      std::fflush(stdout);
      return false;
    }
  }

  void renderPreview() {
    if (!validProgram_ || !fsTriangle_) return;
    if (preview_.w != previewW_ || preview_.h != previewH_) {
      preview_ = FrameTarget::color(previewW_, previewH_, ::gl::RGBA8, ::gl::RGBA, ::gl::UNSIGNED_BYTE,
                                    {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false});
    }
    preview_.bind();
    ::glDisable(::gl::BLEND); ::glDisable(::gl::DEPTH_TEST);
    ::glClearColor(0.002f, 0.004f, 0.012f, 1.0f); ::glClear(::gl::COLOR_BUFFER_BIT);
    validProgram_->use();
    const float bpm = 128.0f;
    const float beat = previewTime_ * bpm / 60.0f;
    const float bar = beat / 4.0f;
    const float phase = beat - std::floor(beat);
    auto set = [&](const char* n, float v) { validProgram_->set1f(n, v); };
    validProgram_->set2f("uResolution", (float)previewW_, (float)previewH_);
    set("uTime", previewTime_); set("uBPM", bpm); set("uBeat", beat); set("uBar", bar); set("uBeatPhase", phase);
    const float audioLevel = audioLoaded_ ? audio_.react.energy.load() : simulatedAudio_[0];
    const float bass = audioLoaded_ ? audio_.react.bass.load() : simulatedAudio_[1];
    const float mid = audioLoaded_ ? audio_.react.mid.load() : simulatedAudio_[2];
    const float treble = audioLoaded_ ? audio_.react.treble.load() : simulatedAudio_[3];
    const float kick = audioLoaded_ ? audio_.react.kick.load() : simulatedAudio_[4];
    const float snare = audioLoaded_ ? audio_.react.onset.load() : simulatedAudio_[5];
    set("uAudioLevel", audioLevel); set("uBass", bass); set("uMid", mid); set("uTreble", treble);
    set("uKick", kick); set("uSnare", snare);
    validProgram_->set4f("uColor", 0.0f, 0.85f, 1.0f, 1.0f);
    validProgram_->set4f("uColor2", 0.8f, 0.05f, 0.75f, 1.0f);
    set("uIntensity", 1.0f); set("uSpeed", previewSpeed_); set("uScale", 1.0f);
    // Shadertoy-converted shaders declare these extras - bind them so folded
    // buffers and iMouse/iFrame/iDate-consuming code preview realistically.
    validProgram_->set1f("uTimeDelta", (float)delta_);
    validProgram_->set1i("uFrame", previewFrame_);
    validProgram_->set4f("uMouse", 0.0f, 0.0f, 0.0f, 0.0f);
    validProgram_->set4f("uDate", 2026.0f, 8.0f, 10.0f, 0.0f);
    validProgram_->set4f("uChannelTime", previewTime_, previewTime_, previewTime_, previewTime_);
    for (int ci = 0; ci < 4; ++ci) {
      char n[40]; std::snprintf(n, sizeof(n), "uChannelResolution[%d]", ci);
      validProgram_->set4f(n, (float)previewW_, (float)previewH_, 1.0f, 1.0f);
    }
    // bind the shader's uChannel0..3 samplers: a user-loaded image if one is
    // set for the channel, otherwise a tileable procedural noise texture (the
    // old 2x2 checkerboard placeholder made every channel-using import render
    // a giant checkerboard, so it is gone - unbound channels now read as
    // neutral noise, which is what most Shadertoy shaders expect from
    // iChannel0/1 anyway).
    ensureDefaultChannelTexture();
    for (int ci = 0; ci < 4; ++ci) {
      if (!channelUsed_[(size_t)ci]) continue;
      char n[32]; std::snprintf(n, sizeof(n), "uChannel%d", ci);
      const int loc = ::glGetUniformLocation(validProgram_->id(), n);
      if (loc < 0) continue;
      const Texture& tex = channelTex_[(size_t)ci].tex ? channelTex_[(size_t)ci] : defaultChannelTex_;
      ::glUniform1i(loc, ci);
      tex.bind(ci);
    }
    ::glActiveTexture(::gl::TEXTURE0);
    previewFrame_++;
    for (const ShaderParamDecl& p : params_) {
      const char* n = p.name.c_str();
      if (p.type == ShaderParamType::Float || p.type == ShaderParamType::Int || p.type == ShaderParamType::Bool)
        set(n, p.value);
      else if (p.type == ShaderParamType::Color)
        validProgram_->set4f(n, p.vector[0], p.vector[1], p.vector[2], p.vector[3]);
      else if (p.type == ShaderParamType::Vec2)
        validProgram_->set2f(n, p.vector[0], p.vector[1]);
      else if (p.type == ShaderParamType::Vec3)
        validProgram_->set3f(n, p.vector[0], p.vector[1], p.vector[2]);
      else if (p.type == ShaderParamType::Vec4)
        validProgram_->set4f(n, p.vector[0], p.vector[1], p.vector[2], p.vector[3]);
    }
    fsTriangle_->draw(3);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  }

  void pollGeneration() {
    if (!generationInFlight_ || !generationFuture_.valid()) return;
    if (generationFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    generationInFlight_ = false;
    try {
      GeneratedShader g = generationFuture_.get();
      std::printf("[SHADER-AI][TRACE] Generate #%d RESULT RECEIVED response=%zu bytes hash=%s preview=\\\"%s\\\"", generationSerial_, g.responseBytes, g.responseHash.c_str(), g.responsePreview.c_str());
      std::putchar('\n');
      std::fflush(stdout);
      traceSource("extracted GLSL", g.fragment);
      applyGenerated(g, "Version " + std::to_string((int)history_.size() + 1));
      std::printf("[SHADER-AI][TRACE] Generate #%d RESULT APPLIED source-hash=%s", generationSerial_, sourceSignatureText(fragment_).c_str());
      std::putchar('\n');
      std::fflush(stdout);
      status_ = "Shader received (" + std::to_string(g.fragment.size()) + " fragment bytes) - compiling";
      if (compileNow()) {
        conversation_ = explanation_;
        // Severity-driven self-heal: a FRESH generation whose output is in any
        // degenerate class (never drew / uniform / time-only / near-black) is
        // re-asked up to providerConfig_.autoRepairMax times before falling
        // back to the warning. The diagnosis is still in diagnostics_, so each
        // repair prompt carries it and the model gets a real chance to fix it.
        // A repair's own result never re-triggers the chain (generationIsRepair_).
        const bool degenerate = lastProbe_.degenerate();
        if (degenerate && !generationIsRepair_ && providerConfig_.autoRepairEnabled &&
            autoRepairsUsed_ < std::max(0, providerConfig_.autoRepairMax)) {
          ++autoRepairsUsed_;
          std::printf("[SHADER-AI][TRACE] Generate #%d degenerate-output auto-repair: re-asking %d/%d (%s)\n",
                      generationSerial_, autoRepairsUsed_, providerConfig_.autoRepairMax,
                      lastProbe_.diagnosis().c_str());
          std::fflush(stdout);
          generate(true, /*autoRepair=*/true);
          status_ = "Generated shader renders degenerate output - auto-repairing (" +
                    std::to_string(autoRepairsUsed_) + "/" +
                    std::to_string(providerConfig_.autoRepairMax) + ")...";
          return;
        }
        if (degenerate) {
          status_ = "Applied generated shader (request #" + std::to_string(generationSerial_) + ")" +
                    (autoRepairsUsed_ > 0
                         ? " (auto-repaired " + std::to_string(autoRepairsUsed_) + "x - " +
                               std::to_string(autoRepairsUsed_ + 1) + " requests)"
                         : "") +
                    " - warning: renders a degenerate frame - Ask AI to Fix or edit the source";
        } else {
          status_ = "Applied generated shader (request #" + std::to_string(generationSerial_) + ")" +
                    (autoRepairsUsed_ > 0
                         ? " (auto-repaired " + std::to_string(autoRepairsUsed_) + "x - " +
                               std::to_string(autoRepairsUsed_ + 1) + " requests)"
                         : "");
        }
      }
    } catch (const std::exception& e) {
      std::printf("[SHADER-AI][TRACE] Generate #%d RESULT FAILED: %s", generationSerial_, e.what());
      std::putchar('\n');
      std::fflush(stdout);
      status_ = std::string("AI provider failed: ") + e.what();
      diagnostics_ = e.what();
    }
  }

  void generate(bool repair = false, bool autoRepair = false) {
    const int clickId = generationSerial_ + 1;
    std::printf("[SHADER-AI][TRACE] Generate #%d GENERATE CLICKED action=%s%s", clickId,
                repair ? "repair" : "generate", autoRepair ? " (auto)" : "");
    std::putchar('\n');
    std::fflush(stdout);
    if (generationInFlight_) {
      status_ = "A shader generation request is already running";
      return;
    }
    GenerationRequest req;
    req.kind = kind_; req.prompt = promptBuf_.data(); req.currentFragment = fragment_; req.currentVertex = vertex_;
    req.diagnostics = diagnostics_; req.config = providerConfig_;
    if (!repair) req.staticHint = spatialLintHint(fragment_);
    if (repair && providerConfig_.sendRepairImage && !repairImageDataUrl_.empty())
      req.repairImageDataUrl = repairImageDataUrl_;
    const int requestId = ++generationSerial_;
    req.generationId = requestId;
    std::printf("[SHADER-AI][TRACE] Generate #%d REQUEST STARTED provider=%s model=%s prompt-length=%zu", requestId, providerConfig_.provider.c_str(), providerConfig_.model.c_str(), req.prompt.size());
    std::putchar('\n');
    std::fflush(stdout);
    prompt_ = req.prompt;
    provider_ = makeShaderAiProvider(providerConfig_);
    status_ = repair ? "Asking AI to repair shader..." : "Generating shader...";
    diagnostics_.clear();
    // The auto-repair chain counter: a fresh generation (or a manual repair)
    // starts a new chain at 0; the auto path bumps it so the status line can
    // show "auto-repair 1/N" and the final request count.
    if (!repair || !autoRepair) autoRepairsUsed_ = 0;
    generationIsRepair_ = repair;
    generationInFlight_ = true;
    generationFuture_ = std::async(std::launch::async, [req = std::move(req), repair]() {
      auto provider = makeShaderAiProvider(req.config);
      return repair ? provider->repair(req) : provider->generate(req);
    });
  }

  void restoreHistory(int index) {
    if (index < 0 || index >= (int)history_.size()) return;
    const ShaderAiVersion& v = history_[(size_t)index];
    fragment_ = v.fragment; vertex_ = v.vertex; specification_ = v.specification; prompt_ = v.prompt;
    promptBuf_ = textBuffer(prompt_); fragBuf_ = textBuffer(fragment_); vertBuf_ = textBuffer(vertex_); specBuf_ = textBuffer(specification_, 8192);
    ++sourceWidgetRevision_;
    params_ = parseShaderParams(fragment_); sourceDirty_ = true; compileQueued_ = false; compileNow();
    status_ = "Restored " + v.label;
    currentVersionLabel_ = v.label;
  }

  bool saveShaders(const std::string& path) {
    std::filesystem::path p(path);
    if (p.extension() != ".frag") p += ".frag";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { status_ = "Save failed: " + p.string(); return false; }
    f << fragment_; f.close();
    if (kind_ == ShaderKind::Vertex || kind_ == ShaderKind::Pair) {
      std::filesystem::path vp = p; vp.replace_extension(".vert");
      std::ofstream v(vp, std::ios::binary | std::ios::trunc); v << vertex_;
    }
    exportedPath_ = p.string(); sourceDirty_ = false; status_ = "Saved shader: " + p.string();
    return true;
  }

  bool saveProject(const std::string& path) {
    ShaderAiProject p; p.path = path; p.prompt = prompt_; p.specification = specification_; p.kind = kind_; p.fragment = fragment_; p.vertex = vertex_; p.previewSpeed = previewSpeed_; p.previewWidth = previewW_; p.previewHeight = previewH_; p.history = history_; p.channelTextures = channelPath_; p.channelUrls = channelDownloadUrl_;
    std::string error;
    if (!p.save(path, &error)) { status_ = "Project save failed: " + error; return false; }
    status_ = "Saved project: " + path; projectPath_ = path; return true;
  }

  bool loadProject(const std::string& path) {
    ShaderAiProject p; std::string error;
    if (!p.load(path, &error)) { status_ = "Project load failed: " + error; return false; }
    kind_ = p.kind; prompt_ = p.prompt; specification_ = p.specification; fragment_ = p.fragment; vertex_ = p.vertex; history_ = p.history; projectPath_ = path;
    currentVersionLabel_ = "Current";
    promptBuf_ = textBuffer(prompt_); specBuf_ = textBuffer(specification_, 8192); fragBuf_ = textBuffer(fragment_); vertBuf_ = textBuffer(vertex_); ++sourceWidgetRevision_; params_ = parseShaderParams(fragment_);
    previewSpeed_ = p.previewSpeed; previewW_ = p.previewWidth; previewH_ = p.previewHeight; sourceDirty_ = true; compileNow();
    // a freshly loaded document supersedes any previous channel download state
    channelDownloadState_ = {};
    channelDownloadUrl_ = {};
    channelDownloadError_ = {};
    resetChannelDownloadProgress();
    // bind cached textures, and queue re-downloads for channels whose cache
    // file is missing but whose source URL is known
    std::vector<ChannelDownloadTask> redownloads;
    for (int ci = 0; ci < 4; ++ci) {
      clearChannelTexture(ci);
      channelDownloadUrl_[(size_t)ci] = p.channelUrls[(size_t)ci];
      if (!p.channelTextures[(size_t)ci].empty()) {
        std::error_code ec;
        if (std::filesystem::exists(p.channelTextures[(size_t)ci], ec) && !ec) {
          if (loadChannelTextureFromFile(ci, p.channelTextures[(size_t)ci]))
            channelDownloadState_[(size_t)ci] = ChannelDownloadState::Ok;
          continue;
        }
      }
      if (!p.channelUrls[(size_t)ci].empty()) {
        const std::string dest =
            (std::filesystem::path(dataDir_) / "textures" / channelTextureFilename(p.channelUrls[(size_t)ci])).string();
        auto it = std::find_if(redownloads.begin(), redownloads.end(),
                               [&](const ChannelDownloadTask& t2) { return t2.destPath == dest; });
        if (it == redownloads.end()) redownloads.push_back({p.channelUrls[(size_t)ci], dest, {ci}});
        else it->channels.push_back(ci);
      }
    }
    if (!redownloads.empty()) {
      startChannelDownloads(redownloads);
      status_ = "Loaded project: " + path + " - re-downloading " +
                std::to_string(redownloads.size()) + " missing channel texture(s)";
    } else {
      status_ = "Loaded project: " + path;
    }
    return true;
  }

  void drawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::TextDisabled("NULL SECTOR"); ImGui::SameLine(); ImGui::Text("AI SHADER GENERATOR");
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Save Shader As...")) {
#ifdef _WIN32
        const std::string p = nativeSaveDialog("Fragment Shader (*.frag)\0*.frag\0All files\0*.*\0", "generated_effect.frag", "Save Shader");
        if (!p.empty()) saveShaders(p);
#else
        showSavePopup_ = true;
#endif
      }
      if (ImGui::MenuItem("Save Project")) saveProject(projectPath_);
      if (ImGui::MenuItem("Save Project As...")) {
#ifdef _WIN32
        const std::string p = nativeSaveDialog("Shader AI Project (*.nsshad)\0*.nsshad\0All files\0*.*\0", "shader_ai_project.nsshad", "Save Shader Project");
        if (!p.empty()) saveProject(p);
#else
        showProjectPopup_ = true;
#endif
      }
      if (ImGui::MenuItem("Open Project...")) {
#ifdef _WIN32
        const std::string p = nativeOpenDialog("Shader AI Project (*.nsshad)\0*.nsshad\0All files\0*.*\0", "Open Shader Project");
        if (!p.empty()) loadProject(p);
#else
        showProjectPopup_ = true;
#endif
      }
      if (ImGui::MenuItem("Import Shadertoy...", "Ctrl+I")) showShadertoyImport_ = true;
      if (ImGui::MenuItem("Copy GLSL")) ImGui::SetClipboardText(fragment_.c_str());
      if (ImGui::MenuItem("Copy NSD snippet")) ImGui::SetClipboardText(("shader " + std::filesystem::path(exportedPath_.empty() ? "generated_effect.frag" : exportedPath_).filename().string()).c_str());
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("AI")) {
      if (ImGui::MenuItem("Generate Shader", nullptr, false, !generationInFlight_)) generate(false);
      if (ImGui::MenuItem("Ask AI to Fix", nullptr, false, !diagnostics_.empty())) generate(true);
      if (ImGui::MenuItem("Enhance Prompt")) {
        specification_ = "Visual: procedural demoscene effect\nMotion: continuous camera and bar-synced color\nAudio: bass deformation, kick brightness\nTechnique: " + prompt_;
        specBuf_ = textBuffer(specification_, 8192);
        status_ = "Prompt specification prepared for review";
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Settings", nullptr, &showSettings_);
      ImGui::MenuItem("Fullscreen Preview", "F11", &fullscreenPreview_);
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }

  void drawPrompt() {
    ImGui::BeginChild("ai_prompt", ImVec2(300, 0), true);
    ImGui::SeparatorText("AI PROMPT");
    ImGui::TextDisabled("Describe appearance, motion, technique, palette, and audio response.");
    if (ImGui::InputTextMultiline("##prompt", promptBuf_.data(), promptBuf_.size(), ImVec2(-1, 112))) prompt_ = promptBuf_.data();
    const char* kinds[] = {"Fragment Shader", "Vertex Shader", "Vertex + Fragment"};
    int k = (int)kind_; if (ImGui::Combo("Shader type", &k, kinds, 3)) kind_ = (ShaderKind)k;
    if (generationInFlight_) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Generation in progress... the preview remains responsive");
    }
    const bool generationWasInFlight = generationInFlight_;
    if (generationWasInFlight) ImGui::BeginDisabled();
    if (ImGui::Button("Generate Shader", ImVec2(-1, 32))) generate(false);
    if (generationWasInFlight) ImGui::EndDisabled();
    if (ImGui::Button("Enhance specification", ImVec2(-1, 24))) {
      specification_ = "Visual: " + prompt_ + "\nMotion: continuous uTime animation\nAudio: bass -> deformation, kick -> impact\nTechnique: bounded realtime GLSL";
      specBuf_ = textBuffer(specification_, 8192);
    }
    ImGui::SeparatorText("Quick ideas");
    const char* ideas[] = {"Cyberpunk tunnel", "Audio plasma", "CRT terminal", "Raymarched landscape", "Neurofunk glitch", "SDF typography"};
    for (const char* idea : ideas) if (ImGui::SmallButton(idea)) {
      std::string text = std::string("Create a ") + idea + " with strong demoscene motion, cyan and violet palette, and kick/bass reactivity.";
      promptBuf_ = textBuffer(text, 8192); prompt_ = text;
    }
    ImGui::SeparatorText("Technical specification");
    if (ImGui::InputTextMultiline("##spec", specBuf_.data(), specBuf_.size(), ImVec2(-1, 145))) specification_ = specBuf_.data();
    ImGui::TextDisabled("Provider: %s", provider_ ? provider_->name() : providerConfig_.provider.c_str());
    ImGui::EndChild();
  }

  void invalidatePreview() {
    preview_.destroy();
    preview_.w = preview_.h = 0;
  }

  void refreshPreview() {
    if (compileQueued_ || sourceDirty_) compileNow();
    invalidatePreview();
    previewTime_ = 0.0f;
    status_ = validProgram_ ? "Live preview refreshed" : "Preview unavailable - compile a valid shader";
  }

  void drawPreview() {
    ImGui::BeginChild("ai_preview", ImVec2(0, 360), true);
    ImGui::SeparatorText("LIVE PREVIEW  //  16:9");
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = std::max(320.0f, avail.x); float h = w * 9.0f / 16.0f;
    if (h > avail.y - 54) { h = std::max(180.0f, avail.y - 54); w = h * 16.0f / 9.0f; }
    if (preview_.colorTex()) ImGui::Image((ImTextureID)(intptr_t)preview_.colorTex(), ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button(playing_ ? "Pause" : "Play")) setPlayback(!playing_);
    ImGui::SameLine(); if (ImGui::Button("Restart")) previewTime_ = 0;
    ImGui::SameLine(); if (ImGui::Button("Refresh view")) refreshPreview();
    ImGui::SliderFloat("Speed", &previewSpeed_, 0.0f, 4.0f);
    ImGui::SliderInt("Width", &previewW_, 320, 1920);
    ImGui::SliderInt("Height", &previewH_, 180, 1080);
    ImGui::EndGroup();
    ImGui::EndChild();
  }

  void loadAudioDialog() {
#ifdef _WIN32
    std::string filter = "Audio (*.wav;*.mp3;*.ogg)"; filter.push_back('\0');
    filter += "*.wav;*.mp3;*.ogg"; filter.push_back('\0');
    filter += "All files"; filter.push_back('\0'); filter += "*.*"; filter.push_back('\0');
    const std::string path = nativeOpenDialog(filter.c_str(), "Load Audio Preview");
    if (!path.empty() && audioReady_) {
      // Audio loading must not replace the shader document. Keep a snapshot
      // around the decode/commit so a future audio backend cannot accidentally
      // clear the active source or its editor buffer.
      const std::string shaderBefore = fragment_;
      const std::string vertexBefore = vertex_;
      const std::vector<char> fragmentBufferBefore = fragBuf_;
      const std::vector<char> vertexBufferBefore = vertBuf_;
      const ShaderKind kindBefore = kind_;
      bool loaded = false;
      if (audio_.started) {
        // Runtime replacement is transactional and stops the callback while
        // the decoded buffer is committed.
        loaded = audio_.swapTrack(path);
      } else {
        audio_.loadTrack(path);
        loaded = audio_.trackMode;
        if (loaded) audio_.start();
      }
      if (!loaded || !audio_.trackMode) {
        audioLoaded_ = !audio_.trackPath().empty() && audio_.trackMode;
        status_ = "Audio load failed; current shader and audio were kept";
        return;
      }
      audioLoaded_ = true;
      setPlayback(playing_);

      // Defensive document restoration: audio operations are not allowed to
      // mutate shader state, even if a backend implementation changes later.
      if (fragment_ != shaderBefore || vertex_ != vertexBefore ||
          fragBuf_ != fragmentBufferBefore || vertBuf_ != vertexBufferBefore ||
          kind_ != kindBefore) {
        kind_ = kindBefore;
        fragment_ = shaderBefore;
        vertex_ = vertexBefore;
        fragBuf_ = fragmentBufferBefore;
        vertBuf_ = vertexBufferBefore;
        ++sourceWidgetRevision_;
        params_ = parseShaderParams(fragment_);
        sourceDirty_ = true;
        compileQueued_ = true;
        editClock_ = nowSeconds();
        status_ = "Audio loaded; shader restored and recompiling: " + path;
      } else {
        status_ = "Audio loaded: " + path;
      }
    }
#else
    status_ = "Use the OpenAI/audio integration on Windows builds, or set the audio path in a future provider panel";
#endif
  }

  void drawAudio() {
    ImGui::SeparatorText(audioLoaded_ ? "AUDIO PREVIEW  //  LIVE FFT REACTIVITY" : "SIMULATED AUDIO  //  drag sliders without a track");
    if (ImGui::Button("Load audio...")) loadAudioDialog();
    if (audioLoaded_) { ImGui::SameLine(); ImGui::TextDisabled("track: %s", audio_.trackPath().c_str()); }
    const char* names[] = {"Audio Level", "Bass", "Mid", "Treble", "Kick", "Snare"};
    for (int i = 0; i < 6; ++i) {
      ImGui::PushID(i); ImGui::Text("%-12s", names[i]); ImGui::SameLine();
      ImGui::ProgressBar(simulatedAudio_[i], ImVec2(-1, 14));
      ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("##v", &simulatedAudio_[i], 0, 1, "%.2f"); ImGui::PopID();
    }
  }

  void ensureDefaultChannelTexture() {
    if (defaultChannelTex_.tex) return;
    const int size = 256;
    const std::vector<unsigned char> px = makeChannelNoisePixels(size);
    defaultChannelTex_ = Texture::fromRGBA(size, size, px.data(),
                                           {::gl::LINEAR, ::gl::LINEAR, ::gl::REPEAT, false});
  }

  void clearChannelTexture(int ci) {
    if (ci < 0 || ci > 3) return;
    channelTex_[(size_t)ci].destroy();
    channelPath_[(size_t)ci].clear();
    channelDownloadState_[(size_t)ci] = ChannelDownloadState::None;
    channelDownloadUrl_[(size_t)ci].clear();
    channelDownloadError_[(size_t)ci].clear();
    channelDownloadProgress_[(size_t)ci].bytes.store(0);
    channelDownloadProgress_[(size_t)ci].total.store(0);
  }

  bool loadChannelTextureFromFile(int ci, const std::string& path) {
    if (ci < 0 || ci > 3) return false;
    int w = 0, h = 0, comp = 0;
    // stb_image rows are top-down; flip so texture v=0 (bottom-left, the GL
    // convention) matches the bottom of the image like Shadertoy's upload.
    stbi_set_flip_vertically_on_load(1);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    stbi_set_flip_vertically_on_load(0);
    if (!px) {
      status_ = "Texture load failed for " + path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "?");
      return false;
    }
    channelTex_[(size_t)ci].destroy();
    channelTex_[(size_t)ci] = Texture::fromRGBA(w, h, px, {::gl::LINEAR, ::gl::LINEAR, ::gl::REPEAT, true});
    stbi_image_free(px);
    channelPath_[(size_t)ci] = path;
    status_ = "Channel uChannel" + std::to_string(ci) + " texture: " + path;
    return true;
  }

  void cancelChannelDownloads() {
    if (!channelDownloadInFlight_) return;
    channelDownloadCancel_.store(true);
    status_ = "Cancelling channel texture download...";
  }

  void startChannelDownloads(const std::vector<ChannelDownloadTask>& tasks) {
    ++channelDownloadBatch_;
    const int batch = channelDownloadBatch_;
    channelDownloadCancel_.store(false);
    pendingChannelTasks_ = tasks;
    for (const auto& t : tasks) {
      for (int ch : t.channels) {
        channelDownloadState_[(size_t)ch] = ChannelDownloadState::Pending;
        channelDownloadUrl_[(size_t)ch] = t.url;
        channelDownloadError_[(size_t)ch].clear();
        channelDownloadProgress_[(size_t)ch].bytes.store(0);
        channelDownloadProgress_[(size_t)ch].total.store(0);
        channelDownloadAttempt_[(size_t)ch].store(0);
      }
    }
    // snapshot the tunable retry policy now: the worker runs on a background
    // thread, so it must not read providerConfig_ while the UI thread may edit
    // it. the worker only touches the per-channel atomic progress/attempt
    // slots; the app outlives the batch because shutdown() waits for the future
    const int maxAttempts = std::max(1, providerConfig_.channelRetryMaxAttempts);
    const int backoffMs = std::max(0, providerConfig_.channelRetryBackoffMs);
    channelDownloadFuture_ = std::async(std::launch::async, [tasks, batch, maxAttempts, backoffMs, this]() {
      ChannelDownloadBatchResult out;
      out.batch = batch;
      for (const auto& t : tasks) {
        if (channelDownloadCancel_.load()) {
          ChannelDownloadResult r;
          r.destPath = t.destPath;
          r.ok = false;
          r.error = "cancelled";
          out.results.push_back(std::move(r));
          continue;
        }
        ChannelDownloadResult r;
        r.destPath = t.destPath;
        // a task's channels share one file, so report progress through the
        // first channel's slot; shared channels mirror it in the UI
        DownloadProgress* prog = t.channels.empty() ? nullptr
                                                    : &channelDownloadProgress_[(size_t)t.channels.front()];
        r.ok = runChannelDownload(
            [&](std::string& err) {
              if (prog) { prog->bytes.store(0); prog->total.store(0); }
              return downloadUrlToFile(t.url, t.destPath, &err, prog, &channelDownloadCancel_);
            },
            &channelDownloadCancel_, &r.error, maxAttempts, backoffMs,
            [&](int attempt, int /*maxAtt*/) {
              for (int ch : t.channels) channelDownloadAttempt_[(size_t)ch].store(attempt);
            });
        if (prog) {
          const long long bytes = prog->bytes.load();
          const long long total = prog->total.load();
          for (int ch : t.channels) {
            channelDownloadProgress_[(size_t)ch].bytes.store(bytes);
            channelDownloadProgress_[(size_t)ch].total.store(total);
          }
        }
        out.results.push_back(std::move(r));
      }
      return out;
    });
    channelDownloadInFlight_ = true;
    status_ = "Downloading " + std::to_string(tasks.size()) + " channel texture(s)...";
  }

  /** the atomic progress slot a channel's download reports into (channels that
   *  share one cached file mirror the first channel's slot) */
  DownloadProgress* channelProgressSlot(int ci) {
    if (ci < 0 || ci > 3) return nullptr;
    for (const auto& t : pendingChannelTasks_) {
      if (t.channels.empty()) continue;
      if (std::find(t.channels.begin(), t.channels.end(), ci) != t.channels.end())
        return &channelDownloadProgress_[(size_t)t.channels.front()];
    }
    return nullptr;
  }

  void resetChannelDownloadProgress() {
    for (auto& p : channelDownloadProgress_) { p.bytes.store(0); p.total.store(0); }
    for (auto& a : channelDownloadAttempt_) a.store(0);
  }

  static std::string formatDownloadBytes(long long bytes) {
    char buf[40];
    if (bytes >= 1000000) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1000000.0);
    else if (bytes >= 1000) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    return buf;
  }

  void pollChannelDownloads() {
    if (!channelDownloadInFlight_ || !channelDownloadFuture_.valid()) return;
    if (channelDownloadFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    channelDownloadInFlight_ = false;
    ChannelDownloadBatchResult batch;
    try {
      batch = channelDownloadFuture_.get();
    } catch (...) {
      pendingChannelTasks_.clear();
      status_ = "Channel texture download failed";
      return;
    }
    // a newer import superseded this batch - drop stale results
    if (batch.batch != channelDownloadBatch_) {
      pendingChannelTasks_.clear();
      return;
    }
    // the user cancelled: put every affected channel back to the unbound
    // default (discard partial files, drop Retry state)
    if (channelDownloadCancel_.load()) {
      channelDownloadCancel_.store(false);
      for (const auto& t : pendingChannelTasks_) {
        for (int ch : t.channels) {
          channelDownloadState_[(size_t)ch] = ChannelDownloadState::None;
          channelDownloadUrl_[(size_t)ch].clear();
          channelDownloadError_[(size_t)ch].clear();
          channelDownloadProgress_[(size_t)ch].bytes.store(0);
          channelDownloadProgress_[(size_t)ch].total.store(0);
          channelDownloadAttempt_[(size_t)ch].store(0);
        }
      }
      pendingChannelTasks_.clear();
      status_ = "Channel texture download cancelled";
      return;
    }
    int bound = 0;
    std::string failures;
    for (const auto& r : batch.results) {
      for (auto& t : pendingChannelTasks_) {
        if (t.destPath != r.destPath) continue;
        for (int ch : t.channels) {
          if (r.ok) {
            if (loadChannelTextureFromFile(ch, r.destPath)) {
              channelDownloadState_[(size_t)ch] = ChannelDownloadState::Ok;
              ++bound;
            } else {
              channelDownloadState_[(size_t)ch] = ChannelDownloadState::Failed;
              channelDownloadError_[(size_t)ch] = "downloaded file is not a readable image";
              if (failures.size() < 220) { if (!failures.empty()) failures += "; "; failures += channelDownloadError_[(size_t)ch]; }
            }
          } else {
            channelDownloadState_[(size_t)ch] = ChannelDownloadState::Failed;
            channelDownloadError_[(size_t)ch] = r.error;
            if (failures.size() < 220) { if (!failures.empty()) failures += "; "; failures += r.error; }
          }
        }
      }
    }
    pendingChannelTasks_.clear();
    if (bound > 0) status_ = "Downloaded and bound " + std::to_string(bound) + " channel texture(s)";
    else if (!failures.empty()) status_ = "Channel texture download failed: " + failures;
    else status_ = "Channel texture download finished";
  }

  void retryChannelDownload(int ci) {
    if (ci < 0 || ci > 3 || channelDownloadUrl_[(size_t)ci].empty()) return;
    if (channelDownloadInFlight_) {
      status_ = "Channel texture download already in progress";
      return;
    }
    // force a fresh fetch: a previous attempt may have left a file that
    // failed to decode as an image
    const std::string dest =
        (std::filesystem::path(dataDir_) / "textures" / channelTextureFilename(channelDownloadUrl_[(size_t)ci])).string();
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    ChannelDownloadTask task;
    task.url = channelDownloadUrl_[(size_t)ci];
    task.destPath = dest;
    task.channels.push_back(ci);
    startChannelDownloads({task});
  }

  void setChannelTexturePixels(int ci, int w, int h, const void* px) {
    if (ci < 0 || ci > 3) return;
    channelTex_[(size_t)ci].destroy();
    channelTex_[(size_t)ci] = Texture::fromRGBA(w, h, px, {::gl::NEAREST, ::gl::NEAREST, ::gl::REPEAT, false});
    channelPath_[(size_t)ci].clear();
  }

  void loadChannelImageDialog(int ci) {
#ifdef _WIN32
    std::string filter = "Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif)"; filter.push_back('\0');
    filter += "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif"; filter.push_back('\0');
    filter += "All files"; filter.push_back('\0'); filter += "*.*"; filter.push_back('\0');
    const std::string p = nativeOpenDialog(filter.c_str(), ("Load uChannel" + std::to_string(ci) + " texture").c_str());
    if (!p.empty() && loadChannelTextureFromFile(ci, p)) {
      // a manual bind supersedes any pending/retryable URL fetch
      channelDownloadState_[(size_t)ci] = ChannelDownloadState::Ok;
      channelDownloadUrl_[(size_t)ci].clear();
      channelDownloadError_[(size_t)ci].clear();
    }
#else
    status_ = "Texture file dialogs are available on Windows builds";
#endif
  }

  void drawChannels() {
    ImGui::SeparatorText("TEXTURE CHANNELS  //  uChannel0..3 (Shadertoy imports)");
    if (channelDownloadInFlight_) {
      if (ImGui::SmallButton("Cancel downloads")) cancelChannelDownloads();
      ImGui::SameLine();
      ImGui::TextDisabled("stopping discards any partial files");
    }
    bool any = false;
    for (int ci = 0; ci < 4; ++ci) {
      if (!channelUsed_[(size_t)ci]) continue;
      any = true;
      ImGui::PushID(ci);
      ImGui::Text("uChannel%d", ci);
      ImGui::SameLine();
      const ChannelDownloadState ds = channelDownloadState_[(size_t)ci];
      if (ds == ChannelDownloadState::Pending) {
        DownloadProgress* prog = channelProgressSlot(ci);
        const long long bytes = prog ? prog->bytes.load() : 0;
        const long long total = prog ? prog->total.load() : 0;
        const int attempt = channelDownloadAttempt_[(size_t)ci].load();
        const int maxAttempts = std::max(1, providerConfig_.channelRetryMaxAttempts);
        const bool retrying = attempt > 1;
        if (total > 0) {
          const float frac = std::min(1.0f, (float)bytes / (float)total);
          char label[96];
          if (retrying)
            std::snprintf(label, sizeof(label), "retrying %d/%d... %s / %s", attempt, maxAttempts,
                          formatDownloadBytes(bytes).c_str(), formatDownloadBytes(total).c_str());
          else
            std::snprintf(label, sizeof(label), "%s / %s",
                          formatDownloadBytes(bytes).c_str(), formatDownloadBytes(total).c_str());
          ImGui::ProgressBar(frac, ImVec2(retrying ? 320 : 210, 0), label);
        } else {
          if (retrying)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "retrying %d/%d... %s", attempt,
                               maxAttempts, formatDownloadBytes(bytes).c_str());
          else
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "downloading channel texture... %s",
                               formatDownloadBytes(bytes).c_str());
        }
      } else if (ds == ChannelDownloadState::Failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "fetch failed: %s", channelDownloadError_[(size_t)ci].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry")) retryChannelDownload(ci);
        ImGui::SameLine();
        if (ImGui::SmallButton("Load...")) loadChannelImageDialog(ci);
      } else if (!channelPath_[(size_t)ci].empty()) {
        ImGui::TextDisabled("%s", channelPath_[(size_t)ci].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
          clearChannelTexture(ci);
          status_ = "uChannel" + std::to_string(ci) + " reset to default noise";
        }
      } else {
        ImGui::TextDisabled("default noise (load an image to bind)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Load...")) loadChannelImageDialog(ci);
      }
      ImGui::PopID();
    }
    if (!any)
      ImGui::TextDisabled("The current fragment declares no sampler2D uChannelN. Import a Shadertoy shader that samples iChannel0..3 to bind textures here.");
  }

  void drawParameters() {
    ImGui::BeginChild("ai_params", ImVec2(270, 0), true);
    ImGui::SeparatorText("PARAMETERS");
    for (ShaderParamDecl& p : params_) {
      ImGui::PushID(p.name.c_str());
      if (p.type == ShaderParamType::Color) ImGui::ColorEdit4(p.name.c_str(), p.vector.data());
      else if (p.type == ShaderParamType::Bool) { bool b = p.value > 0.5f; if (ImGui::Checkbox(p.name.c_str(), &b)) p.value = b ? 1.0f : 0.0f; }
      else if (p.type == ShaderParamType::Vec2) ImGui::DragFloat2(p.name.c_str(), p.vector.data(), 0.01f);
      else if (p.type == ShaderParamType::Vec3) ImGui::DragFloat3(p.name.c_str(), p.vector.data(), 0.01f);
      else if (p.type == ShaderParamType::Vec4) ImGui::DragFloat4(p.name.c_str(), p.vector.data(), 0.01f);
      else ImGui::SliderFloat(p.name.c_str(), &p.value, p.min, p.max, p.type == ShaderParamType::Int ? "%.0f" : "%.3f");
      ImGui::PopID();
    }
    if (params_.empty()) ImGui::TextDisabled("No // @param declarations found.");
    if (ImGui::Button("Reset parameters")) for (auto& p : params_) { p.value = p.defaultValue; p.vector = p.defaultVector; }
    ImGui::SeparatorText("Performance estimate");
    const std::string source = fragment_ + vertex_;
    int loops = 0; for (size_t p = 0; (p = source.find("for (", p)) != std::string::npos; p += 5) ++loops;
    int textures = 0; for (size_t p = 0; (p = source.find("texture", p)) != std::string::npos; p += 7) ++textures;
    ImGui::Text("ALU:        %s", source.size() > 9000 ? "High" : source.size() > 4500 ? "Medium" : "Low");
    ImGui::Text("Texture:    %s", textures > 3 ? "Medium" : "Low");
    ImGui::Text("Loops:      %s (%d found)", loops > 2 ? "High" : loops ? "Medium" : "Low", loops);
    std::string sourceLower = source;
    for (char& c : sourceLower) c = (char)std::tolower((unsigned char)c);
    ImGui::Text("Raymarch:   %s", sourceLower.find("raymarch") != std::string::npos ? "bounded / inspect" : "none");
    ImGui::TextWrapped("Hint: ask the AI to optimize for GTX 1070 while preserving the appearance.");
    ImGui::EndChild();
  }

  void drawSource() {
    ImGui::BeginChild("ai_source", ImVec2(0, 330), true);
    ImGui::SeparatorText("GLSL SOURCE");
    if (kind_ != ShaderKind::Fragment) {
      ImGui::RadioButton("Fragment", &sourceTab_, 0); ImGui::SameLine();
      ImGui::RadioButton("Vertex", &sourceTab_, 1);
    } else {
      sourceTab_ = 0;
    }
    const std::string& source = sourceTab_ == 0 ? fragment_ : vertex_;
    const int lineCount = (int)std::count(source.begin(), source.end(), '\n') + 1;
    ImGui::BeginChild("source_lines", ImVec2(46, 0), true, ImGuiWindowFlags_NoScrollbar);
    for (int i = 1; i <= lineCount; ++i) ImGui::TextDisabled("%4d", i);
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("source_edit", ImVec2(0, 0), false);
    const std::string widgetId = std::string(sourceTab_ == 0 ? "##fragment_source" : "##vertex_source") + "_" + std::to_string(sourceWidgetRevision_);
    std::vector<char>& buf = sourceTab_ == 0 ? fragBuf_ : vertBuf_;
    if (lastSourceWidgetRevisionLogged_ != sourceWidgetRevision_) {
      const std::string& widgetSource = sourceTab_ == 0 ? fragment_ : vertex_;
      traceSource("source text widget before render", widgetSource);
      std::printf("[SHADER-AI][TRACE] Generate #%d text widget buffer: hash=%s bytes=%zu id=%s\n",
                  generationSerial_, sourceSignatureText(buf.data()).c_str(), buf.size(), widgetId.c_str());
      lastSourceWidgetRevisionLogged_ = sourceWidgetRevision_;
    }
    if (ImGui::InputTextMultiline(widgetId.c_str(), buf.data(), buf.size(), ImVec2(-1, -1), ImGuiInputTextFlags_AllowTabInput)) syncSourcesFromUi();
    if (lastVisibleSourceRevisionLogged_ != sourceWidgetRevision_) {
      std::printf("[SHADER-AI][TRACE] Generate #%d VISIBLE SOURCE HASH: %s bytes=%zu", generationSerial_, sourceSignatureText(buf.data()).c_str(), std::strlen(buf.data()));
      std::putchar('\n');
      std::fflush(stdout);
      lastVisibleSourceRevisionLogged_ = sourceWidgetRevision_;
    }
    ImGui::EndChild(); ImGui::EndChild();
  }

  void drawDiagnostics() {
    ImGui::BeginChild("ai_bottom", ImVec2(0, 190), true);
    ImGui::BeginTabBar("ai_tabs");
    if (ImGui::BeginTabItem("Diagnostics")) {
      // green = clean, amber = compiles but renders as a uniform color, red = compile error
      const ImVec4 statusCol = diagnostics_.empty() ? ImVec4(0.3f, 1, 0.7f, 1)
                               : flatOutput_ ? ImVec4(1.0f, 0.85f, 0.3f, 1)
                               : ImVec4(1, 0.3f, 0.3f, 1);
      ImGui::TextColored(statusCol, "%s", status_.c_str());
      if (!diagnostics_.empty()) {
        ImGui::TextWrapped("%s", diagnostics_.c_str());
        if (ImGui::Button("Ask AI to Fix")) generate(true);
        for (const auto& d : parseShaderDiagnostics(diagnostics_)) {
          if (ImGui::Selectable(("line " + std::to_string(d.line) + ": " + d.message).c_str())) sourceLine_ = d.line;
        }
      }
      if (!conversation_.empty()) { ImGui::SeparatorText("AI explanation"); ImGui::TextWrapped("%s", conversation_.c_str()); }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Generation history")) {
      for (int i = (int)history_.size() - 1; i >= 0; --i) {
        const auto& h = history_[(size_t)i];
        if (ImGui::Selectable(h.label.c_str(), selectedHistory_ == i)) selectedHistory_ = i;
        if (selectedHistory_ == i) {
          ImGui::SameLine(); if (ImGui::SmallButton("Restore")) restoreHistory(i);
        }
      }
      if (history_.empty()) ImGui::TextDisabled("Generate a version to begin history.");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::EndChild();
  }

  void pollModelRefresh() {
    if (!modelRefreshInFlight_ || !modelFuture_.valid()) return;
    if (modelFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    modelRefreshInFlight_ = false;
    try {
      ModelRefreshResult result = modelFuture_.get();
      if (result.models.empty()) {
        modelStatus_ = result.error.empty() ? "No models returned by provider" : "Model discovery failed: " + result.error;
        status_ = modelStatus_;
        return;
      }
      availableModels_ = std::move(result.models);
      modelStatus_ = "Loaded " + std::to_string(availableModels_.size()) + " models";
      bool selected = false;
      for (const auto& model : availableModels_) {
        if (model.id == providerConfig_.model) { selected = true; break; }
      }
      if (!selected) providerConfig_.model = availableModels_.front().id;
    } catch (const std::exception& e) {
      modelStatus_ = std::string("Model discovery failed: ") + e.what();
      status_ = modelStatus_;
    }
  }

  void refreshModels() {
    if (modelRefreshInFlight_) return;
    const ProviderConfig config = providerConfig_;
    provider_ = makeShaderAiProvider(config);
    modelStatus_ = "Refreshing available models...";
    status_ = modelStatus_;
    modelRefreshInFlight_ = true;
    modelFuture_ = std::async(std::launch::async, [config]() {
      ModelRefreshResult result;
      auto provider = makeShaderAiProvider(config);
      result.models = provider->listModels(config, &result.error);
      return result;
    });
  }

  void saveSettings() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(settingsPath_).parent_path(), ec);
    std::string error;
    if (!saveShaderAiSettings(settingsPath_, providerConfig_, &error)) {
      status_ = "Settings save failed: " + error;
      return;
    }
    status_ = "Provider settings saved";
    modelStatus_ = "Settings saved";
  }

  void reloadSettings() {
    ProviderConfig loaded = providerConfig_;
    std::string error;
    if (!loadShaderAiSettings(settingsPath_, loaded, &error)) {
      status_ = "Settings load failed: " + error;
      return;
    }
    providerConfig_ = std::move(loaded);
    provider_ = makeShaderAiProvider(providerConfig_);
    availableModels_.clear();
    availableModels_.push_back({providerConfig_.model, "configured"});
    modelStatus_ = "Settings reloaded";
    status_ = "Provider settings reloaded";
  }

  void drawSettings() {
    if (!showSettings_) return;
    ImGui::SetNextWindowSize(ImVec2(580, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Provider Settings", &showSettings_)) { ImGui::End(); return; }
    char provider[96] = {}; std::snprintf(provider, sizeof(provider), "%s", providerConfig_.provider.c_str());
    char endpoint[256] = {}; std::snprintf(endpoint, sizeof(endpoint), "%s", providerConfig_.endpoint.c_str());
    char key[256] = {}; std::snprintf(key, sizeof(key), "%s", providerConfig_.apiKey.c_str());
    if (ImGui::InputText("Provider", provider, sizeof(provider))) providerConfig_.provider = provider;
    if (ImGui::InputText("Endpoint", endpoint, sizeof(endpoint))) providerConfig_.endpoint = endpoint;
    if (ImGui::InputText("API key", key, sizeof(key), ImGuiInputTextFlags_Password)) providerConfig_.apiKey = key;

    std::vector<const char*> modelLabels;
    modelLabels.reserve(availableModels_.size());
    int modelIndex = 0;
    for (size_t i = 0; i < availableModels_.size(); ++i) {
      modelLabels.push_back(availableModels_[i].id.c_str());
      if (availableModels_[i].id == providerConfig_.model) modelIndex = (int)i;
    }
    if (!modelLabels.empty() && ImGui::Combo("Model", &modelIndex, modelLabels.data(), (int)modelLabels.size()))
      providerConfig_.model = availableModels_[(size_t)modelIndex].id;
    const bool modelRefreshWasInFlight = modelRefreshInFlight_;
    if (modelRefreshWasInFlight) ImGui::BeginDisabled();
    if (ImGui::Button("Refresh available models")) refreshModels();
    if (modelRefreshWasInFlight) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", modelStatus_.empty() ? "provider /v1/models" : modelStatus_.c_str());
    if (!providerConfig_.model.empty()) ImGui::TextDisabled("Selected: %s", providerConfig_.model.c_str());
    ImGui::SliderFloat("Temperature", &providerConfig_.temperature, 0, 1);
    ImGui::SliderInt("Max tokens", &providerConfig_.maxTokens, 256, 16384);
    ImGui::SliderInt("Request timeout (seconds)", &providerConfig_.timeoutSeconds, 30, 1800);
    ImGui::TextDisabled("Reasoning models (gpt-5, o-series) can take minutes before responding; raise this if a generation times out.");
    ImGui::SeparatorText("Channel texture downloads");
    ImGui::SliderInt("Retry attempts", &providerConfig_.channelRetryMaxAttempts, 1, 5);
    ImGui::SliderInt("Retry backoff (ms)", &providerConfig_.channelRetryBackoffMs, 0, 10000);
    ImGui::TextDisabled("Transient channel-texture download failures (timeouts, transport errors, HTTP 5xx) retry up to this many attempts with the wait growing by the backoff each time; HTTP 4xx is never retried. The active attempt shows in the Texture Channels row.");
    ImGui::SeparatorText("Preview self-check");
    ImGui::Checkbox("Auto-repair degenerate generations (never drew / uniform / near-black)", &providerConfig_.autoRepairEnabled);
    if (providerConfig_.autoRepairEnabled) {
      ImGui::SliderInt("Max auto-repairs per generation", &providerConfig_.autoRepairMax, 0, 3);
      ImGui::TextDisabled("0 = never re-ask automatically (the warning stays). Each automatic repair costs one extra API request - the counter and total request count show in the status line.");
    }
    ImGui::Checkbox("Send the failing frame image to the model on repair (vision)", &providerConfig_.sendRepairImage);
    ImGui::TextDisabled("Attaches the captured render as an image part on repair, for image-capable OpenAI-compatible models - bigger requests, but the model sees the actual broken frame.");
    ImGui::SeparatorText("Settings");
    if (ImGui::Button("Save settings")) saveSettings();
    ImGui::SameLine();
    if (ImGui::Button("Reload settings")) reloadSettings();
    ImGui::TextWrapped("Settings file: %s\nThe API key is protected with Windows user encryption and is never written to .nsshad shader projects.", settingsPath_.c_str());
    ImGui::End();
  }

  void importShadertoyFromFile() {
#ifdef _WIN32
    std::string filter = "Shadertoy GLSL (*.glsl;*.frag;*.txt)"; filter.push_back('\0');
    filter += "*.glsl;*.frag;*.txt"; filter.push_back('\0');
    filter += "All files"; filter.push_back('\0'); filter += "*.*"; filter.push_back('\0');
    const std::string path = nativeOpenDialog(filter.c_str(), "Import Shadertoy");
    if (path.empty()) return;
    const std::string text = readText(path);
    if (text.empty()) { status_ = "Cannot read " + path; return; }
    shadertoyBuf_ = textBuffer(text, 65536);
    status_ = "Loaded Shadertoy source: " + path;
#else
    status_ = "Use the file dialog on Windows builds, or paste the source below";
#endif
  }

  void applyShadertoyConversion(const ShadertoyConvertResult& r) {
    if (!r.ok) { diagnostics_ = "Shadertoy conversion failed: " + r.error; status_ = diagnostics_; return; }
    // a new import supersedes any previous channel download state
    channelDownloadState_ = {};
    channelDownloadUrl_ = {};
    channelDownloadError_ = {};
    resetChannelDownloadProgress();
    fragment_ = r.fragment;
    vertex_ = "";
    kind_ = ShaderKind::Fragment;
    fragBuf_ = textBuffer(fragment_);
    vertBuf_ = textBuffer(vertex_);
    ++sourceWidgetRevision_;
    params_ = parseShaderParams(fragment_);
    specification_ = "Converted from Shadertoy.\nChannels folded into a single fragment shader by the Null Sector converter.\nBind the listed sampler textures at runtime (uChannelN).";
    specBuf_ = textBuffer(specification_, 8192);
    prompt_ = "Imported Shadertoy shader (all channels folded)";
    promptBuf_ = textBuffer(prompt_, 8192);
    sourceDirty_ = true;
    compileQueued_ = true;
    conversation_ = r.notes.empty() ? "Conversion clean - no channel notes." : "Conversion notes:\n" + [&]() {
      std::string out;
      for (const auto& n : r.notes) out += "- " + n + "\n";
      for (const auto& t : r.requiredTextures) out += "- bind " + t + "\n";
      return out;
    }();
    status_ = "Shadertoy converted - " + std::to_string(r.foldedBuffers.size()) + " buffer(s) folded";
    // Auto-bind textures the source referenced (`#iChannel0 "file.png"` or
    // `texture:file.png`) from data/textures/ so the preview shows real
    // content immediately instead of the default noise placeholder. URLs (and
    // Shadertoy API asset paths) are downloaded to data/textures/ in the
    // background and bound when they land.
    std::vector<ChannelDownloadTask> downloads;
    for (const auto& t : r.requiredTextures) {
      if (t.rfind("uChannel", 0) != 0 || t.size() < 9) continue;
      const int ci = t[8] - '0';
      if (ci < 0 || ci > 3) continue;
      const size_t lp = t.find('('), rp = t.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos || rp <= lp) continue;
      std::string target = t.substr(lp + 1, rp - lp - 1);
      const size_t first = target.find_first_not_of(" \t\r\n");
      const size_t last = target.find_last_not_of(" \t\r\n");
      if (first == std::string::npos) continue;
      target = target.substr(first, last - first + 1);
      if (target.empty() || target.find("bind a texture") != std::string::npos ||
          target.find("unbound") != std::string::npos || target.find("live scene") != std::string::npos ||
          target.find("reads black") != std::string::npos)
        continue;
      std::string url;
      if (target.rfind("https://", 0) == 0 || target.rfind("http://", 0) == 0) url = target;
      else if (target.rfind("/media/", 0) == 0 || target.rfind("/presets/", 0) == 0)
        url = "https://www.shadertoy.com" + target;  // Shadertoy API asset paths
      if (url.empty()) {
        // plain file name: probe the local texture cache
        const std::string candidate = (std::filesystem::path(dataDir_) / "textures" / target).string();
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec && loadChannelTextureFromFile(ci, candidate))
          channelDownloadState_[(size_t)ci] = ChannelDownloadState::Ok;
        continue;
      }
      const std::string dest =
          (std::filesystem::path(dataDir_) / "textures" / channelTextureFilename(url)).string();
      std::error_code ec;
      if (std::filesystem::exists(dest, ec) && !ec && loadChannelTextureFromFile(ci, dest)) {
        // already cached from a previous import - bind directly
        channelDownloadState_[(size_t)ci] = ChannelDownloadState::Ok;
        continue;
      }
      auto it = std::find_if(downloads.begin(), downloads.end(),
                             [&](const ChannelDownloadTask& t2) { return t2.destPath == dest; });
      if (it == downloads.end()) downloads.push_back({url, dest, {ci}});
      else it->channels.push_back(ci);
    }
    if (!downloads.empty()) startChannelDownloads(downloads);
  }

  void drawShadertoyImport() {
    if (!showShadertoyImport_) return;
    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Shadertoy Import", &showShadertoyImport_)) { ImGui::End(); return; }
    ImGui::TextWrapped("Paste Shadertoy code (single or multi-pass, `// pass:` markers or a JSON API export). "
                       "Standard `#iChannelN \"spec\"` lines and unwired sampled channels become bindable "
                       "uChannelN samplers; texture URLs are downloaded to data/textures/ and bound "
                       "automatically. Buffers fold into callable functions; audio/keyboard channels "
                       "are stubbed. Shadertoy uniforms are remapped to the Null Sector set.");
    ImGui::Separator();
    if (ImGui::Button("Open .glsl file...")) importShadertoyFromFile();
    ImGui::SameLine();
    ImGui::TextDisabled("or paste below");
    ImGui::InputTextMultiline("##shadertoy_src", shadertoyBuf_.data(), shadertoyBuf_.size(),
                              ImVec2(-1, 300), ImGuiInputTextFlags_AllowTabInput);
    ImGui::Separator();
    static bool foldBuffers = true;
    ImGui::Checkbox("Fold buffer passes into the file", &foldBuffers);
    if (ImGui::Button("Convert to fragment shader", ImVec2(280, 0))) {
      ShadertoyConvertOptions opts;
      opts.sourceLabel = "pasted source";
      ShadertoyConvertResult r = convertShadertoyToFragment(shadertoyBuf_.data(), opts);
      if (r.ok) {
        applyShadertoyConversion(r);
        showShadertoyImport_ = false;
      } else {
        diagnostics_ = r.error;
        status_ = "Shadertoy conversion failed";
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy source to clipboard")) ImGui::SetClipboardText(shadertoyBuf_.data());
    ImGui::Separator();
    ImGui::TextWrapped("Hint: convert in a headless shell with  ns_shader_ai --convert-shadertoy=in.glsl --out=out.frag");
    ImGui::End();
  }

  void drawSavePopup() {
    if (showSavePopup_) ImGui::OpenPopup("Save Shader");
    if (ImGui::BeginPopupModal("Save Shader", &showSavePopup_)) {
      ImGui::InputText("Path", savePathBuf_.data(), savePathBuf_.size());
      if (ImGui::Button("Save")) { saveShaders(savePathBuf_.data()); showSavePopup_ = false; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Cancel")) { showSavePopup_ = false; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
    if (showProjectPopup_) ImGui::OpenPopup("Project File");
    if (ImGui::BeginPopupModal("Project File", &showProjectPopup_)) {
      ImGui::InputText("Path", projectPathBuf_.data(), projectPathBuf_.size());
      if (ImGui::Button("Save")) { saveProject(projectPathBuf_.data()); showProjectPopup_ = false; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Load")) { loadProject(projectPathBuf_.data()); showProjectPopup_ = false; ImGui::CloseCurrentPopup(); }
      ImGui::SameLine(); if (ImGui::Button("Cancel")) { showProjectPopup_ = false; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
  }

  void setPlayback(bool playing) {
    playing_ = playing;
    audio_.setPlaying(playing_);
    status_ = playing_ ? "Playback started" : "Playback paused";
  }

  void draw() {
    pollGeneration();
    pollModelRefresh();
    pollChannelDownloads();
    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::GetIO().WantTextInput)
      setPlayback(!playing_);
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) refreshPreview();
    if (ImGui::IsKeyPressed(ImGuiKey_I) && ImGui::GetIO().KeyCtrl) showShadertoyImport_ = true;
    if (compileQueued_ && nowSeconds() - editClock_ > 0.30) compileNow();
    if (playing_) previewTime_ += delta_ * previewSpeed_;
    renderPreview();
    ImGui::Begin("Null Sector AI Shader Generator", nullptr, ImGuiWindowFlags_MenuBar);
    drawMenu();
    if (fullscreenPreview_) {
      if (preview_.colorTex()) ImGui::Image((ImTextureID)(intptr_t)preview_.colorTex(), ImGui::GetContentRegionAvail(), ImVec2(0, 1), ImVec2(1, 0));
      ImGui::End(); drawSettings(); drawSavePopup(); return;
    }
    drawPrompt(); ImGui::SameLine();
    ImGui::BeginChild("ai_center", ImVec2(0, 0), false);
    drawPreview();
    ImGui::BeginChild("ai_code_row", ImVec2(0, 0), false);
    drawSource(); ImGui::SameLine(); drawParameters();
    drawAudio(); drawChannels(); drawDiagnostics();
    ImGui::EndChild(); ImGui::EndChild();
    ImGui::End();
    drawSettings(); drawSavePopup(); drawShadertoyImport();
  }

  bool readPreviewPixel(unsigned char out[4]) {
    if (!preview_.fbo || !validProgram_) return false;
    preview_.bind();
    ::glPixelStorei(::gl::PACK_ALIGNMENT, 1);
    ::glReadPixels(preview_.w / 2, preview_.h / 2, 1, 1, ::gl::RGBA, ::gl::UNSIGNED_BYTE, out);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
    return true;
  }

  int runSmoke() {
    GenerationRequest pair; pair.kind = ShaderKind::Pair; pair.prompt = "Create a plasma tunnel";
    BuiltinDemosceneProvider builtin;
    GeneratedShader g = builtin.generate(pair);
    if (g.fragment.empty() || g.vertex.empty()) return 1;
    applyGenerated(g, "Smoke Pair");
    if (!compileNow()) return 2;
    // Exercise the same asynchronous path used by the Generate button with
    // the offline provider, then render the real ImGui source widget. This
    // makes the smoke trace cover worker handoff and InputText synchronization.
    providerConfig_.provider = "builtin-demoscene";
    providerConfig_.apiKey.clear();
    generate(false);
    for (int wait = 0; wait < 200 && generationInFlight_; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      pollGeneration();
    }
    if (generationInFlight_ || fragment_ != std::string(fragBuf_.data())) return 18;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Shader AI Trace");
    drawSource();
    ImGui::End();
    ImGui::Render();

    // Deterministic end-to-end source/widget/compile/render test. This uses
    // explicit red and green shaders, not a visual variation workaround.
    auto renderSourceWidgetForSmoke = [&]() {
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ImGui::Begin("Shader AI Deterministic Source Test");
      drawSource();
      ImGui::End();
      ImGui::Render();
      return fragment_ == std::string(fragBuf_.data());
    };
    GeneratedShader red;
    red.kind = ShaderKind::Fragment;
    red.fragment = "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\nvoid main() { FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    applyGenerated(red, "Deterministic Red");
    if (!compileNow() || !renderSourceWidgetForSmoke()) return 19;
    renderPreview();
    unsigned char redPixel[4] = {};
    if (!readPreviewPixel(redPixel) || redPixel[0] < 240 || redPixel[1] > 20 || redPixel[2] > 20) {
      std::printf("[SHADER-AI] smoke: deterministic red preview FAIL (%u,%u,%u,%u)\n",
                  redPixel[0], redPixel[1], redPixel[2], redPixel[3]);
      return 20;
    }
    GeneratedShader green;
    green.kind = ShaderKind::Fragment;
    green.fragment = "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\nvoid main() { FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    applyGenerated(green, "Deterministic Green");
    if (!compileNow() || !renderSourceWidgetForSmoke()) return 21;
    renderPreview();
    unsigned char greenPixel[4] = {};
    if (!readPreviewPixel(greenPixel) || greenPixel[0] > 20 || greenPixel[1] < 240 || greenPixel[2] > 20) {
      std::printf("[SHADER-AI] smoke: deterministic green preview FAIL (%u,%u,%u,%u)\n",
                  greenPixel[0], greenPixel[1], greenPixel[2], greenPixel[3]);
      return 22;
    }

    // Flat-output detection: a shader that never uses the pixel position must
    // be flagged (and made repairable), while a spatial shader must not be.
    GeneratedShader flat;
    flat.kind = ShaderKind::Fragment;
    flat.fragment = "#version 300 es\nout vec4 fragColor;\nuniform float uTime;\nvoid main() { fragColor = vec4(fract(uTime), 0.5, 0.5, 1.0); }\n";
    applyGenerated(flat, "Deterministic Flat");
    if (!compileNow() || !flatOutput_ || diagnostics_.empty()) {
      std::printf("[SHADER-AI] smoke: flat-output detection FAIL (flagged=%d diagnostics=%zu)\n",
                  (int)flatOutput_, diagnostics_.size());
      return 24;
    }
    GeneratedShader spatial;
    spatial.kind = ShaderKind::Fragment;
    spatial.fragment = "#version 300 es\nin vec2 vUV;\nout vec4 fragColor;\nvoid main() { fragColor = vec4(vUV.x, 0.5, 0.5, 1.0); }\n";
    applyGenerated(spatial, "Deterministic Spatial");
    if (!compileNow() || flatOutput_ || !diagnostics_.empty()) {
      std::printf("[SHADER-AI] smoke: spatial shader falsely flagged flat (flat=%d)\n", (int)flatOutput_);
      return 25;
    }

    // Auto-repair: a fresh generation that renders flat must trigger exactly
    // one automatic re-ask, a successful repair must be applied, and a flat
    // repair result must NOT trigger a second automatic ask. The provider
    // results are injected as ready futures so the real pollGeneration() path
    // (apply -> compile -> flat check -> re-ask) is exercised deterministically
    // without a network provider.
    {
      providerConfig_.provider = "builtin-demoscene";
      providerConfig_.apiKey.clear();
      const std::string flatSrc = "#version 300 es\nout vec4 fragColor;\nuniform float uTime;\n"
                                  "void main() { fragColor = vec4(fract(uTime), 0.5, 0.5, 1.0); }\n";
      const std::string spatialSrc = "#version 300 es\nin vec2 vUV;\nout vec4 fragColor;\n"
                                     "void main() { fragColor = vec4(vUV.x, 0.5, 0.5, 1.0); }\n";
      GeneratedShader flatGen; flatGen.kind = ShaderKind::Fragment; flatGen.fragment = flatSrc;
      GeneratedShader spatialGen; spatialGen.kind = ShaderKind::Fragment; spatialGen.fragment = spatialSrc;

      // (1) a fresh flat generation -> pollGeneration applies it and re-asks once
      generationIsRepair_ = false;
      autoRepairsUsed_ = 0;
      generationInFlight_ = true;
      { std::promise<GeneratedShader> pr; pr.set_value(flatGen); generationFuture_ = pr.get_future(); }
      const int serialBefore = generationSerial_;
      pollGeneration();
      if (!generationInFlight_ || generationSerial_ != serialBefore + 1 || !generationIsRepair_ ||
          autoRepairsUsed_ != 1) {
        std::printf("[SHADER-AI] smoke: flat auto-repair did not re-ask once (inflight=%d serial=%d->%d repair=%d auto=%d)\n",
                    (int)generationInFlight_, serialBefore, generationSerial_, (int)generationIsRepair_,
                    autoRepairsUsed_);
        return 30;
      }

      // (2) the repaired (spatial) result -> applied, no further re-ask
      { std::promise<GeneratedShader> pr; pr.set_value(spatialGen); generationFuture_ = pr.get_future(); }
      pollGeneration();
      if (generationInFlight_ || flatOutput_ || !diagnostics_.empty()) {
        std::printf("[SHADER-AI] smoke: auto-repair result handling FAIL (inflight=%d flat=%d diagnostics=%zu)\n",
                    (int)generationInFlight_, (int)flatOutput_, diagnostics_.size());
        return 31;
      }

      // (3) a flat repair result -> warned, but NOT re-asked again
      generationIsRepair_ = true;
      generationInFlight_ = true;
      { std::promise<GeneratedShader> pr; pr.set_value(flatGen); generationFuture_ = pr.get_future(); }
      const int serialBefore2 = generationSerial_;
      pollGeneration();
      if (generationInFlight_ || generationSerial_ != serialBefore2 || !flatOutput_ ||
          autoRepairsUsed_ != 1) {
        std::printf("[SHADER-AI] smoke: flat repair wrongly auto-re-asked (inflight=%d serial=%d->%d flat=%d auto=%d)\n",
                    (int)generationInFlight_, serialBefore2, generationSerial_, (int)flatOutput_,
                    autoRepairsUsed_);
        return 32;
      }
      generationIsRepair_ = false;
      autoRepairsUsed_ = 0;
    }

    // PNG frame encoding (the vision repair input): a small RGBA buffer must
    // produce a base64 PNG data URL with a valid signature.
    {
      std::vector<unsigned char> rgba((size_t)4 * 4 * 4, 128);
      const std::string dataUrl = encodePngDataUrl(rgba.data(), 4, 4);
      if (dataUrl.rfind("data:image/png;base64,", 0) != 0) {
        std::printf("[SHADER-AI] smoke: PNG data URL encoding FAIL (len=%zu)\n", dataUrl.size());
        return 33;
      }
      if (dataUrl.find("iVBOR") == std::string::npos) {  // base64 of the PNG magic
        std::printf("[SHADER-AI] smoke: PNG signature missing from data URL\n");
        return 34;
      }
    }

    applyGenerated(g, "Smoke Pair Restore");
    if (!compileNow()) return 23;
    const unsigned oldProgram = validProgram_ ? validProgram_->id() : 0;
    fragment_ = "#version 300 es\nvoid main(){ gl_FragColor = missingName; }\n";
    fragBuf_ = textBuffer(fragment_); compileNow();
    if (!validProgram_ || validProgram_->id() != oldProgram || diagnostics_.empty()) return 3;
    fragment_ = validFragment_; fragBuf_ = textBuffer(fragment_); compileNow();
    if (!validProgram_ || validFragment_.empty()) return 4;
    params_ = parseShaderParams(validFragment_);
    if (params_.size() < 3) return 5;
    ShaderAiProject p; p.prompt = "smoke"; p.kind = ShaderKind::Pair; p.fragment = validFragment_; p.vertex = validVertex_; p.history = history_;
    p.channelUrls[1] = "https://example.invalid/smoke.png";
    std::string error; const std::string path = (std::filesystem::path(dataDir_) / ".shader_ai_smoke.nsshad").string();
    if (!p.save(path, &error)) return 6;
    ShaderAiProject q; if (!q.load(path, &error) || q.fragment != p.fragment || q.vertex != p.vertex ||
                          q.channelUrls[1] != p.channelUrls[1]) return 7;
    ProviderConfig localConfig; localConfig.model = "null-sector-local";
    std::string modelError; const auto localModels = BuiltinDemosceneProvider().listModels(localConfig, &modelError);
    if (localModels.size() != 1 || localModels.front().id != localConfig.model || !modelError.empty()) return 8;
    std::error_code ec;
    // Provider settings round-trip, including the API key. On Windows the
    // implementation uses DPAPI; the temporary file is removed immediately.
    {
      ProviderConfig savedConfig;
      savedConfig.provider = "openai-compatible";
      savedConfig.model = "gpt-test";
      savedConfig.endpoint = "https://example.invalid/v1/chat/completions";
      savedConfig.apiKey = "sk-null-sector-smoke";
      savedConfig.temperature = 0.2f;
      savedConfig.maxTokens = 1234;
      savedConfig.timeoutSeconds = 750;
      savedConfig.channelRetryMaxAttempts = 5;
      savedConfig.channelRetryBackoffMs = 2500;
      savedConfig.autoRepairEnabled = false;
      savedConfig.autoRepairMax = 2;
      savedConfig.sendRepairImage = true;
      const std::string settingsFile = (std::filesystem::path(dataDir_) / ".shader_ai_settings_smoke.json").string();
      std::string settingsError;
      if (!saveShaderAiSettings(settingsFile, savedConfig, &settingsError)) return 9;
      ProviderConfig loadedConfig;
      if (!loadShaderAiSettings(settingsFile, loadedConfig, &settingsError) ||
          loadedConfig.provider != savedConfig.provider || loadedConfig.model != savedConfig.model ||
          loadedConfig.endpoint != savedConfig.endpoint || loadedConfig.apiKey != savedConfig.apiKey ||
          loadedConfig.maxTokens != savedConfig.maxTokens ||
          loadedConfig.timeoutSeconds != savedConfig.timeoutSeconds ||
          loadedConfig.channelRetryMaxAttempts != savedConfig.channelRetryMaxAttempts ||
          loadedConfig.channelRetryBackoffMs != savedConfig.channelRetryBackoffMs ||
          loadedConfig.autoRepairEnabled != savedConfig.autoRepairEnabled ||
          loadedConfig.autoRepairMax != savedConfig.autoRepairMax ||
          loadedConfig.sendRepairImage != savedConfig.sendRepairImage) return 10;
      std::filesystem::remove(settingsFile, ec);
    }
    std::filesystem::remove(path, ec);
    // shadertoy converter smoke: convert a multi-pass file, assert the
    // folding/wiring, and compile the result through the real engine path
    {
      const std::string markerSrc =
          "// pass: common\n"
          "float h(vec2 p) { return fract(p.x + p.y); }\n"
          "// pass: buffer_a\n"
          "// channel: iChannel1 = audio\n"
          "void mainImage(out vec4 o, in vec2 p) {\n"
          "  vec2 uv = p / iResolution.xy;\n"
          "  o = vec4(h(uv), 0.0, 0.0, 1.0) + texture(iChannel1, uv);\n"
          "}\n"
          "// pass: buffer_b\n"
          "void mainImage(out vec4 o, in vec2 p) {\n"
          "  vec2 uv = p / iResolution.xy;\n"
          "  o = texture(iChannel0, uv);\n"
          "}\n"
          "// pass: image\n"
          "// channel: iChannel2 = keyboard\n"
          "// channel: iChannel3 = texture:smoke\n"
          "void mainImage(out vec4 o, in vec2 p) {\n"
          "  vec2 uv = p / iResolution.xy;\n"
          "  o = texture(iChannel0, uv) + texture(iChannel2, uv)\n"
          "    + texture(iChannel3, uv) + vec4(uv, 0.0, 1.0);\n"
          "}\n";
      ShadertoyConvertOptions o;
      o.sourceLabel = "smoke";
      const auto conv = convertShadertoyToFragment(markerSrc, o);
      const std::string& frag = conv.fragment;
      const bool buffersFolded =
          conv.ok && conv.foldedBuffers.size() == 2 &&
          frag.find("st_buffer_a(") != std::string::npos &&
          frag.find("st_buffer_b(") != std::string::npos;
      // the buffer chain and image pass sample through the folded calls, the
      // audio/keyboard channels read black, and the texture channel became a
      // sampler the caller must bind
      const bool rewired =
          frag.find("st_buffer_a(uv * iResolution.xy)") != std::string::npos &&
          frag.find("st_buffer_b(uv * iResolution.xy)") != std::string::npos &&
          frag.find("vec4(0.0)") != std::string::npos &&
          frag.find("uniform sampler2D uChannel3;") != std::string::npos;
      bool bindNoted = false;
      for (const auto& t : conv.requiredTextures)
        if (t.rfind("uChannel3", 0) == 0) bindNoted = true;
      if (!buffersFolded || !rewired || !bindNoted) {
        std::printf("[SHADER-AI] smoke: shadertoy converter FAIL\n");
        return 12;
      }
      // compile the converted shader through the real engine Shader path
      const std::string saved = fragment_;
      fragment_ = conv.fragment;
      fragBuf_ = textBuffer(fragment_);
      if (!compileNow()) {
        std::printf("[SHADER-AI] smoke: shadertoy converted shader compile FAIL\n");
        return 13;
      }
      fragment_ = saved;
    }
    // Standard Shadertoy `#iChannelN "spec"` wiring: multiple texture
    // channels plus a keyboard channel must all wire and compile.
    {
      const std::string hashSrc =
          "#iChannel0 \"https://example.invalid/a.png\"\n"
          "#iChannel1 \"smoke2\"\n"
          "#iChannel2 \"keyboard\"\n"
          "#iChannel3 \"https://example.invalid/b.png\"\n"
          "void mainImage(out vec4 o, in vec2 p) {\n"
          "  vec2 uv = p / iResolution.xy;\n"
          "  o = texture(iChannel0, uv) + texture(iChannel1, uv)\n"
          "    + texture(iChannel2, uv) + texture(iChannel3, uv) + vec4(uv, 0.0, 1.0);\n"
          "}\n";
      ShadertoyConvertOptions hopt;
      hopt.sourceLabel = "smoke-hash";
      const auto hconv = convertShadertoyToFragment(hashSrc, hopt);
      const std::string& hfrag = hconv.fragment;
      if (!hconv.ok || hfrag.find("uniform sampler2D uChannel0;") == std::string::npos ||
          hfrag.find("uniform sampler2D uChannel1;") == std::string::npos ||
          hfrag.find("uniform sampler2D uChannel3;") == std::string::npos ||
          hfrag.find("vec4(0.0)") == std::string::npos) {
        std::printf("[SHADER-AI] smoke: shadertoy #iChannel wiring FAIL\n");
        return 30;
      }
      const std::string saved3 = fragment_;
      fragment_ = hconv.fragment;
      fragBuf_ = textBuffer(fragment_);
      if (!compileNow()) {
        std::printf("[SHADER-AI] smoke: shadertoy #iChannel compile FAIL\n");
        return 31;
      }
      fragment_ = saved3;
    }
    // Auto-binding of a channel texture referenced by a local file name
    // through applyShadertoyConversion (the https:// URL path spawns a real
    // download, which the offline smoke deliberately does not exercise): a
    // cached file in data/textures/ must load and bind immediately, and the
    // URL->cache filename derivation must be deterministic.
    {
      const std::string chanBmp =
          (std::filesystem::path(dataDir_) / "textures" / ".shader_ai_smoke_chan.bmp").string();
      {
        std::filesystem::create_directories(std::filesystem::path(chanBmp).parent_path());
        std::ofstream b(chanBmp, std::ios::binary);
        const unsigned char hdr[54] = {
            'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
            0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        b.write((const char*)hdr, sizeof(hdr));
        const unsigned char px[4] = {0, 0, 255, 0};
        b.write((const char*)px, sizeof(px));
      }
      const std::string chanSrc =
          "#iChannel0 \".shader_ai_smoke_chan.bmp\"\n"
          "void mainImage(out vec4 o, in vec2 p) { o = texture(iChannel0, p / iResolution.xy); }\n";
      ShadertoyConvertOptions copts;
      copts.sourceLabel = "smoke-channel-bind";
      const auto cconv = convertShadertoyToFragment(chanSrc, copts);
      bool ok = cconv.ok;
      if (ok) {
        applyShadertoyConversion(cconv);
        ok = channelTex_[0].tex && !channelPath_[0].empty() && channelPath_[0] == chanBmp &&
             channelDownloadState_[0] == ChannelDownloadState::Ok;
      }
      const bool namesOk = channelTextureFilename("https://www.shadertoy.com/media/a/abc.png") == "abc.png" &&
                           channelTextureFilename("https://i.imgur.com/x.png?w=640") == "x.png" &&
                           channelTextureFilename("https://example.invalid/").rfind("channel_", 0) == 0;
      std::error_code ec;
      std::filesystem::remove(chanBmp, ec);
      // clearing the texture must reset the per-channel download state
      clearChannelTexture(0);
      const bool clearOk = channelDownloadState_[0] == ChannelDownloadState::None &&
                           channelDownloadUrl_[0].empty() && channelDownloadError_[0].empty();
      if (!ok) {
        std::printf("[SHADER-AI] smoke: channel local auto-bind FAIL\n");
        return 32;
      }
      if (!namesOk) {
        std::printf("[SHADER-AI] smoke: channel download filename FAIL\n");
        return 33;
      }
      if (!clearOk) {
        std::printf("[SHADER-AI] smoke: channel download state reset FAIL\n");
        return 34;
      }
    }
    // cancellation: a pre-set cancel flag must abort before any network I/O
    // and leave no (partial) file behind
    {
      std::atomic<bool> cancelled{true};
      const std::string cancelDest =
          (std::filesystem::path(dataDir_) / "textures" / ".shader_ai_smoke_cancel.bmp").string();
      std::string cancelError;
      const bool cancelOk =
          !downloadUrlToFile("https://example.invalid/cancel.bmp", cancelDest, &cancelError, nullptr, &cancelled) &&
          cancelError == "download cancelled" && !std::filesystem::exists(cancelDest);
      if (!cancelOk) {
        std::printf("[SHADER-AI] smoke: channel download cancel FAIL (%s)\n", cancelError.c_str());
        return 35;
      }
    }
    // project load with a stored channel URL: the cached file binds directly
    // (no download), and the URL is preserved so re-saving keeps it
    {
      const std::string projBmp =
          (std::filesystem::path(dataDir_) / "textures" / ".shader_ai_smoke_proj.bmp").string();
      {
        std::filesystem::create_directories(std::filesystem::path(projBmp).parent_path());
        std::ofstream b(projBmp, std::ios::binary);
        const unsigned char hdr[54] = {
            'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
            0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        b.write((const char*)hdr, sizeof(hdr));
        const unsigned char px[4] = {0, 0, 255, 0};
        b.write((const char*)px, sizeof(px));
      }
      ShaderAiProject proj;
      proj.prompt = "load smoke";
      proj.kind = ShaderKind::Fragment;
      proj.fragment =
          "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\nuniform sampler2D uChannel0;\n"
          "void main(){ FragColor = texture(uChannel0, vUV); }\n";
      proj.channelTextures[0] = projBmp;
      proj.channelUrls[0] = "https://example.invalid/proj.png";
      const std::string projPath = (std::filesystem::path(dataDir_) / ".shader_ai_smoke_load.nsshad").string();
      std::string projErr;
      bool ok = proj.save(projPath, &projErr) && loadProject(projPath);
      ok = ok && channelTex_[0].tex && !channelPath_[0].empty() && channelPath_[0] == projBmp &&
           channelDownloadUrl_[0] == proj.channelUrls[0] &&
           channelDownloadState_[0] == ChannelDownloadState::Ok;
      std::error_code ec;
      std::filesystem::remove(projBmp, ec);
      std::filesystem::remove(projPath, ec);
      clearChannelTexture(0);
      if (!ok) {
        std::printf("[SHADER-AI] smoke: channel project load FAIL\n");
        return 36;
      }
    }
    // auto-retry with backoff: HTTP 4xx is permanent (single attempt),
    // transient failures retry up to the attempt cap, a failure followed by
    // success binds on the next attempt, and cancel stops everything
    {
      int calls = 0;
      std::string retryErr;
      // 4xx -> no retry
      calls = 0;
      const bool perm = !runChannelDownload([&](std::string& e) { ++calls; e = "HTTP 404 for url"; return false; },
                                            nullptr, &retryErr, 3, 1);
      if (!perm || calls != 1) {
        std::printf("[SHADER-AI] smoke: download retry 4xx FAIL (calls=%d)\n", calls);
        return 37;
      }
      // transient transport failure -> all three attempts
      calls = 0;
      const bool trans =
          !runChannelDownload([&](std::string& e) { ++calls; e = "download failed at WinHttpReceiveResponse"; return false; },
                              nullptr, &retryErr, 3, 1);
      if (!trans || calls != 3) {
        std::printf("[SHADER-AI] smoke: download retry transient FAIL (calls=%d)\n", calls);
        return 38;
      }
      // failure then success -> two attempts, success reported
      calls = 0;
      const bool recovered = runChannelDownload(
          [&](std::string& e) { ++calls; if (calls < 2) { e = "timeout"; return false; } return true; },
          nullptr, &retryErr, 3, 1);
      if (!recovered || calls != 2) {
        std::printf("[SHADER-AI] smoke: download retry recovery FAIL (calls=%d)\n", calls);
        return 39;
      }
      // cancel flag set up front -> no attempts, "cancelled"
      std::atomic<bool> cancelNow{true};
      calls = 0;
      const bool cancelled =
          !runChannelDownload([&](std::string& e) { ++calls; e = "x"; return false; },
                              &cancelNow, &retryErr, 3, 1) &&
          retryErr == "cancelled" && calls == 0;
      if (!cancelled) {
        std::printf("[SHADER-AI] smoke: download retry cancel FAIL (calls=%d)\n", calls);
        return 40;
      }
      // the onAttempt callback reports the active attempt before each try, so
      // the channel row can show "retrying N/M..." during backoff and fetch
      calls = 0;
      std::vector<int> attemptsSeen;
      const bool seq = !runChannelDownload(
                           [&](std::string& e) { ++calls; e = "timeout"; return false; },
                           nullptr, &retryErr, 3, 1,
                           [&](int attempt, int maxAtt) {
                             attemptsSeen.push_back(attempt);
                             if (maxAtt != 3) attemptsSeen.push_back(-maxAtt);
                           }) &&
                       calls == 3 && attemptsSeen == std::vector<int>({1, 2, 3});
      if (!seq) {
        std::printf("[SHADER-AI] smoke: download retry attempt report FAIL (calls=%d)\n", calls);
        return 41;
      }
    }
    // Shadertoy JSON API export: renderpass code + names map onto the same
    // passes, Sound is skipped, and the folded result compiles. "info" sits
    // BEFORE renderpass on purpose: unwrapping the wrapper copies a member of
    // the same Value back onto itself, which a single-field export hides but
    // a multi-field one corrupts
    {
      const std::string jsonSrc =
          "{\"Shader\":{\"info\":{\"name\":\"smoke\"},\"renderpass\":["
          "{\"name\":\"Common\",\"code\":\"float h(vec2 p){return fract(p.x);}\"},"
          "{\"name\":\"Buffer A\",\"code\":\"void mainImage(out vec4 o, in vec2 p){vec2 uv=p/iResolution.xy;o=vec4(h(uv),0.0,0.0,1.0);}\"},"
          "{\"name\":\"Image\",\"code\":\"void mainImage(out vec4 o, in vec2 p){vec2 uv=p/iResolution.xy;o=texture(iChannel0,uv);}\"},"
          "{\"name\":\"Sound\",\"code\":\"void mainSound(out vec4 s, in vec2 i){s=vec4(0.0);}\"}"
          "]}}";
      ShadertoyConvertOptions jopts;
      jopts.sourceLabel = "smoke-json";
      const auto jconv = convertShadertoyToFragment(jsonSrc, jopts);
      const std::string& jfrag = jconv.fragment;
      bool soundSkipped = false;
      for (const auto& n : jconv.notes)
        if (n.find("Sound") != std::string::npos) soundSkipped = true;
      if (!jconv.ok || jconv.foldedBuffers.size() != 1 ||
          jfrag.find("st_buffer_a(uv * iResolution.xy)") == std::string::npos ||
          !soundSkipped) {
        std::printf("[SHADER-AI] smoke: shadertoy JSON export FAIL\n");
        return 14;
      }
      const std::string saved2 = fragment_;
      fragment_ = jconv.fragment;
      fragBuf_ = textBuffer(fragment_);
      if (!compileNow()) {
        std::printf("[SHADER-AI] smoke: shadertoy JSON export compile FAIL\n");
        return 15;
      }
      fragment_ = saved2;
    }
    // Channel textures: a shader that samples uChannel0 must preview the
    // bound texture, and the unbound default must be the tileable noise (not
    // the old 2x2 checkerboard placeholder that made every channel-using
    // Shadertoy import render as a giant checkerboard).
    {
      GeneratedShader chan;
      chan.kind = ShaderKind::Fragment;
      chan.fragment =
          "#version 330 core\n"
          "in vec2 vUV;\n"
          "out vec4 FragColor;\n"
          "uniform sampler2D uChannel0;\n"
          "void main() { FragColor = texture(uChannel0, vUV); }\n";
      const std::array<bool, 4> used = usedSamplerChannels(chan.fragment);
      if (!used[0] || used[1] || used[2] || used[3]) {
        std::printf("[SHADER-AI] smoke: channel usage detection FAIL\n");
        return 24;
      }
      applyGenerated(chan, "Channel Texture Bind");
      if (!compileNow()) {
        std::printf("[SHADER-AI] smoke: channel shader compile FAIL\n");
        return 25;
      }
      const unsigned char magenta[16] = {
          255, 0, 255, 255, 255, 0, 255, 255,
          255, 0, 255, 255, 255, 0, 255, 255};
      setChannelTexturePixels(0, 2, 2, magenta);
      renderPreview();
      unsigned char chanPixel[4] = {};
      if (!readPreviewPixel(chanPixel) || chanPixel[0] < 240 || chanPixel[1] > 20 || chanPixel[2] < 240) {
        std::printf("[SHADER-AI] smoke: channel texture bind FAIL (%u,%u,%u,%u)\n",
                    chanPixel[0], chanPixel[1], chanPixel[2], chanPixel[3]);
        return 26;
      }
      // reset to the procedural default: must differ from the bound texture
      // and from the old pink/purple checkerboard palette (60/200, 40/120, 90/200)
      clearChannelTexture(0);
      ensureDefaultChannelTexture();
      renderPreview();
      unsigned char noisePixel[4] = {};
      if (!readPreviewPixel(noisePixel) || (noisePixel[0] >= 240 && noisePixel[2] >= 240) ||
          ((noisePixel[0] == 60 || noisePixel[0] == 200) &&
           (noisePixel[1] == 40 || noisePixel[1] == 120) &&
           (noisePixel[2] == 90 || noisePixel[2] == 200))) {
        std::printf("[SHADER-AI] smoke: channel default noise FAIL (%u,%u,%u,%u)\n",
                    noisePixel[0], noisePixel[1], noisePixel[2], noisePixel[3]);
        return 27;
      }
      // file-based load path: a 1x1 red BMP through the same stb_image route
      // the Load... button uses must drive the preview to red
      const std::string bmpPath = (std::filesystem::path(dataDir_) / ".shader_ai_smoke.bmp").string();
      {
        std::ofstream b(bmpPath, std::ios::binary);
        const unsigned char hdr[54] = {
            'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
            0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        b.write((const char*)hdr, sizeof(hdr));
        const unsigned char px[4] = {0, 0, 255, 0};  // BGR + stride pad
        b.write((const char*)px, sizeof(px));
      }
      if (!loadChannelTextureFromFile(0, bmpPath)) {
        std::printf("[SHADER-AI] smoke: channel file load FAIL\n");
        return 28;
      }
      renderPreview();
      unsigned char filePixel[4] = {};
      if (!readPreviewPixel(filePixel) || filePixel[0] < 240 || filePixel[1] > 20 || filePixel[2] > 20) {
        std::printf("[SHADER-AI] smoke: channel file texture bind FAIL (%u,%u,%u,%u)\n",
                    filePixel[0], filePixel[1], filePixel[2], filePixel[3]);
        std::error_code ec;
        std::filesystem::remove(bmpPath, ec);
        return 29;
      }
      std::error_code ec2;
      std::filesystem::remove(bmpPath, ec2);
      clearChannelTexture(0);
    }
    // Playback controls must not touch the active shader document. This also
    // exercises the explicit audio pause/resume state used by Space and the
    // Preview button (without requiring a physical audio file or device).
    {
      const std::string shaderBeforePlayback = fragment_;
      setPlayback(false);
      if (playing_ || audio_.isPlaying() || fragment_ != shaderBeforePlayback) {
        std::printf("[SHADER-AI] smoke: playback pause contract FAIL\n");
        return 16;
      }
      setPlayback(true);
      if (!playing_ || !audio_.isPlaying() || fragment_ != shaderBeforePlayback) {
        std::printf("[SHADER-AI] smoke: playback resume contract FAIL\n");
        return 17;
      }
    }
    std::printf("[SHADER-AI] smoke: generation, pair compile, failure recovery, metadata, model list, project round-trip, shadertoy convert, playback controls - PASS\n");
    return 0;
  }

  void setDelta(float d) {
    delta_ = std::min(0.25f, std::max(0.0f, d));
    if (audioLoaded_) audio_.update();
  }

private:
  GLFWwindow* window_ = nullptr;
  std::string shaderDir_, dataDir_, settingsPath_;
  FrameTarget preview_;
  // Degenerate-output detection ("solid color" / never-drew / near-black
  // shaders) lives in the shared engine RenderProbe; the last verdict is kept
  // here for the UI and the auto-repair path, and the captured frame is PNG-
  // encoded for vision-capable repair requests.
  RenderProbeResult lastProbe_;
  int lastProbeW_ = 0, lastProbeH_ = 0;
  bool flatOutput_ = false;  // uniform-color class (drives the amber status)
  std::string repairImageDataUrl_;
  std::string currentVersionLabel_ = "Version 1";
  std::unique_ptr<Mesh> fsTriangle_;
  std::unique_ptr<Shader> validProgram_;
  std::unique_ptr<ShaderAiProvider> provider_;
  AudioEngine audio_;
  ProviderConfig providerConfig_;
  ShaderKind kind_ = ShaderKind::Fragment;
  std::string prompt_, specification_, explanation_, conversation_, fragment_, vertex_, validFragment_, validVertex_, lastCompiled_;
  std::string diagnostics_, status_ = "Starting...", exportedPath_, projectPath_;
  std::vector<char> promptBuf_, specBuf_, fragBuf_, vertBuf_, savePathBuf_, projectPathBuf_;
  std::vector<ShaderParamDecl> params_;
  std::vector<ShaderAiVersion> history_;
  std::vector<AvailableModel> availableModels_;
  std::string modelStatus_;
  float previewTime_ = 0, previewSpeed_ = 1;
  float simulatedAudio_[6] = {0.35f, 0.5f, 0.35f, 0.25f, 0.0f, 0.0f};
  int previewW_ = 960, previewH_ = 540, selectedHistory_ = -1, sourceLine_ = 0, sourceTab_ = 0;
  bool playing_ = true, sourceDirty_ = false, compileQueued_ = false, imguiReady_ = false;
  bool generationInFlight_ = false;
  // Whether the in-flight request is a repair (true) or a fresh generation
  // (false). A fresh generation that renders a degenerate frame is
  // auto-repaired up to providerConfig_.autoRepairMax times; a repair's own
  // result never re-triggers another automatic ask.
  bool generationIsRepair_ = false;
  // Automatic repair chain counter: bumped by the auto path so the status line
  // can show "auto-repair 1/N" and the final request count; reset to 0 when a
  // fresh generation (or a manual repair) starts.
  int autoRepairsUsed_ = 0;
  std::future<GeneratedShader> generationFuture_;
  bool modelRefreshInFlight_ = false;
  std::future<ModelRefreshResult> modelFuture_;
  bool audioReady_ = false, audioLoaded_ = false;
  bool showSettings_ = false, fullscreenPreview_ = false, showSavePopup_ = false, showProjectPopup_ = false, showShadertoyImport_ = false;
  std::vector<char> shadertoyBuf_ = textBuffer("", 65536);
  // per-channel textures for converted Shadertoy imports (uChannel0..3);
  // unbound channels read the tileable noise default, never a checkerboard
  std::array<Texture, 4> channelTex_;
  std::array<std::string, 4> channelPath_;
  std::array<bool, 4> channelUsed_{};
  Texture defaultChannelTex_;
  // background downloads of channel textures referenced by https:// URLs;
  // results are bound to the channels on the UI thread when the batch lands
  std::vector<ChannelDownloadTask> pendingChannelTasks_;
  std::future<ChannelDownloadBatchResult> channelDownloadFuture_;
  bool channelDownloadInFlight_ = false;
  int channelDownloadBatch_ = 0;
  std::atomic<bool> channelDownloadCancel_{false};
  std::array<ChannelDownloadState, 4> channelDownloadState_{};
  std::array<std::string, 4> channelDownloadUrl_;
  std::array<std::string, 4> channelDownloadError_;
  std::array<DownloadProgress, 4> channelDownloadProgress_;
  // active retry attempt (1..max) reported by the worker while a channel's
  // download is Pending; 0 = not yet started. Rendered as "retrying N/M..."
  std::array<std::atomic<int>, 4> channelDownloadAttempt_{};
  int previewFrame_ = 0;
  int generationSerial_ = 0;
  uint64_t sourceWidgetRevision_ = 0;
  int lastSourceWidgetRevisionLogged_ = -1;
  uint64_t lastVisibleSourceRevisionLogged_ = UINT64_MAX;
  double editClock_ = 0, delta_ = 1.0 / 60.0;
};

} // namespace
} // namespace ns

int main(int argc, char** argv) {
  bool smoke = false; float seconds = 0.0f; int width = 1440, height = 900;
  std::string convertInput, convertOutput, downloadUrl;
  int retryAttempts = 3, retryBackoffMs = 1200;  // match ProviderConfig defaults
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--smoke") smoke = true;
    else if (a == "--help" || a == "-h") {
      std::printf("NULL SECTOR // AI SHADER GENERATOR\n"
                  "  --smoke                   compile a generated pair, test failure recovery, metadata and project IO\n"
                  "  --window=WxH              preview window size (default 1440x900)\n"
                  "  --seconds=N               auto-close after N seconds (CI/manual preview)\n"
                  "  --convert-shadertoy=FILE  convert a Shadertoy .glsl (single-pass or `// pass:` markers)\n"
                  "                            or a JSON API export to one fragment shader\n"
                  "  --out=FILE                output path for --convert-shadertoy (default: input + .frag)\n"
                  "  --download-url=URL        fetch a channel texture into data/textures/ and exit\n"
                  "                            (retries transient failures like the editor pipeline)\n"
                  "  --retry-attempts=N        attempts for --download-url (default 3)\n"
                  "  --retry-backoff-ms=M      base backoff between attempts (default 1200)\n\n"
                  "The UI defaults to the offline Null Sector Demoscene AI. Configure an\n"
                  "OpenAI-compatible provider from View > Settings when desired.\n");
      return 0;
    } else if (a.rfind("--window=", 0) == 0) std::sscanf(a.c_str() + 9, "%dx%d", &width, &height);
    else if (a.rfind("--seconds=", 0) == 0) seconds = (float)std::atof(a.c_str() + 10);
    else if (a.rfind("--convert-shadertoy=", 0) == 0) convertInput = a.substr(20);
    else if (a.rfind("--out=", 0) == 0) convertOutput = a.substr(6);
    else if (a.rfind("--download-url=", 0) == 0) downloadUrl = a.substr(15);
    else if (a.rfind("--retry-attempts=", 0) == 0) retryAttempts = std::max(1, std::atoi(a.c_str() + 17));
    else if (a.rfind("--retry-backoff-ms=", 0) == 0) retryBackoffMs = std::max(0, std::atoi(a.c_str() + 19));
  }
  // headless conversion: no window needed
  if (!convertInput.empty()) {
    std::ifstream f(convertInput);
    if (!f) { std::fprintf(stderr, "[SHADER-AI] cannot read %s\n", convertInput.c_str()); return 1; }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (convertOutput.empty()) convertOutput = convertInput + ".frag";
    ns::ShadertoyConvertOptions opts;
    opts.sourceLabel = convertInput;
    const auto r = ns::convertShadertoyToFragment(text, opts);
    if (!r.ok) { std::fprintf(stderr, "[SHADER-AI] conversion failed: %s\n", r.error.c_str()); return 1; }
    std::ofstream out(convertOutput, std::ios::binary | std::ios::trunc);
    if (!out) { std::fprintf(stderr, "[SHADER-AI] cannot write %s\n", convertOutput.c_str()); return 1; }
    out << r.fragment; out.close();
    std::printf("[SHADER-AI] Converted %s\n", convertInput.c_str());
    std::printf("  Output: %s  (%zu bytes, %d buffer(s) folded)\n",
                convertOutput.c_str(), r.fragment.size(), (int)r.foldedBuffers.size());
    for (const auto& n : r.notes) std::printf("  Note: %s\n", n.c_str());
    for (const auto& t : r.requiredTextures) std::printf("  Bind: %s\n", t.c_str());
    return 0;
  }
  // headless channel texture prefetch (used by the import flow's download
  // pipeline and handy from the command line): fetch into data/textures/.
  // Runs through the same retry helper as the editor worker, so a transient
  // failure (timeout, transport error, HTTP 5xx) backs off and retries while
  // HTTP 4xx stays permanent - a flaky server doesn't fail a prefetch that
  // the app would have recovered from.
  if (!downloadUrl.empty()) {
    const std::string dataDir = ns::resolveRuntimeDir("NULLSECTOR_DATA_DIR", NULLSECTOR_DATA_DIR, "data");
    const std::string dest =
        (std::filesystem::path(dataDir) / "textures" / ns::channelTextureFilename(downloadUrl)).string();
    std::printf("[SHADER-AI] downloading %s -> %s (retry: %d attempts, %d ms backoff)\n",
                downloadUrl.c_str(), dest.c_str(), retryAttempts, retryBackoffMs);
    std::string dlError;
    bool ok = ns::runChannelDownload(
        [&](std::string& err) { return ns::downloadUrlToFile(downloadUrl, dest, &err); },
        nullptr, &dlError, retryAttempts, retryBackoffMs);
    if (!ok) {
      std::fprintf(stderr, "[SHADER-AI] download failed: %s\n", dlError.c_str());
      return 1;
    }
    std::printf("[SHADER-AI] downloaded %zu bytes -> %s\n",
                (size_t)std::filesystem::file_size(dest, std::error_code{}), dest.c_str());
    return 0;
  }
  if (!glfwInit()) { std::fprintf(stderr, "[SHADER-AI] glfwInit failed\n"); return 1; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  GLFWwindow* window = glfwCreateWindow(width, height, "NULL SECTOR // AI SHADER GENERATOR", nullptr, nullptr);
  if (!window) { glfwTerminate(); return 1; }
  glfwMakeContextCurrent(window); glfwSwapInterval(1);
  if (!::glLoadFunctions()) { glfwDestroyWindow(window); glfwTerminate(); return 1; }

  auto dfs = std::make_unique<ns::DirectoryFileSystem>();
  const std::string shaderDir = ns::resolveRuntimeDir("NULLSECTOR_SHADER_DIR", NULLSECTOR_SHADER_DIR, "shaders");
  const std::string dataDir = ns::resolveRuntimeDir("NULLSECTOR_DATA_DIR", NULLSECTOR_DATA_DIR, "data");
  const std::string assetDir = ns::resolveRuntimeDir("NULLSECTOR_ASSET_DIR", NULLSECTOR_ASSET_DIR, "assets");
  std::error_code ec; const std::string cwd = std::filesystem::current_path(ec).string();
  dfs->mount("shaders", shaderDir); dfs->mount("data", dataDir); dfs->mount("assets", assetDir); if (!ec) dfs->mount("", cwd);
  ns::setRuntimeFS(std::move(dfs));

  ns::ShaderAiApp app(window, shaderDir, dataDir);
  app.initImGui();
  if (smoke) {
    const int rc = app.runSmoke();
    app.shutdown();
    glfwDestroyWindow(window); glfwTerminate(); return rc;
  }
  const double started = ns::nowSeconds();
  double last = started;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    const double now = ns::nowSeconds();
    app.setDelta((float)(now - last)); last = now;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app.draw();
    ImGui::Render();
    int fbW = 0, fbH = 0; glfwGetFramebufferSize(window, &fbW, &fbH);
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0); ::glViewport(0, 0, fbW, fbH);
    ::glClearColor(0.015f, 0.02f, 0.035f, 1.0f); ::glClear(::gl::COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
    if (seconds > 0 && now - started >= seconds) glfwSetWindowShouldClose(window, 1);
  }
  app.shutdown();
  glfwDestroyWindow(window); glfwTerminate(); return 0;
}
