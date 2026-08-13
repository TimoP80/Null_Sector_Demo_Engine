#include "shader_ai.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#endif

namespace ns {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

uint64_t textSignature(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string textSignatureString(const std::string& text) {
  char result[32];
  std::snprintf(result, sizeof(result), "%016llX", (unsigned long long)textSignature(text));
  return result;
}

std::string tracePreview(const std::string& text) {
  std::string result;
  result.reserve(std::min<size_t>(200, text.size()));
  for (char c : text) {
    if (result.size() >= 200) break;
    if (c == 13 || c == 10 || c == 9) result.push_back(' ');
    else result.push_back(c);
  }
  return result;
}

std::string normalizeApiKey(std::string s) {
  const auto trim = [](std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value.clear(); return; }
    const size_t last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
  };
  // Accept values copied from a .env file, a JSON/string setting, or an
  // Authorization header as well as the raw key from the dashboard.
  if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
      (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
    s.erase(0, 3);
  trim(s);
  for (const char quote : {'"', '\'', '`'}) {
    if (s.size() >= 2 && s.front() == quote && s.back() == quote) {
      s = s.substr(1, s.size() - 2);
      trim(s);
      break;
    }
  }
  const std::string assignment = "openai_api_key=";
  if (lower(s).rfind(assignment, 0) == 0) {
    s.erase(0, assignment.size());
    trim(s);
  }
  if (s.size() > 7 && lower(s.substr(0, 7)) == "bearer ") {
    s.erase(0, 7);
    trim(s);
  }
  // API keys do not contain whitespace; remove copied line wrapping without
  // altering the key characters themselves.
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }), s.end());
  return s;
}

std::string glslFloat(float v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.6g", (double)v);
  return b;
}

std::string stripMarkdownFences(std::string s) {
  const size_t a = s.find("```");
  if (a != std::string::npos) {
    const size_t line = s.find('\n', a);
    const size_t b = s.rfind("```");
    if (line != std::string::npos && b > line) s = s.substr(line + 1, b - line - 1);
  }
  return s;
}

std::string stripFences(std::string s) {
  s = stripMarkdownFences(std::move(s));
  const size_t first = s.find('{');
  const size_t last = s.rfind('}');
  if (first != std::string::npos && last > first) s = s.substr(first, last - first + 1);
  return s;
}

ShaderKind kindFromString(const std::string& s) {
  const std::string x = lower(s);
  if (x == "vertex" || x == "vert") return ShaderKind::Vertex;
  if (x == "pair" || x == "vertex+fragment" || x == "combined") return ShaderKind::Pair;
  return ShaderKind::Fragment;
}

std::string kindForPrompt(ShaderKind k) { return shaderKindName(k); }

std::string compatibilityPreamble() {
  return R"GLSL(#version 300 es
precision highp float;
precision highp int;

// NULL SECTOR COMPATIBILITY MODE
// The preview and Shader Lab bind these ordinary uniforms by name.
uniform vec2 uResolution;
uniform float uTime;
uniform float uBPM;
uniform float uBeat;
uniform float uBar;
uniform float uBeatPhase;
uniform float uAudioLevel;
uniform float uBass;
uniform float uMid;
uniform float uTreble;
uniform float uKick;
uniform float uSnare;
uniform vec4 uColor;
uniform vec4 uColor2;
uniform float uIntensity;
uniform float uSpeed;
uniform float uScale;

)GLSL";
}

std::string commonFunctions() {
  return R"GLSL(
float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}
float noise2(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash21(i), b = hash21(i + vec2(1, 0));
  float c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1, 1));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) {
    v += a * noise2(p);
    p = p * 2.03 + 17.1;
    a *= 0.5;
  }
  return v;
}
vec3 palette(float t) {
  vec3 a = vec3(0.02, 0.01, 0.08);
  vec3 b = vec3(0.00, 0.30, 0.52);
  vec3 c = vec3(0.02, 0.95, 0.95);
  vec3 d = vec3(0.85, 0.08, 0.72);
  return a + b * cos(6.28318 * (c * t + d));
}
)GLSL";
}

