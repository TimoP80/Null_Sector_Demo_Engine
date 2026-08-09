// ---------------------------------------------------------------------------
// DemoApp - the data-driven director. Replaces the hardcoded main-loop scene
// switch: it owns the framework subsystems (scripting, timeline, scene graph,
// animation, camera rigs, asset manager, shader manager, post stack, file
// watcher) and drives the whole show from data/demo.nsd + the JSON files it
// references. Changing the demo never requires touching this class or the
// engine - the script, scenes, timelines, post presets and shaders are all
// external and hot-reloadable.
//
// Frame flow:
//   update(show, dt)   timeline events -> commands; animations; camera rig;
//                      live reload (shaders/scripts/textures/models)
//   render()           active effects -> HDR -> 3D mesh layer -> sprites/text
//                      -> post (legacy music-reactive chain or a preset stack)
//                      -> present + fade overlay
// ---------------------------------------------------------------------------
#pragma once

#include "engine/audio.hpp"
#include "engine/camera.hpp"
#include "engine/framebuffer.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include "engine/ubo.hpp"
#include "effects/base_fwd.hpp"
#include "app/appassets.hpp"
#include "app/model.hpp"
#include "app/poststack.hpp"
#include "app/shadermanager.hpp"
#include "framework/anim/animation.hpp"
#include "framework/camera/camerarig.hpp"
#include "framework/core/value.hpp"
#include "framework/resources/assetmanager.hpp"
#include "framework/resources/filewatcher.hpp"
#include "framework/scene/scenegraph.hpp"
#include "framework/script/scriptengine.hpp"
#include "framework/timeline/timelineeditor.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns {

class CineText;
class PostFX;

class DemoApp {
public:
  // wiring handed in by the shell (main.cpp); all must outlive the app
  struct Input {
    Renderer* r = nullptr;
    Assets* assets = nullptr;
    SharedBlock* shared = nullptr;
    PostFX* postfx = nullptr;
    Camera* camera = nullptr;
    AudioEngine* audio = nullptr;
    Timeline* timeline = nullptr;
    float* showClock = nullptr;     // &director.show
    bool* directorPaused = nullptr;
    void (*setDirectorScale)(float) = nullptr;
    std::string scriptPath = "data/demo.nsd";
    std::string pluginDir;          // optional data/plugins directory
  };

  /** audio uniforms exposed to data-driven shaders. Naming follows the
   *  request's react.* signals: react.kick -> uKick, react.bass -> uBass,
   *  react.mid -> uMid, react.high -> uHigh (= the treble band, an alias),
   *  react.energy -> uVolume, react.beat -> uBeat, react.bar -> uBar.
   *  kick/bass/mid/treble/energy come from the audio analyser (frame-rate
   *  independent envelope followers), beat/bar from the timeline's beat
   *  clock (deterministic during scrubbing). */
  struct AudioUniforms {
    float bass = 0, mid = 0, treble = 0, kick = 0, volume = 0, beat = 0, bar = 0, high = 0;
  };

  DemoApp() = default;
  ~DemoApp();
  DemoApp(const DemoApp&) = delete;
  DemoApp& operator=(const DemoApp&) = delete;

  /** load the script + build everything; throws on fatal errors */
  void init(const Input& in);

  void resize(int w, int h);

  /** advance the show: events, animations, camera, live reload */
  void update(float show, float dt);

  /** render one frame into the HDR target + present */
  void render();
  /** draw a one-line on-screen toast (bottom-left, in the demo's caption
   *  style) on top of the presented frame - the plain demo's track-switch
   *  readout. alpha fades it in/out; empty text or alpha<=0 is a no-op. */
  void drawToast(const std::string& text, float alpha);

  // --- transport (from the shell's key handling) ----------------------------
  void togglePause() { if (in_.directorPaused) *in_.directorPaused = !*in_.directorPaused; }
  void setSpeed(float s) { if (in_.setDirectorScale) in_.setDirectorScale(s); }
  void seek(float t);
  void toggleLoop();
  void jumpSection(int offset);

