#include "app/demoapp.hpp"
#include "app/effectreg.hpp"
#include "app/hotreloadcheck.hpp"
#include "app/shadertoy.hpp"
#include "effects/greetings.hpp"
#include "effects/intro.hpp"
#include "effects/scene.hpp"
#include "effects/tunnel.hpp"
#include "engine/assets.hpp"
#include "engine/cinetext.hpp"
#include "engine/paths.hpp"
#include "engine/postprocess.hpp"
#include "framework/core/json.hpp"
#include "framework/vfs/vfs.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <thread>

namespace ns {

static std::string dataDir() { return AppAssets::dataDir(); }

/** read a script through the runtime VFS, falling back to a direct file
 *  read for absolute editor paths. Returns "" when neither works. */
static std::string readScriptText(const std::string& path) {
  std::string text = runtimeFS().readText(path);
  if (text.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path, std::ios::binary);
      if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
      }
    }
  }
  return text;
}

DemoApp::~DemoApp() { delete cineText_; }

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
void DemoApp::init(const Input& in) {
  in_ = in;
  ctx_.r = in_.r;
  ctx_.assets = in_.assets;
  ctx_.timeline = in_.timeline;
  ctx_.camera = in_.camera;
  ctx_.audio = in_.audio;
  ctx_.shared = in_.shared;
  ctx_.post = in_.postfx;

  shaders_.setShaderDir(AppAssets::shaderDir());
  AppAssets::init(assets_);

  // effect plugin system: builtins + any plugins in data/plugins/
  registerBuiltinEffects();
  if (!in_.pluginDir.empty()) {
    std::error_code ec;
    // NOTE: a missing dir yields an end iterator (ec set, body never runs),
    // so the check must live AFTER the loop to fire in that case.
    for (const auto& e : std::filesystem::directory_iterator(in_.pluginDir, ec)) {
      if (ec) break;
      const std::string ext = e.path().extension().string();
      if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
        loadEffectPlugin(e.path().string());
      }
    }
    if (ec) {
      Log::info("FX", "no plugin directory at '" + in_.pluginDir + "' - skipping plugin scan");
    }
  }

  // script -> timeline. build() must run BEFORE buildSections(): it flattens
  // the `at` blocks AND populates script_.sections() (the section schedule is
  // produced by build(), not load()), so the DemoApp section list + the
  // timeline's data-driven schedule are correct from the very first frame
  // (previously they silently fell back to the built-in schedule until the
  // first live reload).
  {
    const std::string text = readScriptText(in_.scriptPath);
    if (text.empty()) {
      throw std::runtime_error("demo script not found: " + in_.scriptPath +
                               " (use --demo=PATH or place data/demo.nsd next to the exe)");
    }
    script_.loadText(text, in_.scriptPath);
  }
  script_.build(editor_);
  buildSections();
  editor_.play();

  // model renderer + default camera rig
  modelRenderer_.init(shaders_);
  auto def = std::make_unique<CameraRig>();
  def->type = "static";
  def->pos = {0, 0, 2};
  def->fov = 55;
  rigs_["default"] = std::move(def);

  post_ = std::make_unique<PostStack>(*in_.r);

  // live reload watcher: shaders + data + assets
  watcher_.add(AppAssets::shaderDir(), {".vert", ".frag", ".glsl"});
  watcher_.add(dataDir(), {".nsd", ".json", ".glsl", ".obj", ".png", ".jpg", ".mtl"});
  watcher_.add(resolveRuntimeDir("NULLSECTOR_ASSET_DIR", NULLSECTOR_ASSET_DIR, "assets"),
               {".png", ".jpg", ".ttf"});
  watcher_.poll();  // baseline

  // shared internal programs
  copyProg_ = shaders_.get("fullscreen.vert", "post_copy.frag");
  spriteProg_ = shaders_.get("sprite.vert", "sprite.frag");

  // render targets
  rebuildTargets();

  cineText_ = new CineText();
  cineText_->init(*in_.assets);

  initDone_ = true;
  Log::info("APP", "DemoApp ready: '" + script_.script().title + "' (" +
                       std::to_string((int)editor_.duration) + "s, " +
                       std::to_string(script_.scenes().size()) + " scenes, " +
                       std::to_string(script_.sections().size()) + " sections)");
}

void DemoApp::rebuildTargets() {
  const TextureOpts opts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  hdr_ = FrameTarget::color(std::max(2, in_.r->resW), std::max(2, in_.r->resH),
                            ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts);
  meshFbo_ = FrameTarget::colorDepth(std::max(2, in_.r->resW), std::max(2, in_.r->resH));
  fxTargets_.clear();
  ctx_.hdr = &hdr_;
}

void DemoApp::buildSections() {
  // convert the script-engine sections into the engine timeline's schedule
  std::vector<SectionInfo> secs;
  const float bs = in_.timeline->beatSec();
  int idx = 0;
  for (const auto& s : script_.sections()) {
    SectionInfo si;
    si.index = idx++;
    si.name = s.name;
    si.startSec = s.start;
    si.endSec = s.end;
    si.duration = s.duration;
    si.intensity = s.intensity;
    si.chapter = s.chapter;
    si.startBeat = s.start / bs;
    si.endBeat = s.end / bs;
    secs.push_back(si);
  }
  if (!secs.empty()) {
    in_.timeline->setSections(std::move(secs));
    in_.timeline->setBpm(script_.bpm());
  }
  sections_ = script_.sections();
}

// ---------------------------------------------------------------------------
// per-frame update
// ---------------------------------------------------------------------------
void DemoApp::update(float show, float dt) {
  frame_++;
  lastDt_ = dt;
  runSeconds_ += dt;

  // transport sync: the editor follows the show clock
  const float d = show - lastShow_;
  if (lastShow_ > -1e8f && d < -0.5f) {
    // loop / restart: re-arm everything
    editor_.seek(show);
    anims_.stopAll();
    fadeAlpha_ = 0;
    fadeGoal_ = 0;
    transitionT_ = -1;
  } else if (in_.directorPaused && *in_.directorPaused) {
    editor_.pause();
    if (std::fabs(editor_.time - show) > 0.01f) catchUpSeek(show);
  } else {
    editor_.play();
    editor_.update(dt);
    if (std::fabs(editor_.time - show) > 0.2f) catchUpSeek(show);
  }
  lastShow_ = show;

  // fired timeline events -> commands (each event stamps its own time so
  // markers/fades land on their cue points even when dispatched by a seek).
  // Iterate by index: a dispatched `jump` command seeks and can fire more
  // events into fired_ mid-loop (a range-for would invalidate its iterator).
  if (!editor_.fired().empty()) {
    for (size_t i = 0; i < editor_.fired().size(); ++i)
      dispatchCmds(editor_.fired()[i].cmds, editor_.fired()[i].time);
    editor_.consumeFired();
  }

  // camera rig (procedural baseline) ...
  applyCamera(show);

  // ... then explicit keyframed animations win over the rig: camera anims
  // (pos/fov/...) authored in the script override the procedural pose
  anims_.update(dt);
  applySamples();

  // scene graph world matrices
  scene_.update();

  // fades / transitions
  updateFade(dt);

  // audio uniforms for data-driven effects
  audioUniforms_.bass = in_.audio->react.bass.load();
  audioUniforms_.mid = in_.audio->react.mid.load();
  audioUniforms_.treble = in_.audio->react.treble.load();
  audioUniforms_.high = audioUniforms_.treble;  // react.high = the treble band
  audioUniforms_.kick = in_.audio->react.kick.load();
  audioUniforms_.volume = in_.audio->react.energy.load();
  audioUniforms_.beat = in_.timeline->s.beatPulse;
  audioUniforms_.bar = in_.timeline->s.barPulse;

  // live reload (deferred to frame start - never mid-render)
  watcher_.poll();
  const auto& changed = watcher_.changed();
  if (!changed.empty()) {
    pollLiveReload(changed);
  } else {
    // No watcher-reported change, but a cached shader's source may have been
    // deleted/recreated without the watcher noticing between polls. The
    // manager stats its own dependencies every frame (its 1s retry throttle
    // prevents spam), so poll it anyway - this is what lets a
    // deleted-then-restored shader come back live without a restart.
    shaders_.pollHotReload(changed);
  }
  if (reloadQueued_) {
    reloadQueued_ = false;
    reloadScript();
  }
}

void DemoApp::seek(float t) {
  catchUpSeek(t);
  lastShow_ = -1e9f;  // force the next update to re-arm
}

