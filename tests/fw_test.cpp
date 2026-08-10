// ---------------------------------------------------------------------------
// fw_test - unit tests for the GL-free framework core.
// Build:  cmake -S . -B build && cmake --build build --target ns_fw_tests
// Run:    build/ns_fw_tests
// ---------------------------------------------------------------------------
#include "framework/anim/animation.hpp"
#include "framework/camera/camerarig.hpp"
#include "framework/core/json.hpp"
#include "framework/core/log.hpp"
#include "framework/core/ffmpegpipe.hpp"
#include "framework/core/value.hpp"
#include "framework/resources/assetmanager.hpp"
#include "framework/resources/filewatcher.hpp"
#include "framework/scene/scenegraph.hpp"
#include "framework/script/scriptengine.hpp"
#include "framework/script/scriptparser.hpp"
#include "framework/script/nsdwriter.hpp"
#include "editor/document.hpp"
#include "framework/timeline/timelineeditor.hpp"
#include "framework/vfs/directoryfs.hpp"
#include "framework/vfs/nspack.hpp"
#include "framework/vfs/packagefs.hpp"
#include "framework/vfs/vfs.hpp"
#include "app/shadertoyparse.hpp"
#include "engine/gputimer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace ns {

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (cond) { g_passed++; }                                                   \
    else {                                                                      \
      g_failed++;                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);        \
    }                                                                           \
  } while (0)

#define CHECK_NEAR(a, b, eps, msg) CHECK(std::fabs((a) - (b)) < (eps), msg)

// ---------------------------------------------------------------------------
static void testJson() {
  const std::string src =
      R"({
        "title": "NULL SECTOR DEMO ENGINE",
        "bpm": 216.0,
        "flags": [true, false, null],
        "nested": { "a": [1, 2, { "b": "x" }], "ok": true }
      })";
  const Value v = Json::parse(src);
  CHECK(v.isObj(), "json object");
  CHECK(v.get("title").asStr() == "NULL SECTOR DEMO ENGINE", "json string");
  CHECK_NEAR(v.get("bpm").asNum(), 216.0, 1e-9, "json number");
  CHECK(v.get("flags").size() == 3, "json array");
  CHECK(v.get("flags").atIndex(0).asBool(true) == true, "json bool");
  CHECK(v.get("flags").atIndex(1).asBool() == false, "json false");
  CHECK(v.get("flags").atIndex(2).isNull(), "json null");
  CHECK(v.at("nested.a.1").asNum() == 2.0, "json dot path");
  CHECK(v.at("nested.a.2.b").asStr() == "x", "json nested dot path");
  CHECK(v.at("missing.key").isNull(), "json missing path -> null");

  // round trip
  const std::string out = Json::serialize(v, 0);
  const Value v2 = Json::parse(out);
  CHECK(v2.get("title").asStr() == "NULL SECTOR DEMO ENGINE", "json round trip");
  CHECK(v2.at("nested.a.2.b").asStr() == "x", "json round trip nested");

  // errors
  bool threw = false;
  try { Json::parse("{ \"a\": "); } catch (const JsonError&) { threw = true; }
  CHECK(threw, "json throws on truncated input");

  // Value::toFloats
  float f[4];
  Value vec = Value::array();
  vec.push(Value(1.0)); vec.push(Value(2.0)); vec.push(Value(3.0));
  CHECK(vec.toFloats(f, 4) == 3, "toFloats count");
  CHECK(f[2] == 3.0f, "toFloats value");
  Value sc = Value(42.0);
  CHECK(sc.toFloats(f, 4) == 1 && f[0] == 42.0f, "toFloats scalar");
  Value str = Value("1,2,3");
  CHECK(str.toFloats(f, 4) == 3 && f[1] == 2.0f, "toFloats string");
}

// ---------------------------------------------------------------------------
static void testScriptParser() {
  // the exact example from the request
  const std::string src = R"(
demo "NULL SECTOR DEMO ENGINE" {
    bpm 216
}

scene Intro {
    bars 60  intensity 0.12  chapter 0
    camera IntroCam { rig drift; pos (0,0,2.4); fov (50,64); buildUp (49,58); amp 2.8; freq 0.19; handheld (0.05,0.4) }
    show intro
    play music
    fade in 2
}

at 0.0
{
    camera IntroCam
    show intro
    play music
    fade in
}

at 12.5
{
    load tunnel
}

at 18.0
{
    camera FlyThrough
}

at 25
{
    shader CRT
}

at 40
{
    transition Bloom
}
)";
  const Script s = ScriptParser::parse(src, "test");
  CHECK(s.bpm == 216.0f, "script bpm");
  CHECK(s.title == "NULL SECTOR DEMO ENGINE", "script title");
  CHECK(s.scenes.size() == 1, "one scene");
  CHECK(s.scenes[0].name == "Intro", "scene name");
  CHECK(s.scenes[0].bars == 60, "scene bars");
  CHECK_NEAR(s.scenes[0].intensity, 0.12f, 1e-6f, "scene intensity");
  CHECK(s.scenes[0].setup.size() == 4, "scene setup cmds");
  CHECK(s.scenes[0].setup[0].name == "camera", "scene setup camera cmd");
  CHECK(s.scenes[0].setup[0].s("rig") == "drift", "camera rig option");
  CHECK(s.main.size() == 5, "five at blocks");
  CHECK_NEAR(s.main[0].time, 0.0f, 1e-6f, "at 0.0");
  CHECK_NEAR(s.main[3].time, 25.0f, 1e-6f, "at 25");
  CHECK(s.main[3].cmds[0].name == "shader", "shader cmd");
  CHECK(s.main[3].cmds[0].args[0].asStr() == "CRT", "shader arg");
  CHECK(s.main[4].cmds[0].name == "transition", "transition cmd");
  CHECK(s.main[4].cmds[0].args[0].asStr() == "Bloom", "transition arg");

  // time forms
  Script t;
  const std::string src2 = R"(
bpm 120
at 1:05.5 { marker "one" }
at beat 128 { marker "two" }
at bar 32 { marker "three" }
at 66.667 s { marker "four" }
)";
  const Script s2 = ScriptParser::parse(src2, "times");
  CHECK_NEAR(s2.main[0].time, 65.5f, 1e-4f, "mm:ss time");
  CHECK_NEAR(s2.main[1].time, 128.0f * 0.5f, 1e-4f, "beat time @120bpm");
  CHECK_NEAR(s2.main[2].time, 32.0f * 2.0f, 1e-4f, "bar time @120bpm");
  CHECK_NEAR(s2.main[3].time, 66.667f, 1e-3f, "seconds + s unit");

  // comments + vectors
  const std::string src3 =
      "# header comment\n"
      "// line comment\n"
      "at 1 { camera C { pos (1, -2, 3.5); target (0,0,0) } } /* block */\n";
  const Script s3 = ScriptParser::parse(src3, "comments");
  CHECK(s3.main.size() == 1, "one at after comments");
  CHECK(s3.main[0].cmds[0].name == "camera", "camera cmd");
  float f[3];
  s3.main[0].cmds[0].opts.get("pos").toFloats(f, 3);
  CHECK(f[0] == 1.0f && f[1] == -2.0f && f[2] == 3.5f, "vector with negatives");

  // image nodes accept both the explicit node+file form and the convenient
  // one-argument form; transition options stay in the command AST for the
  // runtime director to apply at scene activation.
  {
    const Script image = ScriptParser::parse(R"(
scene Gallery {
    image poster poster.png { pos (0,0,0); size (2,1,1); transition fade; duration 0.75 }
    at 2 { image logo.png { transition zoom  } }
}
)", "image");
    CHECK(image.scenes[0].setup.size() == 1, "image commands parse in scene setup");
    CHECK(image.scenes[0].setup[0].name == "image", "image command name");
    CHECK(image.scenes[0].setup[0].args[0].asStr() == "poster", "image node name arg");
    CHECK(image.scenes[0].setup[0].args[1].asStr() == "poster.png", "image texture arg");
    CHECK(image.scenes[0].setup[0].s("transition") == "fade", "image transition option");
    CHECK_NEAR(image.scenes[0].setup[0].f("duration"), 0.75f, 1e-6f, "image transition duration");
    CHECK(image.scenes[0].blocks[0].cmds[0].args[0].asStr() == "logo.png", "one-argument image form");
  }

  // errors
  bool threw = false;
  try { ScriptParser::parse("at { }", "bad"); } catch (const ScriptError&) { threw = true; }
  CHECK(threw, "parser throws on missing time");
}

// ---------------------------------------------------------------------------
// testScriptDiagnostics - the parser's error quality: every failure carries
// filename:line:column and a "did you mean" suggestion when a known name is
// close (a typo like `blom 0.8` says "did you mean 'bloom'?" instead of a
// bare "unknown"). Also locks in the format-compatible forms: `tempo` as a
// bpm alias, `beat(65)` / `bar(32)` parenthesized units, glued plural units
// (`2bars`, `32beats`), the `visible` scene option, scene-level post
// shorthand (`bloom 0.8` -> `post { bloom 0.8 }`) and `effect X` as an
// alias for `show X`.
// ---------------------------------------------------------------------------
static void testScriptDiagnostics() {
  // unknown demo option -> suggestion + file:line:col format
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("demo \"X\" { duretion 40 }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw, "unknown demo option throws");
    CHECK(msg.find("prod.nsd:1:") != std::string::npos, "demo error has file:line");
    CHECK(msg.find("did you mean 'duration'") != std::string::npos, "demo option suggestion");
  }
  // unknown scene property -> suggestion (the request's exact example)
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("scene A { blom 0.8 }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw, "unknown scene property throws");
    CHECK(msg.find("did you mean 'bloom'") != std::string::npos, "scene property suggestion");
  }
  // unknown command -> suggestion
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("at 0 { shw intro }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw, "unknown command throws");
    CHECK(msg.find("did you mean 'show'") != std::string::npos, "command suggestion");
  }
  // unknown interpolator -> suggestion
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("at 0 { anim x camera.fov smoth { 0 50; 4 60 } }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw, "unknown interpolator throws");
    CHECK(msg.find("did you mean 'smooth'") != std::string::npos, "interp suggestion");
  }
  // unknown rig -> suggestion (a camera command lives inside a scene/at block)
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("at 0 { camera C { rig drfit } }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw, "unknown rig throws");
    CHECK(msg.find("did you mean 'drift'") != std::string::npos, "rig suggestion");
  }
  // syntax error carries line + column
  {
    bool threw = false;
    std::string msg;
    try { ScriptParser::parse("at { }", "prod.nsd"); }
    catch (const ScriptError& e) { threw = true; msg = e.what(); }
    CHECK(threw && msg.find("prod.nsd:1:4") != std::string::npos, "syntax error has file:line:col");
  }

  // format-compatible forms parse into the right AST
  {
    const std::string src = R"(
demo "EXAMPLE" { tempo 140; duration 2bars }
scene A { bars 4  visible false
    bloom 0.8
    effect intro
}
at beat(65) { marker "m1" }
at 32beats { marker "m2" }
)";
    const Script s = ScriptParser::parse(src, "forms");
    CHECK_NEAR(s.bpm, 140.0f, 1e-6f, "tempo alias sets bpm");
    // bar-unit fields resolve at the declared tempo regardless of field order:
    // `duration` written ABOVE `tempo` must still resolve at 140, not the
    // 216 default (the header is pre-scanned for bpm before parsing values)
    {
      const Script s2 = ScriptParser::parse(
          "demo \"X\" { duration 2bars; tempo 140 }", "order");
      CHECK_NEAR(s2.duration, 2.0f * 4.0f * (60.0f / 140.0f), 1e-3f,
                 "duration-before-tempo resolves at the declared bpm");
    }
    CHECK_NEAR(s.duration, 2.0f * 4.0f * (60.0f / 140.0f), 1e-3f, "duration 2bars resolves");
    CHECK(s.scenes[0].visible == false, "scene visible option");
    // bloom 0.8 -> a post command carrying the bloom option
    bool hasPostBloom = false;
    for (const auto& c : s.scenes[0].setup)
      if (c.name == "post" && c.opts.get("bloom").asFloat() == 0.8f) hasPostBloom = true;
    CHECK(hasPostBloom, "scene-level bloom shorthand becomes a post command");
    // effect intro -> show intro alias
    bool hasShowIntro = false;
    for (const auto& c : s.scenes[0].setup)
      if (c.name == "show" && !c.args.empty() && c.args[0].asStr() == "intro") hasShowIntro = true;
    CHECK(hasShowIntro, "effect X alias becomes show X");
    CHECK_NEAR(s.main[0].time, 65.0f * (60.0f / 140.0f), 1e-3f, "beat(65) parenthesized form");
    CHECK_NEAR(s.main[1].time, 32.0f * (60.0f / 140.0f), 1e-3f, "32beats glued plural unit");
  }
}

