#include "framework/script/scriptparser.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace ns {

namespace {

// ---------------------------------------------------------------------------
// tokenizer
// ---------------------------------------------------------------------------
enum class Tok { End, Ident, Number, Time, Str, LBrace, RBrace, LParen, RParen, Comma, Semicolon, Equal, Newline };

struct Token {
  Tok kind = Tok::End;
  std::string text;   // ident / string / time text
  double num = 0;
  int line = 1;
  int col = 1;
};

inline bool isSep(Tok k) {
  return k == Tok::Newline || k == Tok::Semicolon || k == Tok::Comma || k == Tok::Equal;
}

class Lexer {
public:
  Lexer(const std::string& src, const std::string& label) : s_(src), label_(label) {}

  [[noreturn]] void fail(const std::string& msg) const {
    std::ostringstream o;
    o << label_ << ":" << line_ << ":" << col_ << ": " << msg;
    throw ScriptError(o.str());
  }

  const std::string& label() const { return label_; }

  Token next() {
    skipSpace();
    Token t;
    t.line = line_;
    t.col = col_;
    const char c = peek();
    if (c == '\0') { t.kind = Tok::End; return t; }
    if (c == '\n') { t.kind = Tok::Newline; advance(); return t; }
    if (c == ';') { t.kind = Tok::Semicolon; advance(); return t; }
    if (c == ',') { t.kind = Tok::Comma; advance(); return t; }
    if (c == '=') { t.kind = Tok::Equal; advance(); return t; }
    if (c == '{') { t.kind = Tok::LBrace; advance(); return t; }
    if (c == '}') { t.kind = Tok::RBrace; advance(); return t; }
    if (c == '(') { t.kind = Tok::LParen; advance(); return t; }
    if (c == ')') { t.kind = Tok::RParen; advance(); return t; }
    if (c == '"') { t.kind = Tok::Str; t.text = readString(); return t; }
    if (c == '/' && peekAt(1) == '/') { skipLine(); return next(); }
    if (c == '#') { skipLine(); return next(); }
    if (c == '/' && peekAt(1) == '*') { skipBlock(); return next(); }
    if (isIdentStart(c)) { t.kind = Tok::Ident; t.text = readIdent(); return t; }
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
      return readNumberOrTime(t);
    }
    fail(std::string("unexpected character '") + c + "'");
  }

  /** look ahead one token without consuming (only used for unit disambiguation) */
  bool peekIdentIs(const char* kw) {
    const char c = peek();
    if (!isIdentStart(c)) return false;
    const size_t save = i_;
    const int saveLine = line_;
    const int saveCol = col_;
    const std::string w = readIdent();
    const bool match = w == kw;
    i_ = save;
    line_ = saveLine;
    col_ = saveCol;
    return match;
  }

private:
  const std::string& s_;
  std::string label_;
  size_t i_ = 0;
  int line_ = 1;
  int col_ = 1;

  char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
  char peekAt(size_t k) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
  char advance() {
    const char c = s_[i_++];
    if (c == '\n') { line_++; col_ = 1; } else { col_++; }
    return c;
  }
  static bool isIdentStart(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
  }
  static bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '/' ||
           c == '-' || c == '+' || c == ':';
  }
  void skipSpace() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r') advance();
      else break;
    }
  }
  void skipLine() {
    while (i_ < s_.size() && s_[i_] != '\n') advance();
  }
  void skipBlock() {
    advance(); advance();  // consume /*
    while (i_ < s_.size()) {
      if (peek() == '*' && peekAt(1) == '/') { advance(); advance(); return; }
      advance();
    }
  }
  std::string readString() {
    advance();  // opening quote
    std::string out;
    while (true) {
      const char c = peek();
      if (c == '\0' || c == '\n') fail("unterminated string");
      if (c == '"') { advance(); return out; }
      if (c == '\\') {
        advance();
        const char e = advance();
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          default: out += e;
        }
      } else {
        out += advance();
      }
    }
  }
  std::string readIdent() {
    const size_t start = i_;
    while (i_ < s_.size() && isIdentChar(s_[i_])) advance();
    return s_.substr(start, i_ - start);
  }
  Token readNumberOrTime(Token t) {
    const size_t start = i_;
    while (i_ < s_.size() && isIdentChar(s_[i_])) advance();
    const std::string run = s_.substr(start, i_ - start);
    // mm:ss time?
    if (run.find(':') != std::string::npos) {
      t.kind = Tok::Time;
      t.text = run;
      return t;
    }
    // pure number?
    char* end = nullptr;
    const double v = std::strtod(run.c_str(), &end);
    if (end && *end == '\0' && !run.empty()) {
      t.kind = Tok::Number;
      t.num = v;
      return t;
    }
    t.kind = Tok::Ident;
    t.text = run;
    return t;
  }
};

