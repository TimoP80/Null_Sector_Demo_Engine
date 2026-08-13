// ---------------------------------------------------------------------------
// CineText - shared cinematic text helper for the native port.
// Wraps a bitmap TextMesh + the cine_text.frag program so every effect can
// drop in styled, audio-reactive captions with one call. Envelope + hash
// helpers stay here so scenes share identical timing maths.
// ---------------------------------------------------------------------------
#pragma once

#include "effects/base_fwd.hpp"
#include "engine/assets.hpp"
#include "engine/renderer.hpp"
#include "engine/shader.hpp"
#include "engine/textmesh.hpp"
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace ns {

enum class CineStyle : int {
  Terminal = 0,
  Holo = 1,
  Glitch = 2,
  Neon = 3,
  Scan = 4,
  Dissolve = 5,
  Chrome = 6,
  Outline = 7,
};

class CineText {
public:
  /** build the program + grab the font metrics (must be called after the
   *  GL context + font atlas exist, from the effect's init) */
  void init(const Assets& assets) {
    prog_ = std::make_unique<Shader>("text.vert", "cine_text.frag");
    font_ = assets.fontMetrics;
  }

  /** draw a single styled line centered at NDC centerY (top = +1, bottom = -1) */
  void line(EffectContext& ctx, const std::string& text, float centerY, int sizePx,
            CineStyle style, float alpha, float seed = 0, float glow = 0.5f,
            float progress = 1.0f, float energy = 0, float centerX = 0,
            const Assets* fontAssets = nullptr, float align = -1.0f) {
    if (alpha <= 0.01f || text.empty()) return;
    const Assets& atlas = fontAssets ? *fontAssets : *ctx.assets;
    const Renderer& r = *ctx.r;
    font_ = atlas.fontMetrics;
    textMesh_.build({{text, align, seed}}, font_,
                    {r.viewW, r.viewH, sizePx, 0, centerX}, centerY);
    if (textMesh_.empty()) return;
    prog_->use();
    prog_->set1i("uTex", 0);
    prog_->set1f("uTime", ctx.time);
    prog_->set1f("uAlpha", alpha);
    prog_->set1f("uGlow", glow);
    prog_->set1f("uSeed", seed);
    prog_->set1f("uProgress", progress);
    prog_->set1f("uEnergy", energy);
    prog_->set1i("uStyle", (int)style);
    atlas.fontTex.bind(0);
    textMesh_.draw();
  }

  /** typewriter: reveal the first `shown` chars, cursor while typing */
  void typed(EffectContext& ctx, const std::string& text, float centerY, int sizePx,
             CineStyle style, int shown, float alpha, float seed = 0, float glow = 0.5f,
             float energy = 0, float centerX = 0, const Assets* fontAssets = nullptr) {
    if (shown <= 0 || alpha <= 0.01f) return;
    std::string s = text.substr(0, (size_t)shown);
    if (shown < (int)text.size()) s += "_";  // real '_' glyph = the typing cursor
    line(ctx, s, centerY, sizePx, style, alpha, seed, glow, 1.0f, energy, centerX, fontAssets);
  }

  /** scramble: `shown` chars resolved, the rest jitter through random glyphs */
  void scramble(EffectContext& ctx, const std::string& text, float centerY, int sizePx,
                CineStyle style, int shown, float alpha, float seed = 0, float glow = 0.5f,
                float energy = 0, float centerX = 0, const Assets* fontAssets = nullptr) {
    if (shown <= 0 || alpha <= 0.01f) return;
    static constexpr char GLYPHS[] = "01<>/\\|#@$%&*+=-;:";
    static const int NGLYPHS = (int)sizeof(GLYPHS) - 1;  // excludes the nul
    std::string s;
    s.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
      if ((int)i < shown || text[i] == ' ') {
        s += text[i];
      } else {
        const float h = hash01(seed * 13.7f + (float)i * 0.91f + std::floor(ctx.time * 18.0f) * 0.37f);
        s += GLYPHS[(int)(h * NGLYPHS) % NGLYPHS];
      }
    }
    line(ctx, s, centerY, sizePx, style, alpha, seed, glow, 1.0f, energy, centerX, fontAssets);
  }

  // --- timing helpers ---------------------------------------------------------

  /** linear attack / hold / release envelope; returns 0 outside the window */
  static float env(float t, float attack, float hold, float release) {
    if (t < 0) return 0;
    if (t < attack) return t / attack;
    if (t < attack + hold) return 1;
    if (t < attack + hold + release) return 1 - (t - attack - hold) / release;
    return 0;
  }

  /** smooth (eased) attack / hold / release envelope */
  static float senv(float t, float attack, float hold, float release) {
    const float a = env(t, attack, hold, release);
    return a * a * (3 - 2 * a);
  }

  /** deterministic 0..1 hash from a float seed */
  static float hash01(float x) {
    x = std::sin(x * 127.1f) * 43758.5453f;
    return x - std::floor(x);
  }

private:
  std::unique_ptr<Shader> prog_;
  FontMetrics font_;
  TextMesh textMesh_;
};

}  // namespace ns