// ---------------------------------------------------------------------------
static void testScriptEngineAndTimeline() {
  const std::string src = R"(
bpm 216
scene A { bars 8  intensity 0.4  chapter 1
    camera ACam { rig static; pos (0,0,2) }
    show intro
    at 2 { marker "sub" }
}
scene B { bars 4  intensity 0.9  chapter 3
    show tunnel
}
at 0 { show A }
at 18.0 { show B; transition fade 1 }
)";
  ScriptEngine eng;
  CHECK(eng.loadText(src, "eng"), "script engine load");
  TimelineEditor editor;
  eng.build(editor);
  const auto& secs = eng.sections();
  CHECK(secs.size() == 2, "two sections");
  CHECK(secs[0].name == "A", "section 0 name");
  CHECK_NEAR(secs[0].start, 0.0f, 1e-4f, "section 0 start");
  CHECK_NEAR(secs[0].duration, 8.0f * 4.0f * (60.0f / 216.0f), 1e-3f, "section 0 duration in bars");
  CHECK_NEAR(secs[1].start, 18.0f, 1e-4f, "section 1 start");
  CHECK_NEAR(secs[1].intensity, 0.9f, 1e-5f, "section 1 intensity");

  // events: 2 top-level at + scene A setup (at 0) + scene A sub (at 2) + scene B setup (at 18)
  CHECK(editor.events.size() == 5, "event count");
  CHECK_NEAR(editor.duration, 18.0f + 4.0f * 4.0f * (60.0f / 216.0f), 1e-3f, "timeline duration");

  // transport
  editor.play();
  int fired = 0;
  editor.update(1.0f);
  fired += (int)editor.fired().size();
  editor.consumeFired();
  CHECK(fired >= 2, "events fired at t=0 (top at + scene setup)");
  editor.update(1.0f);  // t=2 -> scene A sub fires
  bool sawSub = false;
  for (const auto& e : editor.fired()) {
    for (const auto& c : e.cmds) if (c.name == "marker") sawSub = true;
  }
  CHECK(sawSub, "scene-relative sub-block fired at scene start + 2");
  editor.consumeFired();

  // pause
  editor.pause();
  editor.update(5.0f);
  CHECK(editor.fired().empty(), "no events while paused");

  // seek re-arms
  editor.seek(10.0f);
  editor.play();
  editor.update(5.0f);  // crosses 15, no events between 10 and 15
  CHECK(editor.fired().empty(), "seek does not refire past events");
  editor.consumeFired();
  editor.update(4.0f);  // crosses 18 -> transition + scene B setup
  bool sawTrans = false;
  for (const auto& e : editor.fired())
    for (const auto& c : e.cmds) if (c.name == "transition") sawTrans = true;
  CHECK(sawTrans, "event at 18 fired after seek");

  // looping
  editor.consumeFired();
  editor.setLoop(true, 0, 18.0f);
  editor.seek(17.0f);
  editor.play();
  editor.update(2.0f);  // wraps to 1.0, crossing 0 -> refires t=0 events
  int n0 = 0;
  for (const auto& e : editor.fired()) if (e.time <= 0.001f) n0++;
  CHECK(n0 >= 2, "loop wrap refires events at 0");
  editor.consumeFired();

  // fireWindow: the seek catch-up primitive (what DemoApp::seek uses so a
  // scrub establishes the scene at the target). Fires exactly the events in
  // (lo, hi] in order, without moving the clock or the fire boundary.
  editor.fireWindow(10.0f, 30.0f);  // forward window: crosses the 18s event
  bool sawFireTrans = false;
  int fireWindowN = 0;
  for (const auto& e : editor.fired()) {
    if (e.time > 10.001f && e.time <= 30.0f) fireWindowN++;
    for (const auto& c : e.cmds)
      if (c.name == "transition") sawFireTrans = true;
  }
  CHECK(fireWindowN >= 1 && sawFireTrans, "fireWindow fires the crossed 18s event");
  CHECK_NEAR(editor.time, 1.0f, 1e-4f, "fireWindow does not move the clock");
  editor.consumeFired();
  editor.fireWindow(10.0f, 5.0f);  // inverted window (lo > hi) -> nothing
  CHECK(editor.fired().empty(), "fireWindow inverted window fires nothing");
  editor.consumeFired();

  // serialization round trip
  const Value j = editor.toJson();
  TimelineEditor editor2;
  editor2.fromJson(j);
  CHECK(editor2.events.size() == editor.events.size(), "timeline json round trip");
}

// ---------------------------------------------------------------------------
static void testAnimation() {
  // interpolators
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Linear, 0.5f, 0, 10), 5.0f, 1e-5f, "linear mid");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Linear, 0.0f, 3, 10), 3.0f, 1e-5f, "linear start");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Linear, 1.0f, 3, 10), 10.0f, 1e-5f, "linear end");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Smooth, 0.5f, 0, 1), 0.5f, 1e-5f, "smooth mid");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::EaseOut, 0.0f, 0, 1), 0.0f, 1e-5f, "ease-out start");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::EaseOut, 1.0f, 0, 1), 1.0f, 1e-5f, "ease-out end");
  const float b = AnimationSystem::interpValue(Interp::Bounce, 0.5f, 0, 1);
  CHECK(b >= 0.0f && b <= 1.0f, "bounce stays in range");
  const float el = AnimationSystem::interpValue(Interp::Elastic, 0.5f, 0, 1);
  CHECK(el >= 0.0f && el <= 1.2f, "elastic stays in range");
  // elastic overshoots (standard easeOutElastic peaks above 1 mid-tween)
  const float el2 = AnimationSystem::interpValue(Interp::Elastic, 0.2f, 0, 1);
  CHECK(el2 > 1.0f, "elastic overshoots above 1");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Elastic, 0.0f, 0, 1), 0.0f, 1e-5f, "elastic start");
  CHECK_NEAR(AnimationSystem::interpValue(Interp::Elastic, 1.0f, 0, 1), 1.0f, 1e-5f, "elastic end");

  // channel sampling
  AnimationSystem sys;
  auto anim = std::make_shared<Animation>();
  anim->name = "cameraIntro";
  anim->duration = 16.0f;
  AnimChannel ch;
  ch.target = "camera";
  ch.property = "fov";
  ch.keys = {
    {0.0f, Value(50.0), Interp::Linear},
    {8.0f, Value(64.0), Interp::Smooth},
    {16.0f, Value(58.0), Interp::Elastic},
  };
  anim->channels.push_back(ch);
  sys.add(anim);

  float out[4];
  int n = AnimationSystem::sampleChannel(ch, 0.0f, out, 4);
  CHECK(n == 1 && out[0] == 50.0f, "channel sample start");
  n = AnimationSystem::sampleChannel(ch, 16.0f, out, 4);
  CHECK(n == 1 && out[0] == 58.0f, "channel sample end");
  n = AnimationSystem::sampleChannel(ch, 8.0f, out, 4);
  CHECK(n == 1 && out[0] == 64.0f, "channel sample mid");
  n = AnimationSystem::sampleChannel(ch, 4.0f, out, 4);
  CHECK(n == 1 && out[0] > 50.0f && out[0] < 64.0f, "channel sample interpolates");
  n = AnimationSystem::sampleChannel(ch, -5.0f, out, 4);
  CHECK(out[0] == 50.0f, "channel clamps before first key");
  n = AnimationSystem::sampleChannel(ch, 99.0f, out, 4);
  CHECK(out[0] == 58.0f, "channel clamps after last key");

  // vector channel
  AnimChannel vc;
  vc.target = "camera";
  vc.property = "pos";
  V3 va0 = {0, 0, 2}, vb0 = {1, 1, 2};
  Value va = Value::array(); va.push(Value(0.0)); va.push(Value(0.0)); va.push(Value(2.0));
  Value vb = Value::array(); vb.push(Value(1.0)); vb.push(Value(1.0)); vb.push(Value(2.0));
  (void)va0; (void)vb0;
  vc.keys = {{0.0f, va, Interp::Linear}, {1.0f, vb, Interp::Linear}};
  n = AnimationSystem::sampleChannel(vc, 0.5f, out, 4);
  CHECK(n == 3 && out[0] == 0.5f && out[1] == 0.5f && out[2] == 2.0f, "vector interpolation");

  // runtime play + samples
  sys.play("cameraIntro");
  sys.update(4.0f);
  const auto& samples = sys.samples();
  bool sawFov = false;
  for (const auto& s : samples) if (s.target == "camera" && s.property == "fov") sawFov = true;
  CHECK(sawFov, "runtime samples contain camera.fov");
  sys.consumeSamples();
  sys.update(20.0f);  // past the end -> retires (non-looping)
  CHECK(!sys.isPlaying("cameraIntro"), "finished animation retires");
  sys.stopAll();

  // parse interp names
  CHECK(parseInterp("bounce") == Interp::Bounce, "parse interp bounce");
  CHECK(parseInterp("wat") == Interp::Linear, "unknown interp -> linear");
}

// ---------------------------------------------------------------------------
static void testSceneGraph() {
  SceneGraph g;
  SceneNode* cam = g.addNode("cam1", NodeType::Camera, CamData{62.0f, 0.05f, 400.0f, {0, 0, 0}, "drift"});
  CHECK(cam != nullptr, "add camera node");
  CHECK(g.find("cam1") == cam, "find by name");

  // hierarchy + world transform
  SceneNode* parent = g.addNode("p", NodeType::Empty);
  SceneNode* child = g.addNode("c", NodeType::Sprite, SpriteData{}, parent);
  parent->setPos({1, 2, 3});
  child->setPos({4, 5, 6});
  g.update();
  CHECK_NEAR(child->world[12], 5.0f, 1e-4f, "world translation x (parent + child)");
  CHECK_NEAR(child->world[13], 7.0f, 1e-4f, "world translation y");
  CHECK_NEAR(child->world[14], 9.0f, 1e-4f, "world translation z");

  // scale propagates: world.x = parent.pos.x + parent.scale.x * child.pos.x
  parent->setScale(2.0f);
  g.update();
  CHECK_NEAR(child->world[12], 1.0f + 2.0f * 4.0f, 1e-4f, "scaled parent world pos");

  // visibility / enabled
  CHECK(child->isActive(), "child active");
  parent->enabled = false;
  CHECK(!child->isActive(), "disabled parent deactivates child");
  parent->enabled = true;
  child->visible = false;
  CHECK(!child->isActive(), "invisible child inactive");

  // tags
  parent->tags.push_back("solid");
  auto tagged = g.findTag("solid");
  CHECK(tagged.size() == 1 && tagged[0] == parent, "find by tag");

  // node types
  SceneNode* l = g.addNode("key", NodeType::Light, LightData{"point", {1, 0, 0}, 3.0f, 10, 45, false});
  CHECK(l->asLight() != nullptr, "light payload accessor");
  CHECK(l->asCamera() == nullptr, "wrong accessor returns null");
  CHECK(l->asLight()->intensity == 3.0f, "light intensity");

  // serialization round trip
  const Value j = g.toJson();
  SceneGraph g2;
  g2.fromJson(j);
  SceneNode* c2 = g2.find("c");
  CHECK(c2 != nullptr, "scene json round trip finds child");
  CHECK(c2->type == NodeType::Sprite, "scene json round trip type");
  SceneNode* l2 = g2.find("key");
  CHECK(l2->asLight() && l2->asLight()->color[0] == 1.0f, "scene json round trip payload");
  g2.update();
  CHECK_NEAR(c2->world[12], 9.0f, 1e-3f, "restored world matrix");
}

// ---------------------------------------------------------------------------
static void testCameraRig() {
  // static rig
  CameraRig st;
  st.type = "static";
  st.pos = {1, 2, 3};
  st.target = {0, 0, 0};
  st.fov = 55.0f;
  const RigSample s = st.sample(10.0f, 5.0f);
  CHECK(s.pos[0] == 1.0f && s.pos[1] == 2.0f && s.pos[2] == 3.0f, "static pos");
  CHECK_NEAR(s.fovDeg, 55.0f, 1e-5f, "static fov");

  // fly rig moves forward with local time
  CameraRig fly;
  fly.type = "fly";
  fly.pos = {0, 0, 14};
  fly.speed = 16.0f;
  const RigSample f0 = fly.sample(5.0f, 0.0f);
  const RigSample f1 = fly.sample(5.0f, 2.0f);
  CHECK_NEAR(f0.pos[2], 14.0f, 1e-4f, "fly start z");
  CHECK_NEAR(f1.pos[2], 14.0f - 32.0f, 1e-4f, "fly z after 2s");

  // orbit stays on a (slowly breathing) circle around radius `radius`
  CameraRig orb;
  orb.type = "orbit";
  orb.radius = 4.0f;
  orb.pos = {0, 1.5f, 0};
  orb.omega = 1.0f;
  const RigSample o0 = orb.sample(0, 0.0f);
  const RigSample o1 = orb.sample(0, 3.14159265f / 2.0f);
  const float r0 = std::sqrt(o0.pos[0] * o0.pos[0] + o0.pos[2] * o0.pos[2]);
  CHECK_NEAR(r0, 4.0f, 1e-3f, "orbit radius at 0");
  CHECK(std::fabs(o1.pos[0]) < 1e-3f, "orbit x at pi/2 (top of the circle)");
  const float r1 = std::sqrt(o1.pos[0] * o1.pos[0] + o1.pos[2] * o1.pos[2]);
  CHECK(r1 > 3.5f && r1 < 4.6f, "orbit stays near radius 4 with drift");

  // fromCmd parsing
  Cmd c;
  c.name = "camera";
  c.opts.set("rig") = Value("drift");
  c.opts.set("pos") = Value("(0,0,2.4)");  // string form handled by toFloats
  c.opts.set("fov") = Value("(50,64)");
  c.opts.set("buildUp") = Value("(49,58)");
  c.opts.set("amp") = Value(2.8);
  auto rig = CameraRig::fromCmd(c);
  CHECK(rig->type == "drift", "rig type from cmd");
  CHECK(rig->pos[2] == 2.4f, "rig pos from cmd");
  CHECK_NEAR(rig->fov, 64.0f, 1e-5f, "rig fov target");
  CHECK_NEAR(rig->fovBase, 50.0f, 1e-5f, "rig fov base");
  CHECK_NEAR(rig->buildUpStart, 49.0f, 1e-5f, "rig buildUp start");
  const RigSample r0s = rig->sample(49.0f, 0.0f);
  const RigSample r1s = rig->sample(58.0f, 0.0f);
  CHECK_NEAR(r0s.fovDeg, 50.0f, 1e-3f, "fov ramp at start");
  CHECK_NEAR(r1s.fovDeg, 64.0f, 1e-3f, "fov ramp at end");
}

