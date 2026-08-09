// ---------------------------------------------------------------------------
// editor_curves.cpp - keyframe curve editor + inspector keyframe buttons.
//
// The curve editor is a UI over the EXISTING data: `anim` commands in the
// document AST (Cmd.keys = KeyframeRow list), which the runtime's
// AnimationSystem consumes verbatim (DemoApp::cmdAnim). Editing the document
// marks it dirty and pushes a live preview into the runtime animation library
// (DemoApp::editorApplyAnim) so the viewport updates instantly; Ctrl+S
// commits the document to the .nsd and reloads the show.
//
//   left   channel list (every `anim` command in the script)
//   right  curve canvas: drag keys (time snaps to the beat/bar grid when
//          scrub quantization is on), double-click adds a key, right-click
//          edits/removes, Delete removes the selection, Copy/Paste moves
//          keys, and the per-key interpolator is a combo when selected.
// ---------------------------------------------------------------------------
#include "editor/editor.hpp"
#include "framework/anim/animation.hpp"
#include "framework/script/nsdwriter.hpp"
#include "framework/core/log.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ns {

namespace {

// color helpers mirroring editor.cpp's palette (kept local so this TU stays
// self-contained; the values must match the main file's theme)
inline ImU32 c32(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
  return IM_COL32(r, g, b, a);
}
inline ImU32 c32f(float r, float g, float b, float a = 1.0f) {
  return c32((unsigned)(r * 255), (unsigned)(g * 255), (unsigned)(b * 255),
             (unsigned)(a * 255));
}
const ImU32 kPhosphor = c32(94, 240, 200);
const ImU32 kHot = c32(255, 95, 143);
const ImU32 kAmber = c32(255, 196, 107);
const ImU32 kDim = c32(133, 146, 167);
const ImU32 kFaint = c32(86, 98, 121);

/** euler XYZ degrees (mirrors the static helper in editor.cpp) */
V3 quatToEulerDeg(const Q4& qin) {
  const Q4 q = qNorm(qin);
  const float m0 = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
  const float m3 = 2 * (q[0] * q[1] - q[2] * q[3]);
  const float m6 = 2 * (q[0] * q[2] + q[1] * q[3]);
  const float m7 = 2 * (q[1] * q[2] - q[0] * q[3]);
  const float m8 = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
  constexpr float kRad2Deg = 180.0f / 3.14159265f;
  return {std::atan2(-m7, m8) * kRad2Deg,
          std::asin(clampf(m6, -1.0f, 1.0f)) * kRad2Deg,
          std::atan2(-m3, m0) * kRad2Deg};
}

const char* kCurveInterps[] = {"linear", "smooth", "cubic", "bezier",
                               "ease-in", "ease-out", "ease-in-out",
                               "bounce", "elastic"};
const int kCurveInterpCount =
    (int)(sizeof kCurveInterps / sizeof kCurveInterps[0]);

int interpIndex(const std::string& n) {
  for (int i = 0; i < kCurveInterpCount; i++)
    if (n == kCurveInterps[i]) return i;
  return 0;
}

std::string valueLabel(const Value& v) {
  float f[8];
  const int n = v.toFloats(f, 8);
  if (n <= 0) return v.toString();
  std::string s;
  for (int i = 0; i < n; i++) {
    char b[24];
    std::snprintf(b, sizeof b, "%.3g", f[i]);
    if (i) s += ", ";
    s += b;
  }
  return s;
}

Value floatVecToValue(const float* f, int n) {
  Value::Array a;
  for (int i = 0; i < n; i++) a.push_back(Value((double)f[i]));
  return Value(std::move(a));
}

/** the runtime AnimChannel equivalent of an `anim` Cmd (used to sample the
 *  curve for new keys and to draw the polyline) */
AnimChannel channelFromCmd(const Cmd& c) {
  AnimChannel ch;
  const std::string targetProp = c.args.size() > 1 ? c.args[1].asStr() : "";
  const size_t dot = targetProp.find('.');
  ch.target = dot != std::string::npos ? targetProp.substr(0, dot) : targetProp;
  ch.property = dot != std::string::npos ? targetProp.substr(dot + 1) : targetProp;
  const Interp dflt =
      c.args.size() >= 3 ? parseInterp(c.args[2].asStr()) : Interp::Linear;
  for (const auto& k : c.keys) {
    AnimKey key;
    key.t = k.t;
    key.v = k.v;
    key.interp = k.interp.empty() ? dflt : parseInterp(k.interp);
    ch.keys.push_back(std::move(key));
  }
  return ch;
}

/** insert a key at time T (replacing any key within 0.1 ms), keeping the
 *  channel sorted by time */
void insertKeyRow(Cmd& c, float t, const Value& v, const std::string& interp) {
  for (auto& k : c.keys) {
    if (std::fabs(k.t - t) < 1e-4f) {
      k.v = v;
      k.interp = interp;
      return;
    }
  }
  KeyframeRow k;
  k.t = t;
  k.v = v;
  k.interp = interp;
  c.keys.push_back(std::move(k));
  std::stable_sort(c.keys.begin(), c.keys.end(),
                   [](const KeyframeRow& a, const KeyframeRow& b) {
                     return a.t < b.t;
                   });
}

}  // namespace