void DemoApp::catchUpSeek(float t) {
  const float from = editor_.time;
  editor_.seek(t);
  const float jump = t - from;
  if (jump > 0.05f) {
    // forward scrub: fire every event the clock jumped over so the scene /
    // effect at the target is the one the show would have reached by playing
    // there (a plain seek re-arms lastFire_ to t and silently skips them all
    // - the viewport kept showing the pre-scrub scene, or went dark when a
    // crossed event hid it).
    editor_.fireWindow(from, t);
    // advance the floor so a backward drag right after never re-fires the
    // window we just crossed (each in-scene cue fires at most once per visit)
    catchUpFloor_ = std::max(catchUpFloor_, t);
  } else if (jump < -0.05f) {
    // backward scrub: events cannot be un-fired, so re-establish the section
    // containing t (its setup: camera / show / fade / ...) and fire its
    // in-scene events after its start. The section-start `show` event itself
    // is not fired - activateScene() is its direct equivalent and avoids
    // re-arming markers/loads from earlier sections. The floor guard makes a
    // DRAG re-fire each section's cues at most once: without it every drag
    // frame would re-dispatch e.g. a `transition fade`, resetting the fade
    // and sticking the screen at black until the drag stops.
    const SceneSection* sec = sectionContaining(t);
    if (sec) {
      // the entry signal is the ACTUAL current scene, not a bookkeeping
      // member: a forward scrub that crossed into another section already
      // activated it, so a later backward re-entry must re-establish it (a
      // cached "last section" would go stale and skip the re-activation,
      // leaving the wrong scene up while the clock sits in the old section)
      if (sec->name != activeScene_) {
        activateScene(sec->name);
        catchUpFloor_ = sec->start;
        editor_.fireWindow(sec->start, t);
      } else if (t > catchUpFloor_) {
        editor_.fireWindow(catchUpFloor_, t);
      }
      catchUpFloor_ = std::max(catchUpFloor_, t);
    } else {
      editor_.fireWindow(0.0f, t);  // t before every section: replay from 0
    }
  }
}

const SceneSection* DemoApp::sectionContaining(float t) const {
  const SceneSection* best = nullptr;
  for (const auto& s : sections_) {
    if (t >= s.start && t < s.end) return &s;
    if (s.start <= t) best = &s;  // t past the last end -> the last section
  }
  return best;
}

void DemoApp::toggleLoop() { editor_.toggleLoop(); }

void DemoApp::jumpSection(int offset) {
  if (sections_.empty()) return;
  const float show = in_.showClock ? *in_.showClock : editor_.time;
  int idx = 0;
  for (size_t i = 0; i < sections_.size(); i++) {
    if (show >= sections_[i].start) idx = (int)i;
    else break;
  }
  idx = std::max(0, std::min((int)sections_.size() - 1, idx + offset));
  seek(sections_[(size_t)idx].start);
}

// ---------------------------------------------------------------------------
// camera
// ---------------------------------------------------------------------------
void DemoApp::applyCamera(float show) {
  auto it = rigs_.find(activeCamera_);
  const CameraRig* rig = it != rigs_.end() ? it->second.get() : rigs_["default"].get();
  float local = 0;
  for (const auto& s : sections_) {
    if (show >= s.start && show < s.end) { local = show - s.start; break; }
  }
  rig->apply(*in_.camera, show, local);
}

// ---------------------------------------------------------------------------
// command dispatch
// ---------------------------------------------------------------------------
void DemoApp::dispatchCmds(const std::vector<Cmd>& cmds, float at) {
  for (const auto& c : cmds) dispatch(c, at);
}

void DemoApp::dispatch(const Cmd& cmd, float at) {
  const std::string& n = cmd.name;
  if (n == "show") {
    if (cmd.args.empty()) { Log::warn("SCRIPT", "show needs a name"); return; }
    const std::string tgt = cmd.args[0].asStr();
    if (script_.scene(tgt)) activateScene(tgt);
    else showEffect(tgt, cmd.opts);
  } else if (n == "hide") {
    if (!cmd.args.empty()) hideEffect(cmd.args[0].asStr());
  } else if (n == "load") {
    if (cmd.args.size() >= 2 && cmd.args[0].asStr() == "shadertoy") {
      Value p = Value::object();
      p.set("file") = Value(cmd.args[1].asStr());
      if (cmd.args.size() >= 3) p.set("tex") = cmd.args[2];
      showEffect("shadertoy:" + cmd.args[1].asStr(), p);
    } else if (cmd.args.size() >= 2 && cmd.args[0].asStr() == "plugin") {
      loadEffectPlugin(cmd.args[1].asStr());
    } else if (cmd.args.size() >= 2 && cmd.args[0].asStr() == "material") {
      loadMaterial(cmd.args[1].asStr());
    } else if (cmd.args.size() >= 2 && cmd.args[0].asStr() == "model") {
      loadModelFile(cmd.args[1].asStr());
    } else if (!cmd.args.empty() && effectFactory().has(cmd.args[0].asStr())) {
      showEffect(cmd.args[0].asStr(), cmd.opts);
    } else {
      Log::warn("SCRIPT", "load: unknown target '" +
                              (cmd.args.empty() ? std::string() : cmd.args[0].asStr()) + "'");
    }
  } else if (n == "shader") {
    if (!cmd.args.empty()) {
      Value p = Value::object();
      p.set("frag") = cmd.args[0];
      p.set("handoff") = Value(cmd.b("handoff", false));
      showEffect("quad:" + cmd.args[0].asStr(), p);
    }
  } else if (n == "camera") {
    setCamera(cmd);
  } else if (n == "play") {
    Log::info("SCRIPT", "play: " + (cmd.args.empty() ? std::string("music") : cmd.args[0].asStr()));
  } else if (n == "fade") {
    const std::string dir = cmd.args.empty() ? "in" : cmd.args[0].asStr();
    const float dur = cmd.args.size() >= 2 ? cmd.args[1].asFloat(1.0f) : 1.0f;
    fadeGoal_ = dir == "out" ? 1 : 0;
    fadeSpeed_ = dur > 0 ? 1.0f / dur : 0;
  } else if (n == "transition") {
    const std::string type = cmd.args.empty() ? "fade" : cmd.args[0].asStr();
    const float dur = cmd.args.size() >= 2 ? cmd.args[1].asFloat(1.0f) : 1.0f;
    if (type == "fade") {
      transitionT_ = 0;
      transitionDur_ = dur;
      fadeGoal_ = 1;
      fadeSpeed_ = 1.0f / std::max(dur * 0.5f, 0.05f);
    } else if (type == "bloom") {
      if (in_.postfx) in_.postfx->fx.bloom = 3.0f;
      Log::info("POST", "transition bloom (music-reactive chain)");
    } else {
      Log::warn("SCRIPT", "unknown transition '" + type + "' (fade | bloom)");
    }
  } else if (n == "post") {
    if (cmd.args.size() >= 2 && cmd.args[0].asStr() == "preset") {
      loadPreset(cmd.args[1].asStr());
    } else if (!cmd.args.empty() && cmd.args[0].asStr() == "legacy") {
      usingPostStack_ = false;
      postPreset_ = "";
      Log::info("POST", "legacy music-reactive post chain");
    } else if (in_.postfx) {
      if (!cmd.opts.get("bloom").isNull()) in_.postfx->fx.bloom = cmd.opts.get("bloom").asFloat();
      if (!cmd.opts.get("glitch").isNull()) in_.postfx->fx.glitch = cmd.opts.get("glitch").asFloat();
      if (!cmd.opts.get("exposure").isNull()) in_.postfx->fx.exposure = cmd.opts.get("exposure").asFloat();
    }
  } else if (n == "anim") {
    cmdAnim(cmd);
  } else if (n == "marker") {
    // markers land on their EVENT time (at >= 0), never on the dispatch
    // moment - a seek catch-up dispatches events from the past, and without
    // this the cue would be stamped at the scrub target instead of its cue
    // point (and duplicated on every replay).
    if (!cmd.args.empty())
      editor_.addMarker(cmd.args[0].asStr(), at >= 0 ? at : editor_.time);
  } else if (n == "speed") {
    setSpeed(cmd.args.empty() ? 1.0f : cmd.args[0].asFloat(1.0f));
  } else if (n == "loop") {
    toggleLoop();
  } else if (n == "jump") {
    if (!cmd.args.empty()) seek(cmd.args[0].asFloat(0));
  } else if (n == "mesh" || n == "sprite" || n == "text" || n == "light" ||
             n == "particles" || n == "empty" || n == "postnode" || n == "quadnode") {
    cmdSceneNode(cmd);
  } else {
    Log::warn("SCRIPT", "unknown command '" + n + "'");
  }
}