// ---------------------------------------------------------------------------
static void testAssetManager() {
  AssetManager am;
  int loads = 0, frees = 0;
  am.registerKind(
      "texture",
      [&](const std::string& p) { loads++; return (void*)(uintptr_t)(loads * 100 + 7); },
      [&](void* h) { (void)h; frees++; },
      [&](const std::string& p, void*& h) {
        (void)p;
        h = (void*)(uintptr_t)999;
        return true;
      });

  void* a = am.acquire("a.png", "texture");
  void* b = am.acquire("a.png", "texture");  // cached
  CHECK(a == b, "asset cache returns same handle");
  CHECK(loads == 1, "asset loaded once");
  CHECK(am.find("a.png", "texture")->refs == 2, "refcount 2");

  am.release("a.png", "texture");
  CHECK(am.find("a.png", "texture")->refs == 1, "refcount 1 after release");
  am.release("a.png", "texture");
  CHECK(am.find("a.png", "texture") == nullptr, "asset freed at zero refs");
  CHECK(frees == 1, "free called once");

  // reload
  (void)am.acquire("a.png", "texture");
  const uint64_t v0 = am.version("a.png", "texture");
  am.markDirty("a.png");
  CHECK(am.anyDirty(), "asset dirty after markDirty");
  const int n = am.reloadDirty();
  CHECK(n == 1, "one asset reloaded");
  CHECK(am.version("a.png", "texture") == v0 + 1, "version bumped after reload");
  CHECK(!am.anyDirty(), "clean after reload");

  // unknown kind
  CHECK(am.acquire("x.txt", "nope") == nullptr, "unknown kind returns null");
  am.clear();

  // a failed load is NOT terminal: markDirty + reloadDirty retries the loader
  // (this is how a texture/model dropped in after the show started gets
  // picked up without a restart)
  {
    AssetManager am2;
    int calls = 0;
    am2.registerKind(
        "tex",
        [&](const std::string&) -> void* { return ++calls >= 2 ? (void*)(uintptr_t)777 : nullptr; },
        [](void*) {}, nullptr);
    CHECK(am2.acquire("b.png", "tex") == nullptr, "first load fails -> null handle");
    CHECK(calls == 1, "loader ran once on the failed acquire");
    am2.markDirty("b.png");
    const int n = am2.reloadDirty();
    CHECK(n == 1, "failed asset retried + loaded on reloadDirty");
    AssetInfo* info = am2.find("b.png", "tex");
    CHECK(info && info->loaded, "retried asset is loaded");
    CHECK(info && info->handle == (void*)(uintptr_t)777, "retried asset handle set");
    am2.clear();
  }
}

// ---------------------------------------------------------------------------
static void testFileWatcher() {
  const std::string dir = "fw_test_tmp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  const std::string file = dir + "/a.txt";
  {
    std::ofstream f(file);
    f << "one";
  }
  int changed = 0;
  FileWatcher w([&](const std::string&) { changed++; });
  w.add(dir);
  w.poll();  // baseline (initial scan does not fire)

  // rewrite with a different SIZE (mtime granularity varies by filesystem)
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::ofstream f(file);
    f << "two-two-two-two";
  }
  const int n = w.poll();
  CHECK(n >= 1, "watcher detected a change");
  CHECK(changed >= 1, "watcher callback fired");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(w.poll() == 0, "no spurious changes");

  // a deleted file is REPORTED as a change (a missing dependency must fail
  // loudly, not silently drop off the watch set), and a file recreated after
  // deletion re-enters the watch set and is reported again - this is what
  // lets a deleted-then-restored shader/texture reload live
  std::filesystem::remove(file, ec);
  const int nDel = w.poll();
  CHECK(nDel >= 1, "watcher reports a deleted file");
  CHECK(changed >= 2, "watcher callback fired for the deletion");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(w.poll() == 0, "no spam while the file stays deleted");
  {
    std::ofstream f(file);
    f << "recreated";
  }
  const int nRe = w.poll();
  CHECK(nRe >= 1, "recreated file re-enters the watch set");
  CHECK(changed >= 3, "watcher callback fired for the recreation");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(w.poll() == 0, "no spurious changes after recreation");

  // markDirty through the asset pipeline
  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// testLiveReloadChain - GL-free proof of the live-reload loop exactly as the
// app wires it:
//
//     FileWatcher::poll()  ->  changed()  ->  AssetManager::markDirty()  ->  reloadDirty()
//
// The loader treats the file's CONTENT as the opaque handle (a heap string),
// so the tests can assert that the fixed content is what actually goes live -
// no restart, no GL. The acquire paths are "/"-concatenated like the app's
// dataDir() + "/textures/...", while the watcher reports native (backslash
// on Windows) paths: the AssetManager must canonicalize so they match.
// ---------------------------------------------------------------------------
static std::function<void*(const std::string&)> contentLoader() {
  return [](const std::string& path) -> void* {
    std::ifstream f(path);
    if (!f) return nullptr;
    const std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (c.find("BROKEN") != std::string::npos) return nullptr;  // loader rejects garbage
    return new std::string(c);
  };
}

static std::function<bool(const std::string&, void*&)> contentReloadFn() {
  return [](const std::string& path, void*& handle) -> bool {
    std::ifstream f(path);
    if (!f) return false;
    const std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (c.find("BROKEN") != std::string::npos) return false;
    delete static_cast<std::string*>(handle);
    handle = new std::string(c);
    return true;
  };
}

static std::function<void(void*)> contentFree() {
  return [](void* h) { delete static_cast<std::string*>(h); };
}

// mirror DemoApp::update: poll the watcher, mark every reported file dirty,
// then run the deferred reload pass. The real pollLiveReload() runs
// reloadDirty() whenever the watcher reported anything (it is a no-op when
// nothing is dirty), so it is called unconditionally here too.
static int pumpReload(FileWatcher& w, AssetManager& am) {
  const int n = w.poll();
  if (n > 0) {
    for (const auto& f : w.changed()) am.markDirty(f);
  }
  am.reloadDirty();
  return n;
}

static void testLiveReloadChain() {
  const std::string root = "fw_live_tmp";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  // --- 1. dropped in late: acquire fails, file fixed on disk, picked up live
  {
    const std::string dir = root + "/late";
    std::filesystem::create_directories(dir, ec);
    const std::string file = dir + "/tex.png";  // "/" concatenation, like the app
    {
      std::ofstream f(file);
      f << "BROKEN-LATE-MISSING";
    }

    AssetManager am;
    am.registerKind("texture", contentLoader(), contentFree());
    FileWatcher w;
    w.add(dir);
    w.poll();  // baseline (initial scan does not fire)

    CHECK(am.acquire(file, "texture") == nullptr, "acquire fails while the file is broken");
    AssetInfo* info = am.find(file, "texture");
    CHECK(info && !info->loaded, "failed acquire registered as unloaded");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      std::ofstream f(file);
      f << "TEXTURE-OK-42";
    }
    CHECK(pumpReload(w, am) >= 1, "watcher reported the fix");
    info = am.find(file, "texture");
    CHECK(info && info->loaded, "fixed file picked up WITHOUT restart");
    CHECK(info && info->handle && *static_cast<std::string*>(info->handle) == "TEXTURE-OK-42",
          "handle carries the fixed content");
    CHECK(info && info->version == 1, "retried load bumped version to 1");
    CHECK(info && info->error.empty(), "retry error cleared on success");
    am.clear();
  }

  // --- 2. live edit: loaded asset, source edited, handle swapped in place
  {
    const std::string dir = root + "/edit";
    std::filesystem::create_directories(dir, ec);
    const std::string file = dir + "/tex.png";
    {
      std::ofstream f(file);
      f << "TEXTURE-V1";
    }

    AssetManager am;
    am.registerKind("texture", contentLoader(), contentFree(), contentReloadFn());
    FileWatcher w;
    w.add(dir);
    w.poll();  // baseline

    void* h1 = am.acquire(file, "texture");
    CHECK(h1 && *static_cast<std::string*>(h1) == "TEXTURE-V1", "initial load");
    const uint64_t v0 = am.version(file, "texture");
    CHECK(v0 == 1, "initial version 1");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      std::ofstream f(file);
      f << "TEXTURE-V2-EDITED";
    }
    CHECK(pumpReload(w, am) >= 1, "watcher reported the edit");
    AssetInfo* info = am.find(file, "texture");
    CHECK(info && info->version == v0 + 1, "version bumped after live edit");
    CHECK(info && info->handle && *static_cast<std::string*>(info->handle) == "TEXTURE-V2-EDITED",
          "handle swapped to the edited content");
    CHECK(info && info->error.empty(), "reload error cleared on success");
    am.clear();
  }

  // --- 3. delete + restore: previous version kept while missing (no spam),
  //        restored file comes back live
  {
    const std::string dir = root + "/gone";
    std::filesystem::create_directories(dir, ec);
    const std::string file = dir + "/tex.png";
    {
      std::ofstream f(file);
      f << "TEXTURE-V3";
    }

    AssetManager am;
    am.registerKind("texture", contentLoader(), contentFree(), contentReloadFn());
    FileWatcher w;
    w.add(dir);
    w.poll();  // baseline

    void* h3 = am.acquire(file, "texture");
    CHECK(h3 && *static_cast<std::string*>(h3) == "TEXTURE-V3", "initial load v3");

    std::filesystem::remove(file, ec);
    CHECK(pumpReload(w, am) >= 1, "deletion reported by the watcher");
    AssetInfo* info = am.find(file, "texture");
    CHECK(info && info->loaded, "asset stays loaded while the source is missing");
    CHECK(info && info->handle && *static_cast<std::string*>(info->handle) == "TEXTURE-V3",
          "previous version kept");
    CHECK(info && info->version == 1, "version unchanged while missing");
    CHECK(info && !info->error.empty(), "failed reload records an informative error");
    CHECK(pumpReload(w, am) == 0, "no spam while the file stays missing");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      std::ofstream f(file);
      f << "TEXTURE-V4-RESTORED";
    }
    CHECK(pumpReload(w, am) >= 1, "restored file reported again");
    info = am.find(file, "texture");
    CHECK(info && info->version == 2, "restore bumped the version");
    CHECK(info && info->handle && *static_cast<std::string*>(info->handle) == "TEXTURE-V4-RESTORED",
          "restored content is live");
    CHECK(info && info->error.empty(), "restore error cleared - full cycle recovered");
    am.clear();
  }

  std::filesystem::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
static void testValueStrings() {
  Value o = Value::object();
  o.set("a") = Value("x");
  CHECK(o.toString().find("\"a\"") != std::string::npos, "object toString");
  CHECK(o.get("b").isNull(), "missing key returns null");
  o.set("b") = Value(5.0);
  CHECK(o.get("b").asNum() == 5.0, "set inserts");
}