// ---------------------------------------------------------------------------
// inspector keyframe button: a small drawn diamond (the default font has no
// ◆ glyph, and the sources stay ASCII for MSVC without /utf-8)
// ---------------------------------------------------------------------------
bool editorKeyframeButton(const char* id) {
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("kf", ImVec2(20.0f, ImGui::GetFrameHeight()));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("keyframe this property");
  const ImVec2 c = ImGui::GetItemRectMin();
  const ImVec2 sz = ImGui::GetItemRectSize();
  const float x = c.x + sz.x * 0.5f, y = c.y + sz.y * 0.5f;
  const ImU32 col = ImGui::IsItemHovered() ? c32(255, 95, 143) : c32(255, 196, 107);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddTriangleFilled(ImVec2(x, y - 4.5f), ImVec2(x - 4.0f, y), ImVec2(x + 4.0f, y), col);
  dl->AddTriangleFilled(ImVec2(x, y + 4.5f), ImVec2(x - 4.0f, y), ImVec2(x + 4.0f, y), col);
  ImGui::PopID();
  return clicked;
}

// ---------------------------------------------------------------------------
// channel list + live preview
// ---------------------------------------------------------------------------
void DemoEditor::rebuildCurveList() {
  curveCmds_ = doc_.animCmds();
  if (curveSel_ < 0 || curveSel_ >= (int)curveCmds_.size()) {
    curveSel_ = curveCmds_.empty() ? -1 : 0;
    selKeys_.clear();
    keyDragging_ = false;
    dragKey_ = -1;
  }
}

void DemoEditor::applyChannelToRuntime(Cmd& c) {
  if (!w_.app) return;
  w_.app->editorApplyAnim(c);
}

float DemoEditor::snapKeyTime(float t) const {
  if (quantize_) {
    const float beat = w_.timeline ? w_.timeline->beatSec() : 60.0f / 216.0f;
    const float grid = quantizeGrid_ == 1 ? beat * 4.0f : beat;
    return std::floor((t - beatOffset_) / grid + 0.5f) * grid + beatOffset_;
  }
  return std::round(t * 60.0f) / 60.0f;
}

