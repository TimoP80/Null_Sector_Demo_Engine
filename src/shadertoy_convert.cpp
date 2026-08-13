// ---------------------------------------------------------------------------
// shadertoy_convert.cpp - GL-free Shadertoy -> single fragment shader converter.
// Reuses the engine's strict pass splitter (app/shadertoyparse.hpp) so a file
// that parses for the runtime importer converts identically. Shadertoy JSON
// API exports are parsed down to the same `// pass:` marker text first. All
// channel work is text transformation; nothing here touches the GL.
// ---------------------------------------------------------------------------
#include "shadertoy_convert.hpp"

#include "app/shadertoyparse.hpp"
#include "framework/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace ns {
namespace {

// ---------------------------------------------------------------------------
// small comment/string/preprocessor-aware source scanner
// ---------------------------------------------------------------------------
struct Edit {
  size_t begin = 0, end = 0;
  std::string text;
};

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

/** find the index of the ')' matching s[open] (a '('), skipping comments and
 *  string literals; returns std::string::npos when unbalanced. */
size_t matchParen(const std::string& s, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      const size_t nl = s.find('\n', i);
      i = nl == std::string::npos ? s.size() : nl + 1;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      const size_t close = s.find("*/", i + 2);
      i = close == std::string::npos ? s.size() : close + 2;
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') ++i;
        ++i;
      }
      ++i;
      continue;
    }
    if (c == '(') ++depth;
    else if (c == ')') {
      --depth;
      if (depth == 0) return i;
    }
    ++i;
  }
  return std::string::npos;
}

/** tokenize into identifiers with their positions; comments, strings and
 *  preprocessor lines are skipped (no identifiers inside them). */
std::vector<std::pair<size_t, size_t>> identifiers(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      const size_t nl = s.find('\n', i);
      i = nl == std::string::npos ? s.size() : nl + 1;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      const size_t close = s.find("*/", i + 2);
      i = close == std::string::npos ? s.size() : close + 2;
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') ++i;
        ++i;
      }
      ++i;
      continue;
    }
    if (c == '#') {
      const size_t nl = s.find('\n', i);
      i = nl == std::string::npos ? s.size() : nl + 1;
      continue;
    }
    if (isIdentStart(c)) {
      size_t j = i + 1;
      while (j < s.size() && isIdentChar(s[j])) ++j;
      out.emplace_back(i, j);
      i = j;
      continue;
    }
    ++i;
  }
  return out;
}

/** GLSL reserved words that can be followed by ( ... ) { at top level; these
 *  must never be treated as user function definitions. */
bool isControlKeyword(const std::string& name) {
  static const std::set<std::string> k = {"if", "for", "while", "switch", "return"};
  return k.count(name) != 0;
}

/** names of #define macros in the source - calls to macros must NOT be
 *  renamed (the preprocessor would still expand the old name). */
std::set<std::string> macroNames(const std::string& s) {
  std::set<std::string> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) {
    size_t p = line.find("#define");
    if (p == std::string::npos) continue;
    size_t q = p + 7;
    while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
    size_t r = q;
    while (r < line.size() && isIdentChar(line[r])) ++r;
    if (r > q) out.insert(line.substr(q, r - q));
  }
  return out;
}

// ---------------------------------------------------------------------------
// top-level function detection + collision-safe renaming
// ---------------------------------------------------------------------------
/** rename `from` -> `to` wherever it is used as a callable (identifier
 *  directly followed by '('), skipping comments/strings/preprocessor. */
