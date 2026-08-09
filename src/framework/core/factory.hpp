// ---------------------------------------------------------------------------
// Factory - a small type-erased registry used for the effect plugin system.
// Effects (and asset loaders) register themselves by name; data files address
// them by name, so new content never requires touching the engine loop.
//
//   NS_REGISTER_EFFECT("tunnel", [](const Value& p) {
//       return std::unique_ptr<Effect>(new TunnelFX); });
//
// Registration happens in the effect's own translation unit (or the app glue),
// runs before main(), and is fully thread-safe at that point. The registry is
// keyed by name; `create()` throws an informative error for unknown names.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/value.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ns {

template <typename Base>
class Factory {
public:
  using Fn = std::function<std::unique_ptr<Base>(const Value& params)>;

  void reg(const std::string& name, Fn fn) {
    std::lock_guard<std::mutex> lock(m_);
    factories_[name] = std::move(fn);
  }

  bool has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_);
    return factories_.count(name) != 0;
  }

  /** create an instance; throws std::runtime_error on unknown names */
  std::unique_ptr<Base> create(const std::string& name, const Value& params = Value::null()) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = factories_.find(name);
    if (it == factories_.end()) {
      throw std::runtime_error("no factory registered for '" + name + "'");
    }
    return it->second(params);
  }

  std::vector<std::string> names() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& kv : factories_) out.push_back(kv.first);
    return out;
  }

  size_t count() const {
    std::lock_guard<std::mutex> lock(m_);
    return factories_.size();
  }

private:
  mutable std::mutex m_;
  std::map<std::string, Fn> factories_;
};

/** the process-wide factory for an arbitrary base type */
template <typename Base>
inline Factory<Base>& factory() {
  static Factory<Base> f;
  return f;
}

/** register a factory for an arbitrary base type; returns true so it can be
 *  used in a static-init expression:  static const bool _ = regFactory<X>(...); */
template <typename Base>
inline bool regFactory(const std::string& name, typename Factory<Base>::Fn fn) {
  factory<Base>().reg(name, std::move(fn));
  return true;
}

}  // namespace ns
