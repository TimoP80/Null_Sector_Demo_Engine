// ---------------------------------------------------------------------------
// Value - a small variant (Null/Bool/Num/Str/Array/Object) shared by the JSON
// layer, the scripting DSL and scene serialization. Objects keep insertion
// order (script commands and JSON objects both care), lookups are linear on a
// small map - plenty fast for data-driven scenes.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ns {

class Value {
public:
  enum class Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };

  using Pair = std::pair<std::string, Value>;
  using Object = std::vector<Pair>;
  using Array = std::vector<Value>;

  Value() = default;
  explicit Value(bool b) : type_(Type::Bool), num_(b ? 1.0 : 0.0) {}
  explicit Value(double n) : type_(Type::Num), num_(n) {}
  explicit Value(float n) : type_(Type::Num), num_(n) {}
  explicit Value(int n) : type_(Type::Num), num_((double)n) {}
  explicit Value(std::string s) : type_(Type::Str), str_(std::move(s)) {}
  explicit Value(const char* s) : type_(Type::Str), str_(s ? s : "") {}
  explicit Value(Array a) : type_(Type::Arr), arr_(std::move(a)) {}
  explicit Value(Object o) : type_(Type::Obj), obj_(std::move(o)) {}

  static Value null() { return Value(); }
  static Value array() { return Value(Array{}); }
  static Value object() { return Value(Object{}); }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNum() const { return type_ == Type::Num; }
  bool isStr() const { return type_ == Type::Str; }
  bool isArr() const { return type_ == Type::Arr; }
  bool isObj() const { return type_ == Type::Obj; }

  bool asBool(bool dflt = false) const { return type_ == Type::Bool ? num_ != 0.0 : dflt; }
  double asNum(double dflt = 0.0) const { return type_ == Type::Num ? num_ : dflt; }
  float asFloat(float dflt = 0.0f) const { return (float)asNum(dflt); }
  int asInt(int dflt = 0) const { return (int)asNum(dflt); }
  const std::string& asStr(const std::string& dflt = {}) const {
    static const std::string kEmpty;
    return type_ == Type::Str ? str_ : dflt;
  }
  const Array& asArr() const { return arr_; }
  const Object& asObj() const { return obj_; }

  // --- object access --------------------------------------------------------
  /** index into an object by key; returns null() when absent */
  const Value& get(const std::string& key) const {
    static const Value kNull;
    if (type_ != Type::Obj) return kNull;
    for (const auto& p : obj_) if (p.first == key) return p.second;
    return kNull;
  }
  /** nested access with dot path ("camera.fov", "lights.0.color") - numeric
   *  segments address array elements */
  const Value& at(const std::string& dotPath) const {
    const Value* cur = this;
    std::string key;
    const auto step = [&]() {
      if (key.empty()) return;
      if (cur->isArr()) {
        int idx = 0;
        for (char ch : key) {
          if (ch < '0' || ch > '9') { idx = -1; break; }
          idx = idx * 10 + (ch - '0');
        }
        cur = &cur->atIndex(idx >= 0 ? (size_t)idx : (size_t)-1);
      } else {
        cur = &cur->get(key);
      }
      key.clear();
    };
    for (size_t i = 0; i <= dotPath.size(); i++) {
      if (i == dotPath.size() || dotPath[i] == '.') step();
      else key += dotPath[i];
    }
    return *cur;
  }
  /** mutable object entry (inserts when missing) */
  Value& set(const std::string& key) {
    if (type_ != Type::Obj) { *this = Value::object(); }
    for (auto& p : obj_) if (p.first == key) return p.second;
    obj_.emplace_back(key, Value::null());
    return obj_.back().second;
  }

  // --- array access ---------------------------------------------------------
  size_t size() const {
    if (type_ == Type::Arr) return arr_.size();
    if (type_ == Type::Obj) return obj_.size();
    if (type_ == Type::Str) return str_.size();
    return 0;
  }
  const Value& atIndex(size_t i) const {
    static const Value kNull;
    return (type_ == Type::Arr && i < arr_.size()) ? arr_[i] : kNull;
  }
  void push(Value v) {
    if (type_ != Type::Arr) *this = Value::array();
    arr_.push_back(std::move(v));
  }

  // --- convenience ----------------------------------------------------------
  std::string toString() const;

  /** scalar/vector coercion used by animation + uniforms: 1, 2, 3 or 4 floats
   *  out of a scalar / array / "r,g,b" string */
  int toFloats(float* out, int maxN) const;

private:
  Type type_ = Type::Null;
  double num_ = 0.0;
  std::string str_;
  Array arr_;
  Object obj_;
};

}  // namespace ns