void DemoApp::setCamera(const Cmd& cmd) {
  if (cmd.args.empty()) return;
  const std::string name = cmd.args[0].asStr();
  if (!cmd.opts.get("rig").isNull() || (cmd.opts.isObj() && cmd.opts.size() > 0)) {
    rigs_[name] = CameraRig::fromCmd(cmd);
  } else if (!rigs_.count(name)) {
    auto r = std::make_unique<CameraRig>();
    r->type = "static";
    rigs_[name] = std::move(r);
  }
  activeCamera_ = name;
}

// ---------------------------------------------------------------------------
// effects
// ---------------------------------------------------------------------------
Effect* DemoApp::instance(const std::string& name, const Value& params) {
  auto it = effects_.find(name);
  if (it != effects_.end()) return it->second.get();
  std::string factoryName = name;
  Value p = params;
  if (name.rfind("shadertoy:", 0) == 0) {
    factoryName = "shadertoy";
    p.set("file") = Value(name.substr(10));
  } else if (name.rfind("quad:", 0) == 0) {
    factoryName = "quad";
    if (p.get("frag").isNull()) p.set("frag") = Value(name.substr(5));
  }
  std::unique_ptr<Effect> e = effectFactory().create(factoryName, p);
  try {
    e->init(ctx_);
  } catch (const std::exception& ex) {
    Log::error("FX", "effect '" + name + "' init failed: " + ex.what());
    return nullptr;
  }
  Effect* out = e.get();

  // one-time wiring for the logo / greetings wordmark
  if (name == "logo" && !logoWired_) {
    logoWired_ = true;
    if (loadSplashLogo(logoWordmark_)) {
      if (auto* sfx = dynamic_cast<SceneFX*>(e.get())) {
        sfx->setTex("uLogoTex", logoWordmark_.tex, 10);
        sfx->useFont(true);
      }
    }
  }
  if (name == "greetings" && logoWordmark_.tex) {
    if (auto* gfx = dynamic_cast<GreetingsFX*>(e.get())) {
      gfx->setWordmark(logoWordmark_.tex, (float)logoWordmark_.w / (float)logoWordmark_.h);
    }
  }

  effects_[name] = std::move(e);
  return out;
}

Effect* DemoApp::findEffect(const std::string& name) {
  auto it = effects_.find(name);
  return it == effects_.end() ? nullptr : it->second.get();
}

void DemoApp::showEffect(const std::string& name, const Value& opts) {
  Effect* e = instance(name, opts);
  if (!e) return;
  if (std::find(activeEffects_.begin(), activeEffects_.end(), name) != activeEffects_.end()) return;

  // apply per-show params
  if (auto* sfx = dynamic_cast<SceneFX*>(e)) {
    sfx->mode = opts.get("mode").asFloat(sfx->mode);
    if (!opts.get("renderScale").isNull()) sfx->renderScale = opts.get("renderScale").asFloat();
  }
  if (auto* tfx = dynamic_cast<TunnelFX*>(e)) tfx->mode = opts.get("mode").asFloat(tfx->mode);
  if (auto* pfx = dynamic_cast<ParticleStormFX*>(e)) pfx->mode = opts.get("mode").asFloat(pfx->mode);
  if (auto* gfx = dynamic_cast<GreetingsFX*>(e)) gfx->mode = opts.get("mode").asFloat(gfx->mode);

  // fullscreen scenes replace each other unless `keep` is set
  if (!opts.get("keep").asBool(false)) {
    for (const auto& old : activeEffects_) hideEffect(old);
  }
  activeEffects_.push_back(name);
  Log::info("FX", "show " + name);
}

void DemoApp::hideEffect(const std::string& name) {
  auto it = std::find(activeEffects_.begin(), activeEffects_.end(), name);
  if (it != activeEffects_.end()) activeEffects_.erase(it);
}

void DemoApp::activateScene(const std::string& name) {
  const SceneBundle* b = script_.scene(name);
  if (!b) return;
  activeScene_ = name;
  dispatchCmds(b->setup);  // camera / show / fade / post ...
  Log::info("SCENE", "activate " + name);
}

// ---------------------------------------------------------------------------
// scene graph building
// ---------------------------------------------------------------------------
void DemoApp::cmdSceneNode(const Cmd& cmd) {
  std::string nodeName = cmd.args.empty() ? cmd.name : cmd.args[0].asStr();
  if (cmd.name == "mesh" && nodeName.rfind('.') != std::string::npos) {
    nodeName = std::filesystem::path(nodeName).stem().string();  // file.obj -> file
  }
  NodeType type = NodeType::Empty;
  if (cmd.name == "mesh") type = NodeType::Mesh;
  else if (cmd.name == "sprite") type = NodeType::Sprite;
  else if (cmd.name == "text") type = NodeType::Text;
  else if (cmd.name == "light") type = NodeType::Light;
  else if (cmd.name == "particles") type = NodeType::Particles;
  else if (cmd.name == "quadnode") type = NodeType::Quad;
  else if (cmd.name == "postnode") type = NodeType::Post;

  SceneNode* node = scene_.find(nodeName);
  if (!node) node = scene_.addNode(nodeName, type);
  else node->type = type;

  float f[4];
  if (cmd.opts.get("pos").toFloats(f, 3) == 3) node->setPos({f[0], f[1], f[2]});
  if (cmd.opts.get("euler").toFloats(f, 3) == 3) node->setEuler({f[0], f[1], f[2]});
  if (cmd.opts.get("scale").toFloats(f, 3) == 3) node->setScale({f[0], f[1], f[2]});
  else if (!cmd.opts.get("scale").isNull()) node->setScale(cmd.opts.get("scale").asFloat(1));
  node->visible = cmd.opts.get("visible").asBool(true);
  if (!cmd.opts.get("layer").isNull()) node->layer = cmd.opts.get("layer").asInt(0);
  if (!cmd.opts.get("tag").isNull()) {
    const std::string t = cmd.opts.get("tag").asStr();
    if (std::find(node->tags.begin(), node->tags.end(), t) == node->tags.end()) node->tags.push_back(t);
  }

  switch (type) {
    case NodeType::Mesh: {
      MeshData md;
      md.model = cmd.opts.get("model").asStr(cmd.args.empty() ? "" : cmd.args[0].asStr());
      md.scale = cmd.opts.get("meshScale").asFloat(1);
      md.material = cmd.opts.get("material").asStr();  // material NAME (bound at draw)
      node->payload = md;
      break;
    }
    case NodeType::Sprite: {
      SpriteData sd;
      sd.tex = cmd.opts.get("tex").asStr(cmd.args.empty() ? "" : cmd.args[0].asStr());
      if (cmd.opts.get("color").toFloats(f, 4) == 4) sd.color = {f[0], f[1], f[2], f[3]};
      sd.opacity = cmd.opts.get("opacity").asFloat(1);
      if (cmd.opts.get("size").toFloats(f, 3) == 3) sd.size = {f[0], f[1], f[2]};
      node->payload = sd;
      break;
    }
    case NodeType::Text: {
      TextData td;
      td.text = cmd.opts.get("text").asStr(cmd.args.empty() ? "" : cmd.args[0].asStr());
      td.sizePx = cmd.opts.get("size").asInt(24);
      td.style = cmd.opts.get("style").asStr("neon");
      if (cmd.opts.get("color").toFloats(f, 4) == 4) td.color = {f[0], f[1], f[2], f[3]};
      td.opacity = cmd.opts.get("opacity").asFloat(1);
      node->payload = td;
      break;
    }
    case NodeType::Light: {
      LightData ld;
      ld.type = cmd.opts.get("type").asStr("point");
      if (cmd.opts.get("color").toFloats(f, 3) == 3) ld.color = {f[0], f[1], f[2]};
      ld.intensity = cmd.opts.get("intensity").asFloat(1);
      ld.range = cmd.opts.get("range").asFloat(10);
      ld.angle = cmd.opts.get("angle").asFloat(45);
      node->payload = ld;
      break;
    }
    case NodeType::Particles: {
      ParticleData pd;
      pd.frag = cmd.opts.get("frag").asStr("particles.frag");
      pd.vert = cmd.opts.get("vert").asStr("particles.vert");
      pd.count = cmd.opts.get("count").asInt(5000);
      node->payload = pd;
      break;
    }
    case NodeType::Quad: {
      QuadData qd;
      qd.frag = cmd.opts.get("frag").asStr("passthrough.frag");
      qd.handoff = cmd.opts.get("handoff").asBool(false);
      node->payload = qd;
      break;
    }
    case NodeType::Post: {
      PostData pd;
      pd.preset = cmd.opts.get("preset").asStr();
      node->payload = pd;
      break;
    }
    default: break;
  }
  node->dirty = true;
}