std::string makeFragment(const GenerationRequest& r, std::string* technique) {
  const std::string p = lower(r.prompt);
  std::string tech = "layered procedural plasma";
  if (p.find("tunnel") != std::string::npos || p.find("corridor") != std::string::npos ||
      p.find("cable") != std::string::npos) tech = "polar tunnel with cable bands";
  else if (p.find("crt") != std::string::npos || p.find("terminal") != std::string::npos)
    tech = "scanline CRT terminal treatment";
  else if (p.find("raymarch") != std::string::npos || p.find("landscape") != std::string::npos ||
           p.find("metaball") != std::string::npos) tech = "bounded SDF-inspired raymarch";
  else if (p.find("glitch") != std::string::npos || p.find("neuro") != std::string::npos)
    tech = "audio-driven slice glitch";
  if (technique) *technique = tech;

  std::ostringstream s;
  s << compatibilityPreamble()
    << "// @param glow float 0.0 8.0 2.0\n"
    << "// @param distortion float 0.0 1.0 0.12\n"
    << "// @param speed float 0.0 5.0 1.0\n"
    << "// @param color color #00d9ff\n"
    << commonFunctions()
    << "in vec2 vUV;\nout vec4 fragColor;\n"
    << "uniform float glow;\nuniform float distortion;\nuniform float speed;\nuniform vec4 color;\n\n"
    << "void main() {\n"
    << "  vec2 uv = vUV;\n"
    << "  vec2 q = uv - 0.5; q.x *= uResolution.x / max(1.0, uResolution.y);\n"
    << "  float bass = clamp(uBass, 0.0, 1.0);\n"
    << "  float hit = max(uKick, uSnare * 0.55);\n"
    << "  float t = uTime * max(0.05, speed * uSpeed);\n"
    << "  vec3 bg = vec3(0.002, 0.004, 0.015);\n"
    << "  vec3 col = vec3(0.0);\n";

  if (tech == "polar tunnel with cable bands") {
    s << "  float radius = max(0.025, length(q));\n"
      << "  float angle = atan(q.y, q.x);\n"
      << "  float depth = 1.0 / radius + t * (1.2 + bass * 2.0);\n"
      << "  float bands = abs(sin(angle * 9.0 + depth * 0.85 + sin(depth * 0.2) * distortion * 4.0));\n"
      << "  float cables = smoothstep(0.22, 0.02, abs(bands - 0.5));\n"
      << "  float rings = smoothstep(0.18, 0.0, abs(fract(depth * 0.08) - 0.5));\n"
      << "  float glowMask = (cables + rings * 0.4) / (1.0 + radius * 5.0);\n"
      << "  col = mix(vec3(0.01, 0.02, 0.08), palette(angle / 6.28318 + uBar * 0.03), glowMask * (1.5 + glow));\n"
      << "  col += vec3(0.0, 0.35, 0.8) * hit * 2.0;\n";
  } else if (tech == "scanline CRT terminal treatment") {
    s << "  float scan = 0.5 + 0.5 * sin(uv.y * uResolution.y * 0.65);\n"
      << "  float glyph = step(0.78, noise2(floor(uv * vec2(72.0, 38.0)) + vec2(floor(t * 5.0), 0.0)));\n"
      << "  float flicker = 0.82 + 0.18 * noise2(vec2(floor(t * 30.0), 2.0));\n"
      << "  col = vec3(0.05, 0.95, 0.28) * glyph * (0.65 + scan * 0.45) * flicker;\n"
      << "  bg = vec3(0.002, 0.012, 0.006) + vec3(0.0, 0.08, 0.025) * glyph;\n"
      << "  col += vec3(0.0, 0.2, 0.05) * hit;\n";
  } else if (tech == "bounded SDF-inspired raymarch") {
    s << "  vec3 ro = vec3(0.0, 0.0, -3.5 - t * 0.18);\n"
      << "  vec3 rd = normalize(vec3(q, 1.4));\n"
      << "  float hitDist = 0.0; float glowField = 0.0;\n"
      << "  for (int i = 0; i < 48; ++i) {\n"
      << "    vec3 pos = ro + rd * hitDist;\n"
      << "    float d = length(pos.xy) - (0.65 + 0.10 * sin(pos.z * 2.0 + t));\n"
      << "    d = abs(d) + 0.03 * noise2(pos.xy * 3.0 + pos.zz);\n"
      << "    glowField += exp(-10.0 * d) * 0.018;\n"
      << "    hitDist += max(0.012, d * 0.55);\n"
      << "  }\n"
      << "  col = palette(hitDist * 0.08 + uBar * 0.04) * (glowField + hit * 0.3);\n"
      << "  bg = vec3(0.004, 0.002, 0.018);\n";
  } else if (tech == "audio-driven slice glitch") {
    s << "  float slice = floor(uv.y * 48.0);\n"
      << "  float tear = (noise2(vec2(slice, floor(t * 8.0))) - 0.5) * distortion * (0.25 + hit);\n"
      << "  vec2 g = uv + vec2(tear, 0.0);\n"
      << "  float n = fbm(g * 6.0 + vec2(t * 0.4, -t * 0.23));\n"
      << "  col = mix(vec3(0.02, 0.01, 0.08), palette(n + uBar * 0.05), 0.8);\n"
      << "  col.r += noise2(g * 90.0 + t) * hit * 1.7;\n"
      << "  col.b += noise2(g * 120.0 - t) * bass * 1.5;\n";
  } else {
    s << "  float n = fbm(q * (4.0 + uScale * 3.0) + vec2(t * 0.18, -t * 0.12));\n"
      << "  float wave = 0.5 + 0.5 * sin(q.x * 10.0 + n * 7.0 + t);\n"
      << "  col = palette(n * 0.8 + wave * 0.25 + uBar * 0.035) * (0.5 + glow * 0.25);\n"
      << "  col += vec3(0.0, 0.28, 0.55) * bass + vec3(0.7, 0.05, 0.55) * hit;\n";
  }
  s << "  col *= (0.75 + uIntensity * 0.35);\n"
    << "  col *= 1.0 + 0.35 * sin(uv.y * uResolution.y * 0.7) * distortion;\n"
    << "  fragColor = vec4(bg + col, 1.0);\n"
    << "}\n";
  return s.str();
}

std::string makeVertex() {
  return compatibilityPreamble() + R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
  float wave = sin(aPos.x * 8.0 + uTime * uSpeed) * 0.06 * (0.3 + uBass);
  float beatWarp = uKick * 0.08 * sin(aPos.y * 18.0);
  vUV = aUV;
  gl_Position = vec4(aPos.x, aPos.y + wave + beatWarp, 0.0, 1.0);
}
)GLSL";
}

std::string makeSpecification(const GenerationRequest& r, const std::string& technique) {
  std::ostringstream s;
  s << "Visual\n- " << (r.prompt.empty() ? "procedural demoscene atmosphere" : r.prompt)
    << "\n\nMotion\n- continuous uTime animation\n- beat/bar palette drift\n\nAudio\n- bass -> deformation and brightness\n- kick/snare -> impact pulses\n\nTechnique\n- " << technique
    << "\n- fullscreen GLSL 330-compatible source\n- bounded loops suitable for realtime iteration";
  return s.str();
}

GeneratedShader builtinGenerate(const GenerationRequest& r) {
  GeneratedShader out;
  out.kind = r.kind;
  std::string technique;
  out.fragment = makeFragment(r, &technique);
  if (r.kind == ShaderKind::Vertex || r.kind == ShaderKind::Pair) out.vertex = makeVertex();
  out.specification = makeSpecification(r, technique);
  out.explanation = "Generated with the offline Null Sector demoscene provider using the " + technique + " template. The source is ordinary editable GLSL.";
  return out;
}