std::string renameCallable(const std::string& src, const std::string& from, const std::string& to) {
  std::string out;
  out.reserve(src.size());
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      const size_t nl = src.find('\n', i);
      out.append(src, i, (nl == std::string::npos ? src.size() : nl + 1) - i);
      i = nl == std::string::npos ? src.size() : nl + 1;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      const size_t close = src.find("*/", i + 2);
      const size_t end = close == std::string::npos ? src.size() : close + 2;
      out.append(src, i, end - i);
      i = end;
      continue;
    }
    if (c == '"') {
      const size_t start = i;
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\') ++i;
        ++i;
      }
      if (i < src.size()) ++i;
      out.append(src, start, i - start);
      continue;
    }
    if (c == '#') {
      const size_t nl = src.find('\n', i);
      out.append(src, i, (nl == std::string::npos ? src.size() : nl + 1) - i);
      i = nl == std::string::npos ? src.size() : nl + 1;
      continue;
    }
    if (isIdentStart(c)) {
      size_t j = i + 1;
      while (j < src.size() && isIdentChar(src[j])) ++j;
      const std::string word = src.substr(i, j - i);
      size_t k = j;
      while (k < src.size() && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) ++k;
      if (word == from && k < src.size() && src[k] == '(') {
        out += to;
        i = j;
        continue;
      }
      out.append(src, i, j - i);
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

/** names of top-level user function definitions in a pass body (not control
 *  keywords, not mainImage/main, not macros). */
std::set<std::string> topLevelFunctionNames(const std::string& src) {
  std::set<std::string> names;
  const auto macros = macroNames(src);
  // walk tokens, track brace depth; a definition is ident ( ... ) { at depth 0
  int depth = 0;
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      const size_t nl = src.find('\n', i);
      i = nl == std::string::npos ? src.size() : nl + 1;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      const size_t close = src.find("*/", i + 2);
      i = close == std::string::npos ? src.size() : close + 2;
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\') ++i;
        ++i;
      }
      ++i;
      continue;
    }
    if (c == '#') {
      const size_t nl = src.find('\n', i);
      i = nl == std::string::npos ? src.size() : nl + 1;
      continue;
    }
    if (isIdentStart(c)) {
      size_t j = i + 1;
      while (j < src.size() && isIdentChar(src[j])) ++j;
      const std::string word = src.substr(i, j - i);
      if (depth == 0 && word != "mainImage" && word != "main" && !isControlKeyword(word) &&
          macros.count(word) == 0) {
        size_t k = j;
        while (k < src.size() && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) ++k;
        if (k < src.size() && src[k] == '(') {
          const size_t close = matchParen(src, k);
          if (close != std::string::npos) {
            size_t b = close + 1;
            while (b < src.size() && (src[b] == ' ' || src[b] == '\t' || src[b] == '\r' || src[b] == '\n')) ++b;
            if (b < src.size() && src[b] == '{') names.insert(word);
          }
        }
      }
      i = j;
      continue;
    }
    if (c == '{') ++depth;
    else if (c == '}') depth = std::max(0, depth - 1);
    ++i;
  }
  return names;
}

/** prefix every top-level user function in a pass with `prefix` so passes
 *  folded into one file cannot collide (common Shadertoy passes all define
 *  their own hash/noise/warp helpers). mainImage is renamed too - the caller
 *  emits a wrapper that calls the prefixed name. */
std::string prefixPassFunctions(const std::string& src, const std::string& prefix) {
  std::string out = src;
  const std::set<std::string> fns = topLevelFunctionNames(src);
  for (const auto& fn : fns) {
    out = renameCallable(out, fn, prefix + fn);
  }
  if (out.find("mainImage") != std::string::npos) {
    out = renameCallable(out, "mainImage", prefix + "mainImage");
  }
  return out;
}

// ---------------------------------------------------------------------------
// channel call rewriting
// ---------------------------------------------------------------------------
const char* kTextureFuncs[] = {"texture", "textureLod", "texture2D", "texture2DLodEXT",
                               "textureProj", "texture2DProj"};

/** split the inside of texture( ... ) at top-level commas. */
std::vector<std::string> splitTopLevelArgs(const std::string& s, size_t open, size_t close) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = open + 1;
  for (size_t i = open + 1; i <= close; ++i) {
    if (i == close) {
      std::string arg = s.substr(start, i - start);
      arg.erase(arg.find_last_not_of(" \t\r\n") == std::string::npos ? 0 : arg.find_last_not_of(" \t\r\n") + 1);
      arg.erase(0, arg.find_first_not_of(" \t\r\n"));
      if (!arg.empty()) out.push_back(arg);
      break;
    }
    if (s[i] == '(') ++depth;
    else if (s[i] == ')') --depth;
    else if (s[i] == ',' && depth == 0) {
      std::string arg = s.substr(start, i - start);
      arg.erase(arg.find_last_not_of(" \t\r\n") == std::string::npos ? 0 : arg.find_last_not_of(" \t\r\n") + 1);
      arg.erase(0, arg.find_first_not_of(" \t\r\n"));
      if (!arg.empty()) out.push_back(arg);
      start = i + 1;
    }
  }
  return out;
}