// ---------------------------------------------------------------------------
// the panel
// ---------------------------------------------------------------------------
void DemoEditor::drawCurveEditor() {
  if (!showCurves_) return;
  ImGui::Begin("Curves", &showCurves_);
  rebuildCurveList();

  if (curveCmds_.empty()) {
    ImGui::TextDisabled("No `anim` channels in the script yet.");
    ImGui::TextDisabled("Use the inspector keyframe button (key) on a node's");
    ImGui::TextDisabled("transform, or add an `anim` command to the .nsd.");
    ImGui::End();
    return;
  }

  // --- channel list ---------------------------------------------------------
  const float listW = std::min(230.0f, ImGui::GetContentRegionAvail().x * 0.3f);
  ImGui::BeginChild("curve_channels", ImVec2(listW, 0), true);
  for (int i = 0; i < (int)curveCmds_.size(); i++) {
    Cmd& c = *curveCmds_[i];
    std::string label = c.args.empty() ? "(unnamed)" : c.args[0].asStr();
    if (c.args.size() > 1) label += "  " + c.args[1].asStr();
    if (ImGui::Selectable(label.c_str(), curveSel_ == i)) {
      curveSel_ = i;
      selKeys_.clear();
      keyDragging_ = false;
      dragKey_ = -1;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", nsdSerializeCmd(c).c_str());
  }
  ImGui::EndChild();
  ImGui::SameLine();

  Cmd& ch = *curveCmds_[curveSel_];
  const AnimChannel chan = channelFromCmd(ch);

  // --- canvas geometry -------------------------------------------------------
  const float availW = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
  const float availH = std::max(ImGui::GetContentRegionAvail().y - 34.0f, 120.0f);
  const float barH = 24.0f;
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();

  float tMax = 1.0f;
  for (const auto& k : ch.keys) tMax = std::max(tMax, k.t);
  const float dur = w_.app ? w_.app->editor().duration : 0;
  if (dur > 0) tMax = std::max(tMax, dur);
  tMax *= 1.05f;
  const float tMin = 0.0f;

  float vMin = -1.0f, vMax = 1.0f;
  bool any = false;
  for (const auto& k : ch.keys) {
    float f[8];
    const int n = k.v.toFloats(f, 8);
    if (n > 0) {
      if (!any) { vMin = vMax = f[0]; any = true; }
      else { vMin = std::min(vMin, f[0]); vMax = std::max(vMax, f[0]); }
    }
  }
  if (vMax - vMin < 1e-4f) { vMin -= 0.5f; vMax += 0.5f; }
  const float vPad = (vMax - vMin) * 0.12f;
  vMin -= vPad;
  vMax += vPad;

  auto tx = [&](float t) { return o.x + (t - tMin) / (tMax - tMin) * availW; };
  auto ty = [&](float v) {
    return o.y + barH + (1.0f - (v - vMin) / (vMax - vMin)) * (availH - barH);
  };
  auto tinv = [&](float x) { return tMin + (x - o.x) / availW * (tMax - tMin); };
  auto vinv = [&](float y) {
    return vMax - (y - o.y - barH) / (availH - barH) * (vMax - vMin);
  };

  // --- background + grids -----------------------------------------------------
  dl->AddRectFilled(o, ImVec2(o.x + availW, o.y + availH), c32(13, 18, 27));
  const float beat = w_.timeline ? w_.timeline->beatSec() : 60.0f / 216.0f;
  const float bar = beat * 4.0f;
  if (availW / tMax * bar > 22.0f) {
    for (float t = bar; t < tMax; t += bar) {
      dl->AddLine(ImVec2(tx(t), o.y + barH), ImVec2(tx(t), o.y + availH),
                  c32(42, 54, 72));
    }
  } else if (availW / tMax * beat > 6.0f) {
    for (float t = beat; t < tMax; t += beat) {
      dl->AddLine(ImVec2(tx(t), o.y + barH), ImVec2(tx(t), o.y + availH),
                  c32(34, 44, 60));
    }
  }
  if (vMin < 0 && vMax > 0) {
    dl->AddLine(ImVec2(o.x, ty(0)), ImVec2(o.x + availW, ty(0)), c32(52, 64, 84));
  }
  const float show = w_.director ? w_.director->show : 0;
  if (show >= tMin && show <= tMax) {
    dl->AddLine(ImVec2(tx(show), o.y + barH), ImVec2(tx(show), o.y + availH),
                kHot, 1.2f);
  }
  char rb[64];
  std::snprintf(rb, sizeof rb, "%.1fs .. %.1fs  (%s)",
                tMin, tMax, chan.target.c_str());
  dl->AddText(ImVec2(o.x + 6, o.y + 4), kFaint, rb);

  // --- curve polyline (sampled through the runtime interpolator) ----------------
  if (ch.keys.size() >= 2) {
    const int steps = 20;
    std::vector<ImVec2> pts;
    pts.reserve((ch.keys.size() - 1) * steps + 2);
    for (size_t i = 1; i < chan.keys.size(); i++) {
      const float ta = chan.keys[i - 1].t, tb = chan.keys[i].t;
      if (tb - ta < 1e-5f) continue;
      const float span = tb - ta;
      for (int s = 0; s < steps; s++) {
        const float t01 = (float)s / steps;
        const float t = ta + span * t01;
        float out[8];
        const int n = AnimationSystem::sampleChannel(chan, t, out, 8);
        pts.push_back(ImVec2(tx(t), ty(n > 0 ? out[0] : 0.0f)));
      }
    }
    {
      float out[8];
      const int n = AnimationSystem::sampleChannel(chan, chan.keys.back().t, out, 8);
      pts.push_back(ImVec2(tx(chan.keys.back().t),
                           ty(n > 0 ? out[0] : chan.keys.back().v.asFloat())));
    }
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), kPhosphor, 0, 1.6f);
  }

  // --- keys -------------------------------------------------------------------
  for (size_t i = 0; i < ch.keys.size(); i++) {
    float f[8];
    const int n = ch.keys[i].v.toFloats(f, 8);
    if (n < 1) continue;
    const float x = tx(ch.keys[i].t), y = ty(f[0]);
    const bool sel =
        std::find(selKeys_.begin(), selKeys_.end(), (int)i) != selKeys_.end();
    const ImU32 col = sel ? kHot : kAmber;
    dl->AddTriangleFilled(ImVec2(x, y - 5), ImVec2(x - 4, y), ImVec2(x + 4, y), col);
    dl->AddTriangleFilled(ImVec2(x, y + 5), ImVec2(x - 4, y), ImVec2(x + 4, y), col);
    if (sel || (keyDragging_ && dragKey_ == (int)i)) {
      dl->AddText(ImVec2(x + 7, y - 8), kDim, valueLabel(ch.keys[i].v).c_str());
    }
  }

  // --- interactions ------------------------------------------------------------
  ImGui::SetCursorScreenPos(o);
  ImGui::InvisibleButton("curve_canvas", ImVec2(availW, availH));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = io.MousePos;

  int hit = -1;
  for (size_t i = 0; i < ch.keys.size(); i++) {
    float f[8];
    const int n = ch.keys[i].v.toFloats(f, 8);
    if (n < 1) continue;
    const float x = tx(ch.keys[i].t), y = ty(f[0]);
    if (std::fabs(mouse.x - x) < 7 && std::fabs(mouse.y - y) < 9) { hit = (int)i; break; }
  }

  if (hovered && !keyDragging_) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (hit >= 0) {
        if (!io.KeyCtrl) selKeys_.clear();
        const auto it = std::find(selKeys_.begin(), selKeys_.end(), hit);
        if (it != selKeys_.end()) selKeys_.erase(it);
        else selKeys_.push_back(hit);
        if (selKeys_.empty()) selKeys_.push_back(hit);
        // start a drag: remember every selected key's original t/v so the
        // whole selection moves together
        dragKey_ = hit;
        dragKeyT0_ = ch.keys[hit].t;
        dragKeyV0_ = 0.0f;
        {
          float f0[8];
          if (ch.keys[hit].v.toFloats(f0, 8) > 0) dragKeyV0_ = f0[0];
        }
        dragOrigT_.clear();
        dragOrigV_.clear();
        for (int si : selKeys_) {
          if (si < 0 || si >= (int)ch.keys.size()) continue;
          dragOrigT_.push_back(ch.keys[si].t);
          float f0[8];
          dragOrigV_.push_back(ch.keys[si].v.toFloats(f0, 8) > 0 ? f0[0] : 0.0f);
        }
        keyDragging_ = true;
        doc_.beginEdit("keyframe drag");
      } else {
        selKeys_.clear();
      }
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit < 0) {
      // add a key at the cursor: the cursor y sets the first component, any
      // extra vector components keep the curve's sampled values
      const float t = snapKeyTime(std::max(tinv(mouse.x), 0.0f));
      const float v = vinv(mouse.y);
      float samp[8];
      const int n = AnimationSystem::sampleChannel(chan, t, samp, 8);
      Value val;
      if (n >= 2) {
        samp[0] = v;
        val = floatVecToValue(samp, n);
      } else {
        val = Value((double)v);
      }
      doc_.beginEdit("add keyframe");
      insertKeyRow(ch, t, val, "");
      doc_.endEdit();
      applyChannelToRuntime(ch);
      selKeys_.clear();
      for (size_t i = 0; i < ch.keys.size(); i++)
        if (std::fabs(ch.keys[i].t - t) < 1e-4f) selKeys_.push_back((int)i);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
      selKeys_.clear();
      selKeys_.push_back(hit);
      ImGui::OpenPopup("curve_key_popup");
    }
  }

  // key drag (continues while the button is held, even outside hover)
  if (keyDragging_ && dragKey_ >= 0 && dragKey_ < (int)ch.keys.size()) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      keyDragging_ = false;
      dragKey_ = -1;
      doc_.endEdit();
      applyChannelToRuntime(ch);
      rebuildCurveList();
    } else {
      const float t = snapKeyTime(std::max(tinv(mouse.x), 0.0f));
      const float v = vinv(mouse.y);
      const float dt = t - dragKeyT0_;
      const float dv = v - dragKeyV0_;
      size_t si = 0;
      for (int ki : selKeys_) {
        if (ki < 0 || ki >= (int)ch.keys.size()) continue;
        float f[8];
        const int n = ch.keys[ki].v.toFloats(f, 8);
        if (n <= 0) continue;
        if (ki == dragKey_) {
          ch.keys[ki].t = t;
          if (n >= 2) {
            for (int k = 0; k < n; k++) f[k] += dv;
            ch.keys[ki].v = floatVecToValue(f, n);
          } else {
            ch.keys[ki].v = Value((double)v);
          }
        } else if (si < dragOrigT_.size()) {
          ch.keys[ki].t = dragOrigT_[si] + dt;
          for (int k = 0; k < n; k++) f[k] += dv;
          ch.keys[ki].v = floatVecToValue(f, n);
        }
        si++;
      }
      std::stable_sort(ch.keys.begin(), ch.keys.end(),
                       [](const KeyframeRow& a, const KeyframeRow& b) {
                         return a.t < b.t;
                       });
      applyChannelToRuntime(ch);  // live preview while dragging
    }
  }

  // --- right-click key popup ----------------------------------------------------
  if (ImGui::BeginPopup("curve_key_popup")) {
    if (!selKeys_.empty()) {
      if (ImGui::MenuItem("Delete")) {
        doc_.beginEdit("delete keyframe");
        std::vector<int> ks = selKeys_;
        std::sort(ks.begin(), ks.end(), std::greater<int>());
        for (int ki : ks)
          if (ki >= 0 && ki < (int)ch.keys.size())
            ch.keys.erase(ch.keys.begin() + ki);
        doc_.endEdit();
        applyChannelToRuntime(ch);
        selKeys_.clear();
      }
      if (ImGui::MenuItem("Copy")) {
        curveClip_.clear();
        const float anchor = ch.keys[selKeys_[0]].t;
        for (int ki : selKeys_) {
          if (ki < 0 || ki >= (int)ch.keys.size()) continue;
          curveClip_.push_back(
              {ch.keys[ki].t - anchor, ch.keys[ki].v, ch.keys[ki].interp});
        }
      }
      if (ImGui::BeginMenu("Interpolator")) {
        for (int i = 0; i < kCurveInterpCount; i++) {
          if (ImGui::MenuItem(kCurveInterps[i])) {
            doc_.beginEdit("keyframe interp");
            for (int ki : selKeys_)
              if (ki >= 0 && ki < (int)ch.keys.size())
                ch.keys[ki].interp = kCurveInterps[i];
            doc_.endEdit();
            applyChannelToRuntime(ch);
          }
        }
        ImGui::EndMenu();
      }
    }
    ImGui::EndPopup();
  }

  // --- toolbar row (below the canvas) -------------------------------------------
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + availH + 4));
  if (ImGui::Button("Add Key")) {
    float t = show;
    if (t < tMin || t > tMax) t = ch.keys.empty() ? 0.0f : ch.keys.back().t + 1.0f;
    t = snapKeyTime(std::max(t, 0.0f));
    float samp[8];
    const int n = AnimationSystem::sampleChannel(chan, t, samp, 8);
    Value val = Value((double)(n > 0 ? samp[0] : 0.0f));
    if (n > 1) val = floatVecToValue(samp, n);
    doc_.beginEdit("add keyframe");
    insertKeyRow(ch, t, val, "");
    doc_.endEdit();
    applyChannelToRuntime(ch);
    selKeys_.clear();
    for (size_t i = 0; i < ch.keys.size(); i++)
      if (std::fabs(ch.keys[i].t - t) < 1e-4f) selKeys_.push_back((int)i);
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete") && !selKeys_.empty()) {
    doc_.beginEdit("delete keyframe");
    std::vector<int> ks = selKeys_;
    std::sort(ks.begin(), ks.end(), std::greater<int>());
    for (int ki : ks)
      if (ki >= 0 && ki < (int)ch.keys.size())
        ch.keys.erase(ch.keys.begin() + ki);
    doc_.endEdit();
    applyChannelToRuntime(ch);
    selKeys_.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy") && !selKeys_.empty()) {
    curveClip_.clear();
    const float anchor = ch.keys[selKeys_[0]].t;
    for (int ki : selKeys_) {
      if (ki < 0 || ki >= (int)ch.keys.size()) continue;
      curveClip_.push_back(
          {ch.keys[ki].t - anchor, ch.keys[ki].v, ch.keys[ki].interp});
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Paste") && !curveClip_.empty()) {
    const float tBase = snapKeyTime(std::max(show, 0.0f));
    doc_.beginEdit("paste keyframe");
    for (const auto& kc : curveClip_) insertKeyRow(ch, tBase + kc.t, kc.v, kc.interp);
    doc_.endEdit();
    applyChannelToRuntime(ch);
    selKeys_.clear();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("snap: %s",
                      quantize_ ? (quantizeGrid_ == 1 ? "bar" : "beat") : "1/60s");
  ImGui::SameLine();
  ImGui::TextDisabled("target: %s.%s", chan.target.c_str(), chan.property.c_str());
  if (!selKeys_.empty()) {
    ImGui::SameLine();
    int curI = interpIndex(ch.keys[selKeys_[0]].interp);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("##keyinterp", &curI, kCurveInterps, kCurveInterpCount)) {
      doc_.beginEdit("keyframe interp");
      for (int ki : selKeys_)
        if (ki >= 0 && ki < (int)ch.keys.size())
          ch.keys[ki].interp = kCurveInterps[curI];
      doc_.endEdit();
      applyChannelToRuntime(ch);
    }
  }

  ImGui::End();
}

