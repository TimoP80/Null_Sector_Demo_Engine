// ---------------------------------------------------------------------------
// Null Sector AI Shader Generator - provider-neutral shader authoring core.
// The UI and providers use this small model; exported files remain ordinary
// GLSL and do not depend on the generator at runtime.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/json.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ns {

enum class ShaderKind { Fragment, Vertex, Pair };

enum class ShaderParamType { Float, Int, Bool, Color, Vec2, Vec3, Vec4 };

struct ShaderParamDecl {
  std::string name;
  ShaderParamType type = ShaderParamType::Float;
  float min = 0.0f;
  float max = 1.0f;
  float value = 0.0f;
  float defaultValue = 0.0f;
  std::array<float, 4> vector = {0, 0, 0, 1};
  std::array<float, 4> defaultVector = {0, 0, 0, 1};
};

struct ProviderConfig {
  std::string provider = "builtin-demoscene";
  std::string model = "null-sector-local";
  std::string endpoint = "https://api.openai.com/v1/chat/completions";
  std::string apiKey; // never serialized into a shader project; user settings may protect it
  float temperature = 0.35f;
  int maxTokens = 4096;
  // Seconds the HTTP client waits for the provider to complete the request.
  // Reasoning models (gpt-5, o-series) think before emitting a response, so
  // a non-streaming call can take minutes; the default is intentionally
  // generous and can be tuned in Provider settings.
  int timeoutSeconds = 600;
  // Channel texture download retry policy: number of attempts for transient
  // failures (timeouts, transport errors, HTTP 5xx) and the base backoff in
  // milliseconds between attempts (grows linearly: backoffMs, 2*backoffMs, ...).
  // HTTP 4xx responses are permanent and never retried. Tunable in Provider
  // settings; the active attempt is shown in the Texture Channels row.
  int channelRetryMaxAttempts = 3;
  int channelRetryBackoffMs = 1200;
  // Preview self-check behavior (persisted with the provider settings):
  // auto-repair re-asks the model when a FRESH generation renders a degenerate
  // frame (never drew / uniform / near-black), up to autoRepairMax times per
  // generation (0 disables the auto path entirely - the warning stays);
  // sendRepairImage attaches the failing frame as a vision input on repair
  // (OpenAI-compatible providers that support images).
  bool autoRepairEnabled = true;
  int autoRepairMax = 1;
  bool sendRepairImage = false;
};

struct AvailableModel {
  std::string id;
  std::string owner;
};

struct GenerationRequest {
  // UI-assigned ID used to correlate the asynchronous provider trace with the
  // Generate action that started it.
  int generationId = 0;
  ShaderKind kind = ShaderKind::Fragment;
  std::string prompt;
  std::string currentFragment;
  std::string currentVertex;
  std::string diagnostics;
  ProviderConfig config;
  // Advisory static analysis of the current source ("this output has no
  // per-pixel term"), appended to fresh-generation prompts before any render.
  std::string staticHint;
  // Base64 PNG data URL of the failing frame, attached as a vision input on
  // repair when the provider config allows it.
  std::string repairImageDataUrl;
};

struct GeneratedShader {
  ShaderKind kind = ShaderKind::Fragment;
  std::string specification;
  std::string explanation;
  std::string fragment;
  std::string vertex;
  // Size/hash/short preview of the provider response before shader extraction;
  // zero/empty for the offline built-in provider.
  size_t responseBytes = 0;
  std::string responseHash;
  std::string responsePreview;
};

class ShaderAiProvider {
public:
  virtual ~ShaderAiProvider() = default;
  virtual const char* name() const = 0;
  virtual GeneratedShader generate(const GenerationRequest& request) = 0;
  virtual GeneratedShader repair(const GenerationRequest& request) = 0;
  virtual std::vector<AvailableModel> listModels(const ProviderConfig& config, std::string* error = nullptr) = 0;
};

/** A deterministic provider that ships with the tool. It gives the user a
 * working demoscene shader immediately and is also the offline fallback when
 * no API provider is configured. */
class BuiltinDemosceneProvider final : public ShaderAiProvider {
public:
  const char* name() const override { return "Built-in Demoscene AI"; }
  GeneratedShader generate(const GenerationRequest& request) override;
  GeneratedShader repair(const GenerationRequest& request) override;
  std::vector<AvailableModel> listModels(const ProviderConfig& config, std::string* error = nullptr) override;
};