/** recursively rewrite texture(iChannelN, ...) calls inside one pass body. */
std::string rewriteChannelCalls(const std::string& src,
                                const std::array<ShadertoyChannelBind, 4>& channels,
                                const std::map<std::string, std::string>& bufferFuncs,
                                std::vector<std::string>& notes,
                                std::vector<std::string>& requiredTextures) {
  std::string out;
  out.reserve(src.size());
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      const size_t nl = src.find('\n', i);
      out.append(src, i, (nl == std::string::npos ? src.size() : nl + 1) - i);
      i = nl == std::string::npos ? src.size() : nl + 1;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      const size_t close = src.find("*/", i + 2);
      const size_t end = close == std::string::npos ? src.size() : close + 2;
      out.append(src, i, end - i);
      i = end;
      continue;
    }
    if (c == '"') {
      const size_t start = i;
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\') ++i;
        ++i;
      }
      if (i < src.size()) ++i;
      out.append(src, start, i - start);
      continue;
    }
    if (isIdentStart(c)) {
      size_t j = i + 1;
      while (j < src.size() && isIdentChar(src[j])) ++j;
      const std::string word = src.substr(i, j - i);
      const bool isTexFunc = std::find_if(std::begin(kTextureFuncs), std::end(kTextureFuncs),
                                          [&](const char* f) { return word == f; }) != std::end(kTextureFuncs);
      if (!isTexFunc) {
        out.append(src, i, j - i);
        i = j;
        continue;
      }
      size_t k = j;
      while (k < src.size() && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) ++k;
      if (k >= src.size() || src[k] != '(') {
        out.append(src, i, j - i);
        i = j;
        continue;
      }
      // does this call start with iChannelN ?
      size_t q = k + 1;
      while (q < src.size() && (src[q] == ' ' || src[q] == '\t' || src[q] == '\r' || src[q] == '\n')) ++q;
      int ch = -1;
      if (q + 9 <= src.size() && src.compare(q, 8, "iChannel") == 0 && src[q + 8] >= '0' && src[q + 8] <= '3') {
        ch = src[q + 8] - '0';
      }
      if (ch < 0) {
        out.append(src, i, j - i);
        i = j;
        continue;
      }
      const size_t close = matchParen(src, k);
      if (close == std::string::npos) {
        out.append(src, i, j - i);
        i = j;
        continue;
      }
      const auto args = splitTopLevelArgs(src, k, close);
      std::string uv = args.size() > 1 ? args[1] : "vec2(0.5)";
      const ShadertoyChannelBind& bind = channels[(size_t)ch];
      if (bind.kind == ShadertoyChannelKind::Buffer) {
        const auto it = bufferFuncs.find(bind.target);
        if (it == bufferFuncs.end()) {
          notes.push_back("channel iChannel" + std::to_string(ch) + " references unknown buffer '" +
                          bind.target + "' - replaced with vec4(0.0)");
          out += "vec4(0.0)";
        } else {
          uv = rewriteChannelCalls(uv, channels, bufferFuncs, notes, requiredTextures);
          out += it->second + "(" + uv + " * iResolution.xy)";
        }
      } else if (bind.kind == ShadertoyChannelKind::Texture) {
        const std::string name = "uChannel" + std::to_string(ch);
        requiredTextures.push_back(name + " (" + (bind.target.empty() ? "bind a texture" : bind.target) + ")");
        uv = rewriteChannelCalls(uv, channels, bufferFuncs, notes, requiredTextures);
        const std::string fn = word == "textureLod" || word == "texture2DLodEXT" ? "texture" : word;
        out += fn + "(" + name + ", " + uv + ")";
      } else {
        if (bind.kind == ShadertoyChannelKind::Audio)
          notes.push_back("channel iChannel" + std::to_string(ch) + " is audio - replaced with vec4(0.0)");
        else if (bind.kind == ShadertoyChannelKind::Keyboard)
          notes.push_back("channel iChannel" + std::to_string(ch) + " is keyboard - replaced with vec4(0.0)");
        out += "vec4(0.0)";
      }
      i = close + 1;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// ---------------------------------------------------------------------------
// pass preprocessing
// ---------------------------------------------------------------------------
/** strip Shadertoy boilerplate that would collide with the emitted shim:
 *  #version, #extension, precision lines and Shadertoy i* uniform declarations. */
std::string stripShadertoyBoilerplate(const std::string& src) {
  std::istringstream in(src);
  std::ostringstream out;
  std::string line;
  const std::regex uniformRe(
      R"(^\s*uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(?:float|int|vec2|vec3|vec4|sampler2D|samplerCube)\s+i(?:Time|TimeDelta|Frame|FrameRate|Resolution|Mouse|Date|SampleRate|ChannelTime|ChannelResolution|Channel[0-3])\s*(?:\[[^\]]*\])?\s*;)");
  const std::regex versionRe(R"(^\s*#\s*(version|extension)\b)");
  const std::regex precisionRe(R"(^\s*precision\s+(?:highp|mediump|lowp)\s+(?:float|int)\s*;)");
  while (std::getline(in, line)) {
    if (std::regex_search(line, versionRe)) continue;
    if (std::regex_search(line, precisionRe)) continue;
    if (std::regex_search(line, uniformRe)) continue;
    out << line << "\n";
  }
  return out.str();
}

/** parse `// channel: iChannelN = spec` wiring comments out of a pass and
 *  remove them from the emitted source. */
std::array<ShadertoyChannelBind, 4> extractChannelComments(std::string& src) {
  std::array<ShadertoyChannelBind, 4> binds;
  std::istringstream in(src);
  std::ostringstream out;
  std::string line;
  const std::regex re(R"(^\s*//\s*channel\s*:\s*iChannel([0-3])\s*=\s*(\S+))");
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re)) {
      const int idx = m[1].str()[0] - '0';
      const std::string spec = m[2].str();
      const std::string lower = [&]() { std::string x = spec; for (char& c2 : x) c2 = (char)std::tolower((unsigned char)c2); return x; }();
      if (lower == "none") binds[(size_t)idx].kind = ShadertoyChannelKind::None;
      else if (lower == "audio") binds[(size_t)idx].kind = ShadertoyChannelKind::Audio;
      else if (lower == "keyboard") binds[(size_t)idx].kind = ShadertoyChannelKind::Keyboard;
      else if (lower.rfind("buffer:", 0) == 0) {
        binds[(size_t)idx].kind = ShadertoyChannelKind::Buffer;
        binds[(size_t)idx].target = spec.substr(7);
      } else if (lower.rfind("texture:", 0) == 0) {
        binds[(size_t)idx].kind = ShadertoyChannelKind::Texture;
        binds[(size_t)idx].target = spec.substr(8);
      } else if (lower.rfind("scene", 0) == 0) {
        binds[(size_t)idx].kind = ShadertoyChannelKind::Texture;
        binds[(size_t)idx].target = "live scene";
      } else {
        // a bare word is a buffer pass name (the engine's channel wiring)
        binds[(size_t)idx].kind = ShadertoyChannelKind::Buffer;
        binds[(size_t)idx].target = spec;
      }
      continue;
    }
    // standard Shadertoy resource lines: `#iChannelN "spec"` (the syntax every
    // Shadertoy export/copy uses for textures, cubemaps, videos, audio and
    // keyboard channels - the `// channel:` form above is the engine's richer
    // equivalent). Quoted specs may be URLs, file names or keyword channels.
    {
      size_t p = 0;
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      if (p < line.size() && line[p] == '#') {
        size_t q = p + 1;
        while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
        if (q + 9 <= line.size() && line.compare(q, 8, "iChannel") == 0 &&
            line[q + 8] >= '0' && line[q + 8] <= '3') {
          const int idx = line[q + 8] - '0';
          size_t s = q + 9;
          while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
          std::string spec;
          if (s < line.size() && (line[s] == '"' || line[s] == '\'')) {
            const char quote = line[s];
            const size_t e = line.find(quote, s + 1);
            if (e != std::string::npos) spec = line.substr(s + 1, e - s - 1);
          } else {
            spec = line.substr(s);
            const size_t c = spec.find("//");
            if (c != std::string::npos) spec.resize(c);
          }
          const size_t f = spec.find_first_not_of(" \t\r");
          const size_t l = spec.find_last_not_of(" \t\r");
          if (f != std::string::npos) spec = spec.substr(f, l - f + 1);
          const std::string lower = [&]() { std::string x = spec; for (char& c2 : x) c2 = (char)std::tolower((unsigned char)c2); return x; }();
          ShadertoyChannelBind& bind = binds[(size_t)idx];
          if (lower == "none") bind.kind = ShadertoyChannelKind::None;
          else if (lower == "audio") bind.kind = ShadertoyChannelKind::Audio;
          else if (lower == "keyboard") bind.kind = ShadertoyChannelKind::Keyboard;
          else {
            bind.kind = ShadertoyChannelKind::Texture;
            bind.target = spec.empty() ? "bind a texture" : spec;
          }
          continue;
        }
      }
    }
    out << line << "\n";
  }
  src = out.str();
  return binds;
}