float parseClockTime(const std::string& t);  // defined below the parser

// ---------------------------------------------------------------------------
// diagnostics: edit distance + "did you mean" suggestions
// ---------------------------------------------------------------------------
/** Levenshtein edit distance between two words (used to suggest a likely
 *  spelling for an unknown property / command / option / interpolator). */
int editDistance(const std::string& a, const std::string& b) {
  const size_t n = a.size(), m = b.size();
  std::vector<int> row(m + 1), prev(m + 1);
  for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
  for (size_t i = 1; i <= n; i++) {
    row[0] = (int)i;
    for (size_t j = 1; j <= m; j++) {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      row[j] = std::min({prev[j] + 1, row[j - 1] + 1, prev[j - 1] + cost});
    }
    prev.swap(row);
  }
  return prev[m];
}

/** up to two candidates within maxDist of word, closest first ("" when none) */
std::vector<std::string> didYouMean(const std::string& word,
                                    const std::vector<std::string>& cands,
                                    int maxDist = 2) {
  std::vector<std::string> out;
  int best = maxDist + 1;
  for (const auto& c : cands) {
    const int d = editDistance(word, c);
    if (d < best) {
      best = d;
      out.clear();
      out.push_back(c);
    } else if (d == best && d <= maxDist && out.size() < 2) {
      out.push_back(c);
    }
  }
  return out;
}

std::string suggest(const std::string& word, const std::vector<std::string>& cands) {
  const auto v = didYouMean(word, cands);
  if (v.empty()) return "";
  if (v.size() == 1) return " - did you mean '" + v[0] + "'?";
  return " - did you mean '" + v[0] + "' or '" + v[1] + "'?";
}

inline bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// known-name tables (also drive the editor's syntax highlighting, so new
// commands/options should be added here AND in docs/docs.js)
const std::vector<std::string> kDemoOptions = {"bpm", "tempo", "duration"};
const std::vector<std::string> kTopKeywords = {"scene", "at", "bpm", "tempo", "duration", "demo"};
const std::vector<std::string> kSceneOptions = {"bars", "duration", "intensity", "chapter", "title", "visible"};
const std::vector<std::string> kPostOptions = {"bloom", "glitch", "exposure", "heat", "vignette",
                                               "grain", "scanlines", "crt", "fxaa", "dof",
                                               "chromatic", "pixelate", "fog", "motionblur"};
const std::vector<std::string> kCommands = {
    "show", "hide", "load", "shader", "camera", "play", "fade", "transition", "post",
    "anim", "marker", "speed", "loop", "jump", "mesh", "sprite", "image", "text", "light",
    "particles", "empty", "postnode", "quadnode"};
const std::vector<std::string> kInterps = {"linear", "smooth", "smoothstep", "cubic", "bezier",
                                           "ease-in", "ease-out", "ease-in-out", "bounce", "elastic"};
const std::vector<std::string> kRigs = {"static", "drift", "fly", "nave", "orbit", "spiral",
                                        "hover", "city", "descend", "path"};

inline bool isCommand(const std::string& s) { return contains(kCommands, s); }
inline bool isInterp(const std::string& s) { return contains(kInterps, s); }
inline bool isRig(const std::string& s) { return contains(kRigs, s); }
inline bool isPostOption(const std::string& s) { return contains(kPostOptions, s); }

// ---------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------
class Parser {
public:
  Parser(Lexer& lx, float bpm) : lx_(lx), bpm_(bpm), label_(lx.label()) { cur_ = lx_.next(); }

  [[noreturn]] void fail(const std::string& msg) {
    std::ostringstream o;
    o << label_ << ":" << cur_.line << ":" << cur_.col << ": " << msg;
    throw ScriptError(o.str());
  }