void DemoApp::cmdAnim(const Cmd& cmd) {
  if (cmd.args.size() < 2) {
    Log::warn("SCRIPT", "anim needs: name target.property [interp]");
    return;
  }
  const std::string animName = cmd.args[0].asStr();
  const std::string targetProp = cmd.args[1].asStr();
  const size_t dot = targetProp.find('.');
  const std::string target = dot != std::string::npos ? targetProp.substr(0, dot) : targetProp;
  const std::string property = dot != std::string::npos ? targetProp.substr(dot + 1) : targetProp;
  const Interp interp = cmd.args.size() >= 3 ? parseInterp(cmd.args[2].asStr()) : Interp::Linear;

  auto anim = anims_.get(animName);
  if (!anim) {
    anim = std::make_shared<Animation>();
    anim->name = animName;
    anims_.add(anim);
  }
  AnimChannel ch;
  ch.target = target;
  ch.property = property;
  for (const auto& k : cmd.keys) {
    AnimKey key;
    key.t = k.t;
    key.v = k.v;
    key.interp = k.interp.empty() ? interp : parseInterp(k.interp);
    ch.keys.push_back(std::move(key));
  }
  anim->channels.erase(std::remove_if(anim->channels.begin(), anim->channels.end(),
                                      [&](const AnimChannel& c) {
                                        return c.target == target && c.property == property;
                                      }),
                       anim->channels.end());
  anim->channels.push_back(std::move(ch));
  anim->duration = 0;
  for (const auto& c : anim->channels)
    for (const auto& k : c.keys) anim->duration = std::max(anim->duration, k.t);
  anim->loop = cmd.b("loop", false);
  anims_.play(animName);
}

// ---------------------------------------------------------------------------
// animation application
// ---------------------------------------------------------------------------
void DemoApp::applySamples() {
  for (const auto& s : anims_.samples()) applySample(s);
  anims_.consumeSamples();
}

void DemoApp::applySample(const AnimSample& s) {
  float f[8];
  const int n = s.value.toFloats(f, 8);

  if (s.target == "camera") {
    if (s.property == "fov" && n >= 1) in_.camera->fov = f[0] * 3.14159265f / 180.0f;
    else if (s.property == "pos" && n >= 3) in_.camera->pos = {f[0], f[1], f[2]};
    else if (s.property == "target" && n >= 3) in_.camera->lookAt({f[0], f[1], f[2]});
    else if (s.property == "handheld" && n >= 1) in_.camera->handheld = f[0];
    else if (s.property == "dofFocus" && n >= 1) in_.camera->dofFocus = f[0];
    else if (s.property == "dofAperture" && n >= 1) in_.camera->dofAperture = f[0];
    return;
  }
  if (s.target == "post") {
    if (!in_.postfx) return;
    if (s.property == "bloom" && n >= 1) in_.postfx->fx.bloom = f[0];
    else if (s.property == "glitch" && n >= 1) in_.postfx->fx.glitch = f[0];
    else if (s.property == "exposure" && n >= 1) in_.postfx->fx.exposure = f[0];
    else if (s.property == "heat" && n >= 1) in_.postfx->fx.heat = f[0];
    return;
  }
  if (s.target.rfind("effect:", 0) == 0) {
    const std::string name = s.target.substr(7);
    const auto eit = effects_.find(name);
    Effect* e = eit != effects_.end() ? eit->second.get() : nullptr;
    if (!e) return;
    if (s.property == "mode" && n >= 1) {
      if (auto* sfx = dynamic_cast<SceneFX*>(e)) sfx->mode = f[0];
      else if (auto* tfx = dynamic_cast<TunnelFX*>(e)) tfx->mode = f[0];
      else if (auto* pfx = dynamic_cast<ParticleStormFX*>(e)) pfx->mode = f[0];
      else if (auto* gfx = dynamic_cast<GreetingsFX*>(e)) gfx->mode = f[0];
    } else if (s.property.rfind("uniform:", 0) == 0 && n >= 1) {
      const std::string uname = s.property.substr(8);
      if (auto* sfx = dynamic_cast<SceneFX*>(e)) sfx->extraUniforms[uname] = f[0];
      else if (auto* st = dynamic_cast<ShadertoyFX*>(e)) st->uniforms[uname] = f[0];
    }
    return;
  }
  if (s.target.rfind("node:", 0) == 0) {
    SceneNode* node = scene_.find(s.target.substr(5));
    if (!node) return;
    if (s.property == "pos" && n >= 3) node->setPos({f[0], f[1], f[2]});
    else if (s.property == "euler" && n >= 3) node->setEuler({f[0], f[1], f[2]});
    else if (s.property == "scale" && n >= 1)
      node->setScale(n >= 3 ? V3{f[0], f[1], f[2]} : V3{f[0], f[0], f[0]});
    else if (s.property == "visible" && n >= 1) node->visible = f[0] > 0.5f;
    else if (s.property == "opacity" && n >= 1) {
      if (auto* sd = node->asSprite()) sd->opacity = f[0];
      if (auto* td = node->asText()) td->opacity = f[0];
    } else if (s.property == "color" && n >= 3) {
      if (auto* sd = node->asSprite()) sd->color = {f[0], f[1], f[2], sd->color[3]};
      if (auto* td = node->asText()) td->color = {f[0], f[1], f[2], td->color[3]};
    } else if (s.property == "text" && s.value.isStr()) {
      if (auto* td = node->asText()) td->text = s.value.asStr();
    }
    return;
  }
  if (s.target.rfind("light:", 0) == 0) {
    SceneNode* node = scene_.find(s.target.substr(6));
    LightData* ld = node ? node->asLight() : nullptr;
    if (!ld) return;
    if (s.property == "intensity" && n >= 1) ld->intensity = f[0];
    else if (s.property == "color" && n >= 3) ld->color = {f[0], f[1], f[2]};
    return;
  }
}

// ---------------------------------------------------------------------------
// post presets + fade
// ---------------------------------------------------------------------------
void DemoApp::loadPreset(const std::string& name) {
  const std::string path = "data/post/" + name + ".json";
  if (!post_->loadPresetFile(path, shaders_)) {
    Log::error("POST", "preset load failed: " + path);
    return;
  }
  usingPostStack_ = true;
  postPreset_ = name;
  post_->resize(in_.r->resW, in_.r->resH);
  std::string chain;
  for (const auto& c : post_->chain()) chain += c + " ";
  Log::info("POST", "preset '" + name + "' active: " + chain);
}

void DemoApp::updateFade(float dt) {
  if (transitionT_ >= 0) {
    transitionT_ += dt;
    const float half = std::max(transitionDur_ * 0.5f, 0.05f);
    if (transitionT_ >= transitionDur_) {
      transitionT_ = -1;
      fadeAlpha_ = 0;
      fadeGoal_ = 0;
      fadeSpeed_ = 0;
    } else if (transitionT_ >= half) {
      fadeGoal_ = 0;
      fadeSpeed_ = 1.0f / half;
    }
  }
  if (fadeSpeed_ > 0) {
    if (fadeGoal_ > fadeAlpha_) fadeAlpha_ = std::min(fadeGoal_, fadeAlpha_ + fadeSpeed_ * dt);
    else fadeAlpha_ = std::max(fadeGoal_, fadeAlpha_ - fadeSpeed_ * dt);
    if (std::fabs(fadeAlpha_ - fadeGoal_) < 0.001f) fadeSpeed_ = 0;
  }
}