/** OpenAI-compatible chat-completions provider. It is isolated from the UI
 * and can be replaced by another provider without changing the editor. */
class OpenAiCompatibleProvider final : public ShaderAiProvider {
public:
  const char* name() const override { return "OpenAI-compatible API"; }
  GeneratedShader generate(const GenerationRequest& request) override;
  GeneratedShader repair(const GenerationRequest& request) override;
  std::vector<AvailableModel> listModels(const ProviderConfig& config, std::string* error = nullptr) override;
};

std::unique_ptr<ShaderAiProvider> makeShaderAiProvider(const ProviderConfig& config);

// User-level provider settings. API keys are protected with the platform
// credential store on Windows and are deliberately excluded from .nsshad.
bool saveShaderAiSettings(const std::string& file, const ProviderConfig& config, std::string* error = nullptr);
bool loadShaderAiSettings(const std::string& file, ProviderConfig& config, std::string* error = nullptr);

// Fetch a binary resource (Shadertoy channel texture) over HTTP(S) into a
// local file. Writes to <destPath>.part and renames on success, so a failed
// or interrupted download never leaves a corrupt cached file. Returns true on
// success; on failure *error holds a readable reason. Non-Windows builds
// report that downloads are unavailable.

// Optional live progress sink for downloadUrlToFile: the worker thread bumps
// bytes as data arrives and sets total once the response headers are known
// (0 = length unknown, e.g. chunked encoding).
struct DownloadProgress {
  std::atomic<long long> bytes{0};
  std::atomic<long long> total{0};
};

// Optional cancel signal: when non-null and set, the fetch stops at the next
// safe point (before connecting, or between read chunks) and any partial
// <destPath>.part file is discarded. Returns false with error "download
// cancelled" in that case.
bool downloadUrlToFile(const std::string& url, const std::string& destPath, std::string* error = nullptr,
                       DownloadProgress* progress = nullptr,
                       const std::atomic<bool>* cancel = nullptr);

std::vector<ShaderParamDecl> parseShaderParams(const std::string& source);
std::string shaderParamTypeName(ShaderParamType type);
std::string shaderKindName(ShaderKind kind);

struct ShaderAiVersion {
  std::string label;
  std::string prompt;
  std::string fragment;
  std::string vertex;
  std::string specification;
};

/** Lightweight companion project. API keys are deliberately absent. */
struct ShaderAiProject {
  std::string path;
  std::string prompt;
  std::string specification;
  ShaderKind kind = ShaderKind::Fragment;
  std::string fragment;
  std::string vertex;
  std::vector<ShaderAiVersion> history;
  float previewSpeed = 1.0f;
  int previewWidth = 960;
  int previewHeight = 540;
  // Optional image files bound to the converted shader's uChannel0..3
  // samplers (Shadertoy texture channels). Empty = procedural noise default.
  std::array<std::string, 4> channelTextures;
  // Source URLs the channel textures were downloaded from (empty for local
  // files). Persisted so a missing cache file is re-downloaded on load.
  std::array<std::string, 4> channelUrls;

  bool save(const std::string& file, std::string* error = nullptr) const;
  bool load(const std::string& file, std::string* error = nullptr);
};

/** Advisory static lint: does the current source reference any per-pixel
 *  input (vUV / gl_FragCoord / a texture lookup / a `uv` variable)? Returns a
 *  hint string when the source has none - the classic degenerate generation -
 *  and an empty string when it looks positional or the analysis is
 *  inconclusive. Deliberately conservative: a hint is a nudge, never a fail. */
std::string spatialLintHint(const std::string& source);

/** Encode raw RGBA8 pixels (OpenGL bottom-up row order) as a base64 PNG data
 *  URL, or an empty string on failure. Used to send the failing frame to
 *  vision-capable providers on repair. */
std::string encodePngDataUrl(const unsigned char* rgba, int w, int h);

/** Parse common desktop GLSL compiler formats into a clickable line/column. */
struct ShaderDiagnostic {
  int line = 0;
  int column = 0;
  std::string message;
};
std::vector<ShaderDiagnostic> parseShaderDiagnostics(const std::string& compilerLog);

} // namespace ns