#ifdef _WIN32
std::string winHttpErrorText(DWORD code) {
  if (code == ERROR_WINHTTP_TIMEOUT)
    return "the provider request timed out; the model may still be thinking - increase the request timeout in Provider settings";
  char buffer[512] = {};
  const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD count = FormatMessageA(flags, nullptr, code, 0, buffer, (DWORD)sizeof(buffer), nullptr);
  std::string text = count ? std::string(buffer, count) : std::string("unknown WinHTTP error");
  while (!text.empty() && (text.back() == '\\r' || text.back() == '\\n' || text.back() == ' ')) text.pop_back();
  return text;
}

std::string winHttpRequest(const std::string& method, const std::string& endpoint,
                           const std::string& body, const std::string& apiKey,
                           int timeoutSeconds) {
  const std::string cleanKey = normalizeApiKey(apiKey);
  if (cleanKey.empty()) throw std::runtime_error("provider API key is empty");
  std::printf("[SHADER-AI][TRACE] provider request: %s %s\n", method.c_str(), endpoint.c_str());
  std::fflush(stdout);
  std::wstring wide;
  int wn = MultiByteToWideChar(CP_UTF8, 0, endpoint.c_str(), -1, nullptr, 0);
  wide.resize((size_t)std::max(1, wn));
  MultiByteToWideChar(CP_UTF8, 0, endpoint.c_str(), -1, wide.data(), wn);
  URL_COMPONENTSW wp{};
  wp.dwStructSize = sizeof(wp);
  wchar_t whost[256] = {}, wpath[2048] = {};
  wp.lpszHostName = whost; wp.dwHostNameLength = 256;
  wp.lpszUrlPath = wpath; wp.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &wp)) throw std::runtime_error("invalid provider endpoint");
  HINTERNET session = WinHttpOpen(L"NullSectorShaderAI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) throw std::runtime_error("WinHTTP session failed");
  std::wstring hostW(whost, wp.dwHostNameLength);
  std::wstring pathW(wpath, wp.dwUrlPathLength);
  HINTERNET conn = WinHttpConnect(session, hostW.c_str(), wp.nPort, 0);
  if (!conn) { WinHttpCloseHandle(session); throw std::runtime_error("provider connection failed"); }
  const DWORD flags = wp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  int mn = MultiByteToWideChar(CP_UTF8, 0, method.c_str(), -1, nullptr, 0);
  std::wstring methodW((size_t)std::max(1, mn), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, method.c_str(), -1, methodW.data(), mn);
  HINTERNET req = WinHttpOpenRequest(conn, methodW.c_str(), pathW.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); throw std::runtime_error("provider request failed"); }
  // Never leave the editor blocked indefinitely when DNS, TLS, or a provider
  // is unavailable. Generation runs off the UI thread, but this also bounds
  // shutdown and gives the user a useful diagnostic in a predictable time.
  // The receive timeout follows the user setting: reasoning models (gpt-5,
  // o-series) can take minutes before the first response byte arrives, so a
  // fixed short budget would abort valid generations.
  const DWORD timeoutMs = (DWORD)std::max(1, timeoutSeconds) * 1000;
  WinHttpSetTimeouts(req, 5000, 5000, std::min<DWORD>(timeoutMs, 60000), timeoutMs);
  std::string headers = "Authorization: Bearer " + cleanKey + "\r\n";
  if (!body.empty()) headers = "Content-Type: application/json\r\n" + headers;
  std::wstring headersW(headers.begin(), headers.end());
  const char* failedStage = nullptr;
  DWORD transportError = ERROR_SUCCESS;
  BOOL ok = WinHttpSendRequest(req, headersW.c_str(), (DWORD)-1L,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0);
  if (!ok) {
    failedStage = "WinHttpSendRequest";
    transportError = GetLastError();
  }
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
      failedStage = "WinHttpReceiveResponse";
      transportError = GetLastError();
    }
  }
  DWORD status = 0, statusSize = sizeof(status);
  if (ok && !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    failedStage = "WinHttpQueryHeaders";
    transportError = GetLastError();
    ok = FALSE;
  }
  std::string response;
  if (ok) {
    DWORD available = 0;
    while (true) {
      if (!WinHttpQueryDataAvailable(req, &available)) {
        failedStage = "WinHttpQueryDataAvailable";
        transportError = GetLastError();
        ok = FALSE;
        break;
      }
      if (available == 0) break;
      std::string chunk(available, '\0'); DWORD got = 0;
      if (!WinHttpReadData(req, chunk.data(), available, &got)) {
        failedStage = "WinHttpReadData";
        transportError = GetLastError();
        ok = FALSE;
        break;
      }
      if (got == 0) break;
      response.append(chunk.data(), got);
    }
  }
  WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
  std::printf("[SHADER-AI][TRACE] provider response: HTTP %lu body=%zu bytes\n",
              (unsigned long)status, response.size());
  if (!ok) {
    std::printf("[SHADER-AI][TRACE] provider transport failure: stage=%s code=%lu (%s)\n",
                failedStage ? failedStage : "unknown", (unsigned long)transportError,
                winHttpErrorText(transportError).c_str());
  }
  std::fflush(stdout);
  if (!ok) {
    throw std::runtime_error(std::string("provider HTTP request failed at ") +
                             (failedStage ? failedStage : "unknown stage") +
                             " (" + std::to_string(transportError) + "): " +
                             winHttpErrorText(transportError));
  }
  if (status >= 400) {
    std::string safeResponse = response;
    size_t at = 0;
    while (!cleanKey.empty() && (at = safeResponse.find(cleanKey, at)) != std::string::npos) {
      safeResponse.replace(at, cleanKey.size(), "<redacted-api-key>");
      at += 19;
    }
    if (status == 401) {
      throw std::runtime_error("OpenAI authentication failed (401). Verify that the active project key is pasted without quotes, that the key is not revoked, and that the endpoint belongs to the same project. Response: " + safeResponse);
    }
    throw std::runtime_error("provider HTTP error " + std::to_string(status) + ": " + safeResponse);
  }
  return response;
}
#endif