// ---------------------------------------------------------------------------
// live reload
// ---------------------------------------------------------------------------
void DemoApp::pollLiveReload(const std::vector<std::string>& changed) {
  bool shaderChanged = false, dataChanged = false, assetChanged = false;
  for (const auto& f : changed) {
    const std::string ext = std::filesystem::path(f).extension().string();
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") shaderChanged = true;
    else if (ext == ".nsd") dataChanged = true;
    else if (ext == ".json") { dataChanged = true; assetChanged = true; }
    else assetChanged = true;
  }
  if (shaderChanged) {
    const int n = shaders_.pollHotReload(changed);
    if (n > 0) Log::info("RELOAD", "recompiled " + std::to_string(n) + " shader program(s)");
    for (const auto& f : changed) {
      const std::filesystem::path p(f);
      if (p.extension().string() == ".glsl" && p.parent_path().filename() == "shadertoy") {
        for (auto& kv : effects_) {
          if (auto* st = dynamic_cast<ShadertoyFX*>(kv.second.get())) {
            try {
              st->reload(ctx_);
            } catch (const std::exception& ex) {
              Log::error("RELOAD", "shadertoy reload failed (kept previous): " + std::string(ex.what()));
            }
          }
        }
      }
    }
  }
  if (dataChanged) {
    for (const auto& f : changed) {
      const std::filesystem::path p(f);
      if (p.extension().string() == ".nsd") reloadQueued_ = true;
      else assets_.markDirty(f);
    }
  }
  if (assetChanged) {
    for (const auto& f : changed) {
      if (std::filesystem::path(f).extension().string() == ".nsd") continue;
      assets_.markDirty(f);
    }
  }
  if (assets_.reloadDirty() > 0) Log::info("RELOAD", "assets reloaded");
}