  Script parseScript() {
    Script s;
    // peek at the first declaration to pick up bpm early so times resolve
    // correctly; a `demo` header may carry bpm.
    if (cur_.kind == Tok::Ident && cur_.text == "demo") {
      parseDemo(s);
    }
    bpm_ = s.bpm;
    while (cur_.kind != Tok::End) {
      if (isSep(cur_.kind)) {
        advance();
        continue;
      }
      if (cur_.kind != Tok::Ident) fail("expected a declaration, 'scene' or 'at'");
      const std::string kw = cur_.text;
      if (kw == "scene") {
        s.scenes.push_back(parseScene());
      } else if (kw == "at") {
        s.main.push_back(parseAtBlock(/*sceneRelative=*/false));
      } else if (kw == "bpm" || kw == "tempo") {
        advance();
        s.bpm = expectNumber("bpm");
        bpm_ = s.bpm;
      } else if (kw == "duration") {
        advance();
        s.duration = parseTimeValue("duration");
      } else if (kw == "demo") {
        parseDemo(s);
        bpm_ = s.bpm;
      } else {
        fail("unknown top-level keyword '" + kw + "'" + suggest(kw, kTopKeywords) +
             " (expected scene / at / bpm / duration / demo)");
      }
    }
    return s;
  }

private:
  Lexer& lx_;
  Token cur_;
  float bpm_;
  std::string label_;

  void advance() { cur_ = lx_.next(); }
  void skipEq() { if (cur_.kind == Tok::Equal) advance(); }

  double expectNumber(const char* what) {
    if (cur_.kind != Tok::Number) fail(std::string("expected a number for ") + what);
    const double v = cur_.num;
    advance();
    return v;
  }
  std::string expectIdent(const char* what) {
    if (cur_.kind != Tok::Ident) fail(std::string("expected a name for ") + what);
    const std::string v = cur_.text;
    advance();
    return v;
  }
  void expectPunct(Tok k, const char* what) {
    if (cur_.kind != k) fail(std::string("expected '") + what + "'");
    advance();
  }

  /** parse a time value: NUMBER [s|beat|bar] | mm:ss | beat N | bar N |
   *  | beat(N) | bar(N) - all resolved against the declared bpm */
  float parseTimeValue(const char* what = "time") {
    if (cur_.kind == Tok::Time) {
      const float t = parseClockTime(cur_.text);
      advance();
      return t;
    }
    if (cur_.kind == Tok::Number) {
      const float v = (float)cur_.num;
      advance();
      // optional unit suffix as a separate ident token (singular + plural)
      if (cur_.kind == Tok::Ident) {
        const std::string& u = cur_.text;
        if (u == "s") { advance(); return v; }
        if (u == "beat" || u == "beats") { advance(); return v * beatSec(bpm_); }
        if (u == "bar" || u == "bars") { advance(); return v * barSec(bpm_); }
      }
      return v;
    }
    if (cur_.kind == Tok::Ident) {
      // `beat 65`, `bar 32`, `beat(65)`, `bar(32)` ...
      if (cur_.text == "beat" || cur_.text == "bar") {
        const bool isBar = cur_.text == "bar";
        advance();
        // beat(N) / bar(N) parenthesized form
        const bool paren = cur_.kind == Tok::LParen;
        if (paren) advance();
        const double n = expectNumber(isBar ? "bar index" : "beat index");
        if (paren) expectPunct(Tok::RParen, ")");
        return (float)(n * (isBar ? barSec(bpm_) : beatSec(bpm_)));
      }
      // glued unit: `32bars`, `16beats`, `66.667s` (single lexer token)
      const std::string& t = cur_.text;
      for (const auto& suf : {"bars", "beats", "bar", "beat", "s"}) {
        const size_t sn = std::string(suf).size();
        if (t.size() > sn && t.compare(t.size() - sn, sn, suf) == 0) {
          const std::string num = t.substr(0, t.size() - sn);
          char* end = nullptr;
          const double v = std::strtod(num.c_str(), &end);
          if (end && *end == '\0' && !num.empty()) {
            advance();
            float mult = 1.0f;
            if (suf == std::string("bar") || suf == std::string("bars")) mult = barSec(bpm_);
            else if (suf == std::string("beat") || suf == std::string("beats")) mult = beatSec(bpm_);
            return (float)(v * mult);
          }
        }
      }
    }
    fail(std::string("expected a ") + what + " (seconds, mm:ss, 'beat N' or 'bar N')");
    return 0;
  }