std::string modelListEndpoint(const std::string& endpoint) {
  std::string clean = endpoint;
  const size_t query = clean.find('?');
  if (query != std::string::npos) clean.resize(query);
  while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
  const std::string completions = "/chat/completions";
  const std::string responses = "/responses";
  if (clean.size() >= completions.size() && clean.compare(clean.size() - completions.size(), completions.size(), completions) == 0)
    return clean.substr(0, clean.size() - completions.size()) + "/models";
  if (clean.size() >= responses.size() && clean.compare(clean.size() - responses.size(), responses.size(), responses) == 0)
    return clean.substr(0, clean.size() - responses.size()) + "/models";
  const size_t slash = clean.rfind('/');
  return slash == std::string::npos ? clean + "/models" : clean.substr(0, slash + 1) + "models";
}

std::string remotePrompt(const GenerationRequest& r, bool repair) {
  std::ostringstream s;
  s << "You are the Null Sector demoscene shader programmer. Return ONLY a JSON object with keys "
       "specification, explanation, fragment, vertex. No markdown fences.\n"
    << "Write a complete, self-contained GLSL ES 3.00 fragment shader that starts with \"#version 300 es\" "
       "and declares its own output, e.g. \"out vec4 fragColor;\", assigned in main(). Do not use "
       "gl_FragColor. The render pass provides the varying \"in vec2 vUV;\" (screen UV, 0..1) which "
       "you may declare and use, or compute your own per-pixel position with \"vec2 uv = "
       "gl_FragCoord.xy / uResolution;\". THE OUTPUT MUST VARY ACROSS THE SCREEN: every pixel's "
       "color has to depend on its position (vUV / uv / gl_FragCoord), never on uTime or other "
       "uniforms alone - a screen that is one solid flashing color is a bug.\n"
    << "These uniforms are already declared - do not redeclare them: uResolution, uTime, uBPM, "
       "uBeat, uBar, uBeatPhase, uAudioLevel, uBass, uMid, uTreble, uKick, uSnare, uColor, "
       "uColor2, uIntensity, uSpeed, uScale. Include // @param metadata. Keep loops bounded and "
       "avoid undefined engine APIs.\n";
  s << "Shader type: " << shaderKindName(r.kind) << "\n";
  if (repair) s << "Repair this shader without changing its visual intent. Compiler diagnostics:\n" << r.diagnostics << "\n";
  else s << "Create or modify the effect from this request:\n" << r.prompt << "\n";
  if (repair) {
    if (!r.currentFragment.empty()) s << "\nExisting fragment source to repair:\n" << r.currentFragment;
    if (!r.currentVertex.empty()) s << "\nExisting vertex source to repair:\n" << r.currentVertex;
  } else {
    s << "\nGenerate a fresh complete shader from the request. Do not return the existing shader unchanged; rewrite it as a new implementation while preserving the requested visual intent.\n";
  }
  return s.str();
}

std::string responseContent(const Value& root) {
  auto contentText = [](const Value& content) {
    if (content.isStr()) return content.asStr();
    if (!content.isArr()) return std::string();
    std::string result;
    for (const Value& part : content.asArr()) {
      const std::string text = part.get("text").asStr();
      if (!text.empty()) result += text;
      else result += part.get("content").asStr();
    }
    return result;
  };

  // Chat Completions response shape.
  std::string result = contentText(root.at("choices.0.message.content"));
  if (!result.empty()) return result;

  // Responses API response shape. Supporting both lets users select newer
  // models/endpoints without silently treating a valid response as empty.
  for (const Value& item : root.get("output").asArr()) {
    result += contentText(item.get("content"));
    if (!item.get("text").asStr().empty()) result += item.get("text").asStr();
  }
  if (!result.empty()) return result;
  return root.get("output_text").asStr();
}

std::string cleanGeneratedSource(std::string source) {
  source = stripMarkdownFences(std::move(source));
  // Compatible models sometimes put a short explanation before or after a
  // valid shader. Keep the complete declarations and remove that prose.
  size_t start = source.find("#version");
  if (start == std::string::npos) {
    const size_t main = source.find("void main");
    const size_t mainImage = source.find("void mainImage");
    if (main == std::string::npos) start = mainImage;
    else if (mainImage == std::string::npos) start = main;
    else start = std::min(main, mainImage);
    const size_t declaration = source.find("out vec4");
    if (declaration != std::string::npos && (start == std::string::npos || declaration < start)) start = declaration;
  }
  if (start != std::string::npos && start > 0) source.erase(0, start);
  const size_t lastBrace = source.rfind('}');
  if (lastBrace != std::string::npos) source.resize(lastBrace + 1);
  const size_t first = source.find_first_not_of(" \t\r\n");
  if (first != std::string::npos) source.erase(0, first);
  const size_t last = source.find_last_not_of(" \t\r\n");
  if (last != std::string::npos && last + 1 < source.size()) source.resize(last + 1);
  return source;
}

/** Make a provider fragment source compile as a Null Sector fullscreen pass.
 *  Without an explicit #version the source silently compiles as legacy GLSL,
 *  which accepts gl_FragColor and time-only outputs - the classic "flashing
 *  solid color" degenerate shader. The pass provides `in vec2 vUV`; the
 *  canonical output name is `out vec4 fragColor`. */
std::string hardenFragmentSource(std::string source) {
  if (source.find("#version") == std::string::npos)
    source = "#version 300 es\nprecision highp float;\nprecision highp int;\n\n" + source;
  size_t p = 0;
  while ((p = source.find("gl_FragColor", p)) != std::string::npos) {
    source.replace(p, 12, "fragColor");
    p += 9;
  }
  const bool hasFragColor = source.find("out vec4 fragColor") != std::string::npos;
  const bool hasFragColorCap = source.find("out vec4 FragColor") != std::string::npos;
  if (!hasFragColor && hasFragColorCap) {
    // a differently-cased output declaration is renamed to the canonical name
    size_t q = 0;
    while ((q = source.find("FragColor", q)) != std::string::npos) {
      source.replace(q, 9, "fragColor");
      q += 9;
    }
  } else if (!hasFragColor) {
    const size_t main = source.find("void main");
    source.insert(main == std::string::npos ? source.size() : main, "out vec4 fragColor;\n");
  }
  return source;
}