void DemoApp::reloadScript() {
  try {
    ScriptEngine fresh;
    const std::string text = readScriptText(in_.scriptPath);
    if (text.empty() || !fresh.loadText(text, in_.scriptPath)) {
      Log::error("RELOAD", "script reload failed (kept previous)");
      return;
    }
    script_ = std::move(fresh);
    editor_.clear();
    script_.build(editor_);
    buildSections();
    activeScene_.clear();
    activeEffects_.clear();
    catchUpFloor_ = -1e9f;
    editor_.seek(0);
    editor_.play();
    Log::info("RELOAD", "script reloaded: " + in_.scriptPath);
  } catch (const std::exception& e) {
    Log::error("RELOAD", std::string("script reload failed: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// --check-hotreload smoke mode
// ---------------------------------------------------------------------------
namespace {
inline double hrNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool hrHasLine(const std::vector<std::string>& lines, const char* needle) {
  for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

int DemoApp::runHotReloadCheck() {
  const std::string dir = AppAssets::shaderDir();
  const std::string file = dir + "/" + kHotReloadCheckFrag;

  // capture every [SHADER] line so the keep-previous + recompile log pair is
  // verified programmatically, not by eyeballing stderr. Single-threaded by
  // design: this mode runs before audio.start(), so no other thread logs
  // while the sink is armed.
  std::vector<std::string> logLines;
  Log::setSink([&](const std::string& line) { logLines.push_back(line); });

  auto fail = [&](const std::string& why) -> int {
    Log::setSink({});
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::fprintf(stderr, "[HOTRELOAD] FAIL: %s\n", why.c_str());
    return 1;
  };
  auto pass = [&]() -> int {
    Log::setSink({});
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::fprintf(stderr, "[HOTRELOAD] PASS: broken edit kept the previous program; fixing "
                         "the file recompiled it live\n");
    return 0;
  };

  // warm the program (the shell wrote the valid file before init, so the
  // watcher's baseline scan already covers it - the full chain is exercised:
  // watcher -> pollLiveReload -> ShaderManager -> log)
  std::shared_ptr<ProgramState> st;
  try {
    st = shaders_.get("fullscreen.vert", kHotReloadCheckFrag).state;
  } catch (const std::exception& e) {
    return fail("warm compile failed: " + std::string(e.what()));
  }
  const unsigned id0 = st->id;
  if (!id0) return fail("warm compile produced no program");

  // run frames (update() alone drives the reload pipeline; no rendering)
  // until the predicate holds or the deadline passes
  auto pump = [&](const std::function<bool()>& done, double maxSec) {
    float show = 0;
    double last = hrNow();
    const double t0 = last;
    while (hrNow() - t0 < maxSec) {
      const double n = hrNow();
      const float dt = (float)(n - last);
      last = n;
      show += dt;
      update(show, dt);
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  };

  // write a shader source and confirm it landed (an antivirus/file-lock race
  // can silently no-op the write; the check must fail loudly, not misread the
  // pipeline)
  auto writeChecked = [&](const std::string& content, const char* marker) -> bool {
    {
      std::ofstream f(file);
      f << content;
    }
    std::ifstream back(file);
    std::string s((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
    return s.find(marker) != std::string::npos;
  };

  // 1. BREAK the shader on disk: the reload must fail loudly, keep the
  //    previous program (same GL id - compileProgram only swaps on success),
  //    and log the keeping-previous-version warn
  if (!writeChecked(kHotReloadFragBroken, "BADTOKEN"))
    return fail("could not write the broken shader (" + file + ")");
  // isDirty() compares the file's mtime against the warm compile's clock
  // stamp; coarse filesystem mtime granularity can swallow a fast rewrite
  // (same tick -> mt > mtime_ is false and the reload never fires). Re-touch
  // until the FS reports a strictly newer mtime - the exact comparison the
  // manager makes - so the reload attempt is guaranteed to happen.
  {
    const double touchDeadline = hrNow() + 3.0;
    bool newer = false;
    while (hrNow() < touchDeadline) {
      std::error_code mec;
      const auto mt = std::filesystem::last_write_time(file, mec);
      if (!mec && fileTimeToEpochSec(mt) > st->mtime_) {
        newer = true;
        break;
      }
      {
        std::ofstream f(file);
        f << kHotReloadFragBroken;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!newer)
      return fail("filesystem never reported a newer mtime for the broken write (" + file + ")");
  }
  const bool sawBreak = pump([&] { return hrHasLine(logLines, "keeping previous version"); }, 6.0);
  if (!sawBreak) return fail("no 'hot-reload failed ... keeping previous version' warn after breaking the shader");
  if (st->ok) return fail("program reported ok after a failed reload");
  if (st->id != id0) return fail("previous program was NOT kept live after the failed reload (id changed)");

  // 2. FIX the shader: the retry must recompile and log the hot-reloaded info
  if (!writeChecked(kHotReloadFragValid, "fragColor = vec4(vUV"))
    return fail("could not write the fixed shader (" + file + ")");
  const bool sawFix = pump([&] { return hrHasLine(logLines, "hot-reloaded"); }, 6.0);
  if (!sawFix) return fail("no 'hot-reloaded' line after fixing the shader (retry did not fire)");
  if (!st->ok) return fail("program still not ok after the fix");
  if (!st->error.empty()) return fail("compile error not cleared after the fix");

  // 3. the log pair, in order: the keep-previous warn BEFORE the recompile
  size_t warnPos = std::string::npos, reloadPos = std::string::npos;
  for (size_t i = 0; i < logLines.size(); i++) {
    const std::string& l = logLines[i];
    if (warnPos == std::string::npos && l.find("keeping previous version") != std::string::npos)
      warnPos = i;
    if (l.find("hot-reloaded") != std::string::npos && reloadPos == std::string::npos) reloadPos = i;
  }
  if (warnPos == std::string::npos || reloadPos == std::string::npos || warnPos >= reloadPos)
    return fail("log pair out of order or missing (warn must precede recompile)");

  return pass();
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
FrameTarget& DemoApp::scaledTarget(const std::string& key, float scale) {
  const int w = std::max(2, (int)(in_.r->resW * scale));
  const int h = std::max(2, (int)(in_.r->resH * scale));
  auto it = fxTargets_.find(key);
  if (it != fxTargets_.end() && it->second.w == w && it->second.h == h) return it->second;
  const TextureOpts opts{::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false};
  fxTargets_[key] = FrameTarget::color(w, h, ::gl::RGBA16F, ::gl::RGBA, ::gl::HALF_FLOAT, opts);
  return fxTargets_[key];
}

void DemoApp::pushAudioUniforms(Effect* e) {
  if (auto* sfx = dynamic_cast<SceneFX*>(e)) {
    sfx->extraUniforms["uBass"] = audioUniforms_.bass;
    sfx->extraUniforms["uMid"] = audioUniforms_.mid;
    sfx->extraUniforms["uTreble"] = audioUniforms_.treble;
    sfx->extraUniforms["uHigh"] = audioUniforms_.high;
    sfx->extraUniforms["uKick"] = audioUniforms_.kick;
    sfx->extraUniforms["uBeat"] = audioUniforms_.beat;
    sfx->extraUniforms["uBar"] = audioUniforms_.bar;
    sfx->extraUniforms["uVolume"] = audioUniforms_.volume;
  } else if (auto* st = dynamic_cast<ShadertoyFX*>(e)) {
    st->uniforms["uBass"] = audioUniforms_.bass;
    st->uniforms["uMid"] = audioUniforms_.mid;
    st->uniforms["uTreble"] = audioUniforms_.treble;
    st->uniforms["uHigh"] = audioUniforms_.high;
    st->uniforms["uKick"] = audioUniforms_.kick;
    st->uniforms["uBeat"] = audioUniforms_.beat;
    st->uniforms["uBar"] = audioUniforms_.bar;
    st->uniforms["uVolume"] = audioUniforms_.volume;
  }
}

void DemoApp::renderEffects() {
  ::glDisable(::gl::BLEND);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);
  for (const auto& name : activeEffects_) {
    Effect* e = effects_[name].get();
    if (!e) continue;
    pushAudioUniforms(e);
    float scale = 1.0f;
    if (auto* sfx = dynamic_cast<SceneFX*>(e)) scale = sfx->renderScale;
    if (scale < 1.0f) {
      FrameTarget& t = scaledTarget(name, scale);
      t.bind();
      ::glClearColor(0, 0, 0, 1);
      ::glClear(::gl::COLOR_BUFFER_BIT);
      ctx_.hdr = &t;
      e->render(ctx_);
      // composite the reduced scene back into the full-res HDR target that
      // feeds the post stack + screen (the shader drew at uSceneRes; without
      // this blit the scaled texture would never be displayed)
      hdr_.bind();
      ::glDisable(::gl::BLEND);
      ::glActiveTexture(::gl::TEXTURE0);
      ::glBindTexture(::gl::TEXTURE_2D, t.colorTex());
      copyProg_.use();
      copyProg_.set1i("uTex", 0);
      copyProg_.set2f("uRes", (float)hdr_.w, (float)hdr_.h);
      in_.r->fsTriangle.draw(3);
    } else {
      ctx_.hdr = &hdr_;
      e->render(ctx_);
    }
  }
  ctx_.hdr = &hdr_;
}

void DemoApp::loadMaterial(const std::string& name) {
  const std::string path = "data/materials/" + name + ".json";
  const std::string text = runtimeFS().readText(path);
  if (text.empty()) {
    Log::warn("ASSET", "material not found: " + path);
    return;
  }
  try {
    const Value v = Json::parseText(text);
    materials_[name] = Material::fromJson(v);
    Log::info("ASSET", "material '" + name + "' loaded");
  } catch (const std::exception& e) {
    Log::error("ASSET", "material load failed: " + path + " (" + e.what() + ")");
  }
}

void DemoApp::editorLoadTexture(const std::string& name) {
  if (name.empty()) return;
  // warm the asset-manager cache for the sprite pipeline (data/textures)
  void* h = assets_.acquire("data/textures/" + name, "texture");
  if (!h) {
    Log::error("EDITOR", "texture load failed: " + name);
    return;
  }
  Log::info("EDITOR", "texture loaded: " + name);
}

void DemoApp::editorOpenScript(const std::string& path) {
  if (!std::filesystem::exists(path)) {
    Log::error("EDITOR", "script not found: " + path);
    return;
  }
  // parse first: a broken script must not strand in_.scriptPath on the failing
  // file (every later F2/watcher reload would chase it while the show keeps
  // running the old script). reloadScript() parses again - cheap for a user
  // action, and it reuses the exact F2/watch reload path on success.
  ScriptEngine fresh;
  if (!fresh.load(path)) {
    Log::error("EDITOR", "script rejected (kept current): " + path);
    return;
  }
  in_.scriptPath = path;
  reloadScript();
}

void DemoApp::loadModelFile(const std::string& file) {
  auto it = modelCache_.find(file);
  if (it != modelCache_.end()) return;
  auto m = std::make_shared<Model>();
  const std::string path = "data/models/" + file;
  if (!ObjImporter::load(path, *m)) {
    modelCache_[file] = nullptr;
    return;
  }
  modelCache_[file] = m;
}

void DemoApp::renderMeshLayer() {
  const auto meshNodes = scene_.nodesOf(NodeType::Mesh, true);
  if (meshNodes.empty()) return;

  // ensure the model cache is warm
  for (SceneNode* mn : meshNodes) {
    MeshData* md = mn->asMesh();
    if (md && !md->model.empty()) loadModelFile(md->model);
  }

  LightUniforms lights;
  for (SceneNode* ln : scene_.nodesOf(NodeType::Light, true)) {
    LightData* ld = ln->asLight();
    const V3 wpos = {ln->world[12], ln->world[13], ln->world[14]};
    const V3 fwd = {-ln->world[8], -ln->world[9], -ln->world[10]};
    if (ld->type == "directional") lights.addDir(fwd, ld->color, ld->intensity);
    else if (ld->type == "spot") lights.addSpot(wpos, fwd, ld->color, ld->intensity, ld->range, ld->angle);
    else lights.addPoint(wpos, ld->color, ld->intensity, ld->range);
  }

  meshFbo_.bind();
  ::glClearColor(0, 0, 0, 0);
  ::glClear(::gl::COLOR_BUFFER_BIT | ::gl::DEPTH_BUFFER_BIT);
  ::glEnable(::gl::DEPTH_TEST);
  ::glDisable(::gl::CULL_FACE);
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);

  for (SceneNode* mn : meshNodes) {
    MeshData* md = mn->asMesh();
    if (!md || md->model.empty()) continue;
    auto it = modelCache_.find(md->model);
    if (it == modelCache_.end() || !it->second) continue;
    // world * meshScale
    Mat4 out{};
    const Mat4& m = mn->world;
    const float s = md->scale;
    for (int c = 0; c < 4; c++) {
      out[c * 4 + 0] = m[c * 4 + 0] * s;
      out[c * 4 + 1] = m[c * 4 + 1] * s;
      out[c * 4 + 2] = m[c * 4 + 2] * s;
      out[c * 4 + 3] = m[c * 4 + 3];
    }
    // per-node material override (by name; falls back to the model's own)
    const Material* overrideMat = nullptr;
    if (!md->material.empty()) {
      auto mit = materials_.find(md->material);
      if (mit != materials_.end()) overrideMat = &mit->second;
    }
    modelRenderer_.drawModel(*it->second, out.data(), lights, 0.18f, overrideMat);
  }
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::BLEND);

  // composite the 3D layer over the HDR scene (alpha = material opacity)
  hdr_.bind();
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
  ::glActiveTexture(::gl::TEXTURE0);
  ::glBindTexture(::gl::TEXTURE_2D, meshFbo_.colorTex());
  copyProg_.use();
  copyProg_.set1i("uTex", 0);
  copyProg_.set2f("uRes", (float)hdr_.w, (float)hdr_.h);
  in_.r->fsTriangle.draw(3);
  ::glDisable(::gl::BLEND);
}

void DemoApp::renderSpritesAndText() {
  const auto sprites = scene_.nodesOf(NodeType::Sprite, true);
  if (!sprites.empty()) {
    if (!spriteQuadBuilt_) {
      const float p[12] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
      const float u[12] = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
      spriteQuad_.setBuffer(0, p, 12, 2, ::gl::STATIC_DRAW);
      spriteQuad_.setBuffer(1, u, 12, 2, ::gl::STATIC_DRAW);
      spriteQuadBuilt_ = true;
    }
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    ::glDisable(::gl::DEPTH_TEST);
    spriteProg_.use();
    for (SceneNode* n : sprites) {
      SpriteData* sd = n->asSprite();
      Texture* tex = nullptr;
      if (!sd->tex.empty()) {
        // acquire once per texture path (refcounted by the asset manager);
        // cached so the per-frame draw never bumps the refcount
        auto it = spriteTex_.find(sd->tex);
        if (it != spriteTex_.end()) {
          tex = it->second;
        } else {
          void* h = assets_.acquire("data/textures/" + sd->tex, "texture");
          tex = static_cast<Texture*>(h);
          spriteTex_[sd->tex] = tex;
        }
      }
      // world * size
      Mat4 out{};
      const Mat4& m = n->world;
      const float sx = sd->size[0], sy = sd->size[1], sz = sd->size[2];
      for (int c = 0; c < 4; c++) {
        out[c * 4 + 0] = m[c * 4 + 0] * sx;
        out[c * 4 + 1] = m[c * 4 + 1] * sy;
        out[c * 4 + 2] = m[c * 4 + 2] * sz;
        out[c * 4 + 3] = m[c * 4 + 3];
      }
      spriteProg_.setMat4("uModel", out.data());
      if (tex && tex->tex) {
        ::glActiveTexture(::gl::TEXTURE0);
        ::glBindTexture(::gl::TEXTURE_2D, tex->tex);
      }
      spriteProg_.set1i("uTex", 0);
      spriteProg_.set4f("uColor", sd->color[0], sd->color[1], sd->color[2], sd->color[3]);
      spriteProg_.set1f("uOpacity", sd->opacity);
      spriteQuad_.draw(6);
    }
    ::glDisable(::gl::BLEND);
  }

  const auto texts = scene_.nodesOf(NodeType::Text, true);
  for (SceneNode* n : texts) {
    TextData* td = n->asText();
    if (td->text.empty() || td->opacity <= 0.01f) continue;
    CineStyle style = CineStyle::Neon;
    if (td->style == "terminal") style = CineStyle::Terminal;
    else if (td->style == "holo") style = CineStyle::Holo;
    else if (td->style == "glitch") style = CineStyle::Glitch;
    else if (td->style == "scan") style = CineStyle::Scan;
    else if (td->style == "dissolve") style = CineStyle::Dissolve;
    else if (td->style == "chrome") style = CineStyle::Chrome;
    else if (td->style == "outline") style = CineStyle::Outline;
    cineText_->line(ctx_, td->text, n->pos[1], td->sizePx, style, td->opacity, 0, 0.5f, 1.0f, 0,
                    n->pos[0] * 0.5f);
  }
}

void DemoApp::render() {
  if (!initDone_) return;
  Renderer& r = *in_.r;
  ctx_.time = in_.showClock ? *in_.showClock : 0;
  ctx_.dt = lastDt_;

  in_.shared->write(&ctx_);
  in_.shared->commit();

  if (in_.postfx) in_.postfx->fx = {};  // effects set their own per frame

  hdr_.bind();
  ::glClearColor(0, 0, 0, 1);
  ::glClear(::gl::COLOR_BUFFER_BIT);

  renderEffects();

  renderMeshLayer();

  renderSpritesAndText();

  // --- post -----------------------------------------------------------------
  const unsigned sceneTex = hdr_.colorTex();
  const float motion = 0.12f;
  if (usingPostStack_) {
    PostCtx pctx;
    pctx.r = &r;   // fsTriangle VAO for the PostStack pass draws
    pctx.time = ctx_.time;
    pctx.dt = ctx_.dt;
    pctx.camera = in_.camera;
    pctx.motion = motion;
    if (in_.postfx) {
      pctx.bloomMul = in_.postfx->fx.bloom;
      pctx.glitch = in_.postfx->fx.glitch;
      pctx.exposure = in_.postfx->fx.exposure;
      pctx.heat = in_.postfx->fx.heat;
      pctx.kick = in_.postfx->fx.kick;
    }
    const unsigned finalTex = post_->process(sceneTex, pctx, r.resW, r.resH);
    post_->present(finalTex, r.viewW, r.viewH);
  } else {
    in_.postfx->process(sceneTex, *in_.camera, motion, ctx_.time);
  }

  // fade overlay (drawn over the presented frame)
  if (fadeAlpha_ > 0.003f) {
    static ProgramRef fadeProg;
    if (!fadeProg.state) fadeProg = shaders_.get("fullscreen.vert", "fade.frag");
    ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
    ::glViewport(0, 0, r.viewW, r.viewH);
    ::glEnable(::gl::BLEND);
    ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
    fadeProg.use();
    fadeProg.set1f("uAlpha", fadeAlpha_);
    r.fsTriangle.draw(3);
    ::glDisable(::gl::BLEND);
  }
}

void DemoApp::drawToast(const std::string& text, float alpha) {
  if (alpha <= 0.01f || text.empty() || !cineText_) return;
  Renderer& r = *in_.r;
  // draw on top of the presented frame, exactly like the fade overlay
  ::glBindFramebuffer(::gl::FRAMEBUFFER, 0);
  ::glViewport(0, 0, r.viewW, r.viewH);
  ::glEnable(::gl::BLEND);
  ::glBlendFunc(::gl::SRC_ALPHA, ::gl::ONE_MINUS_SRC_ALPHA);
  ::glDisable(::gl::DEPTH_TEST);
  ::glDisable(::gl::SCISSOR_TEST);  // a post pass could leave a rect active
  cineText_->line(ctx_, text, -0.82f, 30, CineStyle::Terminal, alpha, 0, 0.5f,
                  1.0f, 0, -0.62f);
  ::glDisable(::gl::BLEND);
}

void DemoApp::resize(int w, int h) {
  (void)w; (void)h;
  if (!initDone_) return;
  rebuildTargets();
  for (auto& kv : effects_) kv.second->resize(ctx_);
  if (post_) post_->resize(in_.r->resW, in_.r->resH);
}

// ---------------------------------------------------------------------------
// perf dump (--perf-json) - one sample per timed effect + the post stack
// ---------------------------------------------------------------------------
/** one timed effect's live perf snapshot (kind + context + current EMA).
 *  timed = the effect classes that own a PerfTimer; everything else
 *  (intro/tunnel/greetings/cinetext...) is untimed and reports kind "".
 *
 *  THIS IS THE SINGLE timed-effect classifier. writePerfJson() and the
 *  per-second perfCsvTick() both consume it, so a future timed effect type
 *  (e.g. a plugin FX that adds a PerfTimer) is registered here and nowhere
 *  else - add it to the three branches and both dumps follow. */
namespace {
struct PerfView {
  const char* kind = "";  // "shadertoy" / "scene" / "particles" / ""
  std::string context;    // "renderScale 0.50", "count 5000", ...
  double emaMs = -1.0;    // current EMA, -1 when not a timed effect, 0 when
                          // timed but no sample has been collected yet
  double rawMs = -1.0;    // most recent UNSMOOTHED sample (--perf-raw rows),
                          // same -1/0 semantics as emaMs
};
PerfView perfViewOf(Effect* e) {
  PerfView v;
  if (!e) return v;
  if (auto* st = dynamic_cast<ShadertoyFX*>(e)) {
    v.kind = "shadertoy";
    v.context = "renderScale " + fmtMs((double)st->renderScale());
    v.emaMs = st->emaMs();
    v.rawMs = st->lastRawMs();
  } else if (auto* sfx = dynamic_cast<SceneFX*>(e)) {
    v.kind = "scene";
    v.context = "renderScale " + fmtMs((double)sfx->renderScale);
    v.emaMs = sfx->emaMs();
    v.rawMs = sfx->lastRawMs();
  } else if (auto* psx = dynamic_cast<ParticleStormFX*>(e)) {
    v.kind = "particles";
    v.context = "count " + std::to_string(psx->count());
    v.emaMs = psx->emaMs();
    v.rawMs = psx->lastRawMs();
  }
  return v;
}
}  // namespace

void DemoApp::writePerfJson(const std::string& path) {
  Value root = Value::object();
  root.set("title") = Value(script_.script().title);
  root.set("run_seconds") = Value((double)runSeconds_);

  Value effects = Value::array();
  for (const auto& kv : effects_) {
    Effect* e = kv.second.get();
    if (!e) continue;
    const PerfView v = perfViewOf(e);
    if (v.kind[0] == '\0') continue;  // not a timed effect (intro/tunnel/...)
    PerfSample ps = e->perfSample();
    Value entry = Value::object();
    entry.set("kind") = Value(v.kind);
    entry.set("name") = Value(kv.first);
    // shadertoy extras (file + actual buffer size) don't fit the shared view
    if (auto* st = dynamic_cast<ShadertoyFX*>(e)) {
      entry.set("file") = Value(st->file());
      entry.set("buffers") =
          Value(std::to_string(st->bufferWidth()) + "x" + std::to_string(st->bufferHeight()));
    }
    // the shared context string is "<name> <value>" (renderScale 0.50,
    // count 5000) - emit it back as the same JSON fields as before
    if (!v.context.empty()) {
      const size_t sp = v.context.find(' ');
      if (sp != std::string::npos) {
        entry.set(v.context.substr(0, sp)) = Value((double)std::atof(v.context.c_str() + sp + 1));
      }
    }
    entry.set("medianMs") = Value(ps.medianMs);
    entry.set("meanMs") = Value(ps.meanMs);
    entry.set("minMs") = Value(ps.minMs);
    entry.set("maxMs") = Value(ps.maxMs);
    entry.set("frames") = Value((double)ps.frames);
    effects.push(entry);
    Log::info("PERF", std::string(v.kind) + " '" + kv.first + "': " + fmtMs(ps.medianMs) +
               " ms/frame median over " + std::to_string(ps.frames) + " frames");
  }

  // the post stack always runs - its sample is the whole pipeline
  if (in_.postfx) {
    Value entry = Value::object();
    entry.set("kind") = Value("post");
    entry.set("name") = Value("post");
    entry.set("bloom") = Value((double)in_.postfx->bloomLevels());
    const PerfSample ps = in_.postfx->perfSample();
    entry.set("medianMs") = Value(ps.medianMs);
    entry.set("meanMs") = Value(ps.meanMs);
    entry.set("minMs") = Value(ps.minMs);
    entry.set("maxMs") = Value(ps.maxMs);
    entry.set("frames") = Value((double)ps.frames);
    effects.push(entry);
    Log::info("PERF", "post: " + fmtMs(ps.medianMs) + " ms/frame median over " +
               std::to_string(ps.frames) + " frames (bloom " +
               std::to_string(in_.postfx->bloomLevels()) + ")");
  }

  root.set("effects") = effects;
  Json::writeFile(path, root, 2);
  Log::info("PERF", "wrote " + path + " (" + std::to_string(effects.size()) + " samples)");
}

// ---------------------------------------------------------------------------
// perf csv (--perf-csv) - per-second GPU-time rows for the timed effects
// ---------------------------------------------------------------------------
void DemoApp::beginPerfCsv(const std::string& path) {
  perfCsvPath_ = path;
  perfCsvAccum_ = 0;
  perfCsvSeconds_ = 0;
}

void DemoApp::perfCsvTick(float dt) {
  if (perfCsvPath_.empty()) return;
  perfCsvAccum_ += dt;
  if (perfCsvAccum_ < 1.0f) return;

  // one row per elapsed second: current EMA of every ACTIVE timed effect
  // (the things actually rendering this second) + the post stack, which
  // always runs. Inactive effects stay absent from their seconds' rows -
  // that IS the plot of what the show cost over time.
  //
  // Row labels are bucket STARTS: the first row (samples from t=0..1s) is
  // 0.00, the next 1.00, ... so a row's ms is the EMA during [t, t+1).
  std::string row = fmtMs((double)perfCsvSeconds_) + "\t";
  int n = 0;
  for (const auto& name : activeEffects_) {
    Effect* e = effects_[name].get();
    const PerfView v = perfViewOf(e);
    if (v.kind[0] == '\0' || v.emaMs <= 0.0) continue;  // untimed / no sample yet
    row += v.kind;
    row += "\t" + name;
    row += "\t" + v.context;
    row += "\t" + fmtMs(v.emaMs) + "\n";
    n++;
  }
  if (in_.postfx) {
    const double ms = in_.postfx->emaMs();
    if (ms > 0.0) {
      row += "post\tpost\tbloom " + std::to_string(in_.postfx->bloomLevels()) + "\t" +
             fmtMs(ms) + "\n";
      n++;
    }
  }

  if (n == 0) {
    // no timed effect has a sample yet - wait for one instead of filling the
    // file with empty rows (the first second is usually still warming up)
    perfCsvAccum_ = 0;
    return;
  }

  // lazy open: write the header the first time a row is actually written
  if (!perfCsv_.is_open()) {
    perfCsv_.open(perfCsvPath_);
    if (!perfCsv_) {
      Log::error("PERF", "could not open " + perfCsvPath_);
      perfCsvPath_.clear();
      return;
    }
    perfCsv_ << "t\tkind\tname\tcontext\tms\n";
    Log::info("PERF", "per-second CSV: " + perfCsvPath_);
  }
  perfCsv_ << row;
  perfCsv_.flush();  // tail -f friendly while the show runs

  perfCsvSeconds_ += 1.0f;
  // carry the fractional part: a frame with dt > 1s (a stall, debugger pause,
  // heavy load) must not silently drop the time it covered - fmod keeps the
  // plot continuous instead of losing up to (dt - 1)s per stall
  perfCsvAccum_ = std::fmod((double)perfCsvAccum_, 1.0);
}

void DemoApp::finishPerfCsv() {
  if (!perfCsv_.is_open()) {
    if (!perfCsvPath_.empty()) {
      Log::info("PERF", "per-second CSV: no samples recorded (" + perfCsvPath_ + ")");
      perfCsvPath_.clear();
    }
    return;
  }
  perfCsv_.close();
  Log::info("PERF", "closed " + perfCsvPath_ + " (" + fmtMs((double)perfCsvSeconds_) +
             "s of per-second samples)");
  perfCsvPath_.clear();
}

// ---------------------------------------------------------------------------
// --perf-raw: one row per distinct NEW raw sample. The per-second CSV
// smooths the spikes away (that is the point of the EMA); this dump keeps
// every raw per-frame value the GPU finishes, so a renderScale change is
// visible as the sharpest step and stalls as real peaks. Each effect
// contributes a row only when its raw value CHANGED since the last written
// row - a PerfTimer collects at most one sample per frame, and none on
// frames where the GPU is still working on the ring's oldest pair, so this
// collapses to roughly one row per collected sample, never one per frame.
// A repeated value (e.g. a flat 0.29 ms post stack) is a horizontal line
// either way - the y-axis carries all the information the plot needs.
//
// The t column is the wall second the row was flushed - the sample itself
// landed 1-2 frames earlier, when the GPU finished the queried work. The
// plotter reads these rows with --mode=raw.
// ---------------------------------------------------------------------------
void DemoApp::beginPerfRaw(const std::string& path) {
  perfRawPath_ = path;
  perfRawWall_ = 0.0;
  perfRawFlushAccum_ = 0;
  perfRawLast_.clear();
}

void DemoApp::perfRawTick(float dt) {
  if (perfRawPath_.empty()) return;
  perfRawWall_ += dt;
  const std::string t = fmtMs(perfRawWall_);

  std::string rows;
  for (const auto& name : activeEffects_) {
    Effect* e = effects_[name].get();
    const PerfView v = perfViewOf(e);
    if (v.kind[0] == '\0' || v.rawMs <= 0.0) continue;  // untimed / never sampled
    if (v.rawMs == perfRawLast_[name]) continue;        // value unchanged since last row
    perfRawLast_[name] = v.rawMs;
    rows += t + "\t" + v.kind + "\t" + name + "\t" + v.context + "\t" +
            fmtMs(v.rawMs) + "\n";
  }
  if (in_.postfx) {
    const double raw = in_.postfx->lastRawMs();
    if (raw > 0.0 && raw != perfRawLast_["post"]) {
      perfRawLast_["post"] = raw;
      rows += t + "\tpost\tpost\tbloom " + std::to_string(in_.postfx->bloomLevels()) +
              "\t" + fmtMs(raw) + "\n";
    }
  }
  if (rows.empty()) return;

  // lazy open: write the header the first time a sample is actually written
  if (!perfRaw_.is_open()) {
    perfRaw_.open(perfRawPath_);
    if (!perfRaw_) {
      Log::error("PERF", "could not open " + perfRawPath_);
      perfRawPath_.clear();
      return;
    }
    perfRaw_ << "t\tkind\tname\tcontext\tms\n";
    Log::info("PERF", "per-sample raw CSV: " + perfRawPath_);
  }
  perfRaw_ << rows;

  // flush once per second, not per row - at up to ~hundreds of rows/sec a
  // per-row flush would make the dump itself a bottleneck (the per-second
  // CSV flushes every row because it is tiny by comparison)
  perfRawFlushAccum_ += dt;
  if (perfRawFlushAccum_ >= 1.0f) {
    perfRaw_.flush();
    perfRawFlushAccum_ = 0;
  }
}

void DemoApp::finishPerfRaw() {
  if (!perfRaw_.is_open()) {
    if (!perfRawPath_.empty()) {
      Log::info("PERF", "per-sample raw CSV: no samples recorded (" + perfRawPath_ + ")");
      perfRawPath_.clear();
    }
    return;
  }
  perfRaw_.flush();
  perfRaw_.close();
  Log::info("PERF", "closed " + perfRawPath_ + " (raw per-frame samples)");
  perfRawPath_.clear();
}

}  // namespace ns