  /** parse one value: NUMBER | STRING | IDENT | ( values ) */
  Value parseValue() {
    switch (cur_.kind) {
      case Tok::Number: {
        const double v = cur_.num;
        advance();
        return Value(v);
      }
      case Tok::Str: {
        const std::string v = cur_.text;
        advance();
        return Value(v);
      }
      case Tok::Ident: {
        const std::string v = cur_.text;
        advance();
        return Value(v);
      }
      case Tok::Time: {
        const std::string v = cur_.text;
        advance();
        return Value(v);  // caller can parseTime() it if needed
      }
      case Tok::LParen: {
        advance();
        Value::Array arr;
        while (true) {
          if (cur_.kind == Tok::RParen) { advance(); break; }
          if (cur_.kind == Tok::Comma || cur_.kind == Tok::Newline || cur_.kind == Tok::Semicolon) { advance(); continue; }
          if (cur_.kind == Tok::End) fail("unterminated vector");
          arr.push_back(parseValue());
        }
        return Value(std::move(arr));
      }
      default:
        fail("expected a value (number, string, name or (vector))");
    }
  }

  /** one key/value pair row inside an option block (also keyframe rows) */
  void parseOptionEntry(Cmd& cmd) {
    if (cur_.kind == Tok::Ident) {
      const std::string key = cur_.text;
      advance();
      skipEq();
      const Value v = parseValue();
      if (cmd.name == "text" && key == "text" && v.isStr()) {
        // Text is the one scene option where an unquoted value containing
        // spaces is unambiguous and especially common in the editor. Accept
        // `text title { text HELLO WORLD pos (...) }` as well as the canonical
        // quoted form. Stop at a known following text option so compact
        // one-line setup commands remain parseable.
        std::string text = v.asStr();
        const auto isTextOption = [](const std::string& option) {
          return option == "pos" || option == "euler" || option == "scale" ||
                 option == "visible" || option == "layer" || option == "tag" ||
                 option == "text" || option == "size" || option == "style" ||
                 option == "color" || option == "opacity" || option == "align";
        };
        while ((cur_.kind == Tok::Ident || cur_.kind == Tok::Str) &&
               !(cur_.kind == Tok::Ident && isTextOption(cur_.text))) {
          if (!text.empty()) text += ' ';
          text += cur_.text;
          advance();
        }
        cmd.opts.set(key) = Value(std::move(text));
      } else {
        cmd.opts.set(key) = v;
      }
    } else if (cur_.kind == Tok::Number || cur_.kind == Tok::Time) {
      // keyframe row: TIME VALUE [interp]
      KeyframeRow row;
      row.t = parseTimeValue();
      row.v = parseValue();
      if (cur_.kind == Tok::Ident) {
        row.interp = cur_.text;
        if (cmd.name == "anim" && !isInterp(row.interp)) {
          fail("unknown interpolator '" + row.interp + "'" + suggest(row.interp, kInterps));
        }
        advance();
      }
      cmd.keys.push_back(std::move(row));
    } else {
      fail("expected an option key or a keyframe time inside '{ }'");
    }
  }

  /** parse a command: IDENT value* [ { options } ] */
  Cmd parseCommand() {
    Cmd cmd;
    cmd.name = expectIdent("command");
    while (true) {
      if (cur_.kind == Tok::LBrace) {
        advance();
        while (cur_.kind != Tok::RBrace) {
          if (cur_.kind == Tok::End) fail("unterminated '{' block");
          if (isSep(cur_.kind)) { advance(); continue; }
          parseOptionEntry(cmd);
        }
        advance();  // }
        break;
      }
      if (cur_.kind == Tok::Ident || cur_.kind == Tok::Number || cur_.kind == Tok::Str ||
          cur_.kind == Tok::LParen || cur_.kind == Tok::Time) {
        cmd.args.push_back(parseValue());
      } else {
        break;
      }
    }
    // --- diagnostics: catch typos at parse time (filename:line:col included
    // in every fail() above) ------------------------------------------------
    if (!isCommand(cmd.name)) {
      fail("unknown command '" + cmd.name + "'" + suggest(cmd.name, kCommands));
    }
    if (cmd.name == "anim" && cmd.args.size() >= 3 && cmd.args[2].isStr() &&
        !isInterp(cmd.args[2].asStr())) {
      fail("unknown interpolator '" + cmd.args[2].asStr() + "'" +
           suggest(cmd.args[2].asStr(), kInterps));
    }
    if (cmd.name == "camera" && cmd.opts.get("rig").isStr()) {
      const std::string rig = cmd.opts.get("rig").asStr();
      if (!isRig(rig)) {
        fail("unknown camera rig '" + rig + "'" + suggest(rig, kRigs));
      }
    }
    return cmd;
  }