  // --- effect library (the editor's hierarchy) ------------------------------
  /** show an effect instance now (creates it if needed); safe from the UI */
  void editorShowEffect(const std::string& name) { showEffect(name, Value::null()); }
  /** force-recompile every cached program whose source changed on disk (the
   *  editor's shader scratch calls this after writing a file, so the live
   *  preview updates on the next frame without waiting for the watcher
   *  cadence; a broken edit keeps the previous program + logs, like reload) */
  void reloadShaders() { shaders_.reloadAll(); }
  /** hide an effect instance now; safe from the UI */
  void editorHideEffect(const std::string& name) { hideEffect(name); }
  /** load a material by name (data/materials/NAME.json); safe from the UI */
  void editorLoadMaterial(const std::string& name) { loadMaterial(name); }
  /** apply a post preset by name (data/post/NAME.json); safe from the UI */
  void editorLoadPreset(const std::string& name) { loadPreset(name); }
  /** warm/load a texture by name (data/textures/NAME.ext); safe from the UI */
  void editorLoadTexture(const std::string& name);
  /** warm the model cache for data/models/NAME.obj; safe from the UI */
  void editorLoadModel(const std::string& file) { loadModelFile(file); }
  /** switch the running demo script to PATH and reload it; safe from the UI */
  void editorOpenScript(const std::string& path);
  /** the live-reload watcher. The editor re-baselines it after writing the
   *  demo script itself (Add Scene) so the watcher doesn't double-reload the
   *  same edit through the polling path. */
  FileWatcher& editableWatcher() { return watcher_; }

  // --- introspection (debug console / editor) --------------------------------
  const ScriptEngine& script() const { return script_; }
  const TimelineEditor& editor() const { return editor_; }
  const SceneGraph& scene() const { return scene_; }
  const AnimationSystem& anims() const { return anims_; }
  const AssetManager& assets() const { return assets_; }
  std::vector<std::string> activeEffects() const { return activeEffects_; }
  std::string activeScene() const { return activeScene_; }
  std::string activeCamera() const { return activeCamera_; }
  bool usingPostStack() const { return usingPostStack_; }
  const std::vector<SceneSection>& sections() const { return sections_; }
  const std::string& scriptPath() const { return in_.scriptPath; }

  // --- editor access (mutation hooks - the scene graph / timeline are read
  //     per frame by the director, so edits here go live immediately) ---------
  SceneGraph& editableScene() { return scene_; }
  TimelineEditor& editableEditor() { return editor_; }
  /** every instanced effect (active or not), keyed by instance name */
  const std::map<std::string, std::unique_ptr<Effect>>& allEffects() const { return effects_; }
  Effect* findEffect(const std::string& name);

  /** reload the demo script (live reload; safe at frame start) */
  void reloadScript();

  /** write one GPU-time sample per effect (shadertoy/scene/particles) plus
   *  the post stack to a JSON file - the --perf-json exit dump. Samples are
   *  the stable recorded-window stats (median/mean/min/max + frame count)
   *  accumulated over the run, so scripted A/B runs of a renderScale option
   *  can be diffed. Safe to call any time before the effects are destroyed
   *  (reads stats only, no GL). Logs one [PERF] line per effect. */
  void writePerfJson(const std::string& path);

  /** begin a per-second GPU-time CSV (--perf-csv). Lazily creates the file
   *  on the first tick that has a sample; safe when no timed effect is
   *  active yet. */
  void beginPerfCsv(const std::string& path);

  /** accumulate dt and append one row per elapsed second: the current EMA
   *  ms/frame of every ACTIVE timed effect (shadertoy/scene/particles) plus
   *  the post stack, so a renderScale change can be plotted over time.
   *  Reads stats only, no GL - safe from the main loop. */
  void perfCsvTick(float dt);

  /** flush + close the CSV and log a summary line (call at exit). */
  void finishPerfCsv();

  /** begin a per-sample raw GPU-time CSV (--perf-raw). Like the per-second
   *  CSV but one row per COLLECTED sample (every frame the GPU finishes),
   *  with the unsmoothed ms - so spikes the EMA hides show up. Lazily
   *  creates the file on the first tick that has a sample. */
  void beginPerfRaw(const std::string& path);

  /** each frame: write one row per effect that has a NEW raw sample since
   *  the last written row (dedup keeps the file at one row per collected
   *  sample, not one per frame). Reads stats only, no GL - safe from the
   *  main loop. Flushes the stream once per second (tail -f friendly). */
  void perfRawTick(float dt);

  /** flush + close the raw CSV and log a summary line (call at exit). */
  void finishPerfRaw();

  /** --check-hotreload smoke mode: mutate a temp shader on disk and verify,
   *  from the manager state AND the captured log pair, that a broken edit
   *  keeps the previous program live and that fixing it recompiles.
   *  Returns 0 (pass) or 1 (fail); prints [HOTRELOAD] verdicts to stderr. */
  int runHotReloadCheck();

private:
  // subsystems
  ShaderManager shaders_;
  AssetManager assets_;
  FileWatcher watcher_;
  ScriptEngine script_;
  TimelineEditor editor_;
  SceneGraph scene_;
  AnimationSystem anims_;
  std::unique_ptr<PostStack> post_;   // needs the Renderer at ctor - built in init()
  ModelRenderer modelRenderer_;
  CineText* cineText_ = nullptr;

