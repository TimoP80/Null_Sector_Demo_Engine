#include "framework/scene/scenegraph.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace ns {

// ---------------------------------------------------------------------------
// matrix helpers (column-major, GL convention)
// ---------------------------------------------------------------------------
static Mat4 mat4Identity() {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  return m;
}
static Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
  Mat4 m{};
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float v = 0;
      for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
      m[c * 4 + r] = v;
    }
  }
  return m;
}
static Mat4 mat4Translate(const V3& t) {
  Mat4 m = mat4Identity();
  m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
  return m;
}
static Mat4 mat4Scale(const V3& s) {
  Mat4 m = mat4Identity();
  m[0] = s[0]; m[5] = s[1]; m[10] = s[2];
  return m;
}
static Mat4 mat4FromQuat(const Q4& qin) {
  const Q4 q = qNorm(qin);
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat4 m = mat4Identity();
  m[0] = 1 - 2 * (y * y + z * z);
  m[1] = 2 * (x * y + z * w);
  m[2] = 2 * (x * z - y * w);
  m[4] = 2 * (x * y - z * w);
  m[5] = 1 - 2 * (x * x + z * z);
  m[6] = 2 * (y * z + x * w);
  m[8] = 2 * (x * z + y * w);
  m[9] = 2 * (y * z - x * w);
  m[10] = 1 - 2 * (x * x + y * y);
  return m;
}
static Q4 eulerToQuat(const V3& deg) {
  const float rx = deg[0] * 3.14159265f / 180.0f;
  const float ry = deg[1] * 3.14159265f / 180.0f;
  const float rz = deg[2] * 3.14159265f / 180.0f;
  const float cx = std::cos(rx / 2), sx = std::sin(rx / 2);
  const float cy = std::cos(ry / 2), sy = std::sin(ry / 2);
  const float cz = std::cos(rz / 2), sz = std::sin(rz / 2);
  return Q4{sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
}

// ---------------------------------------------------------------------------
// node type names
// ---------------------------------------------------------------------------
const char* nodeTypeName(NodeType t) {
  switch (t) {
    case NodeType::Empty: return "empty";
    case NodeType::Camera: return "camera";
    case NodeType::Light: return "light";
    case NodeType::Mesh: return "mesh";
    case NodeType::Particles: return "particles";
    case NodeType::Quad: return "quad";
    case NodeType::Sprite: return "sprite";
    case NodeType::Text: return "text";
    case NodeType::Post: return "post";
    case NodeType::TimelineSystem: return "timeline";
  }
  return "empty";
}

NodeType nodeTypeFromName(const std::string& n) {
  if (n == "camera") return NodeType::Camera;
  if (n == "light") return NodeType::Light;
  if (n == "mesh") return NodeType::Mesh;
  if (n == "particles") return NodeType::Particles;
  if (n == "quad") return NodeType::Quad;
  if (n == "sprite") return NodeType::Sprite;
  if (n == "text") return NodeType::Text;
  if (n == "post") return NodeType::Post;
  if (n == "timeline") return NodeType::TimelineSystem;
  return NodeType::Empty;
}

// ---------------------------------------------------------------------------
// SceneNode
// ---------------------------------------------------------------------------
SceneNode::SceneNode(std::string n, NodeType t, NodePayload p)
    : name(std::move(n)), type(t), payload(std::move(p)) {}

SceneNode* SceneNode::addChild(std::unique_ptr<SceneNode> child) {
  child->parent = this;
  child->dirty = true;
  children.push_back(std::move(child));
  return children.back().get();
}

SceneNode* SceneNode::find(const std::string& n) {
  if (name == n) return this;
  for (auto& c : children) {
    if (SceneNode* f = c->find(n)) return f;
  }
  return nullptr;
}

const SceneNode* SceneNode::find(const std::string& n) const {
  if (name == n) return this;
  for (const auto& c : children) {
    if (const SceneNode* f = c->find(n)) return f;
  }
  return nullptr;
}

std::vector<SceneNode*> SceneNode::findTag(const std::string& tag) {
  std::vector<SceneNode*> out;
  if (std::find(tags.begin(), tags.end(), tag) != tags.end()) out.push_back(this);
  for (auto& c : children) {
    auto sub = c->findTag(tag);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

void SceneNode::removeChild(const std::string& n) {
  children.erase(std::remove_if(children.begin(), children.end(),
                                [&](const std::unique_ptr<SceneNode>& c) { return c->name == n; }),
                 children.end());
}

void SceneNode::setEuler(const V3& deg) {
  rot = eulerToQuat(deg);
  dirty = true;
}

void SceneNode::computeLocal() {
  const Mat4 t = mat4Translate(pos);
  const Mat4 r = mat4FromQuat(rot);
  const Mat4 s = mat4Scale(scale);
  local = mat4Mul(t, mat4Mul(r, s));
}

bool SceneNode::updateWorld(bool parentDirty) {
  const bool mine = dirty || parentDirty;
  if (mine) {
    computeLocal();
    world = parent ? mat4Mul(parent->world, local) : local;
    dirty = false;
  }
  for (auto& c : children) c->updateWorld(mine);
  return mine;
}

bool SceneNode::isActive() const {
  const SceneNode* n = this;
  while (n) {
    if (!n->enabled || !n->visible) return false;
    n = n->parent;
  }
  return true;
}

// --- typed access ---------------------------------------------------------------
#define NS_ACCESSOR(TYPE, NAME, STRUCT)                       \
  STRUCT* SceneNode::NAME() {                                 \
    return std::holds_alternative<STRUCT>(payload) ? &std::get<STRUCT>(payload) : nullptr; \
  }
NS_ACCESSOR(camera, asCamera, CamData)
NS_ACCESSOR(light, asLight, LightData)
NS_ACCESSOR(mesh, asMesh, MeshData)
NS_ACCESSOR(particles, asParticles, ParticleData)
NS_ACCESSOR(quad, asQuad, QuadData)
NS_ACCESSOR(sprite, asSprite, SpriteData)
NS_ACCESSOR(text, asText, TextData)
NS_ACCESSOR(post, asPost, PostData)
NS_ACCESSOR(timeline, asTimeline, TimelineData)
#undef NS_ACCESSOR

// --- serialization ---------------------------------------------------------------
static Value vecToJson(const std::array<float, 3>& v) {
  Value a = Value::array();
  a.push(Value((double)v[0]));
  a.push(Value((double)v[1]));
  a.push(Value((double)v[2]));
  return a;
}
static Value vec4ToJson(const std::array<float, 4>& v) {
  Value a = Value::array();
  a.push(Value((double)v[0]));
  a.push(Value((double)v[1]));
  a.push(Value((double)v[2]));
  a.push(Value((double)v[3]));
  return a;
}
static std::array<float, 3> jsonToVec3(const Value& v) {
  std::array<float, 3> out{0, 0, 0};
  for (int i = 0; i < 3; i++) out[i] = v.atIndex((size_t)i).asFloat(0);
  return out;
}
static std::array<float, 4> jsonToVec4(const Value& v) {
  std::array<float, 4> out{0, 0, 0, 0};
  for (int i = 0; i < 4; i++) out[i] = v.atIndex((size_t)i).asFloat(0);
  return out;
}

void SceneNode::toJsonInto(Value& o) const {
  o.set("name") = Value(name);
  o.set("type") = Value(nodeTypeName(type));
  o.set("pos") = vecToJson(pos);
  o.set("rot") = vec4ToJson(rot);
  o.set("scale") = vecToJson(scale);
  o.set("visible") = Value(visible);
  o.set("enabled") = Value(enabled);
  o.set("layer") = Value(layer);
  Value tg = Value::array();
  for (const auto& t : tags) tg.push(Value(t));
  o.set("tags") = std::move(tg);

  switch (type) {
    case NodeType::Camera: {
      const CamData& d = std::get<CamData>(payload);
      Value p = Value::object();
      p.set("fov") = Value((double)d.fov);
      p.set("near") = Value((double)d.nearP);
      p.set("far") = Value((double)d.farP);
      p.set("target") = vecToJson(d.target);
      p.set("rig") = Value(d.rig);
      o.set("camera") = std::move(p);
      break;
    }
    case NodeType::Light: {
      const LightData& d = std::get<LightData>(payload);
      Value p = Value::object();
      p.set("type") = Value(d.type);
      p.set("color") = vecToJson(d.color);
      p.set("intensity") = Value((double)d.intensity);
      p.set("range") = Value((double)d.range);
      p.set("angle") = Value((double)d.angle);
      p.set("shadow") = Value(d.castShadow);
      o.set("light") = std::move(p);
      break;
    }
    case NodeType::Mesh: {
      const MeshData& d = std::get<MeshData>(payload);
      Value p = Value::object();
      p.set("model") = Value(d.model);
      p.set("material") = Value(d.material);
      p.set("scale") = Value((double)d.scale);
      p.set("lit") = Value(d.lit);
      o.set("mesh") = std::move(p);
      break;
    }
    case NodeType::Particles: {
      const ParticleData& d = std::get<ParticleData>(payload);
      Value p = Value::object();
      p.set("vert") = Value(d.vert);
      p.set("frag") = Value(d.frag);
      p.set("prev") = Value(d.prev);
      p.set("count") = Value(d.count);
      p.set("renderScale") = Value((double)d.renderScale);
      o.set("particles") = std::move(p);
      break;
    }
    case NodeType::Quad: {
      const QuadData& d = std::get<QuadData>(payload);
      Value p = Value::object();
      p.set("frag") = Value(d.frag);
      p.set("handoff") = Value(d.handoff);
      p.set("renderScale") = Value((double)d.renderScale);
      p.set("mode") = Value((double)d.mode);
      o.set("quad") = std::move(p);
      break;
    }
    case NodeType::Sprite: {
      const SpriteData& d = std::get<SpriteData>(payload);
      Value p = Value::object();
      p.set("tex") = Value(d.tex);
      p.set("color") = vec4ToJson(d.color);
      p.set("opacity") = Value((double)d.opacity);
      p.set("size") = vecToJson(d.size);
      o.set("sprite") = std::move(p);
      break;
    }
    case NodeType::Text: {
      const TextData& d = std::get<TextData>(payload);
      Value p = Value::object();
      p.set("text") = Value(d.text);
      p.set("sizePx") = Value(d.sizePx);
      p.set("font") = Value(d.font);
      p.set("style") = Value(d.style);
      p.set("color") = vec4ToJson(d.color);
      p.set("opacity") = Value((double)d.opacity);
      p.set("align") = Value((double)d.align);
      o.set("text") = std::move(p);
      break;
    }
    case NodeType::Post: {
      const PostData& d = std::get<PostData>(payload);
      Value p = Value::object();
      p.set("preset") = Value(d.preset);
      o.set("post") = std::move(p);
      break;
    }
    case NodeType::TimelineSystem: {
      const TimelineData& d = std::get<TimelineData>(payload);
      Value p = Value::object();
      p.set("file") = Value(d.file);
      o.set("timeline") = std::move(p);
      break;
    }
    default: break;
  }

  Value ch = Value::array();
  for (const auto& c : children) {
    Value co = Value::object();
    c->toJsonInto(co);
    ch.push(std::move(co));
  }
  o.set("children") = std::move(ch);
}

Value SceneNode::toJson() const {
  Value o = Value::object();
  toJsonInto(o);
  return o;
}

std::unique_ptr<SceneNode> SceneNode::fromJson(const Value& v) {
  auto node = std::make_unique<SceneNode>();
  node->name = v.get("name").asStr("node");
  node->type = nodeTypeFromName(v.get("type").asStr("empty"));
  node->pos = jsonToVec3(v.get("pos"));
  node->rot = jsonToVec4(v.get("rot"));
  if (node->rot[3] == 0 && node->rot[0] == 0 && node->rot[1] == 0 && node->rot[2] == 0)
    node->rot = Q4{0, 0, 0, 1};
  node->scale = jsonToVec3(v.get("scale"));
  if (node->scale[0] == 0 && node->scale[1] == 0 && node->scale[2] == 0)
    node->scale = V3{1, 1, 1};
  node->visible = v.get("visible").asBool(true);
  node->enabled = v.get("enabled").asBool(true);
  node->layer = v.get("layer").asInt(0);
  for (const auto& t : v.get("tags").asArr()) node->tags.push_back(t.asStr());

  switch (node->type) {
    case NodeType::Camera: {
      const Value& p = v.get("camera");
      CamData d;
      d.fov = (float)p.get("fov").asNum(62);
      d.nearP = (float)p.get("near").asNum(0.05);
      d.farP = (float)p.get("far").asNum(400);
      d.target = jsonToVec3(p.get("target"));
      d.rig = p.get("rig").asStr();
      node->payload = d;
      break;
    }
    case NodeType::Light: {
      const Value& p = v.get("light");
      LightData d;
      d.type = p.get("type").asStr("point");
      d.color = jsonToVec3(p.get("color"));
      if (d.color[0] == 0 && d.color[1] == 0 && d.color[2] == 0) d.color = {1, 1, 1};
      d.intensity = (float)p.get("intensity").asNum(1);
      d.range = (float)p.get("range").asNum(10);
      d.angle = (float)p.get("angle").asNum(45);
      d.castShadow = p.get("shadow").asBool(false);
      node->payload = d;
      break;
    }
    case NodeType::Mesh: {
      const Value& p = v.get("mesh");
      MeshData d;
      d.model = p.get("model").asStr();
      d.material = p.get("material").asStr();
      d.scale = (float)p.get("scale").asNum(1);
      d.lit = p.get("lit").asBool(true);
      node->payload = d;
      break;
    }
    case NodeType::Particles: {
      const Value& p = v.get("particles");
      ParticleData d;
      d.vert = p.get("vert").asStr("particles.vert");
      d.frag = p.get("frag").asStr("particles.frag");
      d.prev = p.get("prev").asStr("particles_prev.frag");
      d.count = p.get("count").asInt(5000);
      d.renderScale = (float)p.get("renderScale").asNum(1);
      node->payload = d;
      break;
    }
    case NodeType::Quad: {
      const Value& p = v.get("quad");
      QuadData d;
      d.frag = p.get("frag").asStr();
      d.handoff = p.get("handoff").asBool(false);
      d.renderScale = (float)p.get("renderScale").asNum(1);
      d.mode = (float)p.get("mode").asNum(0);
      node->payload = d;
      break;
    }
    case NodeType::Sprite: {
      const Value& p = v.get("sprite");
      SpriteData d;
      d.tex = p.get("tex").asStr();
      d.color = jsonToVec4(p.get("color"));
      d.opacity = (float)p.get("opacity").asNum(1);
      d.size = jsonToVec3(p.get("size"));
      if (d.size[0] == 0 && d.size[1] == 0 && d.size[2] == 0) d.size = {1, 1, 1};
      node->payload = d;
      break;
    }
    case NodeType::Text: {
      const Value& p = v.get("text");
      TextData d;
      d.text = p.get("text").asStr();
      d.sizePx = p.get("sizePx").asInt(24);
      d.font = p.get("font").asStr();
      d.style = p.get("style").asStr("neon");
      d.color = jsonToVec4(p.get("color"));
      d.opacity = (float)p.get("opacity").asNum(1);
      d.align = (float)p.get("align").asNum(-1);
      node->payload = d;
      break;
    }
    case NodeType::Post: {
      const Value& p = v.get("post");
      PostData d;
      d.preset = p.get("preset").asStr();
      node->payload = d;
      break;
    }
    case NodeType::TimelineSystem: {
      const Value& p = v.get("timeline");
      TimelineData d;
      d.file = p.get("file").asStr();
      node->payload = d;
      break;
    }
    default: break;
  }

  for (const auto& c : v.get("children").asArr()) {
    node->addChild(fromJson(c));
  }
  return node;
}

// ---------------------------------------------------------------------------
// SceneGraph
// ---------------------------------------------------------------------------
SceneGraph::SceneGraph() {
  root_ = std::make_unique<SceneNode>("world", NodeType::Empty);
}

SceneNode* SceneGraph::addNode(std::string name, NodeType type, NodePayload payload, SceneNode* parent) {
  auto node = std::make_unique<SceneNode>(std::move(name), type, std::move(payload));
  node->dirty = true;
  SceneNode* out = (parent ? parent : root_.get())->addChild(std::move(node));
  return out;
}

void SceneGraph::update() { root_->updateWorld(false); }

std::vector<SceneNode*> SceneGraph::nodesOf(NodeType t, bool activeOnly) {
  std::vector<SceneNode*> out;
  walk([&](SceneNode* n) {
    if (n->type == t && (!activeOnly || n->isActive())) out.push_back(n);
  });
  return out;
}

std::vector<SceneNode*> SceneGraph::findTag(const std::string& tag) {
  std::vector<SceneNode*> out;
  walk([&](SceneNode* n) {
    for (const auto& t : n->tags) if (t == tag) out.push_back(n);
  });
  return out;
}

Value SceneGraph::toJson() const {
  return root_->toJson();
}

void SceneGraph::fromJson(const Value& v) {
  clear();
  auto loaded = SceneNode::fromJson(v);
  loaded->parent = nullptr;
  // transplant children of the loaded root into our root node
  root_->name = loaded->name;
  root_->pos = loaded->pos;
  root_->rot = loaded->rot;
  root_->scale = loaded->scale;
  root_->visible = loaded->visible;
  root_->enabled = loaded->enabled;
  root_->tags = loaded->tags;
  root_->layer = loaded->layer;
  root_->payload = loaded->payload;
  root_->type = loaded->type;
  root_->children = std::move(loaded->children);
  for (auto& c : root_->children) c->parent = root_.get();
  root_->dirty = true;
}

void SceneGraph::clear() {
  root_->children.clear();
  root_->pos = {0, 0, 0};
  root_->rot = {0, 0, 0, 1};
  root_->scale = {1, 1, 1};
  root_->tags.clear();
  root_->layer = 0;
  root_->payload = std::monostate{};
  root_->type = NodeType::Empty;
  root_->name = "world";
  root_->dirty = true;
}

}  // namespace ns