// ---------------------------------------------------------------------------
// testShadertoyParser - the Shadertoy importer's pass splitter is GL-free and
// must never misfire: a false `// pass:` marker (prose mention, nested
// comment) or broken line-end arithmetic used to truncate the pass source
// (single-pass files compiled from mid-comment -> "unexpected ==" cascades)
// or throw std::out_of_range ("invalid string position" on multi-pass files).
// ---------------------------------------------------------------------------
static void testShadertoyParser() {
  // --- prose that merely MENTIONS "// pass:" must not split the file
  {
    const std::string src =
        "// header comment (no `// pass:` markers = image pass).\n"
        "float hash21(vec2 p) { return fract(p.x); }\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(hash21(f)); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1, "prose mention does not create a marker");
    CHECK(passes.size() == 1 && passes[0].name == "image", "prose mention -> single image pass");
    CHECK(passes.size() == 1 && passes[0].src.find("mainImage") != std::string::npos,
          "prose mention keeps the whole body");
  }

  // --- nested "//   // pass: x" descriptions must not split the file
  {
    const std::string src =
        "//   // pass: common    - shared helpers (no program of its own)\n"
        "//   // pass: buffer_a  - renders into buffer A\n"
        "// pass: common\n"
        "float h(vec2 p) { return p.x; }\n"
        "// pass: image\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(h(f)); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 2, "nested mentions ignored; 2 real markers");
    CHECK(passes.size() == 2 && passes[0].name == "common", "first real pass is common");
    CHECK(passes.size() == 2 && passes[1].name == "image", "second real pass is image");
  }

  // --- marker name is a single token: trailing comment text is dropped
  {
    const std::string src =
        "// pass: buffer_a   - renders into the RGBA16F buffer A target\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1 && passes[0].name == "buffer_a", "trailing marker text dropped");
  }

  // --- indented markers still work; content before the first marker is dropped
  {
    const std::string src =
        "// dropped header prose\n"
        "   // pass: common\n"
        "float h(vec2 p) { return p.x; }\n"
        "   // pass: image\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(h(f)); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 2, "indented markers split");
    CHECK(passes.size() == 2 && passes[0].src.find("float h") != std::string::npos,
          "pre-marker prose dropped, common body kept");
  }

  // --- CRLF line endings are handled
  {
    const std::string src =
        "// pass: common\r\n"
        "float h(vec2 p) { return p.x; }\r\n"
        "// pass: image\r\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(h(f)); }\r\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 2, "CRLF markers split");
    CHECK(passes.size() == 2 && passes[1].src.find("// pass:") == std::string::npos,
          "CRLF marker line excluded from the pass source");
  }

  // --- marker at EOF without a trailing newline must not throw or truncate
  {
    const std::string src =
        "// pass: buffer_a\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n"
        "// pass: image";  // no trailing newline
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1, "EOF marker without content: no throw, empty pass dropped");
    CHECK(passes.size() == 1 && passes[0].name == "buffer_a", "EOF marker pass dropped cleanly");
  }

  // --- empty / whitespace-only content is a single (empty) image pass
  {
    const auto passes = splitShadertoyPasses("   \n\n");
    CHECK(passes.size() == 1 && passes[0].name == "image", "whitespace-only -> image pass");
  }

  // --- marker on the FIRST line of the file works
  {
    const std::string src =
        "// pass: image\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1 && passes[0].name == "image", "first-line marker detected");
  }

  // --- `// pass:name` without a space after the colon also works
  {
    const std::string src =
        "// pass:buffer_a\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1 && passes[0].name == "buffer_a", "marker without space after colon");
  }

  // --- a UTF-8 BOM at byte 0 must not hide a first-line marker
  {
    const std::string src =
        std::string("\xEF\xBB\xBF") +
        "// pass: image\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n";
    const auto passes = splitShadertoyPasses(src);
    CHECK(passes.size() == 1 && passes[0].name == "image", "UTF-8 BOM does not hide the marker");
  }

  // --- extractShadertoyRenderScale: per-file buffer resolution option.
  // Same strict first-comment rule as markers: only whitespace before the
  // comment, token-exact, so prose can never trigger it. Clamped to (0,1].
  {
    const std::string src =
        "// header prose\n"
        "// option: renderScale 0.5\n"
        "void mainImage(out vec4 o, in vec2 f) { o = vec4(1.0); }\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 0.5f, 1e-6f, "renderScale 0.5 parsed");
  }
  {
    const std::string src =
        "// option: renderScale 0.25\n"
        "// option: renderScale 0.75\n";  // first wins
    CHECK_NEAR(extractShadertoyRenderScale(src), 0.25f, 1e-6f, "first renderScale wins");
  }
  {
    // prose that merely mentions the text must NOT trigger the option
    const std::string src = "// no `// option: renderScale 0.5` markers here\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 1.0f, 1e-6f, "prose mention ignored");
  }
  {
    // nested description must not trigger the option
    const std::string src = "//   // option: renderScale 0.25 - a nested mention\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 1.0f, 1e-6f, "nested mention ignored");
  }
  {
    // CRLF + BOM are handled; values clamp to (0,1]
    const std::string src = std::string("\xEF\xBB\xBF") + "// option: renderScale 2.0\r\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 1.0f, 1e-6f, "renderScale >1 clamps to 1");
    const std::string neg = "// option: renderScale -0.5\n";
    CHECK_NEAR(extractShadertoyRenderScale(neg), 1.0f, 1e-6f, "negative renderScale -> default");
    const std::string zero = "// option: renderScale 0\n";
    CHECK_NEAR(extractShadertoyRenderScale(zero), 1.0f, 1e-6f, "zero renderScale -> default");
  }
  {
    // missing value falls back to default
    const std::string src = "// option: renderScale\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 1.0f, 1e-6f, "missing value -> default");
  }
  {
    // trailing garbage after the number is NOT accepted (strtof would stop
    // at the first non-numeric char, so `0.5foo` must not parse as 0.5)
    const std::string junk = "// option: renderScale 0.5foo\n";
    CHECK_NEAR(extractShadertoyRenderScale(junk), 1.0f, 1e-6f, "trailing junk rejected");
    const std::string dot = "// option: renderScale 0.5.0\n";
    CHECK_NEAR(extractShadertoyRenderScale(dot), 1.0f, 1e-6f, "malformed number rejected");
  }
  {
    // wrong option name is not renderScale
    const std::string src = "// option: exposure 0.5\n";
    CHECK_NEAR(extractShadertoyRenderScale(src), 1.0f, 1e-6f, "other option ignored");
  }

  // --- the REAL shipped files split into the intended structure
  {
    const std::string dir = NULLSECTOR_DATA_DIR;
    // plasma.glsl is a single-pass sample: header prose mentions the marker
    // text - the old parser treated that mention as a marker and compiled a
    // truncated body; now it must be exactly one image pass
    {
      std::ifstream f(dir + "/shadertoy/plasma.glsl");
      const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const auto passes = splitShadertoyPasses(s);
      CHECK(passes.size() == 1, "plasma.glsl: one pass");
      CHECK(passes.size() == 1 && passes[0].name == "image", "plasma.glsl: image pass");
      CHECK(passes.size() == 1 && passes[0].src.find("mainImage") != std::string::npos,
            "plasma.glsl: body intact");
      CHECK(passes.size() == 1 && passes[0].src.find("hash21") != std::string::npos,
            "plasma.glsl: helpers intact");
      CHECK_NEAR(extractShadertoyRenderScale(s), 1.0f, 1e-6f, "plasma.glsl: full-res default");
    }
    // tunnel_warp.glsl: header DOCUMENTS the markers with nested comments -
    // only the three real marker lines may split, and each pass keeps its body
    {
      std::ifstream f(dir + "/shadertoy/tunnel_warp.glsl");
      const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const auto passes = splitShadertoyPasses(s);
      CHECK(passes.size() == 3, "tunnel_warp.glsl: three passes");
      CHECK(passes.size() >= 3 && passes[0].name == "common", "tunnel_warp: common first");
      CHECK(passes.size() >= 3 && passes[1].name == "buffer_a", "tunnel_warp: buffer_a");
      CHECK(passes.size() >= 3 && passes[2].name == "image", "tunnel_warp: image last");
      if (passes.size() == 3) {
        CHECK(passes[0].src.find("hash21") != std::string::npos, "tunnel_warp: common helpers");
        CHECK(passes[1].src.find("mainImage") != std::string::npos, "tunnel_warp: buffer_a body");
        CHECK(passes[1].src.find("// pass: image") == std::string::npos,
              "tunnel_warp: image marker not leaked into buffer_a");
        CHECK(passes[2].src.find("warp(") != std::string::npos, "tunnel_warp: image body");
      }
      CHECK_NEAR(extractShadertoyRenderScale(s), 0.5f, 1e-6f, "tunnel_warp.glsl: renderScale 0.5");
    }
  }
}

// ---------------------------------------------------------------------------
static void testLogSink() {
  std::vector<std::string> got;
  Log::setSink([&](const std::string& l) { got.push_back(l); });
  Log::info("T", "alpha");
  Log::warn("T", "beta");
  Log::setSink({});
  CHECK(got.size() == 2, "log sink captured both lines");
  CHECK(got.size() >= 2 && got[0].find("[INF][T] alpha") != std::string::npos, "log sink line format (info)");
  CHECK(got.size() >= 2 && got[1].find("[WRN][T] beta") != std::string::npos, "log sink line format (warn)");

  // re-arming a sink replaces the previous one; stderr output is unaffected
  std::vector<std::string> again;
  Log::setSink([&](const std::string& l) { again.push_back(l); });
  Log::error("T", "gamma");
  Log::setSink({});
  CHECK(again.size() == 1 && again[0].find("[ERR][T] gamma") != std::string::npos, "sink re-armed works");
  CHECK(got.size() == 2, "old sink no longer receives lines");
}

// ---------------------------------------------------------------------------
// testGpuTimeStats - the GL-free accumulation behind the Shadertoy importer's
// GPU-timing log line and --check-shadertoy profiling: the EMA used for the
// periodic "X ms/frame GPU" note, the beginProfile/endProfileMs window used
// to measure a mean over N frames, and the log throttle (once per logEvery
// samples, so the note appears ~every 2s at 60fps, not every frame).
// ---------------------------------------------------------------------------
static void testGpuTimeStats() {
  // EMA: starts from the first sample, then decays toward newer ones
  GpuTimeStats s;
  CHECK(s.emaMs == 0.0, "EMA starts at 0");
  CHECK(s.ema() == 0.0, "ema() accessor starts at 0 (no sample yet)");
  s.add(2.0f);
  CHECK_NEAR(s.emaMs, 2.0, 1e-9, "EMA takes the first sample");
  CHECK_NEAR(s.ema(), 2.0, 1e-9, "ema() accessor follows the EMA");
  s.add(10.0f);
  CHECK(s.emaMs > 2.0 && s.emaMs < 10.0, "EMA moves toward the new sample");
  CHECK_NEAR(s.ema(), 2.0 * 0.9 + 10.0 * 0.1, 1e-9, "ema() accessor blend weight");

  // lastMs: the most recent UNSMOOTHED sample (the --perf-raw rows - the
  // raw value keeps spikes that the EMA smooths away)
  GpuTimeStats rw;
  CHECK(rw.lastMs() == 0.0, "last raw ms starts at 0 (no sample yet)");
  rw.add(1.5f);
  CHECK_NEAR(rw.lastMs(), 1.5, 1e-9, "lastMs() is the latest raw sample");
  rw.add(7.5f);
  CHECK_NEAR(rw.lastMs(), 7.5, 1e-9, "lastMs() updates with each sample");
  // the EMA follows the raw value but is NOT equal to it once smoothing
  // has kicked in - raw mode must report the unsmoothed ms
  CHECK(rw.ema() != rw.lastMs(), "raw and EMA diverge after the first sample");

  // profile window: mean over the collected samples, -1 with none
  GpuTimeStats p;
  CHECK(p.meanMs() == -1.0, "mean with no samples is -1");
  p.beginProfile();
  p.add(1.0f);
  p.add(3.0f);
  p.add(2.0f);
  p.endProfile();
  CHECK_NEAR(p.meanMs(), 2.0, 1e-9, "profile mean over the window");
  // samples collected AFTER the window must not move the frozen mean
  p.add(100.0f);
  CHECK_NEAR(p.meanMs(), 2.0, 1e-9, "post-window samples do not move the mean");
  // a second window resets the accumulator
  p.beginProfile();
  p.add(4.0f);
  p.endProfile();
  CHECK_NEAR(p.meanMs(), 4.0, 1e-9, "new window resets the mean");
  // a window with no samples reports -1 (beginProfile resets the sum)
  p.beginProfile();
  p.endProfile();
  CHECK(p.meanMs() == -1.0, "window with no samples is -1");
  CHECK(p.medianMs() == -1.0, "median with no samples is -1");

  // median: robust against a single outlier frame (a driver hiccup can
  // inflate a short-window mean by 2-3x; the median ignores it)
  GpuTimeStats m;
  m.beginProfile();
  for (int i = 0; i < 29; i++) m.add(1.0f);
  m.add(50.0f);  // one outlier
  m.endProfile();
  CHECK_NEAR(m.meanMs(), (29.0 + 50.0) / 30.0, 1e-9, "mean includes the outlier");
  CHECK_NEAR(m.medianMs(), 1.0, 1e-9, "median ignores the outlier");
  // even count -> upper median
  GpuTimeStats e;
  e.beginProfile();
  e.add(1.0f);
  e.add(1.0f);
  e.add(2.0f);
  e.add(3.0f);
  e.endProfile();
  CHECK_NEAR(e.medianMs(), 2.0, 1e-9, "even count upper median");

  // log throttle: exactly once per logEvery collected samples
  GpuTimeStats l;
  for (int i = 0; i < 119; i++) CHECK(!l.logDue(), "no log before the 120th sample");
  CHECK(l.logDue(), "log fires on the 120th sample");
  CHECK(!l.logDue(), "throttle resets after firing");
  // a zero/negative logEvery is clamped so the throttle can never spin
  GpuTimeStats z;
  z.logEvery = 0;
  CHECK(z.logDue(), "logEvery 0 clamps to fire every sample");

  // formatting: two decimals
  CHECK(fmtMs(1.2345) == "1.23", "fmtMs two decimals");
  CHECK(fmtMs(0.0) == "0.00", "fmtMs zero");

  // --- PerfRingState: the GL_TIMESTAMP ring's bookkeeping (the part of
  // PerfTimer that used to live inline in ShadertoyFX and silently dropped
  // every sample when the CPU ran ahead of the GPU - a single next-frame
  // availability check). Trace with a 4-slot ring:
  //   issue x3 -> pending 3, oldest still slot 0; collect -> oldest 1
  //   issue to full -> oldest 1; issue again while full overwrites the
  //   oldest (the write slot IS the oldest) and pending caps at the count
  PerfRingState r(4);
  CHECK(r.pending == 0 && r.oldest() == 0, "empty ring: oldest is the write slot");
  r.issued(); r.issued(); r.issued();
  CHECK(r.pending == 3 && r.oldest() == 0, "issued pairs keep the oldest at slot 0");
  r.collected();
  CHECK(r.pending == 2 && r.oldest() == 1, "collecting the oldest advances it");
  r.issued(); r.issued();
  CHECK(r.pending == 4 && r.oldest() == 1, "ring full: every slot holds a pair");
  r.issued();  // full + issue without collect: overwrites (drops) the oldest
  CHECK(r.pending == 4 && r.oldest() == 2, "full-ring issue drops the oldest pair");
  for (int i = 0; i < 10; i++) r.collected();
  CHECK(r.pending == 0 && r.oldest() == 2, "collected clamps at zero");

  // --- per-run recorder (the --perf-json exit dump reads these): a bounded
  // circular window over the run's samples + a running total
  {
    GpuTimeStats q;
    for (int i = 1; i <= 5; i++) q.add((float)i);  // 1..5
    CHECK(q.recordedCount() == 5, "recorder counts every sample");
    CHECK_NEAR(q.medianRecorded(), 3.0, 1e-9, "recorder median");
    CHECK_NEAR(q.meanRecorded(), 3.0, 1e-9, "recorder mean");
    CHECK_NEAR(q.minRecorded(), 1.0, 1e-9, "recorder min");
    CHECK_NEAR(q.maxRecorded(), 5.0, 1e-9, "recorder max");
  }
  {
    // circular overwrite: cap 4, add 1..6 -> the window holds {3,4,5,6}
    GpuTimeStats q(4);
    for (int i = 1; i <= 6; i++) q.add((float)i);
    CHECK(q.recordedCount() == 6, "recorder counts overwritten samples too");
    CHECK_NEAR(q.medianRecorded(), 5.0, 1e-9, "circular median over the last 4 (upper)");
    CHECK_NEAR(q.meanRecorded(), 4.5, 1e-9, "circular mean over the last 4");
    CHECK_NEAR(q.minRecorded(), 3.0, 1e-9, "circular min");
    CHECK_NEAR(q.maxRecorded(), 6.0, 1e-9, "circular max");
  }
  {
    // cap 0: nothing stored (median/mean -1), but the count still tracks
    GpuTimeStats q(0);
    for (int i = 0; i < 5; i++) q.add(1.0f);
    CHECK(q.recordedCount() == 5, "cap 0 still counts samples");
    CHECK(q.medianRecorded() == -1.0 && q.meanRecorded() == -1.0, "cap 0 -> no window stats");
  }
}