  /** an `at` block body: a list of commands (until '}' or end of input) */
  std::vector<Cmd> parseCmdList() {
    std::vector<Cmd> out;
    while (true) {
      if (cur_.kind == Tok::End) break;
      if (cur_.kind == Tok::RBrace) break;
      if (isSep(cur_.kind)) {
        advance();
        continue;
      }
      if (cur_.kind != Tok::Ident) fail("expected a command");
      out.push_back(parseCommand());
    }
    return out;
  }

  ScriptBlock parseAtBlock(bool sceneRelative) {
    (void)sceneRelative;
    expectIdent("at");  // consume 'at'
    ScriptBlock b;
    b.time = parseTimeValue();
    // `at 0.0\n{ ... }` - the brace may sit on the next line
    while (isSep(cur_.kind)) advance();
    if (cur_.kind == Tok::LBrace) {
      advance();
      b.cmds = parseCmdList();
      expectPunct(Tok::RBrace, "}");
    } else {
      // single-command form: `at 0 show intro`
      b.cmds.push_back(parseCommand());
    }
    return b;
  }

  void parseDemo(Script& s) {
    advance();  // 'demo'
    if (cur_.kind == Tok::Str) {
      s.title = cur_.text;
      advance();
    }
    expectPunct(Tok::LBrace, "{");
    // pre-scan the header for bpm/tempo so bar-unit durations resolve at the
    // declared tempo regardless of field order (`duration 4bars` written above
    // `tempo 140` works). The scan runs on a throwaway copy of the lexer - the
    // main parse stream (and its line/column tracking) is left untouched; the
    // main loop below re-sets bpm_ at its own pace when it reaches the field.
    // (Edge semantics for a malformed header: if two different tempos are
    // declared with a bar-unit duration between them, the duration resolves at
    // the FIRST while s.bpm ends as the LAST - valid files declare one tempo.)
    {
      Lexer probe = lx_;
      Token t = probe.next();
      while (t.kind != Tok::RBrace && t.kind != Tok::End) {
        if (t.kind == Tok::Ident &&
            (t.text == "bpm" || t.text == "tempo")) {
          Token v = probe.next();
          if (v.kind == Tok::Equal) v = probe.next();
          if (v.kind == Tok::Number) bpm_ = s.bpm = (float)v.num;
          break;
        }
        t = probe.next();
      }
    }
    while (cur_.kind != Tok::RBrace) {
      if (cur_.kind == Tok::End) fail("unterminated 'demo' block");
      if (isSep(cur_.kind)) { advance(); continue; }
      if (cur_.kind != Tok::Ident) fail("expected 'bpm' or 'duration' in demo header");
      const std::string k = cur_.text;
      advance();
      skipEq();
      if (k == "bpm" || k == "tempo") {
        s.bpm = (float)expectNumber("bpm");
        bpm_ = s.bpm;  // a later `duration 4bars` in the same header resolves at it
      }
      else if (k == "duration") { s.duration = parseTimeValue("duration"); }
      else fail("unknown demo option '" + k + "'" + suggest(k, kDemoOptions));
    }
    advance();  // }
  }

