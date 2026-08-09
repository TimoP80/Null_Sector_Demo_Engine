#include "framework/resources/assetmanager.hpp"
#include "framework/core/log.hpp"

namespace ns {

const char* assetKindName(AssetKind k) {
  switch (k) {
    case AssetKind::Texture: return "texture";
    case AssetKind::Shader: return "shader";
    case AssetKind::Font: return "font";
    case AssetKind::Model: return "model";
    case AssetKind::Music: return "music";
    case AssetKind::Sample: return "sample";
    case AssetKind::Script: return "script";
    case AssetKind::Video: return "video";
    case AssetKind::Cubemap: return "cubemap";
    case AssetKind::Material: return "material";
    case AssetKind::Timeline: return "timeline";
    case AssetKind::Scene: return "scene";
    case AssetKind::Plugin: return "plugin";
  }
  return "asset";
}

AssetKind assetKindFromName(const std::string& n) {
  if (n == "texture") return AssetKind::Texture;
  if (n == "shader") return AssetKind::Shader;
  if (n == "font") return AssetKind::Font;
  if (n == "model") return AssetKind::Model;
  if (n == "music") return AssetKind::Music;
  if (n == "sample") return AssetKind::Sample;
  if (n == "script") return AssetKind::Script;
  if (n == "video") return AssetKind::Video;
  if (n == "cubemap") return AssetKind::Cubemap;
  if (n == "material") return AssetKind::Material;
  if (n == "timeline") return AssetKind::Timeline;
  if (n == "scene") return AssetKind::Scene;
  if (n == "plugin") return AssetKind::Plugin;
  return AssetKind::Texture;
}

void AssetManager::registerKind(const std::string& kind, LoadFn load, FreeFn free, ReloadFn reload) {
  kinds_[kind] = {std::move(load), std::move(free), std::move(reload)};
}

void* AssetManager::acquire(const std::string& path, const std::string& kind) {
  const std::string k = key(path, kind);
  auto it = assets_.find(k);
  if (it != assets_.end()) {
    it->second.refs++;
    return it->second.handle;
  }
  auto kf = kinds_.find(kind);
  if (kf == kinds_.end()) {
    Log::error("ASSET", "no loader registered for kind '" + kind + "' (" + path + ")");
    return nullptr;
  }
  AssetInfo info;
  info.path = assetCanonicalPath(path);
  info.kind = kind;
  info.handle = kf->second.load(path);
  if (info.handle) {
    info.loaded = true;
    info.version = 1;
  } else {
    info.error = "load failed";
  }
  info.refs = 1;
  assets_[k] = std::move(info);
  if (!assets_[k].handle) {
    Log::error("ASSET", "failed to load " + kind + " '" + path + "'");
  }
  return assets_[k].handle;
}

void AssetManager::release(const std::string& path, const std::string& kind) {
  const std::string k = key(path, kind);
  auto it = assets_.find(k);
  if (it == assets_.end()) return;
  AssetInfo& info = it->second;
  info.refs--;
  if (info.refs <= 0) {
    auto kf = kinds_.find(kind);
    if (kf != kinds_.end() && info.handle) kf->second.free(info.handle);
    assets_.erase(it);
  }
}

AssetInfo* AssetManager::find(const std::string& path, const std::string& kind) {
  auto it = assets_.find(key(path, kind));
  return it != assets_.end() ? &it->second : nullptr;
}

const AssetInfo* AssetManager::find(const std::string& path, const std::string& kind) const {
  auto it = assets_.find(key(path, kind));
  return it != assets_.end() ? &it->second : nullptr;
}

void AssetManager::markDirty(const std::string& path) {
  // mark any asset whose path matches (regardless of kind); compare in the
  // canonical form so watcher-reported and acquired paths always agree
  const std::string canon = assetCanonicalPath(path);
  bool found = false;
  for (auto& kv : assets_) {
    if (kv.second.path == canon) {
      kv.second.dirty = true;
      found = true;
    }
  }
  if (!found) {
    // not loaded yet - nothing to reload; the loader reads fresh on acquire
    Log::debug("ASSET", "markDirty: '" + path + "' not loaded (ignored)");
  }
}

int AssetManager::reloadDirty() {
  int n = 0;
  for (auto& kv : assets_) {
    AssetInfo& info = kv.second;
    if (!info.dirty) continue;
    auto kf = kinds_.find(info.kind);
    if (kf == kinds_.end()) { info.dirty = false; continue; }
    if (!info.loaded) {
      // the load failed earlier - retry the loader now that the file changed
      // (the user may have just dropped it in). A failed load is not
      // terminal: without this retry the asset stays dead until restart.
      void* fresh = kf->second.load(info.path);
      if (fresh) {
        info.handle = fresh;
        info.loaded = true;
        info.error.clear();
        info.version++;
        n++;
        Log::info("ASSET", "loaded " + info.kind + " '" + info.path + "' (retried after failure)");
      } else {
        Log::warn("ASSET", "retry failed for " + info.kind + " '" + info.path + "' - still missing");
      }
      info.dirty = false;
      continue;
    }
    if (kf->second.reload) {
      void* h = info.handle;
      if (kf->second.reload(info.path, h)) {
        info.handle = h;
        info.version++;
        info.error.clear();
        Log::info("ASSET", "reloaded " + info.kind + " '" + info.path + "' (v" +
                               std::to_string(info.version) + ")");
      } else {
        info.error = "reload failed (kept previous)";
        Log::warn("ASSET", "reload failed for " + info.kind + " '" + info.path +
                               "' - keeping previous version");
      }
    } else {
      // no reload fn: re-run the loader and swap
      void* fresh = kf->second.load(info.path);
      if (fresh) {
        if (info.handle) kf->second.free(info.handle);
        info.handle = fresh;
        info.version++;
        Log::info("ASSET", "reloaded " + info.kind + " '" + info.path + "'");
      } else {
        Log::warn("ASSET", "reload produced nothing for '" + info.path + "' - keeping previous");
      }
    }
    info.dirty = false;
    n++;
  }
  return n;
}

bool AssetManager::anyDirty() const {
  for (const auto& kv : assets_) if (kv.second.dirty) return true;
  return false;
}

uint64_t AssetManager::version(const std::string& path, const std::string& kind) const {
  const AssetInfo* i = find(path, kind);
  return i ? i->version : 0;
}

std::vector<AssetInfo*> AssetManager::all() {
  std::vector<AssetInfo*> out;
  for (auto& kv : assets_) out.push_back(&kv.second);
  return out;
}

std::vector<const AssetInfo*> AssetManager::all() const {
  std::vector<const AssetInfo*> out;
  for (const auto& kv : assets_) out.push_back(&kv.second);
  return out;
}

void AssetManager::clear() {
  for (auto& kv : assets_) {
    auto kf = kinds_.find(kv.second.kind);
    if (kf != kinds_.end() && kv.second.handle) kf->second.free(kv.second.handle);
  }
  assets_.clear();
}

}  // namespace ns
