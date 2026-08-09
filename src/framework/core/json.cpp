#include "framework/core/json.hpp"
#include "framework/core/log.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ns {
namespace Json {

// ---------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------
namespace {

struct Cursor {
  const std::string& s;
  size_t i = 0;
  int line = 1;
  int col = 1;

  explicit Cursor(const std::string& src) : s(src) {}

  [[noreturn]] void fail(const std::string& msg) const {
    std::ostringstream o;
    o << "JSON error at " << line << ":" << col << ": " << msg;
    throw JsonError(o.str());
  }
  char peek() const { return i < s.size() ? s[i] : '\0'; }
  char next() {
    if (i >= s.size()) return '\0';
    const char c = s[i++];
    if (c == '\n') { line++; col = 1; } else { col++; }
    return c;
  }
  void skipWs() {
    while (i < s.size()) {
      const char c = s[i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { next(); }
      else break;
    }
  }
};

void parseValue(Cursor& c, Value& out);

void parseNumber(Cursor& c, Value& out) {
  const size_t start = c.i;
  bool dot = false, exp = false;
  while (c.i < c.s.size()) {
    const char ch = c.s[c.i];
    if (ch >= '0' && ch <= '9') { c.next(); }
    else if (ch == '-' && c.i == start) { c.next(); }
    else if (ch == '.' && !dot) { dot = true; c.next(); }
    else if ((ch == 'e' || ch == 'E') && !exp) { exp = true; c.next(); }
    else if ((ch == '+' || ch == '-') && exp && c.i > start && (c.s[c.i - 1] == 'e' || c.s[c.i - 1] == 'E')) { c.next(); }
    else break;
  }
  if (c.i == start) c.fail("expected a number");
  const std::string tok = c.s.substr(start, c.i - start);
  out = Value(std::strtod(tok.c_str(), nullptr));
}

void parseString(Cursor& c, Value& out) {
  std::string s;
  if (c.next() != '"') c.fail("expected string");
  while (true) {
    const char ch = c.next();
    if (ch == '\0') c.fail("unterminated string");
    if (ch == '"') break;
    if (ch == '\\') {
      const char e = c.next();
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          if (c.i + 4 > c.s.size()) c.fail("bad \\u escape");
          unsigned cp = 0;
          for (int k = 0; k < 4; k++) {
            const char h = c.next();
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            else c.fail("bad \\u hex digit");
          }
          if (cp < 0x80) s += (char)cp;
          else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
          } else {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
          }
          break;
        }
        default: c.fail("bad escape \\" + std::string(1, e));
      }
    } else {
      s += ch;
    }
  }
  out = Value(std::move(s));
}

void parseObject(Cursor& c, Value& out) {
  Value::Object obj;
  c.next();  // {
  c.skipWs();
  if (c.peek() == '}') { c.next(); out = Value(std::move(obj)); return; }
  while (true) {
    c.skipWs();
    if (c.peek() != '"') c.fail("expected object key");
    Value keyV;
    parseString(c, keyV);
    c.skipWs();
    if (c.next() != ':') c.fail("expected ':' after key");
    c.skipWs();
    Value v;
    parseValue(c, v);
    obj.emplace_back(keyV.asStr(), std::move(v));
    c.skipWs();
    const char ch = c.next();
    if (ch == ',') continue;
    if (ch == '}') break;
    c.fail("expected ',' or '}' in object");
  }
  out = Value(std::move(obj));
}

void parseArray(Cursor& c, Value& out) {
  Value::Array arr;
  c.next();  // [
  c.skipWs();
  if (c.peek() == ']') { c.next(); out = Value(std::move(arr)); return; }
  while (true) {
    c.skipWs();
    Value v;
    parseValue(c, v);
    arr.push_back(std::move(v));
    c.skipWs();
    const char ch = c.next();
    if (ch == ',') continue;
    if (ch == ']') break;
    c.fail("expected ',' or ']' in array");
  }
  out = Value(std::move(arr));
}

