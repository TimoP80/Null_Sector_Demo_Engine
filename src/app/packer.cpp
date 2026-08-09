#include "app/packer.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"
#include "framework/script/scriptengine.hpp"
#include "framework/script/scriptparser.hpp"
#include "framework/vfs/directoryfs.hpp"
#include "framework/vfs/nspack.hpp"
#include "framework/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace ns {

namespace {

// ---------------------------------------------------------------------------
// reference walker - mirrors the runtime resolution rules (see prodcheck).
// ---------------------------------------------------------------------------
struct PackRef {
  std::string vpath;  // virtual path (data/..., shaders/..., assets/...)
  std::string kind;   // shader|shadertoy|texture|model|material|post|script|font|audio|other
};

void collectCmdRefs(const Cmd& c, std::vector<PackRef>& out) {
  if (c.name == "show" && !c.args.empty()) {
    const std::string t = c.args[0].asStr();
    if (t.rfind("shadertoy:", 0) == 0) {
      out.push_back({"data/shadertoy/" + t.substr(10), "shadertoy"});
      const std::string tex = c.s("tex");
      if (!tex.empty()) out.push_back({"data/textures/" + tex, "texture"});
    } else if (t.rfind("quad:", 0) == 0) {
      out.push_back({"shaders/" + t.substr(5), "shader"});
    }
  } else if (c.name == "load" && c.args.size() >= 2) {
    const std::string kind = c.args[0].asStr();
    const std::string arg = c.args[1].asStr();
    if (kind == "shadertoy") {
      out.push_back({"data/shadertoy/" + arg, "shadertoy"});
      const std::string tex = c.s("tex");
      if (!tex.empty()) out.push_back({"data/textures/" + tex, "texture"});
    } else if (kind == "model") {
      out.push_back({"data/models/" + arg, "model"});
    } else if (kind == "material") {
      out.push_back({"data/materials/" + arg + ".json", "material"});
    } else if (kind == "plugin") {
      // plugins are native libraries: they cannot be dlopen'd from inside a
      // package, so the packer warns and skips them (documented limitation)
      std::fprintf(stderr, "[PACK] warning: plugin \"%s\" is NOT packaged (native libraries cannot load from inside a .nsp)\n", arg.c_str());
    }
  } else if (c.name == "shader" && !c.args.empty()) {
    out.push_back({"shaders/" + c.args[0].asStr(), "shader"});
  } else if (c.name == "post" && c.args.size() >= 2 && c.args[0].asStr() == "preset") {
    out.push_back({"data/post/" + c.args[1].asStr() + ".json", "post"});
  } else if (c.name == "mesh") {
    const std::string model = c.s("model", c.args.empty() ? "" : c.args[0].asStr());
    if (!model.empty()) out.push_back({"data/models/" + model, "model"});
  } else if (c.name == "sprite") {
    const std::string tex = c.s("tex", c.args.empty() ? "" : c.args[0].asStr());
    if (!tex.empty()) out.push_back({"data/textures/" + tex, "texture"});
  }
}

void collectScriptRefs(const ScriptEngine& se, std::vector<PackRef>& out) {
  for (const auto& b : se.scenes()) {
    for (const auto& c : b.setup) collectCmdRefs(c, out);
    for (const auto& blk : b.blocks)
      for (const auto& c : blk.cmds) collectCmdRefs(c, out);
  }
  for (const auto& blk : se.script().main)
    for (const auto& c : blk.cmds) collectCmdRefs(c, out);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
std::string sanitizeTitle(const std::string& t) {
  std::string out;
  for (char ch : t) {
    if (std::isalnum((unsigned char)ch)) out += (char)std::toupper((unsigned char)ch);
    else if (ch == ' ' || ch == '-' || ch == '.') out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out.empty() ? "Production" : out;
}

bool isAudioFile(const std::string& v) {
  const std::string ext = v.size() >= 4 ? v.substr(v.size() - 4) : "";
  const std::string e = ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" ? ext : "";
  (void)e;
  return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
}

}  // namespace

int runProductionPacker(const std::string& rootDir, const std::string& scriptArg,
                        const std::string& trackOverride, const std::string& outputArg) {
  // --- dev-tree VFS ---------------------------------------------------------
  DirectoryFileSystem dev;
  dev.mount("data", rootDir + "/data");
  dev.mount("shaders", rootDir + "/shaders");
  dev.mount("assets", rootDir + "/assets");
  dev.mount("", rootDir);  // catch-all root

  // --- resolve the production script (virtual path) --------------------------
  std::string scriptVpath;
  {
    const std::string norm = normalizeVirtualPath(scriptArg);
    if (!norm.empty() && dev.exists(norm)) scriptVpath = norm;
    else {
      const std::string alt = normalizeVirtualPath("data/" + scriptArg);
      if (!alt.empty() && dev.exists(alt)) scriptVpath = alt;
    }
  }
  if (scriptVpath.empty()) {
    std::fprintf(stderr, "[PACK] cannot find production script \"%s\" under \"%s\"\n", scriptArg.c_str(), rootDir.c_str());
    return 1;
  }

  // --- parse the production (existing parser, no second .nsd parser) ----------
  ScriptEngine se;
  try {
    se.loadText(dev.readText(scriptVpath), scriptVpath);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[PACK] production parse failed: %s\n", e.what());
    return 1;
  }
  const std::string title = se.script().title;

  // --- dependency collection ---------------------------------------------------
  std::set<std::string> files;          // virtual paths
  std::map<std::string, int> kinds;     // kind -> count
  std::map<std::string, std::string> fileKind;  // vpath -> kind
  auto add = [&](const std::string& vp, const std::string& kind) {
    const std::string n = normalizeVirtualPath(vp);
    if (n.empty()) return;
    if (files.insert(n).second) {
      kinds[kind]++;
      fileKind[n] = kind;
    }
  };

  add(scriptVpath, "script");

  // references from the parsed script
  std::vector<PackRef> refs;
  collectScriptRefs(se, refs);
  for (const auto& r : refs) add(r.vpath, r.kind);

  // the whole shaders/ set: effects select shader files by name from code, so
  // the .nsd alone cannot enumerate them (551 KB, all needed at runtime)
  {
    std::vector<std::string> shaders;
    walkVirtualFiles(dev, "shaders", shaders);
    for (const auto& v : shaders) add(v, "shader");
  }

  // fonts: every TrueType under assets/fonts/
  {
    std::vector<std::string> fonts;
    walkVirtualFiles(dev, "assets/fonts", fonts);
    for (const auto& v : fonts) {
      const std::string ext = v.size() >= 4 ? v.substr(v.size() - 4) : "";
      if (ext == ".ttf" || ext == ".otf") add(v, "font");
    }
  }

  // audio: --track, else a track next to the production script (mp3 preferred)
  std::string audioVpath;
  if (!trackOverride.empty()) {
    const std::string n = normalizeVirtualPath(trackOverride);
    if (!n.empty() && dev.exists(n)) audioVpath = n;
  }
  if (audioVpath.empty()) {
    // no explicit --track: look for a soundtrack in the production's own
    // directory AND in the folder named after the script (data/foo.nsd +
    // data/foo/track.mp3) - top-level files only, so other productions'
    // tracks in sibling folders never leak into this package
    const size_t slash = scriptVpath.rfind("/");
    const std::string dir = slash == std::string::npos
                                 ? std::string()
                                 : scriptVpath.substr(0, slash);
    const std::string base = slash == std::string::npos
                                  ? scriptVpath
                                  : scriptVpath.substr(slash + 1);
    const size_t dot = base.rfind(".");
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    const std::string prodDir = dir.empty() ? stem : dir + "/" + stem;
    std::vector<std::string> cands;
    auto scanDir = [&](const std::string& d) {
      for (const auto& e : dev.list(d)) {
        const VFileInfo fi = dev.stat(e);
        if (fi.exists && !fi.isDir && isAudioFile(e)) cands.push_back(e);
      }
    };
    scanDir(dir);
    if (prodDir != dir) scanDir(prodDir);
    std::sort(cands.begin(), cands.end());
    auto score = [](const std::string& v) {
      const std::string ext = v.size() >= 4 ? v.substr(v.size() - 4) : "";
      if (ext == ".mp3") return 0;
      if (ext == ".ogg") return 1;
      if (ext == ".wav") return 2;
      return 3;
    };
    std::stable_sort(cands.begin(), cands.end(),
                     [&](const std::string& a, const std::string& b) {
                       return score(a) < score(b);
                     });
    if (!cands.empty()) audioVpath = cands[0];
    else std::fprintf(stderr, "[PACK] warning: no audio track next to the production (pass --track=...)\n");
  }
  if (!audioVpath.empty()) add(audioVpath, "audio");

  // transitive: material JSONs -> albedo/normal maps (data/textures/)
  {
    std::vector<std::string> mats;
    for (const auto& f : files) {
      if (f.rfind("data/materials/", 0) != 0) continue;
      const std::string ext = f.size() >= 5 ? f.substr(f.size() - 5) : "";
      if (ext == ".json") mats.push_back(f);
    }
    for (const auto& m : mats) {
      Value mv;
      try {
        mv = Json::parseText(dev.readText(m));
      } catch (...) { continue; }
      for (const char* field : {"albedoMap", "normalMap"}) {
        const std::string tex = mv.get(field).asStr();
        if (!tex.empty()) {
          const std::string v = tex.rfind("data/textures/", 0) == 0
                                    ? tex
                                    : "data/textures/" + tex;
          if (dev.exists(v)) add(v, "texture");
        }
      }
    }
  }

  // --- report -------------------------------------------------------------------
  std::fprintf(stderr, "\nNull Sector Production Packer\n");
  std::fprintf(stderr, "------------------------------\n");
  std::fprintf(stderr, "Production: %s\n", title.c_str());
  std::fprintf(stderr, "Source:     %s\n", scriptVpath.c_str());
  if (!audioVpath.empty())
    std::fprintf(stderr, "Audio:      %s\n", audioVpath.c_str());
  std::fprintf(stderr, "\nAssets:\n");
  const char* order[] = {"shader", "shadertoy", "texture", "model", "material",
                         "post",   "font",      "audio",   "script", "other"};
  for (const char* k : order) {
    auto it = kinds.find(k);
    if (it != kinds.end() && it->second > 0)
      std::fprintf(stderr, "  %-10s %d\n", k, it->second);
  }

  // --- write the package -------------------------------------------------------
  const std::string outPath = outputArg.empty()
                                 ? sanitizeTitle(title) + ".nsp"
                                 : outputArg;
  PackageWriter pw;
  std::string err;
  if (!pw.begin(outPath, &err)) {
    std::fprintf(stderr, "[PACK] %s\n", err.c_str());
    return 1;
  }
  for (const auto& v : files) {
    const VFileInfo fi = dev.stat(v);
    if (!fi.exists) {
      std::fprintf(stderr, "[PACK] warning: referenced file missing, skipped: %s\n",
                   v.c_str());
      continue;
    }
    if (!pw.addFile(v, dev.read(v), &err)) {
      std::fprintf(stderr, "[PACK] %s\n", err.c_str());
      return 1;
    }
  }

  uint64_t raw = 0;
  for (const auto& v : files) {
    const VFileInfo fi = dev.stat(v);
    if (fi.exists) raw += fi.size;
  }

  // record which production this package runs (the .ns-production marker)
  if (!pw.setProduction(scriptVpath, &err)) {
    std::fprintf(stderr, "[PACK] %s\n", err.c_str());
    return 1;
  }

  if (!pw.finish(&err)) {
    std::fprintf(stderr, "[PACK] %s\n", err.c_str());
    return 1;
  }
  const uint64_t pkgSize =
      (uint64_t)std::filesystem::file_size(outPath);

  std::fprintf(stderr, "\nFiles:       %zu\n", pw.fileCount());
  std::fprintf(stderr, "Raw size:    %.1f MB\n", raw / 1048576.0);
  std::fprintf(stderr, "Package:     %.1f MB\n", pkgSize / 1048576.0);
  std::fprintf(stderr, "\nCreated:\n  %s\n", outPath.c_str());
  return 0;
}

}  // namespace ns