// ---------------------------------------------------------------------------
// inspector keyframe button (key): keyframe node:<name>.<prop> in the active
// scene's setup, at the current scene-relative time and the current value.
// ---------------------------------------------------------------------------
void DemoEditor::keyframeNodeProperty(SceneNode* n, const char* prop) {
  if (!w_.app || !n || !prop) return;
  const std::string active = w_.app->activeScene();
  SceneDef* sc = doc_.findScene(active);
  if (!sc) {
    Log::warn("EDITOR", "keyframe: no scene '" + active + "' in the document");
    return;
  }
  // anim time base: the scene's start (setup `anim` commands play from scene
  // activation, so anim-local time = show time - scene start)
  float sceneStart = 0;
  for (const auto& sec : w_.app->sections())
    if (sec.name == active) { sceneStart = sec.start; break; }
  const float t = snapKeyTime(
      std::max((w_.director ? w_.director->show : 0) - sceneStart, 0.0f));

  Value v;
  if (std::strcmp(prop, "pos") == 0) v = floatVecToValue(n->pos.data(), 3);
  else if (std::strcmp(prop, "euler") == 0) {
    const V3 e = quatToEulerDeg(n->rot);
    v = floatVecToValue(e.data(), 3);
  } else if (std::strcmp(prop, "scale") == 0) v = floatVecToValue(n->scale.data(), 3);
  else return;

  const std::string targetProp = "node:" + n->name + "." + prop;

  // reuse an existing channel for this node/property inside this scene
  Cmd* found = nullptr;
  for (auto& c : sc->setup) {
    if (c.name == "anim" && c.args.size() >= 2 &&
        c.args[1].asStr() == targetProp) { found = &c; break; }
  }
  if (!found) {
    for (auto& b : sc->blocks)
      for (auto& c : b.cmds)
        if (c.name == "anim" && c.args.size() >= 2 &&
            c.args[1].asStr() == targetProp) { found = &c; break; }
  }

  doc_.beginEdit("keyframe property");
  if (found) {
    insertKeyRow(*found, t, v, "");
    applyChannelToRuntime(*found);
  } else {
    Cmd c;
    c.name = "anim";
    c.args.push_back(Value("node" + n->name + "_" + prop));
    c.args.push_back(Value(targetProp));
    insertKeyRow(c, t, v, "");
    sc->setup.push_back(std::move(c));
    applyChannelToRuntime(sc->setup.back());
  }
  doc_.endEdit();
  Log::info("EDITOR", std::string("keyframed ") + targetProp + " at " +
                std::to_string((double)t) + "s (" + valueLabel(v) + ")");
}

}  // namespace ns