GeneratedShader parseRemote(const std::string& body, ShaderKind kind, int generationId) {
  const std::string json = stripFences(body);
  try {
    const Value root = Json::parseText(json);
    GeneratedShader out;
    out.kind = kind;
    out.specification = root.get("specification").asStr();
    out.explanation = root.get("explanation").asStr();
    out.fragment = hardenFragmentSource(cleanGeneratedSource(root.get("fragment").asStr()));
    out.vertex = cleanGeneratedSource(root.get("vertex").asStr());
    if (out.fragment.empty() && root.get("content").isStr())
      out.fragment = hardenFragmentSource(cleanGeneratedSource(root.get("content").asStr()));
    if (out.fragment.empty()) throw std::runtime_error("provider returned no fragment source");
    std::printf("[SHADER-AI][TRACE] Generate #%d GLSL EXTRACTION RESULT: SUCCESS extracted=%zu bytes hash=%s\n",
                generationId, out.fragment.size(), textSignatureString(out.fragment).c_str());
    std::fflush(stdout);
    return out;
  } catch (const std::exception& jsonError) {
    // Some compatible models ignore the JSON-only instruction and return a
    // fenced or plain GLSL document. It is still useful source, so accept it
    // instead of silently retaining the previous shader.
    const std::string source = stripMarkdownFences(body);
    if (source.find("void main") != std::string::npos ||
        source.find("void mainImage") != std::string::npos) {
      GeneratedShader out;
      out.kind = kind;
      out.fragment = hardenFragmentSource(cleanGeneratedSource(source));
      if (out.fragment.empty()) throw std::runtime_error("provider returned no extractable GLSL source");
      std::printf("[SHADER-AI][TRACE] Generate #%d GLSL EXTRACTION RESULT: SUCCESS (direct GLSL) extracted=%zu bytes hash=%s\n",
                  generationId, out.fragment.size(), textSignatureString(out.fragment).c_str());
      std::fflush(stdout);
      out.explanation = "Provider returned GLSL directly; JSON metadata was unavailable.";
      return out;
    }
    std::printf("[SHADER-AI][TRACE] Generate #%d GLSL EXTRACTION RESULT: FAIL response=%s\n",
                generationId, jsonError.what());
    std::fflush(stdout);
    throw std::runtime_error(std::string("provider response was not valid shader JSON: ") + jsonError.what());
  }
}

GeneratedShader remoteCall(const GenerationRequest& r, bool repair) {
#ifndef _WIN32
  (void)r; (void)repair;
  throw std::runtime_error("OpenAI-compatible provider is currently supported on Windows builds; use the built-in provider offline");
#else
  if (normalizeApiKey(r.config.apiKey).empty()) throw std::runtime_error("provider API key is empty");
  Value root = Value::object();
  root.set("model") = Value(r.config.model);
  const std::string modelName = lower(r.config.model);
  const bool reasoningModel = modelName.find("gpt-5") != std::string::npos ||
                             modelName.rfind("o1", 0) == 0 ||
                             modelName.rfind("o3", 0) == 0 ||
                             modelName.rfind("o4", 0) == 0;
  const bool responsesApi = lower(r.config.endpoint).find("/responses") != std::string::npos;
  if (responsesApi) {
    // The Responses API uses input/max_output_tokens rather than the
    // Chat-Completions messages/max_tokens fields.
    root.set("max_output_tokens") = Value(r.config.maxTokens);
    root.set("input") = Value(remotePrompt(r, repair));
  } else {
    if (reasoningModel) root.set("max_completion_tokens") = Value(r.config.maxTokens);
    else {
      root.set("temperature") = Value((double)r.config.temperature);
      root.set("max_tokens") = Value(r.config.maxTokens);
    }
    Value messages = Value::array();
    Value msg = Value::object();
    msg.set("role") = Value("user");
    msg.set("content") = Value(remotePrompt(r, repair));
    messages.push(std::move(msg));
    root.set("messages") = std::move(messages);
  }
  const std::string response = winHttpRequest("POST", r.config.endpoint, Json::serialize(root, 0),
                                              r.config.apiKey, r.config.timeoutSeconds);
  std::printf("[SHADER-AI][TRACE] Generate #%d REQUEST COMPLETED response=%zu bytes hash=%s preview=\\\"%s\\\"\n",
              r.generationId, response.size(), textSignatureString(response).c_str(), tracePreview(response).c_str());
  std::fflush(stdout);
  Value parsed;
  try {
    parsed = Json::parseText(response);
  } catch (const std::exception& e) {
    std::printf("[SHADER-AI][TRACE] Generate #%d RESPONSE PARSE: FAIL %s", r.generationId, e.what());
    std::putchar('\n');
    std::fflush(stdout);
    throw;
  }
  const std::string content = responseContent(parsed);
  std::printf("[SHADER-AI][TRACE] Generate #%d RESPONSE CONTENT: %zu bytes", r.generationId, content.size());
  std::putchar('\n');
  std::fflush(stdout);
  if (content.empty()) {
    const std::string apiError = parsed.at("error.message").asStr();
    std::printf("[SHADER-AI][TRACE] Generate #%d GLSL EXTRACTION RESULT: FAIL provider content is empty", r.generationId);
    std::putchar('\n');
    std::fflush(stdout);
    throw std::runtime_error(apiError.empty() ? "provider returned an empty response" : apiError);
  }
  GeneratedShader generated = parseRemote(content, r.kind, r.generationId);
  generated.responseBytes = response.size();
  generated.responseHash = textSignatureString(response);
  generated.responsePreview = tracePreview(response);
  return generated;
#endif
}

Value paramToJson(const ShaderParamDecl& p) {
  Value o = Value::object();
  o.set("name") = Value(p.name);
  o.set("type") = Value(shaderParamTypeName(p.type));
  o.set("min") = Value(p.min); o.set("max") = Value(p.max);
  o.set("value") = Value(p.value);
  Value v = Value::array(); for (float x : p.vector) v.push(Value(x));
  o.set("vector") = std::move(v);
  return o;
}

#ifdef _WIN32
std::string hexEncode(const unsigned char* data, size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out; out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 15]);
  }
  return out;
}

