// ---------------------------------------------------------------------------
// Renderer: GL state, resize, adaptive quality governor (port of renderer.ts).
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include "engine/mesh.hpp"
#include "engine/ubo.hpp"
#include <algorithm>
#include <vector>

namespace ns {

struct Quality {
  float scale = 1.0f;
  bool dof = true;
  int bloom = 3;
  bool fxaa = true;
  float particles = 1.0f;
};

class Renderer {
public:
  Mesh fsTriangle{::gl::TRIANGLES};
  int viewW = 1, viewH = 1;   // device pixel size of the window
  int resW = 1, resH = 1;     // render resolution (scaled)
  bool supportsFloatRT = false;
  Quality quality;
  SharedBlock sharedBlock;

  // adaptive quality state
  float emaMs = 16.7f;
  int badFrames = 0;
  int goodFrames = 0;

  std::vector<void (*)()> observers;

  Renderer() {
    // fullscreen triangle for every screen-space pass (verts + uvs)
    fsTriangle = fullscreenTriangle();
    // baseline GL state
    ::glDisable(::gl::DEPTH_TEST);
    ::glDisable(::gl::CULL_FACE);
    ::glDisable(::gl::BLEND);
    supportsFloatRT = true;  // desktop GL 3.3 core renders RGBA16F fine
  }

  void onResize(void (*cb)()) { observers.push_back(cb); }

  void resize(int w, int h, float dpr = 1.0f) {
    viewW = std::max(2, (int)(w * dpr));
    viewH = std::max(2, (int)(h * dpr));
    resW = std::max(2, (int)(viewW * quality.scale));
    resH = std::max(2, (int)(viewH * quality.scale));
    for (auto cb : observers) cb();
  }

  /**
   * Adaptive quality governor. Drops detail under sustained load, recovers on
   * headroom, with hysteresis (drop after ~0.5s above 24ms EMA, recover after
   * ~1.5s under 15ms / fitting the vsync budget).
   */
  void tick(float frameMs, float workMs, float particleBound = 0) {
    emaMs = emaMs * 0.95f + workMs * 0.05f;

    if (emaMs > 24.0f && workMs > 24.0f) {
      badFrames++;
      goodFrames = 0;
    } else if (emaMs < 15.0f || (frameMs <= 17.0f && workMs <= 16.0f)) {
      goodFrames++;
      badFrames = 0;
    } else {
      badFrames = 0;
      goodFrames = 0;
    }

    if (badFrames > 30) { badFrames = 0; stepDown(particleBound); }
    if (goodFrames > 90) { goodFrames = 0; stepUp(); }
  }

private:
  void stepDown(float particleBound) {
    Quality& q = quality;
    if (particleBound > 0.5f && q.particles > 0.4f) {
      q.particles = std::max(0.4f, q.particles - 0.25f);
    } else if (q.bloom > 1) {
      q.bloom--;
    } else if (q.dof) {
      q.dof = false;
    } else if (q.scale > 0.55f) {
      q.scale = std::max(0.55f, q.scale - 0.15f);
      resize(viewW, viewH);
    }
  }

  void stepUp() {
    Quality& q = quality;
    if (q.scale < 1) {
      q.scale = std::min(1.0f, q.scale + 0.15f);
      resize(viewW, viewH);
    } else if (!q.dof) {
      q.dof = true;
    } else if (q.bloom < 3) {
      q.bloom++;
    } else if (q.particles < 1) {
      q.particles = std::min(1.0f, q.particles + 0.25f);
    }
  }
};

}  // namespace ns
