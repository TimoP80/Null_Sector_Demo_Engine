// ---------------------------------------------------------------------------
// SceneGraph - a proper scene graph for the data-driven director.
//
//   hierarchy        parent/child trees with local + world transforms
//   transforms       local position / rotation (quaternion) / scale; world
//                    matrices recomputed lazily down the dirty chain
//   visibility       visible + enabled (enabled propagates to children)
//   tags / layers    query helpers for selective rendering / updates
//
// Node types: Empty, Camera, Light, Mesh, Particles, Quad (shader fullscreen),
// Sprite, Text, PostEffect, TimelineSystem. The engine-side renderers read the
// payloads; nothing here touches GL.
//
// Serialization: the whole graph round-trips through JSON (scene files).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/math.hpp"
#include "framework/core/value.hpp"

#include <array>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ns {

// --- node payloads ------------------------------------------------------------

enum class NodeType : int {
  Empty = 0, Camera, Light, Mesh, Particles, Quad, Sprite, Video, Text, Post, TimelineSystem,
};

const char* nodeTypeName(NodeType t);
NodeType nodeTypeFromName(const std::string& n);

struct CamData {
  float fov = 62.0f, nearP = 0.05f, farP = 400.0f;
  V3 target{0, 0, 0};
  std::string rig;    // camera rig name (see framework/camera/camerarig.hpp)
};

struct LightData {
  std::string type = "point";   // directional | point | spot
  std::array<float, 3> color{1, 1, 1};
  float intensity = 1.0f;
  float range = 10.0f;
  float angle = 45.0f;          // spot cone, degrees
  bool castShadow = false;
};

struct MeshData {
  std::string model;      // path relative to the model dir (data/models/...)
  std::string material;   // named material (data/materials/*.json)
  float scale = 1.0f;
  bool lit = true;
};

struct ParticleData {
  std::string vert = "particles.vert";
  std::string frag = "particles.frag";
  std::string prev = "particles_prev.frag";
  int count = 5000;
  float renderScale = 1.0f;
};

struct QuadData {
  std::string frag;       // fragment shader file (fullscreen.vert implied)
  bool handoff = false;   // in-scene transition handoff (uPrevScene)
  float renderScale = 1.0f;
  float mode = 0.0f;      // per-scene uMode uniform
};

struct SpriteData {
  std::string tex;                    // texture path
  std::array<float, 4> color{1, 1, 1, 1};
  float opacity = 1.0f;
  V3 size{1, 1, 1};                   // quad size in world units
};

struct VideoData {
  std::string file;                   // video path, normally data/video/...
  int width = 1280;
  int height = 720;
  float fps = 30.0f;
  bool loop = true;
  float opacity = 1.0f;
  V3 size{1, 1, 1};
};

struct TextData {
  std::string text;
  int sizePx = 24;
  std::string font;                   // "" = default engine font
  std::string style = "neon";         // terminal | holo | glitch | neon | scan | dissolve | chrome | outline
  std::array<float, 4> color{1, 1, 1, 1};
  float opacity = 1.0f;
  float align = -1.0f;                // -1 center, 0..1 from left, 2 right
};

struct PostData {
  std::string preset;   // post preset name (data/post/*.json)
};

struct TimelineData {
  std::string file;     // timeline file (data/timelines/*.json)
};

using NodePayload = std::variant<std::monostate, CamData, LightData, MeshData,
                                 ParticleData, QuadData, SpriteData, VideoData, TextData,
                                 PostData, TimelineData>;

// --- node ----------------------------------------------------------------------

class SceneGraph;

class SceneNode {
public:
  SceneNode() = default;
  SceneNode(std::string n, NodeType t, NodePayload p = {});

  // identity / structure
  std::string name;
  NodeType type = NodeType::Empty;
  NodePayload payload;
  SceneNode* parent = nullptr;
  std::vector<std::unique_ptr<SceneNode>> children;

  // local transform
  V3 pos{0, 0, 0};
  Q4 rot{0, 0, 0, 1};
  V3 scale{1, 1, 1};

  // visibility / selection
  bool visible = true;
  bool enabled = true;
  std::vector<std::string> tags;
  int layer = 0;

  // computed
  Mat4 local{};   // TRS
  Mat4 world{};   // local * parent->world
  bool dirty = true;

  // --- structure ----------------------------------------------------------------
  SceneNode* addChild(std::unique_ptr<SceneNode> child);
  SceneNode* find(const std::string& n);           // DFS by name
  const SceneNode* find(const std::string& n) const;
  std::vector<SceneNode*> findTag(const std::string& tag);
  void removeChild(const std::string& n);

  // --- transforms ---------------------------------------------------------------
  void setPos(const V3& p) { pos = p; dirty = true; }
  void setRot(const Q4& q) { rot = q; dirty = true; }
  void setScale(const V3& s) { scale = s; dirty = true; }
  void setScale(float s) { scale = {s, s, s}; dirty = true; }
  /** euler XYZ degrees */
  void setEuler(const V3& deg);

  /** recompute local matrix from TRS */
  void computeLocal();
  /** recompute world matrix; returns true when this node's matrix changed */
  bool updateWorld(bool parentDirty);

  /** enabled AND all ancestors enabled AND visible */
  bool isActive() const;

  // --- typed access --------------------------------------------------------------
  CamData* asCamera();
  LightData* asLight();
  MeshData* asMesh();
  ParticleData* asParticles();
  QuadData* asQuad();
  SpriteData* asSprite();
  VideoData* asVideo();
  TextData* asText();
  PostData* asPost();
  TimelineData* asTimeline();

  // --- serialization ---------------------------------------------------------------
  Value toJson() const;
  static std::unique_ptr<SceneNode> fromJson(const Value& v);

private:
  void toJsonInto(Value& o) const;
};

// --- graph ------------------------------------------------------------------------

class SceneGraph {
public:
  SceneGraph();

  SceneNode* root() { return root_.get(); }
  const SceneNode* root() const { return root_.get(); }

  /** create a node of the given type and attach it (name must be unique) */
  SceneNode* addNode(std::string name, NodeType type, NodePayload payload = {},
                     SceneNode* parent = nullptr);

  SceneNode* find(const std::string& n) { return root_->find(n); }
  const SceneNode* find(const std::string& n) const { return root_->find(n); }

  /** recompute all world matrices (call once per frame) */
  void update();

  /** visit every node (depth-first, parents first) */
  template <typename F>
  void walk(F&& fn) {
    walkNode(root_.get(), fn);
  }
  template <typename F>
  void walkNode(SceneNode* n, F& fn) {
    fn(n);
    for (auto& c : n->children) walkNode(c.get(), fn);
  }

  /** all active nodes of a type (for the director's per-frame dispatch) */
  std::vector<SceneNode*> nodesOf(NodeType t, bool activeOnly = true);

  /** every node carrying a given tag (across the whole tree) */
  std::vector<SceneNode*> findTag(const std::string& tag);

  Value toJson() const;
  void fromJson(const Value& v);
  void clear();

private:
  std::unique_ptr<SceneNode> root_;
};

}  // namespace ns