void parseValue(Cursor& c, Value& out) {
  c.skipWs();
  const char ch = c.peek();
  if (ch == '{') parseObject(c, out);
  else if (ch == '[') parseArray(c, out);
  else if (ch == '"') parseString(c, out);
  else if (ch == 't') { for (int k = 0; k < 4; k++) c.next(); out = Value(true); }
  else if (ch == 'f') { for (int k = 0; k < 5; k++) c.next(); out = Value(false); }
  else if (ch == 'n') { for (int k = 0; k < 4; k++) c.next(); out = Value::null(); }
  else if (ch == '-' || (ch >= '0' && ch <= '9')) parseNumber(c, out);
  else c.fail(std::string("unexpected character '") + ch + "'");
}

}  // namespace

Value parse(const std::string& text) {
  Cursor c(text);
  Value out;
  parseValue(c, out);
  c.skipWs();
  if (c.i < c.s.size()) c.fail("trailing characters after document");
  return out;
}

Value parseFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw JsonError("cannot open JSON file: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

Value parseText(const std::string& text) {
  return parse(text);
}

// ---------------------------------------------------------------------------
// serializer
// ---------------------------------------------------------------------------
namespace {

void writeString(std::string& out, const std::string& s) {
  out += '"';
  for (char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if ((unsigned char)ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)ch);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void write(const Value& v, std::string& out, int indent, int depth) {
  const std::string pad(static_cast<size_t>((depth + 1) * std::max(indent, 0)), ' ');
  const std::string close(static_cast<size_t>(depth * std::max(indent, 0)), ' ');
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Num: {
      char buf[32];
      const double d = v.asNum();
      if (d == (double)(int64_t)d) std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
      else std::snprintf(buf, sizeof(buf), "%.9g", d);
      out += buf;
      break;
    }
    case Value::Type::Str: writeString(out, v.asStr()); break;
    case Value::Type::Arr: {
      const auto& a = v.asArr();
      if (a.empty()) { out += "[]"; break; }
      out += '[';
      for (size_t i = 0; i < a.size(); i++) {
        if (i) out += ',';
        if (indent >= 0) out += '\n' + pad;
        write(a[i], out, indent, depth + 1);
      }
      if (indent >= 0) out += '\n' + close;
      out += ']';
      break;
    }
    case Value::Type::Obj: {
      const auto& o = v.asObj();
      if (o.empty()) { out += "{}"; break; }
      out += '{';
      for (size_t i = 0; i < o.size(); i++) {
        if (i) out += ',';
        if (indent >= 0) out += '\n' + pad;
        writeString(out, o[i].first);
        out += indent >= 0 ? ": " : ":";
        write(o[i].second, out, indent, depth + 1);
      }
      if (indent >= 0) out += '\n' + close;
      out += '}';
      break;
    }
  }
}

}  // namespace

std::string serialize(const Value& v, int indent) {
  std::string out;
  write(v, out, indent, 0);
  return out;
}

void writeFile(const std::string& path, const Value& v, int indent) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw JsonError("cannot write JSON file: " + path);
  f << serialize(v, indent);
}

}  // namespace Json

// ---------------------------------------------------------------------------
// Value helpers (defined here to keep the header light)
// ---------------------------------------------------------------------------
std::string Value::toString() const {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return num_ != 0.0 ? "true" : "false";
    case Type::Num: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.9g", num_);
      return buf;
    }
    case Type::Str: return str_;
    case Type::Arr: return Json::serialize(*this, 0);
    case Type::Obj: return Json::serialize(*this, 0);
  }
  return "?";
}

int Value::toFloats(float* out, int maxN) const {
  int n = 0;
  const auto add = [&](double d) { if (n < maxN) out[n++] = (float)d; };
  switch (type_) {
    case Type::Num: add(num_); break;
    case Type::Arr:
      for (const auto& e : arr_) {
        if (e.isNum()) add(e.asNum());
        else if (e.isArr() || e.isStr()) { int k = e.toFloats(out + n, maxN - n); n += k; }
      }
      break;
    case Type::Str: {
      // "r,g,b", "(r,g,b)", "1 2 3" string form - parens/brackets are separators
      std::string cur;
      for (char ch : str_) {
        if (ch == ',' || ch == ' ' || ch == '(' || ch == ')' || ch == '[' || ch == ']') {
          if (!cur.empty()) { add(std::strtod(cur.c_str(), nullptr)); cur.clear(); }
        } else cur += ch;
      }
      if (!cur.empty()) add(std::strtod(cur.c_str(), nullptr));
      break;
    }
    default: break;
  }
  return n;
}

}  // namespace ns
