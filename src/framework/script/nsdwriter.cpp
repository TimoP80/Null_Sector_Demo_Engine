// ---------------------------------------------------------------------------
// NsdWriter implementation.
//
// The serializer mirrors the lexer's token rules exactly so the output
// re-parses to the same AST: strings are emitted unquoted only when they
// lex back as the same Ident token (alpha/_ start, ident chars only), and
// quoted with escapes otherwise (numbers, times, spaces, keywords that would
// lex differently). Number formatting uses %.9g (the same precision as
// Value::toString). Scene options (bars/intensity/chapter/duration/visible)
// are omitted at their defaults so saves stay minimal; the parser re-applies
// the same defaults.
// ---------------------------------------------------------------------------
#include "framework/script/nsdwriter.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace ns {
namespace {

/** shortest decimal that re-parses to the same value: float-derived values
 *  (times, intensities) print as their clean decimals ("77.8", not
 *  "77.8000031"); doubles that are not exactly float-representable use the
 *  shortest double form (%.9g..%.17g). Exact round-trip either way. */
std::string fmtNum(double v) {
  char b[40];
  const float f = (float)v;
  if ((double)f == v) {
    for (int prec = 6; prec <= 9; prec++) {
      std::snprintf(b, sizeof b, "%.*g", prec, (double)f);
      if ((float)std::strtod(b, nullptr) == f) return b;
    }
    std::snprintf(b, sizeof b, "%.9g", (double)f);
    return b;
  }
  for (int prec = 9; prec <= 17; prec++) {
    std::snprintf(b, sizeof b, "%.*g", prec, v);
    if (std::strtod(b, nullptr) == v) return b;
  }
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

bool isSafeIdent(const std::string& s) {
  if (s.empty()) return false;
  const unsigned char c0 = (unsigned char)s[0];
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (char ch : s) {
    const unsigned char c = (unsigned char)ch;
    if (!(std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-' ||
          c == '+' || c == ':'))
      return false;
  }
  return true;
}

std::string quoteStr(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += '"';
  return out;
}

std::string fmtValue(const Value& v) {
  switch (v.type()) {
    case Value::Type::Num:
      return fmtNum(v.asNum());
    case Value::Type::Bool:
      return v.asBool() ? "true" : "false";
    case Value::Type::Str: {
      const std::string& s = v.asStr();
      return isSafeIdent(s) ? s : quoteStr(s);
    }
    case Value::Type::Arr: {
      std::string out = "(";
      const Value::Array& a = v.asArr();
      for (size_t i = 0; i < a.size(); i++) {
        if (i) out += ",";
        out += fmtValue(a[i]);
      }
      out += ")";
      return out;
    }
    default:
      return "null";
  }
}

std::string fmtCmd(const Cmd& c) {
  std::string out = c.name;
  for (const auto& a : c.args) {
    out += ' ';
    out += fmtValue(a);
  }
  // option pairs and keyframe rows share one '{ }' block (the parser reads a
  // single block per command); options first, then keyframe rows
  const size_t nOpts = c.opts.isObj() ? c.opts.size() : 0;
  const size_t nKeys = c.keys.size();
  if (nOpts + nKeys > 0) {
    out += " {";
    size_t written = 0;
    if (nOpts) {
      for (const auto& p : c.opts.asObj()) {
        out += written ? "; " : " ";
        out += p.first;
        out += ' ';
        out += fmtValue(p.second);
        written++;
      }
    }
    for (const auto& k : c.keys) {
      out += written ? "; " : " ";
      out += fmtNum(k.t);
      out += ' ';
      out += fmtValue(k.v);
      if (!k.interp.empty()) {
        out += ' ';
        out += k.interp;
      }
      written++;
    }
    out += " }";
  }
  return out;
}

std::string fmtBlock(const ScriptBlock& b) {
  std::string out = "at " + fmtNum(b.time) + " {";
  for (size_t i = 0; i < b.cmds.size(); i++) {
    out += i ? "; " : " ";
    out += fmtCmd(b.cmds[i]);
  }
  out += " }";
  return out;
}

}  // namespace

std::string nsdSerializeCmd(const Cmd& c) { return fmtCmd(c); }

std::string nsdSerialize(const Script& s) {
  std::string out;
  out += "demo";
  if (!s.title.empty()) {
    out += ' ';
    out += quoteStr(s.title);
  }
  out += " {\n";
  if (s.bpm > 0 && s.bpm != 216.0f) out += "    bpm " + fmtNum(s.bpm) + "\n";
  if (s.duration > 0) out += "    duration " + fmtNum(s.duration) + "\n";
  out += "}\n\n";

  for (const auto& sc : s.scenes) {
    out += "scene " + sc.name + " {\n";
    std::string head;
    const auto addHead = [&head](const std::string& k, const std::string& v) {
      if (!head.empty()) head += "  ";
      head += k;
      head += ' ';
      head += v;
    };
    if (sc.bars > 0) addHead("bars", std::to_string(sc.bars));
    if (sc.intensity != 0.5f) addHead("intensity", fmtNum(sc.intensity));
    if (sc.chapter != 0) addHead("chapter", std::to_string(sc.chapter));
    if (sc.duration > 0) addHead("duration", fmtNum(sc.duration));
    if (!sc.visible) addHead("visible", "false");
    if (!head.empty()) out += "    " + head + "\n";
    if (!sc.title.empty()) out += "    title " + quoteStr(sc.title) + "\n";
    for (const auto& c : sc.setup) out += "    " + fmtCmd(c) + "\n";
    for (const auto& b : sc.blocks) out += "    " + fmtBlock(b) + "\n";
    out += "}\n\n";
  }

  for (const auto& b : s.main) out += fmtBlock(b) + "\n";

  return out;
}

}  // namespace ns