// ---------------------------------------------------------------------------
// demo data validation: the data/demo.nsd script + every JSON the show
// consumes must parse, build and round-trip (GL-free, so it runs in CI).
// ---------------------------------------------------------------------------
static void testDemoData() {
  const std::string dir = NULLSECTOR_DATA_DIR;
  CHECK(!dir.empty() && std::filesystem::is_directory(dir), "data dir exists");

  // 1. the flagship script parses + builds into the timeline + sections
  ScriptEngine se;
  CHECK(se.load(dir + "/demo.nsd"), "demo.nsd loads");
  CHECK((int)se.scenes().size() == 10, "demo.nsd declares 10 scenes");
  CHECK(se.duration() > 120.0f && se.duration() < 130.0f, "demo.nsd duration ~125s");
  TimelineEditor te;
  se.build(te);  // sections/unresolved populate here
  CHECK((int)se.sections().size() == 10, "demo.nsd yields 10 sections");
  CHECK(se.unresolved().empty(), "demo.nsd has no unresolved shows");
  CHECK(te.events.size() >= 20, "timeline built from script has 20+ events");
  CHECK(te.duration > 120.0f && te.duration < 130.0f, "timeline duration follows the script");
  // the app dispatches `marker NAME` commands - they must reach the timeline
  bool markerCmd = false;
  for (const auto& ev : te.events)
    for (const auto& c : ev.cmds) if (c.name == "marker") markerCmd = true;
  CHECK(markerCmd, "script markers reach the timeline as commands");

  // every scene has a camera rig + at least one show command in its setup
  for (const auto& b : se.scenes()) {
    bool hasCamera = false, hasShow = false;
    for (const auto& c : b.setup) {
      if (c.name == "camera") hasCamera = true;
      if (c.name == "show" || c.name == "load" || c.name == "shader") hasShow = true;
    }
    CHECK(hasCamera, ("scene " + b.name + " declares a camera").c_str());
    CHECK(hasShow, ("scene " + b.name + " shows content").c_str());
  }

  // 2. every JSON data file parses
  const char* jsons[] = {
      "post/cinematic.json", "post/vhs.json", "post/clean.json",
      "materials/chrome.json", "materials/neon.json",
      "scenes/lightbox.json", "timelines/beat_map.json",
  };
  for (const char* f : jsons) {
    bool ok = false;
    try {
      const Value v = Json::parseFile(dir + "/" + f);
      ok = !v.isNull();
    } catch (const std::exception&) {
      ok = false;
    }
    CHECK(ok, (std::string("json parses: ") + f).c_str());
  }

  // 3. the scene JSON round-trips into a SceneGraph with expected nodes
  try {
    SceneGraph g;
    g.fromJson(Json::parseFile(dir + "/scenes/lightbox.json"));
    CHECK(g.find("terrain") != nullptr, "scene json: terrain node");
    CHECK(g.find("gem") != nullptr, "scene json: gem node");
    CHECK(g.find("sun") != nullptr, "scene json: sun light");
    CHECK(g.find("caption") != nullptr, "scene json: caption text");
    SceneNode* terrain = g.find("terrain");
    if (terrain && terrain->asMesh()) CHECK(terrain->asMesh()->model == "terrain.obj", "scene json: terrain model ref");
  } catch (const std::exception& e) {
    CHECK(false, (std::string("scene json round-trip: ") + e.what()).c_str());
  }

  // 4. the timeline JSON round-trips (tracks/events/markers/clips)
  try {
    TimelineEditor t;
    t.fromJson(Json::parseFile(dir + "/timelines/beat_map.json"));
    CHECK(t.duration > 120.0f, "timeline json: duration");
    CHECK(t.tracks.size() == 3, "timeline json: tracks");
    CHECK(t.events.size() >= 4, "timeline json: events");
    CHECK(t.markers.size() == 3, "timeline json: markers");
    CHECK(t.clips.size() == 2, "timeline json: clips");
  } catch (const std::exception& e) {
    CHECK(false, (std::string("timeline json round-trip: ") + e.what()).c_str());
  }

  // 5. material JSON carries the PBR fields the importer reads
  try {
    const Value v = Json::parseFile(dir + "/materials/chrome.json");
    CHECK(v.get("baseColor").size() == 4, "chrome: baseColor vec4");
    CHECK(v.get("metallic").asNum() > 0.9, "chrome: metallic");
    CHECK(v.get("roughness").asNum() < 0.2, "chrome: roughness");
  } catch (const std::exception& e) {
    CHECK(false, (std::string("material json: ") + e.what()).c_str());
  }

  // 6. shadertoy sources exist and split into the intended pass structure
  //    (the splitter is GL-free, so the importer's parsing is CI-verified;
  //    the header prose of both files mentions/nests "// pass:" text and
  //    must NOT create false markers)
  CHECK(std::filesystem::exists(dir + "/shadertoy/plasma.glsl"), "shadertoy: plasma.glsl");
  CHECK(std::filesystem::exists(dir + "/shadertoy/tunnel_warp.glsl"), "shadertoy: tunnel_warp.glsl");
  {
    std::ifstream f(dir + "/shadertoy/plasma.glsl");
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto p = splitShadertoyPasses(s);
    CHECK(p.size() == 1 && p[0].name == "image", "plasma.glsl: single image pass");
  }
  {
    std::ifstream f(dir + "/shadertoy/tunnel_warp.glsl");
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto p = splitShadertoyPasses(s);
    CHECK(p.size() == 3 && p[0].name == "common" && p[1].name == "buffer_a" && p[2].name == "image",
          "tunnel_warp.glsl: common/buffer_a/image passes");
    CHECK_NEAR(extractShadertoyRenderScale(s), 0.5f, 1e-6f, "tunnel_warp.glsl: renderScale 0.5");
  }
  {
    std::ifstream f(dir + "/shadertoy/plasma.glsl");
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_NEAR(extractShadertoyRenderScale(s), 1.0f, 1e-6f, "plasma.glsl: full-res default");
  }

  // 7. the OBJ models exist with the expected structure (v/vt/vn + faces)
  {
    std::ifstream f(dir + "/models/terrain.obj");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(s.find("v ") != std::string::npos, "terrain.obj: vertices");
    CHECK(s.find("vn ") != std::string::npos, "terrain.obj: normals");
    CHECK(s.find("f ") != std::string::npos, "terrain.obj: faces");
  }
  {
    std::ifstream f(dir + "/models/gem.obj");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t nFaces = std::count(s.begin(), s.end(), '\n');
    CHECK(s.find("f ") != std::string::npos && nFaces > 0, "gem.obj: faces");
  }

  // 8. the EXAMPLE production (data/examples/ExampleDemo.nsd) parses, builds
  //    into 4 sections with no unresolved shows, and only references effects
  //    + assets that ship with the engine - so a fresh clone can run it
  //    immediately (the request's "very small example production separate
  //    from Null Sector Demo Engine")
  {
    ScriptEngine ex;
    CHECK(ex.load(dir + "/examples/ExampleDemo.nsd"), "ExampleDemo.nsd loads");
    CHECK(ex.script().bpm == 140.0f, "ExampleDemo: tempo 140");
    TimelineEditor exTe;
    ex.build(exTe);
    CHECK((int)ex.scenes().size() == 4, "ExampleDemo: 4 scenes");
    CHECK((int)ex.sections().size() == 4, "ExampleDemo: 4 sections");
    CHECK(ex.unresolved().empty(), "ExampleDemo: no unresolved shows");
    CHECK(exTe.duration > 40.0f && exTe.duration < 44.0f, "ExampleDemo: duration ~42s");
    // every show target resolves to a scene or a built-in effect name
    const std::vector<std::string> known = {"intro", "shadertoy:plasma.glsl", "tunnel", "greetings"};
    for (const auto& b : ex.scenes()) {
      for (const auto& c : b.setup) {
        if (c.name == "show" && !c.args.empty()) {
          const std::string t = c.args[0].asStr();
          const bool isScene = ex.scene(t) != nullptr;
          const bool isEffect = std::find(known.begin(), known.end(), t) != known.end();
          CHECK(isScene || isEffect, ("ExampleDemo: show target resolves (" + t + ")").c_str());
        }
      }
    }
    // the shadertoy + greetings assets the example references exist on disk
    CHECK(std::filesystem::exists(dir + "/shadertoy/plasma.glsl"), "ExampleDemo: plasma.glsl shipped");
  }
}


// ---------------------------------------------------------------------------
// testVirtualPath - Phase 8 path rules: '/'-normalization, traversal rejection,
// join/parent/fileName helpers.
// ---------------------------------------------------------------------------
static void testVirtualPath() {
  CHECK(normalizeVirtualPath("data/demo.nsd") == "data/demo.nsd", "vpath: plain");
  CHECK(normalizeVirtualPath("data\\demo.nsd") == "data/demo.nsd", "vpath: backslash normalized");
  CHECK(normalizeVirtualPath("./data//demo.nsd") == "data/demo.nsd", "vpath: ./ and // collapsed");
  CHECK(normalizeVirtualPath("data/./demo.nsd") == "data/demo.nsd", "vpath: . segment dropped");
  CHECK(normalizeVirtualPath("../foo") == "", "vpath: leading .. rejected");
  CHECK(normalizeVirtualPath("../../secret") == "", "vpath: deep traversal rejected");
  CHECK(normalizeVirtualPath("a/../b") == "", "vpath: mid traversal rejected");
  CHECK(normalizeVirtualPath("/abs") == "", "vpath: absolute rejected");
  CHECK(normalizeVirtualPath("C:/data") == "", "vpath: drive letter rejected");
  CHECK(normalizeVirtualPath("a:b") == "", "vpath: colon rejected");
  CHECK(normalizeVirtualPath("") == "", "vpath: empty rejected");
  CHECK(normalizeVirtualPath("data/") == "data", "vpath: trailing slash trimmed");

  CHECK(isSafeVirtualPath("data/demo.nsd"), "safe: plain");
  CHECK(!isSafeVirtualPath("../x"), "safe: traversal unsafe");
  CHECK(!isSafeVirtualPath("data\\x"), "safe: backslash not canonical");

  CHECK(joinVirtualPath("data", "demo.nsd") == "data/demo.nsd", "join: simple");
  CHECK(joinVirtualPath("data/", "demo.nsd") == "data/demo.nsd", "join: trailing slash");
  CHECK(joinVirtualPath("data", "../x") == "", "join: traversal rejected");
  CHECK(parentVirtualPath("data/a/b.frag") == "data/a", "parent: nested");
  CHECK(parentVirtualPath("demo.nsd") == "", "parent: bare file");
  CHECK(fileNameVirtualPath("data/a/b.frag") == "b.frag", "fileName: nested");
  CHECK(fileNameVirtualPath("demo.nsd") == "demo.nsd", "fileName: bare");
}