/** Standard Shadertoy code never carries its texture wiring inside the file -
 *  the resources are configured in the Shadertoy editor UI, so pasted/exported
 *  code often samples iChannelN with no comment at all. Keep any sampled but
 *  unwired channel as a bindable texture sampler instead of silently replacing
 *  it with black (the old behaviour for channels past iChannel0/1). */
void inferSampledTextureChannels(const std::string& body,
                                 std::array<ShadertoyChannelBind, 4>& chans,
                                 std::vector<std::string>& notes) {
  for (int ci = 0; ci < 4; ++ci) {
    if (chans[(size_t)ci].kind != ShadertoyChannelKind::None) continue;
    bool sampled = false;
    for (const char* fn : kTextureFuncs) {
      const std::string needle = std::string(fn) + "(iChannel" + std::to_string(ci);
      if (body.find(needle) != std::string::npos) { sampled = true; break; }
    }
    if (sampled) {
      chans[(size_t)ci].kind = ShadertoyChannelKind::Texture;
      chans[(size_t)ci].target = "auto-inferred texture (bind an image or it reads the default noise)";
      notes.push_back("channel iChannel" + std::to_string(ci) +
                      " has no wiring comment - inferred as a texture channel (bind an image or it reads the default noise)");
    }
  }
}

/** the emitted uniform shim block. All Null Sector standard uniforms are
 *  declared so the converted file previews everywhere, plus the Shadertoy
 *  extras that have no Null Sector equivalent. */