bool hexDecode(const std::string& text, std::vector<unsigned char>& out) {
  if (text.size() % 2 != 0) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear(); out.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int hi = digit(text[i]), lo = digit(text[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((unsigned char)((hi << 4) | lo));
  }
  return true;
}
#endif

} // namespace

#ifdef _WIN32
bool downloadUrlToFile(const std::string& url, const std::string& destPath, std::string* error,
                       DownloadProgress* progress, const std::atomic<bool>* cancel) {
  auto fail = [&](const std::string& msg) { if (error) *error = msg; return false; };
  if (cancel && cancel->load()) return fail("download cancelled");
  std::printf("[SHADER-AI][TRACE] texture download: %s -> %s\n", url.c_str(), destPath.c_str());
  std::fflush(stdout);
  std::wstring wide;
  int wn = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
  wide.resize((size_t)std::max(1, wn));
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), wn);
  URL_COMPONENTSW wp{};
  wp.dwStructSize = sizeof(wp);
  wchar_t whost[256] = {}, wpath[4096] = {};
  wp.lpszHostName = whost; wp.dwHostNameLength = 256;
  wp.lpszUrlPath = wpath; wp.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &wp)) return fail("invalid texture URL: " + url);
  HINTERNET session = WinHttpOpen(L"NullSectorShaderAI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return fail("WinHTTP session failed");
  std::wstring hostW(whost, wp.dwHostNameLength);
  std::wstring pathW(wpath, wp.dwUrlPathLength);
  HINTERNET conn = WinHttpConnect(session, hostW.c_str(), wp.nPort, 0);
  if (!conn) { WinHttpCloseHandle(session); return fail("WinHTTP connection failed"); }
  const DWORD flags = wp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET req = WinHttpOpenRequest(conn, L"GET", pathW.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return fail("WinHTTP request failed"); }
  // image hosts (imgur, shadertoy CDNs) redirect: follow the redirect chain
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  WinHttpSetTimeouts(req, 10000, 30000, 30000, 60000);
  WinHttpAddRequestHeaders(req, L"User-Agent: NullSectorShaderAI/1.0 (texture fetch)\r\n", (DWORD)-1L,
                           WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
  const char* failedStage = nullptr;
  DWORD transportError = ERROR_SUCCESS;
  BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (!ok) { failedStage = "WinHttpSendRequest"; transportError = GetLastError(); }
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) { failedStage = "WinHttpReceiveResponse"; transportError = GetLastError(); }
  }
  DWORD status = 0, statusSize = sizeof(status);
  if (ok && !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    failedStage = "WinHttpQueryHeaders";
    transportError = GetLastError();
    ok = FALSE;
  }
  if (!ok) {
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return fail(std::string("download failed at ") + (failedStage ? failedStage : "?") +
                " (" + winHttpErrorText(transportError) + ")");
  }
  if (status < 200 || status >= 300) {
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return fail("HTTP " + std::to_string(status) + " for " + url);
  }
  // expected size for the progress bar (0 = unknown, e.g. chunked encoding)
  if (progress) {
    DWORD len = 0, lenSize = sizeof(len);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &len, &lenSize, WINHTTP_NO_HEADER_INDEX))
      progress->total.store((long long)len);
  }
  // write to <dest>.part and rename on success, so an interrupted download
  // never leaves a corrupt file in the texture cache
  const std::string partPath = destPath + ".part";
  std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return fail("cannot write " + partPath);
  }
  const size_t maxBytes = 64ull * 1024 * 1024;  // sane cap for a channel texture
  size_t total = 0;
  bool readOk = true;
  while (true) {
    if (cancel && cancel->load()) { readOk = false; break; }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(req, &available)) { readOk = false; break; }
    if (available == 0) break;
    if (total + available > maxBytes) { readOk = false; break; }
    std::string chunk(available, '\0');
    DWORD got = 0;
    if (!WinHttpReadData(req, chunk.data(), available, &got)) { readOk = false; break; }
    if (got == 0) break;
    out.write(chunk.data(), got);
    total += got;
    if (progress) progress->bytes.store((long long)total);
  }
  out.close();
  WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
  if (!readOk) {
    std::error_code ec;
    std::filesystem::remove(partPath, ec);
    return fail(cancel && cancel->load() ? "download cancelled" : "texture download interrupted for " + url);
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(destPath).parent_path(), ec);
  std::filesystem::rename(partPath, destPath, ec);
  if (ec) {
    std::filesystem::remove(partPath, ec);
    return fail("cannot move downloaded texture into place: " + ec.message());
  }
  std::printf("[SHADER-AI][TRACE] texture downloaded: %zu bytes -> %s\n", total, destPath.c_str());
  std::fflush(stdout);
  return true;
}
#else
bool downloadUrlToFile(const std::string& url, const std::string& destPath, std::string* error,
                       DownloadProgress* progress, const std::atomic<bool>* cancel) {
  (void)url; (void)destPath; (void)progress; (void)cancel;
  if (error) *error = "texture downloads require a Windows build";
  return false;
}
#endif

std::string shaderKindName(ShaderKind kind) {
  switch (kind) {
    case ShaderKind::Vertex: return "Vertex Shader";
    case ShaderKind::Pair: return "Vertex + Fragment";
    default: return "Fragment Shader";
  }
}

std::string shaderParamTypeName(ShaderParamType type) {
  switch (type) {
    case ShaderParamType::Int: return "int";
    case ShaderParamType::Bool: return "bool";
    case ShaderParamType::Color: return "color";
    case ShaderParamType::Vec2: return "vec2";
    case ShaderParamType::Vec3: return "vec3";
    case ShaderParamType::Vec4: return "vec4";
    default: return "float";
  }
}

std::unique_ptr<ShaderAiProvider> makeShaderAiProvider(const ProviderConfig& config) {
  const std::string provider = lower(config.provider);
  // Entering an API key is an explicit opt-in even if the user left the
  // provider field at its offline default. This prevents model refresh from
  // silently querying the one-model built-in provider.
  if (provider.find("openai") != std::string::npos ||
      provider.find("api") != std::string::npos ||
      !normalizeApiKey(config.apiKey).empty())
    return std::make_unique<OpenAiCompatibleProvider>();
  return std::make_unique<BuiltinDemosceneProvider>();
}