// ---------------------------------------------------------------------------
// testDirectoryFS - DirectoryFileSystem over a scratch tree.
// ---------------------------------------------------------------------------
static void testDirectoryFS() {
  const std::string dir = "fw_vfs_tmp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir + "/sub", ec);
  {
    std::ofstream f(dir + "/hello.txt");
    f << "hello world";
  }
  {
    std::ofstream f(dir + "/sub/deep.txt");
    f << "deep content";
  }
  const std::vector<uint8_t> bin = {0x00, 0x01, 0xFE, 0xFF, 0x42};
  {
    std::ofstream f(dir + "/blob.bin", std::ios::binary);
    f.write((const char*)bin.data(), (std::streamsize)bin.size());
  }

  DirectoryFileSystem fs;
  fs.mount("data", dir);       // data/hello.txt -> <dir>/hello.txt
  fs.mount("", dir);           // catch-all root

  // exists
  CHECK(fs.exists("data/hello.txt"), "dfs: exists mounted");
  CHECK(fs.exists("hello.txt"), "dfs: exists catch-all");
  CHECK(!fs.exists("data/nope.txt"), "dfs: missing file");
  CHECK(!fs.exists("data/sub"), "dfs: dir is not a file");

  // reads
  CHECK(fs.readText("data/hello.txt") == "hello world", "dfs: text read");
  CHECK(fs.read("data/blob.bin") == bin, "dfs: binary read");
  CHECK(fs.readText("data/nope.txt").empty(), "dfs: missing text empty");
  CHECK(fs.read("data/nope.txt").empty(), "dfs: missing binary empty");

  // normalization
  CHECK(fs.readText("data//hello.txt") == "hello world", "dfs: double slash");
  CHECK(fs.readText("data\\hello.txt") == "hello world", "dfs: backslash read");

  // traversal rejection
  CHECK(!fs.exists("../hello.txt"), "dfs: traversal exists rejected");
  CHECK(fs.read("../hello.txt").empty(), "dfs: traversal read rejected");
  CHECK(!fs.exists("C:/windows/win.ini"), "dfs: absolute rejected");
  CHECK(fs.readText("..\\hello.txt").empty(), "dfs: backslash traversal rejected");

  // stat
  const VFileInfo st = fs.stat("data/blob.bin");
  CHECK(st.exists && !st.isDir && st.size == bin.size(), "dfs: stat file");
  const VFileInfo sd = fs.stat("data");
  CHECK(sd.exists && sd.isDir, "dfs: stat dir");
  CHECK(!fs.stat("data/nope.txt").exists, "dfs: stat missing");

  // list (full virtual paths)
  const std::vector<std::string> kids = fs.list("data");
  CHECK(kids.size() == 3, "dfs: list size");
  CHECK(std::find(kids.begin(), kids.end(), "data/blob.bin") != kids.end(), "dfs: list file");
  CHECK(std::find(kids.begin(), kids.end(), "data/sub") != kids.end(), "dfs: list dir");
  const std::vector<std::string> sub = fs.list("data/sub");
  CHECK(sub.size() == 1 && sub[0] == "data/sub/deep.txt", "dfs: list nested");

  // resolve() gives a real path for dev tooling
  const std::string real = fs.resolve("data/hello.txt");
  CHECK(real == dir + "/hello.txt", "dfs: resolve");
  CHECK(fs.resolve("../x").empty(), "dfs: resolve traversal empty");

  std::filesystem::remove_all(dir, ec);
}


// ---------------------------------------------------------------------------
// testFNV - known-vector regression for the FNV-1a 64 content hash (the .nsp
// integrity checksum). The offset basis is the canonical 14695981039346656037
// (0xcbf29ce484222325); the vectors below are the reference values from
// isthe.com/chongo/tech/comp/fnv.
// ---------------------------------------------------------------------------
static void testFNV() {
  CHECK(fnv1a64(nullptr, 0) == 14695981039346656037ull, "fnv: empty = offset basis");
  CHECK(fnv1a64(std::vector<uint8_t>()) == 14695981039346656037ull,
        "fnv: empty vector = offset basis (overload agrees)");

  auto H = [](const char* s) -> uint64_t {
    return fnv1a64((const uint8_t*)s, std::strlen(s));
  };
  CHECK(H("a") == 0xaf63dc4c8601ec8cull, "fnv: 'a'");
  CHECK(H("foobar") == 0x85944171f73967e8ull, "fnv: 'foobar'");
  CHECK(H("hello") == 0xa430d84680aabd0bull, "fnv: 'hello'");
  CHECK(H("chongo was here!") == 0x858e2fa32a55e61dull, "fnv: 'chongo was here!'");
  // 17 bytes incl. the trailing NUL (strlen cannot be used for this one)
  {
    static const uint8_t nulTerm[17] = {
      0x63, 0x68, 0x6f, 0x6e, 0x67, 0x6f, 0x20, 0x77, 0x61,
      0x73, 0x20, 0x68, 0x65, 0x72, 0x65, 0x21, 0x00};
    CHECK(fnv1a64(nulTerm, 17) == 0x46810f40eff60347ull, "fnv: embedded NUL");
  }
}