std::string shimBlock(const std::set<int>& samplerChannels) {
  std::ostringstream s;
  s << "// ---- NULL SECTOR STANDARD UNIFORMS ----\n"
    << "uniform vec2 uResolution;\n"
    << "uniform float uTime;\n"
    << "uniform float uBPM;\n"
    << "uniform float uBeat;\n"
    << "uniform float uBar;\n"
    << "uniform float uBeatPhase;\n"
    << "uniform float uAudioLevel;\n"
    << "uniform float uBass;\n"
    << "uniform float uMid;\n"
    << "uniform float uTreble;\n"
    << "uniform float uKick;\n"
    << "uniform float uSnare;\n"
    << "uniform vec4 uColor;\n"
    << "uniform vec4 uColor2;\n"
    << "uniform float uIntensity;\n"
    << "uniform float uSpeed;\n"
    << "uniform float uScale;\n"
    << "\n// ---- SHADERTOY EXTRAS ----\n"
    << "uniform float uTimeDelta;\n"
    << "uniform int   uFrame;\n"
    << "uniform vec4  uMouse;\n"
    << "uniform vec4  uDate;\n"
    << "uniform vec4  uChannelTime;\n"
    << "uniform vec4  uChannelResolution[4];\n";
  for (int ch : samplerChannels)
    s << "uniform sampler2D uChannel" << ch << ";\n";
  s << "\n// ---- SHADERTOY COMPATIBILITY SHIMS ----\n"
    << "#define iResolution vec3(uResolution, 1.0)\n"
    << "#define iTime uTime\n"
    << "#define iGlobalTime uTime\n"
    << "#define iTimeDelta uTimeDelta\n"
    << "#define iFrame uFrame\n"
    << "#define iMouse uMouse\n"
    << "#define iDate uDate\n"
    << "#define iSampleRate 44100.0\n"
    << "#define iChannelTime uChannelTime\n"
    << "#define iChannelResolution uChannelResolution\n"
    << "#define texture2D texture\n"
    << "#define texture2DLodEXT textureLod\n"
    << "#define texture2DProj textureProj\n"
    << "\n";
  return s.str();
}

std::string sanitizePassKey(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (isIdentChar(c)) out += (char)std::tolower((unsigned char)c);
    else out += '_';
  }
  if (out.empty() || !isIdentStart(out[0])) out = "pass_" + out;
  return out;
}

// ---------------------------------------------------------------------------
// Shadertoy JSON API export support
// ---------------------------------------------------------------------------
/** Render-pass names in an API export can be localized, so matching is
 *  case-insensitive with the English names as fallback; anything unrecognised
 *  is skipped with a note rather than mis-mapped. */
static bool jsonPassMarker(const std::string& name, std::string& marker) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) lower += (char)std::tolower((unsigned char)c);
  if (lower == "common") { marker = "common"; return true; }
  if (lower == "image") { marker = "image"; return true; }
  if (lower.rfind("buffer ", 0) == 0 && lower.size() >= 8) {
    const char letter = lower[7];
    if (letter >= 'a' && letter <= 'd') { marker = "buffer_"; marker += letter; return true; }
  }
  return false;
}

/** Parse a Shadertoy API JSON export ({"Shader": {"renderpass": [...]}} -
 *  the array-wrapped bulk form and a top-level renderpass are accepted too)
 *  into the engine's `// pass:` marker text. Returns 1 on success, 0 when the
 *  source is not JSON (the caller falls back to the GLSL path), and -1 when
 *  the JSON parses but has no usable renderpass array (error set). */