bool saveShaderAiSettings(const std::string& file, const ProviderConfig& config, std::string* error) {
  try {
    Value root = Value::object();
    root.set("format") = Value("null-sector-shader-ai-settings-1");
    root.set("provider") = Value(config.provider);
    root.set("model") = Value(config.model);
    root.set("endpoint") = Value(config.endpoint);
    root.set("temperature") = Value(config.temperature);
    root.set("maxTokens") = Value(config.maxTokens);
    root.set("timeoutSeconds") = Value(config.timeoutSeconds);
    root.set("channelRetryMaxAttempts") = Value(config.channelRetryMaxAttempts);
    root.set("channelRetryBackoffMs") = Value(config.channelRetryBackoffMs);
#ifdef _WIN32
    std::string protectedKey;
    if (!config.apiKey.empty()) {
      DATA_BLOB input{};
      input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(config.apiKey.data()));
      input.cbData = (DWORD)config.apiKey.size();
      DATA_BLOB output{};
      if (!CryptProtectData(&input, L"Null Sector Shader AI API key", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Windows credential protection failed");
      protectedKey = hexEncode(output.pbData, output.cbData);
      LocalFree(output.pbData);
    }
    root.set("apiKeyProtected") = Value(true);
    root.set("apiKey") = Value(protectedKey);
#else
    root.set("apiKeyProtected") = Value(false);
    root.set("apiKey") = Value(config.apiKey);
#endif
    Json::writeFile(file, root, 2);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

bool loadShaderAiSettings(const std::string& file, ProviderConfig& config, std::string* error) {
  try {
    const Value root = Json::parseFile(file);
    config.provider = root.get("provider").asStr(config.provider);
    config.model = root.get("model").asStr(config.model);
    config.endpoint = root.get("endpoint").asStr(config.endpoint);
    config.temperature = root.get("temperature").asFloat(config.temperature);
    config.maxTokens = root.get("maxTokens").asInt(config.maxTokens);
    config.timeoutSeconds = root.get("timeoutSeconds").asInt(config.timeoutSeconds);
    config.channelRetryMaxAttempts = root.get("channelRetryMaxAttempts").asInt(config.channelRetryMaxAttempts);
    config.channelRetryBackoffMs = root.get("channelRetryBackoffMs").asInt(config.channelRetryBackoffMs);
    const std::string storedKey = root.get("apiKey").asStr();
#ifdef _WIN32
    if (root.get("apiKeyProtected").asBool(false)) {
      std::vector<unsigned char> encrypted;
      if (!hexDecode(storedKey, encrypted)) throw std::runtime_error("saved API key settings are corrupt");
      DATA_BLOB input{};
      input.pbData = encrypted.empty() ? nullptr : encrypted.data();
      input.cbData = (DWORD)encrypted.size();
      DATA_BLOB output{};
      if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("saved API key could not be unlocked for this Windows user");
      config.apiKey.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
      LocalFree(output.pbData);
    } else {
      config.apiKey = storedKey;
    }
#else
    config.apiKey = storedKey;
#endif
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

GeneratedShader BuiltinDemosceneProvider::generate(const GenerationRequest& request) {
  return builtinGenerate(request);
}

GeneratedShader BuiltinDemosceneProvider::repair(const GenerationRequest& request) {
  GeneratedShader out = builtinGenerate(request);
  const std::string& f = request.currentFragment;
  if (!f.empty()) {
    out.fragment = f;
    if (out.fragment.find("#version") == std::string::npos)
      out.fragment = compatibilityPreamble() + out.fragment;
    size_t p = 0;
    while ((p = out.fragment.find("gl_FragColor", p)) != std::string::npos) {
      out.fragment.replace(p, 12, "fragColor"); p += 9;
    }
    if (out.fragment.find("out vec4 fragColor") == std::string::npos) {
      const size_t main = out.fragment.find("void main");
      out.fragment.insert(main == std::string::npos ? out.fragment.size() : main,
                          "out vec4 fragColor;\n");
    }
  }
  out.vertex = request.currentVertex.empty() ? out.vertex : request.currentVertex;
  out.explanation = "Applied a bounded compatibility repair: restored version/output declarations and preserved the previous source.";
  return out;
}

std::vector<AvailableModel> BuiltinDemosceneProvider::listModels(const ProviderConfig& config, std::string* error) {
  if (error) error->clear();
  return {{config.model.empty() ? "null-sector-local" : config.model, "built-in"}};
}

GeneratedShader OpenAiCompatibleProvider::generate(const GenerationRequest& request) {
  return remoteCall(request, false);
}

GeneratedShader OpenAiCompatibleProvider::repair(const GenerationRequest& request) {
  return remoteCall(request, true);
}

std::vector<AvailableModel> OpenAiCompatibleProvider::listModels(const ProviderConfig& config, std::string* error) {
  std::vector<AvailableModel> out;
  try {
    if (normalizeApiKey(config.apiKey).empty()) throw std::runtime_error("provider API key is empty");
#ifdef _WIN32
    const std::string response = winHttpRequest("GET", modelListEndpoint(config.endpoint), "",
                                                config.apiKey, config.timeoutSeconds);
    const Value root = Json::parseText(response);
    const Value data = root.get("data");
    if (!data.isArr()) throw std::runtime_error("provider model response has no data array");
    for (const Value& item : data.asArr()) {
      const std::string id = item.get("id").asStr();
      if (!id.empty()) out.push_back({id, item.get("owned_by").asStr()});
    }
    std::sort(out.begin(), out.end(), [](const AvailableModel& a, const AvailableModel& b) { return a.id < b.id; });
    if (out.empty()) throw std::runtime_error("provider returned no models");
#else
    throw std::runtime_error("OpenAI-compatible model discovery is currently supported on Windows builds");
#endif
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    out.clear();
  }
  return out;
}

std::vector<ShaderParamDecl> parseShaderParams(const std::string& source) {
  std::vector<ShaderParamDecl> out;
  std::istringstream in(source);
  std::string line;
  while (std::getline(in, line)) {
    const size_t at = line.find("// @param ");
    if (at == std::string::npos) continue;
    std::istringstream ls(line.substr(at + 10));
    ShaderParamDecl p;
    std::string type;
    ls >> p.name >> type;
    type = lower(type);
    if (p.name.empty()) continue;
    if (type == "float") { p.type = ShaderParamType::Float; ls >> p.min >> p.max >> p.value; p.defaultValue = p.value; }
    else if (type == "int") { p.type = ShaderParamType::Int; ls >> p.min >> p.max >> p.value; p.defaultValue = p.value; }
    else if (type == "bool") { p.type = ShaderParamType::Bool; std::string v; ls >> v; p.value = (lower(v) == "true" ? 1.0f : 0.0f); p.defaultValue = p.value; }
    else if (type == "color") {
      p.type = ShaderParamType::Color; std::string hex; ls >> hex;
      if (hex.size() == 7 && hex[0] == '#') {
        auto h = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10; };
        p.vector = {(h(hex[1]) * 16 + h(hex[2])) / 255.0f,
                    (h(hex[3]) * 16 + h(hex[4])) / 255.0f,
                    (h(hex[5]) * 16 + h(hex[6])) / 255.0f, 1.0f};
        p.defaultVector = p.vector;
      }
    } else {
      if (type == "vec2") p.type = ShaderParamType::Vec2;
      else if (type == "vec3") p.type = ShaderParamType::Vec3;
      else if (type == "vec4") p.type = ShaderParamType::Vec4;
      else continue;
      int n = p.type == ShaderParamType::Vec2 ? 2 : p.type == ShaderParamType::Vec3 ? 3 : 4;
      for (int i = 0; i < n; ++i) ls >> p.vector[(size_t)i];
      p.defaultVector = p.vector;
    }
    if (p.max <= p.min) p.max = p.min + 1.0f;
    out.push_back(p);
  }
  return out;
}

std::vector<ShaderDiagnostic> parseShaderDiagnostics(const std::string& log) {
  std::vector<ShaderDiagnostic> out;
  std::istringstream in(log);
  std::string line;
  const std::regex r1(R"((?:ERROR|WARNING)?\s*:?\s*\d+:(\d+)(?::(\d+))?\s*:?[ \t]*(.*))", std::regex::icase);
  const std::regex r2(R"((?:line|Line)\s+(\d+)(?::(\d+))?\s*:?\s*(.*))");
  while (std::getline(in, line)) {
    std::smatch m;
    ShaderDiagnostic d;
    if (std::regex_search(line, m, r1)) {
      d.line = std::atoi(m[1].str().c_str());
      d.column = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
      d.message = m[3].str();
    } else if (std::regex_search(line, m, r2)) {
      d.line = std::atoi(m[1].str().c_str());
      d.column = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
      d.message = m[3].str();
    } else if (!line.empty()) {
      d.message = line;
    }
    if (!d.message.empty()) out.push_back(std::move(d));
  }
  return out;
}

bool ShaderAiProject::save(const std::string& file, std::string* error) const {
  try {
    Value root = Value::object();
    root.set("format") = Value("null-sector-shader-ai-1");
    root.set("prompt") = Value(prompt);
    root.set("specification") = Value(specification);
    root.set("kind") = Value(kind == ShaderKind::Vertex ? "vertex" : kind == ShaderKind::Pair ? "pair" : "fragment");
    root.set("fragment") = Value(fragment);
    root.set("vertex") = Value(vertex);
    root.set("previewSpeed") = Value(previewSpeed);
    root.set("previewWidth") = Value(previewWidth);
    root.set("previewHeight") = Value(previewHeight);
    Value ct = Value::array();
    for (const auto& s : channelTextures) ct.push(Value(s));
    root.set("channelTextures") = std::move(ct);
    Value cu = Value::array();
    for (const auto& s : channelUrls) cu.push(Value(s));
    root.set("channelUrls") = std::move(cu);
    Value hs = Value::array();
    for (const auto& h : history) {
      Value x = Value::object(); x.set("label") = Value(h.label); x.set("prompt") = Value(h.prompt);
      x.set("fragment") = Value(h.fragment); x.set("vertex") = Value(h.vertex); x.set("specification") = Value(h.specification);
      hs.push(std::move(x));
    }
    root.set("history") = std::move(hs);
    Json::writeFile(file, root, 2);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

bool ShaderAiProject::load(const std::string& file, std::string* error) {
  try {
    const Value root = Json::parseFile(file);
    path = file; prompt = root.get("prompt").asStr(); specification = root.get("specification").asStr();
    kind = kindFromString(root.get("kind").asStr()); fragment = root.get("fragment").asStr(); vertex = root.get("vertex").asStr();
    previewSpeed = root.get("previewSpeed").asFloat(1.0f); previewWidth = root.get("previewWidth").asInt(960); previewHeight = root.get("previewHeight").asInt(540);
    channelTextures = {};
    if (root.get("channelTextures").isArr()) {
      const auto& arr = root.get("channelTextures").asArr();
      for (size_t i = 0; i < arr.size() && i < channelTextures.size(); ++i) channelTextures[i] = arr[i].asStr();
    }
    channelUrls = {};
    if (root.get("channelUrls").isArr()) {
      const auto& arr = root.get("channelUrls").asArr();
      for (size_t i = 0; i < arr.size() && i < channelUrls.size(); ++i) channelUrls[i] = arr[i].asStr();
    }
    history.clear();
    if (root.get("history").isArr()) for (const Value& x : root.get("history").asArr()) {
      ShaderAiVersion h; h.label = x.get("label").asStr(); h.prompt = x.get("prompt").asStr(); h.fragment = x.get("fragment").asStr(); h.vertex = x.get("vertex").asStr(); h.specification = x.get("specification").asStr(); history.push_back(std::move(h));
    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

} // namespace ns