  Input in_;
  EffectContext ctx_;
  bool initDone_ = false;

  // effects + rigs
  std::map<std::string, std::unique_ptr<Effect>> effects_;
  std::map<std::string, std::unique_ptr<CameraRig>> rigs_;
  std::vector<std::string> activeEffects_;
  std::string activeScene_;
  std::string activeCamera_ = "default";
  std::vector<SceneSection> sections_;

  // backward-scrub catch-up state: the highest in-scene time already
  // re-fired in the current section, so a drag re-fires each section's cues
  // at most once (the section-entry signal is activeScene_, see catchUpSeek).
  float catchUpFloor_ = -1e9f;

  // render targets
  FrameTarget hdr_;                 // the HDR scene (owned by the app now)
  FrameTarget meshFbo_;             // 3D layer (color + depth)
  std::map<std::string, FrameTarget> fxTargets_;  // per-effect scaled targets

  // programs owned by the app
  ProgramRef copyProg_;
  ProgramRef spriteProg_;
  Mesh spriteQuad_{::gl::TRIANGLES};
  bool spriteQuadBuilt_ = false;

  // fade / transition state
  float fadeAlpha_ = 0;
  float fadeGoal_ = 0;
  float fadeSpeed_ = 0;             // alpha units / sec (0 = instant)
  float transitionT_ = -1;
  float transitionDur_ = 1;

  // post
  bool usingPostStack_ = false;
  std::string postPreset_;

  // live reload + frame state
  bool reloadQueued_ = false;
  uint64_t frame_ = 0;
  float lastShow_ = -1e9f;
  float lastDt_ = 1.0f / 60.0f;
  float runSeconds_ = 0;   // wall-ish time accumulated in update() (perf dumps)

  // per-second GPU-time CSV (--perf-csv)
  std::string perfCsvPath_;
  std::ofstream perfCsv_;
  float perfCsvAccum_ = 0;   // dt since the last written second
  float perfCsvSeconds_ = 0; // time of the next row (wall-run seconds)

  // per-sample raw GPU-time CSV (--perf-raw)
  std::string perfRawPath_;
  std::ofstream perfRaw_;
  double perfRawWall_ = 0;         // wall seconds accumulated each frame
  float perfRawFlushAccum_ = 0;    // flush the stream once per second
  std::map<std::string, double> perfRawLast_;  // last written ms per name (dedup)

  // model + material caches
  std::map<std::string, std::shared_ptr<Model>> modelCache_;
  std::map<std::string, Material> materials_;

  // data-driven uniform feeds
  AudioUniforms audioUniforms_;
  Texture logoWordmark_;
  bool logoWired_ = false;

  // per-texture handles for sprite nodes (acquired once, never re-bumped)
  std::map<std::string, Texture*> spriteTex_;

  // --- internals ---------------------------------------------------------------
  void buildSections();
  void rebuildTargets();
  void applyCamera(float show);

  /** fire every event a seek/scrub jumped over so the show state at the
   *  target matches what playing there would have produced. Forward: fire
   *  (oldTime, t]. Backward: events cannot be un-fired, so re-activate the
   *  section containing t and fire its in-scene events after its start. */
  void catchUpSeek(float t);
  const SceneSection* sectionContaining(float t) const;
  void dispatchCmds(const std::vector<Cmd>& cmds, float at = -1.0f);
  void dispatch(const Cmd& cmd, float at = -1.0f);
  void showEffect(const std::string& name, const Value& opts);
  void hideEffect(const std::string& name);
  void activateScene(const std::string& name);
  void setCamera(const Cmd& cmd);
  void loadPreset(const std::string& name);
  void loadModelFile(const std::string& file);
  void loadMaterial(const std::string& name);

  // scene graph building
  void cmdSceneNode(const Cmd& cmd);
  void cmdAnim(const Cmd& cmd);

  // render helpers
  void renderEffects();
  void renderMeshLayer();
  void renderSpritesAndText();
  void applySamples();
  void applySample(const AnimSample& s);
  void updateFade(float dt);
  void pollLiveReload(const std::vector<std::string>& changed);
  void pushAudioUniforms(Effect* e);

  Effect* instance(const std::string& name, const Value& params = Value::null());
  FrameTarget& scaledTarget(const std::string& key, float scale);
};

}  // namespace ns