static int parseShadertoyJsonExport(const std::string& source, std::string& markerText,
                                    std::vector<std::string>& notes, std::string& error) {
  Value root;
  try {
    root = Json::parse(source);
  } catch (const JsonError&) {
    return 0;  // not JSON - leave it to the GLSL path
  }
  // locate the renderpass array: {"Shader": {"renderpass": [...]}},
  // {"shader": ...}, a top-level renderpass, or a bare array of exports
  Value shader;
  if (root.isArr()) {
    if (root.size() == 0) { error = "JSON input is empty"; return -1; }
    shader = root.atIndex(0);
  } else if (root.isObj()) {
    shader = root;
  } else {
    return 0;
  }
  if (!shader.isObj()) return 0;
  // unwrap a "Shader"/"shader" wrapper when present. Copy into a local FIRST:
  // assigning a member of the same Value back onto itself (shader =
  // shader.get("Shader")) is self-referential vector assignment and corrupts
  // the object storage (caught by a multi-field export; a single-field one
  // happened to survive)
  {
    const Value wrapper = shader.get("Shader");
    if (wrapper.isObj()) shader = wrapper;
    else {
      const Value lower = shader.get("shader");
      if (lower.isObj()) shader = lower;
    }
  }
  const Value& passes = shader.get("renderpass");
  if (passes.isNull() || !passes.isArr()) {
    error = "JSON input is not a Shadertoy export (no renderpass array)";
    return -1;
  }
  // Phase 1: collect the fragment passes (keeping their per-pass `inputs`
  // arrays - that is where multi-channel Shadertoy exports wire their
  // textures, buffers, audio and keyboard channels).
  struct PassRec {
    std::string marker;   // engine pass marker: "common" | "buffer_a"..d | "image"
    std::string name;
    std::string id;
    std::string code;
    // <channel, wiring> pairs; "buffer:<name>" targets are resolved in
    // phase 2 against the collected buffer passes
    std::vector<std::pair<int, std::string>> binds;
  };
  std::vector<PassRec> recs;
  int skipped = 0;
  for (size_t i = 0; i < passes.size(); ++i) {
    const Value& p = passes.atIndex(i);
    const std::string code = p.get("code").asStr();
    if (code.empty()) continue;
    const std::string name = p.get("name").asStr();
    std::string marker;
    if (!jsonPassMarker(name, marker)) {
      ++skipped;
      notes.push_back("skipped renderpass '" + name + "' (not a fragment pass)");
      continue;
    }
    PassRec rec;
    rec.marker = marker; rec.name = name;
    rec.id = p.get("id").asStr();
    rec.code = code;
    const Value& inputs = p.get("inputs");
    if (inputs.isArr()) {
      for (size_t j = 0; j < inputs.size(); ++j) {
        const Value& in = inputs.atIndex(j);
        const int channel = in.get("channel").asInt(0);
        if (channel < 0 || channel > 3) continue;
        const std::string ctype = in.get("ctype").asStr();
        const std::string src = in.get("src").asStr();
        std::string lower = ctype;
        for (char& c2 : lower) c2 = (char)std::tolower((unsigned char)c2);
        std::string bind;
        if (lower == "texture" || lower == "cube" || lower == "video") {
          if (!src.empty()) bind = "texture:" + src;
        } else if (lower == "audio") bind = "audio";
        else if (lower == "keyboard") bind = "keyboard";
        else if (lower == "none") bind = "none";
        else if (lower == "buffer") bind = "buffer:" + src;
        else
          notes.push_back("renderpass '" + name + "' input ctype '" + ctype + "' not mapped");
        if (!bind.empty()) rec.binds.emplace_back(channel, std::move(bind));
      }
    }
    recs.push_back(std::move(rec));
  }
  if (recs.empty()) {
    error = "JSON export has no convertable fragment passes (only Sound/Cube/unknown passes?)";
    return -1;
  }
  // Phase 2: resolve buffer inputs to the concrete buffer pass markers.
  // Shadertoy references a buffer by its pass id/name ("Buffer A", "sB",
  // "bufferB", ...) - match case/whitespace-insensitively against every
  // buffer pass's name, id and derived letter keys.
  auto norm = [](const std::string& s) {
    std::string out;
    for (char c : s) if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
    return out;
  };
  for (auto& rec : recs) {
    for (auto& b : rec.binds) {
      if (b.second.rfind("buffer:", 0) != 0) continue;
      const std::string want = norm(b.second.substr(7));
      std::string target;
      for (const auto& cand : recs) {
        if (cand.marker.rfind("buffer_", 0) != 0) continue;
        const std::string letter = cand.marker.substr(7);
        if (norm(cand.name) == want || norm(cand.id) == want ||
            norm("buffer" + letter) == want || norm("s" + letter) == want) {
          target = cand.marker;
          break;
        }
      }
      if (target.empty()) {
        notes.push_back("renderpass '" + rec.name + "' buffer input '" + b.second.substr(7) +
                        "' does not match a buffer pass - falling back to the engine chain convention");
        b.second.clear();
      } else {
        b.second = "buffer:" + target;
      }
    }
  }
  // Phase 3: emit marker text with the wired channels as `// channel:`
  // comments so the GLSL pipeline handles them identically.
  std::ostringstream out;
  for (const auto& rec : recs) {
    out << "// pass: " << rec.marker << "\n";
    for (const auto& b : rec.binds)
      if (!b.second.empty()) out << "// channel: iChannel" << b.first << " = " << b.second << "\n";
    out << rec.code << "\n";
  }
  markerText = out.str();
  notes.push_back("parsed as a Shadertoy JSON API export (" + std::to_string(recs.size()) +
                  " fragment pass(es), " + std::to_string(skipped) + " skipped)");
  notes.push_back("API export inputs are mapped to channel wiring when present; unwired channels "
                  "follow the engine chain convention or are inferred as bindable texture samplers");
  return 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// convertShadertoyToFragment
// ---------------------------------------------------------------------------
ShadertoyConvertResult convertShadertoyToFragment(const std::string& source,
                                                  const ShadertoyConvertOptions& opts) {
  ShadertoyConvertResult result;
  try {
    // 0. a Shadertoy JSON API export becomes the same `// pass:` marker text
    //    the GLSL path uses, so folding and channel rewriting are shared
    std::string src = source;
    {
      std::string markerText;
      std::vector<std::string> jsonNotes;
      std::string jsonError;
      const int jr = parseShadertoyJsonExport(source, markerText, jsonNotes, jsonError);
      if (jr < 0) {
        result.error = jsonError;
        return result;
      }
      if (jr == 1) {
        src = markerText;
        for (const auto& n : jsonNotes) result.notes.push_back(n);
      }
    }

    // 1. split into passes (strict marker parser - same as the runtime)
    const std::vector<ShadertoyPass> raw = splitShadertoyPasses(src);
    if (raw.empty()) {
      result.error = "source has no passes";
      return result;
    }

    // 2. classify: common / buffers / image
    std::vector<ShadertoyPass> commons, buffers, images;
    for (const auto& p : raw) {
      if (p.name == "common") commons.push_back(p);
      else if (p.name == "image") images.push_back(p);
      else if (p.name.rfind("buffer", 0) == 0) buffers.push_back(p);
      else images.push_back(p);  // unknown name -> treat as the image pass
    }
    if (images.empty()) {
      // buffers-only file: synthesize an image pass that samples the last buffer
      ShadertoyPass synth;
      synth.name = "image";
      synth.src =
          "// synthetic image pass (no image tab in source)\n"
          "void mainImage(out vec4 o, in vec2 p) { o = texture(iChannel0, p / iResolution.xy); }\n";
      images.push_back(synth);
    }
    ShadertoyPass image = images.front();
    if (images.size() > 1) result.notes.push_back("multiple image passes - only the first is used");
    // an image pass without mainImage (helpers only) gets the same synthetic
    // body as the buffers-only case, so channel rewriting sees it
    if (image.src.find("mainImage") == std::string::npos) {
      image.src +=
          "\nvoid mainImage(out vec4 o, in vec2 p) { o = texture(iChannel0, p / iResolution.xy); }\n";
    }

    // 3. per-pass channel wiring: comments override defaults
    const auto imageComments = extractChannelComments(image.src);
    for (auto& b : commons) extractChannelComments(b.src);
    std::vector<std::array<ShadertoyChannelBind, 4>> bufferComments;
    for (auto& b : buffers) bufferComments.push_back(extractChannelComments(b.src));

    auto imageChannels = imageComments;
    for (size_t i = 0; i < 4; ++i) {
      if (imageChannels[i].kind == ShadertoyChannelKind::Auto) {
        // default engine convention: iChannel0 = last buffer (or a bindable
        // texture when the file has no buffers), iChannel1 = live scene
        if (i == 0) {
          if (!buffers.empty()) {
            imageChannels[i].kind = ShadertoyChannelKind::Buffer;
            imageChannels[i].target = buffers.back().name;
          } else {
            imageChannels[i].kind = ShadertoyChannelKind::Texture;
            imageChannels[i].target = "unbound (reads black)";
          }
        } else if (i == 1) {
          imageChannels[i].kind = ShadertoyChannelKind::Texture;
          imageChannels[i].target = "live scene (bind a texture or it reads black)";
        } else {
          imageChannels[i].kind = ShadertoyChannelKind::None;
        }
      }
    }
    // explicit programmatic wiring wins over defaults (but not over comments)
    for (size_t i = 0; i < 4; ++i) {
      if (imageComments[i].kind == ShadertoyChannelKind::Auto && opts.channels[i].kind != ShadertoyChannelKind::Auto)
        imageChannels[i] = opts.channels[i];
    }
    // sampled-but-unwired channels (Shadertoy code without channel comments)
    // stay bindable texture samplers instead of reading black
    inferSampledTextureChannels(image.src, imageChannels, result.notes);

    // 4. fold buffers: each becomes `vec4 st_<key>(vec2 fragCoord)`; the image
    //    pass's texture(iChannelN) calls are rewritten to call them.
    std::map<std::string, std::string> bufferFuncs;
    std::vector<std::pair<std::string, std::string>> foldedSections;  // <funcName, body>
    for (size_t bi = 0; bi < buffers.size(); ++bi) {
      std::string body = stripShadertoyBoilerplate(buffers[bi].src);
      const std::string key = sanitizePassKey(buffers[bi].name);
      const std::string funcName = "st_" + key;
      // buffer iChannel0 = previous buffer by default (engine chain convention)
      std::array<ShadertoyChannelBind, 4> chans;
      const auto& comments = bufferComments[(size_t)bi];
      for (size_t i = 0; i < 4; ++i) {
        if (comments[i].kind != ShadertoyChannelKind::Auto) chans[i] = comments[i];
        else if (i == 0) {
          if (bi == 0) {
            chans[i].kind = ShadertoyChannelKind::Texture;
            chans[i].target = "external texture for first buffer (bind or black)";
          } else {
            chans[i].kind = ShadertoyChannelKind::Buffer;
            chans[i].target = buffers[bi - 1].name;
          }
        } else chans[i].kind = ShadertoyChannelKind::None;
      }
      inferSampledTextureChannels(body, chans, result.notes);
      body = rewriteChannelCalls(body, chans, bufferFuncs, result.notes, result.requiredTextures);
      const bool hasMain = body.find("mainImage") != std::string::npos;
      body = prefixPassFunctions(body, funcName + "_");
      std::string section = body;
      if (hasMain) {
        // the wrapper that lets the image pass call this buffer by uv
        std::ostringstream f;
        f << "\nvec4 " << funcName << "(vec2 fragCoord) {\n"
          << "  vec4 fragColor;\n"
          << "  " << funcName << "_mainImage(fragColor, fragCoord);\n"
          << "  return fragColor;\n"
          << "}\n";
        section += f.str();
      } else {
        result.notes.push_back("buffer pass '" + buffers[bi].name +
                               "' has no mainImage - emitted as shared helpers only");
      }
      foldedSections.push_back({funcName, section});
      if (hasMain) {
        bufferFuncs[buffers[bi].name] = funcName;
        result.foldedBuffers.push_back(funcName);
      }
    }

    // 5. the image pass: keep mainImage, add a main() that calls it
    std::string imageSrc = stripShadertoyBoilerplate(image.src);
    imageSrc = rewriteChannelCalls(imageSrc, imageChannels, bufferFuncs, result.notes,
                                   result.requiredTextures);
    // image pass helpers keep their names (only buffer passes are prefixed to
    // avoid collisions with common and with each other). The engine runtime
    // prepends common to each pass separately, so if a pass redefines a common
    // helper it would fail there too; the fold preserves the same semantics.

    // 6. which samplers did we keep? derive from requiredTextures names
    std::set<int> samplerChannels;
    for (const auto& rt : result.requiredTextures) {
      if (rt.rfind("uChannel", 0) == 0 && rt.size() >= 9 && rt[8] >= '0' && rt[8] <= '3')
        samplerChannels.insert(rt[8] - '0');
    }
    // de-duplicate the texture list (same channel sampled several times)
    std::vector<std::string> uniqueTextures;
    for (const auto& t : result.requiredTextures)
      if (std::find(uniqueTextures.begin(), uniqueTextures.end(), t) == uniqueTextures.end())
        uniqueTextures.push_back(t);
    result.requiredTextures = std::move(uniqueTextures);
    result.uniforms = {"uTimeDelta", "uFrame", "uMouse", "uDate", "uChannelTime",
                       "uChannelResolution"};

    // 7. assemble the single file
    std::ostringstream out;
    out << "#version 300 es\n"
        << "precision highp float;\n"
        << "precision highp int;\n\n"
        << "// ---------------------------------------------------------------------------\n"
        << "// CONVERTED FROM SHADERTOY - " << (opts.sourceLabel.empty() ? "single fragment shader" : opts.sourceLabel) << "\n"
        << "// All channels folded into this one file by ns_shadertoy_convert.\n";
    if (!result.foldedBuffers.empty()) {
      out << "// Folded buffers:";
      for (const auto& f : result.foldedBuffers) out << " " << f;
      out << "\n";
    }
    if (!result.requiredTextures.empty()) {
      out << "// Sampler channels to bind at runtime:";
      for (const auto& t : result.requiredTextures) out << " " << t;
      out << "\n";
    }
    out << "// ---------------------------------------------------------------------------\n\n"
        << shimBlock(samplerChannels);
    for (const auto& c : commons) out << stripShadertoyBoilerplate(c.src) << "\n";
    for (const auto& f : foldedSections) out << "\n// ---- folded buffer " << f.first << " ----\n" << f.second << "\n";
    out << "\n// ---- image pass ----\n" << imageSrc << "\n"
        << "out vec4 fragColor;\n"
        << "void main() {\n"
        << "  vec2 fragCoord = gl_FragCoord.xy;\n"
        << "  mainImage(fragColor, fragCoord);\n"
        << "}\n";

    result.fragment = out.str();
    result.ok = true;
    return result;
  } catch (const std::exception& e) {
    result.ok = false;
    result.error = e.what();
    return result;
  }
}

}  // namespace ns
