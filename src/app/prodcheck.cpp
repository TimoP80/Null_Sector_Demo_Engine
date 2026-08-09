#include "app/prodcheck.hpp"
#include "app/effectreg.hpp"
#include "framework/script/scriptengine.hpp"
#include "framework/timeline/timelineeditor.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace ns {

namespace {

// known camera rig names (must stay in sync with camerarig.cpp)
const char* const kRigs[] = {"static", "drift", "fly", "nave", "orbit",
                             "spiral", "hover", "city", "descend", "path"};

bool isKnownRig(const std::string& r) {
  for (const char* k : kRigs) if (r == k) return true;
  return false;
}

bool fileExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) && !ec;
}

/** a counting check helper: records ok/total + failure line */
struct Checker {
  ProdCheckResult& r;
  void c(bool ok, const std::string& what) {
    r.total++;
    if (ok) { r.ok++; std::fprintf(stderr, "[PRODCHECK]   ok: %s\n", what.c_str()); }
    else { r.failures.push_back(what); std::fprintf(stderr, "[PRODCHECK] FAIL: %s\n", what.c_str()); }
  }
};

}  // namespace

ProdCheckResult checkProduction(const std::string& scriptPath,
                                const std::string& dataDir,
                                const std::string& shaderDir) {
  ProdCheckResult r;

  // --- parse + build (framework-only, no GL) --------------------------------
  std::fprintf(stderr, "[PRODCHECK] validating production: %s\n", scriptPath.c_str());
  ScriptEngine se;
  try {
    if (!se.load(scriptPath)) {
      r.total++; r.failures.push_back("cannot open script: " + scriptPath);
      std::fprintf(stderr, "[PRODCHECK] FAIL: cannot open script: %s\n", scriptPath.c_str());
      return r;
    }
  } catch (const std::exception& e) {
    r.total++; r.failures.push_back(std::string("script parse failed: ") + e.what());
    std::fprintf(stderr, "[PRODCHECK] FAIL: script parse failed: %s\n", e.what());
    return r;
  }

  Checker chk{r};
  const auto& sc = se.script();
  chk.c(sc.bpm > 0, "tempo declared (" + std::to_string((int)sc.bpm) + " bpm)");
  chk.c(!sc.title.empty(), "production has a title");
  chk.c(!sc.scenes.empty(), "production declares scenes (" + std::to_string(sc.scenes.size()) + ")");

  // --- audio track ------------------------------------------------------------
  // the app auto-searches cwd / assets/ / data/ for a track at boot, so check
  // the same roots. A missing track is a NOTE, not a failure: the show can run
  // silent (--no-track) or score itself with a file dropped in later.
  {
    static const char* const kExts[] = {".wav", ".mp3", ".ogg", ".flac"};
    std::string found;
    // roots derive from dataDir so the probe behaves identically from any cwd:
    // data/ itself, the project root, and the assets/ folder inside it
    const std::string roots[] = {dataDir, dataDir + "/..", dataDir + "/../assets"};
    for (const std::string& root : roots) {
      std::error_code ec;
      for (auto it = std::filesystem::directory_iterator(root, ec);
           !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        const std::string p = it->path().string();
        for (const char* e : kExts) {
          const size_t n = p.size(), m = std::strlen(e);
          bool match = n >= m;
          for (size_t k = 0; match && k < m; k++)
            match = std::tolower((unsigned char)p[n - m + k]) ==
                    std::tolower((unsigned char)e[k]);
          if (match) { found = p; break; }
        }
        if (!found.empty()) break;
      }
      if (!found.empty()) break;
    }
    // a note, deliberately NOT a counted check: the show legitimately runs
    // silent (--no-track / synth-only productions), so this must never fail
    // a production - it only informs the author of what the app will play
    std::fprintf(stderr, "[PRODCHECK] %s: %s\n",
                 found.empty() ? "note" : "  ok",
                 found.empty()
                     ? "audio: no track found under data/ or assets/ - the show runs "
                       "silent unless one is dropped in (ok for synth/--no-track shows)"
                     : ("audio track found: " +
                        std::filesystem::path(found).filename().string())
                       .c_str());
  }

  // --- timeline build + section schedule --------------------------------------
  TimelineEditor te;
  try {
    se.build(te);
  } catch (const std::exception& e) {
    r.total++; r.failures.push_back(std::string("timeline build failed: ") + e.what());
    std::fprintf(stderr, "[PRODCHECK] FAIL: timeline build failed: %s\n", e.what());
    return r;
  }
  const auto& secs = se.sections();
  chk.c(!secs.empty(), "activation events build a section schedule (" + std::to_string(secs.size()) + " sections)");
  chk.c(se.unresolved().empty(), "every 'show' target resolves to a scene or effect");
  chk.c(te.events.size() >= (size_t)secs.size(), "timeline events cover every section");

  // section schedule sanity: monotonic starts, ends within duration
  bool monotonic = true;
  for (size_t i = 1; i < secs.size(); i++)
    if (secs[i].start < secs[i - 1].start - 0.001f) monotonic = false;
  chk.c(monotonic, "section starts are monotonic");
  bool withinDuration = te.duration > 0;
  for (const auto& s : secs)
    if (s.end > te.duration + 0.001f) withinDuration = false;
  chk.c(withinDuration, "sections end within the declared duration (" +
                            std::to_string((int)te.duration) + " s)");

  // --- reference walker --------------------------------------------------------
  // every scene setup + every at-block command is checked for file/registry
  // references. `show X`: scene | effect | shadertoy:FILE | quad:FILE.
  auto checkCmd = [&](const Cmd& c) {
    if (c.name == "show" && !c.args.empty()) {
      const std::string t = c.args[0].asStr();
      if (se.scene(t)) { chk.c(true, "show scene '" + t + "'"); return; }
      if (t.rfind("shadertoy:", 0) == 0) {
        const std::string f = t.substr(10);
        chk.c(fileExists(dataDir + "/shadertoy/" + f),
              "show shadertoy '" + f + "' (data/shadertoy/" + f + ")");
        return;
      }
      if (t.rfind("quad:", 0) == 0) {
        const std::string f = t.substr(5);
        chk.c(fileExists(shaderDir + "/" + f), "show quad shader '" + f + "' (shaders/" + f + ")");
        return;
      }
      chk.c(effectFactory().has(t), "show effect '" + t + "' is registered");
    } else if (c.name == "load" && c.args.size() >= 2) {
      const std::string kind = c.args[0].asStr();
      const std::string arg = c.args[1].asStr();
      if (kind == "shadertoy") {
        chk.c(fileExists(dataDir + "/shadertoy/" + arg),
              "load shadertoy '" + arg + "' (data/shadertoy/" + arg + ")");
      } else if (kind == "model") {
        chk.c(fileExists(dataDir + "/models/" + arg),
              "load model '" + arg + "' (data/models/" + arg + ")");
      } else if (kind == "material") {
        chk.c(fileExists(dataDir + "/materials/" + arg + ".json"),
              "load material '" + arg + "' (data/materials/" + arg + ".json)");
      } else if (kind == "plugin") {
        chk.c(fileExists(arg), "load plugin '" + arg + "'");
      } else {
        chk.c(effectFactory().has(arg), "load effect '" + arg + "' is registered");
      }
    } else if (c.name == "shader" && !c.args.empty()) {
      const std::string f = c.args[0].asStr();
      chk.c(fileExists(shaderDir + "/" + f), "shader '" + f + "' (shaders/" + f + ")");
    } else if (c.name == "post" && c.args.size() >= 2 && c.args[0].asStr() == "preset") {
      const std::string n = c.args[1].asStr();
      chk.c(fileExists(dataDir + "/post/" + n + ".json"),
            "post preset '" + n + "' (data/post/" + n + ".json)");
    } else if (c.name == "camera") {
      if (c.opts.get("rig").isStr()) {
        chk.c(isKnownRig(c.opts.get("rig").asStr()),
              "camera '" + (c.args.empty() ? std::string("?") : c.args[0].asStr()) +
                  "' rig '" + c.opts.get("rig").asStr() + "' is known");
      }
    } else if (c.name == "mesh") {
      const std::string model = c.opts.get("model").asStr(c.args.empty() ? "" : c.args[0].asStr());
      if (!model.empty())
        chk.c(fileExists(dataDir + "/models/" + model), "mesh model '" + model + "'");
    } else if (c.name == "sprite") {
      const std::string tex = c.opts.get("tex").asStr(c.args.empty() ? "" : c.args[0].asStr());
      if (!tex.empty())
        chk.c(fileExists(dataDir + "/textures/" + tex), "sprite texture '" + tex + "'");
    }
  };

  for (const auto& b : se.scenes())
    for (const auto& c : b.setup) checkCmd(c);
  for (const auto& blk : sc.main)
    for (const auto& c : blk.cmds) checkCmd(c);

  std::fprintf(stderr, "[PRODCHECK] %d/%d checks passed%s\n", r.ok, r.total,
               r.failures.empty() ? "" : " (FAILURES ABOVE)");
  return r;
}

}  // namespace ns