// ---------------------------------------------------------------------------
// testPackageFormat - write/read round trip + defensive validation.
// ---------------------------------------------------------------------------
static void testPackageFormat() {
  const std::string dir = "fw_vfs_tmp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string pkg = dir + "/test.nsp";

  // --- build a package: text, binary, empty, large
  std::vector<uint8_t> big(512 * 1024);
  for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 31 + 7);
  const std::vector<uint8_t> bin = {0x00, 0x01, 0xFE, 0xFF};
  {
    PackageWriter w;
    std::string err;
    CHECK(w.begin(pkg, &err), "pkg: begin");
    CHECK(w.addFile("data/demo.nsd", "demo \"TEST\" { bpm 120 }", &err), "pkg: add text");
    CHECK(w.addFile("shaders/a.frag", std::string("#version 330\n"), &err), "pkg: add shader");
    CHECK(w.addFile("assets/blob.bin", bin, &err), "pkg: add binary");
    CHECK(w.addFile("assets/empty.bin", std::vector<uint8_t>(), &err), "pkg: add empty");
    CHECK(w.addFile("assets/big.bin", big, &err), "pkg: add large");
    CHECK(!w.addFile("../escape.txt", "nope", &err), "pkg: traversal rejected at add");
    CHECK(w.setProduction("data/demo.nsd", &err), "pkg: set production");
    CHECK(!w.setProduction("data/missing.nsd", &err), "pkg: production must exist");
    CHECK(w.fileCount() == 5, "pkg: file count");
    CHECK(w.finish(&err), "pkg: finish");
  }

  // --- read it back
  {
    PackageReader r;
    std::string err;
    CHECK(r.open(pkg, &err), "pkg: open");
    CHECK(r.fileCount() == 6, "pkg: reader count (incl. marker)");
    CHECK(r.has("data/demo.nsd"), "pkg: has text");
    CHECK(!r.has("data/nope.nsd"), "pkg: missing absent");
    CHECK(r.readText("data/demo.nsd") == "demo \"TEST\" { bpm 120 }", "pkg: text content");
    CHECK(r.read("assets/blob.bin") == bin, "pkg: binary content");
    CHECK(r.read("assets/empty.bin").empty(), "pkg: empty content");
    CHECK(r.read("assets/big.bin") == big, "pkg: large content");
    CHECK(r.fileSize("assets/big.bin") == big.size(), "pkg: fileSize");
    CHECK(r.readText(".ns-production") == "data/demo.nsd", "pkg: production marker");
    CHECK(r.verifyAll(&err), "pkg: verifyAll clean");
    const std::vector<std::string> names = r.fileList();
    CHECK(names.size() == 6, "pkg: fileList size");
    CHECK(r.read("../escape.txt").empty(), "pkg: traversal read empty");
  }

  // --- defensive validation: corrupt copies of a valid package
  auto copyPkg = [&](const std::string& out) {
    std::filesystem::copy_file(pkg, out, ec);
    return out;
  };

  // bad magic
  {
    const std::string p = copyPkg(dir + "/bad_magic.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(0);
      f.write("XXXX", 4);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: bad magic rejected");
    CHECK(err.find("magic") != std::string::npos, "pkg: bad magic message");
  }

  // unsupported version
  {
    const std::string p = copyPkg(dir + "/bad_ver.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(4);
      const uint32_t v = 999;
      f.write((const char*)&v, 4);  // little-endian on this host (x86)
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: bad version rejected");
    CHECK(err.find("version") != std::string::npos, "pkg: bad version message");
  }

  // invalid offsets: manifestSize claims more than the file holds
  {
    const std::string p = copyPkg(dir + "/bad_off.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(24);  // manifestSize u64
      const uint64_t huge = 1ull << 40;
      f.write((const char*)&huge, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: invalid offset rejected");
  }

  // truncated package (cut in the middle of the data region)
  {
    const std::string p = dir + "/trunc.nsp";
    {
      std::ifstream in(pkg, std::ios::binary);
      const std::string data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      std::ofstream out(p, std::ios::binary | std::ios::trunc);
      out.write(data.data(), (std::streamsize)(data.size() / 2));
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: truncated rejected");
  }

  // checksum failure: flip one byte in a payload
  {
    const std::string p = copyPkg(dir + "/corrupt.nsp");
    {
      // big.bin dominates the package, so the file's midpoint is inside it
      const uint64_t half = (uint64_t)std::filesystem::file_size(p, ec) / 2;
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp((std::streamoff)half, std::ios::beg);
      f.write("X", 1);
    }
    PackageReader r;
    std::string err;
    CHECK(r.open(p, &err), "pkg: corrupt opens structurally");
    CHECK(!r.verifyAll(&err), "pkg: verifyAll catches corruption");
    CHECK(err.find("checksum") != std::string::npos, "pkg: checksum message");
    CHECK(r.read("assets/big.bin").empty(), "pkg: corrupt read rejected");
    CHECK(r.lastError().find("checksum") != std::string::npos, "pkg: read checksum message");
  }

  // invalid dataOffset in the header (points past EOF)
  {
    const std::string p = copyPkg(dir + "/bad_entry.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(32);  // dataOffset u64
      const uint64_t oob = 1ull << 40;
      f.write((const char*)&oob, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: out-of-range data offset rejected");
  }

  // overflow-safe checks: a manifestSize near 2^64 used to wrap in the old
  // manifestOffset + manifestSize addition - now rejected, not wrapped
  {
    const std::string p = copyPkg(dir + "/wrap_manifest.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(24);  // manifestSize u64
      const uint64_t huge = 0xFFFFFFFFFFFFFF00ull;
      f.write((const char*)&huge, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: overflowing manifestSize rejected");
    CHECK(err.find("manifest") != std::string::npos, "pkg: overflow names manifest");
  }

  // overflow-safe checks: an entry offset near 2^64 used to wrap in the old
  // en.offset + en.packedSize addition - now rejected, not wrapped
  {
    const std::string p = copyPkg(dir + "/wrap_entry.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      // first manifest entry (".ns-production" sorts first): its offset u64
      // sits at 48 + 4 + nameLen(15) padded to an 8-byte boundary
      const uint64_t offField = 48 + 4 + 15;
      const uint64_t padded = (offField + 7) & ~7ull;
      f.seekp((std::streamoff)padded);
      const uint64_t huge = 0xFFFFFFFFFFFFFF00ull;
      f.write((const char*)&huge, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: overflowing entry offset rejected");
    CHECK(err.find("offset") != std::string::npos, "pkg: overflow names offset");
  }

  // header flags must be zero for v1
  {
    const std::string p = copyPkg(dir + "/bad_flags.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(8);  // header flags u32
      const uint32_t flags = 1;
      f.write((const char*)&flags, 4);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: nonzero header flags rejected");
    CHECK(err.find("flags") != std::string::npos, "pkg: flags message");
  }

  // reserved field must be zero
  {
    const std::string p = copyPkg(dir + "/bad_reserved.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(40);  // reserved u64
      const uint64_t reserved = 1;
      f.write((const char*)&reserved, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: nonzero reserved field rejected");
  }

  // dataOffset must be 8-aligned
  {
    const std::string p = copyPkg(dir + "/bad_align.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(32);  // dataOffset u64
      uint64_t d = 0;
      f.read((char*)&d, 8);
      f.seekp(32);
      const uint64_t mis = d | 1;  // unalign the (aligned) writer's value
      f.write((const char*)&mis, 8);
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: misaligned dataOffset rejected");
  }

  // overlapping payload ranges are rejected (hand-crafted corruption):
  // walk the manifest and make the SECOND entry point at the FIRST entry's
  // payload, so their ranges [off, off+size) intersect
  {
    const std::string p = copyPkg(dir + "/overlap.nsp");
    {
      std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
      uint64_t pos = 48;  // manifestOffset
      uint64_t firstOff = 0, firstSize = 0;
      bool first = true;
      for (int i = 0; i < 2; i++) {
        uint32_t nl = 0;
        f.seekg((std::streamoff)pos);
        f.read((char*)&nl, 4);
        f.seekg(nl, std::ios::cur);
        pos = (pos + 4 + nl + 7) & ~7ull;
        if (first) {
          f.seekg((std::streamoff)pos);
          f.read((char*)&firstOff, 8);
          f.seekg((std::streamoff)(pos + 8));
          f.read((char*)&firstSize, 8);
          first = false;
        } else {
          f.seekp((std::streamoff)pos);  // overwrite the second offset
          f.write((const char*)&firstOff, 8);
        }
        pos += 40;
      }
      // sanity: the first entry actually has a payload (not an empty file)
      CHECK(firstSize > 0, "pkg: overlap first entry non-empty");
    }
    PackageReader r;
    std::string err;
    CHECK(!r.open(p, &err), "pkg: overlapping entries rejected");
    CHECK(err.find("overlap") != std::string::npos, "pkg: overlap message");
  }

  std::filesystem::remove_all(dir, ec);
}


// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// testPackageCompression - DEFLATE round-trip, keep-only-if-smaller, and the
// integrity hash covering the UNCOMPRESSED bytes.
// ---------------------------------------------------------------------------
static void testPackageCompression() {
  const std::string dir = "fw_vfs_tmp_comp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string pkg = dir + "/comp.nsp";

  // highly compressible blob (repeated lines of text)
  std::vector<uint8_t> text;
  {
    const std::string line = "the quick brown fox jumps over the lazy dog";
    text.reserve(line.size() * 20000);
    for (int i = 0; i < 20000; i++)
      text.insert(text.end(), line.begin(), line.end());
  }
  // incompressible-looking payload (LCG noise)
  std::vector<uint8_t> noise(65536);
  {
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < noise.size(); i++) {
      s = s * 1664525u + 1013904223u;
      noise[i] = (uint8_t)(s >> 24);
    }
  }

  {
    PackageWriter w;
    std::string err;
    CHECK(w.begin(pkg, &err), "comp: begin");
    CHECK(w.addFile("data/text.txt", text, &err, true), "comp: add text");
    CHECK(w.addFile("data/noise.bin", noise, &err, true), "comp: add noise");
    CHECK(w.addFile("data/small.txt", "hi", &err, true), "comp: add small");
    CHECK(w.addFile("data/empty.bin", std::vector<uint8_t>(), &err, true),
          "comp: add empty");
    CHECK(w.setProduction("data/text.txt", &err), "comp: marker");
    CHECK(w.finish(&err), "comp: finish");

    CHECK(w.stats().deflate.count == 1, "comp: one deflated entry");
    CHECK(w.stats().deflate.rawBytes == text.size(), "comp: deflate raw bytes");
    CHECK(w.stats().deflate.storedBytes < text.size(), "comp: deflate shrank");
    CHECK(w.stats().store.count == 4, "comp: four stored entries (incl. marker)");
    CHECK(w.stats().store.storedBytes ==
              noise.size() + 2 + std::string("data/text.txt").size(),
          "comp: stored bytes (noise + small + marker)");
  }

  {
    PackageReader r;
    std::string err;
    CHECK(r.open(pkg, &err), "comp: open");
    CHECK(r.method("data/text.txt") == kNspMethodDeflate, "comp: text method");
    CHECK(r.method("data/noise.bin") == kNspMethodStore, "comp: noise method");
    CHECK(r.method("data/small.txt") == kNspMethodStore, "comp: small method");
    CHECK(r.method("data/empty.bin") == kNspMethodStore, "comp: empty method");
    CHECK(r.read("data/text.txt") == text, "comp: text round-trip");
    CHECK(r.read("data/noise.bin") == noise, "comp: noise round-trip");
    CHECK(r.readText("data/small.txt") == "hi", "comp: small round-trip");
    CHECK(r.read("data/empty.bin").empty(), "comp: empty round-trip");
    CHECK(r.fileSize("data/text.txt") == text.size(),
          "comp: fileSize reports uncompressed size");
    CHECK(r.verifyAll(&err), "comp: verifyAll clean");
  }

  // corruption inside the COMPRESSED payload: the read must be rejected.
  // text.txt sorts last, so its deflate stream occupies the file tail.
  {
    const std::string cp = dir + "/comp_corrupt.nsp";
    std::filesystem::copy_file(pkg, cp, ec);
    const uint64_t sz = (uint64_t)std::filesystem::file_size(cp);
    {
      std::fstream f(cp, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp((std::streamoff)(sz - 16));
      char b = 0;
      f.read(&b, 1);
      b = (char)(b ^ 0x55);
      f.seekp((std::streamoff)(sz - 16));
      f.write(&b, 1);
    }
    PackageReader r;
    std::string err;
    CHECK(r.open(cp, &err), "comp: corrupt open");
    CHECK(r.read("data/text.txt").empty(), "comp: corrupt payload rejected");
    CHECK(!r.lastError().empty(), "comp: corrupt payload reports error");
  }

  std::filesystem::remove_all(dir, ec);
}


// testPackageFS - the .nsp mounted as a VirtualFileSystem.
// ---------------------------------------------------------------------------
static void testPackageFS() {
  const std::string dir = "fw_vfs_tmp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string pkg = dir + "/fs.nsp";
  {
    PackageWriter w;
    std::string err;
    CHECK(w.begin(pkg, &err), "pfs: begin");
    CHECK(w.addFile("data/demo.nsd", "demo \"FS\" { bpm 120 }", &err), "pfs: add nsd");
    CHECK(w.addFile("shaders/x.frag", "void main(){}", &err), "pfs: add frag");
    CHECK(w.addFile("data/sub/deep.txt", "deep", &err), "pfs: add nested");
    CHECK(w.setProduction("data/demo.nsd", &err), "pfs: marker");
    CHECK(w.finish(&err), "pfs: finish");
  }

  PackageFileSystem fs;
  std::string err;
  CHECK(fs.open(pkg, &err), "pfs: open");
  CHECK(fs.isPackage(), "pfs: isPackage");
  CHECK(!runtimeFSIsPackage(), "pfs: default runtime FS is not a package");

  CHECK(fs.exists("data/demo.nsd"), "pfs: exists");
  CHECK(!fs.exists("data/nope.nsd"), "pfs: missing");
  CHECK(fs.readText("data/demo.nsd") == "demo \"FS\" { bpm 120 }", "pfs: readText");
  CHECK(fs.productionScriptPath() == "data/demo.nsd", "pfs: productionScriptPath");

  const VFileInfo st = fs.stat("shaders/x.frag");
  CHECK(st.exists && !st.isDir && st.size == 13, "pfs: stat file");
  CHECK(st.mtime == 0.0, "pfs: packaged files are immutable");
  const VFileInfo sd = fs.stat("data");
  CHECK(sd.exists && sd.isDir, "pfs: stat dir");
  CHECK(!fs.stat("data/nope.nsd").exists, "pfs: stat missing");

  const std::vector<std::string> kids = fs.list("data");
  CHECK(kids.size() == 2, "pfs: list data");
  CHECK(std::find(kids.begin(), kids.end(), "data/demo.nsd") != kids.end(), "pfs: list file");
  CHECK(std::find(kids.begin(), kids.end(), "data/sub") != kids.end(), "pfs: list dir");
  const std::vector<std::string> sub = fs.list("data/sub");
  CHECK(sub.size() == 1 && sub[0] == "data/sub/deep.txt", "pfs: list nested");

  // directory index: root lists top-level dirs + bare files; nested dirs exist
  const std::vector<std::string> root = fs.list("");
  CHECK(std::find(root.begin(), root.end(), "data") != root.end(), "pfs: root has data");
  CHECK(std::find(root.begin(), root.end(), "shaders") != root.end(), "pfs: root has shaders");
  CHECK(fs.stat("data/sub").exists && fs.stat("data/sub").isDir, "pfs: nested dir exists");
  CHECK(!fs.stat("data/nope").exists, "pfs: missing dir absent");
  CHECK(fs.list("data/nope").empty(), "pfs: missing dir lists empty");

  CHECK(!fs.exists("../x"), "pfs: traversal rejected");
  CHECK(fs.read("../x").empty(), "pfs: traversal read empty");
  CHECK(!fs.exists("C:/windows/win.ini"), "pfs: absolute rejected");

  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// testDevPackageEquivalence - a production loaded from the dev tree and the
// same production from a package must resolve identical virtual paths with
// identical contents.
// ---------------------------------------------------------------------------
static void testDevPackageEquivalence() {
  const std::string dir = "fw_vfs_tmp";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string pkg = dir + "/equiv.nsp";

  // dev tree: mount the real data + shaders dirs the way main() does
  const std::string dataDir = NULLSECTOR_DATA_DIR;
  const std::string shaderDir = NULLSECTOR_SHADER_DIR;
  CHECK(std::filesystem::is_directory(dataDir), "equiv: data dir exists");
  CHECK(std::filesystem::is_directory(shaderDir), "equiv: shader dir exists");

  // representative production references (avoid the 61MB soundtrack in tests)
  const char* refs[] = {
      "data/demo.nsd",
      "shaders/compose.frag",
      "shaders/bloom_extract.frag",
      "data/materials/chrome.json",
      "data/post/clean.json",
  };
  DirectoryFileSystem dev;
  dev.mount("data", dataDir);
  dev.mount("shaders", shaderDir);

  // package the references
  {
    PackageWriter w;
    std::string err;
    CHECK(w.begin(pkg, &err), "equiv: begin");
    for (const char* ref : refs) {
      const std::vector<uint8_t> bytes = dev.read(ref);
      CHECK(!bytes.empty(), ("equiv: dev has " + std::string(ref)).c_str());
      CHECK(w.addFile(ref, bytes, &err), "equiv: add");
    }
    CHECK(w.setProduction("data/demo.nsd", &err), "equiv: marker");
    CHECK(w.finish(&err), "equiv: finish");
  }

  // load the package as a VFS and compare path-by-path
  PackageFileSystem pfs;
  std::string err;
  CHECK(pfs.open(pkg, &err), "equiv: open");
  for (const char* ref : refs) {
    CHECK(pfs.exists(ref), ("equiv: pkg has " + std::string(ref)).c_str());
    CHECK(pfs.read(ref) == dev.read(ref),
          ("equiv: contents match " + std::string(ref)).c_str());
    CHECK(pfs.stat(ref).size == dev.stat(ref).size,
          ("equiv: sizes match " + std::string(ref)).c_str());
  }
  // the demo.nsd production parses identically from both
  {
    ScriptEngine devEx, pkgEx;
    CHECK(devEx.loadText(dev.readText("data/demo.nsd"), "data/demo.nsd"),
          "equiv: dev script loads");
    CHECK(pkgEx.loadText(pfs.readText("data/demo.nsd"), "data/demo.nsd"),
          "equiv: pkg script loads");
    CHECK(devEx.script().title == pkgEx.script().title, "equiv: same title");
    CHECK((int)devEx.scenes().size() == (int)pkgEx.scenes().size(),
          "equiv: same scene count");
  }

  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// testNsdRoundTrip - the writer (nsdSerialize) must be the exact inverse of
// the parser: parse -> serialize -> parse must be structurally identical, and
// serialize must be idempotent (stable output).
// ---------------------------------------------------------------------------
namespace {
bool valueEqual(const Value& a, const Value& b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case Value::Type::Null: return true;
    case Value::Type::Bool: return a.asBool() == b.asBool();
    case Value::Type::Num: return a.asNum() == b.asNum();
    case Value::Type::Str: return a.asStr() == b.asStr();
    case Value::Type::Arr: {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); i++)
        if (!valueEqual(a.atIndex(i), b.atIndex(i))) return false;
      return true;
    }
    case Value::Type::Obj: {
      const auto& oa = a.asObj();
      const auto& ob = b.asObj();
      if (oa.size() != ob.size()) return false;
      for (size_t i = 0; i < oa.size(); i++) {
        if (oa[i].first != ob[i].first) return false;
        if (!valueEqual(oa[i].second, ob[i].second)) return false;
      }
      return true;
    }
  }
  return false;
}
bool cmdEqual(const Cmd& a, const Cmd& b) {
  if (a.name != b.name) return false;
  if (a.args.size() != b.args.size()) return false;
  for (size_t i = 0; i < a.args.size(); i++)
    if (!valueEqual(a.args[i], b.args[i])) return false;
  if (!valueEqual(a.opts, b.opts)) return false;
  if (a.keys.size() != b.keys.size()) return false;
  for (size_t i = 0; i < a.keys.size(); i++) {
    if (a.keys[i].t != b.keys[i].t) return false;
    if (a.keys[i].interp != b.keys[i].interp) return false;
    if (!valueEqual(a.keys[i].v, b.keys[i].v)) return false;
  }
  return true;
}
bool blockEqual(const ScriptBlock& a, const ScriptBlock& b) {
  if (a.time != b.time) return false;
  if (a.cmds.size() != b.cmds.size()) return false;
  for (size_t i = 0; i < a.cmds.size(); i++)
    if (!cmdEqual(a.cmds[i], b.cmds[i])) return false;
  return true;
}
bool scriptEqual(const Script& a, const Script& b) {
  if (a.title != b.title || a.bpm != b.bpm || a.duration != b.duration) return false;
  if (a.scenes.size() != b.scenes.size()) return false;
  for (size_t i = 0; i < a.scenes.size(); i++) {
    const SceneDef& x = a.scenes[i];
    const SceneDef& y = b.scenes[i];
    if (x.name != y.name || x.title != y.title || x.bars != y.bars ||
        x.duration != y.duration || x.intensity != y.intensity ||
        x.chapter != y.chapter || x.visible != y.visible) return false;
    if (x.setup.size() != y.setup.size()) return false;
    for (size_t k = 0; k < x.setup.size(); k++)
      if (!cmdEqual(x.setup[k], y.setup[k])) return false;
    if (x.blocks.size() != y.blocks.size()) return false;
    for (size_t k = 0; k < x.blocks.size(); k++)
      if (!blockEqual(x.blocks[k], y.blocks[k])) return false;
  }
  if (a.main.size() != b.main.size()) return false;
  for (size_t i = 0; i < a.main.size(); i++)
    if (!blockEqual(a.main[i], b.main[i])) return false;
  return true;
}
}  // namespace

static void testNsdRoundTrip() {
  const std::string src =
      "demo \"TEST SHOW\" {\n"
      "    bpm 140\n"
      "    duration 60\n"
      "}\n"
      "scene Intro {\n"
      "    bars 8  intensity 0.3  chapter 0\n"
      "    title \"Opening\"\n"
      "    camera IntroCam { rig static; pos (0,0,2.4); fov 55 }\n"
      "    show intro\n"
      "    play music\n"
      "    text caption { text HELLO WORLD pos (0,0,0); size 32 }\n"
      "    at 4 { anim introBloom post.bloom smooth { 0 1.2; 3 2.4; 6 1.5 } }\n"
      "}\n"
      "scene Nave {\n"
      "    bars 4  intensity 0.55  chapter 1  visible false\n"
      "    duration 12\n"
      "    at 2 { marker INSIDE }\n"
      "}\n"
      "at 0 { show Intro; marker START }\n"
      "at 1:05.5 { camera NaveCam { rig drift; pos (1,2,3) }; fade in 2 }\n"
      "at 30 { anim camPos camera.pos cubic { 0 (0,0,4); 8 (2,0,4) linear; 16 (0,0,2) ease-in-out } }\n"
      "at 45 { text \"HELLO WORLD\" { color (1,0.5,0.2,1); size 32 } }\n";
  const Script a = ScriptParser::parse(src, "t");
  CHECK(a.scenes.size() == 2, "roundtrip: 2 scenes parsed");
  CHECK(a.main.size() == 4, "roundtrip: 4 main blocks parsed");
  bool parsedBareText = false;
  for (const auto& cmd : a.scenes[0].setup) {
    if (cmd.name == "text" && cmd.opts.get("text").asStr() == "HELLO WORLD") {
      parsedBareText = true;
      break;
    }
  }
  CHECK(parsedBareText, "roundtrip: unquoted text with spaces parses");
  const std::string out1 = nsdSerialize(a);
  const Script b = ScriptParser::parse(out1, "t1");
  CHECK(scriptEqual(a, b), "roundtrip: parse(serialize(parse(x))) == parse(x)");
  const std::string out2 = nsdSerialize(b);
  CHECK(out1 == out2, "roundtrip: serialize is idempotent");
  CHECK(nsdSerializeCmd(b.main[2].cmds[0]) ==
            nsdSerializeCmd(a.main[2].cmds[0]),
        "roundtrip: per-command serialization stable");

  // the REAL productions must survive the round-trip too (guarded by file
  // existence so CI without the data tree stays green)
  const std::string dataDir = NULLSECTOR_DATA_DIR;
  const char* prods[] = {"demo.nsd", "neural_dust.nsd", "example.nsd"};
  for (const char* prod : prods) {
    const std::string path = dataDir + "/" + prod;
    if (!std::filesystem::exists(path)) continue;
    const Script p1 = ScriptParser::parse(
        [] (const std::string& f) {
          std::ifstream in(f, std::ios::binary);
          std::ostringstream ss;
          ss << in.rdbuf();
          return ss.str();
        }(path), path);
    const Script p2 = ScriptParser::parse(nsdSerialize(p1), path + ".s");
    CHECK(scriptEqual(p1, p2), "roundtrip: production survives serialize");
    CHECK(nsdSerialize(p1) == nsdSerialize(p2),
          "roundtrip: production serialization is idempotent");
  }
}

// ---------------------------------------------------------------------------
// testEditorDocument - the document model: marker ops, dirty state, undo.
// ---------------------------------------------------------------------------
static void testEditorDocument() {
  EditorDocument d;
  const std::string src =
      "demo \"T\" {\n"
      "    bpm 120\n"
      "}\n"
      "scene A {\n"
      "    bars 4\n"
      "    at 2 { marker INSIDE }\n"
      "}\n"
      "at 5 { show A }\n"
      "at 10 { marker M2 }\n"
      "at 12 { marker M3 }\n";
  d.adopt(ScriptParser::parse(src, "t"), "data/_fw_test_doc.nsd");
  CHECK(!d.dirty, "doc: fresh adopt is clean");
  CHECK(!d.canUndo() && !d.canRedo(), "doc: fresh adopt has no undo/redo");

  CHECK(d.findMarker("INSIDE") == nullptr, "doc: scene marker not in main");
  CHECK(d.findMarker("M2") != nullptr && d.findMarker("M3") != nullptr,
        "doc: main markers found");

  // add (duplicate rejected)
  d.beginEdit("add marker");
  CHECK(d.addMarker("NEW", 11.0f), "doc: addMarker");
  CHECK(!d.addMarker("NEW", 1.0f), "doc: duplicate marker rejected");
  d.endEdit();
  CHECK(d.dirty, "doc: edit marks dirty");
  CHECK(d.findMarker("NEW") != nullptr && d.findMarker("NEW")->time == 11.0f,
        "doc: new marker present at 11s");
  CHECK(d.markerNames().size() == 3, "doc: 3 markers after add");
  for (size_t i = 1; i < d.ast.main.size(); i++)
    CHECK(d.ast.main[i - 1].time <= d.ast.main[i].time, "doc: main sorted");

  // move a marker that SHARES its block (split it by adding another command)
  d.ast.main[2].cmds.push_back(Cmd{"show", {}, Value::object(), {}});
  d.beginEdit("move marker");
  CHECK(d.moveMarker("M2", 20.0f), "doc: moveMarker");
  d.endEdit();
  ScriptBlock* b = d.findMarker("M2");
  CHECK(b != nullptr && b->time == 20.0f, "doc: M2 moved to 20s");

  // rename
  d.beginEdit("rename marker");
  CHECK(d.renameMarker("M3", "M3X"), "doc: renameMarker");
  CHECK(!d.renameMarker("M3", "M3X"), "doc: rename of missing marker fails");
  CHECK(!d.renameMarker("M3X", "NEW"), "doc: rename onto a taken name fails");
  d.endEdit();
  CHECK(d.findMarker("M3X") != nullptr && d.findMarker("M3") == nullptr,
        "doc: renamed");

  // remove
  d.beginEdit("delete marker");
  CHECK(d.removeMarker("M3X"), "doc: removeMarker");
  d.endEdit();
  CHECK(d.findMarker("M3X") == nullptr, "doc: M3X gone");

  // undo: back to before the remove (M3X present again)
  d.undo();
  CHECK(d.findMarker("M3X") != nullptr, "doc: undo restores removed marker");
  CHECK(d.dirty, "doc: undo keeps dirty (document changed since save)");
  d.undo();
  CHECK(d.findMarker("M3") != nullptr && d.findMarker("M3X") == nullptr,
        "doc: undo restores the rename");
  CHECK(d.canRedo(), "doc: redo available");
  d.redo();
  CHECK(d.findMarker("M3X") != nullptr, "doc: redo re-applies the rename");

  // no-op gesture leaves no undo entry
  d.beginEdit("move marker");
  d.endEdit();  // nothing changed -> no entry
  CHECK(d.canUndo(), "doc: no-op gesture did not consume undo");

  // save clears dirty + the file round-trips
  const std::string before = d.serialize();
  d.dirty = true;
  CHECK(d.save(), "doc: save writes the file");
  CHECK(!d.dirty, "doc: save clears dirty");
  const Script reparsed = ScriptParser::parse(before, "saved");
  CHECK(scriptEqual(d.ast, reparsed), "doc: saved file re-parses to the doc");

  std::remove("data/_fw_test_doc.nsd");
}

// testFlipRowsInPlace - glReadPixels returns rows bottom-up; rawvideo wants
// top-down, so every exported frame is flipped. Verify the in-place swap.
static void testFlipRowsInPlace() {
  // 2x3 image: row 0 red, row 1 blue (each row = 3 px * 3 ch = 9 bytes)
  unsigned char img[18];
  for (int i = 0; i < 3; i++) { img[i * 3] = 255; img[i * 3 + 1] = 0; img[i * 3 + 2] = 0; }
  for (int i = 0; i < 3; i++) { img[9 + i * 3] = 0; img[9 + i * 3 + 1] = 0; img[9 + i * 3 + 2] = 255; }
  flipRowsInPlace(img, 3, 2);
  CHECK(img[0] == 0 && img[2] == 255, "flip: old row 1 (blue) now on top");
  CHECK(img[9] == 255 && img[11] == 0, "flip: old row 0 (red) now on bottom");

  // odd height (3 rows) + width 1: middle row must stay put
  unsigned char img2[9] = {1, 0, 0, 2, 0, 0, 3, 0, 0};  // rows 0,1,2
  flipRowsInPlace(img2, 1, 3);
  CHECK(img2[0] == 3 && img2[3] == 2 && img2[6] == 1, "flip: odd height swaps ends, center stays");

  // degenerate: 1 row and empty sizes are no-ops (must not crash)
  unsigned char img3[3] = {9, 9, 9};
  flipRowsInPlace(img3, 1, 1);
  CHECK(img3[0] == 9, "flip: single row unchanged");
  flipRowsInPlace(img3, 0, 0);
  CHECK(img3[0] == 9, "flip: zero size no-op");
}

// testFfmpegCaptureCmd - the shared ffmpeg command builder used by both
// the CLI --export-mp4 path and the editor's File > Export MP4... action.
static void testFfmpegCaptureCmd() {
  const std::string c = buildFfmpegCaptureCmd("out.mp4", 640, 360, 60.0f, "");
  CHECK(c.find("rawvideo") != std::string::npos, "ffmpeg rawvideo input");
  CHECK(c.find("-pix_fmt rgb24") != std::string::npos, "ffmpeg rgb24 pixels");
  CHECK(c.find("-s 640x360") != std::string::npos, "ffmpeg size");
  CHECK(c.find("-r 60 ") != std::string::npos, "ffmpeg fps");
  CHECK(c.find("libx264") != std::string::npos, "h264 encoder");
  CHECK(c.find("-movflags +faststart") != std::string::npos, "faststart");
  CHECK(c.find("\"out.mp4\"") != std::string::npos, "quoted output path");
  CHECK(c.find("-i \"") == std::string::npos, "no audio input without a track");
  CHECK(c.find("-shortest") == std::string::npos, "no -shortest without audio");

  const std::string a =
      buildFfmpegCaptureCmd("o u t.mp4", 1920, 1080, 59.94f, "data/track.wav");
  CHECK(a.find("-i \"data/track.wav\"") != std::string::npos, "audio input");
  CHECK(a.find("-c:a aac -b:a 192k") != std::string::npos, "aac audio encode");
  CHECK(a.find("-shortest") != std::string::npos, "-shortest mux");
  CHECK(a.find("\"o u t.mp4\"") != std::string::npos, "spaced path quoted");
  CHECK(a.find("-r 59.94") != std::string::npos, "fractional fps");
}

}  // namespace ns

static void runAll() {
  ns::testJson();
  ns::testScriptParser();
  ns::testScriptDiagnostics();
  ns::testScriptEngineAndTimeline();
  ns::testAnimation();
  ns::testSceneGraph();
  ns::testCameraRig();
  ns::testAssetManager();
  ns::testFileWatcher();
  ns::testLiveReloadChain();
  ns::testShadertoyParser();
  ns::testLogSink();
  ns::testValueStrings();
  ns::testGpuTimeStats();
  ns::testDemoData();
  ns::testVirtualPath();
  ns::testDirectoryFS();
  ns::testFNV();
  ns::testPackageFormat();
  ns::testPackageCompression();
  ns::testPackageFS();
  ns::testDevPackageEquivalence();
  ns::testNsdRoundTrip();
  ns::testEditorDocument();
  ns::testFfmpegCaptureCmd();
  ns::testFlipRowsInPlace();
}

int main() {
  try {
    runAll();
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "UNCAUGHT EXCEPTION: %s\n", ex.what());
    return 2;
  } catch (...) {
    std::fprintf(stderr, "UNCAUGHT NON-STD EXCEPTION\n");
    return 2;
  }
  std::printf("framework tests: %d passed, %d failed\n", ns::g_passed, ns::g_failed);
  return ns::g_failed == 0 ? 0 : 1;
}