  SceneDef parseScene() {
    advance();  // 'scene'
    SceneDef d;
    d.name = expectIdent("scene name");
    expectPunct(Tok::LBrace, "{");
    while (cur_.kind != Tok::RBrace) {
      if (cur_.kind == Tok::End) fail("unterminated 'scene' block");
      if (isSep(cur_.kind)) { advance(); continue; }
      if (cur_.kind == Tok::Ident && cur_.text == "at") {
        d.blocks.push_back(parseAtBlock(/*sceneRelative=*/true));
        continue;
      }
      // scene-level option or a setup command; options are distinguished by
      // name. `camera NAME`, `show X` etc. are setup commands.
      if (cur_.kind == Tok::Ident && cur_.text == "bars") {
        advance();
        skipEq();
        d.bars = (int)expectNumber("bars");
      } else if (cur_.kind == Tok::Ident && cur_.text == "duration") {
        advance();
        skipEq();
        d.duration = parseTimeValue("duration");  // seconds, or bar/beat units
      } else if (cur_.kind == Tok::Ident && cur_.text == "intensity") {
        advance();
        skipEq();
        d.intensity = (float)expectNumber("intensity");
      } else if (cur_.kind == Tok::Ident && cur_.text == "chapter") {
        advance();
        skipEq();
        d.chapter = (int)expectNumber("chapter");
      } else if (cur_.kind == Tok::Ident && cur_.text == "visible") {
        advance();
        skipEq();
        if (cur_.kind == Tok::Ident && (cur_.text == "true" || cur_.text == "false")) {
          d.visible = cur_.text == "true";
          advance();
        } else {
          d.visible = expectNumber("visible (0/1 or true/false)") != 0;
        }
      } else if (cur_.kind == Tok::Ident && cur_.text == "title") {
        advance();
        skipEq();
        if (cur_.kind == Tok::Str || cur_.kind == Tok::Ident) {
          d.title = cur_.text;
          advance();
        } else {
          fail("expected a title string");
        }
      } else if (cur_.kind == Tok::Ident && isPostOption(cur_.text)) {
        // scene-level post shorthand: `bloom 0.8` (or `bloom = 0.8`) becomes
        // a `post { bloom 0.8 }` command (dispatch sets the music-reactive fx)
        Cmd c;
        c.name = "post";
        const std::string key = cur_.text;
        advance();
        skipEq();
        c.opts.set(key) = parseValue();
        d.setup.push_back(std::move(c));
      } else if (cur_.kind == Tok::Ident && cur_.text == "effect") {
        // `effect Tunnel` is an alias for `show Tunnel` (nested scene or effect)
        Cmd c;
        c.name = "show";
        advance();
        c.args.push_back(parseValue());
        d.setup.push_back(std::move(c));
      } else if (cur_.kind == Tok::Ident && !isCommand(cur_.text)) {
        // unknown scene-level name: report it with suggestions BEFORE
        // parseCommand (which would say 'unknown command' without the
        // scene-option context)
        std::vector<std::string> cands = kSceneOptions;
        cands.insert(cands.end(), kPostOptions.begin(), kPostOptions.end());
        cands.insert(cands.end(), kCommands.begin(), kCommands.end());
        fail("unknown scene property '" + cur_.text + "'" + suggest(cur_.text, cands) +
             " (expected a scene option bars/duration/intensity/chapter/title/visible, "
             "a post option like 'bloom 0.8', or a command)");
      } else {
        d.setup.push_back(parseCommand());
      }
    }
    advance();  // }
    return d;
  }
};

float parseClockTime(const std::string& t) {
  // mm:ss(.ms) — also tolerate h:mm:ss
  const size_t c1 = t.find(':');
  if (c1 == std::string::npos) return (float)std::strtod(t.c_str(), nullptr);
  const size_t c2 = t.find(':', c1 + 1);
  const double min = std::strtod(t.substr(0, c1).c_str(), nullptr);
  const double sec = std::strtod(t.substr(c1 + 1).c_str(), nullptr);
  const double hrs = c2 != std::string::npos ? std::strtod(t.substr(0, c1).c_str(), nullptr) : 0;
  if (c2 != std::string::npos) {
    const double m2 = std::strtod(t.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr);
    return (float)(hrs * 3600.0 + m2 * 60.0 + std::strtod(t.substr(c2 + 1).c_str(), nullptr));
  }
  return (float)(min * 60.0 + sec);
}

}  // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
Script ScriptParser::parse(const std::string& text, const std::string& label) {
  Lexer lx(text, label);
  Parser p(lx, 216.0f);
  return p.parseScript();
}

float parseTime(const std::string& tok, float bpm) {
  if (tok.find(':') != std::string::npos) return parseClockTime(tok);
  // number with optional unit
  std::string t = tok;
  float mult = 1.0f;
  const std::string units[] = {"bar", "beat", "s"};
  const float muls[] = {barSec(bpm), beatSec(bpm), 1.0f};
  for (int k = 0; k < 3; k++) {
    if (t.size() > units[k].size() && t.compare(t.size() - units[k].size(), units[k].size(), units[k]) == 0) {
      t = t.substr(0, t.size() - units[k].size());
      mult = muls[k];
      break;
    }
  }
  return (float)std::strtod(t.c_str(), nullptr) * mult;
}

}  // namespace ns
